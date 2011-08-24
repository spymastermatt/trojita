/* Copyright (C) 2006 - 2011 Jan Kundrát <jkt@gentoo.org>

   This file is part of the Trojita Qt IMAP e-mail client,
   http://trojita.flaska.net/

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public
   License as published by the Free Software Foundation; either
   version 2 of the License, or the version 3 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; see the file COPYING.  If not, write to
   the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
   Boston, MA 02110-1301, USA.
*/

#include <QtTest>
#include "test_Imap_Threading.h"
#include "../headless_test.h"
#include "Imap/Model/MsgListModel.h"
#include "Imap/Model/ThreadingMsgListModel.h"
#include "Streams/FakeSocket.h"
#include "test_LibMailboxSync/FakeCapabilitiesInjector.h"

Q_DECLARE_METATYPE(Mapping);

/** @short Test that the ThreadingMsgListModel can process a static THREAD response */
void ImapModelThreadingTest::testStaticThreading()
{
    initialMessages();

    QFETCH(QByteArray, response);
    QFETCH(Mapping, mapping);
    QCOMPARE(SOCK->writtenStuff(), t.mk("UID THREAD REFS utf-8 ALL\r\n"));
    SOCK->fakeReading(QByteArray("* THREAD ") + response + QByteArray("\r\n") + t.last("OK thread\r\n"));
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    verifyMapping(mapping);
    QVERIFY(SOCK->writtenStuff().isEmpty());
    QVERIFY(errorSpy->isEmpty());
}

/** @short Data for the testStaticThreading */
void ImapModelThreadingTest::testStaticThreading_data()
{
    QTest::addColumn<QByteArray>("response");
    QTest::addColumn<Mapping>("mapping");

    Mapping m;

    // A linear subset of messages
    m["0"] = 1; // index 0: UID 1
    m["0.0"] = 0; // index 0.0: invalid
    m["0.1"] = 0; // index 0.1: invalid
    m["1"] = 2;
    QTest::newRow("no-threads")
            << QByteArray("(1)(2)")
            << m;

    // No threading at all; just an unthreaded list of all messages
    m["2"] = 3;
    m["3"] = 4;
    m["4"] = 5;
    m["5"] = 6;
    m["6"] = 7;
    m["7"] = 8;
    m["8"] = 9;
    m["9"] = 10; // index 9: UID 10
    m["10"] = 0; // index 10: invalid
    QTest::newRow("no-threads")
            << QByteArray("(1)(2)(3)(4)(5)(6)(7)(8)(9)(10)")
            << m;

    // A flat list of threads, but now with some added fake nodes for complexity.
    // The expected result is that they get cleared as redundant and useless nodes.
    QTest::newRow("extra-parentheses")
            << QByteArray("(1)((2))(((3)))((((4))))(((((5)))))(6)(7)(8)(9)(((10)))")
            << m;

    // A liner nested list (ie. a message is a child of the previous one)
    m.clear();
    m["0"] = 1;
    m["1"] = 0;
    m["0.0"] = 2;
    m["0.1"] = 0;
    m["0.0.0"] = 0;
    m["0.0.1"] = 0;
    QTest::newRow("linear-threading-just-two")
            << QByteArray("(1 2)")
            << m;

    // The same, but with three messages
    m["0.0.0"] = 3;
    m["0.0.0.0"] = 0;
    QTest::newRow("linear-threading-just-three")
            << QByteArray("(1 2 3)")
            << m;

    // The same, but with some added parentheses
    m["0.0.0"] = 3;
    m["0.0.0.0"] = 0;
    QTest::newRow("linear-threading-just-three-extra-parentheses-outside")
            << QByteArray("((((1 2 3))))")
            << m;
    // The same, but with the extra parentheses in the innermost item
    QTest::newRow("linear-threading-just-three-extra-parentheses-inside")
            << QByteArray("(1 2 (((3))))")
            << m;

    // The same, but with the extra parentheses in the middle item
    // This is actually a server's bug, as the fake node should've been eliminated
    // by the IMAP server.
    m.clear();
    m["0"] = 1;
    m["1"] = 0;
    m["0.0"] = 2;
    m["0.0.0"] = 0;
    m["0.1"] = 3;
    m["0.1.0"] = 0;
    m["0.2"] = 0;
    QTest::newRow("linear-threading-just-three-extra-parentheses-middle")
            << QByteArray("(1 (2) 3)")
            << m;

    // A complex nested hierarchy with nodes to be promoted
    QByteArray response;
    complexMapping(m, response);
    QTest::newRow("complex-threading")
            << response
            << m;
}

