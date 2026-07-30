#pragma once

#include "RelayTraceTypes.h"

#include <optional>
#include <unordered_map>
#include <vector>

namespace oxd {
namespace relay_trace {

enum class MarkerOperationStatus : uint8_t
{
	Ok,
	NoActiveTransaction,
	MissingMarker,
	StaleMarker,
	DuplicateConsume,
};

struct MarkerOperationResult
{
	MarkerOperationStatus status = MarkerOperationStatus::Ok;
	std::string reason;
	std::optional<RelaySiteMarkerFrame> marker;

	explicit operator bool() const
	{
		return status == MarkerOperationStatus::Ok;
	}
};

// Mutable state for one currently-live local SimuTxn key. It owns every value
// it stores; the SimuTxn pointer itself is held only by RelayTraceCollector as
// an ephemeral map key.
class RelayTraceContext
{
public:
	explicit RelayTraceContext(RuntimeTxnTraceContext context = {});

	const RuntimeTxnTraceContext &Transaction() const;
	RuntimeTxnTraceContext &Transaction();

	uint64_t PushMarker(
		const std::string &moduleId,
		RelaySiteOrdinal ordinal,
		uint64_t generation);

	MarkerOperationResult ConsumeMarker();
	MarkerOperationResult PopMarker(
		uint64_t generation,
		const std::string &expectedModuleId,
		RelaySiteOrdinal expectedOrdinal,
		bool requireConsumed);

	uint32_t NextOccurrence(
		const std::string &moduleId,
		RelaySiteOrdinal ordinal);
	bool HasMarkers() const;
	size_t MarkerDepth() const;

private:
	struct OccurrenceKey
	{
		std::string moduleId;
		RelaySiteOrdinal ordinal = InvalidRelaySiteOrdinal;

		bool operator==(const OccurrenceKey &other) const
		{
			return ordinal == other.ordinal && moduleId == other.moduleId;
		}
	};

	struct OccurrenceKeyHash
	{
		size_t operator()(const OccurrenceKey &key) const;
	};

	RuntimeTxnTraceContext m_transaction;
	std::vector<RelaySiteMarkerFrame> m_markers;
	std::unordered_map<OccurrenceKey, uint32_t, OccurrenceKeyHash>
		m_nextOccurrence;
};

const char *ToString(MarkerOperationStatus status);

} // namespace relay_trace
} // namespace oxd
