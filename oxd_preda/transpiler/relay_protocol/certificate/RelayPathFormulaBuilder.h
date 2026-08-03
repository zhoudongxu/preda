#pragma once

#include "RelayPairCandidate.h"

namespace transpiler {
namespace relay_protocol {
namespace certificate {

struct RelayPathFormula
{
	bool supported = false;
	// False means the formula is a conservative reachability
	// over-approximation. UNSAT remains a sound exclusion proof, while SAT
	// must not be treated as proof that co-emission is feasible.
	bool feasibilityExact = false;
	refinement::FormulaExpr formula = refinement::FormulaExpr::Unknown(
		std::string(), SourceLocation(), "path formula was not built");
	std::vector<std::string> supportingCfgFactIds;
	std::string reason;
};

class RelayPathFormulaBuilder
{
public:
	RelayPathFormula Build(
		const RelayProtocolIR &protocol,
		const RelayPairCandidate &candidate,
		const std::string &relaySiteId) const;
};

} // namespace certificate
} // namespace relay_protocol
} // namespace transpiler
