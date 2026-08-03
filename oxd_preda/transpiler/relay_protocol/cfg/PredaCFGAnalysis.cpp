#include "PredaCFGAnalysis.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <vector>

namespace transpiler {
namespace relay_protocol {
namespace cfg {
namespace {

std::set<NodeId> Intersect(
	const std::set<NodeId> &left,
	const std::set<NodeId> &right)
{
	std::set<NodeId> result;
	std::set_intersection(
		left.begin(), left.end(),
		right.begin(), right.end(),
		std::inserter(result, result.begin()));
	return result;
}

std::vector<NodeId> SortedVector(const std::set<NodeId> &values)
{
	return std::vector<NodeId>(values.begin(), values.end());
}

std::set<NodeId> Traverse(
	const NodeId &start,
	const std::map<NodeId, std::vector<NodeId>> &adjacency)
{
	std::set<NodeId> visited;
	std::vector<NodeId> pending{start};
	while (!pending.empty())
	{
		const NodeId current = pending.back();
		pending.pop_back();
		if (!visited.insert(current).second)
			continue;
		auto outgoing = adjacency.find(current);
		if (outgoing == adjacency.end())
			continue;
		pending.insert(
			pending.end(),
			outgoing->second.begin(),
			outgoing->second.end());
	}
	return visited;
}

bool HasReachableCycle(
	const NodeId &entry,
	const std::map<NodeId, std::vector<NodeId>> &adjacency)
{
	std::map<NodeId, uint8_t> color;
	std::function<bool(const NodeId &)> visit =
		[&](const NodeId &node)
		{
			color[node] = 1;
			auto outgoing = adjacency.find(node);
			if (outgoing != adjacency.end())
			{
				for (const NodeId &target : outgoing->second)
				{
					if (color[target] == 1)
						return true;
					if (color[target] == 0 && visit(target))
						return true;
				}
			}
			color[node] = 2;
			return false;
		};
	return !entry.empty() && visit(entry);
}

void AppendUnique(
	std::vector<std::string> &reasons,
	const std::string &reason)
{
	if (std::find(reasons.begin(), reasons.end(), reason) == reasons.end())
		reasons.push_back(reason);
}

} // namespace

PredaFunctionGraphAnalysis PredaCFGAnalysis::Analyze(
	const PredaFunctionCFG &function)
{
	PredaFunctionGraphAnalysis result;
	result.functionId = function.functionId;
	result.status = function.status;
	result.completenessReasons = function.completenessReasons;
	std::set<NodeId> allNodes;
	std::map<NodeId, std::vector<NodeId>> successors;
	std::map<NodeId, std::vector<NodeId>> predecessors;
	for (const PredaCFGNode &node : function.nodes)
	{
		allNodes.insert(node.id);
		successors[node.id];
		predecessors[node.id];
		result.loopNesting[node.id] = node.enclosingLoopIds;
		if (!node.supported || node.kind == PredaCFGNodeKind::Opaque)
		{
			result.dominanceComplete = false;
			result.postDominanceComplete = false;
			result.reachabilityComplete = false;
			AppendUnique(
				result.completenessReasons,
				"unsupported or opaque CFG node prevents complete graph analysis");
		}
	}
	for (const PredaCFGEdge &edge : function.edges)
	{
		successors[edge.source].push_back(edge.target);
		predecessors[edge.target].push_back(edge.source);
		if (!edge.supported)
		{
			result.dominanceComplete = false;
			result.postDominanceComplete = false;
			result.reachabilityComplete = false;
			AppendUnique(
				result.completenessReasons,
				"unsupported CFG edge prevents complete graph analysis");
		}
	}
	if ((!result.dominanceComplete || !result.postDominanceComplete ||
		!result.reachabilityComplete) &&
		result.status == AnalysisStatus::Complete)
	{
		result.status = AnalysisStatus::Conservative;
	}

	const std::set<NodeId> entryReachable =
		Traverse(function.entryNodeId, successors);
	const std::set<NodeId> exitReachable =
		Traverse(function.exitNodeId, predecessors);
	for (const NodeId &node : entryReachable)
	{
		if (exitReachable.find(node) == exitReachable.end())
		{
			result.postDominanceComplete = false;
			if (result.status == AnalysisStatus::Complete)
				result.status = AnalysisStatus::Conservative;
			AppendUnique(
				result.completenessReasons,
				"entry-reachable non-terminating path prevents complete post-dominance");
			break;
		}
	}
	if (HasReachableCycle(function.entryNodeId, successors))
	{
		result.postDominanceComplete = false;
		if (result.status == AnalysisStatus::Complete)
			result.status = AnalysisStatus::Conservative;
		AppendUnique(
			result.completenessReasons,
			"entry-reachable cycle has no proved termination bound for post-dominance");
	}
	for (const NodeId &node : allNodes)
		result.reachableNodes[node] = SortedVector(Traverse(node, successors));

	std::map<NodeId, std::set<NodeId>> dominators;
	for (const NodeId &node : allNodes)
	{
		if (node == function.entryNodeId ||
			entryReachable.find(node) == entryReachable.end())
			dominators[node] = {node};
		else
			dominators[node] = entryReachable;
	}
	bool changed = true;
	while (changed)
	{
		changed = false;
		for (const NodeId &node : entryReachable)
		{
			if (node == function.entryNodeId)
				continue;
			bool initialized = false;
			std::set<NodeId> next;
			for (const NodeId &predecessor : predecessors[node])
			{
				if (entryReachable.find(predecessor) ==
					entryReachable.end())
					continue;
				if (!initialized)
				{
					next = dominators[predecessor];
					initialized = true;
				}
				else
				{
					next = Intersect(next, dominators[predecessor]);
				}
			}
			next.insert(node);
			if (next != dominators[node])
			{
				dominators[node] = std::move(next);
				changed = true;
			}
		}
	}

	std::map<NodeId, std::set<NodeId>> postDominators;
	for (const NodeId &node : allNodes)
	{
		if (node == function.exitNodeId ||
			exitReachable.find(node) == exitReachable.end())
			postDominators[node] = {node};
		else
			postDominators[node] = exitReachable;
	}
	changed = true;
	while (changed)
	{
		changed = false;
		for (const NodeId &node : exitReachable)
		{
			if (node == function.exitNodeId)
				continue;
			bool initialized = false;
			std::set<NodeId> next;
			for (const NodeId &successor : successors[node])
			{
				if (exitReachable.find(successor) == exitReachable.end())
					continue;
				if (!initialized)
				{
					next = postDominators[successor];
					initialized = true;
				}
				else
				{
					next = Intersect(next, postDominators[successor]);
				}
			}
			next.insert(node);
			if (next != postDominators[node])
			{
				postDominators[node] = std::move(next);
				changed = true;
			}
		}
	}

	for (const NodeId &node : allNodes)
	{
		result.dominators[node] = result.dominanceComplete
			? SortedVector(dominators[node])
			: std::vector<NodeId>();
		result.postDominators[node] = result.postDominanceComplete
			? SortedVector(postDominators[node])
			: std::vector<NodeId>();
	}
	if (result.postDominanceComplete)
	{
		for (const PredaCFGNode &controller : function.nodes)
		{
			if (successors[controller.id].size() < 2)
				continue;
			std::set<NodeId> dependents;
			for (const NodeId &candidate : allNodes)
			{
				if (candidate == controller.id ||
					postDominators[controller.id].find(candidate) !=
						postDominators[controller.id].end())
					continue;
				for (const NodeId &successor : successors[controller.id])
				{
					if (postDominators[successor].find(candidate) !=
						postDominators[successor].end())
					{
						dependents.insert(candidate);
						break;
					}
				}
			}
			result.controlDependents[controller.id] =
				SortedVector(dependents);
		}
	}
	return result;
}

} // namespace cfg
} // namespace relay_protocol
} // namespace transpiler
