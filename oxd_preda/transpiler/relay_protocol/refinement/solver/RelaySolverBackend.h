#pragma once

#include "RelaySolverResult.h"

#include "../RelayConstraint.h"
#include "../RelayRefinementSymbol.h"

#include <cstdint>
#include <string>
#include <vector>

namespace transpiler {
namespace relay_protocol {
namespace refinement {
namespace solver {

struct RelaySolverRequest
{
	const std::vector<RelayRefinementSymbol> *symbols = nullptr;
	std::vector<const RelayConstraint *> assumptions;
	const FormulaExpr *goal = nullptr;
	uint64_t timeoutMs = 1000;
};

class RelaySolverBackend
{
public:
	virtual ~RelaySolverBackend() = default;

	virtual std::string GetName() const = 0;
	virtual RelaySolverResult Solve(
		const RelaySolverRequest &request) const = 0;
};

} // namespace solver
} // namespace refinement
} // namespace relay_protocol
} // namespace transpiler
