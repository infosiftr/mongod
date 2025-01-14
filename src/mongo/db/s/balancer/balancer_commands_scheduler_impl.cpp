/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/db/s/balancer/balancer_commands_scheduler_impl.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/migration_secondary_throttle_options.h"
#include "mongo/s/shard_id.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/fail_point.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(pauseBalancerWorkerThread);
MONGO_FAIL_POINT_DEFINE(deferredCleanupCompletedCheckpoint);

const std::string MergeChunksCommandInfo::kCommandName = "mergeChunks";
const std::string MergeChunksCommandInfo::kBounds = "bounds";
const std::string MergeChunksCommandInfo::kShardName = "shardName";
const std::string MergeChunksCommandInfo::kEpoch = "epoch";

const std::string DataSizeCommandInfo::kCommandName = "dataSize";
const std::string DataSizeCommandInfo::kKeyPattern = "keyPattern";
const std::string DataSizeCommandInfo::kMinValue = "min";
const std::string DataSizeCommandInfo::kMaxValue = "max";
const std::string DataSizeCommandInfo::kEstimatedValue = "estimate";

const std::string SplitVectorCommandInfo::kCommandName = "splitVector";
const std::string SplitVectorCommandInfo::kKeyPattern = "keyPattern";
const std::string SplitVectorCommandInfo::kLowerBound = "min";
const std::string SplitVectorCommandInfo::kUpperBound = "max";
const std::string SplitVectorCommandInfo::kMaxChunkSizeBytes = "maxChunkSizeBytes";
const std::string SplitVectorCommandInfo::kMaxSplitPoints = "maxSplitPoints";
const std::string SplitVectorCommandInfo::kMaxChunkObjects = "maxChunkObjects";
const std::string SplitVectorCommandInfo::kForceSplit = "force";

const std::string SplitChunkCommandInfo::kCommandName = "splitChunk";
const std::string SplitChunkCommandInfo::kShardName = "from";
const std::string SplitChunkCommandInfo::kKeyPattern = "keyPattern";
const std::string SplitChunkCommandInfo::kLowerBound = "min";
const std::string SplitChunkCommandInfo::kUpperBound = "max";
const std::string SplitChunkCommandInfo::kEpoch = "epoch";
const std::string SplitChunkCommandInfo::kSplitKeys = "splitKeys";

BalancerCommandsSchedulerImpl::BalancerCommandsSchedulerImpl() {}

BalancerCommandsSchedulerImpl::~BalancerCommandsSchedulerImpl() {
    stop();
}

void BalancerCommandsSchedulerImpl::start(OperationContext* opCtx) {
    LOGV2(5847200, "Balancer command scheduler start requested");
    stdx::lock_guard<Latch> lgss(_startStopMutex);
    stdx::lock_guard<Latch> lg(_mutex);
    if (_state == SchedulerState::Recovering || _state == SchedulerState::Running) {
        return;
    }

    invariant(!_workerThreadHandle.joinable());
    auto requestsToRecover = _loadRequestsToRecover(opCtx);
    _state = requestsToRecover.empty() ? SchedulerState::Running : SchedulerState::Recovering;

    for (auto& requestToRecover : requestsToRecover) {
        _enqueueRequest(lg, std::move(requestToRecover));
    }

    _workerThreadHandle = stdx::thread([this] { _workerThread(); });
}

void BalancerCommandsSchedulerImpl::stop() {
    LOGV2(5847201, "Balancer command scheduler stop requested");
    stdx::lock_guard<Latch> lgss(_startStopMutex);
    {
        stdx::lock_guard<Latch> lg(_mutex);
        if (_state == SchedulerState::Stopped) {
            return;
        }

        invariant(_workerThreadHandle.joinable());
        _state = SchedulerState::Stopping;
        _stateUpdatedCV.notify_all();
    }
    _workerThreadHandle.join();
}

