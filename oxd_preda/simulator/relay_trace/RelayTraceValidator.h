#pragma once

#include "RelayManifestLoader.h"

#include <optional>

namespace oxd {
namespace relay_trace {

struct RelayExecutionValidationInput
{
	RuntimeTxnTraceContext execution;
	std::vector<RelayEmitTraceEvent> emissions;
	std::vector<RelayRouteTraceEvent> routes;
	uint32_t activeShardCount = 0;

	// Supplied only after the root relay tree is complete. It is the maximum
	// depth relative to this execution, not the process-global absolute depth.
	std::optional<uint32_t> observedTransitiveDepth;
};

class RelayTraceValidator
{
public:
	std::vector<RelayValidationResult> ValidateManifestLoad(
		const RelayManifestLoadResult &loadResult) const;

	std::vector<RelayValidationResult> ValidateExecution(
		const LoadedRelayManifest &manifest,
		const RelayExecutionValidationInput &input) const;

private:
	RelayValidationResult Result(
		ValidationStatus status,
		ValidationCheckKind kind,
		const std::string &reason,
		const RuntimeTxnTraceContext &execution,
		const RelayEmitTraceEvent *emission,
		const LoadedRelayManifest &manifest,
		const std::string &expected = {},
		const std::string &actual = {}) const;
};

} // namespace relay_trace
} // namespace oxd
