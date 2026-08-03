#pragma once

#include "PredaCFG.h"
#include "../../antlr_generated/PredaParser.h"

#include <map>
#include <string>
#include <vector>

namespace transpiler {
namespace relay_protocol {

struct RelayProtocolIR;

namespace cfg {

struct PredaCFGFunctionInput
{
	FunctionId functionId;
	std::string function;
	std::string signature;
	ScopeType scope = ScopeType::None;
	bool generatedRelayLambda = false;
	SourceLocation location;
	std::vector<PredaParser::StatementContext *> statements;
};

struct PredaCFGCallFactInput
{
	PredaCallEdge edge;
};

struct PredaCFGIdentifierUseInput
{
	FunctionId functionId;
	SourceLocation location;
	std::string name;
	std::string stateVariableId;
	ScopeType stateScope = ScopeType::None;
	bool stateVariable = false;
	bool functionSymbol = false;
};

struct PredaCFGConditionInput
{
	FunctionId functionId;
	SourceLocation location;
	RelayExprIR expression;
	refinement::FormulaExpr formula;
};

struct PredaCFGBuilderInput
{
	std::vector<PredaCFGFunctionInput> functions;
	std::vector<PredaCFGCallFactInput> calls;
	std::vector<PredaCFGIdentifierUseInput> identifierUses;
	std::vector<PredaCFGConditionInput> conditions;
};

class PredaCFGBuilder
{
public:
	PredaControlFlowIR Build(
		const PredaCFGBuilderInput &input,
		const RelayProtocolIR &protocol) const;
};

} // namespace cfg
} // namespace relay_protocol
} // namespace transpiler
