#include "RelayAnalysisMetrics.h"

#include <limits>

namespace transpiler {
namespace relay_protocol {
namespace metrics {
namespace {

struct RelayAnalysisMetricsState
{
	bool enabled = false;
	std::array<RelayAnalysisPhaseMeasurement,
		RelayAnalysisPhaseCount> phases = {};
};

thread_local RelayAnalysisMetricsState g_state;

size_t Index(RelayAnalysisPhase phase) noexcept
{
	return static_cast<size_t>(phase);
}

uint64_t SaturatingAdd(uint64_t left, uint64_t right) noexcept
{
	const uint64_t maximum = std::numeric_limits<uint64_t>::max();
	return right > maximum - left ? maximum : left + right;
}

} // namespace

void RelayAnalysisProfiler::Reset(bool requested) noexcept
{
	g_state = RelayAnalysisMetricsState();
#ifdef RPREDA_ENABLE_ANALYSIS_PROFILING
	g_state.enabled = requested;
#else
	(void)requested;
#endif
}

bool RelayAnalysisProfiler::IsEnabled() noexcept
{
	return g_state.enabled;
}

void RelayAnalysisProfiler::Record(
	RelayAnalysisPhase phase,
	uint64_t elapsedTimeNs) noexcept
{
	if (!g_state.enabled || phase == RelayAnalysisPhase::Count)
		return;
	RelayAnalysisPhaseMeasurement &measurement =
		g_state.phases[Index(phase)];
	measurement.elapsedTimeNs = SaturatingAdd(
		measurement.elapsedTimeNs,
		elapsedTimeNs);
	measurement.invocations = SaturatingAdd(
		measurement.invocations,
		1);
}

RelayAnalysisMetricsSnapshot RelayAnalysisProfiler::Snapshot() noexcept
{
	RelayAnalysisMetricsSnapshot result;
	result.enabled = g_state.enabled;
	result.phases = g_state.phases;
	return result;
}

const char *ToString(RelayAnalysisPhase phase) noexcept
{
	switch (phase)
	{
	case RelayAnalysisPhase::CFGConstruction:
		return "cfg_construction";
	case RelayAnalysisPhase::CallGraphConstruction:
		return "call_graph_construction";
	case RelayAnalysisPhase::EffectAnalysis:
		return "effect_analysis";
	case RelayAnalysisPhase::ICFGConstruction:
		return "icfg_construction";
	case RelayAnalysisPhase::SummaryAnalysis:
		return "summary_analysis";
	case RelayAnalysisPhase::RefinementGeneration:
		return "refinement_generation";
	case RelayAnalysisPhase::RefinementSolver:
		return "refinement_solver";
	case RelayAnalysisPhase::RefinementTotal:
		return "refinement_total";
	case RelayAnalysisPhase::CertificateGeneration:
		return "certificate_generation";
	case RelayAnalysisPhase::AnalysisTotal:
		return "analysis_total";
	case RelayAnalysisPhase::ManifestEmission:
		return "manifest_emission";
	case RelayAnalysisPhase::Count:
	default:
		return "unknown";
	}
}

} // namespace metrics
} // namespace relay_protocol
} // namespace transpiler
