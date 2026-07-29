#include "Z3FormulaEncoder.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <utility>

namespace transpiler {
namespace relay_protocol {
namespace refinement {
namespace solver {
namespace z3_backend {
namespace {

constexpr size_t kMaximumFormulaDepth = 512;

enum class IntegerSuffixKind
{
	None,
	Unsigned,
	Signed,
	BigInt,
};

struct ParsedIntegerLiteral
{
	std::string decimalValue;
	IntegerSuffixKind suffixKind = IntegerSuffixKind::None;
	uint16_t suffixWidth = 0;
	bool hasSuffixWidth = false;
};

std::string Trim(const std::string &value)
{
	size_t begin = 0;
	while (begin < value.size() &&
		std::isspace(static_cast<unsigned char>(value[begin])))
	{
		++begin;
	}
	size_t end = value.size();
	while (end > begin &&
		std::isspace(static_cast<unsigned char>(value[end - 1])))
	{
		--end;
	}
	return value.substr(begin, end - begin);
}

bool ParseWidth(
	const std::string &text,
	uint16_t &width)
{
	if (text.empty())
		return false;
	uint32_t value = 0;
	for (char character : text)
	{
		if (!std::isdigit(static_cast<unsigned char>(character)))
			return false;
		value = value * 10u +
			static_cast<uint32_t>(character - '0');
		if (value > 0xffffu)
			return false;
	}
	width = static_cast<uint16_t>(value);
	return true;
}

std::string StripLeadingZeros(const std::string &digits)
{
	const size_t firstNonZero = digits.find_first_not_of('0');
	return firstNonZero == std::string::npos
		? std::string("0")
		: digits.substr(firstNonZero);
}

void DecimalMultiplyAdd(
	std::string &decimal,
	unsigned multiplier,
	unsigned addition)
{
	unsigned carry = addition;
	for (size_t index = decimal.size(); index > 0; --index)
	{
		const unsigned value =
			static_cast<unsigned>(decimal[index - 1] - '0') *
				multiplier +
			carry;
		decimal[index - 1] =
			static_cast<char>('0' + (value % 10u));
		carry = value / 10u;
	}
	while (carry != 0)
	{
		decimal.insert(
			decimal.begin(),
			static_cast<char>('0' + (carry % 10u)));
		carry /= 10u;
	}
}

bool HexToDecimal(
	const std::string &digits,
	std::string &decimal,
	std::string &reason)
{
	if (digits.empty())
	{
		reason = "hexadecimal literal has no digits";
		return false;
	}
	decimal = "0";
	for (char character : digits)
	{
		unsigned value = 0;
		if (character >= '0' && character <= '9')
			value = static_cast<unsigned>(character - '0');
		else if (character >= 'a' && character <= 'f')
			value = 10u + static_cast<unsigned>(character - 'a');
		else if (character >= 'A' && character <= 'F')
			value = 10u + static_cast<unsigned>(character - 'A');
		else
		{
			reason =
				"invalid hexadecimal digit in integer literal";
			return false;
		}
		DecimalMultiplyAdd(decimal, 16u, value);
	}
	decimal = StripLeadingZeros(decimal);
	return true;
}

bool ParseIntegerLiteral(
	const std::string &source,
	ParsedIntegerLiteral &parsed,
	std::string &reason)
{
	const std::string text = Trim(source);
	if (text.empty())
	{
		reason = "integer literal is empty";
		return false;
	}

	size_t index = 0;
	bool negative = false;
	if (text[index] == '+' || text[index] == '-')
	{
		negative = text[index] == '-';
		++index;
		if (index == text.size())
		{
			reason = "integer literal contains only a sign";
			return false;
		}
	}

	const bool hexadecimal =
		index + 2 <= text.size() &&
		text[index] == '0' &&
		index + 1 < text.size() &&
		(text[index + 1] == 'x' || text[index + 1] == 'X');
	if (hexadecimal)
		index += 2;

	const size_t digitBegin = index;
	while (index < text.size())
	{
		const unsigned char character =
			static_cast<unsigned char>(text[index]);
		const bool isDigit = hexadecimal
			? std::isxdigit(character) != 0
			: std::isdigit(character) != 0;
		if (!isDigit)
			break;
		++index;
	}
	if (index == digitBegin)
	{
		reason = "integer literal has no digits";
		return false;
	}

	const std::string digits =
		text.substr(digitBegin, index - digitBegin);
	const std::string suffix = text.substr(index);
	if (suffix.empty())
	{
		parsed.suffixKind = IntegerSuffixKind::None;
	}
	else if (suffix == "ib")
	{
		parsed.suffixKind = IntegerSuffixKind::BigInt;
	}
	else if (suffix.front() == 'u' || suffix.front() == 'i')
	{
		parsed.suffixKind = suffix.front() == 'u'
			? IntegerSuffixKind::Unsigned
			: IntegerSuffixKind::Signed;
		if (suffix.size() > 1)
		{
			if (!ParseWidth(suffix.substr(1), parsed.suffixWidth))
			{
				reason =
					"invalid PREDA integer literal suffix '" +
					suffix + "'";
				return false;
			}
			parsed.hasSuffixWidth = true;
			if (!FormulaSort::IsSupportedUnsignedWidth(
					parsed.suffixWidth))
			{
				reason =
					"unsupported PREDA integer literal width " +
					std::to_string(parsed.suffixWidth);
				return false;
			}
		}
	}
	else
	{
		reason =
			"unsupported PREDA integer literal suffix '" +
			suffix + "'";
		return false;
	}

	if (hexadecimal)
	{
		if (!HexToDecimal(
				digits,
				parsed.decimalValue,
				reason))
		{
			return false;
		}
	}
	else
	{
		parsed.decimalValue = StripLeadingZeros(digits);
	}
	if (negative && parsed.decimalValue != "0")
		parsed.decimalValue.insert(parsed.decimalValue.begin(), '-');
	return true;
}

bool ValidateLiteralSuffix(
	const ParsedIntegerLiteral &literal,
	const FormulaSort &sort,
	std::string &reason)
{
	if (sort.kind == FormulaSortKind::Int)
	{
		if (literal.suffixKind == IntegerSuffixKind::Unsigned)
		{
			reason =
				"unsigned PREDA literal cannot be encoded as "
				"mathematical Int without an explicit cast";
			return false;
		}
		return true;
	}
	if (sort.kind != FormulaSortKind::UnsignedBitVector)
	{
		reason = "integer literal does not have an integer formula sort";
		return false;
	}
	if (literal.suffixKind == IntegerSuffixKind::Signed ||
		literal.suffixKind == IntegerSuffixKind::BigInt)
	{
		reason =
			"signed PREDA literal cannot be encoded as an unsigned "
			"bit vector without an explicit cast";
		return false;
	}
	if (literal.hasSuffixWidth &&
		literal.suffixWidth != sort.bitWidth)
	{
		reason =
			"PREDA literal suffix width " +
			std::to_string(literal.suffixWidth) +
			" does not match Formula IR width " +
			std::to_string(sort.bitWidth) +
			"; implicit casts are forbidden";
		return false;
	}
	return true;
}

std::string FormulaSortKey(const FormulaSort &sort)
{
	switch (sort.kind)
	{
	case FormulaSortKind::Bool:
		return "bool";
	case FormulaSortKind::Int:
		return "int";
	case FormulaSortKind::UnsignedBitVector:
		return "bv" + std::to_string(sort.bitWidth);
	case FormulaSortKind::Address:
		return "address";
	case FormulaSortKind::Unknown:
		return "unknown";
	}
	return "unknown";
}

std::string HexEncode(const std::string &value)
{
	static const char digits[] = "0123456789abcdef";
	std::string encoded;
	encoded.reserve(value.size() * 2);
	for (unsigned char character : value)
	{
		encoded.push_back(digits[character >> 4u]);
		encoded.push_back(digits[character & 0x0fu]);
	}
	return encoded;
}

bool NumericSort(const FormulaSort &sort)
{
	return sort.kind == FormulaSortKind::Int ||
		sort.kind == FormulaSortKind::UnsignedBitVector;
}

bool ParseCastSort(
	const std::string &castName,
	FormulaSort &sort)
{
	const std::string name = Trim(castName);
	if (name == "bigint")
	{
		sort = FormulaSort::Int();
		return true;
	}
	if (name.compare(0, 4, "uint") == 0)
	{
		uint16_t width = 0;
		if (!ParseWidth(name.substr(4), width))
			return false;
		sort = FormulaSort::UnsignedBitVector(width);
		return sort.IsKnown();
	}
	if (name.compare(0, 3, "int") == 0)
	{
		uint16_t width = 0;
		if (!ParseWidth(name.substr(3), width))
			return false;
		sort = FormulaSort::Int();
		return true;
	}
	return false;
}

} // namespace

Z3FormulaEncoder::Z3FormulaEncoder(
	::z3::context &context,
	const Z3SymbolEncoder &symbols)
	: m_context(context),
	  m_symbols(symbols)
{
}

Z3ExprEncodingResult Z3FormulaEncoder::Encode(
	const FormulaExpr &formula) const
{
	if (&m_context != &m_symbols.GetContext())
	{
		return Failure(
			formula,
			"formula and symbol encoders use different Z3 contexts");
	}
	if (!m_symbols.IsValid())
	{
		return Failure(
			formula,
			"invalid refinement symbol metadata: " +
			m_symbols.GetValidationError());
	}
	try
	{
		return EncodeInternal(formula, 0);
	}
	catch (const ::z3::exception &error)
	{
		return Failure(
			formula,
			std::string("Z3 rejected the encoded formula: ") +
				error.msg());
	}
}

Z3ExprEncodingResult Z3FormulaEncoder::EncodeInternal(
	const FormulaExpr &formula,
	size_t depth) const
{
	if (depth > kMaximumFormulaDepth)
	{
		return Failure(
			formula,
			"formula nesting exceeds the Z3 encoder limit");
	}
	if (formula.kind == FormulaExprKind::Unknown ||
		formula.sort.kind == FormulaSortKind::Unknown)
	{
		return Failure(
			formula,
			formula.unknownReason.empty()
				? "Unknown formula nodes and sorts cannot be encoded"
				: "Unknown formula cannot be encoded: " +
					formula.unknownReason);
	}

	const Z3SortEncodingResult encodedSort =
		m_symbols.EncodeSort(formula.sort);
	if (!encodedSort.Succeeded())
		return Failure(formula, encodedSort.reason);

	switch (formula.kind)
	{
	case FormulaExprKind::BoolLiteral:
		if (formula.sort != FormulaSort::Bool())
			return Failure(formula, "Boolean literal is not sorted Bool");
		if (formula.literalValue == "true")
		{
			return Z3ExprEncodingResult::Success(
				m_context.bool_val(true));
		}
		if (formula.literalValue == "false")
		{
			return Z3ExprEncodingResult::Success(
				m_context.bool_val(false));
		}
		return Failure(
			formula,
			"Boolean literal must be exactly true or false");

	case FormulaExprKind::IntLiteral:
	case FormulaExprKind::BitVectorLiteral:
	{
		const std::string exactValue =
			formula.literalValue.empty()
				? formula.text
				: formula.literalValue;
		ParsedIntegerLiteral literal;
		std::string reason;
		if (!ParseIntegerLiteral(exactValue, literal, reason))
			return Failure(formula, reason);
		if (!ValidateLiteralSuffix(literal, formula.sort, reason))
			return Failure(formula, reason);
		if (formula.kind == FormulaExprKind::IntLiteral &&
			formula.sort != FormulaSort::Int())
		{
			return Failure(
				formula,
				"IntLiteral node is not sorted mathematical Int");
		}
		if (formula.kind == FormulaExprKind::BitVectorLiteral &&
			formula.sort.kind !=
				FormulaSortKind::UnsignedBitVector)
		{
			return Failure(
				formula,
				"BitVectorLiteral node is not sorted as a bit vector");
		}
		if (formula.sort.kind == FormulaSortKind::Int)
		{
			return Z3ExprEncodingResult::Success(
				m_context.int_val(literal.decimalValue.c_str()));
		}
		return Z3ExprEncodingResult::Success(
			m_context.bv_val(
				literal.decimalValue.c_str(),
				formula.sort.bitWidth));
	}

	case FormulaExprKind::AddressLiteral:
	{
		if (formula.sort != FormulaSort::Address())
			return Failure(formula, "address literal is not sorted Address");
		const std::string exactValue =
			formula.literalValue.empty()
				? formula.text
				: formula.literalValue;
		if (exactValue.empty())
			return Failure(formula, "address literal is empty");
		return Z3ExprEncodingResult::Success(
			m_context.constant(
				("RPreda.AddressLiteral." +
					HexEncode(exactValue))
					.c_str(),
				*encodedSort.value));
	}

	case FormulaExprKind::Symbol:
	{
		const Z3ExprEncodingResult symbol =
			m_symbols.EncodeSymbol(
				formula.symbolId,
				formula.sort);
		return symbol.Succeeded()
			? symbol
			: Failure(formula, symbol.reason);
	}

	case FormulaExprKind::Group:
	{
		if (formula.children.size() != 1)
			return Failure(formula, "group must have exactly one child");
		if (formula.children.front().sort != formula.sort)
		{
			return Failure(
				formula,
				"group result and child sorts differ; implicit casts "
				"are forbidden");
		}
		const Z3ExprEncodingResult child =
			EncodeInternal(formula.children.front(), depth + 1);
		return child.Succeeded()
			? child
			: Failure(formula, "group child: " + child.reason);
	}

	case FormulaExprKind::Unary:
		return EncodeUnary(formula, depth);

	case FormulaExprKind::Binary:
		return EncodeBinary(formula, depth);

	case FormulaExprKind::Nary:
		return EncodeNary(formula, depth);

	case FormulaExprKind::Cast:
		return EncodeCast(formula, depth);

	case FormulaExprKind::Ite:
	{
		if (formula.children.size() != 3)
		{
			return Failure(
				formula,
				"if-then-else must have condition and two result arms");
		}
		if (formula.children[0].sort != FormulaSort::Bool() ||
			formula.children[1].sort != formula.sort ||
			formula.children[2].sort != formula.sort)
		{
			return Failure(
				formula,
				"if-then-else condition/arm sorts do not match its "
				"declared sort");
		}
		const Z3ExprEncodingResult condition =
			EncodeInternal(formula.children[0], depth + 1);
		if (!condition.Succeeded())
			return Failure(formula, "ite condition: " + condition.reason);
		const Z3ExprEncodingResult thenValue =
			EncodeInternal(formula.children[1], depth + 1);
		if (!thenValue.Succeeded())
			return Failure(formula, "ite then arm: " + thenValue.reason);
		const Z3ExprEncodingResult elseValue =
			EncodeInternal(formula.children[2], depth + 1);
		if (!elseValue.Succeeded())
			return Failure(formula, "ite else arm: " + elseValue.reason);
		return Z3ExprEncodingResult::Success(
			::z3::ite(
				*condition.value,
				*thenValue.value,
				*elseValue.value));
	}

	case FormulaExprKind::ArrayLength:
		return EncodeArrayLength(formula, depth);

	case FormulaExprKind::Unknown:
		break;
	}
	return Failure(formula, "unrecognized Formula IR node kind");
}

Z3ExprEncodingResult Z3FormulaEncoder::EncodeUnary(
	const FormulaExpr &formula,
	size_t depth) const
{
	if (formula.children.size() != 1)
		return Failure(formula, "unary formula must have one operand");
	const FormulaExpr &operandFormula = formula.children.front();
	const Z3ExprEncodingResult operand =
		EncodeInternal(operandFormula, depth + 1);
	if (!operand.Succeeded())
		return Failure(formula, "unary operand: " + operand.reason);

	if (formula.op == "!")
	{
		if (operandFormula.sort != FormulaSort::Bool() ||
			formula.sort != FormulaSort::Bool())
		{
			return Failure(
				formula,
				"Boolean negation requires Bool operand and result");
		}
		return Z3ExprEncodingResult::Success(!*operand.value);
	}
	if (formula.op == "+")
	{
		if (!NumericSort(operandFormula.sort) ||
			formula.sort != operandFormula.sort)
		{
			return Failure(
				formula,
				"unary plus requires identical numeric operand/result "
				"sorts");
		}
		return operand;
	}
	if (formula.op == "-")
	{
		if (!NumericSort(operandFormula.sort) ||
			formula.sort != operandFormula.sort)
		{
			return Failure(
				formula,
				"unary minus requires identical numeric operand/result "
				"sorts");
		}
		return Z3ExprEncodingResult::Success(-*operand.value);
	}
	if (formula.op == "~")
	{
		if (operandFormula.sort.kind !=
				FormulaSortKind::UnsignedBitVector ||
			formula.sort != operandFormula.sort)
		{
			return Failure(
				formula,
				"bitwise complement requires an unsigned bit-vector "
				"operand and matching result sort");
		}
		return Z3ExprEncodingResult::Success(~*operand.value);
	}
	return Failure(
		formula,
		"unsupported unary operator '" + formula.op + "'");
}

Z3ExprEncodingResult Z3FormulaEncoder::EncodeBinary(
	const FormulaExpr &formula,
	size_t depth) const
{
	if (formula.children.size() != 2)
		return Failure(formula, "binary formula must have two operands");
	const FormulaExpr &leftFormula = formula.children[0];
	const FormulaExpr &rightFormula = formula.children[1];
	const Z3ExprEncodingResult left =
		EncodeInternal(leftFormula, depth + 1);
	if (!left.Succeeded())
		return Failure(formula, "left operand: " + left.reason);
	const Z3ExprEncodingResult right =
		EncodeInternal(rightFormula, depth + 1);
	if (!right.Succeeded())
		return Failure(formula, "right operand: " + right.reason);

	const bool operandsMatch =
		leftFormula.sort == rightFormula.sort;
	if (formula.op == "implies")
	{
		if (!operandsMatch ||
			leftFormula.sort != FormulaSort::Bool() ||
			formula.sort != FormulaSort::Bool())
		{
			return Failure(
				formula,
				"implication requires Bool operands and result");
		}
		return Z3ExprEncodingResult::Success(
			::z3::implies(*left.value, *right.value));
	}
	if (formula.op == "&&" || formula.op == "||")
	{
		if (!operandsMatch ||
			leftFormula.sort != FormulaSort::Bool() ||
			formula.sort != FormulaSort::Bool())
		{
			return Failure(
				formula,
				"Boolean conjunction/disjunction requires Bool "
				"operands and result");
		}
		return Z3ExprEncodingResult::Success(
			formula.op == "&&"
				? (*left.value && *right.value)
				: (*left.value || *right.value));
	}
	if (formula.op == "==" || formula.op == "!=")
	{
		if (!operandsMatch || formula.sort != FormulaSort::Bool())
		{
			return Failure(
				formula,
				"equality requires identical operand sorts and Bool "
				"result; implicit casts are forbidden");
		}
		const ::z3::expr equality = *left.value == *right.value;
		return Z3ExprEncodingResult::Success(
			formula.op == "==" ? equality : !equality);
	}
	if (formula.op == "<" || formula.op == "<=" ||
		formula.op == ">" || formula.op == ">=")
	{
		if (!operandsMatch || formula.sort != FormulaSort::Bool())
		{
			return Failure(
				formula,
				"comparison requires identical operand sorts and Bool "
				"result");
		}
		if (leftFormula.sort.kind == FormulaSortKind::Int)
		{
			if (formula.op == "<")
				return Z3ExprEncodingResult::Success(
					*left.value < *right.value);
			if (formula.op == "<=")
				return Z3ExprEncodingResult::Success(
					*left.value <= *right.value);
			if (formula.op == ">")
				return Z3ExprEncodingResult::Success(
					*left.value > *right.value);
			return Z3ExprEncodingResult::Success(
				*left.value >= *right.value);
		}
		if (leftFormula.sort.kind ==
			FormulaSortKind::UnsignedBitVector)
		{
			if (formula.op == "<")
				return Z3ExprEncodingResult::Success(
					::z3::ult(*left.value, *right.value));
			if (formula.op == "<=")
				return Z3ExprEncodingResult::Success(
					::z3::ule(*left.value, *right.value));
			if (formula.op == ">")
				return Z3ExprEncodingResult::Success(
					::z3::ugt(*left.value, *right.value));
			return Z3ExprEncodingResult::Success(
				::z3::uge(*left.value, *right.value));
		}
		return Failure(
			formula,
			"ordered comparison is only supported for Int and "
			"unsigned bit vectors");
	}

	if (formula.op == "<<" || formula.op == ">>")
	{
		if (!operandsMatch ||
			leftFormula.sort.kind !=
				FormulaSortKind::UnsignedBitVector ||
			formula.sort != leftFormula.sort)
		{
			return Failure(
				formula,
				"bit-vector shifts require same-width unsigned "
				"operands and matching result sort; implicit shift "
				"amount casts are forbidden");
		}
		return Z3ExprEncodingResult::Success(
			formula.op == "<<"
				? ::z3::shl(*left.value, *right.value)
				: ::z3::lshr(*left.value, *right.value));
	}

	if (!operandsMatch ||
		formula.sort != leftFormula.sort ||
		!NumericSort(leftFormula.sort))
	{
		return Failure(
			formula,
			"arithmetic requires identical numeric operand/result "
			"sorts; implicit casts are forbidden");
	}
	if (formula.op == "+")
		return Z3ExprEncodingResult::Success(*left.value + *right.value);
	if (formula.op == "-")
		return Z3ExprEncodingResult::Success(*left.value - *right.value);
	if (formula.op == "*")
		return Z3ExprEncodingResult::Success(*left.value * *right.value);
	if (formula.op == "/")
	{
		return Z3ExprEncodingResult::Success(
			leftFormula.sort.kind ==
					FormulaSortKind::UnsignedBitVector
				? ::z3::udiv(*left.value, *right.value)
				: *left.value / *right.value);
	}
	if (formula.op == "%")
	{
		return Z3ExprEncodingResult::Success(
			leftFormula.sort.kind ==
					FormulaSortKind::UnsignedBitVector
				? ::z3::urem(*left.value, *right.value)
				: *left.value % *right.value);
	}
	if (formula.op == "&" || formula.op == "^" || formula.op == "|")
	{
		if (leftFormula.sort.kind !=
			FormulaSortKind::UnsignedBitVector)
		{
			return Failure(
				formula,
				"bitwise arithmetic is unsupported for mathematical "
				"Int because no width is available");
		}
		if (formula.op == "&")
			return Z3ExprEncodingResult::Success(
				*left.value & *right.value);
		if (formula.op == "^")
			return Z3ExprEncodingResult::Success(
				*left.value ^ *right.value);
		return Z3ExprEncodingResult::Success(
			*left.value | *right.value);
	}
	return Failure(
		formula,
		"unsupported binary operator '" + formula.op + "'");
}

Z3ExprEncodingResult Z3FormulaEncoder::EncodeNary(
	const FormulaExpr &formula,
	size_t depth) const
{
	const bool booleanOperator =
		formula.op == "&&" || formula.op == "||";
	const bool arithmeticOperator =
		formula.op == "+" || formula.op == "*";
	if (!booleanOperator && !arithmeticOperator)
	{
		return Failure(
			formula,
			"unsupported n-ary operator '" + formula.op + "'");
	}
	if (booleanOperator && formula.sort != FormulaSort::Bool())
		return Failure(formula, "n-ary Boolean formula is not sorted Bool");
	if (arithmeticOperator && !NumericSort(formula.sort))
		return Failure(formula, "n-ary arithmetic formula is not numeric");

	if (formula.children.empty())
	{
		if (formula.op == "&&")
		{
			return Z3ExprEncodingResult::Success(
				m_context.bool_val(true));
		}
		if (formula.op == "||")
		{
			return Z3ExprEncodingResult::Success(
				m_context.bool_val(false));
		}
		const bool product = formula.op == "*";
		if (formula.sort.kind == FormulaSortKind::Int)
		{
			return Z3ExprEncodingResult::Success(
				m_context.int_val(product ? 1 : 0));
		}
		return Z3ExprEncodingResult::Success(
			m_context.bv_val(
				product ? 1u : 0u,
				formula.sort.bitWidth));
	}

	for (const FormulaExpr &child : formula.children)
	{
		if (child.sort != formula.sort)
		{
			return Failure(
				formula,
				"n-ary child sort differs from result sort; implicit "
				"casts are forbidden");
		}
	}
	Z3ExprEncodingResult accumulator =
		EncodeInternal(formula.children.front(), depth + 1);
	if (!accumulator.Succeeded())
	{
		return Failure(
			formula,
			"n-ary operand 0: " + accumulator.reason);
	}
	for (size_t index = 1; index < formula.children.size(); ++index)
	{
		const Z3ExprEncodingResult operand =
			EncodeInternal(formula.children[index], depth + 1);
		if (!operand.Succeeded())
		{
			return Failure(
				formula,
				"n-ary operand " + std::to_string(index) +
				": " + operand.reason);
		}
		if (formula.op == "&&")
			*accumulator.value = *accumulator.value && *operand.value;
		else if (formula.op == "||")
			*accumulator.value = *accumulator.value || *operand.value;
		else if (formula.op == "+")
			*accumulator.value = *accumulator.value + *operand.value;
		else
			*accumulator.value = *accumulator.value * *operand.value;
	}
	return accumulator;
}

Z3ExprEncodingResult Z3FormulaEncoder::EncodeCast(
	const FormulaExpr &formula,
	size_t depth) const
{
	if (formula.children.size() != 1)
		return Failure(formula, "cast must have exactly one operand");
	FormulaSort namedCastSort = FormulaSort::Unknown();
	if (!ParseCastSort(formula.op, namedCastSort))
	{
		return Failure(
			formula,
			"unsupported explicit PREDA cast '" + formula.op + "'");
	}
	if (namedCastSort != formula.sort)
	{
		return Failure(
			formula,
			"explicit cast name '" + formula.op +
			"' does not match its Formula IR result sort");
	}

	const FormulaExpr &operandFormula = formula.children.front();
	if (!NumericSort(operandFormula.sort) ||
		!NumericSort(formula.sort))
	{
		return Failure(
			formula,
			"only explicit integer and unsigned bit-vector casts are "
			"supported");
	}
	const Z3ExprEncodingResult operand =
		EncodeInternal(operandFormula, depth + 1);
	if (!operand.Succeeded())
		return Failure(formula, "cast operand: " + operand.reason);

	if (operandFormula.sort == formula.sort)
		return operand;
	if (operandFormula.sort.kind ==
			FormulaSortKind::UnsignedBitVector &&
		formula.sort.kind ==
			FormulaSortKind::UnsignedBitVector)
	{
		const uint16_t sourceWidth = operandFormula.sort.bitWidth;
		const uint16_t targetWidth = formula.sort.bitWidth;
		if (targetWidth > sourceWidth)
		{
			return Z3ExprEncodingResult::Success(
				::z3::zext(
					*operand.value,
					static_cast<unsigned>(
						targetWidth - sourceWidth)));
		}
		return Z3ExprEncodingResult::Success(
			operand.value->extract(
				static_cast<unsigned>(targetWidth - 1u),
				0u));
	}
	if (operandFormula.sort.kind == FormulaSortKind::Int &&
		formula.sort.kind ==
			FormulaSortKind::UnsignedBitVector)
	{
		return Z3ExprEncodingResult::Success(
			::z3::int2bv(
				formula.sort.bitWidth,
				*operand.value));
	}
	if (operandFormula.sort.kind ==
			FormulaSortKind::UnsignedBitVector &&
		formula.sort.kind == FormulaSortKind::Int)
	{
		// PREDA unsigned values become non-negative mathematical Ints.
		return Z3ExprEncodingResult::Success(
			::z3::bv2int(*operand.value, false));
	}
	return Failure(
		formula,
		"unsupported explicit cast between Formula IR sorts");
}

Z3ExprEncodingResult Z3FormulaEncoder::EncodeArrayLength(
	const FormulaExpr &formula,
	size_t depth) const
{
	if (formula.children.size() != 1)
	{
		return Failure(
			formula,
			"array length must have exactly one receiver");
	}
	if (formula.sort != FormulaSort::UnsignedBitVector(32))
	{
		return Failure(
			formula,
			"PREDA array length must be sorted uint32");
	}
	const FormulaExpr &receiverFormula = formula.children.front();
	const Z3ExprEncodingResult receiver =
		EncodeInternal(receiverFormula, depth + 1);
	if (!receiver.Succeeded())
	{
		return Failure(
			formula,
			"array length receiver: " + receiver.reason);
	}
	const Z3SortEncodingResult receiverSort =
		m_symbols.EncodeSort(receiverFormula.sort);
	if (!receiverSort.Succeeded())
	{
		return Failure(
			formula,
			"array length receiver sort: " + receiverSort.reason);
	}

	const std::string key = FormulaSortKey(receiverFormula.sort);
	auto iterator = m_arrayLengthFunctions.find(key);
	if (iterator == m_arrayLengthFunctions.end())
	{
		const std::string functionName =
			"RPreda.ArrayLength." + key;
		auto declaration = std::make_unique<::z3::func_decl>(
			m_context.function(
				functionName.c_str(),
				*receiverSort.value,
				m_context.bv_sort(32)));
		iterator = m_arrayLengthFunctions.emplace(
			key,
			std::move(declaration)).first;
	}
	return Z3ExprEncodingResult::Success(
		(*iterator->second)(*receiver.value));
}

Z3ExprEncodingResult Z3FormulaEncoder::Failure(
	const FormulaExpr &formula,
	const std::string &reason) const
{
	std::ostringstream message;
	message << reason;
	if (formula.location.IsValid())
	{
		message << " at " << formula.location.line
				<< ":" << formula.location.column;
	}
	if (!formula.text.empty())
		message << " [source: " << formula.text << "]";
	return Z3ExprEncodingResult::Failure(message.str());
}

} // namespace z3_backend
} // namespace solver
} // namespace refinement
} // namespace relay_protocol
} // namespace transpiler
