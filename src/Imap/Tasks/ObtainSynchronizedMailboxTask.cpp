/* Copyright (C) 2007 - 2010 Jan Kundrát <jkt@flaska.net>

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public
   License as published by the Free Software Foundation; either
   version 2 of the License, or version 3 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; see the file COPYING.  If not, write to
   the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
   Boston, MA 02110-1301, USA.
*/

#include "ObtainSynchronizedMailboxTask.h"
#include <QTimer>
#include "MailboxTree.h"
#include "Model.h"

namespace Imap {
namespace Mailbox {

ObtainSynchronizedMailboxTask::ObtainSynchronizedMailboxTask( Model* _model, const QModelIndex& _mailboxIndex, ImapTask* parentTask ) :
    ImapTask( _model ), conn(parentTask), mailboxIndex(_mailboxIndex),
    status(STATE_WAIT_FOR_CONN), uidSyncingMode(UID_SYNC_ALL)
{
    // We do *not* want to add ourselves to the list of dependant tasks here;
    // this is a special case, our perform() will be called by hand later on
}

void ObtainSynchronizedMailboxTask::perform()
{
    parser = conn->parser;
    Q_ASSERT( parser );
    model->_parsers[ parser ].activeTasks.append( this );

    if ( ! mailboxIndex.isValid() ) {
        // FIXME: proper error handling
        qDebug() << "The mailbox went missing, sorry";
        _completed();
        return;
    }

    TreeItemMailbox* mailbox = dynamic_cast<TreeItemMailbox*>( static_cast<TreeItem*>( mailboxIndex.internalPointer() ));
    Q_ASSERT(mailbox);
    TreeItemMsgList* msgList = dynamic_cast<TreeItemMsgList*>( mailbox->_children[0] );
    Q_ASSERT( msgList );

    msgList->_fetchStatus = TreeItem::LOADING;

    QMap<Parser*,Model::ParserState>::iterator it = model->_parsers.find( parser );

    selectCmd = parser->select( mailbox->mailbox() );
    it->commandMap[ selectCmd ] = Model::Task( Model::Task::SELECT, 0 );
    it->mailbox = mailbox;
    mailbox->syncState = SyncState();
    status = STATE_SELECTING;
    qDebug() << "STATE_SELECTING";
}

bool ObtainSynchronizedMailboxTask::handleStateHelper( Imap::Parser* ptr, const Imap::Responses::State* const resp )
{
    qDebug() << Q_FUNC_INFO;
    Q_ASSERT( parser == ptr );
    if ( handleResponseCodeInsideState( resp ) )
        return true;

    if ( resp->tag.isEmpty() )
        return false;

    if ( resp->tag == selectCmd ) {
        IMAP_TASK_ENSURE_VALID_COMMAND( selectCmd, Model::Task::SELECT );

        if ( resp->kind == Responses::OK ) {
            qDebug() << "received OK for selectCmd";
            Q_ASSERT( status == STATE_SELECTING );
            _finalizeSelect();
        } else {
            // FIXME: Tasks API error handling
            model->changeConnectionState( ptr, CONN_STATE_AUTHENTICATED);
        }
        IMAP_TASK_CLEANUP_COMMAND;
        return true;
    } else if ( resp->tag == uidSyncingCmd ) {
        IMAP_TASK_ENSURE_VALID_COMMAND( uidSyncingCmd, Model::Task::SEARCH_UIDS );

        if ( resp->kind == Responses::OK ) {
            qDebug() << "received OK for uidSyncingCmd";
            Q_ASSERT( status == STATE_SYNCING_UIDS );
            Q_ASSERT( mailboxIndex.isValid() ); // FIXME
            TreeItemMailbox* mailbox = dynamic_cast<TreeItemMailbox*>( static_cast<TreeItem*>( mailboxIndex.internalPointer() ));
            Q_ASSERT( mailbox );
            switch ( uidSyncingMode ) {
            case UID_SYNC_ONLY_NEW:
                _finalizeUidSyncOnlyNew( mailbox );
                break;
            default:
                _finalizeUidSyncAll( mailbox );
            }
            syncFlags( mailbox );
        } else {
            // FIXME: error handling
        }
        IMAP_TASK_CLEANUP_COMMAND;
        return true;
    } else if ( resp->tag == flagsCmd ) {
        IMAP_TASK_ENSURE_VALID_COMMAND( flagsCmd, Model::Task::FETCH_FLAGS );

        if ( resp->kind == Responses::OK ) {
            qDebug() << "received OK for flagsCmd";
            Q_ASSERT( status == STATE_SYNCING_FLAGS );
            Q_ASSERT( mailboxIndex.isValid() ); // FIXME
            TreeItemMailbox* mailbox = dynamic_cast<TreeItemMailbox*>( static_cast<TreeItem*>( mailboxIndex.internalPointer() ));
            Q_ASSERT( mailbox );
        } else {
            // FIXME: error handling
        }
        status = STATE_DONE;
        qDebug() << "STATE_DONE";
        _completed();
        IMAP_TASK_CLEANUP_COMMAND;
        return true;
    } else {
        return false;
    }
}

void ObtainSynchronizedMailboxTask::_finalizeSelect()
{
    TreeItemMailbox* mailbox = dynamic_cast<TreeItemMailbox*>( static_cast<TreeItem*>( mailboxIndex.internalPointer() ));
    Q_ASSERT(mailbox);
    TreeItemMsgList* list = dynamic_cast<TreeItemMsgList*>( mailbox->_children[ 0 ] );
    Q_ASSERT( list );

    model->_parsers[ parser ].mailbox = mailbox;
    model->changeConnectionState( parser, CONN_STATE_SELECTED );
    const SyncState& syncState = mailbox->syncState;
    const SyncState& oldState = model->cache()->mailboxSyncState( mailbox->mailbox() );
    list->_totalMessageCount = syncState.exists();
    // Note: syncState.unSeen() is the NUMBER of the first unseen message, not their count!

    const QList<uint>& seqToUid = model->cache()->uidMapping( mailbox->mailbox() );

    if ( static_cast<uint>( seqToUid.size() ) != oldState.exists() ||
         oldState.exists() != static_cast<uint>( list->_children.size() ) ) {

        qDebug() << "Inconsistent cache data, falling back to full sync (" <<
                seqToUid.size() << "in UID map," << oldState.exists() <<
                "EXIST before," << list->_children.size() << "nodes)";
        _fullMboxSync( mailbox, list, syncState );
    } else {
        if ( syncState.isUsableForSyncing() && oldState.isUsableForSyncing() && syncState.uidValidity() == oldState.uidValidity() ) {
            // Perform a nice re-sync

            if ( syncState.uidNext() == oldState.uidNext() ) {
                // No new messages

                if ( syncState.exists() == oldState.exists() ) {
                    // No deletions, either, so we resync only flag changes
                    _syncNoNewNoDeletions( mailbox, list, syncState, seqToUid );
                } else {
                    // Some messages got deleted, but there have been no additions
                    //_syncOnlyDeletions( mailbox, list, syncState );
                    _fullMboxSync( mailbox, list, syncState ); return; // FIXME: change later
                }

            } else {
                // Some new messages were delivered since we checked the last time.
                // There's no guarantee they are still present, though.

                if ( syncState.uidNext() - oldState.uidNext() == syncState.exists() - oldState.exists() ) {
                    // Only some new arrivals, no deletions
                    //_syncOnlyAdditions( mailbox, list, syncState, oldState );
                    _fullMboxSync( mailbox, list, syncState ); return; // FIXME: change later
                } else {
                    // Generic case; we don't know anything about which messages were deleted and which added
                    //_syncGeneric( mailbox, list, syncState );
                    _fullMboxSync( mailbox, list, syncState ); return; // FIXME: change later
                }
            }
        } else {
            // Forget everything, do a dumb sync
            model->cache()->clearAllMessages( mailbox->mailbox() );
            _fullMboxSync( mailbox, list, syncState );
        }
    }
}

void ObtainSynchronizedMailboxTask::_fullMboxSync( TreeItemMailbox* mailbox, TreeItemMsgList* list, const SyncState& syncState )
{
    qDebug() << Q_FUNC_INFO;
    model->cache()->clearUidMapping( mailbox->mailbox() );

    QModelIndex parent = model->createIndex( 0, 0, list );
    if ( ! list->_children.isEmpty() ) {
        model->beginRemoveRows( parent, 0, list->_children.size() - 1 );
        qDeleteAll( list->_children );
        list->_children.clear();
        model->endRemoveRows();
    }
    if ( syncState.exists() ) {
        bool willLoad = model->networkPolicy() == Model::NETWORK_ONLINE && syncState.exists() <= Model::StructureFetchLimit;
        model->beginInsertRows( parent, 0, syncState.exists() - 1 );
        for ( uint i = 0; i < syncState.exists(); ++i ) {
            TreeItemMessage* message = new TreeItemMessage( list );
            message->_offset = i;
            list->_children << message;
            // FIXME: re-evaluate this one when Task migration's done
            //if ( willLoad )
            //    message->_fetchStatus = TreeItem::LOADING;
        }
        model->endInsertRows();

        syncUids( mailbox );

        // FIXME: re-enable optimization (merge UID and FLAGS syncing) when Task migration's done
        list->_numberFetchingStatus = TreeItem::LOADING;
        list->_unreadMessageCount = 0;
    } else {
        list->_totalMessageCount = 0;
        list->_unreadMessageCount = 0;
        list->_numberFetchingStatus = TreeItem::DONE;
        list->_fetchStatus = TreeItem::DONE;
        model->cache()->setMailboxSyncState( mailbox->mailbox(), syncState );
        model->saveUidMap( list );

        // The remote mailbox is empty -> we're done now
        status = STATE_DONE;
        qDebug() << "STATE_DONE";
        _completed();
    }
    model->emitMessageCountChanged( mailbox );
}

void ObtainSynchronizedMailboxTask::_syncNoNewNoDeletions( TreeItemMailbox* mailbox, TreeItemMsgList* list, const SyncState& syncState, const QList<uint>& seqToUid )
{
    qDebug() << Q_FUNC_INFO;

    if ( syncState.exists() ) {
        // Verify that we indeed have all UIDs and not need them anymore
        bool uidsOk = true;
        for ( int i = 0; i < list->_children.size(); ++i ) {
            if ( ! static_cast<TreeItemMessage*>( list->_children[i] )->uid() ) {
                uidsOk = false;
                break;
            }
        }
        Q_ASSERT( uidsOk );
    } else {
        list->_unreadMessageCount = 0;
        list->_totalMessageCount = 0;
        list->_numberFetchingStatus = TreeItem::DONE;
    }

    if ( list->_children.isEmpty() ) {
        QList<TreeItem*> messages;
        for ( uint i = 0; i < syncState.exists(); ++i ) {
            TreeItemMessage* msg = new TreeItemMessage( list );
            msg->_offset = i;
            msg->_uid = seqToUid[ i ];
            messages << msg;
        }
        list->setChildren( messages );

    } else {
        if ( syncState.exists() != static_cast<uint>( list->_children.size() ) ) {
            throw CantHappen( "TreeItemMsgList has wrong number of "
                              "children, even though no change of "
                              "message count occured" );
        }
    }

    list->_fetchStatus = TreeItem::DONE;
    model->cache()->setMailboxSyncState( mailbox->mailbox(), syncState );
    model->saveUidMap( list );

    if ( syncState.exists() ) {
        syncFlags( mailbox );
    } else {
        status = STATE_DONE;
        qDebug() << "STATE_DONE";
        _completed();
    }
}

void ObtainSynchronizedMailboxTask::_syncOnlyDeletions( TreeItemMailbox* mailbox, TreeItemMsgList* list, const SyncState& syncState )
{
    qDebug() << Q_FUNC_INFO;

    list->_numberFetchingStatus = TreeItem::LOADING;
    list->_unreadMessageCount = 0;
    QList<uint>& uidMap = model->_parsers[ parser ].uidMap;
    uidMap.clear();
    for ( uint i = 0; i < syncState.exists(); ++i )
        uidMap << 0;
    model->cache()->clearUidMapping( mailbox->mailbox() );
    syncUids( mailbox );
}

void ObtainSynchronizedMailboxTask::_syncOnlyAdditions( TreeItemMailbox* mailbox, TreeItemMsgList* list, const SyncState& syncState, const SyncState& oldState )
{
    qDebug() << Q_FUNC_INFO;

    Q_UNUSED(syncState);

    // So, we know that messages only got added to the mailbox and that none were removed,
    // neither those that we already know or those that got added while we weren't around.
    // Therefore we ask only for UIDs of new messages

    list->_numberFetchingStatus = TreeItem::LOADING;
    list->_unreadMessageCount = 0;
    uidSyncingMode = UID_SYNC_ONLY_NEW;
    syncUids( mailbox, oldState.uidNext() );
}

void ObtainSynchronizedMailboxTask::_syncGeneric( TreeItemMailbox* mailbox, TreeItemMsgList* list, const SyncState& syncState )
{
    qDebug() << Q_FUNC_INFO;
    // FIXME: might be possible to optimize here...

    /*// At first, let's ask for UID numbers and FLAGS for all messages
    CommandHandle cmd = parser->fetch( Sequence( 1, syncState.exists() ),
                                       QStringList() << "UID" << "FLAGS" );
    model->_parsers[ parser ].commandMap[ cmd ] = Model::Task( Model::Task::FETCH_WITH_FLAGS, mailbox );
    emit model->activityHappening( true );
    model->_parsers[ parser ].responseHandler = model->selectingHandler;*/
    list->_numberFetchingStatus = TreeItem::LOADING;
    list->_unreadMessageCount = 0;
    QList<uint>& uidMap = model->_parsers[ parser ].uidMap;
    uidMap.clear();
    for ( uint i = 0; i < syncState.exists(); ++i )
        uidMap << 0;
    model->cache()->clearUidMapping( mailbox->mailbox() );
    syncUids( mailbox );
}

void ObtainSynchronizedMailboxTask::syncUids( TreeItemMailbox* mailbox, const uint lowestUidToQuery )
{
    qDebug() << Q_FUNC_INFO;

    QString uidSpecification;
    if ( lowestUidToQuery == 0 ) {
        uidSpecification = QLatin1String("ALL");
    } else {
        uidSpecification = QString::fromAscii("UID %1:*").arg( lowestUidToQuery );
    }
    uidSyncingCmd = parser->uidSearchUid( uidSpecification );
    model->_parsers[ parser ].commandMap[ uidSyncingCmd ] = Model::Task( Model::Task::SEARCH_UIDS, 0 );
    emit model->activityHappening( true );
    model->cache()->clearUidMapping( mailbox->mailbox() );
    status = STATE_SYNCING_UIDS;
    qDebug() << "STATE_SYNCING_UIDS";
}

void ObtainSynchronizedMailboxTask::syncFlags( TreeItemMailbox *mailbox )
{
    qDebug() << Q_FUNC_INFO;

    TreeItemMsgList* list = dynamic_cast<TreeItemMsgList*>( mailbox->_children[ 0 ] );
    Q_ASSERT( list );

    flagsCmd = parser->fetch( Sequence( 1, mailbox->syncState.exists() ), QStringList() << QLatin1String("FLAGS") );
    model->_parsers[ parser ].commandMap[ flagsCmd ] = Model::Task( Model::Task::FETCH_FLAGS, 0 );
    emit model->activityHappening( true );
    list->_numberFetchingStatus = TreeItem::LOADING;
    list->_unreadMessageCount = 0;
    status = STATE_SYNCING_FLAGS;
    qDebug() << "STATE_SYNCING_FLAGS";
}

bool ObtainSynchronizedMailboxTask::handleResponseCodeInsideState( const Imap::Responses::State* const resp )
{
    bool res = false;
    switch ( resp->respCode ) {
        case Responses::UNSEEN:
        {
            const Responses::RespData<uint>* const num = dynamic_cast<const Responses::RespData<uint>* const>( resp->respCodeData.data() );
            if ( num ) {
                model->_parsers[ parser ].mailbox->syncState.setUnSeen( num->data );
                res = true;
            } else {
                throw CantHappen( "State response has invalid UNSEEN respCodeData", *resp );
            }
            break;
        }
        case Responses::PERMANENTFLAGS:
        {
            const Responses::RespData<QStringList>* const num = dynamic_cast<const Responses::RespData<QStringList>* const>( resp->respCodeData.data() );
            if ( num ) {
                model->_parsers[ parser ].mailbox->syncState.setPermanentFlags( num->data );
                res = true;
            } else {
                throw CantHappen( "State response has invalid PERMANENTFLAGS respCodeData", *resp );
            }
            break;
        }
        case Responses::UIDNEXT:
        {
            const Responses::RespData<uint>* const num = dynamic_cast<const Responses::RespData<uint>* const>( resp->respCodeData.data() );
            if ( num ) {
                model->_parsers[ parser ].mailbox->syncState.setUidNext( num->data );
                res = true;
            } else {
                throw CantHappen( "State response has invalid UIDNEXT respCodeData", *resp );
            }
            break;
        }
        case Responses::UIDVALIDITY:
        {
            const Responses::RespData<uint>* const num = dynamic_cast<const Responses::RespData<uint>* const>( resp->respCodeData.data() );
            if ( num ) {
                model->_parsers[ parser ].mailbox->syncState.setUidValidity( num->data );
                res = true;
            } else {
                throw CantHappen( "State response has invalid UIDVALIDITY respCodeData", *resp );
            }
            break;
        }
        case Responses::CLOSED:
        case Responses::HIGHESTMODSEQ:
            // FIXME: handle when supporting the qresync
            res = true;
            break;
        default:
            break;
    }
    return res;
}

bool ObtainSynchronizedMailboxTask::handleNumberResponse( Imap::Parser* ptr, const Imap::Responses::NumberResponse* const resp )
{
    Q_ASSERT( ptr == parser );
    switch ( resp->kind ) {
        case Imap::Responses::EXISTS:
            model->_parsers[ ptr ].mailbox->syncState.setExists( resp->number );
            return true;
            break;
        case Imap::Responses::EXPUNGE:
            // must be handled elsewhere
            break;
        case Imap::Responses::RECENT:
            model->_parsers[ ptr ].mailbox->syncState.setRecent( resp->number );
            return true;
            break;
        default:
            throw CantHappen( "Got a NumberResponse of invalid kind. This is supposed to be handled in its constructor!", *resp );
    }
    return false;
}

bool ObtainSynchronizedMailboxTask::handleFlags( Imap::Parser* ptr, const Imap::Responses::Flags* const resp )
{
    Q_ASSERT( ptr == parser );
    model->_parsers[ ptr ].mailbox->syncState.setFlags( resp->flags );
    return true;
}

bool ObtainSynchronizedMailboxTask::handleSearch( Imap::Parser* ptr, const Imap::Responses::Search* const resp )
{
    Q_ASSERT( ptr == parser );
    if ( static_cast<uint>( resp->items.size() ) != model->_parsers[ ptr ].mailbox->syncState.exists() ) {
        throw MailboxException( "UID SEARCH ALL returned unexpected number of enitres", *resp );
    }
    model->_parsers[ ptr ].uidMap = resp->items;
    return true;
}

bool ObtainSynchronizedMailboxTask::handleFetch( Imap::Parser* ptr, const Imap::Responses::Fetch* const resp )
{
    Q_ASSERT ( ptr == parser );
    model->_parsers[ ptr ].mailbox->handleFetchWhileSyncing( model, ptr, *resp );
    return true;
}

/** @short Generic handler for UID syncing

The responsibility of this function is to process the "UID replies" triggered
by previous phases of the syncing activity and use them in order to assign all
messages their UIDs.

This function assumes that we're doing a so-called "full UID sync", ie. we were
forced to ask the server for a full list of all UIDs in a mailbox. This is the
safest, but also the most bandwidth-hungry way to achive our goal.
*/
void ObtainSynchronizedMailboxTask::_finalizeUidSyncAll( TreeItemMailbox* mailbox )
{
    QList<uint>& uidMap = model->_parsers[ parser ].uidMap;

    // Verify that we indeed received UIDs for all messages
    if ( static_cast<uint>( uidMap.size() ) != mailbox->syncState.exists() ) {
        throw MailboxException( "We received a weird number of UIDs for messages in the mailbox.");
    }

    // Store stuff we already have in the cache
    model->cache()->setMailboxSyncState( mailbox->mailbox(), mailbox->syncState );
    model->cache()->setUidMapping( mailbox->mailbox(), uidMap );

    TreeItemMsgList* list = dynamic_cast<TreeItemMsgList*>( mailbox->_children[0] );
    Q_ASSERT( list );
    list->_fetchStatus = TreeItem::DONE;

    QModelIndex parent = model->createIndex( 0, 0, list );

    if ( uidMap.isEmpty() ) {
        // the mailbox is empty
        if ( list->_children.size() > 0 ) {
            model->beginRemoveRows( parent, 0, list->_children.size() - 1 );
            qDeleteAll( list->setChildren( QList<TreeItem*>() ) );
            model->endRemoveRows();
            model->cache()->clearAllMessages( mailbox->mailbox() );
        }
    } else {
        // some messages are present *now*; they might or might not have been there before
        int pos = 0;
        for ( int i = 0; i < uidMap.size(); ++i ) {
            if ( i >= list->_children.size() ) {
                // now we're just adding new messages to the end of the list
                model->beginInsertRows( parent, i, i );
                TreeItemMessage * msg = new TreeItemMessage( list );
                msg->_offset = i;
                msg->_uid = uidMap[ i ];
                list->_children << msg;
                model->endInsertRows();
            } else if ( dynamic_cast<TreeItemMessage*>( list->_children[pos] )->_uid == uidMap[ i ] ) {
                // current message has correct UID
                dynamic_cast<TreeItemMessage*>( list->_children[pos] )->_offset = i;
                continue;
            } else {
                // Traverse the messages we have in the cache, checking their UIDs. The idea here
                // is that we should be deleting all messages with UIDs different from the current
                // "supposed UID" (ie. uidMap[i]); this is a valid behavior per the IMAP standard.
                int pos = i;
                bool found = false;
                while ( pos < list->_children.size() ) {
                    TreeItemMessage* message = dynamic_cast<TreeItemMessage*>( list->_children[pos] );
                    if ( message->_uid != uidMap[ i ] ) {
                        // this message is free to go
                        model->cache()->clearMessage( mailbox->mailbox(), message->uid() );
                        model->beginRemoveRows( parent, pos, pos );
                        delete list->_children.takeAt( pos );
                        // the _offset of all subsequent messages will be updated later
                        model->endRemoveRows();
                    } else {
                        // this message is the correct one -> keep it, go to the next UID
                        found = true;
                        message->_offset = i;
                        break;
                    }
                }
                if ( ! found ) {
                    // Add one message, continue the loop
                    Q_ASSERT( pos == list->_children.size() ); // we're at the end of the list
                    model->beginInsertRows( parent, i, i );
                    TreeItemMessage * msg = new TreeItemMessage( list );
                    msg->_uid = uidMap[ i ];
                    msg->_offset = i;
                    list->_children << msg;
                    model->endInsertRows();
                }
            }
        }
        if ( uidMap.size() != list->_children.size() ) {
            // remove items at the end
            model->beginRemoveRows( parent, uidMap.size(), list->_children.size() - 1 );
            for ( int i = uidMap.size(); i < list->_children.size(); ++i ) {
                TreeItemMessage* message = static_cast<TreeItemMessage*>( list->_children.takeAt( i ) );
                model->cache()->clearMessage( mailbox->mailbox(), message->uid() );
                delete message;
            }
            model->endRemoveRows();
        }
    }

    uidMap.clear();

    int unSeenCount = 0;
    for ( QList<TreeItem*>::const_iterator it = list->_children.begin();
          it != list->_children.end(); ++it ) {
        TreeItemMessage* message = dynamic_cast<TreeItemMessage*>( *it );
        Q_ASSERT( message );
        if ( message->_uid != 0 ) {
            /*throw CantHappen("Port in progress: we shouldn't have to deal with syncingFlags here, sorry.");
            message->_flags = _parsers[ parser ].syncingFlags[ message->_uid ];
            if ( message->uid() )
                cache()->setMsgFlags( mailbox->mailbox(), message->uid(), message->_flags );
            if ( ! message->isMarkedAsRead() )
                ++unSeenCount;
            message->_flagsHandled = true;
            QModelIndex index = createIndex( message->row(), 0, message );
            emit dataChanged( index, index );*/
        }
    }
    list->_totalMessageCount = list->_children.size();
    list->_unreadMessageCount = unSeenCount;
    list->_numberFetchingStatus = TreeItem::DONE;
    model->saveUidMap( list );
    model->emitMessageCountChanged( mailbox );
    model->changeConnectionState( parser, CONN_STATE_SELECTED );
}

/** @short Process delivered UIDs for new messages

This function is similar to _finalizeUidSyncAll, except that it operates on
a smaller set of messages.
*/
void ObtainSynchronizedMailboxTask::_finalizeUidSyncOnlyNew( TreeItemMailbox* mailbox )
{
    qDebug() << Q_FUNC_INFO;

    const SyncState& oldState = model->cache()->mailboxSyncState( mailbox->mailbox() );
    QList<uint>& uidMap = model->_parsers[ parser ].uidMap;

    TreeItemMsgList* list = dynamic_cast<TreeItemMsgList*>( mailbox->_children[0] );
    Q_ASSERT( list );
    list->_fetchStatus = TreeItem::DONE;

    // This invariant is set up in Model::_askForMessagesInMailbox
    Q_ASSERT( oldState.exists() == static_cast<uint>( list->_children.size() ) );

    // Be sure there really are some new messages
    Q_ASSERT( mailbox->syncState.exists() > oldState.exists() );

    QModelIndex parent = model->createIndex( 0, 0, list );
    int offset = list->_children.size();
    qDebug() << "beginRemoveRows( ..." << offset << mailbox->syncState.exists() - 1;
    model->beginInsertRows( parent, offset, mailbox->syncState.exists() - 1 );
    for ( int i = 0; i < uidMap.size(); ++i ) {
        TreeItemMessage * msg = new TreeItemMessage( list );
        msg->_offset = i + offset;
        msg->_uid = uidMap[ i ];
        list->_children << msg;
        qDebug() << "Adding message with UID" << msg->_uid;
    }
    model->endInsertRows();

    uidMap.clear();
    for ( int i = 0; i < list->_children.size(); ++i ) {
        uint uid = static_cast<TreeItemMessage*>( list->_children[ i ] )->_uid;
        qDebug() << "Message" << i << "has UID" << uid;
        Q_ASSERT( uid );
        uidMap << uid;
    }

    list->_totalMessageCount = list->_children.size();
    list->_unreadMessageCount = 0; // FIXME
    list->_numberFetchingStatus = TreeItem::DONE; // FIXME: they aren't done yet
    model->saveUidMap( list );
    model->cache()->setUidMapping( mailbox->mailbox(), uidMap );
    uidMap.clear();
    model->emitMessageCountChanged( mailbox );
    model->changeConnectionState( parser, CONN_STATE_SELECTED );
}

}
}