std::unique_ptr<MoveChunkResponse> BalancerCommandsSchedulerImpl::requestMoveChunk(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ChunkType& chunk,
    const ShardId& recipient,
    const MoveChunkSettings& commandSettings,
    bool issuedByRemoteUser) {

    auto externalClientInfo =
        issuedByRemoteUser ? boost::optional<ExternalClientInfo>(opCtx) : boost::none;

    auto commandInfo = std::make_shared<MoveChunkCommandInfo>(nss,
                                                              chunk.getShard(),
                                                              recipient,
                                                              chunk.getMin(),
                                                              chunk.getMax(),
                                                              commandSettings.maxChunkSizeBytes,
                                                              commandSettings.secondaryThrottle,
                                                              commandSettings.waitForDelete,
                                                              commandSettings.forceJumbo,
                                                              chunk.getVersion(),
                                                              std::move(externalClientInfo));

    auto requestCollectionInfo = _buildAndEnqueueNewRequest(opCtx, std::move(commandInfo));
    return std::make_unique<MoveChunkResponseImpl>(std::move(requestCollectionInfo));
}

std::unique_ptr<MergeChunksResponse> BalancerCommandsSchedulerImpl::requestMergeChunks(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ShardId& shardId,
    const ChunkRange& chunkRange,
    const ChunkVersion& version) {

    auto commandInfo = std::make_shared<MergeChunksCommandInfo>(
        nss, shardId, chunkRange.getMin(), chunkRange.getMax(), version);

    auto requestCollectionInfo = _buildAndEnqueueNewRequest(opCtx, std::move(commandInfo));
    return std::make_unique<MergeChunksResponseImpl>(std::move(requestCollectionInfo));
}

std::unique_ptr<SplitVectorResponse> BalancerCommandsSchedulerImpl::requestSplitVector(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ChunkType& chunk,
    const KeyPattern& keyPattern,
    const SplitVectorSettings& commandSettings) {

    auto commandInfo = std::make_shared<SplitVectorCommandInfo>(nss,
                                                                chunk.getShard(),
                                                                keyPattern.toBSON(),
                                                                chunk.getMin(),
                                                                chunk.getMax(),
                                                                commandSettings.maxSplitPoints,
                                                                commandSettings.maxChunkObjects,
                                                                commandSettings.maxChunkSizeBytes,
                                                                commandSettings.force);

    auto requestCollectionInfo = _buildAndEnqueueNewRequest(opCtx, std::move(commandInfo));
    return std::make_unique<SplitVectorResponseImpl>(std::move(requestCollectionInfo));
}

std::unique_ptr<SplitChunkResponse> BalancerCommandsSchedulerImpl::requestSplitChunk(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ChunkType& chunk,
    const KeyPattern& shardKeyPattern,
    const std::vector<BSONObj>& splitPoints) {

    auto commandInfo = std::make_shared<SplitChunkCommandInfo>(nss,
                                                               chunk.getShard(),
                                                               shardKeyPattern.toBSON(),
                                                               chunk.getMin(),
                                                               chunk.getMax(),
                                                               chunk.getVersion(),
                                                               splitPoints);

    auto requestCollectionInfo = _buildAndEnqueueNewRequest(opCtx, std::move(commandInfo));
    return std::make_unique<SplitChunkResponseImpl>(std::move(requestCollectionInfo));
}

std::unique_ptr<ChunkDataSizeResponse> BalancerCommandsSchedulerImpl::requestDataSize(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ShardId& shardId,
    const ChunkRange& chunkRange,
    const ChunkVersion& version,
    const KeyPattern& keyPattern,
    bool estimatedValue) {
    auto commandInfo = std::make_shared<DataSizeCommandInfo>(nss,
                                                             shardId,
                                                             keyPattern.toBSON(),
                                                             chunkRange.getMin(),
                                                             chunkRange.getMax(),
                                                             estimatedValue,
                                                             version);

    auto requestCollectionInfo = _buildAndEnqueueNewRequest(opCtx, std::move(commandInfo));
    return std::make_unique<ChunkDataSizeResponseImpl>(std::move(requestCollectionInfo));
}

