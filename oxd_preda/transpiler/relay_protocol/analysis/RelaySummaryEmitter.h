#pragma once

#include "RelayProtocolSummary.h"
#include "../../../3rdParty/nlohmann/json.hpp"

namespace transpiler {
namespace relay_protocol {
namespace analysis {

class RelaySummaryEmitter
{
public:
	static nlohmann::ordered_json Emit(
		const RelayProtocolSummary &summary);
};

} // namespace analysis
} // namespace relay_protocol
} // namespace transpiler
