#pragma once

#include "../../runtime/relay_plan/RelayPlanMetrics.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace oxd {
namespace relay_optimization {

struct OptimizationMetricsSnapshot
{
	relay_plan::RelayPlanMetricsSnapshot plan;

	uint64_t optimizationEligibleInvocations = 0;
	uint64_t optimizationFallbackInvocations = 0;

	uint64_t relayBufferReserveCalls = 0;
	uint64_t relayBufferReservedElements = 0;
	uint64_t relayBufferReserveSkippedUnknown = 0;
	uint64_t relayBufferReserveSkippedLimit = 0;
	uint64_t relayBufferReserveFailures = 0;
	uint64_t relayBufferCapacityMisses = 0;
	uint64_t relayBufferCapacityGrowthEvents = 0;
	uint64_t broadcastCloneReserveCalls = 0;

	uint64_t queueSinglePushCalls = 0;
	uint64_t queueLegacyBulkPushCalls = 0;
	uint64_t queueBatchPushCalls = 0;
	uint64_t queueBatchElements = 0;
	uint64_t queueLockAcquisitions = 0;
	uint64_t queueNotifications = 0;
	uint64_t maximumBatchSize = 0;
	uint64_t queueBatchFallbacks = 0;
	uint64_t schedulerSelectionCalls = 0;
	uint64_t schedulerPrioritySelections = 0;
	uint64_t schedulerReorders = 0;
	uint64_t schedulerCertificateUses = 0;
	uint64_t schedulerFallbacks = 0;

	uint64_t logicalRelayEmissions = 0;
	uint64_t physicalRelayRoutes = 0;
	uint64_t relayExecutions = 0;
	uint64_t broadcastPhysicalClones = 0;

	uint64_t planLookupTimeNs = 0;
	uint64_t reserveTimeNs = 0;
	uint64_t relayGenerationTimeNs = 0;
	uint64_t routingTimeNs = 0;
	uint64_t dispatchTimeNs = 0;
	uint64_t queuePushTimeNs = 0;
	uint64_t schedulerDecisionTimeNs = 0;

	uint64_t auditSamples = 0;
	uint64_t auditPassed = 0;
	uint64_t auditFailed = 0;
};

enum class MeasurementWindowStatus : uint8_t
{
	NotStarted = 0,
	Active,
	Completed,
	ReportWithoutRestart,
};

inline const char* ToString(MeasurementWindowStatus status) noexcept
{
	switch(status)
	{
	case MeasurementWindowStatus::NotStarted:
		return "not_started";
	case MeasurementWindowStatus::Active:
		return "active";
	case MeasurementWindowStatus::Completed:
		return "completed";
	case MeasurementWindowStatus::ReportWithoutRestart:
		return "report_without_restart";
	}
	return "not_started";
}

struct OptimizationMeasurementWindowSnapshot
{
	MeasurementWindowStatus status =
		MeasurementWindowStatus::NotStarted;
	uint64_t restartCount = 0;
	uint64_t reportCount = 0;
	bool metricsAvailable = false;
	OptimizationMetricsSnapshot metrics;
};

struct OptimizationMetrics
{
	relay_plan::RelayPlanMetrics plan;

	std::atomic<uint64_t> optimizationEligibleInvocations{0};
	std::atomic<uint64_t> optimizationFallbackInvocations{0};

	std::atomic<uint64_t> relayBufferReserveCalls{0};
	std::atomic<uint64_t> relayBufferReservedElements{0};
	std::atomic<uint64_t> relayBufferReserveSkippedUnknown{0};
	std::atomic<uint64_t> relayBufferReserveSkippedLimit{0};
	std::atomic<uint64_t> relayBufferReserveFailures{0};
	std::atomic<uint64_t> relayBufferCapacityMisses{0};
	std::atomic<uint64_t> relayBufferCapacityGrowthEvents{0};
	std::atomic<uint64_t> broadcastCloneReserveCalls{0};

	std::atomic<uint64_t> queueSinglePushCalls{0};
	std::atomic<uint64_t> queueLegacyBulkPushCalls{0};
	std::atomic<uint64_t> queueBatchPushCalls{0};
	std::atomic<uint64_t> queueBatchElements{0};
	std::atomic<uint64_t> queueLockAcquisitions{0};
	std::atomic<uint64_t> queueNotifications{0};
	std::atomic<uint64_t> maximumBatchSize{0};
	std::atomic<uint64_t> queueBatchFallbacks{0};

