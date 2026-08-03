#pragma once

#include "RelayPairCandidate.h"

namespace transpiler {
namespace relay_protocol {
namespace certificate {

class RelayPairCandidateBuilder
{
public:
	RelayPairCandidatesByFunction Build(
		const RelayProtocolIR &protocol) const;
};

} // namespace certificate
} // namespace relay_protocol
} // namespace transpiler
