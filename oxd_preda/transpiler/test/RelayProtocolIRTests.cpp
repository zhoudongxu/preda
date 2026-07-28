#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../transpiler.h"
#include "../relay_protocol/analysis/RelaySummaryBuilder.h"
#include "../../3rdParty/nlohmann/json.hpp"

#if defined(_WIN32)
extern "C" __declspec(dllimport) transpiler::ITranspiler* CreateTranspilerInstance(const char* options);
#else
extern "C" transpiler::ITranspiler* CreateTranspilerInstance(const char* options);
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
	CHECK_DETAIL(compiler->BuildParseTree(source.c_str()), path + CompileDiagnostics(*compiler));
	CHECK_DETAIL(compiler->PreCompile("RelayProtocolTests"), path + CompileDiagnostics(*compiler));

	EmptyContractSymbolDatabase symbols;
	CHECK_DETAIL(compiler->Compile("RelayProtocolTests", &symbols), path + CompileDiagnostics(*compiler));

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
	std::ostringstream detail;
	detail << "generated C++ changed: size=" << result.generatedCpp.size()
		<< ", fnv1a64=0x" << std::hex << Fnv1a64(result.generatedCpp);
	CHECK_DETAIL(result.generatedCpp.size() == expectedSize, detail.str());
	CHECK_DETAIL(Fnv1a64(result.generatedCpp) == expectedHash, detail.str());
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
	CHECK(RequireField(manifest, "schema_version").get<uint64_t>() == 2);
	CHECK(RequireString(manifest, "dapp") == "RelayProtocolTests");
	CHECK(RequireString(manifest, "contract") == contract);
	RequireArray(manifest, "relay_sites");
	RequireArray(manifest, "handlers");
	RequireArray(manifest, "edges");
	const Json& functions = RequireArray(manifest, "functions");
	for (const Json& function : functions)
	{
		const Json& summary = RequireField(function, "summary");
		CheckSummaryRequiredFields(summary);
	}
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
	RequireField(site, "target_function");
	RequireArray(site, "arguments");
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
	CHECK(result.generatedCpp.find("prlrt::relay(") != std::string::npos);

	const Json& sites = RequireArray(result.manifest, "relay_sites");
	CHECK(sites.size() == 1);
	const Json& site = sites.front();
	CheckSiteCommon(site, "ProtocolNamedAddress", "send", "address", "custom_scope", "address");
	CHECK(RequireString(RequireField(site, "target"), "kind") == "identifier");
	CHECK(Compact(RequireString(RequireField(site, "target"), "text")) == "target");
	CHECK(RequireString(site, "target_function") == "receive");

	const Json& arguments = RequireArray(site, "arguments");
	CHECK(arguments.size() == 1);
	CHECK(RequireString(arguments.front(), "type") == "int32");
	const Json& argumentExpression = RequireField(arguments.front(), "expression");
	CHECK(RequireString(argumentExpression, "kind") == "identifier");
	CHECK(RequireString(argumentExpression, "type") == "int32");
	CHECK(Compact(RequireString(argumentExpression, "text")) == "value");

	CheckResolvedHandler(result.manifest, site, "named", "address");
	const Json* handler = FindHandlerById(result.manifest, RequireField(site, "handler_id"));
	CHECK(RequireString(*handler, "name") == "receive");
	CHECK(RequireArray(*handler, "parameter_types") == Json::array({ "int32" }));
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
		false);
	CheckSummaryBoolean(summary, "has_opaque", false);
	CheckSummaryFanout(summary, "single_target");
	CheckSummaryOrdering(summary, "trivial");
	CHECK(RequireString(summary, "analysis_status") == "conservative");
}

