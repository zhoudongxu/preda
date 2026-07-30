#include "RelayTraceCollector.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace oxd {
namespace relay_trace {
namespace {

class TraceRecordingTimer
{
public:
	explicit TraceRecordingTimer(std::atomic<uint64_t> &nanoseconds)
		: m_nanoseconds(nanoseconds),
		  m_begin(std::chrono::steady_clock::now())
	{
	}

	~TraceRecordingTimer()
	{
		const auto elapsed =
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - m_begin);
		m_nanoseconds.fetch_add(
			static_cast<uint64_t>(elapsed.count()),
			std::memory_order_relaxed);
	}

private:
	std::atomic<uint64_t> &m_nanoseconds;
	std::chrono::steady_clock::time_point m_begin;
};

} // namespace

RelayTraceCollector::RelayTraceCollector(TraceMode mode)
	: m_strictMode(mode == TraceMode::Strict),
	  m_mode(mode)
{
}

TraceMode RelayTraceCollector::Mode() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_mode;
}

void RelayTraceCollector::SetMode(TraceMode mode)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_mode = mode;
	m_strictMode.store(
		mode == TraceMode::Strict,
		std::memory_order_release);
}

uint64_t RelayTraceCollector::AllocateTraceId()
{
	return m_nextTraceId.fetch_add(1, std::memory_order_relaxed);
}

std::optional<RuntimeTxnTraceContext> RelayTraceCollector::BeginExecution(
	const ExecutionBeginInput &input)
{
	TraceRecordingTimer timer(m_traceRecordingNanoseconds);
	if (input.transaction == nullptr)
		return std::nullopt;

	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_mode == TraceMode::Off)
		return std::nullopt;

	auto found = m_transactions.find(input.transaction);
	if (found == m_transactions.end())
	{
		RuntimeTxnTraceContext context;
		context.traceTxId = AllocateTraceId();
		context.rootTraceTxId = context.traceTxId;
		context.contractInvoke = input.contractInvoke;
		context.opcode = input.opcode;
		context.moduleId = input.moduleId;
		context.sourceFunctionId = input.sourceFunctionId;
		context.ownerShard = input.ownerShard;
		context.isRelay = input.isRelay;

		if (input.isRelay)
		{
			InstrumentationErrorLocked(
				input.transaction,
				"relay execution has no trace side-table metadata");
		}
		found = m_transactions.emplace(
			input.transaction,
			Entry{RelayTraceContext(std::move(context))}).first;
	}

	RuntimeTxnTraceContext &context = found->second.context.Transaction();
	context.contractInvoke = input.contractInvoke;
	context.opcode = input.opcode;
	if (!input.moduleId.empty())
		context.moduleId = input.moduleId;
	if (!input.sourceFunctionId.empty())
		context.sourceFunctionId = input.sourceFunctionId;
	if (input.ownerShard != NoTargetShard)
		context.ownerShard = input.ownerShard;
	context.isRelay = input.isRelay || context.isRelay;

	RelayExecutionTraceEvent event;
	event.traceTxId = context.traceTxId;
	event.rootTraceTxId = context.rootTraceTxId;
	event.parentTraceTxId = context.parentTraceTxId;
	event.depth = context.depth;
	event.opcode = context.opcode;
	event.moduleId = context.moduleId;
	event.sourceFunctionId = context.sourceFunctionId;
	event.isRelay = context.isRelay;
	event.started = true;
	m_executions.push_back(std::move(event));
	found->second.executionEventIndex = m_executions.size() - 1;

	++m_counters.microtransactionsObserved;
	if (context.isRelay)
		++m_counters.relayExecutions;
	else
		++m_counters.sourceTransactionsObserved;
	m_counters.maximumObservedDepth =
		std::max(m_counters.maximumObservedDepth, context.depth);

	return context;
}

std::optional<RuntimeTxnTraceContext> RelayTraceCollector::Lookup(
	const SimuTxn *transaction) const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	auto found = m_transactions.find(transaction);
	if (found == m_transactions.end())
		return std::nullopt;
	return found->second.context.Transaction();
}

