#include "MutationEngine.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <initializer_list>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace rpreda {
namespace mutation {
namespace {

using Json = nlohmann::ordered_json;

class Utf8SourceMap
{
public:
	explicit Utf8SourceMap(const std::string &source)
	{
		m_codePointToByte.push_back(0);
		m_lineAtCodePoint.push_back(1);
		m_columnAtCodePoint.push_back(0);
		size_t cursor = 0;
		uint32_t line = 1;
		uint32_t column = 0;
		while (cursor < source.size())
		{
			const unsigned char lead =
				static_cast<unsigned char>(source[cursor]);
			size_t width = 1;
			if ((lead & 0x80U) == 0)
				width = 1;
			else if ((lead & 0xe0U) == 0xc0U)
				width = 2;
			else if ((lead & 0xf0U) == 0xe0U)
				width = 3;
			else if ((lead & 0xf8U) == 0xf0U)
				width = 4;
			else
				throw std::runtime_error("source is not valid UTF-8");
			if (cursor + width > source.size())
				throw std::runtime_error("source ends inside a UTF-8 sequence");
			for (size_t index = 1; index < width; ++index)
			{
				const unsigned char continuation =
					static_cast<unsigned char>(source[cursor + index]);
				if ((continuation & 0xc0U) != 0x80U)
					throw std::runtime_error("source contains invalid UTF-8");
			}
			if (width == 1 && source[cursor] == '\n')
			{
				++line;
				column = 0;
			}
			else
			{
				++column;
			}
			cursor += width;
			m_codePointToByte.push_back(cursor);
			m_lineAtCodePoint.push_back(line);
			m_columnAtCodePoint.push_back(column);
		}
	}

	void Map(SourceLocation &location) const
	{
		if (location.startOffset < 0 || location.endOffset < location.startOffset)
			return;
		const uint64_t start = static_cast<uint64_t>(location.startOffset);
		const uint64_t afterEnd = static_cast<uint64_t>(location.endOffset) + 1;
		if (afterEnd >= m_codePointToByte.size() ||
			start >= m_codePointToByte.size())
		{
			return;
		}
		location.byteStartOffset =
			static_cast<int64_t>(m_codePointToByte[static_cast<size_t>(start)]);
		location.byteEndOffset = static_cast<int64_t>(
			m_codePointToByte[static_cast<size_t>(afterEnd)] - 1);
		location.line = m_lineAtCodePoint[static_cast<size_t>(start)];
		location.column = m_columnAtCodePoint[static_cast<size_t>(start)];
		location.endLine = m_lineAtCodePoint[static_cast<size_t>(afterEnd)];
		location.endColumn = m_columnAtCodePoint[static_cast<size_t>(afterEnd)];
	}

	SourceLocation FromByteRange(
		const std::string &source,
		size_t byteStart,
		size_t byteEndInclusive) const
	{
		SourceLocation result;
		if (byteStart > byteEndInclusive || byteEndInclusive >= source.size())
			return result;
		const auto start = std::lower_bound(
			m_codePointToByte.begin(), m_codePointToByte.end(), byteStart);
		const auto afterEnd = std::lower_bound(
			m_codePointToByte.begin(), m_codePointToByte.end(),
			byteEndInclusive + 1);
		if (start == m_codePointToByte.end() || *start != byteStart ||
			afterEnd == m_codePointToByte.end() ||
			*afterEnd != byteEndInclusive + 1)
		{
			return result;
		}
		const size_t startIndex = static_cast<size_t>(
			std::distance(m_codePointToByte.begin(), start));
		const size_t afterEndIndex = static_cast<size_t>(
			std::distance(m_codePointToByte.begin(), afterEnd));
		result.startOffset = static_cast<int64_t>(startIndex);
		result.endOffset = static_cast<int64_t>(afterEndIndex - 1);
		result.byteStartOffset = static_cast<int64_t>(byteStart);
		result.byteEndOffset = static_cast<int64_t>(byteEndInclusive);
		result.line = m_lineAtCodePoint[startIndex];
		result.column = m_columnAtCodePoint[startIndex];
		result.endLine = m_lineAtCodePoint[afterEndIndex];
		result.endColumn = m_columnAtCodePoint[afterEndIndex];
		return result;
	}

private:
	std::vector<size_t> m_codePointToByte;
	std::vector<uint32_t> m_lineAtCodePoint;
	std::vector<uint32_t> m_columnAtCodePoint;
};

struct ExpressionView
{
	std::string text;
	std::string type;
	SourceLocation location;
};

// Read-only projection of the owning schema-v5 Formula IR.  Semantic
// mutations use these compiler-produced expression trees and locations rather
// than attempting to recover expression structure from source text.
struct FormulaView
{
	std::string kind;
	std::string sortKind;
	uint16_t bitWidth = 0;
	std::string sourceText;
	std::string op;
	std::string symbolId;
	std::string literalValue;
	SourceLocation location;
	std::vector<FormulaView> children;

	bool IsPresent() const noexcept
	{
		return !kind.empty();
	}

