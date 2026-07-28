#pragma once

#include "RelayCardinalityExpr.h"
#include "RelayDepthExpr.h"
#include "../../transpiler/PredaCommon.h"

#include <cstdint>
#include <string>
#include <vector>

namespace transpiler {
namespace relay_protocol {
namespace analysis {

enum class RelayFanoutKind : uint8_t
{
	SingleTarget,
	AllShards,
};

enum class RelayOrderingStatus : uint8_t
{
	Trivial,
	Partial,
	Unknown,
};

struct RelayOrderingConstraint
{
	std::string beforeRelaySiteId;
	std::string afterRelaySiteId;
	std::string reason;
};

struct RelayOrderingSummary
{
	RelayOrderingStatus status = RelayOrderingStatus::Unknown;
	std::vector<RelayOrderingConstraint> mustPrecede;
	std::string reason;
};

enum class RelayAnalysisStatus : uint8_t
{
	Exact,
	Conservative,
	Unknown,
};

struct RelayProtocolSummary
{
	RelayCardinalityExpr relayCount;
	RelayCardinalityExpr relayCountUpperBound;
	RelayDepthExpr maxDepth;
	std::vector<std::string> relaySiteIds;
	std::vector<ScopeType> targetScopeKinds;
	std::vector<RelayFanoutKind> fanoutKinds;
	bool targetsKnownBeforeExecution = true;
	bool hasOpaque = false;
	bool hasUnmodeledRelayReachableCall = false;
	RelayOrderingSummary ordering;
	RelayAnalysisStatus analysisStatus = RelayAnalysisStatus::Unknown;
};

} // namespace analysis
} // namespace relay_protocol
} // namespace transpiler
