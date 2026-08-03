#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#if defined(RPREDA_ANALYSIS_PROFILER_EXPORTS)
#define RPREDA_ANALYSIS_PROFILER_API __declspec(dllexport)
#else
#define RPREDA_ANALYSIS_PROFILER_API __declspec(dllimport)
#endif
#else
#define RPREDA_ANALYSIS_PROFILER_API \
	__attribute__((visibility("default")))
#endif

namespace transpiler {
namespace relay_protocol {
namespace metrics {

// These phases are observational only. They never feed a result back into
// lowering, the generated C++ program, routing, or runtime execution.
enum class RelayAnalysisPhase : uint8_t
{
	CFGConstruction,
	CallGraphConstruction,
	EffectAnalysis,
	ICFGConstruction,
	SummaryAnalysis,
	RefinementGeneration,
	RefinementSolver,
	RefinementTotal,
	CertificateGeneration,
	AnalysisTotal,
	ManifestEmission,
	Count,
};

constexpr size_t RelayAnalysisPhaseCount =
	static_cast<size_t>(RelayAnalysisPhase::Count);

struct RelayAnalysisPhaseMeasurement
{
	uint64_t elapsedTimeNs = 0;
	uint64_t invocations = 0;
};

struct RelayAnalysisMetricsSnapshot
{
	bool enabled = false;
	std::array<RelayAnalysisPhaseMeasurement,
		RelayAnalysisPhaseCount> phases = {};
};

class RPREDA_ANALYSIS_PROFILER_API RelayAnalysisProfiler
{
public:
	// A session is explicitly enabled by the standalone analysis driver. A
	// normal compiler process never enables it. In a build where profiling is
	// disabled at CMake time, requested=true still produces a disabled session.
	static void Reset(bool requested = true) noexcept;
	static bool IsEnabled() noexcept;
	static void Record(
		RelayAnalysisPhase phase,
		uint64_t elapsedTimeNs) noexcept;
	static RelayAnalysisMetricsSnapshot Snapshot() noexcept;
};

RPREDA_ANALYSIS_PROFILER_API const char *ToString(
	RelayAnalysisPhase phase) noexcept;

class ScopedRelayAnalysisPhase
{
public:
	explicit ScopedRelayAnalysisPhase(
		RelayAnalysisPhase phase) noexcept
		: m_phase(phase),
		  m_active(RelayAnalysisProfiler::IsEnabled())
	{
		if (m_active)
			m_start = Clock::now();
	}

	~ScopedRelayAnalysisPhase() noexcept
	{
		Stop();
	}

	ScopedRelayAnalysisPhase(
		const ScopedRelayAnalysisPhase &) = delete;
	ScopedRelayAnalysisPhase &operator=(
		const ScopedRelayAnalysisPhase &) = delete;

	void Stop() noexcept
	{
		if (!m_active)
			return;
		const auto elapsed =
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				Clock::now() - m_start)
				.count();
		RelayAnalysisProfiler::Record(
			m_phase,
			elapsed > 0 ? static_cast<uint64_t>(elapsed) : 0);
		m_active = false;
	}

private:
	using Clock = std::chrono::steady_clock;
	RelayAnalysisPhase m_phase;
	bool m_active = false;
	Clock::time_point m_start;
};

} // namespace metrics
} // namespace relay_protocol
} // namespace transpiler

#undef RPREDA_ANALYSIS_PROFILER_API