	bool IsKnown() const noexcept
	{
		if (kind.empty() || kind == "Unknown" || sortKind.empty() ||
			sortKind == "Unknown")
		{
			return false;
		}
		return std::all_of(children.begin(), children.end(),
			[](const FormulaView &child) { return child.IsKnown(); });
	}
};

struct ArgumentView
{
	std::string type;
	ExpressionView expression;
};

struct BranchView
{
	ExpressionView condition;
	bool polarity = true;
};

struct SiteView
{
	std::string id;
	std::string sourceFunctionId;
	std::string sourceScope;
	std::string relayKind;
	std::string targetScope;
	std::string targetFunction;
	std::string handlerId;
	SourceLocation location;
	ExpressionView target;
	std::vector<ArgumentView> arguments;
	std::vector<BranchView> branches;
	FormulaView refinementTarget;
	std::map<size_t, FormulaView> refinementArguments;
	FormulaView refinementGuard;
};

struct HandlerView
{
	std::string id;
	std::string kind;
	std::string name;
	std::string targetFunctionId;
	std::string scope;
	std::vector<std::string> parameterTypes;
};

struct ParameterView
{
	std::string name;
	std::string type;
	SourceLocation location;
};

struct FunctionView
{
	std::string id;
	std::string scope;
	SourceLocation location;
	std::vector<ParameterView> parameters;
};

struct RefinementSymbolView
{
	std::string id;
	std::string kind;
	std::string sourceFunctionId;
	std::string sourceName;
	std::string sourceType;
	std::string sortKind;
	uint16_t bitWidth = 0;
	SourceLocation location;
};

struct PairCertificateView
{
	std::string certificateFunctionId;
	std::string sourceFunctionId;
	std::string siteA;
	std::string siteB;
	std::string relation;
	std::string status;
};

struct ManifestView
{
	std::string contract;
	std::vector<SiteView> sites;
	std::unordered_map<std::string, HandlerView> handlers;
	std::unordered_map<std::string, FunctionView> functions;
	std::unordered_map<std::string, RefinementSymbolView> refinementSymbols;
	std::vector<PairCertificateView> pairCertificates;
};

SourceLocation ParseLocation(const Json &value, const Utf8SourceMap &sourceMap)
{
	SourceLocation result;
	if (!value.is_object())
		return result;
	result.line = value.value("line", uint32_t(0));
	result.column = value.value("column", uint32_t(0));
	result.endLine = value.value("end_line", uint32_t(0));
	result.endColumn = value.value("end_column", uint32_t(0));
	result.startOffset = value.value("start_offset", int64_t(-1));
	result.endOffset = value.value("end_offset", int64_t(-1));
	sourceMap.Map(result);
	return result;
}

ExpressionView ParseExpression(
	const Json &value,
	const Utf8SourceMap &sourceMap)
{
	ExpressionView result;
	if (!value.is_object())
		return result;
	result.text = value.value("text", std::string());
	result.type = value.value("type", std::string());
	if (value.contains("location"))
		result.location = ParseLocation(value["location"], sourceMap);
	return result;
}

FormulaView ParseFormula(
	const Json &value,
	const Utf8SourceMap &sourceMap)
{
	FormulaView result;
	if (!value.is_object())
		return result;
	result.kind = value.value("kind", std::string());
	result.sourceText = value.value("source_text", std::string());
	result.op = value.value("operator", std::string());
	result.symbolId = value.value("symbol_id", std::string());
	result.literalValue = value.value("literal_value", std::string());
	if (value.contains("sort") && value["sort"].is_object())
	{
		result.sortKind = value["sort"].value("kind", std::string());
		const uint32_t width =
			value["sort"].value("bit_width", uint32_t(0));
		result.bitWidth = width <= UINT16_MAX
			? static_cast<uint16_t>(width)
			: 0;
	}
	if (value.contains("location"))
		result.location = ParseLocation(value["location"], sourceMap);
	if (value.contains("children") && value["children"].is_array())
	{
		for (const Json &child : value["children"])
			result.children.push_back(ParseFormula(child, sourceMap));
	}
	return result;
}

const Json *RelationRightHandSide(
	const Json &formula,
	bool equalityConsequent)
{
	if (!formula.is_object() ||
		formula.value("kind", std::string()) != "Binary" ||
		formula.value("operator", std::string()) != "implies" ||
		!formula.contains("children") ||
		!formula["children"].is_array() ||
		formula["children"].size() != 2)
	{
		return nullptr;
	}
	const Json &consequent = formula["children"][1];
	if (!equalityConsequent)
		return &consequent;
	if (!consequent.is_object() ||
		consequent.value("kind", std::string()) != "Binary" ||
		consequent.value("operator", std::string()) != "==" ||
		!consequent.contains("children") ||
		!consequent["children"].is_array() ||
		consequent["children"].size() != 2)
	{
		return nullptr;
	}
	return &consequent["children"][1];
}

ManifestView ParseManifest(const Json &root, const std::string &source)
{
	if (!root.is_object())
		throw std::runtime_error("relay manifest root is not an object");
	if (!root.contains("relay_sites") || !root["relay_sites"].is_array())
		throw std::runtime_error("relay manifest has no relay_sites array");

	const Utf8SourceMap sourceMap(source);
	ManifestView result;
	result.contract = root.value("contract", std::string());
	for (const Json &entry : root["relay_sites"])
	{
		if (!entry.is_object())
			continue;
		SiteView site;
		site.id = entry.value("id", std::string());
		site.sourceFunctionId =
			entry.value("source_function_id", std::string());
		site.sourceScope = entry.value("source_scope", std::string());
		site.relayKind = entry.value("relay_kind", std::string());
		site.targetScope = entry.value("target_scope", std::string());
		site.targetFunction =
			entry.value("target_function", std::string());
		site.handlerId = entry.value("handler_id", std::string());
		if (entry.contains("location"))
			site.location = ParseLocation(entry["location"], sourceMap);
		if (entry.contains("target"))
			site.target = ParseExpression(entry["target"], sourceMap);
		if (entry.contains("arguments") && entry["arguments"].is_array())
		{
			for (const Json &argumentJson : entry["arguments"])
			{
				ArgumentView argument;
				argument.type =
					argumentJson.value("type", std::string());
				if (argumentJson.contains("expression"))
					argument.expression =
						ParseExpression(argumentJson["expression"], sourceMap);
				site.arguments.push_back(std::move(argument));
			}
		}
		if (entry.contains("branches") && entry["branches"].is_array())
		{
			for (const Json &branchJson : entry["branches"])
			{
				BranchView branch;
				branch.polarity = branchJson.value("polarity", true);
				if (branchJson.contains("condition"))
					branch.condition =
						ParseExpression(branchJson["condition"], sourceMap);
				site.branches.push_back(std::move(branch));
			}
		}
		result.sites.push_back(std::move(site));
	}

	if (root.contains("handlers") && root["handlers"].is_array())
	{
		for (const Json &entry : root["handlers"])
		{
			HandlerView handler;
			handler.id = entry.value("id", std::string());
			handler.kind = entry.value("kind", std::string());
			handler.name = entry.value("name", std::string());
			handler.targetFunctionId =
				entry.value("target_function_id", std::string());
			handler.scope = entry.value("scope", std::string());
			if (entry.contains("parameter_types") &&
				entry["parameter_types"].is_array())
			{
				for (const Json &parameterType : entry["parameter_types"])
					handler.parameterTypes.push_back(
						parameterType.get<std::string>());
			}
			if (!handler.id.empty())
				result.handlers.emplace(handler.id, std::move(handler));
		}
	}

	const Json *controlFlow = root.contains("control_flow")
		? &root["control_flow"]
		: nullptr;
	if (controlFlow != nullptr && controlFlow->is_object() &&
		controlFlow->contains("functions") &&
		(*controlFlow)["functions"].is_array())
	{
		for (const Json &entry : (*controlFlow)["functions"])
		{
			FunctionView function;
			function.id = entry.value("function_id", std::string());
			function.scope = entry.value("scope_name", std::string());
			if (entry.contains("location"))
				function.location = ParseLocation(entry["location"], sourceMap);
			if (!function.id.empty())
				result.functions.emplace(function.id, std::move(function));
		}
	}

	if (root.contains("refinement") && root["refinement"].is_object() &&
		root["refinement"].contains("symbols") &&
		root["refinement"]["symbols"].is_array())
	{
		for (const Json &symbol : root["refinement"]["symbols"])
		{
			RefinementSymbolView symbolView;
			symbolView.id = symbol.value("id", std::string());
			symbolView.kind = symbol.value("kind", std::string());
			symbolView.sourceFunctionId =
				symbol.value("source_function_id", std::string());
			symbolView.sourceName =
				symbol.value("source_name", std::string());
			symbolView.sourceType =
				symbol.value("preda_type", std::string());
			if (symbol.contains("sort") && symbol["sort"].is_object())
			{
				symbolView.sortKind =
					symbol["sort"].value("kind", std::string());
				const uint32_t width = symbol["sort"].value(
					"bit_width", uint32_t(0));
				symbolView.bitWidth = width <= UINT16_MAX
					? static_cast<uint16_t>(width)
					: 0;
			}
			if (symbol.contains("location"))
				symbolView.location =
					ParseLocation(symbol["location"], sourceMap);
			if (!symbolView.id.empty())
				result.refinementSymbols.emplace(
					symbolView.id, symbolView);
			if (symbolView.kind != "SourceFunctionParameter")
				continue;
			const std::string functionId =
				symbolView.sourceFunctionId;
			if (functionId.empty())
				continue;
			FunctionView &function = result.functions[functionId];
			function.id = functionId;
			ParameterView parameter;
			parameter.name = symbolView.sourceName;
			parameter.type = symbolView.sourceType;
			parameter.location = symbolView.location;
			function.parameters.push_back(std::move(parameter));
		}
	}

	if (root.contains("refinement") && root["refinement"].is_object() &&
		root["refinement"].contains("constraints") &&
		root["refinement"]["constraints"].is_array())
	{
		for (const Json &constraint : root["refinement"]["constraints"])
		{
			if (!constraint.is_object() || !constraint.contains("formula"))
				continue;
			const std::string siteId =
				constraint.value("relay_site_id", std::string());
			auto found = std::find_if(result.sites.begin(), result.sites.end(),
				[&siteId](const SiteView &site) { return site.id == siteId; });
			if (found == result.sites.end())
				continue;
			const std::string kind =
				constraint.value("kind", std::string());
			const bool equality = kind == "RelayTargetRelation" ||
				kind == "RelayArgumentRelation";
			const Json *rhs = RelationRightHandSide(
				constraint["formula"], equality);
			if (rhs == nullptr)
				continue;
			FormulaView parsed = ParseFormula(*rhs, sourceMap);
			if (kind == "RelayTargetRelation")
				found->refinementTarget = std::move(parsed);
			else if (kind == "RelayArgumentRelation")
			{
				const int64_t argumentIndex =
					constraint.value("argument_index", int64_t(-1));
				if (argumentIndex >= 0)
					found->refinementArguments.emplace(
						static_cast<size_t>(argumentIndex),
						std::move(parsed));
			}
			else if (kind == "RelayGuardNecessity")
				found->refinementGuard = std::move(parsed);
		}
	}

	if (root.contains("parallel_certificate") &&
		root["parallel_certificate"].is_object() &&
		root["parallel_certificate"].contains("functions") &&
		root["parallel_certificate"]["functions"].is_array())
	{
		for (const Json &function :
			root["parallel_certificate"]["functions"])
		{
			if (!function.is_object() ||
				!function.contains("pair_relations") ||
				!function["pair_relations"].is_array())
			{
				continue;
			}
			const std::string certificateFunctionId =
				function.value("source_function_id", std::string());
			for (const Json &pair : function["pair_relations"])
			{
				if (!pair.is_object())
					continue;
				PairCertificateView view;
				view.certificateFunctionId = certificateFunctionId;
				view.sourceFunctionId = pair.value(
					"source_function_id", certificateFunctionId);
				view.siteA = pair.value("site_a", std::string());
				view.siteB = pair.value("site_b", std::string());
				view.relation = pair.value("relation", std::string());
				view.status = pair.value("status", std::string());
				if (!view.siteA.empty() && !view.siteB.empty())
					result.pairCertificates.push_back(std::move(view));
			}
		}
	}
	for (auto &entry : result.functions)
	{
		auto &parameters = entry.second.parameters;
		std::sort(parameters.begin(), parameters.end(),
			[](const ParameterView &left, const ParameterView &right) {
				return left.location.byteStartOffset < right.location.byteStartOffset;
			});
	}
	std::sort(result.sites.begin(), result.sites.end(),
		[](const SiteView &left, const SiteView &right) {
			if (left.location.byteStartOffset != right.location.byteStartOffset)
				return left.location.byteStartOffset < right.location.byteStartOffset;
			return left.id < right.id;
		});
	return result;
}

uint64_t Fnv1a64(const std::string &value)
{
	uint64_t hash = UINT64_C(14695981039346656037);
	for (unsigned char byte : value)
	{
		hash ^= uint64_t(byte);
		hash *= UINT64_C(1099511628211);
	}
	return hash;
}

std::string Hex64(uint64_t value)
{
	std::ostringstream output;
	output << std::hex << std::setfill('0') << std::setw(16) << value;
	return output.str();
}

std::string Hash128(const std::string &value)
{
	return Hex64(Fnv1a64("rpreda-hash128-a|" + value)) +
		Hex64(Fnv1a64("rpreda-hash128-b|" + value));
}

std::string Digest(const std::string &value)
{
	return "fnv1a128:" + Hash128(value);
}

void AppendCanonical(std::ostringstream &output, const std::string &value)
{
	output << value.size() << ':' << value;
}

std::string Fragment(
	const std::string &source,
	const SourceLocation &location)
{
	if (!location.IsValidFor(source))
		return {};
	return source.substr(
		static_cast<size_t>(location.byteStartOffset),
		static_cast<size_t>(
			location.byteEndOffset - location.byteStartOffset + 1));
}

bool IsIdentifierCharacter(char value)
{
	return std::isalnum(static_cast<unsigned char>(value)) || value == '_';
}

bool IsPredaIdentifier(const std::string &value)
{
	if (value.empty() ||
		!(std::isalpha(static_cast<unsigned char>(value.front())) ||
			value.front() == '_'))
	{
		return false;
	}
	return std::all_of(value.begin() + 1, value.end(),
		[](char character) { return IsIdentifierCharacter(character); });
}

size_t SkipTrivia(
	const std::string &source,
	size_t cursor,
	size_t end)
{
	for (;;)
	{
		while (cursor < end &&
			std::isspace(static_cast<unsigned char>(source[cursor])))
		{
			++cursor;
		}
		if (cursor + 1 >= end || source[cursor] != '/')
			return cursor;
		if (source[cursor + 1] == '/')
		{
			cursor += 2;
			while (cursor < end && source[cursor] != '\n')
				++cursor;
			continue;
		}
		if (source[cursor + 1] == '*')
		{
			const size_t close = source.find("*/", cursor + 2);
			if (close == std::string::npos || close + 2 > end)
				return end;
			cursor = close + 2;
			continue;
		}
		return cursor;
	}
}

bool ContainsOnlyTrivia(
	const std::string &source,
	size_t begin,
	size_t end)
{
	return begin <= end && SkipTrivia(source, begin, end) == end;
}

size_t FindCodeCharacter(
	const std::string &source,
	size_t begin,
	size_t end,
	char sought)
{
	enum class Mode { Code, String, LineComment, BlockComment };
	Mode mode = Mode::Code;
	bool escaped = false;
	for (size_t cursor = begin; cursor < end; ++cursor)
	{
		const char current = source[cursor];
		const char next = cursor + 1 < end ? source[cursor + 1] : '\0';
		if (mode == Mode::LineComment)
		{
			if (current == '\n')
				mode = Mode::Code;
			continue;
		}
		if (mode == Mode::BlockComment)
		{
			if (current == '*' && next == '/')
			{
				mode = Mode::Code;
				++cursor;
			}
			continue;
		}
		if (mode == Mode::String)
		{
			if (escaped)
				escaped = false;
			else if (current == '\\')
				escaped = true;
			else if (current == '"')
				mode = Mode::Code;
			continue;
		}
		if (current == '/' && next == '/')
		{
			mode = Mode::LineComment;
			++cursor;
		}
		else if (current == '/' && next == '*')
		{
			mode = Mode::BlockComment;
			++cursor;
		}
		else if (current == '"')
		{
			mode = Mode::String;
		}
		else if (current == sought)
		{
			return cursor;
		}
	}
	return std::string::npos;
}

char PreviousSignificantCodeCharacter(
	const std::string &source,
	size_t begin,
	size_t end)
{
	enum class Mode { Code, String, LineComment, BlockComment };
	Mode mode = Mode::Code;
	bool escaped = false;
	char previous = '\0';
	for (size_t cursor = begin; cursor < end; ++cursor)
	{
		const char current = source[cursor];
		const char next = cursor + 1 < end ? source[cursor + 1] : '\0';
		if (mode == Mode::LineComment)
		{
			if (current == '\n')
				mode = Mode::Code;
			continue;
		}
		if (mode == Mode::BlockComment)
		{
			if (current == '*' && next == '/')
			{
				mode = Mode::Code;
				++cursor;
			}
			continue;
		}
		if (mode == Mode::String)
		{
			if (escaped)
				escaped = false;
			else if (current == '\\')
				escaped = true;
			else if (current == '"')
			{
				mode = Mode::Code;
				previous = '"';
			}
			continue;
		}
		if (current == '/' && next == '/')
		{
			mode = Mode::LineComment;
			++cursor;
		}
		else if (current == '/' && next == '*')
		{
			mode = Mode::BlockComment;
			++cursor;
		}
		else if (current == '"')
		{
			mode = Mode::String;
		}
		else if (!std::isspace(static_cast<unsigned char>(current)))
		{
			previous = current;
		}
	}
	return previous;
}

int64_t PreviousSignificantCodeOffset(
	const std::string &source,
	size_t begin,
	size_t end)
{
	enum class Mode { Code, String, LineComment, BlockComment };
	Mode mode = Mode::Code;
	bool escaped = false;
	int64_t previous = -1;
	for (size_t cursor = begin; cursor < end; ++cursor)
	{
		const char current = source[cursor];
		const char next = cursor + 1 < end ? source[cursor + 1] : '\0';
		if (mode == Mode::LineComment)
		{
			if (current == '\n')
				mode = Mode::Code;
			continue;
		}
		if (mode == Mode::BlockComment)
		{
			if (current == '*' && next == '/')
			{
				mode = Mode::Code;
				++cursor;
			}
			continue;
		}
		if (mode == Mode::String)
		{
			if (escaped)
				escaped = false;
			else if (current == '\\')
				escaped = true;
			else if (current == '"')
			{
				mode = Mode::Code;
				previous = static_cast<int64_t>(cursor);
			}
			continue;
		}
		if (current == '/' && next == '/')
		{
			mode = Mode::LineComment;
			++cursor;
		}
		else if (current == '/' && next == '*')
		{
			mode = Mode::BlockComment;
			++cursor;
		}
		else if (current == '"')
		{
			mode = Mode::String;
		}
		else if (!std::isspace(static_cast<unsigned char>(current)))
		{
			previous = static_cast<int64_t>(cursor);
		}
	}
	return previous;
}

SourceLocation FindRelayTypeLocation(
	const std::string &source,
	const Utf8SourceMap &sourceMap,
	const SiteView &site)
{
	if (site.target.location.IsValidFor(source))
	{
		return sourceMap.FromByteRange(source,
			static_cast<size_t>(site.target.location.byteStartOffset),
			static_cast<size_t>(site.target.location.byteEndOffset));
	}
	SourceLocation result;
	if (!site.location.IsValidFor(source))
		return result;
	const size_t begin = static_cast<size_t>(site.location.byteStartOffset);
	const size_t end = static_cast<size_t>(site.location.byteEndOffset) + 1;
	const size_t at = FindCodeCharacter(source, begin, end, '@');
	if (at == std::string::npos || at >= end)
		return result;
	size_t tokenStart = SkipTrivia(source, at + 1, end);
	size_t tokenEnd = tokenStart;
	while (tokenEnd < end && IsIdentifierCharacter(source[tokenEnd]))
		++tokenEnd;
	if (tokenStart == tokenEnd)
		return result;
	return sourceMap.FromByteRange(source, tokenStart, tokenEnd - 1);
}

SourceLocation FindNamedHandlerLocation(
	const std::string &source,
	const Utf8SourceMap &sourceMap,
	const SiteView &site,
	const HandlerView &handler)
{
	SourceLocation result;
	if (!site.location.IsValidFor(source) || handler.name.empty())
		return result;
	const SourceLocation typeLocation =
		FindRelayTypeLocation(source, sourceMap, site);
	size_t cursor = typeLocation.byteEndOffset >= 0
		? static_cast<size_t>(typeLocation.byteEndOffset + 1)
		: static_cast<size_t>(site.location.byteStartOffset);
	const size_t end = static_cast<size_t>(site.location.byteEndOffset) + 1;
	cursor = SkipTrivia(source, cursor, end);
	const size_t after = cursor + handler.name.size();
	if (after > end || source.compare(cursor, handler.name.size(), handler.name) != 0 ||
		(cursor > 0 && IsIdentifierCharacter(source[cursor - 1])) ||
		(after < source.size() && IsIdentifierCharacter(source[after])))
	{
		return result;
	}
	const size_t next = SkipTrivia(source, after, end);
	if (next < end && source[next] == '(')
		return sourceMap.FromByteRange(source, cursor, after - 1);
	return result;
}

std::string IndentationAt(const std::string &source, int64_t offset)
{
	if (offset < 0 || static_cast<uint64_t>(offset) > source.size())
		return {};
	const size_t position = static_cast<size_t>(offset);
	const size_t lineStart = position == 0
		? 0
		: source.rfind('\n', position - 1) == std::string::npos
			? 0
			: source.rfind('\n', position - 1) + 1;
	size_t cursor = lineStart;
	while (cursor < source.size() && cursor < position &&
		(source[cursor] == ' ' || source[cursor] == '\t'))
	{
		++cursor;
	}
	return source.substr(lineStart, cursor - lineStart);
}

std::string BlankPreservingNewlines(std::string value)
{
	for (char &character : value)
	{
		if (character != '\n' && character != '\r')
			character = ' ';
	}
	return value;
}

bool NonOverlapping(const SourceLocation &left, const SourceLocation &right)
{
	return left.byteEndOffset < right.byteStartOffset ||
		right.byteEndOffset < left.byteStartOffset;
}

int64_t EnclosingBraceAt(const std::string &source, int64_t byteOffset)
{
	if (byteOffset < 0 || static_cast<uint64_t>(byteOffset) > source.size())
		return -1;
	enum class Mode { Code, String, LineComment, BlockComment };
	Mode mode = Mode::Code;
	bool escaped = false;
	std::vector<int64_t> braces;
	for (size_t cursor = 0; cursor < static_cast<size_t>(byteOffset); ++cursor)
	{
		const char current = source[cursor];
		const char next = cursor + 1 < source.size() ? source[cursor + 1] : '\0';
		if (mode == Mode::LineComment)
		{
			if (current == '\n')
				mode = Mode::Code;
			continue;
		}
		if (mode == Mode::BlockComment)
		{
			if (current == '*' && next == '/')
			{
				mode = Mode::Code;
				++cursor;
			}
			continue;
		}
		if (mode == Mode::String)
		{
			if (escaped)
			{
				escaped = false;
				continue;
			}
			if (current == '\\')
				escaped = true;
			else if (current == '"')
				mode = Mode::Code;
			continue;
		}
		if (current == '/' && next == '/')
		{
			mode = Mode::LineComment;
			++cursor;
		}
		else if (current == '/' && next == '*')
		{
			mode = Mode::BlockComment;
			++cursor;
		}
		else if (current == '"')
		{
			mode = Mode::String;
		}
		else if (current == '{')
		{
			braces.push_back(static_cast<int64_t>(cursor));
		}
		else if (current == '}' && !braces.empty())
		{
			braces.pop_back();
		}
	}
	return braces.empty() ? -1 : braces.back();
}

std::string DefaultExpression(
	const std::string &type,
	const std::string &sourceScope)
{
	if (type == "bool")
		return "false";
	if (type == "address" && sourceScope == "address")
		return "__transaction.get_self_address()";
	if (type == "bigint")
		return "0ib";
	if (type.rfind("uint", 0) == 0)
		return "0u" + type.substr(4);
	if (type.rfind("int", 0) == 0)
		return "0i" + type.substr(3);
	return {};
}

std::string UnitLiteral(
	const FormulaView &formula,
	const std::string &predaType)
{
	if (formula.sortKind == "UnsignedBitVector" && formula.bitWidth != 0)
		return "1u" + std::to_string(formula.bitWidth);
	if (formula.sortKind != "Int")
		return {};
	if (predaType == "bigint")
		return "1ib";
	if (predaType.rfind("int", 0) == 0 && predaType.size() > 3)
		return "1i" + predaType.substr(3);
	return {};
}

bool SameFormulaSort(
	const FormulaView &formula,
	const RefinementSymbolView &symbol)
{
	return formula.sortKind == symbol.sortKind &&
		(formula.sortKind != "UnsignedBitVector" ||
			formula.bitWidth == symbol.bitWidth);
}

void CollectSymbolFormulaLeaves(
	const FormulaView &formula,
	std::vector<const FormulaView *> &result)
{
	if (formula.kind == "Symbol" && !formula.symbolId.empty())
		result.push_back(&formula);
	for (const FormulaView &child : formula.children)
		CollectSymbolFormulaLeaves(child, result);
}

bool IsBoundaryComparison(const std::string &op)
{
	return op == ">=" || op == "<=" || op == ">" || op == "<";
}

std::string ChangedBoundaryOperator(const std::string &op)
{
	if (op == ">=") return ">";
	if (op == "<=") return "<";
	if (op == ">") return ">=";
	if (op == "<") return "<=";
	return {};
}

void CollectBoundaryComparisons(
	const FormulaView &formula,
	std::vector<const FormulaView *> &result)
{
	if (formula.kind == "Binary" && formula.children.size() == 2 &&
		IsBoundaryComparison(formula.op))
	{
		result.push_back(&formula);
	}
	for (const FormulaView &child : formula.children)
		CollectBoundaryComparisons(child, result);
}

SourceLocation FindFormulaOperatorLocation(
	const std::string &source,
	const Utf8SourceMap &sourceMap,
	const FormulaView &formula)
{
	SourceLocation result;
	if (formula.children.size() != 2 || formula.op.empty())
		return result;
	const SourceLocation &left = formula.children[0].location;
	const SourceLocation &right = formula.children[1].location;
	if (!left.IsValidFor(source) || !right.IsValidFor(source) ||
		left.byteEndOffset >= right.byteStartOffset)
	{
		return result;
	}
	const size_t begin = static_cast<size_t>(left.byteEndOffset + 1);
	const size_t end = static_cast<size_t>(right.byteStartOffset);
	const size_t cursor = SkipTrivia(source, begin, end);
	if (cursor + formula.op.size() > end ||
		source.compare(cursor, formula.op.size(), formula.op) != 0)
	{
		return result;
	}
	return sourceMap.FromByteRange(
		source, cursor, cursor + formula.op.size() - 1);
}

std::vector<std::string> UniqueStrings(std::vector<std::string> values)
{
	values.erase(std::remove_if(values.begin(), values.end(),
		[](const std::string &value) { return value.empty(); }), values.end());
	std::sort(values.begin(), values.end());
	values.erase(std::unique(values.begin(), values.end()), values.end());
	return values;
}

std::string SelectCandidate(
	std::vector<std::string> values,
	const std::string &excluded,
	uint64_t seed,
	const std::string &key)
{
	values = UniqueStrings(std::move(values));
	values.erase(std::remove(values.begin(), values.end(), excluded), values.end());
	if (values.empty())
		return {};
	const std::string selector = std::to_string(seed) + "|" + key;
	return values[static_cast<size_t>(Fnv1a64(selector) % values.size())];
}

bool SameHandlerSignature(
	const HandlerView &left,
	const HandlerView &right)
{
	return left.scope == right.scope &&
		left.parameterTypes == right.parameterTypes;
}

class Generator
{
public:
	Generator(
		const std::string &source,
		const ManifestView &manifest,
		const MutationOptions &options,
		MutationGenerationResult &result)
		: m_source(source),
		  m_sourceMap(source),
		  m_manifest(manifest),
		  m_options(options),
		  m_result(result)
	{
	}

