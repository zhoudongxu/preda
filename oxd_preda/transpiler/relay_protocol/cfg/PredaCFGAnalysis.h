#pragma once

#include "PredaCFG.h"

namespace transpiler {
namespace relay_protocol {
namespace cfg {

class PredaCFGAnalysis
{
public:
	static PredaFunctionGraphAnalysis Analyze(
		const PredaFunctionCFG &function);
};

} // namespace cfg
} // namespace relay_protocol
} // namespace transpiler
