/**
 *    Copyright (C) 2019-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include <deque>
#include <limits>
#include <memory>

#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/oplog_buffer_blocking_queue.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace repl {
namespace {

/**
 * Minimal implementation of OplogApplier for testing.
 * executor::TaskExecutor is required only to test startup().
 */
class OplogApplierMock : public OplogApplier {
    OplogApplierMock(const OplogApplierMock&) = delete;
    OplogApplierMock& operator=(const OplogApplierMock&) = delete;

public:
    explicit OplogApplierMock(OplogBuffer* oplogBuffer);

    void _run(OplogBuffer* oplogBuffer) final;
    void _shutdown() final;
    StatusWith<OpTime> _multiApply(OperationContext* opCtx, Operations ops) final;
};

OplogApplierMock::OplogApplierMock(OplogBuffer* oplogBuffer)
    : OplogApplier(nullptr, oplogBuffer, nullptr) {}

void OplogApplierMock::_run(OplogBuffer* oplogBuffer) {}

void OplogApplierMock::_shutdown() {}

StatusWith<OpTime> OplogApplierMock::_multiApply(OperationContext* opCtx, Operations ops) {
    return OpTime();
}

class OplogApplierTest : public unittest::Test {
public:
    void setUp() final;
    void tearDown() final;

protected:
    std::unique_ptr<OplogBuffer> _buffer;
    std::unique_ptr<OplogApplier> _applier;
    std::unique_ptr<OperationContext> _opCtx;
    OplogApplier::BatchLimits _limits;
};

void OplogApplierTest::setUp() {
    _buffer = std::make_unique<OplogBufferBlockingQueue>(nullptr);
    _applier = std::make_unique<OplogApplierMock>(_buffer.get());
    // The OplogApplier interface expects an OperationContext* but the mock implementations in this
    // test will not be dereferencing the pointer. Therefore, it is sufficient to use an
    // OperationContextNoop.
    _opCtx = std::make_unique<OperationContextNoop>();

    _limits.bytes = std::numeric_limits<decltype(_limits.bytes)>::max();
    _limits.ops = std::numeric_limits<decltype(_limits.ops)>::max();
}

void OplogApplierTest::tearDown() {
    _limits = {};
    _opCtx = {};
    _applier = {};
    _buffer = {};
}

/**
 * Generates an insert oplog entry with the given number used for the timestamp.
 */
OplogEntry makeInsertOplogEntry(int t, const NamespaceString& nss) {
    BSONObj oField = BSON("_id" << t << "a" << t);
    return OplogEntry(OpTime(Timestamp(t, 1), 1),  // optime
                      boost::none,                 // hash
                      OpTypeEnum::kInsert,         // op type
                      nss,                         // namespace
                      boost::none,                 // uuid
                      boost::none,                 // fromMigrate
                      OplogEntry::kOplogVersion,   // version
                      oField,                      // o
                      boost::none,                 // o2
                      {},                          // sessionInfo
                      boost::none,                 // upsert
                      Date_t::min() + Seconds(t),  // wall clock time
                      boost::none,                 // statement id
                      boost::none,   // optime of previous write within same transaction
                      boost::none,   // pre-image optime
                      boost::none,   // post-image optime
                      boost::none);  // prepare
}

/**
 * Generates an applyOps oplog entry with the given number used for the timestamp.
 */
OplogEntry makeApplyOpsOplogEntry(int t, bool prepare) {
    auto nss = NamespaceString(NamespaceString::kAdminDb).getCommandNS();
    BSONObj oField = BSON("applyOps" << BSONArray());
    return OplogEntry(OpTime(Timestamp(t, 1), 1),  // optime
                      boost::none,                 // hash
                      OpTypeEnum::kCommand,        // op type
                      nss,                         // namespace
                      boost::none,                 // uuid
                      boost::none,                 // fromMigrate
                      OplogEntry::kOplogVersion,   // version
                      oField,                      // o
                      boost::none,                 // o2
                      {},                          // sessionInfo
                      boost::none,                 // upsert
                      Date_t::min() + Seconds(t),  // wall clock time
                      boost::none,                 // statement id
                      boost::none,  // optime of previous write within same transaction
                      boost::none,  // pre-image optime
                      boost::none,  // post-image optime
                      prepare);     // prepare
}

/**
 * Generates a commitTransaction oplog entry with the given number used for the timestamp.
 */
OplogEntry makeCommitTransactionOplogEntry(int t, StringData dbName, bool prepared, int count) {
    auto nss = NamespaceString(dbName).getCommandNS();
    CommitTransactionOplogObject cmdObj;
    cmdObj.setPrepared(prepared);
    cmdObj.setCount(count);
    BSONObj oField = cmdObj.toBSON();
    return OplogEntry(OpTime(Timestamp(t, 1), 1),  // optime
                      boost::none,                 // hash
                      OpTypeEnum::kCommand,        // op type
                      nss,                         // namespace
                      boost::none,                 // uuid
                      boost::none,                 // fromMigrate
                      OplogEntry::kOplogVersion,   // version
                      oField,                      // o
                      boost::none,                 // o2
                      {},                          // sessionInfo
                      boost::none,                 // upsert
                      Date_t::min() + Seconds(t),  // wall clock time
                      boost::none,                 // statement id
                      boost::none,   // optime of previous write within same transaction
                      boost::none,   // pre-image optime
                      boost::none,   // post-image optime
                      boost::none);  // prepare
}

/**
 * Returns string representation of OplogApplier::Operations.
 */