	void Run()
	{
		for (MutationKind kind : EnabledKinds())
		{
			const size_t before = m_kindCounts[kind];
			switch (kind)
			{
			case MutationKind::RelayTargetReplace: TargetReplace(); break;
			case MutationKind::RelayTargetSwap: TargetSwap(); break;
			case MutationKind::RelayArgumentReplace: ArgumentReplace(); break;
			case MutationKind::RelayDelete: Delete(); break;
			case MutationKind::RelayDuplicate: Duplicate(); break;
			case MutationKind::RelayOrderSwap: OrderSwap(); break;
			case MutationKind::GuardNegate: NegateGuards(); break;
			case MutationKind::HandlerReplace: ReplaceHandlers(); break;
			case MutationKind::RelayKindChange: ChangeRelayKinds(); break;
			case MutationKind::BroadcastToSingle: BroadcastToSingle(); break;
			case MutationKind::SingleToBroadcast: SingleToBroadcast(); break;
			case MutationKind::IntroduceAlias: IntroduceAlias(); break;
			case MutationKind::IntroduceRelayRecursion:
				IntroduceRelayRecursion();
				break;
			case MutationKind::TargetArithmeticPerturb:
				TargetArithmeticPerturb();
				break;
			case MutationKind::TargetVariableSwap:
				TargetVariableSwap();
				break;
			case MutationKind::ArgumentArithmeticPerturb:
				ArgumentArithmeticPerturb();
				break;
			case MutationKind::GuardBoundaryChange:
				GuardBoundaryChange();
				break;
			}
			if (m_kindCounts[kind] == before && m_options.includeUnsupported)
				AddUnsupported(kind, "no type-safe source candidate was available");
		}
	}

private:
	const std::string &m_source;
	Utf8SourceMap m_sourceMap;
	const ManifestView &m_manifest;
	const MutationOptions &m_options;
	MutationGenerationResult &m_result;
	std::map<MutationKind, size_t> m_kindCounts;
	std::map<std::string, std::string> m_idCanonicals;

