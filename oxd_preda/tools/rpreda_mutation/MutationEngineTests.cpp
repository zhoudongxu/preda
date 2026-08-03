#include "MutationEngine.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using Json = nlohmann::ordered_json;
using namespace rpreda::mutation;

#define CHECK(condition) \
	do { if (!(condition)) throw std::runtime_error( \
		std::string("check failed: ") + #condition); } while (false)

const std::string Source = u8R"PRD(// Unicode prefix: 变异框架
contract MutationFixture {
    @uint32 function h1(uint32 value) {
    }

    @uint32 function h2(uint32 value) {
        return;
    }

    @global function hg(uint32 value) {
    }

    @shard function hs(uint32 value) {
    }

    @uint32 function keyed(
        bool choose,
        uint32 first,
        uint32 second,
        uint32 base,
        uint32 value
    ) export {
        if (choose) {
            relay@first /* decoy h1( */ h1(value);
        } else {
            relay@second h2(value);
        }
        relay@base h1(value);
        // certified adjacent sibling
        relay@second h2(value);
        uint32 local = value;
        relay@(base + 1u32) h1(value);
    }

    @global function broadcast(uint32 value) export {
        relay@/* kind */shards hs(value);
        relay@next hg(value);
    }

    @uint32 function lambda_capture(
        uint32 target,
        uint32 value,
        uint32 other
    ) export {
        relay@target (/* ^fake */ ^value) {
        }
    }
}
)PRD";

size_t FindNth(const std::string &needle, size_t occurrence = 0)
{
	size_t cursor = 0;
	for (size_t index = 0; index <= occurrence; ++index)
	{
		cursor = Source.find(needle, cursor);
		if (cursor == std::string::npos)
			throw std::runtime_error("fixture substring not found: " + needle);
		if (index != occurrence)
			cursor += needle.size();
	}
	return cursor;
}

size_t CodePointCount(const std::string &value, size_t byteEnd)
{
	size_t result = 0;
	for (size_t index = 0; index < byteEnd; ++index)
	{
		const unsigned char byte = static_cast<unsigned char>(value[index]);
		if ((byte & 0xc0U) != 0x80U)
			++result;
	}
	return result;
}

Json Location(size_t byteStart, size_t byteEndInclusive)
{
	const size_t startCp = CodePointCount(Source, byteStart);
	const size_t endCp = CodePointCount(Source, byteEndInclusive + 1) - 1;
	uint32_t line = 1;
	uint32_t column = 0;
	uint32_t endLine = 1;
	uint32_t endColumn = 0;
	for (size_t index = 0; index < Source.size();)
	{
		if (index == byteStart)
		{
			line = endLine;
			column = endColumn;
		}
		if (index > byteEndInclusive)
			break;
		const unsigned char lead = static_cast<unsigned char>(Source[index]);
		const size_t width = (lead & 0x80U) == 0 ? 1 :
			(lead & 0xe0U) == 0xc0U ? 2 :
			(lead & 0xf0U) == 0xe0U ? 3 : 4;
		if (width == 1 && Source[index] == '\n')
		{
			++endLine;
			endColumn = 0;
		}
		else
			++endColumn;
		index += width;
	}
	return Json{
		{"line", line}, {"column", column},
		{"end_line", endLine}, {"end_column", endColumn},
		{"start_offset", static_cast<int64_t>(startCp)},
		{"end_offset", static_cast<int64_t>(endCp)},
	};
}

Json Expression(
	const std::string &text,
	const std::string &type,
	size_t byteStart)
{
	return Json{
		{"kind", "identifier"},
		{"text", text},
		{"type", type},
		{"location", Location(byteStart, byteStart + text.size() - 1)},
		{"children", Json::array()},
	};
}

std::pair<size_t, size_t> FunctionSpan(const std::string &name)
{
	const size_t namePosition = FindNth("function " + name + "(");
	const size_t start = Source.rfind('@', namePosition);
	const size_t open = Source.find('{', namePosition);
	int depth = 0;
	for (size_t cursor = open; cursor < Source.size(); ++cursor)
	{
		if (Source[cursor] == '{')
			++depth;
		else if (Source[cursor] == '}' && --depth == 0)
			return {start, cursor};
	}
	throw std::runtime_error("function close brace not found");
}

Json BuildManifest()
{
	const std::string contract = "d.MutationFixture";
	const std::string keyed = contract +
		"::keyed(bool,uint32,uint32,uint32,uint32)";
	const std::string broadcast = contract + "::broadcast(uint32)";
	const std::string lambdaCapture = contract +
		"::lambda_capture(uint32,uint32,uint32)";
	const std::map<std::string, std::string> functionIds = {
		{"h1", contract + "::h1(uint32)"},
		{"h2", contract + "::h2(uint32)"},
		{"hg", contract + "::hg(uint32)"},
		{"hs", contract + "::hs(uint32)"},
		{"keyed", keyed},
		{"broadcast", broadcast},
		{"lambda_capture", lambdaCapture},
	};
	const std::map<std::string, std::string> handlerIds = {
		{"h1", "handler_h1"}, {"h2", "handler_h2"},
		{"hg", "handler_hg"}, {"hs", "handler_hs"},
		{"lambda", "handler_lambda_73"},
	};

	Json root{
		{"schema_version", 5},
		{"contract", contract},
		{"relay_sites", Json::array()},
		{"handlers", Json::array()},
		{"refinement", Json{{"symbols", Json::array()}}},
		{"control_flow", Json{{"functions", Json::array()}}},
		{"parallel_certificate", Json{{"functions", Json::array()}}},
	};

	const auto addHandler = [&](const std::string &name,
		const std::string &scope) {
		root["handlers"].push_back(Json{
			{"id", handlerIds.at(name)}, {"kind", "named"},
			{"name", name}, {"target_function_id", functionIds.at(name)},
			{"scope", scope}, {"parameter_types", Json::array({"uint32"})},
		});
	};
	addHandler("h1", "uint32");
	addHandler("h2", "uint32");
	addHandler("hg", "global");
	addHandler("hs", "shard");
	root["handlers"].push_back(Json{
		{"id", handlerIds.at("lambda")}, {"kind", "lambda"},
		{"name", "__relaylambda_73"},
		{"target_function_id", "synthetic_lambda_function_73"},
		{"scope", "uint32"},
		{"parameter_types", Json::array({"uint32"})},
	});

	const auto addFunction = [&](const std::string &name,
		const std::vector<std::pair<std::string, std::string>> &parameters) {
		const auto span = FunctionSpan(name);
		root["control_flow"]["functions"].push_back(Json{
			{"function_id", functionIds.at(name)},
			{"scope_name", name == "broadcast" ? "global" : "uint32"},
			{"location", Location(span.first, span.second)},
		});
		const size_t declarationEnd = Source.find('{', span.first);
		size_t search = span.first;
		for (const auto &parameter : parameters)
		{
			search = Source.find(parameter.first, search);
			CHECK(search != std::string::npos && search < declarationEnd);
			root["refinement"]["symbols"].push_back(Json{
				{"kind", "SourceFunctionParameter"},
				{"source_function_id", functionIds.at(name)},
				{"source_name", parameter.first},
				{"preda_type", parameter.second},
				{"location", Location(search, search + parameter.first.size() - 1)},
			});
			search += parameter.first.size();
		}
	};
	addFunction("h1", {{"value", "uint32"}});
	addFunction("h2", {{"value", "uint32"}});
	addFunction("hg", {{"value", "uint32"}});
	addFunction("hs", {{"value", "uint32"}});
	addFunction("keyed", {{"choose", "bool"}, {"first", "uint32"},
		{"second", "uint32"}, {"base", "uint32"}, {"value", "uint32"}});
	addFunction("broadcast", {{"value", "uint32"}});
	addFunction("lambda_capture", {{"target", "uint32"},
		{"value", "uint32"}, {"other", "uint32"}});

	const size_t choose = FindNth("choose", 1);
	const auto addSite = [&](const std::string &id,
		const std::string &functionId,
		const std::string &sourceScope,
		const std::string &statement,
		size_t occurrence,
		const std::string &relayKind,
		const std::string &target,
		const std::string &targetScope,
		const std::string &handler,
		bool guarded,
		bool omitTargetLocation = false) {
		const size_t statementStart = FindNth(statement, occurrence);
		const size_t statementEnd = statementStart + statement.size() - 1;
		const std::string targetToken = target.empty()
			? relayKind
			: target;
		const size_t targetStart = Source.find(targetToken,
			statementStart + std::string("relay@").size());
		CHECK(targetStart != std::string::npos && targetStart <= statementEnd);
		Json targetJson{
			{"kind", target.empty() ? "builtin_target" : "identifier"},
			{"text", targetToken},
			{"type", targetScope},
			{"children", Json::array()},
		};
		if (!omitTargetLocation)
			targetJson["location"] = Location(
				targetStart, targetStart + targetToken.size() - 1);
		const size_t argumentStart = Source.find("value", statementStart);
		CHECK(argumentStart != std::string::npos && argumentStart <= statementEnd);
		Json branches = Json::array();
		if (guarded)
		{
			branches.push_back(Json{
				{"condition", Expression("choose", "bool", choose)},
				{"polarity", id == "site_0"},
			});
		}
		root["relay_sites"].push_back(Json{
			{"id", id}, {"source_function_id", functionId},
			{"source_scope", sourceScope}, {"relay_kind", relayKind},
			{"target_scope", targetScope}, {"target_function", handler},
			{"handler_id", handlerIds.at(handler)},
			{"location", Location(statementStart, statementEnd)},
			{"target", targetJson},
			{"arguments", Json::array({Json{
				{"type", "uint32"},
				{"expression", Expression("value", "uint32", argumentStart)},
			}})},
			{"branches", branches},
		});
	};

	addSite("site_0", keyed, "uint32",
		"relay@first /* decoy h1( */ h1(value);", 0,
		"custom_scope", "first", "uint32", "h1", true);
	addSite("site_1", keyed, "uint32", "relay@second h2(value);", 0,
		"custom_scope", "second", "uint32", "h2", true);
	addSite("site_2", keyed, "uint32", "relay@base h1(value);", 0,
		"custom_scope", "base", "uint32", "h1", false);
	addSite("site_3", keyed, "uint32", "relay@second h2(value);", 1,
		"custom_scope", "second", "uint32", "h2", false);
	addSite("site_6", keyed, "uint32", "relay@(base + 1u32) h1(value);", 0,
		"custom_scope", "(base + 1u32)", "uint32", "h1", false);
	addSite("site_4", broadcast, "global",
		"relay@/* kind */shards hs(value);", 0,
		"shards", "", "shard", "hs", false, true);
	addSite("site_5", broadcast, "global", "relay@next hg(value);", 0,
		"next", "", "global", "hg", false);

	const std::string lambdaPrefix =
		"relay@target (/* ^fake */ ^value) {";
	const size_t lambdaStart = FindNth(lambdaPrefix);
	const size_t lambdaOpen = Source.find('{', lambdaStart);
	const size_t lambdaEnd = Source.find('}', lambdaOpen);
	const size_t lambdaTarget = Source.find("target", lambdaStart);
	const size_t lambdaArgument = Source.find("value", lambdaTarget);
	root["relay_sites"].push_back(Json{
		{"id", "site_7"}, {"source_function_id", lambdaCapture},
		{"source_scope", "uint32"}, {"relay_kind", "custom_scope"},
		{"target_scope", "uint32"},
		{"target_function", "__relaylambda_73"},
		{"handler_id", handlerIds.at("lambda")},
		{"location", Location(lambdaStart, lambdaEnd)},
		{"target", Expression("target", "uint32", lambdaTarget)},
		{"arguments", Json::array({Json{
			{"type", "uint32"},
			{"expression", Expression("value", "uint32", lambdaArgument)},
		}})},
		{"branches", Json::array()},
	});

	Json pairs = Json::array({
		Json{{"site_a", "site_0"}, {"site_b", "site_1"},
			{"relation", "ProvedMayAlias"}, {"status", "Proved"}},
		Json{{"site_a", "site_2"}, {"site_b", "site_3"},
			{"relation", "MustPrecedeAB"}, {"status", "Proved"}},
		Json{{"site_a", "site_3"}, {"site_b", "site_6"},
			{"relation", "MustPrecedeAB"}, {"status", "Proved"}},
		Json{{"site_a", "site_2"}, {"site_b", "site_6"},
			{"relation", "CoEmissionIndependent"}, {"status", "Proved"}},
	});
	for (Json &pair : pairs)
		pair["source_function_id"] = keyed;
	root["parallel_certificate"]["functions"].push_back(Json{
		{"source_function_id", keyed}, {"pair_relations", std::move(pairs)},
	});
	return root;
}

const std::string SemanticSource = R"PRD(contract SemanticFixture {
    @uint32 function sink(uint32 amount) {
    }

    @address function route(uint32 x, uint32 y, uint32 amount) export {
        uint32 target = uint32(x) * 65536u32 + uint32(y);
        if (amount >= 1u32) {
            relay@target sink(amount);
        }
    }
}
)PRD";

