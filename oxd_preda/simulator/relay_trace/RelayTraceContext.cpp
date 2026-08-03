#include "RelayTraceContext.h"

namespace oxd {
namespace relay_trace {

RelayTraceContext::RelayTraceContext(RuntimeTxnTraceContext context)
	: m_transaction(std::move(context))
{
}

const RuntimeTxnTraceContext &RelayTraceContext::Transaction() const
{
	return m_transaction;
}

RuntimeTxnTraceContext &RelayTraceContext::Transaction()
{
	return m_transaction;
}

uint64_t RelayTraceContext::PushMarker(
	const std::string &moduleId,
	RelaySiteOrdinal ordinal,
	uint64_t generation)
{
	RelaySiteMarkerFrame marker;
	marker.moduleId = moduleId;
	marker.ordinal = ordinal;
	marker.generation = generation;
	marker.traceTxId = m_transaction.traceTxId;
	m_markers.push_back(std::move(marker));
	return generation;
}

MarkerOperationResult RelayTraceContext::ConsumeMarker()
{
	if (m_markers.empty())
	{
		return {
			MarkerOperationStatus::MissingMarker,
			"relay creation has no active relay-site marker",
			std::nullopt,
		};
	}

	RelaySiteMarkerFrame &marker = m_markers.back();
	if (marker.traceTxId != m_transaction.traceTxId)
	{
		return {
			MarkerOperationStatus::StaleMarker,
			"relay-site marker belongs to another transaction generation",
			marker,
		};
	}
	if (marker.consumed)
	{
		return {
			MarkerOperationStatus::DuplicateConsume,
			"relay-site marker was already consumed",
			marker,
		};
	}

	marker.consumed = true;
	return {MarkerOperationStatus::Ok, {}, marker};
}

MarkerOperationResult RelayTraceContext::PopMarker(
	uint64_t generation,
	const std::string &expectedModuleId,
	RelaySiteOrdinal expectedOrdinal,
	bool requireConsumed)
{
	if (m_markers.empty())
	{
		return {
			MarkerOperationStatus::MissingMarker,
			"relay-site marker stack is empty",
			std::nullopt,
		};
	}

	RelaySiteMarkerFrame marker = m_markers.back();
	if (marker.generation != generation ||
		marker.moduleId != expectedModuleId ||
		marker.ordinal != expectedOrdinal ||
		marker.traceTxId != m_transaction.traceTxId)
	{
		return {
			MarkerOperationStatus::StaleMarker,
			"relay-site marker generation does not match the active frame",
			marker,
		};
	}
	if (requireConsumed && !marker.consumed)
	{
		// A marker scope must be restored deterministically even when its
		// relay call was skipped (for example, while unwinding an exception).
		// Keep the diagnostic, but do not strand the frame on the context
		// stack after the generated wrapper has already restored its local
		// marker stack.
		m_markers.pop_back();
		return {
			MarkerOperationStatus::MissingMarker,
			"relay-site marker was restored before relay creation consumed it",
			marker,
		};
	}

	m_markers.pop_back();
	return {MarkerOperationStatus::Ok, {}, marker};
}

size_t RelayTraceContext::OccurrenceKeyHash::operator()(
	const OccurrenceKey &key) const
{
	const size_t left = std::hash<std::string>{}(key.moduleId);
	const size_t right = std::hash<RelaySiteOrdinal>{}(key.ordinal);
	return left ^ (right + 0x9e3779b9U + (left << 6) + (left >> 2));
}

uint32_t RelayTraceContext::NextOccurrence(
	const std::string &moduleId,
	RelaySiteOrdinal ordinal)
{
	uint32_t &next = m_nextOccurrence[OccurrenceKey{moduleId, ordinal}];
	return next++;
}

uint64_t RelayTraceContext::NextEmissionSequence()
{
	return m_nextEmissionSequence++;
}

bool RelayTraceContext::HasMarkers() const
{
	return !m_markers.empty();
}

size_t RelayTraceContext::MarkerDepth() const
{
	return m_markers.size();
}

const char *ToString(MarkerOperationStatus status)
{
	switch (status)
	{
	case MarkerOperationStatus::Ok: return "Ok";
	case MarkerOperationStatus::NoActiveTransaction:
		return "NoActiveTransaction";
	case MarkerOperationStatus::MissingMarker: return "MissingMarker";
	case MarkerOperationStatus::StaleMarker: return "StaleMarker";
	case MarkerOperationStatus::DuplicateConsume: return "DuplicateConsume";
	}
	return "MissingMarker";
}

} // namespace relay_trace
} // namespace oxd