	struct StableIdentity
	{
		std::string id;
		std::string canonical;
	};

	std::vector<MutationKind> EnabledKinds() const
	{
		if (m_options.enabledKinds.empty())
			return {AllMutationKinds.begin(), AllMutationKinds.end()};
		std::vector<MutationKind> result = m_options.enabledKinds;
		std::sort(result.begin(), result.end(),
			[](MutationKind left, MutationKind right) {
				return static_cast<uint8_t>(left) < static_cast<uint8_t>(right);
			});
		result.erase(std::unique(result.begin(), result.end()), result.end());
		return result;
	}

	bool AtLimit(MutationKind kind) const
	{
		return m_options.maxMutantsPerKind != 0 &&
			m_kindCounts.at(kind) >= m_options.maxMutantsPerKind;
	}

	StableIdentity StableId(
		MutationKind kind,
		const std::vector<std::string> &,
		const std::vector<SourceEdit> &edits,
		const std::string &suffix = std::string()) const
	{
		std::vector<SourceEdit> ordered = edits;
		std::sort(ordered.begin(), ordered.end(),
			[](const SourceEdit &left, const SourceEdit &right) {
				if (left.startOffset != right.startOffset)
					return left.startOffset < right.startOffset;
				if (left.endOffset != right.endOffset)
					return left.endOffset < right.endOffset;
				if (left.replacement != right.replacement)
					return left.replacement < right.replacement;
				return left.expected < right.expected;
			});
		std::ostringstream canonical;
		// Site ordinals, generated lambda names, opcodes, and listener order
		// are intentionally absent. The source digest plus structural edit
		// anchors keep IDs stable across manifest enumeration changes.
		AppendCanonical(canonical, Digest(m_source));
		AppendCanonical(canonical, m_manifest.contract);
		AppendCanonical(canonical, ToString(kind));
		for (const SourceEdit &edit : ordered)
		{
			AppendCanonical(canonical, std::to_string(edit.startOffset));
			AppendCanonical(canonical, std::to_string(edit.endOffset));
			AppendCanonical(canonical, edit.expected);
			AppendCanonical(canonical, edit.replacement);
		}
		AppendCanonical(canonical, suffix);
		StableIdentity result;
		result.canonical = canonical.str();
		result.id = "mut_" + std::string(ToString(kind)) + "_" +
			Hash128(result.canonical);
		return result;
	}

