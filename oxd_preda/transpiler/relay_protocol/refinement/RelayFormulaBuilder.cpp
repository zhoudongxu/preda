#include "RelayFormulaBuilder.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace transpiler {
namespace relay_protocol {
namespace refinement {
namespace {

std::string Trim(std::string value)
{
	while (!value.empty() &&
		std::isspace(static_cast<unsigned char>(value.front())))
	{
		value.erase(value.begin());
	}
	while (!value.empty() &&
		std::isspace(static_cast<unsigned char>(value.back())))
	{
		value.pop_back();
	}
	return value;
}

bool StartsWith(const std::string &value, const char *prefix)
{
	const std::string prefixString(prefix);
	return value.size() >= prefixString.size() &&
		value.compare(0, prefixString.size(), prefixString) == 0;
}

bool EndsWith(const std::string &value, const char *suffix)
{
	const std::string suffixString(suffix);
	return value.size() >= suffixString.size() &&
		value.compare(
			value.size() - suffixString.size(),
			suffixString.size(),
			suffixString) == 0;
}

bool ParseWidth(
	const std::string &digits,
	uint16_t &width)
{
	if (digits.empty())
		return false;
	for (char character : digits)
	{
		if (!std::isdigit(static_cast<unsigned char>(character)))
			return false;
	}
	const unsigned long parsed = std::strtoul(digits.c_str(), nullptr, 10);
	if (parsed > 0xffffUL)
		return false;
	width = static_cast<uint16_t>(parsed);
	return true;
}

FormulaSort IntegerSortFromLiteral(const std::string &text)
{
	if (EndsWith(text, "ib"))
		return FormulaSort::Int();

	const size_t unsignedPosition = text.rfind('u');
	const size_t signedPosition = text.rfind('i');
	size_t suffixPosition = std::string::npos;
	bool isUnsigned = false;
	if (unsignedPosition != std::string::npos &&
		(signedPosition == std::string::npos ||
			unsignedPosition > signedPosition))
	{
		suffixPosition = unsignedPosition;
		isUnsigned = true;
	}
	else if (signedPosition != std::string::npos)
	{
		suffixPosition = signedPosition;
	}

	if (suffixPosition == std::string::npos)
		return FormulaSort::Int();
	if (!isUnsigned)
		return FormulaSort::Int();

	uint16_t width = 32;
	if (suffixPosition + 1 < text.size() &&
		!ParseWidth(text.substr(suffixPosition + 1), width))
	{
		return FormulaSort::Unknown();
	}
	return FormulaSort::UnsignedBitVector(width);
}

bool IsIntegerLiteralText(const std::string &text)
{
	if (text.empty())
		return false;
	size_t index = 0;
	if (text[index] == '-')
	{
		++index;
		if (index == text.size())
			return false;
	}
	if (index + 2 <= text.size() &&
		text[index] == '0' &&
		index + 1 < text.size() &&
		(text[index + 1] == 'x' || text[index + 1] == 'X'))
	{
		index += 2;
		if (index == text.size())
			return false;
		bool sawDigit = false;
		for (; index < text.size(); ++index)
		{
			const char character = text[index];
			if (std::isxdigit(static_cast<unsigned char>(character)))
			{
				sawDigit = true;
				continue;
			}
			return sawDigit && (character == 'u' || character == 'i');
		}
		return sawDigit;
	}
	return std::isdigit(static_cast<unsigned char>(text[index])) != 0;
}

bool IsComparisonOperator(const std::string &op)
{
	return op == "<" || op == ">" || op == "<=" || op == ">=" ||
		op == "==" || op == "!=";
}

bool IsBooleanOperator(const std::string &op)
{
	return op == "&&" || op == "||";
}

bool IsArithmeticOperator(const std::string &op)
{
	return op == "+" || op == "-" || op == "*" || op == "/" ||
		op == "%" || op == "<<" || op == ">>" || op == "&" ||
		op == "^" || op == "|";
}

bool IsNumericSort(const FormulaSort &sort)
{
	return sort.kind == FormulaSortKind::Int ||
		sort.kind == FormulaSortKind::UnsignedBitVector;
}

bool CompatibleComparisonSorts(
	const FormulaSort &left,
	const FormulaSort &right)
{
	if (!left.IsKnown() || !right.IsKnown())
		return false;
	if (left == right)
		return true;
	return false;
}

std::string CalleeName(const RelayExprIR &expression)
{
	if (expression.children.empty())
		return std::string();
	const RelayExprIR &callee = expression.children.front();
	if (callee.kind == RelayExprKind::Keyword ||
		callee.kind == RelayExprKind::Identifier)
	{
		return callee.text;
	}
	return std::string();
}

bool IsArrayLengthCall(const RelayExprIR &expression)
{
	if (expression.kind != RelayExprKind::Call ||
		expression.children.size() != 1)
	{
		return false;
	}
	const RelayExprIR &callee = expression.children.front();
	return callee.kind == RelayExprKind::MemberAccess &&
		callee.children.size() >= 2 &&
		callee.children.back().kind == RelayExprKind::Identifier &&
		callee.children.back().text == "length";
}

FormulaExpr UnknownFromChild(
	const RelayExprIR &expression,
	const FormulaExpr &child,
	const std::string &context)
{
	return FormulaExpr::Unknown(
		expression.text,
		expression.location,
		context + ": " +
			(child.unknownReason.empty()
				? std::string("child formula is unknown")
				: child.unknownReason));
}

} // namespace

FormulaSort RelayFormulaBuilder::SortFromPredaType(
	const std::string &sourceType)
{
	std::string type = Trim(sourceType);
	if (StartsWith(type, "const "))
		type = Trim(type.substr(6));
	if (type == "bool")
		return FormulaSort::Bool();
	if (type == "address")
		return FormulaSort::Address();
	if (type == "bigint")
		return FormulaSort::Int();
	if (StartsWith(type, "int"))
	{
		uint16_t ignoredWidth = 0;
		return ParseWidth(type.substr(3), ignoredWidth)
			? FormulaSort::Int()
			: FormulaSort::Unknown();
	}
	if (StartsWith(type, "uint"))
	{
		uint16_t width = 0;
		return ParseWidth(type.substr(4), width)
			? FormulaSort::UnsignedBitVector(width)
			: FormulaSort::Unknown();
	}
	return FormulaSort::Unknown();
}

bool RelayFormulaBuilder::IsSupportedIntegerCast(
	const std::string &callee)
{
	const FormulaSort sort = SortFromPredaType(callee);
	return sort.kind == FormulaSortKind::Int ||
		sort.kind == FormulaSortKind::UnsignedBitVector;
}

FormulaExpr RelayFormulaBuilder::Build(
	const RelayExprIR &expression) const
{
	return Build(expression, RelayFormulaResolver());
}

FormulaExpr RelayFormulaBuilder::Build(
	const RelayExprIR &expression,
	const RelayFormulaResolver &resolver) const
{
	std::vector<std::string> activeDefinitions;
	return BuildInternal(
		expression,
		resolver,
		activeDefinitions,
		0);
}

FormulaExpr RelayFormulaBuilder::BuildLiteral(
	const RelayExprIR &expression) const
{
	FormulaSort sort = SortFromPredaType(expression.type);
	if (expression.text == "true" || expression.text == "false")
	{
		return FormulaExpr::BoolLiteral(
			expression.text == "true",
			expression.text,
			expression.location);
	}
	if (sort.kind == FormulaSortKind::Address ||
		EndsWith(expression.text, ":ed25519") ||
		EndsWith(expression.text, ":contract"))
	{
		return FormulaExpr::AddressLiteral(
			expression.text,
			expression.location);
	}
	if (!IsIntegerLiteralText(expression.text))
	{
		return FormulaExpr::Unknown(
			expression.text,
			expression.location,
			"literal form is not supported by refinement formula IR");
	}
	if (!sort.IsKnown())
		sort = IntegerSortFromLiteral(expression.text);
	if (sort.kind == FormulaSortKind::Int)
	{
		FormulaExpr result =
			FormulaExpr::IntLiteral(expression.text, expression.location);
		result.text = expression.text;
		return result;
	}
	if (sort.kind == FormulaSortKind::UnsignedBitVector)
	{
		return FormulaExpr::BitVectorLiteral(
			expression.text,
			sort.bitWidth,
			expression.location);
	}
	return FormulaExpr::Unknown(
		expression.text,
		expression.location,
		"integer literal has an unsupported PREDA sort");
}

FormulaExpr RelayFormulaBuilder::BuildCall(
	const RelayExprIR &expression,
	const RelayFormulaResolver &resolver,
	std::vector<std::string> &activeDefinitions,
	size_t depth) const
{
	if (IsArrayLengthCall(expression))
	{
		const RelayExprIR &member = expression.children.front();
		FormulaExpr receiver = BuildInternal(
			member.children.front(),
			resolver,
			activeDefinitions,
			depth + 1);
		if (receiver.IsUnknown())
			return UnknownFromChild(
				expression,
				receiver,
				"array length receiver is unsupported");
		return FormulaExpr::ArrayLength(
			std::move(receiver),
			expression.location,
			expression.text);
	}

	const std::string callee = CalleeName(expression);
	if (!IsSupportedIntegerCast(callee))
	{
		return FormulaExpr::Unknown(
			expression.text,
			expression.location,
			callee.empty()
				? "call target is not a supported integer cast or array length"
				: "call to '" + callee +
					"' has no pure refinement formula summary");
	}
	if (expression.children.size() != 2)
	{
		return FormulaExpr::Unknown(
			expression.text,
			expression.location,
			"integer cast '" + callee +
				"' requires exactly one operand");
	}
	FormulaExpr operand = BuildInternal(
		expression.children[1],
		resolver,
		activeDefinitions,
		depth + 1);
	if (operand.IsUnknown())
		return UnknownFromChild(expression, operand, "cast operand is unsupported");
	if (!IsNumericSort(operand.sort))
	{
		return FormulaExpr::Unknown(
			expression.text,
			expression.location,
			"integer cast operand does not have a supported integer sort");
	}
	const FormulaSort resultSort = SortFromPredaType(callee);
	return FormulaExpr::Cast(
		callee,
		std::move(operand),
		resultSort,
		expression.location,
		expression.text);
}

FormulaExpr RelayFormulaBuilder::BuildInternal(
	const RelayExprIR &expression,
	const RelayFormulaResolver &resolver,
	std::vector<std::string> &activeDefinitions,
	size_t depth) const
{
	if (depth > 256)
	{
		return FormulaExpr::Unknown(
			expression.text,
			expression.location,
			"formula expansion exceeded the maximum nesting depth");
	}

	if (resolver.value)
	{
		const RelayFormulaResolution resolution = resolver.value(expression);
		if (resolution.handled)
		{
			if (resolution.formula != nullptr)
				return *resolution.formula;
			if (resolution.symbol != nullptr)
			{
				return FormulaExpr::Symbol(
					resolution.symbol->id,
					resolution.symbol->sort,
					expression.location,
					expression.text);
			}
			return FormulaExpr::Unknown(
				expression.text,
				expression.location,
				resolution.unavailableReason.empty()
					? "current symbolic value was invalidated"
					: resolution.unavailableReason);
		}
	}
	if (resolver.definition)
	{
		const FormulaExpr *definition = resolver.definition(expression);
		if (definition != nullptr)
			return *definition;
	}
	if (resolver.symbol)
	{
		const RelayRefinementSymbol *symbol = resolver.symbol(expression);
		if (symbol != nullptr)
		{
			return FormulaExpr::Symbol(
				symbol->id,
				symbol->sort,
				expression.location,
				expression.text);
		}
	}

	if (expression.op == "?:" && expression.children.size() == 3)
	{
		FormulaExpr condition = BuildInternal(
			expression.children[0],
			resolver,
			activeDefinitions,
			depth + 1);
		FormulaExpr thenValue = BuildInternal(
			expression.children[1],
			resolver,
			activeDefinitions,
			depth + 1);
		FormulaExpr elseValue = BuildInternal(
			expression.children[2],
			resolver,
			activeDefinitions,
			depth + 1);
		if (condition.IsUnknown() ||
			thenValue.IsUnknown() ||
			elseValue.IsUnknown() ||
			condition.sort != FormulaSort::Bool() ||
			thenValue.sort != elseValue.sort)
		{
			return FormulaExpr::Unknown(
				expression.text,
				expression.location,
				"if-then-else expression has unsupported operands or sorts");
		}
		const FormulaSort resultSort = thenValue.sort;
		return FormulaExpr::Ite(
			std::move(condition),
			std::move(thenValue),
			std::move(elseValue),
			resultSort,
			expression.location,
			expression.text);
	}

	switch (expression.kind)
	{
	case RelayExprKind::Literal:
		return BuildLiteral(expression);
	case RelayExprKind::Identifier:
		return FormulaExpr::Unknown(
			expression.text,
			expression.location,
			"unresolved refinement identifier '" +
				expression.text + "'");
	case RelayExprKind::Keyword:
		return FormulaExpr::Unknown(
			expression.text,
			expression.location,
			"keyword '" + expression.text +
				"' has no registered refinement symbol");
	case RelayExprKind::Group:
	{
		if (expression.children.size() != 1)
		{
			return FormulaExpr::Unknown(
				expression.text,
				expression.location,
				"group expression does not have exactly one child");
		}
		FormulaExpr child = BuildInternal(
			expression.children.front(),
			resolver,
			activeDefinitions,
			depth + 1);
		if (child.IsUnknown())
			return UnknownFromChild(expression, child, "group child is unsupported");
		return FormulaExpr::Group(
			std::move(child),
			expression.location,
			expression.text);
	}
	case RelayExprKind::Unary:
	{
		if (expression.children.size() != 1)
		{
			return FormulaExpr::Unknown(
				expression.text,
				expression.location,
				"unary expression does not have exactly one operand");
		}
		FormulaExpr child = BuildInternal(
			expression.children.front(),
			resolver,
			activeDefinitions,
			depth + 1);
		if (child.IsUnknown())
			return UnknownFromChild(expression, child, "unary operand is unsupported");
		FormulaSort resultSort = SortFromPredaType(expression.type);
		if (!resultSort.IsKnown())
			resultSort = expression.op == "!" ? FormulaSort::Bool() : child.sort;
		const bool valid =
			(expression.op == "!" && child.sort == FormulaSort::Bool()) ||
			((expression.op == "+" || expression.op == "-") &&
				IsNumericSort(child.sort)) ||
			(expression.op == "~" &&
				child.sort.kind == FormulaSortKind::UnsignedBitVector);
		if (!valid)
		{
			return FormulaExpr::Unknown(
				expression.text,
				expression.location,
				"unary operator '" + expression.op +
					"' is unsupported for the operand sort");
		}
		return FormulaExpr::Unary(
			expression.op,
			std::move(child),
			resultSort,
			expression.location,
			expression.text);
	}
	case RelayExprKind::Binary:
	{
		if (expression.children.size() != 2)
		{
			return FormulaExpr::Unknown(
				expression.text,
				expression.location,
				"binary expression does not have exactly two operands");
		}
		FormulaExpr left = BuildInternal(
			expression.children[0],
			resolver,
			activeDefinitions,
			depth + 1);
		FormulaExpr right = BuildInternal(
			expression.children[1],
			resolver,
			activeDefinitions,
			depth + 1);
		if (left.IsUnknown())
			return UnknownFromChild(expression, left, "left operand is unsupported");
		if (right.IsUnknown())
			return UnknownFromChild(expression, right, "right operand is unsupported");

		if (IsComparisonOperator(expression.op))
		{
			if (!CompatibleComparisonSorts(left.sort, right.sort))
			{
				return FormulaExpr::Unknown(
					expression.text,
					expression.location,
					"comparison operands have incompatible formula sorts");
			}
			return FormulaExpr::Binary(
				expression.op,
				std::move(left),
				std::move(right),
				FormulaSort::Bool(),
				expression.location,
				expression.text);
		}
		if (IsBooleanOperator(expression.op))
		{
			if (left.sort != FormulaSort::Bool() ||
				right.sort != FormulaSort::Bool())
			{
				return FormulaExpr::Unknown(
					expression.text,
					expression.location,
					"Boolean operator operands are not Bool");
			}
			return FormulaExpr::Binary(
				expression.op,
				std::move(left),
				std::move(right),
				FormulaSort::Bool(),
				expression.location,
				expression.text);
		}
		if (!IsArithmeticOperator(expression.op))
		{
			return FormulaExpr::Unknown(
				expression.text,
				expression.location,
				"binary operator '" + expression.op +
					"' is not supported by refinement formula IR");
		}
		FormulaSort resultSort = SortFromPredaType(expression.type);
		if (!resultSort.IsKnown())
		{
			if (left.sort == right.sort && IsNumericSort(left.sort))
				resultSort = left.sort;
			else if ((expression.op == "<<" || expression.op == ">>") &&
				IsNumericSort(left.sort) && IsNumericSort(right.sort))
			{
				resultSort = left.sort;
			}
		}
		if (!resultSort.IsKnown() || !IsNumericSort(resultSort))
		{
			return FormulaExpr::Unknown(
				expression.text,
				expression.location,
				"arithmetic operands do not determine a safe PREDA numeric sort");
		}
		if (expression.op != "<<" && expression.op != ">>" &&
			left.sort != right.sort)
		{
			return FormulaExpr::Unknown(
				expression.text,
				expression.location,
				"arithmetic operands have different fixed-width sorts");
		}
		return FormulaExpr::Binary(
			expression.op,
			std::move(left),
			std::move(right),
			resultSort,
			expression.location,
			expression.text);
	}
	case RelayExprKind::Call:
		return BuildCall(
			expression,
			resolver,
			activeDefinitions,
			depth);
	case RelayExprKind::Opaque:
		return FormulaExpr::Unknown(
			expression.text,
			expression.location,
			expression.opaqueReason.empty()
				? "opaque relay expression"
				: expression.opaqueReason);
	case RelayExprKind::MemberAccess:
		return FormulaExpr::Unknown(
			expression.text,
			expression.location,
			"member access is unsupported except for array length()");
	case RelayExprKind::Index:
		return FormulaExpr::Unknown(
			expression.text,
			expression.location,
			"index expressions are not modeled by refinement formula IR");
	default:
		return FormulaExpr::Unknown(
			expression.text,
			expression.location,
			"unsupported relay expression kind");
	}
}

} // namespace refinement
} // namespace relay_protocol
} // namespace transpiler