uint64_t RelayTraceCollector::PushMarker(
	const SimuTxn *transaction,
	const std::string &moduleId,
	RelaySiteOrdinal ordinal)
{
	TraceRecordingTimer timer(m_traceRecordingNanoseconds);
	if (transaction == nullptr)
		return 0;

	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_mode == TraceMode::Off)
		return 0;

	auto found = m_transactions.find(transaction);
	if (found == m_transactions.end())
	{
		InstrumentationErrorLocked(
			transaction,
			"cannot install relay-site marker without active execution",
			ordinal);
		return 0;
	}

	const uint64_t generation =
		m_nextMarkerGeneration.fetch_add(1, std::memory_order_relaxed);
	return found->second.context.PushMarker(
		moduleId,
		ordinal,
		generation);
}

MarkerOperationResult RelayTraceCollector::PopMarker(
	const SimuTxn *transaction,
	uint64_t generation,
	const std::string &expectedModuleId,
	RelaySiteOrdinal expectedOrdinal,
	bool requireConsumed)
{
	TraceRecordingTimer timer(m_traceRecordingNanoseconds);
	if (generation == 0 && Mode() == TraceMode::Off)
		return {};

	std::lock_guard<std::mutex> lock(m_mutex);
	auto found = m_transactions.find(transaction);
	if (found == m_transactions.end())
	{
		MarkerOperationResult result{
			MarkerOperationStatus::NoActiveTransaction,
			"cannot restore marker without active execution",
			std::nullopt,
		};
		InstrumentationErrorLocked(transaction, result.reason);
		return result;
	}

	MarkerOperationResult result =
		found->second.context.PopMarker(
			generation,
			expectedModuleId,
			expectedOrdinal,
			requireConsumed);
	if (!result)
	{
		InstrumentationErrorLocked(
			transaction,
			result.reason,
			result.marker
				? result.marker->ordinal
				: InvalidRelaySiteOrdinal);
	}
	return result;
}

MarkerOperationResult RelayTraceCollector::RegisterRelayCreation(
	const RelayCreateInput &input)
{
	TraceRecordingTimer timer(m_traceRecordingNanoseconds);
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_mode == TraceMode::Off)
		return {};

	auto parent = m_transactions.find(input.parentTransaction);
	if (parent == m_transactions.end())
	{
		MarkerOperationResult result{
			MarkerOperationStatus::NoActiveTransaction,
			"relay creation has no active parent transaction",
			std::nullopt,
		};
		InstrumentationErrorLocked(input.parentTransaction, result.reason);
		return result;
	}
	if (input.childTransaction == nullptr)
	{
		MarkerOperationResult result{
			MarkerOperationStatus::NoActiveTransaction,
			"relay creation returned a null child transaction",
			std::nullopt,
		};
		InstrumentationErrorLocked(input.parentTransaction, result.reason);
		return result;
	}
	if (m_transactions.find(input.childTransaction) != m_transactions.end())
	{
		MarkerOperationResult result{
			MarkerOperationStatus::DuplicateConsume,
			"child transaction is already registered",
			std::nullopt,
		};
		InstrumentationErrorLocked(input.parentTransaction, result.reason);
		return result;
	}

	MarkerOperationResult marker = parent->second.context.ConsumeMarker();
	const bool markerTrusted = static_cast<bool>(marker);
	if (!markerTrusted)
	{
		InstrumentationErrorLocked(
			input.parentTransaction,
			marker.reason,
			marker.marker
				? marker.marker->ordinal
				: InvalidRelaySiteOrdinal);
		RelaySiteMarkerFrame invalid;
		invalid.ordinal = InvalidRelaySiteOrdinal;
		invalid.traceTxId =
			parent->second.context.Transaction().traceTxId;
		invalid.consumed = true;
		marker.marker = std::move(invalid);
	}

	const RuntimeTxnTraceContext &parentContext =
		parent->second.context.Transaction();
	const uint32_t occurrence =
		parent->second.context.NextOccurrence(
			marker.marker->moduleId,
			marker.marker->ordinal);

	RuntimeTxnTraceContext childContext;
	childContext.traceTxId = AllocateTraceId();
	childContext.rootTraceTxId = parentContext.rootTraceTxId;
	childContext.parentTraceTxId = parentContext.traceTxId;
	childContext.contractInvoke = input.targetContractInvoke;
	childContext.opcode = input.targetOpcode;
	childContext.moduleId = input.targetModuleId;
	childContext.depth = parentContext.depth + 1;
	childContext.ownerShard = input.ownerShard;
	childContext.isRelay = true;

	PendingRelay pending;
	pending.event.childTraceTxId = childContext.traceTxId;
	pending.event.rootTraceTxId = childContext.rootTraceTxId;
	pending.event.parentTraceTxId = childContext.parentTraceTxId;
	pending.event.relaySiteOrdinal = marker.marker->ordinal;
	pending.event.occurrenceIndex = occurrence;
	pending.event.depth = childContext.depth;
	pending.event.sourceModuleId = marker.marker->moduleId;
	if (marker.marker->moduleId == parentContext.moduleId)
		pending.event.sourceFunctionId = parentContext.sourceFunctionId;
	pending.event.targetContractInvoke = input.targetContractInvoke;
	pending.event.actualOpcode = input.targetOpcode;
	if (input.serializedArguments != nullptr &&
		input.serializedArgumentsSize != 0)
	{
		pending.event.actualSerializedArgs.assign(
			input.serializedArguments,
			input.serializedArguments + input.serializedArgumentsSize);
	}

	Entry child{RelayTraceContext(std::move(childContext))};
	child.pendingRelay = std::move(pending);
	m_transactions.emplace(input.childTransaction, std::move(child));
	return marker;
}

