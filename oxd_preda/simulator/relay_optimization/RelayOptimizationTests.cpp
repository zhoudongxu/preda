#include "RelayOptimizationAudit.h"
#include "RelayOptimizationReport.h"
#include "RelayOptimizationTypes.h"
#include "RelayReservePlanner.h"

#include "../../3rdParty/nlohmann/json.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace oxd;
using namespace oxd::relay_optimization;

void Require(bool condition, const char *message)
{
	if(!condition)
	{
		std::cerr << "relay optimization test failed: " <<
			message << '\n';
		std::exit(1);
	}
}

relay_plan::FunctionRelayPlan EligibleExactPlan(uint64_t count)
{
	relay_plan::FunctionRelayPlan plan;
	plan.bindingTrusted = true;
	plan.functionOpcodeTrusted = true;
	plan.optimizationEligible = true;
	plan.directCount.kind =
		relay_plan::CountPlanKind::ExactConstant;
	plan.directCount.value = count;
	return plan;
}

void RequireAuditFailure(
	const RelayOptimizationAudit &audit,
	AuditCheckKind kind,
	uint64_t expected,
	uint64_t actual,
	const char *message)
{
	Require(audit.FailureLatched(), message);
	const auto failure = audit.FirstFailure();
	Require(failure.has_value(), message);
	Require(failure->kind == kind, message);
	Require(failure->expected == expected, message);
	Require(failure->actual == actual, message);
}

void TestConfig()
{
	OptimizationMode mode{};
	OptimizationAblation ablation{};
	AuditSampleRate rate;
	std::string error;
	Require(
		ParseOptimizationMode("OPTIMIZE_AUDIT", mode, error) &&
			mode == OptimizationMode::OptimizeAudit,
		"optimize_audit mode parsing");
	Require(
		ParseOptimizationAblation(
			"verified_reserve_plus_batch",
			ablation,
			error) &&
			ablation ==
				OptimizationAblation::VerifiedReservePlusBatch,
		"ablation parsing");
	Require(
		ParseAuditSampleRate("1/1000", rate, error) &&
			rate.numerator == 1 && rate.denominator == 1000,
		"sample-rate parsing");
	Require(
		!ParseAuditSampleRate("2/1", rate, error),
		"invalid sample-rate rejection");
	uint64_t unsignedValue = 0;
	Require(
		ParseUnsignedDecimal(
			"18446744073709551615",
			unsignedValue,
			error) &&
			unsignedValue == std::numeric_limits<uint64_t>::max(),
		"maximum uint64 parsing");
	Require(
		!ParseUnsignedDecimal(
			"18446744073709551616",
			unsignedValue,
			error),
		"overflowing uint64 rejection");

	OptimizationConfig config;
	config.mode = OptimizationMode::Optimize;
	Require(
		ValidateOptimizationConfig(config, false, error) &&
			config.ablation ==
				OptimizationAblation::VerifiedReservePlusBatch,
		"default optimize ablation");
	config.mode = OptimizationMode::Baseline;
	config.ablation =
		OptimizationAblation::GenericBatchOnly;
	Require(
		!ValidateOptimizationConfig(config, true, error),
		"baseline with optimization ablation rejection");
}

