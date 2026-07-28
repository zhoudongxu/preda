#include "RelaySummaryEmitter.h"

#include <algorithm>
#include <utility>

namespace transpiler {
namespace relay_protocol {
namespace analysis {
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

const char *CardinalityKindName(RelayCardinalityExprKind kind)
{
	switch (kind)
	{
	case RelayCardinalityExprKind::Constant: return "constant";
	case RelayCardinalityExprKind::Sum: return "sum";
	case RelayCardinalityExprKind::Product: return "product";
	case RelayCardinalityExprKind::Ite: return "ite";
	case RelayCardinalityExprKind::Unknown: return "unknown";
	default: return "unknown";
	}
}

const char *DepthKindName(RelayDepthExprKind kind)
{
	switch (kind)
	{
	case RelayDepthExprKind::Constant: return "constant";
	case RelayDepthExprKind::Maximum: return "maximum";
	case RelayDepthExprKind::Successor: return "successor";
	case RelayDepthExprKind::Unknown: return "unknown";
	default: return "unknown";
	}
}

const char *FanoutKindName(RelayFanoutKind kind)
{
	switch (kind)
	{
	case RelayFanoutKind::SingleTarget: return "single_target";
	case RelayFanoutKind::AllShards: return "all_shards";
	default: return "unknown";
	}
}

const char *OrderingStatusName(RelayOrderingStatus status)
{
	switch (status)
	{
	case RelayOrderingStatus::Trivial: return "trivial";
	case RelayOrderingStatus::Partial: return "partial";
	case RelayOrderingStatus::Unknown: return "unknown";
	default: return "unknown";
	}
}

const char *AnalysisStatusName(RelayAnalysisStatus status)
{
	switch (status)
	{
	case RelayAnalysisStatus::Exact: return "exact";
	case RelayAnalysisStatus::Conservative: return "conservative";
	case RelayAnalysisStatus::Unknown: return "unknown";
	default: return "unknown";
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
	if (expression.kind == RelayExprKind::Opaque ||
		!expression.opaqueReason.empty())
	{
		result["opaque_reason"] = expression.opaqueReason;
	}
	return result;
}

Json EmitCardinality(const RelayCardinalityExpr &expression)
{
	Json result = {
		{"kind", CardinalityKindName(expression.kind)},
	};
	switch (expression.kind)
	{
	case RelayCardinalityExprKind::Constant:
		result["value"] = expression.value;
		break;
	case RelayCardinalityExprKind::Sum:
	case RelayCardinalityExprKind::Product:
		result["children"] = Json::array();
		for (const RelayCardinalityExpr &child : expression.children)
			result["children"].push_back(EmitCardinality(child));
		break;
	case RelayCardinalityExprKind::Ite:
		result["predicate"] = EmitExpression(expression.predicate);
		result["children"] = Json::array();
		for (const RelayCardinalityExpr &child : expression.children)
			result["children"].push_back(EmitCardinality(child));
		break;
	case RelayCardinalityExprKind::Unknown:
		result["reason"] = expression.reason;
		break;
	}
	return result;
}

Json EmitDepth(const RelayDepthExpr &expression)
{
	Json result = {
		{"kind", DepthKindName(expression.kind)},
	};
	switch (expression.kind)
	{
	case RelayDepthExprKind::Constant:
		result["value"] = expression.value;
		break;
	case RelayDepthExprKind::Maximum:
	case RelayDepthExprKind::Successor:
		result["children"] = Json::array();
		for (const RelayDepthExpr &child : expression.children)
			result["children"].push_back(EmitDepth(child));
		break;
	case RelayDepthExprKind::Unknown:
		result["reason"] = expression.reason;
		break;
	}
	return result;
}

template <typename Value>
void SortAndUnique(std::vector<Value> &values)
{
	std::sort(values.begin(), values.end());
	values.erase(std::unique(values.begin(), values.end()), values.end());
}

Json EmitOrdering(const RelayOrderingSummary &ordering)
{
	std::vector<RelayOrderingConstraint> constraints =
		ordering.mustPrecede;
	std::sort(
		constraints.begin(),
		constraints.end(),
		[](const RelayOrderingConstraint &left,
			const RelayOrderingConstraint &right) {
			if (left.beforeRelaySiteId != right.beforeRelaySiteId)
			{
				return left.beforeRelaySiteId <
					right.beforeRelaySiteId;
			}
			if (left.afterRelaySiteId != right.afterRelaySiteId)
			{
				return left.afterRelaySiteId <
					right.afterRelaySiteId;
			}
			return left.reason < right.reason;
		});
	constraints.erase(
		std::unique(
			constraints.begin(),
			constraints.end(),
			[](const RelayOrderingConstraint &left,
				const RelayOrderingConstraint &right) {
				return left.beforeRelaySiteId ==
						right.beforeRelaySiteId &&
					left.afterRelaySiteId ==
						right.afterRelaySiteId &&
					left.reason == right.reason;
			}),
		constraints.end());

	Json result = {
		{"status", OrderingStatusName(ordering.status)},
	};
	result["must_precede"] = Json::array();
	for (const RelayOrderingConstraint &constraint : constraints)
	{
		result["must_precede"].push_back(Json{
			{"before_relay_site_id", constraint.beforeRelaySiteId},
			{"after_relay_site_id", constraint.afterRelaySiteId},
			{"reason", constraint.reason},
		});
	}
	result["listener_order_used_as_proof"] = false;
	result["reason"] = ordering.reason;
	return result;
}

} // namespace

nlohmann::ordered_json RelaySummaryEmitter::Emit(
	const RelayProtocolSummary &summary)
{
	std::vector<std::string> relaySiteIds = summary.relaySiteIds;
	SortAndUnique(relaySiteIds);

	std::vector<std::string> scopeNames;
	scopeNames.reserve(summary.targetScopeKinds.size());
	for (ScopeType scope : summary.targetScopeKinds)
		scopeNames.push_back(ScopeName(scope));
	SortAndUnique(scopeNames);

	std::vector<std::string> fanoutNames;
	fanoutNames.reserve(summary.fanoutKinds.size());
	for (RelayFanoutKind fanout : summary.fanoutKinds)
		fanoutNames.push_back(FanoutKindName(fanout));
	SortAndUnique(fanoutNames);

	return Json{
		{"relay_count", EmitCardinality(summary.relayCount)},
		{"relay_count_upper_bound",
			EmitCardinality(summary.relayCountUpperBound)},
		{"max_depth", EmitDepth(summary.maxDepth)},
		{"relay_site_set", std::move(relaySiteIds)},
		{"target_scope_kinds", std::move(scopeNames)},
		{"targets_known_before_execution",
			summary.targetsKnownBeforeExecution},
		{"has_opaque", summary.hasOpaque},
		{"has_unmodeled_relay_reachable_call",
			summary.hasUnmodeledRelayReachableCall},
		{"fanout", std::move(fanoutNames)},
		{"ordering", EmitOrdering(summary.ordering)},
		{"analysis_status",
			AnalysisStatusName(summary.analysisStatus)},
	};
}

} // namespace analysis
} // namespace relay_protocol
} // namespace transpiler
