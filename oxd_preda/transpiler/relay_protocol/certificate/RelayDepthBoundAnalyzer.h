#pragma once

#include "ParallelRelayCertificate.h"

namespace transpiler {
namespace relay_protocol {

struct RelayProtocolIR;

namespace certificate {

class RelayDepthBoundAnalyzer
{
public:
	void Analyze(
		const RelayProtocolIR &protocol,
		FunctionParallelRelayCertificate &certificate) const;
};

} // namespace certificate
} // namespace relay_protocol
} // namespace transpiler
