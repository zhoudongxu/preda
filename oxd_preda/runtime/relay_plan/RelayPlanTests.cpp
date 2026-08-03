#include "RelayPlanRegistry.h"

#include "../../3rdParty/nlohmann/json.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using Json = nlohmann::ordered_json;
using namespace oxd::relay_plan;

struct TestState
{
	int failures = 0;

	void Expect(bool condition, const char *message)
	{
		if (condition)
			return;
		++failures;
		std::cerr << "FAILED: " << message << '\n';
	}
};

template <typename Predicate>
bool WaitUntil(Predicate predicate)
{
	const auto deadline =
		std::chrono::steady_clock::now() +
		std::chrono::seconds(5);
	while (!predicate())
	{
		if (std::chrono::steady_clock::now() >= deadline)
			return false;
		std::this_thread::sleep_for(
			std::chrono::milliseconds(1));
	}
	return true;
}

std::shared_ptr<RelayPlanLoadResult> SyntheticLoadedResult(
	const char *diagnostic)
{
	auto result = std::make_shared<RelayPlanLoadResult>();
	result->status = ManifestLoadStatus::Loaded;
	result->diagnostic = diagnostic;
	result->manifest = std::make_shared<BoundRelayManifest>();
	return result;
}

Json SolverResult(const std::string &backend, const std::string &status)
{
	return Json{
		{"backend", backend},
		{"status", status},
		{"elapsed_time_ms", 0.0},
		{"assumption_constraint_ids", Json::array()},
		{"reason", ""},
		{"projected_counterexample", Json::array()},
	};
}

Json CountObligation(
	const std::string &kind,
	const std::string &id,
	const std::string &proofRole,
	const std::string &backend,
	const std::string &solverStatus,
	bool includeSolverResult = true)
{
	Json obligation{
		{"id", id},
		{"kind", kind},
		{"status", "Generated"},
		{"source_function_id", "root"},
		{"relay_site_id", ""},
		{"related_relay_site_id", ""},
		{"proof_role", proofRole},
	};
	if (includeSolverResult)
	{
		obligation["solver_result"] =
			SolverResult(backend, solverStatus);
	}
	return obligation;
}

Json BuildManifest(
	const std::string &moduleId,
	const Json &count,
	const Json &upper,
	const Json &obligations)
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
		{"binding_complete", true},
	};
	root["handlers"] = Json::array({
		Json{
			{"id", "handler-0"},
			{"opcode", uint32_t(42)},
			{"resolved", true},
		},
	});
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
	root["functions"] = Json::array({
		Json{
			{"source_function_id", "root"},
			{"exported_opcode", uint32_t(7)},
			{"summary",
			 Json{
				 {"relay_count", count},
				 {"relay_count_upper_bound", upper},
				 {"max_depth",
				  Json{{"kind", "constant"}, {"value", uint64_t(1)}}},
				 {"relay_site_set", Json::array({"site-0"})},
				 {"fanout", Json::array({"single_target"})},
				 {"has_opaque", false},
				 {"has_unmodeled_relay_reachable_call", false},
			 }},
		},
	});
	root["refinement"] =
		Json{{"proof_obligations", obligations}};
	return root;
}

std::string BindManifest(Json &manifest)
{
	std::string error;
	const std::string hash =
		RelayPlanLoader::ComputeManifestSelfHash(
			manifest.dump(2),
			&error);
	if (hash.empty())
		throw std::runtime_error(error);
	manifest["artifact_binding"]["manifest_hash"] = hash;
	return hash;
}

void WriteText(const std::filesystem::path &path, const std::string &text)
{
	std::filesystem::create_directories(path.parent_path());
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	output << text;
}

ExpectedArtifactBinding Expected(
	const std::string &moduleId,
	const std::string &trustedHash)
{
	ExpectedArtifactBinding expected;
	expected.identity.dapp = "testdapp";
	expected.identity.contract = "Contract";
	expected.identity.transpilerVersion = "test-version";
	expected.identity.intermediateHash = "intermediate";
	expected.identity.moduleId = moduleId;
	expected.identity.moduleHashKind = "preda_module_id";
	expected.identity.moduleHash = moduleId;
	expected.requireModuleHash = true;
	expected.trustedManifestHash = trustedHash;
	return expected;
}