	bool ReserveId(const StableIdentity &identity)
	{
		auto inserted = m_idCanonicals.emplace(identity.id, identity.canonical);
		if (inserted.second)
			return true;
		if (inserted.first->second == identity.canonical)
			return false;
		throw std::runtime_error(
			"stable mutation ID collision for " + identity.id);
	}

	void AddUnsupported(MutationKind kind, const std::string &reason)
	{
		MutationRecord record;
		record.mutationType = kind;
		record.status = MutationGenerationStatus::Unsupported;
		record.contract = m_manifest.contract;
		record.description = std::string(ToString(kind)) +
			" could not be instantiated";
		record.reason = reason;
		record.originalCode = m_source;
		const StableIdentity identity = StableId(kind, {}, {}, reason);
		record.mutationId = identity.id;
		if (ReserveId(identity))
			m_result.mutations.push_back(std::move(record));
	}

	void Add(
		MutationKind kind,
		const std::vector<const SiteView *> &sites,
		std::vector<SourceEdit> edits,
		const std::string &description,
		const SourceLocation &location,
		const std::string &functionId = std::string())
	{
		if (m_options.maxMutantsPerKind != 0 &&
			m_kindCounts[kind] >= m_options.maxMutantsPerKind)
		{
			return;
		}
		std::vector<std::string> siteIds;
		for (const SiteView *site : sites)
		{
			if (site != nullptr)
				siteIds.push_back(site->id);
		}
		std::sort(siteIds.begin(), siteIds.end());
		const StableIdentity identity = StableId(kind, siteIds, edits);
		const std::string mutationId = identity.id;
		if (!ReserveId(identity))
			return;

		std::string mutated;
		std::string error;
		if (!MutationEngine::ApplyEdits(m_source, edits, mutated, error))
		{
			m_result.diagnostics.push_back(
				mutationId + ": edit rejected: " + error);
			return;
		}
		if (mutated == m_source)
		{
			m_result.diagnostics.push_back(
				mutationId + ": edit did not change the source");
			return;
		}

		MutationRecord record;
		record.mutationId = mutationId;
		record.mutationType = kind;
		record.status = MutationGenerationStatus::Generated;
		record.contract = m_manifest.contract;
		record.sourceFunctionId = !functionId.empty()
			? functionId
			: sites.empty() || sites.front() == nullptr
				? std::string()
				: sites.front()->sourceFunctionId;
		record.relaySiteIds = std::move(siteIds);
		record.location = location;
		record.description = description;
		record.originalCode = m_source;
		record.mutatedCode = std::move(mutated);
		record.edits = std::move(edits);
		for (const SourceEdit &edit : record.edits)
		{
			record.originalFragment += edit.expected;
			record.mutatedFragment += edit.replacement;
		}
		m_result.mutations.push_back(std::move(record));
		++m_kindCounts[kind];
	}

	std::vector<std::string> ParametersOfType(
		const std::string &functionId,
		const std::string &type) const
	{
		std::vector<std::string> result;
		auto found = m_manifest.functions.find(functionId);
		if (found == m_manifest.functions.end())
			return result;
		for (const ParameterView &parameter : found->second.parameters)
		{
			if (parameter.type == type)
				result.push_back(parameter.name);
		}
		return result;
	}

	bool IsSourceParameterExpression(
		const SiteView &site,
		const ExpressionView &expression) const
	{
		const std::vector<std::string> parameters =
			ParametersOfType(site.sourceFunctionId, expression.type);
		return std::find(parameters.begin(), parameters.end(), expression.text) !=
			parameters.end();
	}

	std::string SiteAnchor(const SiteView &site) const
	{
		return m_manifest.contract + "@" +
			std::to_string(site.location.startOffset) + ":" +
			std::to_string(site.location.endOffset);
	}

	bool HasProvedPairRelation(
		const SiteView &left,
		const SiteView &right,
		std::initializer_list<const char *> allowedRelations) const
	{
		for (const PairCertificateView &certificate :
			m_manifest.pairCertificates)
		{
			const bool matchingFunction =
				left.sourceFunctionId == right.sourceFunctionId &&
				!left.sourceFunctionId.empty() &&
				certificate.certificateFunctionId == left.sourceFunctionId &&
				certificate.sourceFunctionId == left.sourceFunctionId;
			const bool matchingSites =
				(certificate.siteA == left.id &&
				 certificate.siteB == right.id) ||
				(certificate.siteA == right.id &&
				 certificate.siteB == left.id);
			if (!matchingFunction || !matchingSites ||
				certificate.status != "Proved")
				continue;
			if (std::any_of(allowedRelations.begin(), allowedRelations.end(),
				[&certificate](const char *relation) {
					return certificate.relation == relation;
				}))
			{
				return true;
			}
		}
		return false;
	}

	bool HasProvedLeftBeforeRight(
		const SiteView &left,
		const SiteView &right) const
	{
		if (left.sourceFunctionId != right.sourceFunctionId ||
			left.sourceFunctionId.empty())
		{
			return false;
		}
		for (const PairCertificateView &certificate :
			m_manifest.pairCertificates)
		{
			if (certificate.status != "Proved" ||
				certificate.certificateFunctionId != left.sourceFunctionId ||
				certificate.sourceFunctionId != left.sourceFunctionId)
			{
				continue;
			}
			const bool aIsLeft = certificate.siteA == left.id &&
				certificate.siteB == right.id;
			const bool bIsLeft = certificate.siteB == left.id &&
				certificate.siteA == right.id;
			if ((aIsLeft && certificate.relation == "MustPrecedeAB") ||
				(bIsLeft && certificate.relation == "MustPrecedeBA"))
			{
				return true;
			}
		}
		return false;
	}

	std::vector<std::string> ReplacementExpressions(
		const SiteView &site,
		const std::string &type) const
	{
		std::vector<std::string> candidates =
			ParametersOfType(site.sourceFunctionId, type);
		candidates.push_back(DefaultExpression(type, site.sourceScope));
		return UniqueStrings(std::move(candidates));
	}

