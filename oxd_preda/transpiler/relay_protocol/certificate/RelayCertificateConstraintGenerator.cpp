#include "RelayCertificateConstraintGenerator.h"

#include <algorithm>

namespace transpiler {
namespace relay_protocol {
namespace certificate {
namespace {

const RelaySite *FindSite(
	const RelayProtocolIR &protocol,
	const std::string &siteId)
{
	for (const RelaySite &site : protocol.relaySites)
		if (site.id == siteId)
			return &site;
	return nullptr;
}

std::vector<std::string> TargetConstraintIds(
	const RelayProtocolIR &protocol,
	const RelayPairCandidate &candidate)
{
	std::vector<std::string> ids;
	for (const refinement::RelayConstraint &constraint :
		protocol.refinementConstraints)
	{
		if (constraint.kind !=
			refinement::RelayConstraintKind::RelayTargetRelation)
		{
			continue;
		}
		if (constraint.relaySiteId == candidate.siteA ||
			constraint.relaySiteId == candidate.siteB)
		{
			ids.push_back(constraint.id);
		}
	}
	std::sort(ids.begin(), ids.end());
	ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
	return ids;
}

refinement::RelayProofObligation BaseObligation(
	const RelayPairCandidate &candidate,
	const std::string &suffix,
	refinement::RelayProofObligationKind kind)
{
	refinement::RelayProofObligation obligation;
	obligation.id = candidate.id + "." + suffix;
	obligation.kind = kind;
	obligation.status =
		refinement::RelayProofObligationStatus::Generated;
	obligation.role =
		refinement::RelayProofObligationRole::SolverGoal;
	obligation.sourceFunctionId = candidate.rootFunctionId;
	obligation.relaySiteId = candidate.siteA;
	obligation.relatedRelaySiteId = candidate.siteB;
	obligation.location = candidate.siteALocation;
	return obligation;
}

refinement::RelayProofObligation UnsupportedObligation(
	const RelayPairCandidate &candidate,
	const std::string &reason)
{
	refinement::RelayProofObligation obligation = BaseObligation(
		candidate,
		"target_independence",
		refinement::RelayProofObligationKind::RelayTargetIndependence);
	obligation.status =
		refinement::RelayProofObligationStatus::Unsupported;
	obligation.reason = reason;
	obligation.goal = refinement::FormulaExpr::Unknown(
		std::string(), candidate.siteALocation, reason);
	return obligation;
}

} // namespace

RelayPairProofPlan RelayCertificateConstraintGenerator::BuildMutualExclusion(
	const RelayPairCandidate &candidate,
	const RelayPathFormula &pathA,
	const RelayPathFormula &pathB) const
{
	RelayPairProofPlan result;
	if (!pathA.supported || !pathB.supported)
	{
		result.reason = !pathA.supported ? pathA.reason : pathB.reason;
		return result;
	}
	result.jointPath = refinement::FormulaExpr::Nary(
		"&&",
		{pathA.formula, pathB.formula},
		refinement::FormulaSort::Bool(),
		candidate.siteALocation);
	result.coEmissionFeasibilityExact =
		pathA.feasibilityExact && pathB.feasibilityExact;
	result.mutualExclusion = BaseObligation(
		candidate,
		"mutual_exclusion",
		refinement::RelayProofObligationKind::RelayMutualExclusion);
	result.mutualExclusion.goal = refinement::FormulaExpr::Unary(
		"!",
		result.jointPath,
		refinement::FormulaSort::Bool(),
		candidate.siteALocation);
	result.supportingCfgFactIds = pathA.supportingCfgFactIds;
	result.supportingCfgFactIds.insert(
		result.supportingCfgFactIds.end(),
		pathB.supportingCfgFactIds.begin(),
		pathB.supportingCfgFactIds.end());
	std::sort(
		result.supportingCfgFactIds.begin(),
		result.supportingCfgFactIds.end());
	result.supportingCfgFactIds.erase(
		std::unique(
			result.supportingCfgFactIds.begin(),
			result.supportingCfgFactIds.end()),
		result.supportingCfgFactIds.end());
	result.supported = true;
	result.reason = result.coEmissionFeasibilityExact
		? "exact joint path formula generated from the acyclic CFG"
		: "joint path reachability over-approximation generated from the acyclic CFG";
	return result;
}

refinement::RelayProofObligation
RelayCertificateConstraintGenerator::BuildTargetIndependence(
	const RelayProtocolIR &protocol,
	const RelayPairCandidate &candidate,
	const refinement::FormulaExpr &jointPath) const
{
	if (!candidate.sameSourceFunction ||
		candidate.ownerFunctionA != candidate.rootFunctionId)
	{
		return UnsupportedObligation(
			candidate,
			"cross-function target proofs require formal/actual parameter binding");
	}
	if (!candidate.targetFormulasSupported)
	{
		return UnsupportedObligation(
			candidate,
			"relay target formulas are unavailable or have incompatible sorts");
	}
	const RelaySite *first = FindSite(protocol, candidate.siteA);
	const RelaySite *second = FindSite(protocol, candidate.siteB);
	if (first == nullptr || second == nullptr)
		return UnsupportedObligation(candidate, "relay site is unavailable");

	refinement::FormulaExpr distinct = refinement::FormulaExpr::Binary(
		"!=",
		first->refinementTargetFormula,
		second->refinementTargetFormula,
		refinement::FormulaSort::Bool(),
		candidate.siteALocation);
	refinement::RelayProofObligation obligation = BaseObligation(
		candidate,
		"target_independence",
		refinement::RelayProofObligationKind::RelayTargetIndependence);
	obligation.goal = refinement::FormulaExpr::Binary(
		"implies",
		jointPath,
		std::move(distinct),
		refinement::FormulaSort::Bool(),
		candidate.siteALocation);
	obligation.constraintIds = TargetConstraintIds(protocol, candidate);
	return obligation;
}

} // namespace certificate
} // namespace relay_protocol
} // namespace transpiler
