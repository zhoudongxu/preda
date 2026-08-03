#include "RelayOptimizationReport.h"

#include "../../3rdParty/nlohmann/json.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <thread>

namespace oxd {
namespace relay_optimization {
namespace {

using Json = nlohmann::ordered_json;

struct AuditAccounting
{
	uint64_t pending = 0;
	bool consistent = true;
};

AuditAccounting GetAuditAccounting(
	const OptimizationMetricsSnapshot &metrics)
{
	AuditAccounting out;
	if(metrics.auditPassed >
		std::numeric_limits<uint64_t>::max() - metrics.auditFailed)
	{
		out.consistent = false;
		return out;
	}
	const uint64_t resolved =
		metrics.auditPassed + metrics.auditFailed;
	if(resolved > metrics.auditSamples)
	{
		out.consistent = false;
		return out;
	}
	out.pending = metrics.auditSamples - resolved;
	return out;
}

Json PlanCountersJson(
	const relay_plan::RelayPlanMetricsSnapshot &metrics)
{
	return Json{
		{"plan_loads", metrics.planLoads},
		{"plan_load_failures", metrics.planLoadFailures},
		{"plan_binding_failures", metrics.planBindingFailures},
		{"plan_cache_hits", metrics.planCacheHits},
		{"plan_cache_misses", metrics.planCacheMisses},
		{"plan_invalidations", metrics.planInvalidations},
		{"plan_reloads", metrics.planReloads},
		{"optimization_eligible_lookups",
		 metrics.optimizationEligibleLookups},
		{"optimization_fallback_lookups",
		 metrics.optimizationFallbackLookups},
	};
}

Json CountersJson(
	const OptimizationMetricsSnapshot &metrics)
{
	const AuditAccounting audit = GetAuditAccounting(metrics);
	Json counters = PlanCountersJson(metrics.plan);
	counters.update(Json{
		{"optimization_eligible_invocations",
		 metrics.optimizationEligibleInvocations},
		{"optimization_fallback_invocations",
		 metrics.optimizationFallbackInvocations},
		{"relay_buffer_reserve_calls",
		 metrics.relayBufferReserveCalls},
		{"relay_buffer_reserved_elements",
		 metrics.relayBufferReservedElements},
		{"relay_buffer_reserve_skipped_unknown",
		 metrics.relayBufferReserveSkippedUnknown},
		{"relay_buffer_reserve_skipped_limit",
		 metrics.relayBufferReserveSkippedLimit},
		{"relay_buffer_reserve_failures",
		 metrics.relayBufferReserveFailures},
		{"relay_buffer_capacity_misses",
		 metrics.relayBufferCapacityMisses},
		{"relay_buffer_capacity_growth_events",
		 metrics.relayBufferCapacityGrowthEvents},
		{"broadcast_clone_reserve_calls",
		 metrics.broadcastCloneReserveCalls},
		{"queue_single_push_calls", metrics.queueSinglePushCalls},
		{"queue_legacy_bulk_push_calls",
		 metrics.queueLegacyBulkPushCalls},
		{"queue_batch_push_calls", metrics.queueBatchPushCalls},
		{"queue_batch_elements", metrics.queueBatchElements},
		{"queue_lock_acquisitions", metrics.queueLockAcquisitions},
		{"queue_notifications", metrics.queueNotifications},
		{"maximum_batch_size", metrics.maximumBatchSize},
		{"queue_batch_fallbacks", metrics.queueBatchFallbacks},
		{"logical_relay_emissions", metrics.logicalRelayEmissions},
		{"physical_relay_routes", metrics.physicalRelayRoutes},
		{"relay_executions", metrics.relayExecutions},
		{"broadcast_physical_clones",
		 metrics.broadcastPhysicalClones},
		{"audit_samples", metrics.auditSamples},
			{"audit_passed", metrics.auditPassed},
			{"audit_failed", metrics.auditFailed},
			{"audit_pending", audit.pending},
			{"audit_accounting_consistent", audit.consistent},
		});
	return counters;
}

Json TimingJson(const OptimizationMetricsSnapshot &metrics)
{
	return Json{
		{"plan_lookup_time_ns", metrics.planLookupTimeNs},
		{"reserve_time_ns", metrics.reserveTimeNs},
		{"relay_generation_time_ns", metrics.relayGenerationTimeNs},
		{"routing_time_ns", metrics.routingTimeNs},
		{"dispatch_time_ns", metrics.dispatchTimeNs},
		{"queue_push_time_ns", metrics.queuePushTimeNs},
	};
}

Json DerivedJson(const OptimizationMetricsSnapshot &metrics)
{
	const uint64_t batchCount = metrics.queueBatchPushCalls;
	return Json{
		{"average_batch_size_numerator",
		 metrics.queueBatchElements},
		{"average_batch_size_denominator", batchCount},
		{"average_batch_size",
		 batchCount
			? static_cast<double>(metrics.queueBatchElements) /
				static_cast<double>(batchCount)
			: 0.0},
	};
}

Json MetricsScopeJson(const OptimizationMetricsSnapshot &metrics)
{
	return Json{
		{"counters", CountersJson(metrics)},
		{"timings_ns", TimingJson(metrics)},
		{"derived", DerivedJson(metrics)},
	};
}

Json MeasurementWindowJson(
	const OptimizationMeasurementWindowSnapshot &window)
{
	Json result{
		{"status", ToString(window.status)},
		{"restart_count", window.restartCount},
		{"report_count", window.reportCount},
		{"metrics_available", window.metricsAvailable},
	};
	if(window.metricsAvailable)
	{
		const Json scope = MetricsScopeJson(window.metrics);
		result["counters"] = scope["counters"];
		result["timings_ns"] = scope["timings_ns"];
		result["derived"] = scope["derived"];
	}
	else
	{
		result["counters"] = nullptr;
		result["timings_ns"] = nullptr;
		result["derived"] = nullptr;
	}
	return result;
}

Json AuditJson(
	const OptimizationMetricsSnapshot &metrics,
	const std::optional<AuditFailure> &failure)
{
	const AuditAccounting accounting = GetAuditAccounting(metrics);
	Json result{
		{"samples", metrics.auditSamples},
		{"passed", metrics.auditPassed},
		{"failed", metrics.auditFailed},
		{"pending", accounting.pending},
		{"accounting_consistent", accounting.consistent},
		{"failure_latched", failure.has_value()},
	};
	if(failure)
	{
		result["first_failure"] = Json{
			{"check_kind", ToString(failure->kind)},
			{"root_identity", failure->rootIdentity},
			{"expected", failure->expected},
			{"actual", failure->actual},
			{"reason", failure->reason},
		};
	}
	return result;
}

} // namespace

std::string RelayOptimizationReport::BuildJson(
	const OptimizationConfig &config,
	const OptimizationMetricsSnapshot &metrics,
	const std::optional<AuditFailure> &failure,
	const OptimizationMeasurementWindowSnapshot &measurementWindow) const
{
	const Json lifetime = MetricsScopeJson(metrics);
	const Json root{
		{"report_schema_version", 2},
		{"config", Json{
			{"mode", ToString(config.mode)},
			{"ablation", ToString(config.ablation)},
			{"max_relay_reserve", config.maxRelayReserve},
			{"audit_sample_rate", Json{
				{"numerator", config.auditSampleRate.numerator},
				{"denominator", config.auditSampleRate.denominator},
			}},
		}},
		// Keep the schema-v1 root aliases so existing activation gates and
		// report readers continue to consume process-lifetime metrics.
		{"counters", lifetime["counters"]},
		{"timings_ns", lifetime["timings_ns"]},
		{"derived", lifetime["derived"]},
		{"lifetime", lifetime},
		{"measurement_window",
		 MeasurementWindowJson(measurementWindow)},
		{"audit", AuditJson(metrics, failure)},
	};
	return root.dump(2);
}

bool RelayOptimizationReport::WriteAtomically(
	const std::string &path,
	const OptimizationConfig &config,
	const OptimizationMetricsSnapshot &metrics,
	const std::optional<AuditFailure> &failure,
	std::string *error,
	const OptimizationMeasurementWindowSnapshot &measurementWindow) const
{
	if(path.empty())
		return true;

	try
	{
		const std::filesystem::path destination(path);
		if(destination.has_parent_path())
		{
			std::filesystem::create_directories(
				destination.parent_path());
		}
		std::ostringstream suffix;
		suffix << ".tmp." <<
			std::chrono::steady_clock::now()
				.time_since_epoch().count() <<
			"." << std::hash<std::thread::id>{}(
				std::this_thread::get_id());
		const std::filesystem::path temporary =
			destination.string() + suffix.str();

		{
			std::ofstream stream(
				temporary,
				std::ios::out | std::ios::binary | std::ios::trunc);
			if(!stream)
				throw std::runtime_error("cannot open temporary report");
			const std::string contents =
				BuildJson(
					config,
					metrics,
					failure,
					measurementWindow);
			stream.write(
				contents.data(),
				static_cast<std::streamsize>(contents.size()));
			stream.flush();
			if(!stream)
				throw std::runtime_error("cannot write temporary report");
		}

		std::error_code renameError;
		std::filesystem::rename(
			temporary,
			destination,
			renameError);
		if(renameError)
		{
			std::error_code removeError;
			std::filesystem::remove(destination, removeError);
			renameError.clear();
			std::filesystem::rename(
				temporary,
				destination,
				renameError);
		}
		if(renameError)
		{
			std::error_code ignored;
			std::filesystem::remove(temporary, ignored);
			throw std::runtime_error(renameError.message());
		}
		return true;
	}
	catch(const std::exception &exception)
	{
		if(error)
			*error = exception.what();
		return false;
	}
}

} // namespace relay_optimization
} // namespace oxd
