#pragma once

#include <atomic>
#include <cstdint>

namespace oxd {
namespace relay_plan {

struct RelayPlanMetricsSnapshot
{
	uint64_t planLoads = 0;
	uint64_t planLoadFailures = 0;
	uint64_t planBindingFailures = 0;
	uint64_t planCacheHits = 0;
	uint64_t planCacheMisses = 0;
	uint64_t planInvalidations = 0;
	uint64_t planReloads = 0;
	uint64_t optimizationEligibleLookups = 0;
	uint64_t optimizationFallbackLookups = 0;
};

struct RelayPlanMetrics
{
	std::atomic<uint64_t> planLoads{0};
	std::atomic<uint64_t> planLoadFailures{0};
	std::atomic<uint64_t> planBindingFailures{0};
	std::atomic<uint64_t> planCacheHits{0};
	std::atomic<uint64_t> planCacheMisses{0};
	std::atomic<uint64_t> planInvalidations{0};
	std::atomic<uint64_t> planReloads{0};
	std::atomic<uint64_t> optimizationEligibleLookups{0};
	std::atomic<uint64_t> optimizationFallbackLookups{0};

	RelayPlanMetricsSnapshot Snapshot() const noexcept
	{
		RelayPlanMetricsSnapshot result;
		result.planLoads = planLoads.load(std::memory_order_relaxed);
		result.planLoadFailures =
			planLoadFailures.load(std::memory_order_relaxed);
		result.planBindingFailures =
			planBindingFailures.load(std::memory_order_relaxed);
		result.planCacheHits =
			planCacheHits.load(std::memory_order_relaxed);
		result.planCacheMisses =
			planCacheMisses.load(std::memory_order_relaxed);
		result.planInvalidations =
			planInvalidations.load(std::memory_order_relaxed);
		result.planReloads =
			planReloads.load(std::memory_order_relaxed);
		result.optimizationEligibleLookups =
			optimizationEligibleLookups.load(std::memory_order_relaxed);
		result.optimizationFallbackLookups =
			optimizationFallbackLookups.load(std::memory_order_relaxed);
		return result;
	}
};

} // namespace relay_plan
} // namespace oxd
