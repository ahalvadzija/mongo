// wiredtiger_recovery_unit.cpp


/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_begin_transaction_block.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_prepare_conflict.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/util/hex.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

// Always notifies prepare conflict waiters when a transaction commits or aborts, even when the
// transaction is not prepared. This should always be enabled if WTPrepareConflictForReads is
// used, which fails randomly. If this is not enabled, no prepare conflicts will be resolved,
// because the recovery unit may not ever actually be in a prepared state.
MONGO_FAIL_POINT_DEFINE(WTAlwaysNotifyPrepareConflictWaiters);

// SnapshotIds need to be globally unique, as they are used in a WorkingSetMember to
// determine if documents changed, but a different recovery unit may be used across a getMore,
// so there is a chance the snapshot ID will be reused.
AtomicUInt64 nextSnapshotId{1};

logger::LogSeverity kSlowTransactionSeverity = logger::LogSeverity::Debug(1);

/**
 * Returns a string representation of WiredTigerRecoveryUnit::State for logging.
 */
std::string toString(WiredTigerRecoveryUnit::State state) {
    switch (state) {
        case WiredTigerRecoveryUnit::State::kInactive:
            return "Inactive";
        case WiredTigerRecoveryUnit::State::kInactiveInUnitOfWork:
            return "InactiveInUnitOfWork";
        case WiredTigerRecoveryUnit::State::kActiveNotInUnitOfWork:
            return "ActiveNotInUnitOfWork";
        case WiredTigerRecoveryUnit::State::kActive:
            return "Active";
        case WiredTigerRecoveryUnit::State::kCommitting:
            return "Committing";
        case WiredTigerRecoveryUnit::State::kAborting:
            return "Aborting";
    }
    MONGO_UNREACHABLE;
}

}  // namespace

WiredTigerRecoveryUnit::WiredTigerRecoveryUnit(WiredTigerSessionCache* sc)
    : WiredTigerRecoveryUnit(sc, sc->getKVEngine()->getOplogManager()) {}

WiredTigerRecoveryUnit::WiredTigerRecoveryUnit(WiredTigerSessionCache* sc,
                                               WiredTigerOplogManager* oplogManager)
    : _sessionCache(sc),
      _oplogManager(oplogManager),
      _mySnapshotId(nextSnapshotId.fetchAndAdd(1)) {}

WiredTigerRecoveryUnit::~WiredTigerRecoveryUnit() {
    invariant(!_inUnitOfWork(), toString(_state));
    _abort();
}

void WiredTigerRecoveryUnit::_commit() {
    // Since we cannot have both a _lastTimestampSet and a _commitTimestamp, we set the
    // commit time as whichever is non-empty. If both are empty, then _lastTimestampSet will
    // be boost::none and we'll set the commit time to that.
    auto commitTime = _commitTimestamp.isNull() ? _lastTimestampSet : _commitTimestamp;

    try {
        bool notifyDone = !_prepareTimestamp.isNull();
        if (_session && _isActive()) {
            _txnClose(true);
        }
        _setState(State::kCommitting);

        if (MONGO_FAIL_POINT(WTAlwaysNotifyPrepareConflictWaiters)) {
            notifyDone = true;
        }

        if (notifyDone) {
            _sessionCache->notifyPreparedUnitOfWorkHasCommittedOrAborted();
        }

        for (Changes::const_iterator it = _changes.begin(), end = _changes.end(); it != end; ++it) {
            (*it)->commit(commitTime);
        }
        _changes.clear();
    } catch (...) {
        std::terminate();
    }

    _setState(State::kInactive);
}

void WiredTigerRecoveryUnit::_abort() {
    try {
        bool notifyDone = !_prepareTimestamp.isNull();
        if (_session && _isActive()) {
            _txnClose(false);
        }
        _setState(State::kAborting);

        if (MONGO_FAIL_POINT(WTAlwaysNotifyPrepareConflictWaiters)) {
            notifyDone = true;
        }

        if (notifyDone) {
            _sessionCache->notifyPreparedUnitOfWorkHasCommittedOrAborted();
        }

        for (Changes::const_reverse_iterator it = _changes.rbegin(), end = _changes.rend();
             it != end;
             ++it) {
            Change* change = it->get();
            LOG(2) << "CUSTOM ROLLBACK " << redact(demangleName(typeid(*change)));
            change->rollback();
        }
        _changes.clear();
    } catch (...) {
        std::terminate();
    }

    _setState(State::kInactive);
}