Json SemanticLocation(size_t start, size_t endInclusive)
{
	return Json{
		{"line", 0}, {"column", 0},
		{"end_line", 0}, {"end_column", 0},
		{"start_offset", static_cast<int64_t>(start)},
		{"end_offset", static_cast<int64_t>(endInclusive)},
	};
}

Json FormulaSort(const std::string &kind, uint32_t width = 0)
{
	Json result{{"kind", kind}};
	if (width != 0)
	{
		result["bit_width"] = width;
		result["preda_name"] = "uint" + std::to_string(width);
	}
	return result;
}

Json FormulaSymbol(
	const std::string &id,
	const std::string &text,
	const Json &sort,
	size_t start,
	size_t endInclusive)
{
	return Json{
		{"kind", "Symbol"}, {"sort", sort},
		{"source_text", text}, {"symbol_id", id},
		{"operator", ""}, {"children", Json::array()},
		{"location", SemanticLocation(start, endInclusive)},
	};
}

Json FormulaBinary(
	const std::string &op,
	Json left,
	Json right,
	const Json &sort,
	size_t start,
	size_t endInclusive,
	const std::string &text = std::string())
{
	return Json{
		{"kind", "Binary"}, {"sort", sort},
		{"source_text", text}, {"operator", op},
		{"children", Json::array({std::move(left), std::move(right)})},
		{"location", SemanticLocation(start, endInclusive)},
	};
}