ResponseHandle BalancerCommandsSchedulerImpl::_buildAndEnqueueNewRequest(
    OperationContext* opCtx, std::shared_ptr<CommandInfo>&& commandInfo) {
    const auto newRequestId = UUID::gen();
    LOGV2(5847202,
          "Enqueuing new Balancer command request",
          "reqId"_attr = newRequestId,
          "command"_attr = redact(commandInfo->serialise().toString()),
          "recoveryDocRequired"_attr = commandInfo->requiresRecoveryOnCrash());

    if (commandInfo->requiresRecoveryOnCrash()) {
        DBDirectClient dbClient(opCtx);
        PersistedBalancerCommand recoveryDoc(newRequestId,
                                             commandInfo->serialise(),
                                             commandInfo->getTarget(),
                                             commandInfo->getNameSpace(),
                                             commandInfo->requiresDistributedLock());
        std::vector<BSONObj> serialisedRecoveryInfo;
        serialisedRecoveryInfo.emplace_back(recoveryDoc.toBSON());
        auto reply = dbClient.insertAcknowledged(
            NamespaceString::kConfigBalancerCommandsNamespace.toString(),
            serialisedRecoveryInfo,
            true,
            WriteConcernOptions::Majority);

        if (auto writeStatus = getStatusFromWriteCommandReply(reply); !writeStatus.isOK()) {
            LOGV2(5847210,
                  "Failed to persist request command document",
                  "reqId"_attr = newRequestId,
                  "status"_attr = writeStatus);
            return ResponseHandle(newRequestId, writeStatus);
        }
    }

    RequestData pendingRequest(newRequestId, std::move(commandInfo));

    stdx::unique_lock<Latch> ul(_mutex);
    _stateUpdatedCV.wait(ul, [this] { return _state != SchedulerState::Recovering; });

    return _enqueueRequest(ul, std::move(pendingRequest));
}

ResponseHandle BalancerCommandsSchedulerImpl::_enqueueRequest(WithLock, RequestData&& request) {
    auto requestId = request.getId();
    auto deferredResponseHandle = request.getResponseHandle();
    if (_state == SchedulerState::Recovering || _state == SchedulerState::Running) {
        _incompleteRequests.emplace(std::make_pair(requestId, std::move(request)));
        _pendingRequestIds.push_back(requestId);
        _stateUpdatedCV.notify_all();
    } else {
        deferredResponseHandle.handle->set(Status(
            ErrorCodes::BalancerInterrupted, "Request rejected - balancer scheduler is stopped"));
    }

    return deferredResponseHandle;
}

bool BalancerCommandsSchedulerImpl::_canSubmitNewRequests(WithLock) {
    return (!_pendingRequestIds.empty() && MONGO_likely(!pauseBalancerWorkerThread.shouldFail()));
}

bool BalancerCommandsSchedulerImpl::_deferredCleanupRequired(WithLock) {
    return (!_obsoleteRecoveryDocumentIds.empty() || !_lockedReferencesToRelease.empty());
}

