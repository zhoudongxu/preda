#pragma once

#include "../../runtime/relay_plan/RelayPlan.h"

#include <cstddef>
#include <cstdint>

namespace oxd {
namespace relay_optimization {

enum class ReserveDecisionKind : uint8_t
{
	ExactConstant,
	ProvedConstantUpperBound,
	SkippedNoPlan,
	SkippedIneligible,
	SkippedUnknown,
	SkippedZero,
	SkippedLimit,
	SkippedOverflow,
};

struct ReserveDecision
{
	ReserveDecisionKind kind = ReserveDecisionKind::SkippedNoPlan;
	relay_plan::CountPlanKind sourceKind =
		relay_plan::CountPlanKind::Unknown;
	uint64_t addition = 0;
	size_t requiredCapacity = 0;

	bool ShouldReserve() const noexcept
	{
		return kind == ReserveDecisionKind::ExactConstant ||
			kind == ReserveDecisionKind::ProvedConstantUpperBound;
	}

	bool HasTrustedCount() const noexcept
	{
		return sourceKind ==
				relay_plan::CountPlanKind::ExactConstant ||
			sourceKind ==
				relay_plan::CountPlanKind::ProvedConstantUpperBound;
	}
};

ReserveDecision SelectDirectRelayReserve(
	const relay_plan::FunctionRelayPlan *plan,
	size_t currentSize,
	uint64_t maximumReserve) noexcept;

bool CheckedRequiredCapacity(
	size_t currentSize,
	uint64_t addition,
	uint64_t maximumReserve,
	size_t &out) noexcept;

bool CheckedAggregateBroadcastReserve(
	uint64_t logicalRelayCount,
	uint32_t activeShardCount,
	uint64_t maximumReserve,
	uint64_t &outPhysicalElements) noexcept;

} // namespace relay_optimization
} // namespace oxd
