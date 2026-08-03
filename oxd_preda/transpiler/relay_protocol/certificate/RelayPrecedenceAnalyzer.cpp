#include "RelayPrecedenceAnalyzer.h"

#include <algorithm>

namespace transpiler {
namespace relay_protocol {
namespace certificate {
namespace {

const cfg::PredaFunctionGraphAnalysis *FindAnalysis(
	const RelayProtocolIR &protocol,
	const RelayPairCandidate &candidate)
{
	const bool localRoot = candidate.sameSourceFunction &&
		candidate.ownerFunctionA == candidate.rootFunctionId;
	const std::vector<cfg::PredaFunctionGraphAnalysis> &analyses =
		localRoot
			? protocol.controlFlow.relayIcfg.functionAnalyses
			: protocol.controlFlow.relayIcfg.synchronousCompositionAnalyses;
	for (const cfg::PredaFunctionGraphAnalysis &analysis : analyses)
		if (analysis.functionId == candidate.rootFunctionId)
			return &analysis;
	return nullptr;
}

bool Contains(
	const std::map<cfg::NodeId, std::vector<cfg::NodeId>> &relation,
	const cfg::NodeId &node,
	const cfg::NodeId &candidate)
{
	const auto item = relation.find(node);
	return item != relation.end() &&
		std::find(item->second.begin(), item->second.end(), candidate) !=
			item->second.end();
}

bool HasCompletePrecedenceFacts(
	const cfg::PredaFunctionGraphAnalysis &analysis)
{
	if (analysis.status == cfg::AnalysisStatus::Complete)
		return true;
	if (analysis.status != cfg::AnalysisStatus::Conservative ||
		analysis.completenessReasons.empty())
	{
		return false;
	}
	for (const std::string &reason : analysis.completenessReasons)
	{
		if (reason != "relay emission may fail through the PREDA runtime" &&
			reason !=
				"entry-reachable non-terminating path prevents complete post-dominance")
			return false;
	}
	return true;
}

} // namespace

RelayPrecedenceResult RelayPrecedenceAnalyzer::Analyze(
	const RelayProtocolIR &protocol,
	const RelayPairCandidate &candidate) const
{
	RelayPrecedenceResult result;
	if (!candidate.exactContext)
	{
		result.reason = "must-precede requires one exact complete ICFG context";
		return result;
	}
	if (!candidate.loopFreeContext)
	{
		result.reason =
			"must-precede is unknown without occurrence-indexed loop semantics";
		return result;
	}
	if (candidate.structuralRelation !=
		cfg::RelaySiteRelationStatus::CoReachable)
	{
		result.reason = "relay sites are not structurally co-reachable";
		return result;
	}
	const cfg::PredaFunctionGraphAnalysis *analysis =
		FindAnalysis(protocol, candidate);
	if (analysis == nullptr ||
		!HasCompletePrecedenceFacts(*analysis) ||
		!analysis->dominanceComplete ||
		!analysis->reachabilityComplete)
	{
		result.reason = "complete dominance facts are unavailable";
		return result;
	}

	const bool aDominatesB = Contains(
		analysis->dominators, candidate.nodeB, candidate.nodeA);
	const bool bDominatesA = Contains(
		analysis->dominators, candidate.nodeA, candidate.nodeB);
	if (aDominatesB == bDominatesA)
	{
		result.reason = aDominatesB
			? "ambiguous cyclic dominance relation"
			: "neither relay site dominates the other";
		return result;
	}
	result.proved = true;
	result.relation = aDominatesB
		? RelayPairRelation::MustPrecedeAB
		: RelayPairRelation::MustPrecedeBA;
	result.supportingCfgFactIds = candidate.supportingCfgFactIds;
	std::sort(
		result.supportingCfgFactIds.begin(),
		result.supportingCfgFactIds.end());
	result.supportingCfgFactIds.erase(
		std::unique(
			result.supportingCfgFactIds.begin(),
			result.supportingCfgFactIds.end()),
		result.supportingCfgFactIds.end());
	result.reason = aDominatesB
		? "site A strictly dominates site B in the exact loop-free ICFG"
		: "site B strictly dominates site A in the exact loop-free ICFG";
	return result;
}

} // namespace certificate
} // namespace relay_protocol
} // namespace transpiler
