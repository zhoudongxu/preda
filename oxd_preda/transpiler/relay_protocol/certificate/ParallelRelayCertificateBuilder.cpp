#include "ParallelRelayCertificateBuilder.h"

#include "RelayCertificateConstraintGenerator.h"
#include "RelayDepthBoundAnalyzer.h"
#include "RelayIndependenceAnalyzer.h"
#include "RelayPairCandidateBuilder.h"
#include "RelayPathFormulaBuilder.h"
#include "RelayPrecedenceAnalyzer.h"
#include "RelayWorkBoundAnalyzer.h"

#include "../RelayProtocolIR.h"
#include "../refinement/solver/RelayProofRunner.h"

#include <algorithm>

namespace transpiler {
namespace relay_protocol {
namespace certificate {
namespace {

refinement::RelayProofObligation RunAndAppend(
	RelayProtocolIR &protocol,
	const refinement::solver::RelaySolverBackend *backend,
	refinement::RelayProofObligation obligation)
{
	std::vector<refinement::RelayProofObligation> pending;
	pending.push_back(std::move(obligation));
	refinement::solver::RelayProofRunner runner(backend);
	runner.Run(
		protocol.refinementSymbols,
		protocol.refinementConstraints,
		pending);
	protocol.refinementProofObligations.push_back(pending.front());
	return std::move(pending.front());
}

RelayPairCertificate BaseCertificate(
	const RelayPairCandidate &candidate,
	const std::string &suffix)
{
	RelayPairCertificate certificate;
	certificate.id = candidate.id + "." + suffix;
	certificate.sourceFunctionId = candidate.rootFunctionId;
	certificate.siteA = candidate.siteA;
	certificate.siteB = candidate.siteB;
	certificate.siteALocation = candidate.siteALocation;
	certificate.siteBLocation = candidate.siteBLocation;
	certificate.supportingCfgFactIds = candidate.supportingCfgFactIds;
	std::sort(
		certificate.supportingCfgFactIds.begin(),
		certificate.supportingCfgFactIds.end());
	certificate.supportingCfgFactIds.erase(
		std::unique(
			certificate.supportingCfgFactIds.begin(),
			certificate.supportingCfgFactIds.end()),
		certificate.supportingCfgFactIds.end());
	return certificate;
}

void SetUnknown(
	RelayPairCertificate &certificate,
	const std::string &reason,
	CertificatePropertyStatus status = CertificatePropertyStatus::Unknown)
{
	certificate.relation = RelayPairRelation::Unknown;
	certificate.status = status;
	certificate.reason = reason;
}

void SetFunctionStatus(FunctionParallelRelayCertificate &certificate)
{
	bool anyProved = false;
	bool anyUnknown = false;
	bool anyUnsupported = false;
	for (const RelayPairCertificate &pair : certificate.pairRelations)
	{
		anyProved = anyProved ||
			pair.status == CertificatePropertyStatus::Proved;
		anyUnknown = anyUnknown ||
			pair.status == CertificatePropertyStatus::Unknown ||
			pair.status == CertificatePropertyStatus::Conservative;
		anyUnsupported = anyUnsupported ||
			pair.status == CertificatePropertyStatus::Unsupported;
	}
	const bool boundUnknown =
		certificate.directWorkBound.status == CertificateStatus::Unknown ||
		certificate.transitiveWorkBound.status == CertificateStatus::Unknown ||
		certificate.physicalRouteWork.status == CertificateStatus::Unknown ||
		certificate.depthBound.status == CertificateStatus::Unknown;
	const bool boundUnsupported =
		certificate.directWorkBound.status == CertificateStatus::Unsupported ||
		certificate.transitiveWorkBound.status == CertificateStatus::Unsupported ||
		certificate.physicalRouteWork.status == CertificateStatus::Unsupported ||
		certificate.depthBound.status == CertificateStatus::Unsupported;
	const bool boundConservative =
		certificate.directWorkBound.status == CertificateStatus::Conservative ||
		certificate.transitiveWorkBound.status == CertificateStatus::Conservative ||
		certificate.physicalRouteWork.status == CertificateStatus::Conservative ||
		certificate.depthBound.status == CertificateStatus::Conservative;
	const bool anyBoundComplete =
		certificate.directWorkBound.status == CertificateStatus::Complete ||
		certificate.transitiveWorkBound.status == CertificateStatus::Complete ||
		certificate.physicalRouteWork.status == CertificateStatus::Complete ||
		certificate.depthBound.status == CertificateStatus::Complete;

	if (!anyUnknown && !anyUnsupported && !boundUnknown &&
		!boundUnsupported && !boundConservative)
	{
		certificate.status = CertificateStatus::Complete;
		certificate.reason = "all generated pair and bound certificates are complete";
	}
	else if (anyProved || boundConservative || anyBoundComplete)
	{
		certificate.status = CertificateStatus::Conservative;
		certificate.reason =
			"some properties are proved while others remain conservative or unknown";
	}
	else if (anyUnsupported || boundUnsupported)
	{
		certificate.status = CertificateStatus::Unsupported;
		certificate.reason = "all useful properties are unsupported";
	}
	else
	{
		certificate.status = CertificateStatus::Unknown;
		certificate.reason = "no complete pair or bound certificate is available";
	}
}

} // namespace

void ParallelRelayCertificateBuilder::Build(
	RelayProtocolIR &protocol,
	const refinement::solver::RelaySolverBackend *solverBackend) const
{
	protocol.parallelCertificate.Reset();
	const RelayPairCandidatesByFunction candidates =
		RelayPairCandidateBuilder().Build(protocol);
	RelayPathFormulaBuilder pathBuilder;
	RelayCertificateConstraintGenerator constraintGenerator;
	RelayPrecedenceAnalyzer precedenceAnalyzer;
	RelayIndependenceAnalyzer independenceAnalyzer;
	RelayWorkBoundAnalyzer workAnalyzer;
	RelayDepthBoundAnalyzer depthAnalyzer;

	for (const cfg::PredaFunctionCFG &function :
		protocol.controlFlow.functions)
	{
		FunctionParallelRelayCertificate functionCertificate;
		functionCertificate.sourceFunctionId = function.functionId;
		const auto functionCandidates = candidates.find(function.functionId);
		if (functionCandidates != candidates.end())
		{
			for (const RelayPairCandidate &candidate :
				functionCandidates->second)
			{
				RelayPairCertificate classification =
					BaseCertificate(candidate, "classification");
				if (!candidate.exactContext || !candidate.loopFreeContext)
				{
					SetUnknown(
						classification,
						!candidate.exactContext
							? candidate.reason
							: "loop occurrence is not modeled by Phase-E certificates");
					functionCertificate.pairRelations.push_back(
						std::move(classification));
					continue;
				}

				const RelayPathFormula pathA = pathBuilder.Build(
					protocol, candidate, candidate.siteA);
				const RelayPathFormula pathB = pathBuilder.Build(
					protocol, candidate, candidate.siteB);
				const RelayPairProofPlan plan =
					constraintGenerator.BuildMutualExclusion(
						candidate, pathA, pathB);
				if (!plan.supported)
				{
					const RelayPrecedenceResult precedence =
						precedenceAnalyzer.Analyze(protocol, candidate);
					if (precedence.proved)
					{
						RelayPairCertificate proved =
							BaseCertificate(candidate, "precedence");
						proved.relation = precedence.relation;
						proved.status = CertificatePropertyStatus::Proved;
						proved.reason = precedence.reason;
						proved.supportingCfgFactIds =
							precedence.supportingCfgFactIds;
						functionCertificate.pairRelations.push_back(
							std::move(proved));
					}
					SetUnknown(
						classification,
						plan.reason,
						CertificatePropertyStatus::Unsupported);
					classification.id = candidate.id + ".conflict";
					functionCertificate.pairRelations.push_back(
						std::move(classification));
					continue;
				}

				classification.supportingCfgFactIds =
					plan.supportingCfgFactIds;
				refinement::RelayProofObligation exclusion = RunAndAppend(
					protocol, solverBackend, plan.mutualExclusion);
				independenceAnalyzer.ClassifyMutualExclusion(
					candidate, exclusion, classification);
				if (classification.relation ==
					RelayPairRelation::MutuallyExclusive)
				{
					if (!plan.coEmissionFeasibilityExact)
					{
						classification.reason =
							"Z3 proved joint emission UNSAT on a conservative path over-approximation";
					}
					classification.id = candidate.id + ".exclusion";
					functionCertificate.pairRelations.push_back(
						std::move(classification));
					continue;
				}

				const RelayPrecedenceResult precedence =
					precedenceAnalyzer.Analyze(protocol, candidate);
				if (precedence.proved)
				{
					RelayPairCertificate proved =
						BaseCertificate(candidate, "precedence");
					proved.relation = precedence.relation;
					proved.status = CertificatePropertyStatus::Proved;
					proved.reason = precedence.reason;
					proved.supportingCfgFactIds =
						precedence.supportingCfgFactIds;
					functionCertificate.pairRelations.push_back(
						std::move(proved));
				}
				if (exclusion.solverResult.status !=
					refinement::solver::RelaySolverStatus::Disproved)
				{
					classification.id = candidate.id + ".conflict";
					functionCertificate.pairRelations.push_back(
						std::move(classification));
					continue;
				}
				if (!plan.coEmissionFeasibilityExact)
				{
					SetUnknown(
						classification,
						"joint-path SAT is only a conservative reachability witness; feasible co-emission is not established",
						CertificatePropertyStatus::Conservative);
					classification.id = candidate.id + ".conflict";
					functionCertificate.pairRelations.push_back(
						std::move(classification));
					continue;
				}

				classification.id = candidate.id + ".conflict";
				if (!independenceAnalyzer.PassesIndependenceGate(
						candidate, classification))
				{
					functionCertificate.pairRelations.push_back(
						std::move(classification));
					continue;
				}
				refinement::RelayProofObligation target = RunAndAppend(
					protocol,
					solverBackend,
					constraintGenerator.BuildTargetIndependence(
						protocol, candidate, plan.jointPath));
				independenceAnalyzer.ClassifyTargetIndependence(
					candidate, target, classification);
				functionCertificate.pairRelations.push_back(
					std::move(classification));
			}
		}

		workAnalyzer.Analyze(protocol, functionCertificate);
		depthAnalyzer.Analyze(protocol, functionCertificate);
		for (RelayPairCertificate &pair : functionCertificate.pairRelations)
		{
			std::sort(
				pair.supportingCfgFactIds.begin(),
				pair.supportingCfgFactIds.end());
			pair.supportingCfgFactIds.erase(
				std::unique(
					pair.supportingCfgFactIds.begin(),
					pair.supportingCfgFactIds.end()),
				pair.supportingCfgFactIds.end());
		}
		std::sort(
			functionCertificate.pairRelations.begin(),
			functionCertificate.pairRelations.end(),
			[](const RelayPairCertificate &left,
			   const RelayPairCertificate &right)
			{
				return left.id < right.id;
			});
		SetFunctionStatus(functionCertificate);
		protocol.parallelCertificate.functions.push_back(
			std::move(functionCertificate));
	}
	std::sort(
		protocol.parallelCertificate.functions.begin(),
		protocol.parallelCertificate.functions.end(),
		[](const FunctionParallelRelayCertificate &left,
		   const FunctionParallelRelayCertificate &right)
		{
			return left.sourceFunctionId < right.sourceFunctionId;
		});
}

} // namespace certificate
} // namespace relay_protocol
} // namespace transpiler
