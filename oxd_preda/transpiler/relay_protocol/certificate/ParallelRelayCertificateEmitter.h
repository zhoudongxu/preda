#pragma once

#include "ParallelRelayCertificate.h"

#include "../../../3rdParty/nlohmann/json.hpp"

namespace transpiler {
namespace relay_protocol {
namespace certificate {

class ParallelRelayCertificateEmitter
{
public:
	static nlohmann::ordered_json Emit(
		const ParallelRelayCertificate &certificate);
};

} // namespace certificate
} // namespace relay_protocol
} // namespace transpiler
