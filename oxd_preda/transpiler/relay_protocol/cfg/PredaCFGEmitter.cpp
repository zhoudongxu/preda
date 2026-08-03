#include "PredaCFGEmitter.h"

namespace transpiler {
namespace relay_protocol {
namespace cfg {
namespace {

using Json = nlohmann::ordered_json;

const char *StatusName(AnalysisStatus status)
{
	switch (status)
	{
	case AnalysisStatus::Complete: return "Complete";
	case AnalysisStatus::Conservative: return "Conservative";
	case AnalysisStatus::Unsupported: return "Unsupported";
	case AnalysisStatus::Unknown: return "Unknown";
	default: return "Unknown";
	}
}

const char *NodeKindName(PredaCFGNodeKind kind)
{
	switch (kind)
	{
	case PredaCFGNodeKind::Entry: return "Entry";
	case PredaCFGNodeKind::Exit: return "Exit";
	case PredaCFGNodeKind::Basic: return "Basic";
	case PredaCFGNodeKind::Branch: return "Branch";
	case PredaCFGNodeKind::LoopHeader: return "LoopHeader";
	case PredaCFGNodeKind::LoopLatch: return "LoopLatch";
	case PredaCFGNodeKind::Break: return "Break";
	case PredaCFGNodeKind::Continue: return "Continue";
	case PredaCFGNodeKind::Return: return "Return";
	case PredaCFGNodeKind::SynchronousCall: return "SynchronousCall";
	case PredaCFGNodeKind::RelayEmit: return "RelayEmit";
	case PredaCFGNodeKind::AbortOrFailure: return "AbortOrFailure";
	case PredaCFGNodeKind::Opaque: return "Opaque";
	default: return "Opaque";
	}
}

const char *EdgeKindName(PredaCFGEdgeKind kind)
{
	switch (kind)
	{
	case PredaCFGEdgeKind::Fallthrough: return "Fallthrough";
	case PredaCFGEdgeKind::TrueBranch: return "TrueBranch";
	case PredaCFGEdgeKind::FalseBranch: return "FalseBranch";
	case PredaCFGEdgeKind::LoopBack: return "LoopBack";
	case PredaCFGEdgeKind::BreakExit: return "BreakExit";
	case PredaCFGEdgeKind::ContinueBack: return "ContinueBack";
	case PredaCFGEdgeKind::ReturnExit: return "ReturnExit";
	case PredaCFGEdgeKind::CallToCallee: return "CallToCallee";
	case PredaCFGEdgeKind::ReturnToCaller: return "ReturnToCaller";
	case PredaCFGEdgeKind::ExceptionalOrFailure:
		return "ExceptionalOrFailure";
	case PredaCFGEdgeKind::Unknown: return "Unknown";
	default: return "Unknown";
	}
}

const char *CallKindName(PredaCallKind kind)
{
	switch (kind)
	{
	case PredaCallKind::Synchronous: return "Synchronous";
	case PredaCallKind::Relay: return "Relay";
	case PredaCallKind::ExternalUnknown: return "ExternalUnknown";
	case PredaCallKind::CompilerGeneratedHelper:
		return "CompilerGeneratedHelper";
	case PredaCallKind::RuntimeHelper: return "RuntimeHelper";
	default: return "ExternalUnknown";
	}
}

const char *RegionKindName(RegionKind kind)
{
	switch (kind)
	{
	case RegionKind::Function: return "Function";
	case RegionKind::BranchArm: return "BranchArm";
	case RegionKind::LoopBody: return "LoopBody";
	case RegionKind::SynchronousCallSite: return "SynchronousCallSite";
	case RegionKind::RelayHandler: return "RelayHandler";
	case RegionKind::RelayRegion: return "RelayRegion";
	default: return "Function";
	}
}

const char *InterproceduralKindName(InterproceduralEdgeKind kind)
{
	switch (kind)
	{
	case InterproceduralEdgeKind::SyncCall: return "SyncCall";
	case InterproceduralEdgeKind::SyncReturn: return "SyncReturn";
	case InterproceduralEdgeKind::AsyncRelaySpawn:
		return "AsyncRelaySpawn";
	default: return "SyncCall";
	}
}

const char *RelationStatusName(RelaySiteRelationStatus status)
{
	switch (status)
	{
	case RelaySiteRelationStatus::CoReachable: return "CoReachable";
	case RelaySiteRelationStatus::MutuallyExclusive:
		return "MutuallyExclusive";
	case RelaySiteRelationStatus::Unknown: return "Unknown";
	default: return "Unknown";
	}
}

const char *FormulaKindName(refinement::FormulaExprKind kind)
{
	switch (kind)
	{
	case refinement::FormulaExprKind::BoolLiteral: return "BoolLiteral";
	case refinement::FormulaExprKind::IntLiteral: return "IntLiteral";
	case refinement::FormulaExprKind::BitVectorLiteral:
		return "BitVectorLiteral";
	case refinement::FormulaExprKind::AddressLiteral: return "AddressLiteral";
	case refinement::FormulaExprKind::Symbol: return "Symbol";
	case refinement::FormulaExprKind::Group: return "Group";
	case refinement::FormulaExprKind::Unary: return "Unary";
	case refinement::FormulaExprKind::Binary: return "Binary";
	case refinement::FormulaExprKind::Nary: return "Nary";
	case refinement::FormulaExprKind::Cast: return "Cast";
	case refinement::FormulaExprKind::Ite: return "Ite";
	case refinement::FormulaExprKind::ArrayLength: return "ArrayLength";
	case refinement::FormulaExprKind::Unknown: return "Unknown";
	default: return "Unknown";
	}
}

const char *SortKindName(refinement::FormulaSortKind kind)
{
	switch (kind)
	{
	case refinement::FormulaSortKind::Bool: return "Bool";
	case refinement::FormulaSortKind::Int: return "Int";
	case refinement::FormulaSortKind::UnsignedBitVector:
		return "UnsignedBitVector";
	case refinement::FormulaSortKind::Address: return "Address";
	case refinement::FormulaSortKind::Unknown: return "Unknown";
	default: return "Unknown";
	}
}

Json EmitLocation(const SourceLocation &location)
{
	return Json{
		{"line", location.line},
		{"column", location.column},
		{"end_line", location.endLine},
		{"end_column", location.endColumn},
		{"start_offset", location.startOffset},
		{"end_offset", location.endOffset},
	};
}

Json EmitFormula(const refinement::FormulaExpr &formula)
{
	Json result = {
		{"kind", FormulaKindName(formula.kind)},
		{"sort", Json{
			{"kind", SortKindName(formula.sort.kind)},
			{"bit_width", formula.sort.bitWidth},
		}},
		{"text", formula.text},
		{"operator", formula.op},
		{"symbol_id", formula.symbolId},
		{"literal_value", formula.literalValue},
		{"location", EmitLocation(formula.location)},
		{"children", Json::array()},
	};
	for (const refinement::FormulaExpr &child : formula.children)
		result["children"].push_back(EmitFormula(child));
	if (!formula.unknownReason.empty())
		result["unknown_reason"] = formula.unknownReason;
	return result;
}

Json EmitEffect(const EffectSummary &effect)
{
	return Json{
		{"reads_current_scope_state", effect.readsCurrentScopeState},
		{"writes_current_scope_state", effect.writesCurrentScopeState},
		{"reads_global_state", effect.readsGlobalState},
		{"writes_global_state", effect.writesGlobalState},
		{"writes_local_state", effect.writesLocalState},
		{"modifies_loop_induction_variable",
			effect.modifiesLoopInductionVariable},
		{"may_emit_relay", effect.mayEmitRelay},
		{"may_call_unknown", effect.mayCallUnknown},
		{"may_return_early", effect.mayReturnEarly},
		{"may_break", effect.mayBreak},
		{"may_continue", effect.mayContinue},
		{"may_abort_or_fail", effect.mayAbortOrFail},
		{"may_have_external_effect", effect.mayHaveExternalEffect},
		{"read_state_variables", effect.readStateVariables},
		{"written_state_variables", effect.writtenStateVariables},
		{"status", StatusName(effect.status)},
		{"reason", effect.reason},
	};
}

Json EmitStringMap(
	const std::map<std::string, std::vector<std::string>> &values)
{
	Json result = Json::object();
	for (const auto &item : values)
		result[item.first] = item.second;
	return result;
}

Json EmitBoolMap(const std::map<std::string, bool> &values)
{
	Json result = Json::object();
	for (const auto &item : values)
		result[item.first] = item.second;
	return result;
}

} // namespace

Json PredaCFGEmitter::Emit(const PredaControlFlowIR &controlFlow)
{
	Json result = {
		{"extension_schema_version", 1},
		{"implemented_phases", Json::array({"A", "B", "C", "D"})},
	};
	result["functions"] = Json::array();
	for (const PredaFunctionCFG &function : controlFlow.functions)
	{
		Json item = {
			{"function_id", function.functionId},
			{"function", function.function},
			{"signature", function.signature},
			{"scope", static_cast<uint32_t>(function.scope)},
			{"generated_relay_lambda", function.generatedRelayLambda},
			{"location", EmitLocation(function.location)},
			{"entry_node_id", function.entryNodeId},
			{"exit_node_id", function.exitNodeId},
			{"status", StatusName(function.status)},
			{"completeness_reasons", function.completenessReasons},
			{"effect", EmitEffect(function.effect)},
			{"nodes", Json::array()},
			{"edges", Json::array()},
			{"regions", Json::array()},
		};
		for (const PredaCFGNode &node : function.nodes)
		{
			Json nodeJson = {
				{"id", node.id},
				{"kind", NodeKindName(node.kind)},
				{"source_function_id", node.sourceFunctionId},
				{"location", EmitLocation(node.location)},
				{"successors", node.successors},
				{"predecessors", node.predecessors},
				{"enclosing_loop_ids", node.enclosingLoopIds},
				{"supported", node.supported},
				{"unsupported_reason", node.unsupportedReason},
				{"direct_effect", EmitEffect(node.directEffect)},
			};
			if (!node.relaySiteId.empty())
				nodeJson["relay_site_id"] = node.relaySiteId;
			if (!node.calleeFunctionId.empty())
				nodeJson["callee_function_id"] = node.calleeFunctionId;
			if (node.hasCondition)
				nodeJson["condition"] = EmitFormula(node.condition);
			item["nodes"].push_back(std::move(nodeJson));
		}
		for (const PredaCFGEdge &edge : function.edges)
		{
			item["edges"].push_back(Json{
				{"id", edge.id},
				{"source", edge.source},
				{"target", edge.target},
				{"kind", EdgeKindName(edge.kind)},
				{"supported", edge.supported},
				{"reason", edge.reason},
			});
		}
		for (const PredaCFGRegion &region : function.regions)
		{
			item["regions"].push_back(Json{
				{"id", region.id},
				{"kind", RegionKindName(region.kind)},
				{"source_function_id", region.sourceFunctionId},
				{"location", EmitLocation(region.location)},
				{"node_ids", region.nodeIds},
				{"relay_site_id", region.relaySiteId},
				{"call_site_node_id", region.callSiteNodeId},
			});
		}
		result["functions"].push_back(std::move(item));
	}

	Json callGraph = {
		{"functions", controlFlow.callGraph.functions},
		{"edges", Json::array()},
		{"analysis", Json{
			{"strongly_connected_components",
				controlFlow.callGraph.analysis.stronglyConnectedComponents},
			{"recursive_functions",
				controlFlow.callGraph.analysis.recursiveFunctions},
			{"topological_components",
				controlFlow.callGraph.analysis.topologicalComponents},
			{"reverse_callers",
				EmitStringMap(controlFlow.callGraph.analysis.reverseCallers)},
			{"has_unresolved_outgoing",
				EmitBoolMap(controlFlow.callGraph.analysis.hasUnresolvedOutgoing)},
			{"relay_reachable",
				EmitBoolMap(controlFlow.callGraph.analysis.relayReachable)},
		}},
	};
	for (const PredaCallEdge &edge : controlFlow.callGraph.edges)
	{
		callGraph["edges"].push_back(Json{
			{"id", edge.id},
			{"caller", edge.caller},
			{"callee", edge.callee},
			{"kind", CallKindName(edge.kind)},
			{"call_site", EmitLocation(edge.callSite)},
			{"source_text", edge.sourceText},
			{"resolved", edge.resolved},
			{"callee_is_const", edge.calleeIsConst},
			{"unresolved_reason", edge.unresolvedReason},
		});
	}
	result["synchronous_call_graph"] = std::move(callGraph);

	result["region_effects"] = Json::array();
	for (const PredaRegionEffect &region : controlFlow.regionEffects)
	{
		result["region_effects"].push_back(Json{
			{"region_id", region.regionId},
			{"kind", RegionKindName(region.kind)},
			{"source_function_id", region.sourceFunctionId},
			{"relay_site_id", region.relaySiteId},
			{"call_site_node_id", region.callSiteNodeId},
			{"node_ids", region.nodeIds},
			{"effect", EmitEffect(region.effect)},
		});
	}

	Json icfg = {
		{"interprocedural_edges", Json::array()},
		{"function_analyses", Json::array()},
		{"synchronous_composition_analyses", Json::array()},
		{"relay_site_relations", Json::array()},
		{"synchronous_reachable_functions",
			EmitStringMap(
				controlFlow.relayIcfg.synchronousReachableFunctions)},
		{"async_reachable_handlers",
			EmitStringMap(controlFlow.relayIcfg.asyncReachableHandlers)},
		{"synchronous_sccs", controlFlow.relayIcfg.synchronousSccs},
	};
	for (const PredaInterproceduralEdge &edge :
		controlFlow.relayIcfg.interproceduralEdges)
	{
		icfg["interprocedural_edges"].push_back(Json{
			{"id", edge.id},
			{"kind", InterproceduralKindName(edge.kind)},
			{"source_function_id", edge.sourceFunctionId},
			{"source_node_id", edge.sourceNodeId},
			{"target_function_id", edge.targetFunctionId},
			{"target_node_id", edge.targetNodeId},
			{"relay_site_id", edge.relaySiteId},
			{"resolved", edge.resolved},
			{"reason", edge.reason},
		});
	}
	for (const PredaFunctionGraphAnalysis &analysis :
		controlFlow.relayIcfg.functionAnalyses)
	{
		icfg["function_analyses"].push_back(Json{
			{"function_id", analysis.functionId},
			{"status", StatusName(analysis.status)},
			{"completeness_reasons", analysis.completenessReasons},
			{"dominance_complete", analysis.dominanceComplete},
			{"post_dominance_complete", analysis.postDominanceComplete},
			{"reachability_complete", analysis.reachabilityComplete},
			{"dominators", EmitStringMap(analysis.dominators)},
			{"post_dominators", EmitStringMap(analysis.postDominators)},
			{"control_dependents",
				EmitStringMap(analysis.controlDependents)},
			{"reachable_nodes", EmitStringMap(analysis.reachableNodes)},
			{"loop_nesting", EmitStringMap(analysis.loopNesting)},
		});
	}
	for (const PredaFunctionGraphAnalysis &analysis :
		controlFlow.relayIcfg.synchronousCompositionAnalyses)
	{
		icfg["synchronous_composition_analyses"].push_back(Json{
			{"root_function_id", analysis.functionId},
			{"status", StatusName(analysis.status)},
			{"completeness_reasons", analysis.completenessReasons},
			{"dominance_complete", analysis.dominanceComplete},
			{"post_dominance_complete", analysis.postDominanceComplete},
			{"reachability_complete", analysis.reachabilityComplete},
			{"dominators", EmitStringMap(analysis.dominators)},
			{"post_dominators", EmitStringMap(analysis.postDominators)},
			{"control_dependents",
				EmitStringMap(analysis.controlDependents)},
			{"reachable_nodes", EmitStringMap(analysis.reachableNodes)},
			{"loop_nesting", EmitStringMap(analysis.loopNesting)},
		});
	}
	for (const RelaySiteRelation &relation :
		controlFlow.relayIcfg.relaySiteRelations)
	{
		icfg["relay_site_relations"].push_back(Json{
			{"first_relay_site_id", relation.firstRelaySiteId},
			{"second_relay_site_id", relation.secondRelaySiteId},
			{"status", RelationStatusName(relation.status)},
			{"reason", relation.reason},
		});
	}
	result["relay_icfg"] = std::move(icfg);
	return result;
}

} // namespace cfg
} // namespace relay_protocol
} // namespace transpiler
