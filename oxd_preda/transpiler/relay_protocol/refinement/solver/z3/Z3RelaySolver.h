#pragma once

#include "../RelaySolverBackend.h"

namespace transpiler {
namespace relay_protocol {
namespace refinement {
namespace solver {
namespace z3_backend {

// Optional Z3 implementation of the solver-independent relay refinement
// backend. Assumption selection and circular-proof rejection happen in
// RelayProofRunner; this class executes the checked A / A && !G algorithm.
class Z3RelaySolver final : public RelaySolverBackend
{
public:
	std::string GetName() const override;

	RelaySolverResult Solve(
		const RelaySolverRequest &request) const override;
};

} // namespace z3_backend
} // namespace solver
} // namespace refinement
} // namespace relay_protocol
} // namespace transpiler
