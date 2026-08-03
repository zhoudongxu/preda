#pragma once

#include "relay_manifest_abi.h"

#include <cstdint>

// This header deliberately defines optional side interfaces instead of
// extending any existing RVM vtable.  Trace and stock builds can therefore
// reject one another explicitly without changing the PREDA execution ABI.
#if defined(RPREDA_ENABLE_RUNTIME_TRACE)

namespace rvm
{

constexpr uint32_t RPredaRuntimeTraceAbiVersion = 1;

// Compatibility names retained for the existing trace loader. Artifact
// binding is now a common bound-manifest ABI and is not trace-specific.
using RelayTraceArtifactBinding = RelayManifestArtifactBinding;
using IRelayTraceModuleMetadataProvider =
	IRelayManifestModuleMetadataProvider;

struct IRelayTraceExecutionContext
{
	virtual ~IRelayTraceExecutionContext() = default;

	virtual void PushRelayTraceSite(
		const ContractModuleID &emittingModule,
		uint32_t siteOrdinal) noexcept = 0;
	virtual void PopRelayTraceSite(
		const ContractModuleID &emittingModule,
		uint32_t siteOrdinal) noexcept = 0;
};

} // namespace rvm

#endif
