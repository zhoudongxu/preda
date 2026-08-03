#pragma once

#include "RelayPairCandidate.h"
#include "ParallelRelayCertificate.h"

namespace transpiler {
namespace relay_protocol {
namespace certificate {

struct RelayPrecedenceResult
{
	bool proved = false;
	RelayPairRelation relation = RelayPairRelation::Unknown;
	std::vector<std::string> supportingCfgFactIds;
	std::string reason;
};

class RelayPrecedenceAnalyzer
{
public:
	RelayPrecedenceResult Analyze(
		const RelayProtocolIR &protocol,
		const RelayPairCandidate &candidate) const;
};

} // namespace certificate
} // namespace relay_protocol
} // namespace transpiler
