#include "RelayPlanRegistry.h"

#include <exception>
#include <sstream>
#include <utility>

namespace oxd {
namespace relay_plan {

RelayPlanRegistry::RelayPlanRegistry(
	std::string relayProtocolRoot,
	RelayPlanMetrics *metrics)
	: RelayPlanRegistry(
		std::move(relayProtocolRoot),
		metrics,
		LoadFunction{})
{
}

RelayPlanRegistry::RelayPlanRegistry(
	std::string relayProtocolRoot,
	RelayPlanMetrics *metrics,
	LoadFunction loadFunction)
	: m_loader(std::move(relayProtocolRoot)),
	  m_loadFunction(std::move(loadFunction)),
	  m_metrics(metrics == nullptr ? &m_ownedMetrics : metrics)
{
}

std::string RelayPlanRegistry::CacheKey(
	const ExpectedArtifactBinding &expected)
{
	std::ostringstream key;
	key << RelayPlanLoader::NormalizeIdentity(
				expected.identity.moduleId)
		<< '|'
		<< expected.manifestPath << '\x1f'
		<< expected.identity.dapp << '\x1f'
		<< expected.identity.contract << '\x1f'
		<< expected.identity.transpilerVersion << '\x1f'
		<< expected.identity.intermediateHash << '\x1f'
		<< expected.identity.moduleId << '\x1f'
		<< expected.identity.moduleHashKind << '\x1f'
		<< expected.identity.moduleHash << '\x1f'
		<< RelayPlanLoader::NormalizeIdentity(
				expected.identity.manifestHash)
		<< '\x1f'
		<< RelayPlanLoader::NormalizeIdentity(
				expected.trustedManifestHash)
		<< '\x1f'
		<< expected.requireTranspilerVersion << '\x1f'
		<< expected.requireIntermediateHash << '\x1f'
		<< expected.requireModuleHash << '\x1f'
		<< expected.requireManifestSelfHash;
	return key.str();
}

bool RelayPlanRegistry::IsBindingFailure(
	ManifestLoadStatus status) noexcept
{
	return status == ManifestLoadStatus::ManifestBindingMissing ||
		status == ManifestLoadStatus::ManifestBindingMismatch ||
		status == ManifestLoadStatus::ManifestHashMismatch;
}

std::shared_ptr<const RelayPlanLoadResult>
RelayPlanRegistry::MakeLoaderFailure(
	const ExpectedArtifactBinding &expected,
	const char *diagnostic) noexcept
{
	try
	{
		auto failure = std::make_shared<RelayPlanLoadResult>();
		failure->status = ManifestLoadStatus::ManifestParseError;
		failure->diagnostic = diagnostic == nullptr
			? "relay plan loader failed"
			: diagnostic;
		failure->sourcePath = expected.manifestPath;
		failure->expectedBinding = expected.identity;
		return failure;
	}
	catch (...)
	{
		// A null load result is also a conservative fallback. In particular,
		// allocation failure while preserving a diagnostic must never strand
		// waiters behind a permanently-loading cache entry.
		return nullptr;
	}
}

std::shared_ptr<const RelayPlanLoadResult> RelayPlanRegistry::Load(
	const ExpectedArtifactBinding &expected)
{
	const std::string key = CacheKey(expected);
	const std::string moduleId =
		RelayPlanLoader::NormalizeIdentity(expected.identity.moduleId);
	for (;;)
	{
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			auto found = m_cache.find(key);
			if (found != m_cache.end())
			{
				const std::shared_ptr<CacheEntry> entry =
					found->second;
				m_metrics->planCacheHits.fetch_add(
					1,
					std::memory_order_relaxed);
				entry->ready.wait(lock, [&entry]() {
					return !entry->loading ||
						entry->invalidated;
				});
				if (entry->invalidated)
				{
					// Invalidation removes the entry from m_cache before
					// waking us. Retry against the post-invalidation
					// generation; never return the discarded result.
					continue;
				}
				return entry->result;
			}
		}

		auto entry = std::make_shared<CacheEntry>();
		entry->normalizedModuleId = moduleId;
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			auto found = m_cache.find(key);
			if (found != m_cache.end())
			{
				// Another caller installed the generation after our first
				// miss. Loop so it is observed through the normal waiter
				// path and cache-hit accounting.
				continue;
			}
			entry->isReload =
				!moduleId.empty() &&
				m_invalidatedModules.erase(moduleId) != 0;
			m_cache.emplace(key, entry);
			m_metrics->planCacheMisses.fetch_add(
				1,
				std::memory_order_relaxed);
		}

		// The expensive file I/O, JSON parsing, canonicalization, and hashing
		// are deliberately outside the registry mutex. The placeholder above
		// still gives concurrent callers a single load for this complete
		// binding key.
		m_metrics->planLoads.fetch_add(1, std::memory_order_relaxed);
		std::shared_ptr<const RelayPlanLoadResult> result;
		try
		{
			result = m_loadFunction
				? m_loadFunction(expected)
				: m_loader.Load(expected);
		}
		catch (const std::exception &exception)
		{
			result = MakeLoaderFailure(
				expected,
				exception.what());
		}
		catch (...)
		{
			result = MakeLoaderFailure(
				expected,
				"relay plan loader threw a non-standard exception");
		}
		if (!result || !static_cast<bool>(*result))
		{
			m_metrics->planLoadFailures.fetch_add(
				1,
				std::memory_order_relaxed);
		}
		if (result && IsBindingFailure(result->status))
		{
			m_metrics->planBindingFailures.fetch_add(
				1,
				std::memory_order_relaxed);
		}
		if (entry->isReload)
		{
			m_metrics->planReloads.fetch_add(
				1,
				std::memory_order_relaxed);
		}

