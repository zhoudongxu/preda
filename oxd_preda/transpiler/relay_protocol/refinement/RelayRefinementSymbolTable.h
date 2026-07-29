#pragma once

#include "RelayFormulaBuilder.h"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace transpiler {
namespace relay_protocol {

struct FunctionProtocol;
struct RelaySite;

namespace refinement {

struct RelayRefinementValueBinding
{
	std::string sourceName;
	std::string sourceType;
	std::string symbolId;
	FormulaExpr formula;
	bool hasFormula = false;
	bool valid = true;
	bool mutableValue = true;
	std::string invalidReason;
	SourceLocation location;
};

class RelayRefinementSymbolTable
{
public:
	void Reset();

	const RelayRefinementSymbol &EnsureParameter(
		const std::string &functionId,
		const std::string &name,
		const std::string &sourceType,
		const analysis::RelayExpressionDependency &dependency,
		const SourceLocation &location = SourceLocation());
	const RelayRefinementSymbol &EnsureCurrentScopeKey(
		const std::string &functionId,
		const std::string &sourceType,
		const analysis::RelayExpressionDependency &dependency,
		const SourceLocation &location = SourceLocation());
	const RelayRefinementSymbol &EnsurePreState(
		const std::string &functionId,
		const std::string &name,
		const std::string &sourceType,
		const analysis::RelayExpressionDependency &dependency,
		const SourceLocation &location = SourceLocation());
	const RelayRefinementSymbol &EnsureLoopVariable(
		const std::string &functionId,
		const std::string &name,
		const std::string &sourceType,
		const analysis::RelayExpressionDependency &dependency,
		const SourceLocation &location = SourceLocation());

	const RelayRefinementSymbol &EnsureRelayEmission(
		const std::string &functionId,
		const std::string &relaySiteId,
		const SourceLocation &location = SourceLocation());
	const RelayRefinementSymbol &EnsureActualTarget(
		const std::string &functionId,
		const std::string &relaySiteId,
		const std::string &sourceType,
		const analysis::RelayExpressionDependency &dependency,
		const SourceLocation &location = SourceLocation());
	const RelayRefinementSymbol &EnsureActualArgument(
		const std::string &functionId,
		const std::string &relaySiteId,
		size_t argumentIndex,
		const std::string &sourceType,
		const analysis::RelayExpressionDependency &dependency,
		const SourceLocation &location = SourceLocation());
	const RelayRefinementSymbol &EnsureDirectRelayCount(
		const std::string &functionId,
		const SourceLocation &location = SourceLocation());

	// Convenience overloads used by the constraint generator.
	const RelayRefinementSymbol &EnsureRelayEmission(const RelaySite &site);
	const RelayRefinementSymbol &EnsureActualTarget(const RelaySite &site);
	const RelayRefinementSymbol &EnsureActualArgument(
		const RelaySite &site,
		size_t argumentIndex);
	const RelayRefinementSymbol &EnsureDirectRelayCount(
		const FunctionProtocol &function);

	// Current-value scopes are analysis state, separate from immutable entry
	// symbols. A local initializer is frozen as an owning FormulaExpr.
	void BeginFunctionValues(const std::string &functionId);
	void EndFunctionValues(const std::string &functionId);
	void PushScope(const std::string &functionId);
	void PopScope(const std::string &functionId);
	void BindSymbolValue(
		const std::string &functionId,
		const std::string &name,
		const std::string &symbolId,
		const std::string &sourceType,
		bool mutableValue,
		const SourceLocation &location = SourceLocation());
	void SetLocalFormula(
		const std::string &functionId,
		const std::string &name,
		const std::string &sourceType,
		const FormulaExpr &formula,
		const SourceLocation &location = SourceLocation());
	void RegisterLocalDefinition(
		const std::string &functionId,
		const std::string &name,
		const std::string &sourceType,
		const FormulaExpr &formula,
		const SourceLocation &location = SourceLocation())
	{
		SetLocalFormula(
			functionId,
			name,
			sourceType,
			formula,
			location);
	}
	void InvalidateValue(
		const std::string &functionId,
		const std::string &name,
		const std::string &reason,
		const SourceLocation &location = SourceLocation());
	void InvalidateLocalDefinition(
		const std::string &functionId,
		const std::string &name,
		const std::string &reason,
		const SourceLocation &location = SourceLocation())
	{
		InvalidateValue(functionId, name, reason, location);
	}
	void InvalidateAllMutable(
		const std::string &functionId,
		const std::string &reason,
		const SourceLocation &location = SourceLocation());

	RelayFormulaResolution ResolveCurrentValue(
		const std::string &functionId,
		const RelayExprIR &expression) const;
	const RelayRefinementSymbol *ResolveExpressionSymbol(
		const std::string &functionId,
		const RelayExprIR &expression) const;
	const FormulaExpr *ResolveLocalDefinition(
		const std::string &functionId,
		const RelayExprIR &expression) const;
	RelayFormulaResolver MakeResolver(const std::string &functionId) const;

	const RelayRefinementSymbol *Find(const std::string &symbolId) const;
	const std::vector<RelayRefinementSymbol> &GetSymbols() const
	{
		return m_symbols;
	}

	static std::string StableId(
		RelayRefinementSymbolKind kind,
		const std::string &functionId,
		const std::string &sourceName = std::string(),
		const std::string &relaySiteId = std::string(),
		int64_t argumentIndex = -1);

private:
	using ValueScope = std::map<std::string, RelayRefinementValueBinding>;

	const RelayRefinementSymbol &Ensure(
		RelayRefinementSymbol symbol);
	RelayRefinementValueBinding *FindMutableBinding(
		const std::string &functionId,
		const std::string &name);
	const RelayRefinementValueBinding *FindBinding(
		const std::string &functionId,
		const std::string &name) const;

	std::vector<RelayRefinementSymbol> m_symbols;
	std::map<std::string, size_t> m_symbolIndexes;
	std::map<std::string, std::vector<ValueScope>> m_valueScopes;
};

} // namespace refinement
} // namespace relay_protocol
} // namespace transpiler