void TestReserve()
{
	auto exact = EligibleExactPlan(0);
	ReserveDecision decision =
		SelectDirectRelayReserve(&exact, 0, 1000000);
	Require(
		decision.kind == ReserveDecisionKind::SkippedZero &&
			decision.HasTrustedCount() && decision.addition == 0,
		"trusted exact-zero reserve skip");

	exact.directCount.value = 1;
	decision = SelectDirectRelayReserve(&exact, 9, 1000000);
	Require(
		decision.kind == ReserveDecisionKind::ExactConstant &&
			decision.addition == 1 &&
			decision.requiredCapacity == 10,
		"exact-one reserve");

	exact.directCount.value = 100;
	decision = SelectDirectRelayReserve(&exact, 7, 1000000);
	Require(
		decision.kind == ReserveDecisionKind::ExactConstant &&
			decision.addition == 100 &&
			decision.requiredCapacity == 107,
		"exact-hundred reserve");

	exact.directCount.value = 1000001;
	decision = SelectDirectRelayReserve(&exact, 0, 1000000);
	Require(
		decision.kind == ReserveDecisionKind::SkippedLimit,
		"limit reserve skip");
	Require(
		decision.HasTrustedCount(),
		"limit skip retains trusted-count provenance");

	auto upper = EligibleExactPlan(1);
	upper.directCount.kind =
		relay_plan::CountPlanKind::Unknown;
	upper.directCountUpperBound.kind =
		relay_plan::CountPlanKind::ProvedConstantUpperBound;
	upper.directCountUpperBound.value = 7;
	decision = SelectDirectRelayReserve(&upper, 3, 1000000);
	Require(
		decision.kind ==
			ReserveDecisionKind::ProvedConstantUpperBound &&
			decision.requiredCapacity == 10,
		"proved upper-bound reserve");
	upper.directCountUpperBound.value = 0;
	decision = SelectDirectRelayReserve(&upper, 3, 1000000);
	Require(
		decision.kind == ReserveDecisionKind::SkippedZero &&
			decision.HasTrustedCount(),
		"trusted upper-bound zero reserve skip");

	auto unknown = EligibleExactPlan(1);
	unknown.directCount.kind = relay_plan::CountPlanKind::Unknown;
	decision = SelectDirectRelayReserve(&unknown, 0, 1000000);
	Require(
		decision.kind == ReserveDecisionKind::SkippedUnknown &&
			!decision.HasTrustedCount(),
		"unknown count fallback");
	unknown.directCount.kind =
		relay_plan::CountPlanKind::SymbolicUnsupportedAtRuntime;
	decision = SelectDirectRelayReserve(&unknown, 0, 1000000);
	Require(
		decision.kind == ReserveDecisionKind::SkippedUnknown,
		"symbolic unsupported count fallback");
	decision = SelectDirectRelayReserve(nullptr, 0, 1000000);
	Require(
		decision.kind == ReserveDecisionKind::SkippedNoPlan,
		"missing plan fallback");

	upper.bindingTrusted = false;
	decision = SelectDirectRelayReserve(&upper, 0, 1000000);
	Require(
		decision.kind == ReserveDecisionKind::SkippedIneligible,
		"untrusted plan skip");
	auto global = EligibleExactPlan(1);
	global.mayGlobal = true;
	decision = SelectDirectRelayReserve(&global, 4, 1000000);
	Require(
		decision.kind == ReserveDecisionKind::ExactConstant &&
			decision.requiredCapacity == 5,
		"global logical emission uses trusted direct count");

	size_t required = 0;
	Require(
		CheckedRequiredCapacity(7, 100, 100, required) &&
			required == 107,
		"reserve limit applies to the per-invocation addition");
	Require(
		!CheckedRequiredCapacity(7, 101, 100, required),
		"reserve addition above limit rejected");
	Require(
		!CheckedRequiredCapacity(
			std::numeric_limits<size_t>::max(),
			1,
			std::numeric_limits<uint64_t>::max(),
			required),
		"required-capacity addition overflow rejected");

	uint64_t physical = 0;
	Require(
		CheckedAggregateBroadcastReserve(1, 4, 100, physical) &&
			physical == 4,
		"one logical broadcast emission reserves one clone per shard");
	Require(
		CheckedAggregateBroadcastReserve(3, 4, 100, physical) &&
			physical == 12,
		"broadcast aggregate reserve");
	Require(
		CheckedAggregateBroadcastReserve(3, 7, 100, physical) &&
			physical == 21,
		"broadcast reserve follows changed active shard count");
	Require(
		!CheckedAggregateBroadcastReserve(3, 7, 20, physical),
		"broadcast aggregate limit rejection");
	Require(
		!CheckedAggregateBroadcastReserve(
			std::numeric_limits<uint64_t>::max(),
			2,
			std::numeric_limits<uint64_t>::max(),
			physical),
		"broadcast multiplication overflow");
}