		bool invalidatedDuringLoad = false;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			entry->result = result;
			if (!entry->invalidated &&
				!entry->normalizedModuleId.empty() &&
				entry->result &&
				static_cast<bool>(*entry->result))
			{
				try
				{
					m_latestByModule[entry->normalizedModuleId] =
						entry->result;
				}
				catch (...)
				{
					// The binding-key load remains valid even if the
					// convenience module-only index cannot allocate.
				}
			}
			// Publish completion last. Every path, including loader and
			// latest-index failures, reaches this store and wakeup.
			entry->loading = false;
			invalidatedDuringLoad = entry->invalidated;
		}
		entry->ready.notify_all();
		if (invalidatedDuringLoad)
		{
			// Invalidate/Clear won the generation race while I/O was in
			// flight. Discard this result and retry the current generation.
			continue;
		}
		return result;
	}
}

RelayPlanLookupResult RelayPlanRegistry::Lookup(
	const ExpectedArtifactBinding &expected,
	uint32_t opcode)
{
	RelayPlanLookupResult lookup;
	lookup.loadResult = Load(expected);
	if (lookup.loadResult &&
		static_cast<bool>(*lookup.loadResult))
	{
		const BoundRelayManifest &manifest =
			*lookup.loadResult->manifest;
		auto functionId =
			manifest.functionIdByOpcode.find(opcode);
		if (functionId != manifest.functionIdByOpcode.end())
		{
			auto function =
				manifest.functionsById.find(functionId->second);
			if (function != manifest.functionsById.end())
				lookup.function = &function->second;
		}
	}

	if (lookup)
	{
		m_metrics->optimizationEligibleLookups.fetch_add(
			1,
			std::memory_order_relaxed);
	}
	else
	{
		m_metrics->optimizationFallbackLookups.fetch_add(
			1,
			std::memory_order_relaxed);
	}
	return lookup;
}

RelayPlanLookupResult RelayPlanRegistry::Lookup(
	const std::string &moduleId,
	uint32_t opcode) const
{
	RelayPlanLookupResult lookup;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		auto latest = m_latestByModule.find(
			RelayPlanLoader::NormalizeIdentity(moduleId));
		if (latest != m_latestByModule.end())
			lookup.loadResult = latest->second;
	}

	if (lookup.loadResult &&
		static_cast<bool>(*lookup.loadResult))
	{
		const BoundRelayManifest &manifest =
			*lookup.loadResult->manifest;
		auto functionId =
			manifest.functionIdByOpcode.find(opcode);
		if (functionId != manifest.functionIdByOpcode.end())
		{
			auto function =
				manifest.functionsById.find(functionId->second);
			if (function != manifest.functionsById.end())
				lookup.function = &function->second;
		}
	}

	if (lookup.loadResult)
	{
		m_metrics->planCacheHits.fetch_add(
			1,
			std::memory_order_relaxed);
		if (lookup)
		{
			m_metrics->optimizationEligibleLookups.fetch_add(
				1,
				std::memory_order_relaxed);
		}
		else
		{
			m_metrics->optimizationFallbackLookups.fetch_add(
				1,
				std::memory_order_relaxed);
		}
	}
	return lookup;
}

void RelayPlanRegistry::Invalidate(const std::string &moduleId)
{
	const std::string normalized =
		RelayPlanLoader::NormalizeIdentity(moduleId);
	if (normalized.empty())
		return;

	std::lock_guard<std::mutex> lock(m_mutex);
	bool invalidated = false;
	for (auto iterator = m_cache.begin(); iterator != m_cache.end();)
	{
		if (iterator->second->normalizedModuleId == normalized)
		{
			iterator->second->invalidated = true;
			iterator->second->ready.notify_all();
			iterator = m_cache.erase(iterator);
			invalidated = true;
		}
		else
		{
			++iterator;
		}
	}
	invalidated =
		m_latestByModule.erase(normalized) != 0 || invalidated;
	m_invalidatedModules.insert(normalized);
	if (invalidated)
	{
		m_metrics->planInvalidations.fetch_add(
			1,
			std::memory_order_relaxed);
	}
}

void RelayPlanRegistry::Clear()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_cache.empty() && m_latestByModule.empty())
		return;

	for (auto &entry : m_cache)
	{
		entry.second->invalidated = true;
		entry.second->ready.notify_all();
		if (!entry.second->normalizedModuleId.empty())
			m_invalidatedModules.insert(
				entry.second->normalizedModuleId);
	}
	for (const auto &entry : m_latestByModule)
		m_invalidatedModules.insert(entry.first);
	m_cache.clear();
	m_latestByModule.clear();
	m_metrics->planInvalidations.fetch_add(
		1,
		std::memory_order_relaxed);
}

} // namespace relay_plan
} // namespace oxd
