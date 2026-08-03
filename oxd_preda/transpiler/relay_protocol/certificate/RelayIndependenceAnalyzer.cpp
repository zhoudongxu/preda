#include "RelayIndependenceAnalyzer.h"

namespace transpiler {
namespace relay_protocol {
namespace certificate {
namespace {

using refinement::solver::RelaySolverStatus;

bool HasGlobalOrExternalEffect(const cfg::EffectSummary &effect)
{
	return effect.readsGlobalState ||
		effect.writesGlobalState ||
		effect.mayCallUnknown ||
		effect.mayHaveExternalEffect;
}

bool HasUnprovedTransitiveEffect(const cfg::EffectSummary &effect)
{
	return effect.mayEmitRelay;
}

} // namespace

bool RelayIndependenceAnalyzer::PassesIndependenceGate(
	const RelayPairCandidate &candidate,
	RelayPairCertificate &certificate) const
{
	if (!candidate.effectsComplete)
	{
		certificate.relation = RelayPairRelation::Unknown;
		certificate.status = CertificatePropertyStatus::Unknown;
		certificate.reason =
			"relay-region effect summary is unavailable or incomplete";
		return false;
	}
	if (candidate.relayKindA == RelayKind::Global ||
		candidate.relayKindB == RelayKind::Global ||
		candidate.relayKindA == RelayKind::Shards ||
		candidate.relayKindB == RelayKind::Shards)
	{
		certificate.relation = RelayPairRelation::PotentialConflict;
		certificate.status = CertificatePropertyStatus::Conservative;
		certificate.reason =
			"global or all-shards relay destinations are not partition-disjoint";
		return false;
	}
	if (candidate.targetScopeA != candidate.targetScopeB)
	{
		certificate.relation = RelayPairRelation::Unknown;
		certificate.status = CertificatePropertyStatus::Unknown;
		certificate.reason =
			"target scopes differ and no cross-scope non-conflict rule is established";
		return false;
	}
	if (HasGlobalOrExternalEffect(candidate.effectA) ||
		HasGlobalOrExternalEffect(candidate.effectB))
	{
		certificate.relation = RelayPairRelation::PotentialConflict;
		certificate.status = CertificatePropertyStatus::Conservative;
		certificate.reason =
			"global, external, or unresolved handler effects prevent independence";
		return false;
	}
	if (HasUnprovedTransitiveEffect(candidate.effectA) ||
		HasUnprovedTransitiveEffect(candidate.effectB))
	{
		certificate.relation = RelayPairRelation::Unknown;
		certificate.status = CertificatePropertyStatus::Unknown;
		certificate.reason =
			"relay handler emits further relays without a transitive disjointness proof";
		return false;
	}
	if (!candidate.targetFormulasSupported)
	{
		certificate.relation = RelayPairRelation::Unknown;
		certificate.status = CertificatePropertyStatus::Unsupported;
		certificate.reason =
			"target formulas are unsupported or have incompatible sorts";
		return false;
	}
	if (!candidate.sameSourceFunction ||
		candidate.ownerFunctionA != candidate.rootFunctionId)
	{
		certificate.relation = RelayPairRelation::Unknown;
		certificate.status = CertificatePropertyStatus::Unsupported;
		certificate.reason =
			"target proof outside the root function requires formal/actual parameter binding";
		return false;
	}
	return true;
}

void RelayIndependenceAnalyzer::ClassifyMutualExclusion(
	const RelayPairCandidate &candidate,
	const refinement::RelayProofObligation &obligation,
	RelayPairCertificate &certificate) const
{
	certificate.feasibilityResult = obligation.solverResult;
	certificate.supportingSolverResultIds.push_back(obligation.id);
	switch (obligation.solverResult.status)
	{
	case RelaySolverStatus::Proved:
		certificate.relation = RelayPairRelation::MutuallyExclusive;
		certificate.status = CertificatePropertyStatus::Proved;
		certificate.reason =
			"Z3 proved !(PathA && PathB) for exact acyclic CFG paths";
		break;
	case RelaySolverStatus::Disproved:
		certificate.relation = RelayPairRelation::Unknown;
		certificate.status = CertificatePropertyStatus::Conservative;
		certificate.reason =
			"joint path condition is satisfiable; target conflict analysis is required";
		break;
	case RelaySolverStatus::NotRun:
		certificate.relation = RelayPairRelation::Unknown;
		certificate.status = CertificatePropertyStatus::Unknown;
		certificate.reason =
			"Z3 is disabled; no solver-derived pair relation was asserted";
		break;
	case RelaySolverStatus::Unsupported:
	case RelaySolverStatus::EncodingError:
		certificate.relation = RelayPairRelation::Unknown;
		certificate.status = CertificatePropertyStatus::Unsupported;
		certificate.reason = obligation.solverResult.reason;
		break;
	default:
		certificate.relation = RelayPairRelation::Unknown;
		certificate.status = CertificatePropertyStatus::Unknown;
		certificate.reason = obligation.solverResult.reason;
		break;
	}
}

void RelayIndependenceAnalyzer::ClassifyTargetIndependence(
	const RelayPairCandidate &candidate,
	const refinement::RelayProofObligation &obligation,
	RelayPairCertificate &certificate) const
{
	certificate.relationResult = obligation.solverResult;
	certificate.supportingSolverResultIds.push_back(obligation.id);
	certificate.supportingConstraintIds.insert(
		certificate.supportingConstraintIds.end(),
		obligation.constraintIds.begin(),
		obligation.constraintIds.end());
	switch (obligation.solverResult.status)
	{
	case RelaySolverStatus::Proved:
		certificate.relation = RelayPairRelation::CoEmissionIndependent;
		certificate.status = CertificatePropertyStatus::Proved;
		certificate.reason =
			"joint path is satisfiable and Z3 proved distinct targets under it";
		break;
	case RelaySolverStatus::Disproved:
		certificate.relation = RelayPairRelation::ProvedMayAlias;
		certificate.status = CertificatePropertyStatus::Proved;
		certificate.reason =
			"Z3 found a jointly emitted target-alias counterexample";
		certificate.counterexample =
			obligation.solverResult.projectedCounterexample;
		if (certificate.counterexample.empty())
		{
			refinement::solver::RelayCounterexampleValue witness;
			witness.symbolId = candidate.siteA + " == " + candidate.siteB;
			witness.value = "equal targets under a satisfiable joint path";
			certificate.counterexample.push_back(std::move(witness));
		}
		break;
	case RelaySolverStatus::NotRun:
		certificate.relation = RelayPairRelation::Unknown;
		certificate.status = CertificatePropertyStatus::Unknown;
		certificate.reason =
			"Z3 is disabled; target independence was not checked";
		break;
	case RelaySolverStatus::Unsupported:
	case RelaySolverStatus::EncodingError:
		certificate.relation = RelayPairRelation::Unknown;
		certificate.status = CertificatePropertyStatus::Unsupported;
		certificate.reason = obligation.solverResult.reason;
		break;
	default:
		certificate.relation = RelayPairRelation::Unknown;
		certificate.status = CertificatePropertyStatus::Unknown;
		certificate.reason = obligation.solverResult.reason;
		break;
	}
}

} // namespace certificate
} // namespace relay_protocol
} // namespace transpiler