bool RelayTraceCollector::FinalizeRelayEmission(
	const RelayFinalizeInput &input)
{
	TraceRecordingTimer timer(m_traceRecordingNanoseconds);
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_mode == TraceMode::Off)
		return true;

	auto child = m_transactions.find(input.childTransaction);
	if (child == m_transactions.end() || !child->second.pendingRelay)
	{
		InstrumentationErrorLocked(
			input.childTransaction,
			"relay emission finalization has no pending child metadata");
		return false;
	}
	PendingRelay &pending = *child->second.pendingRelay;
	if (pending.finalized)
	{
		InstrumentationErrorLocked(
			input.childTransaction,
			"relay emission was finalized more than once",
			pending.event.relaySiteOrdinal);
		return false;
	}

	pending.event.actualTarget.Assign(input.targetData, input.targetSize);
	pending.event.actualTargetScope = input.targetScope;
	pending.event.relayKind = input.relayKind;
	if (!input.relaySiteId.empty())
		pending.event.relaySiteId = input.relaySiteId;
	pending.finalized = true;
	m_emissionIndicesByParent[pending.event.parentTraceTxId].push_back(
		m_emissions.size());
	m_emissions.push_back(pending.event);

	++m_counters.logicalRelayEmissions;
	if (input.relayKind == RelayKind::AllShards)
		++m_counters.broadcastLogicalEmissions;
	return true;
}

std::optional<RelayEmitTraceEvent>
RelayTraceCollector::PendingRelayIdentity(
	const SimuTxn *childTransaction) const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	auto child = m_transactions.find(childTransaction);
	if (child == m_transactions.end() || !child->second.pendingRelay)
		return std::nullopt;
	return child->second.pendingRelay->event;
}

