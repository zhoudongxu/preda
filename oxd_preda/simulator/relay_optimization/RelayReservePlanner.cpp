#include "RelayReservePlanner.h"

#include <limits>

namespace oxd {
namespace relay_optimization {

bool CheckedRequiredCapacity(
	size_t currentSize,
	uint64_t addition,
	uint64_t maximumReserve,
	size_t &out) noexcept
{
	if(addition > maximumReserve)
		return false;
	if(addition >
		static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
	{
		return false;
	}
	const size_t converted = static_cast<size_t>(addition);
	if(converted > std::numeric_limits<size_t>::max() - currentSize)
		return false;
	out = currentSize + converted;
	return true;
}

ReserveDecision SelectDirectRelayReserve(
	const relay_plan::FunctionRelayPlan *plan,
	size_t currentSize,
	uint64_t maximumReserve) noexcept
{
	ReserveDecision out;
	if(!plan)
		return out;
	if(!plan->bindingTrusted ||
		!plan->functionOpcodeTrusted ||
		!plan->optimizationEligible)
	{
		out.kind = ReserveDecisionKind::SkippedIneligible;
		return out;
	}

	const relay_plan::CountPlan *selected = nullptr;
	if(plan->directCount.kind ==
		relay_plan::CountPlanKind::ExactConstant)
	{
		selected = &plan->directCount;
		out.kind = ReserveDecisionKind::ExactConstant;
	}
	else if(plan->directCountUpperBound.kind ==
		relay_plan::CountPlanKind::ProvedConstantUpperBound)
	{
		selected = &plan->directCountUpperBound;
		out.kind = ReserveDecisionKind::ProvedConstantUpperBound;
	}
	else
	{
		out.kind = ReserveDecisionKind::SkippedUnknown;
		return out;
	}

	out.addition = selected->value;
	out.sourceKind = selected->kind;
	if(out.addition == 0)
	{
		out.kind = ReserveDecisionKind::SkippedZero;
		return out;
	}
	if(out.addition > maximumReserve)
	{
		out.kind = ReserveDecisionKind::SkippedLimit;
		return out;
	}
	if(!CheckedRequiredCapacity(
		currentSize,
		out.addition,
		maximumReserve,
		out.requiredCapacity))
	{
		out.kind = ReserveDecisionKind::SkippedOverflow;
	}
	return out;
}

bool CheckedAggregateBroadcastReserve(
	uint64_t logicalRelayCount,
	uint32_t activeShardCount,
	uint64_t maximumReserve,
	uint64_t &outPhysicalElements) noexcept
{
	if(activeShardCount != 0 &&
		logicalRelayCount >
		std::numeric_limits<uint64_t>::max() / activeShardCount)
	{
		return false;
	}
	outPhysicalElements =
		logicalRelayCount * static_cast<uint64_t>(activeShardCount);
	return outPhysicalElements <= maximumReserve;
}

} // namespace relay_optimization
} // namespace oxd