void TestAuditPassAndLineage()
{
	OptimizationMetrics metrics;
	RelayOptimizationAudit audit({1, 1}, &metrics);
	const uint8_t root[] = {1, 2, 3, 4};
	const uint64_t identity =
		audit.StableRootIdentity(root, sizeof(root));
	Require(
		identity == audit.StableRootIdentity(root, sizeof(root)),
		"deterministic audit identity");
	const AuditIdentityPart identityParts[] = {
		{root, 2},
		{root + 2, 2},
	};
	Require(
		identity == audit.StableRootIdentity(
			identityParts,
			std::size(identityParts)),
		"scatter/gather audit identity preserves canonical bytes");
	Require(audit.ShouldSample(identity), "deterministic sample");

	int rootTransaction = 0;
	int childTransaction = 0;
	audit.RegisterSampledTransaction(
		&rootTransaction,
		identity,
		true);
	audit.InheritTransaction(
		&rootTransaction,
		&childTransaction);
	Require(
		audit.RootForTransaction(&childTransaction) ==
			std::optional<uint64_t>(identity),
		"audit root lineage");
	Require(
		audit.CheckDirectRelayCount(identity, 1, 1, 1),
		"direct-count audit pass");
	Require(
		audit.CheckClassifiedExactlyOnce(identity, 1),
		"classification audit pass");
	Require(
		audit.CheckDestinationShard(identity, 2, 2),
		"destination audit pass");
	Require(
		audit.CheckBatch(identity, {10, 20}, {10, 20}),
		"batch audit pass");
	Require(
		audit.CheckBatchObservation(
			identity,
			2,
			2,
			2,
			true),
		"observed committed batch audit pass");
	Require(
		audit.CheckBroadcastClones(identity, 3, {2, 0, 1}),
		"broadcast audit pass");
	Require(
		audit.CheckBroadcastClones(identity, 3, 3, true),
		"observed broadcast audit pass");
	Require(!audit.FailureLatched(), "pass must not latch failure");
	audit.ForgetTransaction(&rootTransaction);
	audit.ForgetTransaction(&childTransaction);
	Require(
		!audit.RootForTransaction(&rootTransaction),
		"forgotten root lineage");
	audit.FinalizeSamples();
	const auto snapshot = metrics.Snapshot();
	Require(snapshot.auditSamples == 1, "one sampled audit root");
	Require(snapshot.auditPassed == 1, "normal audit sample passes");
	Require(snapshot.auditFailed == 0, "normal audit sample does not fail");
}