void WiredTigerRecoveryUnit::beginUnitOfWork(OperationContext* opCtx) {
    invariant(!_inUnitOfWork(), toString(_state));
    invariant(!_isCommittingOrAborting(),
              str::stream() << "cannot begin unit of work while commit or rollback handlers are "
                               "running: "
                            << toString(_state));
    _setState(_isActive() ? State::kActive : State::kInactiveInUnitOfWork);
}

void WiredTigerRecoveryUnit::prepareUnitOfWork() {
    invariant(_inUnitOfWork(), toString(_state));
    invariant(!_prepareTimestamp.isNull());

    auto session = getSession();
    WT_SESSION* s = session->getSession();

    LOG(1) << "preparing transaction at time: " << _prepareTimestamp;

    const std::string conf = "prepare_timestamp=" + integerToHex(_prepareTimestamp.asULL());
    // Prepare the transaction.
    invariantWTOK(s->prepare_transaction(s, conf.c_str()));
}

void WiredTigerRecoveryUnit::commitUnitOfWork() {
    invariant(_inUnitOfWork(), toString(_state));
    _commit();
}

void WiredTigerRecoveryUnit::abortUnitOfWork() {
    invariant(_inUnitOfWork(), toString(_state));
    _abort();
}

void WiredTigerRecoveryUnit::_ensureSession() {
    if (!_session) {
        _session = _sessionCache->getSession();
    }
}

bool WiredTigerRecoveryUnit::waitUntilDurable() {
    invariant(!_inUnitOfWork(), toString(_state));
    const bool forceCheckpoint = false;
    const bool stableCheckpoint = false;
    _sessionCache->waitUntilDurable(forceCheckpoint, stableCheckpoint);
    return true;
}

bool WiredTigerRecoveryUnit::waitUntilUnjournaledWritesDurable() {
    invariant(!_inUnitOfWork(), toString(_state));
    const bool forceCheckpoint = true;
    const bool stableCheckpoint = true;
    // Calling `waitUntilDurable` with `forceCheckpoint` set to false only performs a log
    // (journal) flush, and thus has no effect on unjournaled writes. Setting `forceCheckpoint` to
    // true will lock in stable writes to unjournaled tables.
    _sessionCache->waitUntilDurable(forceCheckpoint, stableCheckpoint);
    return true;
}

void WiredTigerRecoveryUnit::registerChange(Change* change) {
    invariant(_inUnitOfWork(), toString(_state));
    _changes.push_back(std::unique_ptr<Change>{change});
}

void WiredTigerRecoveryUnit::assertInActiveTxn() const {
    if (_isActive()) {
        return;
    }
    severe() << "Recovery unit is not active. Current state: " << toString(_state);
    fassertFailed(28575);
}

WiredTigerSession* WiredTigerRecoveryUnit::getSession() {
    if (!_isActive()) {
        _txnOpen();
        _setState(_inUnitOfWork() ? State::kActive : State::kActiveNotInUnitOfWork);
    }
    return _session.get();
}

WiredTigerSession* WiredTigerRecoveryUnit::getSessionNoTxn() {
    _ensureSession();
    WiredTigerSession* session = _session.get();

    // Handling queued drops can be slow, which is not desired for internal operations like FTDC
    // sampling. Disable handling of queued drops for such sessions.
    session->dropQueuedIdentsAtSessionEndAllowed(false);
    return session;
}

void WiredTigerRecoveryUnit::abandonSnapshot() {
    invariant(!_inUnitOfWork(), toString(_state));
    if (_isActive()) {
        // Can't be in a WriteUnitOfWork, so safe to rollback
        _txnClose(false);
    }
    _setState(State::kInactive);
}

void WiredTigerRecoveryUnit::preallocateSnapshot() {
    // Begin a new transaction, if one is not already started.
    getSession();
}

