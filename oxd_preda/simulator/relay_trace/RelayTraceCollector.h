#pragma once

#include "RelayTraceContext.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace oxd {

struct SimuTxn;

namespace relay_trace {

struct ExecutionBeginInput
{
	const SimuTxn *transaction = nullptr;
	rvm::ContractInvokeId contractInvoke = rvm::ContractInvokeIdInvalid;
	uint32_t opcode = 0;
	std::string moduleId;
	std::string sourceFunctionId;
	uint32_t ownerShard = NoTargetShard;
	bool isRelay = false;
};

struct RelayCreateInput
{
	const SimuTxn *parentTransaction = nullptr;
	const SimuTxn *childTransaction = nullptr;
	rvm::ContractInvokeId targetContractInvoke =
		rvm::ContractInvokeIdInvalid;
	uint32_t targetOpcode = 0;
	const uint8_t *serializedArguments = nullptr;
	size_t serializedArgumentsSize = 0;
	std::string targetModuleId;
	uint32_t ownerShard = NoTargetShard;
};

struct RelayFinalizeInput
{
	const SimuTxn *childTransaction = nullptr;
	const uint8_t *targetData = nullptr;
	size_t targetSize = 0;
	ScopeKind targetScope = ScopeKind::Unknown;
	RelayKind relayKind = RelayKind::Unknown;
	std::string relaySiteId;
};

// A bounded, owning view of the trace records produced directly by one
// executing microtransaction. Runtime validation uses this instead of
// copying and scanning the complete run-wide trace after every invocation.
struct RelayExecutionTraceSlice
{
	std::vector<RelayEmitTraceEvent> emissions;
	std::vector<RelayRouteTraceEvent> routes;
};

class RelayTraceCollector
{
public:
	explicit RelayTraceCollector(TraceMode mode = TraceMode::Off);

	TraceMode Mode() const;
	void SetMode(TraceMode mode);

	std::optional<RuntimeTxnTraceContext> BeginExecution(
		const ExecutionBeginInput &input);
	std::optional<RuntimeTxnTraceContext> Lookup(
		const SimuTxn *transaction) const;

	uint64_t PushMarker(
		const SimuTxn *transaction,
		const std::string &moduleId,
		RelaySiteOrdinal ordinal);
	MarkerOperationResult PopMarker(
		const SimuTxn *transaction,
		uint64_t generation,
		const std::string &expectedModuleId,
		RelaySiteOrdinal expectedOrdinal,
		bool requireConsumed = false);

	// Called at the real allocation point. It consumes the active marker and
	// installs owning child metadata, but deliberately does not publish a
	// logical emission yet: target/kind are initialized by the EmitRelay*
	// caller after SimuShard::_CreateRelayTxn returns.
	MarkerOperationResult RegisterRelayCreation(
		const RelayCreateInput &input);

	// Called exactly once after the original EmitRelay* implementation has
	// initialized target, flags and kind. This publishes one logical event.
	bool FinalizeRelayEmission(const RelayFinalizeInput &input);
	std::optional<RelayEmitTraceEvent> PendingRelayIdentity(
		const SimuTxn *childTransaction) const;
	bool SetPendingRelayResolvedIdentity(
		const SimuTxn *childTransaction,
		const std::string &relaySiteId,
		const std::string &sourceFunctionId);

	// Copies trace-only metadata for a physical broadcast clone. It does not
	// create another logical emission.
	bool CloneRelayMetadata(
		const SimuTxn *original,
		const SimuTxn *clone,
		uint32_t cloneOwnerShard);

	bool RecordRoute(
		const SimuTxn *transaction,
		uint32_t targetShard,
		RouteKind routeKind,
		uint32_t activeShardCount = 0);

	bool EndExecution(
		const SimuTxn *transaction,
		bool completed,
		bool succeeded);

	void AddValidationResult(const RelayValidationResult &result);
	void AddManifestLoadFailure(bool bindingFailure);
	void AddTimings(const RelayTraceTimings &timings);

	// Runtime observer boundaries call this from catch(...). It must never
	// throw back into PREDA execution. In strict mode it latches failure for
	// the simulator safe point; in observe mode execution continues.
	void RecordObserverFailureNoexcept(
		const char *operation) noexcept;

	bool StrictFailureLatched() const;
	RelayExecutionTraceSlice ExecutionSlice(uint64_t parentTraceTxId) const;
	RelayTraceSnapshot Snapshot() const;

	// Clear only live pointer-keyed metadata and marker stacks. Recorded,
	// owning events remain available for final reporting.
	void ShutdownClearLiveTransactions();
	void Reset();

private:
	struct PendingRelay
	{
		RelayEmitTraceEvent event;
		bool finalized = false;
	};

	struct Entry
	{
		RelayTraceContext context;
		std::optional<PendingRelay> pendingRelay;
		size_t executionEventIndex = std::numeric_limits<size_t>::max();
	};

	RelayValidationResult InstrumentationErrorLocked(
		const SimuTxn *transaction,
		const std::string &reason,
		RelaySiteOrdinal ordinal = InvalidRelaySiteOrdinal);
	void AddValidationResultLocked(const RelayValidationResult &result);
	uint64_t AllocateTraceId();

	mutable std::mutex m_mutex;
	std::unordered_map<const SimuTxn *, Entry> m_transactions;
	std::vector<RelayEmitTraceEvent> m_emissions;
	std::vector<RelayRouteTraceEvent> m_routes;
	std::vector<RelayExecutionTraceEvent> m_executions;
	std::vector<RelayValidationResult> m_validationResults;
	std::unordered_map<uint64_t, std::vector<size_t>>
		m_emissionIndicesByParent;
	std::unordered_map<uint64_t, std::vector<size_t>>
		m_routeIndicesByParent;
	RelayTraceCounters m_counters;
	RelayTraceTimings m_timings;

	std::atomic<uint64_t> m_nextTraceId{1};
	std::atomic<uint64_t> m_nextMarkerGeneration{1};
	std::atomic<uint64_t> m_traceRecordingNanoseconds{0};
	std::atomic<uint64_t> m_emergencyInstrumentationFailures{0};
	std::atomic<bool> m_strictFailure{false};
	std::atomic<bool> m_strictMode{false};
	TraceMode m_mode;
};

} // namespace relay_trace
} // namespace oxd