std::filesystem::path ManifestPath(
	const std::filesystem::path &root,
	const std::string &moduleId)
{
	return root / "by_module" /
		(moduleId + ".relay_protocol.json");
}

Json Constant(uint64_t value)
{
	return Json{{"kind", "constant"}, {"value", value}};
}

Json Unknown()
{
	return Json{{"kind", "unknown"}, {"reason", "test unknown"}};
}

Json CertificateEvidence(
	const std::string &kind,
	const std::string &id,
	const std::string &status)
{
	return Json{
		{"id", id},
		{"kind", kind},
		{"status", "Generated"},
		{"source_function_id", "root"},
		{"proof_role", "SolverGoal"},
		{"solver_result", SolverResult("z3", status)},
	};
}

Json LogicalWorkCertificate(
	const std::string &id,
	uint64_t bound,
	const std::string &status = "Complete")
{
	return Json{
		{"certificate_id", id},
		{"status", status},
		{"exact", Constant(bound)},
		{"upper_bound", Constant(bound)},
		{"reason", "test logical work certificate"},
		{"supporting_cfg_fact_ids", Json::array()},
		{"supporting_constraint_ids", Json::array()},
		{"supporting_solver_result_ids", Json::array()},
	};
}

void AddParallelCertificate(Json &manifest)
{
	manifest["relay_sites"].push_back(Json{
		{"ordinal", uint32_t(1)},
		{"id", "site-1"},
		{"source_function_id", "root"},
		{"handler_id", "handler-0"},
		{"relay_kind", "custom_scope"},
		{"target_scope", "address"},
		{"location", Json{{"line", 9}, {"column", 2}}},
	});
	manifest["functions"][0]["summary"]["relay_site_set"].push_back(
		"site-1");
	manifest["control_flow"] = Json{
		{"functions", Json::array({
			Json{
				{"function_id", "root"},
				{"nodes", Json::array({
					Json{{"id", "cfg-root-entry"}},
				})},
			},
		})},
	};
	manifest["refinement"]["constraints"] = Json::array({
		Json{{"id", "constraint-target-separation"}},
	});
	manifest["refinement"]["proof_obligations"].push_back(
		CertificateEvidence(
			"RelayMutualExclusion",
			"pair-coemit-witness",
			"Disproved"));
	manifest["refinement"]["proof_obligations"].push_back(
		CertificateEvidence(
			"RelayTargetIndependence",
			"pair-target-independent",
			"Proved"));

	const Json direct = LogicalWorkCertificate("work-direct-root", 2);
	const Json transitive =
		LogicalWorkCertificate("work-transitive-root", 4, "Conservative");
	const Json depth =
		LogicalWorkCertificate("work-depth-root", 2, "Conservative");
	const Json physical{
		{"certificate_id", "work-physical-root"},
		{"status", "Conservative"},
		{"bound_kind", "ParameterizedUpperBound"},
		{"constant_term", uint64_t(1)},
		{"active_shard_count_coefficient", uint64_t(1)},
		{"expression", "1 + active_shard_count"},
		{"reason", "test physical route certificate"},
		{"supporting_cfg_fact_ids", Json::array()},
		{"supporting_constraint_ids", Json::array()},
		{"supporting_solver_result_ids", Json::array()},
	};
	manifest["parallel_certificate"] = Json{
		{"extension_schema_version", uint32_t(1)},
		{"functions", Json::array({
			Json{
				{"source_function_id", "root"},
				{"certificate_status", "Conservative"},
				{"reason", "test function certificate"},
				{"pair_relations", Json::array({
					Json{
						{"certificate_id", "pair-independent-root"},
						{"site_a", "site-0"},
						{"site_b", "site-1"},
						{"relation", "CoEmissionIndependent"},
						{"status", "Proved"},
						{"reason", "co-emission and target inequality proved"},
						{"supporting_cfg_fact_ids", Json::array({"cfg-root-entry"})},
						{"supporting_constraint_ids", Json::array({"constraint-target-separation"})},
						{"supporting_solver_result_ids", Json::array({"pair-coemit-witness", "pair-target-independent"})},
						{"counterexample", nullptr},
						{"location", Json{
							{"site_a", Json{{"line", 3}, {"column", 4}}},
							{"site_b", Json{{"line", 9}, {"column", 2}}},
						}},
					},
				})},
				{"direct_work_bound", direct},
				{"direct_logical_work", direct},
				{"transitive_work_bound", transitive},
				{"transitive_logical_work", transitive},
				{"physical_route_work", physical},
				{"depth_bound", depth},
				{"relay_tree_depth", depth},
			},
		})},
	};
}

