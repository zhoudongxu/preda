#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "../../native/abi/vm_types.h"
#include "../../runtime/relay_plan/BoundRelayManifest.h"

namespace oxd {
namespace relay_trace {

using RelaySiteOrdinal = relay_plan::RelaySiteOrdinal;

constexpr RelaySiteOrdinal InvalidRelaySiteOrdinal =
	relay_plan::InvalidRelaySiteOrdinal;
constexpr uint64_t NoTraceTransaction = 0;
constexpr uint32_t NoTargetShard = std::numeric_limits<uint32_t>::max();

enum class TraceMode : uint8_t
{
	Off,
	Observe,
	Strict,
};

using RelayKind = relay_plan::RelayKind;

enum class RouteKind : uint8_t
{
	IntraShard,
	CrossShard,
	Global,
	AllShardsBroadcast,
	DeferredNext,
	Unknown,
};

using ScopeKind = relay_plan::ScopeKind;
using ManifestLoadStatus = relay_plan::ManifestLoadStatus;
using ParallelCertificateStatus =
	relay_plan::ParallelCertificateStatus;
using RelayPairCertificateRelation =
	relay_plan::RelayPairCertificateRelation;

enum class ValidationStatus : uint8_t
{
	Passed,
	Mismatch,
	SkippedUnsupported,
	ManifestNotLoaded,
	ManifestBindingMismatch,
	TraceInstrumentationError,
	NotApplicable,
};

enum class ValidationCheckKind : uint8_t
{
	ArtifactBinding,
	RelaySiteIdentity,
	HandlerOpcode,
	RelayKind,
	TargetScopeKind,
	DirectCount,
	CountUpperBound,
	Depth,
	Fanout,
	Routing,
	CoemissionNonAlias,
	CertificateMutuallyExclusive,
	CertificateMustPrecede,
	CertificateCoEmissionIndependent,
	CertificateProvedMayAlias,
	DirectLogicalWork,
	TransitiveLogicalWork,
	PhysicalRouteWork,
	RelayTreeDepth,
	TargetRelation,
	ArgumentRelation,
	GuardNecessity,
	Instrumentation,
	Unknown,
};

using TraceSourceLocation = relay_plan::RelaySourceLocation;
using ArtifactIdentity = relay_plan::ArtifactIdentity;

// Owning copy of a runtime scope key. It deliberately does not retain
// rvm::ScopeKey::Data or a pointer into SimuTxn.
struct OwnedScopeTarget
{
	std::vector<uint8_t> bytes;

	OwnedScopeTarget() = default;
	OwnedScopeTarget(const uint8_t *data, size_t size)
	{
		Assign(data, size);
	}

	void Assign(const uint8_t *data, size_t size)
	{
		if (data == nullptr || size == 0)
		{
			bytes.clear();
			return;
		}
		bytes.assign(data, data + size);
	}

	bool operator==(const OwnedScopeTarget &other) const
	{
		return bytes == other.bytes;
	}

	bool operator!=(const OwnedScopeTarget &other) const
	{
		return !(*this == other);
	}
};

struct RuntimeTxnTraceContext
{
	uint64_t traceTxId = NoTraceTransaction;
	uint64_t rootTraceTxId = NoTraceTransaction;
	uint64_t parentTraceTxId = NoTraceTransaction;

	rvm::ContractInvokeId contractInvoke = rvm::ContractInvokeIdInvalid;
	uint32_t opcode = 0;
	std::string sourceFunctionId;
	std::string moduleId;

	uint32_t depth = 0;
	uint32_t ownerShard = NoTargetShard;
	bool isRelay = false;
};

struct RelaySiteMarkerFrame
{
	std::string moduleId;
	RelaySiteOrdinal ordinal = InvalidRelaySiteOrdinal;
	uint64_t generation = 0;
	uint64_t traceTxId = NoTraceTransaction;
	bool consumed = false;
};

struct RelayEmitTraceEvent
{
	uint64_t childTraceTxId = NoTraceTransaction;
	uint64_t rootTraceTxId = NoTraceTransaction;
	uint64_t parentTraceTxId = NoTraceTransaction;

	RelaySiteOrdinal relaySiteOrdinal = InvalidRelaySiteOrdinal;
	std::string relaySiteId;
	uint32_t occurrenceIndex = 0;
	// Total order of logical relay creation within one parent
	// microtransaction. Unlike occurrenceIndex this is shared by all sites.
	uint64_t emissionSequence = 0;
	uint32_t depth = 0;

	std::string sourceModuleId;
	std::string sourceFunctionId;
	rvm::ContractInvokeId targetContractInvoke =
		rvm::ContractInvokeIdInvalid;
	OwnedScopeTarget actualTarget;
	ScopeKind actualTargetScope = ScopeKind::Unknown;
	uint32_t actualOpcode = 0;
	std::vector<uint8_t> actualSerializedArgs;
	RelayKind relayKind = RelayKind::Unknown;
};

struct RelayRouteTraceEvent
{
	uint64_t physicalTraceTxId = NoTraceTransaction;
	uint64_t rootTraceTxId = NoTraceTransaction;
	uint64_t parentTraceTxId = NoTraceTransaction;

