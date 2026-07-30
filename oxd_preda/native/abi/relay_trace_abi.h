#pragma once

#include "vm_types.h"

#include <cstdint>

// This header deliberately defines optional side interfaces instead of
// extending any existing RVM vtable.  Trace and stock builds can therefore
// reject one another explicitly without changing the PREDA execution ABI.
#if defined(RPREDA_ENABLE_RUNTIME_TRACE)

namespace rvm
{

constexpr uint32_t RPredaRuntimeTraceAbiVersion = 1;

struct RelayTraceArtifactBinding
{
	ContractModuleID moduleId{};
	HashValue intermediateHash{};
	HashValue manifestHash{};
	ConstString dapp{};
	ConstString contract{};
	ConstString transpilerVersion{};
	bool bindingComplete = false;
};

struct IRelayTraceModuleMetadataProvider
{
	virtual ~IRelayTraceModuleMetadataProvider() = default;

	virtual bool GetRelayTraceArtifactBinding(
		const ContractModuleID &moduleId,
		RelayTraceArtifactBinding &out) const noexcept = 0;
};

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
