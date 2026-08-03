#include "RelayTraceFaultInjector.h"
#include "RelayTraceValidator.h"

#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace oxd {
namespace relay_trace {
namespace tests {
namespace {

struct TestState
{
	int failures = 0;

	void Expect(bool condition, const std::string &description)
	{
		if (condition)
			return;
		++failures;
		std::cerr << "FAILED: " << description << '\n';
	}
};

RelayEmitTraceEvent Event(
	const std::string &function,
	const std::string &site,
	RelaySiteOrdinal ordinal,
	uint32_t occurrence,
	uint64_t sequence,
	ScopeKind scope)
{
	RelayEmitTraceEvent event;
	event.childTraceTxId = 10 + occurrence;
	event.rootTraceTxId = 1;
	event.parentTraceTxId = 1;
	event.relaySiteOrdinal = ordinal;
	event.relaySiteId = site;
	event.occurrenceIndex = occurrence;
	event.emissionSequence = sequence;
	event.depth = 1;
	event.sourceModuleId = "module-a";
	event.sourceFunctionId = function;
	event.actualTargetScope = scope;
	event.actualOpcode = 7;
	event.relayKind = RelayKind::CustomScope;
	const uint32_t target = 42;
	event.actualTarget.Assign(
		reinterpret_cast<const uint8_t *>(&target),
		sizeof(target));
	return event;
}

RelayRouteTraceEvent Route(const RelayEmitTraceEvent &event)
{
	RelayRouteTraceEvent route;
	route.physicalTraceTxId = event.childTraceTxId;
	route.rootTraceTxId = event.rootTraceTxId;
	route.parentTraceTxId = event.parentTraceTxId;
	route.relaySiteOrdinal = event.relaySiteOrdinal;
	route.relaySiteId = event.relaySiteId;
	route.sourceModuleId = event.sourceModuleId;
	route.occurrenceIndex = event.occurrenceIndex;
	route.targetShard = 2;
	route.activeShardCount = 4;
	route.routeKind = RouteKind::CrossShard;
	return route;
}

RuntimeTraceFaultSpec ScopeSpec()
{
	RuntimeTraceFaultSpec spec;
	spec.mutationId = "runtime-scope-1";
	spec.kind = RuntimeTraceFaultKind::RuntimeTargetScopeCorruption;
	spec.seed = 88;
	spec.selector.sourceModuleId = "module-a";
	spec.selector.sourceFunctionId = "Contract::root(uint32)";
	spec.selector.relaySiteId = "relay_site_1";
	spec.selector.occurrenceIndex = 0;
	spec.replacementScope = ScopeKind::Uint64;
	return spec;
}

RuntimeTraceFaultSpec DuplicateSpec()
{
	RuntimeTraceFaultSpec spec = ScopeSpec();
	spec.mutationId = "runtime-duplicate-1";
	spec.kind = RuntimeTraceFaultKind::RuntimeRelayDuplicate;
	spec.replacementScope.reset();
	return spec;
}

LoadedRelayManifest Manifest()
{
	LoadedRelayManifest manifest;
	manifest.schemaVersion = 5;
	manifest.binding.contract = "Contract";
	manifest.binding.moduleId = "module-a";

	ManifestRelaySite site;
	site.ordinal = 1;
	site.id = "relay_site_1";
	site.sourceFunctionId = "Contract::root(uint32)";
	site.handlerId = "handler-1";
	site.expectedOpcode = 7;
	site.handlerResolved = true;
	site.relayKind = RelayKind::CustomScope;
	site.targetScope = ScopeKind::Uint32;
	manifest.sitesByOrdinal.emplace(site.ordinal, site);
	manifest.ordinalBySiteId.emplace(site.id, site.ordinal);

	ManifestFunctionSummary summary;
	summary.sourceFunctionId = site.sourceFunctionId;
	summary.exactDirectRelayCount = 1;
	summary.directRelayCountUpperBound = 1;
	manifest.functionsById.emplace(summary.sourceFunctionId, summary);
	return manifest;
}

RelayExecutionValidationInput ValidationInput(
	const RelayExecutionTraceSlice &slice)
{
	RelayExecutionValidationInput input;
	input.execution.traceTxId = 1;
	input.execution.rootTraceTxId = 1;
	input.execution.moduleId = "module-a";
	input.execution.sourceFunctionId = "Contract::root(uint32)";
	input.execution.ownerShard = 0;
	input.execution.opcode = 3;
	input.emissions = slice.emissions;
	input.routes = slice.routes;
	input.activeShardCount = 4;
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

void TestStrictJsonParsing(TestState &state)
{
	const std::string valid = R"JSON({
  "schema_version": 1,
  "mutation_id": "runtime-scope-1",
  "kind": "RuntimeTargetScopeCorruption",
  "seed": 88,
  "selector": {
    "source_module_id": "module-a",
    "source_function_id": "Contract::root(uint32)",
    "relay_site_id": "relay_site_1",
    "occurrence_index": 0,
    "root_trace_tx_id": 1,
    "parent_trace_tx_id": 1
  },
  "replacement_scope": "uint64"
})JSON";
	std::string error;
	auto parsed = RelayTraceFaultInjector::ParseSpecJson(valid, error);
	state.Expect(
		parsed && error.empty() && parsed->seed == 88 &&
			parsed->replacementScope == ScopeKind::Uint64 &&
			parsed->selector.rootTraceTxId == 1,
		"valid strict JSON fault spec parses");