void TestAuditRootOccurrenceIdentity()
{
	const uint8_t payloadA[] = {1, 2, 3, 4};
	const uint8_t payloadB[] = {5, 6, 7, 8};

	RelayOptimizationAudit first({1, 1}, nullptr);
	const uint64_t baseA =
		first.StableRootIdentity(payloadA, sizeof(payloadA));
	const uint64_t baseB =
		first.StableRootIdentity(payloadB, sizeof(payloadB));
	int firstA = 0;
	int firstB = 0;
	int secondA = 0;
	int secondB = 0;
	Require(
		first.RegisterSourceIssuance(&firstA) &&
			first.RegisterSourceIssuance(&firstB) &&
			first.RegisterSourceIssuance(&secondA) &&
			first.RegisterSourceIssuance(&secondB),
		"source issuance metadata is admitted serially");
	const uint64_t a0 =
		first.RootIdentityForIssuance(
			baseA,
			*first.ConsumeSourceIssuance(&firstA));
	const uint64_t b0 =
		first.RootIdentityForIssuance(
			baseB,
			*first.ConsumeSourceIssuance(&firstB));
	const uint64_t a1 =
		first.RootIdentityForIssuance(
			baseA,
			*first.ConsumeSourceIssuance(&secondA));
	const uint64_t b1 =
		first.RootIdentityForIssuance(
			baseB,
			*first.ConsumeSourceIssuance(&secondB));
	Require(a0 != a1, "same payload occurrences have unique identities");
	Require(b0 != b1, "second payload occurrences have unique identities");

	RelayOptimizationAudit reordered({1, 1}, nullptr);
	const uint64_t reorderedBaseA =
		reordered.StableRootIdentity(payloadA, sizeof(payloadA));
	const uint64_t reorderedBaseB =
		reordered.StableRootIdentity(payloadB, sizeof(payloadB));
	Require(
		reordered.RegisterSourceIssuance(&firstA) &&
			reordered.RegisterSourceIssuance(&firstB) &&
			reordered.RegisterSourceIssuance(&secondA) &&
			reordered.RegisterSourceIssuance(&secondB),
		"reordered worker execution has the same admission metadata");
	const uint64_t reorderedB0 =
		reordered.RootIdentityForIssuance(
			reorderedBaseB,
			*reordered.ConsumeSourceIssuance(&firstB));
	const uint64_t reorderedA0 =
		reordered.RootIdentityForIssuance(
			reorderedBaseA,
			*reordered.ConsumeSourceIssuance(&firstA));
	const uint64_t reorderedB1 =
		reordered.RootIdentityForIssuance(
			reorderedBaseB,
			*reordered.ConsumeSourceIssuance(&secondB));
	const uint64_t reorderedA1 =
		reordered.RootIdentityForIssuance(
			reorderedBaseA,
			*reordered.ConsumeSourceIssuance(&secondA));

	Require(
		a0 == reorderedA0 && a1 == reorderedA1,
		"worker execution order preserves payload A source identities");
	Require(
		b0 == reorderedB0 && b1 == reorderedB1,
		"worker execution order preserves payload B source identities");

	OptimizationMetrics metrics;
	RelayOptimizationAudit accounting({1, 1}, &metrics);
	const uint64_t accountingBase =
		accounting.StableRootIdentity(payloadA, sizeof(payloadA));
	int transactions[3] = {};
	for(int &transaction : transactions)
	{
		Require(
			accounting.RegisterSourceIssuance(&transaction),
			"accounting source issuance");
		const uint64_t identity =
			accounting.RootIdentityForIssuance(
				accountingBase,
				*accounting.ConsumeSourceIssuance(
					&transaction));
		Require(
			accounting.ShouldSample(identity),
			"full-rate occurrence is sampled");
		accounting.RegisterSampledTransaction(
			&transaction,
			identity,
			true);
	}
	accounting.FinalizeSamples();
	const auto snapshot = metrics.Snapshot();
	Require(
		snapshot.auditSamples == 3,
		"repeated canonical payload counts every source occurrence");
	Require(
		snapshot.auditPassed == 3,
		"repeated canonical payload finalizes every source occurrence");
	Require(
		snapshot.auditFailed == 0,
		"repeated canonical payload accounting does not fail");
}

void TestAuditWrongCounts()
{
	{
		RelayOptimizationAudit audit({1, 1}, nullptr);
		Require(
			!audit.CheckDirectRelayCount(101, 2, 2, 1),
			"dropped direct relay audit fail");
		RequireAuditFailure(
			audit,
			AuditCheckKind::DirectRelayCount,
			2,
			1,
			"dropped direct relay failure details");
	}
	{
		RelayOptimizationAudit audit({1, 1}, nullptr);
		Require(
			!audit.CheckDirectRelayCount(102, 1, 1, 2),
			"duplicated direct relay audit fail");
		RequireAuditFailure(
			audit,
			AuditCheckKind::DirectRelayCount,
			1,
			2,
			"duplicated direct relay failure details");
	}
	{
		RelayOptimizationAudit audit({1, 1}, nullptr);
		Require(
			!audit.CheckDirectRelayCount(103, std::nullopt, 3, 4),
			"relay upper-bound audit fail");
		RequireAuditFailure(
			audit,
			AuditCheckKind::RelayCountUpperBound,
			3,
			4,
			"relay upper-bound failure details");
	}
	{
		RelayOptimizationAudit audit({1, 1}, nullptr);
		Require(
			audit.CheckDirectRelayCount(
				104,
				std::nullopt,
				std::nullopt,
				999),
			"unknown count must not invent an audit bound");
		Require(!audit.FailureLatched(), "unknown count does not fail");
	}
}