void WiredTigerRecoveryUnit::_txnClose(bool commit) {
    invariant(_isActive(), toString(_state));
    WT_SESSION* s = _session->getSession();
    if (_timer) {
        const int transactionTime = _timer->millis();
        // `serverGlobalParams.slowMs` can be set to values <= 0. In those cases, give logging a
        // break.
        if (transactionTime >= std::max(1, serverGlobalParams.slowMS)) {
            LOG(kSlowTransactionSeverity) << "Slow WT transaction. Lifetime of SnapshotId "
                                          << _mySnapshotId << " was " << transactionTime << "ms";
        }
    }

    int wtRet;
    if (commit) {
        if (!_commitTimestamp.isNull()) {
            const std::string conf = "commit_timestamp=" + integerToHex(_commitTimestamp.asULL());
            invariantWTOK(s->timestamp_transaction(s, conf.c_str()));
            _isTimestamped = true;
        }

        wtRet = s->commit_transaction(s, nullptr);
        LOG(3) << "WT commit_transaction for snapshot id " << _mySnapshotId;
    } else {
        wtRet = s->rollback_transaction(s, nullptr);
        invariant(!wtRet);
        LOG(3) << "WT rollback_transaction for snapshot id " << _mySnapshotId;
    }

    if (_isTimestamped) {
        if (!_orderedCommit) {
            // We only need to update oplog visibility where commits can be out-of-order with
            // respect to their assigned optime and such commits might otherwise be visible.
            // This should happen only on primary nodes.
            _oplogManager->triggerJournalFlush();
        }
        _isTimestamped = false;
    }
    invariantWTOK(wtRet);

    invariant(!_lastTimestampSet || _commitTimestamp.isNull(),
              str::stream() << "Cannot have both a _lastTimestampSet and a "
                               "_commitTimestamp. _lastTimestampSet: "
                            << _lastTimestampSet->toString()
                            << ". _commitTimestamp: "
                            << _commitTimestamp.toString());

    // We reset the _lastTimestampSet between transactions. Since it is legal for one
    // transaction on a RecoveryUnit to call setTimestamp() and another to call
    // setCommitTimestamp().
    _lastTimestampSet = boost::none;

    _prepareTimestamp = Timestamp();
    _mySnapshotId = nextSnapshotId.fetchAndAdd(1);
    _isOplogReader = false;
    _orderedCommit = true;  // Default value is true; we assume all writes are ordered.
}

SnapshotId WiredTigerRecoveryUnit::getSnapshotId() const {
    // TODO: use actual wiredtiger txn id
    return SnapshotId(_mySnapshotId);
}

Status WiredTigerRecoveryUnit::obtainMajorityCommittedSnapshot() {
    invariant(_timestampReadSource == ReadSource::kMajorityCommitted);
    auto snapshotName = _sessionCache->snapshotManager().getMinSnapshotForNextCommittedRead();
    if (!snapshotName) {
        return {ErrorCodes::ReadConcernMajorityNotAvailableYet,
                "Read concern majority reads are currently not possible."};
    }
    _majorityCommittedSnapshot = *snapshotName;
    return Status::OK();
}

boost::optional<Timestamp> WiredTigerRecoveryUnit::getPointInTimeReadTimestamp() const {
    if (_timestampReadSource == ReadSource::kProvided ||
        _timestampReadSource == ReadSource::kLastAppliedSnapshot ||
        _timestampReadSource == ReadSource::kAllCommittedSnapshot) {
        invariant(!_readAtTimestamp.isNull());
        return _readAtTimestamp;
    }

    if (_timestampReadSource == ReadSource::kLastApplied && !_readAtTimestamp.isNull()) {
        return _readAtTimestamp;
    }

    if (_timestampReadSource == ReadSource::kMajorityCommitted) {
        invariant(!_majorityCommittedSnapshot.isNull());
        return _majorityCommittedSnapshot;
    }

    return boost::none;
}

void WiredTigerRecoveryUnit::_txnOpen() {
    invariant(!_isActive(), toString(_state));
    invariant(!_isCommittingOrAborting(),
              str::stream() << "commit or rollback handler reopened transaction: "
                            << toString(_state));
    _ensureSession();

    // Only start a timer for transaction's lifetime if we're going to log it.
    if (shouldLog(kSlowTransactionSeverity)) {
        _timer.reset(new Timer());
    }
    WT_SESSION* session = _session->getSession();

    switch (_timestampReadSource) {
        case ReadSource::kUnset:
        case ReadSource::kNoTimestamp: {
            WiredTigerBeginTxnBlock txnOpen(session, _ignorePrepared);

            if (_isOplogReader) {
                auto status =
                    txnOpen.setTimestamp(Timestamp(_oplogManager->getOplogReadTimestamp()),
                                         WiredTigerBeginTxnBlock::RoundToOldest::kRound);
                fassert(50771, status);
            }
            txnOpen.done();
            break;
        }
        case ReadSource::kMajorityCommitted: {
            // We reset _majorityCommittedSnapshot to the actual read timestamp used when the
            // transaction was started.
            _majorityCommittedSnapshot =
                _sessionCache->snapshotManager().beginTransactionOnCommittedSnapshot(
                    session, _ignorePrepared);
            break;
        }
        case ReadSource::kLastApplied: {
            if (_sessionCache->snapshotManager().getLocalSnapshot()) {
                _readAtTimestamp = _sessionCache->snapshotManager().beginTransactionOnLocalSnapshot(
                    session, _ignorePrepared);
            } else {
                WiredTigerBeginTxnBlock(session, _ignorePrepared).done();
            }
            break;
        }
        case ReadSource::kAllCommittedSnapshot: {
            if (_readAtTimestamp.isNull()) {
                _readAtTimestamp = _beginTransactionAtAllCommittedTimestamp(session);
                break;
            }
            // Intentionally continue to the next case to read at the _readAtTimestamp.
        }
        case ReadSource::kLastAppliedSnapshot: {
            // Only ever read the last applied timestamp once, and continue reusing it for
            // subsequent transactions.
            if (_readAtTimestamp.isNull()) {
                _readAtTimestamp = _sessionCache->snapshotManager().beginTransactionOnLocalSnapshot(
                    session, _ignorePrepared);
                break;
            }
            // Intentionally continue to the next case to read at the _readAtTimestamp.
        }
        case ReadSource::kProvided: {
            WiredTigerBeginTxnBlock txnOpen(session, _ignorePrepared);
            auto status = txnOpen.setTimestamp(_readAtTimestamp);

            if (!status.isOK() && status.code() == ErrorCodes::BadValue) {
                uasserted(ErrorCodes::SnapshotTooOld,
                          str::stream() << "Read timestamp " << _readAtTimestamp.toString()
                                        << " is older than the oldest available timestamp.");
            }
            uassertStatusOK(status);
            txnOpen.done();
            break;
        }
    }

    LOG(3) << "WT begin_transaction for snapshot id " << _mySnapshotId;
}