std::string toString(const OplogApplier::Operations& ops) {
    StringBuilder sb;
    sb << "[";
    for (const auto& op : ops) {
        sb << " " << op.toString();
    }
    sb << " ]";
    return sb.str();
}

constexpr auto dbName = "test"_sd;

TEST_F(OplogApplierTest, GetNextApplierBatchGroupsCrudOps) {
    OplogApplier::Operations srcOps;
    srcOps.push_back(makeInsertOplogEntry(1, NamespaceString(dbName, "foo")));
    srcOps.push_back(makeInsertOplogEntry(2, NamespaceString(dbName, "bar")));
    _applier->enqueue(_opCtx.get(), srcOps.cbegin(), srcOps.cend());

    auto batch = unittest::assertGet(_applier->getNextApplierBatch(_opCtx.get(), _limits));
    ASSERT_EQUALS(srcOps.size(), batch.size()) << toString(batch);
    ASSERT_EQUALS(srcOps[0], batch[0]);
    ASSERT_EQUALS(srcOps[1], batch[1]);
}

TEST_F(OplogApplierTest, GetNextApplierBatchReturnsPreparedApplyOpsOpInOwnBatch) {
    OplogApplier::Operations srcOps;
    srcOps.push_back(makeApplyOpsOplogEntry(1, true));
    srcOps.push_back(makeInsertOplogEntry(2, NamespaceString(dbName, "bar")));
    _applier->enqueue(_opCtx.get(), srcOps.cbegin(), srcOps.cend());

    auto batch = unittest::assertGet(_applier->getNextApplierBatch(_opCtx.get(), _limits));
    ASSERT_EQUALS(1U, batch.size()) << toString(batch);
    ASSERT_EQUALS(srcOps[0], batch[0]);
}

TEST_F(OplogApplierTest, GetNextApplierBatchGroupsUnpreparedApplyOpsOpWithOtherOps) {
    OplogApplier::Operations srcOps;
    srcOps.push_back(makeApplyOpsOplogEntry(1, false));
    srcOps.push_back(makeInsertOplogEntry(2, NamespaceString(dbName, "bar")));
    _applier->enqueue(_opCtx.get(), srcOps.cbegin(), srcOps.cend());

    auto batch = unittest::assertGet(_applier->getNextApplierBatch(_opCtx.get(), _limits));
    ASSERT_EQUALS(2U, batch.size()) << toString(batch);
    ASSERT_EQUALS(srcOps[0], batch[0]);
    ASSERT_EQUALS(srcOps[1], batch[1]);
}

TEST_F(OplogApplierTest, GetNextApplierBatchReturnsSystemDotViewsOpInOwnBatch) {
    OplogApplier::Operations srcOps;
    srcOps.push_back(makeInsertOplogEntry(
        1, NamespaceString(dbName, NamespaceString::kSystemDotViewsCollectionName)));
    srcOps.push_back(makeInsertOplogEntry(2, NamespaceString(dbName, "bar")));
    _applier->enqueue(_opCtx.get(), srcOps.cbegin(), srcOps.cend());

    auto batch = unittest::assertGet(_applier->getNextApplierBatch(_opCtx.get(), _limits));
    ASSERT_EQUALS(1U, batch.size()) << toString(batch);
    ASSERT_EQUALS(srcOps[0], batch[0]);
}

TEST_F(OplogApplierTest, GetNextApplierBatchReturnsServerConfigurationOpInOwnBatch) {
    OplogApplier::Operations srcOps;
    srcOps.push_back(makeInsertOplogEntry(1, NamespaceString::kServerConfigurationNamespace));
    srcOps.push_back(makeInsertOplogEntry(2, NamespaceString(dbName, "bar")));
    _applier->enqueue(_opCtx.get(), srcOps.cbegin(), srcOps.cend());

    auto batch = unittest::assertGet(_applier->getNextApplierBatch(_opCtx.get(), _limits));
    ASSERT_EQUALS(1U, batch.size()) << toString(batch);
    ASSERT_EQUALS(srcOps[0], batch[0]);
}

TEST_F(OplogApplierTest, GetNextApplierBatchReturnsPreparedCommitTransactionOpInOwnBatch) {
    OplogApplier::Operations srcOps;
    srcOps.push_back(makeCommitTransactionOplogEntry(1, dbName, true, 3));
    srcOps.push_back(makeInsertOplogEntry(2, NamespaceString(dbName, "bar")));
    _applier->enqueue(_opCtx.get(), srcOps.cbegin(), srcOps.cend());

    auto batch = unittest::assertGet(_applier->getNextApplierBatch(_opCtx.get(), _limits));
    ASSERT_EQUALS(1U, batch.size()) << toString(batch);
    ASSERT_EQUALS(srcOps[0], batch[0]);
}

TEST_F(OplogApplierTest, GetNextApplierBatchGroupsUnpreparedCommitTransactionOpWithOtherOps) {
    OplogApplier::Operations srcOps;
    srcOps.push_back(makeCommitTransactionOplogEntry(1, dbName, false, 3));
    srcOps.push_back(makeInsertOplogEntry(2, NamespaceString(dbName, "bar")));
    _applier->enqueue(_opCtx.get(), srcOps.cbegin(), srcOps.cend());

    auto batch = unittest::assertGet(_applier->getNextApplierBatch(_opCtx.get(), _limits));
    ASSERT_EQUALS(2U, batch.size()) << toString(batch);
    ASSERT_EQUALS(srcOps[0], batch[0]);
    ASSERT_EQUALS(srcOps[1], batch[1]);
}

}  // namespace
}  // namespace repl
}  // namespace mongo