CommandSubmissionResult BalancerCommandsSchedulerImpl::_submit(
    OperationContext* opCtx, const CommandSubmissionHandle& handle) {
    LOGV2(5847203, "Balancer command request submitted for execution", "reqId"_attr = handle.id);

    auto executor = Grid::get(opCtx)->getExecutorPool()->getFixedExecutor();
    const auto shardWithStatus =
        Grid::get(opCtx)->shardRegistry()->getShard(opCtx, handle.commandInfo->getTarget());
    if (!shardWithStatus.isOK()) {
        return CommandSubmissionResult(handle.id, false, shardWithStatus.getStatus());
    }

    const auto shardHostWithStatus = shardWithStatus.getValue()->getTargeter()->findHost(
        opCtx, ReadPreferenceSetting{ReadPreference::PrimaryOnly});
    if (!shardHostWithStatus.isOK()) {
        return CommandSubmissionResult(handle.id, false, shardHostWithStatus.getStatus());
    }

    const executor::RemoteCommandRequest remoteCommand(shardHostWithStatus.getValue(),
                                                       NamespaceString::kAdminDb.toString(),
                                                       handle.commandInfo->serialise(),
                                                       opCtx);

    auto onRemoteResponseReceived =
        [this,
         requestId = handle.id](const executor::TaskExecutor::RemoteCommandCallbackArgs& args) {
            _applyCommandResponse(requestId, args.response);
        };

    if (handle.commandInfo->requiresDistributedLock()) {
        Status lockAcquisitionResponse =
            _distributedLocks.acquireFor(opCtx, handle.commandInfo->getNameSpace());
        if (!lockAcquisitionResponse.isOK()) {
            return CommandSubmissionResult(handle.id, false, lockAcquisitionResponse);
        }
    }

    auto remoteCommandHandleWithStatus =
        executor->scheduleRemoteCommand(remoteCommand, onRemoteResponseReceived);
    return (
        remoteCommandHandleWithStatus.isOK()
            ? CommandSubmissionResult(handle.id, true, remoteCommandHandleWithStatus.getValue())
            : CommandSubmissionResult(handle.id, true, remoteCommandHandleWithStatus.getStatus()));
}

void BalancerCommandsSchedulerImpl::_applySubmissionResult(
    WithLock, CommandSubmissionResult&& submissionResult) {
    auto requestToUpdateIt = _incompleteRequests.find(submissionResult.id);
    if (requestToUpdateIt == _incompleteRequests.end()) {
        LOGV2(5847209,
              "Skipping _applySubmissionResult: request already completed/canceled",
              "reqId"_attr = submissionResult.id);
        return;
    }
    /*
     * Rules for processing the outcome of a command submission:
     * If successful,
     * - set the execution context on the RequestData to be able to serve future cancel requests
     * - update the data structure describing running command requests
     * If failed,
     * - store the outcome into the deferred response
     * - remove the RequestData and its persisted info (if any)
     */
    auto& requestToUpdate = requestToUpdateIt->second;
    if (submissionResult.context.isOK()) {
        requestToUpdate.addExecutionContext(std::move(submissionResult.context.getValue()));
        _runningRequestIds.insert(submissionResult.id);
    } else {
        const auto& submittedCommandInfo = requestToUpdate.getCommandInfo();
        if (submissionResult.acquiredDistLock) {
            _lockedReferencesToRelease.emplace_back(submittedCommandInfo.getNameSpace());
        }
        if (submittedCommandInfo.requiresRecoveryOnCrash()) {
            _obsoleteRecoveryDocumentIds.push_back(submissionResult.id);
        }
        requestToUpdate.setOutcome(submissionResult.context.getStatus());
        _incompleteRequests.erase(requestToUpdateIt);
    }
}

void BalancerCommandsSchedulerImpl::_applyCommandResponse(
    UUID requestId, const executor::TaskExecutor::ResponseStatus& response) {
    {
        stdx::lock_guard<Latch> lg(_mutex);
        auto requestToCompleteIt = _incompleteRequests.find(requestId);
        if (_state == SchedulerState::Stopping || _state == SchedulerState::Stopped ||
            requestToCompleteIt == _incompleteRequests.end()) {
            // Drop the response - the request is being cancelled in the worker thread.
            return;
        }
        auto& requestToComplete = requestToCompleteIt->second;
        requestToComplete.setOutcome(response);
        auto& commandInfo = requestToComplete.getCommandInfo();
        if (commandInfo.requiresDistributedLock()) {
            _lockedReferencesToRelease.emplace_back(commandInfo.getNameSpace());
        }
        if (commandInfo.requiresRecoveryOnCrash()) {
            _obsoleteRecoveryDocumentIds.push_back(requestId);
        }
        _runningRequestIds.erase(requestId);
        _incompleteRequests.erase(requestToCompleteIt);
        if (_incompleteRequests.empty() && _state == SchedulerState::Recovering) {
            LOGV2(5847213, "Balancer scheduler recovery complete. Switching to regular execution");
            _state = SchedulerState::Running;
        }
        _stateUpdatedCV.notify_all();
    }
    LOGV2(5847204,
          "Execution of balancer command request completed",
          "reqId"_attr = requestId,
          "response"_attr = response.toString());
}

