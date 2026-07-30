#include "RelayTraceReport.h"

#include "../../3rdParty/nlohmann/json.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace oxd {
namespace relay_trace {
namespace {

using Json = nlohmann::ordered_json;

Json LocationJson(const TraceSourceLocation &location)
{
	return Json{
		{"line", location.line},
		{"column", location.column},
		{"end_line", location.endLine},
		{"end_column", location.endColumn},
		{"start_offset", location.startOffset},
		{"end_offset", location.endOffset},
	};
}

Json ArtifactJson(const ArtifactIdentity &identity)
{
	return Json{
		{"dapp", identity.dapp},
		{"contract", identity.contract},
		{"transpiler_version", identity.transpilerVersion},
		{"intermediate_hash", identity.intermediateHash},
		{"module_id", identity.moduleId},
		{"module_hash_kind", identity.moduleHashKind},
		{"module_hash", identity.moduleHash},
		{"manifest_hash_algorithm", identity.manifestHashAlgorithm},
		{"manifest_hash", identity.manifestHash},
		{"binding_complete", identity.bindingComplete},
	};
}

Json MismatchJson(const RelayTraceMismatch &mismatch)
{
	return Json{
		{"root_trace_tx_id", mismatch.rootTraceTxId},
		{"parent_trace_tx_id", mismatch.parentTraceTxId},
		{"current_trace_tx_id", mismatch.currentTraceTxId},
		{"contract", mismatch.contract},
		{"function", mismatch.function},
		{"opcode", mismatch.opcode},
		{"relay_site_id", mismatch.relaySiteId},
		{"relay_site_ordinal", mismatch.relaySiteOrdinal},
		{"occurrence_index", mismatch.occurrenceIndex},
		{"check_kind", ToString(mismatch.checkKind)},
		{"expected", mismatch.expected},
		{"actual", mismatch.actual},
		{"source_location", LocationJson(mismatch.sourceLocation)},
		{"manifest_identity", ArtifactJson(mismatch.manifestIdentity)},
		{"module_identity", mismatch.moduleIdentity},
		{"diagnostic_reason", mismatch.diagnosticReason},
	};
}

Json ValidationJson(const RelayValidationResult &result)
{
	return Json{
		{"status", ToString(result.status)},
		{"check_kind", ToString(result.checkKind)},
		{"reason", result.reason},
		{"detail", MismatchJson(result.detail)},
	};
}

Json CountersJson(const RelayTraceCounters &counters)
{
	Json byKind = Json::object();
	for (const auto &entry : counters.checksByKind)
		byKind[ToString(entry.first)] = entry.second;

	Json byKindAndStatus = Json::object();
	for (const auto &kindEntry : counters.checksByKindAndStatus)
	{
		Json byStatus = Json::object();
		for (const auto &statusEntry : kindEntry.second)
			byStatus[ToString(statusEntry.first)] = statusEntry.second;
		byKindAndStatus[ToString(kindEntry.first)] = std::move(byStatus);
	}

	return Json{
		{"source_transactions_observed",
		 counters.sourceTransactionsObserved},
		{"microtransactions_observed", counters.microtransactionsObserved},
		{"logical_relay_emissions", counters.logicalRelayEmissions},
		{"physical_relay_routes", counters.physicalRelayRoutes},
		{"relay_executions", counters.relayExecutions},
		{"intra_shard_relays", counters.intraShardRelays},
		{"cross_shard_relays", counters.crossShardRelays},
		{"global_relays", counters.globalRelays},
		{"broadcast_logical_emissions",
		 counters.broadcastLogicalEmissions},
		{"broadcast_physical_clones",
		 counters.broadcastPhysicalClones},
		{"deferred_relays", counters.deferredRelays},
		{"maximum_observed_depth", counters.maximumObservedDepth},
		{"checks_passed", counters.checksPassed},
		{"checks_mismatched", counters.checksMismatched},
		{"checks_skipped", counters.checksSkipped},
		{"manifest_load_failures", counters.manifestLoadFailures},
		{"manifest_binding_failures", counters.manifestBindingFailures},
		{"instrumentation_failures", counters.instrumentationFailures},
		{"checks_by_kind", std::move(byKind)},
		{"checks_by_kind_and_status", std::move(byKindAndStatus)},
	};
}

Json TimingsJson(const RelayTraceTimings &timings)
{
	return Json{
		{"manifest_load_time_ms", timings.manifestLoadTimeMs},
		{"trace_recording_time_ms", timings.traceRecordingTimeMs},
		{"validation_time_ms", timings.validationTimeMs},
		{"report_serialization_time_ms",
		 timings.reportSerializationTimeMs},
	};
}

bool RouteBelongsToEmission(
	const RelayRouteTraceEvent &route,
	const RelayEmitTraceEvent &emission)
{
	return route.rootTraceTxId == emission.rootTraceTxId &&
		route.parentTraceTxId == emission.parentTraceTxId &&
		route.sourceModuleId == emission.sourceModuleId &&
		route.relaySiteOrdinal == emission.relaySiteOrdinal &&
		route.occurrenceIndex == emission.occurrenceIndex;
}

size_t BroadcastPhysicalCloneCount(
	const RelayEmitTraceEvent &event,
	const std::vector<RelayRouteTraceEvent> &routes)
{
	if (event.relayKind != RelayKind::AllShards)
		return 0;
	return static_cast<size_t>(std::count_if(
		routes.begin(),
		routes.end(),
		[&event](const RelayRouteTraceEvent &route)
		{
			return route.routeKind == RouteKind::AllShardsBroadcast &&
				RouteBelongsToEmission(route, event);
		}));
}

Json EmissionJson(
	const RelayEmitTraceEvent &event,
	const std::vector<RelayRouteTraceEvent> &routes)
{
	return Json{
		{"child_trace_tx_id", event.childTraceTxId},
		{"root_trace_tx_id", event.rootTraceTxId},
		{"parent_trace_tx_id", event.parentTraceTxId},
		{"relay_site_ordinal", event.relaySiteOrdinal},
		{"relay_site_id", event.relaySiteId},
		{"occurrence_index", event.occurrenceIndex},
		{"depth", event.depth},
		{"source_module_id", event.sourceModuleId},
		{"source_function_id", event.sourceFunctionId},
		{"target_contract_invoke",
		 static_cast<uint64_t>(event.targetContractInvoke)},
		{"actual_target_hex",
		 BytesToHex(
			event.actualTarget.bytes.data(),
			event.actualTarget.bytes.size())},
		{"actual_target_scope", ToString(event.actualTargetScope)},
		{"actual_opcode", event.actualOpcode},
		{"actual_serialized_args_hex",
		 BytesToHex(
			event.actualSerializedArgs.data(),
			event.actualSerializedArgs.size())},
		{"relay_kind", ToString(event.relayKind)},
		{"physical_clone_count",
		 BroadcastPhysicalCloneCount(event, routes)},
	};
}

Json RouteJson(const RelayRouteTraceEvent &event)
{
	return Json{
		{"physical_trace_tx_id", event.physicalTraceTxId},
		{"root_trace_tx_id", event.rootTraceTxId},
		{"parent_trace_tx_id", event.parentTraceTxId},
		{"relay_site_ordinal", event.relaySiteOrdinal},
		{"relay_site_id", event.relaySiteId},
		{"source_module_id", event.sourceModuleId},
		{"occurrence_index", event.occurrenceIndex},
		{"target_shard", event.targetShard},
		{"active_shard_count", event.activeShardCount},
		{"route_kind", ToString(event.routeKind)},
	};
}

Json ExecutionJson(const RelayExecutionTraceEvent &event)
{
	return Json{
		{"trace_tx_id", event.traceTxId},
		{"root_trace_tx_id", event.rootTraceTxId},
		{"parent_trace_tx_id", event.parentTraceTxId},
		{"depth", event.depth},
		{"opcode", event.opcode},
		{"module_id", event.moduleId},
		{"source_function_id", event.sourceFunctionId},
		{"is_relay", event.isRelay},
		{"started", event.started},
		{"completed", event.completed},
		{"succeeded", event.succeeded},
	};
}

Json ReportJson(const RelayTraceSnapshot &snapshot)
{
	Json root{
		{"report_schema_version", 1},
		{"counters", CountersJson(snapshot.counters)},
		{"timings", TimingsJson(snapshot.timings)},
		{"logical_relay_emissions", Json::array()},
		{"physical_relay_routes", Json::array()},
		{"relay_executions", Json::array()},
		{"validation_results", Json::array()},
	};
	for (const RelayEmitTraceEvent &event : snapshot.emissions)
		root["logical_relay_emissions"].push_back(
			EmissionJson(event, snapshot.routes));
	for (const RelayRouteTraceEvent &event : snapshot.routes)
		root["physical_relay_routes"].push_back(RouteJson(event));
	for (const RelayExecutionTraceEvent &event : snapshot.executions)
		root["relay_executions"].push_back(ExecutionJson(event));
	for (const RelayValidationResult &result : snapshot.validationResults)
		root["validation_results"].push_back(ValidationJson(result));
	return root;
}

} // namespace

RelayTraceSerializedReport RelayTraceReport::BuildJson(
	const RelayTraceSnapshot &snapshot,
	bool pretty) const
{
	const auto begin = std::chrono::steady_clock::now();
	RelayTraceSerializedReport result;
	result.contents = ReportJson(snapshot).dump(pretty ? 2 : -1);
	const auto end = std::chrono::steady_clock::now();
	result.elapsedTimeMs =
		std::chrono::duration<double, std::milli>(end - begin).count();
	return result;
}

RelayTraceSerializedReport RelayTraceReport::BuildJsonLines(
	const RelayTraceSnapshot &snapshot) const
{
	const auto begin = std::chrono::steady_clock::now();
	std::ostringstream output;
	output << Json{
		{"record_type", "summary"},
		{"report_schema_version", 1},
		{"counters", CountersJson(snapshot.counters)},
		{"timings", TimingsJson(snapshot.timings)},
	}.dump() << '\n';
	for (const RelayEmitTraceEvent &event : snapshot.emissions)
	{
		output << Json{
			{"record_type", "logical_relay_emission"},
			{"event", EmissionJson(event, snapshot.routes)},
		}.dump() << '\n';
	}
	for (const RelayRouteTraceEvent &event : snapshot.routes)
	{
		output << Json{
			{"record_type", "physical_relay_route"},
			{"event", RouteJson(event)},
		}.dump() << '\n';
	}
	for (const RelayExecutionTraceEvent &event : snapshot.executions)
	{
		output << Json{
			{"record_type", "relay_execution"},
			{"event", ExecutionJson(event)},
		}.dump() << '\n';
	}
	for (const RelayValidationResult &validation :
		snapshot.validationResults)
	{
		// Mismatches and instrumentation failures are never sampled or
		// dropped; every validation result has its own JSONL record.
		output << Json{
			{"record_type", "validation"},
			{"result", ValidationJson(validation)},
		}.dump() << '\n';
	}

	RelayTraceSerializedReport result;
	result.contents = output.str();
	const auto end = std::chrono::steady_clock::now();
	result.elapsedTimeMs =
		std::chrono::duration<double, std::milli>(end - begin).count();
	return result;
}

bool RelayTraceReport::WriteAtomically(
	const std::string &path,
	const std::string &contents,
	std::string *error) const
{
	std::lock_guard<std::mutex> lock(m_writeMutex);
	try
	{
		const std::filesystem::path destination(path);
		if (!destination.parent_path().empty())
			std::filesystem::create_directories(destination.parent_path());
		const std::filesystem::path temporary =
			destination.string() + ".tmp";
		{
			std::ofstream output(
				temporary,
				std::ios::binary | std::ios::trunc);
			if (!output)
			{
				if (error)
					*error = "cannot open temporary trace report";
				return false;
			}
			output.write(contents.data(), contents.size());
			if (!output)
			{
				if (error)
					*error = "cannot write temporary trace report";
				return false;
			}
		}
		std::error_code renameError;
		std::filesystem::rename(temporary, destination, renameError);
		if (renameError)
		{
			std::error_code removeError;
			std::filesystem::remove(destination, removeError);
			renameError.clear();
			std::filesystem::rename(temporary, destination, renameError);
		}
		if (renameError)
		{
			if (error)
				*error =
					"cannot atomically publish trace report: " +
					renameError.message();
			return false;
		}
		return true;
	}
	catch (const std::exception &exception)
	{
		if (error)
			*error = exception.what();
		return false;
	}
}

bool RelayTraceReport::WriteJson(
	const std::string &path,
	const RelayTraceSnapshot &snapshot,
	std::string *error) const
{
	return WriteAtomically(path, BuildJson(snapshot).contents, error);
}

bool RelayTraceReport::WriteJsonLines(
	const std::string &path,
	const RelayTraceSnapshot &snapshot,
	std::string *error) const
{
	return WriteAtomically(path, BuildJsonLines(snapshot).contents, error);
}

} // namespace relay_trace
} // namespace oxd
