#pragma once

#include "../RelayExprIR.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace transpiler {
namespace relay_protocol {
namespace refinement {

enum class FormulaSortKind : uint8_t
{
	Bool,
	Int,
	UnsignedBitVector,
	Address,
	Unknown,
};

struct FormulaSort
{
	FormulaSortKind kind = FormulaSortKind::Unknown;
	uint16_t bitWidth = 0;

	static FormulaSort Bool()
	{
		return FormulaSort{FormulaSortKind::Bool, 0};
	}

	static FormulaSort Int()
	{
		return FormulaSort{FormulaSortKind::Int, 0};
	}

	static FormulaSort UnsignedBitVector(uint16_t width)
	{
		return IsSupportedUnsignedWidth(width)
			? FormulaSort{FormulaSortKind::UnsignedBitVector, width}
			: Unknown();
	}

	static FormulaSort Address()
	{
		return FormulaSort{FormulaSortKind::Address, 0};
	}

	static FormulaSort Unknown()
	{
		return FormulaSort{FormulaSortKind::Unknown, 0};
	}

	static bool IsSupportedUnsignedWidth(uint16_t width)
	{
		switch (width)
		{
		case 8:
		case 16:
		case 32:
		case 64:
		case 96:
		case 128:
		case 160:
		case 256:
		case 512:
			return true;
		default:
			return false;
		}
	}

	bool IsKnown() const
	{
		return kind != FormulaSortKind::Unknown;
	}

	bool operator==(const FormulaSort &other) const
	{
		return kind == other.kind && bitWidth == other.bitWidth;
	}

	bool operator!=(const FormulaSort &other) const
	{
		return !(*this == other);
	}
};

enum class FormulaExprKind : uint8_t
{
	BoolLiteral,
	IntLiteral,
	BitVectorLiteral,
	AddressLiteral,
	Symbol,
	Group,
	Unary,
	Binary,
	Nary,
	Cast,
	Ite,
	ArrayLength,
	Unknown,
};

// An owning solver-independent formula tree. No parser contexts, compiler
// symbols, or string views survive in this representation.
struct FormulaExpr
{
	FormulaExprKind kind = FormulaExprKind::Unknown;
	FormulaSort sort = FormulaSort::Unknown();
	std::string text;
	std::string op;
	SourceLocation location;
	std::vector<FormulaExpr> children;
	std::string symbolId;
	// Exact PREDA spelling, including radix and integer suffix.
	std::string literalValue;
	std::string unknownReason;

	static FormulaExpr BoolLiteral(
		bool value,
		const std::string &sourceText = std::string(),
		const SourceLocation &sourceLocation = SourceLocation())
	{
		FormulaExpr result;
		result.kind = FormulaExprKind::BoolLiteral;
		result.sort = FormulaSort::Bool();
		result.text = sourceText.empty() ? (value ? "true" : "false") : sourceText;
		result.literalValue = value ? "true" : "false";
		result.location = sourceLocation;
		return result;
	}

	static FormulaExpr IntLiteral(
		const std::string &exactValue,
		const SourceLocation &sourceLocation = SourceLocation())
	{
		FormulaExpr result;
		result.kind = FormulaExprKind::IntLiteral;
		result.sort = FormulaSort::Int();
		result.text = exactValue;
		result.literalValue = exactValue;
		result.location = sourceLocation;
		return result;
	}

	static FormulaExpr BitVectorLiteral(
		const std::string &exactValue,
		uint16_t width,
		const SourceLocation &sourceLocation = SourceLocation())
	{
		FormulaExpr result;
		result.kind = FormulaExprKind::BitVectorLiteral;
		result.sort = FormulaSort::UnsignedBitVector(width);
		result.text = exactValue;
		result.literalValue = exactValue;
		result.location = sourceLocation;
		if (!result.sort.IsKnown())
		{
			result.kind = FormulaExprKind::Unknown;
			result.unknownReason =
				"unsupported unsigned bit-vector width " +
				std::to_string(width);
		}
		return result;
	}

	static FormulaExpr AddressLiteral(
		const std::string &exactValue,
		const SourceLocation &sourceLocation = SourceLocation())
	{
		FormulaExpr result;
		result.kind = FormulaExprKind::AddressLiteral;
		result.sort = FormulaSort::Address();
		result.text = exactValue;
		result.literalValue = exactValue;
		result.location = sourceLocation;
		return result;
	}