std::shared_ptr<const RelayPlanLoadResult> LoadWrittenManifest(
	const std::filesystem::path &root,
	const std::string &module,
	Json manifest)
{
	manifest["artifact_binding"].erase("manifest_hash");
	const std::string hash = BindManifest(manifest);
	WriteText(ManifestPath(root, module), manifest.dump(2));
	RelayPlanRegistry registry(root.string());
	return registry.Load(Expected(module, hash));
}

void TestExactPlanAndCapabilities(
	TestState &state,
	const std::filesystem::path &root)
{
	const std::string module = "exact-module";
	Json manifest = BuildManifest(
		module,
		Constant(1),
		Constant(1),
		Json::array({
			CountObligation(
				"RelayCountEquality",
				"count-equality-root",
				"EstablishedByConstruction",
				"compiler",
				"EstablishedByConstruction"),
			CountObligation(
				"RelayCountUpperBound",
				"count-upper-root",
				"SolverGoal",
				"z3",
				"Proved"),
		}));
	const std::string hash = BindManifest(manifest);
	WriteText(ManifestPath(root, module), manifest.dump(2));

	RelayPlanRegistry registry(root.string());
	auto lookup = registry.Lookup(Expected(module, hash), 7);
	state.Expect(
		static_cast<bool>(lookup),
		"correct complete binding and compiler-established exact count are eligible");
	state.Expect(
		lookup.function != nullptr &&
			lookup.function->directCount.kind ==
				CountPlanKind::ExactConstant &&
			lookup.function->directCount.value == 1,
		"exact constant plan retains its value and evidence");
	state.Expect(
		lookup.function != nullptr &&
			lookup.function->mayCustom &&
			!lookup.function->mayGlobal &&
			!lookup.function->mayBroadcast &&
			!lookup.function->mayDeferred &&
			!lookup.function->mayIntra,
		"relay kind capabilities are conservatively aggregated from relay sites");
	state.Expect(
		!registry.Lookup(module, 99),
		"unknown opcode falls back");
	state.Expect(
		static_cast<bool>(registry.Lookup(module, 7)),
		"module plus opcode lookup reuses the immutable loaded plan");
}

void TestProvedUpperBound(
	TestState &state,
	const std::filesystem::path &root)
{
	const std::string module = "upper-module";
	Json manifest = BuildManifest(
		module,
		Json{{"kind", "ite"}, {"condition", "cond"}},
		Constant(3),
		Json::array({
			CountObligation(
				"RelayCountUpperBound",
				"count-upper-root",
				"SolverGoal",
				"z3",
				"Proved"),
		}));
	const std::string hash = BindManifest(manifest);
	WriteText(ManifestPath(root, module), manifest.dump(2));

	RelayPlanRegistry registry(root.string());
	auto lookup = registry.Lookup(Expected(module, hash), 7);
	state.Expect(
		static_cast<bool>(lookup) &&
			lookup.function->directCount.kind ==
				CountPlanKind::SymbolicUnsupportedAtRuntime &&
			lookup.function->directCountUpperBound.kind ==
				CountPlanKind::ProvedConstantUpperBound &&
			lookup.function->directCountUpperBound.value == 3,
		"a solver-Proved constant upper bound is eligible when exact count is symbolic");
}

void TestUnacceptableEvidence(
	TestState &state,
	const std::filesystem::path &root)
{
	struct Case
	{
		const char *module;
		const char *solverStatus;
		bool includeSolver;
	};
	const Case cases[] = {
		{"generated-only-module", "", false},
		{"disproved-module", "Disproved", true},
		{"unknown-module", "Unknown", true},
		{"unsupported-module", "Unsupported", true},
		{"encoding-error-module", "EncodingError", true},
		{"inconsistent-module", "InconsistentAssumptions", true},
	};

	for (const Case &testCase : cases)
	{
		Json manifest = BuildManifest(
			testCase.module,
			Unknown(),
			Constant(5),
			Json::array({
				CountObligation(
					"RelayCountUpperBound",
					"count-upper-root",
					"SolverGoal",
					"z3",
					testCase.solverStatus,
					testCase.includeSolver),
			}));
		const std::string hash = BindManifest(manifest);
		WriteText(
			ManifestPath(root, testCase.module),
			manifest.dump(2));

		RelayPlanRegistry registry(root.string());
		auto lookup =
			registry.Lookup(Expected(testCase.module, hash), 7);
		state.Expect(
			!lookup &&
				lookup.function != nullptr &&
				!lookup.function->reserveEligible,
			"non-Proved upper-bound evidence cannot guide runtime reserve");
	}
}

