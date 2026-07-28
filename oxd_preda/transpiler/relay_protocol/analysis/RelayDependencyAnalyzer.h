#pragma once

#include "RelayExpressionDependency.h"
#include "../RelayExprIR.h"
#include "../../transpiler/PredaCommon.h"

#include <map>
#include <string>
#include <vector>

namespace transpiler {
namespace relay_protocol {
namespace analysis {

enum class RelaySymbolOriginKind : uint8_t
{
	Parameter,
	CurrentScopeKey,
	State,
	Local,
	LoopVariable,
	Constant,
};

struct RelaySymbolOrigin
{
	RelaySymbolOriginKind kind = RelaySymbolOriginKind::Local;
	RelayExpressionDependency dependency;
	// Listener discovery order does not prove that a write executes on every
	// path, so all writes are joined monotonically with the prior origin.
	bool initialized = false;
};

struct RelayFunctionSymbolEnvironment
{
	std::string functionId;
	ScopeType scope = ScopeType::None;
	std::vector<std::map<std::string, RelaySymbolOrigin>> lexicalScopes;
};

// This class contains no parser or runtime state. The listener feeds it
// declarations and expression effects in source traversal order, and stores
// Analyze() results on persistent relay targets and arguments.
class RelayDependencyAnalyzer
{
public:
	RelayDependencyAnalyzer();

	void Reset();
	void RegisterStateVariable(const std::string &name);
	void RegisterConstant(const std::string &name);
	void RegisterTypeSymbol(const std::string &name);
	void RegisterPureCallSummary(
		const std::string &callee,
		const RelayExpressionDependency &summary);

	void BeginFunction(
		const std::string &functionId,
		ScopeType scope,
		const std::vector<std::string> &parameterNames);
	void EndFunction();
	bool IsInsideFunction() const;

	void PushScope();
	void PopScope();
	void DeclareCurrentScopeKey(const std::string &name);
	void DeclareLoopVariable(
		const std::string &name,
		const RelayExprIR *initializer = nullptr);
	void PromoteLoopVariable(const std::string &name);
	void DeclareLocal(
		const std::string &name,
		const RelayExprIR *initializer = nullptr);

	// Records assignments and compound assignments contained in expression.
	// Writes to locals are conservative monotone joins after initialization.
	void RecordExpressionEffects(const RelayExprIR &expression);

	RelayExpressionDependency Analyze(const RelayExprIR &expression) const;

	const std::map<std::string, RelayFunctionSymbolEnvironment> &
	GetFunctionEnvironments() const
	{
		return m_completedEnvironments;
	}

	static RelayExpressionDependency FromClass(
		RelayDependencyClass dependencyClass,
		const std::string &reason = std::string());
	static RelayExpressionDependency Union(
		const RelayExpressionDependency &left,
		const RelayExpressionDependency &right);

private:
	RelayExpressionDependency AnalyzeCall(const RelayExprIR &expression) const;
	RelayExpressionDependency AnalyzeChildren(
		const std::vector<RelayExprIR> &children,
		size_t begin = 0,
		size_t end = static_cast<size_t>(-1)) const;
	const RelaySymbolOrigin *FindSymbol(const std::string &name) const;
	RelaySymbolOrigin *FindMutableLocal(const std::string &name);
	void AssignLocal(
		const std::string &name,
		const RelayExpressionDependency &dependency,
		bool compound);
	void TaintMutableBindings(
		const RelayExpressionDependency &dependency);
	void RecordUnsummarizedCallEffects(
		const RelayExprIR &expression);
	void InstallBuiltinPureSummaries();

	std::map<std::string, RelaySymbolOrigin> m_contractSymbols;
	std::map<std::string, RelayExpressionDependency> m_pureCallSummaries;
	std::map<std::string, bool> m_typeSymbols;
	RelayFunctionSymbolEnvironment m_currentEnvironment;
	std::map<std::string, RelayFunctionSymbolEnvironment>
		m_completedEnvironments;
	bool m_insideFunction = false;
};

} // namespace analysis
} // namespace relay_protocol
} // namespace transpiler