	const std::string missingOccurrence = R"JSON({
  "schema_version": 1,
  "mutation_id": "bad",
  "kind": "RuntimeRelayDuplicate",
  "seed": 88,
  "selector": {
    "source_function_id": "Contract::root(uint32)",
    "relay_site_id": "relay_site_1"
  }
})JSON";
	parsed = RelayTraceFaultInjector::ParseSpecJson(
		missingOccurrence,
		error);
	state.Expect(
		!parsed && error.find("occurrence_index") != std::string::npos,
		"missing occurrence selector is rejected");

	const std::string unknownKind = R"JSON({
  "schema_version": 1,
  "mutation_id": "bad",
  "kind": "RuntimeMagicFault",
  "seed": 88,
  "selector": {
    "source_function_id": "Contract::root(uint32)",
    "relay_site_id": "relay_site_1",
    "occurrence_index": 0
  }
})JSON";
	parsed = RelayTraceFaultInjector::ParseSpecJson(unknownKind, error);
	state.Expect(
		!parsed && error.find("unknown runtime fault kind") !=
			std::string::npos,
		"unknown fault kind is rejected");
}

void TestSelectorMissDoesNotConsume(TestState &state)
{
	RuntimeTraceFaultSpec spec = ScopeSpec();
	RelayTraceFaultInjector injector(spec);
	RelayExecutionTraceSlice unrelated;
	unrelated.emissions.push_back(Event(
		"Contract::other(uint32)",
		"relay_site_9",
		9,
		0,
		0,
		ScopeKind::Uint32));
	auto miss = injector.Apply(unrelated);
	state.Expect(
		miss.status == RuntimeTraceFaultApplicationStatus::NotApplied &&
			!injector.Applied() && !injector.Terminal(),
		"selector miss does not consume one-shot injector");

	RelayExecutionTraceSlice matching;
	matching.emissions.push_back(Event(
		spec.selector.sourceFunctionId,
		spec.selector.relaySiteId,
		1,
		0,
		0,
		ScopeKind::Uint32));
	auto applied = injector.Apply(matching);
	state.Expect(
		applied.status == RuntimeTraceFaultApplicationStatus::Applied &&
			injector.Applied() && injector.Terminal(),
		"later matching slice applies fault exactly once");
}

