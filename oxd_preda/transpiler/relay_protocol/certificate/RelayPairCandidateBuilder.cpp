#include "RelayPairCandidateBuilder.h"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>

namespace transpiler {
namespace relay_protocol {
namespace certificate {
namespace {

bool ContainsUnknownFormula(const refinement::FormulaExpr &formula)
{
	if (formula.kind == refinement::FormulaExprKind::Unknown ||
		formula.sort.kind == refinement::FormulaSortKind::Unknown)
	{
		return true;
	}
	for (const refinement::FormulaExpr &child : formula.children)
	{
		if (ContainsUnknownFormula(child))
			return true;
	}
	return false;
}

const RelaySite *FindSite(
	const RelayProtocolIR &protocol,
	const std::string &id)
{
	for (const RelaySite &site : protocol.relaySites)
		if (site.id == id)
			return &site;
	return nullptr;
}

struct SiteNode
{
	const cfg::PredaFunctionCFG *function = nullptr;
	const cfg::PredaCFGNode *node = nullptr;
};

std::map<std::string, SiteNode> BuildSiteNodeIndex(
	const RelayProtocolIR &protocol)
{
	std::map<std::string, SiteNode> result;
	for (const cfg::PredaFunctionCFG &function :
		protocol.controlFlow.functions)
	{
		for (const cfg::PredaCFGNode &node : function.nodes)
		{
			if (node.kind == cfg::PredaCFGNodeKind::RelayEmit &&
				!node.relaySiteId.empty())
			{
				result[node.relaySiteId] = SiteNode{&function, &node};
			}
		}
	}
	return result;
}

const cfg::RelaySiteRelation *FindRelation(
	const RelayProtocolIR &protocol,
	const std::string &first,
	const std::string &second)
{
	for (const cfg::RelaySiteRelation &relation :
		protocol.controlFlow.relayIcfg.relaySiteRelations)
	{
		if ((relation.firstRelaySiteId == first &&
			 relation.secondRelaySiteId == second) ||
			(relation.firstRelaySiteId == second &&
			 relation.secondRelaySiteId == first))
		{
			return &relation;
		}
	}
	return nullptr;
}

const cfg::PredaFunctionGraphAnalysis *FindAnalysis(
	const std::vector<cfg::PredaFunctionGraphAnalysis> &analyses,
	const std::string &functionId)
{
	for (const cfg::PredaFunctionGraphAnalysis &analysis : analyses)
		if (analysis.functionId == functionId)
			return &analysis;
	return nullptr;
}

const cfg::PredaRegionEffect *FindHandlerEffect(
	const RelayProtocolIR &protocol,
	const RelaySite &site)
{
	const std::string expected = "region::handler::" + site.handlerId;
	for (const cfg::PredaRegionEffect &effect :
		protocol.controlFlow.regionEffects)
	{
		if (effect.kind == cfg::RegionKind::RelayHandler &&
			effect.regionId == expected)
		{
			return &effect;
		}
	}
	return nullptr;
}

bool ClosureHasAnyLoop(
	const RelayProtocolIR &protocol,
	const std::set<std::string> &functionIds)
{
	for (const cfg::PredaFunctionCFG &function :
		protocol.controlFlow.functions)
	{
		if (functionIds.find(function.functionId) == functionIds.end())
			continue;
		for (const cfg::PredaCFGNode &node : function.nodes)
		{
			if (!node.enclosingLoopIds.empty() ||
				node.kind == cfg::PredaCFGNodeKind::LoopHeader ||
				node.kind == cfg::PredaCFGNodeKind::LoopLatch)
			{
				return true;
			}
		}
	}
	return false;
}

bool ClosureHasAmbiguousSyncCallContext(
	const RelayProtocolIR &protocol,
	const std::set<std::string> &functionIds)
{
	std::map<std::string, size_t> incomingOccurrences;
	for (const cfg::PredaInterproceduralEdge &edge :
		protocol.controlFlow.relayIcfg.interproceduralEdges)
	{
		if (edge.kind != cfg::InterproceduralEdgeKind::SyncCall ||
			functionIds.find(edge.sourceFunctionId) == functionIds.end())
		{
			continue;
		}
		if (!edge.resolved || ++incomingOccurrences[edge.targetFunctionId] > 1)
			return true;
	}
	return false;
}

std::string CandidateId(
	const std::string &root,
	const std::string &first,
	const std::string &second)
{
	return "parallel.pair." + root + "." + first + "." + second;
}

} // namespace

RelayPairCandidatesByFunction RelayPairCandidateBuilder::Build(
	const RelayProtocolIR &protocol) const
{
	RelayPairCandidatesByFunction result;
	const std::map<std::string, SiteNode> siteNodes =
		BuildSiteNodeIndex(protocol);

	for (const cfg::PredaFunctionCFG &rootFunction :
		protocol.controlFlow.functions)
	{
		std::set<std::string> closure;
		closure.insert(rootFunction.functionId);
		const auto reachable = protocol.controlFlow.relayIcfg
			.synchronousReachableFunctions.find(rootFunction.functionId);
		if (reachable != protocol.controlFlow.relayIcfg
			.synchronousReachableFunctions.end())
		{
			closure.insert(reachable->second.begin(), reachable->second.end());
		}

		std::vector<const RelaySite *> sites;
		const bool ambiguousSyncContext =
			ClosureHasAmbiguousSyncCallContext(protocol, closure);
		for (const RelaySite &site : protocol.relaySites)
		{
			if (closure.find(site.sourceFunctionId) != closure.end())
				sites.push_back(&site);
		}
		std::sort(
			sites.begin(),
			sites.end(),
			[](const RelaySite *left, const RelaySite *right)
			{
				return left->id < right->id;
			});

		std::vector<RelayPairCandidate> &functionPairs =
			result[rootFunction.functionId];
		for (size_t firstIndex = 0;
			firstIndex < sites.size();
			++firstIndex)
		{
			for (size_t secondIndex = firstIndex + 1;
				secondIndex < sites.size();
				++secondIndex)
			{
				const RelaySite &first = *sites[firstIndex];
				const RelaySite &second = *sites[secondIndex];
				RelayPairCandidate candidate;
				candidate.id = CandidateId(
					rootFunction.functionId, first.id, second.id);
				candidate.rootFunctionId = rootFunction.functionId;
				candidate.siteA = first.id;
				candidate.siteB = second.id;
				candidate.ownerFunctionA = first.sourceFunctionId;
				candidate.ownerFunctionB = second.sourceFunctionId;
				candidate.siteALocation = first.location;
				candidate.siteBLocation = second.location;
				candidate.targetScopeA = first.targetScope;
				candidate.targetScopeB = second.targetScope;
				candidate.relayKindA = first.relayKind;
				candidate.relayKindB = second.relayKind;
				candidate.sameSourceFunction =
					first.sourceFunctionId == second.sourceFunctionId;
				candidate.targetFormulasSupported =
					!ContainsUnknownFormula(first.refinementTargetFormula) &&
					!ContainsUnknownFormula(second.refinementTargetFormula) &&
					first.refinementTargetFormula.sort.IsKnown() &&
					first.refinementTargetFormula.sort ==
						second.refinementTargetFormula.sort;

				const auto firstNode = siteNodes.find(first.id);
				const auto secondNode = siteNodes.find(second.id);
				if (firstNode == siteNodes.end() || secondNode == siteNodes.end())
				{
					candidate.reason =
						"one or both RelayEmit CFG nodes are missing";
					functionPairs.push_back(std::move(candidate));
					continue;
				}
				candidate.nodeA = firstNode->second.node->id;
				candidate.nodeB = secondNode->second.node->id;
				candidate.supportingCfgFactIds = {
					rootFunction.functionId,
					candidate.nodeA,
					candidate.nodeB,
				};

				const cfg::RelaySiteRelation *relation =
					FindRelation(protocol, first.id, second.id);
				if (relation != nullptr)
				{
					candidate.structuralRelation = relation->status;
					candidate.reason = relation->reason;
				}
				else
				{
					candidate.reason =
						"A-D ICFG has no relation for this relay-site pair";
				}

				const cfg::PredaFunctionGraphAnalysis *analysis = nullptr;
				if (candidate.sameSourceFunction &&
					first.sourceFunctionId == rootFunction.functionId)
				{
					analysis = FindAnalysis(
						protocol.controlFlow.relayIcfg.functionAnalyses,
						rootFunction.functionId);
				}
				else
				{
					analysis = FindAnalysis(
						protocol.controlFlow.relayIcfg
							.synchronousCompositionAnalyses,
						rootFunction.functionId);
				}
				// Exact path feasibility needs complete dominance and
				// reachability, but not post-dominance. A composed analysis
				// may be Conservative solely because post-dominance is
				// unavailable; RelayPrecedenceAnalyzer independently keeps
				// the stronger Complete-status requirement for MustPrecede.
				candidate.exactContext = analysis != nullptr &&
					(analysis->status == cfg::AnalysisStatus::Complete ||
					 analysis->status == cfg::AnalysisStatus::Conservative) &&
					analysis->dominanceComplete &&
					analysis->reachabilityComplete &&
					!ambiguousSyncContext &&
					candidate.structuralRelation !=
						cfg::RelaySiteRelationStatus::Unknown;
				if (ambiguousSyncContext)
					candidate.reason =
						"synchronous callee has multiple call occurrences in the relevant context";
				candidate.loopFreeContext =
					first.loops.empty() && second.loops.empty() &&
					firstNode->second.node->enclosingLoopIds.empty() &&
					secondNode->second.node->enclosingLoopIds.empty() &&
					!ClosureHasAnyLoop(protocol, closure);

				const cfg::PredaRegionEffect *firstEffect =
					FindHandlerEffect(protocol, first);
				const cfg::PredaRegionEffect *secondEffect =
					FindHandlerEffect(protocol, second);
				if (firstEffect != nullptr && secondEffect != nullptr)
				{
					candidate.effectA = firstEffect->effect;
					candidate.effectB = secondEffect->effect;
					candidate.effectsComplete =
						candidate.effectA.status == cfg::AnalysisStatus::Complete &&
						candidate.effectB.status == cfg::AnalysisStatus::Complete;
					candidate.supportingCfgFactIds.push_back(firstEffect->regionId);
					candidate.supportingCfgFactIds.push_back(secondEffect->regionId);
				}
				functionPairs.push_back(std::move(candidate));
			}
		}
	}
	return result;
}

} // namespace certificate
} // namespace relay_protocol
} // namespace transpiler
