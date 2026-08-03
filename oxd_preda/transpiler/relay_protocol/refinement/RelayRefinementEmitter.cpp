#include "RelayRefinementEmitter.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace transpiler {
namespace relay_protocol {
namespace refinement {
namespace {

using Json = nlohmann::ordered_json;

const char *FormulaSortKindName(FormulaSortKind kind)
{
	switch (kind)
	{
	case FormulaSortKind::Bool: return "Bool";
	case FormulaSortKind::Int: return "Int";
	case FormulaSortKind::UnsignedBitVector:
		return "UnsignedBitVector";
	case FormulaSortKind::Address: return "Address";
	case FormulaSortKind::Unknown:
	default:
		return "Unknown";
	}
}

const char *FormulaExprKindName(FormulaExprKind kind)
{
	switch (kind)
	{
	case FormulaExprKind::BoolLiteral: return "BoolLiteral";
	case FormulaExprKind::IntLiteral: return "IntLiteral";
	case FormulaExprKind::BitVectorLiteral: return "BitVectorLiteral";
	case FormulaExprKind::AddressLiteral: return "AddressLiteral";
	case FormulaExprKind::Symbol: return "Symbol";
	case FormulaExprKind::Group: return "Group";
	case FormulaExprKind::Unary: return "Unary";
	case FormulaExprKind::Binary: return "Binary";
	case FormulaExprKind::Nary: return "Nary";
	case FormulaExprKind::Cast: return "Cast";
	case FormulaExprKind::Ite: return "Ite";
	case FormulaExprKind::ArrayLength: return "ArrayLength";
	case FormulaExprKind::Unknown:
	default:
		return "Unknown";
	}
}

const char *SymbolKindName(RelayRefinementSymbolKind kind)
{
	switch (kind)
	{
	case RelayRefinementSymbolKind::SourceFunctionParameter:
		return "SourceFunctionParameter";
	case RelayRefinementSymbolKind::CurrentScopeKey:
		return "CurrentScopeKey";
	case RelayRefinementSymbolKind::PreStateVariable:
		return "PreStateVariable";
	case RelayRefinementSymbolKind::LoopVariable:
		return "LoopVariable";
	case RelayRefinementSymbolKind::RelayEmission:
		return "RelayEmission";
	case RelayRefinementSymbolKind::ActualRelayTarget:
		return "ActualRelayTarget";
	case RelayRefinementSymbolKind::ActualRelayArgument:
		return "ActualRelayArgument";
	case RelayRefinementSymbolKind::DirectRelayCount:
		return "DirectRelayCount";
	default:
		return "Unknown";
	}
}

const char *ConstraintKindName(RelayConstraintKind kind)
{
	switch (kind)
	{
	case RelayConstraintKind::RelayTargetRelation:
		return "RelayTargetRelation";
	case RelayConstraintKind::RelayArgumentRelation:
		return "RelayArgumentRelation";
	case RelayConstraintKind::RelayGuardNecessity:
		return "RelayGuardNecessity";
	case RelayConstraintKind::RelayGuardEquivalence:
		return "RelayGuardEquivalence";
	case RelayConstraintKind::RelayCountEquality:
		return "RelayCountEquality";
	case RelayConstraintKind::RelayCountNonNegative:
		return "RelayCountNonNegative";
	case RelayConstraintKind::RelayCountUpperBound:
		return "RelayCountUpperBound";
	default:
		return "Unknown";
	}
}

#if defined(RPREDA_ENABLE_Z3) || \
	defined(RPREDA_ENABLE_BOUND_RELAY_MANIFEST)
const char *ConstraintRoleName(RelayConstraintRole role)
{
	switch (role)
	{
	case RelayConstraintRole::SemanticDefinition:
		return "SemanticDefinition";
	case RelayConstraintRole::SolverAssumption:
		return "SolverAssumption";
	case RelayConstraintRole::SolverGoal:
	default:
		return "SolverGoal";
	}
}
#endif

const char *ProofObligationKindName(
	RelayProofObligationKind kind)
{
	switch (kind)
	{
	case RelayProofObligationKind::RelayTargetEquality:
		return "RelayTargetEquality";
	case RelayProofObligationKind::RelayArgumentEquality:
		return "RelayArgumentEquality";
	case RelayProofObligationKind::RelayGuardNecessity:
		return "RelayGuardNecessity";
	case RelayProofObligationKind::RelayGuardEquivalence:
		return "RelayGuardEquivalence";
	case RelayProofObligationKind::RelayCountEquality:
		return "RelayCountEquality";
	case RelayProofObligationKind::RelayCountUpperBound:
		return "RelayCountUpperBound";
	case RelayProofObligationKind::TargetNonAliasCandidate:
		return "TargetNonAliasCandidate";
	case RelayProofObligationKind::RelayMutualExclusion:
		return "RelayMutualExclusion";
	case RelayProofObligationKind::RelayTargetIndependence:
		return "RelayTargetIndependence";
	case RelayProofObligationKind::BooleanRefinement:
		return "BooleanRefinement";
	case RelayProofObligationKind::Unknown:
	default:
		return "Unknown";
	}
}

const char *ProofObligationStatusName(
	RelayProofObligationStatus status)
{
	switch (status)
	{
	case RelayProofObligationStatus::Generated:
		return "Generated";
	case RelayProofObligationStatus::Unsupported:
	default:
		return "Unsupported";
	}
}

#if defined(RPREDA_ENABLE_Z3) || \
	defined(RPREDA_ENABLE_BOUND_RELAY_MANIFEST)
const char *ProofObligationRoleName(
	RelayProofObligationRole role)
{
	switch (role)
	{
	case RelayProofObligationRole::EstablishedByConstruction:
		return "EstablishedByConstruction";
	case RelayProofObligationRole::SolverGoal:
	default:
		return "SolverGoal";
	}
}

const char *SolverStatusName(
	solver::RelaySolverStatus status)
{
	switch (status)
	{
	case solver::RelaySolverStatus::NotRun:
		return "NotRun";
	case solver::RelaySolverStatus::EstablishedByConstruction:
		return "EstablishedByConstruction";
	case solver::RelaySolverStatus::Proved:
		return "Proved";
	case solver::RelaySolverStatus::Disproved:
		return "Disproved";
	case solver::RelaySolverStatus::Unknown:
		return "Unknown";
	case solver::RelaySolverStatus::Unsupported:
		return "Unsupported";
	case solver::RelaySolverStatus::InconsistentAssumptions:
		return "InconsistentAssumptions";
	case solver::RelaySolverStatus::EncodingError:
	default:
		return "EncodingError";
	}
}
#endif

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

std::string BitVectorPredaName(uint16_t bitWidth)
{
	return "uint" + std::to_string(bitWidth);
}

Json EmitSort(const FormulaSort &sort)
{
	Json result = {
		{"kind", FormulaSortKindName(sort.kind)},
	};
	if (sort.kind == FormulaSortKind::UnsignedBitVector)
	{
		result["bit_width"] = sort.bitWidth;
		result["preda_name"] = BitVectorPredaName(sort.bitWidth);
	}
	return result;
}

Json EmitFormula(const FormulaExpr &formula)
{
	Json result = {
		{"kind", FormulaExprKindName(formula.kind)},
		{"sort", EmitSort(formula.sort)},
		{"source_text", formula.text},
		{"operator", formula.op},
		{"location", EmitLocation(formula.location)},
	};
	if (!formula.symbolId.empty())
		result["symbol_id"] = formula.symbolId;
	if (!formula.literalValue.empty())
		result["literal_value"] = formula.literalValue;
	if (formula.kind == FormulaExprKind::Unknown ||
		!formula.unknownReason.empty())
	{
		result["reason"] = formula.unknownReason;
	}
	result["children"] = Json::array();
	for (const FormulaExpr &child : formula.children)
		result["children"].push_back(EmitFormula(child));
	return result;
}

Json EmitDependencies(
	const analysis::RelayExpressionDependency &dependency)
{
	std::vector<std::string> dependencyNames;
	dependencyNames.reserve(dependency.classes.size());
	for (analysis::RelayDependencyClass dependencyClass :
		dependency.classes)
	{
		dependencyNames.push_back(
			DependencyClassName(dependencyClass));
	}
	std::sort(dependencyNames.begin(), dependencyNames.end());
	dependencyNames.erase(
		std::unique(
			dependencyNames.begin(),
			dependencyNames.end()),
		dependencyNames.end());

	Json result = Json::array();
	for (const std::string &dependencyName : dependencyNames)
		result.push_back(dependencyName);
	return result;
}

Json EmitSymbol(const RelayRefinementSymbol &symbol)
{
	Json result = {
		{"id", symbol.id},
		{"kind", SymbolKindName(symbol.kind)},
		{"source_function_id", symbol.sourceFunctionId},
		{"source_name", symbol.sourceName},
		{"preda_type", symbol.sourceType},
		{"sort", EmitSort(symbol.sort)},
		{"dependencies", EmitDependencies(symbol.dependency)},
		{"availability", AvailabilityStageName(symbol.availability)},
		{"admission_time_evaluable",
			symbol.dependency.admissionTimeEvaluable},
		{"location", EmitLocation(symbol.location)},
		{"relay_site_id", symbol.relaySiteId},
		{"argument_index", symbol.argumentIndex},
	};
	if (!symbol.dependency.reason.empty())
		result["dependency_reason"] = symbol.dependency.reason;
	return result;
}

Json EmitConstraint(const RelayConstraint &constraint)
{
	Json result = {
		{"id", constraint.id},
		{"kind", ConstraintKindName(constraint.kind)},
		{"source_function_id", constraint.sourceFunctionId},
		{"relay_site_id", constraint.relaySiteId},
		{"argument_index", constraint.argumentIndex},
		{"formula", EmitFormula(constraint.formula)},
		{"location", EmitLocation(constraint.location)},
	};
#if defined(RPREDA_ENABLE_Z3) || \
	defined(RPREDA_ENABLE_BOUND_RELAY_MANIFEST)
	result["role"] = ConstraintRoleName(constraint.role);
#endif
	return result;
}

#if defined(RPREDA_ENABLE_Z3) || \
	defined(RPREDA_ENABLE_BOUND_RELAY_MANIFEST)
Json EmitSolverResult(
	const solver::RelaySolverResult &solverResult)
{
	std::vector<std::string> assumptionIds =
		solverResult.assumptionConstraintIds;
	std::sort(assumptionIds.begin(), assumptionIds.end());
	assumptionIds.erase(
		std::unique(assumptionIds.begin(), assumptionIds.end()),
		assumptionIds.end());

	Json counterexample = Json::array();
	for (const solver::RelayCounterexampleValue &value :
		solverResult.projectedCounterexample)
	{
		counterexample.push_back(Json{
			{"symbol_id", value.symbolId},
			{"sort", EmitSort(value.sort)},
			{"value", value.value},
		});
	}

	return Json{
		{"backend", solverResult.backend},
		{"status", SolverStatusName(solverResult.status)},
		{"elapsed_time_ms", solverResult.elapsedTimeMs},
		{"assumption_constraint_ids", std::move(assumptionIds)},
		{"reason", solverResult.reason},
		{"projected_counterexample", std::move(counterexample)},
	};
}
#endif

Json EmitProofObligation(
	const RelayProofObligation &obligation)
{
	std::vector<std::string> constraintIds =
		obligation.constraintIds;
	std::sort(constraintIds.begin(), constraintIds.end());
	constraintIds.erase(
		std::unique(constraintIds.begin(), constraintIds.end()),
		constraintIds.end());

	Json result = {
		{"id", obligation.id},
		{"kind", ProofObligationKindName(obligation.kind)},
		{"status",
			ProofObligationStatusName(obligation.status)},
		{"source_function_id", obligation.sourceFunctionId},
		{"relay_site_id", obligation.relaySiteId},
		{"related_relay_site_id",
			obligation.relatedRelaySiteId},
		{"argument_index", obligation.argumentIndex},
		{"constraint_ids", std::move(constraintIds)},
		{"goal", EmitFormula(obligation.goal)},
		{"location", EmitLocation(obligation.location)},
		{"reason", obligation.reason},
	};
#if defined(RPREDA_ENABLE_Z3) || \
	defined(RPREDA_ENABLE_BOUND_RELAY_MANIFEST)
	result["proof_role"] =
		ProofObligationRoleName(obligation.role);
	result["solver_result"] =
		EmitSolverResult(obligation.solverResult);
#endif
	return result;
}

template <typename Value>
std::vector<const Value *> SortById(
	const std::vector<Value> &values)
{
	std::vector<const Value *> sorted;
	sorted.reserve(values.size());
	for (const Value &value : values)
		sorted.push_back(&value);
	std::stable_sort(
		sorted.begin(),
		sorted.end(),
		[](const Value *left, const Value *right) {
			return left->id < right->id;
		});
	return sorted;
}

} // namespace

nlohmann::ordered_json RelayRefinementEmitter::Emit(
	const std::vector<RelayRefinementSymbol> &symbols,
	const std::vector<RelayConstraint> &constraints,
	const std::vector<RelayProofObligation> &proofObligations)
{
	Json result = {
		{"symbols", Json::array()},
		{"constraints", Json::array()},
		{"proof_obligations", Json::array()},
	};

	for (const RelayRefinementSymbol *symbol : SortById(symbols))
		result["symbols"].push_back(EmitSymbol(*symbol));
	for (const RelayConstraint *constraint : SortById(constraints))
		result["constraints"].push_back(EmitConstraint(*constraint));
	for (const RelayProofObligation *obligation :
		SortById(proofObligations))
	{
		result["proof_obligations"].push_back(
			EmitProofObligation(*obligation));
	}
	return result;
}

} // namespace refinement
} // namespace relay_protocol
} // namespace transpiler
