#pragma once

#include <cstdint>
#include <string>

namespace oxd {
namespace relay_optimization {

enum class OptimizationMode : uint8_t
{
	Baseline,
	Optimize,
	OptimizeAudit,
};

enum class OptimizationAblation : uint8_t
{
	Baseline,
	GenericBatchOnly,
	VerifiedReserveOnly,
	VerifiedReservePlusBatch,
};

struct AuditSampleRate
{
	uint64_t numerator = 1;
	uint64_t denominator = 1000;

	bool Enabled() const noexcept
	{
		return numerator != 0;
	}
};

struct OptimizationConfig
{
	OptimizationMode mode = OptimizationMode::Baseline;
	OptimizationAblation ablation = OptimizationAblation::Baseline;
	uint64_t maxRelayReserve = 1000000;
	AuditSampleRate auditSampleRate;
	std::string reportPath;

	bool UsesRelayPlan() const noexcept
	{
		return ablation == OptimizationAblation::VerifiedReserveOnly ||
			ablation == OptimizationAblation::VerifiedReservePlusBatch;
	}

	bool UsesBatchFastPath() const noexcept
	{
		return ablation == OptimizationAblation::GenericBatchOnly ||
			ablation == OptimizationAblation::VerifiedReservePlusBatch;
	}

	bool AuditEnabled() const noexcept
	{
		return mode == OptimizationMode::OptimizeAudit;
	}
};

const char *ToString(OptimizationMode value) noexcept;
const char *ToString(OptimizationAblation value) noexcept;

bool ParseOptimizationMode(
	const std::string &text,
	OptimizationMode &out,
	std::string &error);
bool ParseOptimizationAblation(
	const std::string &text,
	OptimizationAblation &out,
	std::string &error);
bool ParseAuditSampleRate(
	const std::string &text,
	AuditSampleRate &out,
	std::string &error);
bool ParseUnsignedDecimal(
	const std::string &text,
	uint64_t &out,
	std::string &error);
bool ValidateOptimizationConfig(
	OptimizationConfig &config,
	bool ablationWasExplicit,
	std::string &error);

} // namespace relay_optimization
} // namespace oxd