Json BuildSemanticManifest()
{
	const std::string functionId =
		"d.SemanticFixture::route(uint32,uint32,uint32)";
	const std::string siteId = "semantic_site";
	const Json uint32Sort = FormulaSort("UnsignedBitVector", 32);
	const Json boolSort = FormulaSort("Bool");
	const size_t relayStart = SemanticSource.find("relay@target");
	const size_t relayEnd = SemanticSource.find(';', relayStart);
	const size_t relayTarget = SemanticSource.find("target", relayStart);
	const size_t relayArgument = SemanticSource.find("amount", relayTarget);
	const size_t rhsStart = SemanticSource.find("uint32(x)");
	const size_t rhsEnd = SemanticSource.find("uint32(y)", rhsStart) +
		std::string("uint32(y)").size() - 1;
	const size_t xUse = SemanticSource.find('x', rhsStart);
	const size_t constantStart = SemanticSource.find("65536u32", xUse);
	const size_t yCastStart = SemanticSource.find("uint32(y)", constantStart);
	const size_t yUse = SemanticSource.find('y', yCastStart);
	const size_t guardStart = SemanticSource.find("amount >= 1u32");
	const size_t guardAmount = guardStart;
	const size_t guardLiteral = SemanticSource.find("1u32", guardStart);
	CHECK(relayStart != std::string::npos && relayEnd != std::string::npos &&
		rhsStart != std::string::npos && rhsEnd != std::string::npos &&
		guardStart != std::string::npos);

	const std::string xId = "semantic.parameter.x";
	const std::string yId = "semantic.parameter.y";
	const std::string amountId = "semantic.parameter.amount";
	const std::string emittedId = "semantic.emitted";
	const std::string actualTargetId = "semantic.actual_target";
	const std::string actualArgumentId = "semantic.actual_argument.0";

	Json x = FormulaSymbol(xId, "x", uint32Sort, xUse, xUse);
	Json castX{
		{"kind", "Cast"}, {"sort", uint32Sort},
		{"source_text", "uint32(x)"}, {"operator", "uint32"},
		{"children", Json::array({std::move(x)})},
		{"location", SemanticLocation(rhsStart,
			rhsStart + std::string("uint32(x)").size() - 1)},
	};
	Json literal{
		{"kind", "BitVectorLiteral"}, {"sort", uint32Sort},
		{"source_text", "65536u32"}, {"literal_value", "65536u32"},
		{"operator", ""}, {"children", Json::array()},
		{"location", SemanticLocation(constantStart,
			constantStart + std::string("65536u32").size() - 1)},
	};
	const size_t productEnd = constantStart + std::string("65536u32").size() - 1;
	Json product = FormulaBinary("*", std::move(castX), std::move(literal),
		uint32Sort, rhsStart, productEnd,
		"uint32(x) * 65536u32");
	Json y = FormulaSymbol(yId, "y", uint32Sort, yUse, yUse);
	Json castY{
		{"kind", "Cast"}, {"sort", uint32Sort},
		{"source_text", "uint32(y)"}, {"operator", "uint32"},
		{"children", Json::array({std::move(y)})},
		{"location", SemanticLocation(yCastStart, rhsEnd)},
	};
	Json targetFormula = FormulaBinary("+", std::move(product),
		std::move(castY), uint32Sort, rhsStart, rhsEnd,
		"uint32(x) * 65536u32 + uint32(y)");

	Json emitted = FormulaSymbol(emittedId, "emitted", boolSort,
		relayStart, relayEnd);
	Json actualTarget = FormulaSymbol(actualTargetId, "actual_target",
		uint32Sort, relayStart, relayEnd);
	Json targetEquality = FormulaBinary("==", std::move(actualTarget),
		std::move(targetFormula), boolSort, relayStart, relayEnd);
	Json targetRelation = FormulaBinary("implies", emitted, targetEquality,
		boolSort, relayStart, relayEnd);

	Json actualArgument = FormulaSymbol(actualArgumentId, "actual_arg_0",
		uint32Sort, relayArgument,
		relayArgument + std::string("amount").size() - 1);
	Json amountArgument = FormulaSymbol(amountId, "amount", uint32Sort,
		relayArgument, relayArgument + std::string("amount").size() - 1);
	Json argumentEquality = FormulaBinary("==", std::move(actualArgument),
		std::move(amountArgument), boolSort, relayArgument,
		relayArgument + std::string("amount").size() - 1);
	Json argumentRelation = FormulaBinary("implies", emitted,
		std::move(argumentEquality), boolSort, relayStart, relayEnd);

	Json guardAmountFormula = FormulaSymbol(amountId, "amount", uint32Sort,
		guardAmount, guardAmount + std::string("amount").size() - 1);
	Json guardLiteralFormula{
		{"kind", "BitVectorLiteral"}, {"sort", uint32Sort},
		{"source_text", "1u32"}, {"literal_value", "1u32"},
		{"operator", ""}, {"children", Json::array()},
		{"location", SemanticLocation(guardLiteral,
			guardLiteral + std::string("1u32").size() - 1)},
	};
	Json guard = FormulaBinary(">=", std::move(guardAmountFormula),
		std::move(guardLiteralFormula), boolSort, guardStart,
		guardLiteral + std::string("1u32").size() - 1,
		"amount >= 1u32");
	Json guardRelation = FormulaBinary("implies", std::move(emitted),
		std::move(guard), boolSort, relayStart, relayEnd);

	Json root{
		{"schema_version", 5}, {"contract", "d.SemanticFixture"},
		{"relay_sites", Json::array()}, {"handlers", Json::array()},
		{"control_flow", Json{{"functions", Json::array()}}},
		{"parallel_certificate", Json{{"functions", Json::array()}}},
		{"refinement", Json{
			{"symbols", Json::array()}, {"constraints", Json::array()}}},
	};
	root["relay_sites"].push_back(Json{
		{"id", siteId}, {"source_function_id", functionId},
		{"source_scope", "address"}, {"relay_kind", "custom_scope"},
		{"target_scope", "uint32"}, {"target_function", "sink"},
		{"handler_id", "sink_handler"},
		{"location", SemanticLocation(relayStart, relayEnd)},
		{"target", Json{{"kind", "identifier"}, {"text", "target"},
			{"type", "uint32"},
			{"location", SemanticLocation(relayTarget,
				relayTarget + std::string("target").size() - 1)},
			{"children", Json::array()}}},
		{"arguments", Json::array({Json{
			{"type", "uint32"},
			{"expression", Json{{"kind", "identifier"},
				{"text", "amount"}, {"type", "uint32"},
				{"location", SemanticLocation(relayArgument,
					relayArgument + std::string("amount").size() - 1)},
				{"children", Json::array()}}},
		}})},
		{"branches", Json::array()},
	});
	root["handlers"].push_back(Json{
		{"id", "sink_handler"}, {"kind", "named"}, {"name", "sink"},
		{"target_function_id", "d.SemanticFixture::sink(uint32)"},
		{"scope", "uint32"}, {"parameter_types", Json::array({"uint32"})},
	});

	const size_t declaration = SemanticSource.find("function route");
	for (const auto &parameter : std::vector<std::pair<std::string, std::string>>{
		{"x", xId}, {"y", yId}, {"amount", amountId}})
	{
		const size_t location = SemanticSource.find(parameter.first, declaration);
		root["refinement"]["symbols"].push_back(Json{
			{"id", parameter.second}, {"kind", "SourceFunctionParameter"},
			{"source_function_id", functionId},
			{"source_name", parameter.first}, {"preda_type", "uint32"},
			{"sort", uint32Sort},
			{"location", SemanticLocation(location,
				location + parameter.first.size() - 1)},
		});
	}
	root["refinement"]["constraints"].push_back(Json{
		{"id", "semantic.target"}, {"kind", "RelayTargetRelation"},
		{"source_function_id", functionId}, {"relay_site_id", siteId},
		{"argument_index", -1}, {"formula", std::move(targetRelation)},
	});
	root["refinement"]["constraints"].push_back(Json{
		{"id", "semantic.argument"}, {"kind", "RelayArgumentRelation"},
		{"source_function_id", functionId}, {"relay_site_id", siteId},
		{"argument_index", 0}, {"formula", std::move(argumentRelation)},
	});
	root["refinement"]["constraints"].push_back(Json{
		{"id", "semantic.guard"}, {"kind", "RelayGuardNecessity"},
		{"source_function_id", functionId}, {"relay_site_id", siteId},
		{"argument_index", -1}, {"formula", std::move(guardRelation)},
	});
	return root;
}

