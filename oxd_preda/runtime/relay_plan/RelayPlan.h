#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace oxd {
namespace relay_plan {

enum class CountPlanKind : uint8_t
{
	ExactConstant,
	ProvedConstantUpperBound,
	SymbolicUnsupportedAtRuntime,
	Unknown,
};

enum class FanoutKind : uint8_t
{
	SingleTarget,
	AllShards,
	Unknown,
};

struct PropertyEvidence
{
	std::string propertyId;
	std::string obligationStatus;
	std::string proofRole;
	std::string backend;
	std::string solverStatus;
	std::vector<std::string> assumptionConstraintIds;
};

struct CountPlan
{
	CountPlanKind kind = CountPlanKind::Unknown;
	uint64_t value = 0;
	std::string sourcePropertyId;
	PropertyEvidence evidence;
	std::string ineligibleReason;

	bool IsUsableForReserve() const noexcept
	{
		return kind == CountPlanKind::ExactConstant ||
			kind == CountPlanKind::ProvedConstantUpperBound;
	}
};

// This is intentionally a superset of the former trace-only
// ManifestFunctionSummary.  The optional raw summary values remain available
// to trace validation, while CountPlan records whether a value has sufficient
// compiler/solver evidence to affect runtime allocation.
struct FunctionRelayPlan
{
	std::string sourceFunctionId;
	uint32_t exportedOpcode = 0;
	bool hasExportedOpcode = false;

	std::optional<uint64_t> exactDirectRelayCount;
	std::optional<uint64_t> directRelayCountUpperBound;
	std::optional<uint32_t> maximumDepth;
	std::string relayCountExpressionKind;
	std::string relayCountUpperBoundExpressionKind;
	bool hasOpaque = false;
	bool hasUnmodeledRelayReachableCall = false;
	std::vector<std::string> relaySiteIds;
	std::vector<std::string> fanoutKinds;
	std::vector<FanoutKind> fanout;

	// Conservative capabilities aggregated from relay_sites[].relay_kind.
	// Custom-scope routing can become either intra- or cross-shard only after
	// the real target key is evaluated, so mayIntra is deliberately not
	// inferred from CustomScope.
	bool mayGlobal = false;
	bool mayBroadcast = false;
	bool mayCustom = false;
	bool mayDeferred = false;
	bool mayIntra = false;
	bool hasUnknownRelayKind = false;

	CountPlan directCount;
	CountPlan directCountUpperBound;

	bool bindingTrusted = false;
	bool functionOpcodeTrusted = false;
	bool reserveEligible = false;
	bool fanoutEligible = false;
	bool optimizationEligible = false;
	std::string ineligibleReason;
};

} // namespace relay_plan
} // namespace oxd
