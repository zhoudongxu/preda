#include "RelayRefinementSymbolTable.h"

#include "../RelayProtocolIR.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

namespace transpiler {
namespace relay_protocol {
namespace refinement {
namespace {

analysis::RelayExpressionDependency DependencyFor(
	analysis::RelayDependencyClass dependencyClass,
	analysis::RelayAvailabilityStage availability)
{
	analysis::RelayExpressionDependency result;
	result.classes.push_back(dependencyClass);
	result.earliestAvailability = availability;
	result.admissionTimeEvaluable =
		availability <= analysis::RelayAvailabilityStage::AdmissionTime;
	return result;
}

const char *KindToken(RelayRefinementSymbolKind kind)
{
	switch (kind)
	{
	case RelayRefinementSymbolKind::SourceFunctionParameter:
		return "parameter";
	case RelayRefinementSymbolKind::CurrentScopeKey:
		return "current_scope_key";
	case RelayRefinementSymbolKind::PreStateVariable:
		return "pre_state";
	case RelayRefinementSymbolKind::LoopVariable:
		return "loop_variable";
	case RelayRefinementSymbolKind::RelayEmission:
		return "relay_emitted";
	case RelayRefinementSymbolKind::ActualRelayTarget:
		return "actual_target";
	case RelayRefinementSymbolKind::ActualRelayArgument:
		return "actual_argument";
	case RelayRefinementSymbolKind::DirectRelayCount:
		return "direct_relay_count";
	default:
		return "unknown";
	}
}

std::string EscapeIdComponent(const std::string &value)
{
	static const char hex[] = "0123456789ABCDEF";
	std::string result;
	result.reserve(value.size());
	for (unsigned char character : value)
	{
		if (std::isalnum(character) ||
			character == '_' ||
			character == '-' ||
			character == '.')
		{
			result.push_back(static_cast<char>(character));
		}
		else
		{
			result.push_back('%');
			result.push_back(hex[(character >> 4) & 0xf]);
			result.push_back(hex[character & 0xf]);
		}
	}
	return result;
}

std::string ScopeSourceType(ScopeType scope)
{
	switch (scope)
	{
	case ScopeType::Address:
		return "address";
	case ScopeType::Uint32:
		return "uint32";
	case ScopeType::Uint64:
		return "uint64";
	case ScopeType::Uint96:
		return "uint96";
	case ScopeType::Uint128:
		return "uint128";
	case ScopeType::Uint160:
		return "uint160";
	case ScopeType::Uint256:
		return "uint256";
	case ScopeType::Uint512:
		return "uint512";
	default:
		return std::string();
	}
}

bool IsCurrentScopeExpression(const RelayExprIR &expression)
{
	return (expression.kind == RelayExprKind::Keyword &&
			expression.text == "next") ||
		(expression.kind == RelayExprKind::Call &&
			expression.text == "__transaction.get_self_address()");
}

} // namespace

void RelayRefinementSymbolTable::Reset()
{
	m_symbols.clear();
	m_symbolIndexes.clear();
	m_valueScopes.clear();
}

std::string RelayRefinementSymbolTable::StableId(
	RelayRefinementSymbolKind kind,
	const std::string &functionId,
	const std::string &sourceName,
	const std::string &relaySiteId,
	int64_t argumentIndex)
{
	std::string result = "rpreda.symbol.";
	result += KindToken(kind);
	result += ".fn.";
	result += EscapeIdComponent(functionId);
	if (!sourceName.empty())
	{
		result += ".name.";
		result += EscapeIdComponent(sourceName);
	}
	if (!relaySiteId.empty())
	{
		result += ".site.";
		result += EscapeIdComponent(relaySiteId);
	}
	if (argumentIndex >= 0)
	{
		result += ".arg.";
		result += std::to_string(argumentIndex);
	}
	return result;
}

const RelayRefinementSymbol &RelayRefinementSymbolTable::Ensure(
	RelayRefinementSymbol symbol)
{
	const auto found = m_symbolIndexes.find(symbol.id);
	if (found != m_symbolIndexes.end())
	{
		RelayRefinementSymbol &existing = m_symbols[found->second];
		if (existing.sourceType.empty())
			existing.sourceType = symbol.sourceType;
		if (!existing.sort.IsKnown())
			existing.sort = symbol.sort;
		if (!existing.location.IsValid() && symbol.location.IsValid())
			existing.location = symbol.location;
		if (existing.dependency.classes.empty() &&
			!symbol.dependency.classes.empty())
		{
			existing.dependency = symbol.dependency;
			existing.availability = symbol.availability;
		}
		return existing;
	}
	m_symbolIndexes[symbol.id] = m_symbols.size();
	m_symbols.push_back(std::move(symbol));
	return m_symbols.back();
}

const RelayRefinementSymbol &
RelayRefinementSymbolTable::EnsureParameter(
	const std::string &functionId,
	const std::string &name,
	const std::string &sourceType,
	const analysis::RelayExpressionDependency &dependency,
	const SourceLocation &location)
{
	RelayRefinementSymbol symbol;
	symbol.kind = RelayRefinementSymbolKind::SourceFunctionParameter;
	symbol.id = StableId(symbol.kind, functionId, name);
	symbol.sourceFunctionId = functionId;
	symbol.sourceName = name;
	symbol.sourceType = sourceType;
	symbol.sort = RelayFormulaBuilder::SortFromPredaType(sourceType);
	symbol.dependency = dependency;
	symbol.availability = dependency.earliestAvailability;
	symbol.location = location;
	const RelayRefinementSymbol &ensured = Ensure(std::move(symbol));
	BindSymbolValue(
		functionId,
		name,
		ensured.id,
		sourceType,
		true,
		location);
	return ensured;
}

const RelayRefinementSymbol &
RelayRefinementSymbolTable::EnsureCurrentScopeKey(
	const std::string &functionId,
	const std::string &sourceType,
	const analysis::RelayExpressionDependency &dependency,
	const SourceLocation &location)
{
	RelayRefinementSymbol symbol;
	symbol.kind = RelayRefinementSymbolKind::CurrentScopeKey;
	symbol.id = StableId(symbol.kind, functionId, "$current_scope_key");
	symbol.sourceFunctionId = functionId;
	symbol.sourceName = "$current_scope_key";
	symbol.sourceType = sourceType;
	symbol.sort = RelayFormulaBuilder::SortFromPredaType(sourceType);
	symbol.dependency = dependency;
	symbol.availability = dependency.earliestAvailability;
	symbol.location = location;
	const RelayRefinementSymbol &ensured = Ensure(std::move(symbol));
	BindSymbolValue(
		functionId,
		"$current_scope_key",
		ensured.id,
		sourceType,
		false,
		location);
	return ensured;
}

const RelayRefinementSymbol &
RelayRefinementSymbolTable::EnsurePreState(
	const std::string &functionId,
	const std::string &name,
	const std::string &sourceType,
	const analysis::RelayExpressionDependency &dependency,
	const SourceLocation &location)
{
	RelayRefinementSymbol symbol;
	symbol.kind = RelayRefinementSymbolKind::PreStateVariable;
	symbol.id = StableId(symbol.kind, functionId, name);
	symbol.sourceFunctionId = functionId;
	symbol.sourceName = name;
	symbol.sourceType = sourceType;
	symbol.sort = RelayFormulaBuilder::SortFromPredaType(sourceType);
	symbol.dependency = dependency;
	symbol.availability = dependency.earliestAvailability;
	symbol.location = location;
	const RelayRefinementSymbol &ensured = Ensure(std::move(symbol));
	BindSymbolValue(
		functionId,
		name,
		ensured.id,
		sourceType,
		true,
		location);
	return ensured;
}

const RelayRefinementSymbol &
RelayRefinementSymbolTable::EnsureLoopVariable(
	const std::string &functionId,
	const std::string &name,
	const std::string &sourceType,
	const analysis::RelayExpressionDependency &dependency,
	const SourceLocation &location)
{
	const RelayRefinementValueBinding *priorBinding =
		FindBinding(functionId, name);
	const std::string resolvedSourceType =
		!sourceType.empty()
			? sourceType
			: (priorBinding == nullptr
				? std::string()
				: priorBinding->sourceType);
	RelayRefinementSymbol symbol;
	symbol.kind = RelayRefinementSymbolKind::LoopVariable;
	// Lexically distinct nested loops may reuse the same source name. Include
	// the declaration offset when available so their owning symbols cannot
	// alias merely because the display name matches.
	const std::string stableSourceName =
		location.startOffset >= 0
			? name + "@" + std::to_string(location.startOffset)
			: name;
	symbol.id = StableId(
		symbol.kind,
		functionId,
		stableSourceName);
	symbol.sourceFunctionId = functionId;
	symbol.sourceName = name;
	symbol.sourceType = resolvedSourceType;
	symbol.sort =
		RelayFormulaBuilder::SortFromPredaType(resolvedSourceType);
	symbol.dependency = dependency;
	symbol.availability = dependency.earliestAvailability;
	symbol.location = location;
	const RelayRefinementSymbol &ensured = Ensure(std::move(symbol));
	BindSymbolValue(
		functionId,
		name,
		ensured.id,
		resolvedSourceType,
		true,
		location);
	return ensured;
}

const RelayRefinementSymbol &
RelayRefinementSymbolTable::EnsureRelayEmission(
	const std::string &functionId,
	const std::string &relaySiteId,
	const SourceLocation &location)
{
	RelayRefinementSymbol symbol;
	symbol.kind = RelayRefinementSymbolKind::RelayEmission;
	symbol.id = StableId(
		symbol.kind,
		functionId,
		std::string(),
		relaySiteId);
	symbol.sourceFunctionId = functionId;
	symbol.sourceName = "emitted";
	symbol.sourceType = "bool";
	symbol.sort = FormulaSort::Bool();
	symbol.dependency = DependencyFor(
		analysis::RelayDependencyClass::LocalDerived,
		analysis::RelayAvailabilityStage::DuringExecution);
	symbol.availability = symbol.dependency.earliestAvailability;
	symbol.location = location;
	symbol.relaySiteId = relaySiteId;
	return Ensure(std::move(symbol));
}

const RelayRefinementSymbol &
RelayRefinementSymbolTable::EnsureActualTarget(
	const std::string &functionId,
	const std::string &relaySiteId,
	const std::string &sourceType,
	const analysis::RelayExpressionDependency &dependency,
	const SourceLocation &location)
{
	RelayRefinementSymbol symbol;
	symbol.kind = RelayRefinementSymbolKind::ActualRelayTarget;
	symbol.id = StableId(
		symbol.kind,
		functionId,
		std::string(),
		relaySiteId);
	symbol.sourceFunctionId = functionId;
	symbol.sourceName = "actual_target";
	symbol.sourceType = sourceType;
	symbol.sort = RelayFormulaBuilder::SortFromPredaType(sourceType);
	symbol.dependency = dependency;
	symbol.availability = dependency.earliestAvailability;
	symbol.location = location;
	symbol.relaySiteId = relaySiteId;
	return Ensure(std::move(symbol));
}

const RelayRefinementSymbol &
RelayRefinementSymbolTable::EnsureActualArgument(
	const std::string &functionId,
	const std::string &relaySiteId,
	size_t argumentIndex,
	const std::string &sourceType,
	const analysis::RelayExpressionDependency &dependency,
	const SourceLocation &location)
{
	RelayRefinementSymbol symbol;
	symbol.kind = RelayRefinementSymbolKind::ActualRelayArgument;
	symbol.id = StableId(
		symbol.kind,
		functionId,
		std::string(),
		relaySiteId,
		static_cast<int64_t>(argumentIndex));
	symbol.sourceFunctionId = functionId;
	symbol.sourceName =
		"actual_arg_" + std::to_string(argumentIndex);
	symbol.sourceType = sourceType;
	symbol.sort = RelayFormulaBuilder::SortFromPredaType(sourceType);
	symbol.dependency = dependency;
	symbol.availability = dependency.earliestAvailability;
	symbol.location = location;
	symbol.relaySiteId = relaySiteId;
	symbol.argumentIndex = static_cast<int64_t>(argumentIndex);
	return Ensure(std::move(symbol));
}

const RelayRefinementSymbol &
RelayRefinementSymbolTable::EnsureDirectRelayCount(
	const std::string &functionId,
	const SourceLocation &location)
{
	RelayRefinementSymbol symbol;
	symbol.kind = RelayRefinementSymbolKind::DirectRelayCount;
	symbol.id = StableId(symbol.kind, functionId);
	symbol.sourceFunctionId = functionId;
	symbol.sourceName = "direct_relay_count";
	symbol.sourceType = "mathematical_int";
	symbol.sort = FormulaSort::Int();
	symbol.dependency = DependencyFor(
		analysis::RelayDependencyClass::LocalDerived,
		analysis::RelayAvailabilityStage::DuringExecution);
	symbol.availability = symbol.dependency.earliestAvailability;
	symbol.location = location;
	return Ensure(std::move(symbol));
}

const RelayRefinementSymbol &
RelayRefinementSymbolTable::EnsureRelayEmission(
	const RelaySite &site)
{
	return EnsureRelayEmission(
		site.sourceFunctionId,
		site.id,
		site.location);
}

const RelayRefinementSymbol &
RelayRefinementSymbolTable::EnsureActualTarget(
	const RelaySite &site)
{
	std::string sourceType = site.target.type;
	if (sourceType.empty())
		sourceType = ScopeSourceType(site.targetScope);
	return EnsureActualTarget(
		site.sourceFunctionId,
		site.id,
		sourceType,
		site.targetDependency,
		site.location);
}

const RelayRefinementSymbol &
RelayRefinementSymbolTable::EnsureActualArgument(
	const RelaySite &site,
	size_t argumentIndex)
{
	if (argumentIndex >= site.arguments.size())
	{
		// A caller bug should not produce an unstable or aliased symbol. The
		// explicit index is retained with Unknown metadata for diagnostics.
		return EnsureActualArgument(
			site.sourceFunctionId,
			site.id,
			argumentIndex,
			std::string(),
			analysis::RelayExpressionDependency(),
			site.location);
	}
	const RelayArgument &argument = site.arguments[argumentIndex];
	return EnsureActualArgument(
		site.sourceFunctionId,
		site.id,
		argumentIndex,
		argument.type,
		argument.dependency,
		argument.expression.location);
}

const RelayRefinementSymbol &
RelayRefinementSymbolTable::EnsureDirectRelayCount(
	const FunctionProtocol &function)
{
	return EnsureDirectRelayCount(
		function.sourceFunctionId,
		function.root.location);
}

void RelayRefinementSymbolTable::BeginFunctionValues(
	const std::string &functionId)
{
	m_valueScopes[functionId].clear();
	m_valueScopes[functionId].emplace_back();
}

void RelayRefinementSymbolTable::EndFunctionValues(
	const std::string &functionId)
{
	auto found = m_valueScopes.find(functionId);
	if (found == m_valueScopes.end())
		return;
	while (found->second.size() > 1)
		found->second.pop_back();
}

void RelayRefinementSymbolTable::PushScope(
	const std::string &functionId)
{
	auto &scopes = m_valueScopes[functionId];
	if (scopes.empty())
		scopes.emplace_back();
	scopes.emplace_back();
}

void RelayRefinementSymbolTable::PopScope(
	const std::string &functionId)
{
	auto found = m_valueScopes.find(functionId);
	if (found != m_valueScopes.end() &&
		found->second.size() > 1)
	{
		found->second.pop_back();
	}
}

void RelayRefinementSymbolTable::BindSymbolValue(
	const std::string &functionId,
	const std::string &name,
	const std::string &symbolId,
	const std::string &sourceType,
	bool mutableValue,
	const SourceLocation &location)
{
	auto &scopes = m_valueScopes[functionId];
	if (scopes.empty())
		scopes.emplace_back();
	RelayRefinementValueBinding binding;
	binding.sourceName = name;
	binding.sourceType = sourceType;
	binding.symbolId = symbolId;
	binding.mutableValue = mutableValue;
	binding.location = location;
	scopes.back()[name] = std::move(binding);
}

void RelayRefinementSymbolTable::SetLocalFormula(
	const std::string &functionId,
	const std::string &name,
	const std::string &sourceType,
	const FormulaExpr &formula,
	const SourceLocation &location)
{
	auto &scopes = m_valueScopes[functionId];
	if (scopes.empty())
		scopes.emplace_back();
	RelayRefinementValueBinding binding;
	binding.sourceName = name;
	binding.sourceType = sourceType;
	binding.formula = formula;
	binding.hasFormula = true;
	binding.mutableValue = true;
	binding.location = location;
	scopes.back()[name] = std::move(binding);
}

RelayRefinementValueBinding *
RelayRefinementSymbolTable::FindMutableBinding(
	const std::string &functionId,
	const std::string &name)
{
	auto found = m_valueScopes.find(functionId);
	if (found == m_valueScopes.end())
		return nullptr;
	for (auto scope = found->second.rbegin();
		scope != found->second.rend();
		++scope)
	{
		auto binding = scope->find(name);
		if (binding != scope->end())
			return &binding->second;
	}
	return nullptr;
}

const RelayRefinementValueBinding *
RelayRefinementSymbolTable::FindBinding(
	const std::string &functionId,
	const std::string &name) const
{
	const auto found = m_valueScopes.find(functionId);
	if (found == m_valueScopes.end())
		return nullptr;
	for (auto scope = found->second.rbegin();
		scope != found->second.rend();
		++scope)
	{
		const auto binding = scope->find(name);
		if (binding != scope->end())
			return &binding->second;
	}
	return nullptr;
}

void RelayRefinementSymbolTable::InvalidateValue(
	const std::string &functionId,
	const std::string &name,
	const std::string &reason,
	const SourceLocation &location)
{
	RelayRefinementValueBinding *binding =
		FindMutableBinding(functionId, name);
	if (binding == nullptr)
	{
		auto &scopes = m_valueScopes[functionId];
		if (scopes.empty())
			scopes.emplace_back();
		RelayRefinementValueBinding created;
		created.sourceName = name;
		created.valid = false;
		created.invalidReason = reason;
		created.location = location;
		scopes.back()[name] = std::move(created);
		return;
	}
	if (!binding->mutableValue)
		return;
	binding->valid = false;
	binding->invalidReason = reason;
	if (location.IsValid())
		binding->location = location;
}

void RelayRefinementSymbolTable::InvalidateAllMutable(
	const std::string &functionId,
	const std::string &reason,
	const SourceLocation &location)
{
	auto found = m_valueScopes.find(functionId);
	if (found == m_valueScopes.end())
		return;
	for (ValueScope &scope : found->second)
	{
		for (auto &nameAndBinding : scope)
		{
			RelayRefinementValueBinding &binding =
				nameAndBinding.second;
			if (!binding.mutableValue)
				continue;
			binding.valid = false;
			binding.invalidReason = reason;
			if (location.IsValid())
				binding.location = location;
		}
	}
}

RelayFormulaResolution
RelayRefinementSymbolTable::ResolveCurrentValue(
	const std::string &functionId,
	const RelayExprIR &expression) const
{
	const RelayRefinementValueBinding *binding = nullptr;
	if (expression.kind == RelayExprKind::Identifier)
		binding = FindBinding(functionId, expression.text);
	else if (IsCurrentScopeExpression(expression))
		binding = FindBinding(functionId, "$current_scope_key");
	else
		return RelayFormulaResolution();

	if (binding == nullptr)
		return RelayFormulaResolution();

	RelayFormulaResolution result;
	result.handled = true;
	if (!binding->valid)
	{
		result.unavailableReason = binding->invalidReason.empty()
			? "current value of '" + binding->sourceName +
				"' is unavailable"
			: binding->invalidReason;
		return result;
	}
	if (binding->hasFormula)
	{
		result.formula = &binding->formula;
		return result;
	}
	if (!binding->symbolId.empty())
	{
		result.symbol = Find(binding->symbolId);
		if (result.symbol == nullptr)
		{
			result.unavailableReason =
				"current value refers to missing symbol '" +
				binding->symbolId + "'";
		}
		return result;
	}
	result.unavailableReason =
		"current value of '" + binding->sourceName +
			"' has no formula or entry symbol";
	return result;
}

const RelayRefinementSymbol *
RelayRefinementSymbolTable::ResolveExpressionSymbol(
	const std::string &functionId,
	const RelayExprIR &expression) const
{
	return ResolveCurrentValue(functionId, expression).symbol;
}

const FormulaExpr *
RelayRefinementSymbolTable::ResolveLocalDefinition(
	const std::string &functionId,
	const RelayExprIR &expression) const
{
	return ResolveCurrentValue(functionId, expression).formula;
}

RelayFormulaResolver RelayRefinementSymbolTable::MakeResolver(
	const std::string &functionId) const
{
	RelayFormulaResolver resolver;
	resolver.value =
		[this, functionId](const RelayExprIR &expression) {
			return ResolveCurrentValue(functionId, expression);
		};
	return resolver;
}

const RelayRefinementSymbol *RelayRefinementSymbolTable::Find(
	const std::string &symbolId) const
{
	const auto found = m_symbolIndexes.find(symbolId);
	return found == m_symbolIndexes.end()
		? nullptr
		: &m_symbols[found->second];
}

} // namespace refinement
} // namespace relay_protocol
} // namespace transpiler