void TestBindingFailuresAndReload(
	TestState &state,
	const std::filesystem::path &root)
{
	const std::string module = "reload-module";
	Json manifest = BuildManifest(
		module,
		Constant(1),
		Constant(1),
		Json::array({
			CountObligation(
				"RelayCountEquality",
				"count-equality-root",
				"EstablishedByConstruction",
				"compiler",
				"EstablishedByConstruction"),
		}));
	const std::string firstHash = BindManifest(manifest);
	const std::filesystem::path path = ManifestPath(root, module);
	WriteText(path, manifest.dump(2));

	RelayPlanRegistry registry(root.string());
	auto first = registry.Load(Expected(module, firstHash));
	state.Expect(
		first && static_cast<bool>(*first),
		"initial trusted module manifest loads");

	manifest["functions"][0]["summary"]["relay_count"]["value"] =
		uint64_t(2);
	manifest["functions"][0]["summary"]["relay_count_upper_bound"]["value"] =
		uint64_t(2);
	manifest["artifact_binding"].erase("manifest_hash");
	const std::string secondHash = BindManifest(manifest);
	WriteText(path, manifest.dump(2));

	auto cached = registry.Load(Expected(module, firstHash));
	state.Expect(
		cached == first,
		"module manifest remains immutable until explicit invalidation");
	registry.Invalidate(module);
	auto staleExpected = registry.Load(Expected(module, firstHash));
	state.Expect(
		staleExpected->status == ManifestLoadStatus::ManifestHashMismatch,
		"changed manifest is rejected against the stale trusted hash");

	registry.Invalidate(module);
	auto reloaded = registry.Lookup(Expected(module, secondHash), 7);
	state.Expect(
		reloaded &&
			reloaded.function->directCount.value == 2,
		"module invalidation permits a newly bound manifest to reload");
	state.Expect(
		registry.Metrics().Snapshot().planReloads >= 1 &&
			registry.Metrics().Snapshot().planInvalidations >= 1,
		"reload and invalidation metrics are recorded");

	const std::string staleModule = "stale-binding-module";
	Json stale = BuildManifest(
		module,
		Constant(1),
		Constant(1),
		Json::array());
	const std::string staleHash = BindManifest(stale);
	WriteText(ManifestPath(root, staleModule), stale.dump(2));
	auto mismatch =
		registry.Load(Expected(staleModule, staleHash));
	state.Expect(
		mismatch->status ==
			ManifestLoadStatus::ManifestBindingMismatch,
		"stale module binding falls back");

	Json tampered = BuildManifest(
		"tampered-module",
		Constant(1),
		Constant(1),
		Json::array());
	const std::string tamperedHash = BindManifest(tampered);
	tampered["handlers"][0]["opcode"] = uint32_t(99);
	WriteText(
		ManifestPath(root, "tampered-module"),
		tampered.dump(2));
	auto hashMismatch = registry.Load(
		Expected("tampered-module", tamperedHash));
	state.Expect(
		hashMismatch->status ==
			ManifestLoadStatus::ManifestHashMismatch,
		"manifest self-hash mismatch falls back");
}

