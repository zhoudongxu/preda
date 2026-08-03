#include "PredaCallGraphAnalysis.h"

#include <algorithm>
#include <functional>
#include <map>
#include <queue>
#include <set>

namespace transpiler {
namespace relay_protocol {
namespace cfg {
namespace {

void SortUnique(std::vector<FunctionId> &values)
{
	std::sort(values.begin(), values.end());
	values.erase(std::unique(values.begin(), values.end()), values.end());
}

} // namespace

void PredaCallGraphAnalyzer::Analyze(
	PredaSynchronousCallGraph &graph,
	const std::vector<PredaFunctionCFG> &functions)
{
	graph.analysis = cfg::PredaCallGraphAnalysis();
	SortUnique(graph.functions);
	std::set<FunctionId> functionSet(
		graph.functions.begin(), graph.functions.end());
	std::map<FunctionId, std::vector<FunctionId>> adjacency;
	for (const FunctionId &function : graph.functions)
	{
		adjacency[function];
		graph.analysis.reverseCallers[function];
		graph.analysis.hasUnresolvedOutgoing[function] = false;
		graph.analysis.relayReachable[function] = false;
	}
	for (const PredaFunctionCFG &function : functions)
	{
		for (const PredaCFGNode &node : function.nodes)
		{
			if (node.kind == PredaCFGNodeKind::RelayEmit)
				graph.analysis.relayReachable[function.functionId] = true;
		}
	}
	for (const PredaCallEdge &edge : graph.edges)
	{
		if (edge.kind == PredaCallKind::Relay)
		{
			graph.analysis.relayReachable[edge.caller] = true;
			continue;
		}
		if (edge.kind != PredaCallKind::Synchronous ||
			!edge.resolved ||
			functionSet.find(edge.callee) == functionSet.end())
		{
			if (edge.kind == PredaCallKind::ExternalUnknown ||
				edge.kind == PredaCallKind::Synchronous)
			{
				graph.analysis.hasUnresolvedOutgoing[edge.caller] = true;
			}
			continue;
		}
		adjacency[edge.caller].push_back(edge.callee);
		graph.analysis.reverseCallers[edge.callee].push_back(edge.caller);
	}
	for (auto &item : adjacency)
		SortUnique(item.second);
	for (auto &item : graph.analysis.reverseCallers)
		SortUnique(item.second);

	std::map<FunctionId, int> index;
	std::map<FunctionId, int> lowLink;
	std::set<FunctionId> onStack;
	std::vector<FunctionId> stack;
	int nextIndex = 0;
	std::function<void(const FunctionId &)> visit =
		[&](const FunctionId &function)
		{
			index[function] = nextIndex;
			lowLink[function] = nextIndex;
			++nextIndex;
			stack.push_back(function);
			onStack.insert(function);
			for (const FunctionId &callee : adjacency[function])
			{
				if (index.find(callee) == index.end())
				{
					visit(callee);
					lowLink[function] = std::min(
						lowLink[function], lowLink[callee]);
				}
				else if (onStack.find(callee) != onStack.end())
				{
					lowLink[function] = std::min(
						lowLink[function], index[callee]);
				}
			}
			if (lowLink[function] != index[function])
				return;
			std::vector<FunctionId> component;
			for (;;)
			{
				const FunctionId member = stack.back();
				stack.pop_back();
				onStack.erase(member);
				component.push_back(member);
				if (member == function)
					break;
			}
			SortUnique(component);
			graph.analysis.stronglyConnectedComponents.push_back(
				std::move(component));
		};
	for (const FunctionId &function : graph.functions)
	{
		if (index.find(function) == index.end())
			visit(function);
	}
	std::sort(
		graph.analysis.stronglyConnectedComponents.begin(),
		graph.analysis.stronglyConnectedComponents.end(),
		[](const std::vector<FunctionId> &left,
			const std::vector<FunctionId> &right)
		{
			return left < right;
		});

	std::map<FunctionId, size_t> componentOf;
	for (size_t componentIndex = 0;
		componentIndex <
			graph.analysis.stronglyConnectedComponents.size();
		++componentIndex)
	{
		const std::vector<FunctionId> &component =
			graph.analysis.stronglyConnectedComponents[componentIndex];
		for (const FunctionId &member : component)
			componentOf[member] = componentIndex;
		bool recursive = component.size() > 1;
		if (!recursive && !component.empty())
		{
			const FunctionId &member = component.front();
			recursive = std::find(
				adjacency[member].begin(),
				adjacency[member].end(),
				member) != adjacency[member].end();
		}
		if (recursive)
		{
			graph.analysis.recursiveFunctions.insert(
				graph.analysis.recursiveFunctions.end(),
				component.begin(),
				component.end());
		}
	}
	SortUnique(graph.analysis.recursiveFunctions);

	std::vector<std::set<size_t>> componentEdges(
		graph.analysis.stronglyConnectedComponents.size());
	std::vector<size_t> indegree(componentEdges.size(), 0);
	for (const auto &item : adjacency)
	{
		const size_t sourceComponent = componentOf[item.first];
		for (const FunctionId &callee : item.second)
		{
			const size_t targetComponent = componentOf[callee];
			if (sourceComponent != targetComponent &&
				componentEdges[sourceComponent].insert(targetComponent).second)
			{
				++indegree[targetComponent];
			}
		}
	}
	std::priority_queue<size_t, std::vector<size_t>, std::greater<size_t>> ready;
	for (size_t component = 0; component < indegree.size(); ++component)
	{
		if (indegree[component] == 0)
			ready.push(component);
	}
	while (!ready.empty())
	{
		const size_t component = ready.top();
		ready.pop();
		graph.analysis.topologicalComponents.push_back(
			graph.analysis.stronglyConnectedComponents[component]);
		for (size_t target : componentEdges[component])
		{
			if (--indegree[target] == 0)
				ready.push(target);
		}
	}

	bool changed = true;
	while (changed)
	{
		changed = false;
		for (const auto &item : adjacency)
		{
			if (graph.analysis.relayReachable[item.first])
				continue;
			for (const FunctionId &callee : item.second)
			{
				if (graph.analysis.relayReachable[callee])
				{
					graph.analysis.relayReachable[item.first] = true;
					changed = true;
					break;
				}
			}
		}
	}
}

} // namespace cfg
} // namespace relay_protocol
} // namespace transpiler