bool RelayTraceCollector::SetPendingRelayResolvedIdentity(
	const SimuTxn *childTransaction,
	const std::string &relaySiteId,
	const std::string &sourceFunctionId)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_mode == TraceMode::Off)
		return true;
	auto child = m_transactions.find(childTransaction);
	if (child == m_transactions.end() || !child->second.pendingRelay)
	{
		InstrumentationErrorLocked(
			childTransaction,
			"cannot resolve identity without pending relay metadata");
		return false;
	}
	if (child->second.pendingRelay->finalized)
	{
		InstrumentationErrorLocked(
			childTransaction,
			"cannot change relay identity after logical emission finalization",
			child->second.pendingRelay->event.relaySiteOrdinal);
		return false;
	}
	child->second.pendingRelay->event.relaySiteId = relaySiteId;
	child->second.pendingRelay->event.sourceFunctionId = sourceFunctionId;
	return true;
}

bool RelayTraceCollector::CloneRelayMetadata(
	const SimuTxn *original,
	const SimuTxn *clone,
	uint32_t cloneOwnerShard)
{
	TraceRecordingTimer timer(m_traceRecordingNanoseconds);
	if (clone == nullptr)
		return false;

	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_mode == TraceMode::Off)
		return true;

	auto source = m_transactions.find(original);
	if (source == m_transactions.end() || !source->second.pendingRelay)
	{
		InstrumentationErrorLocked(
			original,
			"broadcast clone source has no relay trace metadata");
		return false;
	}
	if (m_transactions.find(clone) != m_transactions.end())
	{
		InstrumentationErrorLocked(
			clone,
			"broadcast clone is already registered",
			source->second.pendingRelay->event.relaySiteOrdinal);
		return false;
	}

	RuntimeTxnTraceContext context = source->second.context.Transaction();
	context.traceTxId = AllocateTraceId();
	context.ownerShard = cloneOwnerShard;

	Entry cloneEntry{RelayTraceContext(std::move(context))};
	cloneEntry.pendingRelay = source->second.pendingRelay;
	cloneEntry.pendingRelay->event.childTraceTxId =
		cloneEntry.context.Transaction().traceTxId;
	m_transactions.emplace(clone, std::move(cloneEntry));
	return true;
}

bool RelayTraceCollector::RecordRoute(
	const SimuTxn *transaction,
	uint32_t targetShard,
	RouteKind routeKind,
	uint32_t activeShardCount)
{
	TraceRecordingTimer timer(m_traceRecordingNanoseconds);
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_mode == TraceMode::Off)
		return true;

	auto found = m_transactions.find(transaction);
	if (found == m_transactions.end() || !found->second.pendingRelay)
	{
		InstrumentationErrorLocked(
			transaction,
			"physical route has no relay trace metadata");
		return false;
	}

	const PendingRelay &pending = *found->second.pendingRelay;
	if (!pending.finalized)
	{
		InstrumentationErrorLocked(
			transaction,
			"physical route was recorded before logical emission finalization",
			pending.event.relaySiteOrdinal);
		return false;
	}

	RelayRouteTraceEvent event;
	event.physicalTraceTxId =
		found->second.context.Transaction().traceTxId;
	event.rootTraceTxId = pending.event.rootTraceTxId;
	event.parentTraceTxId = pending.event.parentTraceTxId;
	event.relaySiteOrdinal = pending.event.relaySiteOrdinal;
	event.relaySiteId = pending.event.relaySiteId;
	event.sourceModuleId = pending.event.sourceModuleId;
	event.occurrenceIndex = pending.event.occurrenceIndex;
	event.targetShard = targetShard;
	event.activeShardCount = activeShardCount;
	event.routeKind = routeKind;
	m_routeIndicesByParent[event.parentTraceTxId].push_back(
		m_routes.size());
	m_routes.push_back(std::move(event));

	++m_counters.physicalRelayRoutes;
	switch (routeKind)
	{
	case RouteKind::IntraShard: ++m_counters.intraShardRelays; break;
	case RouteKind::CrossShard: ++m_counters.crossShardRelays; break;
	case RouteKind::Global: ++m_counters.globalRelays; break;
	case RouteKind::AllShardsBroadcast:
		++m_counters.broadcastPhysicalClones;
		break;
	case RouteKind::DeferredNext: ++m_counters.deferredRelays; break;
	case RouteKind::Unknown: break;
	}
	return true;
}