	void TargetReplace()
	{
		for (const SiteView &site : m_manifest.sites)
		{
			if (site.relayKind != "custom_scope" ||
				!site.target.location.IsValidFor(m_source))
			{
				continue;
			}
			const std::string replacement = SelectCandidate(
				ReplacementExpressions(site, site.target.type),
				site.target.text,
				m_options.seed,
				std::string(ToString(MutationKind::RelayTargetReplace)) +
					SiteAnchor(site));
			if (replacement.empty())
				continue;
			SourceEdit edit{
				site.target.location.byteStartOffset,
				site.target.location.byteEndOffset,
				replacement,
				Fragment(m_source, site.target.location)};
			Add(MutationKind::RelayTargetReplace, {&site}, {edit},
				"replace relay target '" + site.target.text + "' with '" +
					replacement + "'",
				site.target.location);
		}
	}

	void TargetSwap()
	{
		for (size_t i = 0; i < m_manifest.sites.size(); ++i)
		{
			for (size_t j = i + 1; j < m_manifest.sites.size(); ++j)
			{
				const SiteView &left = m_manifest.sites[i];
				const SiteView &right = m_manifest.sites[j];
				if (left.sourceFunctionId != right.sourceFunctionId ||
					left.relayKind != "custom_scope" ||
					right.relayKind != "custom_scope" ||
					left.target.type != right.target.type ||
					left.target.text == right.target.text ||
					!IsSourceParameterExpression(left, left.target) ||
					!IsSourceParameterExpression(right, right.target) ||
					!left.target.location.IsValidFor(m_source) ||
					!right.target.location.IsValidFor(m_source) ||
					EnclosingBraceAt(m_source, left.location.byteStartOffset) !=
						EnclosingBraceAt(m_source, right.location.byteStartOffset))
				{
					continue;
				}
				SourceEdit leftEdit{left.target.location.byteStartOffset,
					left.target.location.byteEndOffset, right.target.text,
					Fragment(m_source, left.target.location)};
				SourceEdit rightEdit{right.target.location.byteStartOffset,
					right.target.location.byteEndOffset, left.target.text,
					Fragment(m_source, right.target.location)};
				Add(MutationKind::RelayTargetSwap, {&left, &right},
					{leftEdit, rightEdit},
					"swap the targets of " + left.id + " and " + right.id,
					left.location);
			}
		}
	}

	void ArgumentReplace()
	{
		for (const SiteView &site : m_manifest.sites)
		{
			for (size_t index = 0; index < site.arguments.size(); ++index)
			{
				const ArgumentView &argument = site.arguments[index];
				if (!site.location.IsValidFor(m_source) ||
					!argument.expression.location.IsValidFor(m_source))
					continue;
				const bool caretCapture =
					PreviousSignificantCodeCharacter(
						m_source,
						static_cast<size_t>(site.location.byteStartOffset),
						static_cast<size_t>(
							argument.expression.location.byteStartOffset)) == '^';
				std::vector<std::string> candidates = caretCapture
					? ParametersOfType(site.sourceFunctionId, argument.type)
					: ReplacementExpressions(site, argument.type);
				if (caretCapture)
				{
					candidates.erase(std::remove_if(
						candidates.begin(), candidates.end(),
						[](const std::string &candidate) {
							return !IsPredaIdentifier(candidate);
						}), candidates.end());
				}
				const std::string replacement = SelectCandidate(
					std::move(candidates),
					argument.expression.text,
					m_options.seed,
					std::string(ToString(MutationKind::RelayArgumentReplace)) +
						SiteAnchor(site) + std::to_string(index));
				if (replacement.empty())
					continue;
				SourceEdit edit{argument.expression.location.byteStartOffset,
					argument.expression.location.byteEndOffset, replacement,
					Fragment(m_source, argument.expression.location)};
				Add(MutationKind::RelayArgumentReplace, {&site}, {edit},
					"replace relay argument " + std::to_string(index) +
						" with '" + replacement + "'",
					argument.expression.location);
			}
		}
	}

	void Delete()
	{
		for (const SiteView &site : m_manifest.sites)
		{
			if (!site.location.IsValidFor(m_source))
				continue;
			const std::string original = Fragment(m_source, site.location);
			SourceEdit edit{site.location.byteStartOffset,
				site.location.byteEndOffset,
				BlankPreservingNewlines(original), original};
			Add(MutationKind::RelayDelete, {&site}, {edit},
				"delete relay site " + site.id, site.location);
		}
	}

	void Duplicate()
	{
		for (const SiteView &site : m_manifest.sites)
		{
			if (!site.location.IsValidFor(m_source))
				continue;
			const std::string statement = Fragment(m_source, site.location);
			const std::string insertion = "\n" +
				IndentationAt(m_source, site.location.byteStartOffset) + statement;
			SourceEdit edit{site.location.byteEndOffset + 1,
				site.location.byteEndOffset,
				insertion, std::string()};
			Add(MutationKind::RelayDuplicate, {&site}, {edit},
				"duplicate relay site " + site.id, site.location);
		}
	}

	void OrderSwap()
	{
		for (size_t i = 0; i < m_manifest.sites.size(); ++i)
		{
			for (size_t j = i + 1; j < m_manifest.sites.size(); ++j)
			{
				const SiteView &left = m_manifest.sites[i];
				const SiteView &right = m_manifest.sites[j];
				if (left.sourceFunctionId != right.sourceFunctionId ||
					!left.location.IsValidFor(m_source) ||
					!right.location.IsValidFor(m_source) ||
					!NonOverlapping(left.location, right.location) ||
					!ContainsOnlyTrivia(m_source,
						static_cast<size_t>(left.location.byteEndOffset + 1),
						static_cast<size_t>(right.location.byteStartOffset)) ||
					!HasProvedLeftBeforeRight(left, right) ||
					EnclosingBraceAt(m_source, left.location.byteStartOffset) !=
						EnclosingBraceAt(m_source, right.location.byteStartOffset))
				{
					continue;
				}
				const std::string leftText = Fragment(m_source, left.location);
				const std::string rightText = Fragment(m_source, right.location);
				SourceEdit leftEdit{left.location.byteStartOffset,
					left.location.byteEndOffset, rightText, leftText};
				SourceEdit rightEdit{right.location.byteStartOffset,
					right.location.byteEndOffset, leftText, rightText};
				Add(MutationKind::RelayOrderSwap, {&left, &right},
					{leftEdit, rightEdit},
					"swap the source order of " + left.id + " and " + right.id,
					left.location);
			}
		}
	}

	void NegateGuards()
	{
		std::set<std::pair<int64_t, int64_t>> emitted;
		for (const SiteView &site : m_manifest.sites)
		{
			for (const BranchView &branch : site.branches)
			{
				const SourceLocation &location = branch.condition.location;
				if (!location.IsValidFor(m_source) ||
					!emitted.emplace(
						location.byteStartOffset,
						location.byteEndOffset).second)
				{
					continue;
				}
				const std::string original = Fragment(m_source, location);
				SourceEdit edit{location.byteStartOffset,
					location.byteEndOffset,
					"!(" + original + ")", original};
				Add(MutationKind::GuardNegate, {&site}, {edit},
					"negate enclosing branch guard '" + original + "'",
					location);
			}
		}
	}

	void TargetArithmeticPerturb()
	{
		for (const SiteView &site : m_manifest.sites)
		{
			const FormulaView &formula = site.refinementTarget;
			if (site.relayKind != "custom_scope" || !formula.IsKnown() ||
				!formula.location.IsValidFor(m_source))
			{
				continue;
			}
			const std::string unit = UnitLiteral(formula, site.targetScope);
			if (unit.empty())
				continue;
			const std::string original = Fragment(m_source, formula.location);
			if (original.empty())
				continue;
			const std::string replacement =
				"(" + original + " + " + unit + ")";
			SourceEdit edit{formula.location.byteStartOffset,
				formula.location.byteEndOffset, replacement, original};
			Add(MutationKind::TargetArithmeticPerturb, {&site}, {edit},
				"perturb FormulaIR relay target by one while preserving "
				"its fixed-width sort",
				formula.location);
		}
	}

