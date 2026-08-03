#pragma once

#if !defined(RPREDA_ENABLE_TRACE_FAULT_INJECTION)
#error "RelayTraceFaultInjector is test-only; enable RPREDA_ENABLE_TRACE_FAULT_INJECTION explicitly"
#endif

#include "RelayTraceCollector.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace oxd {
namespace relay_trace {

// These faults mutate only an owning RelayExecutionTraceSlice immediately
// before validation. They must never be applied to SimuTxn, collector-owned
// events, routing queues, or worker state.
enum class RuntimeTraceFaultKind : uint8_t
{
	RuntimeTargetScopeCorruption,
	RuntimeRelayDuplicate,
};

enum class RuntimeTraceFaultApplicationStatus : uint8_t
{
	NotApplied,
	Applied,
	AlreadyApplied,
	Invalid,
};

struct RuntimeTraceFaultSelector
{
	std::string sourceModuleId;
	std::string sourceFunctionId;
	std::string relaySiteId;
	uint32_t occurrenceIndex = 0;
	std::optional<uint64_t> rootTraceTxId;
	std::optional<uint64_t> parentTraceTxId;
};

struct RuntimeTraceFaultSpec
{
	uint32_t schemaVersion = 1;
	std::string mutationId;
	RuntimeTraceFaultKind kind =
		RuntimeTraceFaultKind::RuntimeTargetScopeCorruption;
	RuntimeTraceFaultSelector selector;
	uint64_t seed = 0;
	std::optional<ScopeKind> replacementScope;
};

struct RuntimeTraceFaultApplication
{
	RuntimeTraceFaultApplicationStatus status =
		RuntimeTraceFaultApplicationStatus::NotApplied;
	std::string mutationId;
	RuntimeTraceFaultKind kind =
		RuntimeTraceFaultKind::RuntimeTargetScopeCorruption;
	std::string reason;
	std::string sourceFunctionId;
	std::string relaySiteId;
	uint64_t rootTraceTxId = NoTraceTransaction;
	uint64_t parentTraceTxId = NoTraceTransaction;
	uint32_t occurrenceIndex = 0;
	uint32_t duplicateOccurrenceIndex = 0;
	ScopeKind originalScope = ScopeKind::Unknown;
	ScopeKind mutatedScope = ScopeKind::Unknown;
	size_t clonedRouteCount = 0;
};

class RelayTraceFaultInjector
{
public:
	explicit RelayTraceFaultInjector(RuntimeTraceFaultSpec spec);

	// Parsing is strict: missing selectors, unknown kinds/scopes, wrong JSON
	// types, and unsupported schema versions return an error.
	static std::optional<RuntimeTraceFaultSpec> ParseSpecJson(
		const std::string &contents,
		std::string &error);
	static std::unique_ptr<RelayTraceFaultInjector> LoadSpecFile(
		const std::string &path,
		std::string &error);

	// Applies at most once. A selector miss does not consume the injector, so a
	// later execution slice may still match. Once Applied or Invalid is
	// returned, subsequent calls return AlreadyApplied.
	RuntimeTraceFaultApplication Apply(RelayExecutionTraceSlice &slice);

	bool Applied() const;
	bool Terminal() const;
	const RuntimeTraceFaultSpec &Spec() const;

private:
	bool Matches(const RelayEmitTraceEvent &event) const;
	RuntimeTraceFaultApplication BaseApplication() const;

	RuntimeTraceFaultSpec m_spec;
	mutable std::mutex m_mutex;
	bool m_applied = false;
	bool m_terminal = false;
};

const char *ToString(RuntimeTraceFaultKind kind);
const char *ToString(RuntimeTraceFaultApplicationStatus status);

} // namespace relay_trace
} // namespace oxd
