#include "RelayOptimizationAudit.h"

#include <algorithm>

namespace oxd {
namespace relay_optimization {
namespace {

constexpr uint64_t FnvOffsetBasis = 14695981039346656037ULL;
constexpr uint64_t FnvPrime = 1099511628211ULL;
constexpr char AuditDomain[] = "rpreda-audit-v1";
constexpr char AuditOccurrenceDomain[] =
	"rpreda-audit-root-occurrence-v1";

uint64_t FnvAppend(
	uint64_t hash,
	const uint8_t *data,
	size_t size) noexcept
{
	for(size_t i = 0; i < size; ++i)
	{
		hash ^= data[i];
		hash *= FnvPrime;
	}
	return hash;
}

uint64_t AppendUint64BigEndian(
	uint64_t hash,
	uint64_t value) noexcept
{
	for(size_t byte = 0; byte < sizeof(value); ++byte)
	{
		const size_t shift = (sizeof(value) - byte - 1) * 8;
		const uint8_t encoded =
			static_cast<uint8_t>((value >> shift) & 0xff);
		hash = FnvAppend(hash, &encoded, 1);
	}
	return hash;
}

} // namespace

const char *ToString(AuditCheckKind value) noexcept
{
	switch(value)
	{
	case AuditCheckKind::DirectRelayCount:
		return "direct_relay_count";
	case AuditCheckKind::RelayCountUpperBound:
		return "relay_count_upper_bound";
	case AuditCheckKind::LogicalClassification:
		return "logical_classification";
	case AuditCheckKind::DestinationShard:
		return "destination_shard";
	case AuditCheckKind::BatchElementCount:
		return "batch_element_count";
	case AuditCheckKind::BatchOrder:
		return "batch_order";
	case AuditCheckKind::BroadcastCloneCount:
		return "broadcast_clone_count";
	case AuditCheckKind::InjectedFailure:
		return "injected_failure";
	}
	return "unknown";
}

RelayOptimizationAudit::RelayOptimizationAudit(
	AuditSampleRate sampleRate,
	OptimizationMetrics *metrics) noexcept
	: m_sampleRate(sampleRate)
	, m_metrics(metrics)
{
}

uint64_t RelayOptimizationAudit::StableRootIdentity(
	const uint8_t *canonicalBytes,
	size_t size) const noexcept
{
	const AuditIdentityPart part{canonicalBytes, size};
	return StableRootIdentity(&part, 1);
}

uint64_t RelayOptimizationAudit::StableRootIdentity(
	const AuditIdentityPart *parts,
	size_t partCount) const noexcept
{
	uint64_t hash = FnvAppend(
		FnvOffsetBasis,
		reinterpret_cast<const uint8_t *>(AuditDomain),
		sizeof(AuditDomain) - 1);
	const uint8_t separator = 0;
	hash = FnvAppend(hash, &separator, 1);
	for(size_t index = 0; parts && index < partCount; ++index)
	{
		if(parts[index].data && parts[index].size)
		{
			hash = FnvAppend(
				hash,
				parts[index].data,
				parts[index].size);
		}
	}
	return hash;
}

bool RelayOptimizationAudit::RegisterSourceIssuance(
	const void *transaction) noexcept
{
	if(!transaction)
		return true;

	bool allocationFailed = false;
	bool ordinalOverflow = false;
	bool duplicatePointer = false;
	try
	{
		std::lock_guard<std::mutex> lock(m_failureMutex);
		if(m_nextSourceIssuanceOrdinal == UINT64_MAX)
		{
			ordinalOverflow = true;
		}
		else
		{
			const uint64_t ordinal =
				m_nextSourceIssuanceOrdinal++;
			const auto inserted =
				m_sourceIssuanceOrdinals.emplace(
					transaction,
					ordinal);
			if(!inserted.second)
			{
				duplicatePointer = true;
				inserted.first->second = ordinal;
			}
		}
	}
	catch(...)
	{
		// Issuance tracking is audit-only side metadata. Never let an
		// allocation failure prevent the source transaction from being queued.
		allocationFailed = true;
	}

	if(allocationFailed || ordinalOverflow || duplicatePointer)
	{
		FailSample(
			AuditCheckKind::LogicalClassification,
			0,
			1,
			0,
			allocationFailed
				? "audit source-issuance allocation failed"
				: ordinalOverflow
					? "audit source-issuance ordinal overflow"
					: "audit source pointer was issued more than once");
		return false;
	}
	return true;
}

std::optional<uint64_t>
RelayOptimizationAudit::ConsumeSourceIssuance(
	const void *transaction) noexcept
{
	if(!transaction)
		return std::nullopt;

	std::optional<uint64_t> ordinal;
	bool metadataFailure = false;
	try
	{
		std::lock_guard<std::mutex> lock(m_failureMutex);
		const auto found =
			m_sourceIssuanceOrdinals.find(transaction);
		if(found != m_sourceIssuanceOrdinals.end())
		{
			ordinal = found->second;
			m_sourceIssuanceOrdinals.erase(found);
		}
	}
	catch(...)
	{
		metadataFailure = true;
	}
	if(!ordinal)
	{
		FailSample(
			AuditCheckKind::LogicalClassification,
			0,
			1,
			0,
			metadataFailure
				? "audit source-issuance lookup failed"
				: "audit source transaction has no issuance identity");
	}
	return ordinal;
}

uint64_t RelayOptimizationAudit::RootIdentityForIssuance(
	uint64_t canonicalIdentity,
	uint64_t issuanceOrdinal) const noexcept
{
	uint64_t hash = FnvAppend(
		FnvOffsetBasis,
		reinterpret_cast<const uint8_t *>(
			AuditOccurrenceDomain),
		sizeof(AuditOccurrenceDomain) - 1);
	const uint8_t separator = 0;
	hash = FnvAppend(hash, &separator, 1);
	hash = AppendUint64BigEndian(hash, canonicalIdentity);
	hash = AppendUint64BigEndian(hash, issuanceOrdinal);
	return hash;
}

bool RelayOptimizationAudit::ShouldSample(
	uint64_t rootIdentity) const noexcept
{
	return m_sampleRate.denominator != 0 &&
		(rootIdentity % m_sampleRate.denominator) <
			m_sampleRate.numerator;
}

void RelayOptimizationAudit::BeginSample() noexcept
{
	if(m_metrics)
	{
		m_metrics->auditSamples.fetch_add(
			1,
			std::memory_order_relaxed);
	}
}

void RelayOptimizationAudit::PassSample() noexcept
{
	if(m_metrics)
	{
		m_metrics->auditPassed.fetch_add(
			1,
			std::memory_order_relaxed);
	}
}

void RelayOptimizationAudit::FailSample(
	const AuditFailure &failure) noexcept
{
	FailSample(
		failure.kind,
		failure.rootIdentity,
		failure.expected,
		failure.actual,
		failure.reason.c_str());
}

void RelayOptimizationAudit::FailSample(
	AuditCheckKind kind,
	uint64_t rootIdentity,
	uint64_t expectedValue,
	uint64_t actualValue,
	const char *reason) noexcept
{
	bool expectedLatch = false;
	const bool firstFailure = m_failed.compare_exchange_strong(
		expectedLatch,
		true,
		std::memory_order_acq_rel,
		std::memory_order_acquire);
	if(firstFailure)
	{
		m_firstFailureFallbackKind.store(
			static_cast<uint8_t>(kind),
			std::memory_order_relaxed);
		m_firstFailureFallbackRoot.store(
			rootIdentity,
			std::memory_order_relaxed);
		m_firstFailureFallbackExpected.store(
			expectedValue,
			std::memory_order_relaxed);
		m_firstFailureFallbackActual.store(
			actualValue,
			std::memory_order_relaxed);
		m_firstFailureFallbackReady.store(
			true,
			std::memory_order_release);
	}

	bool firstForRoot = false;
	bool rootTrackingFailed = false;
	{
		try
		{
			std::lock_guard<std::mutex> lock(m_failureMutex);
			firstForRoot = m_failedRoots.insert(rootIdentity).second;
			if(firstFailure)
			{
				m_firstFailure.emplace();
				m_firstFailure->kind = kind;
				m_firstFailure->rootIdentity = rootIdentity;
				m_firstFailure->expected = expectedValue;
				m_firstFailure->actual = actualValue;
				if(reason)
					m_firstFailure->reason = reason;
			}
		}
		catch(...)
		{
			// The audit is side metadata. Even allocation failure while
			// preserving diagnostics must never escape into a worker or undo
			// an already committed queue operation.
			rootTrackingFailed = true;
		}
	}
	if(m_metrics &&
		(firstForRoot || (firstFailure && rootTrackingFailed)))
	{
		m_metrics->auditFailed.fetch_add(
			1,
			std::memory_order_relaxed);
	}
}

const char *RelayOptimizationAudit::DefaultFailureReason(
	AuditCheckKind kind) noexcept
{
	switch(kind)
	{
	case AuditCheckKind::DirectRelayCount:
		return "direct relay count audit failed";
	case AuditCheckKind::RelayCountUpperBound:
		return "relay count upper-bound audit failed";
	case AuditCheckKind::LogicalClassification:
		return "logical relay classification audit failed";
	case AuditCheckKind::DestinationShard:
		return "destination shard audit failed";
	case AuditCheckKind::BatchElementCount:
		return "batch element-count audit failed";
	case AuditCheckKind::BatchOrder:
		return "batch identity/order audit failed";
	case AuditCheckKind::BroadcastCloneCount:
		return "broadcast clone audit failed";
	case AuditCheckKind::InjectedFailure:
		return "test-only injected audit failure";
	}
	return "runtime optimization audit failed";
}

void RelayOptimizationAudit::RegisterSampledTransaction(
	const void *transaction,
	uint64_t rootIdentity,
	bool isRoot) noexcept
{
	if(!transaction)
		return;
	bool insertedRoot = false;
	try
	{
		std::lock_guard<std::mutex> lock(m_failureMutex);
		m_transactionRoots[transaction] = rootIdentity;
		if(isRoot)
			insertedRoot = m_sampledRoots.insert(rootIdentity).second;
	}
	catch(...)
	{
		FailSample(
			AuditCheckKind::LogicalClassification,
			rootIdentity,
			1,
			0,
			"audit side metadata allocation failed");
		return;
	}
	if(insertedRoot)
		BeginSample();
}

void RelayOptimizationAudit::InheritTransaction(
	const void *parent,
	const void *child) noexcept
{
	if(!parent || !child)
		return;
	try
	{
		std::lock_guard<std::mutex> lock(m_failureMutex);
		const auto found = m_transactionRoots.find(parent);
		if(found != m_transactionRoots.end())
			m_transactionRoots[child] = found->second;
	}
	catch(...)
	{
		// Failure to allocate audit-only metadata cannot affect execution.
		// Latch a deterministic audit failure when the parent was sampled.
		const auto root = RootForTransaction(parent);
		if(root)
		{
			FailSample(
				AuditCheckKind::LogicalClassification,
				*root,
				1,
				0,
				"audit lineage allocation failed");
		}
	}
}

std::optional<uint64_t>
RelayOptimizationAudit::RootForTransaction(
	const void *transaction) const noexcept
{
	if(!transaction)
		return std::nullopt;
	try
	{
		std::lock_guard<std::mutex> lock(m_failureMutex);
		const auto found = m_transactionRoots.find(transaction);
		if(found != m_transactionRoots.end())
			return found->second;
	}
	catch(...)
	{
	}
	return std::nullopt;
}

void RelayOptimizationAudit::ForgetTransaction(
	const void *transaction) noexcept
{
	if(!transaction)
		return;
	try
	{
		std::lock_guard<std::mutex> lock(m_failureMutex);
		m_transactionRoots.erase(transaction);
	}
	catch(...)
	{
	}
}

void RelayOptimizationAudit::FinalizeSamples() noexcept
{
	uint64_t passed = 0;
	try
	{
		std::lock_guard<std::mutex> lock(m_failureMutex);
		for(uint64_t root : m_sampledRoots)
		{
			if(m_failedRoots.find(root) == m_failedRoots.end())
				++passed;
		}
		m_transactionRoots.clear();
		m_sampledRoots.clear();
		m_failedRoots.clear();
		m_sourceIssuanceOrdinals.clear();
	}
	catch(...)
	{
	}
	if(m_metrics && passed)
	{
		m_metrics->auditPassed.fetch_add(
			passed,
			std::memory_order_relaxed);
	}
}

bool RelayOptimizationAudit::CheckDirectRelayCount(
	uint64_t rootIdentity,
	std::optional<uint64_t> exact,
	std::optional<uint64_t> upperBound,
	uint64_t actual) noexcept
{
	if(ConsumeInjectedFailure(
		AuditCheckKind::DirectRelayCount,
		rootIdentity))
	{
		return false;
	}
	if(exact && actual != *exact)
	{
		FailSample(
			AuditCheckKind::DirectRelayCount,
			rootIdentity,
			*exact,
			actual,
			"actual direct relay count differs from the trusted exact count");
		return false;
	}
	if(upperBound && actual > *upperBound)
	{
		FailSample(
			AuditCheckKind::RelayCountUpperBound,
			rootIdentity,
			*upperBound,
			actual,
			"actual direct relay count exceeds the trusted upper bound");
		return false;
	}
	return true;
}

bool RelayOptimizationAudit::CheckClassifiedExactlyOnce(
	uint64_t rootIdentity,
	uint64_t classificationCount) noexcept
{
	if(ConsumeInjectedFailure(
		AuditCheckKind::LogicalClassification,
		rootIdentity))
	{
		return false;
	}
	if(classificationCount == 1)
		return true;
	FailSample(
		AuditCheckKind::LogicalClassification,
		rootIdentity,
		1,
		classificationCount,
		"logical relay was not classified exactly once");
	return false;
}

bool RelayOptimizationAudit::CheckDestinationShard(
	uint64_t rootIdentity,
	uint32_t expected,
	uint32_t actual) noexcept
{
	if(ConsumeInjectedFailure(
		AuditCheckKind::DestinationShard,
		rootIdentity))
	{
		return false;
	}
	if(expected == actual)
		return true;
	FailSample(
		AuditCheckKind::DestinationShard,
		rootIdentity,
		expected,
		actual,
		"destination differs from the original GetShardIndex result");
	return false;
}

bool RelayOptimizationAudit::CheckBatch(
	uint64_t rootIdentity,
	const std::vector<uint64_t> &inputIdentities,
	const std::vector<uint64_t> &insertedIdentities) noexcept
{
	if(ConsumeInjectedFailure(
		AuditCheckKind::BatchElementCount,
		rootIdentity))
	{
		return false;
	}
	if(inputIdentities.size() != insertedIdentities.size())
	{
		FailSample(
			AuditCheckKind::BatchElementCount,
			rootIdentity,
			static_cast<uint64_t>(inputIdentities.size()),
			static_cast<uint64_t>(insertedIdentities.size()),
			"queue insertion changed the batch element count");
		return false;
	}
	if(ConsumeInjectedFailure(
		AuditCheckKind::BatchOrder,
		rootIdentity))
	{
		return false;
	}
	if(inputIdentities == insertedIdentities)
		return true;
	FailSample(
		AuditCheckKind::BatchOrder,
		rootIdentity,
		static_cast<uint64_t>(inputIdentities.size()),
		static_cast<uint64_t>(insertedIdentities.size()),
		"queue insertion changed batch order or element identity");
	return false;
}

bool RelayOptimizationAudit::CheckBatchObservation(
	uint64_t rootIdentity,
	uint64_t expectedCount,
	uint64_t queueSizeDelta,
	uint64_t observedTailCount,
	bool identityAndOrderPreserved) noexcept
{
	if(ConsumeInjectedFailure(
		AuditCheckKind::BatchElementCount,
		rootIdentity))
	{
		return false;
	}
	if(queueSizeDelta != expectedCount)
	{
		FailSample(
			AuditCheckKind::BatchElementCount,
			rootIdentity,
			expectedCount,
			queueSizeDelta,
			"queue size delta differs from the committed batch size");
		return false;
	}
	if(observedTailCount != expectedCount)
	{
		FailSample(
			AuditCheckKind::BatchElementCount,
			rootIdentity,
			expectedCount,
			observedTailCount,
			"observed queue tail differs from the committed batch size");
		return false;
	}
	if(ConsumeInjectedFailure(
		AuditCheckKind::BatchOrder,
		rootIdentity))
	{
		return false;
	}
	if(identityAndOrderPreserved)
		return true;
	FailSample(
		AuditCheckKind::BatchOrder,
		rootIdentity,
		expectedCount,
		observedTailCount,
		"queue tail changed batch order or element identity");
	return false;
}

bool RelayOptimizationAudit::CheckBroadcastClones(
	uint64_t rootIdentity,
	uint32_t activeShardCount,
	const std::vector<uint32_t> &destinationShards) noexcept
{
	if(ConsumeInjectedFailure(
		AuditCheckKind::BroadcastCloneCount,
		rootIdentity))
	{
		return false;
	}
	if(destinationShards.size() != activeShardCount)
	{
		FailSample(
			AuditCheckKind::BroadcastCloneCount,
			rootIdentity,
			activeShardCount,
			static_cast<uint64_t>(destinationShards.size()),
			"broadcast physical clone count differs from active shard count");
		return false;
	}
	for(uint32_t expectedShard = 0;
		expectedShard < activeShardCount;
		++expectedShard)
	{
		uint64_t occurrences = 0;
		for(uint32_t observedShard : destinationShards)
		{
			if(observedShard == expectedShard)
				++occurrences;
		}
		if(occurrences != 1)
		{
			FailSample(
				AuditCheckKind::BroadcastCloneCount,
				rootIdentity,
				1,
				occurrences,
				"broadcast destination is missing or duplicated");
			return false;
		}
	}
	return true;
}

bool RelayOptimizationAudit::CheckBroadcastClones(
	uint64_t rootIdentity,
	uint32_t activeShardCount,
	uint64_t observedPhysicalCount,
	bool exactlyOnePerDestination) noexcept
{
	if(ConsumeInjectedFailure(
		AuditCheckKind::BroadcastCloneCount,
		rootIdentity))
	{
		return false;
	}
	if(observedPhysicalCount != activeShardCount)
	{
		FailSample(
			AuditCheckKind::BroadcastCloneCount,
			rootIdentity,
			activeShardCount,
			observedPhysicalCount,
			"broadcast physical clone count differs from active shard count");
		return false;
	}
	if(exactlyOnePerDestination)
		return true;
	FailSample(
		AuditCheckKind::BroadcastCloneCount,
		rootIdentity,
		activeShardCount,
		observedPhysicalCount,
		"broadcast destination coverage is incomplete or duplicated");
	return false;
}

std::optional<AuditFailure>
RelayOptimizationAudit::FirstFailure() const noexcept
{
	try
	{
		std::lock_guard<std::mutex> lock(m_failureMutex);
		if(m_firstFailure)
			return m_firstFailure;
	}
	catch(...)
	{
	}
	if(!m_firstFailureFallbackReady.load(std::memory_order_acquire))
		return std::nullopt;
	try
	{
		AuditFailure failure;
		failure.kind = static_cast<AuditCheckKind>(
			m_firstFailureFallbackKind.load(
				std::memory_order_relaxed));
		failure.rootIdentity =
			m_firstFailureFallbackRoot.load(
				std::memory_order_relaxed);
		failure.expected =
			m_firstFailureFallbackExpected.load(
				std::memory_order_relaxed);
		failure.actual =
			m_firstFailureFallbackActual.load(
				std::memory_order_relaxed);
		failure.reason = DefaultFailureReason(failure.kind);
		return failure;
	}
	catch(...)
	{
		return std::nullopt;
	}
}

bool RelayOptimizationAudit::ConsumeInjectedFailure(
	AuditCheckKind kind,
	uint64_t rootIdentity) noexcept
{
#ifdef RPREDA_RUNTIME_OPTIMIZATION_TESTING
	int expected = static_cast<int>(kind);
	if(m_injectedFailure.compare_exchange_strong(
		expected,
		-1,
		std::memory_order_acq_rel,
		std::memory_order_acquire))
	{
		FailSample(
			AuditCheckKind::InjectedFailure,
			rootIdentity,
			0,
			0,
			"test-only injected audit mismatch");
		return true;
	}
#else
	(void)kind;
	(void)rootIdentity;
#endif
	return false;
}

} // namespace relay_optimization
} // namespace oxd
