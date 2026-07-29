#pragma once

#include "Z3SymbolEncoder.h"

#include "../../RelayFormulaIR.h"

#include <z3++.h>

#include <memory>
#include <string>
#include <unordered_map>

namespace transpiler {
namespace relay_protocol {
namespace refinement {
namespace solver {
namespace z3_backend {

// Encodes the owning, solver-independent Formula IR without inventing
// implicit casts or unconstrained replacements for Unknown nodes.
class Z3FormulaEncoder
{
public:
	Z3FormulaEncoder(
		::z3::context &context,
		const Z3SymbolEncoder &symbols);

	Z3ExprEncodingResult Encode(const FormulaExpr &formula) const;

private:
	Z3ExprEncodingResult EncodeInternal(
		const FormulaExpr &formula,
		size_t depth) const;

	Z3ExprEncodingResult EncodeUnary(
		const FormulaExpr &formula,
		size_t depth) const;

	Z3ExprEncodingResult EncodeBinary(
		const FormulaExpr &formula,
		size_t depth) const;

	Z3ExprEncodingResult EncodeNary(
		const FormulaExpr &formula,
		size_t depth) const;

	Z3ExprEncodingResult EncodeCast(
		const FormulaExpr &formula,
		size_t depth) const;

	Z3ExprEncodingResult EncodeArrayLength(
		const FormulaExpr &formula,
		size_t depth) const;

	Z3ExprEncodingResult Failure(
		const FormulaExpr &formula,
		const std::string &reason) const;

	::z3::context &m_context;
	const Z3SymbolEncoder &m_symbols;
	mutable std::unordered_map<
		std::string,
		std::unique_ptr<::z3::func_decl>>
		m_arrayLengthFunctions;
};

} // namespace z3_backend
} // namespace solver
} // namespace refinement
} // namespace relay_protocol
} // namespace transpiler
