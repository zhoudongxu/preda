#pragma once

#include "RelaySolverBackend.h"

#include "../RelayProofObligation.h"

#include <cstdint>
#include <string>
#include <vector>

namespace transpiler {
namespace relay_protocol {
namespace refinement {
namespace solver {

struct RelayProofRunnerOptions
{
	uint64_t perObligationTimeoutMs = 1000;
	std::string disabledReason =
		"RPREDA_ENABLE_Z3 is disabled";
};

class RelayProofRunner
{
public:
	explicit RelayProofRunner(
		const RelaySolverBackend *backend = nullptr,
		RelayProofRunnerOptions options =
			RelayProofRunnerOptions());

	void Run(
		const std::vector<RelayRefinementSymbol> &symbols,
		const std::vector<RelayConstraint> &constraints,
		std::vector<RelayProofObligation> &obligations) const;

	RelaySolverResult RunOne(
		const std::vector<RelayRefinementSymbol> &symbols,
		const std::vector<RelayConstraint> &constraints,
		const RelayProofObligation &obligation) const;

private:
	const RelaySolverBackend *m_backend = nullptr;
	RelayProofRunnerOptions m_options;
};

} // namespace solver
} // namespace refinement
} // namespace relay_protocol
} // namespace transpiler
