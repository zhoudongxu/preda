#pragma once

#include "ParallelRelayCertificate.h"
#include "RelayPairCandidate.h"

#include "../refinement/RelayProofObligation.h"

namespace transpiler {
namespace relay_protocol {
namespace certificate {

class RelayIndependenceAnalyzer
{
public:
	bool PassesIndependenceGate(
		const RelayPairCandidate &candidate,
		RelayPairCertificate &certificate) const;

	void ClassifyMutualExclusion(
		const RelayPairCandidate &candidate,
		const refinement::RelayProofObligation &obligation,
		RelayPairCertificate &certificate) const;

	void ClassifyTargetIndependence(
		const RelayPairCandidate &candidate,
		const refinement::RelayProofObligation &obligation,
		RelayPairCertificate &certificate) const;
};

} // namespace certificate
} // namespace relay_protocol
} // namespace transpiler