std::vector<RequestData> BalancerCommandsSchedulerImpl::_loadRequestsToRecover(
    OperationContext* opCtx) {
    std::vector<RequestData> requestsToRecover;
    auto documentProcessor = [&requestsToRecover, opCtx](const BSONObj& commandToRecoverDoc) {
        auto originalCommand = PersistedBalancerCommand::parse(
            IDLParserErrorContext("BalancerCommandsScheduler"), commandToRecoverDoc);
        auto recoveryCommand = std::make_shared<RecoveryCommandInfo>(originalCommand);
        LOGV2(5847212,
              "Command request recovered and set for rescheduling",
              "reqId"_attr = originalCommand.getRequestId(),
              "command"_attr = redact(recoveryCommand->serialise()));
        requestsToRecover.emplace_back(originalCommand.getRequestId(), std::move(recoveryCommand));
    };
    DBDirectClient dbClient(opCtx);
    try {
        dbClient.query(
            documentProcessor, NamespaceString::kConfigBalancerCommandsNamespace, BSONObj());
    } catch (const DBException& e) {
        LOGV2(5847225, "Failed to load requests to recover", "error"_attr = redact(e));
    }

    return requestsToRecover;
}

void BalancerCommandsSchedulerImpl::_performDeferredCleanup(WithLock, OperationContext* opCtx) {
    auto recoveryDocsDeleted = [this, opCtx] {
        if (_obsoleteRecoveryDocumentIds.empty()) {
            return false;
        }
        BSONObjBuilder queryBuilder;
        if (_obsoleteRecoveryDocumentIds.size() == 1) {
            _obsoleteRecoveryDocumentIds[0].appendToBuilder(
                &queryBuilder, PersistedBalancerCommand::kRequestIdFieldName);
        } else {
            BSONObjBuilder requestIdClause(
                queryBuilder.subobjStart(PersistedBalancerCommand::kRequestIdFieldName));
            BSONArrayBuilder valuesBuilder(requestIdClause.subarrayStart("$in"));
            for (const auto& requestId : _obsoleteRecoveryDocumentIds) {
                requestId.appendToArrayBuilder(&valuesBuilder);
            }
        }

        _obsoleteRecoveryDocumentIds.clear();

        auto query = queryBuilder.obj();
        DBDirectClient dbClient(opCtx);

        auto reply = dbClient.removeAcknowledged(
            NamespaceString::kConfigBalancerCommandsNamespace.toString(), query);

        LOGV2(5847211,
              "Clean up of obsolete document info performed",
              "query"_attr = query,
              "reply"_attr = reply);
        return true;
    }();

    auto distributedLocksReleased = [this, opCtx] {
        if (_lockedReferencesToRelease.empty()) {
            return false;
        }
        for (const auto& nss : _lockedReferencesToRelease) {
            _distributedLocks.releaseFor(opCtx, nss);
        }
        _lockedReferencesToRelease.clear();
        return true;
    }();

    if (recoveryDocsDeleted || distributedLocksReleased) {
        deferredCleanupCompletedCheckpoint.pauseWhileSet();
    }
}

