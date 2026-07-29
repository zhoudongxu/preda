#pragma once

#include "../RelayFormulaIR.h"

#include <cstdint>
#include <string>
#include <vector>

namespace transpiler {
namespace relay_protocol {
namespace refinement {
namespace solver {

enum class RelaySolverStatus : uint8_t
{
	NotRun,
	EstablishedByConstruction,
	Proved,
	Disproved,
	Unknown,
	Unsupported,
	InconsistentAssumptions,
	EncodingError,
};

struct RelayCounterexampleValue
{
	std::string symbolId;
	FormulaSort sort = FormulaSort::Unknown();
	std::string value;
};

struct RelaySolverResult
{
	std::string backend = "none";
	RelaySolverStatus status = RelaySolverStatus::NotRun;
	uint64_t elapsedTimeMs = 0;
	std::vector<std::string> assumptionConstraintIds;
	std::string reason;
	std::vector<RelayCounterexampleValue> projectedCounterexample;
};

} // namespace solver
} // namespace refinement
} // namespace relay_protocol
} // namespace transpiler
