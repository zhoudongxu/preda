#pragma once

#include "../../RelayRefinementSymbol.h"

#include <z3++.h>

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace transpiler {
namespace relay_protocol {
namespace refinement {
namespace solver {
namespace z3_backend {

struct Z3SortEncodingResult
{
	std::optional<::z3::sort> value;
	std::string reason;

	bool Succeeded() const
	{
		return value.has_value();
	}

	static Z3SortEncodingResult Success(::z3::sort sort);
	static Z3SortEncodingResult Failure(const std::string &reason);
};

struct Z3ExprEncodingResult
{
	std::optional<::z3::expr> value;
	std::string reason;

	bool Succeeded() const
	{
		return value.has_value();
	}

	static Z3ExprEncodingResult Success(::z3::expr expression);
	static Z3ExprEncodingResult Failure(const std::string &reason);
};

// Context-bound encoder for declared refinement symbols. Formula nodes may
// only refer to IDs present in the compiler-produced metadata, and the sort
// recorded on the formula must exactly match the metadata sort.
class Z3SymbolEncoder
{
public:
	Z3SymbolEncoder(
		::z3::context &context,
		const std::vector<RelayRefinementSymbol> &symbols);

	::z3::context &GetContext() const;

	bool IsValid() const;
	const std::string &GetValidationError() const;

	Z3SortEncodingResult EncodeSort(const FormulaSort &sort) const;

	Z3ExprEncodingResult EncodeSymbol(
		const std::string &symbolId,
		const FormulaSort &formulaSort) const;

	const RelayRefinementSymbol *FindMetadata(
		const std::string &symbolId) const;

private:
	::z3::context &m_context;
	const std::vector<RelayRefinementSymbol> &m_symbols;
	std::optional<::z3::sort> m_addressSort;
	std::unordered_map<std::string, const RelayRefinementSymbol *>
		m_symbolsById;
	std::string m_validationError;
};

} // namespace z3_backend
} // namespace solver
} // namespace refinement
} // namespace relay_protocol
} // namespace transpiler