void TestAuditBatchFailures()
{
	{
		RelayOptimizationAudit audit({1, 1}, nullptr);
		int firstTransaction = 0;
		int secondTransaction = 0;
		int *transactions[] = {
			&firstTransaction,
			&secondTransaction,
		};
		audit.RegisterSampledTransaction(
			&firstTransaction,
			190,
			true);
		audit.RegisterSampledTransaction(
			&secondTransaction,
			191,
			true);
		std::vector<uint64_t> roots;
		Require(
			audit.SnapshotUniqueTransactionRoots(
				transactions,
				std::size(transactions),
				roots) &&
				roots == std::vector<uint64_t>({190, 191}),
			"batch roots are snapshotted before queue ownership transfer");
		audit.ForgetTransaction(&firstTransaction);
		audit.ForgetTransaction(&secondTransaction);
		Require(
			!audit.RootForTransaction(&firstTransaction) &&
				!audit.RootForTransaction(&secondTransaction),
			"worker-side lineage cleanup is visible after snapshot");
		for(uint64_t root : roots)
		{
			audit.CheckBatchObservation(
				root,
				2,
				1,
				1,
				false);
		}
		RequireAuditFailure(
			audit,
			AuditCheckKind::BatchElementCount,
			2,
			1,
			"batch audit survives worker-side lineage cleanup");
	}
	{
		RelayOptimizationAudit audit({1, 1}, nullptr);
		Require(
			!audit.CheckBatch(201, {10, 20}, {10}),
			"batch drop audit fail");
		RequireAuditFailure(
			audit,
			AuditCheckKind::BatchElementCount,
			2,
			1,
			"batch drop failure details");
	}
	{
		RelayOptimizationAudit audit({1, 1}, nullptr);
		Require(
			!audit.CheckBatch(202, {10, 20}, {10, 10}),
			"batch duplicate audit fail");
		RequireAuditFailure(
			audit,
			AuditCheckKind::BatchOrder,
			2,
			2,
			"batch duplicate failure details");
	}
	{
		RelayOptimizationAudit audit({1, 1}, nullptr);
		Require(
			!audit.CheckBatch(203, {10, 20}, {20, 10}),
			"batch reorder audit fail");
		RequireAuditFailure(
			audit,
			AuditCheckKind::BatchOrder,
			2,
			2,
			"batch reorder failure details");
	}
	{
		RelayOptimizationAudit audit({1, 1}, nullptr);
		Require(
			!audit.CheckBatchObservation(
				204,
				2,
				1,
				2,
				true),
			"observed queue-size drop audit fail");
		RequireAuditFailure(
			audit,
			AuditCheckKind::BatchElementCount,
			2,
			1,
			"observed queue-size drop failure details");
	}
	{
		RelayOptimizationAudit audit({1, 1}, nullptr);
		Require(
			!audit.CheckBatchObservation(
				205,
				2,
				2,
				3,
				true),
			"observed queue-tail duplicate audit fail");
		RequireAuditFailure(
			audit,
			AuditCheckKind::BatchElementCount,
			2,
			3,
			"observed queue-tail duplicate failure details");
	}
	{
		RelayOptimizationAudit audit({1, 1}, nullptr);
		Require(
			!audit.CheckBatchObservation(
				206,
				2,
				2,
				2,
				false),
			"observed queue-tail reorder audit fail");
		RequireAuditFailure(
			audit,
			AuditCheckKind::BatchOrder,
			2,
			2,
			"observed queue-tail reorder failure details");
	}
}