void BalancerCommandsSchedulerImpl::_workerThread() {
    ON_BLOCK_EXIT([this] {
        LOGV2(5847208, "Leaving balancer command scheduler thread");
        stdx::lock_guard<Latch> lg(_mutex);
        _state = SchedulerState::Stopped;
        _stateUpdatedCV.notify_all();
    });

    Client::initThread("BalancerCommandsScheduler");
    stdx::unordered_map<UUID, RequestData, UUID::Hash> requestsToCleanUpOnExit;
    LOGV2(5847205, "Balancer scheduler thread started");

    while (true) {
        std::vector<CommandSubmissionHandle> commandsToSubmit;
        {
            stdx::unique_lock<Latch> ul(_mutex);
            invariant(_state != SchedulerState::Stopped);
            _stateUpdatedCV.wait(ul, [this, &ul] {
                return (_state == SchedulerState::Stopping || _canSubmitNewRequests(ul) ||
                        _deferredCleanupRequired(ul));
            });

            // Completed commands defer the clean up of acquired resources
            {
                auto opCtxHolder = cc().makeOperationContext();
                _performDeferredCleanup(ul, opCtxHolder.get());
            }

            if (_state == SchedulerState::Stopping) {
                _runningRequestIds.clear();
                _pendingRequestIds.clear();
                _incompleteRequests.swap(requestsToCleanUpOnExit);
                break;
            }

            if (!_canSubmitNewRequests(ul)) {
                continue;
            }

            // 1. Pick up new requests to be submitted from the pending list
            for (auto pendingRequestId : _pendingRequestIds) {
                const auto& requestData = _incompleteRequests.at(pendingRequestId);
                commandsToSubmit.push_back(requestData.getSubmissionInfo());
            }
            _pendingRequestIds.clear();
        }

        // 2. Serve the picked up requests, submitting their related commands.
        std::vector<CommandSubmissionResult> submissionResults;
        for (auto& submissionInfo : commandsToSubmit) {
            auto opCtxHolder = cc().makeOperationContext();
            if (submissionInfo.commandInfo) {
                submissionInfo.commandInfo.get()->attachOperationMetadataTo(opCtxHolder.get());
            }
            submissionResults.push_back(_submit(opCtxHolder.get(), submissionInfo));
            if (!submissionResults.back().context.isOK()) {
                LOGV2(5847206,
                      "Submission for scheduler command request failed",
                      "reqId"_attr = submissionResults.back().id,
                      "cause"_attr = submissionResults.back().context.getStatus());
            }
        }

        // 3. Process the outcome of each submission.
        int numRunningRequests = 0;
        int numPendingRequests = 0;
        {
            stdx::lock_guard<Latch> lg(_mutex);
            for (auto& submissionResult : submissionResults) {
                _applySubmissionResult(lg, std::move(submissionResult));
            }
            numRunningRequests = _runningRequestIds.size();
            numPendingRequests = _pendingRequestIds.size();
        }
        LOGV2_DEBUG(5847207,
                    1,
                    "Ending balancer command scheduler round",
                    "numRunningRequests"_attr = numRunningRequests,
                    "numPendingRequests"_attr = numPendingRequests);
    }

    // In case of clean exit, cancel all the pending/running command requests
    // (but keep the related descriptor documents to ensure they will be reissued on recovery).
    auto opCtxHolder = cc().makeOperationContext();
    auto executor = Grid::get(opCtxHolder.get())->getExecutorPool()->getFixedExecutor();
    for (auto& idAndRequest : requestsToCleanUpOnExit) {
        idAndRequest.second.setOutcome(Status(
            ErrorCodes::BalancerInterrupted, "Request cancelled - balancer scheduler is stopping"));
        const auto& cancelHandle = idAndRequest.second.getExecutionContext();
        if (cancelHandle) {
            executor->cancel(*cancelHandle);
        }
        _distributedLocks.releaseFor(opCtxHolder.get(),
                                     idAndRequest.second.getCommandInfo().getNameSpace());
    }
}


}  // namespace mongo
