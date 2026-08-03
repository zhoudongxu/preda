#pragma once

#include "RelayPlan.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace oxd {
namespace relay_plan {

using RelaySiteOrdinal = uint32_t;

constexpr RelaySiteOrdinal InvalidRelaySiteOrdinal =
	std::numeric_limits<RelaySiteOrdinal>::max();

enum class RelayKind : uint8_t
{
	CustomScope,
	Global,
	AllShards,
	DeferredNext,
	Unknown,
};

enum class ScopeKind : uint8_t
{
	None,
	Global,
	Shard,
	Address,
	Uint32,
	Uint64,
	Uint96,
	Uint128,
	Uint160,
	Uint256,
	Uint512,
	Unknown,
};

enum class ManifestLoadStatus : uint8_t
{
	ManifestNotFound,
	ManifestParseError,
	ManifestSchemaUnsupported,
	ManifestBindingMissing,
	ManifestBindingMismatch,
	ManifestHashMismatch,
	Loaded,
};

struct RelaySourceLocation
{
	int64_t line = 0;
	int64_t column = 0;
	int64_t endLine = 0;
	int64_t endColumn = 0;
	int64_t startOffset = -1;
	int64_t endOffset = -1;
};

struct ArtifactIdentity
{
	std::string dapp;
	std::string contract;
	std::string transpilerVersion;
	std::string intermediateHash;
	std::string moduleId;
	std::string moduleHashKind;
	std::string moduleHash;
	std::string manifestHashAlgorithm;
	std::string manifestHash;
	bool bindingComplete = false;
};

struct ExpectedArtifactBinding
{
	ArtifactIdentity identity;

	// Optional exact file. Otherwise the loader uses
	// <relayProtocolRoot>/by_module/<module-id>.relay_protocol.json.
	std::string manifestPath;

	bool requireTranspilerVersion = true;
	bool requireIntermediateHash = true;
	bool requireModuleHash = false;
	bool requireManifestSelfHash = true;

	// This value comes from the independently persisted module database, not
	// from the sidecar being verified.
	std::string trustedManifestHash;
};

struct BoundRelaySite
{
	RelaySiteOrdinal ordinal = InvalidRelaySiteOrdinal;
	std::string id;
	std::string sourceFunctionId;
	std::string handlerId;
	uint32_t expectedOpcode = 0;
	bool handlerResolved = false;
	RelayKind relayKind = RelayKind::Unknown;
	ScopeKind targetScope = ScopeKind::Unknown;
	RelaySourceLocation location;
};

struct ManifestNonAliasProof
{
	std::string obligationId;
	std::string leftRelaySiteId;
	std::string rightRelaySiteId;
	bool solverProved = false;
};

// Additive schema-v5 parallel-certificate extension. These records are
// immutable runtime observations only; the optimizer deliberately does not
// consume them.
enum class ParallelCertificateStatus : uint8_t
{
	Proved,
	Complete,
	Conservative,
	Unknown,
	Unsupported,
};

enum class RelayPairCertificateRelation : uint8_t
{
	MutuallyExclusive,
	MustPrecedeAB,
	MustPrecedeBA,
	CoEmissionIndependent,
	ProvedMayAlias,
	PotentialConflict,
	Unknown,
};

enum class ManifestWorkBoundKind : uint8_t
{
	Constant,
	ParameterizedUpperBound,
	Unknown,
};

struct ManifestWorkCertificate
{
	std::string certificateId;
	ParallelCertificateStatus status =
		ParallelCertificateStatus::Unknown;
	std::optional<uint64_t> exact;
	std::optional<uint64_t> upperBound;
	bool parameterized = false;
	ManifestWorkBoundKind boundKind = ManifestWorkBoundKind::Unknown;
	uint64_t constantTerm = 0;
	uint64_t activeShardCountCoefficient = 0;
	std::string expressionKind;
	std::string expression;
	std::string exactExpressionJson;
	std::string upperBoundExpressionJson;
	std::string reason;
	std::vector<std::string> supportingCfgFactIds;
	std::vector<std::string> supportingConstraintIds;
	std::vector<std::string> supportingSolverResultIds;
};

struct ManifestRelayPairCertificate
{
	std::string certificateId;
	std::string siteA;
	std::string siteB;
	RelayPairCertificateRelation relation =
		RelayPairCertificateRelation::Unknown;
	ParallelCertificateStatus status =
		ParallelCertificateStatus::Unknown;
	std::string reason;
	std::vector<std::string> supportingCfgFactIds;
	std::vector<std::string> supportingConstraintIds;
	std::vector<std::string> supportingSolverResultIds;
	std::string counterexampleJson;
	RelaySourceLocation locationA;
	RelaySourceLocation locationB;
};

struct ManifestFunctionParallelCertificate
{
	std::string sourceFunctionId;
	ParallelCertificateStatus status =
		ParallelCertificateStatus::Unknown;
	std::string reason;
	std::vector<ManifestRelayPairCertificate> pairRelations;
	ManifestWorkCertificate directLogicalWork;
	ManifestWorkCertificate transitiveLogicalWork;
	ManifestWorkCertificate physicalRouteWork;
	ManifestWorkCertificate relayTreeDepth;
};

struct BoundRelayManifest
{
	uint32_t schemaVersion = 0;
	std::string sourcePath;
	ArtifactIdentity binding;
	std::string recomputedManifestHash;
	bool bindingTrusted = false;

	std::unordered_map<RelaySiteOrdinal, BoundRelaySite> sitesByOrdinal;
	std::unordered_map<std::string, RelaySiteOrdinal> ordinalBySiteId;
	std::unordered_map<std::string, FunctionRelayPlan> functionsById;
	std::unordered_map<uint32_t, std::string> functionIdByOpcode;
	std::vector<ManifestNonAliasProof> nonAliasProofs;
	uint32_t parallelCertificateExtensionSchemaVersion = 0;
	std::unordered_map<
		std::string,
		ManifestFunctionParallelCertificate>
		parallelCertificatesByFunction;
};

struct RelayPlanLoadResult
{
	ManifestLoadStatus status = ManifestLoadStatus::ManifestNotFound;
	std::string diagnostic;
	std::string sourcePath;
	ArtifactIdentity expectedBinding;
	ArtifactIdentity observedBinding;
	std::shared_ptr<const BoundRelayManifest> manifest;
	double elapsedTimeMs = 0.0;

	explicit operator bool() const
	{
		return status == ManifestLoadStatus::Loaded && manifest != nullptr;
	}
};

} // namespace relay_plan
} // namespace oxd
