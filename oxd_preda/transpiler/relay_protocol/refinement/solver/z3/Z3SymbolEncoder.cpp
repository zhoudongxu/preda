#include "Z3SymbolEncoder.h"

#include <sstream>
#include <utility>

namespace transpiler {
namespace relay_protocol {
namespace refinement {
namespace solver {
namespace z3_backend {
namespace {

std::string SortName(const FormulaSort &sort)
{
	switch (sort.kind)
	{
	case FormulaSortKind::Bool:
		return "Bool";
	case FormulaSortKind::Int:
		return "Int";
	case FormulaSortKind::UnsignedBitVector:
		return "uint" + std::to_string(sort.bitWidth);
	case FormulaSortKind::Address:
		return "Address";
	case FormulaSortKind::Unknown:
		return "Unknown";
	}
	return "Unknown";
}

} // namespace

Z3SortEncodingResult Z3SortEncodingResult::Success(::z3::sort sort)
{
	Z3SortEncodingResult result;
	result.value.emplace(std::move(sort));
	return result;
}

Z3SortEncodingResult Z3SortEncodingResult::Failure(
	const std::string &reason)
{
	Z3SortEncodingResult result;
	result.reason = reason;
	return result;
}

Z3ExprEncodingResult Z3ExprEncodingResult::Success(
	::z3::expr expression)
{
	Z3ExprEncodingResult result;
	result.value.emplace(std::move(expression));
	return result;
}

Z3ExprEncodingResult Z3ExprEncodingResult::Failure(
	const std::string &reason)
{
	Z3ExprEncodingResult result;
	result.reason = reason;
	return result;
}

Z3SymbolEncoder::Z3SymbolEncoder(
	::z3::context &context,
	const std::vector<RelayRefinementSymbol> &symbols)
	: m_context(context),
	  m_symbols(symbols),
	  m_addressSort(
		  context.uninterpreted_sort("RPreda.Address"))
{
	for (const RelayRefinementSymbol &symbol : m_symbols)
	{
		if (symbol.id.empty())
		{
			if (m_validationError.empty())
				m_validationError =
					"refinement symbol metadata contains an empty ID";
			continue;
		}
		const auto inserted =
			m_symbolsById.emplace(symbol.id, &symbol);
		if (!inserted.second && m_validationError.empty())
		{
			m_validationError =
				"duplicate refinement symbol metadata ID '" +
				symbol.id + "'";
		}
	}
}

::z3::context &Z3SymbolEncoder::GetContext() const
{
	return m_context;
}

bool Z3SymbolEncoder::IsValid() const
{
	return m_validationError.empty();
}

const std::string &Z3SymbolEncoder::GetValidationError() const
{
	return m_validationError;
}

Z3SortEncodingResult Z3SymbolEncoder::EncodeSort(
	const FormulaSort &sort) const
{
	try
	{
		switch (sort.kind)
		{
		case FormulaSortKind::Bool:
			if (sort.bitWidth != 0)
			{
				return Z3SortEncodingResult::Failure(
					"Bool formula sort unexpectedly carries a bit width");
			}
			return Z3SortEncodingResult::Success(
				m_context.bool_sort());

		case FormulaSortKind::Int:
			if (sort.bitWidth != 0)
			{
				return Z3SortEncodingResult::Failure(
					"Int formula sort unexpectedly carries a bit width");
			}
			return Z3SortEncodingResult::Success(
				m_context.int_sort());

		case FormulaSortKind::UnsignedBitVector:
			if (!FormulaSort::IsSupportedUnsignedWidth(sort.bitWidth))
			{
				return Z3SortEncodingResult::Failure(
					"unsupported unsigned bit-vector width " +
					std::to_string(sort.bitWidth));
			}
			return Z3SortEncodingResult::Success(
				m_context.bv_sort(sort.bitWidth));

		case FormulaSortKind::Address:
			if (sort.bitWidth != 0)
			{
				return Z3SortEncodingResult::Failure(
					"Address formula sort unexpectedly carries a bit width");
			}
			// All Address values in one context share this single
			// uninterpreted sort.
			if (!m_addressSort.has_value())
			{
				return Z3SortEncodingResult::Failure(
					"the context-bound Address sort is unavailable");
			}
			return Z3SortEncodingResult::Success(*m_addressSort);

		case FormulaSortKind::Unknown:
			return Z3SortEncodingResult::Failure(
				"Unknown formula sort cannot be encoded");
		}
	}
	catch (const ::z3::exception &error)
	{
		return Z3SortEncodingResult::Failure(
			"Z3 rejected formula sort " + SortName(sort) +
			": " + error.msg());
	}

	return Z3SortEncodingResult::Failure(
		"unrecognized formula sort kind");
}

Z3ExprEncodingResult Z3SymbolEncoder::EncodeSymbol(
	const std::string &symbolId,
	const FormulaSort &formulaSort) const
{
	if (!IsValid())
		return Z3ExprEncodingResult::Failure(m_validationError);
	if (symbolId.empty())
	{
		return Z3ExprEncodingResult::Failure(
			"formula symbol has an empty stable ID");
	}

	const RelayRefinementSymbol *metadata =
		FindMetadata(symbolId);
	if (metadata == nullptr)
	{
		return Z3ExprEncodingResult::Failure(
			"formula symbol '" + symbolId +
			"' is absent from refinement symbol metadata");
	}
	if (metadata->sort != formulaSort)
	{
		return Z3ExprEncodingResult::Failure(
			"formula symbol '" + symbolId + "' has sort " +
			SortName(formulaSort) +
			", but metadata records " +
			SortName(metadata->sort) +
			"; implicit casts are forbidden");
	}

	const Z3SortEncodingResult encodedSort =
		EncodeSort(metadata->sort);
	if (!encodedSort.Succeeded())
	{
		return Z3ExprEncodingResult::Failure(
			"cannot encode refinement symbol '" + symbolId +
			"': " + encodedSort.reason);
	}

	try
	{
		return Z3ExprEncodingResult::Success(
			m_context.constant(
				symbolId.c_str(),
				*encodedSort.value));
	}
	catch (const ::z3::exception &error)
	{
		return Z3ExprEncodingResult::Failure(
			"Z3 rejected refinement symbol '" + symbolId +
			"': " + error.msg());
	}
}

const RelayRefinementSymbol *Z3SymbolEncoder::FindMetadata(
	const std::string &symbolId) const
{
	const auto iterator = m_symbolsById.find(symbolId);
	return iterator == m_symbolsById.end()
		? nullptr
		: iterator->second;
}

} // namespace z3_backend
} // namespace solver
} // namespace refinement
} // namespace relay_protocol
} // namespace transpiler
