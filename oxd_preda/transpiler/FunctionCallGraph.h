#pragma once

#include "transpiler/BaseTranspiler.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace antlr4 {
class ParserRuleContext;
}

enum class FunctionCallOrigin : uint8_t
{
	Ordinary,
	TypeConstructor,
	RuntimeHelper,
	External,
};

struct FunctionCallSite
{
	uint32_t line = 0;
	uint32_t column = 0;
	uint32_t endLine = 0;
	uint32_t endColumn = 0;
	int64_t startOffset = -1;
	int64_t endOffset = -1;
	std::string text;
};

struct ResolvedFunctionCall
{
	transpiler::FunctionRef caller;
	transpiler::FunctionRef callee;
	FunctionCallOrigin origin = FunctionCallOrigin::Ordinary;
	FunctionCallSite callSite;
	bool calleeResolved = false;
};

struct ResolvedIdentifierUse
{
	transpiler::FunctionRef function;
	transpiler::DefinedIdentifierPtr identifier;
	transpiler::ConcreteTypePtr outerType;
	FunctionCallSite sourceSite;
};

class FunctionCallGraph
{
public:
	std::map<transpiler::FunctionSignature *, std::set<transpiler::FunctionSignature *>> m_functionCallerSets;
	std::vector<ResolvedFunctionCall> m_resolvedCalls;
	std::vector<ResolvedIdentifierUse> m_identifierUses;

	void RecordCall(
		const transpiler::FunctionRef &caller,
		const transpiler::FunctionRef &callee,
		antlr4::ParserRuleContext *callContext,
		FunctionCallOrigin origin);
	void RecordIdentifierUse(
		const transpiler::FunctionRef &function,
		const transpiler::DefinedIdentifierPtr &identifier,
		const transpiler::ConcreteTypePtr &outerType,
		antlr4::ParserRuleContext *sourceContext);
};
