#pragma once

#include "ParallelRelayCertificate.h"

#include "../refinement/solver/RelaySolverBackend.h"

namespace transpiler {
namespace relay_protocol {

struct RelayProtocolIR;

namespace certificate {

class ParallelRelayCertificateBuilder
{
public:
	void Build(
		RelayProtocolIR &protocol,
		const refinement::solver::RelaySolverBackend *solverBackend) const;
};

} // namespace certificate
} // namespace relay_protocol
} // namespace transpiler
