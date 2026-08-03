#pragma once

#include "RelayPlanLoader.h"
#include "RelayPlanMetrics.h"

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace oxd {
namespace relay_plan {

struct RelayPlanLookupResult
{
	std::shared_ptr<const RelayPlanLoadResult> loadResult;
	const FunctionRelayPlan *function = nullptr;

	explicit operator bool() const noexcept
	{
		return loadResult && static_cast<bool>(*loadResult) &&
			function != nullptr && function->optimizationEligible;
	}
};

class RelayPlanRegistry
{
public:
	using LoadFunction = std::function<
		std::shared_ptr<RelayPlanLoadResult>(
			const ExpectedArtifactBinding &)>;

	explicit RelayPlanRegistry(
		std::string relayProtocolRoot,
		RelayPlanMetrics *metrics = nullptr);
	RelayPlanRegistry(
		std::string relayProtocolRoot,
		RelayPlanMetrics *metrics,
		LoadFunction loadFunction);

	std::shared_ptr<const RelayPlanLoadResult> Load(
		const ExpectedArtifactBinding &expected);

	RelayPlanLookupResult Lookup(
		const ExpectedArtifactBinding &expected,
		uint32_t opcode);
	RelayPlanLookupResult Lookup(
		const std::string &moduleId,
		uint32_t opcode) const;

	void Invalidate(const std::string &moduleId);
	void Clear();

	RelayPlanMetrics &Metrics() noexcept
	{
		return *m_metrics;
	}
	const RelayPlanMetrics &Metrics() const noexcept
	{
		return *m_metrics;
	}

	static std::string ComputeManifestSelfHash(
		const std::string &manifestText,
		std::string *error = nullptr)
	{
		return RelayPlanLoader::ComputeManifestSelfHash(
			manifestText,
			error);
	}

	static std::string RuntimeHashIdentity(const rvm::HashValue &hash)
	{
		return RelayPlanLoader::RuntimeHashIdentity(hash);
	}

private:
	struct CacheEntry
	{
		bool loading = true;
		bool invalidated = false;
		bool isReload = false;
		std::string normalizedModuleId;
		std::shared_ptr<const RelayPlanLoadResult> result;
		std::condition_variable ready;
	};

	static std::string CacheKey(const ExpectedArtifactBinding &expected);
	static bool IsBindingFailure(ManifestLoadStatus status) noexcept;
	static std::shared_ptr<const RelayPlanLoadResult>
		MakeLoaderFailure(
			const ExpectedArtifactBinding &expected,
			const char *diagnostic) noexcept;

	RelayPlanLoader m_loader;
	LoadFunction m_loadFunction;
	mutable std::mutex m_mutex;
	std::unordered_map<std::string, std::shared_ptr<CacheEntry>> m_cache;
	std::unordered_map<
		std::string,
		std::shared_ptr<const RelayPlanLoadResult>> m_latestByModule;
	std::unordered_set<std::string> m_invalidatedModules;
	RelayPlanMetrics m_ownedMetrics;
	RelayPlanMetrics *m_metrics = nullptr;
};

} // namespace relay_plan
} // namespace oxd