/** @short Test deletion of one message */
void ImapModelThreadingTest::testDynamicThreading()
{
    initialMessages();

    // A complex nested hierarchy with nodes to be promoted
    Mapping mapping;
    QByteArray response;
    complexMapping(mapping, response);

    QCOMPARE(SOCK->writtenStuff(), t.mk("UID THREAD REFS utf-8 ALL\r\n"));
    SOCK->fakeReading(QByteArray("* THREAD ") + response + QByteArray("\r\n") + t.last("OK thread\r\n"));
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    verifyMapping(mapping);
    QCOMPARE(threadingModel->rowCount(QModelIndex()), 4);
    IndexMapping indexMap = buildIndexMap(mapping);
    verifyIndexMap(indexMap, mapping);

    // this one will be deleted
    QPersistentModelIndex delete10 = findItem("3.1.0");
    QVERIFY(delete10.isValid());

    // its parent
    QPersistentModelIndex msg9 = findItem("3.1");
    QCOMPARE(QPersistentModelIndex(delete10.parent()), msg9);
    QCOMPARE(threadingModel->rowCount(msg9), 1);

    // Delete the last message; it's some leaf
    SOCK->fakeReading("* 10 EXPUNGE\r\n");
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    --existsA;
    QCOMPARE(msgListModel->rowCount(QModelIndex()), static_cast<int>(existsA));
    QCOMPARE(threadingModel->rowCount(msg9), 0);
    QVERIFY(!delete10.isValid());
    mapping.remove("3.1.0.0");
    mapping["3.1.0"] = 0;
    indexMap.remove("3.1.0.0");
    verifyMapping(mapping);
    verifyIndexMap(indexMap, mapping);

    QPersistentModelIndex msg2 = findItem("1");
    QVERIFY(msg2.isValid());
    QPersistentModelIndex msg3 = findItem("1.0");
    QVERIFY(msg3.isValid());

    // Delete the root of the second thread
    SOCK->fakeReading("* 2 EXPUNGE\r\n");
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    --existsA;
    QCOMPARE(msgListModel->rowCount(QModelIndex()), static_cast<int>(existsA));
    QCOMPARE(threadingModel->rowCount(QModelIndex()), 4);
    QPersistentModelIndex newMsg3 = findItem("1");
    QVERIFY(!msg2.isValid());
    QVERIFY(msg3.isValid());
    QCOMPARE(msg3, newMsg3);
    mapping.remove("1.0.0");
    mapping["1.0"] = 0;
    mapping["1"] = 3;
    verifyMapping(mapping);
    // Check the changed persistent indexes
    indexMap.remove("1.0.0");
    indexMap["1"] = indexMap["1.0"];
    indexMap.remove("1.0");
    verifyIndexMap(indexMap, mapping);

    // Push a new message, but with an unknown UID so far
    ++existsA;
    ++uidNextA;
    QCOMPARE(existsA, 9u);
    QCOMPARE(uidNextA, 67u);
    SOCK->fakeReading("* 9 EXISTS\r\n");
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();

    QByteArray fetchCommand1 = t.mk("UID FETCH 10:* (FLAGS)\r\n");
    QByteArray delayedFetchResponse1 = t.last("OK uid fetch\r\n");
    QByteArray threadCommand1 = t.mk("UID THREAD REFS utf-8 ALL\r\n");
    QByteArray delayedThreadResponse1 = t.last("OK threading\r\n");
    QCOMPARE(SOCK->writtenStuff(), fetchCommand1 + threadCommand1);

    QByteArray fetchUntagged1("* 9 FETCH (UID 11 FLAGS (\\Recent))\r\n");
    QByteArray threadUntagged1("* THREAD (1)(3)(4 (5)(6))((7)(8)(9))\r\n");

    // Check that we've registered that change
    QCOMPARE(msgListModel->rowCount(QModelIndex()), static_cast<int>(existsA));

    if (1) {
        SOCK->fakeReading(fetchUntagged1 + delayedFetchResponse1 + threadUntagged1 + delayedThreadResponse1);
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
        mapping["4"] = 11;
        indexMap["4"] = findItem("4");
        /*verifyMapping(mapping);
        verifyIndexMap(indexMap, mapping);*/
    }

    qDebug() << SOCK->writtenStuff();

    QVERIFY(SOCK->writtenStuff().isEmpty());
    QVERIFY(errorSpy->isEmpty());
}

/** @short Create a tuple of (mapping, string)*/
void ImapModelThreadingTest::complexMapping(Mapping &m, QByteArray &response)
{
    m.clear();
    m["0"] = 1;
    m["0.0"] = 0;
    m["1"] = 2;
    m["1.0"] = 3;
    m["1.0.0"] = 0;
    m["2"] = 4;
    m["2.0"] = 5;
    m["2.0.0"] = 0;
    m["2.1"] = 6;
    m["2.1.0"] = 0;
    m["3"] = 7;
    m["3.0"] = 8;
    m["3.0.0"] = 0;
    m["3.1"] = 9;
    m["3.1.0"] = 10;
    m["3.1.0.0"] = 0;
    m["3.2"] = 0;
    m["4"] = 0;
    response = QByteArray("(1)(2 3)(4 (5)(6))((7)(8)(9 10))");
}