void TestParallelCertificateLoading(
	TestState &state,
	const std::filesystem::path &root)
{
	const std::string module = "parallel-certificate-module";
	Json manifest = BuildManifest(
		module,
		Constant(2),
		Constant(2),
		Json::array({
			CountObligation(
				"RelayCountEquality",
				"count-equality-root",
				"EstablishedByConstruction",
				"compiler",
				"EstablishedByConstruction"),
		}));
	AddParallelCertificate(manifest);
	const std::string hash = BindManifest(manifest);
	WriteText(ManifestPath(root, module), manifest.dump(2));

	RelayPlanRegistry registry(root.string());
	auto loaded = registry.Load(Expected(module, hash));
	state.Expect(
		loaded && static_cast<bool>(*loaded),
		"schema-v5 manifest accepts the additive certificate extension");
	if (!loaded || !loaded->manifest)
		return;
	auto found = loaded->manifest->parallelCertificatesByFunction.find(
		"root");
	state.Expect(
		loaded->manifest->parallelCertificateExtensionSchemaVersion == 1 &&
			found !=
				loaded->manifest->parallelCertificatesByFunction.end(),
		"certificate extension is retained as owning runtime data");
	if (found ==
		loaded->manifest->parallelCertificatesByFunction.end())
	{
		return;
	}
	const ManifestFunctionParallelCertificate &certificate =
		found->second;
	state.Expect(
		certificate.pairRelations.size() == 1 &&
			certificate.pairRelations.front().relation ==
				RelayPairCertificateRelation::CoEmissionIndependent &&
			certificate.pairRelations.front().status ==
				ParallelCertificateStatus::Proved,
		"proved pair relation and its evidence-backed status are preserved");
	state.Expect(
		certificate.directLogicalWork.upperBound ==
			std::optional<uint64_t>(2) &&
			certificate.transitiveLogicalWork.upperBound ==
				std::optional<uint64_t>(4) &&
			certificate.physicalRouteWork.boundKind ==
				ManifestWorkBoundKind::ParameterizedUpperBound &&
			certificate.physicalRouteWork.activeShardCountCoefficient == 1,
		"logical and shard-parameterized physical work bounds are retained");
	state.Expect(
		registry.Lookup(module, 7).function != nullptr &&
			registry.Lookup(module, 7).function->directCount.kind ==
				CountPlanKind::ExactConstant,
		"certificate loading does not alter optimizer plan eligibility");
}

void TestParallelCertificateRejections(
	TestState &state,
	const std::filesystem::path &root)
{
	auto base = [](const std::string &module)
	{
		Json manifest = BuildManifest(
			module,
			Constant(2),
			Constant(2),
			Json::array());
		AddParallelCertificate(manifest);
		return manifest;
	};

	{
		const std::string module = "certificate-missing-evidence-module";
		Json manifest = base(module);
		manifest["parallel_certificate"]["functions"][0]
			["pair_relations"][0]["supporting_solver_result_ids"][1] =
			"missing-proof";
		auto loaded = LoadWrittenManifest(root, module, std::move(manifest));
		state.Expect(
			loaded && loaded->status ==
				ManifestLoadStatus::ManifestParseError,
			"certificate rejects dangling solver evidence references");
	}

	{
		const std::string module = "certificate-invalid-proof-gate-module";
		Json manifest = base(module);
		manifest["refinement"]["proof_obligations"].back()
			["solver_result"]["status"] = "Disproved";
		auto loaded = LoadWrittenManifest(root, module, std::move(manifest));
		state.Expect(
			loaded && loaded->status ==
				ManifestLoadStatus::ManifestParseError,
			"strong pair relation rejects evidence with the wrong proof status");
	}

	{
		const std::string module = "certificate-duplicate-id-module";
		Json manifest = base(module);
		manifest["parallel_certificate"]["functions"][0]
			["physical_route_work"]["certificate_id"] =
			"work-direct-root";
		auto loaded = LoadWrittenManifest(root, module, std::move(manifest));
		state.Expect(
			loaded && loaded->status ==
				ManifestLoadStatus::ManifestParseError,
			"certificate rejects duplicate property identifiers");
	}

	{
		const std::string module = "certificate-alias-disagreement-module";
		Json manifest = base(module);
		manifest["parallel_certificate"]["functions"][0]
			["direct_logical_work"]["upper_bound"]["value"] = uint64_t(3);
		auto loaded = LoadWrittenManifest(root, module, std::move(manifest));
		state.Expect(
			loaded && loaded->status ==
				ManifestLoadStatus::ManifestParseError,
			"certificate rejects disagreeing compatibility aliases");
	}

	{
		const std::string module = "certificate-version-module";
		Json manifest = base(module);
		manifest["parallel_certificate"]["extension_schema_version"] =
			uint32_t(2);
		auto loaded = LoadWrittenManifest(root, module, std::move(manifest));
		state.Expect(
			loaded && loaded->status ==
				ManifestLoadStatus::ManifestParseError,
			"certificate rejects an unsupported extension schema version");
	}
}

