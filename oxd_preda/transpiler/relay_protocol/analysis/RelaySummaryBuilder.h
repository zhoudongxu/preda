#pragma once

#include "../RelayProtocolIR.h"

namespace transpiler {
namespace relay_protocol {
namespace analysis {

class RelaySummaryBuilder
{
public:
	// Populates FunctionProtocol::summary for every collected source function.
	// The builder only reads protocol facts and never participates in lowering
	// or runtime relay dispatch.
	void Build(RelayProtocolIR &protocol) const;
};

} // namespace analysis
} // namespace relay_protocol
} // namespace transpiler
