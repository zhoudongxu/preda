#pragma once

#include "../RelayProtocolIR.h"

#include <map>
#include <string>
#include <vector>

namespace transpiler {
namespace relay_protocol {
namespace certificate {

struct RelayPairCandidate
{
	std::string id;
	std::string rootFunctionId;
	std::string siteA;
	std::string siteB;
	std::string ownerFunctionA;
	std::string ownerFunctionB;
	cfg::NodeId nodeA;
	cfg::NodeId nodeB;
	SourceLocation siteALocation;
	SourceLocation siteBLocation;
	ScopeType targetScopeA = ScopeType::None;
	ScopeType targetScopeB = ScopeType::None;
	RelayKind relayKindA = RelayKind::CustomScope;
	RelayKind relayKindB = RelayKind::CustomScope;
	cfg::RelaySiteRelationStatus structuralRelation =
		cfg::RelaySiteRelationStatus::Unknown;
	bool sameSourceFunction = false;
	bool exactContext = false;
	bool loopFreeContext = false;
	bool targetFormulasSupported = false;
	bool effectsComplete = false;
	cfg::EffectSummary effectA;
	cfg::EffectSummary effectB;
	std::vector<std::string> supportingCfgFactIds;
	std::string reason;
};

using RelayPairCandidatesByFunction =
	std::map<std::string, std::vector<RelayPairCandidate>>;

} // namespace certificate
} // namespace relay_protocol
} // namespace transpiler
