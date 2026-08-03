#include "ParallelRelayCertificateEmitter.h"

#include <algorithm>

namespace transpiler {
namespace relay_protocol {
namespace certificate {
namespace {

using Json = nlohmann::ordered_json;

const char *RelationName(RelayPairRelation relation)
{
	switch (relation)
	{
	case RelayPairRelation::MutuallyExclusive: return "MutuallyExclusive";
	case RelayPairRelation::MustPrecedeAB: return "MustPrecedeAB";
	case RelayPairRelation::MustPrecedeBA: return "MustPrecedeBA";
	case RelayPairRelation::CoEmissionIndependent:
		return "CoEmissionIndependent";
	case RelayPairRelation::ProvedMayAlias: return "ProvedMayAlias";
	case RelayPairRelation::PotentialConflict: return "PotentialConflict";
	case RelayPairRelation::Unknown:
	default: return "Unknown";
	}
}

const char *StatusName(CertificateStatus status)
{
	switch (status)
	{
	case CertificateStatus::Complete: return "Complete";
	case CertificateStatus::Conservative: return "Conservative";
	case CertificateStatus::Unsupported: return "Unsupported";
	case CertificateStatus::Unknown:
	default: return "Unknown";
	}
}

const char *PropertyStatusName(CertificatePropertyStatus status)
{
	switch (status)
	{
	case CertificatePropertyStatus::Proved: return "Proved";
	case CertificatePropertyStatus::Conservative: return "Conservative";
	case CertificatePropertyStatus::Unsupported: return "Unsupported";
	case CertificatePropertyStatus::Unknown:
	default: return "Unknown";
	}
}

const char *PhysicalKindName(PhysicalWorkBoundKind kind)
{
	switch (kind)
	{
	case PhysicalWorkBoundKind::Constant: return "Constant";
	case PhysicalWorkBoundKind::ParameterizedUpperBound:
		return "ParameterizedUpperBound";
	case PhysicalWorkBoundKind::Unknown:
	default: return "Unknown";
	}
}

const char *SolverStatusName(
	refinement::solver::RelaySolverStatus status)
{
	using refinement::solver::RelaySolverStatus;
	switch (status)
	{
	case RelaySolverStatus::NotRun: return "NotRun";
	case RelaySolverStatus::EstablishedByConstruction:
		return "EstablishedByConstruction";
	case RelaySolverStatus::Proved: return "Proved";
	case RelaySolverStatus::Disproved: return "Disproved";
	case RelaySolverStatus::Unknown: return "Unknown";
	case RelaySolverStatus::Unsupported: return "Unsupported";
	case RelaySolverStatus::InconsistentAssumptions:
		return "InconsistentAssumptions";
	case RelaySolverStatus::EncodingError:
	default: return "EncodingError";
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

Json EmitSort(const refinement::FormulaSort &sort)
{
	const char *kind = "Unknown";
	switch (sort.kind)
	{
	case refinement::FormulaSortKind::Bool: kind = "Bool"; break;
	case refinement::FormulaSortKind::Int: kind = "Int"; break;
	case refinement::FormulaSortKind::UnsignedBitVector:
		kind = "UnsignedBitVector";
		break;
	case refinement::FormulaSortKind::Address: kind = "Address"; break;
	case refinement::FormulaSortKind::Unknown: break;
	}
	return Json{{"kind", kind}, {"bit_width", sort.bitWidth}};
}

Json EmitCounterexample(
	const std::vector<refinement::solver::RelayCounterexampleValue> &values)
{
	Json result = Json::array();
	for (const auto &value : values)
	{
		result.push_back(Json{
			{"symbol_id", value.symbolId},
			{"sort", EmitSort(value.sort)},
			{"value", value.value},
		});
	}
	return result;
}

Json EmitSolverResult(
	const refinement::solver::RelaySolverResult &result)
{
	return Json{
		{"backend", result.backend},
		{"status", SolverStatusName(result.status)},
		{"elapsed_time_ms", result.elapsedTimeMs},
		{"assumption_constraint_ids", result.assumptionConstraintIds},
		{"reason", result.reason},
		{"projected_counterexample",
			EmitCounterexample(result.projectedCounterexample)},
	};
}

const char *CardinalityKindName(analysis::RelayCardinalityExprKind kind)
{
	switch (kind)
	{
	case analysis::RelayCardinalityExprKind::Constant: return "constant";
	case analysis::RelayCardinalityExprKind::Sum: return "sum";
	case analysis::RelayCardinalityExprKind::Product: return "product";
	case analysis::RelayCardinalityExprKind::Ite: return "ite";
	case analysis::RelayCardinalityExprKind::Unknown:
	default: return "unknown";
	}
}

Json EmitSourceExpression(const RelayExprIR &expression)
{
	Json result = {
		{"text", expression.text},
		{"type", expression.type},
		{"operator", expression.op},
		{"location", EmitLocation(expression.location)},
	};
	result["children"] = Json::array();
	for (const RelayExprIR &child : expression.children)
		result["children"].push_back(EmitSourceExpression(child));
	return result;
}

Json EmitCardinality(const analysis::RelayCardinalityExpr &expression)
{
	Json result = {{"kind", CardinalityKindName(expression.kind)}};
	if (expression.kind == analysis::RelayCardinalityExprKind::Constant)
		result["value"] = expression.value;
	else if (expression.kind == analysis::RelayCardinalityExprKind::Unknown)
		result["reason"] = expression.reason;
	else
	{
		if (expression.kind == analysis::RelayCardinalityExprKind::Ite)
			result["predicate"] = EmitSourceExpression(expression.predicate);
		result["children"] = Json::array();
		for (const analysis::RelayCardinalityExpr &child : expression.children)
			result["children"].push_back(EmitCardinality(child));
	}
	return result;
}

const char *DepthKindName(analysis::RelayDepthExprKind kind)
{
	switch (kind)
	{
	case analysis::RelayDepthExprKind::Constant: return "constant";
	case analysis::RelayDepthExprKind::Maximum: return "maximum";
	case analysis::RelayDepthExprKind::Successor: return "successor";
	case analysis::RelayDepthExprKind::Unknown:
	default: return "unknown";
	}
}

Json EmitDepth(const analysis::RelayDepthExpr &expression)
{
	Json result = {{"kind", DepthKindName(expression.kind)}};
	if (expression.kind == analysis::RelayDepthExprKind::Constant)
		result["value"] = expression.value;
	else if (expression.kind == analysis::RelayDepthExprKind::Unknown)
		result["reason"] = expression.reason;
	else
	{
		result["children"] = Json::array();
		for (const analysis::RelayDepthExpr &child : expression.children)
			result["children"].push_back(EmitDepth(child));
	}
	return result;
}

Json EmitLogicalBound(const RelayLogicalWorkBound &bound)
{
	return Json{
		{"certificate_id", bound.id},
		{"status", StatusName(bound.status)},
		{"exact", EmitCardinality(bound.exact)},
		{"upper_bound", EmitCardinality(bound.upperBound)},
		{"reason", bound.reason},
		{"supporting_cfg_fact_ids", bound.supportingCfgFactIds},
		{"supporting_constraint_ids", bound.supportingConstraintIds},
		{"supporting_solver_result_ids", bound.supportingSolverResultIds},
	};
}

Json EmitPhysicalBound(const RelayPhysicalWorkBound &bound)
{
	return Json{
		{"certificate_id", bound.id},
		{"status", StatusName(bound.status)},
		{"bound_kind", PhysicalKindName(bound.boundKind)},
		{"constant_term", bound.constantTerm},
		{"active_shard_count_coefficient",
			bound.activeShardCountCoefficient},
		{"expression", bound.expression},
		{"reason", bound.reason},
		{"supporting_cfg_fact_ids", bound.supportingCfgFactIds},
		{"supporting_constraint_ids", bound.supportingConstraintIds},
		{"supporting_solver_result_ids", bound.supportingSolverResultIds},
	};
}

Json EmitDepthBound(const RelayTreeDepthBound &bound)
{
	return Json{
		{"certificate_id", bound.id},
		{"status", StatusName(bound.status)},
		{"exact", EmitDepth(bound.exact)},
		{"upper_bound", EmitDepth(bound.upperBound)},
		{"reason", bound.reason},
		{"supporting_cfg_fact_ids", bound.supportingCfgFactIds},
		{"supporting_constraint_ids", bound.supportingConstraintIds},
		{"supporting_solver_result_ids", bound.supportingSolverResultIds},
	};
}

Json EmitPair(const RelayPairCertificate &pair)
{
	return Json{
		{"certificate_id", pair.id},
		{"source_function_id", pair.sourceFunctionId},
		{"site_a", pair.siteA},
		{"site_b", pair.siteB},
		{"relation", RelationName(pair.relation)},
		{"status", PropertyStatusName(pair.status)},
		{"reason", pair.reason},
		{"supporting_cfg_fact_ids", pair.supportingCfgFactIds},
		{"supporting_constraint_ids", pair.supportingConstraintIds},
		{"supporting_solver_result_ids", pair.supportingSolverResultIds},
		{"solver_results", Json{
			{"co_emission", EmitSolverResult(pair.feasibilityResult)},
			{"relation", EmitSolverResult(pair.relationResult)},
		}},
		{"counterexample", EmitCounterexample(pair.counterexample)},
		{"location", Json{
			{"site_a", EmitLocation(pair.siteALocation)},
			{"site_b", EmitLocation(pair.siteBLocation)},
		}},
		{"source_locations", Json::array({
			Json{{"relay_site_id", pair.siteA},
				 {"location", EmitLocation(pair.siteALocation)}},
			Json{{"relay_site_id", pair.siteB},
				 {"location", EmitLocation(pair.siteBLocation)}},
		})},
	};
}

} // namespace

nlohmann::ordered_json ParallelRelayCertificateEmitter::Emit(
	const ParallelRelayCertificate &certificate)
{
	Json result = {
		{"extension_schema_version", certificate.extensionSchemaVersion},
		{"functions", Json::array()},
	};
	for (const FunctionParallelRelayCertificate &function :
		certificate.functions)
	{
		Json item = {
			{"source_function_id", function.sourceFunctionId},
			{"certificate_status", StatusName(function.status)},
			{"reason", function.reason},
			{"pair_relations", Json::array()},
		};
		for (const RelayPairCertificate &pair : function.pairRelations)
			item["pair_relations"].push_back(EmitPair(pair));
		item["direct_work_bound"] = EmitLogicalBound(function.directWorkBound);
		item["transitive_work_bound"] =
			EmitLogicalBound(function.transitiveWorkBound);
		item["depth_bound"] = EmitDepthBound(function.depthBound);
		// Stable aliases used by the runtime-validation extension.
		item["direct_logical_work"] = item["direct_work_bound"];
		item["transitive_logical_work"] = item["transitive_work_bound"];
		item["relay_tree_depth"] = item["depth_bound"];
		item["physical_route_work"] =
			EmitPhysicalBound(function.physicalRouteWork);
		result["functions"].push_back(std::move(item));
	}
	return result;
}

} // namespace certificate
} // namespace relay_protocol
} // namespace transpiler
