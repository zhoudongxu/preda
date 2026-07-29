#pragma once

#include "RelayConstraint.h"
#include "RelayProofObligation.h"
#include "RelayRefinementSymbol.h"

#include "../../../3rdParty/nlohmann/json.hpp"

#include <vector>

namespace transpiler {
namespace relay_protocol {
namespace refinement {

class RelayRefinementEmitter
{
public:
	static nlohmann::ordered_json Emit(
		const std::vector<RelayRefinementSymbol> &symbols,
		const std::vector<RelayConstraint> &constraints,
		const std::vector<RelayProofObligation> &proofObligations);
};

} // namespace refinement
} // namespace relay_protocol
} // namespace transpiler
