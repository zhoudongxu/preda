#pragma once

#include <cstdint>

namespace prlrt {

using RelaySiteOrdinal = uint32_t;

constexpr uint32_t RPREDA_RUNTIME_TRACE_ABI_VERSION = 1;

#if !defined(__wasm32__)

struct IRelayTraceRuntimeInterface
{
	virtual void PushRelayTraceSite(RelaySiteOrdinal ordinal) noexcept = 0;
	virtual void PopRelayTraceSite(RelaySiteOrdinal ordinal) noexcept = 0;
};

extern thread_local IRelayTraceRuntimeInterface*
	g_relayTraceRuntimeInterface;

class RelayTraceSiteGuard
{
public:
	explicit RelayTraceSiteGuard(RelaySiteOrdinal ordinal) noexcept
		: m_interface(g_relayTraceRuntimeInterface)
		, m_ordinal(ordinal)
	{
		if (m_interface != nullptr)
			m_interface->PushRelayTraceSite(m_ordinal);
	}

	~RelayTraceSiteGuard() noexcept
	{
		if (m_interface != nullptr)
			m_interface->PopRelayTraceSite(m_ordinal);
	}

	RelayTraceSiteGuard(const RelayTraceSiteGuard &) = delete;
	RelayTraceSiteGuard &operator=(const RelayTraceSiteGuard &) = delete;
	RelayTraceSiteGuard(RelayTraceSiteGuard &&) = delete;
	RelayTraceSiteGuard &operator=(RelayTraceSiteGuard &&) = delete;

private:
	IRelayTraceRuntimeInterface *m_interface;
	RelaySiteOrdinal m_ordinal;
};

#else

// Runtime relay tracing is currently implemented only for native contract
// modules. Keeping a no-op guard here lets a trace-enabled toolchain continue
// compiling WASM modules through the original relay ABI without introducing
// trace-only imports.
class RelayTraceSiteGuard
{
public:
	explicit RelayTraceSiteGuard(RelaySiteOrdinal) noexcept {}
};

#endif

} // namespace prlrt
