#pragma once

#include "RelayFormulaIR.h"
#include "../analysis/RelayExpressionDependency.h"

#include <cstdint>
#include <string>

namespace transpiler {
namespace relay_protocol {
namespace refinement {

enum class RelayRefinementSymbolKind : uint8_t
{
	SourceFunctionParameter,
	CurrentScopeKey,
	PreStateVariable,
	LoopVariable,
	RelayEmission,
	ActualRelayTarget,
	ActualRelayArgument,
	DirectRelayCount,
};

struct RelayRefinementSymbol
{
	std::string id;
	RelayRefinementSymbolKind kind =
		RelayRefinementSymbolKind::SourceFunctionParameter;
	std::string sourceFunctionId;
	std::string sourceName;
	std::string sourceType;
	FormulaSort sort = FormulaSort::Unknown();
	analysis::RelayExpressionDependency dependency;
	analysis::RelayAvailabilityStage availability =
		analysis::RelayAvailabilityStage::Unknown;
	SourceLocation location;
	std::string relaySiteId;
	int64_t argumentIndex = -1;
};

} // namespace refinement
} // namespace relay_protocol
} // namespace transpiler