void TestScopeCorruptionIsValidationOnly(TestState &state)
{
	RuntimeTraceFaultSpec spec = ScopeSpec();
	RelayTraceFaultInjector injector(spec);
	RelayExecutionTraceSlice collectorOwned;
	collectorOwned.emissions.push_back(Event(
		spec.selector.sourceFunctionId,
		spec.selector.relaySiteId,
		1,
		0,
		4,
		ScopeKind::Uint32));
	collectorOwned.routes.push_back(Route(collectorOwned.emissions.front()));
	RelayExecutionTraceSlice validationCopy = collectorOwned;

	const auto baseline = RelayTraceValidator().ValidateExecution(
		Manifest(),
		ValidationInput(validationCopy));
	state.Expect(
		HasStatus(
			baseline,
			ValidationCheckKind::TargetScopeKind,
			ValidationStatus::Passed),
		"unmutated target scope passes validation");

	const auto applied = injector.Apply(validationCopy);
	state.Expect(
		applied.status == RuntimeTraceFaultApplicationStatus::Applied &&
			applied.originalScope == ScopeKind::Uint32 &&
			applied.mutatedScope == ScopeKind::Uint64,
		"target scope fault reports exact before/after values");
	state.Expect(
		collectorOwned.emissions.front().actualTargetScope ==
			ScopeKind::Uint32 &&
			validationCopy.emissions.front().actualTargetScope ==
			ScopeKind::Uint64 &&
			collectorOwned.routes.size() == validationCopy.routes.size() &&
			collectorOwned.routes.front().targetShard ==
				validationCopy.routes.front().targetShard &&
			collectorOwned.routes.front().occurrenceIndex ==
				validationCopy.routes.front().occurrenceIndex,
		"only owning validation copy changes; routes remain untouched");

	const auto mutated = RelayTraceValidator().ValidateExecution(
		Manifest(),
		ValidationInput(validationCopy));
	state.Expect(
		HasStatus(
			mutated,
			ValidationCheckKind::TargetScopeKind,
			ValidationStatus::Mismatch),
		"existing validator independently detects scope corruption");

	const auto second = injector.Apply(validationCopy);
	state.Expect(
		second.status ==
			RuntimeTraceFaultApplicationStatus::AlreadyApplied,
		"scope fault cannot apply twice");
}

void TestRelayDuplicatePreservesRoutingAndBreaksCount(TestState &state)
{
	RuntimeTraceFaultSpec spec = DuplicateSpec();
	RelayTraceFaultInjector injector(spec);
	RelayExecutionTraceSlice collectorOwned;
	collectorOwned.emissions.push_back(Event(
		spec.selector.sourceFunctionId,
		spec.selector.relaySiteId,
		1,
		0,
		8,
		ScopeKind::Uint32));
	collectorOwned.routes.push_back(Route(collectorOwned.emissions.front()));
	RelayExecutionTraceSlice validationCopy = collectorOwned;

	const auto applied = injector.Apply(validationCopy);
	state.Expect(
		applied.status == RuntimeTraceFaultApplicationStatus::Applied &&
			applied.duplicateOccurrenceIndex == 1 &&
			applied.clonedRouteCount == 1 &&
			validationCopy.emissions.size() == 2 &&
			validationCopy.routes.size() == 2,
		"duplicate adds one logical emission and its matching route");
	state.Expect(
		collectorOwned.emissions.size() == 1 &&
			collectorOwned.routes.size() == 1,
		"collector-owned source slice remains unchanged");
	state.Expect(
		validationCopy.emissions.back().occurrenceIndex == 1 &&
			validationCopy.emissions.back().emissionSequence == 9 &&
			validationCopy.routes.back().occurrenceIndex == 1 &&
			validationCopy.routes.back().physicalTraceTxId ==
				NoTraceTransaction,
		"duplicate identities are deterministic and explicitly synthetic");

	const auto results = RelayTraceValidator().ValidateExecution(
		Manifest(),
		ValidationInput(validationCopy));
	state.Expect(
		HasStatus(
			results,
			ValidationCheckKind::DirectCount,
			ValidationStatus::Mismatch) &&
			HasStatus(
				results,
				ValidationCheckKind::CountUpperBound,
				ValidationStatus::Mismatch),
		"existing validator independently detects duplicated relay work");
	state.Expect(
		!HasStatus(
			results,
			ValidationCheckKind::Fanout,
			ValidationStatus::Mismatch) &&
			!HasStatus(
				results,
				ValidationCheckKind::Routing,
				ValidationStatus::Mismatch),
		"duplicate fault does not manufacture an unrelated routing failure");

	const size_t emissionCount = validationCopy.emissions.size();
	const auto second = injector.Apply(validationCopy);
	state.Expect(
		second.status ==
			RuntimeTraceFaultApplicationStatus::AlreadyApplied &&
			validationCopy.emissions.size() == emissionCount,
		"duplicate fault cannot apply twice");
}