std::map<MutationKind, size_t> Counts(const MutationGenerationResult &result)
{
	std::map<MutationKind, size_t> counts;
	for (const MutationRecord &record : result.mutations)
	{
		if (record.status == MutationGenerationStatus::Generated)
			++counts[record.mutationType];
	}
	return counts;
}

std::vector<std::string> StableIds(const MutationGenerationResult &result)
{
	std::vector<std::string> ids;
	for (const MutationRecord &record : result.mutations)
		ids.push_back(record.mutationId);
	std::sort(ids.begin(), ids.end());
	return ids;
}

bool HasSites(
	const MutationRecord &record,
	std::initializer_list<const char *> expected)
{
	std::vector<std::string> actual = record.relaySiteIds;
	std::vector<std::string> wanted;
	for (const char *site : expected)
		wanted.emplace_back(site);
	std::sort(actual.begin(), actual.end());
	std::sort(wanted.begin(), wanted.end());
	return actual == wanted;
}

const SourceEdit *FindEdit(
	const MutationRecord &record,
	const std::string &expected,
	const std::string &replacement)
{
	for (const SourceEdit &edit : record.edits)
	{
		if (edit.expected == expected && edit.replacement == replacement)
			return &edit;
	}
	return nullptr;
}

bool IsIdentifier(const std::string &value)
{
	if (value.empty() ||
		!(std::isalpha(static_cast<unsigned char>(value.front())) ||
			value.front() == '_'))
	{
		return false;
	}
	return std::all_of(value.begin() + 1, value.end(), [](char character) {
		return std::isalnum(static_cast<unsigned char>(character)) ||
			character == '_';
	});
}

