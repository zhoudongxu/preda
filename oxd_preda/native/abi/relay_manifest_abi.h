#pragma once

#include "vm_types.h"

#include <cstdint>

// Optional side ABI shared by consumers of the bound relay manifest.  It is
// deliberately separate from runtime trace execution markers so an
// optimization-only build does not change the generated contract ABI.
#if defined(RPREDA_ENABLE_BOUND_RELAY_MANIFEST) || \
	defined(RPREDA_ENABLE_RUNTIME_TRACE)

namespace rvm
{

constexpr uint32_t RPredaBoundRelayManifestAbiVersion = 1;

struct RelayManifestArtifactBinding
{
	ContractModuleID moduleId{};
	HashValue intermediateHash{};
	HashValue manifestHash{};
	ConstString dapp{};
	ConstString contract{};
	ConstString transpilerVersion{};
	bool bindingComplete = false;
};

struct IRelayManifestModuleMetadataProvider
{
	virtual ~IRelayManifestModuleMetadataProvider() = default;

	virtual bool GetRelayManifestArtifactBinding(
		const ContractModuleID &moduleId,
		RelayManifestArtifactBinding &out) const noexcept = 0;

	// Source-compatible bridge for the original trace-only consumer.  The
	// trace header aliases its legacy provider name to this common interface.
	bool GetRelayTraceArtifactBinding(
		const ContractModuleID &moduleId,
		RelayManifestArtifactBinding &out) const noexcept
	{
		return GetRelayManifestArtifactBinding(moduleId, out);
	}
};

} // namespace rvm

#endif