/** @short Prepare an index to a threaded message */
QModelIndex ImapModelThreadingTest::findItem(const QList<int> &where)
{
    QModelIndex index = QModelIndex();
    for (QList<int>::const_iterator it = where.begin(); it != where.end(); ++it) {
        index = threadingModel->index(*it, 0, index);
        if (it + 1 != where.end()) {
            // this index is an intermediate one in the path, hence it should not really fail
            Q_ASSERT(index.isValid());
        }
    }
    return index;
}

/** @short Prepare an index to a threaded message based on a compressed text index description */
QModelIndex ImapModelThreadingTest::findItem(const QString &where)
{
    QStringList list = where.split(QLatin1Char('.'));
    Q_ASSERT(!list.isEmpty());
    QList<int> items;
    Q_FOREACH(const QString number, list) {
        bool ok;
        items << number.toInt(&ok);
        Q_ASSERT(ok);
    }
    return findItem(items);
}

/** @short Make sure that the specified indexes resolve to proper UIDs */
void ImapModelThreadingTest::verifyMapping(const Mapping &mapping)
{
    for(Mapping::const_iterator it = mapping.begin(); it != mapping.end(); ++it) {
        QModelIndex index = findItem(it.key());
        if (it.value()) {
            // it's a supposedly valid index
            if (!index.isValid()) {
                qDebug() << "Invalid index at" << it.key();
            }
            QVERIFY(index.isValid());
            int got = index.data(Imap::Mailbox::RoleMessageUid).toInt();
            if (got != it.value()) {
                qDebug() << "Index" << it.key();
            }
            QCOMPARE(got, it.value());
        } else {
            // we expect this one to be a fake
            if (index.isValid()) {
                qDebug() << "Valid index at" << it.key();
            }
            QVERIFY(!index.isValid());
        }
    }
}

/** @short Create a map of (position -> persistent_index) based on the current state of the model */
IndexMapping ImapModelThreadingTest::buildIndexMap(const Mapping &mapping)
{
    IndexMapping res;
    Q_FOREACH(const QString &key, mapping.keys()) {
        // only include real indexes
        res[key] = findItem(key);
    }
    return res;
}

void ImapModelThreadingTest::verifyIndexMap(const IndexMapping &indexMap, const Mapping &map)
{
    Q_FOREACH(const QString key, indexMap.keys()) {
        if (!map.contains(key)) {
            qDebug() << "Table contains an index for" << key << ", but mapping to UIDs indicates that the index should not be there. Bug in the unit test, I'd say.";
            QFAIL("Extra index found in the map");
        }
        const QPersistentModelIndex &idx = indexMap[key];
        int expected = map[key];
        if (expected) {
            if (!idx.isValid()) {
                qDebug() << "Invalid persistent index for" << key;
            }
            QVERIFY(idx.isValid());
            QCOMPARE(idx.data(Imap::Mailbox::RoleMessageUid).toInt(), expected);
        } else {
            if (idx.isValid()) {
                qDebug() << "Persistent index for" << key << "should not be valid";
            }
            QVERIFY(!idx.isValid());
        }
    }
}

void ImapModelThreadingTest::initTestCase()
{
    LibMailboxSync::initTestCase();
    msgListModel = 0;
    threadingModel = 0;
}

void ImapModelThreadingTest::init()
{
    LibMailboxSync::init();

    // Got to pretend that we support threads. Well, we really do :).
    FakeCapabilitiesInjector injector(model);
    injector.injectCapability(QLatin1String("THREAD=REFS"));

    // Setup the threading model
    msgListModel = new Imap::Mailbox::MsgListModel(this, model);
    threadingModel = new Imap::Mailbox::ThreadingMsgListModel(this);
    threadingModel->setSourceModel(msgListModel);
}

void ImapModelThreadingTest::initialMessages()
{
    // Setup ten fake messages
    existsA = 10;
    uidValidityA = 333;
    for (uint i = 1; i <= existsA; ++i) {
        uidMapA << i;
    }
    uidNextA = 66;
    helperSyncAWithMessagesEmptyState();

    // open the mailbox
    msgListModel->setMailbox(idxA);
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
}

void ImapModelThreadingTest::cleanup()
{
    LibMailboxSync::cleanup();
    threadingModel->deleteLater();
    threadingModel = 0;
    msgListModel->deleteLater();
    msgListModel = 0;
}

TROJITA_HEADLESS_TEST( ImapModelThreadingTest )