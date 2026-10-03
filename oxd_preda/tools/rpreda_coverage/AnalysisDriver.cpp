#include "../../transpiler/transpiler.h"
#include "../../transpiler/relay_protocol/metrics/RelayAnalysisMetrics.h"
#include "../../3rdParty/nlohmann/json.hpp"

#ifdef RPREDA_ENABLE_RUNTIME_TRACE
#include "../../bin/compile_env/include/relay_trace.h"
#endif

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#if defined(_WIN32)
extern "C" __declspec(dllimport) transpiler::ITranspiler *
	CreateTranspilerInstance(const char *options);
#else
extern "C" transpiler::ITranspiler *
	CreateTranspilerInstance(const char *options);
#endif

#ifdef RPREDA_ENABLE_RUNTIME_TRACE
thread_local prlrt::IRelayTraceRuntimeInterface *
	prlrt::g_relayTraceRuntimeInterface = nullptr;
#endif

namespace {

using Json = nlohmann::ordered_json;
using Clock = std::chrono::steady_clock;
using transpiler::relay_protocol::metrics::RelayAnalysisMetricsSnapshot;
using transpiler::relay_protocol::metrics::RelayAnalysisPhase;
using transpiler::relay_protocol::metrics::RelayAnalysisPhaseCount;
using transpiler::relay_protocol::metrics::RelayAnalysisProfiler;

#ifdef RPREDA_ENABLE_ANALYSIS_PROFILING
constexpr bool BuildHasAnalysisProfiling = true;
#else
constexpr bool BuildHasAnalysisProfiling = false;
#endif
#ifdef RPREDA_ENABLE_RUNTIME_TRACE
constexpr bool BuildHasRuntimeTrace = true;
#else
constexpr bool BuildHasRuntimeTrace = false;
#endif
#ifdef RPREDA_ENABLE_RUNTIME_OPTIMIZATION
constexpr bool BuildHasRuntimeOptimization = true;
#else
constexpr bool BuildHasRuntimeOptimization = false;
#endif
#ifdef RPREDA_ENABLE_Z3
constexpr bool BuildHasZ3 = true;
#else
constexpr bool BuildHasZ3 = false;
#endif
#ifdef RPREDA_ENABLE_BOUND_RELAY_MANIFEST
constexpr bool BuildHasBoundManifest = true;
#else
constexpr bool BuildHasBoundManifest = false;
#endif

struct Options
{
	std::filesystem::path source;
	std::filesystem::path manifest;
	std::filesystem::path metrics;
	std::filesystem::path generatedCpp;
	std::string dapp = "RPredaScalability";
	std::string analysisMode = "full";
	bool profiling = true;
};

class TranspilerDeleter
{
public:
	void operator()(transpiler::ITranspiler *instance) const noexcept
	{
		if (instance != nullptr)
			instance->Release();
	}
};

using TranspilerPtr =
	std::unique_ptr<transpiler::ITranspiler, TranspilerDeleter>;

class EmptyContractSymbolDatabase final
	: public transpiler::IContractSymbolDatabase
{
public:
	transpiler::IContractSymbols *GetContractSymbols(
		const char *) const override
	{
		return nullptr;
	}

	bool ContractExists(const char *, const char *) const override
	{
		return false;
	}
};

uint64_t ElapsedNs(const Clock::time_point &start) noexcept
{
	const auto elapsed =
		std::chrono::duration_cast<std::chrono::nanoseconds>(
			Clock::now() - start)
			.count();
	return elapsed > 0 ? static_cast<uint64_t>(elapsed) : 0;
}

std::string ReadFile(const std::filesystem::path &path)
{
	std::ifstream input(path, std::ios::binary);
	if (!input)
		throw std::runtime_error("cannot open source file: " + path.string());
	return std::string(
		std::istreambuf_iterator<char>(input),
		std::istreambuf_iterator<char>());
}

void WriteFileAtomically(
	const std::filesystem::path &path,
	const std::string &contents)
{
	if (!path.parent_path().empty())
		std::filesystem::create_directories(path.parent_path());
	const std::filesystem::path temporary = path.string() + ".tmp";
	std::error_code ignored;
	std::filesystem::remove(temporary, ignored);
	{
		std::ofstream output(
			temporary,
			std::ios::binary | std::ios::trunc);
		if (!output)
			throw std::runtime_error(
				"cannot open output file: " + temporary.string());
		output.write(contents.data(),
			static_cast<std::streamsize>(contents.size()));
		if (!output)
			throw std::runtime_error(
				"cannot write output file: " + temporary.string());
	}
	std::filesystem::remove(path, ignored);
	std::filesystem::rename(temporary, path);
}

std::string Diagnostics(const transpiler::ITranspiler &compiler)
{
	std::ostringstream output;
	for (uint32_t index = 0;
		index < compiler.GetNumCompileErrors();
		++index)
	{
		uint32_t line = 0;
		uint32_t column = 0;
		compiler.GetCompileErrorPos(index, line, column);
		if (index != 0)
			output << "; ";
		output << '[' << line << ':' << column << "] "
			<< compiler.GetCompileErrorMsg(index);
	}
	return output.str();
}

Json PhaseMetricsJson(const RelayAnalysisMetricsSnapshot &snapshot)
{
	Json result = Json::object();
	for (size_t index = 0; index < RelayAnalysisPhaseCount; ++index)
	{
		const RelayAnalysisPhase phase =
			static_cast<RelayAnalysisPhase>(index);
		const auto &measurement = snapshot.phases[index];
		result[transpiler::relay_protocol::metrics::ToString(phase)] = Json{
			{"elapsed_time_ns", measurement.elapsedTimeNs},
			{"elapsed_time_ms",
			 static_cast<double>(measurement.elapsedTimeNs) / 1000000.0},
			{"invocations", measurement.invocations},
		};
	}
	return result;
}

void Usage(std::ostream &output)
{
	output
		<< "Usage: rpreda_analysis_driver --source FILE --manifest FILE "
			"--metrics FILE [--dapp NAME] [--generated-cpp FILE] "
			"[--analysis-mode site_scan|cfg_icfg|formula_smt|full] "
			"[--profile on|off]\n";
}

Options ParseOptions(int argc, char **argv)
{
	Options options;
	for (int index = 1; index < argc; ++index)
	{
		const std::string argument = argv[index];
		if (argument == "--help" || argument == "-h")
		{
			Usage(std::cout);
			std::exit(0);
		}
		if (index + 1 >= argc)
			throw std::runtime_error("missing value for " + argument);
		const std::string value = argv[++index];
		if (argument == "--source")
			options.source = value;
		else if (argument == "--manifest")
			options.manifest = value;
		else if (argument == "--metrics")
			options.metrics = value;
		else if (argument == "--generated-cpp")
			options.generatedCpp = value;
		else if (argument == "--dapp")
			options.dapp = value;
		else if (argument == "--analysis-mode")
		{
			if (value != "site_scan" && value != "cfg_icfg" &&
				value != "formula_smt" && value != "full")
				throw std::runtime_error("invalid --analysis-mode: " + value);
			options.analysisMode = value;
		}
		else if (argument == "--profile")
		{
			if (value == "on")
				options.profiling = true;
			else if (value == "off")
				options.profiling = false;
			else
				throw std::runtime_error(
					"--profile must be 'on' or 'off'");
		}
		else
			throw std::runtime_error("unknown option: " + argument);
	}
	if (options.source.empty() || options.manifest.empty() ||
		options.metrics.empty())
	{
		throw std::runtime_error(
			"--source, --manifest, and --metrics are required");
	}
	if (options.dapp.empty())
		throw std::runtime_error("--dapp must not be empty");
	return options;
}

Json BaseMetrics(
	const Options &options,
	const RelayAnalysisMetricsSnapshot &snapshot,
	const std::string &status,
	const std::string &stage,
	const std::string &reason,
	uint64_t parseNs,
	uint64_t precompileNs,
	uint64_t compileNs,
	uint64_t pipelineNs,
	transpiler::ITranspiler *compiler)
{
	Json phaseTimesMs = Json::object();
	for (size_t index = 0; index < RelayAnalysisPhaseCount; ++index)
	{
		const RelayAnalysisPhase phase =
			static_cast<RelayAnalysisPhase>(index);
		phaseTimesMs[
			transpiler::relay_protocol::metrics::ToString(phase)] =
			static_cast<double>(snapshot.phases[index].elapsedTimeNs) /
			1000000.0;
	}
	const double totalAnalysisMs =
		static_cast<double>(
			snapshot.phases[static_cast<size_t>(
				RelayAnalysisPhase::AnalysisTotal)].elapsedTimeNs) /
		1000000.0;
	Json result = {
		{"schema_version", 1},
		{"status", status},
		{"failed_stage", stage},
		{"reason", reason},
		{"source_file_name", options.source.filename().string()},
		{"dapp", options.dapp},
		{"profiling_requested", options.profiling},
		{"profiling_enabled", snapshot.enabled},
		{"build_features", Json{
			{"analysis_profiling", BuildHasAnalysisProfiling},
			{"runtime_trace", BuildHasRuntimeTrace},
			{"runtime_optimization", BuildHasRuntimeOptimization},
			{"z3", BuildHasZ3},
			{"bound_manifest", BuildHasBoundManifest},
		}},
		{"transpiler_version",
		 compiler == nullptr || compiler->GetVersion() == nullptr
			 ? std::string()
			 : std::string(compiler->GetVersion())},
		{"analysis_mode", options.analysisMode},
		{"driver_timings", Json{
			{"parse_time_ns", parseNs},
			{"precompile_time_ns", precompileNs},
			{"compile_time_ns", compileNs},
			{"pipeline_time_ns", pipelineNs},
		}},
		{"phase_times_ms", std::move(phaseTimesMs)},
		{"total_analysis_ms", totalAnalysisMs},
		{"analysis_phases", PhaseMetricsJson(snapshot)},
	};
	return result;
}

int Run(const Options &options)
{
	const std::string source = ReadFile(options.source);
	RelayAnalysisProfiler::Reset(options.profiling);
	const Clock::time_point pipelineStart = Clock::now();
	const std::string transpilerOptions =
		"disabledebugprint relay-analysis=" + options.analysisMode;
	TranspilerPtr compiler(CreateTranspilerInstance(transpilerOptions.c_str()));
	if (!compiler)
		throw std::runtime_error("CreateTranspilerInstance returned null");

	uint64_t parseNs = 0;
	uint64_t precompileNs = 0;
	uint64_t compileNs = 0;
	std::string status = "Compiled";
	std::string failedStage;
	std::string reason;

	Clock::time_point stageStart = Clock::now();
	const bool parsed = compiler->BuildParseTree(source.c_str());
	parseNs = ElapsedNs(stageStart);
	if (!parsed)
	{
		status = "CompilerRejected";
		failedStage = "BuildParseTree";
		reason = Diagnostics(*compiler);
	}

	bool precompiled = false;
	if (parsed)
	{
		stageStart = Clock::now();
		precompiled = compiler->PreCompile(options.dapp.c_str());
		precompileNs = ElapsedNs(stageStart);
		if (!precompiled)
		{
			status = "CompilerRejected";
			failedStage = "PreCompile";
			reason = Diagnostics(*compiler);
		}
	}

	bool compiled = false;
	if (precompiled)
	{
		EmptyContractSymbolDatabase symbols;
		stageStart = Clock::now();
		compiled = compiler->Compile(options.dapp.c_str(), &symbols);
		compileNs = ElapsedNs(stageStart);
		if (!compiled)
		{
			status = "CompilerRejected";
			failedStage = "Compile";
			reason = Diagnostics(*compiler);
		}
	}

	if (compiled)
	{
		const char *manifest = compiler->GetRelayProtocolJson();
		const char *generatedCpp = compiler->GetOutput();
		if (manifest == nullptr || manifest[0] == '\0')
			throw std::runtime_error("compiler produced an empty relay manifest");
		if (generatedCpp == nullptr)
			throw std::runtime_error("compiler produced a null C++ output");
		WriteFileAtomically(options.manifest, manifest);
		if (!options.generatedCpp.empty())
			WriteFileAtomically(options.generatedCpp, generatedCpp);
	}

	const RelayAnalysisMetricsSnapshot snapshot =
		RelayAnalysisProfiler::Snapshot();
	Json metrics = BaseMetrics(
		options,
		snapshot,
		status,
		failedStage,
		reason,
		parseNs,
		precompileNs,
		compileNs,
		ElapsedNs(pipelineStart),
		compiler.get());
	if (compiled)
	{
		metrics["artifacts"] = Json{
			{"manifest_size_bytes",
			 static_cast<uint64_t>(
				 std::filesystem::file_size(options.manifest))},
			{"generated_cpp_size_bytes",
			 static_cast<uint64_t>(std::string(compiler->GetOutput()).size())},
		};
	}
	WriteFileAtomically(options.metrics, metrics.dump(2));
	return compiled ? 0 : 2;
}

} // namespace

int main(int argc, char **argv)
{
	try
	{
		return Run(ParseOptions(argc, argv));
	}
	catch (const std::exception &error)
	{
		std::cerr << "rpreda_analysis_driver: " << error.what() << '\n';
		return 1;
	}
}
