#pragma once

#include "RelayTraceTypes.h"
#include "../../runtime/relay_plan/RelayPlanRegistry.h"

namespace oxd {
namespace relay_trace {

// Source-compatible names for the former trace-local implementation.  Trace
// validation and runtime optimization now consume the same immutable,
// binding-verified manifest and the same module/opcode registry.
using ExpectedArtifactBinding = relay_plan::ExpectedArtifactBinding;
using ManifestRelaySite = relay_plan::BoundRelaySite;
using ManifestFunctionSummary = relay_plan::FunctionRelayPlan;
using ManifestNonAliasProof = relay_plan::ManifestNonAliasProof;
using ManifestRelayPairCertificate =
	relay_plan::ManifestRelayPairCertificate;
using ManifestFunctionParallelCertificate =
	relay_plan::ManifestFunctionParallelCertificate;
using ManifestWorkCertificate = relay_plan::ManifestWorkCertificate;
using LoadedRelayManifest = relay_plan::BoundRelayManifest;
using RelayManifestLoadResult = relay_plan::RelayPlanLoadResult;
using RelayManifestLoader = relay_plan::RelayPlanRegistry;

} // namespace relay_trace
} // namespace oxd
