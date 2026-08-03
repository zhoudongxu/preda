#pragma once

#include "RelayPathFormulaBuilder.h"

#include "../refinement/RelayProofObligation.h"

namespace transpiler {
namespace relay_protocol {
namespace certificate {

struct RelayPairProofPlan
{
	bool supported = false;
	bool coEmissionFeasibilityExact = false;
	refinement::FormulaExpr jointPath = refinement::FormulaExpr::Unknown(
		std::string(), SourceLocation(), "joint path was not built");
	refinement::RelayProofObligation mutualExclusion;
	std::vector<std::string> supportingCfgFactIds;
	std::string reason;
};

class RelayCertificateConstraintGenerator
{
public:
	RelayPairProofPlan BuildMutualExclusion(
		const RelayPairCandidate &candidate,
		const RelayPathFormula &pathA,
		const RelayPathFormula &pathB) const;

	refinement::RelayProofObligation BuildTargetIndependence(
		const RelayProtocolIR &protocol,
		const RelayPairCandidate &candidate,
		const refinement::FormulaExpr &jointPath) const;
};

} // namespace certificate
} // namespace relay_protocol
} // namespace transpiler
