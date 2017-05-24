/**
 *    Copyright 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/op_observer_impl.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;

ServiceContext::UniqueOperationContext makeOpCtx() {
    return cc().makeOperationContext();
}

class DatabaseTest : public ServiceContextMongoDTest {
private:
    void setUp() override;
    void tearDown() override;

protected:
    ServiceContext::UniqueOperationContext _opCtx;
    NamespaceString _nss;
};

void DatabaseTest::setUp() {
    // Set up mongod.
    ServiceContextMongoDTest::setUp();

    auto service = getServiceContext();
    _opCtx = cc().makeOperationContext();

    // Set up ReplicationCoordinator and create oplog.
    repl::ReplicationCoordinator::set(service,
                                      stdx::make_unique<repl::ReplicationCoordinatorMock>(service));
    repl::setOplogCollectionName();
    repl::createOplog(_opCtx.get());

    // Ensure that we are primary.
    auto replCoord = repl::ReplicationCoordinator::get(_opCtx.get());
    ASSERT_TRUE(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));

    // Set up OpObserver so that Database will append actual oplog entries to the oplog using
    // repl::logOp(). repl::logOp() will also store the oplog entry's optime in ReplClientInfo.
    service->setOpObserver(stdx::make_unique<OpObserverImpl>());

    _nss = NamespaceString("test.foo");
}

void DatabaseTest::tearDown() {
    _nss = {};
    _opCtx = {};
    ServiceContextMongoDTest::tearDown();
}

void _testDropCollection(OperationContext* opCtx,
                         const NamespaceString& nss,
                         bool createCollectionBeforeDrop) {
    writeConflictRetry(
        opCtx, "testDropCollection", nss.ns(), [opCtx, nss, createCollectionBeforeDrop] {
            AutoGetOrCreateDb autoDb(opCtx, nss.db(), MODE_X);
            auto db = autoDb.getDb();
            ASSERT_TRUE(db);

            WriteUnitOfWork wuow(opCtx);
            if (createCollectionBeforeDrop) {
                ASSERT_TRUE(db->createCollection(opCtx, nss.ns()));
            } else {
                ASSERT_FALSE(db->getCollection(opCtx, nss));
            }

            ASSERT_OK(db->dropCollection(opCtx, nss.ns()));

            ASSERT_FALSE(db->getCollection(opCtx, nss));
            wuow.commit();
        });
}

TEST_F(DatabaseTest, DropCollectionReturnsOKIfCollectionDoesNotExist) {
    _testDropCollection(_opCtx.get(), _nss, false);
    // Check last optime for this client to ensure no entries were appended to the oplog.
    ASSERT_EQUALS(repl::OpTime(), repl::ReplClientInfo::forClient(&cc()).getLastOp());
}

TEST_F(DatabaseTest, DropCollectionDropsCollectionButDoesNotLogOperationIfWritesAreNotReplicated) {
    repl::UnreplicatedWritesBlock uwb(_opCtx.get());
    ASSERT_FALSE(_opCtx->writesAreReplicated());
    ASSERT_TRUE(
        repl::ReplicationCoordinator::get(_opCtx.get())->isOplogDisabledFor(_opCtx.get(), _nss));

    _testDropCollection(_opCtx.get(), _nss, true);

    // Drop optime is null because no op was written to the oplog.
    auto dropOpTime = repl::ReplClientInfo::forClient(&cc()).getLastOp();
    ASSERT_EQUALS(repl::OpTime(), dropOpTime);
}

TEST_F(DatabaseTest, DropCollectionDropsCollectionAndLogsOperationIfWritesAreReplicated) {
    ASSERT_TRUE(_opCtx->writesAreReplicated());
    ASSERT_FALSE(
        repl::ReplicationCoordinator::get(_opCtx.get())->isOplogDisabledFor(_opCtx.get(), _nss));

    _testDropCollection(_opCtx.get(), _nss, true);

    // Drop optime is non-null because an op was written to the oplog.
    auto dropOpTime = repl::ReplClientInfo::forClient(&cc()).getLastOp();
    ASSERT_GREATER_THAN(dropOpTime, repl::OpTime());
}

}  // namespace