#pragma once

#include "PredaCFG.h"

namespace transpiler {
namespace relay_protocol {

struct RelayProtocolIR;

namespace cfg {

class PredaEffectAnalysis
{
public:
	static std::vector<PredaRegionEffect> Build(
		std::vector<PredaFunctionCFG> &functions,
		const PredaSynchronousCallGraph &callGraph,
		const RelayProtocolIR &protocol);

	static void MergeInto(
		EffectSummary &destination,
		const EffectSummary &source);
};

} // namespace cfg
} // namespace relay_protocol
} // namespace transpiler