	// Scheduler counters are reported separately from queue counters so the
	// experiment can measure policy overhead and conservative fallback use.
	std::atomic<uint64_t> schedulerSelectionCalls{0};
	std::atomic<uint64_t> schedulerPrioritySelections{0};
	std::atomic<uint64_t> schedulerReorders{0};
	std::atomic<uint64_t> schedulerCertificateUses{0};
	std::atomic<uint64_t> schedulerFallbacks{0};

	std::atomic<uint64_t> logicalRelayEmissions{0};
	std::atomic<uint64_t> physicalRelayRoutes{0};
	std::atomic<uint64_t> relayExecutions{0};
	std::atomic<uint64_t> broadcastPhysicalClones{0};

	std::atomic<uint64_t> planLookupTimeNs{0};
	std::atomic<uint64_t> reserveTimeNs{0};
	std::atomic<uint64_t> relayGenerationTimeNs{0};
	std::atomic<uint64_t> routingTimeNs{0};
	std::atomic<uint64_t> dispatchTimeNs{0};
	std::atomic<uint64_t> queuePushTimeNs{0};
	std::atomic<uint64_t> schedulerDecisionTimeNs{0};

	std::atomic<uint64_t> auditSamples{0};
	std::atomic<uint64_t> auditPassed{0};
	std::atomic<uint64_t> auditFailed{0};

	void ObserveBatchSize(uint64_t value) noexcept
	{
		ObserveMaximum(maximumBatchSize, value);
		if(m_measurementWindowActive.load(std::memory_order_acquire))
			ObserveMaximum(m_measurementWindowMaximumBatchSize, value);
	}

	OptimizationMetricsSnapshot Snapshot() const noexcept
	{
		OptimizationMetricsSnapshot out;
		out.plan = plan.Snapshot();

#define RPREDA_SNAPSHOT_COUNTER(name) \
		out.name = name.load(std::memory_order_relaxed)
		RPREDA_SNAPSHOT_COUNTER(optimizationEligibleInvocations);
		RPREDA_SNAPSHOT_COUNTER(optimizationFallbackInvocations);
		RPREDA_SNAPSHOT_COUNTER(relayBufferReserveCalls);
		RPREDA_SNAPSHOT_COUNTER(relayBufferReservedElements);
		RPREDA_SNAPSHOT_COUNTER(relayBufferReserveSkippedUnknown);
		RPREDA_SNAPSHOT_COUNTER(relayBufferReserveSkippedLimit);
		RPREDA_SNAPSHOT_COUNTER(relayBufferReserveFailures);
		RPREDA_SNAPSHOT_COUNTER(relayBufferCapacityMisses);
		RPREDA_SNAPSHOT_COUNTER(relayBufferCapacityGrowthEvents);
		RPREDA_SNAPSHOT_COUNTER(broadcastCloneReserveCalls);
		RPREDA_SNAPSHOT_COUNTER(queueSinglePushCalls);
		RPREDA_SNAPSHOT_COUNTER(queueLegacyBulkPushCalls);
		RPREDA_SNAPSHOT_COUNTER(queueBatchPushCalls);
		RPREDA_SNAPSHOT_COUNTER(queueBatchElements);
		RPREDA_SNAPSHOT_COUNTER(queueLockAcquisitions);
		RPREDA_SNAPSHOT_COUNTER(queueNotifications);
		RPREDA_SNAPSHOT_COUNTER(maximumBatchSize);
		RPREDA_SNAPSHOT_COUNTER(queueBatchFallbacks);
		RPREDA_SNAPSHOT_COUNTER(schedulerSelectionCalls);
		RPREDA_SNAPSHOT_COUNTER(schedulerPrioritySelections);
		RPREDA_SNAPSHOT_COUNTER(schedulerReorders);
		RPREDA_SNAPSHOT_COUNTER(schedulerCertificateUses);
		RPREDA_SNAPSHOT_COUNTER(schedulerFallbacks);
		RPREDA_SNAPSHOT_COUNTER(logicalRelayEmissions);
		RPREDA_SNAPSHOT_COUNTER(physicalRelayRoutes);
		RPREDA_SNAPSHOT_COUNTER(relayExecutions);
		RPREDA_SNAPSHOT_COUNTER(broadcastPhysicalClones);
		RPREDA_SNAPSHOT_COUNTER(planLookupTimeNs);
		RPREDA_SNAPSHOT_COUNTER(reserveTimeNs);
		RPREDA_SNAPSHOT_COUNTER(relayGenerationTimeNs);
		RPREDA_SNAPSHOT_COUNTER(routingTimeNs);
		RPREDA_SNAPSHOT_COUNTER(dispatchTimeNs);
		RPREDA_SNAPSHOT_COUNTER(queuePushTimeNs);
		RPREDA_SNAPSHOT_COUNTER(schedulerDecisionTimeNs);
		RPREDA_SNAPSHOT_COUNTER(auditSamples);
		RPREDA_SNAPSHOT_COUNTER(auditPassed);
		RPREDA_SNAPSHOT_COUNTER(auditFailed);
#undef RPREDA_SNAPSHOT_COUNTER
		return out;
	}