bool RelayTraceCollector::EndExecution(
	const SimuTxn *transaction,
	bool completed,
	bool succeeded)
{
	TraceRecordingTimer timer(m_traceRecordingNanoseconds);
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_mode == TraceMode::Off)
		return true;

	auto found = m_transactions.find(transaction);
	if (found == m_transactions.end())
	{
		InstrumentationErrorLocked(
			transaction,
			"execution end has no active trace context");
		return false;
	}

	if (found->second.context.HasMarkers())
	{
		InstrumentationErrorLocked(
			transaction,
			"execution ended with un-restored relay-site marker frames");
	}

	if (found->second.executionEventIndex < m_executions.size())
	{
		RelayExecutionTraceEvent &event =
			m_executions[found->second.executionEventIndex];
		event.completed = completed;
		event.succeeded = succeeded;
	}
	else
	{
		InstrumentationErrorLocked(
			transaction,
			"execution end has no matching execution-begin event");
	}

	m_transactions.erase(found);
	return true;
}

RelayValidationResult RelayTraceCollector::InstrumentationErrorLocked(
	const SimuTxn *transaction,
	const std::string &reason,
	RelaySiteOrdinal ordinal)
{
	RelayValidationResult result;
	result.status = ValidationStatus::TraceInstrumentationError;
	result.checkKind = ValidationCheckKind::Instrumentation;
	result.reason = reason;
	result.detail.checkKind = ValidationCheckKind::Instrumentation;
	result.detail.relaySiteOrdinal = ordinal;
	result.detail.diagnosticReason = reason;

	auto found = m_transactions.find(transaction);
	if (found != m_transactions.end())
	{
		const RuntimeTxnTraceContext &context =
			found->second.context.Transaction();
		result.detail.rootTraceTxId = context.rootTraceTxId;
		result.detail.parentTraceTxId = context.parentTraceTxId;
		result.detail.currentTraceTxId = context.traceTxId;
		result.detail.function = context.sourceFunctionId;
		result.detail.opcode = context.opcode;
		result.detail.moduleIdentity = context.moduleId;
	}
	AddValidationResultLocked(result);
	return result;
}

void RelayTraceCollector::AddValidationResultLocked(
	const RelayValidationResult &result)
{
	m_validationResults.push_back(result);
	++m_counters.checksByKind[result.checkKind];
	++m_counters.checksByKindAndStatus[result.checkKind][result.status];
	switch (result.status)
	{
	case ValidationStatus::Passed:
		++m_counters.checksPassed;
		break;
	case ValidationStatus::Mismatch:
		++m_counters.checksMismatched;
		if (m_mode == TraceMode::Strict)
			m_strictFailure.store(true, std::memory_order_release);
		break;
	case ValidationStatus::TraceInstrumentationError:
		++m_counters.instrumentationFailures;
		++m_counters.checksMismatched;
		if (m_mode == TraceMode::Strict)
			m_strictFailure.store(true, std::memory_order_release);
		break;
	default:
		++m_counters.checksSkipped;
		break;
	}
}

void RelayTraceCollector::AddValidationResult(
	const RelayValidationResult &result)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_mode != TraceMode::Off)
		AddValidationResultLocked(result);
}

void RelayTraceCollector::AddManifestLoadFailure(bool bindingFailure)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	++m_counters.manifestLoadFailures;
	if (bindingFailure)
		++m_counters.manifestBindingFailures;
	if (m_mode == TraceMode::Strict)
		m_strictFailure.store(true, std::memory_order_release);
}

void RelayTraceCollector::AddTimings(const RelayTraceTimings &timings)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_timings.manifestLoadTimeMs += timings.manifestLoadTimeMs;
	m_timings.traceRecordingTimeMs += timings.traceRecordingTimeMs;
	m_timings.validationTimeMs += timings.validationTimeMs;
	m_timings.reportSerializationTimeMs +=
		timings.reportSerializationTimeMs;
}