Json RenumberManifest(Json manifest)
{
	std::unordered_map<std::string, std::string> siteIds;
	for (size_t index = 0; index < manifest["relay_sites"].size(); ++index)
	{
		const std::string old = manifest["relay_sites"][index]["id"];
		siteIds.emplace(old, "renumbered_site_" + std::to_string(100 + index));
	}
	std::unordered_map<std::string, std::string> handlerIds;
	for (size_t index = 0; index < manifest["handlers"].size(); ++index)
	{
		const std::string old = manifest["handlers"][index]["id"];
		handlerIds.emplace(old,
			"renumbered_handler_" + std::to_string(200 + index));
	}
	std::unordered_map<std::string, std::string> functionIds;
	for (size_t index = 0;
		index < manifest["control_flow"]["functions"].size(); ++index)
	{
		const std::string old =
			manifest["control_flow"]["functions"][index]["function_id"];
		functionIds.emplace(old,
			"renumbered_function_" + std::to_string(300 + index));
	}
	for (const Json &handler : manifest["handlers"])
	{
		const std::string old = handler.value("target_function_id", std::string());
		if (!old.empty() && functionIds.count(old) == 0)
			functionIds.emplace(old, "renumbered_synthetic_lambda_function");
	}

	for (Json &site : manifest["relay_sites"])
	{
		site["id"] = siteIds.at(site["id"].get<std::string>());
		site["handler_id"] =
			handlerIds.at(site["handler_id"].get<std::string>());
		site["source_function_id"] =
			functionIds.at(site["source_function_id"].get<std::string>());
	}
	for (Json &handler : manifest["handlers"])
	{
		handler["id"] = handlerIds.at(handler["id"].get<std::string>());
		handler["target_function_id"] = functionIds.at(
			handler["target_function_id"].get<std::string>());
	}
	for (Json &function : manifest["control_flow"]["functions"])
	{
		function["function_id"] =
			functionIds.at(function["function_id"].get<std::string>());
	}
	for (Json &symbol : manifest["refinement"]["symbols"])
	{
		symbol["source_function_id"] =
			functionIds.at(symbol["source_function_id"].get<std::string>());
	}
	for (Json &function : manifest["parallel_certificate"]["functions"])
	{
		function["source_function_id"] = functionIds.at(
			function["source_function_id"].get<std::string>());
		for (Json &pair : function["pair_relations"])
		{
			pair["site_a"] = siteIds.at(pair["site_a"].get<std::string>());
			pair["site_b"] = siteIds.at(pair["site_b"].get<std::string>());
			pair["source_function_id"] = functionIds.at(
				pair["source_function_id"].get<std::string>());
		}
	}
	std::reverse(manifest["relay_sites"].begin(),
		manifest["relay_sites"].end());
	std::reverse(manifest["handlers"].begin(), manifest["handlers"].end());
	return manifest;
}

