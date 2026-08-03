#include "RelayTraceFaultInjector.h"

#include "../../3rdParty/nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <sstream>
#include <tuple>
#include <utility>
#include <vector>

namespace oxd {
namespace relay_trace {
namespace {

using Json = nlohmann::ordered_json;

std::string Lower(std::string value)
{
	std::transform(
		value.begin(),
		value.end(),
		value.begin(),
		[](unsigned char character)
		{
			return static_cast<char>(std::tolower(character));
		});
	return value;
}

bool ReadRequiredString(
	const Json &object,
	const char *name,
	std::string &value,
	std::string &error)
{
	auto found = object.find(name);
	if (found == object.end() || !found->is_string())
	{
		error = std::string("missing or non-string field: ") + name;
		return false;
	}
	value = found->get<std::string>();
	if (value.empty())
	{
		error = std::string("field must not be empty: ") + name;
		return false;
	}
	return true;
}

bool ReadOptionalString(
	const Json &object,
	const char *name,
	std::string &value,
	std::string &error)
{
	auto found = object.find(name);
	if (found == object.end())
		return true;
	if (!found->is_string())
	{
		error = std::string("non-string field: ") + name;
		return false;
	}
	value = found->get<std::string>();
	return true;
}

template <typename T>
bool ReadUnsigned(
	const Json &object,
	const char *name,
	T &value,
	bool required,
	std::string &error)
{
	auto found = object.find(name);
	if (found == object.end())
	{
		if (required)
			error = std::string("missing unsigned field: ") + name;
		return !required;
	}
	if (!found->is_number_integer() && !found->is_number_unsigned())
	{
		error = std::string("non-integer field: ") + name;
		return false;
	}
	try
	{
		uint64_t parsed = 0;
		if (found->is_number_unsigned())
		{
			parsed = found->get<uint64_t>();
		}
		else
		{
			const int64_t signedValue = found->get<int64_t>();
			if (signedValue < 0)
			{
				error = std::string("unsigned field is out of range: ") + name;
				return false;
			}
			parsed = static_cast<uint64_t>(signedValue);
		}
		if (parsed >
			static_cast<uint64_t>(std::numeric_limits<T>::max()))
		{
			error = std::string("unsigned field is out of range: ") + name;
			return false;
		}
		value = static_cast<T>(parsed);
		return true;
	}
	catch (const std::exception &)
	{
		error = std::string("unsigned field is out of range: ") + name;
		return false;
	}
}

bool ReadOptionalUint64(
	const Json &object,
	const char *name,
	std::optional<uint64_t> &value,
	std::string &error)
{
	auto found = object.find(name);
	if (found == object.end())
		return true;
	uint64_t parsed = 0;
	if (!ReadUnsigned(object, name, parsed, true, error))
		return false;
	value = parsed;
	return true;
}

std::optional<RuntimeTraceFaultKind> ParseKind(const std::string &value)
{
	if (value == "RuntimeTargetScopeCorruption")
		return RuntimeTraceFaultKind::RuntimeTargetScopeCorruption;
	if (value == "RuntimeRelayDuplicate")
		return RuntimeTraceFaultKind::RuntimeRelayDuplicate;
	return std::nullopt;
}

std::optional<ScopeKind> ParseScope(const std::string &value)
{
	const std::string lower = Lower(value);
	if (lower == "none") return ScopeKind::None;
	if (lower == "global") return ScopeKind::Global;
	if (lower == "shard") return ScopeKind::Shard;
	if (lower == "address") return ScopeKind::Address;
	if (lower == "uint32") return ScopeKind::Uint32;
	if (lower == "uint64") return ScopeKind::Uint64;
	if (lower == "uint96") return ScopeKind::Uint96;
	if (lower == "uint128") return ScopeKind::Uint128;
	if (lower == "uint160") return ScopeKind::Uint160;
	if (lower == "uint256") return ScopeKind::Uint256;
	if (lower == "uint512") return ScopeKind::Uint512;
	return std::nullopt;
}

bool SameLogicalEmission(
	const RelayRouteTraceEvent &route,
	const RelayEmitTraceEvent &emission)
{
	return route.parentTraceTxId == emission.parentTraceTxId &&
		route.sourceModuleId == emission.sourceModuleId &&
		route.relaySiteOrdinal == emission.relaySiteOrdinal &&
		route.relaySiteId == emission.relaySiteId &&
		route.occurrenceIndex == emission.occurrenceIndex;
}

} // namespace

RelayTraceFaultInjector::RelayTraceFaultInjector(RuntimeTraceFaultSpec spec)
	: m_spec(std::move(spec))
{
}

std::optional<RuntimeTraceFaultSpec>
RelayTraceFaultInjector::ParseSpecJson(
	const std::string &contents,
	std::string &error)
{
	error.clear();
	try
	{
		const Json root = Json::parse(contents);
		if (!root.is_object())
		{
			error = "fault spec root must be an object";
			return std::nullopt;
		}

		RuntimeTraceFaultSpec spec;
		if (!ReadUnsigned(
				root,
				"schema_version",
				spec.schemaVersion,
				true,
				error))
		{
			return std::nullopt;
		}
		if (spec.schemaVersion != 1)
		{
			error = "unsupported fault spec schema_version";
			return std::nullopt;
		}
		if (!ReadRequiredString(root, "mutation_id", spec.mutationId, error))
			return std::nullopt;
		std::string kind;
		if (!ReadRequiredString(root, "kind", kind, error))
			return std::nullopt;
		const auto parsedKind = ParseKind(kind);
		if (!parsedKind)
		{
			error = "unknown runtime fault kind: " + kind;
			return std::nullopt;
		}
		spec.kind = *parsedKind;
		if (!ReadUnsigned(root, "seed", spec.seed, true, error))
			return std::nullopt;

		auto selector = root.find("selector");
		if (selector == root.end() || !selector->is_object())
		{
			error = "missing or non-object field: selector";
			return std::nullopt;
		}
		if (!ReadOptionalString(
				*selector,
				"source_module_id",
				spec.selector.sourceModuleId,
				error) ||
			!ReadRequiredString(
				*selector,
				"source_function_id",
				spec.selector.sourceFunctionId,
				error) ||
			!ReadRequiredString(
				*selector,
				"relay_site_id",
				spec.selector.relaySiteId,
				error) ||
			!ReadUnsigned(
				*selector,
				"occurrence_index",
				spec.selector.occurrenceIndex,
				true,
				error) ||
			!ReadOptionalUint64(
				*selector,
				"root_trace_tx_id",
				spec.selector.rootTraceTxId,
				error) ||
			!ReadOptionalUint64(
				*selector,
				"parent_trace_tx_id",
				spec.selector.parentTraceTxId,
				error))
		{
			return std::nullopt;
		}

		if (spec.kind ==
			RuntimeTraceFaultKind::RuntimeTargetScopeCorruption)
		{
			std::string replacement;
			if (!ReadRequiredString(
					root,
					"replacement_scope",
					replacement,
					error))
			{
				return std::nullopt;
			}
			spec.replacementScope = ParseScope(replacement);
			if (!spec.replacementScope)
			{
				error = "unknown replacement_scope: " + replacement;
				return std::nullopt;
			}
		}

		return spec;
	}
	catch (const std::exception &exception)
	{
		error = std::string("invalid fault spec JSON: ") + exception.what();
		return std::nullopt;
	}
}

std::unique_ptr<RelayTraceFaultInjector>
RelayTraceFaultInjector::LoadSpecFile(
	const std::string &path,
	std::string &error)
{
	error.clear();
	std::ifstream input(path, std::ios::binary);
	if (!input)
	{
		error = "cannot open runtime fault spec: " + path;
		return {};
	}
	std::ostringstream contents;
	contents << input.rdbuf();
	if (!input.good() && !input.eof())
	{
		error = "cannot read runtime fault spec: " + path;
		return {};
	}
	auto spec = ParseSpecJson(contents.str(), error);
	if (!spec)
		return {};
	return std::make_unique<RelayTraceFaultInjector>(std::move(*spec));
}

bool RelayTraceFaultInjector::Matches(
	const RelayEmitTraceEvent &event) const
{
	const RuntimeTraceFaultSelector &selector = m_spec.selector;
	return (selector.sourceModuleId.empty() ||
			event.sourceModuleId == selector.sourceModuleId) &&
		event.sourceFunctionId == selector.sourceFunctionId &&
		event.relaySiteId == selector.relaySiteId &&
		event.occurrenceIndex == selector.occurrenceIndex &&
		(!selector.rootTraceTxId ||
			event.rootTraceTxId == *selector.rootTraceTxId) &&
		(!selector.parentTraceTxId ||
			event.parentTraceTxId == *selector.parentTraceTxId);
}

RuntimeTraceFaultApplication
RelayTraceFaultInjector::BaseApplication() const
{
	RuntimeTraceFaultApplication result;
	result.mutationId = m_spec.mutationId;
	result.kind = m_spec.kind;
	result.sourceFunctionId = m_spec.selector.sourceFunctionId;
	result.relaySiteId = m_spec.selector.relaySiteId;
	result.occurrenceIndex = m_spec.selector.occurrenceIndex;
	return result;
}

RuntimeTraceFaultApplication RelayTraceFaultInjector::Apply(
	RelayExecutionTraceSlice &slice)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	RuntimeTraceFaultApplication result = BaseApplication();
	if (m_terminal)
	{
		result.status =
			RuntimeTraceFaultApplicationStatus::AlreadyApplied;
		result.reason = m_applied
			? "runtime trace fault was already applied"
			: "runtime trace fault reached a terminal invalid state";
		return result;
	}