	RelaySiteOrdinal relaySiteOrdinal = InvalidRelaySiteOrdinal;
	std::string relaySiteId;
	std::string sourceModuleId;
	uint32_t occurrenceIndex = 0;
	uint32_t targetShard = NoTargetShard;
	// Snapshot of the simulator's active shard count at the original routing
	// point. Zero means the observer could not obtain it.
	uint32_t activeShardCount = 0;
	RouteKind routeKind = RouteKind::Unknown;
};

struct RelayExecutionTraceEvent
{
	uint64_t traceTxId = NoTraceTransaction;
	uint64_t rootTraceTxId = NoTraceTransaction;
	uint64_t parentTraceTxId = NoTraceTransaction;

	uint32_t depth = 0;
	uint32_t opcode = 0;
	std::string moduleId;
	std::string sourceFunctionId;
	bool isRelay = false;
	bool started = false;
	bool completed = false;
	bool succeeded = false;
};

struct RelayTraceMismatch
{
	uint64_t rootTraceTxId = NoTraceTransaction;
	uint64_t parentTraceTxId = NoTraceTransaction;
	uint64_t currentTraceTxId = NoTraceTransaction;

	std::string contract;
	std::string function;
	uint32_t opcode = 0;
	std::string relaySiteId;
	std::string propertyId;
	std::string certificateId;
	std::string siteA;
	std::string siteB;
	RelayPairCertificateRelation certificateRelation =
		RelayPairCertificateRelation::Unknown;
	RelaySiteOrdinal relaySiteOrdinal = InvalidRelaySiteOrdinal;
	uint32_t occurrenceIndex = 0;

	ValidationCheckKind checkKind = ValidationCheckKind::Unknown;
	std::string expected;
	std::string actual;
	TraceSourceLocation sourceLocation;
	TraceSourceLocation relatedSourceLocation;
	ArtifactIdentity manifestIdentity;
	std::string moduleIdentity;
	std::string diagnosticReason;
};

struct RelayValidationResult
{
	ValidationStatus status = ValidationStatus::SkippedUnsupported;
	ValidationCheckKind checkKind = ValidationCheckKind::Unknown;
	std::string reason;
	RelayTraceMismatch detail;
};

struct RelayTraceTimings
{
	double manifestLoadTimeMs = 0.0;
	double traceRecordingTimeMs = 0.0;
	double validationTimeMs = 0.0;
	double reportSerializationTimeMs = 0.0;
};

struct RelayTraceCounters
{
	uint64_t sourceTransactionsObserved = 0;
	uint64_t microtransactionsObserved = 0;
	uint64_t logicalRelayEmissions = 0;
	uint64_t physicalRelayRoutes = 0;
	uint64_t relayExecutions = 0;

	uint64_t intraShardRelays = 0;
	uint64_t crossShardRelays = 0;
	uint64_t globalRelays = 0;
	uint64_t broadcastLogicalEmissions = 0;
	uint64_t broadcastPhysicalClones = 0;
	uint64_t deferredRelays = 0;

	uint32_t maximumObservedDepth = 0;

	uint64_t checksPassed = 0;
	uint64_t checksMismatched = 0;
	uint64_t checksSkipped = 0;
	uint64_t manifestLoadFailures = 0;
	uint64_t manifestBindingFailures = 0;
	uint64_t instrumentationFailures = 0;

	std::map<ValidationCheckKind, uint64_t> checksByKind;
	// Additive report detail. checksByKind is retained for schema
	// compatibility while this map makes each kind/status combination directly
	// machine-readable.
	std::map<
		ValidationCheckKind,
		std::map<ValidationStatus, uint64_t>> checksByKindAndStatus;
};

struct RelayTraceSnapshot
{
	RelayTraceCounters counters;
	RelayTraceTimings timings;
	std::vector<RelayEmitTraceEvent> emissions;
	std::vector<RelayRouteTraceEvent> routes;
	std::vector<RelayExecutionTraceEvent> executions;
	std::vector<RelayValidationResult> validationResults;
};

const char *ToString(TraceMode mode);
const char *ToString(RelayKind kind);
const char *ToString(RouteKind kind);
const char *ToString(ScopeKind kind);
const char *ToString(ManifestLoadStatus status);
const char *ToString(ValidationStatus status);
const char *ToString(ValidationCheckKind kind);
const char *ToString(ParallelCertificateStatus status);
const char *ToString(RelayPairCertificateRelation relation);

std::string BytesToHex(const uint8_t *data, size_t size);
std::string ModuleIdToHex(const rvm::ContractModuleID &moduleId);

} // namespace relay_trace
} // namespace oxd