void TestAuditRoutingFailures()
{
	{
		RelayOptimizationAudit audit({1, 1}, nullptr);
		Require(
			!audit.CheckClassifiedExactlyOnce(300, 0),
			"dropped logical classification audit fail");
		RequireAuditFailure(
			audit,
			AuditCheckKind::LogicalClassification,
			1,
			0,
			"dropped classification failure details");
	}
	{
		RelayOptimizationAudit audit({1, 1}, nullptr);
		Require(
			!audit.CheckClassifiedExactlyOnce(301, 2),
			"duplicate logical classification audit fail");
		RequireAuditFailure(
			audit,
			AuditCheckKind::LogicalClassification,
			1,
			2,
			"duplicate classification failure details");
	}
	{
		RelayOptimizationAudit audit({1, 1}, nullptr);
		Require(
			!audit.CheckDestinationShard(302, 3, 4),
			"destination mismatch audit fail");
		RequireAuditFailure(
			audit,
			AuditCheckKind::DestinationShard,
			3,
			4,
			"destination mismatch failure details");
	}
	{
		RelayOptimizationAudit audit({1, 1}, nullptr);
		Require(
			!audit.CheckBroadcastClones(303, 4, {0, 1, 2}),
			"missing broadcast clone audit fail");
		RequireAuditFailure(
			audit,
			AuditCheckKind::BroadcastCloneCount,
			4,
			3,
			"missing broadcast clone failure details");
	}
	{
		RelayOptimizationAudit audit({1, 1}, nullptr);
		Require(
			!audit.CheckBroadcastClones(304, 4, {0, 1, 1, 3}),
			"duplicate broadcast destination audit fail");
		RequireAuditFailure(
			audit,
			AuditCheckKind::BroadcastCloneCount,
			1,
			2,
			"duplicate broadcast destination failure details");
	}
	{
		RelayOptimizationAudit audit({1, 1}, nullptr);
		Require(
			!audit.CheckBroadcastClones(305, 4, 3, true),
			"observed missing broadcast clone audit fail");
		RequireAuditFailure(
			audit,
			AuditCheckKind::BroadcastCloneCount,
			4,
			3,
			"observed missing broadcast clone failure details");
	}
	{
		RelayOptimizationAudit audit({1, 1}, nullptr);
		Require(
			!audit.CheckBroadcastClones(306, 4, 4, false),
			"observed duplicate broadcast destination audit fail");
		RequireAuditFailure(
			audit,
			AuditCheckKind::BroadcastCloneCount,
			4,
			4,
			"observed duplicate broadcast destination details");
	}
}

void TestAuditFailureLatch()
{
	OptimizationMetrics metrics;
	RelayOptimizationAudit audit({1, 1}, &metrics);
	int rootTransaction = 0;
	audit.RegisterSampledTransaction(&rootTransaction, 401, true);
	audit.FailSample(AuditFailure{
		AuditCheckKind::InjectedFailure,
		401,
		1,
		0,
		"test-only injected failure"});
	RequireAuditFailure(
		audit,
		AuditCheckKind::InjectedFailure,
		1,
		0,
		"safe-point failure latch");
	Require(
		!audit.CheckDestinationShard(401, 7, 8),
		"later audit failure still reported");
	const auto first = audit.FirstFailure();
	Require(
		first && first->kind == AuditCheckKind::InjectedFailure,
		"first failure remains latched");
	audit.FinalizeSamples();
	const auto snapshot = metrics.Snapshot();
	Require(snapshot.auditSamples == 1, "latched sample counted");
	Require(snapshot.auditFailed == 1, "failed root counted exactly once");
	Require(snapshot.auditPassed == 0, "failed root never counted passed");
}

#ifdef RPREDA_RUNTIME_OPTIMIZATION_TESTING
void TestAuditInjectionHook()
{
	RelayOptimizationAudit audit({1, 1}, nullptr);
	audit.InjectFailureOnce(AuditCheckKind::DestinationShard);
	Require(
		!audit.CheckDestinationShard(402, 7, 7),
		"test-only fault injection forces a matching check to fail");
	RequireAuditFailure(
		audit,
		AuditCheckKind::InjectedFailure,
		0,
		0,
		"test-only injected failure details");
	Require(
		audit.CheckDestinationShard(402, 7, 7),
		"test-only fault injection is consumed exactly once");
}
#endif

