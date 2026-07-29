#pragma once

#include "RelayFormulaIR.h"

#include "../RelayExprIR.h"

#include <cstdint>
#include <string>

namespace transpiler {
namespace relay_protocol {
namespace refinement {

// Constraints are asserted facts produced by the compiler analysis.  A
// relation that cannot be represented soundly is omitted; the corresponding
// proof obligation is retained with Unsupported status instead.
enum class RelayConstraintKind : uint8_t
{
	RelayTargetRelation,
	RelayArgumentRelation,
	RelayGuardNecessity,
	RelayGuardEquivalence,
	RelayCountEquality,
	RelayCountNonNegative,
	RelayCountUpperBound,
};

struct RelayConstraint
{
	std::string id;
	RelayConstraintKind kind = RelayConstraintKind::RelayTargetRelation;
	std::string sourceFunctionId;
	std::string relaySiteId;
	int64_t argumentIndex = -1;
	FormulaExpr formula;
	SourceLocation location;
};

} // namespace refinement
} // namespace relay_protocol
} // namespace transpiler