	void TargetVariableSwap()
	{
		for (const SiteView &site : m_manifest.sites)
		{
			const FormulaView &formula = site.refinementTarget;
			if (site.relayKind != "custom_scope" || !formula.IsKnown())
				continue;

			std::vector<const FormulaView *> leaves;
			CollectSymbolFormulaLeaves(formula, leaves);
			for (const FormulaView *leaf : leaves)
			{
				if (leaf == nullptr || !leaf->location.IsValidFor(m_source))
					continue;
				auto current =
					m_manifest.refinementSymbols.find(leaf->symbolId);
				if (current == m_manifest.refinementSymbols.end() ||
					current->second.kind != "SourceFunctionParameter" ||
					current->second.sourceFunctionId != site.sourceFunctionId ||
					!SameFormulaSort(*leaf, current->second))
				{
					continue;
				}

				std::vector<std::string> alternatives;
				for (const FormulaView *candidateLeaf : leaves)
				{
					if (candidateLeaf == nullptr)
						continue;
					auto candidate = m_manifest.refinementSymbols.find(
						candidateLeaf->symbolId);
					if (candidate == m_manifest.refinementSymbols.end() ||
						candidate->second.kind != "SourceFunctionParameter" ||
						candidate->second.sourceFunctionId != site.sourceFunctionId ||
						candidate->second.sourceType != current->second.sourceType ||
						!SameFormulaSort(*leaf, candidate->second))
					{
						continue;
					}
					alternatives.push_back(candidate->second.sourceName);
				}
				const std::string replacement = SelectCandidate(
					std::move(alternatives),
					current->second.sourceName,
					m_options.seed,
					std::string(ToString(MutationKind::TargetVariableSwap)) +
						SiteAnchor(site) + std::to_string(
							leaf->location.startOffset));
				if (replacement.empty())
					continue;
				const std::string original = Fragment(m_source, leaf->location);
				if (original != current->second.sourceName)
					continue;
				SourceEdit edit{leaf->location.byteStartOffset,
					leaf->location.byteEndOffset, replacement, original};
				Add(MutationKind::TargetVariableSwap, {&site}, {edit},
					"replace target FormulaIR parameter '" + original +
						"' with same-typed parameter '" + replacement + "'",
					leaf->location);
			}
		}
	}

	void ArgumentArithmeticPerturb()
	{
		for (const SiteView &site : m_manifest.sites)
		{
			for (const auto &entry : site.refinementArguments)
			{
				const size_t argumentIndex = entry.first;
				const FormulaView &formula = entry.second;
				if (argumentIndex >= site.arguments.size() ||
					!formula.IsKnown() ||
					!formula.location.IsValidFor(m_source))
				{
					continue;
				}
				const ArgumentView &argument = site.arguments[argumentIndex];
				const int64_t previousOffset = site.location.IsValidFor(m_source)
					? PreviousSignificantCodeOffset(
						m_source,
						static_cast<size_t>(site.location.byteStartOffset),
						static_cast<size_t>(formula.location.byteStartOffset))
					: -1;
				const bool caretCapture = previousOffset >= 0 &&
					m_source[static_cast<size_t>(previousOffset)] == '^';
				const std::string unit = UnitLiteral(formula, argument.type);
				if (unit.empty())
					continue;
				const std::string original = Fragment(m_source, formula.location);
				if (original.empty())
					continue;
				std::string expected = original;
				std::string replacement =
					"(" + original + " + " + unit + ")";
				int64_t editStart = formula.location.byteStartOffset;
				if (caretCapture)
				{
					// PREDA's ^name capture admits only an identifier.  Rewriting
					// the capture as an explicitly typed relay-lambda binding keeps
					// the generated handler signature unchanged while allowing an
					// arithmetic perturbation of the captured value.
					if (!IsPredaIdentifier(original))
						continue;
					editStart = previousOffset;
					expected = m_source.substr(
						static_cast<size_t>(editStart),
						static_cast<size_t>(
							formula.location.byteEndOffset - editStart + 1));
					replacement = argument.type + " " + original + " = (" +
						original + " + " + unit + ")";
				}
				SourceEdit edit{editStart, formula.location.byteEndOffset,
					replacement, expected};
				Add(MutationKind::ArgumentArithmeticPerturb, {&site}, {edit},
					"perturb relay argument FormulaIR by one while preserving "
					"its numeric sort",
					formula.location);
			}
		}
	}

	void GuardBoundaryChange()
	{
		std::set<std::pair<int64_t, int64_t>> emitted;
		for (const SiteView &site : m_manifest.sites)
		{
			if (!site.refinementGuard.IsKnown())
				continue;
			std::vector<const FormulaView *> comparisons;
			CollectBoundaryComparisons(site.refinementGuard, comparisons);
			for (const FormulaView *comparison : comparisons)
			{
				if (comparison == nullptr)
					continue;
				const SourceLocation location = FindFormulaOperatorLocation(
					m_source, m_sourceMap, *comparison);
				if (!location.IsValidFor(m_source) ||
					!emitted.emplace(location.byteStartOffset,
						location.byteEndOffset).second)
				{
					continue;
				}
				const std::string replacement =
					ChangedBoundaryOperator(comparison->op);
				if (replacement.empty())
					continue;
				SourceEdit edit{location.byteStartOffset,
					location.byteEndOffset, replacement,
					Fragment(m_source, location)};
				Add(MutationKind::GuardBoundaryChange, {&site}, {edit},
					"change guard boundary operator '" + comparison->op +
						"' to '" + replacement + "'",
					location);
			}
		}
	}

	void ReplaceHandlers()
	{
		for (const SiteView &site : m_manifest.sites)
		{
			auto current = m_manifest.handlers.find(site.handlerId);
			if (current == m_manifest.handlers.end() ||
				current->second.kind != "named")
			{
				continue;
			}
			std::vector<const HandlerView *> alternatives;
			for (const auto &entry : m_manifest.handlers)
			{
				if (entry.first != site.handlerId && entry.second.kind == "named" &&
					SameHandlerSignature(current->second, entry.second))
				{
					alternatives.push_back(&entry.second);
				}
			}
			std::sort(alternatives.begin(), alternatives.end(),
				[](const HandlerView *left, const HandlerView *right) {
					return left->name < right->name;
				});
			if (alternatives.empty())
				continue;
			const size_t selected = static_cast<size_t>(Fnv1a64(
				std::to_string(m_options.seed) + "|handler|" + SiteAnchor(site)) %
				alternatives.size());
			const HandlerView &replacement = *alternatives[selected];
			const SourceLocation location =
				FindNamedHandlerLocation(
					m_source, m_sourceMap, site, current->second);
			if (!location.IsValidFor(m_source))
				continue;
			SourceEdit edit{location.byteStartOffset,
				location.byteEndOffset,
				replacement.name, Fragment(m_source, location)};
			Add(MutationKind::HandlerReplace, {&site}, {edit},
				"replace handler '" + current->second.name + "' with '" +
					replacement.name + "'",
				location);
		}
	}

	void ChangeRelayKinds()
	{
		for (const SiteView &site : m_manifest.sites)
		{
			// Keep this operator distinct from the broadcast operators and
			// generate the one generally type-preserving change: a keyed relay
			// to the current key of the same source scope.
			if (site.relayKind != "custom_scope" ||
				site.sourceScope != site.targetScope ||
				site.sourceScope == "shard")
			{
				continue;
			}
			const SourceLocation location =
				FindRelayTypeLocation(m_source, m_sourceMap, site);
			if (!location.IsValidFor(m_source))
				continue;
			const std::string replacement = "next";
			SourceEdit edit{location.byteStartOffset,
				location.byteEndOffset,
				replacement, Fragment(m_source, location)};
			Add(MutationKind::RelayKindChange, {&site}, {edit},
				"change relay kind from '" + site.relayKind + "' to '" +
					replacement + "'",
				location);
		}
	}

	const HandlerView *FindCompatibleNamedHandler(
		const HandlerView &current,
		const std::string &scope) const
	{
		std::vector<const HandlerView *> candidates;
		for (const auto &entry : m_manifest.handlers)
		{
			const HandlerView &handler = entry.second;
			if (handler.kind == "named" && handler.scope == scope &&
				handler.parameterTypes == current.parameterTypes)
			{
				candidates.push_back(&handler);
			}
		}
		if (candidates.empty())
			return nullptr;
		std::sort(candidates.begin(), candidates.end(),
			[](const HandlerView *left, const HandlerView *right) {
				return left->name < right->name;
			});
		return candidates.front();
	}

	void BroadcastToSingle()
	{
		for (const SiteView &site : m_manifest.sites)
		{
			if (site.relayKind != "shards" || site.sourceScope != "global")
				continue;
			const SourceLocation typeLocation =
				FindRelayTypeLocation(m_source, m_sourceMap, site);
			if (!typeLocation.IsValidFor(m_source))
				continue;
			std::vector<SourceEdit> edits{{typeLocation.byteStartOffset,
				typeLocation.byteEndOffset, "next",
				Fragment(m_source, typeLocation)}};
			auto current = m_manifest.handlers.find(site.handlerId);
			if (current == m_manifest.handlers.end())
				continue;
			if (current->second.kind == "named")
			{
				const HandlerView *replacement =
					FindCompatibleNamedHandler(current->second, site.sourceScope);
				const SourceLocation handlerLocation =
					FindNamedHandlerLocation(
						m_source, m_sourceMap, site, current->second);
				if (replacement == nullptr || !handlerLocation.IsValidFor(m_source))
					continue;
				edits.push_back(SourceEdit{handlerLocation.byteStartOffset,
					handlerLocation.byteEndOffset, replacement->name,
					Fragment(m_source, handlerLocation)});
			}
			else if (current->second.kind != "lambda")
				continue;
			Add(MutationKind::BroadcastToSingle, {&site}, std::move(edits),
				"replace all-shards broadcast with one next-scope relay",
				typeLocation);
		}
	}

