#pragma once

#include "RelayProtocolIR.h"
#include "analysis/RelayDependencyAnalyzer.h"
#include "../antlr_generated/PredaParser.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace transpiler {

struct PredaTranspilerContext;

namespace relay_protocol {

struct RelayArgumentInput
{
	PredaParser::ExpressionContext *expression = nullptr;
	antlr4::ParserRuleContext *sourceContext = nullptr;
	std::string text;
	std::string type;
};

struct RelaySiteInput
{
	PredaParser::RelayStatementContext *context = nullptr;
	std::string sourceContract;
	std::string sourceFunction;
	std::string sourceFunctionId;
	std::string sourceFunctionSignature;
	uint64_t sourceFunctionOverloadIndex = 0;
	ScopeType sourceScope = ScopeType::None;
	RelayKind relayKind = RelayKind::CustomScope;
	PredaParser::ExpressionContext *targetExpression = nullptr;
	std::string targetText;
	std::string targetType;
	ScopeType targetScope = ScopeType::None;
	std::string targetFunction;
	std::string targetFunctionId;
	std::string targetFunctionSignature;
	uint64_t targetFunctionOverloadIndex = 0;
	std::vector<RelayArgumentInput> arguments;
	bool lambdaHandler = false;
	int64_t opcode = -1;
};

class RelayProtocolCollector
{
public:
	explicit RelayProtocolCollector(PredaTranspilerContext &context);

	void Reset(const std::string &dappName, const std::string &contractName);
	void RegisterStateVariable(const std::string &name);
	void RegisterConstant(const std::string &name);
	void RegisterTypeSymbol(const std::string &name);
	void BeginFunctionDependencyAnalysis(
		const std::string &functionId,
		ScopeType scope,
		const std::vector<std::string> &parameterNames);
	void EndFunctionDependencyAnalysis();
	void PushDependencyScope();
	void PopDependencyScope();
	void DeclareLocalDependency(
		const std::string &name,
		PredaParser::ExpressionContext *initializer);
	void DeclareLoopVariableDependency(
		const std::string &name,
		PredaParser::ExpressionContext *initializer);
	void PromoteLoopVariableDependency(
		PredaParser::ExpressionContext *update);
	void RecordExpressionEffects(
		PredaParser::ExpressionContext *expression);
	void WidenLoopDependencies(
		antlr4::ParserRuleContext *loopContext);
	std::string CollectRelay(const RelaySiteInput &input);
	void ResolveLambdaHandler(
		const std::string &relaySiteId,
		const std::string &handlerName,
		int64_t opcode,
		ScopeType scope,
		const std::vector<std::string> &parameterTypes,
		const std::string &targetFunctionId,
		const std::string &targetFunctionSignature,
		uint64_t targetFunctionOverloadIndex);
	void SetFunctionRelayReachability(
		const std::string &targetFunctionId,
		bool mayEmitRelay,
		bool hasUnmodeledRelayReachableCall);
	void Finalize();
	void BuildSummaries();

	RelayExprIR BuildExpression(
		PredaParser::ExpressionContext *context,
		const std::string &type = std::string()) const;

private:
	RelayProtocolIR &m_ir;
	analysis::RelayDependencyAnalyzer m_dependencyAnalyzer;
	std::map<std::string, size_t> m_namedHandlers;
	bool m_finalized = false;

	RelayExprIR BuildSourceExpression(
		antlr4::ParserRuleContext *sourceContext,
		PredaParser::ExpressionContext *expression,
		const std::string &text,
		const std::string &type) const;
	std::vector<BranchCondition> CollectBranches(PredaParser::RelayStatementContext *context) const;
	std::vector<LoopProtocol> CollectLoops(PredaParser::RelayStatementContext *context) const;
	LoopProtocol BuildForLoop(PredaParser::ForStatementContext *context) const;
	FunctionProtocol &GetOrCreateFunction(
		const std::string &contract,
		const std::string &function,
		const std::string &sourceFunctionId,
		const std::string &sourceFunctionSignature,
		uint64_t sourceFunctionOverloadIndex,
		ScopeType scope);
	ProtocolNode BuildProtocolNode(const RelaySite &site) const;
};

} // namespace relay_protocol
} // namespace transpiler