void TestMeasurementWindow()
{
	OptimizationMetrics invalidMetrics;
	invalidMetrics.ReportMeasurementWindow();
	const auto invalidWindow =
		invalidMetrics.SnapshotMeasurementWindow();
	Require(
		invalidWindow.status ==
			MeasurementWindowStatus::ReportWithoutRestart,
		"report without restart is diagnosed");
	Require(
		!invalidWindow.metricsAvailable,
		"invalid measurement window has no metrics");

	OptimizationMetrics metrics;
	metrics.plan.planLoads.store(1);
	metrics.optimizationEligibleInvocations.store(2);
	metrics.ObserveBatchSize(99);
	Require(
		metrics.SnapshotMeasurementWindow().status ==
			MeasurementWindowStatus::NotStarted,
		"measurement window starts absent");

	metrics.RestartMeasurementWindow();
	const auto active = metrics.SnapshotMeasurementWindow();
	Require(
		active.status == MeasurementWindowStatus::Active,
		"restart activates measurement window");
	Require(
		!active.metricsAvailable,
		"active window is not published as complete");

	metrics.plan.planCacheHits.fetch_add(3);
	metrics.optimizationEligibleInvocations.fetch_add(4);
	metrics.queueBatchPushCalls.fetch_add(2);
	metrics.queueBatchElements.fetch_add(7);
	metrics.queuePushTimeNs.fetch_add(1234);
	metrics.auditSamples.fetch_add(2);
	metrics.ObserveBatchSize(7);
	metrics.ObserveBatchSize(3);
	metrics.ReportMeasurementWindow();

	// Lifetime accounting continues after stopwatch.report and remains
	// available separately from the frozen measurement window.
	metrics.plan.planLoads.fetch_add(1);
	metrics.optimizationEligibleInvocations.fetch_add(8);
	metrics.queueBatchElements.fetch_add(11);
	metrics.auditPassed.fetch_add(2);
	metrics.ObserveBatchSize(101);

	const auto window = metrics.SnapshotMeasurementWindow();
	Require(
		window.status == MeasurementWindowStatus::Completed,
		"report completes measurement window");
	Require(window.metricsAvailable, "completed window has metrics");
	Require(window.restartCount == 1, "window restart count");
	Require(window.reportCount == 1, "window report count");
	Require(
		window.metrics.plan.planLoads == 0,
		"pre/post-window plan loads excluded");
	Require(
		window.metrics.plan.planCacheHits == 3,
		"window plan cache hits");
	Require(
		window.metrics.optimizationEligibleInvocations == 4,
		"window eligible invocations");
	Require(
		window.metrics.queueBatchPushCalls == 2 &&
			window.metrics.queueBatchElements == 7,
		"window batch counters");
	Require(
		window.metrics.maximumBatchSize == 7,
		"window maximum batch is independent of lifetime maximum");
	Require(
		window.metrics.queuePushTimeNs == 1234,
		"window timing counters");
	Require(
		window.metrics.auditSamples == 2 &&
			window.metrics.auditPassed == 0 &&
			window.metrics.auditFailed == 0,
		"window keeps unresolved audit samples pending");

	const auto lifetime = metrics.Snapshot();
	Require(lifetime.plan.planLoads == 2, "lifetime plan loads retained");
	Require(
		lifetime.optimizationEligibleInvocations == 14,
		"lifetime eligible invocations retained");
	Require(
		lifetime.queueBatchElements == 18,
		"lifetime batch elements retained");
	Require(
		lifetime.maximumBatchSize == 101,
		"lifetime maximum batch retained");

	OptimizationConfig config;
	RelayOptimizationReport report;
	const auto parsed = nlohmann::json::parse(
		report.BuildJson(config, lifetime, std::nullopt, window));
	Require(
		parsed["report_schema_version"] == 2,
		"measurement report schema");
	Require(
		parsed["counters"]["plan_loads"] == 2 &&
			parsed["lifetime"]["counters"]["plan_loads"] == 2,
		"schema-v1 alias and explicit lifetime agree");
	Require(
		parsed["measurement_window"]["status"] == "completed" &&
			parsed["measurement_window"]["metrics_available"] == true,
		"completed measurement status emitted");
	Require(
		parsed["measurement_window"]["counters"]
			["optimization_eligible_invocations"] == 4,
		"measurement counters emitted");
	Require(
		parsed["measurement_window"]["derived"]
			["average_batch_size"] == 3.5,
		"measurement derived metrics emitted");
	Require(
		parsed["measurement_window"]["counters"]
				["audit_pending"] == 2 &&
			parsed["measurement_window"]["counters"]
				["audit_accounting_consistent"] == true,
		"measurement audit accounting exposes unresolved samples");
	Require(
		parsed["lifetime"]["counters"]["audit_pending"] == 0 &&
			parsed["lifetime"]["counters"]
				["audit_accounting_consistent"] == true,
		"lifetime audit accounting resolves finalized samples");
}

