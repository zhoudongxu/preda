#include "RelayManifestEmitter.h"
#include "certificate/ParallelRelayCertificateEmitter.h"
#include "analysis/RelaySummaryEmitter.h"
#include "cfg/PredaCFGEmitter.h"
#include "metrics/RelayAnalysisMetrics.h"
#include "refinement/RelayRefinementEmitter.h"

#include "../../3rdParty/nlohmann/json.hpp"

namespace transpiler {
namespace relay_protocol {
namespace {

using Json = nlohmann::ordered_json;

const char *ScopeName(ScopeType scope)
{
	switch (scope)
	{
	case ScopeType::None: return "none";
	case ScopeType::Global: return "global";
	case ScopeType::Shard: return "shard";
	case ScopeType::Address: return "address";
	case ScopeType::Uint32: return "uint32";
	case ScopeType::Uint64: return "uint64";
	case ScopeType::Uint96: return "uint96";
	case ScopeType::Uint128: return "uint128";
	case ScopeType::Uint160: return "uint160";
	case ScopeType::Uint256: return "uint256";
	case ScopeType::Uint512: return "uint512";
	default: return "unknown";
	}
}

const char *RelayKindName(RelayKind kind)
{
	switch (kind)
	{
	case RelayKind::CustomScope: return "custom_scope";
	case RelayKind::Global: return "global";
	case RelayKind::Shards: return "shards";
	case RelayKind::Next: return "next";
	default: return "unknown";
	}
}

const char *HandlerKindName(RelayHandlerKind kind)
{
	switch (kind)
	{
	case RelayHandlerKind::Named: return "named";
	case RelayHandlerKind::Lambda: return "lambda";
	default: return "unknown";
	}
}

const char *ExpressionKindName(RelayExprKind kind)
{
	switch (kind)
	{
	case RelayExprKind::Identifier: return "identifier";
	case RelayExprKind::Literal: return "literal";
	case RelayExprKind::Keyword: return "keyword";
	case RelayExprKind::MemberAccess: return "member_access";
	case RelayExprKind::Index: return "index";
	case RelayExprKind::Unary: return "unary";
	case RelayExprKind::Binary: return "binary";
	case RelayExprKind::Call: return "call";
	case RelayExprKind::Group: return "group";
	case RelayExprKind::Opaque: return "opaque";
	default: return "opaque";
	}
}

const char *DependencyClassName(
	analysis::RelayDependencyClass dependencyClass)
{
	switch (dependencyClass)
	{
	case analysis::RelayDependencyClass::Constant:
		return "Constant";
	case analysis::RelayDependencyClass::TransactionArgument:
		return "TransactionArgument";
	case analysis::RelayDependencyClass::CurrentScopeKey:
		return "CurrentScopeKey";
	case analysis::RelayDependencyClass::CurrentScopeState:
		return "CurrentScopeState";
	case analysis::RelayDependencyClass::LocalDerived:
		return "LocalDerived";
	case analysis::RelayDependencyClass::LoopVariable:
		return "LoopVariable";
	case analysis::RelayDependencyClass::ExternalCallResult:
		return "ExternalCallResult";
	case analysis::RelayDependencyClass::Opaque:
	default:
		return "Opaque";
	}
}

const char *AvailabilityStageName(
	analysis::RelayAvailabilityStage stage)
{
	switch (stage)
	{
	case analysis::RelayAvailabilityStage::CompileTime:
		return "CompileTime";
	case analysis::RelayAvailabilityStage::AdmissionTime:
		return "AdmissionTime";
	case analysis::RelayAvailabilityStage::AfterScopeLoad:
		return "AfterScopeLoad";
	case analysis::RelayAvailabilityStage::DuringExecution:
		return "DuringExecution";
	case analysis::RelayAvailabilityStage::Unknown:
	default:
		return "Unknown";
	}
}

const char *ProtocolNodeKindName(ProtocolNodeKind kind)
{
	switch (kind)
	{
	case ProtocolNodeKind::End: return "End";
	case ProtocolNodeKind::Emit: return "Emit";
	case ProtocolNodeKind::Branch: return "Branch";
	case ProtocolNodeKind::Sequence: return "Sequence";
	case ProtocolNodeKind::Parallel: return "Parallel";
	case ProtocolNodeKind::Repeat: return "Repeat";
	case ProtocolNodeKind::Call: return "Call";
	case ProtocolNodeKind::Opaque: return "Opaque";
	default: return "Opaque";
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

Json EmitExpression(const RelayExprIR &expression)
{
	Json result = {
		{"kind", ExpressionKindName(expression.kind)},
		{"text", expression.text},
		{"type", expression.type},
		{"operator", expression.op},
		{"location", EmitLocation(expression.location)},
	};
	result["children"] = Json::array();
	for (const RelayExprIR &child : expression.children)
		result["children"].push_back(EmitExpression(child));
	if (expression.kind == RelayExprKind::Opaque || !expression.opaqueReason.empty())
		result["opaque_reason"] = expression.opaqueReason;
	return result;
}

Json EmitDependency(
	const analysis::RelayExpressionDependency &dependency)
{
	Json result = {
		{"dependencies", Json::array()},
		{"earliest_availability",
			AvailabilityStageName(dependency.earliestAvailability)},
		{"admission_time_evaluable",
			dependency.admissionTimeEvaluable},
	};
	for (analysis::RelayDependencyClass dependencyClass :
		dependency.classes)
	{
		result["dependencies"].push_back(
			DependencyClassName(dependencyClass));
	}
	if (!dependency.reason.empty())
		result["reason"] = dependency.reason;
	return result;
}

Json EmitBranch(const BranchCondition &branch)
{
	return Json{
		{"condition", EmitExpression(branch.condition)},
		{"polarity", branch.polarity},
		{"arm", branch.arm},
		{"location", EmitLocation(branch.location)},
	};
}

Json EmitLoop(const LoopProtocol &loop)
{
	return Json{
		{"kind", loop.kind},
		{"location", EmitLocation(loop.location)},
		{"initializer", EmitExpression(loop.initializer)},
		{"condition", EmitExpression(loop.condition)},
		{"update", EmitExpression(loop.update)},
		{"statically_bounded", loop.staticallyBounded},
		{"induction_variable", loop.inductionVariable},
		{"initial_value", loop.initialValue},
		{"comparison", loop.comparison},
		{"bound_value", loop.boundValue},
		{"step", loop.step},
		{"opaque_reason", loop.opaqueReason},
		{"body_may_exit_early", loop.bodyMayExitEarly},
	};
}

Json EmitNode(const ProtocolNode &node)
{
	Json result = {
		{"kind", ProtocolNodeKindName(node.kind)},
		{"location", EmitLocation(node.location)},
	};
	if (!node.relaySiteId.empty())
		result["relay_site_id"] = node.relaySiteId;
	if (!node.callee.empty())
		result["callee"] = node.callee;
	if (node.kind == ProtocolNodeKind::Branch)
	{
		result["condition"] = EmitExpression(node.condition);
		result["polarity"] = node.conditionPolarity;
	}
	if (node.kind == ProtocolNodeKind::Repeat)
		result["loop"] = EmitLoop(node.loop);
	if (node.kind == ProtocolNodeKind::Opaque || !node.opaqueReason.empty())
		result["opaque_reason"] = node.opaqueReason;
	result["children"] = Json::array();
	for (const ProtocolNode &child : node.children)
		result["children"].push_back(EmitNode(child));
	return result;
}

} // namespace

std::string RelayManifestEmitter::Emit(const RelayProtocolIR &protocol)
{
	metrics::ScopedRelayAnalysisPhase timer(
		metrics::RelayAnalysisPhase::ManifestEmission);
	Json root = {
		{"schema_version", protocol.schemaVersion},
		{"dapp", protocol.dapp},
		{"contract", protocol.contract},
	};

	root["relay_sites"] = Json::array();
	for (const RelaySite &site : protocol.relaySites)
	{
		Json item = {
			{"id", site.id},
			{"source_contract", site.sourceContract},
			{"source_function", site.sourceFunction},
			{"source_function_id", site.sourceFunctionId},
			{"source_function_signature", site.sourceFunctionSignature},
			{"source_function_overload_index", site.sourceFunctionOverloadIndex},
			{"source_scope", ScopeName(site.sourceScope)},
			{"location", EmitLocation(site.location)},
			{"relay_kind", RelayKindName(site.relayKind)},
			{"target", EmitExpression(site.target)},
			{"target_dependency",
				EmitDependency(site.targetDependency)},
			{"target_scope", ScopeName(site.targetScope)},
			{"target_function", site.targetFunction},
			{"handler_id", site.handlerId},
		};
#ifdef RPREDA_ENABLE_BOUND_RELAY_MANIFEST
		item["ordinal"] = site.ordinal;
#endif
		item["arguments"] = Json::array();
		for (const RelayArgument &argument : site.arguments)
		{
			item["arguments"].push_back(Json{
				{"type", argument.type},
				{"expression", EmitExpression(argument.expression)},
				{"dependency",
					EmitDependency(argument.dependency)},
			});
		}
		item["branches"] = Json::array();
		for (const BranchCondition &branch : site.branches)
			item["branches"].push_back(EmitBranch(branch));
		item["loops"] = Json::array();
		for (const LoopProtocol &loop : site.loops)
			item["loops"].push_back(EmitLoop(loop));
		root["relay_sites"].push_back(std::move(item));
	}

	root["handlers"] = Json::array();
	for (const RelayHandler &handler : protocol.handlers)
	{
		root["handlers"].push_back(Json{
			{"id", handler.id},
			{"kind", HandlerKindName(handler.kind)},
			{"contract", handler.contract},
			{"name", handler.name},
			{"target_function_id", handler.targetFunctionId},
			{"target_function_signature", handler.targetFunctionSignature},
			{"target_function_overload_index",
				handler.targetFunctionOverloadIndex},
			{"scope", ScopeName(handler.scope)},
			{"opcode", handler.opcode},
			{"resolved", handler.resolved},
			{"relay_reachability_known",
				handler.relayReachabilityKnown},
			{"may_emit_relay", handler.mayEmitRelay},
			{"location", EmitLocation(handler.location)},
			{"parameter_types", handler.parameterTypes},
		});
	}

	root["edges"] = Json::array();
	for (const RelayProtocolEdge &edge : protocol.edges)
	{
		root["edges"].push_back(Json{
			{"id", edge.id},
			{"source_function", edge.sourceFunction},
			{"source_function_id", edge.sourceFunctionId},
			{"source_function_signature", edge.sourceFunctionSignature},
			{"source_function_overload_index", edge.sourceFunctionOverloadIndex},
			{"relay_site_id", edge.relaySiteId},
			{"handler_id", edge.handlerId},
			{"resolved", edge.resolved},
		});
	}

	root["functions"] = Json::array();
	for (const FunctionProtocol &function : protocol.functions)
	{
		Json item = {
			{"contract", function.contract},
			{"function", function.function},
			{"source_function_id", function.sourceFunctionId},
			{"source_function_signature", function.sourceFunctionSignature},
			{"source_function_overload_index", function.sourceFunctionOverloadIndex},
			{"scope", ScopeName(function.scope)},
			{"relay_site_ids", function.relaySiteIds},
			{"root", EmitNode(function.root)},
		};
#ifdef RPREDA_ENABLE_BOUND_RELAY_MANIFEST
		item["exported_opcode"] = function.exportedOpcode;
#endif
		item["summary"] =
			analysis::RelaySummaryEmitter::Emit(function.summary);
		root["functions"].push_back(std::move(item));
	}
	root["refinement"] =
		refinement::RelayRefinementEmitter::Emit(
			protocol.refinementSymbols,
			protocol.refinementConstraints,
			protocol.refinementProofObligations);
	// Keep schema v4/v5 compatibility during the Phase A-D checkpoint.  The
	// strict runtime loader consumes schema v5 and ignores this additive,
	// compiler-only section.
	root["control_flow"] = cfg::PredaCFGEmitter::Emit(
		protocol.controlFlow);
	root["parallel_certificate"] =
		certificate::ParallelRelayCertificateEmitter::Emit(
			protocol.parallelCertificate);
	return root.dump(2);
}

} // namespace relay_protocol
} // namespace transpiler
