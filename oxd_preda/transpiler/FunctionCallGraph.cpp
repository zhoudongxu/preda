#include "FunctionCallGraph.h"

#include "antlr4-runtime.h"

#include <utility>

namespace {

FunctionCallSite MakeCallSite(antlr4::ParserRuleContext *context)
{
	FunctionCallSite result;
	if (context == nullptr || context->start == nullptr)
		return result;
	result.line = static_cast<uint32_t>(context->start->getLine());
	result.column = static_cast<uint32_t>(
		context->start->getCharPositionInLine());
	result.startOffset = static_cast<int64_t>(
		context->start->getStartIndex());
	result.text = context->getText();
	if (context->stop != nullptr)
	{
		result.endLine = static_cast<uint32_t>(context->stop->getLine());
		result.endColumn = static_cast<uint32_t>(
			context->stop->getCharPositionInLine() +
			context->stop->getText().size());
		result.endOffset = static_cast<int64_t>(
			context->stop->getStopIndex());
	}
	else
	{
		result.endLine = result.line;
		result.endColumn = result.column;
		result.endOffset = result.startOffset;
	}
	return result;
}

bool SameFunctionRef(
	const transpiler::FunctionRef &left,
	const transpiler::FunctionRef &right)
{
	return left.functionIdentifier == right.functionIdentifier &&
		left.overloadIndex == right.overloadIndex;
}

} // namespace

void FunctionCallGraph::RecordCall(
	const transpiler::FunctionRef &caller,
	const transpiler::FunctionRef &callee,
	antlr4::ParserRuleContext *callContext,
	FunctionCallOrigin origin)
{
	ResolvedFunctionCall event;
	event.caller = caller;
	event.callee = callee;
	event.origin = origin;
	event.callSite = MakeCallSite(callContext);
	event.calleeResolved =
		callee.functionIdentifier != nullptr &&
		callee.GetSignature() != nullptr;

	for (const ResolvedFunctionCall &existing : m_resolvedCalls)
	{
		if (SameFunctionRef(existing.caller, event.caller) &&
			SameFunctionRef(existing.callee, event.callee) &&
			existing.origin == event.origin &&
			existing.callSite.startOffset == event.callSite.startOffset &&
			existing.callSite.endOffset == event.callSite.endOffset)
		{
			return;
		}
	}
	m_resolvedCalls.push_back(std::move(event));

	transpiler::FunctionRef mutableCallee = callee;
	transpiler::FunctionRef mutableCaller = caller;
	transpiler::FunctionSignature *calleeSignature =
		mutableCallee.GetSignature();
	transpiler::FunctionSignature *callerSignature =
		mutableCaller.GetSignature();
	if (calleeSignature != nullptr && callerSignature != nullptr)
	{
		auto inserted =
			m_functionCallerSets.try_emplace(calleeSignature).first;
		inserted->second.insert(callerSignature);
	}
}

void FunctionCallGraph::RecordIdentifierUse(
	const transpiler::FunctionRef &function,
	const transpiler::DefinedIdentifierPtr &identifier,
	const transpiler::ConcreteTypePtr &outerType,
	antlr4::ParserRuleContext *sourceContext)
{
	if (function.GetSignature() == nullptr || identifier == nullptr)
		return;
	ResolvedIdentifierUse event;
	event.function = function;
	event.identifier = identifier;
	event.outerType = outerType;
	event.sourceSite = MakeCallSite(sourceContext);
	for (const ResolvedIdentifierUse &existing : m_identifierUses)
	{
		if (SameFunctionRef(existing.function, event.function) &&
			existing.identifier == event.identifier &&
			existing.sourceSite.startOffset == event.sourceSite.startOffset &&
			existing.sourceSite.endOffset == event.sourceSite.endOffset)
		{
			return;
		}
	}
	m_identifierUses.push_back(std::move(event));
}