Timestamp WiredTigerRecoveryUnit::_beginTransactionAtAllCommittedTimestamp(WT_SESSION* session) {
    WiredTigerBeginTxnBlock txnOpen(session, _ignorePrepared);
    Timestamp txnTimestamp = Timestamp(_oplogManager->fetchAllCommittedValue(session->connection));
    auto status =
        txnOpen.setTimestamp(txnTimestamp, WiredTigerBeginTxnBlock::RoundToOldest::kRound);
    fassert(50948, status);

    // Since this is not in a critical section, we might have rounded to oldest between
    // calling getAllCommitted and setTimestamp.  We need to get the actual read timestamp we
    // used.
    char buf[(2 * 8 /*bytes in hex*/) + 1 /*nul terminator*/];
    auto wtstatus = session->query_timestamp(session, buf, "get=read");
    invariantWTOK(wtstatus);
    uint64_t read_timestamp;
    fassert(50949, parseNumberFromStringWithBase(buf, 16, &read_timestamp));
    txnOpen.done();
    return Timestamp(read_timestamp);
}

Status WiredTigerRecoveryUnit::setTimestamp(Timestamp timestamp) {
    _ensureSession();
    LOG(3) << "WT set timestamp of future write operations to " << timestamp;
    WT_SESSION* session = _session->getSession();
    invariant(_inUnitOfWork(), toString(_state));
    invariant(_prepareTimestamp.isNull());
    invariant(_commitTimestamp.isNull(),
              str::stream() << "Commit timestamp set to " << _commitTimestamp.toString()
                            << " and trying to set WUOW timestamp to "
                            << timestamp.toString());

    _lastTimestampSet = timestamp;

    // Starts the WT transaction associated with this session.
    getSession();

    const std::string conf = "commit_timestamp=" + integerToHex(timestamp.asULL());
    auto rc = session->timestamp_transaction(session, conf.c_str());
    if (rc == 0) {
        _isTimestamped = true;
    }
    return wtRCToStatus(rc, "timestamp_transaction");
}

void WiredTigerRecoveryUnit::setCommitTimestamp(Timestamp timestamp) {
    // This can be called either outside of a WriteUnitOfWork or in a prepared transaction after
    // setPrepareTimestamp() is called. Prepared transactions ensure the correct timestamping
    // semantics and the set-once commitTimestamp behavior is exactly what prepared transactions
    // want.
    invariant(!_inUnitOfWork() || !_prepareTimestamp.isNull(), toString(_state));
    invariant(_commitTimestamp.isNull(),
              str::stream() << "Commit timestamp set to " << _commitTimestamp.toString()
                            << " and trying to set it to "
                            << timestamp.toString());
    invariant(!_lastTimestampSet,
              str::stream() << "Last timestamp set is " << _lastTimestampSet->toString()
                            << " and trying to set commit timestamp to "
                            << timestamp.toString());
    invariant(!_isTimestamped);

    _commitTimestamp = timestamp;
}

Timestamp WiredTigerRecoveryUnit::getCommitTimestamp() const {
    return _commitTimestamp;
}