void RelayTraceCollector::RecordObserverFailureNoexcept(
	const char *operation) noexcept
{
	try
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_mode == TraceMode::Off)
			return;

		RelayValidationResult result;
		result.status =
			ValidationStatus::TraceInstrumentationError;
		result.checkKind =
			ValidationCheckKind::Instrumentation;
		result.reason =
			"runtime trace observer raised an exception";
		if (operation != nullptr && *operation != '\0')
		{
			result.reason += " during ";
			result.reason += operation;
		}
		result.detail.checkKind = result.checkKind;
		result.detail.diagnosticReason = result.reason;
		AddValidationResultLocked(result);
	}
	catch (...)
	{
		// Even diagnostics can fail under allocation pressure. Preserve a
		// counter and the strict-mode latch using only lock-free atomics.
		m_emergencyInstrumentationFailures.fetch_add(
			1,
			std::memory_order_relaxed);
		if (m_strictMode.load(std::memory_order_acquire))
			m_strictFailure.store(true, std::memory_order_release);
	}
}

bool RelayTraceCollector::StrictFailureLatched() const
{
	return m_strictFailure.load(std::memory_order_acquire);
}

RelayExecutionTraceSlice RelayTraceCollector::ExecutionSlice(
	uint64_t parentTraceTxId) const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	RelayExecutionTraceSlice result;

	auto emissionIndices =
		m_emissionIndicesByParent.find(parentTraceTxId);
	if (emissionIndices != m_emissionIndicesByParent.end())
	{
		result.emissions.reserve(emissionIndices->second.size());
		for (size_t index : emissionIndices->second)
		{
			if (index < m_emissions.size())
				result.emissions.push_back(m_emissions[index]);
		}
	}

	auto routeIndices = m_routeIndicesByParent.find(parentTraceTxId);
	if (routeIndices != m_routeIndicesByParent.end())
	{
		result.routes.reserve(routeIndices->second.size());
		for (size_t index : routeIndices->second)
		{
			if (index < m_routes.size())
				result.routes.push_back(m_routes[index]);
		}
	}
	return result;
}

RelayTraceSnapshot RelayTraceCollector::Snapshot() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	RelayTraceSnapshot result;
	result.counters = m_counters;
	const uint64_t emergencyFailures =
		m_emergencyInstrumentationFailures.load(
			std::memory_order_relaxed);
	result.counters.instrumentationFailures += emergencyFailures;
	result.counters.checksMismatched += emergencyFailures;
	result.counters.checksByKind[
		ValidationCheckKind::Instrumentation] += emergencyFailures;
	result.counters.checksByKindAndStatus[
		ValidationCheckKind::Instrumentation]
		[ValidationStatus::TraceInstrumentationError] +=
			emergencyFailures;
	result.timings = m_timings;
	result.timings.traceRecordingTimeMs +=
		static_cast<double>(
			m_traceRecordingNanoseconds.load(
				std::memory_order_relaxed)) /
		1000000.0;
	result.emissions = m_emissions;
	result.routes = m_routes;
	result.executions = m_executions;
	result.validationResults = m_validationResults;
	return result;
}

void RelayTraceCollector::ShutdownClearLiveTransactions()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_transactions.clear();
}

void RelayTraceCollector::Reset()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_transactions.clear();
	m_emissions.clear();
	m_routes.clear();
	m_executions.clear();
	m_validationResults.clear();
	m_emissionIndicesByParent.clear();
	m_routeIndicesByParent.clear();
	m_counters = {};
	m_timings = {};
	m_nextTraceId.store(1, std::memory_order_relaxed);
	m_nextMarkerGeneration.store(1, std::memory_order_relaxed);
	m_traceRecordingNanoseconds.store(0, std::memory_order_relaxed);
	m_emergencyInstrumentationFailures.store(
		0,
		std::memory_order_relaxed);
	m_strictFailure.store(false, std::memory_order_release);
}

} // namespace relay_trace
} // namespace oxd
