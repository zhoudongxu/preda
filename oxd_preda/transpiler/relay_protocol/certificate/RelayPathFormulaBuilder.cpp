#include "RelayPathFormulaBuilder.h"

#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace transpiler {
namespace relay_protocol {
namespace certificate {
namespace {

bool ContainsUnknown(const refinement::FormulaExpr &formula)
{
	if (formula.kind == refinement::FormulaExprKind::Unknown ||
		formula.sort.kind == refinement::FormulaSortKind::Unknown)
	{
		return true;
	}
	for (const refinement::FormulaExpr &child : formula.children)
		if (ContainsUnknown(child))
			return true;
	return false;
}

refinement::FormulaExpr And(
	refinement::FormulaExpr left,
	refinement::FormulaExpr right)
{
	return refinement::FormulaExpr::Nary(
		"&&",
		{std::move(left), std::move(right)},
		refinement::FormulaSort::Bool());
}

refinement::FormulaExpr Or(std::vector<refinement::FormulaExpr> terms)
{
	if (terms.empty())
		return refinement::FormulaExpr::BoolLiteral(false);
	if (terms.size() == 1)
		return std::move(terms.front());
	return refinement::FormulaExpr::Nary(
		"||", std::move(terms), refinement::FormulaSort::Bool());
}

refinement::FormulaExpr Not(refinement::FormulaExpr value)
{
	return refinement::FormulaExpr::Unary(
		"!", std::move(value), refinement::FormulaSort::Bool());
}

struct CombinedEdge
{
	std::string id;
	cfg::NodeId source;
	cfg::NodeId target;
	cfg::PredaCFGEdgeKind kind = cfg::PredaCFGEdgeKind::Fallthrough;
	bool supported = true;
	std::string reason;
};

struct CombinedGraph
{
	cfg::NodeId entry;
	std::map<cfg::NodeId, const cfg::PredaCFGNode *> nodes;
	std::map<cfg::NodeId, const cfg::PredaFunctionCFG *> owners;
	std::vector<CombinedEdge> edges;
	std::map<cfg::NodeId, std::vector<size_t>> outgoing;
	std::map<cfg::NodeId, std::vector<size_t>> incoming;
	std::vector<std::string> baseEvidence;
	bool supported = true;
	bool feasibilityExact = true;
	std::string reason;
};

const cfg::PredaFunctionCFG *FindFunction(
	const RelayProtocolIR &protocol,
	const std::string &functionId)
{
	for (const cfg::PredaFunctionCFG &function :
		protocol.controlFlow.functions)
		if (function.functionId == functionId)
			return &function;
	return nullptr;
}

bool ConservativeCfgHasOnlyExplicitRelayFailure(
	const cfg::PredaFunctionCFG &function)
{
	if (function.completenessReasons.empty())
		return false;
	for (const std::string &reason : function.completenessReasons)
	{
		if (reason != "relay emission may fail through the PREDA runtime")
			return false;
	}
	return true;
}

void AddEdge(CombinedGraph &graph, CombinedEdge edge)
{
	const size_t index = graph.edges.size();
	graph.outgoing[edge.source].push_back(index);
	graph.incoming[edge.target].push_back(index);
	graph.edges.push_back(std::move(edge));
}

std::set<std::string> SynchronousClosure(
	const RelayProtocolIR &protocol,
	const std::string &rootFunctionId)
{
	std::set<std::string> result;
	result.insert(rootFunctionId);
	const auto closure = protocol.controlFlow.relayIcfg
		.synchronousReachableFunctions.find(rootFunctionId);
	if (closure != protocol.controlFlow.relayIcfg
		.synchronousReachableFunctions.end())
	{
		result.insert(closure->second.begin(), closure->second.end());
	}
	return result;
}

CombinedGraph BuildCombinedGraph(
	const RelayProtocolIR &protocol,
	const RelayPairCandidate &candidate)
{
	CombinedGraph graph;
	const cfg::PredaFunctionCFG *root =
		FindFunction(protocol, candidate.rootFunctionId);
	if (root == nullptr)
	{
		graph.supported = false;
		graph.reason = "root function CFG is unavailable";
		return graph;
	}
	graph.entry = root->entryNodeId;
	const std::set<std::string> closure =
		SynchronousClosure(protocol, candidate.rootFunctionId);
	for (const std::string &functionId : closure)
	{
		const cfg::PredaFunctionCFG *function =
			FindFunction(protocol, functionId);
		if (function == nullptr ||
			(function->status != cfg::AnalysisStatus::Complete &&
			 function->status != cfg::AnalysisStatus::Conservative))
		{
			graph.supported = false;
			graph.reason =
				"synchronous closure contains an unsupported or unknown CFG";
			return graph;
		}
		if (function->status == cfg::AnalysisStatus::Conservative &&
			!ConservativeCfgHasOnlyExplicitRelayFailure(*function))
			graph.feasibilityExact = false;
		graph.baseEvidence.push_back(functionId);
		for (const cfg::PredaCFGNode &node : function->nodes)
		{
			if (!node.supported ||
				node.kind == cfg::PredaCFGNodeKind::Opaque)
			{
				graph.supported = false;
				graph.reason =
					"synchronous closure contains an unsupported CFG node";
				return graph;
			}
			if (!graph.nodes.emplace(node.id, &node).second)
			{
				graph.supported = false;
				graph.reason = "CFG node IDs are not unique in the synchronous closure";
				return graph;
			}
			graph.owners[node.id] = function;
		}
	}

	std::set<cfg::NodeId> composedCallNodes;
	for (const cfg::PredaInterproceduralEdge &edge :
		protocol.controlFlow.relayIcfg.interproceduralEdges)
	{
		if (edge.kind == cfg::InterproceduralEdgeKind::SyncCall &&
			closure.find(edge.sourceFunctionId) != closure.end())
		{
			if (!edge.resolved || graph.nodes.find(edge.sourceNodeId) == graph.nodes.end() ||
				graph.nodes.find(edge.targetNodeId) == graph.nodes.end())
			{
				graph.supported = false;
				graph.reason = "synchronous call composition is unresolved";
				return graph;
			}
			const cfg::PredaCFGNode *callNode = graph.nodes[edge.sourceNodeId];
			// A resolved callee's PREDA relay-failure paths are explicit in its
			// CFG, with both the normal return and terminal failure sink present.
			// Unknown/external calls have no compiler-owned path semantics.
			if (callNode->directEffect.mayCallUnknown ||
				callNode->directEffect.mayHaveExternalEffect)
			{
				graph.supported = false;
				graph.reason =
					"synchronous call has unknown or external path semantics";
				return graph;
			}
			composedCallNodes.insert(edge.sourceNodeId);
			AddEdge(graph, CombinedEdge{
				edge.id,
				edge.sourceNodeId,
				edge.targetNodeId,
				cfg::PredaCFGEdgeKind::CallToCallee,
				true,
				edge.reason,
			});
			graph.baseEvidence.push_back(edge.id);
		}
		else if (edge.kind == cfg::InterproceduralEdgeKind::SyncReturn &&
			closure.find(edge.sourceFunctionId) != closure.end() &&
			closure.find(edge.targetFunctionId) != closure.end())
		{
			if (!edge.resolved || graph.nodes.find(edge.sourceNodeId) == graph.nodes.end() ||
				graph.nodes.find(edge.targetNodeId) == graph.nodes.end())
			{
				graph.supported = false;
				graph.reason = "synchronous return composition is unresolved";
				return graph;
			}
			AddEdge(graph, CombinedEdge{
				edge.id,
				edge.sourceNodeId,
				edge.targetNodeId,
				cfg::PredaCFGEdgeKind::ReturnToCaller,
				true,
				edge.reason,
			});
			graph.baseEvidence.push_back(edge.id);
		}
	}

	for (const std::string &functionId : closure)
	{
		const cfg::PredaFunctionCFG *function =
			FindFunction(protocol, functionId);
		for (const cfg::PredaCFGEdge &edge : function->edges)
		{
			if (graph.nodes.find(edge.source) == graph.nodes.end() ||
				graph.nodes.find(edge.target) == graph.nodes.end())
				continue;
			// The per-function call-node fallthrough is replaced by the exact
			// call/return pair above. Keeping it would permit bypassing a callee.
			if (composedCallNodes.find(edge.source) != composedCallNodes.end())
				continue;
			AddEdge(graph, CombinedEdge{
				edge.id,
				edge.source,
				edge.target,
				edge.kind,
				edge.supported,
				edge.reason,
			});
		}
	}
	return graph;
}

bool EdgePredicate(
	const CombinedGraph &graph,
	const CombinedEdge &edge,
	refinement::FormulaExpr &predicate,
	std::string &reason)
{
	if (!edge.supported || edge.kind == cfg::PredaCFGEdgeKind::Unknown)
	{
		reason = edge.reason.empty()
			? "path contains an unsupported CFG edge"
			: edge.reason;
		return false;
	}
	if (edge.kind != cfg::PredaCFGEdgeKind::TrueBranch &&
		edge.kind != cfg::PredaCFGEdgeKind::FalseBranch)
	{
		predicate = refinement::FormulaExpr::BoolLiteral(true);
		return true;
	}
	const auto source = graph.nodes.find(edge.source);
	if (source == graph.nodes.end() || !source->second->hasCondition ||
		source->second->condition.sort != refinement::FormulaSort::Bool() ||
		ContainsUnknown(source->second->condition))
	{
		reason = "branch edge has no exact supported Boolean condition";
		return false;
	}
	predicate = source->second->condition;
	if (edge.kind == cfg::PredaCFGEdgeKind::FalseBranch)
		predicate = Not(std::move(predicate));
	return true;
}

std::set<cfg::NodeId> Traverse(
	const cfg::NodeId &start,
	const std::map<cfg::NodeId, std::vector<size_t>> &adjacency,
	const std::vector<CombinedEdge> &edges,
	bool reverse)
{
	std::set<cfg::NodeId> visited;
	std::vector<cfg::NodeId> pending{start};
	while (!pending.empty())
	{
		const cfg::NodeId current = pending.back();
		pending.pop_back();
		if (!visited.insert(current).second)
			continue;
		const auto item = adjacency.find(current);
		if (item == adjacency.end())
			continue;
		for (size_t index : item->second)
			pending.push_back(reverse ? edges[index].source : edges[index].target);
	}
	return visited;
}

} // namespace

RelayPathFormula RelayPathFormulaBuilder::Build(
	const RelayProtocolIR &protocol,
	const RelayPairCandidate &candidate,
	const std::string &relaySiteId) const
{
	RelayPathFormula result;
	if (!candidate.exactContext || !candidate.loopFreeContext)
	{
		result.reason = !candidate.exactContext
			? "path formula requires an exact unique CFG/ICFG context"
			: "path formula does not model loop occurrences";
		return result;
	}
	CombinedGraph graph = BuildCombinedGraph(protocol, candidate);
	if (!graph.supported)
	{
		result.reason = graph.reason;
		return result;
	}

	cfg::NodeId target;
	for (const auto &item : graph.nodes)
	{
		if (item.second->kind == cfg::PredaCFGNodeKind::RelayEmit &&
			item.second->relaySiteId == relaySiteId)
		{
			target = item.first;
			break;
		}
	}
	if (target.empty())
	{
		result.reason = "relay site has no node in the composed ICFG";
		return result;
	}

	const std::set<cfg::NodeId> fromEntry = Traverse(
		graph.entry, graph.outgoing, graph.edges, false);
	const std::set<cfg::NodeId> toTarget = Traverse(
		target, graph.incoming, graph.edges, true);
	std::set<cfg::NodeId> relevant;
	std::set_intersection(
		fromEntry.begin(), fromEntry.end(),
		toTarget.begin(), toTarget.end(),
		std::inserter(relevant, relevant.end()));
	if (relevant.find(graph.entry) == relevant.end() ||
		relevant.find(target) == relevant.end())
	{
		result.reason = "relay site is unreachable from the composed root entry";
		return result;
	}

	std::map<cfg::NodeId, size_t> indegree;
	for (const cfg::NodeId &node : relevant)
		indegree[node] = 0;
	for (const CombinedEdge &edge : graph.edges)
		if (relevant.count(edge.source) && relevant.count(edge.target))
			++indegree[edge.target];
	std::vector<cfg::NodeId> ready;
	for (const auto &item : indegree)
		if (item.second == 0)
			ready.push_back(item.first);
	std::sort(ready.begin(), ready.end());
	std::vector<cfg::NodeId> order;
	while (!ready.empty())
	{
		const cfg::NodeId current = ready.front();
		ready.erase(ready.begin());
		order.push_back(current);
		const auto outgoing = graph.outgoing.find(current);
		if (outgoing == graph.outgoing.end())
			continue;
		for (size_t edgeIndex : outgoing->second)
		{
			const cfg::NodeId &successor = graph.edges[edgeIndex].target;
			auto item = indegree.find(successor);
			if (item == indegree.end())
				continue;
			if (--item->second == 0)
			{
				ready.push_back(successor);
				std::sort(ready.begin(), ready.end());
			}
		}
	}
	if (order.size() != relevant.size())
	{
		result.reason = "path formula requires an acyclic relevant ICFG slice";
		return result;
	}

	std::map<cfg::NodeId, refinement::FormulaExpr> paths;
	paths[graph.entry] = refinement::FormulaExpr::BoolLiteral(true);
	for (const cfg::NodeId &node : order)
	{
		if (node == graph.entry)
			continue;
		std::vector<refinement::FormulaExpr> incomingTerms;
		const auto incoming = graph.incoming.find(node);
		if (incoming != graph.incoming.end())
		{
			for (size_t edgeIndex : incoming->second)
			{
				const CombinedEdge &edge = graph.edges[edgeIndex];
				if (!relevant.count(edge.source))
					continue;
				const auto prefix = paths.find(edge.source);
				if (prefix == paths.end())
					continue;
				refinement::FormulaExpr predicate;
				if (!EdgePredicate(graph, edge, predicate, result.reason))
					return result;
				incomingTerms.push_back(And(prefix->second, std::move(predicate)));
				result.supportingCfgFactIds.push_back(edge.id);
			}
		}
		paths[node] = Or(std::move(incomingTerms));
		result.supportingCfgFactIds.push_back(node);
	}
	const auto targetPath = paths.find(target);
	if (targetPath == paths.end() || ContainsUnknown(targetPath->second))
	{
		result.reason = "relay-site path formula is unavailable";
		return result;
	}
	result.supported = true;
	result.feasibilityExact = graph.feasibilityExact;
	result.formula = targetPath->second;
	result.supportingCfgFactIds.insert(
		result.supportingCfgFactIds.end(),
		graph.baseEvidence.begin(), graph.baseEvidence.end());
	std::sort(
		result.supportingCfgFactIds.begin(),
		result.supportingCfgFactIds.end());
	result.supportingCfgFactIds.erase(
		std::unique(
			result.supportingCfgFactIds.begin(),
			result.supportingCfgFactIds.end()),
		result.supportingCfgFactIds.end());
	result.reason = graph.feasibilityExact
		? "exact acyclic PREDA ICFG path formula"
		: "acyclic PREDA ICFG path reachability over-approximation";
	return result;
}

} // namespace certificate
} // namespace relay_protocol
} // namespace transpiler