void TestAllOperatorsAndSafetyConditions()
{
	const Json manifest = BuildManifest();
	MutationOptions options;
	options.seed = 88;
	options.maxMutantsPerKind = 32;
	const MutationEngine engine;
	const auto result = engine.Generate(Source, manifest.dump(), options);
	CHECK(result.diagnostics.empty());
	const auto counts = Counts(result);
	for (MutationKind kind : AllMutationKinds)
		CHECK(counts.count(kind) != 0 && counts.at(kind) != 0);
	CHECK(counts.at(MutationKind::GuardNegate) == 1);
	CHECK(counts.at(MutationKind::RelayOrderSwap) == 1);
	CHECK(counts.at(MutationKind::IntroduceAlias) == 1);

	std::set<std::string> uniqueIds;
	bool sawGuard = false;
	bool sawBroadcastToSingle = false;
	bool sawSingleToBroadcast = false;
	bool sawCaretCapture = false;
	bool sawCommentSafeHandler = false;
	bool sawEarlyReturnRecursion = false;
	bool sawCertifiedOrder = false;
	bool sawCertifiedAlias = false;
	for (const MutationRecord &record : result.mutations)
	{
		CHECK(uniqueIds.insert(record.mutationId).second);
		CHECK(record.originalCode == Source);
		CHECK(record.mutatedCode != Source);
		CHECK(record.mutatedCode.find(u8"变异框架") != std::string::npos);
		CHECK(record.mutationId.size() >= 32);
		CHECK(std::all_of(record.mutationId.end() - 32,
			record.mutationId.end(), [](char character) {
				return std::isxdigit(static_cast<unsigned char>(character));
			}));
		CHECK(record.location.startOffset >= 0);
		CHECK(record.location.endOffset >= record.location.startOffset);
		CHECK(record.location.line > 0);
		CHECK(record.location.endLine >= record.location.line);

		if (record.mutationType == MutationKind::GuardNegate)
			sawGuard = record.mutatedCode.find("if (!(choose))") !=
				std::string::npos;
		if (record.mutationType == MutationKind::BroadcastToSingle)
		{
			CHECK(record.edits.size() == 2);
			CHECK(FindEdit(record, "shards", "next") != nullptr);
			CHECK(FindEdit(record, "hs", "hg") != nullptr);
			sawBroadcastToSingle = record.mutatedCode.find(
				"relay@/* kind */next hg(value);") != std::string::npos;
		}
		if (record.mutationType == MutationKind::SingleToBroadcast)
		{
			CHECK(record.edits.size() == 2);
			CHECK(FindEdit(record, "next", "shards") != nullptr);
			CHECK(FindEdit(record, "hg", "hs") != nullptr);
			sawSingleToBroadcast = record.mutatedCode.find(
				"relay@shards hs(value);") != std::string::npos;
		}
		if (record.mutationType == MutationKind::RelayArgumentReplace &&
			HasSites(record, {"site_7"}))
		{
			CHECK(record.edits.size() == 1);
			CHECK(record.edits[0].expected == "value");
			CHECK(IsIdentifier(record.edits[0].replacement));
			CHECK(record.edits[0].replacement != "0u32");
			CHECK(record.mutatedCode.find(
				"^" + record.edits[0].replacement + ")") != std::string::npos);
			sawCaretCapture = true;
		}
		if (record.mutationType == MutationKind::HandlerReplace &&
			HasSites(record, {"site_0"}))
		{
			const size_t decoy = Source.find("decoy h1(");
			CHECK(record.edits.size() == 1);
			CHECK(record.edits[0].startOffset >
				static_cast<int64_t>(decoy + std::string("decoy h1(").size()));
			CHECK(record.mutatedCode.find("/* decoy h1( */ h2(value)") !=
				std::string::npos);
			sawCommentSafeHandler = true;
		}
		if (record.mutationType == MutationKind::IntroduceRelayRecursion &&
			record.sourceFunctionId == "d.MutationFixture::h2(uint32)")
		{
			const size_t h2 = record.mutatedCode.find("function h2(");
			const size_t inserted = record.mutatedCode.find(
				"relay@value h2(value);", h2);
			const size_t earlyReturn = record.mutatedCode.find("return;", h2);
			CHECK(inserted != std::string::npos && inserted < earlyReturn);
			sawEarlyReturnRecursion = true;
		}
		if (record.mutationType == MutationKind::RelayOrderSwap)
		{
			CHECK(HasSites(record, {"site_2", "site_3"}));
			sawCertifiedOrder = true;
		}
		if (record.mutationType == MutationKind::IntroduceAlias)
		{
			CHECK(HasSites(record, {"site_2", "site_6"}));
			CHECK(FindEdit(record, "(base + 1u32)", "base") != nullptr);
			sawCertifiedAlias = true;
		}
	}
	CHECK(sawGuard);
	CHECK(sawBroadcastToSingle);
	CHECK(sawSingleToBroadcast);
	CHECK(sawCaretCapture);
	CHECK(sawCommentSafeHandler);
	CHECK(sawEarlyReturnRecursion);
	CHECK(sawCertifiedOrder);
	CHECK(sawCertifiedAlias);
}