void TestLambdaAddress(const std::string& fixtureDirectory)
{
	const CompileResult result = CompileFixture(fixtureDirectory, "lambda_address.prd");
	CheckTopLevel(result, "ProtocolLambdaAddress");
	CheckGeneratedCppGolden(result, 11606, UINT64_C(0x04e76cef5d6c344a));
	CHECK(result.generatedCpp.find("prlrt::relay(") != std::string::npos);

	const Json& sites = RequireArray(result.manifest, "relay_sites");
	CHECK(sites.size() == 1);
	const Json& site = sites.front();
	CheckSiteCommon(site, "ProtocolLambdaAddress", "send", "address", "custom_scope", "address");
	CHECK(RequireString(RequireField(site, "target"), "kind") == "identifier");
	CHECK(RequireArray(site, "arguments").size() == 1);
	CHECK(RequireString(RequireArray(site, "arguments").front(), "type") == "int32");
	CHECK(RequireString(
		RequireField(RequireArray(site, "arguments").front(), "expression"),
		"type") == "int32");

	CheckResolvedHandler(result.manifest, site, "lambda", "address");
	const Json* handler = FindHandlerById(result.manifest, RequireField(site, "handler_id"));
	CHECK(RequireString(*handler, "name").find("__relaylambda_") == 0);
	CHECK(RequireArray(*handler, "parameter_types") == Json::array({ "int32" }));
	CHECK(ContainsNodeKind(RequireArray(result.manifest, "functions"), "Emit"));
}

void TestGlobal(const std::string& fixtureDirectory)
{
	const CompileResult result = CompileFixture(fixtureDirectory, "global.prd");
	CheckTopLevel(result, "ProtocolGlobal");
	CheckGeneratedCppGolden(result, 11093, UINT64_C(0x69d91801a572b556));
	CHECK(result.generatedCpp.find("prlrt::relay_global(") != std::string::npos);

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
	CHECK(result.generatedCpp.find("prlrt::relay_shards(") != std::string::npos);

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
		false);
	CheckSummaryBoolean(summary, "has_opaque", false);
	CheckSummaryOrdering(summary, "trivial");
	CHECK(RequireString(summary, "analysis_status") == "conservative");
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
		false);
	CheckSummaryBoolean(summary, "has_opaque", false);
	CheckSummaryOrdering(summary, "trivial");
	CHECK(RequireString(summary, "analysis_status") == "conservative");
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
		false);
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
}

void TestSummaryConditional(const std::string& fixtureDirectory)
{
	const CompileResult result =
		CompileFixture(fixtureDirectory, "summary_conditional.prd");
	CheckTopLevel(result, "ProtocolSummaryConditional");
	CHECK(result.generatedCpp.find("prlrt::relay(") != std::string::npos);

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
		false);
	CheckSummaryBoolean(summary, "has_opaque", false);
	CheckSummaryFanout(summary, "single_target");
	CheckSummaryOrdering(summary, "trivial");
	CHECK(RequireString(summary, "analysis_status") == "conservative");
}

void TestSummarySequential(const std::string& fixtureDirectory)
{
	const CompileResult result =
		CompileFixture(fixtureDirectory, "summary_sequential.prd");
	CheckTopLevel(result, "ProtocolSummarySequential");
	CHECK(result.generatedCpp.find("prlrt::relay(") != std::string::npos);

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
		false);
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
	CHECK(result.generatedCpp.find("prlrt::relay(") != std::string::npos);
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
			false);
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
	CHECK(result.generatedCpp.find("prlrt::relay(") != std::string::npos);

	const Json& sites = RequireArray(result.manifest, "relay_sites");
	const Json& edges = RequireArray(result.manifest, "edges");
	const Json& functions = RequireArray(result.manifest, "functions");
	CHECK(sites.size() == 2);
	CHECK(edges.size() == 2);
	CHECK(functions.size() == 2);

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

	for (const Json& function : functions)
	{
		CHECK(RequireString(function, "function") == "send");
		CHECK(!RequireString(function, "source_function_id").empty());
		CHECK(!RequireString(function, "source_function_signature").empty());
		CHECK(RequireField(
			function,
			"source_function_overload_index").is_number_unsigned());
		CHECK(RequireArray(function, "relay_site_ids").size() == 1);
	}

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
		{ "static summary for conditional relay", &TestSummaryConditional },
		{ "static summary for sequential relay sites", &TestSummarySequential },
		{ "static summary for recursive relay handler", &TestSummaryRecursiveHandler },
		{ "opaque protocol node stays conservative", &TestOpaqueProtocolNodeIsConservative },
		{ "ordinary relay-reachable call keeps depth unknown", &TestSummaryUnmodeledRelayReachableCall },
		{ "overloaded source functions", &TestOverloadedSource },
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
