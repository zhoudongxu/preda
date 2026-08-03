#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "../transpiler.h"
#include "../relay_protocol/analysis/RelaySummaryBuilder.h"
#include "../relay_protocol/cfg/PredaCallGraphAnalysis.h"
#include "../relay_protocol/cfg/PredaEffectAnalysis.h"
#include "../../3rdParty/nlohmann/json.hpp"

#ifdef RPREDA_ENABLE_RUNTIME_TRACE
#include "../../bin/compile_env/include/relay_trace.h"
#endif

#ifdef RPREDA_ENABLE_Z3
#include "../relay_protocol/refinement/solver/RelayProofRunner.h"
#include "../relay_protocol/refinement/solver/z3/Z3RelaySolver.h"
#endif

#if defined(_WIN32)
extern "C" __declspec(dllimport) transpiler::ITranspiler* CreateTranspilerInstance(const char* options);
#else
extern "C" transpiler::ITranspiler* CreateTranspilerInstance(const char* options);
#endif

#ifdef RPREDA_ENABLE_RUNTIME_TRACE
thread_local prlrt::IRelayTraceRuntimeInterface*
	prlrt::g_relayTraceRuntimeInterface = nullptr;
#endif

namespace {

using Json = nlohmann::json;

class TestFailure : public std::runtime_error
{
public:
	explicit TestFailure(const std::string& message)
		: std::runtime_error(message)
	{
	}
};

void Check(bool condition, const char* expression, const char* file, int line, const std::string& detail = {})
{
	if (condition)
		return;

	std::ostringstream message;
	message << file << ':' << line << ": CHECK(" << expression << ") failed";
	if (!detail.empty())
		message << ": " << detail;
	throw TestFailure(message.str());
}

#define CHECK(condition) Check(static_cast<bool>(condition), #condition, __FILE__, __LINE__)
#define CHECK_DETAIL(condition, detail) Check(static_cast<bool>(condition), #condition, __FILE__, __LINE__, (detail))

struct TranspilerDeleter
{
	void operator()(transpiler::ITranspiler* instance) const
	{
		if (instance != nullptr)
			instance->Release();
	}
};

using TranspilerPtr = std::unique_ptr<transpiler::ITranspiler, TranspilerDeleter>;

class EmptyContractSymbolDatabase : public transpiler::IContractSymbolDatabase
{
public:
	transpiler::IContractSymbols* GetContractSymbols(const char*) const override
	{
		return nullptr;
	}

