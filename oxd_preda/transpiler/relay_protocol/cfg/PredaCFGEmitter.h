#pragma once

#include "PredaCFG.h"
#include "../../../3rdParty/nlohmann/json.hpp"

namespace transpiler {
namespace relay_protocol {
namespace cfg {

class PredaCFGEmitter
{
public:
	static nlohmann::ordered_json Emit(
		const PredaControlFlowIR &controlFlow);
};

} // namespace cfg
} // namespace relay_protocol
} // namespace transpiler