void TestAmbiguousAndNoOpSpecsAreInvalid(TestState &state)
{
	RuntimeTraceFaultSpec duplicate = DuplicateSpec();
	RelayTraceFaultInjector ambiguousInjector(duplicate);
	RelayExecutionTraceSlice ambiguous;
	ambiguous.emissions.push_back(Event(
		duplicate.selector.sourceFunctionId,
		duplicate.selector.relaySiteId,
		1,
		0,
		1,
		ScopeKind::Uint32));
	ambiguous.emissions.push_back(ambiguous.emissions.front());
	const auto ambiguity = ambiguousInjector.Apply(ambiguous);
	state.Expect(
		ambiguity.status == RuntimeTraceFaultApplicationStatus::Invalid &&
			!ambiguousInjector.Applied() && ambiguousInjector.Terminal() &&
			ambiguous.emissions.size() == 2,
		"ambiguous selector fails closed without mutation");

	RuntimeTraceFaultSpec noOp = ScopeSpec();
	noOp.replacementScope = ScopeKind::Uint32;
	RelayTraceFaultInjector noOpInjector(noOp);
	RelayExecutionTraceSlice slice;
	slice.emissions.push_back(Event(
		noOp.selector.sourceFunctionId,
		noOp.selector.relaySiteId,
		1,
		0,
		1,
		ScopeKind::Uint32));
	const auto invalid = noOpInjector.Apply(slice);
	state.Expect(
		invalid.status == RuntimeTraceFaultApplicationStatus::Invalid &&
			slice.emissions.front().actualTargetScope == ScopeKind::Uint32,
		"no-op replacement is invalid and leaves slice unchanged");
}

void TestConcurrentOneShot(TestState &state)
{
	RuntimeTraceFaultSpec spec = ScopeSpec();
	RelayTraceFaultInjector injector(spec);
	auto makeSlice = [&spec]()
	{
		RelayExecutionTraceSlice slice;
		slice.emissions.push_back(Event(
			spec.selector.sourceFunctionId,
			spec.selector.relaySiteId,
			1,
			0,
			1,
			ScopeKind::Uint32));
		return slice;
	};
	RelayExecutionTraceSlice left = makeSlice();
	RelayExecutionTraceSlice right = makeSlice();
	RuntimeTraceFaultApplication leftResult;
	RuntimeTraceFaultApplication rightResult;
	std::thread first([&]() { leftResult = injector.Apply(left); });
	std::thread second([&]() { rightResult = injector.Apply(right); });
	first.join();
	second.join();
	const size_t appliedCount =
		(leftResult.status == RuntimeTraceFaultApplicationStatus::Applied ? 1 : 0) +
		(rightResult.status == RuntimeTraceFaultApplicationStatus::Applied ? 1 : 0);
	const size_t alreadyCount =
		(leftResult.status == RuntimeTraceFaultApplicationStatus::AlreadyApplied
			? 1
			: 0) +
		(rightResult.status == RuntimeTraceFaultApplicationStatus::AlreadyApplied
			? 1
			: 0);
	state.Expect(
		appliedCount == 1 && alreadyCount == 1 && injector.Applied(),
		"concurrent matching slices still receive exactly one application");
}

} // namespace

int RunRelayTraceFaultInjectorUnitTests()
{
	TestState state;
	TestStrictJsonParsing(state);
	TestSelectorMissDoesNotConsume(state);
	TestScopeCorruptionIsValidationOnly(state);
	TestRelayDuplicatePreservesRoutingAndBreaksCount(state);
	TestAmbiguousAndNoOpSpecsAreInvalid(state);
	TestConcurrentOneShot(state);
	if (state.failures == 0)
		std::cout << "Relay trace fault injector unit tests passed\n";
	return state.failures;
}

} // namespace tests
} // namespace relay_trace
} // namespace oxd

#ifdef RPREDA_TRACE_FAULT_INJECTOR_STANDALONE_TEST_MAIN
int main()
{
	return oxd::relay_trace::tests::
		RunRelayTraceFaultInjectorUnitTests() == 0 ? 0 : 1;
}
#endif
