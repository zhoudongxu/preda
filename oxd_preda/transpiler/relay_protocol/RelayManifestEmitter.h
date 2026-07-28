#pragma once

#include "RelayProtocolIR.h"

#include <string>

namespace transpiler {
namespace relay_protocol {

class RelayManifestEmitter
{
public:
	static std::string Emit(const RelayProtocolIR &protocol);
};

} // namespace relay_protocol
} // namespace transpiler
