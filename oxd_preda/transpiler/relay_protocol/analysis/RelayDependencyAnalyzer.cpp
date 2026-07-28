#include "RelayDependencyAnalyzer.h"

#include <algorithm>
#include <utility>

namespace transpiler {
namespace relay_protocol {
namespace analysis {
namespace {

RelayAvailabilityStage AvailabilityForClass(
	RelayDependencyClass dependencyClass)
{
	switch (dependencyClass)
	{
	case RelayDependencyClass::Constant:
		return RelayAvailabilityStage::CompileTime;
	case RelayDependencyClass::TransactionArgument:
	case RelayDependencyClass::CurrentScopeKey:
		return RelayAvailabilityStage::AdmissionTime;
	case RelayDependencyClass::CurrentScopeState:
		return RelayAvailabilityStage::AfterScopeLoad;
	case RelayDependencyClass::LocalDerived:
	case RelayDependencyClass::LoopVariable:
	case RelayDependencyClass::ExternalCallResult:
		return RelayAvailabilityStage::DuringExecution;
	case RelayDependencyClass::Opaque:
	default:
		return RelayAvailabilityStage::Unknown;
	}
}

void Normalize(RelayExpressionDependency &dependency)
{
	std::sort(dependency.classes.begin(), dependency.classes.end());
	dependency.classes.erase(
		std::unique(
			dependency.classes.begin(),
			dependency.classes.end()),
		dependency.classes.end());

	// Constants do not make a dynamic expression depend on an additional
	// runtime origin. This keeps `x * 65536 + y` classified as a transaction
	// argument expression rather than Constant + TransactionArgument.
	if (dependency.classes.size() > 1)
	{
		dependency.classes.erase(
			std::remove(
				dependency.classes.begin(),
				dependency.classes.end(),
				RelayDependencyClass::Constant),
			dependency.classes.end());
	}

	if (dependency.classes.empty())
	{
		dependency.classes.push_back(RelayDependencyClass::Opaque);
		if (dependency.reason.empty())
			dependency.reason = "expression has no classified dependency";
	}

	dependency.earliestAvailability = RelayAvailabilityStage::CompileTime;
	for (RelayDependencyClass dependencyClass : dependency.classes)
	{
		dependency.earliestAvailability = std::max(
			dependency.earliestAvailability,
			AvailabilityForClass(dependencyClass));
	}
	dependency.admissionTimeEvaluable =
		dependency.earliestAvailability <=
		RelayAvailabilityStage::AdmissionTime;
}

bool IsAssignmentOperator(const std::string &op)
{
	return op == "=" ||
		op == "+=" ||
		op == "-=" ||
		op == "*=" ||
		op == "/=" ||
		op == "%=" ||
		op == "<<=" ||
		op == ">>=" ||
		op == "&=" ||
		op == "^=" ||
		op == "|=";
}

bool IsIncrementOrDecrement(const std::string &op)
{
	return op == "++" || op == "--";
}

const RelayExprIR &StripLValueGroups(
	const RelayExprIR &expression)
{
	const RelayExprIR *current = &expression;
	while (current->kind == RelayExprKind::Group &&
		current->children.size() == 1)
	{
		current = &current->children.front();
	}
	return *current;
}

std::string RootIdentifier(const RelayExprIR &expression)
{
	switch (expression.kind)
	{
	case RelayExprKind::Identifier:
		return expression.text;
	case RelayExprKind::Group:
	case RelayExprKind::MemberAccess:
	case RelayExprKind::Index:
		return expression.children.empty()
			? std::string()
			: RootIdentifier(expression.children.front());
	default:
		return std::string();
	}
}

} // namespace

RelayDependencyAnalyzer::RelayDependencyAnalyzer()
{
	Reset();
}

void RelayDependencyAnalyzer::Reset()
{
	m_contractSymbols.clear();
	m_pureCallSummaries.clear();
	m_typeSymbols.clear();
	m_currentEnvironment = RelayFunctionSymbolEnvironment();
	m_completedEnvironments.clear();
	m_insideFunction = false;
	InstallBuiltinPureSummaries();
}

void RelayDependencyAnalyzer::InstallBuiltinPureSummaries()
{
	// PREDA's address-scope API is backed by
	// ExecutionContext::GetScopeTarget(), so the target key is available from
	// the admitted invocation before contract scope state is loaded.
	RegisterPureCallSummary(
		"__transaction.get_self_address",
		FromClass(RelayDependencyClass::CurrentScopeKey));
}

RelayExpressionDependency RelayDependencyAnalyzer::FromClass(
	RelayDependencyClass dependencyClass,
	const std::string &reason)
{
	RelayExpressionDependency result;
	result.classes.push_back(dependencyClass);
	result.reason = reason;
	Normalize(result);
	return result;
}

RelayExpressionDependency RelayDependencyAnalyzer::Union(
	const RelayExpressionDependency &left,
	const RelayExpressionDependency &right)
{
	RelayExpressionDependency result;
	result.classes = left.classes;
	result.classes.insert(
		result.classes.end(),
		right.classes.begin(),
		right.classes.end());
	if (left.reason.empty())
		result.reason = right.reason;
	else if (right.reason.empty() || left.reason == right.reason)
		result.reason = left.reason;
	else
		result.reason = left.reason + "; " + right.reason;
	Normalize(result);
	return result;
}

void RelayDependencyAnalyzer::RegisterStateVariable(
	const std::string &name)
{
	RelaySymbolOrigin symbol;
	symbol.kind = RelaySymbolOriginKind::State;
	symbol.dependency =
		FromClass(RelayDependencyClass::CurrentScopeState);
	symbol.initialized = true;
	m_contractSymbols[name] = std::move(symbol);
}

void RelayDependencyAnalyzer::RegisterConstant(const std::string &name)
{
	RelaySymbolOrigin symbol;
	symbol.kind = RelaySymbolOriginKind::Constant;
	symbol.dependency = FromClass(RelayDependencyClass::Constant);
	symbol.initialized = true;
	m_contractSymbols[name] = std::move(symbol);
}

void RelayDependencyAnalyzer::RegisterTypeSymbol(const std::string &name)
{
	m_typeSymbols[name] = true;
}

void RelayDependencyAnalyzer::RegisterPureCallSummary(
	const std::string &callee,
	const RelayExpressionDependency &summary)
{
	RelayExpressionDependency normalized = summary;
	Normalize(normalized);
	m_pureCallSummaries[callee] = std::move(normalized);
}

void RelayDependencyAnalyzer::BeginFunction(
	const std::string &functionId,
	ScopeType scope,
	const std::vector<std::string> &parameterNames)
{
	if (m_insideFunction)
		EndFunction();

	m_currentEnvironment = RelayFunctionSymbolEnvironment();
	m_currentEnvironment.functionId = functionId;
	m_currentEnvironment.scope = scope;
	m_currentEnvironment.lexicalScopes.emplace_back();
	m_insideFunction = true;
	for (const auto &nameAndSymbol : m_contractSymbols)
	{
		m_currentEnvironment.lexicalScopes.back().emplace(
			nameAndSymbol.first,
			nameAndSymbol.second);
	}
	for (const std::string &parameterName : parameterNames)
	{
		RelaySymbolOrigin symbol;
		symbol.kind = RelaySymbolOriginKind::Parameter;
		symbol.dependency =
			FromClass(RelayDependencyClass::TransactionArgument);
		symbol.initialized = true;
		m_currentEnvironment.lexicalScopes.back()[parameterName] =
			std::move(symbol);
	}
	if (scope != ScopeType::None &&
		scope != ScopeType::Global &&
		scope != ScopeType::Shard)
	{
		// PREDA only exposes an address getter today, but all keyed function
		// environments still carry the semantic current target binding.
		DeclareCurrentScopeKey("$current_scope_key");
	}
}

void RelayDependencyAnalyzer::EndFunction()
{
	if (!m_insideFunction)
		return;
	m_completedEnvironments[m_currentEnvironment.functionId] =
		m_currentEnvironment;
	m_currentEnvironment = RelayFunctionSymbolEnvironment();
	m_insideFunction = false;
}

bool RelayDependencyAnalyzer::IsInsideFunction() const
{
	return m_insideFunction;
}

void RelayDependencyAnalyzer::PushScope()
{
	if (m_insideFunction)
		m_currentEnvironment.lexicalScopes.emplace_back();
}

void RelayDependencyAnalyzer::PopScope()
{
	if (m_insideFunction &&
		m_currentEnvironment.lexicalScopes.size() > 1)
	{
		m_currentEnvironment.lexicalScopes.pop_back();
	}
}

void RelayDependencyAnalyzer::DeclareCurrentScopeKey(
	const std::string &name)
{
	if (!m_insideFunction)
		return;
	if (m_currentEnvironment.lexicalScopes.empty())
		m_currentEnvironment.lexicalScopes.emplace_back();
	RelaySymbolOrigin symbol;
	symbol.kind = RelaySymbolOriginKind::CurrentScopeKey;
	symbol.dependency =
		FromClass(RelayDependencyClass::CurrentScopeKey);
	symbol.initialized = true;
	m_currentEnvironment.lexicalScopes.back()[name] =
		std::move(symbol);
}

void RelayDependencyAnalyzer::DeclareLoopVariable(
	const std::string &name,
	const RelayExprIR *initializer)
{
	if (!m_insideFunction)
		return;
	if (m_currentEnvironment.lexicalScopes.empty())
		m_currentEnvironment.lexicalScopes.emplace_back();
	RelaySymbolOrigin symbol;
	symbol.kind = RelaySymbolOriginKind::LoopVariable;
	symbol.dependency =
		FromClass(RelayDependencyClass::LoopVariable);
	if (initializer != nullptr)
	{
		symbol.dependency = Union(
			symbol.dependency,
			Analyze(*initializer));
	}
	symbol.initialized = true;
	m_currentEnvironment.lexicalScopes.back()[name] =
		std::move(symbol);
}

void RelayDependencyAnalyzer::PromoteLoopVariable(
	const std::string &name)
{
	RelaySymbolOrigin *symbol = FindMutableLocal(name);
	if (symbol == nullptr)
		return;
	symbol->kind = RelaySymbolOriginKind::LoopVariable;
	symbol->dependency = Union(
		symbol->dependency,
		FromClass(RelayDependencyClass::LoopVariable));
	symbol->initialized = true;
}

void RelayDependencyAnalyzer::DeclareLocal(
	const std::string &name,
	const RelayExprIR *initializer)
{
	if (!m_insideFunction)
		return;
	if (m_currentEnvironment.lexicalScopes.empty())
		m_currentEnvironment.lexicalScopes.emplace_back();
	RelaySymbolOrigin symbol;
	symbol.kind = RelaySymbolOriginKind::Local;
	if (initializer == nullptr)
	{
		symbol.dependency = FromClass(
			RelayDependencyClass::LocalDerived,
			"default-initialized local value");
		symbol.initialized = false;
	}
	else
	{
		symbol.dependency = Analyze(*initializer);
		symbol.initialized = true;
	}
	m_currentEnvironment.lexicalScopes.back()[name] =
		std::move(symbol);
}

const RelaySymbolOrigin *RelayDependencyAnalyzer::FindSymbol(
	const std::string &name) const
{
	if (m_insideFunction)
	{
		for (auto scope =
				m_currentEnvironment.lexicalScopes.rbegin();
			scope != m_currentEnvironment.lexicalScopes.rend();
			++scope)
		{
			const auto symbol = scope->find(name);
			if (symbol != scope->end())
				return &symbol->second;
		}
	}
	const auto contractSymbol = m_contractSymbols.find(name);
	return contractSymbol == m_contractSymbols.end()
		? nullptr
		: &contractSymbol->second;
}

RelaySymbolOrigin *RelayDependencyAnalyzer::FindMutableLocal(
	const std::string &name)
{
	if (!m_insideFunction)
		return nullptr;
	for (auto scope = m_currentEnvironment.lexicalScopes.rbegin();
		scope != m_currentEnvironment.lexicalScopes.rend();
		++scope)
	{
		auto symbol = scope->find(name);
		if (symbol != scope->end() &&
			(symbol->second.kind == RelaySymbolOriginKind::Parameter ||
				symbol->second.kind == RelaySymbolOriginKind::State ||
				symbol->second.kind == RelaySymbolOriginKind::Local ||
				symbol->second.kind ==
					RelaySymbolOriginKind::LoopVariable))
		{
			return &symbol->second;
		}
	}
	return nullptr;
}

RelayExpressionDependency RelayDependencyAnalyzer::AnalyzeChildren(
	const std::vector<RelayExprIR> &children,
	size_t begin,
	size_t end) const
{
	if (end == static_cast<size_t>(-1) || end > children.size())
		end = children.size();
	if (begin >= end)
		return FromClass(RelayDependencyClass::Constant);

	RelayExpressionDependency result = Analyze(children[begin]);
	for (size_t i = begin + 1; i < end; ++i)
		result = Union(result, Analyze(children[i]));
	return result;
}

RelayExpressionDependency RelayDependencyAnalyzer::AnalyzeCall(
	const RelayExprIR &expression) const
{
	if (expression.children.empty())
	{
		return FromClass(
			RelayDependencyClass::ExternalCallResult,
			"call expression has no persistent callee");
	}

	const RelayExprIR &callee = expression.children.front();
	const auto pureSummary = m_pureCallSummaries.find(callee.text);
	if (pureSummary != m_pureCallSummaries.end())
	{
		RelayExpressionDependency result = pureSummary->second;
		for (size_t i = 1; i < expression.children.size(); ++i)
			result = Union(result, Analyze(expression.children[i]));
		return result;
	}

	const bool isCast =
		callee.kind == RelayExprKind::Keyword ||
		m_typeSymbols.find(callee.text) != m_typeSymbols.end();
	if (isCast)
	{
		// A zero-argument value constructor is a compile-time default value;
		// otherwise a cast preserves the dependencies of its operands.
		return AnalyzeChildren(expression.children, 1);
	}

	// Without an explicit pure summary, a call result is produced during
	// execution. Its arguments do not make that result admission-time
	// evaluable and are intentionally not substituted for the result origin.
	return FromClass(
		RelayDependencyClass::ExternalCallResult,
		"function call has no registered pure dependency summary");
}

RelayExpressionDependency RelayDependencyAnalyzer::Analyze(
	const RelayExprIR &expression) const
{
	switch (expression.kind)
	{
	case RelayExprKind::Literal:
		return FromClass(RelayDependencyClass::Constant);

	case RelayExprKind::Keyword:
		if (expression.text == "next")
			return FromClass(RelayDependencyClass::CurrentScopeKey);
		return FromClass(RelayDependencyClass::Constant);

	case RelayExprKind::Identifier:
	{
		const RelaySymbolOrigin *symbol = FindSymbol(expression.text);
		if (symbol != nullptr)
			return symbol->dependency;
		if (expression.text == "__transaction" ||
			expression.text == "__block" ||
			expression.text == "__debug")
		{
			// The context object itself is a compiler-provided handle. Calls
			// on it are classified by AnalyzeCall().
			return FromClass(RelayDependencyClass::Constant);
		}
		return FromClass(
			RelayDependencyClass::Opaque,
			"identifier origin is unavailable: " + expression.text);
	}

	case RelayExprKind::MemberAccess:
		if (expression.children.empty())
		{
			return FromClass(
				RelayDependencyClass::Opaque,
				"member expression has no persistent receiver");
		}
		// BuildExpression stores the member-name token as the final Identifier
		// child. It is a constant selector; all value-bearing children are
		// unioned here.
		return AnalyzeChildren(
			expression.children,
			0,
			expression.children.size() - 1);

	case RelayExprKind::Index:
	case RelayExprKind::Unary:
	case RelayExprKind::Binary:
	case RelayExprKind::Group:
		return AnalyzeChildren(expression.children);

	case RelayExprKind::Call:
		return AnalyzeCall(expression);

	case RelayExprKind::Opaque:
	default:
		return FromClass(
			RelayDependencyClass::Opaque,
			expression.opaqueReason.empty()
				? "opaque expression"
				: expression.opaqueReason);
	}
}

void RelayDependencyAnalyzer::AssignLocal(
	const std::string &name,
	const RelayExpressionDependency &dependency,
	bool /*compound*/)
{
	RelaySymbolOrigin *symbol = FindMutableLocal(name);
	if (symbol == nullptr)
		return;

	// Listener order alone does not prove that a write executes on every path
	// (for example, it may be inside a branch or a zero-trip loop). Always
	// retain the prior origin and join the new value conservatively.
	symbol->dependency =
		Union(symbol->dependency, dependency);
	symbol->initialized = true;
}

void RelayDependencyAnalyzer::TaintMutableBindings(
	const RelayExpressionDependency &dependency)
{
	if (!m_insideFunction)
		return;

	for (std::map<std::string, RelaySymbolOrigin> &scope :
		m_currentEnvironment.lexicalScopes)
	{
		for (auto &nameAndSymbol : scope)
		{
			switch (nameAndSymbol.second.kind)
			{
			case RelaySymbolOriginKind::Parameter:
			case RelaySymbolOriginKind::State:
			case RelaySymbolOriginKind::Local:
			case RelaySymbolOriginKind::LoopVariable:
				nameAndSymbol.second.dependency = Union(
					nameAndSymbol.second.dependency,
					dependency);
				nameAndSymbol.second.initialized = true;
				break;
			case RelaySymbolOriginKind::CurrentScopeKey:
			case RelaySymbolOriginKind::Constant:
				break;
			}
		}
	}
}

void RelayDependencyAnalyzer::RecordUnsummarizedCallEffects(
	const RelayExprIR &expression)
{
	if (expression.kind != RelayExprKind::Call ||
		expression.children.empty())
	{
		return;
	}

	const RelayExprIR &callee = expression.children.front();
	if (m_pureCallSummaries.find(callee.text) !=
			m_pureCallSummaries.end() ||
		callee.kind == RelayExprKind::Keyword ||
		m_typeSymbols.find(callee.text) != m_typeSymbols.end())
	{
		return;
	}

	const RelayExpressionDependency callEffect = FromClass(
		RelayDependencyClass::ExternalCallResult,
		"unsummarized call may produce execution-time side effects");

	// An ordinary PREDA function may update contract state. Keep a
	// function-local state overlay so this conservative taint does not leak
	// into independently analyzed source functions.
	for (std::map<std::string, RelaySymbolOrigin> &scope :
		m_currentEnvironment.lexicalScopes)
	{
		for (auto &nameAndSymbol : scope)
		{
			if (nameAndSymbol.second.kind ==
				RelaySymbolOriginKind::State)
			{
				nameAndSymbol.second.dependency = Union(
					nameAndSymbol.second.dependency,
					callEffect);
			}
		}
	}

	// Without a callee effect summary, receivers and arguments may be
	// mutated through reference-like PREDA values. Taint their root bindings
	// rather than assuming a by-value, side-effect-free call.
	if (callee.kind == RelayExprKind::MemberAccess &&
		!callee.children.empty())
	{
		AssignLocal(
			RootIdentifier(callee.children.front()),
			callEffect,
			true);
	}
	for (size_t i = 1; i < expression.children.size(); ++i)
	{
		AssignLocal(
			RootIdentifier(expression.children[i]),
			callEffect,
			true);
	}
}

void RelayDependencyAnalyzer::RecordExpressionEffects(
	const RelayExprIR &expression)
{
	if (expression.kind == RelayExprKind::Opaque)
	{
		// The persistent expression IR intentionally does not invent structure
		// for unsupported syntax. Its hidden children may contain calls or
		// writes, so no mutable binding may remain optimistically available.
		TaintMutableBindings(FromClass(
			RelayDependencyClass::Opaque,
			expression.opaqueReason.empty()
				? "opaque expression may contain execution-time side effects"
				: expression.opaqueReason));
	}
	else if (expression.kind == RelayExprKind::Binary &&
		IsAssignmentOperator(expression.op) &&
		expression.children.size() >= 2)
	{
		const RelayExprIR &left = expression.children.front();
		const std::string destination =
			RootIdentifier(left);
		RelayExpressionDependency assigned =
			Analyze(expression.children[1]);
		for (size_t i = 2; i < expression.children.size(); ++i)
			assigned = Union(assigned, Analyze(expression.children[i]));
		// Index/member lvalues carry value dependencies of their receiver and
		// index expressions, so include the lvalue analysis for structured
		// destinations and compound assignments.
		if (expression.op != "=" ||
			StripLValueGroups(left).kind !=
				RelayExprKind::Identifier)
		{
			assigned = Union(assigned, Analyze(left));
		}
		AssignLocal(destination, assigned, expression.op != "=");
	}
	else if (expression.kind == RelayExprKind::Unary &&
		IsIncrementOrDecrement(expression.op) &&
		!expression.children.empty())
	{
		const RelayExprIR &operand = expression.children.front();
		AssignLocal(
			RootIdentifier(operand),
			Analyze(operand),
			true);
	}
	else if (expression.kind == RelayExprKind::Call)
	{
		RecordUnsummarizedCallEffects(expression);
	}

	for (const RelayExprIR &child : expression.children)
		RecordExpressionEffects(child);
}

} // namespace analysis
} // namespace relay_protocol
} // namespace transpiler