void TestConcurrentSingleLoad(
	TestState &state,
	const std::filesystem::path &root)
{
	const std::string module = "concurrent-module";
	Json manifest = BuildManifest(
		module,
		Constant(1),
		Constant(1),
		Json::array({
			CountObligation(
				"RelayCountEquality",
				"count-equality-root",
				"EstablishedByConstruction",
				"compiler",
				"EstablishedByConstruction"),
		}));
	const std::string hash = BindManifest(manifest);
	WriteText(ManifestPath(root, module), manifest.dump(2));
	const ExpectedArtifactBinding expected = Expected(module, hash);

	RelayPlanRegistry registry(root.string());
	constexpr size_t ThreadCount = 16;
	std::vector<std::shared_ptr<const RelayPlanLoadResult>> results(
		ThreadCount);
	std::vector<std::thread> threads;
	threads.reserve(ThreadCount);
	for (size_t index = 0; index < ThreadCount; ++index)
	{
		threads.emplace_back([&, index]() {
			results[index] = registry.Load(expected);
		});
	}
	for (std::thread &thread : threads)
		thread.join();

	bool same = results.front() != nullptr;
	for (const auto &result : results)
		same = same && result == results.front();
	const RelayPlanMetricsSnapshot metrics =
		registry.Metrics().Snapshot();
	state.Expect(
		same && metrics.planLoads == 1 &&
			metrics.planCacheMisses == 1 &&
			metrics.planCacheHits == ThreadCount - 1,
		"concurrent callers share one parse outside the global mutex");
}

void TestInvalidateDuringInflightLoad(TestState &state)
{
	const ExpectedArtifactBinding expected =
		Expected("invalidate-race-module", "");
	auto stale =
		SyntheticLoadedResult("pre-invalidation generation");
	auto fresh =
		SyntheticLoadedResult("post-invalidation generation");
	std::atomic<uint32_t> loadCalls{0};
	std::mutex gateMutex;
	std::condition_variable gate;
	bool firstLoadStarted = false;
	bool releaseFirstLoad = false;

	RelayPlanRegistry registry(
		{},
		nullptr,
		[&](const ExpectedArtifactBinding &)
			-> std::shared_ptr<RelayPlanLoadResult>
		{
			const uint32_t call =
				loadCalls.fetch_add(
					1,
					std::memory_order_relaxed) + 1;
			if (call != 1)
				return fresh;
			std::unique_lock<std::mutex> lock(gateMutex);
			firstLoadStarted = true;
			gate.notify_all();
			gate.wait(lock, [&]() {
				return releaseFirstLoad;
			});
			return stale;
		});

	std::shared_ptr<const RelayPlanLoadResult> firstResult;
	std::shared_ptr<const RelayPlanLoadResult> waiterResult;
	std::thread first([&]() {
		firstResult = registry.Load(expected);
	});
	{
		std::unique_lock<std::mutex> lock(gateMutex);
		const bool started = gate.wait_for(
			lock,
			std::chrono::seconds(5),
			[&]() { return firstLoadStarted; });
		state.Expect(
			started,
			"invalidation test loader starts");
	}
	std::thread waiter([&]() {
		waiterResult = registry.Load(expected);
	});
	const bool waiterBlockedOnGeneration = WaitUntil([&]() {
		return registry.Metrics().Snapshot().planCacheHits >= 1;
	});
	state.Expect(
		waiterBlockedOnGeneration,
		"concurrent waiter observes the in-flight generation");

	registry.Invalidate(expected.identity.moduleId);
	{
		std::lock_guard<std::mutex> lock(gateMutex);
		releaseFirstLoad = true;
	}
	gate.notify_all();
	first.join();
	waiter.join();

	state.Expect(
		firstResult == fresh && waiterResult == fresh,
		"invalidation discards the in-flight result for loader and waiters");
	state.Expect(
		registry.Lookup(expected.identity.moduleId, 0).loadResult ==
			fresh,
		"module-only lookup publishes only the post-invalidation generation");
	state.Expect(
		loadCalls.load(std::memory_order_relaxed) == 2,
		"invalidation loads exactly one post-invalidation generation");
	const RelayPlanMetricsSnapshot metrics =
		registry.Metrics().Snapshot();
	state.Expect(
		metrics.planInvalidations == 1 &&
			metrics.planReloads == 1 &&
			metrics.planLoads == 2,
		"in-flight invalidation and reload metrics are recorded");
}

