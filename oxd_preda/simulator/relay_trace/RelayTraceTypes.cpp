#include "RelayTraceTypes.h"

namespace oxd {
namespace relay_trace {

const char *ToString(TraceMode mode)
{
	switch (mode)
	{
	case TraceMode::Off: return "off";
	case TraceMode::Observe: return "observe";
	case TraceMode::Strict: return "strict";
	}
	return "off";
}

const char *ToString(RelayKind kind)
{
	switch (kind)
	{
	case RelayKind::CustomScope: return "custom_scope";
	case RelayKind::Global: return "global";
	case RelayKind::AllShards: return "shards";
	case RelayKind::DeferredNext: return "next";
	case RelayKind::Unknown: return "unknown";
	}
	return "unknown";
}

const char *ToString(RouteKind kind)
{
	switch (kind)
	{
	case RouteKind::IntraShard: return "IntraShard";
	case RouteKind::CrossShard: return "CrossShard";
	case RouteKind::Global: return "Global";
	case RouteKind::AllShardsBroadcast: return "AllShardsBroadcast";
	case RouteKind::DeferredNext: return "DeferredNext";
	case RouteKind::Unknown: return "Unknown";
	}
	return "Unknown";
}

const char *ToString(ScopeKind kind)
{
	switch (kind)
	{
	case ScopeKind::None: return "none";
	case ScopeKind::Global: return "global";
	case ScopeKind::Shard: return "shard";
	case ScopeKind::Address: return "address";
	case ScopeKind::Uint32: return "uint32";
	case ScopeKind::Uint64: return "uint64";
	case ScopeKind::Uint96: return "uint96";
	case ScopeKind::Uint128: return "uint128";
	case ScopeKind::Uint160: return "uint160";
	case ScopeKind::Uint256: return "uint256";
	case ScopeKind::Uint512: return "uint512";
	case ScopeKind::Unknown: return "unknown";
	}
	return "unknown";
}

const char *ToString(ManifestLoadStatus status)
{
	switch (status)
	{
	case ManifestLoadStatus::ManifestNotFound: return "ManifestNotFound";
	case ManifestLoadStatus::ManifestParseError: return "ManifestParseError";
	case ManifestLoadStatus::ManifestSchemaUnsupported:
		return "ManifestSchemaUnsupported";
	case ManifestLoadStatus::ManifestBindingMissing:
		return "ManifestBindingMissing";
	case ManifestLoadStatus::ManifestBindingMismatch:
		return "ManifestBindingMismatch";
	case ManifestLoadStatus::ManifestHashMismatch:
		return "ManifestHashMismatch";
	case ManifestLoadStatus::Loaded: return "Loaded";
	}
	return "ManifestNotFound";
}

const char *ToString(ValidationStatus status)
{
	switch (status)
	{
	case ValidationStatus::Passed: return "Passed";
	case ValidationStatus::Mismatch: return "Mismatch";
	case ValidationStatus::SkippedUnsupported: return "SkippedUnsupported";
	case ValidationStatus::ManifestNotLoaded: return "ManifestNotLoaded";
	case ValidationStatus::ManifestBindingMismatch:
		return "ManifestBindingMismatch";
	case ValidationStatus::TraceInstrumentationError:
		return "TraceInstrumentationError";
	case ValidationStatus::NotApplicable: return "NotApplicable";
	}
	return "SkippedUnsupported";
}

const char *ToString(ValidationCheckKind kind)
{
	switch (kind)
	{
	case ValidationCheckKind::ArtifactBinding: return "artifact_binding";
	case ValidationCheckKind::RelaySiteIdentity: return "relay_site_identity";
	case ValidationCheckKind::HandlerOpcode: return "handler_opcode";
	case ValidationCheckKind::RelayKind: return "relay_kind";
	case ValidationCheckKind::TargetScopeKind: return "target_scope_kind";
	case ValidationCheckKind::DirectCount: return "direct_count";
	case ValidationCheckKind::CountUpperBound: return "count_upper_bound";
	case ValidationCheckKind::Depth: return "depth";
	case ValidationCheckKind::Fanout: return "fanout";
	case ValidationCheckKind::Routing: return "routing";
	case ValidationCheckKind::CoemissionNonAlias:
		return "coemission_non_alias";
	case ValidationCheckKind::CertificateMutuallyExclusive:
		return "certificate_mutual_exclusion";
	case ValidationCheckKind::CertificateMustPrecede:
		return "certificate_must_precede";
	case ValidationCheckKind::CertificateCoEmissionIndependent:
		return "certificate_coemission_independence";
	case ValidationCheckKind::CertificateProvedMayAlias:
		return "certificate_proved_may_alias";
	case ValidationCheckKind::DirectLogicalWork:
		return "certificate_direct_work";
	case ValidationCheckKind::TransitiveLogicalWork:
		return "certificate_transitive_work";
	case ValidationCheckKind::PhysicalRouteWork:
		return "certificate_physical_work";
	case ValidationCheckKind::RelayTreeDepth:
		return "certificate_depth";
	case ValidationCheckKind::TargetRelation: return "target_relation";
	case ValidationCheckKind::ArgumentRelation: return "argument_relation";
	case ValidationCheckKind::GuardNecessity: return "guard_necessity";
	case ValidationCheckKind::Instrumentation: return "instrumentation";
	case ValidationCheckKind::Unknown: return "unknown";
	}
	return "unknown";
}

const char *ToString(ParallelCertificateStatus status)
{
	switch (status)
	{
	case ParallelCertificateStatus::Proved: return "Proved";
	case ParallelCertificateStatus::Complete: return "Complete";
	case ParallelCertificateStatus::Conservative: return "Conservative";
	case ParallelCertificateStatus::Unknown: return "Unknown";
	case ParallelCertificateStatus::Unsupported: return "Unsupported";
	}
	return "Unknown";
}

const char *ToString(RelayPairCertificateRelation relation)
{
	switch (relation)
	{
	case RelayPairCertificateRelation::MutuallyExclusive:
		return "MutuallyExclusive";
	case RelayPairCertificateRelation::MustPrecedeAB:
		return "MustPrecedeAB";
	case RelayPairCertificateRelation::MustPrecedeBA:
		return "MustPrecedeBA";
	case RelayPairCertificateRelation::CoEmissionIndependent:
		return "CoEmissionIndependent";
	case RelayPairCertificateRelation::ProvedMayAlias:
		return "ProvedMayAlias";
	case RelayPairCertificateRelation::PotentialConflict:
		return "PotentialConflict";
	case RelayPairCertificateRelation::Unknown:
		return "Unknown";
	}
	return "Unknown";
}

std::string BytesToHex(const uint8_t *data, size_t size)
{
	static constexpr char digits[] = "0123456789abcdef";
	std::string result;
	result.resize(size * 2);
	for (size_t i = 0; i < size; ++i)
	{
		result[i * 2] = digits[data[i] >> 4];
		result[i * 2 + 1] = digits[data[i] & 0x0f];
	}
	return result;
}

std::string ModuleIdToHex(const rvm::ContractModuleID &moduleId)
{
	return BytesToHex(
		reinterpret_cast<const uint8_t *>(&moduleId),
		sizeof(moduleId));
}

} // namespace relay_trace
} // namespace oxd
