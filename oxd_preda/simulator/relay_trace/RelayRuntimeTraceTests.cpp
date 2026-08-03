#include "RelayManifestLoader.h"
#include "RelayTraceCollector.h"
#include "RelayTraceReport.h"
#include "RelayTraceValidator.h"

#include "../../3rdParty/nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace oxd {
namespace relay_trace {
namespace tests {
#ifdef RPREDA_ENABLE_TRACE_FAULT_INJECTION
int RunRelayTraceFaultInjectorUnitTests();
#endif
namespace {

using Json = nlohmann::ordered_json;

struct TestState
{
	int failures = 0;

	void Expect(bool condition, const std::string &message)
	{
		if (condition)
			return;
		++failures;
		std::cerr << "FAIL: " << message << '\n';
	}
};

struct OpaqueTransactionPool
{
	std::array<std::max_align_t, 128> storage{};

	const SimuTxn *At(size_t index) const
	{
		return reinterpret_cast<const SimuTxn *>(&storage.at(index));
	}
};

rvm::ContractInvokeId Contract(uint64_t value)
{
	return static_cast<rvm::ContractInvokeId>(value);
}

ExecutionBeginInput SourceBegin(
	const SimuTxn *transaction,
	const std::string &module,
	const std::string &function,
	uint32_t opcode = 1)
{
	ExecutionBeginInput input;
	input.transaction = transaction;
	input.contractInvoke = Contract(1);
	input.opcode = opcode;
	input.moduleId = module;
	input.sourceFunctionId = function;
	input.ownerShard = 0;
	return input;
}

RelayCreateInput CreateRelay(
	const SimuTxn *parent,
	const SimuTxn *child,
	uint32_t opcode = 9)
{
	static const uint8_t arguments[] = {0x10, 0x20, 0x30};
	RelayCreateInput input;
	input.parentTransaction = parent;
	input.childTransaction = child;
	input.targetContractInvoke = Contract(2);
	input.targetOpcode = opcode;
	input.serializedArguments = arguments;
	input.serializedArgumentsSize = sizeof(arguments);
	input.targetModuleId = "target-module";
	input.ownerShard = 0;
	return input;
}

RelayFinalizeInput FinalizeRelay(
	const SimuTxn *child,
	RelayKind kind,
	ScopeKind scope,
	const std::string &site,
	uint8_t target)
{
	RelayFinalizeInput input;
	input.childTransaction = child;
	input.targetData = nullptr;
	input.targetSize = 0;
	input.targetScope = scope;
	input.relayKind = kind;
	input.relaySiteId = site;

	// The caller replaces these with stable storage before invoking
	// FinalizeRelayEmission. This helper's scalar target is copied by value in
	// the test immediately below.
	(void)target;
	return input;
}

bool HasStatus(
	const std::vector<RelayValidationResult> &results,
	ValidationCheckKind kind,
	ValidationStatus status)
{
	for (const RelayValidationResult &result : results)
	{
		if (result.checkKind == kind && result.status == status)
			return true;
	}
	return false;
}

bool HasNonEmptyReason(
	const std::vector<RelayValidationResult> &results,
	ValidationCheckKind kind,
	ValidationStatus status)
{
	for (const RelayValidationResult &result : results)
	{
		if (result.checkKind == kind &&
			result.status == status &&
			!result.reason.empty())
		{
			return true;
		}
	}
	return false;
}

const RelayValidationResult *FindResult(
	const std::vector<RelayValidationResult> &results,
	ValidationCheckKind kind)
{
	for (const RelayValidationResult &result : results)
	{
		if (result.checkKind == kind)
			return &result;
	}
	return nullptr;
}

void TestNamedRelayTreeAndPointerReuse(TestState &state)
{
	OpaqueTransactionPool transactions;
	const SimuTxn *root = transactions.At(0);
	const SimuTxn *child = transactions.At(1);

	RelayTraceCollector collector(TraceMode::Observe);
	auto rootContext =
		collector.BeginExecution(SourceBegin(root, "module-a", "root"));
	state.Expect(rootContext.has_value(), "source execution is registered");

	const uint64_t generation = collector.PushMarker(root, "module-a", 7);
	state.Expect(generation != 0, "named relay marker is installed");
	MarkerOperationResult creation =
		collector.RegisterRelayCreation(CreateRelay(root, child, 42));
	state.Expect(
		creation.status == MarkerOperationStatus::Ok,
		"relay creation consumes the named marker");

	state.Expect(
		collector.SetPendingRelayResolvedIdentity(child, "site-7", "root"),
		"manifest identity resolves before finalization");
	const uint8_t target[] = {0xaa, 0xbb};
	RelayFinalizeInput finalize =
		FinalizeRelay(
			child,
			RelayKind::CustomScope,
			ScopeKind::Address,
			"site-7",
			0);
	finalize.targetData = target;
	finalize.targetSize = sizeof(target);
	state.Expect(
		collector.FinalizeRelayEmission(finalize),
		"logical emission finalizes exactly once");
	state.Expect(
		collector.PopMarker(root, generation, "module-a", 7, true).status ==
			MarkerOperationStatus::Ok,
		"consumed marker restores with matching module and ordinal");
	state.Expect(
		collector.RecordRoute(child, 3, RouteKind::CrossShard),
		"original runtime shard result is recorded");

	auto childBefore = collector.Lookup(child);
	state.Expect(childBefore.has_value(), "child side-table metadata exists");
	if (childBefore && rootContext)
	{
		state.Expect(
			childBefore->rootTraceTxId == rootContext->traceTxId,
			"child keeps source root trace id");
		state.Expect(
			childBefore->parentTraceTxId == rootContext->traceTxId,
			"child parent is the currently executing microtransaction");
		state.Expect(childBefore->depth == 1, "first relay depth is one");
	}

	auto childExecution = SourceBegin(child, "target-module", "handler", 42);
	childExecution.isRelay = true;
	state.Expect(
		collector.BeginExecution(childExecution).has_value(),
		"relay execution restores side-table metadata");
	state.Expect(
		collector.EndExecution(child, true, true),
		"relay execution metadata is erased before object release");
	state.Expect(
		collector.EndExecution(root, true, true),
		"source execution metadata is erased");

	const RelayTraceSnapshot snapshot = collector.Snapshot();
	state.Expect(
		snapshot.counters.logicalRelayEmissions == 1,
		"one source relay creates one logical event");
	state.Expect(
		snapshot.counters.physicalRelayRoutes == 1,
		"single-target relay creates one physical route");
	state.Expect(
		snapshot.timings.traceRecordingTimeMs > 0.0,
		"trace recording time is accumulated without recursive locking");
	state.Expect(
		snapshot.emissions.size() == 1 &&
			snapshot.emissions.front().relaySiteOrdinal == 7 &&
			snapshot.emissions.front().relaySiteId == "site-7" &&
			snapshot.emissions.front().actualOpcode == 42,
		"named relay event preserves site identity and opcode");

	// Local object addresses may be reused after EndExecution. A new
	// execution at the same address must receive a fresh trace identity.
	auto reused =
		collector.BeginExecution(SourceBegin(root, "module-a", "root"));
	state.Expect(
		reused && rootContext &&
			reused->traceTxId != rootContext->traceTxId,
		"pointer reuse cannot revive stale transaction metadata");
	collector.EndExecution(root, true, true);
}

void TestTraceSideTableDoesNotMutateTransactions(TestState &state)
{
	OpaqueTransactionPool transactions;
	std::memset(
		transactions.storage.data(),
		0xa5,
		sizeof(transactions.storage));
	std::array<unsigned char, sizeof(transactions.storage)> before{};
	std::memcpy(
		before.data(),
		transactions.storage.data(),
		before.size());

	const SimuTxn *root = transactions.At(0);
	const SimuTxn *child = transactions.At(1);
	RelayTraceCollector collector(TraceMode::Observe);
	collector.BeginExecution(SourceBegin(root, "module-a", "root"));
	const uint64_t generation =
		collector.PushMarker(root, "module-a", 3);
	collector.RegisterRelayCreation(CreateRelay(root, child, 17));
	collector.SetPendingRelayResolvedIdentity(child, "site-3", "root");
	const uint8_t target[] = {0x01, 0x02, 0x03, 0x04};
	RelayFinalizeInput finalize =
		FinalizeRelay(
			child,
			RelayKind::CustomScope,
			ScopeKind::Uint32,
			"site-3",
			0);
	finalize.targetData = target;
	finalize.targetSize = sizeof(target);
	collector.FinalizeRelayEmission(finalize);
	collector.PopMarker(root, generation, "module-a", 3, true);
	collector.RecordRoute(child, 0, RouteKind::IntraShard, 1);
	auto relayBegin =
		SourceBegin(child, "target-module", "handler", 17);
	relayBegin.isRelay = true;
	collector.BeginExecution(relayBegin);
	collector.EndExecution(child, true, true);
	collector.EndExecution(root, true, true);

	state.Expect(
		std::memcmp(
			before.data(),
			transactions.storage.data(),
			before.size()) == 0,
		"trace metadata remains in the side table and never mutates "
		"transaction object bytes");
}

void TestOccurrencesAndCrossModuleIdentity(TestState &state)
{
	OpaqueTransactionPool transactions;
	const SimuTxn *root = transactions.At(0);
	RelayTraceCollector collector(TraceMode::Observe);
	collector.BeginExecution(SourceBegin(root, "module-a", "root"));

	for (uint32_t index = 0; index < 3; ++index)
	{
		const SimuTxn *child = transactions.At(index + 1);
		const uint64_t generation =
			collector.PushMarker(root, "module-a", 5);
		collector.RegisterRelayCreation(CreateRelay(root, child));
		const uint8_t target = static_cast<uint8_t>(index);
		RelayFinalizeInput finalize =
			FinalizeRelay(
				child,
				RelayKind::CustomScope,
				ScopeKind::Uint32,
				"loop-site",
				target);
		finalize.targetData = &target;
		finalize.targetSize = sizeof(target);
		collector.SetPendingRelayResolvedIdentity(
			child,
			"loop-site",
			"root");
		collector.FinalizeRelayEmission(finalize);
		collector.PopMarker(root, generation, "module-a", 5, true);
	}

	const SimuTxn *crossModuleChild = transactions.At(10);
	const uint64_t crossGeneration =
		collector.PushMarker(root, "module-b", 5);
	collector.RegisterRelayCreation(
		CreateRelay(root, crossModuleChild, 77));
	const uint8_t crossTarget = 9;
	RelayFinalizeInput crossFinalize =
		FinalizeRelay(
			crossModuleChild,
			RelayKind::Global,
			ScopeKind::Global,
			"module-b-site",
			crossTarget);
	crossFinalize.targetData = &crossTarget;
	crossFinalize.targetSize = sizeof(crossTarget);
	collector.SetPendingRelayResolvedIdentity(
		crossModuleChild,
		"module-b-site",
		"module-b-function");
	collector.FinalizeRelayEmission(crossFinalize);
	collector.PopMarker(
		root,
		crossGeneration,
		"module-b",
		5,
		true);

	const RelayTraceSnapshot snapshot = collector.Snapshot();
	state.Expect(snapshot.emissions.size() == 4, "four emissions recorded");
	if (snapshot.emissions.size() == 4)
	{
		state.Expect(
			snapshot.emissions[0].occurrenceIndex == 0 &&
				snapshot.emissions[1].occurrenceIndex == 1 &&
				snapshot.emissions[2].occurrenceIndex == 2,
			"bounded-loop site occurrence is parent-local and monotonic");
		state.Expect(
			snapshot.emissions[3].occurrenceIndex == 0,
			"same ordinal in another module has an independent occurrence");
		state.Expect(
			snapshot.emissions[0].emissionSequence == 0 &&
				snapshot.emissions[1].emissionSequence == 1 &&
				snapshot.emissions[2].emissionSequence == 2 &&
				snapshot.emissions[3].emissionSequence == 3,
			"one parent has a total logical emission order across sites and modules");
		state.Expect(
			snapshot.emissions[3].sourceModuleId == "module-b" &&
				snapshot.emissions[3].sourceFunctionId ==
					"module-b-function",
			"cross-contract relay records the emitting module and function");
	}
	collector.ShutdownClearLiveTransactions();
}

void TestMarkerFailuresRemainTraceable(TestState &state)
{
	OpaqueTransactionPool transactions;
	const SimuTxn *root = transactions.At(0);
	const SimuTxn *missingChild = transactions.At(1);
	RelayTraceCollector collector(TraceMode::Strict);
	collector.BeginExecution(SourceBegin(root, "module-a", "root"));

	MarkerOperationResult missing =
		collector.RegisterRelayCreation(CreateRelay(root, missingChild));
	state.Expect(
		missing.status == MarkerOperationStatus::MissingMarker,
		"missing marker is reported");
	auto pending = collector.PendingRelayIdentity(missingChild);
	state.Expect(
		pending &&
			pending->relaySiteOrdinal == InvalidRelaySiteOrdinal,
		"missing marker still creates invalid-ordinal child metadata");

	const SimuTxn *firstConsumeChild = transactions.At(2);
	const SimuTxn *duplicateConsumeChild = transactions.At(3);
	const uint64_t consumeGeneration =
		collector.PushMarker(root, "module-a", 4);
	state.Expect(
		collector.RegisterRelayCreation(
			CreateRelay(root, firstConsumeChild)).status ==
			MarkerOperationStatus::Ok,
		"first relay creation consumes its active marker");
	MarkerOperationResult duplicate =
		collector.RegisterRelayCreation(
			CreateRelay(root, duplicateConsumeChild));
	state.Expect(
		duplicate.status == MarkerOperationStatus::DuplicateConsume,
		"second creation under one marker is a duplicate consume");
	auto duplicatePending =
		collector.PendingRelayIdentity(duplicateConsumeChild);
	state.Expect(
		duplicatePending &&
			duplicatePending->relaySiteOrdinal ==
				InvalidRelaySiteOrdinal,
		"duplicate consume remains traceable as an invalid site");
	state.Expect(
		collector.PopMarker(
			root,
			consumeGeneration,
			"module-a",
			4,
			true).status == MarkerOperationStatus::Ok,
		"consumed marker restores after duplicate detection");

	const uint64_t outer = collector.PushMarker(root, "module-a", 1);
	const uint64_t inner = collector.PushMarker(root, "module-a", 2);
	state.Expect(
		collector.PopMarker(root, outer, "module-a", 1).status ==
			MarkerOperationStatus::StaleMarker,
		"out-of-order restore is a stale marker");
	state.Expect(
		collector.PopMarker(root, inner, "module-a", 2).status ==
			MarkerOperationStatus::Ok,
		"nested marker restores the active frame");
	state.Expect(
		collector.PopMarker(root, outer, "module-a", 1).status ==
			MarkerOperationStatus::Ok,
		"outer marker is restored after nested scope");
	state.Expect(
		collector.StrictFailureLatched(),
		"strict mode deterministically latches instrumentation failure");

	const RelayTraceSnapshot snapshot = collector.Snapshot();
	state.Expect(
		snapshot.counters.instrumentationFailures >= 2,
		"marker failures remain in the validation report");
	collector.ShutdownClearLiveTransactions();
}

void TestUnconsumedMarkerRestoreIsDeterministic(TestState &state)
{
	OpaqueTransactionPool transactions;
	const SimuTxn *root = transactions.At(0);
	const SimuTxn *unexpectedChild = transactions.At(1);
	const SimuTxn *nextChild = transactions.At(2);
	RelayTraceCollector collector(TraceMode::Observe);
	collector.BeginExecution(SourceBegin(root, "module-a", "root"));

	// Models a generated relay wrapper unwinding before the relay runtime call:
	// the scope guard restores a marker which was never consumed.
	const uint64_t unwoundGeneration =
		collector.PushMarker(root, "module-a", 8);
	MarkerOperationResult unwound = collector.PopMarker(
		root,
		unwoundGeneration,
		"module-a",
		8,
		true);
	state.Expect(
		unwound.status == MarkerOperationStatus::MissingMarker,
		"unconsumed marker restoration remains diagnostic");

	MarkerOperationResult afterUnwind =
		collector.RegisterRelayCreation(CreateRelay(root, unexpectedChild));
	state.Expect(
		afterUnwind.status == MarkerOperationStatus::MissingMarker,
		"unconsumed restoration pops the frame instead of leaking it");

	const uint64_t nextGeneration =
		collector.PushMarker(root, "module-a", 9);
	state.Expect(
		collector.RegisterRelayCreation(
			CreateRelay(root, nextChild)).status ==
			MarkerOperationStatus::Ok,
		"a later wrapper starts with a clean marker stack");
	state.Expect(
		collector.PopMarker(
			root,
			nextGeneration,
			"module-a",
			9,
			true).status == MarkerOperationStatus::Ok,
		"later consumed marker restores normally");
	collector.ShutdownClearLiveTransactions();
}

void TestNestedRelayTree(TestState &state)
{
	OpaqueTransactionPool transactions;
	const SimuTxn *root = transactions.At(0);
	const SimuTxn *first = transactions.At(1);
	const SimuTxn *nested = transactions.At(2);
	RelayTraceCollector collector(TraceMode::Observe);

	auto rootContext =
		collector.BeginExecution(SourceBegin(root, "module-a", "root"));
	uint64_t firstGeneration =
		collector.PushMarker(root, "module-a", 0);
	collector.RegisterRelayCreation(CreateRelay(root, first, 10));
	const uint8_t firstTarget = 1;
	RelayFinalizeInput firstFinalize =
		FinalizeRelay(
			first,
			RelayKind::CustomScope,
			ScopeKind::Address,
			"site-root",
			firstTarget);
	firstFinalize.targetData = &firstTarget;
	firstFinalize.targetSize = sizeof(firstTarget);
	collector.SetPendingRelayResolvedIdentity(first, "site-root", "root");
	collector.FinalizeRelayEmission(firstFinalize);
	collector.PopMarker(
		root,
		firstGeneration,
		"module-a",
		0,
		true);
	auto firstBegin = SourceBegin(first, "module-a", "lambda-handler", 10);
	firstBegin.isRelay = true;
	auto firstContext = collector.BeginExecution(firstBegin);

	uint64_t nestedGeneration =
		collector.PushMarker(first, "module-a", 1);
	collector.RegisterRelayCreation(CreateRelay(first, nested, 11));
	const uint8_t nestedTarget = 2;
	RelayFinalizeInput nestedFinalize =
		FinalizeRelay(
			nested,
			RelayKind::DeferredNext,
			ScopeKind::Address,
			"site-nested",
			nestedTarget);
	nestedFinalize.targetData = &nestedTarget;
	nestedFinalize.targetSize = sizeof(nestedTarget);
	collector.SetPendingRelayResolvedIdentity(
		nested,
		"site-nested",
		"lambda-handler");
	collector.FinalizeRelayEmission(nestedFinalize);
	collector.PopMarker(
		first,
		nestedGeneration,
		"module-a",
		1,
		true);

	auto nestedContext = collector.Lookup(nested);
	state.Expect(
		rootContext && firstContext && nestedContext &&
			firstContext->depth == 1 &&
			nestedContext->depth == 2,
		"nested relay depth is reconstructed from side-table parents");
	if (rootContext && firstContext && nestedContext)
	{
		state.Expect(
			nestedContext->rootTraceTxId == rootContext->traceTxId,
			"nested relay keeps the source transaction root");
		state.Expect(
			nestedContext->parentTraceTxId == firstContext->traceTxId,
			"nested relay parent is the first relay execution");
	}
	collector.ShutdownClearLiveTransactions();
}

void TestBroadcastLogicalPhysicalSplit(TestState &state)
{
	OpaqueTransactionPool transactions;
	const SimuTxn *root = transactions.At(0);
	const SimuTxn *original = transactions.At(1);
	RelayTraceCollector collector(TraceMode::Observe);
	collector.BeginExecution(SourceBegin(root, "module-a", "root"));

	const uint64_t generation =
		collector.PushMarker(root, "module-a", 3);
	collector.RegisterRelayCreation(CreateRelay(root, original, 12));
	const uint8_t target = 0;
	RelayFinalizeInput finalize =
		FinalizeRelay(
			original,
			RelayKind::AllShards,
			ScopeKind::Shard,
			"broadcast-site",
			target);
	finalize.targetData = &target;
	finalize.targetSize = sizeof(target);
	collector.SetPendingRelayResolvedIdentity(
		original,
		"broadcast-site",
		"root");
	collector.FinalizeRelayEmission(finalize);
	collector.PopMarker(root, generation, "module-a", 3, true);

	std::vector<uint64_t> physicalIds;
	auto originalContext = collector.Lookup(original);
	if (originalContext)
		physicalIds.push_back(originalContext->traceTxId);
	collector.RecordRoute(
		original,
		0,
		RouteKind::AllShardsBroadcast,
		4);
	for (uint32_t shard = 1; shard < 4; ++shard)
	{
		const SimuTxn *clone = transactions.At(shard + 1);
		state.Expect(
			collector.CloneRelayMetadata(original, clone, shard),
			"broadcast clone receives trace-only metadata");
		auto cloneContext = collector.Lookup(clone);
		if (cloneContext)
			physicalIds.push_back(cloneContext->traceTxId);
		collector.RecordRoute(
			clone,
			shard,
			RouteKind::AllShardsBroadcast,
			4);
	}

	const RelayTraceSnapshot snapshot = collector.Snapshot();
	state.Expect(
		snapshot.counters.logicalRelayEmissions == 1 &&
			snapshot.counters.broadcastLogicalEmissions == 1,
		"relay@shards remains one logical source event");
	state.Expect(
		snapshot.counters.physicalRelayRoutes == 4 &&
			snapshot.counters.broadcastPhysicalClones == 4,
		"relay@shards records one physical route per active shard");
	state.Expect(
		std::all_of(
			snapshot.routes.begin(),
			snapshot.routes.end(),
			[](const RelayRouteTraceEvent &route)
			{
				return route.activeShardCount == 4;
			}),
		"every physical route snapshots the active shard count");
	std::sort(physicalIds.begin(), physicalIds.end());
	state.Expect(
		physicalIds.size() == 4 &&
			std::adjacent_find(
				physicalIds.begin(),
				physicalIds.end()) == physicalIds.end(),
		"every physical broadcast transaction has a distinct trace id");

	RelayTraceReport report;
	const Json parsed =
		Json::parse(report.BuildJson(snapshot, false).contents);
	const std::string jsonLines =
		report.BuildJsonLines(snapshot).contents;
	state.Expect(
		parsed["logical_relay_emissions"].size() == 1 &&
			parsed["logical_relay_emissions"][0]
				["physical_clone_count"].get<uint64_t>() == 4,
		"broadcast logical report group includes its physical clone count");
	state.Expect(
		parsed["physical_relay_routes"].size() == 4 &&
			std::all_of(
				parsed["physical_relay_routes"].begin(),
				parsed["physical_relay_routes"].end(),
				[](const Json &route)
				{
					return route["active_shard_count"].
						get<uint32_t>() == 4;
				}),
		"route report emits active_shard_count");
	state.Expect(
		jsonLines.find("\"physical_clone_count\":4") !=
				std::string::npos &&
			jsonLines.find("\"active_shard_count\":4") !=
				std::string::npos,
		"JSONL report preserves broadcast and shard-count metadata");
	collector.ShutdownClearLiveTransactions();
}

LoadedRelayManifest BuildValidationManifest()
{
	LoadedRelayManifest manifest;
	manifest.schemaVersion = 5;
	manifest.binding.contract = "Contract";
	manifest.binding.moduleId = "module-a";

	for (uint32_t ordinal = 0; ordinal < 2; ++ordinal)
	{
		ManifestRelaySite site;
		site.ordinal = ordinal;
		site.id = "site-" + std::to_string(ordinal);
		site.sourceFunctionId = "root";
		site.handlerId = "handler";
		site.expectedOpcode = 42 + ordinal;
		site.handlerResolved = true;
		site.relayKind = RelayKind::CustomScope;
		site.targetScope = ScopeKind::Address;
		manifest.ordinalBySiteId.emplace(site.id, ordinal);
		manifest.sitesByOrdinal.emplace(ordinal, std::move(site));
	}

	ManifestFunctionSummary summary;
	summary.sourceFunctionId = "root";
	summary.exactDirectRelayCount = 2;
	summary.directRelayCountUpperBound = 2;
	summary.maximumDepth = 1;
	manifest.functionsById.emplace("root", std::move(summary));

	ManifestNonAliasProof proof;
	proof.obligationId = "nonalias-0-1";
	proof.leftRelaySiteId = "site-0";
	proof.rightRelaySiteId = "site-1";
	proof.solverProved = true;
	manifest.nonAliasProofs.push_back(std::move(proof));
	return manifest;
}

ManifestWorkCertificate RuntimeWorkCertificate(
	const std::string &id,
	uint64_t upperBound,
	ParallelCertificateStatus status =
		ParallelCertificateStatus::Complete)
{
	ManifestWorkCertificate work;
	work.certificateId = id;
	work.status = status;
	work.exact = upperBound;
	work.upperBound = upperBound;
	work.boundKind = relay_plan::ManifestWorkBoundKind::Constant;
	work.constantTerm = upperBound;
	work.expression = std::to_string(upperBound);
	work.reason = "runtime test certificate";
	return work;
}

ManifestRelayPairCertificate RuntimePairCertificate(
	RelayPairCertificateRelation relation,
	const std::string &id = "pair-runtime-root")
{
	ManifestRelayPairCertificate pair;
	pair.certificateId = id;
	pair.siteA = "site-0";
	pair.siteB = "site-1";
	pair.relation = relation;
	pair.status = relation ==
			RelayPairCertificateRelation::PotentialConflict ||
		relation == RelayPairCertificateRelation::Unknown
		? ParallelCertificateStatus::Conservative
		: ParallelCertificateStatus::Proved;
	pair.reason = "runtime pair test certificate";
	pair.locationA.line = 3;
	pair.locationA.column = 4;
	pair.locationB.line = 9;
	pair.locationB.column = 2;
	return pair;
}

LoadedRelayManifest BuildParallelValidationManifest(
	RelayPairCertificateRelation relation)
{
	LoadedRelayManifest manifest = BuildValidationManifest();
	manifest.nonAliasProofs.clear();
	manifest.parallelCertificateExtensionSchemaVersion = 1;
	ManifestFunctionParallelCertificate certificate;
	certificate.sourceFunctionId = "root";
	certificate.status = ParallelCertificateStatus::Conservative;
	certificate.reason = "runtime test function certificate";
	certificate.pairRelations.push_back(
		RuntimePairCertificate(relation));
	certificate.directLogicalWork =
		RuntimeWorkCertificate("work-direct-runtime-root", 2);
	certificate.transitiveLogicalWork = RuntimeWorkCertificate(
		"work-transitive-runtime-root",
		4,
		ParallelCertificateStatus::Conservative);
	certificate.physicalRouteWork = RuntimeWorkCertificate(
		"work-physical-runtime-root",
		5,
		ParallelCertificateStatus::Conservative);
	certificate.physicalRouteWork.boundKind =
		relay_plan::ManifestWorkBoundKind::ParameterizedUpperBound;
	certificate.physicalRouteWork.constantTerm = 1;
	certificate.physicalRouteWork.activeShardCountCoefficient = 1;
	certificate.physicalRouteWork.expression =
		"1 + active_shard_count";
	certificate.relayTreeDepth = RuntimeWorkCertificate(
		"work-depth-runtime-root",
		2,
		ParallelCertificateStatus::Conservative);
	manifest.parallelCertificatesByFunction.emplace(
		"root",
		std::move(certificate));
	return manifest;
}

RelayEmitTraceEvent ValidationEmission(
	uint32_t ordinal,
	uint32_t opcode,
	uint8_t target)
{
	RelayEmitTraceEvent emission;
	emission.childTraceTxId = 100 + ordinal;
	emission.rootTraceTxId = 1;
	emission.parentTraceTxId = 1;
	emission.relaySiteOrdinal = ordinal;
	emission.relaySiteId = "site-" + std::to_string(ordinal);
	emission.emissionSequence = ordinal;
	emission.sourceModuleId = "module-a";
	emission.sourceFunctionId = "root";
	emission.actualOpcode = opcode;
	emission.actualTargetScope = ScopeKind::Address;
	std::array<uint8_t, 36> targetBytes{};
	targetBytes.front() = target;
	emission.actualTarget.Assign(targetBytes.data(), targetBytes.size());
	emission.relayKind = RelayKind::CustomScope;
	emission.depth = 1;
	return emission;
}

RelayRouteTraceEvent ValidationRoute(uint32_t ordinal, uint32_t shard)
{
	RelayRouteTraceEvent route;
	route.physicalTraceTxId = 100 + ordinal;
	route.rootTraceTxId = 1;
	route.parentTraceTxId = 1;
	route.relaySiteOrdinal = ordinal;
	route.relaySiteId = "site-" + std::to_string(ordinal);
	route.sourceModuleId = "module-a";
	route.targetShard = shard;
	route.routeKind =
		shard == 0 ? RouteKind::IntraShard : RouteKind::CrossShard;
	return route;
}

void TestValidatorAndNonAlias(TestState &state)
{
	LoadedRelayManifest manifest = BuildValidationManifest();
	RelayExecutionValidationInput input;
	input.execution.traceTxId = 1;
	input.execution.rootTraceTxId = 1;
	input.execution.moduleId = "module-a";
	input.execution.sourceFunctionId = "root";
	input.execution.opcode = 1;
	input.execution.ownerShard = 0;
	input.emissions = {
		ValidationEmission(0, 42, 1),
		ValidationEmission(1, 43, 2),
	};
	input.routes = {
		ValidationRoute(0, 0),
		ValidationRoute(1, 3),
	};
	RelayRouteTraceEvent unrelatedCrossModuleRoute =
		ValidationRoute(0, 2);
	unrelatedCrossModuleRoute.sourceModuleId = "module-b";
	input.routes.push_back(std::move(unrelatedCrossModuleRoute));
	input.observedTransitiveDepth = 1;

	RelayTraceValidator validator;
	auto passed = validator.ValidateExecution(manifest, input);
	state.Expect(
		!HasStatus(
			passed,
			ValidationCheckKind::CoemissionNonAlias,
			ValidationStatus::Mismatch),
		"different co-emitted targets satisfy a proved non-alias property");
	state.Expect(
		!HasStatus(
			passed,
			ValidationCheckKind::Fanout,
			ValidationStatus::Mismatch),
		"route correlation includes source module for equal ordinals");
	state.Expect(
		HasStatus(
			passed,
			ValidationCheckKind::DirectCount,
			ValidationStatus::Passed) &&
			HasStatus(
				passed,
				ValidationCheckKind::CountUpperBound,
				ValidationStatus::Passed) &&
			HasStatus(
				passed,
				ValidationCheckKind::Depth,
				ValidationStatus::Passed),
		"constant direct count, upper bound and completed depth validate");
	state.Expect(
		HasNonEmptyReason(
			passed,
			ValidationCheckKind::TargetRelation,
			ValidationStatus::SkippedUnsupported) &&
			HasNonEmptyReason(
				passed,
				ValidationCheckKind::ArgumentRelation,
				ValidationStatus::SkippedUnsupported) &&
			HasNonEmptyReason(
				passed,
				ValidationCheckKind::GuardNecessity,
				ValidationStatus::SkippedUnsupported),
		"unsupported Formula IR replay is explicit and diagnostic");

	input.emissions[1].actualTarget = input.emissions[0].actualTarget;
	auto mismatched = validator.ValidateExecution(manifest, input);
	state.Expect(
		HasStatus(
			mismatched,
			ValidationCheckKind::CoemissionNonAlias,
			ValidationStatus::Mismatch),
		"same injected target violates a proved co-emission non-alias goal");

	input.emissions.resize(1);
	input.routes.resize(1);
	auto notApplicable = validator.ValidateExecution(manifest, input);
	state.Expect(
		HasStatus(
			notApplicable,
			ValidationCheckKind::CoemissionNonAlias,
			ValidationStatus::NotApplicable),
		"non-co-emitted sites are NotApplicable, not a non-alias failure");

	RelayExecutionValidationInput wrongCrossTarget = input;
	wrongCrossTarget.emissions = {
		ValidationEmission(0, 42, 1),
		ValidationEmission(1, 43, 2),
	};
	wrongCrossTarget.routes = {
		ValidationRoute(0, 0),
		ValidationRoute(1, 0),
	};
	wrongCrossTarget.routes[1].routeKind = RouteKind::CrossShard;
	auto wrongCrossResults =
		validator.ValidateExecution(manifest, wrongCrossTarget);
	state.Expect(
		HasStatus(
			wrongCrossResults,
			ValidationCheckKind::Routing,
			ValidationStatus::Mismatch),
		"cross-shard route cannot target the executing owner shard");
}

RelayExecutionValidationInput ParallelValidationInput()
{
	RelayExecutionValidationInput input;
	input.execution.traceTxId = 1;
	input.execution.rootTraceTxId = 1;
	input.execution.moduleId = "module-a";
	input.execution.sourceFunctionId = "root";
	input.execution.opcode = 1;
	input.execution.ownerShard = 0;
	input.activeShardCount = 4;
	input.emissions = {
		ValidationEmission(0, 42, 1),
		ValidationEmission(1, 43, 2),
	};
	input.routes = {
		ValidationRoute(0, 0),
		ValidationRoute(1, 3),
	};
	input.observedTransitiveLogicalWork = 4;
	input.observedPhysicalRouteWork = 5;
	input.observedTransitiveDepth = 2;
	return input;
}

void TestParallelCertificateValidation(TestState &state)
{
	RelayTraceValidator validator;

	{
		LoadedRelayManifest manifest = BuildParallelValidationManifest(
			RelayPairCertificateRelation::MutuallyExclusive);
		RelayExecutionValidationInput input = ParallelValidationInput();
		auto results = validator.ValidateExecution(manifest, input);
		state.Expect(
			HasStatus(
				results,
				ValidationCheckKind::CertificateMutuallyExclusive,
				ValidationStatus::Mismatch),
			"joint runtime emission violates a proved mutual-exclusion certificate");
		const RelayValidationResult *detail = FindResult(
			results,
			ValidationCheckKind::CertificateMutuallyExclusive);
		state.Expect(
			detail != nullptr &&
				detail->detail.propertyId == "pair-runtime-root" &&
				detail->detail.certificateId == "pair-runtime-root" &&
				detail->detail.siteA == "site-0" &&
				detail->detail.siteB == "site-1" &&
				detail->detail.certificateRelation ==
					RelayPairCertificateRelation::MutuallyExclusive &&
				detail->detail.sourceLocation.line == 3 &&
				detail->detail.relatedSourceLocation.line == 9,
			"certificate mismatch reports property, pair, relation, and both locations");
		input.emissions.pop_back();
		input.routes.pop_back();
		results = validator.ValidateExecution(manifest, input);
		state.Expect(
			HasStatus(
				results,
				ValidationCheckKind::CertificateMutuallyExclusive,
				ValidationStatus::Passed),
			"a single emitted site satisfies mutual exclusion");
	}

	{
		LoadedRelayManifest manifest = BuildParallelValidationManifest(
			RelayPairCertificateRelation::MustPrecedeAB);
		RelayExecutionValidationInput input = ParallelValidationInput();
		auto ordered = validator.ValidateExecution(manifest, input);
		state.Expect(
			HasStatus(
				ordered,
				ValidationCheckKind::CertificateMustPrecede,
				ValidationStatus::Passed),
			"per-parent emission sequence validates MustPrecede");
		std::swap(
			input.emissions[0].emissionSequence,
			input.emissions[1].emissionSequence);
		auto reversed = validator.ValidateExecution(manifest, input);
		state.Expect(
			HasStatus(
				reversed,
				ValidationCheckKind::CertificateMustPrecede,
				ValidationStatus::Mismatch),
			"reversed emission sequence violates MustPrecede");
		input.emissions.push_back(input.emissions.front());
		input.emissions.back().occurrenceIndex = 1;
		auto repeated = validator.ValidateExecution(manifest, input);
		state.Expect(
			HasStatus(
				repeated,
				ValidationCheckKind::CertificateMustPrecede,
				ValidationStatus::SkippedUnsupported),
			"multiple occurrences do not receive an unsound ordering result");
	}

	{
		LoadedRelayManifest manifest = BuildParallelValidationManifest(
			RelayPairCertificateRelation::CoEmissionIndependent);
		RelayExecutionValidationInput input = ParallelValidationInput();
		// A synchronous helper keeps the relay site's static function
		// identity while the certificate is selected by the relevant root
		// invocation in input.execution.sourceFunctionId.
		manifest.sitesByOrdinal.at(1).sourceFunctionId = "sync-helper";
		input.emissions[1].sourceFunctionId = "sync-helper";
		auto independent = validator.ValidateExecution(manifest, input);
		state.Expect(
			HasStatus(
				independent,
				ValidationCheckKind::CertificateCoEmissionIndependent,
				ValidationStatus::Passed),
			"root certificate composes synchronous-helper relay sites without changing static site ownership");
		const RelayValidationResult *helperDetail = FindResult(
			independent,
			ValidationCheckKind::CertificateCoEmissionIndependent);
		state.Expect(
			helperDetail != nullptr &&
				helperDetail->detail.function == "root",
			"certificate reports retain relevant-root ownership across synchronous helpers");

		RelayExecutionValidationInput unknownScope = input;
		unknownScope.emissions[0].actualTargetScope = ScopeKind::Unknown;
		auto unknownScopeResults =
			validator.ValidateExecution(manifest, unknownScope);
		state.Expect(
			HasStatus(
				unknownScopeResults,
				ValidationCheckKind::CertificateCoEmissionIndependent,
				ValidationStatus::SkippedUnsupported),
			"target-based certificates require a known runtime target scope");

		LoadedRelayManifest mismatchedScopeManifest = manifest;
		mismatchedScopeManifest.sitesByOrdinal.at(0).targetScope =
			ScopeKind::Uint32;
		auto mismatchedScopeResults =
			validator.ValidateExecution(mismatchedScopeManifest, input);
		state.Expect(
			HasStatus(
				mismatchedScopeResults,
				ValidationCheckKind::CertificateCoEmissionIndependent,
				ValidationStatus::SkippedUnsupported),
			"target-based certificates require runtime scope to match the bound manifest");

		RelayExecutionValidationInput incompleteTarget = input;
		incompleteTarget.emissions[0].actualTarget.bytes.clear();
		auto incompleteTargetResults =
			validator.ValidateExecution(manifest, incompleteTarget);
		state.Expect(
			HasStatus(
				incompleteTargetResults,
				ValidationCheckKind::CertificateCoEmissionIndependent,
				ValidationStatus::SkippedUnsupported),
			"target-based certificates reject incomplete keyed target bytes");

		RelayExecutionValidationInput singleEmission = input;
		singleEmission.emissions.pop_back();
		singleEmission.routes.pop_back();
		auto notApplicable =
			validator.ValidateExecution(manifest, singleEmission);
		state.Expect(
			HasStatus(
				notApplicable,
				ValidationCheckKind::CertificateCoEmissionIndependent,
				ValidationStatus::NotApplicable),
			"co-emission independence is NotApplicable when only one certified site emits");
		input.emissions[1].actualTarget = input.emissions[0].actualTarget;
		auto aliased = validator.ValidateExecution(manifest, input);
		state.Expect(
			HasStatus(
				aliased,
				ValidationCheckKind::CertificateCoEmissionIndependent,
				ValidationStatus::Mismatch),
			"equal observed targets violate co-emission independence");
	}

	{
		LoadedRelayManifest manifest = BuildParallelValidationManifest(
			RelayPairCertificateRelation::ProvedMayAlias);
		RelayExecutionValidationInput input = ParallelValidationInput();
		input.emissions[1].actualTarget = input.emissions[0].actualTarget;
		auto observed = validator.ValidateExecution(manifest, input);
		state.Expect(
			HasStatus(
				observed,
				ValidationCheckKind::CertificateProvedMayAlias,
				ValidationStatus::Passed),
			"equal runtime targets record an existential alias observation");
		input.emissions[1] = ValidationEmission(1, 43, 2);
		auto notObserved = validator.ValidateExecution(manifest, input);
		state.Expect(
			HasStatus(
				notObserved,
				ValidationCheckKind::CertificateProvedMayAlias,
				ValidationStatus::NotApplicable),
			"one execution need not realize a proved may-alias witness");
	}
}

void TestParallelCertificateWorkBoundsAndStrictLatch(TestState &state)
{
	LoadedRelayManifest manifest = BuildParallelValidationManifest(
		RelayPairCertificateRelation::PotentialConflict);
	RelayExecutionValidationInput input = ParallelValidationInput();
	RelayTraceValidator validator;
	auto passed = validator.ValidateExecution(manifest, input);
	state.Expect(
		HasStatus(
			passed,
			ValidationCheckKind::DirectLogicalWork,
			ValidationStatus::Passed) &&
			HasStatus(
				passed,
				ValidationCheckKind::TransitiveLogicalWork,
				ValidationStatus::Passed) &&
			HasStatus(
				passed,
				ValidationCheckKind::PhysicalRouteWork,
				ValidationStatus::Passed) &&
			HasStatus(
				passed,
				ValidationCheckKind::RelayTreeDepth,
				ValidationStatus::Passed),
		"complete and conservative finite certificate bounds validate observed work");
	state.Expect(
		HasStatus(
			passed,
			ValidationCheckKind::Unknown,
			ValidationStatus::SkippedUnsupported),
		"PotentialConflict remains an explicit unsupported runtime property");

	LoadedRelayManifest legacyUnmodeled = manifest;
	legacyUnmodeled.functionsById["root"]
		.hasUnmodeledRelayReachableCall = true;
	auto legacySkipped = validator.ValidateExecution(legacyUnmodeled, input);
	state.Expect(
		HasStatus(
			legacySkipped,
			ValidationCheckKind::DirectCount,
			ValidationStatus::SkippedUnsupported) &&
			HasStatus(
				legacySkipped,
				ValidationCheckKind::CountUpperBound,
				ValidationStatus::SkippedUnsupported) &&
			HasStatus(
				legacySkipped,
				ValidationCheckKind::Depth,
				ValidationStatus::SkippedUnsupported),
		"legacy count and depth checks skip relay-reachable synchronous calls instead of reporting a local-only false pass");
	state.Expect(
		HasStatus(
			legacySkipped,
			ValidationCheckKind::DirectLogicalWork,
			ValidationStatus::Passed) &&
			HasStatus(
				legacySkipped,
				ValidationCheckKind::TransitiveLogicalWork,
				ValidationStatus::Passed) &&
			HasStatus(
				legacySkipped,
				ValidationCheckKind::RelayTreeDepth,
				ValidationStatus::Passed),
		"Phase-E composed work/depth certificates remain active when legacy local-only summaries are skipped");

	LoadedRelayManifest directExceededManifest = manifest;
	directExceededManifest.parallelCertificatesByFunction["root"]
		.directLogicalWork = RuntimeWorkCertificate(
			"work-direct-runtime-root",
			1);
	auto directExceeded =
		validator.ValidateExecution(directExceededManifest, input);
	state.Expect(
		HasStatus(
			directExceeded,
			ValidationCheckKind::DirectLogicalWork,
			ValidationStatus::Mismatch),
		"direct logical work above the certified bound is a mismatch");

	input.observedTransitiveLogicalWork = 5;
	auto transitiveExceeded = validator.ValidateExecution(manifest, input);
	state.Expect(
		HasStatus(
			transitiveExceeded,
			ValidationCheckKind::TransitiveLogicalWork,
			ValidationStatus::Mismatch),
		"transitive logical work above the certified bound is a mismatch");

	input = ParallelValidationInput();
	input.observedTransitiveDepth = 3;
	auto depthExceeded = validator.ValidateExecution(manifest, input);
	state.Expect(
		HasStatus(
			depthExceeded,
			ValidationCheckKind::RelayTreeDepth,
			ValidationStatus::Mismatch),
		"relay-tree depth above the certified bound is a mismatch");

	input = ParallelValidationInput();
	input.observedPhysicalRouteWork = 6;
	auto exceeded = validator.ValidateExecution(manifest, input);
	state.Expect(
		HasStatus(
			exceeded,
			ValidationCheckKind::PhysicalRouteWork,
			ValidationStatus::Mismatch),
		"physical work checks constant plus coefficient times active shard count");

	RelayTraceCollector strictCollector(TraceMode::Strict);
	for (const RelayValidationResult &result : exceeded)
	{
		if (result.checkKind == ValidationCheckKind::PhysicalRouteWork)
			strictCollector.AddValidationResult(result);
	}
	state.Expect(
		strictCollector.StrictFailureLatched(),
		"certificate mismatch reaches the existing strict-mode safe-point latch");

	manifest.parallelCertificatesByFunction["root"]
		.transitiveLogicalWork.status = ParallelCertificateStatus::Unknown;
	manifest.parallelCertificatesByFunction["root"]
		.transitiveLogicalWork.upperBound.reset();
	manifest.parallelCertificatesByFunction["root"]
		.transitiveLogicalWork.exact.reset();
	input = ParallelValidationInput();
	auto unknown = validator.ValidateExecution(manifest, input);
	state.Expect(
		HasNonEmptyReason(
			unknown,
			ValidationCheckKind::TransitiveLogicalWork,
			ValidationStatus::SkippedUnsupported),
		"Unknown work certificates are never silently treated as finite bounds");

	input = ParallelValidationInput();
	input.emissions[1].sourceFunctionId.clear();
	auto incompleteIdentity = validator.ValidateExecution(manifest, input);
	state.Expect(
		HasStatus(
			incompleteIdentity,
			ValidationCheckKind::DirectLogicalWork,
			ValidationStatus::SkippedUnsupported) &&
			HasStatus(
				incompleteIdentity,
				ValidationCheckKind::Unknown,
				ValidationStatus::SkippedUnsupported),
		"incomplete static site identity suppresses direct-work and pair conclusions");

	const RelayValidationResult *physical = FindResult(
		exceeded,
		ValidationCheckKind::PhysicalRouteWork);
	RelayTraceSnapshot snapshot;
	if (physical != nullptr)
		snapshot.validationResults.push_back(*physical);
	RelayTraceReport report;
	const Json json = Json::parse(report.BuildJson(snapshot, false).contents);
	state.Expect(
		json["validation_results"].size() == 1 &&
			json["validation_results"][0]["detail"]["property_id"] ==
				"work-physical-runtime-root" &&
			json["validation_results"][0]["detail"]["certificate_id"] ==
				"work-physical-runtime-root" &&
			json["validation_results"][0]["check_kind"] ==
				"certificate_physical_work",
		"JSON report exposes the certificate property and check kind");
}

void TestBroadcastRouteCoverage(TestState &state)
{
	LoadedRelayManifest manifest;
	manifest.schemaVersion = 5;
	manifest.binding.contract = "Contract";
	manifest.binding.moduleId = "module-a";

	ManifestRelaySite site;
	site.ordinal = 3;
	site.id = "broadcast-site";
	site.sourceFunctionId = "root";
	site.handlerId = "broadcast-handler";
	site.expectedOpcode = 12;
	site.handlerResolved = true;
	site.relayKind = RelayKind::AllShards;
	site.targetScope = ScopeKind::Shard;
	manifest.sitesByOrdinal.emplace(site.ordinal, site);
	manifest.ordinalBySiteId.emplace(site.id, site.ordinal);

	ManifestFunctionSummary summary;
	summary.sourceFunctionId = "root";
	summary.exactDirectRelayCount = 1;
	summary.directRelayCountUpperBound = 1;
	manifest.functionsById.emplace("root", std::move(summary));

	RelayExecutionValidationInput input;
	input.execution.traceTxId = 1;
	input.execution.rootTraceTxId = 1;
	input.execution.moduleId = "module-a";
	input.execution.sourceFunctionId = "root";
	input.execution.ownerShard = 0;
	input.activeShardCount = 4;

	RelayEmitTraceEvent emission;
	emission.childTraceTxId = 2;
	emission.rootTraceTxId = 1;
	emission.parentTraceTxId = 1;
	emission.relaySiteOrdinal = 3;
	emission.relaySiteId = "broadcast-site";
	emission.sourceModuleId = "module-a";
	emission.sourceFunctionId = "root";
	emission.actualOpcode = 12;
	emission.actualTargetScope = ScopeKind::Shard;
	emission.relayKind = RelayKind::AllShards;
	input.emissions.push_back(emission);

	for (uint32_t shard = 0; shard < input.activeShardCount; ++shard)
	{
		RelayRouteTraceEvent route;
		route.physicalTraceTxId = 10 + shard;
		route.rootTraceTxId = 1;
		route.parentTraceTxId = 1;
		route.relaySiteOrdinal = 3;
		route.relaySiteId = "broadcast-site";
		route.sourceModuleId = "module-a";
		route.targetShard = shard;
		route.routeKind = RouteKind::AllShardsBroadcast;
		input.routes.push_back(route);
	}

	RelayTraceValidator validator;
	auto covered = validator.ValidateExecution(manifest, input);
	state.Expect(
		HasStatus(
			covered,
			ValidationCheckKind::Fanout,
			ValidationStatus::Passed) &&
			HasStatus(
				covered,
				ValidationCheckKind::Routing,
				ValidationStatus::Passed),
		"broadcast routes uniquely cover every active shard");

	input.routes.back().targetShard = 2;
	auto duplicated = validator.ValidateExecution(manifest, input);
	state.Expect(
		HasStatus(
			duplicated,
			ValidationCheckKind::Routing,
			ValidationStatus::Mismatch),
		"duplicate broadcast target shard fails coverage validation");

	input.routes.clear();
	auto missing = validator.ValidateExecution(manifest, input);
	state.Expect(
		HasStatus(
			missing,
			ValidationCheckKind::Fanout,
			ValidationStatus::Mismatch) &&
			HasStatus(
				missing,
				ValidationCheckKind::Routing,
				ValidationStatus::Mismatch),
		"missing physical routes cannot vacuously pass routing");
}

Json BuildUnboundManifest(
	const std::string &moduleId,
	bool completeBinding = true)
{
	Json root;
	root["schema_version"] = uint32_t(5);
	root["artifact_binding"] = Json{
		{"dapp", "testdapp"},
		{"contract", "Contract"},
		{"transpiler_version", "test-version"},
		{"intermediate_hash", "intermediate"},
		{"module_id", moduleId},
		{"module_hash_kind", "preda_module_id"},
		{"module_hash", moduleId},
		{"manifest_hash_algorithm", "sha256"},
		{"binding_complete", completeBinding},
	};
	root["relay_sites"] = Json::array({
		Json{
			{"ordinal", uint32_t(0)},
			{"id", "site-0"},
			{"source_function_id", "root"},
			{"handler_id", "handler-0"},
			{"relay_kind", "custom_scope"},
			{"target_scope", "address"},
			{"location", Json{{"line", 3}, {"column", 4}}},
		},
	});
	root["handlers"] = Json::array({
		Json{
			{"id", "handler-0"},
			{"opcode", uint32_t(42)},
			{"resolved", true},
		},
	});
	root["functions"] = Json::array({
		Json{
			{"source_function_id", "root"},
			{"exported_opcode", int64_t(7)},
			{"summary",
			 Json{
				 {"relay_count",
				  Json{{"kind", "constant"}, {"value", uint64_t(1)}}},
				 {"relay_count_upper_bound",
				  Json{{"kind", "constant"}, {"value", uint64_t(1)}}},
				 {"max_depth",
				  Json{{"kind", "constant"}, {"value", uint64_t(1)}}},
				 {"relay_site_set", Json::array({"site-0"})},
				 {"fanout", Json::array({"single_target"})},
				 {"has_opaque", false},
			 }},
		},
	});
	root["refinement"] =
		Json{{"proof_obligations", Json::array()}};
	return root;
}

std::string BindManifest(Json &root)
{
	const std::string unbound = root.dump(2);
	std::string error;
	const std::string hash =
		RelayManifestLoader::ComputeManifestSelfHash(unbound, &error);
	root["artifact_binding"]["manifest_hash"] = hash;
	return hash;
}

void WriteText(const std::filesystem::path &path, const std::string &text)
{
	std::filesystem::create_directories(path.parent_path());
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	output << text;
}

ExpectedArtifactBinding ExpectedBinding(const std::string &moduleId)
{
	ExpectedArtifactBinding expected;
	expected.identity.dapp = "testdapp";
	expected.identity.contract = "Contract";
	expected.identity.transpilerVersion = "test-version";
	expected.identity.intermediateHash = "intermediate";
	expected.identity.moduleId = moduleId;
	expected.identity.moduleHashKind = "preda_module_id";
	expected.identity.moduleHash = moduleId;
	return expected;
}

void TestManifestBindingAndCache(TestState &state)
{
	const auto unique =
		std::chrono::steady_clock::now().time_since_epoch().count();
	const std::filesystem::path root =
		std::filesystem::temp_directory_path() /
		("rpreda-relay-trace-tests-" + std::to_string(unique));
	const std::string moduleId = "module-identity";
	const std::filesystem::path path =
		root / "by_module" /
		(moduleId + ".relay_protocol.json");

	Json manifest = BuildUnboundManifest(moduleId);
	const std::string trustedHash = BindManifest(manifest);
	WriteText(path, manifest.dump(2));

	RelayManifestLoader loader(root.string());
	ExpectedArtifactBinding expected = ExpectedBinding(moduleId);
	expected.trustedManifestHash = trustedHash;
	auto loaded = loader.Load(expected);
	state.Expect(
		loaded->status == ManifestLoadStatus::Loaded &&
			loaded->manifest != nullptr,
		"correct schema-v5 manifest/module binding loads");
	if (loaded->manifest)
	{
		auto function = loaded->manifest->functionIdByOpcode.find(7);
		state.Expect(
			function != loaded->manifest->functionIdByOpcode.end() &&
				function->second == "root",
			"functions[].exported_opcode resolves runtime function identity");
	}

	// Cache remains stable until an explicit module reload/invalidation.
	manifest["handlers"][0]["opcode"] = uint32_t(99);
	WriteText(path, manifest.dump(2));
	auto cached = loader.Load(expected);
	state.Expect(
		cached->status == ManifestLoadStatus::Loaded,
		"loaded module manifest is cached, not reparsed per relay");
	loader.Invalidate(moduleId);
	auto tampered = loader.Load(expected);
	state.Expect(
		tampered->status == ManifestLoadStatus::ManifestHashMismatch,
		"tampered manifest is rejected after module cache invalidation");

	const std::string staleModule = "stale-module";
	const std::filesystem::path stalePath =
		root / "by_module" /
		(staleModule + ".relay_protocol.json");
	Json stale = BuildUnboundManifest(moduleId);
	BindManifest(stale);
	WriteText(stalePath, stale.dump(2));
	auto staleResult = loader.Load(ExpectedBinding(staleModule));
	state.Expect(
		staleResult->status ==
			ManifestLoadStatus::ManifestBindingMismatch,
		"same logical contract with a different module identity is rejected");
	state.Expect(
		staleResult->expectedBinding.moduleId == staleModule &&
			staleResult->observedBinding.moduleId == moduleId &&
			staleResult->sourcePath == stalePath.string(),
		"binding failure retains expected, observed, and source identities");
	const auto staleValidation =
		RelayTraceValidator().ValidateManifestLoad(*staleResult);
	state.Expect(
		staleValidation.size() == 1 &&
			staleValidation.front().detail.moduleIdentity == staleModule &&
			staleValidation.front().detail.manifestIdentity.moduleId ==
				moduleId &&
			!staleValidation.front().detail.expected.empty() &&
			!staleValidation.front().detail.actual.empty(),
		"binding mismatch report contains actionable identity diagnostics");

	const std::string incompleteModule = "incomplete-module";
	const std::filesystem::path incompletePath =
		root / "by_module" /
		(incompleteModule + ".relay_protocol.json");
	Json incomplete = BuildUnboundManifest(incompleteModule, false);
	BindManifest(incomplete);
	WriteText(incompletePath, incomplete.dump(2));
	auto incompleteResult =
		loader.Load(ExpectedBinding(incompleteModule));
	state.Expect(
		incompleteResult->status ==
			ManifestLoadStatus::ManifestBindingMissing,
		"binding_complete=false is never trusted");

	const std::string tamperedHashModule = "tampered-hash-module";
	const std::filesystem::path tamperedHashPath =
		root / "by_module" /
		(tamperedHashModule + ".relay_protocol.json");
	Json tamperedHash = BuildUnboundManifest(tamperedHashModule);
	BindManifest(tamperedHash);
	tamperedHash["artifact_binding"]["manifest_hash"] =
		std::string(64, '0');
	WriteText(tamperedHashPath, tamperedHash.dump(2));
	auto tamperedHashResult =
		loader.Load(ExpectedBinding(tamperedHashModule));
	state.Expect(
		tamperedHashResult->status ==
			ManifestLoadStatus::ManifestHashMismatch,
		"an independently tampered manifest_hash is rejected");

	const std::string missingBindingModule = "missing-binding-module";
	const std::filesystem::path missingBindingPath =
		root / "by_module" /
		(missingBindingModule + ".relay_protocol.json");
	Json missingBinding = BuildUnboundManifest(missingBindingModule);
	missingBinding.erase("artifact_binding");
	WriteText(missingBindingPath, missingBinding.dump(2));
	auto missingBindingResult =
		loader.Load(ExpectedBinding(missingBindingModule));
	state.Expect(
		missingBindingResult->status ==
			ManifestLoadStatus::ManifestBindingMissing,
		"a missing artifact_binding object is rejected");

	const std::string missingFieldModule = "missing-binding-field-module";
	const std::filesystem::path missingFieldPath =
		root / "by_module" /
		(missingFieldModule + ".relay_protocol.json");
	Json missingField = BuildUnboundManifest(missingFieldModule);
	BindManifest(missingField);
	missingField["artifact_binding"].erase("module_hash_kind");
	WriteText(missingFieldPath, missingField.dump(2));
	auto missingFieldResult =
		loader.Load(ExpectedBinding(missingFieldModule));
	state.Expect(
		missingFieldResult->status ==
			ManifestLoadStatus::ManifestBindingMissing,
		"a missing required artifact binding field is rejected");

	const std::string missingModule = "does-not-exist";
	auto missing = loader.Load(ExpectedBinding(missingModule));
	state.Expect(
		missing->status == ManifestLoadStatus::ManifestNotFound,
		"unrelated sidecars cannot turn a missing module into a mismatch");

	const std::string malformedSchemaModule = "malformed-schema-module";
	const std::filesystem::path malformedSchemaPath =
		root / "by_module" /
		(malformedSchemaModule + ".relay_protocol.json");
	Json malformedSchema = BuildUnboundManifest(malformedSchemaModule);
	malformedSchema["schema_version"] = "five";
	BindManifest(malformedSchema);
	WriteText(malformedSchemaPath, malformedSchema.dump(2));
	std::shared_ptr<const RelayManifestLoadResult> malformedSchemaResult;
	bool schemaExceptionEscaped = false;
	try
	{
		malformedSchemaResult =
			loader.Load(ExpectedBinding(malformedSchemaModule));
	}
	catch (...)
	{
		schemaExceptionEscaped = true;
	}
	state.Expect(
		!schemaExceptionEscaped &&
			malformedSchemaResult &&
			malformedSchemaResult->status ==
				ManifestLoadStatus::ManifestParseError,
		"a wrong top-level schema type returns ManifestParseError");

	const std::string malformedNestedModule = "malformed-nested-module";
	const std::filesystem::path malformedNestedPath =
		root / "by_module" /
		(malformedNestedModule + ".relay_protocol.json");
	Json malformedNested = BuildUnboundManifest(malformedNestedModule);
	malformedNested["functions"][0]["summary"]["relay_site_set"] =
		Json::array({uint32_t(7)});
	BindManifest(malformedNested);
	WriteText(malformedNestedPath, malformedNested.dump(2));
	std::shared_ptr<const RelayManifestLoadResult> malformedNestedResult;
	bool nestedExceptionEscaped = false;
	try
	{
		malformedNestedResult =
			loader.Load(ExpectedBinding(malformedNestedModule));
	}
	catch (...)
	{
		nestedExceptionEscaped = true;
	}
	state.Expect(
		!nestedExceptionEscaped &&
			malformedNestedResult &&
			malformedNestedResult->status ==
				ManifestLoadStatus::ManifestParseError,
		"a wrong nested schema type returns ManifestParseError");

	std::error_code ignored;
	std::filesystem::remove_all(root, ignored);
}

void TestReportAndStrictMismatch(TestState &state)
{
	RelayTraceCollector collector(TraceMode::Strict);
	RelayValidationResult mismatch;
	mismatch.status = ValidationStatus::Mismatch;
	mismatch.checkKind = ValidationCheckKind::DirectCount;
	mismatch.reason = "injected mismatch";
	mismatch.detail.checkKind = mismatch.checkKind;
	mismatch.detail.relaySiteId = "site-0";
	mismatch.detail.diagnosticReason = mismatch.reason;
	collector.AddValidationResult(mismatch);
	RelayValidationResult passed = mismatch;
	passed.status = ValidationStatus::Passed;
	passed.reason = "injected pass";
	passed.detail.diagnosticReason = passed.reason;
	collector.AddValidationResult(passed);
	state.Expect(
		collector.StrictFailureLatched(),
		"strict mismatch latches deterministic failure without worker throw");

	RelayTraceReport report;
	const RelayTraceSnapshot snapshot = collector.Snapshot();
	const RelayTraceSerializedReport json = report.BuildJson(snapshot, false);
	const RelayTraceSerializedReport jsonl =
		report.BuildJsonLines(snapshot);
	Json parsed = Json::parse(json.contents);
	state.Expect(
		parsed["counters"]["checks_mismatched"].get<uint64_t>() == 1,
		"JSON report contains aggregate mismatch counters");
	state.Expect(
		parsed["counters"]["checks_by_kind"]["direct_count"].
				get<uint64_t>() == 2 &&
			parsed["counters"]["checks_by_kind_and_status"]
				["direct_count"]["Mismatch"].get<uint64_t>() == 1 &&
			parsed["counters"]["checks_by_kind_and_status"]
				["direct_count"]["Passed"].get<uint64_t>() == 1,
		"JSON report preserves totals and adds kind/status counters");
	state.Expect(
		jsonl.contents.find("\"record_type\":\"validation\"") !=
			std::string::npos &&
			jsonl.contents.find("injected mismatch") !=
				std::string::npos,
		"JSONL report never samples away mismatch events");

	RelayTraceCollector strictObserver(TraceMode::Strict);
	strictObserver.RecordObserverFailureNoexcept("unit-test hook");
	const RelayTraceSnapshot strictObserverSnapshot =
		strictObserver.Snapshot();
	state.Expect(
		strictObserver.StrictFailureLatched() &&
			strictObserverSnapshot.counters.instrumentationFailures == 1 &&
			!strictObserverSnapshot.validationResults.empty() &&
			!strictObserverSnapshot.validationResults.front().reason.empty(),
		"observer exceptions are diagnostic and latch only at a strict safe point");

	RelayTraceCollector observeObserver(TraceMode::Observe);
	observeObserver.RecordObserverFailureNoexcept("unit-test hook");
	state.Expect(
		!observeObserver.StrictFailureLatched() &&
			observeObserver.Snapshot().
				counters.instrumentationFailures == 1,
		"observe-mode observer exception remains non-semantic");
}

void TestMultithreadedIsolation(TestState &state)
{
	OpaqueTransactionPool transactions;
	RelayTraceCollector collector(TraceMode::Observe);
	constexpr size_t WorkerCount = 8;
	std::atomic<bool> start{false};
	std::vector<std::thread> workers;
	workers.reserve(WorkerCount);

	for (size_t worker = 0; worker < WorkerCount; ++worker)
	{
		workers.emplace_back([&, worker]() {
			while (!start.load(std::memory_order_acquire))
				std::this_thread::yield();
			const SimuTxn *root = transactions.At(worker * 2);
			const SimuTxn *child = transactions.At(worker * 2 + 1);
			collector.BeginExecution(
				SourceBegin(
					root,
					"module-" + std::to_string(worker),
					"root"));
			const uint64_t generation = collector.PushMarker(
				root,
				"module-" + std::to_string(worker),
				static_cast<RelaySiteOrdinal>(worker));
			collector.RegisterRelayCreation(
				CreateRelay(root, child, static_cast<uint32_t>(100 + worker)));
			const uint8_t target = static_cast<uint8_t>(worker);
			RelayFinalizeInput finalize =
				FinalizeRelay(
					child,
					RelayKind::CustomScope,
					ScopeKind::Uint32,
					"site-" + std::to_string(worker),
					target);
			finalize.targetData = &target;
			finalize.targetSize = sizeof(target);
			collector.SetPendingRelayResolvedIdentity(
				child,
				"site-" + std::to_string(worker),
				"root");
			collector.FinalizeRelayEmission(finalize);
			collector.PopMarker(
				root,
				generation,
				"module-" + std::to_string(worker),
				static_cast<RelaySiteOrdinal>(worker),
				true);
			auto relayBegin = SourceBegin(
				child,
				"target-module",
				"handler",
				static_cast<uint32_t>(100 + worker));
			relayBegin.isRelay = true;
			collector.BeginExecution(relayBegin);
			collector.EndExecution(child, true, true);
			collector.EndExecution(root, true, true);
		});
	}
	start.store(true, std::memory_order_release);
	for (std::thread &worker : workers)
		worker.join();

	const RelayTraceSnapshot snapshot = collector.Snapshot();
	state.Expect(
		snapshot.emissions.size() == WorkerCount,
		"multi-worker relay events merge without data races");
	for (const RelayEmitTraceEvent &emission : snapshot.emissions)
	{
		state.Expect(
			emission.occurrenceIndex == 0,
			"occurrence counters do not cross parent transactions");
		state.Expect(
			emission.relaySiteOrdinal != InvalidRelaySiteOrdinal,
			"worker marker identity does not leak across executions");
	}
}

} // namespace

int RunRelayRuntimeTraceUnitTests()
{
	TestState state;
	TestNamedRelayTreeAndPointerReuse(state);
	TestTraceSideTableDoesNotMutateTransactions(state);
	TestOccurrencesAndCrossModuleIdentity(state);
	TestMarkerFailuresRemainTraceable(state);
	TestUnconsumedMarkerRestoreIsDeterministic(state);
	TestNestedRelayTree(state);
	TestBroadcastLogicalPhysicalSplit(state);
	TestValidatorAndNonAlias(state);
	TestParallelCertificateValidation(state);
	TestParallelCertificateWorkBoundsAndStrictLatch(state);
	TestBroadcastRouteCoverage(state);
	TestManifestBindingAndCache(state);
	TestReportAndStrictMismatch(state);
	TestMultithreadedIsolation(state);
#ifdef RPREDA_ENABLE_TRACE_FAULT_INJECTION
	state.failures += RunRelayTraceFaultInjectorUnitTests();
#endif
	if (state.failures == 0)
		std::cout << "Relay runtime trace unit tests passed\n";
	return state.failures;
}

} // namespace tests
} // namespace relay_trace
} // namespace oxd

#ifdef RPREDA_RUNTIME_TRACE_STANDALONE_TEST_MAIN
int main()
{
	return oxd::relay_trace::tests::RunRelayRuntimeTraceUnitTests() == 0
		? 0
		: 1;
}
#endif