void TestLoaderExceptionWakesWaiters(TestState &state)
{
	const ExpectedArtifactBinding expected =
		Expected("throwing-loader-module", "");
	constexpr size_t ThreadCount = 8;
	std::atomic<uint32_t> loadCalls{0};
	std::mutex gateMutex;
	std::condition_variable gate;
	bool loaderStarted = false;
	bool releaseLoader = false;

	RelayPlanRegistry registry(
		{},
		nullptr,
		[&](const ExpectedArtifactBinding &)
			-> std::shared_ptr<RelayPlanLoadResult>
		{
			loadCalls.fetch_add(1, std::memory_order_relaxed);
			std::unique_lock<std::mutex> lock(gateMutex);
			loaderStarted = true;
			gate.notify_all();
			gate.wait(lock, [&]() {
				return releaseLoader;
			});
			throw std::runtime_error(
				"synthetic loader failure");
		});

	std::vector<std::shared_ptr<const RelayPlanLoadResult>> results(
		ThreadCount);
	std::vector<std::thread> threads;
	threads.reserve(ThreadCount);
	for (size_t index = 0; index < ThreadCount; ++index)
	{
		threads.emplace_back([&, index]() {
			results[index] = registry.Load(expected);
		});
	}
	{
		std::unique_lock<std::mutex> lock(gateMutex);
		const bool started = gate.wait_for(
			lock,
			std::chrono::seconds(5),
			[&]() { return loaderStarted; });
		state.Expect(
			started,
			"throwing loader starts");
	}
	const bool allWaitersBlocked = WaitUntil([&]() {
		return registry.Metrics().Snapshot().planCacheHits >=
			ThreadCount - 1;
	});
	state.Expect(
		allWaitersBlocked,
		"all concurrent callers wait on the throwing loader");
	{
		std::lock_guard<std::mutex> lock(gateMutex);
		releaseLoader = true;
	}
	gate.notify_all();
	for (std::thread &thread : threads)
		thread.join();

	bool sharedFallback =
		results.front() != nullptr &&
		results.front()->status ==
			ManifestLoadStatus::ManifestParseError &&
		results.front()->diagnostic ==
			"synthetic loader failure";
	for (const auto &result : results)
		sharedFallback =
			sharedFallback && result == results.front();
	state.Expect(
		sharedFallback,
		"loader exception publishes one conservative fallback to every waiter");
	state.Expect(
		registry.Load(expected) == results.front(),
		"loader exception fallback is a completed cache entry");
	const RelayPlanMetricsSnapshot metrics =
		registry.Metrics().Snapshot();
	state.Expect(
		loadCalls.load(std::memory_order_relaxed) == 1 &&
			metrics.planLoads == 1 &&
			metrics.planLoadFailures == 1,
		"throwing loader executes once and records one failed load");
}

int RunRelayPlanTests()
{
	TestState state;
	const auto unique =
		std::chrono::steady_clock::now().
			time_since_epoch().count();
	const std::filesystem::path root =
		std::filesystem::temp_directory_path() /
		("rpreda-relay-plan-tests-" + std::to_string(unique));

	try
	{
		TestExactPlanAndCapabilities(state, root);
		TestProvedUpperBound(state, root);
		TestUnacceptableEvidence(state, root);
		TestBindingFailuresAndReload(state, root);
		TestParallelCertificateLoading(state, root);
		TestParallelCertificateRejections(state, root);
		TestConcurrentSingleLoad(state, root);
		TestInvalidateDuringInflightLoad(state);
		TestLoaderExceptionWakesWaiters(state);
	}
	catch (const std::exception &exception)
	{
		++state.failures;
		std::cerr << "FAILED with exception: " <<
			exception.what() << '\n';
	}

	std::error_code ignored;
	std::filesystem::remove_all(root, ignored);
	if (state.failures == 0)
	{
		std::cout << "Relay plan tests passed\n";
		return 0;
	}
	std::cerr << state.failures << " relay plan test(s) failed\n";
	return 1;
}

} // namespace

#ifdef RPREDA_RELAY_PLAN_TEST_MAIN
int main()
{
	return RunRelayPlanTests();
}
#endif
