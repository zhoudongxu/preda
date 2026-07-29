#pragma once

#include "RelayConstraint.h"
#include "RelayProofObligation.h"

#include "../RelayProtocolIR.h"

#include <vector>

namespace transpiler {
namespace relay_protocol {
namespace refinement {

class RelayFormulaBuilder;
class RelayRefinementSymbolTable;

struct RelayConstraintGenerationResult
{
	std::vector<RelayConstraint> constraints;
	std::vector<RelayProofObligation> proofObligations;
};

class RelayConstraintGenerator
{
public:
	// Generates owning FormulaIR constraints and proof goals.  This pass is
	// read-only with respect to RelayProtocolIR and has no lowering/runtime
	// side effects.  The symbol table is extended with stable synthetic
	// emitted/actual/count symbols as required.
	RelayConstraintGenerationResult Generate(
		const RelayProtocolIR &protocol,
		RelayRefinementSymbolTable &symbols,
		const RelayFormulaBuilder &formulaBuilder) const;
};

} // namespace refinement
} // namespace relay_protocol
} // namespace transpiler