void TestCertificatePreconditions()
{
	MutationOptions aliasOnly;
	aliasOnly.enabledKinds = {MutationKind::IntroduceAlias};
	Json manifest = BuildManifest();
	for (Json &pair :
		manifest["parallel_certificate"]["functions"][0]["pair_relations"])
	{
		if (pair["site_a"] == "site_2" && pair["site_b"] == "site_6")
			pair["status"] = "Unknown";
	}
	const MutationEngine engine;
	const auto noAlias = engine.Generate(Source, manifest.dump(), aliasOnly);
	CHECK(noAlias.diagnostics.empty());
	CHECK(Counts(noAlias).count(MutationKind::IntroduceAlias) == 0);

	MutationOptions orderOnly;
	orderOnly.enabledKinds = {MutationKind::RelayOrderSwap};
	manifest = BuildManifest();
	for (Json &pair :
		manifest["parallel_certificate"]["functions"][0]["pair_relations"])
	{
		if (pair["site_a"] == "site_2" && pair["site_b"] == "site_3")
			pair["relation"] = "CoEmissionIndependent";
	}
	const auto noOrder = engine.Generate(Source, manifest.dump(), orderOnly);
	CHECK(noOrder.diagnostics.empty());
	CHECK(Counts(noOrder).count(MutationKind::RelayOrderSwap) == 0);

	manifest = BuildManifest();
	for (Json &pair :
		manifest["parallel_certificate"]["functions"][0]["pair_relations"])
	{
		if (pair["site_a"] == "site_2" && pair["site_b"] == "site_3")
			pair["relation"] = "MustPrecedeBA";
	}
	const auto wrongDirection =
		engine.Generate(Source, manifest.dump(), orderOnly);
	CHECK(wrongDirection.diagnostics.empty());
	CHECK(Counts(wrongDirection).count(MutationKind::RelayOrderSwap) == 0);

	manifest = BuildManifest();
	for (Json &pair :
		manifest["parallel_certificate"]["functions"][0]["pair_relations"])
	{
		if (pair["site_a"] == "site_2" && pair["site_b"] == "site_3")
		{
			std::swap(pair["site_a"], pair["site_b"]);
			pair["relation"] = "MustPrecedeBA";
		}
	}
	const auto reversedCertificate =
		engine.Generate(Source, manifest.dump(), orderOnly);
	CHECK(reversedCertificate.diagnostics.empty());
	CHECK(Counts(reversedCertificate).at(MutationKind::RelayOrderSwap) == 1);

	manifest = BuildManifest();
	manifest["parallel_certificate"]["functions"][0]
		["source_function_id"] = "d.MutationFixture::unrelated()";
	const auto wrongContext =
		engine.Generate(Source, manifest.dump(), aliasOnly);
	CHECK(wrongContext.diagnostics.empty());
	CHECK(Counts(wrongContext).count(MutationKind::IntroduceAlias) == 0);
}

void TestStableIdsAcrossManifestRenumbering()
{
	const Json manifest = BuildManifest();
	MutationOptions options;
	options.seed = 88;
	options.maxMutantsPerKind = 32;
	const MutationEngine engine;
	const auto first = engine.Generate(Source, manifest.dump(), options);
	const auto second = engine.Generate(Source, manifest.dump(), options);
	CHECK(first.diagnostics.empty());
	CHECK(second.diagnostics.empty());
	CHECK(StableIds(first) == StableIds(second));
	const Json renumbered = RenumberManifest(manifest);
	const auto afterRenumber =
		engine.Generate(Source, renumbered.dump(), options);
	CHECK(afterRenumber.diagnostics.empty());
	CHECK(StableIds(first) == StableIds(afterRenumber));
}

void TestExpectedTextAndOverlapSafety()
{
	std::string mutated;
	std::string error;
	CHECK(!MutationEngine::ApplyEdits("abcdef", {{1, 2, "X", "wrong"}},
		mutated, error));
	CHECK(error.find("does not match") != std::string::npos);
	CHECK(!MutationEngine::ApplyEdits("abcdef",
		{{1, 3, "X", "bcd"}, {3, 4, "Y", "de"}}, mutated, error));
	CHECK(error == "mutation edits overlap");
	CHECK(MutationEngine::ApplyEdits("abcdef",
		{{1, 2, "X", "bc"}, {5, 4, "Y", ""}}, mutated, error));
	CHECK(mutated == "aXdeYf");
}