void WiredTigerRecoveryUnit::clearCommitTimestamp() {
    invariant(!_inUnitOfWork(), toString(_state));
    invariant(!_commitTimestamp.isNull());
    invariant(!_lastTimestampSet,
              str::stream() << "Last timestamp set is " << _lastTimestampSet->toString()
                            << " and trying to clear commit timestamp.");
    invariant(!_isTimestamped);

    _commitTimestamp = Timestamp();
}

void WiredTigerRecoveryUnit::setPrepareTimestamp(Timestamp timestamp) {
    invariant(_inUnitOfWork(), toString(_state));
    invariant(_prepareTimestamp.isNull(),
              str::stream() << "Trying to set prepare timestamp to " << timestamp.toString()
                            << ". It's already set to "
                            << _prepareTimestamp.toString());
    invariant(_commitTimestamp.isNull(),
              str::stream() << "Commit timestamp is " << _commitTimestamp.toString()
                            << " and trying to set prepare timestamp to "
                            << timestamp.toString());
    invariant(!_lastTimestampSet,
              str::stream() << "Last timestamp set is " << _lastTimestampSet->toString()
                            << " and trying to set prepare timestamp to "
                            << timestamp.toString());

    _prepareTimestamp = timestamp;
}

Timestamp WiredTigerRecoveryUnit::getPrepareTimestamp() const {
    invariant(_inUnitOfWork(), toString(_state));
    invariant(!_prepareTimestamp.isNull());
    invariant(_commitTimestamp.isNull(),
              str::stream() << "Commit timestamp is " << _commitTimestamp.toString()
                            << " and trying to get prepare timestamp of "
                            << _prepareTimestamp.toString());
    invariant(!_lastTimestampSet,
              str::stream() << "Last timestamp set is " << _lastTimestampSet->toString()
                            << " and trying to get prepare timestamp of "
                            << _prepareTimestamp.toString());

    return _prepareTimestamp;
}

void WiredTigerRecoveryUnit::setIgnorePrepared(bool value) {
    _ignorePrepared = (value) ? WiredTigerBeginTxnBlock::IgnorePrepared::kIgnore
                              : WiredTigerBeginTxnBlock::IgnorePrepared::kNoIgnore;
}

void WiredTigerRecoveryUnit::setTimestampReadSource(ReadSource readSource,
                                                    boost::optional<Timestamp> provided) {
    LOG(3) << "setting timestamp read source: " << static_cast<int>(readSource)
           << ", provided timestamp: " << ((provided) ? provided->toString() : "none");

    invariant(!_isActive() || _timestampReadSource == readSource,
              str::stream() << "Current state: " << toString(_state)
                            << ". Invalid internal state while setting timestamp read source: "
                            << static_cast<int>(readSource)
                            << ", provided timestamp: "
                            << (provided ? provided->toString() : "none"));
    invariant(!provided == (readSource != ReadSource::kProvided));
    invariant(!(provided && provided->isNull()));

    _timestampReadSource = readSource;
    _readAtTimestamp = (provided) ? *provided : Timestamp();
}

RecoveryUnit::ReadSource WiredTigerRecoveryUnit::getTimestampReadSource() const {
    return _timestampReadSource;
}

void WiredTigerRecoveryUnit::beginIdle() {
    // Close all cursors, we don't want to keep any old cached cursors around.
    if (_session) {
        _session->closeAllCursors("");
    }
}

BSONObj WiredTigerRecoveryUnit::getOperationStatistics() const {
    BSONObjBuilder bob;
    if (!_session)
        return bob.obj();

    WT_SESSION* s = _session->getSession();
    invariant(s);

    Status status = WiredTigerUtil::exportOperationStatsInfoToBSON(
        s, "statistics:session", "statistics=(fast)", &bob);
    if (!status.isOK()) {
        bob.append("error", "unable to retrieve storage statistics");
        bob.append("code", static_cast<int>(status.code()));
        bob.append("reason", status.reason());
    }

    return bob.obj();
}

void WiredTigerRecoveryUnit::_setState(State newState) {
    _state = newState;
}

bool WiredTigerRecoveryUnit::_isActive() const {
    return State::kActiveNotInUnitOfWork == _state || State::kActive == _state;
}

bool WiredTigerRecoveryUnit::_inUnitOfWork() const {
    return State::kInactiveInUnitOfWork == _state || State::kActive == _state;
}

bool WiredTigerRecoveryUnit::_isCommittingOrAborting() const {
    return State::kCommitting == _state || State::kAborting == _state;
}

}  // namespace mongo