	void SingleToBroadcast()
	{
		for (const SiteView &site : m_manifest.sites)
		{
			if (site.relayKind == "shards" || site.sourceScope != "global")
				continue;
			const SourceLocation typeLocation =
				FindRelayTypeLocation(m_source, m_sourceMap, site);
			if (!typeLocation.IsValidFor(m_source))
				continue;
			std::vector<SourceEdit> edits{{typeLocation.byteStartOffset,
				typeLocation.byteEndOffset, "shards",
				Fragment(m_source, typeLocation)}};
			auto current = m_manifest.handlers.find(site.handlerId);
			if (current == m_manifest.handlers.end())
				continue;
			if (current->second.kind == "named")
			{
				const HandlerView *replacement =
					FindCompatibleNamedHandler(current->second, "shard");
				const SourceLocation handlerLocation =
					FindNamedHandlerLocation(
						m_source, m_sourceMap, site, current->second);
				if (replacement == nullptr || !handlerLocation.IsValidFor(m_source))
					continue;
				edits.push_back(SourceEdit{handlerLocation.byteStartOffset,
					handlerLocation.byteEndOffset, replacement->name,
					Fragment(m_source, handlerLocation)});
			}
			else if (current->second.kind != "lambda")
				continue;
			Add(MutationKind::SingleToBroadcast, {&site}, std::move(edits),
				"replace a single-target relay with relay@shards",
				typeLocation);
		}
	}

	void IntroduceAlias()
	{
		for (size_t i = 0; i < m_manifest.sites.size(); ++i)
		{
			for (size_t j = i + 1; j < m_manifest.sites.size(); ++j)
			{
				const SiteView &left = m_manifest.sites[i];
				const SiteView &right = m_manifest.sites[j];
				if (left.sourceFunctionId != right.sourceFunctionId ||
					left.relayKind != "custom_scope" ||
					right.relayKind != "custom_scope" ||
					left.target.type != right.target.type ||
					left.target.text == right.target.text ||
					!IsSourceParameterExpression(left, left.target) ||
					!right.target.location.IsValidFor(m_source) ||
					!HasProvedPairRelation(left, right,
						{"CoEmissionIndependent"}) ||
					EnclosingBraceAt(m_source, left.location.byteStartOffset) !=
						EnclosingBraceAt(m_source, right.location.byteStartOffset))
				{
					continue;
				}
				SourceEdit edit{right.target.location.byteStartOffset,
					right.target.location.byteEndOffset, left.target.text,
					Fragment(m_source, right.target.location)};
				Add(MutationKind::IntroduceAlias, {&left, &right}, {edit},
					"force " + right.id + " to alias target of " + left.id,
					right.target.location);
			}
		}
	}

	void IntroduceRelayRecursion()
	{
		std::set<std::string> emittedHandlers;
		for (const SiteView &site : m_manifest.sites)
		{
			auto handlerFound = m_manifest.handlers.find(site.handlerId);
			if (handlerFound == m_manifest.handlers.end())
				continue;
			const HandlerView &handler = handlerFound->second;
			if (handler.kind != "named" || handler.name.empty() ||
				handler.targetFunctionId.empty() ||
				!emittedHandlers.insert(handler.id).second)
			{
				continue;
			}
			auto functionFound =
				m_manifest.functions.find(handler.targetFunctionId);
			if (functionFound == m_manifest.functions.end() ||
				!functionFound->second.location.IsValidFor(m_source))
			{
				continue;
			}
			const FunctionView &function = functionFound->second;
			if (handler.parameterTypes.size() != function.parameters.size())
				continue;
			std::string target;
			if (handler.scope == "global")
				target = "next";
			else if (handler.scope == "shard")
				continue;
			else
			{
				for (const ParameterView &parameter : function.parameters)
				{
					if (parameter.type == handler.scope)
					{
						target = parameter.name;
						break;
					}
				}
				if (target.empty() && handler.scope == "address")
					target = "__transaction.get_self_address()";
			}
			if (target.empty())
				continue;
			std::ostringstream arguments;
			for (size_t index = 0; index < function.parameters.size(); ++index)
			{
				if (function.parameters[index].type != handler.parameterTypes[index])
				{
					arguments.str(std::string());
					break;
				}
				if (index != 0)
					arguments << ", ";
				arguments << function.parameters[index].name;
			}
			if (!function.parameters.empty() && arguments.str().empty())
				continue;
			const std::string relayType = handler.scope == "global"
				? "next"
				: target;
			const size_t functionBegin =
				static_cast<size_t>(function.location.byteStartOffset);
			const size_t functionEnd =
				static_cast<size_t>(function.location.byteEndOffset) + 1;
			const size_t bodyOpen = FindCodeCharacter(
				m_source, functionBegin, functionEnd, '{');
			if (bodyOpen == std::string::npos)
				continue;
			const std::string functionIndent =
				IndentationAt(m_source, function.location.byteStartOffset);
			const std::string indent = functionIndent + "    ";
			const std::string insertion = "\n" + indent + "relay@" +
				relayType + " " + handler.name + "(" + arguments.str() + ");";
			SourceEdit edit{static_cast<int64_t>(bodyOpen + 1),
				static_cast<int64_t>(bodyOpen), insertion, std::string()};
			Add(MutationKind::IntroduceRelayRecursion, {&site}, {edit},
				"insert a self-relay into handler '" + handler.name + "'",
				m_sourceMap.FromByteRange(m_source, bodyOpen, bodyOpen),
				handler.targetFunctionId);
		}
	}
};

} // namespace

const char *ToString(MutationGenerationStatus status)
{
	return status == MutationGenerationStatus::Generated
		? "Generated"
		: "Unsupported";
}

bool MutationEngine::ApplyEdits(
	const std::string &sourceCode,
	const std::vector<SourceEdit> &edits,
	std::string &mutatedCode,
	std::string &error)
{
	mutatedCode = sourceCode;
	error.clear();
	if (edits.empty())
	{
		error = "mutation contains no edits";
		return false;
	}
	std::vector<SourceEdit> ordered = edits;
	std::sort(ordered.begin(), ordered.end(),
		[](const SourceEdit &left, const SourceEdit &right) {
			if (left.startOffset != right.startOffset)
				return left.startOffset < right.startOffset;
			return left.endOffset < right.endOffset;
		});
	int64_t previousEnd = -1;
	for (const SourceEdit &edit : ordered)
	{
		const bool insertion = edit.endOffset < edit.startOffset;
		if (edit.startOffset < 0 ||
			static_cast<uint64_t>(edit.startOffset) > sourceCode.size())
		{
			error = "edit start offset is outside source";
			return false;
		}
		if (!insertion &&
			(edit.endOffset < edit.startOffset ||
			 static_cast<uint64_t>(edit.endOffset) >= sourceCode.size()))
		{
			error = "edit end offset is outside source";
			return false;
		}
		if (edit.startOffset <= previousEnd)
		{
			error = "mutation edits overlap";
			return false;
		}
		previousEnd = insertion ? edit.startOffset - 1 : edit.endOffset;
		if (!edit.expected.empty())
		{
			const std::string actual = sourceCode.substr(
				static_cast<size_t>(edit.startOffset),
				static_cast<size_t>(edit.endOffset - edit.startOffset + 1));
			if (actual != edit.expected)
			{
				error = "source text does not match manifest-backed expected text";
				return false;
			}
		}
	}
	for (auto iterator = ordered.rbegin(); iterator != ordered.rend(); ++iterator)
	{
		const SourceEdit &edit = *iterator;
		if (edit.endOffset < edit.startOffset)
		{
			mutatedCode.insert(
				static_cast<size_t>(edit.startOffset), edit.replacement);
		}
		else
		{
			mutatedCode.replace(
				static_cast<size_t>(edit.startOffset),
				static_cast<size_t>(edit.endOffset - edit.startOffset + 1),
				edit.replacement);
		}
	}
	return true;
}

MutationGenerationResult MutationEngine::Generate(
	const std::string &sourceCode,
	const std::string &relayManifestJson,
	const MutationOptions &options) const
{
	MutationGenerationResult result;
	result.seed = options.seed;
	result.sourceDigest = Digest(sourceCode);
	result.manifestDigest = Digest(relayManifestJson);
	try
	{
		const Json manifestJson = Json::parse(relayManifestJson);
		const ManifestView manifest = ParseManifest(manifestJson, sourceCode);
		result.contract = manifest.contract;
		Generator(sourceCode, manifest, options, result).Run();
	}
	catch (const std::exception &exception)
	{
		result.diagnostics.push_back(
			std::string("mutation generation failed: ") + exception.what());
	}
	return result;
}

} // namespace mutation
} // namespace rpreda