	bool ContractExists(const char*, const char*) const override
	{
		return false;
	}
};

struct CompileResult
{
	Json manifest;
	std::string generatedCpp;
};

std::string ReadFile(const std::string& path)
{
	std::ifstream input(path);
	CHECK_DETAIL(input.is_open(), "cannot open fixture " + path);
	return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::string CompileDiagnostics(const transpiler::ITranspiler& compiler)
{
	std::ostringstream output;
	for (uint32_t i = 0; i < compiler.GetNumCompileErrors(); ++i)
	{
		uint32_t line = 0;
		uint32_t column = 0;
		compiler.GetCompileErrorPos(i, line, column);
		output << " [" << line << ':' << column << "] " << compiler.GetCompileErrorMsg(i);
	}
	return output.str();
}

CompileResult CompileFixture(const std::string& fixtureDirectory, const std::string& fileName)
{
	const std::string path = fixtureDirectory + '/' + fileName;
	const std::string source = ReadFile(path);

	TranspilerPtr compiler(CreateTranspilerInstance(nullptr));
	CHECK_DETAIL(compiler != nullptr, "CreateTranspilerInstance returned null");
	const bool parsed = compiler->BuildParseTree(source.c_str());
	CHECK_DETAIL(parsed, path + CompileDiagnostics(*compiler));
	const bool precompiled = compiler->PreCompile("RelayProtocolTests");
	CHECK_DETAIL(precompiled, path + CompileDiagnostics(*compiler));

	EmptyContractSymbolDatabase symbols;
	const bool compiled = compiler->Compile("RelayProtocolTests", &symbols);
	CHECK_DETAIL(compiled, path + CompileDiagnostics(*compiler));

	const char* manifestText = compiler->GetRelayProtocolJson();
	CHECK_DETAIL(manifestText != nullptr, path + ": GetRelayProtocolJson returned null");
	CHECK_DETAIL(manifestText[0] != '\0', path + ": GetRelayProtocolJson returned an empty document");

	CompileResult result;
	try
	{
		result.manifest = Json::parse(manifestText);
	}
	catch (const std::exception& error)
	{
		throw TestFailure(path + ": invalid relay protocol JSON: " + error.what());
	}

	const char* generatedCpp = compiler->GetOutput();
	CHECK_DETAIL(generatedCpp != nullptr, path + ": GetOutput returned null");
	result.generatedCpp = generatedCpp;
	return result;
}

const Json& RequireField(const Json& object, const char* field)
{
	CHECK_DETAIL(object.is_object(), std::string("expected object while reading field ") + field);
	const auto iterator = object.find(field);
	CHECK_DETAIL(iterator != object.end(), std::string("missing JSON field ") + field);
	return *iterator;
}

const Json& RequireArray(const Json& object, const char* field)
{
	const Json& value = RequireField(object, field);
	CHECK_DETAIL(value.is_array(), std::string("field is not an array: ") + field);
	return value;
}

std::string RequireString(const Json& object, const char* field)
{
	const Json& value = RequireField(object, field);
	CHECK_DETAIL(value.is_string(), std::string("field is not a string: ") + field);
	return value.get<std::string>();
}

std::string Compact(std::string text)
{
	text.erase(
		std::remove_if(
			text.begin(),
			text.end(),
			[](unsigned char character) { return std::isspace(character) != 0; }),
		text.end());
	return text;
}

uint64_t Fnv1a64(const std::string& bytes)
{
	uint64_t hash = UINT64_C(14695981039346656037);
	for (unsigned char byte : bytes)
	{
		hash ^= byte;
		hash *= UINT64_C(1099511628211);
	}
	return hash;
}

void CheckGeneratedCppGolden(
	const CompileResult& result,
	size_t expectedSize,
	uint64_t expectedHash)
{
#ifdef RPREDA_ENABLE_RUNTIME_TRACE
	(void)expectedSize;
	(void)expectedHash;
	const std::string& generated = result.generatedCpp;
	const size_t legacyFactory =
		generated.find("_CreateInstance(prlrt::IRuntimeInterface *pInterface");
	const size_t traceAbi =
		generated.find("_RPredaRuntimeTraceAbiVersion() { return 1; }");
	const size_t traceFactory =
		generated.find("_CreateInstance_RPredaTraceV1(");
	CHECK(legacyFactory != std::string::npos);
	CHECK(traceAbi != std::string::npos);
	CHECK(traceFactory != std::string::npos);
	const size_t wasmGuard =
		generated.rfind("#if defined(__wasm32__)", legacyFactory);
	const size_t nativeBranch = generated.find("#else", legacyFactory);
	const size_t factoryEnd = generated.find("#endif", traceFactory);
	CHECK(wasmGuard != std::string::npos);
	CHECK(nativeBranch != std::string::npos);
	CHECK(factoryEnd != std::string::npos);
	CHECK(wasmGuard < legacyFactory);
	CHECK(legacyFactory < nativeBranch);
	CHECK(nativeBranch < traceAbi);
	CHECK(traceAbi < traceFactory);
	CHECK(traceFactory < factoryEnd);
	CHECK(
		generated.find(
			"prlrt::g_relayTraceRuntimeInterface = pTraceInterface;",
			traceFactory) < factoryEnd);
#else
	std::ostringstream detail;
	detail << "generated C++ changed: size=" << result.generatedCpp.size()
		<< ", fnv1a64=0x" << std::hex << Fnv1a64(result.generatedCpp);
	CHECK_DETAIL(result.generatedCpp.size() == expectedSize, detail.str());
	CHECK_DETAIL(Fnv1a64(result.generatedCpp) == expectedHash, detail.str());
#endif
}

void CheckGeneratedRelayCall(
	const CompileResult& result,
	const std::string& stockCall,
	const std::string& tracedCall)
{
#ifdef RPREDA_ENABLE_RUNTIME_TRACE
	CHECK(result.generatedCpp.find(tracedCall) != std::string::npos);
#else
	(void)tracedCall;
	CHECK(result.generatedCpp.find(stockCall) != std::string::npos);
#endif
}

bool EndsWith(const std::string& text, const std::string& suffix)
{
	return text.size() >= suffix.size()
		&& text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool HasPositiveLine(const Json& value)
{
	if (!value.is_object())
		return false;

	for (auto iterator = value.begin(); iterator != value.end(); ++iterator)
	{
		std::string key = iterator.key();
		std::transform(
			key.begin(),
			key.end(),
			key.begin(),
			[](unsigned char character) { return static_cast<char>(std::tolower(character)); });
		if (key.find("line") != std::string::npos
			&& iterator.value().is_number_integer()
			&& iterator.value().get<int64_t>() > 0)
			return true;
	}
	return false;
}

bool ContainsNodeKind(const Json& value, const std::string& expectedKind)
{
	if (value.is_object())
	{
		const auto kind = value.find("kind");
		if (kind != value.end() && kind->is_string() && kind->get<std::string>() == expectedKind)
			return true;
		for (auto iterator = value.begin(); iterator != value.end(); ++iterator)
			if (ContainsNodeKind(iterator.value(), expectedKind))
				return true;
	}
	else if (value.is_array())
	{
		for (const Json& element : value)
			if (ContainsNodeKind(element, expectedKind))
				return true;
	}
	return false;
}

const Json* FindExpressionKind(const Json& value, const std::string& expectedKind)
{
	if (value.is_object())
	{
		const auto kind = value.find("kind");
		if (kind != value.end() && kind->is_string() && kind->get<std::string>() == expectedKind)
			return &value;
		for (auto iterator = value.begin(); iterator != value.end(); ++iterator)
			if (const Json* found = FindExpressionKind(iterator.value(), expectedKind))
				return found;
	}
	else if (value.is_array())
	{
		for (const Json& element : value)
			if (const Json* found = FindExpressionKind(element, expectedKind))
				return found;
	}
	return nullptr;
}

const Json* FindSiteByTargetText(const Json& manifest, const std::string& targetText)
{
	for (const Json& site : RequireArray(manifest, "relay_sites"))
	{
		const Json& target = RequireField(site, "target");
		const auto text = target.find("text");
		if (text != target.end()
			&& text->is_string()
			&& Compact(text->get<std::string>()).find(Compact(targetText)) != std::string::npos)
			return &site;
	}
	return nullptr;
}

const Json* FindHandlerById(const Json& manifest, const Json& id)
{
	for (const Json& handler : RequireArray(manifest, "handlers"))
		if (RequireField(handler, "id") == id)
			return &handler;
	return nullptr;
}

const Json* FindFunctionByName(const Json& manifest, const std::string& functionName)
{
	for (const Json& function : RequireArray(manifest, "functions"))
		if (RequireString(function, "function") == functionName)
			return &function;
	return nullptr;
}

const Json& RequireControlFlow(const Json& manifest)
{
	const Json& controlFlow = RequireField(manifest, "control_flow");
	CHECK(controlFlow.is_object());
	CHECK(RequireField(controlFlow, "extension_schema_version") == 1);
	return controlFlow;
}

const Json& RequireParallelFunction(
	const Json& manifest,
	const std::string& functionName)
{
	std::string functionId;
	for (const Json& site : RequireArray(manifest, "relay_sites"))
		if (RequireString(site, "source_function") == functionName)
			functionId = RequireString(site, "source_function_id");
	CHECK_DETAIL(!functionId.empty(), "missing relay function " + functionName);
	const Json& certificate = RequireField(manifest, "parallel_certificate");
	CHECK(RequireField(certificate, "extension_schema_version") == 1);
	for (const Json& function : RequireArray(certificate, "functions"))
		if (RequireString(function, "source_function_id") == functionId)
			return function;
	throw TestFailure("missing parallel certificate for " + functionName);
}

void CheckConstant(
	const Json& expression,
	uint64_t expected,
	const std::string& detail)
{
	CHECK_DETAIL(RequireString(expression, "kind") == "constant", detail);
	CHECK_DETAIL(RequireField(expression, "value") == expected, detail);
}

void CheckUniqueEvidence(const Json& bound, const std::string& detail)
{
	std::set<std::string> seen;
	for (const Json& value : RequireArray(bound, "supporting_cfg_fact_ids"))
	{
		CHECK_DETAIL(value.is_string(), detail + " evidence is not a string");
		CHECK_DETAIL(
			seen.insert(value.get<std::string>()).second,
			detail + " has duplicate evidence");
	}
}

const Json* FindCFGFunctionByName(
	const Json& controlFlow,
	const std::string& functionName,
	bool generatedRelayLambda = false)
{
	for (const Json& function : RequireArray(controlFlow, "functions"))
	{
		if (RequireString(function, "function") == functionName &&
			RequireField(function, "generated_relay_lambda") ==
				generatedRelayLambda)
		{
			return &function;
		}
	}
	return nullptr;
}

const Json* FindFunctionGraphAnalysis(
	const Json& controlFlow,
	const std::string& functionId)
{
	const Json& icfg = RequireField(controlFlow, "relay_icfg");
	for (const Json& analysis : RequireArray(icfg, "function_analyses"))
		if (RequireString(analysis, "function_id") == functionId)
			return &analysis;
	return nullptr;
}

bool CFGHasNodeKind(const Json& function, const std::string& kind)
{
	for (const Json& node : RequireArray(function, "nodes"))
		if (RequireString(node, "kind") == kind)
			return true;
	return false;
}

bool CFGHasEdgeKind(const Json& function, const std::string& kind)
{
	for (const Json& edge : RequireArray(function, "edges"))
		if (RequireString(edge, "kind") == kind)
			return true;
	return false;
}

bool JsonArrayContains(const Json& values, const std::string& expected)
{
	CHECK(values.is_array());
	for (const Json& value : values)
		if (value.is_string() && value.get<std::string>() == expected)
			return true;
	return false;
}

const Json& RequireFunctionSummary(
	const Json& manifest,
	const std::string& functionName)
{
	const Json* function = FindFunctionByName(manifest, functionName);
	CHECK_DETAIL(
		function != nullptr,
		"missing function protocol for " + functionName);
	const Json& summary = RequireField(*function, "summary");
	CHECK_DETAIL(
		summary.is_object(),
		"function summary is not an object for " + functionName);
	return summary;
}

const Json& RequireRefinement(const Json& manifest)
{
	const Json& refinement = RequireField(manifest, "refinement");
	CHECK(refinement.is_object());
	RequireArray(refinement, "symbols");
	RequireArray(refinement, "constraints");
	RequireArray(refinement, "proof_obligations");
	return refinement;
}

const Json* FindRefinementItem(
	const Json& items,
	const std::string& kind,
	const std::string& relaySiteId = std::string(),
	const std::string& sourceFunctionId = std::string(),
	int64_t argumentIndex = -2)
{
	CHECK(items.is_array());
	for (const Json& item : items)
	{
		if (RequireString(item, "kind") != kind)
			continue;
		if (!relaySiteId.empty()
			&& RequireString(item, "relay_site_id") != relaySiteId)
			continue;
		if (!sourceFunctionId.empty()
			&& RequireString(item, "source_function_id") != sourceFunctionId)
			continue;
		if (argumentIndex != -2)
		{
			const Json& actualIndex =
				RequireField(item, "argument_index");
			if (!actualIndex.is_number_integer()
				|| actualIndex.get<int64_t>() != argumentIndex)
				continue;
		}
		return &item;
	}
	return nullptr;
}

const Json* FindRefinementSymbol(
	const Json& refinement,
	const std::string& kind,
	const std::string& sourceName = std::string(),
	const std::string& relaySiteId = std::string(),
	const std::string& sourceFunctionId = std::string(),
	int64_t argumentIndex = -2)
{
	for (const Json& symbol : RequireArray(refinement, "symbols"))
	{
		if (RequireString(symbol, "kind") != kind)
			continue;
		if (!sourceName.empty()
			&& RequireString(symbol, "source_name") != sourceName)
			continue;
		if (!relaySiteId.empty()
			&& RequireString(symbol, "relay_site_id") != relaySiteId)
			continue;
		if (!sourceFunctionId.empty()
			&& RequireString(
				symbol,
				"source_function_id") != sourceFunctionId)
			continue;
		if (argumentIndex != -2)
		{
			const Json& actualIndex =
				RequireField(symbol, "argument_index");
			if (!actualIndex.is_number_integer()
				|| actualIndex.get<int64_t>() != argumentIndex)
				continue;
		}
		return &symbol;
	}
	return nullptr;
}

const Json* FindConstraint(
	const Json& refinement,
	const std::string& kind,
	const std::string& relaySiteId = std::string(),
	const std::string& sourceFunctionId = std::string(),
	int64_t argumentIndex = -2)
{
	return FindRefinementItem(
		RequireArray(refinement, "constraints"),
		kind,
		relaySiteId,
		sourceFunctionId,
		argumentIndex);
}

const Json* FindProofObligation(
	const Json& refinement,
	const std::string& kind,
	const std::string& relaySiteId = std::string(),
	const std::string& sourceFunctionId = std::string(),
	int64_t argumentIndex = -2)
{
	return FindRefinementItem(
		RequireArray(refinement, "proof_obligations"),
		kind,
		relaySiteId,
		sourceFunctionId,
		argumentIndex);
}

#if defined(RPREDA_ENABLE_Z3) || \
	defined(RPREDA_ENABLE_BOUND_RELAY_MANIFEST)
const Json& RequireSolverResult(const Json& obligation)
{
	const Json& result = RequireField(obligation, "solver_result");
	CHECK(result.is_object());
	RequireString(result, "backend");
	RequireString(result, "status");
	CHECK(
		RequireField(result, "elapsed_time_ms").is_number_unsigned()
		|| RequireField(result, "elapsed_time_ms").is_number_integer());
	RequireArray(result, "assumption_constraint_ids");
	RequireString(result, "reason");
	RequireArray(result, "projected_counterexample");
	return result;
}

void CheckSolverStatus(
	const Json* obligation,
	const std::string& expectedStatus,
	const std::string& detail)
{
	CHECK_DETAIL(obligation != nullptr, "missing obligation: " + detail);
	const Json& result = RequireSolverResult(*obligation);
	CHECK_DETAIL(
		RequireString(result, "status") == expectedStatus,
		"unexpected solver status for " + detail + ": "
			+ result.dump());
}
#endif

const Json* FindFormulaNode(
	const Json& formula,
	const std::string& kind,
	const std::string& operatorText = std::string())
{
	if (!formula.is_object())
		return nullptr;
	if (RequireString(formula, "kind") == kind
		&& (operatorText.empty()
			|| RequireString(formula, "operator") == operatorText))
		return &formula;
	for (const Json& child : RequireArray(formula, "children"))
		if (const Json* found =
			FindFormulaNode(child, kind, operatorText))
			return found;
	return nullptr;
}

const Json& RequireBinaryChild(
	const Json& formula,
	const std::string& operatorText,
	size_t childIndex)
{
	CHECK_DETAIL(
		RequireString(formula, "kind") == "Binary",
		"expected Binary formula: " + formula.dump());
	CHECK_DETAIL(
		RequireString(formula, "operator") == operatorText,
		"expected operator " + operatorText + ": " + formula.dump());
	const Json& children = RequireArray(formula, "children");
	CHECK_DETAIL(
		children.size() == 2,
		"binary formula does not have two children: " + formula.dump());
	CHECK(childIndex < children.size());
	return children[childIndex];
}

const Json& RequireRelationSourceExpression(
	const Json& constraint)
{
	const Json& implication = RequireField(constraint, "formula");
	const Json& equality =
		RequireBinaryChild(implication, "implies", 1);
	return RequireBinaryChild(equality, "==", 1);
}

const Json& RequireCountExpression(
	const Json& constraint,
	const std::string& expectedOperator = "==")
{
	return RequireBinaryChild(
		RequireField(constraint, "formula"),
		expectedOperator,
		1);
}

const Json& UnwrapGroups(const Json& formula)
{
	const Json* current = &formula;
	while (RequireString(*current, "kind") == "Group")
	{
		const Json& children = RequireArray(*current, "children");
		CHECK(children.size() == 1);
		current = &children.front();
	}
	return *current;
}

void CheckFormulaSort(
	const Json& formula,
	const std::string& expectedKind,
	int64_t expectedBitWidth = -1)
{
	const Json& sort = RequireField(formula, "sort");
	CHECK(sort.is_object());
	CHECK(RequireString(sort, "kind") == expectedKind);
	if (expectedBitWidth >= 0)
	{
		const Json& bitWidth = RequireField(sort, "bit_width");
		CHECK(
			bitWidth.is_number_integer()
			|| bitWidth.is_number_unsigned());
		CHECK(bitWidth.get<int64_t>() == expectedBitWidth);
	}
}

void CheckObligationStatus(
	const Json* obligation,
	const std::string& expectedStatus,
	const std::string& detail)
{
	CHECK_DETAIL(obligation != nullptr, "missing obligation: " + detail);
	CHECK_DETAIL(
		RequireString(*obligation, "status") == expectedStatus,
		"unexpected obligation status for " + detail + ": "
			+ obligation->dump());
	if (expectedStatus == "Unsupported")
		CHECK_DETAIL(
			!RequireString(*obligation, "reason").empty(),
			"unsupported obligation has no reason: " + detail);
}

void CollectFormulaSymbolIds(
	const Json& formula,
	std::set<std::string>& symbolIds)
{
	if (!formula.is_object())
		return;
	if (RequireString(formula, "kind") == "Symbol")
		symbolIds.insert(RequireString(formula, "symbol_id"));
	for (const Json& child : RequireArray(formula, "children"))
		CollectFormulaSymbolIds(child, symbolIds);
}

void CheckRefinementIntegrity(const Json& manifest)
{
	const Json& refinement = RequireRefinement(manifest);
	const Json& symbols = RequireArray(refinement, "symbols");
	const Json& constraints = RequireArray(refinement, "constraints");
	const Json& obligations =
		RequireArray(refinement, "proof_obligations");

	std::set<std::string> symbolIds;
	std::set<std::string> allIds;
	for (const Json& symbol : symbols)
	{
		const std::string id = RequireString(symbol, "id");
		CHECK(!id.empty());
		CHECK_DETAIL(
			allIds.insert(id).second,
			"duplicate refinement ID: " + id);
		symbolIds.insert(id);
		RequireString(symbol, "kind");
		RequireString(symbol, "preda_type");
		CHECK(RequireField(symbol, "sort").is_object());
		RequireArray(symbol, "dependencies");
		RequireString(symbol, "availability");
		CHECK(RequireField(
			symbol,
			"admission_time_evaluable").is_boolean());
	}

	const std::set<std::string> allowedConstraintKinds = {
		"RelayTargetRelation",
		"RelayArgumentRelation",
		"RelayGuardNecessity",
		"RelayGuardEquivalence",
		"RelayCountEquality",
		"RelayCountNonNegative",
		"RelayCountUpperBound",
	};
	const std::set<std::string> allowedObligationKinds = {
		"RelayTargetEquality",
		"RelayArgumentEquality",
		"RelayGuardNecessity",
		"RelayGuardEquivalence",
		"RelayCountEquality",
		"RelayCountUpperBound",
		"TargetNonAliasCandidate",
		"RelayMutualExclusion",
		"RelayTargetIndependence",
		"BooleanRefinement",
		"Unknown",
	};

	for (const Json& constraint : constraints)
	{
		const std::string id = RequireString(constraint, "id");
		CHECK(!id.empty());
		CHECK_DETAIL(
			allIds.insert(id).second,
			"duplicate refinement ID: " + id);
		const std::string kind =
			RequireString(constraint, "kind");
		CHECK_DETAIL(
			allowedConstraintKinds.count(kind) == 1,
			"unknown refinement constraint kind: " + kind);
		CHECK(kind.find("Order") == std::string::npos);
#if defined(RPREDA_ENABLE_Z3) || \
	defined(RPREDA_ENABLE_BOUND_RELAY_MANIFEST)
		const std::string role =
			RequireString(constraint, "role");
		CHECK(
			role == "SemanticDefinition"
			|| role == "SolverAssumption"
			|| role == "SolverGoal");
#else
		CHECK(constraint.find("role") == constraint.end());
#endif
		std::set<std::string> referencedSymbols;
		CollectFormulaSymbolIds(
			RequireField(constraint, "formula"),
			referencedSymbols);
		for (const std::string& symbolId : referencedSymbols)
			CHECK_DETAIL(
				symbolIds.count(symbolId) == 1,
				"constraint references unknown symbol: " + symbolId);
	}

	for (const Json& obligation : obligations)
	{
		const std::string id = RequireString(obligation, "id");
		CHECK(!id.empty());
		CHECK_DETAIL(
			allIds.insert(id).second,
			"duplicate refinement ID: " + id);
		const std::string kind =
			RequireString(obligation, "kind");
		CHECK_DETAIL(
			allowedObligationKinds.count(kind) == 1,
			"unknown refinement proof-obligation kind: " + kind);
		CHECK(kind.find("Order") == std::string::npos);
		const std::string status =
			RequireString(obligation, "status");
		CHECK(status == "Generated" || status == "Unsupported");
		CHECK(status != "Proved");
#if defined(RPREDA_ENABLE_Z3) || \
	defined(RPREDA_ENABLE_BOUND_RELAY_MANIFEST)
		const std::string proofRole =
			RequireString(obligation, "proof_role");
		CHECK(
			proofRole == "EstablishedByConstruction"
			|| proofRole == "SolverGoal");
		const Json& solverResult =
			RequireSolverResult(obligation);
#if defined(RPREDA_ENABLE_BOUND_RELAY_MANIFEST) && \
	!defined(RPREDA_ENABLE_Z3)
		const std::string solverStatus =
			RequireString(solverResult, "status");
		if (status == "Unsupported")
		{
			CHECK(solverStatus == "Unsupported");
		}
		else if (proofRole == "EstablishedByConstruction")
		{
			CHECK(
				solverStatus ==
				"EstablishedByConstruction");
		}
		else
		{
			CHECK(solverStatus == "NotRun");
		}
#endif
#else
		// Solver metadata is an optional schema extension. A build with the
		// feature disabled must retain the schema-v4 formula artifact without
		// pretending that a solver query ran.
		CHECK(
			obligation.find("solver_result")
			== obligation.end());
		CHECK(
			obligation.find("proof_role")
			== obligation.end());
#endif
		for (const Json& constraintId :
			RequireArray(obligation, "constraint_ids"))
		{
			CHECK(constraintId.is_string());
			CHECK_DETAIL(
				allIds.count(constraintId.get<std::string>()) == 1,
				"proof obligation references unknown constraint: "
					+ constraintId.get<std::string>());
		}
		std::set<std::string> referencedSymbols;
		CollectFormulaSymbolIds(
			RequireField(obligation, "goal"),
			referencedSymbols);
		for (const std::string& symbolId : referencedSymbols)
			CHECK_DETAIL(
				symbolIds.count(symbolId) == 1,
				"proof obligation references unknown symbol: "
					+ symbolId);
	}

	for (const Json& constraint : constraints)
	{
		if (RequireString(constraint, "kind")
			!= "RelayCountNonNegative")
			continue;
		const Json& zero = RequireBinaryChild(
			RequireField(constraint, "formula"),
			">=",
			1);
		CHECK(RequireString(zero, "kind") == "IntLiteral");
		CHECK(RequireString(zero, "literal_value") == "0");
	}
}

void CheckConstantExpression(
	const Json& expression,
	uint64_t expectedValue,
	const std::string& field)
{
	CHECK_DETAIL(
		RequireString(expression, "kind") == "constant",
		field + " is not a constant expression: " + expression.dump());
	const Json& value = RequireField(expression, "value");
	CHECK_DETAIL(
		value.is_number_unsigned(),
		field + " constant value is not unsigned: " + expression.dump());
	CHECK_DETAIL(
		value.get<uint64_t>() == expectedValue,
		field + " has an unexpected value: " + expression.dump());
}

void CheckUnknownExpression(
	const Json& expression,
	const std::string& field)
{
	CHECK_DETAIL(
		RequireString(expression, "kind") == "unknown",
		field + " is not Unknown: " + expression.dump());
	CHECK_DETAIL(
		!RequireString(expression, "reason").empty(),
		field + " Unknown expression has no reason");
}

const Json& RequireSummaryArray(
	const Json& summary,
	const char* field)
{
	return RequireArray(summary, field);
}

void CheckSummaryBoolean(
	const Json& summary,
	const char* field,
	bool expected)
{
	const Json& value = RequireField(summary, field);
	CHECK_DETAIL(
		value.is_boolean(),
		std::string("summary field is not boolean: ") + field);
	CHECK_DETAIL(
		value.get<bool>() == expected,
		std::string("unexpected summary boolean: ") + field);
}

void CheckSummaryScopeKinds(
	const Json& summary,
	const Json& expected)
{
	CHECK(RequireSummaryArray(summary, "target_scope_kinds") == expected);
}

void CheckSummaryFanout(
	const Json& summary,
	const std::string& expectedKind)
{
	const Json& fanout = RequireSummaryArray(summary, "fanout");
	CHECK_DETAIL(
		std::find(fanout.begin(), fanout.end(), expectedKind) != fanout.end(),
		"missing summary fanout kind " + expectedKind + ": " + fanout.dump());
}

void CheckSummaryOrdering(
	const Json& summary,
	const std::string& expectedStatus)
{
	const Json& ordering = RequireField(summary, "ordering");
	CHECK(ordering.is_object());
	CHECK(RequireString(ordering, "status") == expectedStatus);
	CHECK(RequireArray(ordering, "must_precede").empty());
	const Json& listenerOrderUsed =
		RequireField(ordering, "listener_order_used_as_proof");
	CHECK(listenerOrderUsed.is_boolean());
	CHECK(!listenerOrderUsed.get<bool>());
	CHECK(!RequireString(ordering, "reason").empty());
}

void CheckDependency(
	const Json& dependency,
	const Json& expectedClasses,
	const std::string& expectedAvailability,
	bool expectedAdmissionTimeEvaluable)
{
	CHECK(dependency.is_object());
	CHECK(
		RequireArray(dependency, "dependencies") ==
		expectedClasses);
	CHECK(
		RequireString(dependency, "earliest_availability") ==
		expectedAvailability);
	const Json& admission =
		RequireField(dependency, "admission_time_evaluable");
	CHECK(admission.is_boolean());
	CHECK(
		admission.get<bool>() ==
		expectedAdmissionTimeEvaluable);
}

void CheckSummaryRequiredFields(const Json& summary)
{
	CHECK(RequireField(summary, "relay_count").is_object());
	CHECK(RequireField(summary, "relay_count_upper_bound").is_object());
	CHECK(RequireField(summary, "max_depth").is_object());
	RequireSummaryArray(summary, "relay_site_set");
	RequireSummaryArray(summary, "target_scope_kinds");
	RequireSummaryArray(summary, "fanout");
	CHECK(RequireField(summary, "targets_known_before_execution").is_boolean());
	CHECK(RequireField(summary, "has_opaque").is_boolean());
	CHECK(
		RequireField(
			summary,
			"has_unmodeled_relay_reachable_call").is_boolean());
	CHECK(RequireField(summary, "ordering").is_object());
	CHECK(RequireField(summary, "analysis_status").is_string());
}

void CheckTopLevel(const CompileResult& result, const std::string& contract)
{
	const Json& manifest = result.manifest;
	CHECK(manifest.is_object());
	CHECK(RequireField(manifest, "schema_version").is_number_unsigned());
#ifdef RPREDA_ENABLE_BOUND_RELAY_MANIFEST
	CHECK(RequireField(manifest, "schema_version").get<uint64_t>() == 5);
#else
	CHECK(RequireField(manifest, "schema_version").get<uint64_t>() == 4);
#endif
	CHECK(RequireString(manifest, "dapp") == "RelayProtocolTests");
	CHECK(RequireString(manifest, "contract") == contract);
	const Json& sites = RequireArray(manifest, "relay_sites");
#ifdef RPREDA_ENABLE_BOUND_RELAY_MANIFEST
	for (size_t siteIndex = 0; siteIndex < sites.size(); ++siteIndex)
	{
		const Json& ordinal = RequireField(sites[siteIndex], "ordinal");
		CHECK(
			ordinal.is_number_unsigned() ||
			ordinal.is_number_integer());
		CHECK(ordinal.get<uint64_t>() == siteIndex);
	}
#else
	for (const Json& site : sites)
		CHECK(site.find("ordinal") == site.end());
#endif
	RequireArray(manifest, "handlers");
	RequireArray(manifest, "edges");
	const Json& functions = RequireArray(manifest, "functions");
	for (const Json& function : functions)
	{
#ifdef RPREDA_ENABLE_BOUND_RELAY_MANIFEST
		CHECK(
			RequireField(
				function,
				"exported_opcode").is_number_integer());
#else
		CHECK(
			function.find("exported_opcode") ==
			function.end());
#endif
		const Json& summary = RequireField(function, "summary");
		CheckSummaryRequiredFields(summary);
	}
	CheckRefinementIntegrity(manifest);
}

void CheckSiteCommon(
	const Json& site,
	const std::string& contract,
	const std::string& sourceFunction,
	const std::string& sourceScope,
	const std::string& relayKind,
	const std::string& targetScope)
{
	CHECK(!RequireField(site, "id").is_null());
	CHECK(EndsWith(RequireString(site, "source_contract"), contract));
	CHECK(RequireString(site, "source_function") == sourceFunction);
	CHECK(!RequireString(site, "source_function_id").empty());
	CHECK(!RequireString(site, "source_function_signature").empty());
	CHECK(RequireField(site, "source_function_overload_index").is_number_unsigned());
	CHECK(RequireString(site, "source_scope") == sourceScope);
	CHECK(RequireString(site, "relay_kind") == relayKind);
	CHECK(RequireString(site, "target_scope") == targetScope);
	CHECK(HasPositiveLine(RequireField(site, "location")));
	CHECK(RequireField(site, "target").is_object());
	CHECK(RequireField(site, "target_dependency").is_object());
	RequireField(site, "target_function");
	const Json& arguments = RequireArray(site, "arguments");
	for (const Json& argument : arguments)
		CHECK(RequireField(argument, "dependency").is_object());
	RequireArray(site, "branches");
	RequireArray(site, "loops");
	CHECK(!RequireField(site, "handler_id").is_null());
}

void CheckResolvedHandler(
	const Json& manifest,
	const Json& site,
	const std::string& expectedKind,
	const std::string& expectedScope)
{
	const Json* handler = FindHandlerById(manifest, RequireField(site, "handler_id"));
	CHECK_DETAIL(handler != nullptr, "site handler_id does not resolve to a handler");
	CHECK(RequireString(*handler, "kind") == expectedKind);
	CHECK(RequireString(*handler, "scope") == expectedScope);
	CHECK(RequireField(*handler, "resolved").is_boolean());
	CHECK(RequireField(*handler, "resolved").get<bool>());
	CHECK(RequireField(*handler, "opcode").is_number_integer());
	CHECK(RequireField(*handler, "opcode").get<int64_t>() >= 0);
	CHECK(HasPositiveLine(RequireField(*handler, "location")));
	RequireArray(*handler, "parameter_types");
}

void TestNamedAddress(const std::string& fixtureDirectory)
{
	const CompileResult result = CompileFixture(fixtureDirectory, "named_address.prd");
	CheckTopLevel(result, "ProtocolNamedAddress");
	CheckGeneratedCppGolden(result, 11536, UINT64_C(0xecb496c05dadef7a));
	CheckGeneratedRelayCall(
		result,
		"prlrt::relay(",
		"prlrt::relay_traced(0, ");

	const Json& sites = RequireArray(result.manifest, "relay_sites");
	CHECK(sites.size() == 1);
	const Json& site = sites.front();
	CheckSiteCommon(site, "ProtocolNamedAddress", "send", "address", "custom_scope", "address");
	CHECK(RequireString(RequireField(site, "target"), "kind") == "identifier");
	CHECK(Compact(RequireString(RequireField(site, "target"), "text")) == "target");
	CHECK(RequireString(site, "target_function") == "receive");
	CheckDependency(
		RequireField(site, "target_dependency"),
		Json::array({ "TransactionArgument" }),
		"AdmissionTime",
		true);

	const Json& arguments = RequireArray(site, "arguments");
	CHECK(arguments.size() == 1);
	CHECK(RequireString(arguments.front(), "type") == "int32");
	const Json& argumentExpression = RequireField(arguments.front(), "expression");
	CHECK(RequireString(argumentExpression, "kind") == "identifier");
	CHECK(RequireString(argumentExpression, "type") == "int32");
	CHECK(Compact(RequireString(argumentExpression, "text")) == "value");
	CheckDependency(
		RequireField(arguments.front(), "dependency"),
		Json::array({ "TransactionArgument" }),
		"AdmissionTime",
		true);

	CheckResolvedHandler(result.manifest, site, "named", "address");
	const Json* handler = FindHandlerById(result.manifest, RequireField(site, "handler_id"));
	CHECK(RequireString(*handler, "name") == "receive");
	CHECK(RequireArray(*handler, "parameter_types") == Json::array({ "int32" }));
#ifdef RPREDA_ENABLE_BOUND_RELAY_MANIFEST
	const std::string targetFunctionId =
		RequireString(*handler, "target_function_id");
	const Json* targetFunction = nullptr;
	for (const Json& function : RequireArray(result.manifest, "functions"))
	{
		if (RequireString(function, "source_function_id") ==
			targetFunctionId)
		{
			targetFunction = &function;
			break;
		}
	}
	CHECK(targetFunction != nullptr);
	CHECK(RequireArray(*targetFunction, "relay_site_ids").empty());
	CHECK(
		RequireField(*targetFunction, "exported_opcode").get<int64_t>() ==
		RequireField(*handler, "opcode").get<int64_t>());
#endif
	CHECK(RequireArray(result.manifest, "edges").size() == 1);
	CHECK(ContainsNodeKind(RequireArray(result.manifest, "functions"), "Sequence"));
	CHECK(ContainsNodeKind(RequireArray(result.manifest, "functions"), "Emit"));
	CHECK(ContainsNodeKind(RequireArray(result.manifest, "functions"), "Call"));
	CHECK(ContainsNodeKind(RequireArray(result.manifest, "functions"), "End"));

	const Json& summary =
		RequireFunctionSummary(result.manifest, "send");
	CheckConstantExpression(
		RequireField(summary, "relay_count"),
		1,
		"named relay_count");
	CheckConstantExpression(
		RequireField(summary, "relay_count_upper_bound"),
		1,
		"named relay_count_upper_bound");
	CheckConstantExpression(
		RequireField(summary, "max_depth"),
		1,
		"named max_depth");
	CHECK(
		RequireSummaryArray(summary, "relay_site_set") ==
		Json::array({ RequireString(site, "id") }));
	CheckSummaryScopeKinds(summary, Json::array({ "address" }));
	CheckSummaryBoolean(
		summary,
		"targets_known_before_execution",
		true);
	CheckSummaryBoolean(summary, "has_opaque", false);
	CheckSummaryFanout(summary, "single_target");
	CheckSummaryOrdering(summary, "trivial");
	CHECK(RequireString(summary, "analysis_status") == "exact");

	const Json& refinement = RequireRefinement(result.manifest);
	const std::string siteId = RequireString(site, "id");
	const Json* targetRelation =
		FindConstraint(refinement, "RelayTargetRelation", siteId);
	CHECK(targetRelation != nullptr);
	const Json& targetFormula =
		RequireRelationSourceExpression(*targetRelation);
	CHECK(RequireString(targetFormula, "kind") == "Symbol");
	CheckFormulaSort(targetFormula, "Address");
	const Json* targetParameter =
		FindRefinementSymbol(
			refinement,
			"SourceFunctionParameter",
			"target");
	CHECK(targetParameter != nullptr);
	CHECK(
		RequireString(targetFormula, "symbol_id") ==
		RequireString(*targetParameter, "id"));
	CHECK(
		RequireArray(*targetParameter, "dependencies") ==
		Json::array({ "TransactionArgument" }));
	CHECK(
		RequireString(*targetParameter, "availability") ==
		"AdmissionTime");
	CheckObligationStatus(
		FindProofObligation(
			refinement,
			"RelayTargetEquality",
			siteId),
		"Generated",
		"named address target equality");
}

void TestLambdaAddress(const std::string& fixtureDirectory)
{
	const CompileResult result = CompileFixture(fixtureDirectory, "lambda_address.prd");
	CheckTopLevel(result, "ProtocolLambdaAddress");
	CheckGeneratedCppGolden(result, 11606, UINT64_C(0x04e76cef5d6c344a));
	CheckGeneratedRelayCall(
		result,
		"prlrt::relay(",
		"prlrt::relay_traced(0, ");

	const Json& sites = RequireArray(result.manifest, "relay_sites");
	CHECK(sites.size() == 1);
	const Json& site = sites.front();
	CheckSiteCommon(site, "ProtocolLambdaAddress", "send", "address", "custom_scope", "address");
	CHECK(RequireString(RequireField(site, "target"), "kind") == "identifier");
	CheckDependency(
		RequireField(site, "target_dependency"),
		Json::array({ "TransactionArgument" }),
		"AdmissionTime",
		true);
	CHECK(RequireArray(site, "arguments").size() == 1);
	CHECK(RequireString(RequireArray(site, "arguments").front(), "type") == "int32");
	CHECK(RequireString(
		RequireField(RequireArray(site, "arguments").front(), "expression"),
		"type") == "int32");
	CheckDependency(
		RequireField(
			RequireArray(site, "arguments").front(),
			"dependency"),
		Json::array({ "TransactionArgument" }),
		"AdmissionTime",
		true);

	CheckResolvedHandler(result.manifest, site, "lambda", "address");
	const Json* handler = FindHandlerById(result.manifest, RequireField(site, "handler_id"));
	CHECK(RequireString(*handler, "name").find("__relaylambda_") == 0);
	CHECK(RequireArray(*handler, "parameter_types") == Json::array({ "int32" }));
#ifdef RPREDA_ENABLE_BOUND_RELAY_MANIFEST
	const std::string targetFunctionId =
		RequireString(*handler, "target_function_id");
	const Json* targetFunction = nullptr;
	for (const Json& function : RequireArray(result.manifest, "functions"))
	{
		if (RequireString(function, "source_function_id") ==
			targetFunctionId)
		{
			targetFunction = &function;
			break;
		}
	}
	CHECK(targetFunction != nullptr);
	CHECK(RequireArray(*targetFunction, "relay_site_ids").empty());
	CHECK(
		RequireField(*targetFunction, "exported_opcode").get<int64_t>() ==
		RequireField(*handler, "opcode").get<int64_t>());
#endif
	CHECK(ContainsNodeKind(RequireArray(result.manifest, "functions"), "Emit"));
}

void TestGlobal(const std::string& fixtureDirectory)
{
	const CompileResult result = CompileFixture(fixtureDirectory, "global.prd");
	CheckTopLevel(result, "ProtocolGlobal");
	CheckGeneratedCppGolden(result, 11093, UINT64_C(0x69d91801a572b556));
	CheckGeneratedRelayCall(
		result,
		"prlrt::relay_global(",
		"prlrt::relay_global_traced(0, ");

	const Json& sites = RequireArray(result.manifest, "relay_sites");
	CHECK(sites.size() == 1);
	const Json& site = sites.front();
	CheckSiteCommon(site, "ProtocolGlobal", "send", "address", "global", "global");
	CHECK(RequireString(site, "target_function") == "receive");
	CHECK(RequireString(RequireField(site, "target"), "kind") == "keyword");
	CHECK(Compact(RequireString(RequireField(site, "target"), "text")) == "global");
	CheckResolvedHandler(result.manifest, site, "named", "global");
}

void TestShards(const std::string& fixtureDirectory)
{
	const CompileResult result = CompileFixture(fixtureDirectory, "shards.prd");
	CheckTopLevel(result, "ProtocolShards");
	CheckGeneratedCppGolden(result, 11117, UINT64_C(0x9883a654003c85b4));
	CheckGeneratedRelayCall(
		result,
		"prlrt::relay_shards(",
		"prlrt::relay_shards_traced(0, ");

	const Json& sites = RequireArray(result.manifest, "relay_sites");
	CHECK(sites.size() == 1);
	const Json& site = sites.front();
	CheckSiteCommon(site, "ProtocolShards", "broadcast", "global", "shards", "shard");
	CHECK(RequireString(site, "target_function") == "receive");
	CHECK(RequireString(RequireField(site, "target"), "kind") == "keyword");
	CHECK(Compact(RequireString(RequireField(site, "target"), "text")) == "shards");
	CheckResolvedHandler(result.manifest, site, "named", "shard");
	CHECK(ContainsNodeKind(RequireArray(result.manifest, "functions"), "Parallel"));

	const Json& summary =
		RequireFunctionSummary(result.manifest, "broadcast");
	CheckConstantExpression(
		RequireField(summary, "relay_count"),
		1,
		"shards relay_count");
	CheckConstantExpression(
		RequireField(summary, "relay_count_upper_bound"),
		1,
		"shards relay_count_upper_bound");
	CheckConstantExpression(
		RequireField(summary, "max_depth"),
		1,
		"shards max_depth");
	CheckSummaryScopeKinds(summary, Json::array({ "shard" }));
	CheckSummaryBoolean(
		summary,
		"targets_known_before_execution",
		true);
	CheckSummaryBoolean(summary, "has_opaque", false);
	CheckSummaryFanout(summary, "all_shards");
	CheckSummaryOrdering(summary, "trivial");
	CHECK(RequireString(summary, "analysis_status") == "exact");
}

void TestIfElse(const std::string& fixtureDirectory)
{
	const CompileResult result = CompileFixture(fixtureDirectory, "if_else.prd");
	CheckTopLevel(result, "ProtocolIfElse");
	CheckGeneratedCppGolden(result, 11917, UINT64_C(0xd1719ecee3d1fbde));

	const Json& sites = RequireArray(result.manifest, "relay_sites");
	CHECK(sites.size() == 2);
	const Json* primary = FindSiteByTargetText(result.manifest, "primary");
	const Json* fallback = FindSiteByTargetText(result.manifest, "fallback");
	CHECK(primary != nullptr);
	CHECK(fallback != nullptr);

	CheckSiteCommon(*primary, "ProtocolIfElse", "send", "address", "custom_scope", "address");
	CheckSiteCommon(*fallback, "ProtocolIfElse", "send", "address", "custom_scope", "address");
	CHECK(RequireArray(*primary, "branches").size() == 1);
	CHECK(RequireArray(*fallback, "branches").size() == 1);
	CHECK(Compact(RequireArray(*primary, "branches").dump()).find("use_primary") != std::string::npos);
	CHECK(Compact(RequireArray(*fallback, "branches").dump()).find("use_primary") != std::string::npos);
	CHECK(RequireField(RequireArray(*primary, "branches").front(), "polarity").get<bool>());
	CHECK(!RequireField(RequireArray(*fallback, "branches").front(), "polarity").get<bool>());
	CHECK(RequireArray(*primary, "branches") != RequireArray(*fallback, "branches"));
	CHECK(RequireArray(*primary, "loops").empty());
	CHECK(RequireArray(*fallback, "loops").empty());
	CHECK(ContainsNodeKind(RequireArray(result.manifest, "functions"), "Branch"));
	CHECK(ContainsNodeKind(RequireArray(result.manifest, "functions"), "Emit"));

	const Json& refinement = RequireRefinement(result.manifest);
	const Json* primaryGuard = FindConstraint(
		refinement,
		"RelayGuardNecessity",
		RequireString(*primary, "id"));
	const Json* fallbackGuard = FindConstraint(
		refinement,
		"RelayGuardNecessity",
		RequireString(*fallback, "id"));
	CHECK(primaryGuard != nullptr);
	CHECK(fallbackGuard != nullptr);
	const Json& primaryPredicate = RequireBinaryChild(
		RequireField(*primaryGuard, "formula"),
		"implies",
		1);
	CHECK(RequireString(primaryPredicate, "kind") == "Symbol");
	CHECK(RequireString(primaryPredicate, "source_text") == "use_primary");
	CheckFormulaSort(primaryPredicate, "Bool");
	const Json& negativePredicate = RequireBinaryChild(
		RequireField(*fallbackGuard, "formula"),
		"implies",
		1);
	CHECK(RequireString(negativePredicate, "kind") == "Unary");
	CHECK(RequireString(negativePredicate, "operator") == "!");
	const Json& negativeChildren =
		RequireArray(negativePredicate, "children");
	CHECK(negativeChildren.size() == 1);
	CHECK(RequireString(negativeChildren.front(), "kind") == "Symbol");
	CHECK(
		RequireString(negativeChildren.front(), "source_text") ==
		"use_primary");
	CheckObligationStatus(
		FindProofObligation(
			refinement,
			"RelayGuardNecessity",
			RequireString(*fallback, "id")),
		"Generated",
		"negative branch guard necessity");
	CheckObligationStatus(
		FindProofObligation(
			refinement,
			"RelayGuardEquivalence",
			RequireString(*fallback, "id")),
		"Unsupported",
		"negative branch guard equivalence remains conservative");
}

void TestElseIfChain(const std::string& fixtureDirectory)
{
	const CompileResult result = CompileFixture(fixtureDirectory, "else_if_chain.prd");
	CheckTopLevel(result, "ProtocolElseIfChain");
	CheckGeneratedCppGolden(result, 12289, UINT64_C(0x39dd87043a22b87f));

	const Json& sites = RequireArray(result.manifest, "relay_sites");
	CHECK(sites.size() == 1);
	const Json& site = sites.front();
	CheckSiteCommon(site, "ProtocolElseIfChain", "send", "address", "custom_scope", "address");

	const Json& guards = RequireArray(site, "branches");
	CHECK(guards.size() == 3);
	const std::vector<std::string> expectedConditions = { "first", "prior", "current" };
	const std::vector<bool> expectedPolarities = { false, false, true };
	for (size_t i = 0; i < expectedConditions.size(); ++i)
	{
		CHECK(Compact(RequireString(RequireField(guards[i], "condition"), "text")) ==
			expectedConditions[i]);
		CHECK(RequireField(guards[i], "polarity").get<bool>() == expectedPolarities[i]);
	}

	const Json* function = FindFunctionByName(result.manifest, "send");
	CHECK(function != nullptr);
	const Json* node = &RequireField(*function, "root");
	CHECK(RequireString(*node, "kind") == "Sequence");
	const Json& rootChildren = RequireArray(*node, "children");
	CHECK(rootChildren.size() == 2);
	node = &rootChildren.front();
	for (size_t i = 0; i < expectedConditions.size(); ++i)
	{
		CHECK(RequireString(*node, "kind") == "Branch");
		CHECK(Compact(RequireString(RequireField(*node, "condition"), "text")) ==
			expectedConditions[i]);
		CHECK(RequireField(*node, "polarity").get<bool>() == expectedPolarities[i]);
		const Json& children = RequireArray(*node, "children");
		CHECK(children.size() == 1);
		node = &children.front();
	}
	CHECK(RequireString(*node, "kind") == "Emit");
}

void TestBoundedFor(const std::string& fixtureDirectory)
{
	const CompileResult result = CompileFixture(fixtureDirectory, "bounded_for.prd");
	CheckTopLevel(result, "ProtocolBoundedFor");
	CheckGeneratedCppGolden(result, 11630, UINT64_C(0x72530f493534f237));

	const Json& sites = RequireArray(result.manifest, "relay_sites");
	CHECK(sites.size() == 1);
	const Json& site = sites.front();
	CheckSiteCommon(site, "ProtocolBoundedFor", "send", "address", "custom_scope", "address");
	CheckDependency(
		RequireField(site, "target_dependency"),
		Json::array({ "TransactionArgument" }),
		"AdmissionTime",
		true);
	CHECK(RequireArray(site, "arguments").size() == 1);
	CheckDependency(
		RequireField(
			RequireArray(site, "arguments").front(),
			"dependency"),
		Json::array({
			"TransactionArgument",
			"LoopVariable",
		}),
		"DuringExecution",
		false);
	CHECK(RequireArray(site, "branches").empty());
	CHECK(RequireArray(site, "loops").size() == 1);
	const Json& loopObject = RequireArray(site, "loops").front();
	CHECK(RequireField(loopObject, "statically_bounded").is_boolean());
	CHECK(RequireField(loopObject, "statically_bounded").get<bool>());
	CHECK(RequireString(loopObject, "induction_variable") == "i");
	CHECK(RequireString(loopObject, "comparison") == "<");
	CHECK(RequireString(loopObject, "step") == "+1");
	const std::string loop = Compact(loopObject.dump());
	CHECK(loop.find("i") != std::string::npos);
	CHECK(loop.find("3u32") != std::string::npos);
	CHECK(loop.find("<") != std::string::npos);
	CHECK(ContainsNodeKind(RequireArray(result.manifest, "functions"), "Repeat"));
	CHECK(ContainsNodeKind(RequireArray(result.manifest, "functions"), "Emit"));

	const Json& summary =
		RequireFunctionSummary(result.manifest, "send");
	CheckConstantExpression(
		RequireField(summary, "relay_count"),
		3,
		"bounded loop relay_count");
	CheckConstantExpression(
		RequireField(summary, "relay_count_upper_bound"),
		3,
		"bounded loop relay_count_upper_bound");
	CheckConstantExpression(
		RequireField(summary, "max_depth"),
		1,
		"bounded loop max_depth");
	CHECK(RequireSummaryArray(summary, "relay_site_set").size() == 1);
	CheckSummaryScopeKinds(summary, Json::array({ "address" }));
	CheckSummaryBoolean(
		summary,
		"targets_known_before_execution",
		true);
	CheckSummaryBoolean(summary, "has_opaque", false);
	CheckSummaryOrdering(summary, "trivial");
	CHECK(RequireString(summary, "analysis_status") == "exact");

	const Json& refinement = RequireRefinement(result.manifest);
	const std::string functionId =
		RequireString(site, "source_function_id");
	const Json* countEquality = FindConstraint(
		refinement,
		"RelayCountEquality",
		std::string(),
		functionId);
	CHECK(countEquality != nullptr);
	const Json& count = RequireCountExpression(*countEquality);
	CHECK(RequireString(count, "kind") == "IntLiteral");
	CheckFormulaSort(count, "Int");
	CHECK(RequireString(count, "literal_value") == "3");
	CHECK(FindConstraint(
		refinement,
		"RelayCountNonNegative",
		std::string(),
		functionId) != nullptr);
	CHECK(FindConstraint(
		refinement,
		"RelayCountUpperBound",
		std::string(),
		functionId) != nullptr);
	CheckObligationStatus(
		FindProofObligation(
			refinement,
			"RelayCountEquality",
			std::string(),
			functionId),
		"Generated",
		"bounded loop direct count equality");
}

void CheckRejectedBoundedFor(
	const std::string& fixtureDirectory,
	const std::string& fixture,
	const std::string& contract,
	const std::string& expectedReason)
{
	const CompileResult result = CompileFixture(fixtureDirectory, fixture);
	CheckTopLevel(result, contract);

	const Json& sites = RequireArray(result.manifest, "relay_sites");
	CHECK(sites.size() == 1);
	const Json& loops = RequireArray(sites.front(), "loops");
	CHECK(loops.size() == 1);
	const Json& loop = loops.front();
	CHECK(RequireField(loop, "statically_bounded").is_boolean());
	CHECK(!RequireField(loop, "statically_bounded").get<bool>());
	CHECK(RequireString(loop, "opaque_reason").find(expectedReason) != std::string::npos);

	// Rejected proofs remain observable Repeat nodes with their complete source
	// expressions. The collector only withholds the statically-bounded claim.
	CHECK(RequireField(loop, "initializer").is_object());
	CHECK(RequireField(loop, "condition").is_object());
	CHECK(RequireField(loop, "update").is_object());
	CHECK(HasPositiveLine(RequireField(loop, "location")));
	CHECK(ContainsNodeKind(RequireArray(result.manifest, "functions"), "Repeat"));
	CHECK(ContainsNodeKind(RequireArray(result.manifest, "functions"), "Emit"));

	const Json& summary =
		RequireFunctionSummary(result.manifest, "send");
	CheckUnknownExpression(
		RequireField(summary, "relay_count"),
		fixture + " relay_count");
	CheckUnknownExpression(
		RequireField(summary, "relay_count_upper_bound"),
		fixture + " relay_count_upper_bound");
	CheckConstantExpression(
		RequireField(summary, "max_depth"),
		1,
		fixture + " max_depth");
	CHECK(RequireSummaryArray(summary, "relay_site_set").size() == 1);
	CheckSummaryScopeKinds(summary, Json::array({ "address" }));
	CheckSummaryBoolean(
		summary,
		"targets_known_before_execution",
		true);
	CheckSummaryBoolean(summary, "has_opaque", false);
	CheckSummaryOrdering(summary, "trivial");
	CHECK(RequireString(summary, "analysis_status") == "conservative");

	const Json& refinement = RequireRefinement(result.manifest);
	const std::string functionId =
		RequireString(sites.front(), "source_function_id");
	CHECK(FindConstraint(
		refinement,
		"RelayCountEquality",
		std::string(),
		functionId) == nullptr);
	CHECK(FindConstraint(
		refinement,
		"RelayCountUpperBound",
		std::string(),
		functionId) == nullptr);
	CHECK(FindConstraint(
		refinement,
		"RelayCountNonNegative",
		std::string(),
		functionId) != nullptr);
	CheckObligationStatus(
		FindProofObligation(
			refinement,
			"RelayCountEquality",
			std::string(),
			functionId),
		"Unsupported",
		fixture + " unknown exact direct count");
	CheckObligationStatus(
		FindProofObligation(
			refinement,
			"RelayCountUpperBound",
			std::string(),
			functionId),
		"Unsupported",
		fixture + " unknown finite upper bound");
}

void TestZeroStepIsNotBounded(const std::string& fixtureDirectory)
{
	CheckRejectedBoundedFor(
		fixtureDirectory,
		"unbounded_zero_step.prd",
		"ProtocolUnboundedZeroStep",
		"canonical ++ or --");
}

void TestBodyResetIsNotBounded(const std::string& fixtureDirectory)
{
	CheckRejectedBoundedFor(
		fixtureDirectory,
		"unbounded_body_reset.prd",
		"ProtocolUnboundedBodyReset",
		"body writes or shadows");
}

void TestNonStrictConditionIsNotBounded(const std::string& fixtureDirectory)
{
	CheckRejectedBoundedFor(
		fixtureDirectory,
		"unbounded_non_strict.prd",
		"ProtocolUnboundedNonStrict",
		"strict induction-variable comparison");
}

void TestNestedLambda(const std::string& fixtureDirectory)
{
	const CompileResult result = CompileFixture(fixtureDirectory, "nested_lambda.prd");
	CheckTopLevel(result, "ProtocolNestedLambda");
	CheckGeneratedCppGolden(result, 12725, UINT64_C(0xd0a59a2afffc89cc));

	const Json& sites = RequireArray(result.manifest, "relay_sites");
	CHECK(sites.size() == 2);
	CHECK(RequireArray(result.manifest, "handlers").size() == 2);
	CHECK(RequireArray(result.manifest, "edges").size() == 2);

	const Json* outerSite = nullptr;
	const Json* innerSite = nullptr;
	for (const Json& site : sites)
	{
		if (RequireString(site, "source_function") == "send")
			outerSite = &site;
		else
			innerSite = &site;
	}
	CHECK(outerSite != nullptr);
	CHECK(innerSite != nullptr);

	CheckSiteCommon(*outerSite, "ProtocolNestedLambda", "send", "address", "custom_scope", "address");
	CheckDependency(
		RequireField(*outerSite, "target_dependency"),
		Json::array({ "TransactionArgument" }),
		"AdmissionTime",
		true);
	CheckResolvedHandler(result.manifest, *outerSite, "lambda", "address");
	const Json* outerHandler = FindHandlerById(result.manifest, RequireField(*outerSite, "handler_id"));
	const std::string outerHandlerName = RequireString(*outerHandler, "name");
	CHECK(outerHandlerName.find("__relaylambda_") == 0);

	CHECK(RequireString(*innerSite, "source_function") == outerHandlerName);
	CheckSiteCommon(
		*innerSite,
		"ProtocolNestedLambda",
		outerHandlerName,
		"address",
		"custom_scope",
		"address");
	CheckDependency(
		RequireField(*innerSite, "target_dependency"),
		Json::array({ "TransactionArgument" }),
		"AdmissionTime",
		true);
	CheckResolvedHandler(result.manifest, *innerSite, "lambda", "address");
	const Json* innerHandler = FindHandlerById(result.manifest, RequireField(*innerSite, "handler_id"));
	CHECK(RequireString(*innerHandler, "name").find("__relaylambda_") == 0);
	CHECK(RequireString(*innerHandler, "name") != outerHandlerName);
	CHECK(ContainsNodeKind(RequireArray(result.manifest, "functions"), "Emit"));

	const Json& rootSummary =
		RequireFunctionSummary(result.manifest, "send");
	CheckConstantExpression(
		RequireField(rootSummary, "relay_count"),
		1,
		"nested lambda root relay_count");
	CheckConstantExpression(
		RequireField(rootSummary, "relay_count_upper_bound"),
		1,
		"nested lambda root relay_count_upper_bound");
	CheckConstantExpression(
		RequireField(rootSummary, "max_depth"),
		2,
		"nested lambda root max_depth");
	CHECK(RequireSummaryArray(rootSummary, "relay_site_set").size() == 1);
	CheckSummaryScopeKinds(rootSummary, Json::array({ "address" }));
	CheckSummaryBoolean(
		rootSummary,
		"targets_known_before_execution",
		true);
	CheckSummaryBoolean(rootSummary, "has_opaque", false);
	CheckSummaryOrdering(rootSummary, "trivial");

	const Json& handlerSummary =
		RequireFunctionSummary(result.manifest, outerHandlerName);
	CheckConstantExpression(
		RequireField(handlerSummary, "relay_count"),
		1,
		"nested lambda handler relay_count");
	CheckConstantExpression(
		RequireField(handlerSummary, "relay_count_upper_bound"),
		1,
		"nested lambda handler relay_count_upper_bound");
	CheckConstantExpression(
		RequireField(handlerSummary, "max_depth"),
		1,
		"nested lambda handler max_depth");
	CHECK(RequireSummaryArray(handlerSummary, "relay_site_set").size() == 1);
	CheckSummaryBoolean(handlerSummary, "has_opaque", false);
}

void TestOpaqueFallback(const std::string& fixtureDirectory)
{
	const CompileResult result = CompileFixture(fixtureDirectory, "opaque_fallback.prd");
	CheckTopLevel(result, "ProtocolOpaqueFallback");
	CheckGeneratedCppGolden(result, 12098, UINT64_C(0x49de06dd73c976db));

	const Json& sites = RequireArray(result.manifest, "relay_sites");
	CHECK(sites.size() == 1);
	const Json& site = sites.front();
	CheckSiteCommon(site, "ProtocolOpaqueFallback", "send", "address", "custom_scope", "address");

	const Json* opaque = FindExpressionKind(RequireField(site, "target"), "opaque");
	CHECK_DETAIL(opaque != nullptr, "ternary target was silently discarded instead of becoming Opaque");
	const std::string opaqueText = Compact(RequireString(*opaque, "text"));
	CHECK(opaqueText.find("use_first?first:second") != std::string::npos);
	CHECK(RequireString(*opaque, "type") == "address");
	CHECK(HasPositiveLine(RequireField(*opaque, "location")));
	CHECK(!RequireString(*opaque, "opaque_reason").empty());
	RequireField(*opaque, "operator");
	RequireArray(*opaque, "children");
	CheckDependency(
		RequireField(site, "target_dependency"),
		Json::array({ "Opaque" }),
		"Unknown",
		false);
	CheckResolvedHandler(result.manifest, site, "named", "address");

	const Json& summary =
		RequireFunctionSummary(result.manifest, "send");
	CheckConstantExpression(
		RequireField(summary, "relay_count"),
		1,
		"opaque target relay_count");
	CheckConstantExpression(
		RequireField(summary, "relay_count_upper_bound"),
		1,
		"opaque target relay_count_upper_bound");
	CheckConstantExpression(
		RequireField(summary, "max_depth"),
		1,
		"opaque target max_depth");
	CHECK(RequireSummaryArray(summary, "relay_site_set").size() == 1);
	CheckSummaryScopeKinds(summary, Json::array({ "address" }));
	CheckSummaryBoolean(
		summary,
		"targets_known_before_execution",
		false);
	CheckSummaryBoolean(summary, "has_opaque", true);
	CheckSummaryFanout(summary, "single_target");
	CheckSummaryOrdering(summary, "trivial");
	CHECK(RequireString(summary, "analysis_status") == "conservative");

	const Json& refinement = RequireRefinement(result.manifest);
	const std::string siteId = RequireString(site, "id");
	const std::string functionId =
		RequireString(site, "source_function_id");
	CHECK(FindConstraint(
		refinement,
		"RelayTargetRelation",
		siteId) == nullptr);
	const Json* targetObligation = FindProofObligation(
		refinement,
		"RelayTargetEquality",
		siteId);
	CheckObligationStatus(
		targetObligation,
		"Unsupported",
		"opaque ternary target equality");
	CHECK(
		RequireString(
			RequireField(*targetObligation, "goal"),
			"kind") ==
		"Unknown");

	const Json* countEquality = FindConstraint(
		refinement,
		"RelayCountEquality",
		std::string(),
		functionId);
	CHECK(countEquality != nullptr);
	const Json& count = RequireCountExpression(*countEquality);
	CHECK(RequireString(count, "kind") == "IntLiteral");
	CHECK(RequireString(count, "literal_value") == "1");
	CheckObligationStatus(
		FindProofObligation(
			refinement,
			"RelayCountEquality",
			std::string(),
			functionId),
		"Generated",
		"opaque target does not erase direct count");
}

void TestExpressionDependencies(
	const std::string& fixtureDirectory)
{
	struct DependencyCase
	{
		const char* fixture;
		const char* contract;
		Json dependencies;
		const char* availability;
		bool admissionTimeEvaluable;
	};

	const std::vector<DependencyCase> cases = {
		{
			"dependency_literal.prd",
			"ProtocolDependencyLiteral",
			Json::array({ "Constant" }),
			"CompileTime",
			true,
		},
		{
			"dependency_arithmetic_cast.prd",
			"ProtocolDependencyArithmeticCast",
			Json::array({ "TransactionArgument" }),
			"AdmissionTime",
			true,
		},
		{
			"dependency_unary.prd",
			"ProtocolDependencyUnary",
			Json::array({ "TransactionArgument" }),
			"AdmissionTime",
			true,
		},
		{
			"dependency_current_scope_key.prd",
			"ProtocolDependencyCurrentScopeKey",
			Json::array({ "CurrentScopeKey" }),
			"AdmissionTime",
			true,
		},
		{
			"dependency_next.prd",
			"ProtocolDependencyNext",
			Json::array({ "CurrentScopeKey" }),
			"AdmissionTime",
			true,
		},
		{
			"dependency_state_owner.prd",
			"ProtocolDependencyStateOwner",
			Json::array({ "CurrentScopeState" }),
			"AfterScopeLoad",
			false,
		},
		{
			"dependency_member.prd",
			"ProtocolDependencyMember",
			Json::array({ "CurrentScopeState" }),
			"AfterScopeLoad",
			false,
		},
		{
			"dependency_array_index.prd",
			"ProtocolDependencyArrayIndex",
			Json::array({
				"TransactionArgument",
				"CurrentScopeState",
			}),
			"AfterScopeLoad",
			false,
		},
		{
			"dependency_local_parameter.prd",
			"ProtocolDependencyLocalParameter",
			Json::array({ "TransactionArgument" }),
			"AdmissionTime",
			true,
		},
		{
			"dependency_conditional_initialization.prd",
			"ProtocolDependencyConditionalInitialization",
			Json::array({
				"TransactionArgument",
				"LocalDerived",
			}),
			"DuringExecution",
			false,
		},
		{
			"dependency_local_overwrite.prd",
			"ProtocolDependencyLocalOverwrite",
			Json::array({
				"TransactionArgument",
				"CurrentScopeState",
			}),
			"AfterScopeLoad",
			false,
		},
		{
			"dependency_parameter_overwrite.prd",
			"ProtocolDependencyParameterOverwrite",
			Json::array({
				"TransactionArgument",
				"CurrentScopeState",
			}),
			"AfterScopeLoad",
			false,
		},
		{
			"dependency_state_call_write.prd",
			"ProtocolDependencyStateCallWrite",
			Json::array({
				"CurrentScopeState",
				"ExternalCallResult",
			}),
			"DuringExecution",
			false,
		},
		{
			"dependency_external_call.prd",
			"ProtocolDependencyExternalCall",
			Json::array({ "ExternalCallResult" }),
			"DuringExecution",
			false,
		},
		{
			"dependency_opaque_effect.prd",
			"ProtocolDependencyOpaqueEffect",
			Json::array({
				"CurrentScopeState",
				"Opaque",
			}),
			"Unknown",
			false,
		},
		{
			"dependency_loop_widening.prd",
			"ProtocolDependencyLoopWidening",
			Json::array({
				"TransactionArgument",
				"CurrentScopeState",
			}),
			"AfterScopeLoad",
			false,
		},
		{
			"dependency_existing_loop_variable.prd",
			"ProtocolDependencyExistingLoopVariable",
			Json::array({ "LoopVariable" }),
			"DuringExecution",
			false,
		},
	};

	for (const DependencyCase& dependencyCase : cases)
	{
		const CompileResult result =
			CompileFixture(
				fixtureDirectory,
				dependencyCase.fixture);
		CheckTopLevel(result, dependencyCase.contract);
		if (std::string(dependencyCase.fixture) ==
			"dependency_next.prd")
		{
			CheckGeneratedRelayCall(
				result,
				"prlrt::relay_next(",
				"prlrt::relay_next_traced(0, ");
		}
		const Json& sites =
			RequireArray(result.manifest, "relay_sites");
		CHECK_DETAIL(
			sites.size() == 1,
			std::string(dependencyCase.fixture) +
				": expected one relay site");
		CheckDependency(
			RequireField(
				sites.front(),
				"target_dependency"),
			dependencyCase.dependencies,
			dependencyCase.availability,
			dependencyCase.admissionTimeEvaluable);

		const Json& summary =
			RequireFunctionSummary(result.manifest, "send");
		CheckSummaryBoolean(
			summary,
			"targets_known_before_execution",
			dependencyCase.admissionTimeEvaluable);
	}
}

void TestRefinementLiteralTarget(
	const std::string& fixtureDirectory)
{
	const CompileResult result =
		CompileFixture(fixtureDirectory, "dependency_literal.prd");
	CheckTopLevel(result, "ProtocolDependencyLiteral");
	const Json& site =
		RequireArray(result.manifest, "relay_sites").front();
	const std::string siteId = RequireString(site, "id");
	const Json& refinement = RequireRefinement(result.manifest);
	const Json* targetRelation =
		FindConstraint(refinement, "RelayTargetRelation", siteId);
	CHECK(targetRelation != nullptr);
	const Json& target = UnwrapGroups(
		RequireRelationSourceExpression(*targetRelation));
	CHECK(RequireString(target, "kind") == "BitVectorLiteral");
	CheckFormulaSort(target, "UnsignedBitVector", 32);
	CHECK(RequireString(target, "literal_value") == "7u32");
	CheckObligationStatus(
		FindProofObligation(
			refinement,
			"RelayTargetEquality",
			siteId),
		"Generated",
		"literal target equality");
}

void CheckUint32CoordinateFormula(
	const Json& formula,
	const std::string& detail)
{
	const Json& addition = UnwrapGroups(formula);
	CHECK_DETAIL(
		RequireString(addition, "kind") == "Binary"
			&& RequireString(addition, "operator") == "+",
		detail + ": expected uint32 coordinate addition: "
			+ addition.dump());
	CheckFormulaSort(addition, "UnsignedBitVector", 32);
	const Json& additionChildren =
		RequireArray(addition, "children");
	CHECK(additionChildren.size() == 2);

	const Json& multiplication =
		UnwrapGroups(additionChildren[0]);
	CHECK(RequireString(multiplication, "kind") == "Binary");
	CHECK(RequireString(multiplication, "operator") == "*");
	CheckFormulaSort(
		multiplication,
		"UnsignedBitVector",
		32);
	const Json& multiplicationChildren =
		RequireArray(multiplication, "children");
	CHECK(multiplicationChildren.size() == 2);

	const Json& xCast = UnwrapGroups(multiplicationChildren[0]);
	CHECK(RequireString(xCast, "kind") == "Cast");
	CHECK(RequireString(xCast, "operator") == "uint32");
	CheckFormulaSort(xCast, "UnsignedBitVector", 32);
	const Json& xOperands = RequireArray(xCast, "children");
	CHECK(xOperands.size() == 1);
	CHECK(RequireString(xOperands.front(), "kind") == "Symbol");
	CHECK(RequireString(xOperands.front(), "source_text") == "x");
	CheckFormulaSort(
		xOperands.front(),
		"UnsignedBitVector",
		16);

	const Json& multiplier =
		UnwrapGroups(multiplicationChildren[1]);
	CHECK(RequireString(multiplier, "kind") == "BitVectorLiteral");
	CHECK(RequireString(multiplier, "literal_value") == "65536u32");
	CheckFormulaSort(multiplier, "UnsignedBitVector", 32);

	const Json& yCast = UnwrapGroups(additionChildren[1]);
	CHECK(RequireString(yCast, "kind") == "Cast");
	CHECK(RequireString(yCast, "operator") == "uint32");
	CheckFormulaSort(yCast, "UnsignedBitVector", 32);
	const Json& yOperands = RequireArray(yCast, "children");
	CHECK(yOperands.size() == 1);
	CHECK(RequireString(yOperands.front(), "kind") == "Symbol");
	CHECK(RequireString(yOperands.front(), "source_text") == "y");
	CheckFormulaSort(
		yOperands.front(),
		"UnsignedBitVector",
		16);
}

void TestRefinementArithmeticCastTarget(
	const std::string& fixtureDirectory)
{
	const CompileResult result = CompileFixture(
		fixtureDirectory,
		"dependency_arithmetic_cast.prd");
	CheckTopLevel(result, "ProtocolDependencyArithmeticCast");
	const Json& site =
		RequireArray(result.manifest, "relay_sites").front();
	const Json& refinement = RequireRefinement(result.manifest);
	const Json* targetRelation = FindConstraint(
		refinement,
		"RelayTargetRelation",
		RequireString(site, "id"));
	CHECK(targetRelation != nullptr);
	CheckUint32CoordinateFormula(
		RequireRelationSourceExpression(*targetRelation),
		"arithmetic/cast target");
}

void TestRefinementPreStateTarget(
	const std::string& fixtureDirectory)
{
	const CompileResult result = CompileFixture(
		fixtureDirectory,
		"dependency_state_owner.prd");
	CheckTopLevel(result, "ProtocolDependencyStateOwner");
	const Json& site =
		RequireArray(result.manifest, "relay_sites").front();
	const std::string siteId = RequireString(site, "id");
	const Json& refinement = RequireRefinement(result.manifest);
	const Json* targetRelation =
		FindConstraint(refinement, "RelayTargetRelation", siteId);
	CHECK(targetRelation != nullptr);
	const Json& target =
		RequireRelationSourceExpression(*targetRelation);
	CHECK(RequireString(target, "kind") == "Symbol");
	CheckFormulaSort(target, "Address");
	const Json* preState = FindRefinementSymbol(
		refinement,
		"PreStateVariable",
		"owner",
		std::string(),
		RequireString(site, "source_function_id"));
	CHECK(preState != nullptr);
	CHECK(
		RequireString(target, "symbol_id") ==
		RequireString(*preState, "id"));
	CHECK(
		RequireArray(*preState, "dependencies") ==
		Json::array({ "CurrentScopeState" }));
	CHECK(
		RequireString(*preState, "availability") ==
		"AfterScopeLoad");
	CheckObligationStatus(
		FindProofObligation(
			refinement,
			"RelayTargetEquality",
			siteId),
		"Generated",
		"pre-state target equality");
}

void TestRefinementUint32OverflowAndCast(
	const std::string& fixtureDirectory)
{
	const CompileResult result = CompileFixture(
		fixtureDirectory,
		"refinement_uint32_wrap.prd");
	CheckTopLevel(result, "ProtocolRefinementUint32Wrap");
	const Json& site =
		RequireArray(result.manifest, "relay_sites").front();
	const Json& refinement = RequireRefinement(result.manifest);
	const Json* targetRelation = FindConstraint(
		refinement,
		"RelayTargetRelation",
		RequireString(site, "id"));
	CHECK(targetRelation != nullptr);
	const Json& addition = UnwrapGroups(
		RequireRelationSourceExpression(*targetRelation));
	CHECK(RequireString(addition, "kind") == "Binary");
	CHECK(RequireString(addition, "operator") == "+");
	CheckFormulaSort(addition, "UnsignedBitVector", 32);
	const Json& operands = RequireArray(addition, "children");
	CHECK(operands.size() == 2);
	const Json& cast = UnwrapGroups(operands[0]);
	CHECK(RequireString(cast, "kind") == "Cast");
	CHECK(RequireString(cast, "operator") == "uint32");
	CheckFormulaSort(cast, "UnsignedBitVector", 32);
	const Json& castOperand =
		RequireArray(cast, "children").front();
	CHECK(RequireString(castOperand, "kind") == "Symbol");
	CheckFormulaSort(
		castOperand,
		"UnsignedBitVector",
		64);
	const Json& maximum = UnwrapGroups(operands[1]);
	CHECK(RequireString(maximum, "kind") == "BitVectorLiteral");
	CHECK(
		RequireString(maximum, "literal_value") ==
		"4294967295u32");
	CheckFormulaSort(maximum, "UnsignedBitVector", 32);
	CHECK(FindFormulaNode(addition, "IntLiteral") == nullptr);
}

void TestRefinementBooleanGuardAndArrayLength(
	const std::string& fixtureDirectory)
{
	const CompileResult booleanResult = CompileFixture(
		fixtureDirectory,
		"refinement_boolean_guard.prd");
	CheckTopLevel(
		booleanResult,
		"ProtocolRefinementBooleanGuard");
	const Json& booleanSite =
		RequireArray(booleanResult.manifest, "relay_sites").front();
	const Json& booleanRefinement =
		RequireRefinement(booleanResult.manifest);
	const Json* guard = FindConstraint(
		booleanRefinement,
		"RelayGuardNecessity",
		RequireString(booleanSite, "id"));
	CHECK(guard != nullptr);
	const Json& predicate = RequireBinaryChild(
		RequireField(*guard, "formula"),
		"implies",
		1);
	CHECK(FindFormulaNode(predicate, "Binary", "&&") != nullptr);
	CHECK(FindFormulaNode(predicate, "Binary", "||") != nullptr);
	CHECK(FindFormulaNode(predicate, "Binary", "<") != nullptr);
	CHECK(FindFormulaNode(predicate, "Unary", "!") != nullptr);
	CheckFormulaSort(predicate, "Bool");

	const CompileResult arrayResult = CompileFixture(
		fixtureDirectory,
		"refinement_array_length.prd");
	CheckTopLevel(
		arrayResult,
		"ProtocolRefinementArrayLength");
	const Json& arraySite =
		RequireArray(arrayResult.manifest, "relay_sites").front();
	const std::string arraySiteId =
		RequireString(arraySite, "id");
	const Json& arrayRefinement =
		RequireRefinement(arrayResult.manifest);
	const Json* targetRelation = FindConstraint(
		arrayRefinement,
		"RelayTargetRelation",
		arraySiteId);
	CHECK(targetRelation != nullptr);
	const Json& length = UnwrapGroups(
		RequireRelationSourceExpression(*targetRelation));
	CHECK(RequireString(length, "kind") == "ArrayLength");
	CheckFormulaSort(length, "UnsignedBitVector", 32);
	CHECK(RequireArray(length, "children").size() == 1);
	CheckObligationStatus(
		FindProofObligation(
			arrayRefinement,
			"RelayTargetEquality",
			arraySiteId),
		"Generated",
		"array length target equality");
}

void TestRefinementMillionPixel(
	const std::string& fixtureDirectory)
{
	const CompileResult result = CompileFixture(
		fixtureDirectory + "/../../../simulator/contracts",
		"MillionPixel.prd");
	CheckTopLevel(result, "MillionPixel");
	const Json& sites =
		RequireArray(result.manifest, "relay_sites");
	CHECK(sites.size() == 1);
	const Json& site = sites.front();
	CHECK(Compact(
		RequireString(
			RequireField(site, "target"),
			"text")) == "index");
	const std::string siteId = RequireString(site, "id");
	const Json& refinement = RequireRefinement(result.manifest);
	const Json* targetRelation =
		FindConstraint(refinement, "RelayTargetRelation", siteId);
	CHECK(targetRelation != nullptr);
	CheckUint32CoordinateFormula(
		RequireRelationSourceExpression(*targetRelation),
		"MillionPixel local index target");
	CheckObligationStatus(
		FindProofObligation(
			refinement,
			"RelayTargetEquality",
			siteId),
		"Generated",
		"MillionPixel target equality");
}

void TestSummaryConditional(const std::string& fixtureDirectory)
{
	const CompileResult result =
		CompileFixture(fixtureDirectory, "summary_conditional.prd");
	CheckTopLevel(result, "ProtocolSummaryConditional");
	CheckGeneratedRelayCall(
		result,
		"prlrt::relay(",
		"prlrt::relay_traced(");

	const Json& sites = RequireArray(result.manifest, "relay_sites");
	CHECK(sites.size() == 1);
	const Json& summary =
		RequireFunctionSummary(result.manifest, "send");
	const Json& relayCount = RequireField(summary, "relay_count");
	CHECK(RequireString(relayCount, "kind") == "ite");
	const Json& predicate = RequireField(relayCount, "predicate");
	CHECK(RequireString(predicate, "kind") == "identifier");
	CHECK(Compact(RequireString(predicate, "text")) == "enabled");
	const Json& arms = RequireArray(relayCount, "children");
	CHECK(arms.size() == 2);
	CheckConstantExpression(
		arms[0],
		1,
		"conditional relay_count then arm");
	CheckConstantExpression(
		arms[1],
		0,
		"conditional relay_count else arm");
	CheckConstantExpression(
		RequireField(summary, "relay_count_upper_bound"),
		1,
		"conditional relay_count_upper_bound");
	CheckConstantExpression(
		RequireField(summary, "max_depth"),
		1,
		"conditional max_depth");
	CHECK(
		RequireSummaryArray(summary, "relay_site_set") ==
		Json::array({ RequireString(sites.front(), "id") }));
	CheckSummaryScopeKinds(summary, Json::array({ "address" }));
	CheckSummaryBoolean(
		summary,
		"targets_known_before_execution",
		true);
	CheckSummaryBoolean(summary, "has_opaque", false);
	CheckSummaryFanout(summary, "single_target");
	CheckSummaryOrdering(summary, "trivial");
	CHECK(RequireString(summary, "analysis_status") == "exact");

	const Json& refinement = RequireRefinement(result.manifest);
	const Json& site = sites.front();
	const std::string siteId = RequireString(site, "id");
	const std::string functionId =
		RequireString(site, "source_function_id");

	const Json* argumentRelation = FindConstraint(
		refinement,
		"RelayArgumentRelation",
		siteId,
		std::string(),
		0);
	CHECK(argumentRelation != nullptr);
	const Json& argumentFormula =
		RequireRelationSourceExpression(*argumentRelation);
	CHECK(RequireString(argumentFormula, "kind") == "Symbol");
	CHECK(RequireString(argumentFormula, "source_text") == "value");
	CheckFormulaSort(argumentFormula, "UnsignedBitVector", 32);
	const Json* argumentParameter =
		FindRefinementSymbol(
			refinement,
			"SourceFunctionParameter",
			"value",
			std::string(),
			functionId);
	CHECK(argumentParameter != nullptr);
	CHECK(
		RequireString(argumentFormula, "symbol_id") ==
		RequireString(*argumentParameter, "id"));
	CheckObligationStatus(
		FindProofObligation(
			refinement,
			"RelayArgumentEquality",
			siteId,
			std::string(),
			0),
		"Generated",
		"relay argument equals source parameter");

	const Json* guard = FindConstraint(
		refinement,
		"RelayGuardNecessity",
		siteId);
	CHECK(guard != nullptr);
	const Json& guardPredicate = RequireBinaryChild(
		RequireField(*guard, "formula"),
		"implies",
		1);
	CHECK(RequireString(guardPredicate, "kind") == "Symbol");
	CHECK(RequireString(guardPredicate, "source_text") == "enabled");
	CheckFormulaSort(guardPredicate, "Bool");

	const Json* countEquality = FindConstraint(
		refinement,
		"RelayCountEquality",
		std::string(),
		functionId);
	CHECK(countEquality != nullptr);
	const Json& countFormula =
		RequireCountExpression(*countEquality);
	CHECK(RequireString(countFormula, "kind") == "Ite");
	CheckFormulaSort(countFormula, "Int");
	const Json& countChildren =
		RequireArray(countFormula, "children");
	CHECK(countChildren.size() == 3);
	CHECK(RequireString(countChildren[0], "kind") == "Symbol");
	CHECK(RequireString(countChildren[0], "source_text") == "enabled");
	CHECK(RequireString(countChildren[1], "kind") == "IntLiteral");
	CHECK(RequireString(countChildren[1], "literal_value") == "1");
	CHECK(RequireString(countChildren[2], "kind") == "IntLiteral");
	CHECK(RequireString(countChildren[2], "literal_value") == "0");
	CheckObligationStatus(
		FindProofObligation(
			refinement,
			"RelayCountEquality",
			std::string(),
			functionId),
		"Generated",
		"conditional direct count ite");
	CheckObligationStatus(
		FindProofObligation(
			refinement,
			"RelayGuardEquivalence",
			siteId),
		"Unsupported",
		"conditional guard equivalence needs full CFG proof");
}

void TestSummarySequential(const std::string& fixtureDirectory)
{
	const CompileResult result =
		CompileFixture(fixtureDirectory, "summary_sequential.prd");
	CheckTopLevel(result, "ProtocolSummarySequential");
	CheckGeneratedRelayCall(
		result,
		"prlrt::relay(",
		"prlrt::relay_traced(");

	const Json& sites = RequireArray(result.manifest, "relay_sites");
	CHECK(sites.size() == 2);
	const Json& summary =
		RequireFunctionSummary(result.manifest, "send");
	CheckConstantExpression(
		RequireField(summary, "relay_count"),
		2,
		"sequential relay_count");
	CheckConstantExpression(
		RequireField(summary, "relay_count_upper_bound"),
		2,
		"sequential relay_count_upper_bound");
	CheckConstantExpression(
		RequireField(summary, "max_depth"),
		1,
		"sequential max_depth");
	CHECK(RequireSummaryArray(summary, "relay_site_set").size() == 2);
	CheckSummaryScopeKinds(summary, Json::array({ "address" }));
	CheckSummaryBoolean(
		summary,
		"targets_known_before_execution",
		true);
	CheckSummaryBoolean(summary, "has_opaque", false);
	CheckSummaryFanout(summary, "single_target");

	// Sequence.children currently reflects listener discovery order only. The
	// summary must not turn it into a proved execution-order edge.
	CheckSummaryOrdering(summary, "unknown");
	CHECK(RequireString(summary, "analysis_status") == "conservative");
}

void TestSummaryRecursiveHandler(const std::string& fixtureDirectory)
{
	const CompileResult result =
		CompileFixture(
			fixtureDirectory,
			"summary_recursive_handler.prd");
	CheckTopLevel(result, "ProtocolSummaryRecursiveHandler");
	CheckGeneratedRelayCall(
		result,
		"prlrt::relay(",
		"prlrt::relay_traced(");
	CHECK(RequireArray(result.manifest, "relay_sites").size() == 2);

	for (const std::string functionName : { std::string("recur"), std::string("send") })
	{
		const Json& summary =
			RequireFunctionSummary(result.manifest, functionName);
		CheckConstantExpression(
			RequireField(summary, "relay_count"),
			1,
			functionName + " recursive relay_count");
		CheckConstantExpression(
			RequireField(summary, "relay_count_upper_bound"),
			1,
			functionName + " recursive relay_count_upper_bound");
		CheckUnknownExpression(
			RequireField(summary, "max_depth"),
			functionName + " recursive max_depth");
		CHECK(
			Compact(
				RequireString(
					RequireField(summary, "max_depth"),
					"reason"))
				.find("recursive") != std::string::npos);
		CHECK(RequireSummaryArray(summary, "relay_site_set").size() == 1);
		CheckSummaryScopeKinds(summary, Json::array({ "address" }));
		CheckSummaryBoolean(
			summary,
			"targets_known_before_execution",
			true);
		CheckSummaryBoolean(summary, "has_opaque", false);
		CheckSummaryFanout(summary, "single_target");
		CheckSummaryOrdering(summary, "trivial");
		CHECK(
			RequireString(summary, "analysis_status") ==
			"conservative");
	}
}

void TestOpaqueProtocolNodeIsConservative(const std::string&)
{
	using namespace transpiler::relay_protocol;
	using namespace transpiler::relay_protocol::analysis;

	RelayProtocolIR protocol;
	protocol.Reset("RelayProtocolTests", "SyntheticOpaqueControl");

	FunctionProtocol function;
	function.contract = "RelayProtocolTests.SyntheticOpaqueControl";
	function.function = "send";
	function.sourceFunctionId =
		"RelayProtocolTests.SyntheticOpaqueControl::send()";
	function.sourceFunctionSignature = "send()";
	function.root.kind = ProtocolNodeKind::Sequence;

	ProtocolNode opaque;
	opaque.kind = ProtocolNodeKind::Opaque;
	opaque.opaqueReason = "synthetic unsupported control flow";
	function.root.children.push_back(std::move(opaque));
	protocol.functions.push_back(std::move(function));

	RelaySummaryBuilder builder;
	builder.Build(protocol);

	CHECK(protocol.functions.size() == 1);
	const RelayProtocolSummary& summary =
		protocol.functions.front().summary;
	CHECK(
		summary.relayCount.kind ==
		RelayCardinalityExprKind::Unknown);
	CHECK(
		summary.relayCountUpperBound.kind ==
		RelayCardinalityExprKind::Unknown);
	CHECK(summary.maxDepth.kind == RelayDepthExprKind::Unknown);
	CHECK(summary.hasOpaque);
	CHECK(summary.analysisStatus == RelayAnalysisStatus::Unknown);
}

void TestSummaryUnmodeledRelayReachableCall(
	const std::string& fixtureDirectory)
{
	const CompileResult result =
		CompileFixture(
			fixtureDirectory,
			"summary_unmodeled_call.prd");
	CheckTopLevel(result, "ProtocolSummaryUnmodeledCall");

	const Json& handlerSummary =
		RequireFunctionSummary(result.manifest, "handler");
	CheckConstantExpression(
		RequireField(handlerSummary, "relay_count"),
		1,
		"unmodeled-call handler relay_count");
	CheckUnknownExpression(
		RequireField(handlerSummary, "max_depth"),
		"unmodeled-call handler max_depth");
	CHECK(
		RequireString(
			RequireField(handlerSummary, "max_depth"),
			"reason").find("ordinary call") != std::string::npos);
	CheckSummaryBoolean(
		handlerSummary,
		"has_unmodeled_relay_reachable_call",
		true);

	const Json& sendSummary =
		RequireFunctionSummary(result.manifest, "send");
	CheckConstantExpression(
		RequireField(sendSummary, "relay_count"),
		1,
		"unmodeled-call send relay_count");
	CheckUnknownExpression(
		RequireField(sendSummary, "max_depth"),
		"unmodeled-call send max_depth");
	CheckSummaryBoolean(
		sendSummary,
		"has_unmodeled_relay_reachable_call",
		false);

	const Json& helperSummary =
		RequireFunctionSummary(result.manifest, "helper");
	CheckConstantExpression(
		RequireField(helperSummary, "max_depth"),
		2,
		"modeled helper max_depth");
	CheckSummaryBoolean(
		helperSummary,
		"has_unmodeled_relay_reachable_call",
		false);
}

void TestOverloadedSource(const std::string& fixtureDirectory)
{
	const CompileResult result = CompileFixture(fixtureDirectory, "overloaded_source.prd");
	CheckTopLevel(result, "ProtocolOverloadedSource");
	CheckGeneratedRelayCall(
		result,
		"prlrt::relay(",
		"prlrt::relay_traced(");

	const Json& sites = RequireArray(result.manifest, "relay_sites");
	const Json& edges = RequireArray(result.manifest, "edges");
	const Json& functions = RequireArray(result.manifest, "functions");
	CHECK(sites.size() == 2);
	CHECK(edges.size() == 2);
#ifdef RPREDA_ENABLE_BOUND_RELAY_MANIFEST
	CHECK(functions.size() == 4);
#else
	CHECK(functions.size() == 2);
#endif

	std::vector<std::string> sourceFunctionIds;
	std::vector<std::string> sourceFunctionSignatures;
	std::vector<uint64_t> overloadIndexes;
	for (const Json& site : sites)
	{
		CheckSiteCommon(
			site,
			"ProtocolOverloadedSource",
			"send",
			"address",
			"custom_scope",
			"address");
		sourceFunctionIds.push_back(RequireString(site, "source_function_id"));
		sourceFunctionSignatures.push_back(
			RequireString(site, "source_function_signature"));
		overloadIndexes.push_back(
			RequireField(site, "source_function_overload_index").get<uint64_t>());
	}
	std::sort(sourceFunctionIds.begin(), sourceFunctionIds.end());
	std::sort(sourceFunctionSignatures.begin(), sourceFunctionSignatures.end());
	std::sort(overloadIndexes.begin(), overloadIndexes.end());
	CHECK(sourceFunctionIds == std::vector<std::string>({
		"RelayProtocolTests.ProtocolOverloadedSource::send(address,int32)",
		"RelayProtocolTests.ProtocolOverloadedSource::send(address,uint32)",
	}));
	CHECK(sourceFunctionSignatures == std::vector<std::string>({
		"send(address,int32)",
		"send(address,uint32)",
	}));
	CHECK(overloadIndexes == std::vector<uint64_t>({ 0, 1 }));

	size_t sourceFunctionCount = 0;
	size_t zeroRelayHandlerCount = 0;
	for (const Json& function : functions)
	{
		const std::string functionName =
			RequireString(function, "function");
		if (functionName != "send")
		{
#ifdef RPREDA_ENABLE_BOUND_RELAY_MANIFEST
			CHECK(functionName == "receive");
			CHECK(RequireArray(function, "relay_site_ids").empty());
			CHECK(
				RequireField(
					function,
					"exported_opcode").get<int64_t>() >= 0);
			++zeroRelayHandlerCount;
			continue;
#else
			CHECK(functionName == "send");
#endif
		}
		++sourceFunctionCount;
		CHECK(!RequireString(function, "source_function_id").empty());
		CHECK(!RequireString(function, "source_function_signature").empty());
		CHECK(RequireField(
			function,
			"source_function_overload_index").is_number_unsigned());
		CHECK(RequireArray(function, "relay_site_ids").size() == 1);
	}
	CHECK(sourceFunctionCount == 2);
#ifdef RPREDA_ENABLE_BOUND_RELAY_MANIFEST
	CHECK(zeroRelayHandlerCount == 2);
#else
	CHECK(zeroRelayHandlerCount == 0);
#endif

	for (const Json& edge : edges)
	{
		const Json* sourceSite = nullptr;
		for (const Json& site : sites)
		{
			if (RequireField(site, "id") == RequireField(edge, "relay_site_id"))
			{
				sourceSite = &site;
				break;
			}
		}
		CHECK_DETAIL(sourceSite != nullptr, "edge relay_site_id does not resolve");
		CHECK(RequireString(edge, "source_function") == "send");
		CHECK(
			RequireString(edge, "source_function_id") ==
			RequireString(*sourceSite, "source_function_id"));
		CHECK(
			RequireString(edge, "source_function_signature") ==
			RequireString(*sourceSite, "source_function_signature"));
		CHECK(
			RequireField(edge, "source_function_overload_index") ==
			RequireField(*sourceSite, "source_function_overload_index"));
	}
}

void TestPredaNativeCFGAndICFG(const std::string& fixtureDirectory)
{
	const CompileResult result =
		CompileFixture(fixtureDirectory, "cfg_phase_ad.prd");
	CheckTopLevel(result, "ProtocolCFGPhaseAD");
	const Json& controlFlow = RequireControlFlow(result.manifest);
	const Json& phases = RequireArray(controlFlow, "implemented_phases");
	CHECK(phases.size() == 4);
	CHECK(JsonArrayContains(phases, "A"));
	CHECK(JsonArrayContains(phases, "B"));
	CHECK(JsonArrayContains(phases, "C"));
	CHECK(JsonArrayContains(phases, "D"));

	const Json* send = FindCFGFunctionByName(controlFlow, "send");
	const Json* helper = FindCFGFunctionByName(controlFlow, "helper");
	const Json* receive = FindCFGFunctionByName(controlFlow, "receive");
	const Json* touchGlobal =
		FindCFGFunctionByName(controlFlow, "touch_global");
	CHECK(send != nullptr);
	CHECK(helper != nullptr);
	CHECK(receive != nullptr);
	CHECK(touchGlobal != nullptr);
	for (const std::string& kind : {
		"Entry", "Exit", "Branch", "LoopHeader", "LoopLatch",
		"Break", "Continue", "Return", "SynchronousCall",
		"RelayEmit" })
	{
		CHECK_DETAIL(CFGHasNodeKind(*send, kind), "missing CFG node kind " + kind);
	}
	for (const std::string& kind : {
		"TrueBranch", "FalseBranch", "LoopBack", "BreakExit",
		"ContinueBack", "ReturnExit" })
	{
		CHECK_DETAIL(CFGHasEdgeKind(*send, kind), "missing CFG edge kind " + kind);
	}
	CHECK(RequireString(*send, "status") != "Unsupported");
	CHECK(!RequireString(*send, "entry_node_id").empty());
	CHECK(!RequireString(*send, "exit_node_id").empty());

	const Json& receiveEffect = RequireField(*receive, "effect");
	CHECK(RequireField(receiveEffect, "writes_current_scope_state") == true);
	CHECK(!RequireArray(receiveEffect, "written_state_variables").empty());
	const Json& globalEffect = RequireField(*touchGlobal, "effect");
	CHECK(RequireField(globalEffect, "writes_global_state") == true);
	const Json& helperEffect = RequireField(*helper, "effect");
	CHECK(RequireField(helperEffect, "may_emit_relay") == true);
	const Json& sendEffect = RequireField(*send, "effect");
	CHECK(RequireField(sendEffect, "may_emit_relay") == true);
	CHECK(RequireField(sendEffect, "writes_current_scope_state") == true);
	CHECK(RequireField(sendEffect, "may_break") == true);
	CHECK(RequireField(sendEffect, "may_continue") == true);
	CHECK(RequireField(sendEffect, "may_return_early") == true);
	bool foundReturnInsideLoop = false;
	for (const Json& node : RequireArray(*send, "nodes"))
	{
		if (RequireString(node, "kind") == "Return" &&
			!RequireArray(node, "enclosing_loop_ids").empty())
		{
			foundReturnInsideLoop = true;
		}
	}
	CHECK(foundReturnInsideLoop);

	const Json& callGraph =
		RequireField(controlFlow, "synchronous_call_graph");
	bool foundSyncHelper = false;
	bool foundRelayEdge = false;
	for (const Json& edge : RequireArray(callGraph, "edges"))
	{
		const std::string kind = RequireString(edge, "kind");
		if (kind == "Synchronous" &&
			RequireString(edge, "callee").find("::helper(") !=
				std::string::npos)
		{
			foundSyncHelper = true;
			CHECK(RequireField(edge, "resolved") == true);
		}
		foundRelayEdge = foundRelayEdge || kind == "Relay";
	}
	CHECK(foundSyncHelper);
	CHECK(foundRelayEdge);
	const Json& callAnalysis = RequireField(callGraph, "analysis");
	const Json& relayReachable = RequireField(callAnalysis, "relay_reachable");
	CHECK(relayReachable.is_object());
	CHECK(RequireField(relayReachable, RequireString(*send, "function_id").c_str()) == true);

	bool hasFunctionRegion = false;
	bool hasBranchRegion = false;
	bool hasLoopRegion = false;
	bool hasCallRegion = false;
	bool hasHandlerRegion = false;
	bool hasRelayRegion = false;
	for (const Json& region : RequireArray(controlFlow, "region_effects"))
	{
		const std::string kind = RequireString(region, "kind");
		hasFunctionRegion |= kind == "Function";
		hasBranchRegion |= kind == "BranchArm";
		hasLoopRegion |= kind == "LoopBody";
		hasCallRegion |= kind == "SynchronousCallSite";
		hasHandlerRegion |= kind == "RelayHandler";
		hasRelayRegion |= kind == "RelayRegion";
	}
	CHECK(hasFunctionRegion);
	CHECK(hasBranchRegion);
	CHECK(hasLoopRegion);
	CHECK(hasCallRegion);
	CHECK(hasHandlerRegion);
	CHECK(hasRelayRegion);

	const Json& icfg = RequireField(controlFlow, "relay_icfg");
	bool hasSyncCall = false;
	bool hasSyncReturn = false;
	bool hasAsyncSpawn = false;
	for (const Json& edge : RequireArray(icfg, "interprocedural_edges"))
	{
		const std::string kind = RequireString(edge, "kind");
		hasSyncCall |= kind == "SyncCall";
		hasSyncReturn |= kind == "SyncReturn";
		hasAsyncSpawn |= kind == "AsyncRelaySpawn";
		if (kind == "AsyncRelaySpawn")
			CHECK(!RequireString(edge, "relay_site_id").empty());
	}
	CHECK(hasSyncCall);
	CHECK(hasSyncReturn);
	CHECK(hasAsyncSpawn);
	CHECK(!RequireArray(icfg, "function_analyses").empty());

	std::vector<std::string> sendSites;
	for (const Json& site : RequireArray(result.manifest, "relay_sites"))
		if (RequireString(site, "source_function") == "send")
			sendSites.push_back(RequireString(site, "id"));
	CHECK(sendSites.size() == 2);
	bool foundExclusivePair = false;
	for (const Json& relation : RequireArray(icfg, "relay_site_relations"))
	{
		const std::string first =
			RequireString(relation, "first_relay_site_id");
		const std::string second =
			RequireString(relation, "second_relay_site_id");
		if (((first == sendSites[0] && second == sendSites[1]) ||
			 (first == sendSites[1] && second == sendSites[0])) &&
			RequireString(relation, "status") == "MutuallyExclusive")
		{
			foundExclusivePair = true;
		}
	}
	CHECK(foundExclusivePair);
}

void TestGeneratedLambdaCFG(const std::string& fixtureDirectory)
{
	const CompileResult result =
		CompileFixture(fixtureDirectory, "nested_lambda.prd");
	const Json& controlFlow = RequireControlFlow(result.manifest);
	size_t lambdaFunctions = 0;
	for (const Json& function : RequireArray(controlFlow, "functions"))
	{
		if (RequireField(function, "generated_relay_lambda") == true)
		{
			++lambdaFunctions;
			CHECK(CFGHasNodeKind(function, "RelayEmit") ||
				RequireString(function, "function").find("__relaylambda_") !=
					std::string::npos);
		}
	}
	CHECK(lambdaFunctions == 2);
	const Json& icfg = RequireField(controlFlow, "relay_icfg");
	size_t asyncSpawns = 0;
	for (const Json& edge : RequireArray(icfg, "interprocedural_edges"))
	{
		if (RequireString(edge, "kind") == "AsyncRelaySpawn")
		{
			++asyncSpawns;
			CHECK(RequireField(edge, "resolved") == true);
		}
	}
	CHECK(asyncSpawns == 2);
}

void TestSynchronousCallGraphRecursion(const std::string& fixtureDirectory)
{
	const CompileResult result =
		CompileFixture(fixtureDirectory, "cfg_recursive_call.prd");
	const Json& controlFlow = RequireControlFlow(result.manifest);
	const Json& callGraph =
		RequireField(controlFlow, "synchronous_call_graph");
	const Json& analysis = RequireField(callGraph, "analysis");
	const Json* recurse = FindCFGFunctionByName(controlFlow, "recurse");
	CHECK(recurse != nullptr);
	const std::string recurseId = RequireString(*recurse, "function_id");
	CHECK(JsonArrayContains(
		RequireArray(analysis, "recursive_functions"), recurseId));
	bool foundSelfEdge = false;
	for (const Json& edge : RequireArray(callGraph, "edges"))
	{
		if (RequireString(edge, "kind") == "Synchronous" &&
			RequireString(edge, "caller") == recurseId &&
			RequireString(edge, "callee") == recurseId)
		{
			foundSelfEdge = true;
		}
	}
	CHECK(foundSelfEdge);
	CHECK(CFGHasNodeKind(*recurse, "SynchronousCall"));
}

void TestOverloadedSynchronousCalls(const std::string& fixtureDirectory)
{
	const CompileResult result =
		CompileFixture(fixtureDirectory, "cfg_overloaded_calls.prd");
	const Json& controlFlow = RequireControlFlow(result.manifest);
	const Json& callGraph =
		RequireField(controlFlow, "synchronous_call_graph");
	std::set<std::string> helperCallees;
	for (const Json& edge : RequireArray(callGraph, "edges"))
	{
		if (RequireString(edge, "kind") == "Synchronous" &&
			RequireString(edge, "callee").find("::helper(") !=
				std::string::npos)
		{
			helperCallees.insert(RequireString(edge, "callee"));
			CHECK(RequireField(edge, "resolved") == true);
		}
	}
	CHECK(helperCallees.size() == 2);
	CHECK(helperCallees.count(
		"RelayProtocolTests.ProtocolCFGOverloadedCalls::helper(int32)") == 1);
	CHECK(helperCallees.count(
		"RelayProtocolTests.ProtocolCFGOverloadedCalls::helper(uint32)") == 1);
}

void TestNestedLoopTargets(const std::string& fixtureDirectory)
{
	const CompileResult result =
		CompileFixture(fixtureDirectory, "cfg_nested_loop.prd");
	const Json& controlFlow = RequireControlFlow(result.manifest);
	const Json* run = FindCFGFunctionByName(controlFlow, "run");
	CHECK(run != nullptr);

	const Json* breakNode = nullptr;
	const Json* continueNode = nullptr;
	for (const Json& node : RequireArray(*run, "nodes"))
	{
		const std::string kind = RequireString(node, "kind");
		if (kind == "Break")
			breakNode = &node;
		else if (kind == "Continue")
			continueNode = &node;
	}
	CHECK(breakNode != nullptr);
	CHECK(continueNode != nullptr);
	CHECK(RequireArray(*breakNode, "enclosing_loop_ids").size() == 2);
	CHECK(RequireArray(*continueNode, "enclosing_loop_ids").size() == 1);

	auto findNode = [&](const std::string& id) -> const Json*
	{
		for (const Json& node : RequireArray(*run, "nodes"))
			if (RequireString(node, "id") == id)
				return &node;
		return nullptr;
	};
	const Json* breakTarget = nullptr;
	const Json* continueTarget = nullptr;
	for (const Json& edge : RequireArray(*run, "edges"))
	{
		if (RequireString(edge, "source") == RequireString(*breakNode, "id") &&
			RequireString(edge, "kind") == "BreakExit")
		{
			breakTarget = findNode(RequireString(edge, "target"));
		}
		if (RequireString(edge, "source") == RequireString(*continueNode, "id") &&
			RequireString(edge, "kind") == "ContinueBack")
		{
			continueTarget = findNode(RequireString(edge, "target"));
		}
	}
	CHECK(breakTarget != nullptr);
	CHECK(RequireArray(*breakTarget, "enclosing_loop_ids").size() == 1);
	CHECK(continueTarget != nullptr);
	CHECK(RequireString(*continueTarget, "kind") == "LoopLatch");

	const Json& effect = RequireField(*run, "effect");
	CHECK(RequireField(effect, "writes_current_scope_state") == true);
	CHECK(RequireField(effect, "writes_local_state") == true);
	CHECK(RequireField(effect, "modifies_loop_induction_variable") == true);
	CHECK(RequireField(effect, "may_break") == true);
	CHECK(RequireField(effect, "may_continue") == true);
}

void TestRuntimeFailureHelper(const std::string& fixtureDirectory)
{
	const CompileResult result =
		CompileFixture(fixtureDirectory, "cfg_abort_helper.prd");
	const Json& controlFlow = RequireControlFlow(result.manifest);
	const Json* check = FindCFGFunctionByName(controlFlow, "check");
	CHECK(check != nullptr);
	CHECK(CFGHasNodeKind(*check, "AbortOrFailure"));
	CHECK(CFGHasEdgeKind(*check, "ExceptionalOrFailure"));
	const Json& effect = RequireField(*check, "effect");
	CHECK(RequireField(effect, "may_abort_or_fail") == true);
	CHECK(RequireString(effect, "status") != "Complete");

	bool foundRuntimeHelper = false;
	for (const Json& edge : RequireArray(
		RequireField(controlFlow, "synchronous_call_graph"), "edges"))
	{
		if (RequireString(edge, "kind") == "RuntimeHelper")
			foundRuntimeHelper = true;
	}
	CHECK(foundRuntimeHelper);
}

void TestUnknownCallAndOpaqueEffects(const std::string&)
{
	namespace cfg = transpiler::relay_protocol::cfg;
	const std::string functionId = "RelayProtocolTests.Synthetic::caller()";

	cfg::PredaFunctionCFG function;
	function.functionId = functionId;
	function.function = "caller";
	function.entryNodeId = "entry";
	function.exitNodeId = "exit";
	cfg::PredaCFGNode opaque;
	opaque.id = "opaque";
	opaque.kind = cfg::PredaCFGNodeKind::Opaque;
	opaque.sourceFunctionId = functionId;
	opaque.supported = false;
	opaque.unsupportedReason = "synthetic opaque statement";
	opaque.directEffect.mayCallUnknown = true;
	opaque.directEffect.mayHaveExternalEffect = true;
	opaque.directEffect.status = cfg::AnalysisStatus::Unknown;
	opaque.directEffect.reason = "synthetic opaque statement";
	function.nodes.push_back(std::move(opaque));

	std::vector<cfg::PredaFunctionCFG> functions{function};
	cfg::PredaSynchronousCallGraph graph;
	graph.functions.push_back(functionId);
	cfg::PredaCallEdge external;
	external.id = "external-call";
	external.caller = functionId;
	external.callee = "External.Contract::unknown()";
	external.kind = cfg::PredaCallKind::ExternalUnknown;
	external.resolved = false;
	external.unresolvedReason = "no local PREDA body";
	graph.edges.push_back(std::move(external));
	cfg::PredaCallGraphAnalyzer::Analyze(graph, functions);
	CHECK(graph.analysis.hasUnresolvedOutgoing.at(functionId));

	transpiler::relay_protocol::RelayProtocolIR protocol;
	const std::vector<cfg::PredaRegionEffect> regions =
		cfg::PredaEffectAnalysis::Build(functions, graph, protocol);
	(void)regions;
	CHECK(functions.front().effect.mayCallUnknown);
	CHECK(functions.front().effect.mayHaveExternalEffect);
	CHECK(functions.front().effect.status == cfg::AnalysisStatus::Unknown);
}

void TestForUpdateCallCFG(const std::string& fixtureDirectory)
{
	const CompileResult result =
		CompileFixture(fixtureDirectory, "cfg_for_update_call.prd");
	const Json& controlFlow = RequireControlFlow(result.manifest);
	const Json* run = FindCFGFunctionByName(controlFlow, "run");
	CHECK(run != nullptr);
	const Json* updateCall = nullptr;
	const Json* continueNode = nullptr;
	for (const Json& node : RequireArray(*run, "nodes"))
	{
		if (RequireString(node, "kind") == "SynchronousCall" &&
			RequireString(node, "callee_function_id").find("::step(") !=
				std::string::npos)
		{
			updateCall = &node;
		}
		if (RequireString(node, "kind") == "Continue")
			continueNode = &node;
	}
	CHECK(updateCall != nullptr);
	CHECK(continueNode != nullptr);
	CHECK(RequireArray(*updateCall, "enclosing_loop_ids").size() == 1);
	bool continueRunsUpdate = false;
	for (const Json& edge : RequireArray(*run, "edges"))
	{
		if (RequireString(edge, "kind") == "ContinueBack" &&
			RequireString(edge, "source") == RequireString(*continueNode, "id") &&
			RequireString(edge, "target") == RequireString(*updateCall, "id"))
		{
			continueRunsUpdate = true;
		}
	}
	CHECK(continueRunsUpdate);
	bool foundICFGCall = false;
	for (const Json& edge : RequireArray(
		RequireField(controlFlow, "relay_icfg"), "interprocedural_edges"))
	{
		if (RequireString(edge, "kind") == "SyncCall" &&
			RequireString(edge, "source_node_id") ==
				RequireString(*updateCall, "id"))
		{
			foundICFGCall = true;
		}
	}
	CHECK(foundICFGCall);
	CHECK(RequireField(
		RequireField(*run, "effect"),
		"writes_current_scope_state") == true);
}

void TestRelayOperandStateEffects(const std::string& fixtureDirectory)
{
	const CompileResult result =
		CompileFixture(fixtureDirectory, "cfg_relay_state_effect.prd");
	const Json& controlFlow = RequireControlFlow(result.manifest);
	for (const std::string& functionName : {"send_named", "send_lambda"})
	{
		const Json* function =
			FindCFGFunctionByName(controlFlow, functionName);
		CHECK(function != nullptr);
		const Json& effect = RequireField(*function, "effect");
		CHECK(RequireField(effect, "reads_current_scope_state") == true);
		CHECK(RequireArray(effect, "read_state_variables").size() >= 2);
		bool relayReadsState = false;
		for (const Json& node : RequireArray(*function, "nodes"))
		{
			if (RequireString(node, "kind") == "RelayEmit" &&
				RequireField(
					RequireField(node, "direct_effect"),
					"reads_current_scope_state") == true)
			{
				relayReadsState = true;
			}
		}
		CHECK(relayReadsState);
	}
}

void TestCallEvaluationOrderConservatism(const std::string& fixtureDirectory)
{
	const CompileResult result =
		CompileFixture(fixtureDirectory, "cfg_call_order.prd");
	const Json& controlFlow = RequireControlFlow(result.manifest);
	const Json* nested = FindCFGFunctionByName(controlFlow, "nested");
	const Json* siblings = FindCFGFunctionByName(controlFlow, "siblings");
	CHECK(nested != nullptr);
	CHECK(siblings != nullptr);

	std::string innerNodeId;
	std::string consumeNodeId;
	for (const Json& node : RequireArray(*nested, "nodes"))
	{
		if (RequireString(node, "kind") != "SynchronousCall")
			continue;
		const std::string callee = RequireString(node, "callee_function_id");
		if (callee.find("::inner(") != std::string::npos)
			innerNodeId = RequireString(node, "id");
		if (callee.find("::consume_one(") != std::string::npos)
			consumeNodeId = RequireString(node, "id");
	}
	CHECK(!innerNodeId.empty());
	CHECK(!consumeNodeId.empty());
	bool innerBeforeOuter = false;
	for (const Json& edge : RequireArray(*nested, "edges"))
	{
		if (RequireString(edge, "source") == innerNodeId &&
			RequireString(edge, "target") == consumeNodeId &&
			RequireString(edge, "kind") == "Fallthrough" &&
			RequireField(edge, "supported") == true)
		{
			innerBeforeOuter = true;
		}
	}
	CHECK(innerBeforeOuter);

	CHECK(RequireString(*siblings, "status") != "Complete");
	CHECK(CFGHasEdgeKind(*siblings, "Unknown"));
	const Json* siblingAnalysis = FindFunctionGraphAnalysis(
		controlFlow,
		RequireString(*siblings, "function_id"));
	CHECK(siblingAnalysis != nullptr);
	CHECK(RequireField(*siblingAnalysis, "reachability_complete") == false);
}

void TestNonterminatingPostDominance(const std::string& fixtureDirectory)
{
	const CompileResult result =
		CompileFixture(fixtureDirectory, "cfg_nonterminating_for.prd");
	const Json& controlFlow = RequireControlFlow(result.manifest);
	const Json* spin = FindCFGFunctionByName(controlFlow, "spin");
	CHECK(spin != nullptr);
	const Json* analysis = FindFunctionGraphAnalysis(
		controlFlow,
		RequireString(*spin, "function_id"));
	CHECK(analysis != nullptr);
	CHECK(RequireField(*analysis, "post_dominance_complete") == false);
	CHECK(RequireString(*analysis, "status") != "Complete");
	CHECK(RequireField(*analysis, "control_dependents").empty());

	const Json* maybeSpin =
		FindCFGFunctionByName(controlFlow, "maybe_spin");
	CHECK(maybeSpin != nullptr);
	const Json* maybeAnalysis = FindFunctionGraphAnalysis(
		controlFlow,
		RequireString(*maybeSpin, "function_id"));
	CHECK(maybeAnalysis != nullptr);
	CHECK(RequireField(*maybeAnalysis, "reachability_complete") == true);
	CHECK(RequireField(*maybeAnalysis, "post_dominance_complete") == false);
	CHECK(RequireField(*maybeAnalysis, "control_dependents").empty());
}

void TestSynchronousRelayComposition(const std::string& fixtureDirectory)
{
	const CompileResult result =
		CompileFixture(fixtureDirectory, "cfg_sync_composition.prd");
	const Json& controlFlow = RequireControlFlow(result.manifest);
	std::vector<std::pair<int64_t, std::string>> sendSites;
	std::string helperSite;
	std::string sendMultiSite;
	std::string helperMultiSite;
	std::string helperLeftSite;
	std::string helperRightSite;
	for (const Json& site : RequireArray(result.manifest, "relay_sites"))
	{
		if (RequireString(site, "source_function") == "send")
		{
			sendSites.emplace_back(
				RequireField(
					RequireField(site, "location"),
					"start_offset").get<int64_t>(),
				RequireString(site, "id"));
		}
		if (RequireString(site, "source_function") == "helper")
			helperSite = RequireString(site, "id");
		if (RequireString(site, "source_function") == "send_multi")
			sendMultiSite = RequireString(site, "id");
		if (RequireString(site, "source_function") == "helper_multi")
			helperMultiSite = RequireString(site, "id");
		if (RequireString(site, "source_function") == "helper_left")
			helperLeftSite = RequireString(site, "id");
		if (RequireString(site, "source_function") == "helper_right")
			helperRightSite = RequireString(site, "id");
	}
	std::sort(sendSites.begin(), sendSites.end());
	CHECK(sendSites.size() == 2);
	CHECK(!helperSite.empty());
	CHECK(!sendMultiSite.empty());
	CHECK(!helperMultiSite.empty());
	CHECK(!helperLeftSite.empty());
	CHECK(!helperRightSite.empty());
	const std::string& firstSendSite = sendSites.front().second;
	const std::string& afterCallSite = sendSites.back().second;
	bool composed = false;
	for (const Json& relation : RequireArray(
		RequireField(controlFlow, "relay_icfg"), "relay_site_relations"))
	{
		const std::string first =
			RequireString(relation, "first_relay_site_id");
		const std::string second =
			RequireString(relation, "second_relay_site_id");
		if (((first == firstSendSite && second == helperSite) ||
			 (first == helperSite && second == firstSendSite)) &&
			RequireString(relation, "status") == "CoReachable")
		{
			composed = true;
		}
	}
	CHECK(composed);
	bool multipleContextStayedUnknown = false;
	for (const Json& relation : RequireArray(
		RequireField(controlFlow, "relay_icfg"), "relay_site_relations"))
	{
		const std::string first =
			RequireString(relation, "first_relay_site_id");
		const std::string second =
			RequireString(relation, "second_relay_site_id");
		if (((first == sendMultiSite && second == helperMultiSite) ||
			 (first == helperMultiSite && second == sendMultiSite)) &&
			RequireString(relation, "status") == "Unknown")
		{
			multipleContextStayedUnknown = true;
		}
	}
	CHECK(multipleContextStayedUnknown);
	bool branchHelpersExclusive = false;
	for (const Json& relation : RequireArray(
		RequireField(controlFlow, "relay_icfg"), "relay_site_relations"))
	{
		const std::string first =
			RequireString(relation, "first_relay_site_id");
		const std::string second =
			RequireString(relation, "second_relay_site_id");
		if (((first == helperLeftSite && second == helperRightSite) ||
			 (first == helperRightSite && second == helperLeftSite)) &&
			RequireString(relation, "status") == "MutuallyExclusive")
		{
			branchHelpersExclusive = true;
		}
	}
	CHECK(branchHelpersExclusive);

	const Json* sendFunction = FindCFGFunctionByName(controlFlow, "send");
	CHECK(sendFunction != nullptr);
	CHECK(RequireField(
		RequireField(*sendFunction, "effect"),
		"may_abort_or_fail") == true);
	const std::string sendFunctionId =
		RequireString(*sendFunction, "function_id");
	const Json& icfg = RequireField(controlFlow, "relay_icfg");
	const Json& syncReachable =
		RequireField(icfg, "synchronous_reachable_functions");
	const Json& sendClosure = RequireField(
		syncReachable,
		sendFunctionId.c_str());
	bool reachesBridge = false;
	bool reachesHelper = false;
	for (const Json& functionId : sendClosure)
	{
		const std::string id = functionId.get<std::string>();
		reachesBridge |= id.find("::bridge(") != std::string::npos;
		reachesHelper |= id.find("::helper(") != std::string::npos;
	}
	CHECK(reachesBridge);
	CHECK(reachesHelper);

	const Json* composition = nullptr;
	for (const Json& candidate :
		RequireArray(icfg, "synchronous_composition_analyses"))
	{
		if (RequireString(candidate, "root_function_id") == sendFunctionId)
		{
			composition = &candidate;
			break;
		}
	}
	CHECK(composition != nullptr);
	CHECK(RequireField(*composition, "reachability_complete") == true);
	CHECK(RequireField(*composition, "dominance_complete") == true);
	CHECK(!RequireField(*composition, "reachable_nodes").empty());
	CHECK(!RequireField(*composition, "dominators").empty());

	const Json* helperFunction = FindCFGFunctionByName(controlFlow, "helper");
	CHECK(helperFunction != nullptr);
	std::string terminalFailureNode;
	for (const Json& node : RequireArray(*helperFunction, "nodes"))
	{
		if (RequireString(node, "kind") == "AbortOrFailure" &&
			RequireArray(node, "successors").empty())
		{
			terminalFailureNode = RequireString(node, "id");
		}
	}
	CHECK(!terminalFailureNode.empty());
	std::string afterCallNode;
	for (const Json& node : RequireArray(*sendFunction, "nodes"))
	{
		if (RequireString(node, "kind") == "RelayEmit")
		{
			const Json& siteId = RequireField(node, "relay_site_id");
			if (siteId.is_string() &&
				siteId.get<std::string>() == afterCallSite)
			{
				afterCallNode = RequireString(node, "id");
			}
		}
	}
	CHECK(!afterCallNode.empty());
	const Json& failureReachable = RequireField(
		RequireField(*composition, "reachable_nodes"),
		terminalFailureNode.c_str());
	CHECK(!JsonArrayContains(failureReachable, afterCallNode));
}

void TestLambdaSynchronousEffectComposition(const std::string& fixtureDirectory)
{
	const CompileResult result =
		CompileFixture(fixtureDirectory, "cfg_lambda_sync_effect.prd");
	const Json& controlFlow = RequireControlFlow(result.manifest);
	const Json* lambda = nullptr;
	for (const Json& function : RequireArray(controlFlow, "functions"))
	{
		if (RequireField(function, "generated_relay_lambda") == true)
		{
			lambda = &function;
			break;
		}
	}
	CHECK(lambda != nullptr);
	CHECK(CFGHasNodeKind(*lambda, "SynchronousCall"));
	CHECK(RequireField(
		RequireField(*lambda, "effect"),
		"writes_current_scope_state") == true);
	bool hasLambdaSyncCall = false;
	for (const Json& edge : RequireArray(
		RequireField(controlFlow, "relay_icfg"), "interprocedural_edges"))
	{
		if (RequireString(edge, "kind") == "SyncCall" &&
			RequireString(edge, "source_function_id") ==
				RequireString(*lambda, "function_id"))
		{
			hasLambdaSyncCall = true;
		}
	}
	CHECK(hasLambdaSyncCall);
}

void TestMutualRecursiveEffectFixedPoint(const std::string& fixtureDirectory)
{
	const CompileResult result = CompileFixture(
		fixtureDirectory,
		"cfg_mutual_recursion_effect.prd");
	const Json& controlFlow = RequireControlFlow(result.manifest);
	const Json* left = FindCFGFunctionByName(controlFlow, "left");
	const Json* right = FindCFGFunctionByName(controlFlow, "right");
	const Json* run = FindCFGFunctionByName(controlFlow, "run");
	CHECK(left != nullptr);
	CHECK(right != nullptr);
	CHECK(run != nullptr);
	for (const Json* function : {left, right, run})
	{
		CHECK(RequireField(
			RequireField(*function, "effect"),
			"writes_current_scope_state") == true);
	}

	const Json& analysis = RequireField(
		RequireField(controlFlow, "synchronous_call_graph"),
		"analysis");
	const std::string leftId = RequireString(*left, "function_id");
	const std::string rightId = RequireString(*right, "function_id");
	CHECK(JsonArrayContains(RequireArray(analysis, "recursive_functions"), leftId));
	CHECK(JsonArrayContains(RequireArray(analysis, "recursive_functions"), rightId));
	bool foundMutualScc = false;
	for (const Json& component :
		RequireArray(analysis, "strongly_connected_components"))
	{
		if (JsonArrayContains(component, leftId) &&
			JsonArrayContains(component, rightId))
		{
			foundMutualScc = true;
		}
	}
	CHECK(foundMutualScc);
}

void TestOpaqueCFGIsNotComplete(const std::string& fixtureDirectory)
{
	const CompileResult result = CompileFixture(
		fixtureDirectory,
		"dependency_opaque_effect.prd");
	const Json& controlFlow = RequireControlFlow(result.manifest);
	const Json* send = FindCFGFunctionByName(controlFlow, "send");
	CHECK(send != nullptr);
	CHECK(RequireString(*send, "status") != "Complete");
	CHECK(CFGHasNodeKind(*send, "Opaque"));
	const Json* analysis = FindFunctionGraphAnalysis(
		controlFlow,
		RequireString(*send, "function_id"));
	CHECK(analysis != nullptr);
	CHECK(RequireField(*analysis, "reachability_complete") == false);
	CHECK(RequireField(*analysis, "dominance_complete") == false);
}

void TestElseIfCFG(const std::string& fixtureDirectory)
{
	const CompileResult result =
		CompileFixture(fixtureDirectory, "else_if_chain.prd");
	const Json& controlFlow = RequireControlFlow(result.manifest);
	const Json* send = FindCFGFunctionByName(controlFlow, "send");
	CHECK(send != nullptr);
	size_t branches = 0;
	for (const Json& node : RequireArray(*send, "nodes"))
		if (RequireString(node, "kind") == "Branch")
			++branches;
	CHECK(branches == 3);
	CHECK(CFGHasEdgeKind(*send, "TrueBranch"));
	CHECK(CFGHasEdgeKind(*send, "FalseBranch"));
}

void TestImplicitFailureCFG(const std::string& fixtureDirectory)
{
	const CompileResult result =
		CompileFixture(fixtureDirectory, "cfg_implicit_failure.prd");
	const Json& controlFlow = RequireControlFlow(result.manifest);
	const Json* divide = FindCFGFunctionByName(controlFlow, "divide");
	CHECK(divide != nullptr);
	CHECK(RequireString(*divide, "status") != "Complete");
	CHECK(CFGHasNodeKind(*divide, "AbortOrFailure"));
	CHECK(CFGHasEdgeKind(*divide, "ExceptionalOrFailure"));
	CHECK(RequireField(
		RequireField(*divide, "effect"),
		"may_abort_or_fail") == true);
	const Json* analysis = FindFunctionGraphAnalysis(
		controlFlow,
		RequireString(*divide, "function_id"));
	CHECK(analysis != nullptr);
	CHECK(RequireField(*analysis, "post_dominance_complete") == false);
}

void TestParallelCertificatePairRelations(
	const std::string& fixtureDirectory)
{
	const CompileResult result = CompileFixture(
		fixtureDirectory, "certificate/pair_relations.prd");
	const Json& straight = RequireParallelFunction(
		result.manifest, "straight_line");
	bool provedPrecedence = false;
	std::ostringstream observedStraight;
	for (const Json& pair : RequireArray(straight, "pair_relations"))
	{
		CheckUniqueEvidence(pair, "straight-line pair evidence");
		const std::string relation = RequireString(pair, "relation");
		const std::string status = RequireString(pair, "status");
		observedStraight << " relation=" << relation
			<< " status=" << status
			<< " reason=" << RequireString(pair, "reason");
		provedPrecedence |= relation == "MustPrecedeAB" &&
			status == "Proved";
	}
	const Json& controlFlow = RequireControlFlow(result.manifest);
	const std::string straightId =
		RequireString(straight, "source_function_id");
	for (const Json& analysis : RequireArray(
		RequireField(controlFlow, "relay_icfg"),
		"function_analyses"))
	{
		if (RequireString(analysis, "function_id") == straightId)
		{
			observedStraight << " analysis_status="
				<< RequireString(analysis, "status")
				<< " dominance_complete="
				<< RequireField(analysis, "dominance_complete")
				<< " reachability_complete="
				<< RequireField(analysis, "reachability_complete")
				<< " completeness_reasons="
				<< RequireField(analysis, "completeness_reasons").dump();
		}
	}
	CHECK_DETAIL(provedPrecedence, observedStraight.str());

	for (const char* functionName : {
		"distinct_arithmetic", "same_target", "exclusive"})
	{
		const Json& function = RequireParallelFunction(
			result.manifest, functionName);
		for (const Json& pair : RequireArray(function, "pair_relations"))
			CheckUniqueEvidence(
				pair, std::string(functionName) + " pair evidence");
	}

#ifdef RPREDA_ENABLE_Z3
	auto requireRelation = [&](const char* functionName,
		const char* expectedRelation,
		bool requireCounterexample = false)
	{
		const Json& function = RequireParallelFunction(
			result.manifest, functionName);
		std::ostringstream observed;
		for (const Json& pair : RequireArray(function, "pair_relations"))
		{
			const std::string relation = RequireString(pair, "relation");
			const std::string status = RequireString(pair, "status");
			observed << " relation=" << relation
				<< " status=" << status
				<< " reason=" << RequireString(pair, "reason");
			if (relation == expectedRelation && status == "Proved")
			{
				if (requireCounterexample)
					CHECK(!RequireField(pair, "counterexample").is_null());
				return;
			}
		}
		throw TestFailure(
			std::string("missing ") + expectedRelation + " for " +
			functionName + ":" + observed.str());
	};
	requireRelation("distinct_arithmetic", "CoEmissionIndependent");
	requireRelation("same_target", "ProvedMayAlias", true);
	requireRelation("exclusive", "MutuallyExclusive");
#endif
}

void TestParallelCertificateWorkSoundness(
	const std::string& fixtureDirectory)
{
	const CompileResult result = CompileFixture(
		fixtureDirectory, "certificate/control_flow.prd");
	const Json& early =
		RequireParallelFunction(result.manifest, "early_return");
	const Json& earlyDirect = RequireField(early, "direct_logical_work");
	CHECK_DETAIL(
		RequireString(earlyDirect, "status") == "Conservative",
		"early direct status=" + RequireString(earlyDirect, "status") +
			", reason=" + RequireString(earlyDirect, "reason"));
	CHECK(RequireString(RequireField(earlyDirect, "exact"), "kind") == "unknown");
	CheckConstant(RequireField(earlyDirect, "upper_bound"), 2, "early-return upper bound");

	const Json& before = RequireParallelFunction(
		result.manifest, "return_before_only_relay");
	const Json& depth = RequireField(before, "relay_tree_depth");
	CHECK(RequireString(depth, "status") == "Conservative");
	CHECK(RequireString(RequireField(depth, "exact"), "kind") == "unknown");
	CheckConstant(RequireField(depth, "upper_bound"), 1, "early-return depth upper bound");

	const Json& bounded = RequireParallelFunction(
		result.manifest, "bounded_occurrences");
	CheckConstant(
		RequireField(
			RequireField(bounded, "direct_logical_work"), "upper_bound"),
		3,
		"u32-suffixed loop direct upper bound");
	CheckConstant(
		RequireField(RequireField(bounded, "transitive_logical_work"), "upper_bound"),
		3,
		"u32-suffixed loop transitive bound: " +
			RequireField(bounded, "transitive_logical_work").dump());
	CHECK(RequireField(
		RequireField(bounded, "physical_route_work"), "constant_term") == 3);

	const Json& loopPair = RequireParallelFunction(
		result.manifest, "loop_pair");
	bool rejectedLoopOccurrence = false;
	for (const Json& pair : RequireArray(loopPair, "pair_relations"))
	{
		rejectedLoopOccurrence |=
			RequireString(pair, "relation") == "Unknown" &&
			RequireString(pair, "reason").find("loop occurrence") !=
				std::string::npos;
	}
	CHECK(rejectedLoopOccurrence);
}

void TestParallelCertificateConservativeContexts(
	const std::string& fixtureDirectory)
{
	const CompileResult sync = CompileFixture(
		fixtureDirectory, "certificate/sync_contexts.prd");
	const Json& repeated = RequireParallelFunction(
		sync.manifest, "repeated_context");
	bool rejectedAmbiguousContext = false;
	for (const Json& pair : RequireArray(repeated, "pair_relations"))
	{
		rejectedAmbiguousContext |=
			RequireString(pair, "relation") == "Unknown" &&
			RequireString(pair, "reason").find("multiple call occurrences") !=
				std::string::npos;
	}
	CHECK(rejectedAmbiguousContext);
	for (const char* field : {
		"direct_logical_work", "transitive_logical_work",
		"physical_route_work", "relay_tree_depth"})
	{
		CheckUniqueEvidence(RequireField(repeated, field), field);
	}

	const CompileResult work = CompileFixture(
		fixtureDirectory, "certificate/work_depth.prd");
	const Json& nested = RequireParallelFunction(
		work.manifest, "nested_broadcast");
	CHECK(RequireString(
		RequireField(nested, "transitive_logical_work"), "status") == "Unknown");
	CHECK(RequireString(
		RequireField(nested, "physical_route_work"), "status") == "Unknown");
	CHECK(RequireString(nested, "certificate_status") != "Complete");

	const Json& root = RequireParallelFunction(work.manifest, "root");
	CHECK(RequireArray(root, "pair_relations").empty());
	const Json& recursive = RequireParallelFunction(
		work.manifest, "recursive_root");
	CHECK(RequireString(
		RequireField(recursive, "transitive_logical_work"), "status") ==
		"Unknown");
	CHECK(RequireString(
		RequireField(recursive, "relay_tree_depth"), "status") == "Unknown");
}

#ifdef RPREDA_ENABLE_Z3
namespace refinement =
	transpiler::relay_protocol::refinement;
namespace solver =
	transpiler::relay_protocol::refinement::solver;
namespace z3_backend =
	transpiler::relay_protocol::refinement::solver::z3_backend;

refinement::RelayRefinementSymbol MakeTestSymbol(
	const std::string& id,
	refinement::RelayRefinementSymbolKind kind,
	const refinement::FormulaSort& sort,
	const std::string& sourceType,
	const std::string& functionId = "solver_test::source()",
	const std::string& siteId = std::string())
{
	refinement::RelayRefinementSymbol symbol;
	symbol.id = id;
	symbol.kind = kind;
	symbol.sourceFunctionId = functionId;
	symbol.sourceName = id;
	symbol.sourceType = sourceType;
	symbol.sort = sort;
	symbol.relaySiteId = siteId;
	return symbol;
}

refinement::RelayProofObligation MakeBooleanGoal(
	const std::string& id,
	refinement::FormulaExpr goal)
{
	refinement::RelayProofObligation obligation;
	obligation.id = id;
	obligation.kind =
		refinement::RelayProofObligationKind::BooleanRefinement;
	obligation.status =
		refinement::RelayProofObligationStatus::Generated;
	obligation.role =
		refinement::RelayProofObligationRole::SolverGoal;
	obligation.sourceFunctionId = "solver_test::source()";
	obligation.goal = std::move(goal);
	return obligation;
}

refinement::RelayConstraint MakeSolverAssumption(
	const std::string& id,
	refinement::FormulaExpr formula)
{
	refinement::RelayConstraint constraint;
	constraint.id = id;
	constraint.kind =
		refinement::RelayConstraintKind::RelayGuardNecessity;
	constraint.role =
		refinement::RelayConstraintRole::SolverAssumption;
	constraint.sourceFunctionId = "solver_test::source()";
	constraint.formula = std::move(formula);
	return constraint;
}

solver::RelaySolverResult RunZ3Goal(
	const std::vector<refinement::RelayRefinementSymbol>& symbols,
	const std::vector<refinement::RelayConstraint>& constraints,
	const refinement::RelayProofObligation& obligation)
{
	z3_backend::Z3RelaySolver backend;
	solver::RelayProofRunner runner(&backend);
	return runner.RunOne(symbols, constraints, obligation);
}

void CheckManualSolverStatus(
	const solver::RelaySolverResult& result,
	solver::RelaySolverStatus expected,
	const std::string& detail)
{
	CHECK_DETAIL(
		result.status == expected,
		"unexpected solver result for " + detail
			+ ": backend=" + result.backend
			+ ", reason=" + result.reason);
}

void TestZ3ParallelCertificateHelperComposition(
	const std::string& fixtureDirectory)
{
	const CompileResult result = CompileFixture(
		fixtureDirectory, "certificate/sync_contexts.prd");
	const Json& exclusive = RequireParallelFunction(
		result.manifest, "exclusive_helper_context");
	bool provedComposedExclusion = false;
	std::ostringstream observedExclusive;
	for (const Json& pair : RequireArray(exclusive, "pair_relations"))
	{
		observedExclusive << " relation=" << RequireString(pair, "relation")
			<< " status=" << RequireString(pair, "status")
			<< " reason=" << RequireString(pair, "reason");
		provedComposedExclusion |=
			RequireString(pair, "relation") == "MutuallyExclusive" &&
			RequireString(pair, "status") == "Proved";
	}
	const Json& exclusiveControlFlow = RequireControlFlow(result.manifest);
	const Json& exclusiveIcfg = RequireField(
		exclusiveControlFlow, "relay_icfg");
	for (const Json& composition : RequireArray(
		exclusiveIcfg, "synchronous_composition_analyses"))
	{
		if (RequireString(composition, "root_function_id") ==
			RequireString(exclusive, "source_function_id"))
		{
			observedExclusive << " composition_status="
				<< RequireString(composition, "status")
				<< " dominance_complete="
				<< RequireField(composition, "dominance_complete")
				<< " reachability_complete="
				<< RequireField(composition, "reachability_complete")
				<< " completeness_reasons="
				<< RequireField(composition, "completeness_reasons").dump();
		}
	}
	for (const Json& function : RequireArray(
		exclusiveControlFlow, "functions"))
	{
		const std::string name = RequireString(function, "function");
		if (name == "exclusive_helper_context" ||
			name == "exclusive_only_helper")
		{
			observedExclusive << " function=" << name
				<< " status=" << RequireString(function, "status")
				<< " completeness_reasons="
				<< RequireField(function, "completeness_reasons").dump();
		}
	}
	CHECK_DETAIL(provedComposedExclusion, observedExclusive.str());

	const Json& unique = RequireParallelFunction(
		result.manifest, "unique_context");
	bool rejectedUnboundHelperTarget = false;
	for (const Json& pair : RequireArray(unique, "pair_relations"))
	{
		rejectedUnboundHelperTarget |=
			RequireString(pair, "status") == "Unsupported" &&
			RequireString(pair, "reason").find("formal/actual") !=
				std::string::npos;
	}
	CHECK(rejectedUnboundHelperTarget);
}

void TestZ3ConditionalCountUpperBound(
	const std::string& fixtureDirectory)
{
	const CompileResult result = CompileFixture(
		fixtureDirectory,
		"summary_conditional.prd");
	CheckTopLevel(result, "ProtocolSummaryConditional");

	const Json& site =
		RequireArray(result.manifest, "relay_sites").front();
	const std::string functionId =
		RequireString(site, "source_function_id");
	const Json& refinementJson =
		RequireRefinement(result.manifest);
	const Json* obligation = FindProofObligation(
		refinementJson,
		"RelayCountUpperBound",
		std::string(),
		functionId);
	CheckSolverStatus(
		obligation,
		"Proved",
		"conditional direct relay count <= 1");

	const Json& solverResult = RequireSolverResult(*obligation);
	CHECK(RequireString(solverResult, "backend") == "z3");
	const Json* countEquality = FindConstraint(
		refinementJson,
		"RelayCountEquality",
		std::string(),
		functionId);
	const Json* upperBound = FindConstraint(
		refinementJson,
		"RelayCountUpperBound",
		std::string(),
		functionId);
	CHECK(countEquality != nullptr);
	CHECK(upperBound != nullptr);
	const Json& assumptions =
		RequireArray(solverResult, "assumption_constraint_ids");
	CHECK(
		std::find(
			assumptions.begin(),
			assumptions.end(),
			RequireString(*countEquality, "id"))
		!= assumptions.end());
	CHECK(
		std::find(
			assumptions.begin(),
			assumptions.end(),
			RequireString(*upperBound, "id"))
		== assumptions.end());
}

void TestZ3SameTargetNonAliasCounterexample(
	const std::string&)
{
	const std::string functionId = "solver_test::source()";
	const std::string firstSite = "site.same_target.a";
	const std::string secondSite = "site.same_target.b";
	const refinement::FormulaSort address =
		refinement::FormulaSort::Address();

	const std::vector<refinement::RelayRefinementSymbol> symbols = {
		MakeTestSymbol(
			"param.target",
			refinement::RelayRefinementSymbolKind::
				SourceFunctionParameter,
			address,
			"address",
			functionId),
		MakeTestSymbol(
			"emit.a",
			refinement::RelayRefinementSymbolKind::RelayEmission,
			refinement::FormulaSort::Bool(),
			"bool",
			functionId,
			firstSite),
		MakeTestSymbol(
			"emit.b",
			refinement::RelayRefinementSymbolKind::RelayEmission,
			refinement::FormulaSort::Bool(),
			"bool",
			functionId,
			secondSite),
		MakeTestSymbol(
			"target.a",
			refinement::RelayRefinementSymbolKind::ActualRelayTarget,
			address,
			"address",
			functionId,
			firstSite),
		MakeTestSymbol(
			"target.b",
			refinement::RelayRefinementSymbolKind::ActualRelayTarget,
			address,
			"address",
			functionId,
			secondSite),
	};

	const refinement::FormulaExpr sourceTarget =
		refinement::FormulaExpr::Symbol("param.target", address);
	const refinement::FormulaExpr emittedA =
		refinement::FormulaExpr::Symbol(
			"emit.a",
			refinement::FormulaSort::Bool());
	const refinement::FormulaExpr emittedB =
		refinement::FormulaExpr::Symbol(
			"emit.b",
			refinement::FormulaSort::Bool());
	const refinement::FormulaExpr targetA =
		refinement::FormulaExpr::Symbol("target.a", address);
	const refinement::FormulaExpr targetB =
		refinement::FormulaExpr::Symbol("target.b", address);

	std::vector<refinement::RelayConstraint> constraints;
	for (const auto& relation :
		std::vector<std::pair<
			std::string,
			refinement::FormulaExpr>>{
			{
				firstSite,
				refinement::FormulaExpr::Binary(
					"implies",
					emittedA,
					refinement::FormulaExpr::Binary(
						"==",
						targetA,
						sourceTarget,
						refinement::FormulaSort::Bool()),
					refinement::FormulaSort::Bool()),
			},
			{
				secondSite,
				refinement::FormulaExpr::Binary(
					"implies",
					emittedB,
					refinement::FormulaExpr::Binary(
						"==",
						targetB,
						sourceTarget,
						refinement::FormulaSort::Bool()),
					refinement::FormulaSort::Bool()),
			},
		})
	{
		refinement::RelayConstraint constraint;
		constraint.id = "target_relation." + relation.first;
		constraint.kind =
			refinement::RelayConstraintKind::RelayTargetRelation;
		constraint.role =
			refinement::RelayConstraintRole::SemanticDefinition;
		constraint.sourceFunctionId = functionId;
		constraint.relaySiteId = relation.first;
		constraint.formula = relation.second;
		constraints.push_back(std::move(constraint));
	}

	refinement::RelayProofObligation obligation;
	obligation.id = "nonalias.same_target";
	obligation.kind =
		refinement::RelayProofObligationKind::
			TargetNonAliasCandidate;
	obligation.status =
		refinement::RelayProofObligationStatus::Generated;
	obligation.role =
		refinement::RelayProofObligationRole::SolverGoal;
	obligation.sourceFunctionId = functionId;
	obligation.relaySiteId = firstSite;
	obligation.relatedRelaySiteId = secondSite;
	obligation.goal = refinement::FormulaExpr::Binary(
		"implies",
		refinement::FormulaExpr::Nary(
			"&&",
			{ emittedA, emittedB },
			refinement::FormulaSort::Bool()),
		refinement::FormulaExpr::Binary(
			"!=",
			targetA,
			targetB,
			refinement::FormulaSort::Bool()),
		refinement::FormulaSort::Bool());

	const solver::RelaySolverResult solved =
		RunZ3Goal(symbols, constraints, obligation);
	CheckManualSolverStatus(
		solved,
		solver::RelaySolverStatus::Disproved,
		"same-target non-alias candidate");
	CHECK_DETAIL(
		!solved.projectedCounterexample.empty(),
		"disproved non-alias goal did not include a counterexample");
}

void TestZ3IfElseGuardedTargets(
	const std::string& fixtureDirectory)
{
	const CompileResult result =
		CompileFixture(fixtureDirectory, "if_else.prd");
	CheckTopLevel(result, "ProtocolIfElse");
	const Json& refinementJson =
		RequireRefinement(result.manifest);
	const Json* obligation = FindProofObligation(
		refinementJson,
		"TargetNonAliasCandidate");
	CheckSolverStatus(
		obligation,
		"Proved",
		"mutually exclusive if/else relay targets");

	const Json& sites =
		RequireArray(result.manifest, "relay_sites");
	CHECK(sites.size() == 2);
	bool foundNegativeGuard = false;
	for (const Json& site : sites)
	{
		const Json* guard = FindConstraint(
			refinementJson,
			"RelayGuardNecessity",
			RequireString(site, "id"));
		CHECK(guard != nullptr);
		const Json& predicate = RequireBinaryChild(
			RequireField(*guard, "formula"),
			"implies",
			1);
		foundNegativeGuard =
			foundNegativeGuard
			|| (RequireString(predicate, "kind") == "Unary"
				&& RequireString(predicate, "operator") == "!");
	}
	CHECK(foundNegativeGuard);
}

void TestZ3BitVector32WrapTautology(const std::string&)
{
	const refinement::FormulaSort bv32 =
		refinement::FormulaSort::UnsignedBitVector(32);
	const std::vector<refinement::RelayRefinementSymbol> symbols = {
		MakeTestSymbol(
			"bv32.x",
			refinement::RelayRefinementSymbolKind::
				SourceFunctionParameter,
			bv32,
			"uint32"),
	};
	const refinement::FormulaExpr x =
		refinement::FormulaExpr::Symbol("bv32.x", bv32);
	refinement::FormulaExpr wrapped =
		refinement::FormulaExpr::Binary(
			"+",
			refinement::FormulaExpr::Binary(
				"+",
				x,
				refinement::FormulaExpr::BitVectorLiteral(
					"4294967295u32",
					32),
				bv32),
			refinement::FormulaExpr::BitVectorLiteral("1u32", 32),
			bv32);
	const refinement::RelayProofObligation obligation =
		MakeBooleanGoal(
			"bv32.wrap",
			refinement::FormulaExpr::Binary(
				"==",
				std::move(wrapped),
				x,
				refinement::FormulaSort::Bool()));
	CheckManualSolverStatus(
		RunZ3Goal(symbols, {}, obligation),
		solver::RelaySolverStatus::Proved,
		"uint32 modular wraparound tautology");
}

void TestZ3BitVectorZeroExtension(const std::string&)
{
	const refinement::FormulaSort bv16 =
		refinement::FormulaSort::UnsignedBitVector(16);
	const refinement::FormulaSort bv32 =
		refinement::FormulaSort::UnsignedBitVector(32);
	const std::vector<refinement::RelayRefinementSymbol> symbols = {
		MakeTestSymbol(
			"bv16.x",
			refinement::RelayRefinementSymbolKind::
				SourceFunctionParameter,
			bv16,
			"uint16"),
	};
	const refinement::FormulaExpr widened =
		refinement::FormulaExpr::Cast(
			"uint32",
			refinement::FormulaExpr::Symbol("bv16.x", bv16),
			bv32);
	const refinement::FormulaExpr highBits =
		refinement::FormulaExpr::Binary(
			">>",
			widened,
			refinement::FormulaExpr::BitVectorLiteral("16u32", 32),
			bv32);
	const refinement::RelayProofObligation obligation =
		MakeBooleanGoal(
			"bv16.to_bv32.zero_extend",
			refinement::FormulaExpr::Binary(
				"==",
				highBits,
				refinement::FormulaExpr::BitVectorLiteral(
					"0u32",
					32),
				refinement::FormulaSort::Bool()));
	CheckManualSolverStatus(
		RunZ3Goal(symbols, {}, obligation),
		solver::RelaySolverStatus::Proved,
		"uint16 to uint32 zero extension");
}

void TestZ3MillionPixelFormulaEquality(
	const std::string& fixtureDirectory)
{
	const CompileResult result = CompileFixture(
		fixtureDirectory + "/../../../simulator/contracts",
		"MillionPixel.prd");
	CheckTopLevel(result, "MillionPixel");
	const Json& site =
		RequireArray(result.manifest, "relay_sites").front();
	const Json* targetRelation = FindConstraint(
		RequireRefinement(result.manifest),
		"RelayTargetRelation",
		RequireString(site, "id"));
	CHECK(targetRelation != nullptr);
	CheckUint32CoordinateFormula(
		RequireRelationSourceExpression(*targetRelation),
		"MillionPixel solver target");

	const refinement::FormulaSort bv16 =
		refinement::FormulaSort::UnsignedBitVector(16);
	const refinement::FormulaSort bv32 =
		refinement::FormulaSort::UnsignedBitVector(32);
	const std::vector<refinement::RelayRefinementSymbol> symbols = {
		MakeTestSymbol("million.x1", refinement::RelayRefinementSymbolKind::SourceFunctionParameter, bv16, "uint16"),
		MakeTestSymbol("million.y1", refinement::RelayRefinementSymbolKind::SourceFunctionParameter, bv16, "uint16"),
		MakeTestSymbol("million.x2", refinement::RelayRefinementSymbolKind::SourceFunctionParameter, bv16, "uint16"),
		MakeTestSymbol("million.y2", refinement::RelayRefinementSymbolKind::SourceFunctionParameter, bv16, "uint16"),
	};
	const refinement::FormulaExpr x1 =
		refinement::FormulaExpr::Symbol("million.x1", bv16);
	const refinement::FormulaExpr y1 =
		refinement::FormulaExpr::Symbol("million.y1", bv16);
	const refinement::FormulaExpr x2 =
		refinement::FormulaExpr::Symbol("million.x2", bv16);
	const refinement::FormulaExpr y2 =
		refinement::FormulaExpr::Symbol("million.y2", bv16);
	const auto makeKey = [&bv32](
		const refinement::FormulaExpr& x,
		const refinement::FormulaExpr& y) {
		return refinement::FormulaExpr::Binary(
			"+",
			refinement::FormulaExpr::Binary(
				"*",
				refinement::FormulaExpr::Cast("uint32", x, bv32),
				refinement::FormulaExpr::BitVectorLiteral(
					"65536u32",
					32),
				bv32),
			refinement::FormulaExpr::Cast("uint32", y, bv32),
			bv32);
	};
	const refinement::FormulaExpr sameCoordinates =
		refinement::FormulaExpr::Nary(
			"&&",
			{
				refinement::FormulaExpr::Binary(
					"==", x1, x2, refinement::FormulaSort::Bool()),
				refinement::FormulaExpr::Binary(
					"==", y1, y2, refinement::FormulaSort::Bool()),
			},
			refinement::FormulaSort::Bool());

	const refinement::RelayProofObligation obligation =
		MakeBooleanGoal(
			"million_pixel.same_coordinates_same_key",
			refinement::FormulaExpr::Binary(
				"implies",
				sameCoordinates,
				refinement::FormulaExpr::Binary(
					"==",
					makeKey(x1, y1),
					makeKey(x2, y2),
					refinement::FormulaSort::Bool()),
				refinement::FormulaSort::Bool()));
	CheckManualSolverStatus(
		RunZ3Goal(
			symbols,
			{},
			obligation),
		solver::RelaySolverStatus::Proved,
		"MillionPixel equal coordinates imply equal uint32 key");
}

void TestZ3UnknownFormulaUnsupported(const std::string&)
{
	const refinement::RelayProofObligation obligation =
		MakeBooleanGoal(
			"unknown.formula",
			refinement::FormulaExpr::Unknown(
				"opaque_ternary",
				transpiler::relay_protocol::SourceLocation(),
				"synthetic opaque target"));
	const solver::RelaySolverResult result =
		RunZ3Goal({}, {}, obligation);
	CheckManualSolverStatus(
		result,
		solver::RelaySolverStatus::Unsupported,
		"Unknown formula");
	CHECK(!result.reason.empty());
}

void TestZ3InconsistentAssumptions(const std::string&)
{
	const std::vector<refinement::RelayRefinementSymbol> symbols = {
		MakeTestSymbol(
			"assumption.flag",
			refinement::RelayRefinementSymbolKind::
				SourceFunctionParameter,
			refinement::FormulaSort::Bool(),
			"bool"),
	};
	const refinement::FormulaExpr flag =
		refinement::FormulaExpr::Symbol(
			"assumption.flag",
			refinement::FormulaSort::Bool());
	const std::vector<refinement::RelayConstraint> constraints = {
		MakeSolverAssumption("assume.flag", flag),
		MakeSolverAssumption(
			"assume.not_flag",
			refinement::FormulaExpr::Unary(
				"!",
				flag,
				refinement::FormulaSort::Bool())),
	};
	const refinement::RelayProofObligation obligation =
		MakeBooleanGoal(
			"inconsistent.assumptions",
			refinement::FormulaExpr::BoolLiteral(true));
	CheckManualSolverStatus(
		RunZ3Goal(symbols, constraints, obligation),
		solver::RelaySolverStatus::InconsistentAssumptions,
		"conflicting SolverAssumption constraints");
}

void TestZ3SemanticTargetIsConstructionFact(
	const std::string& fixtureDirectory)
{
	const CompileResult result =
		CompileFixture(fixtureDirectory, "named_address.prd");
	CheckTopLevel(result, "ProtocolNamedAddress");
	const Json& site =
		RequireArray(result.manifest, "relay_sites").front();
	const Json* obligation = FindProofObligation(
		RequireRefinement(result.manifest),
		"RelayTargetEquality",
		RequireString(site, "id"));
	CheckSolverStatus(
		obligation,
		"EstablishedByConstruction",
		"semantic target equality");
	const Json& solverResult = RequireSolverResult(*obligation);
	CHECK(RequireString(solverResult, "backend") == "compiler");
	CHECK(
		RequireArray(
			solverResult,
			"assumption_constraint_ids").empty());
	CHECK(
		RequireString(solverResult, "reason").find(
			"no solver query") != std::string::npos);
}
#endif

#ifdef RPREDA_ENABLE_RUNTIME_TRACE
using RelayTraceMethod =
	void (prlrt::IRelayTraceRuntimeInterface::*)(
		prlrt::RelaySiteOrdinal) noexcept;

static_assert(
	std::is_same<
		decltype(
			&prlrt::IRelayTraceRuntimeInterface::PushRelayTraceSite),
		RelayTraceMethod>::value,
	"relay trace push ABI changed");
static_assert(
	std::is_same<
		decltype(
			&prlrt::IRelayTraceRuntimeInterface::PopRelayTraceSite),
		RelayTraceMethod>::value,
	"relay trace pop ABI changed");
static_assert(
	std::is_nothrow_constructible<
		prlrt::RelayTraceSiteGuard,
		prlrt::RelaySiteOrdinal>::value,
	"relay trace guard construction must remain noexcept");
static_assert(
	std::is_nothrow_destructible<
		prlrt::RelayTraceSiteGuard>::value,
	"relay trace guard destruction must remain noexcept");
static_assert(
	!std::is_copy_constructible<
		prlrt::RelayTraceSiteGuard>::value,
	"relay trace guard must not be copied");
static_assert(
	!std::is_move_constructible<
		prlrt::RelayTraceSiteGuard>::value,
	"relay trace guard must not be moved");

class RecordingRelayTraceRuntime
	: public prlrt::IRelayTraceRuntimeInterface
{
public:
	struct Event
	{
		bool push;
		prlrt::RelaySiteOrdinal ordinal;
	};

	void PushRelayTraceSite(
		prlrt::RelaySiteOrdinal ordinal) noexcept override
	{
		events.push_back({true, ordinal});
	}

	void PopRelayTraceSite(
		prlrt::RelaySiteOrdinal ordinal) noexcept override
	{
		events.push_back({false, ordinal});
	}

	std::vector<Event> events;
};

void TestRuntimeTraceAbi(const std::string&)
{
	CHECK(prlrt::RPREDA_RUNTIME_TRACE_ABI_VERSION == 1);
	RecordingRelayTraceRuntime recorder;
	prlrt::IRelayTraceRuntimeInterface* previous =
		prlrt::g_relayTraceRuntimeInterface;
	prlrt::g_relayTraceRuntimeInterface = &recorder;

	{
		prlrt::RelayTraceSiteGuard outer(7);
		{
			prlrt::RelayTraceSiteGuard inner(11);
		}
	}
	try
	{
		prlrt::RelayTraceSiteGuard exceptional(19);
		throw std::runtime_error("trace guard unwind");
	}
	catch (const std::runtime_error&)
	{
	}
	prlrt::g_relayTraceRuntimeInterface = previous;

	CHECK(recorder.events.size() == 6);
	CHECK(recorder.events[0].push);
	CHECK(recorder.events[0].ordinal == 7);
	CHECK(recorder.events[1].push);
	CHECK(recorder.events[1].ordinal == 11);
	CHECK(!recorder.events[2].push);
	CHECK(recorder.events[2].ordinal == 11);
	CHECK(!recorder.events[3].push);
	CHECK(recorder.events[3].ordinal == 7);
	CHECK(recorder.events[4].push);
	CHECK(recorder.events[4].ordinal == 19);
	CHECK(!recorder.events[5].push);
	CHECK(recorder.events[5].ordinal == 19);
}
#endif

struct TestCase
{
	const char* name;
	void (*run)(const std::string&);
};

} // namespace

int main(int argc, char** argv)
{
	if (argc != 2)
	{
		std::cerr << "usage: relay_protocol_ir_tests <relay-protocol-fixture-directory>\n";
		return 2;
	}

	const std::string fixtureDirectory = argv[1];
	const std::vector<TestCase> tests = {
#ifdef RPREDA_ENABLE_RUNTIME_TRACE
		{ "runtime trace ABI and RAII guard", &TestRuntimeTraceAbi },
#endif
		{ "named address relay", &TestNamedAddress },
		{ "lambda address relay", &TestLambdaAddress },
		{ "global relay", &TestGlobal },
		{ "shards broadcast", &TestShards },
		{ "relay inside if/else", &TestIfElse },
		{ "relay inside else-if chain", &TestElseIfChain },
		{ "relay inside bounded for-loop", &TestBoundedFor },
		{ "zero-step for-loop is not statically bounded", &TestZeroStepIsNotBounded },
		{ "for-loop body reset is not statically bounded", &TestBodyResetIsNotBounded },
		{ "non-strict for-loop is not statically bounded", &TestNonStrictConditionIsNotBounded },
		{ "nested relay lambda", &TestNestedLambda },
		{ "opaque expression fallback", &TestOpaqueFallback },
		{ "expression dependency and availability analysis", &TestExpressionDependencies },
		{ "refinement literal target equality", &TestRefinementLiteralTarget },
		{ "refinement arithmetic and cast target", &TestRefinementArithmeticCastTarget },
		{ "refinement pre-state target", &TestRefinementPreStateTarget },
		{ "refinement uint32 overflow and cast semantics", &TestRefinementUint32OverflowAndCast },
		{ "refinement Boolean guard and array length", &TestRefinementBooleanGuardAndArrayLength },
		{ "MillionPixel refinement target formula", &TestRefinementMillionPixel },
		{ "static summary for conditional relay", &TestSummaryConditional },
		{ "static summary for sequential relay sites", &TestSummarySequential },
		{ "static summary for recursive relay handler", &TestSummaryRecursiveHandler },
		{ "opaque protocol node stays conservative", &TestOpaqueProtocolNodeIsConservative },
		{ "ordinary relay-reachable call keeps depth unknown", &TestSummaryUnmodeledRelayReachableCall },
		{ "overloaded source functions", &TestOverloadedSource },
		{ "PREDA-native CFG, effects, and relay ICFG", &TestPredaNativeCFGAndICFG },
		{ "generated relay lambda CFG", &TestGeneratedLambdaCFG },
		{ "synchronous call graph recursion", &TestSynchronousCallGraphRecursion },
		{ "overloaded synchronous call resolution", &TestOverloadedSynchronousCalls },
		{ "nested loop break and continue targets", &TestNestedLoopTargets },
		{ "runtime failure helper effect", &TestRuntimeFailureHelper },
		{ "unknown call and opaque effects stay conservative", &TestUnknownCallAndOpaqueEffects },
		{ "for update call CFG and ICFG", &TestForUpdateCallCFG },
		{ "relay operand state effects", &TestRelayOperandStateEffects },
		{ "call evaluation order conservatism", &TestCallEvaluationOrderConservatism },
		{ "nonterminating CFG post-dominance", &TestNonterminatingPostDominance },
		{ "synchronous relay composition", &TestSynchronousRelayComposition },
		{ "generated lambda synchronous effect composition", &TestLambdaSynchronousEffectComposition },
		{ "mutual recursive effect fixed point", &TestMutualRecursiveEffectFixedPoint },
		{ "opaque CFG remains incomplete", &TestOpaqueCFGIsNotComplete },
		{ "else-if CFG", &TestElseIfCFG },
		{ "implicit runtime failure CFG", &TestImplicitFailureCFG },
		{ "parallel certificate core pair relations", &TestParallelCertificatePairRelations },
		{ "parallel certificate work and depth soundness", &TestParallelCertificateWorkSoundness },
		{ "parallel certificate conservative sync and broadcast contexts", &TestParallelCertificateConservativeContexts },
#ifdef RPREDA_ENABLE_Z3
		{ "Z3 parallel certificate helper composition", &TestZ3ParallelCertificateHelperComposition },
		{ "Z3 proves conditional relay count upper bound", &TestZ3ConditionalCountUpperBound },
		{ "Z3 disproves same-target non-alias candidate", &TestZ3SameTargetNonAliasCounterexample },
		{ "Z3 proves if/else guarded targets cannot alias concurrently", &TestZ3IfElseGuardedTargets },
		{ "Z3 preserves uint32 wraparound", &TestZ3BitVector32WrapTautology },
		{ "Z3 zero-extends uint16 to uint32", &TestZ3BitVectorZeroExtension },
		{ "Z3 validates MillionPixel target formula", &TestZ3MillionPixelFormulaEquality },
		{ "Z3 rejects Unknown formulas conservatively", &TestZ3UnknownFormulaUnsupported },
		{ "Z3 detects inconsistent assumptions", &TestZ3InconsistentAssumptions },
		{ "semantic target equality is established by construction", &TestZ3SemanticTargetIsConstructionFact },
#endif
	};

	size_t failures = 0;
	for (const TestCase& test : tests)
	{
		try
		{
			test.run(fixtureDirectory);
			std::cout << "[PASS] " << test.name << '\n';
		}
		catch (const std::exception& error)
		{
			++failures;
			std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
		}
	}

	if (failures != 0)
	{
		std::cerr << failures << " relay protocol IR test(s) failed\n";
		return 1;
	}

	std::cout << tests.size() << " relay protocol IR tests passed\n";
	return 0;
}
