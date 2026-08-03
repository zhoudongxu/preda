#pragma once

#include "RelayOptimizationAudit.h"
#include "RelayOptimizationMetrics.h"
#include "RelayOptimizationTypes.h"

#include <optional>
#include <string>

namespace oxd {
namespace relay_optimization {

class RelayOptimizationReport
{
public:
	std::string BuildJson(
		const OptimizationConfig &config,
		const OptimizationMetricsSnapshot &metrics,
		const std::optional<AuditFailure> &failure,
		const OptimizationMeasurementWindowSnapshot &measurementWindow =
			OptimizationMeasurementWindowSnapshot{}) const;

	bool WriteAtomically(
		const std::string &path,
		const OptimizationConfig &config,
		const OptimizationMetricsSnapshot &metrics,
		const std::optional<AuditFailure> &failure,
		std::string *error = nullptr,
		const OptimizationMeasurementWindowSnapshot &measurementWindow =
			OptimizationMeasurementWindowSnapshot{}) const;
};

} // namespace relay_optimization
} // namespace oxd
