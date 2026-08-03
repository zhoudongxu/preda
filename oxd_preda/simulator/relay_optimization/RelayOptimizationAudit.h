#pragma once

#include "RelayOptimizationMetrics.h"
#include "RelayOptimizationTypes.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace oxd {
namespace relay_optimization {

struct AuditIdentityPart
{
	const uint8_t *data = nullptr;
	size_t size = 0;
};

enum class AuditCheckKind : uint8_t
{
	DirectRelayCount,
	RelayCountUpperBound,
	LogicalClassification,
	DestinationShard,
	BatchElementCount,
	BatchOrder,
	BroadcastCloneCount,
	InjectedFailure,
};

struct AuditFailure
{
	AuditCheckKind kind = AuditCheckKind::DirectRelayCount;
	uint64_t rootIdentity = 0;
	uint64_t expected = 0;
	uint64_t actual = 0;
	std::string reason;
};

const char *ToString(AuditCheckKind value) noexcept;

class RelayOptimizationAudit
{
public:
	RelayOptimizationAudit(
		AuditSampleRate sampleRate,
		OptimizationMetrics *metrics) noexcept;

	uint64_t StableRootIdentity(
			const uint8_t *canonicalBytes,
			size_t size) const noexcept;
	uint64_t StableRootIdentity(
			const AuditIdentityPart *parts,
			size_t partCount) const noexcept;
	// Source issuance is recorded before queue ownership is transferred.
	// Worker execution later consumes this side metadata, so identical
	// transactions executing on different shards never derive identity from
	// worker lock-acquisition order.
	bool RegisterSourceIssuance(
			const void *transaction) noexcept;
	std::optional<uint64_t> ConsumeSourceIssuance(
			const void *transaction) noexcept;
	uint64_t RootIdentityForIssuance(
			uint64_t canonicalIdentity,
			uint64_t issuanceOrdinal) const noexcept;
	bool ShouldSample(uint64_t rootIdentity) const noexcept;

	void BeginSample() noexcept;
	void PassSample() noexcept;
	void FailSample(const AuditFailure &failure) noexcept;
	void FailSample(
			AuditCheckKind kind,
			uint64_t rootIdentity,
			uint64_t expected,
			uint64_t actual,
			const char *reason) noexcept;

	// Pointer keys are observation-only side metadata. They never enter
	// SimuTxn, transaction serialization, hashing, routing, or scheduling.
	void RegisterSampledTransaction(
		const void *transaction,
		uint64_t rootIdentity,
		bool isRoot) noexcept;
	void InheritTransaction(
		const void *parent,
		const void *child) noexcept;
	std::optional<uint64_t> RootForTransaction(
		const void *transaction) const noexcept;
	template<typename Transaction>
	bool SnapshotUniqueTransactionRoots(
		Transaction *const *transactions,
		size_t count,
		std::vector<uint64_t> &roots) noexcept
	{
		roots.clear();
		if(!transactions || count == 0)
			return true;

		std::optional<uint64_t> firstRoot;
		try
		{
			std::lock_guard<std::mutex> lock(m_failureMutex);
			for(size_t index = 0; index < count; ++index)
			{
				const auto found =
					m_transactionRoots.find(transactions[index]);
				if(found != m_transactionRoots.end())
				{
					firstRoot = found->second;
					break;
				}
			}
			if(!firstRoot)
				return true;

			const size_t reserveCount =
				count < m_sampledRoots.size()
					? count
					: m_sampledRoots.size();
			roots.reserve(reserveCount);
			for(size_t index = 0; index < count; ++index)
			{
				const auto found =
					m_transactionRoots.find(transactions[index]);
				if(found == m_transactionRoots.end())
					continue;
				bool duplicate = false;
				for(uint64_t root : roots)
				{
					if(root == found->second)
					{
						duplicate = true;
						break;
					}
				}
				if(!duplicate)
					roots.push_back(found->second);
			}
			return true;
		}
		catch(...)
		{
			// Snapshotting is audit-only and happens before queue ownership is
			// transferred. Preserve execution, but make loss of the check
			// visible at the normal simulator safe point.
			roots.clear();
			FailSample(
				AuditCheckKind::BatchElementCount,
				firstRoot.value_or(0),
				static_cast<uint64_t>(count),
				0,
				"audit batch-root snapshot failed");
			return false;
		}
	}
	void ForgetTransaction(const void *transaction) noexcept;
	void FinalizeSamples() noexcept;

	bool CheckDirectRelayCount(
		uint64_t rootIdentity,
		std::optional<uint64_t> exact,
		std::optional<uint64_t> upperBound,
		uint64_t actual) noexcept;
	bool CheckClassifiedExactlyOnce(
		uint64_t rootIdentity,
		uint64_t classificationCount) noexcept;
	bool CheckDestinationShard(
		uint64_t rootIdentity,
		uint32_t expected,
		uint32_t actual) noexcept;
	bool CheckBatch(
			uint64_t rootIdentity,
			const std::vector<uint64_t> &inputIdentities,
			const std::vector<uint64_t> &insertedIdentities) noexcept;
	bool CheckBatchObservation(
			uint64_t rootIdentity,
			uint64_t expectedCount,
			uint64_t queueSizeDelta,
			uint64_t observedTailCount,
			bool identityAndOrderPreserved) noexcept;
	bool CheckBroadcastClones(
			uint64_t rootIdentity,
			uint32_t activeShardCount,
			const std::vector<uint32_t> &destinationShards) noexcept;
	bool CheckBroadcastClones(
			uint64_t rootIdentity,
			uint32_t activeShardCount,
			uint64_t observedPhysicalCount,
			bool exactlyOnePerDestination) noexcept;

	bool FailureLatched() const noexcept
	{
		return m_failed.load(std::memory_order_acquire);
	}
	std::optional<AuditFailure> FirstFailure() const noexcept;

#ifdef RPREDA_RUNTIME_OPTIMIZATION_TESTING
	void InjectFailureOnce(AuditCheckKind kind) noexcept
	{
		m_injectedFailure.store(
			static_cast<int>(kind),
			std::memory_order_release);
	}
#endif

private:
	bool ConsumeInjectedFailure(
			AuditCheckKind kind,
			uint64_t rootIdentity) noexcept;
	static const char *DefaultFailureReason(
			AuditCheckKind kind) noexcept;

	AuditSampleRate m_sampleRate;
	OptimizationMetrics *m_metrics = nullptr;
	std::atomic<bool> m_failed{false};
	std::atomic<bool> m_firstFailureFallbackReady{false};
	std::atomic<uint8_t> m_firstFailureFallbackKind{
		static_cast<uint8_t>(AuditCheckKind::DirectRelayCount)};
	std::atomic<uint64_t> m_firstFailureFallbackRoot{0};
	std::atomic<uint64_t> m_firstFailureFallbackExpected{0};
	std::atomic<uint64_t> m_firstFailureFallbackActual{0};
	mutable std::mutex m_failureMutex;
	std::optional<AuditFailure> m_firstFailure;
	uint64_t m_nextSourceIssuanceOrdinal = 0;
	std::unordered_map<const void *, uint64_t>
		m_sourceIssuanceOrdinals;
	std::unordered_map<const void *, uint64_t> m_transactionRoots;
	std::unordered_set<uint64_t> m_sampledRoots;
	std::unordered_set<uint64_t> m_failedRoots;
#ifdef RPREDA_RUNTIME_OPTIMIZATION_TESTING
	std::atomic<int> m_injectedFailure{-1};
#endif
};

} // namespace relay_optimization
} // namespace oxd
