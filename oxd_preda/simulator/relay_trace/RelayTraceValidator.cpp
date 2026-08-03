#include "RelayTraceValidator.h"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_map>

namespace oxd {
namespace relay_trace {
namespace {

using LogicalKey =
	std::tuple<uint64_t, std::string, RelaySiteOrdinal, uint32_t>;

LogicalKey Key(const RelayEmitTraceEvent &event)
{
	return {
		event.parentTraceTxId,
		event.sourceModuleId,
		event.relaySiteOrdinal,
		event.occurrenceIndex,
	};
}

LogicalKey Key(const RelayRouteTraceEvent &event)
{
	return {
		event.parentTraceTxId,
		event.sourceModuleId,
		event.relaySiteOrdinal,
		event.occurrenceIndex,
	};
}

std::vector<const RelayRouteTraceEvent *> RoutesFor(
	const RelayEmitTraceEvent &emission,
	const std::vector<RelayRouteTraceEvent> &routes)
{
	std::vector<const RelayRouteTraceEvent *> result;
	const LogicalKey key = Key(emission);
	for (const RelayRouteTraceEvent &route : routes)
	{
		if (Key(route) == key)
			result.push_back(&route);
	}
	return result;
}

bool Contains(const std::vector<std::string> &values, const std::string &value)
{
	return std::find(values.begin(), values.end(), value) != values.end();
}

std::optional<size_t> KeyedScopeWidth(ScopeKind scope)
{
	switch (scope)
	{
	case ScopeKind::Address: return 36;
	case ScopeKind::Uint32: return 4;
	case ScopeKind::Uint64: return 8;
	case ScopeKind::Uint96: return 12;
	case ScopeKind::Uint128: return 16;
	case ScopeKind::Uint160: return 20;
	case ScopeKind::Uint256: return 32;
	case ScopeKind::Uint512: return 64;
	default: return std::nullopt;
	}
}

bool TargetObservationComplete(
	const ManifestRelaySite &site,
	const RelayEmitTraceEvent &event,
	std::string &reason)
{
	if (site.targetScope == ScopeKind::Unknown)
	{
		reason = "manifest target scope is unknown";
		return false;
	}
	if (event.actualTargetScope == ScopeKind::Unknown)
	{
		reason = "runtime target scope is unknown";
		return false;
	}
	if (event.actualTargetScope != site.targetScope)
	{
		reason = "runtime target scope does not match the bound manifest";
		return false;
	}
	const std::optional<size_t> expectedWidth =
		KeyedScopeWidth(site.targetScope);
	if (expectedWidth && event.actualTarget.bytes.size() != *expectedWidth)
	{
		reason = "runtime keyed target bytes are incomplete";
		return false;
	}
	return true;
}

std::string ArtifactSummary(const ArtifactIdentity &identity)
{
	std::ostringstream stream;
	stream << "dapp=" << identity.dapp
		<< ",contract=" << identity.contract
		<< ",transpiler_version=" << identity.transpilerVersion
		<< ",intermediate_hash=" << identity.intermediateHash
		<< ",module_id=" << identity.moduleId
		<< ",module_hash_kind=" << identity.moduleHashKind
		<< ",module_hash=" << identity.moduleHash
		<< ",manifest_hash_algorithm="
		<< identity.manifestHashAlgorithm
		<< ",manifest_hash=" << identity.manifestHash
		<< ",binding_complete="
		<< (identity.bindingComplete ? "true" : "false");
	return stream.str();
}

} // namespace

std::vector<RelayValidationResult>
RelayTraceValidator::ValidateManifestLoad(
	const RelayManifestLoadResult &loadResult) const
{
	RelayValidationResult result;
	result.checkKind = ValidationCheckKind::ArtifactBinding;
	result.reason = loadResult.diagnostic;
	result.detail.checkKind = result.checkKind;
	result.detail.contract =
		!loadResult.observedBinding.contract.empty()
			? loadResult.observedBinding.contract
			: loadResult.expectedBinding.contract;
	result.detail.expected =
		ArtifactSummary(loadResult.expectedBinding);
	result.detail.actual =
		ArtifactSummary(loadResult.observedBinding);
	if (!loadResult.sourcePath.empty())
	{
		result.detail.actual += ",manifest_path=";
		result.detail.actual += loadResult.sourcePath;
	}
	result.detail.manifestIdentity = loadResult.observedBinding;
	result.detail.moduleIdentity = loadResult.expectedBinding.moduleId;
	result.detail.diagnosticReason = result.reason;

	switch (loadResult.status)
	{
	case ManifestLoadStatus::Loaded:
		result.status = ValidationStatus::Passed;
		break;
	case ManifestLoadStatus::ManifestBindingMissing:
	case ManifestLoadStatus::ManifestBindingMismatch:
	case ManifestLoadStatus::ManifestHashMismatch:
		result.status = ValidationStatus::ManifestBindingMismatch;
		break;
	default:
		result.status = ValidationStatus::ManifestNotLoaded;
		break;
	}
	return {std::move(result)};
}

RelayValidationResult RelayTraceValidator::Result(
	ValidationStatus status,
	ValidationCheckKind kind,
	const std::string &reason,
	const RuntimeTxnTraceContext &execution,
	const RelayEmitTraceEvent *emission,
	const LoadedRelayManifest &manifest,
	const std::string &expected,
	const std::string &actual) const
{
	RelayValidationResult result;
	result.status = status;
	result.checkKind = kind;
	result.reason = reason;
	result.detail.rootTraceTxId = execution.rootTraceTxId;
	result.detail.parentTraceTxId = execution.parentTraceTxId;
	result.detail.currentTraceTxId = execution.traceTxId;
	result.detail.contract = manifest.binding.contract;
	result.detail.function = execution.sourceFunctionId;
	result.detail.opcode = execution.opcode;
	result.detail.checkKind = kind;
	result.detail.expected = expected;
	result.detail.actual = actual;
	result.detail.manifestIdentity = manifest.binding;
	result.detail.moduleIdentity = execution.moduleId;
	result.detail.diagnosticReason = reason;
	if (emission)
	{
		result.detail.rootTraceTxId = emission->rootTraceTxId;
		result.detail.parentTraceTxId = emission->parentTraceTxId;
		result.detail.relaySiteId = emission->relaySiteId;
		result.detail.relaySiteOrdinal = emission->relaySiteOrdinal;
		result.detail.occurrenceIndex = emission->occurrenceIndex;
		result.detail.opcode = emission->actualOpcode;
		if (!emission->sourceFunctionId.empty())
			result.detail.function = emission->sourceFunctionId;
		auto site = manifest.sitesByOrdinal.find(
			emission->relaySiteOrdinal);
		if (site != manifest.sitesByOrdinal.end())
			result.detail.sourceLocation = site->second.location;
	}
	return result;
}

RelayValidationResult RelayTraceValidator::CertificateResult(
	ValidationStatus status,
	ValidationCheckKind kind,
	const std::string &reason,
	const RuntimeTxnTraceContext &execution,
	const LoadedRelayManifest &manifest,
	const std::string &certificateId,
	const std::string &siteA,
	const std::string &siteB,
	RelayPairCertificateRelation relation,
	const TraceSourceLocation &locationA,
	const TraceSourceLocation &locationB,
	const RelayEmitTraceEvent *emission,
	const std::string &expected,
	const std::string &actual) const
{
	RelayValidationResult result = Result(
		status,
		kind,
		reason,
		execution,
		emission,
		manifest,
		expected,
		actual);
	result.detail.propertyId = certificateId;
	result.detail.certificateId = certificateId;
	// Certificate ownership is the relevant root invocation; an individual
	// emission may retain a synchronous helper's static source function.
	result.detail.function = execution.sourceFunctionId;
	result.detail.siteA = siteA;
	result.detail.siteB = siteB;
	result.detail.certificateRelation = relation;
	result.detail.sourceLocation = locationA;
	result.detail.relatedSourceLocation = locationB;
	return result;
}

std::vector<RelayValidationResult>
RelayTraceValidator::ValidateExecution(
	const LoadedRelayManifest &manifest,
	const RelayExecutionValidationInput &input) const
{
	std::vector<RelayValidationResult> results;
	std::vector<const RelayEmitTraceEvent *> directEmissions;
	bool directSiteIdentityComplete = true;

	for (const RelayEmitTraceEvent &emission : input.emissions)
	{
		if (emission.parentTraceTxId != input.execution.traceTxId)
			continue;

		auto found = manifest.sitesByOrdinal.find(
			emission.relaySiteOrdinal);
		if (found == manifest.sitesByOrdinal.end())
		{
			directSiteIdentityComplete = false;
			results.push_back(Result(
				ValidationStatus::Mismatch,
				ValidationCheckKind::RelaySiteIdentity,
				"observed relay ordinal is absent from the bound manifest",
				input.execution,
				&emission,
				manifest,
				"known module-local ordinal",
				std::to_string(emission.relaySiteOrdinal)));
			continue;
		}
		const ManifestRelaySite &site = found->second;
		directEmissions.push_back(&emission);

		if (emission.relaySiteId.empty() ||
			emission.sourceFunctionId.empty())
		{
			directSiteIdentityComplete = false;
			results.push_back(Result(
				ValidationStatus::SkippedUnsupported,
				ValidationCheckKind::RelaySiteIdentity,
				"runtime relay site identity is incomplete",
				input.execution,
				&emission,
				manifest,
				site.id + "@" + site.sourceFunctionId,
				emission.relaySiteId + "@" +
					emission.sourceFunctionId));
		}
		else if (emission.relaySiteId != site.id ||
			emission.sourceFunctionId != site.sourceFunctionId)
		{
			directSiteIdentityComplete = false;
			results.push_back(Result(
				ValidationStatus::Mismatch,
				ValidationCheckKind::RelaySiteIdentity,
				"ordinal resolved to a different site or source function",
				input.execution,
				&emission,
				manifest,
				site.id + "@" + site.sourceFunctionId,
				emission.relaySiteId + "@" +
					emission.sourceFunctionId));
		}
		else
		{
			results.push_back(Result(
				ValidationStatus::Passed,
				ValidationCheckKind::RelaySiteIdentity,
				"module-local relay ordinal resolved in the bound manifest",
				input.execution,
				&emission,
				manifest));
		}

		if (!site.handlerResolved)
		{
			results.push_back(Result(
				ValidationStatus::SkippedUnsupported,
				ValidationCheckKind::HandlerOpcode,
				"manifest handler opcode is unresolved",
				input.execution,
				&emission,
				manifest));
		}
		else if (site.expectedOpcode != emission.actualOpcode)
		{
			results.push_back(Result(
				ValidationStatus::Mismatch,
				ValidationCheckKind::HandlerOpcode,
				"observed relay opcode differs from its manifest handler",
				input.execution,
				&emission,
				manifest,
				std::to_string(site.expectedOpcode),
				std::to_string(emission.actualOpcode)));
		}
		else
		{
			results.push_back(Result(
				ValidationStatus::Passed,
				ValidationCheckKind::HandlerOpcode,
				"relay handler opcode matches",
				input.execution,
				&emission,
				manifest));
		}

		if (site.relayKind == RelayKind::Unknown ||
			emission.relayKind == RelayKind::Unknown)
		{
			results.push_back(Result(
				ValidationStatus::SkippedUnsupported,
				ValidationCheckKind::RelayKind,
				"relay kind is unknown",
				input.execution,
				&emission,
				manifest));
		}
		else if (site.relayKind != emission.relayKind)
		{
			results.push_back(Result(
				ValidationStatus::Mismatch,
				ValidationCheckKind::RelayKind,
				"observed relay API kind differs from manifest",
				input.execution,
				&emission,
				manifest,
				ToString(site.relayKind),
				ToString(emission.relayKind)));
		}
		else
		{
			results.push_back(Result(
				ValidationStatus::Passed,
				ValidationCheckKind::RelayKind,
				"relay API kind matches",
				input.execution,
				&emission,
				manifest));
		}

		if (site.targetScope == ScopeKind::Unknown ||
			emission.actualTargetScope == ScopeKind::Unknown)
		{
			results.push_back(Result(
				ValidationStatus::SkippedUnsupported,
				ValidationCheckKind::TargetScopeKind,
				"target scope kind is unknown",
				input.execution,
				&emission,
				manifest));
		}
		else if (site.targetScope != emission.actualTargetScope)
		{
			results.push_back(Result(
				ValidationStatus::Mismatch,
				ValidationCheckKind::TargetScopeKind,
				"observed target scope differs from manifest",
				input.execution,
				&emission,
				manifest,
				ToString(site.targetScope),
				ToString(emission.actualTargetScope)));
		}
		else
		{
			results.push_back(Result(
				ValidationStatus::Passed,
				ValidationCheckKind::TargetScopeKind,
				"target scope kind matches",
				input.execution,
				&emission,
				manifest));
		}

		const std::vector<const RelayRouteTraceEvent *> routes =
			RoutesFor(emission, input.routes);
		size_t expectedRoutes = 1;
		RouteKind expectedKind = RouteKind::Unknown;
		switch (emission.relayKind)
		{
		case RelayKind::CustomScope:
			expectedKind = RouteKind::Unknown;
			break;
		case RelayKind::Global:
			expectedKind = RouteKind::Global;
			break;
		case RelayKind::AllShards:
			expectedRoutes = input.activeShardCount;
			expectedKind = RouteKind::AllShardsBroadcast;
			break;
		case RelayKind::DeferredNext:
			expectedKind = RouteKind::DeferredNext;
			break;
		case RelayKind::Unknown:
			break;
		}

		if (emission.relayKind == RelayKind::Unknown ||
			(emission.relayKind == RelayKind::AllShards &&
			 input.activeShardCount == 0))
		{
			results.push_back(Result(
				ValidationStatus::SkippedUnsupported,
				ValidationCheckKind::Fanout,
				"fanout cannot be checked without relay kind and active shard count",
				input.execution,
				&emission,
				manifest));
		}
		else if (routes.size() != expectedRoutes)
		{
			results.push_back(Result(
				ValidationStatus::Mismatch,
				ValidationCheckKind::Fanout,
				"physical route count differs from logical fanout",
				input.execution,
				&emission,
				manifest,
				std::to_string(expectedRoutes),
				std::to_string(routes.size())));
		}
		else
		{
			results.push_back(Result(
				ValidationStatus::Passed,
				ValidationCheckKind::Fanout,
				"logical emission has the expected physical fanout",
				input.execution,
				&emission,
				manifest));
		}

		ValidationStatus routingStatus = ValidationStatus::Passed;
		std::string routingReason =
			"physical route kind and target shard are consistent with "
			"the original relay API";
		std::string routingExpected;
		std::string routingActual;

		if (emission.relayKind == RelayKind::Unknown)
		{
			routingStatus = ValidationStatus::SkippedUnsupported;
			routingReason =
				"routing cannot be checked for an unknown relay kind";
		}
		else if (routes.empty())
		{
			routingStatus = ValidationStatus::Mismatch;
			routingReason =
				"logical relay emission has no matching physical route";
			routingExpected = "at least one physical route";
			routingActual = "no physical routes";
		}
		else if (routes.size() != expectedRoutes &&
			!(emission.relayKind == RelayKind::AllShards &&
			  input.activeShardCount == 0))
		{
			routingStatus = ValidationStatus::Mismatch;
			routingReason =
				"physical route count prevents target-shard validation";
			routingExpected = std::to_string(expectedRoutes);
			routingActual = std::to_string(routes.size());
		}
		else
		{
			bool routingMismatch = false;
			for (const RelayRouteTraceEvent *route : routes)
			{
				if (route->targetShard == NoTargetShard)
					routingMismatch = true;
			}

			switch (emission.relayKind)
			{
			case RelayKind::CustomScope:
			{
				const bool ownerKnown =
					input.execution.ownerShard != NoTargetShard;
				for (const RelayRouteTraceEvent *route : routes)
				{
					if (route->routeKind != RouteKind::IntraShard &&
						route->routeKind != RouteKind::CrossShard)
					{
						routingMismatch = true;
					}
					else if (ownerKnown &&
						((route->routeKind == RouteKind::IntraShard &&
						  route->targetShard !=
							  input.execution.ownerShard) ||
						 (route->routeKind == RouteKind::CrossShard &&
						  route->targetShard ==
							  input.execution.ownerShard)))
					{
						routingMismatch = true;
					}
				}
				if (!routingMismatch && !ownerKnown)
				{
					routingStatus =
						ValidationStatus::SkippedUnsupported;
					routingReason =
						"target shard was recorded, but the executing "
						"owner shard is unavailable for independent "
						"intra/cross classification";
				}
				break;
			}
			case RelayKind::Global:
				for (const RelayRouteTraceEvent *route : routes)
				{
					if (route->routeKind != RouteKind::Global ||
						route->targetShard != rvm::GlobalShard)
					{
						routingMismatch = true;
					}
				}
				routingExpected =
					"route kind Global, target shard " +
					std::to_string(rvm::GlobalShard);
				break;
			case RelayKind::AllShards:
				if (input.activeShardCount == 0)
				{
					routingStatus =
						ValidationStatus::SkippedUnsupported;
					routingReason =
						"active shard count is unavailable for broadcast "
						"target coverage validation";
					break;
				}
				else
				{
					std::set<uint32_t> targetShards;
					std::ostringstream actualTargets;
					bool first = true;
					for (const RelayRouteTraceEvent *route : routes)
					{
						if (!first)
							actualTargets << ',';
						first = false;
						actualTargets << route->targetShard;
						if (route->routeKind !=
								RouteKind::AllShardsBroadcast ||
							route->targetShard >=
								input.activeShardCount ||
							!targetShards.insert(
								 route->targetShard).second)
						{
							routingMismatch = true;
						}
					}
					if (targetShards.size() != input.activeShardCount)
						routingMismatch = true;
					routingExpected =
						"unique target shards [0," +
						std::to_string(input.activeShardCount) + ")";
					routingActual = actualTargets.str();
				}
				break;
			case RelayKind::DeferredNext:
			{
				const bool ownerKnown =
					input.execution.ownerShard != NoTargetShard;
				for (const RelayRouteTraceEvent *route : routes)
				{
					if (route->routeKind != RouteKind::DeferredNext ||
						(ownerKnown &&
						 route->targetShard !=
							 input.execution.ownerShard))
					{
						routingMismatch = true;
					}
				}
				if (!routingMismatch && !ownerKnown)
				{
					routingStatus =
						ValidationStatus::SkippedUnsupported;
					routingReason =
						"deferred target shard was recorded, but the "
						"executing owner shard is unavailable";
				}
				break;
			}
			case RelayKind::Unknown:
				break;
			}

			if (routingMismatch)
			{
				routingStatus = ValidationStatus::Mismatch;
				routingReason =
					"physical route kind or target shard differs from "
					"the original relay API semantics";
			}
		}
		results.push_back(Result(
			routingStatus,
			ValidationCheckKind::Routing,
			routingReason,
			input.execution,
			&emission,
			manifest,
			routingExpected,
			routingActual));

		// EstablishedByConstruction formula obligations are compiler facts,
		// not independent runtime proofs. Until the runtime can bind every
		// required Formula IR symbol from the real PREDA ABI, report each
		// replay class explicitly instead of silently omitting it or turning it
		// into a false pass.
		results.push_back(Result(
			ValidationStatus::SkippedUnsupported,
			ValidationCheckKind::TargetRelation,
			"target formula replay is unavailable without an independent "
			"runtime Formula IR evaluator and complete symbol binding",
			input.execution,
			&emission,
			manifest));
		results.push_back(Result(
			ValidationStatus::SkippedUnsupported,
			ValidationCheckKind::ArgumentRelation,
			"argument formula replay is unavailable without the PREDA ABI "
			"argument decoder and complete symbol binding",
			input.execution,
			&emission,
			manifest));
		results.push_back(Result(
			ValidationStatus::SkippedUnsupported,
			ValidationCheckKind::GuardNecessity,
			"guard formula replay is unavailable because branch and state "
			"symbols cannot yet be independently recovered at runtime",
			input.execution,
			&emission,
			manifest));
	}

	auto function = manifest.functionsById.find(
		input.execution.sourceFunctionId);
	if (function == manifest.functionsById.end())
	{
		results.push_back(Result(
			ValidationStatus::SkippedUnsupported,
			ValidationCheckKind::DirectCount,
			"executing function has no manifest summary",
			input.execution,
			nullptr,
			manifest));
	}
	else
	{
		const ManifestFunctionSummary &summary = function->second;
		const bool legacySummaryUnsupported =
			summary.hasUnmodeledRelayReachableCall;
		size_t functionEmissionCount = 0;
		for (const RelayEmitTraceEvent *emission : directEmissions)
		{
			auto site = manifest.sitesByOrdinal.find(
				emission->relaySiteOrdinal);
			if (site != manifest.sitesByOrdinal.end() &&
				site->second.sourceFunctionId ==
					input.execution.sourceFunctionId)
			{
				++functionEmissionCount;
			}
		}

		if (legacySummaryUnsupported)
		{
			results.push_back(Result(
				ValidationStatus::SkippedUnsupported,
				ValidationCheckKind::DirectCount,
				"legacy direct count is unavailable when a relay-reachable synchronous call is unmodeled",
				input.execution,
				nullptr,
				manifest));
		}
		else if (summary.exactDirectRelayCount)
		{
			results.push_back(Result(
				functionEmissionCount == *summary.exactDirectRelayCount
					? ValidationStatus::Passed
					: ValidationStatus::Mismatch,
				ValidationCheckKind::DirectCount,
				functionEmissionCount == *summary.exactDirectRelayCount
					? "direct logical relay count matches exact summary"
					: "direct logical relay count differs from exact summary",
				input.execution,
				nullptr,
				manifest,
				std::to_string(*summary.exactDirectRelayCount),
				std::to_string(functionEmissionCount)));
		}
		else
		{
			results.push_back(Result(
				ValidationStatus::SkippedUnsupported,
				ValidationCheckKind::DirectCount,
				"symbolic/unknown direct count is not safely replayable",
				input.execution,
				nullptr,
				manifest));
		}

		if (legacySummaryUnsupported)
		{
			results.push_back(Result(
				ValidationStatus::SkippedUnsupported,
				ValidationCheckKind::CountUpperBound,
				"legacy direct-count upper bound is unavailable when a relay-reachable synchronous call is unmodeled",
				input.execution,
				nullptr,
				manifest));
		}
		else if (summary.directRelayCountUpperBound)
		{
			results.push_back(Result(
				functionEmissionCount <=
						*summary.directRelayCountUpperBound
					? ValidationStatus::Passed
					: ValidationStatus::Mismatch,
				ValidationCheckKind::CountUpperBound,
				functionEmissionCount <=
						*summary.directRelayCountUpperBound
					? "direct relay count is within the finite upper bound"
					: "direct relay count exceeds the finite upper bound",
				input.execution,
				nullptr,
				manifest,
				std::to_string(*summary.directRelayCountUpperBound),
				std::to_string(functionEmissionCount)));
		}
		else
		{
			results.push_back(Result(
				ValidationStatus::SkippedUnsupported,
				ValidationCheckKind::CountUpperBound,
				"manifest has no finite direct relay count upper bound",
				input.execution,
				nullptr,
				manifest));
		}

		if (legacySummaryUnsupported)
		{
			results.push_back(Result(
				ValidationStatus::SkippedUnsupported,
				ValidationCheckKind::Depth,
				"legacy depth is unavailable when a relay-reachable synchronous call is unmodeled",
				input.execution,
				nullptr,
				manifest));
		}
		else if (!summary.maximumDepth || !input.observedTransitiveDepth)
		{
			results.push_back(Result(
				ValidationStatus::SkippedUnsupported,
				ValidationCheckKind::Depth,
				"finite manifest depth or completed runtime subtree depth is unavailable",
				input.execution,
				nullptr,
				manifest));
		}
		else
		{
			results.push_back(Result(
				*input.observedTransitiveDepth <= *summary.maximumDepth
					? ValidationStatus::Passed
					: ValidationStatus::Mismatch,
				ValidationCheckKind::Depth,
				*input.observedTransitiveDepth <= *summary.maximumDepth
					? "observed relay subtree depth is within manifest maximum"
					: "observed relay subtree depth exceeds manifest maximum",
				input.execution,
				nullptr,
				manifest,
				std::to_string(*summary.maximumDepth),
				std::to_string(*input.observedTransitiveDepth)));
		}
	}

	auto parallelFunction =
		manifest.parallelCertificatesByFunction.find(
			input.execution.sourceFunctionId);
	if (manifest.parallelCertificateExtensionSchemaVersion == 1 &&
		parallelFunction ==
			manifest.parallelCertificatesByFunction.end())
	{
		results.push_back(CertificateResult(
			ValidationStatus::SkippedUnsupported,
			ValidationCheckKind::DirectLogicalWork,
			"relevant root function has no parallel certificate",
			input.execution,
			manifest,
			{},
			{},
			{},
			RelayPairCertificateRelation::Unknown,
			{},
			{}));
	}
	else if (parallelFunction !=
		manifest.parallelCertificatesByFunction.end())
	{
		const ManifestFunctionParallelCertificate &certificate =
			parallelFunction->second;
		std::unordered_map<
			std::string,
			std::vector<const RelayEmitTraceEvent *>> emissionsBySite;
		for (const RelayEmitTraceEvent *emission : directEmissions)
		{
			auto site = manifest.sitesByOrdinal.find(
				emission->relaySiteOrdinal);
			if (site != manifest.sitesByOrdinal.end())
				emissionsBySite[site->second.id].push_back(emission);
		}

		auto workResult = [&](const ManifestWorkCertificate &work,
						  ValidationCheckKind kind,
						  const std::optional<uint64_t> &observed,
						  bool physical)
		{
			if (work.status != ParallelCertificateStatus::Complete &&
				work.status !=
					ParallelCertificateStatus::Conservative)
			{
				return CertificateResult(
					ValidationStatus::SkippedUnsupported,
					kind,
					work.reason.empty()
						? "certificate work bound is Unknown or Unsupported"
						: work.reason,
					input.execution,
					manifest,
					work.certificateId,
					{},
					{},
					RelayPairCertificateRelation::Unknown,
					{},
					{});
			}
			if (!observed)
			{
				return CertificateResult(
					ValidationStatus::SkippedUnsupported,
					kind,
					"completed runtime observation for this bound is unavailable",
					input.execution,
					manifest,
					work.certificateId,
					{},
					{},
					RelayPairCertificateRelation::Unknown,
					{},
					{});
			}

			std::optional<uint64_t> bound;
			std::string expected;
			if (physical)
			{
				if (work.boundKind ==
					relay_plan::ManifestWorkBoundKind::Constant)
				{
					bound = work.constantTerm;
				}
				else if (work.boundKind ==
					relay_plan::ManifestWorkBoundKind::
						ParameterizedUpperBound)
				{
					const uint64_t shards = input.activeShardCount;
					if (work.activeShardCountCoefficient != 0 &&
						shards == 0)
					{
						return CertificateResult(
							ValidationStatus::SkippedUnsupported,
							kind,
							"active shard count is unavailable for a parameterized physical bound",
							input.execution,
							manifest,
							work.certificateId,
							{},
							{},
							RelayPairCertificateRelation::Unknown,
							{},
							{});
					}
					if (work.activeShardCountCoefficient != 0 &&
						shards >
							(std::numeric_limits<uint64_t>::max() -
							 work.constantTerm) /
								work.activeShardCountCoefficient)
					{
						return CertificateResult(
							ValidationStatus::SkippedUnsupported,
							kind,
							"physical work bound overflows uint64",
							input.execution,
							manifest,
							work.certificateId,
							{},
							{},
							RelayPairCertificateRelation::Unknown,
							{},
							{});
					}
					bound = work.constantTerm +
						work.activeShardCountCoefficient * shards;
				}
				expected = work.expression;
			}
			else
			{
				bound = work.upperBound
					? work.upperBound
					: work.exact;
			}
			if (!bound)
			{
				return CertificateResult(
					ValidationStatus::SkippedUnsupported,
					kind,
					"certificate bound is unknown or parameterized by an unbound symbol",
					input.execution,
					manifest,
					work.certificateId,
					{},
					{},
					RelayPairCertificateRelation::Unknown,
					{},
					{});
			}
			if (expected.empty())
				expected = std::to_string(*bound);
			const bool within = *observed <= *bound;
			return CertificateResult(
				within
					? ValidationStatus::Passed
					: ValidationStatus::Mismatch,
				kind,
				within
					? "observed work is within the certificate bound"
					: "observed work exceeds the certificate bound",
				input.execution,
				manifest,
				work.certificateId,
				{},
				{},
				RelayPairCertificateRelation::Unknown,
				{},
				{},
				nullptr,
				expected,
				std::to_string(*observed));
		};

		std::optional<uint64_t> directObserved;
		if (directSiteIdentityComplete)
			directObserved = static_cast<uint64_t>(directEmissions.size());
		results.push_back(workResult(
			certificate.directLogicalWork,
			ValidationCheckKind::DirectLogicalWork,
			directObserved,
			false));
		results.push_back(workResult(
			certificate.transitiveLogicalWork,
			ValidationCheckKind::TransitiveLogicalWork,
			input.observedTransitiveLogicalWork,
			false));
		results.push_back(workResult(
			certificate.physicalRouteWork,
			ValidationCheckKind::PhysicalRouteWork,
			input.observedPhysicalRouteWork,
			true));
		const std::optional<uint64_t> observedDepth =
			input.observedTransitiveDepth
				? std::optional<uint64_t>(*input.observedTransitiveDepth)
				: std::nullopt;
		results.push_back(workResult(
			certificate.relayTreeDepth,
			ValidationCheckKind::RelayTreeDepth,
			observedDepth,
			false));

		for (const ManifestRelayPairCertificate &pair :
			certificate.pairRelations)
		{
			ValidationCheckKind checkKind =
				ValidationCheckKind::Unknown;
			switch (pair.relation)
			{
			case RelayPairCertificateRelation::MutuallyExclusive:
				checkKind = ValidationCheckKind::
					CertificateMutuallyExclusive;
				break;
			case RelayPairCertificateRelation::MustPrecedeAB:
			case RelayPairCertificateRelation::MustPrecedeBA:
				checkKind = ValidationCheckKind::CertificateMustPrecede;
				break;
			case RelayPairCertificateRelation::CoEmissionIndependent:
				checkKind = ValidationCheckKind::
					CertificateCoEmissionIndependent;
				break;
			case RelayPairCertificateRelation::ProvedMayAlias:
				checkKind = ValidationCheckKind::
					CertificateProvedMayAlias;
				break;
			case RelayPairCertificateRelation::PotentialConflict:
			case RelayPairCertificateRelation::Unknown:
				break;
			}

			const auto left = emissionsBySite.find(pair.siteA);
			const auto right = emissionsBySite.find(pair.siteB);
			const std::vector<const RelayEmitTraceEvent *> empty;
			const auto &leftEvents = left == emissionsBySite.end()
				? empty
				: left->second;
			const auto &rightEvents = right == emissionsBySite.end()
				? empty
				: right->second;
			const RelayEmitTraceEvent *detailEmission =
				!leftEvents.empty()
					? leftEvents.front()
					: (!rightEvents.empty()
						? rightEvents.front()
						: nullptr);

			auto pairResult = [&](ValidationStatus status,
							  const std::string &reason,
							  const std::string &expected = std::string(),
							  const std::string &actual = std::string())
			{
				return CertificateResult(
					status,
					checkKind,
					reason,
					input.execution,
					manifest,
					pair.certificateId,
					pair.siteA,
					pair.siteB,
					pair.relation,
					pair.locationA,
					pair.locationB,
					detailEmission,
					expected,
					actual);
			};

			if (!directSiteIdentityComplete)
			{
				results.push_back(pairResult(
					ValidationStatus::SkippedUnsupported,
					"runtime relay site identity is incomplete for this parent"));
				continue;
			}

			if (pair.status != ParallelCertificateStatus::Proved ||
				pair.relation ==
					RelayPairCertificateRelation::PotentialConflict ||
				pair.relation == RelayPairCertificateRelation::Unknown)
			{
				results.push_back(pairResult(
					ValidationStatus::SkippedUnsupported,
					pair.reason.empty()
						? "pair certificate is not a proved runtime property"
						: pair.reason));
				continue;
			}

			if (pair.relation ==
				RelayPairCertificateRelation::MutuallyExclusive)
			{
				const bool violated =
					!leftEvents.empty() && !rightEvents.empty();
				results.push_back(pairResult(
					violated
						? ValidationStatus::Mismatch
						: ValidationStatus::Passed,
					violated
						? "mutually-exclusive relay sites were jointly emitted"
						: "no joint emission violated mutual exclusion",
					"sites are not jointly emitted",
					violated ? "both sites emitted" : "not jointly emitted"));
				continue;
			}

			if (leftEvents.size() > 1 || rightEvents.size() > 1)
			{
				results.push_back(pairResult(
					ValidationStatus::SkippedUnsupported,
					"multiple site occurrences require an occurrence-indexed certificate"));
				continue;
			}

			if (pair.relation ==
					RelayPairCertificateRelation::MustPrecedeAB ||
				pair.relation ==
					RelayPairCertificateRelation::MustPrecedeBA)
			{
				const auto &before = pair.relation ==
						RelayPairCertificateRelation::MustPrecedeAB
					? leftEvents
					: rightEvents;
				const auto &after = pair.relation ==
						RelayPairCertificateRelation::MustPrecedeAB
					? rightEvents
					: leftEvents;
				if (after.empty())
				{
					results.push_back(pairResult(
						ValidationStatus::NotApplicable,
						"the consequent relay site was not emitted"));
				}
				else
				{
					const bool ordered = !before.empty() &&
						before.front()->emissionSequence <
							after.front()->emissionSequence;
					results.push_back(pairResult(
						ordered
							? ValidationStatus::Passed
							: ValidationStatus::Mismatch,
						ordered
							? "observed relay emission order satisfies MustPrecede"
							: "consequent relay was emitted without a prior required site",
						"required predecessor has a smaller emission sequence",
						ordered ? "ordered" : "missing or later predecessor"));
				}
				continue;
			}

			if (leftEvents.empty() || rightEvents.empty())
			{
				results.push_back(pairResult(
					ValidationStatus::NotApplicable,
					"proved pair sites were not jointly emitted"));
				continue;
			}

			auto boundSite = [&](const std::string &siteId)
				-> const ManifestRelaySite *
			{
				const auto ordinal = manifest.ordinalBySiteId.find(siteId);
				if (ordinal == manifest.ordinalBySiteId.end())
					return nullptr;
				const auto site = manifest.sitesByOrdinal.find(ordinal->second);
				return site == manifest.sitesByOrdinal.end()
					? nullptr
					: &site->second;
			};
			const ManifestRelaySite *leftSite = boundSite(pair.siteA);
			const ManifestRelaySite *rightSite = boundSite(pair.siteB);
			std::string targetReason;
			if (leftSite == nullptr || rightSite == nullptr)
			{
				targetReason = "bound relay-site metadata is unavailable";
			}
			else if (TargetObservationComplete(
					*leftSite, *leftEvents.front(), targetReason))
			{
				TargetObservationComplete(
					*rightSite, *rightEvents.front(), targetReason);
			}
			if (!targetReason.empty())
			{
				results.push_back(pairResult(
					ValidationStatus::SkippedUnsupported,
					"runtime target observation is incomplete: " +
						targetReason));
				continue;
			}
			const bool sameTarget =
				leftEvents.front()->actualTargetScope ==
					rightEvents.front()->actualTargetScope &&
				leftEvents.front()->actualTarget ==
					rightEvents.front()->actualTarget;
			if (pair.relation ==
				RelayPairCertificateRelation::CoEmissionIndependent)
			{
				results.push_back(pairResult(
					sameTarget
						? ValidationStatus::Mismatch
						: ValidationStatus::Passed,
					sameTarget
						? "co-emission independence was violated by equal runtime targets"
						: "jointly emitted runtime targets are independent",
					"different targets",
					sameTarget ? "same target" : "different targets"));
			}
			else if (pair.relation ==
				RelayPairCertificateRelation::ProvedMayAlias)
			{
				results.push_back(pairResult(
					sameTarget
						? ValidationStatus::Passed
						: ValidationStatus::NotApplicable,
					sameTarget
						? "runtime execution observed the proved alias candidate"
						: "this execution did not realize the existential alias witness",
					"possible equal targets",
					sameTarget ? "same target" : "different targets"));
			}
		}
	}

	if (parallelFunction ==
		manifest.parallelCertificatesByFunction.end())
	{
		std::map<std::string, const RelayEmitTraceEvent *> coemitted;
		for (const RelayEmitTraceEvent *emission : directEmissions)
		{
			auto site = manifest.sitesByOrdinal.find(
				emission->relaySiteOrdinal);
			if (site != manifest.sitesByOrdinal.end())
				coemitted.emplace(site->second.id, emission);
		}
		for (const ManifestNonAliasProof &proof : manifest.nonAliasProofs)
		{
			if (!proof.solverProved)
				continue;
			auto left = coemitted.find(proof.leftRelaySiteId);
			auto right = coemitted.find(proof.rightRelaySiteId);
			if (left == coemitted.end() || right == coemitted.end())
			{
				results.push_back(Result(
					ValidationStatus::NotApplicable,
					ValidationCheckKind::CoemissionNonAlias,
					"proved sites were not jointly emitted by this parent microtransaction",
					input.execution,
					nullptr,
					manifest));
				continue;
			}

			const bool sameTarget =
				left->second->actualTargetScope ==
					right->second->actualTargetScope &&
				left->second->actualTarget == right->second->actualTarget;
			results.push_back(Result(
				sameTarget
					? ValidationStatus::Mismatch
					: ValidationStatus::Passed,
				ValidationCheckKind::CoemissionNonAlias,
				sameTarget
					? "Z3-proved co-emission non-alias was violated by runtime targets"
					: "jointly emitted runtime targets are non-aliased",
				input.execution,
				left->second,
				manifest,
				"different targets",
				sameTarget ? "same target" : "different targets"));
		}
	}

	return results;
}

} // namespace relay_trace
} // namespace oxd