void TestReport()
{
	OptimizationConfig config;
	config.mode = OptimizationMode::Optimize;
	config.ablation =
		OptimizationAblation::VerifiedReservePlusBatch;
	OptimizationMetrics metrics;
	metrics.queueBatchPushCalls.store(2);
	metrics.queueBatchElements.store(7);
	RelayOptimizationReport report;
	const auto parsed =
		nlohmann::json::parse(report.BuildJson(
			config,
			metrics.Snapshot(),
			std::nullopt));
	Require(
		parsed["config"]["mode"] == "optimize",
		"report mode");
	Require(
		parsed["derived"]["average_batch_size_numerator"] == 7,
		"report batch numerator");
	Require(
		parsed["derived"]["average_batch_size_denominator"] == 2,
		"report batch denominator");

	const auto unique = std::chrono::steady_clock::now()
		.time_since_epoch().count();
	const std::filesystem::path directory =
		std::filesystem::temp_directory_path() /
		("rpreda-relay-optimization-tests-" +
		 std::to_string(unique));
	const std::filesystem::path destination =
		directory / "nested" / "report.json";
	std::string error;
	Require(
		report.WriteAtomically(
			destination.string(),
			config,
			metrics.Snapshot(),
			std::nullopt,
			&error),
		"atomic report initial write");
	Require(
		std::filesystem::is_regular_file(destination),
		"atomic report destination exists");

	config.mode = OptimizationMode::OptimizeAudit;
	metrics.queueBatchPushCalls.store(3);
	metrics.queueBatchElements.store(12);
	Require(
		report.WriteAtomically(
			destination.string(),
			config,
			metrics.Snapshot(),
			AuditFailure{
				AuditCheckKind::BatchOrder,
				500,
				2,
				2,
				"synthetic report failure"},
			&error),
		"atomic report replacement");
	std::ifstream stream(destination, std::ios::in | std::ios::binary);
	std::ostringstream contents;
	contents << stream.rdbuf();
	Require(stream.good() || stream.eof(), "atomic report readable");
	const auto onDisk = nlohmann::json::parse(contents.str());
	Require(
		onDisk["config"]["mode"] == "optimize_audit",
		"atomic replacement publishes complete new report");
	Require(
		onDisk["counters"]["queue_batch_elements"] == 12,
		"atomic replacement publishes new counters");
	Require(
		onDisk["audit"]["first_failure"]["check_kind"] ==
			"batch_order",
		"atomic report includes audit failure");
	for(const auto &entry :
		std::filesystem::directory_iterator(destination.parent_path()))
	{
		Require(
			entry.path().filename() == destination.filename(),
			"atomic write leaves no temporary sibling");
	}
	std::filesystem::remove_all(directory);
	Require(
		report.WriteAtomically(
			"", config, metrics.Snapshot(), std::nullopt, &error),
		"empty report path is a no-op");
}

} // namespace

int main()
{
	TestConfig();
	TestReserve();
	TestAuditPassAndLineage();
	TestAuditRootOccurrenceIdentity();
	TestAuditWrongCounts();
	TestAuditBatchFailures();
	TestAuditRoutingFailures();
	TestAuditFailureLatch();
#ifdef RPREDA_RUNTIME_OPTIMIZATION_TESTING
	TestAuditInjectionHook();
#endif
	TestMeasurementWindow();
	TestReport();
	std::cout << "relay optimization tests passed\n";
	return 0;
}