	std::vector<size_t> matches;
	for (size_t index = 0; index < slice.emissions.size(); ++index)
	{
		if (Matches(slice.emissions[index]))
			matches.push_back(index);
	}
	if (matches.empty())
	{
		result.reason = "runtime trace selector did not match this slice";
		return result;
	}
	if (matches.size() != 1)
	{
		m_terminal = true;
		result.status = RuntimeTraceFaultApplicationStatus::Invalid;
		result.reason =
			"runtime trace selector is ambiguous within one execution slice";
		return result;
	}

	const size_t selectedIndex = matches.front();
	const RelayEmitTraceEvent &selected = slice.emissions[selectedIndex];
	result.rootTraceTxId = selected.rootTraceTxId;
	result.parentTraceTxId = selected.parentTraceTxId;
	result.originalScope = selected.actualTargetScope;

	if (m_spec.kind ==
		RuntimeTraceFaultKind::RuntimeTargetScopeCorruption)
	{
		if (!m_spec.replacementScope ||
			*m_spec.replacementScope == ScopeKind::Unknown ||
			*m_spec.replacementScope == selected.actualTargetScope)
		{
			m_terminal = true;
			result.status = RuntimeTraceFaultApplicationStatus::Invalid;
			result.reason =
				"replacement scope is absent, unknown, or unchanged";
			return result;
		}
		slice.emissions[selectedIndex].actualTargetScope =
			*m_spec.replacementScope;
		result.mutatedScope = *m_spec.replacementScope;
		result.status = RuntimeTraceFaultApplicationStatus::Applied;
		result.reason =
			"corrupted target scope in validation-only trace slice";
	}
	else
	{
		uint32_t maximumOccurrence = selected.occurrenceIndex;
		uint64_t maximumSequence = selected.emissionSequence;
		uint64_t maximumChildTraceId = selected.childTraceTxId;
		for (const RelayEmitTraceEvent &emission : slice.emissions)
		{
			if (emission.parentTraceTxId == selected.parentTraceTxId)
			{
				maximumSequence =
					std::max(maximumSequence, emission.emissionSequence);
				maximumChildTraceId =
					std::max(maximumChildTraceId, emission.childTraceTxId);
			}
			if (emission.parentTraceTxId == selected.parentTraceTxId &&
				emission.sourceModuleId == selected.sourceModuleId &&
				emission.relaySiteOrdinal == selected.relaySiteOrdinal &&
				emission.relaySiteId == selected.relaySiteId)
			{
				maximumOccurrence =
					std::max(maximumOccurrence, emission.occurrenceIndex);
			}
		}
		if (maximumOccurrence == std::numeric_limits<uint32_t>::max() ||
			maximumSequence == std::numeric_limits<uint64_t>::max() ||
			maximumChildTraceId == std::numeric_limits<uint64_t>::max())
		{
			m_terminal = true;
			result.status = RuntimeTraceFaultApplicationStatus::Invalid;
			result.reason = "cannot allocate deterministic duplicate identity";
			return result;
		}

		RelayExecutionTraceSlice mutated = slice;
		RelayEmitTraceEvent duplicate = selected;
		duplicate.occurrenceIndex = maximumOccurrence + 1;
		duplicate.emissionSequence = maximumSequence + 1;
		duplicate.childTraceTxId = maximumChildTraceId + 1;
		mutated.emissions.push_back(duplicate);

		const size_t originalRouteCount = slice.routes.size();
		for (size_t index = 0; index < originalRouteCount; ++index)
		{
			if (!SameLogicalEmission(slice.routes[index], selected))
				continue;
			RelayRouteTraceEvent route = slice.routes[index];
			route.occurrenceIndex = duplicate.occurrenceIndex;
			// This synthetic ID exists only in a validation slice. Zero makes
			// that provenance explicit and is not used by route correlation.
			route.physicalTraceTxId = NoTraceTransaction;
			mutated.routes.push_back(std::move(route));
			++result.clonedRouteCount;
		}
		slice = std::move(mutated);
		result.duplicateOccurrenceIndex = duplicate.occurrenceIndex;
		result.mutatedScope = duplicate.actualTargetScope;
		result.status = RuntimeTraceFaultApplicationStatus::Applied;
		result.reason =
			"duplicated logical emission and matching routes in "
			"validation-only trace slice";
	}

