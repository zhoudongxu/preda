#pragma once

#include "RelayTraceTypes.h"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace oxd {
namespace relay_trace {

struct ExpectedArtifactBinding
{
	ArtifactIdentity identity;

	// Optional exact file. Otherwise Loader scans
	// <relayProtocolRoot>/by_module for schema-v5 JSON sidecars.
	std::string manifestPath;

	bool requireTranspilerVersion = true;
	bool requireIntermediateHash = true;
	bool requireModuleHash = false;
	bool requireManifestSelfHash = true;

	// If non-empty, the manifest's declared and recomputed self hash must also
	// match this independently trusted value.
	std::string trustedManifestHash;
};

struct ManifestRelaySite
{
	RelaySiteOrdinal ordinal = InvalidRelaySiteOrdinal;
	std::string id;
	std::string sourceFunctionId;
	std::string handlerId;
	uint32_t expectedOpcode = 0;
	bool handlerResolved = false;
	RelayKind relayKind = RelayKind::Unknown;
	ScopeKind targetScope = ScopeKind::Unknown;
	TraceSourceLocation location;
};

struct ManifestFunctionSummary
{
	std::string sourceFunctionId;
	std::optional<uint64_t> exactDirectRelayCount;
	std::optional<uint64_t> directRelayCountUpperBound;
	std::optional<uint32_t> maximumDepth;
	bool hasOpaque = false;
	bool hasUnmodeledRelayReachableCall = false;
	std::vector<std::string> relaySiteIds;
	std::vector<std::string> fanoutKinds;
};

struct ManifestNonAliasProof
{
	std::string obligationId;
	std::string leftRelaySiteId;
	std::string rightRelaySiteId;
	bool solverProved = false;
};

struct LoadedRelayManifest
{
	uint32_t schemaVersion = 0;
	std::string sourcePath;
	ArtifactIdentity binding;
	std::string recomputedManifestHash;

	std::unordered_map<RelaySiteOrdinal, ManifestRelaySite> sitesByOrdinal;
	std::unordered_map<std::string, RelaySiteOrdinal> ordinalBySiteId;
	std::unordered_map<std::string, ManifestFunctionSummary> functionsById;
	std::unordered_map<uint32_t, std::string> functionIdByOpcode;
	std::vector<ManifestNonAliasProof> nonAliasProofs;
};

struct RelayManifestLoadResult
{
	ManifestLoadStatus status = ManifestLoadStatus::ManifestNotFound;
	std::string diagnostic;
	std::string sourcePath;
	ArtifactIdentity expectedBinding;
	ArtifactIdentity observedBinding;
	std::shared_ptr<const LoadedRelayManifest> manifest;
	double elapsedTimeMs = 0.0;

	explicit operator bool() const
	{
		return status == ManifestLoadStatus::Loaded && manifest != nullptr;
	}
};

class RelayManifestLoader
{
public:
	explicit RelayManifestLoader(std::string relayProtocolRoot);

	std::shared_ptr<const RelayManifestLoadResult> Load(
		const ExpectedArtifactBinding &expected);

	void Invalidate(const std::string &moduleId);
	void Clear();

	// Hash definition shared with the binding emitter: SHA-256 of compact
	// ordered JSON after removing artifact_binding.manifest_hash.
	static std::string ComputeManifestSelfHash(
		const std::string &manifestText,
		std::string *error = nullptr);
	static std::string RuntimeHashIdentity(const rvm::HashValue &hash);

private:
	std::shared_ptr<RelayManifestLoadResult> LoadUncached(
		const ExpectedArtifactBinding &expected) const;

	std::string m_relayProtocolRoot;
	mutable std::mutex m_mutex;
	std::unordered_map<
		std::string,
		std::shared_ptr<const RelayManifestLoadResult>> m_cache;
};

} // namespace relay_trace
} // namespace oxd
