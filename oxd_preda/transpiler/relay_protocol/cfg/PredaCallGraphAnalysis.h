#pragma once

#include "PredaCFG.h"

namespace transpiler {
namespace relay_protocol {
namespace cfg {

class PredaCallGraphAnalyzer
{
public:
	static void Analyze(
		PredaSynchronousCallGraph &graph,
		const std::vector<PredaFunctionCFG> &functions);
};

} // namespace cfg
} // namespace relay_protocol
} // namespace transpiler
