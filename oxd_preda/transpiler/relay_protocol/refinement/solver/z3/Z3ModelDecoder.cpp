#include "Z3ModelDecoder.h"

#include <set>
#include <utility>

namespace transpiler {
namespace relay_protocol {
namespace refinement {
namespace solver {
namespace z3_backend {
namespace {

void CollectSymbolIds(
	const FormulaExpr &formula,
	std::set<std::string> &symbolIds)
{
	if (formula.kind == FormulaExprKind::Symbol &&
		!formula.symbolId.empty())
	{
		symbolIds.insert(formula.symbolId);
	}
	for (const FormulaExpr &child : formula.children)
		CollectSymbolIds(child, symbolIds);
}

} // namespace

Z3ModelDecoder::Z3ModelDecoder(
	const Z3SymbolEncoder &symbols)
	: m_symbols(symbols)
{
}

Z3ModelDecodeResult Z3ModelDecoder::Decode(
	const ::z3::model &model,
	const std::vector<const RelayConstraint *> &assumptions,
	const FormulaExpr &goal) const
{
	Z3ModelDecodeResult result;
	std::set<std::string> symbolIds;
	for (const RelayConstraint *assumption : assumptions)
	{
		if (assumption == nullptr)
		{
			result.reason =
				"cannot project a model for a null solver assumption";
			return result;
		}
		CollectSymbolIds(assumption->formula, symbolIds);
	}
	CollectSymbolIds(goal, symbolIds);

	try
	{
		for (const std::string &symbolId : symbolIds)
		{
			const RelayRefinementSymbol *metadata =
				m_symbols.FindMetadata(symbolId);
			if (metadata == nullptr)
			{
				result.reason =
					"cannot project unknown refinement symbol '" +
					symbolId + "'";
				result.values.clear();
				return result;
			}

			const Z3ExprEncodingResult encoded =
				m_symbols.EncodeSymbol(symbolId, metadata->sort);
			if (!encoded.Succeeded())
			{
				result.reason =
					"cannot encode projected refinement symbol '" +
					symbolId + "': " + encoded.reason;
				result.values.clear();
				return result;
			}

			const ::z3::expr value =
				model.eval(*encoded.value, true);
			RelayCounterexampleValue projected;
			projected.symbolId = symbolId;
			projected.sort = metadata->sort;
			projected.value = value.to_string();
			result.values.push_back(std::move(projected));
		}
	}
	catch (const ::z3::exception &error)
	{
		result.reason =
			std::string("Z3 model projection failed: ") +
			error.msg();
		result.values.clear();
	}
	catch (const std::exception &error)
	{
		result.reason =
			std::string("model projection failed: ") +
			error.what();
		result.values.clear();
	}
	catch (...)
	{
		result.reason =
			"model projection failed with an unknown exception";
		result.values.clear();
	}
	return result;
}

} // namespace z3_backend
} // namespace solver
} // namespace refinement
} // namespace relay_protocol
} // namespace transpiler