	m_applied = true;
	m_terminal = true;
	return result;
}

bool RelayTraceFaultInjector::Applied() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_applied;
}

bool RelayTraceFaultInjector::Terminal() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_terminal;
}

const RuntimeTraceFaultSpec &RelayTraceFaultInjector::Spec() const
{
	return m_spec;
}

const char *ToString(RuntimeTraceFaultKind kind)
{
	switch (kind)
	{
	case RuntimeTraceFaultKind::RuntimeTargetScopeCorruption:
		return "RuntimeTargetScopeCorruption";
	case RuntimeTraceFaultKind::RuntimeRelayDuplicate:
		return "RuntimeRelayDuplicate";
	}
	return "RuntimeTargetScopeCorruption";
}

const char *ToString(RuntimeTraceFaultApplicationStatus status)
{
	switch (status)
	{
	case RuntimeTraceFaultApplicationStatus::NotApplied:
		return "NotApplied";
	case RuntimeTraceFaultApplicationStatus::Applied: return "Applied";
	case RuntimeTraceFaultApplicationStatus::AlreadyApplied:
		return "AlreadyApplied";
	case RuntimeTraceFaultApplicationStatus::Invalid: return "Invalid";
	}
	return "Invalid";
}

} // namespace relay_trace
} // namespace oxd
