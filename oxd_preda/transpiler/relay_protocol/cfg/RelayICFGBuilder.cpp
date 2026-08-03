#include "RelayICFGBuilder.h"

#include "PredaCFGAnalysis.h"
#include "../RelayProtocolIR.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace transpiler {
namespace relay_protocol {
namespace cfg {
namespace {

const PredaFunctionCFG *FindFunction(
	const std::map<FunctionId, const PredaFunctionCFG *> &functions,
	const FunctionId &functionId)
{
	auto found = functions.find(functionId);
	return found == functions.end() ? nullptr : found->second;
}

const RelayHandler *FindHandler(
	const RelayProtocolIR &protocol,
	const std::string &handlerId)
{
	for (const RelayHandler &handler : protocol.handlers)
	{
		if (handler.id == handlerId)
			return &handler;
	}
	return nullptr;
}

std::vector<FunctionId> FunctionClosure(
	const FunctionId &root,
	const std::map<FunctionId, std::vector<FunctionId>> &adjacency)
{
	std::set<FunctionId> visited;
	std::vector<FunctionId> pending{root};
	while (!pending.empty())
	{
		const FunctionId current = pending.back();
		pending.pop_back();
		auto outgoing = adjacency.find(current);
		if (outgoing == adjacency.end())
			continue;
		for (const FunctionId &target : outgoing->second)
		{
			if (visited.insert(target).second)
				pending.push_back(target);
		}
	}
	return std::vector<FunctionId>(visited.begin(), visited.end());
}

std::vector<FunctionId> AsyncHandlerClosure(
	const FunctionId &root,
	const std::map<FunctionId, std::vector<FunctionId>> &syncAdjacency,
	const std::map<FunctionId, std::vector<FunctionId>> &asyncAdjacency)
{
	std::set<FunctionId> visitedFunctions;
	std::set<FunctionId> handlers;
	std::vector<FunctionId> pending{root};
	while (!pending.empty())
	{
		const FunctionId current = pending.back();
		pending.pop_back();
		if (!visitedFunctions.insert(current).second)
			continue;
		auto sync = syncAdjacency.find(current);
		if (sync != syncAdjacency.end())
			pending.insert(pending.end(), sync->second.begin(), sync->second.end());
		auto async = asyncAdjacency.find(current);
		if (async == asyncAdjacency.end())
			continue;
		for (const FunctionId &handler : async->second)
		{
			handlers.insert(handler);
			pending.push_back(handler);
		}
	}
	return std::vector<FunctionId>(handlers.begin(), handlers.end());
}

bool SameLocation(
	const SourceLocation &left,
	const SourceLocation &right)
{
	return left.startOffset == right.startOffset &&
		left.endOffset == right.endOffset;
}

std::set<NodeId> TraverseNodes(
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

struct SynchronousCallContext
{
	const PredaFunctionCFG *caller = nullptr;
	const PredaCFGNode *callNode = nullptr;
	const PredaFunctionCFG *callee = nullptr;
	const PredaCallEdge *callEdge = nullptr;
	std::vector<NodeId> continuations;
	bool exact = false;
	std::string reason;
};

struct SynchronousCompositionIndex
{
	std::map<FunctionId, const PredaFunctionCFG *> functionsById;
	std::map<FunctionId, const PredaFunctionGraphAnalysis *> analysesByFunction;
	std::map<FunctionId, std::vector<const PredaCallEdge *>>
		incomingEdgesByCallee;
	std::map<FunctionId, std::vector<const PredaCallEdge *>>
		outgoingEdgesByCaller;
	std::map<NodeId, SynchronousCallContext> contextsByNode;
	std::map<FunctionId, std::vector<NodeId>> contextNodesByCaller;
	std::map<const PredaCallEdge *, NodeId> contextNodeByEdge;
};

struct SynchronousCompositionGraph
{
	FunctionId rootFunctionId;
	bool exact = false;
	std::string reason;
	std::set<FunctionId> functions;
	std::map<NodeId, std::vector<NodeId>> adjacency;
	std::set<NodeId> entryReachable;
};

const SynchronousCallContext *ContextForEdge(
	const SynchronousCompositionIndex &index,
	const PredaCallEdge *edge)
{
	auto node = index.contextNodeByEdge.find(edge);
	if (node == index.contextNodeByEdge.end())
		return nullptr;
	auto context = index.contextsByNode.find(node->second);
	return context == index.contextsByNode.end() ? nullptr : &context->second;
}

bool FindUniqueSynchronousRoot(
	const FunctionId &functionId,
	const SynchronousCompositionIndex &index,
	const std::set<FunctionId> &recursiveFunctions,
	FunctionId &root,
	std::string &reason)
{
	std::set<FunctionId> visited;
	FunctionId current = functionId;
	for (;;)
	{
		if (!visited.insert(current).second ||
			recursiveFunctions.find(current) != recursiveFunctions.end())
		{
			reason = "synchronous calling context is recursive";
			return false;
		}
		auto incoming = index.incomingEdgesByCallee.find(current);
		if (incoming == index.incomingEdgesByCallee.end() ||
			incoming->second.empty())
		{
			root = current;
			return true;
		}
		if (incoming->second.size() != 1)
		{
			reason =
				"function has multiple synchronous call sites and therefore no unique calling context";
			return false;
		}
		const PredaCallEdge *edge = incoming->second.front();
		const SynchronousCallContext *context = ContextForEdge(index, edge);
		if (!edge->resolved || context == nullptr || !context->exact)
		{
			reason = context != nullptr && !context->reason.empty()
				? context->reason
				: "synchronous caller context is unresolved or has no exact CFG call node";
			return false;
		}
		current = edge->caller;
	}
}

bool FunctionTopologyIsExact(
	const PredaFunctionCFG &function,
	const PredaSynchronousCallGraph &callGraph,
	const SynchronousCompositionIndex &index,
	std::string &reason)
{
	auto analysis = index.analysesByFunction.find(function.functionId);
	if (analysis == index.analysesByFunction.end() ||
		!analysis->second->reachabilityComplete)
	{
		reason =
			"function reachability analysis is incomplete and cannot support a relation proof";
		return false;
	}
	if (function.status == AnalysisStatus::Unsupported ||
		function.status == AnalysisStatus::Unknown)
	{
		reason = "function CFG status is not usable for an exact composition";
		return false;
	}
	if (function.effect.status == AnalysisStatus::Unsupported ||
		function.effect.status == AnalysisStatus::Unknown ||
		function.effect.mayCallUnknown ||
		function.effect.mayHaveExternalEffect)
	{
		reason = "function effect summary contains an unknown or external effect";
		return false;
	}
	for (const PredaCFGNode &node : function.nodes)
	{
		if (!node.supported || node.kind == PredaCFGNodeKind::Opaque)
		{
			reason = "function CFG contains an unsupported or opaque node";
			return false;
		}
		if (node.kind == PredaCFGNodeKind::AbortOrFailure &&
			!node.successors.empty())
		{
			reason =
				"function CFG contains a non-terminal runtime failure node without exact semantics";
			return false;
		}
		if (node.kind == PredaCFGNodeKind::Basic &&
			!node.sourceExpression.text.empty() &&
			node.sourceExpression.kind == RelayExprKind::Opaque)
		{
			reason = "function CFG contains an opaque source expression";
			return false;
		}
	}
	for (const PredaCFGEdge &edge : function.edges)
	{
		if (!edge.supported || edge.kind == PredaCFGEdgeKind::Unknown)
		{
			reason = "function CFG contains an unsupported control-flow edge";
			return false;
		}
	}
	auto unresolved = callGraph.analysis.hasUnresolvedOutgoing.find(
		function.functionId);
	if (unresolved != callGraph.analysis.hasUnresolvedOutgoing.end() &&
		unresolved->second)
	{
		reason = "function call graph has an unresolved outgoing edge";
		return false;
	}
	auto outgoing = index.outgoingEdgesByCaller.find(function.functionId);
	if (outgoing != index.outgoingEdgesByCaller.end())
	{
		for (const PredaCallEdge *edge : outgoing->second)
		{
			const SynchronousCallContext *context = ContextForEdge(index, edge);
			if (!edge->resolved || context == nullptr || !context->exact)
			{
				reason = context != nullptr && !context->reason.empty()
					? context->reason
					: "resolved synchronous call has no exact CFG call node";
				return false;
			}
		}
	}
	for (const PredaCallEdge &edge : callGraph.edges)
	{
		if (edge.caller != function.functionId)
			continue;
		if (edge.kind == PredaCallKind::ExternalUnknown ||
			edge.kind == PredaCallKind::RuntimeHelper)
		{
			reason =
				"function contains an external or runtime call without exact synchronous semantics";
			return false;
		}
	}
	auto contexts = index.contextNodesByCaller.find(function.functionId);
	if (contexts != index.contextNodesByCaller.end())
	{
		for (const NodeId &nodeId : contexts->second)
		{
			auto context = index.contextsByNode.find(nodeId);
			if (context == index.contextsByNode.end() ||
				!context->second.exact)
			{
				reason = context != index.contextsByNode.end() &&
					!context->second.reason.empty()
					? context->second.reason
					: "CFG synchronous call node is not matched by one resolved call edge";
				return false;
			}
		}
	}
	return true;
}

SynchronousCompositionGraph BuildSynchronousComposition(
	const FunctionId &root,
	const PredaSynchronousCallGraph &callGraph,
	const SynchronousCompositionIndex &index,
	const std::set<FunctionId> &recursiveFunctions)
{
	SynchronousCompositionGraph result;
	result.rootFunctionId = root;
	auto rootFunction = index.functionsById.find(root);
	if (rootFunction == index.functionsById.end())
	{
		result.reason = "synchronous root function CFG is unavailable";
		return result;
	}
	auto rootIncoming = index.incomingEdgesByCallee.find(root);
	if (rootIncoming != index.incomingEdgesByCallee.end() &&
		!rootIncoming->second.empty())
	{
		result.reason = "selected synchronous root still has an incoming call edge";
		return result;
	}

	std::vector<FunctionId> pending{root};
	while (!pending.empty())
	{
		const FunctionId current = pending.back();
		pending.pop_back();
		if (!result.functions.insert(current).second)
			continue;
		if (recursiveFunctions.find(current) != recursiveFunctions.end())
		{
			result.reason = "synchronous composition contains recursion";
			return result;
		}
		auto function = index.functionsById.find(current);
		if (function == index.functionsById.end())
		{
			result.reason = "synchronous composition references a missing function CFG";
			return result;
		}
		if (!FunctionTopologyIsExact(
				*function->second,
				callGraph,
				index,
				result.reason))
		{
			return result;
		}
		auto outgoing = index.outgoingEdgesByCaller.find(current);
		if (outgoing == index.outgoingEdgesByCaller.end())
			continue;
		for (const PredaCallEdge *edge : outgoing->second)
		{
			auto incoming = index.incomingEdgesByCallee.find(edge->callee);
			if (incoming == index.incomingEdgesByCallee.end() ||
				incoming->second.size() != 1)
			{
				result.reason =
					"callee does not have one unique synchronous call context";
				return result;
			}
			const SynchronousCallContext *context = ContextForEdge(index, edge);
			if (context == nullptr || !context->exact)
			{
				result.reason = context != nullptr && !context->reason.empty()
					? context->reason
					: "synchronous call context is not exact";
				return result;
			}
			pending.push_back(edge->callee);
		}
	}

	std::map<NodeId, PredaCFGNodeKind> nodeKinds;
	for (const FunctionId &functionId : result.functions)
	{
		const PredaFunctionCFG &function = *index.functionsById.at(functionId);
		for (const PredaCFGNode &node : function.nodes)
		{
			result.adjacency[node.id];
			nodeKinds[node.id] = node.kind;
		}
	}
	for (const FunctionId &functionId : result.functions)
	{
		const PredaFunctionCFG &function = *index.functionsById.at(functionId);
		for (const PredaCFGEdge &edge : function.edges)
		{
			if (nodeKinds[edge.source] == PredaCFGNodeKind::SynchronousCall)
				continue;
			result.adjacency[edge.source].push_back(edge.target);
		}
		auto outgoing = index.outgoingEdgesByCaller.find(functionId);
		if (outgoing == index.outgoingEdgesByCaller.end())
			continue;
		for (const PredaCallEdge *edge : outgoing->second)
		{
			const SynchronousCallContext *context = ContextForEdge(index, edge);
			result.adjacency[context->callNode->id].push_back(
				context->callee->entryNodeId);
			for (const NodeId &continuation : context->continuations)
			{
				result.adjacency[context->callee->exitNodeId].push_back(
					continuation);
			}
		}
	}
	for (auto &item : result.adjacency)
	{
		std::sort(item.second.begin(), item.second.end());
		item.second.erase(
			std::unique(item.second.begin(), item.second.end()),
			item.second.end());
	}
	result.entryReachable = TraverseNodes(
		rootFunction->second->entryNodeId,
		result.adjacency);
	result.exact = true;
	return result;
}

PredaFunctionGraphAnalysis AnalyzeSynchronousComposition(
	const SynchronousCompositionGraph &composition,
	const SynchronousCompositionIndex &index)
{
	if (!composition.exact)
	{
		PredaFunctionGraphAnalysis unsupported;
		unsupported.functionId = composition.rootFunctionId;
		unsupported.status = AnalysisStatus::Unknown;
		unsupported.dominanceComplete = false;
		unsupported.postDominanceComplete = false;
		unsupported.reachabilityComplete = false;
		unsupported.completenessReasons.push_back(
			composition.reason.empty()
				? "synchronous ICFG composition is unavailable"
				: composition.reason);
		return unsupported;
	}

	PredaFunctionCFG combined;
	combined.functionId = composition.rootFunctionId;
	combined.function = "synchronous_icfg";
	combined.status = AnalysisStatus::Complete;
	auto root = index.functionsById.find(composition.rootFunctionId);
	if (root == index.functionsById.end())
	{
		combined.status = AnalysisStatus::Unsupported;
		combined.completenessReasons.push_back(
			"synchronous ICFG root function is missing");
	}
	else
	{
		combined.entryNodeId = root->second->entryNodeId;
		combined.exitNodeId = root->second->exitNodeId;
	}
	for (const FunctionId &functionId : composition.functions)
	{
		auto function = index.functionsById.find(functionId);
		if (function == index.functionsById.end())
			continue;
		combined.nodes.insert(
			combined.nodes.end(),
			function->second->nodes.begin(),
			function->second->nodes.end());
	}
	uint64_t edgeOrdinal = 0;
	for (const auto &item : composition.adjacency)
	{
		for (const NodeId &target : item.second)
		{
			PredaCFGEdge edge;
			edge.id = "sync_icfg_edge::" +
				composition.rootFunctionId + "::" +
				std::to_string(edgeOrdinal++);
			edge.source = item.first;
			edge.target = target;
			edge.kind = PredaCFGEdgeKind::Fallthrough;
			edge.supported = true;
			combined.edges.push_back(std::move(edge));
		}
	}
	return PredaCFGAnalysis::Analyze(combined);
}

} // namespace

RelayICFG RelayICFGBuilder::Build(
	const std::vector<PredaFunctionCFG> &functions,
	const PredaSynchronousCallGraph &callGraph,
	const RelayProtocolIR &protocol)
{
	RelayICFG result;
	std::map<FunctionId, const PredaFunctionCFG *> functionsById;
	std::map<FunctionId, std::vector<FunctionId>> syncAdjacency;
	std::map<FunctionId, std::vector<FunctionId>> asyncAdjacency;
	for (const PredaFunctionCFG &function : functions)
	{
		functionsById[function.functionId] = &function;
		result.functionAnalyses.push_back(
			PredaCFGAnalysis::Analyze(function));
	}

	SynchronousCompositionIndex compositionIndex;
	compositionIndex.functionsById = functionsById;
	for (const PredaFunctionGraphAnalysis &analysis : result.functionAnalyses)
		compositionIndex.analysesByFunction[analysis.functionId] = &analysis;
	for (const PredaCallEdge &edge : callGraph.edges)
	{
		if (edge.kind != PredaCallKind::Synchronous)
			continue;
		compositionIndex.outgoingEdgesByCaller[edge.caller].push_back(&edge);
		compositionIndex.incomingEdgesByCallee[edge.callee].push_back(&edge);
	}
	for (const PredaFunctionCFG &caller : functions)
	{
		for (const PredaCFGNode &callNode : caller.nodes)
		{
			if (callNode.kind != PredaCFGNodeKind::SynchronousCall)
				continue;
			SynchronousCallContext context;
			context.caller = &caller;
			context.callNode = &callNode;
			context.callee = FindFunction(
				functionsById,
				callNode.calleeFunctionId);
			std::vector<const PredaCallEdge *> matches;
			auto outgoing = compositionIndex.outgoingEdgesByCaller.find(
				caller.functionId);
			if (outgoing != compositionIndex.outgoingEdgesByCaller.end())
			{
				for (const PredaCallEdge *edge : outgoing->second)
				{
					if (edge->callee == callNode.calleeFunctionId &&
						SameLocation(edge->callSite, callNode.location))
					{
						matches.push_back(edge);
					}
				}
			}
			bool continuationsExact = true;
			for (const PredaCFGEdge &edge : caller.edges)
			{
				if (edge.source != callNode.id)
					continue;
				if (!edge.supported ||
					edge.kind != PredaCFGEdgeKind::Fallthrough)
				{
					continuationsExact = false;
					continue;
				}
				context.continuations.push_back(edge.target);
			}
			if (matches.size() != 1)
			{
				context.reason =
					"CFG call node does not match exactly one typed synchronous call edge";
			}
			else
			{
				context.callEdge = matches.front();
				if (!context.callEdge->resolved)
					context.reason = "typed synchronous call edge is unresolved";
				else if (context.callee == nullptr)
					context.reason = "synchronous callee CFG is unavailable";
				else if (!callNode.supported || !continuationsExact ||
					context.continuations.empty())
				{
					context.reason =
						"synchronous call node has no exact supported continuation";
				}
				else
					context.exact = true;
			}
			if (context.callEdge != nullptr)
			{
				auto previous = compositionIndex.contextNodeByEdge.find(
					context.callEdge);
				if (previous != compositionIndex.contextNodeByEdge.end())
				{
					context.exact = false;
					context.reason =
						"typed synchronous call edge matches multiple CFG call nodes";
					auto previousContext =
						compositionIndex.contextsByNode.find(previous->second);
					if (previousContext !=
						compositionIndex.contextsByNode.end())
					{
						previousContext->second.exact = false;
						previousContext->second.reason = context.reason;
					}
				}
				else
				{
					compositionIndex.contextNodeByEdge[context.callEdge] =
						callNode.id;
				}
			}
			compositionIndex.contextNodesByCaller[caller.functionId].push_back(
				callNode.id);
			compositionIndex.contextsByNode[callNode.id] = std::move(context);
		}
	}

	uint64_t edgeOrdinal = 0;
	for (const PredaFunctionCFG &caller : functions)
	{
		for (const PredaCFGNode &callNode : caller.nodes)
		{
			if (callNode.kind != PredaCFGNodeKind::SynchronousCall)
				continue;
			const PredaFunctionCFG *callee = FindFunction(
				functionsById,
				callNode.calleeFunctionId);
			PredaInterproceduralEdge call;
			call.id = "icfg_edge_" + std::to_string(edgeOrdinal++);
			call.kind = InterproceduralEdgeKind::SyncCall;
			call.sourceFunctionId = caller.functionId;
			call.sourceNodeId = callNode.id;
			call.targetFunctionId = callNode.calleeFunctionId;
			call.resolved = callee != nullptr;
			if (callee != nullptr)
			{
				call.targetNodeId = callee->entryNodeId;
				syncAdjacency[caller.functionId].push_back(
					callee->functionId);
			}
			else
				call.reason = "synchronous callee CFG is unavailable";
			result.interproceduralEdges.push_back(std::move(call));
			if (callee == nullptr)
				continue;
			for (const NodeId &continuation : callNode.successors)
			{
				PredaInterproceduralEdge returned;
				returned.id =
					"icfg_edge_" + std::to_string(edgeOrdinal++);
				returned.kind = InterproceduralEdgeKind::SyncReturn;
				returned.sourceFunctionId = callee->functionId;
				returned.sourceNodeId = callee->exitNodeId;
				returned.targetFunctionId = caller.functionId;
				returned.targetNodeId = continuation;
				returned.resolved = true;
				result.interproceduralEdges.push_back(
					std::move(returned));
			}
		}
	}

	struct SiteNode
	{
		const PredaFunctionCFG *function = nullptr;
		const PredaCFGNode *node = nullptr;
	};
	std::map<std::string, SiteNode> siteNodes;
	for (const PredaFunctionCFG &function : functions)
	{
		for (const PredaCFGNode &node : function.nodes)
		{
			if (node.kind == PredaCFGNodeKind::RelayEmit &&
				!node.relaySiteId.empty())
			{
				siteNodes[node.relaySiteId] = SiteNode{&function, &node};
			}
		}
	}
	for (const RelayProtocolEdge &relay : protocol.edges)
	{
		auto source = siteNodes.find(relay.relaySiteId);
		const RelayHandler *handler = FindHandler(protocol, relay.handlerId);
		const PredaFunctionCFG *target = handler == nullptr
			? nullptr
			: FindFunction(functionsById, handler->targetFunctionId);
		PredaInterproceduralEdge spawn;
		spawn.id = "icfg_edge_" + std::to_string(edgeOrdinal++);
		spawn.kind = InterproceduralEdgeKind::AsyncRelaySpawn;
		spawn.sourceFunctionId = relay.sourceFunctionId;
		spawn.sourceNodeId = source == siteNodes.end()
			? NodeId()
			: source->second.node->id;
		spawn.targetFunctionId = handler == nullptr
			? FunctionId()
			: handler->targetFunctionId;
		spawn.targetNodeId = target == nullptr
			? NodeId()
			: target->entryNodeId;
		spawn.relaySiteId = relay.relaySiteId;
		spawn.resolved = source != siteNodes.end() &&
			handler != nullptr && handler->resolved && target != nullptr;
		if (!spawn.resolved)
			spawn.reason = "relay site or handler CFG is unresolved";
		result.interproceduralEdges.push_back(std::move(spawn));
		if (handler != nullptr && handler->resolved && target != nullptr)
		{
			asyncAdjacency[relay.sourceFunctionId].push_back(
				handler->targetFunctionId);
		}
	}

	for (auto &item : syncAdjacency)
	{
		std::sort(item.second.begin(), item.second.end());
		item.second.erase(
			std::unique(item.second.begin(), item.second.end()),
			item.second.end());
	}
	for (auto &item : asyncAdjacency)
	{
		std::sort(item.second.begin(), item.second.end());
		item.second.erase(
			std::unique(item.second.begin(), item.second.end()),
			item.second.end());
	}
	for (const PredaFunctionCFG &function : functions)
	{
		result.synchronousReachableFunctions[function.functionId] =
			FunctionClosure(function.functionId, syncAdjacency);
		result.asyncReachableHandlers[function.functionId] =
			AsyncHandlerClosure(
				function.functionId,
				syncAdjacency,
				asyncAdjacency);
	}
	result.synchronousSccs =
		callGraph.analysis.stronglyConnectedComponents;

	const std::set<FunctionId> recursiveFunctions(
		callGraph.analysis.recursiveFunctions.begin(),
		callGraph.analysis.recursiveFunctions.end());
	std::map<FunctionId, SynchronousCompositionGraph> compositionByRoot;
	for (const PredaFunctionCFG &function : functions)
	{
		auto incoming = compositionIndex.incomingEdgesByCallee.find(
			function.functionId);
		const bool isRoot =
			incoming == compositionIndex.incomingEdgesByCallee.end() ||
			incoming->second.empty();
		const bool recursive = recursiveFunctions.find(function.functionId) !=
			recursiveFunctions.end();
		if (!isRoot && !recursive)
			continue;
		compositionByRoot.emplace(
			function.functionId,
			BuildSynchronousComposition(
				function.functionId,
				callGraph,
				compositionIndex,
				recursiveFunctions));
	}
	for (const auto &item : compositionByRoot)
	{
		result.synchronousCompositionAnalyses.push_back(
			AnalyzeSynchronousComposition(item.second, compositionIndex));
	}
	for (size_t first = 0; first < protocol.relaySites.size(); ++first)
	{
		for (size_t second = first + 1;
			second < protocol.relaySites.size();
			++second)
		{
			const RelaySite &left = protocol.relaySites[first];
			const RelaySite &right = protocol.relaySites[second];
			RelaySiteRelation relation;
			relation.firstRelaySiteId = left.id;
			relation.secondRelaySiteId = right.id;
			auto leftNode = siteNodes.find(left.id);
			auto rightNode = siteNodes.find(right.id);
			if (leftNode == siteNodes.end() ||
				rightNode == siteNodes.end())
			{
				relation.reason = "one or both RelayEmit CFG nodes are missing";
			}
			else if (leftNode->second.function->functionId !=
					left.sourceFunctionId ||
				rightNode->second.function->functionId !=
					right.sourceFunctionId)
			{
				relation.reason =
					"RelaySite source function does not match its owning CFG";
			}
			else
			{
				FunctionId leftRoot;
				FunctionId rightRoot;
				std::string leftContextReason;
				std::string rightContextReason;
				const bool leftContextExact = FindUniqueSynchronousRoot(
					leftNode->second.function->functionId,
					compositionIndex,
					recursiveFunctions,
					leftRoot,
					leftContextReason);
				const bool rightContextExact = FindUniqueSynchronousRoot(
					rightNode->second.function->functionId,
					compositionIndex,
					recursiveFunctions,
					rightRoot,
					rightContextReason);
				if (!leftContextExact)
				{
					relation.reason =
						"left relay site has no exact unique synchronous context: " +
						leftContextReason;
				}
				else if (!rightContextExact)
				{
					relation.reason =
						"right relay site has no exact unique synchronous context: " +
						rightContextReason;
				}
				else if (leftRoot != rightRoot)
				{
					relation.reason =
						"relay sites do not share one provable synchronous root context";
				}
				else
				{
					auto composition = compositionByRoot.find(leftRoot);
					if (composition == compositionByRoot.end())
					{
						composition = compositionByRoot.emplace(
							leftRoot,
							BuildSynchronousComposition(
								leftRoot,
								callGraph,
								compositionIndex,
								recursiveFunctions)).first;
					}
					const SynchronousCompositionGraph &graph =
						composition->second;
					if (!graph.exact)
					{
						relation.reason =
							"synchronous composition is incomplete: " +
							graph.reason;
					}
					else if (graph.entryReachable.find(
							leftNode->second.node->id) ==
							graph.entryReachable.end() ||
						graph.entryReachable.find(
							rightNode->second.node->id) ==
							graph.entryReachable.end())
					{
						relation.reason =
							"one or both relay sites are unreachable from the unique synchronous root entry";
					}
					else
					{
						const std::set<NodeId> leftReachable = TraverseNodes(
							leftNode->second.node->id,
							graph.adjacency);
						const std::set<NodeId> rightReachable = TraverseNodes(
							rightNode->second.node->id,
							graph.adjacency);
						const bool leftToRight = leftReachable.find(
							rightNode->second.node->id) !=
							leftReachable.end();
						const bool rightToLeft = rightReachable.find(
							leftNode->second.node->id) !=
							rightReachable.end();
						if (leftToRight || rightToLeft)
						{
							relation.status =
								RelaySiteRelationStatus::CoReachable;
							relation.reason =
								"an exact supported synchronous ICFG path rooted at " +
								leftRoot + " can visit both relay sites";
						}
						else
						{
							relation.status =
								RelaySiteRelationStatus::MutuallyExclusive;
							relation.reason =
								"neither relay site reaches the other in the exact supported synchronous ICFG rooted at " +
								leftRoot;
						}
					}
				}
			}
			result.relaySiteRelations.push_back(std::move(relation));
		}
	}
	return result;
}

} // namespace cfg
} // namespace relay_protocol
} // namespace transpiler
