#pragma once

#include "RelayFormulaIR.h"
#include "RelayRefinementSymbol.h"

#include <functional>
#include <string>
#include <vector>

namespace transpiler {
namespace relay_protocol {
namespace refinement {

struct RelayFormulaResolution
{
	// handled=true with neither symbol nor formula represents an explicitly
	// invalidated/unavailable current value.
	bool handled = false;
	const RelayRefinementSymbol *symbol = nullptr;
	const FormulaExpr *formula = nullptr;
	std::string unavailableReason;
};

using RelayFormulaValueResolver =
	std::function<RelayFormulaResolution(const RelayExprIR &)>;
using RelayFormulaSymbolResolver =
	std::function<const RelayRefinementSymbol *(const RelayExprIR &)>;
using RelayFormulaDefinitionResolver =
	std::function<const FormulaExpr *(const RelayExprIR &)>;

struct RelayFormulaResolver
{
	// The unified current-value resolver takes precedence. The two legacy
	// callbacks remain convenient for read-only generators.
	RelayFormulaValueResolver value;
	RelayFormulaSymbolResolver symbol;
	RelayFormulaDefinitionResolver definition;
};

class RelayFormulaBuilder
{
public:
	FormulaExpr Build(const RelayExprIR &expression) const;
	FormulaExpr Build(
		const RelayExprIR &expression,
		const RelayFormulaResolver &resolver) const;

	static FormulaSort SortFromPredaType(const std::string &type);
	static bool IsSupportedIntegerCast(const std::string &callee);

private:
	FormulaExpr BuildInternal(
		const RelayExprIR &expression,
		const RelayFormulaResolver &resolver,
		std::vector<std::string> &activeDefinitions,
		size_t depth) const;
	FormulaExpr BuildLiteral(const RelayExprIR &expression) const;
	FormulaExpr BuildCall(
		const RelayExprIR &expression,
		const RelayFormulaResolver &resolver,
		std::vector<std::string> &activeDefinitions,
		size_t depth) const;
};

} // namespace refinement
} // namespace relay_protocol
} // namespace transpiler