	void RestartMeasurementWindow()
	{
		const OptimizationMetricsSnapshot start = Snapshot();
		std::lock_guard<std::mutex> guard(m_measurementWindowMutex);
		m_measurementWindowActive.store(false, std::memory_order_release);
		m_measurementWindowStart = start;
		m_measurementWindowEnd = OptimizationMetricsSnapshot{};
		m_measurementWindowMaximumBatchSize.store(
			0,
			std::memory_order_relaxed);
		++m_measurementWindowRestartCount;
		m_measurementWindowStatus = MeasurementWindowStatus::Active;
		m_measurementWindowActive.store(true, std::memory_order_release);
	}

	void ReportMeasurementWindow()
	{
		const OptimizationMetricsSnapshot end = Snapshot();
		const uint64_t maximumBatch =
			m_measurementWindowMaximumBatchSize.load(
				std::memory_order_relaxed);
		std::lock_guard<std::mutex> guard(m_measurementWindowMutex);
		++m_measurementWindowReportCount;
		if(m_measurementWindowStatus !=
			MeasurementWindowStatus::Active)
		{
			if(m_measurementWindowStatus ==
				MeasurementWindowStatus::NotStarted)
			{
				m_measurementWindowStatus =
					MeasurementWindowStatus::ReportWithoutRestart;
			}
			return;
		}
		m_measurementWindowActive.store(false, std::memory_order_release);
		m_measurementWindowEnd = end;
		m_measurementWindowEnd.maximumBatchSize = maximumBatch;
		m_measurementWindowStatus = MeasurementWindowStatus::Completed;
	}

	OptimizationMeasurementWindowSnapshot
	SnapshotMeasurementWindow() const
	{
		std::lock_guard<std::mutex> guard(m_measurementWindowMutex);
		OptimizationMeasurementWindowSnapshot out;
		out.status = m_measurementWindowStatus;
		out.restartCount = m_measurementWindowRestartCount;
		out.reportCount = m_measurementWindowReportCount;
		if(m_measurementWindowStatus ==
			MeasurementWindowStatus::Completed)
		{
			out.metrics = Difference(
				m_measurementWindowEnd,
				m_measurementWindowStart);
			// maximumBatchSize is a gauge, not an additive counter.  The
			// endpoint stores the maximum observed while the window was
			// active, so it must not be subtracted from the lifetime gauge.
			out.metrics.maximumBatchSize =
				m_measurementWindowEnd.maximumBatchSize;
			out.metricsAvailable = true;
		}
		return out;
	}

private:
	static void ObserveMaximum(
		std::atomic<uint64_t> &maximumCounter,
		uint64_t value) noexcept
	{
		uint64_t maximum =
			maximumCounter.load(std::memory_order_relaxed);
		while(maximum < value &&
			!maximumCounter.compare_exchange_weak(
				maximum,
				value,
				std::memory_order_relaxed,
				std::memory_order_relaxed))
		{
		}
	}

	static uint64_t DifferenceCounter(
		uint64_t end,
		uint64_t start) noexcept
	{
		return end >= start ? end - start : 0;
	}

