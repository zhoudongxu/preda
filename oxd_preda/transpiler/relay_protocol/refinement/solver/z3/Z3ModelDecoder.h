#pragma once

#include "Z3SymbolEncoder.h"

#include "../RelaySolverResult.h"
#include "../../RelayConstraint.h"

#include <z3++.h>

#include <string>
#include <vector>

namespace transpiler {
namespace relay_protocol {
namespace refinement {
namespace solver {
namespace z3_backend {

struct Z3ModelDecodeResult
{
	std::vector<RelayCounterexampleValue> values;
	std::string reason;

	bool Succeeded() const
	{
		return reason.empty();
	}
};

// Projects a Z3 countermodel onto the stable compiler symbols that actually
// occur in the selected assumptions or the solver goal. Z3-internal helper
// declarations and unused refinement symbols are deliberately not exposed.
class Z3ModelDecoder
{
public:
	explicit Z3ModelDecoder(
		const Z3SymbolEncoder &symbols);

	Z3ModelDecodeResult Decode(
		const ::z3::model &model,
		const std::vector<const RelayConstraint *> &assumptions,
		const FormulaExpr &goal) const;

private:
	const Z3SymbolEncoder &m_symbols;
};

} // namespace z3_backend
} // namespace solver
} // namespace refinement
} // namespace relay_protocol
} // namespace transpiler
