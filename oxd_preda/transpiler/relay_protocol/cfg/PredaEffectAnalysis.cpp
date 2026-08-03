#include "PredaEffectAnalysis.h"

#include "../RelayProtocolIR.h"

#include <algorithm>
#include <map>
#include <set>

namespace transpiler {
namespace relay_protocol {
namespace cfg {
namespace {

int StatusRank(AnalysisStatus status)
{
	switch (status)
	{
	case AnalysisStatus::Complete: return 0;
	case AnalysisStatus::Conservative: return 1;
	case AnalysisStatus::Unsupported: return 2;
	case AnalysisStatus::Unknown: return 3;
	default: return 3;
	}
}

void AppendReason(std::string &destination, const std::string &reason)
{
	if (reason.empty() || destination == reason)
		return;
	if (destination.find(reason) != std::string::npos)
		return;
	if (!destination.empty())
		destination += "; ";
	destination += reason;
}

bool SameEffect(const EffectSummary &left, const EffectSummary &right)
{
	return left.readsCurrentScopeState == right.readsCurrentScopeState &&
		left.writesCurrentScopeState == right.writesCurrentScopeState &&
		left.readsGlobalState == right.readsGlobalState &&
		left.writesGlobalState == right.writesGlobalState &&
		left.writesLocalState == right.writesLocalState &&
		left.modifiesLoopInductionVariable ==
			right.modifiesLoopInductionVariable &&
		left.mayEmitRelay == right.mayEmitRelay &&
		left.mayCallUnknown == right.mayCallUnknown &&
		left.mayReturnEarly == right.mayReturnEarly &&
		left.mayBreak == right.mayBreak &&
		left.mayContinue == right.mayContinue &&
		left.mayAbortOrFail == right.mayAbortOrFail &&
		left.mayHaveExternalEffect == right.mayHaveExternalEffect &&
		left.readStateVariables == right.readStateVariables &&
		left.writtenStateVariables == right.writtenStateVariables &&
		left.status == right.status &&
		left.reason == right.reason;
}

EffectSummary EffectForNodes(
	const PredaFunctionCFG &function,
	const std::vector<NodeId> &nodeIds,
	const std::map<FunctionId, EffectSummary> &functionEffects)
{
	EffectSummary result;
	std::set<NodeId> selected(nodeIds.begin(), nodeIds.end());
	for (const PredaCFGNode &node : function.nodes)
	{
		if (selected.find(node.id) == selected.end())
			continue;
		PredaEffectAnalysis::MergeInto(result, node.directEffect);
		if (node.kind == PredaCFGNodeKind::SynchronousCall &&
			!node.calleeFunctionId.empty())
		{
			auto callee = functionEffects.find(node.calleeFunctionId);
			if (callee != functionEffects.end())
				PredaEffectAnalysis::MergeInto(result, callee->second);
			else
			{
				result.mayCallUnknown = true;
				result.mayHaveExternalEffect = true;
				result.status = AnalysisStatus::Unknown;
				AppendReason(
					result.reason,
					"synchronous call has no effect summary");
			}
		}
	}
	return result;
}

} // namespace

void PredaEffectAnalysis::MergeInto(
	EffectSummary &destination,
	const EffectSummary &source)
{
	destination.readsCurrentScopeState |= source.readsCurrentScopeState;
	destination.writesCurrentScopeState |= source.writesCurrentScopeState;
	destination.readsGlobalState |= source.readsGlobalState;
	destination.writesGlobalState |= source.writesGlobalState;
	destination.writesLocalState |= source.writesLocalState;
	destination.modifiesLoopInductionVariable |=
		source.modifiesLoopInductionVariable;
	destination.mayEmitRelay |= source.mayEmitRelay;
	destination.mayCallUnknown |= source.mayCallUnknown;
	destination.mayReturnEarly |= source.mayReturnEarly;
	destination.mayBreak |= source.mayBreak;
	destination.mayContinue |= source.mayContinue;
	destination.mayAbortOrFail |= source.mayAbortOrFail;
	destination.mayHaveExternalEffect |= source.mayHaveExternalEffect;
	destination.readStateVariables.insert(
		source.readStateVariables.begin(),
		source.readStateVariables.end());
	destination.writtenStateVariables.insert(
		source.writtenStateVariables.begin(),
		source.writtenStateVariables.end());
	if (StatusRank(source.status) > StatusRank(destination.status))
		destination.status = source.status;
	AppendReason(destination.reason, source.reason);
}

std::vector<PredaRegionEffect> PredaEffectAnalysis::Build(
	std::vector<PredaFunctionCFG> &functions,
	const PredaSynchronousCallGraph &callGraph,
	const RelayProtocolIR &protocol)
{
	std::map<FunctionId, size_t> functionIndex;
	std::map<FunctionId, EffectSummary> directEffects;
	std::map<FunctionId, EffectSummary> functionEffects;
	for (size_t index = 0; index < functions.size(); ++index)
	{
		PredaFunctionCFG &function = functions[index];
		functionIndex[function.functionId] = index;
		EffectSummary effect;
		for (const PredaCFGNode &node : function.nodes)
			MergeInto(effect, node.directEffect);
		if (function.status == AnalysisStatus::Unsupported)
		{
			effect.status = AnalysisStatus::Unsupported;
			AppendReason(effect.reason, "function CFG is unsupported");
		}
		else if (function.status == AnalysisStatus::Conservative &&
			StatusRank(effect.status) <
				StatusRank(AnalysisStatus::Conservative))
		{
			effect.status = AnalysisStatus::Conservative;
			AppendReason(effect.reason, "function CFG is conservative");
		}
		directEffects[function.functionId] = effect;
		functionEffects[function.functionId] = effect;
	}

	for (const PredaCallEdge &edge : callGraph.edges)
	{
		if (edge.kind == PredaCallKind::Synchronous && !edge.resolved)
		{
			EffectSummary &caller = directEffects[edge.caller];
			caller.mayCallUnknown = true;
			caller.mayAbortOrFail = true;
			caller.mayHaveExternalEffect = true;
			caller.status = AnalysisStatus::Unknown;
			AppendReason(caller.reason, "unresolved synchronous callee");
			functionEffects[edge.caller] = caller;
		}
		else if (edge.kind == PredaCallKind::ExternalUnknown)
		{
			EffectSummary &caller = directEffects[edge.caller];
			caller.mayCallUnknown = true;
			caller.mayAbortOrFail = true;
			caller.mayHaveExternalEffect = true;
			caller.status = AnalysisStatus::Unknown;
			AppendReason(caller.reason, "external call effect is unknown");
			functionEffects[edge.caller] = caller;
		}
		else if (edge.kind == PredaCallKind::RuntimeHelper)
		{
			EffectSummary &caller = directEffects[edge.caller];
			caller.mayAbortOrFail = true;
			caller.mayHaveExternalEffect = true;
			if (!edge.resolved)
			{
				caller.mayCallUnknown = true;
				caller.status = AnalysisStatus::Unknown;
				AppendReason(
					caller.reason,
					"runtime helper provenance is unresolved");
			}
			else if (StatusRank(caller.status) <
				StatusRank(AnalysisStatus::Conservative))
			{
				caller.status = AnalysisStatus::Conservative;
				AppendReason(
					caller.reason,
					"runtime helper effects are conservatively modeled");
			}
			functionEffects[edge.caller] = caller;
		}
	}

	const size_t iterationLimit =
		std::max<size_t>(16, functions.size() * 16 + 1);
	bool stable = false;
	for (size_t iteration = 0;
		iteration < iterationLimit;
		++iteration)
	{
		bool changed = false;
		std::map<FunctionId, EffectSummary> next = directEffects;
		for (const PredaCallEdge &edge : callGraph.edges)
		{
			if (edge.kind != PredaCallKind::Synchronous ||
				!edge.resolved)
			{
				continue;
			}
			auto callee = functionEffects.find(edge.callee);
			if (callee == functionEffects.end())
			{
				EffectSummary &caller = next[edge.caller];
				caller.mayCallUnknown = true;
				caller.mayAbortOrFail = true;
				caller.mayHaveExternalEffect = true;
				caller.status = AnalysisStatus::Unknown;
				AppendReason(
					caller.reason,
					"resolved call has no local function body");
				continue;
			}
			MergeInto(next[edge.caller], callee->second);
		}
		for (const auto &item : next)
		{
			auto previous = functionEffects.find(item.first);
			if (previous == functionEffects.end() ||
				!SameEffect(previous->second, item.second))
			{
				changed = true;
				break;
			}
		}
		functionEffects.swap(next);
		if (!changed)
		{
			stable = true;
			break;
		}
	}
	if (!stable)
	{
		for (const FunctionId &function :
			callGraph.analysis.recursiveFunctions)
		{
			EffectSummary &effect = functionEffects[function];
			effect.status = AnalysisStatus::Unknown;
			AppendReason(
				effect.reason,
				"recursive effect fixed point did not stabilize");
		}
	}
	for (PredaFunctionCFG &function : functions)
	{
		function.effect = functionEffects[function.functionId];
		if (function.status == AnalysisStatus::Complete &&
			function.effect.status != AnalysisStatus::Complete)
		{
			function.status = AnalysisStatus::Conservative;
			const std::string reason = function.effect.reason.empty()
				? "interprocedural effect summary is incomplete"
				: function.effect.reason;
			if (std::find(
				function.completenessReasons.begin(),
				function.completenessReasons.end(),
				reason) == function.completenessReasons.end())
			{
				function.completenessReasons.push_back(reason);
			}
		}
	}

	std::vector<PredaRegionEffect> result;
	for (const PredaFunctionCFG &function : functions)
	{
		for (const PredaCFGRegion &region : function.regions)
		{
			PredaRegionEffect effect;
			effect.regionId = region.id;
			effect.kind = region.kind;
			effect.sourceFunctionId = region.sourceFunctionId;
			effect.relaySiteId = region.relaySiteId;
			effect.callSiteNodeId = region.callSiteNodeId;
			effect.nodeIds = region.nodeIds;
			effect.effect = region.kind == RegionKind::Function
				? function.effect
				: EffectForNodes(
					function,
					region.nodeIds,
					functionEffects);
			result.push_back(std::move(effect));
		}
	}

	for (const RelayHandler &handler : protocol.handlers)
	{
		PredaRegionEffect effect;
		effect.regionId = "region::handler::" + handler.id;
		effect.kind = RegionKind::RelayHandler;
		effect.sourceFunctionId = handler.targetFunctionId;
		auto summary = functionEffects.find(handler.targetFunctionId);
		if (handler.resolved && summary != functionEffects.end())
			effect.effect = summary->second;
		else
		{
			effect.effect.mayCallUnknown = true;
			effect.effect.mayHaveExternalEffect = true;
			effect.effect.status = AnalysisStatus::Unknown;
			effect.effect.reason = "relay handler body is unresolved";
		}
		result.push_back(std::move(effect));
	}
	for (const RelaySite &site : protocol.relaySites)
	{
		PredaRegionEffect effect;
		effect.regionId = "region::relay::" + site.id;
		effect.kind = RegionKind::RelayRegion;
		effect.sourceFunctionId = site.sourceFunctionId;
		effect.relaySiteId = site.id;
		effect.effect.mayEmitRelay = true;
		bool foundHandler = false;
		for (const RelayHandler &handler : protocol.handlers)
		{
			if (handler.id != site.handlerId)
				continue;
			foundHandler = true;
			auto handlerEffect = functionEffects.find(
				handler.targetFunctionId);
			if (handler.resolved &&
				handlerEffect != functionEffects.end())
			{
				MergeInto(effect.effect, handlerEffect->second);
			}
			else
			{
				effect.effect.status = AnalysisStatus::Unknown;
				effect.effect.mayCallUnknown = true;
				effect.effect.mayHaveExternalEffect = true;
				AppendReason(
					effect.effect.reason,
					"relay region has an unresolved handler");
			}
			break;
		}
		if (!foundHandler)
		{
			effect.effect.status = AnalysisStatus::Unknown;
			effect.effect.mayCallUnknown = true;
			effect.effect.mayHaveExternalEffect = true;
			AppendReason(
				effect.effect.reason,
				"relay site has no matching handler record");
		}
		result.push_back(std::move(effect));
	}
	return result;
}

} // namespace cfg
} // namespace relay_protocol
} // namespace transpiler