	static OptimizationMetricsSnapshot Difference(
		const OptimizationMetricsSnapshot &end,
		const OptimizationMetricsSnapshot &start) noexcept
	{
		OptimizationMetricsSnapshot out;

#define RPREDA_DIFFERENCE_PLAN_COUNTER(name) \
		out.plan.name = DifferenceCounter(end.plan.name, start.plan.name)
		RPREDA_DIFFERENCE_PLAN_COUNTER(planLoads);
		RPREDA_DIFFERENCE_PLAN_COUNTER(planLoadFailures);
		RPREDA_DIFFERENCE_PLAN_COUNTER(planBindingFailures);
		RPREDA_DIFFERENCE_PLAN_COUNTER(planCacheHits);
		RPREDA_DIFFERENCE_PLAN_COUNTER(planCacheMisses);
		RPREDA_DIFFERENCE_PLAN_COUNTER(planInvalidations);
		RPREDA_DIFFERENCE_PLAN_COUNTER(planReloads);
		RPREDA_DIFFERENCE_PLAN_COUNTER(optimizationEligibleLookups);
		RPREDA_DIFFERENCE_PLAN_COUNTER(optimizationFallbackLookups);
#undef RPREDA_DIFFERENCE_PLAN_COUNTER

#define RPREDA_DIFFERENCE_COUNTER(name) \
		out.name = DifferenceCounter(end.name, start.name)
		RPREDA_DIFFERENCE_COUNTER(optimizationEligibleInvocations);
		RPREDA_DIFFERENCE_COUNTER(optimizationFallbackInvocations);
		RPREDA_DIFFERENCE_COUNTER(relayBufferReserveCalls);
		RPREDA_DIFFERENCE_COUNTER(relayBufferReservedElements);
		RPREDA_DIFFERENCE_COUNTER(relayBufferReserveSkippedUnknown);
		RPREDA_DIFFERENCE_COUNTER(relayBufferReserveSkippedLimit);
		RPREDA_DIFFERENCE_COUNTER(relayBufferReserveFailures);
		RPREDA_DIFFERENCE_COUNTER(relayBufferCapacityMisses);
		RPREDA_DIFFERENCE_COUNTER(relayBufferCapacityGrowthEvents);
		RPREDA_DIFFERENCE_COUNTER(broadcastCloneReserveCalls);
		RPREDA_DIFFERENCE_COUNTER(queueSinglePushCalls);
		RPREDA_DIFFERENCE_COUNTER(queueLegacyBulkPushCalls);
		RPREDA_DIFFERENCE_COUNTER(queueBatchPushCalls);
		RPREDA_DIFFERENCE_COUNTER(queueBatchElements);
		RPREDA_DIFFERENCE_COUNTER(queueLockAcquisitions);
		RPREDA_DIFFERENCE_COUNTER(queueNotifications);
		RPREDA_DIFFERENCE_COUNTER(queueBatchFallbacks);
		RPREDA_DIFFERENCE_COUNTER(schedulerSelectionCalls);
		RPREDA_DIFFERENCE_COUNTER(schedulerPrioritySelections);
		RPREDA_DIFFERENCE_COUNTER(schedulerReorders);
		RPREDA_DIFFERENCE_COUNTER(schedulerCertificateUses);
		RPREDA_DIFFERENCE_COUNTER(schedulerFallbacks);
		RPREDA_DIFFERENCE_COUNTER(logicalRelayEmissions);
		RPREDA_DIFFERENCE_COUNTER(physicalRelayRoutes);
		RPREDA_DIFFERENCE_COUNTER(relayExecutions);
		RPREDA_DIFFERENCE_COUNTER(broadcastPhysicalClones);
		RPREDA_DIFFERENCE_COUNTER(planLookupTimeNs);
		RPREDA_DIFFERENCE_COUNTER(reserveTimeNs);
		RPREDA_DIFFERENCE_COUNTER(relayGenerationTimeNs);
		RPREDA_DIFFERENCE_COUNTER(routingTimeNs);
		RPREDA_DIFFERENCE_COUNTER(dispatchTimeNs);
		RPREDA_DIFFERENCE_COUNTER(queuePushTimeNs);
		RPREDA_DIFFERENCE_COUNTER(schedulerDecisionTimeNs);
		RPREDA_DIFFERENCE_COUNTER(auditSamples);
		RPREDA_DIFFERENCE_COUNTER(auditPassed);
		RPREDA_DIFFERENCE_COUNTER(auditFailed);
#undef RPREDA_DIFFERENCE_COUNTER
		return out;
	}

	mutable std::mutex m_measurementWindowMutex;
	MeasurementWindowStatus m_measurementWindowStatus =
		MeasurementWindowStatus::NotStarted;
	OptimizationMetricsSnapshot m_measurementWindowStart;
	OptimizationMetricsSnapshot m_measurementWindowEnd;
	uint64_t m_measurementWindowRestartCount = 0;
	uint64_t m_measurementWindowReportCount = 0;
	std::atomic<bool> m_measurementWindowActive{false};
	std::atomic<uint64_t> m_measurementWindowMaximumBatchSize{0};
};

} // namespace relay_optimization
} // namespace oxd