	static FormulaExpr Symbol(
		const std::string &id,
		const FormulaSort &symbolSort,
		const SourceLocation &sourceLocation = SourceLocation(),
		const std::string &sourceText = std::string())
	{
		FormulaExpr result;
		result.kind = FormulaExprKind::Symbol;
		result.sort = symbolSort;
		result.text = sourceText;
		result.symbolId = id;
		result.location = sourceLocation;
		return result;
	}

	static FormulaExpr Group(
		FormulaExpr child,
		const SourceLocation &sourceLocation = SourceLocation(),
		const std::string &sourceText = std::string())
	{
		FormulaExpr result;
		result.kind = FormulaExprKind::Group;
		result.sort = child.sort;
		result.text = sourceText;
		result.location = sourceLocation;
		result.children.push_back(std::move(child));
		return result;
	}

	static FormulaExpr Unary(
		const std::string &operatorText,
		FormulaExpr child,
		const FormulaSort &resultSort,
		const SourceLocation &sourceLocation = SourceLocation(),
		const std::string &sourceText = std::string())
	{
		FormulaExpr result;
		result.kind = FormulaExprKind::Unary;
		result.sort = resultSort;
		result.text = sourceText;
		result.op = operatorText;
		result.location = sourceLocation;
		result.children.push_back(std::move(child));
		return result;
	}

	static FormulaExpr Binary(
		const std::string &operatorText,
		FormulaExpr left,
		FormulaExpr right,
		const FormulaSort &resultSort,
		const SourceLocation &sourceLocation = SourceLocation(),
		const std::string &sourceText = std::string())
	{
		FormulaExpr result;
		result.kind = FormulaExprKind::Binary;
		result.sort = resultSort;
		result.text = sourceText;
		result.op = operatorText;
		result.location = sourceLocation;
		result.children.push_back(std::move(left));
		result.children.push_back(std::move(right));
		return result;
	}

	static FormulaExpr Nary(
		const std::string &operatorText,
		std::vector<FormulaExpr> operands,
		const FormulaSort &resultSort,
		const SourceLocation &sourceLocation = SourceLocation(),
		const std::string &sourceText = std::string())
	{
		FormulaExpr result;
		result.kind = FormulaExprKind::Nary;
		result.sort = resultSort;
		result.text = sourceText;
		result.op = operatorText;
		result.location = sourceLocation;
		result.children = std::move(operands);
		return result;
	}

	static FormulaExpr Cast(
		const std::string &castName,
		FormulaExpr value,
		const FormulaSort &resultSort,
		const SourceLocation &sourceLocation = SourceLocation(),
		const std::string &sourceText = std::string())
	{
		FormulaExpr result;
		result.kind = FormulaExprKind::Cast;
		result.sort = resultSort;
		result.text = sourceText;
		result.op = castName;
		result.location = sourceLocation;
		result.children.push_back(std::move(value));
		return result;
	}

	static FormulaExpr Ite(
		FormulaExpr condition,
		FormulaExpr thenValue,
		FormulaExpr elseValue,
		const FormulaSort &resultSort,
		const SourceLocation &sourceLocation = SourceLocation(),
		const std::string &sourceText = std::string())
	{
		FormulaExpr result;
		result.kind = FormulaExprKind::Ite;
		result.sort = resultSort;
		result.text = sourceText;
		result.op = "ite";
		result.location = sourceLocation;
		result.children.push_back(std::move(condition));
		result.children.push_back(std::move(thenValue));
		result.children.push_back(std::move(elseValue));
		return result;
	}

	static FormulaExpr ArrayLength(
		FormulaExpr array,
		const SourceLocation &sourceLocation = SourceLocation(),
		const std::string &sourceText = std::string())
	{
		FormulaExpr result;
		result.kind = FormulaExprKind::ArrayLength;
		result.sort = FormulaSort::UnsignedBitVector(32);
		result.text = sourceText;
		result.op = "length";
		result.location = sourceLocation;
		result.children.push_back(std::move(array));
		return result;
	}

	static FormulaExpr Unknown(
		const std::string &sourceText,
		const SourceLocation &sourceLocation,
		const std::string &reason)
	{
		FormulaExpr result;
		result.kind = FormulaExprKind::Unknown;
		result.sort = FormulaSort::Unknown();
		result.text = sourceText;
		result.location = sourceLocation;
		result.unknownReason = reason;
		return result;
	}

	bool IsUnknown() const
	{
		return kind == FormulaExprKind::Unknown;
	}
};

} // namespace refinement
} // namespace relay_protocol
} // namespace transpiler