void TestSemanticOperatorsAreExplicitAndFormulaBacked()
{
	const Json manifest = BuildSemanticManifest();
	const MutationEngine engine;
	const auto defaultResult = engine.Generate(
		SemanticSource, manifest.dump(), MutationOptions());
	CHECK(defaultResult.diagnostics.empty());
	const auto defaultCounts = Counts(defaultResult);
	for (MutationKind kind : AllSemanticMutationKinds)
		CHECK(defaultCounts.count(kind) == 0);

	MutationOptions semanticOnly;
	semanticOnly.seed = 88;
	semanticOnly.maxMutantsPerKind = 32;
	semanticOnly.enabledKinds.assign(
		AllSemanticMutationKinds.begin(), AllSemanticMutationKinds.end());
	const auto result = engine.Generate(
		SemanticSource, manifest.dump(), semanticOnly);
	CHECK(result.diagnostics.empty());
	const auto counts = Counts(result);
	for (MutationKind kind : AllSemanticMutationKinds)
		CHECK(counts.count(kind) != 0 && counts.at(kind) != 0);

	bool sawTargetArithmetic = false;
	bool sawTargetVariable = false;
	bool sawArgumentArithmetic = false;
	bool sawGuardBoundary = false;
	const size_t rhsStart = SemanticSource.find("uint32(x)");
	for (const MutationRecord &record : result.mutations)
	{
		CHECK(record.status == MutationGenerationStatus::Generated);
		CHECK(record.relaySiteIds == std::vector<std::string>{"semantic_site"});
		switch (record.mutationType)
		{
		case MutationKind::TargetArithmeticPerturb:
			CHECK(record.edits.size() == 1);
			CHECK(record.edits[0].expected ==
				"uint32(x) * 65536u32 + uint32(y)");
			CHECK(record.edits[0].replacement ==
				"(uint32(x) * 65536u32 + uint32(y) + 1u32)");
			CHECK(static_cast<size_t>(record.edits[0].startOffset) == rhsStart);
			sawTargetArithmetic = true;
			break;
		case MutationKind::TargetVariableSwap:
			CHECK(record.edits.size() == 1);
			CHECK((record.edits[0].expected == "x" &&
				record.edits[0].replacement == "y") ||
				(record.edits[0].expected == "y" &&
					record.edits[0].replacement == "x"));
			// The FormulaIR leaf location must point at the target computation,
			// never at a function-parameter declaration.
			CHECK(static_cast<size_t>(record.edits[0].startOffset) >= rhsStart);
			sawTargetVariable = true;
			break;
		case MutationKind::ArgumentArithmeticPerturb:
			CHECK(record.edits.size() == 1);
			CHECK(record.edits[0].expected == "amount");
			CHECK(record.edits[0].replacement == "(amount + 1u32)");
			CHECK(record.mutatedCode.find("sink((amount + 1u32))") !=
				std::string::npos);
			sawArgumentArithmetic = true;
			break;
		case MutationKind::GuardBoundaryChange:
			CHECK(record.edits.size() == 1);
			CHECK(record.edits[0].expected == ">=");
			CHECK(record.edits[0].replacement == ">");
			CHECK(record.mutatedCode.find("if (amount > 1u32)") !=
				std::string::npos);
			sawGuardBoundary = true;
			break;
		default:
			CHECK(false);
		}
	}
	CHECK(sawTargetArithmetic);
	CHECK(sawTargetVariable);
	CHECK(sawArgumentArithmetic);
	CHECK(sawGuardBoundary);

	const auto repeat = engine.Generate(
		SemanticSource, manifest.dump(), semanticOnly);
	CHECK(repeat.diagnostics.empty());
	CHECK(StableIds(result) == StableIds(repeat));

	Json unsupportedManifest = manifest;
	unsupportedManifest["refinement"]["constraints"] = Json::array();
	semanticOnly.includeUnsupported = true;
	const auto unsupported = engine.Generate(
		SemanticSource, unsupportedManifest.dump(), semanticOnly);
	CHECK(unsupported.diagnostics.empty());
	CHECK(unsupported.mutations.size() == AllSemanticMutationKinds.size());
	for (const MutationRecord &record : unsupported.mutations)
		CHECK(record.status == MutationGenerationStatus::Unsupported);

	// A ^name lambda capture cannot be replaced with a parenthesized
	// expression directly.  Verify the semantic operator rewrites the whole
	// capture into PREDA's equivalent typed binding form and ignores the decoy
	// caret inside the comment.
	Json captureManifest = BuildManifest();
	captureManifest["refinement"]["constraints"] = Json::array();
	const size_t captureSiteStart = FindNth(
		"relay@target (/* ^fake */ ^value) {");
	const size_t captureSiteEnd = Source.find('}', captureSiteStart);
	const size_t captureArgument = Source.find("value", captureSiteStart);
	const Json uint32Sort = FormulaSort("UnsignedBitVector", 32);
	const Json boolSort = FormulaSort("Bool");
	const auto sourceSymbol = [&](const std::string &id,
		const std::string &text,
		const Json &sort,
		size_t start,
		size_t endInclusive) {
		return Json{
			{"kind", "Symbol"}, {"sort", sort},
			{"source_text", text}, {"symbol_id", id},
			{"operator", ""}, {"children", Json::array()},
			{"location", Location(start, endInclusive)},
		};
	};
	Json captureEmitted = sourceSymbol("capture.emitted", "emitted",
		boolSort, captureSiteStart, captureSiteEnd);
	Json captureActual = sourceSymbol("capture.actual", "actual_arg_0",
		uint32Sort, captureArgument,
		captureArgument + std::string("value").size() - 1);
	Json captureValue = sourceSymbol("capture.value", "value", uint32Sort,
		captureArgument,
		captureArgument + std::string("value").size() - 1);
	Json captureEquality{
		{"kind", "Binary"}, {"sort", boolSort}, {"operator", "=="},
		{"children", Json::array(
			{std::move(captureActual), std::move(captureValue)})},
		{"location", Location(captureArgument,
			captureArgument + std::string("value").size() - 1)},
	};
	Json captureRelation{
		{"kind", "Binary"}, {"sort", boolSort},
		{"operator", "implies"},
		{"children", Json::array(
			{std::move(captureEmitted), std::move(captureEquality)})},
		{"location", Location(captureSiteStart, captureSiteEnd)},
	};
	captureManifest["refinement"]["constraints"].push_back(Json{
		{"id", "capture.argument"},
		{"kind", "RelayArgumentRelation"},
		{"source_function_id",
			"d.MutationFixture::lambda_capture(uint32,uint32,uint32)"},
		{"relay_site_id", "site_7"}, {"argument_index", 0},
		{"formula", std::move(captureRelation)},
	});
	MutationOptions captureOnly;
	captureOnly.enabledKinds = {MutationKind::ArgumentArithmeticPerturb};
	const auto captureResult = engine.Generate(
		Source, captureManifest.dump(), captureOnly);
	CHECK(captureResult.diagnostics.empty());
	CHECK(captureResult.mutations.size() == 1);
	const MutationRecord &capture = captureResult.mutations.front();
	CHECK(capture.edits.size() == 1);
	CHECK(capture.edits[0].expected == "^value");
	CHECK(capture.edits[0].replacement ==
		"uint32 value = (value + 1u32)");
	CHECK(capture.mutatedCode.find(
		"(/* ^fake */ uint32 value = (value + 1u32))") !=
		std::string::npos);
}

} // namespace

int main()
{
	try
	{
		TestAllOperatorsAndSafetyConditions();
		TestCertificatePreconditions();
		TestStableIdsAcrossManifestRenumbering();
		TestExpectedTextAndOverlapSafety();
		TestSemanticOperatorsAreExplicitAndFormulaBacked();
		std::cout << "R-PREDA mutation engine tests passed\n";
		return 0;
	}
	catch (const std::exception &exception)
	{
		std::cerr << exception.what() << '\n';
		return 1;
	}
}
