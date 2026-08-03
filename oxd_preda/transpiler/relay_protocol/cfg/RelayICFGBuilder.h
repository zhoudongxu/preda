#pragma once

#include "PredaCFG.h"

namespace transpiler {
namespace relay_protocol {

struct RelayProtocolIR;

namespace cfg {

class RelayICFGBuilder
{
public:
	static RelayICFG Build(
		const std::vector<PredaFunctionCFG> &functions,
		const PredaSynchronousCallGraph &callGraph,
		const RelayProtocolIR &protocol);
};

} // namespace cfg
} // namespace relay_protocol
} // namespace transpiler
