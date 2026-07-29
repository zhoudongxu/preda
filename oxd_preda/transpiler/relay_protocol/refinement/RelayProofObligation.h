#pragma once

#include "RelayFormulaIR.h"
#include "solver/RelaySolverResult.h"

#include "../RelayExprIR.h"

#include <cstdint>
#include <string>
#include <vector>

namespace transpiler {
namespace relay_protocol {
namespace refinement {

enum class RelayProofObligationKind : uint8_t
{
	RelayTargetEquality,
	RelayArgumentEquality,
	RelayGuardNecessity,
	RelayGuardEquivalence,
	RelayCountEquality,
	RelayCountUpperBound,
	TargetNonAliasCandidate,
	BooleanRefinement,
	Unknown,
};

// Phase one only constructs goals.  No result may be marked Proved before a
// solver and a checked proof-result import path exist.
enum class RelayProofObligationStatus : uint8_t
{
	Generated,
	Unsupported,
};

enum class RelayProofObligationRole : uint8_t
{
	EstablishedByConstruction,
	SolverGoal,
};

struct RelayProofObligation
{
	std::string id;
	RelayProofObligationKind kind =
		RelayProofObligationKind::Unknown;
	RelayProofObligationStatus status =
		RelayProofObligationStatus::Unsupported;
	RelayProofObligationRole role =
		RelayProofObligationRole::SolverGoal;
	std::string sourceFunctionId;
	std::string relaySiteId;
	std::string relatedRelaySiteId;
	int64_t argumentIndex = -1;
	std::vector<std::string> constraintIds;
	FormulaExpr goal;
	SourceLocation location;
	std::string reason;
	solver::RelaySolverResult solverResult;
};

} // namespace refinement
} // namespace relay_protocol
} // namespace transpiler
