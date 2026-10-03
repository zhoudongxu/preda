#include "RelayOptimizationTypes.h"

#include <algorithm>
#include <cctype>
#include <limits>

namespace oxd {
namespace relay_optimization {
namespace {

std::string Lower(std::string value)
{
	std::transform(
		value.begin(),
		value.end(),
		value.begin(),
		[](unsigned char c)
		{
			return static_cast<char>(std::tolower(c));
		});
	return value;
}

bool ParseDecimalPart(
	const std::string &text,
	uint64_t &out)
{
	if(text.empty())
		return false;

	uint64_t value = 0;
	for(char c : text)
	{
		if(c < '0' || c > '9')
			return false;
		const uint64_t digit = static_cast<uint64_t>(c - '0');
		if(value >
			(std::numeric_limits<uint64_t>::max() - digit) / 10)
		{
			return false;
		}
		value = value * 10 + digit;
	}
	out = value;
	return true;
}

} // namespace

const char *ToString(OptimizationMode value) noexcept
{
	switch(value)
	{
	case OptimizationMode::Baseline:
		return "baseline";
	case OptimizationMode::Optimize:
		return "optimize";
	case OptimizationMode::OptimizeAudit:
		return "optimize_audit";
	}
	return "baseline";
}

const char *ToString(OptimizationAblation value) noexcept
{
	switch(value)
	{
	case OptimizationAblation::Baseline:
		return "baseline";
	case OptimizationAblation::GenericBatchOnly:
		return "generic_batch_only";
	case OptimizationAblation::VerifiedReserveOnly:
		return "verified_reserve_only";
	case OptimizationAblation::VerifiedReservePlusBatch:
		return "verified_reserve_plus_batch";
	}
	return "baseline";
}

bool ParseOptimizationMode(
	const std::string &text,
	OptimizationMode &out,
	std::string &error)
{
	const std::string value = Lower(text);
	if(value == "baseline")
		out = OptimizationMode::Baseline;
	else if(value == "optimize")
		out = OptimizationMode::Optimize;
	else if(value == "optimize_audit" ||
		value == "optimize+audit")
	{
		out = OptimizationMode::OptimizeAudit;
	}
	else
	{
		error =
			"expected baseline, optimize, or optimize_audit";
		return false;
	}
	return true;
}

bool ParseOptimizationAblation(
	const std::string &text,
	OptimizationAblation &out,
	std::string &error)
{
	const std::string value = Lower(text);
	if(value == "baseline")
		out = OptimizationAblation::Baseline;
	else if(value == "generic_batch_only")
		out = OptimizationAblation::GenericBatchOnly;
	else if(value == "verified_reserve_only")
		out = OptimizationAblation::VerifiedReserveOnly;
	else if(value == "verified_reserve_plus_batch")
		out = OptimizationAblation::VerifiedReservePlusBatch;
	else
	{
		error =
			"expected baseline, generic_batch_only, "
			"verified_reserve_only, or verified_reserve_plus_batch";
		return false;
	}
	return true;
}

bool ParseAuditSampleRate(
	const std::string &text,
	AuditSampleRate &out,
	std::string &error)
{
	const size_t slash = text.find('/');
	if(slash == std::string::npos ||
		text.find('/', slash + 1) != std::string::npos)
	{
		error = "expected numerator/denominator";
		return false;
	}

	uint64_t numerator = 0;
	uint64_t denominator = 0;
	if(!ParseDecimalPart(text.substr(0, slash), numerator) ||
		!ParseDecimalPart(text.substr(slash + 1), denominator))
	{
		error = "sample rate contains an invalid or overflowing integer";
		return false;
	}
	if(denominator == 0)
	{
		error = "sample rate denominator must be non-zero";
		return false;
	}
	if(numerator > denominator)
	{
		error = "sample rate numerator must not exceed denominator";
		return false;
	}
	out.numerator = numerator;
	out.denominator = denominator;
	return true;
}

bool ParseUnsignedDecimal(
	const std::string &text,
	uint64_t &out,
	std::string &error)
{
	if(!ParseDecimalPart(text, out))
	{
		error = "expected a non-negative uint64 decimal value";
		return false;
	}
	return true;
}

bool ValidateOptimizationConfig(
	OptimizationConfig &config,
	bool ablationWasExplicit,
	std::string &error,
	bool schedulerWasExplicit)
{
	if(config.mode == OptimizationMode::Baseline)
	{
		if(ablationWasExplicit &&
			config.ablation != OptimizationAblation::Baseline)
		{
			error =
				"baseline mode cannot enable an optimization ablation";
			return false;
		}
		if(schedulerWasExplicit &&
			config.schedulerMode != RelaySchedulerMode::FIFO)
		{
			error =
				"baseline mode cannot enable a non-FIFO scheduler";
			return false;
		}
		config.ablation = OptimizationAblation::Baseline;
		config.schedulerMode = RelaySchedulerMode::FIFO;
		return true;
	}

	if(!ablationWasExplicit)
	{
		config.ablation =
			OptimizationAblation::VerifiedReservePlusBatch;
	}
	else if(config.ablation == OptimizationAblation::Baseline)
	{
		error =
			"optimize modes require a non-baseline ablation";
		return false;
	}
	return true;
}

} // namespace relay_optimization
} // namespace oxd
