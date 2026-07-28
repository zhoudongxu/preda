#include "RelayProtocolCollector.h"
#include "analysis/RelaySummaryBuilder.h"

#include "../transpiler/PredaTranspiler.h"

#include <algorithm>
#include <utility>

namespace transpiler {
namespace relay_protocol {
namespace {

SourceLocation GetLocation(const antlr4::ParserRuleContext *context)
{
	SourceLocation result;
	if (context == nullptr || context->start == nullptr)
		return result;

	result.line = static_cast<uint32_t>(context->start->getLine());
	result.column = static_cast<uint32_t>(context->start->getCharPositionInLine());
	result.startOffset = static_cast<int64_t>(context->start->getStartIndex());
	if (context->stop != nullptr)
	{
		result.endLine = static_cast<uint32_t>(context->stop->getLine());
		result.endColumn = static_cast<uint32_t>(
			context->stop->getCharPositionInLine() + context->stop->getText().size());
		result.endOffset = static_cast<int64_t>(context->stop->getStopIndex());
	}
	else
	{
		result.endLine = result.line;
		result.endColumn = result.column;
		result.endOffset = result.startOffset;
	}
	return result;
}

const char *ExpressionOperator(int expressionType)
{
	static const char *operators[] = {
		"++", "--", "[]", "()", ".", "group", "++", "--", "+", "-", "!", "~",
		"deploy", "*", "/", "%", "+", "-", "<<", ">>", "<", ">", "<=", ">=",
		"==", "!=", "&", "^", "|", "&&", "||", "?:", "=", "+=", "-=", "*=", "/=",
		"%=", "<<=", ">>=", "&=", "^=", "|=", "primary",
	};
	if (expressionType < 0 || expressionType >= static_cast<int>(sizeof(operators) / sizeof(operators[0])))
		return "";
	return operators[expressionType];
}

bool IsUnaryExpression(int expressionType)
{
	return (expressionType >= 0 && expressionType <= 1) ||
		(expressionType >= 6 && expressionType <= 11);
}

bool IsBinaryExpression(int expressionType)
{
	return (expressionType >= 13 && expressionType <= 30) ||
		(expressionType >= 32 && expressionType <= 42);
}

PredaParser::ExpressionContext *StripGroups(PredaParser::ExpressionContext *context)
{
	while (context != nullptr &&
		context->expressionType == static_cast<int>(PredaExpressionTypes::SurroundWithParentheses) &&
		context->expression().size() == 1)
	{
		context = context->expression(0);
	}
	return context;
}

std::string PrimaryIdentifier(PredaParser::ExpressionContext *context)
{
	context = StripGroups(context);
	if (context == nullptr ||
		context->expressionType != static_cast<int>(PredaExpressionTypes::Primary) ||
		context->primaryExpression() == nullptr ||
		context->primaryExpression()->identifier() == nullptr)
	{
		return std::string();
	}
	return context->primaryExpression()->identifier()->getText();
}

bool IsPrimaryLiteral(PredaParser::ExpressionContext *context)
{
	context = StripGroups(context);
	if (context == nullptr ||
		context->expressionType != static_cast<int>(PredaExpressionTypes::Primary) ||
		context->primaryExpression() == nullptr)
	{
		return false;
	}
	PredaParser::PrimaryExpressionContext *primary = context->primaryExpression();
	return primary->identifier() == nullptr &&
		primary->fundamentalTypeName() == nullptr &&
		primary->builtInContainerTypeName() == nullptr;
}

bool IsDirectIdentifierWrite(
	PredaParser::ExpressionContext *context,
	const std::string &identifier)
{
	context = StripGroups(context);
	if (context == nullptr || context->expression().empty())
		return false;

	const int kind = context->expressionType;
	const bool mutating =
		kind == static_cast<int>(PredaExpressionTypes::PostIncrement) ||
		kind == static_cast<int>(PredaExpressionTypes::PostDecrement) ||
		kind == static_cast<int>(PredaExpressionTypes::PreIncrement) ||
		kind == static_cast<int>(PredaExpressionTypes::PreDecrement) ||
		(kind >= static_cast<int>(PredaExpressionTypes::Assignment) &&
			kind <= static_cast<int>(PredaExpressionTypes::AssignmentBitwiseOr));
	return mutating &&
		PrimaryIdentifier(context->expression(0)) == identifier;
}

bool ParseTreeWritesIdentifier(
	antlr4::tree::ParseTree *tree,
	const std::string &identifier)
{
	if (tree == nullptr)
		return false;

	if (auto *expression =
		dynamic_cast<PredaParser::ExpressionContext *>(tree))
	{
		if (IsDirectIdentifierWrite(expression, identifier))
			return true;
	}
	else if (auto *declaration =
		dynamic_cast<PredaParser::LocalVariableDeclarationContext *>(tree))
	{
		// Treat shadowing as a write as well. This deliberately favors false
		// negatives over claiming a loop is bounded when textual bindings are
		// ambiguous to this syntax-only analysis.
		if (declaration->identifier() != nullptr &&
			declaration->identifier()->getText() == identifier)
		{
			return true;
		}
	}

	for (antlr4::tree::ParseTree *child : tree->children)
	{
		if (ParseTreeWritesIdentifier(child, identifier))
			return true;
	}
	return false;
}

bool ParseTreeHasEarlyLoopExit(antlr4::tree::ParseTree *tree)
{
	if (tree == nullptr)
		return false;

	if (dynamic_cast<PredaParser::BreakStatementContext *>(tree) != nullptr ||
		dynamic_cast<PredaParser::ContinueStatementContext *>(tree) != nullptr ||
		dynamic_cast<PredaParser::ReturnStatementContext *>(tree) != nullptr)
	{
		return true;
	}

	for (antlr4::tree::ParseTree *child : tree->children)
	{
		if (ParseTreeHasEarlyLoopExit(child))
			return true;
	}
	return false;
}

RelayExprIR MissingLoopExpression(
	const char *reason,
	const SourceLocation &loopLocation)
{
	RelayExprIR result;
	result.kind = RelayExprKind::Opaque;
	result.location = loopLocation;
	result.opaqueReason = reason;
	return result;
}

} // namespace

RelayProtocolCollector::RelayProtocolCollector(PredaTranspilerContext &context)
	: m_ir(context.m_relayProtocolIR)
{
}

void RelayProtocolCollector::Reset(
	const std::string &dappName,
	const std::string &contractName)
{
	m_ir.Reset(dappName, contractName);
	m_namedHandlers.clear();
	m_finalized = false;
}

RelayExprIR RelayProtocolCollector::BuildExpression(
	PredaParser::ExpressionContext *context,
	const std::string &type) const
{
	RelayExprIR result;
	result.type = type;
	if (context == nullptr)
	{
		result.opaqueReason = "missing expression context";
		return result;
	}

	result.text = context->getText();
	result.location = GetLocation(context);
	result.op = ExpressionOperator(context->expressionType);

	const int expressionType = context->expressionType;
	if (expressionType == static_cast<int>(PredaExpressionTypes::Primary))
	{
		PredaParser::PrimaryExpressionContext *primary = context->primaryExpression();
		if (primary == nullptr)
		{
			result.opaqueReason = "primary expression has no primary node";
			return result;
		}
		if (primary->identifier() != nullptr)
			result.kind = RelayExprKind::Identifier;
		else if (primary->fundamentalTypeName() != nullptr || primary->builtInContainerTypeName() != nullptr)
			result.kind = RelayExprKind::Keyword;
		else
			result.kind = RelayExprKind::Literal;
		return result;
	}

	// Ternaries are intentionally retained as expression-level Opaque in
	// schema v2. They are valid PREDA, but preserving branch evaluation
	// semantics requires a later expression-IR version. The complete source
	// range and text remain available.
	if (expressionType == static_cast<int>(PredaExpressionTypes::TernaryConditional))
	{
		result.kind = RelayExprKind::Opaque;
		result.opaqueReason = "ternary expression is not structurally modeled by relay protocol IR v2";
		return result;
	}

	const std::vector<PredaParser::ExpressionContext *> childExpressions = context->expression();
	if (expressionType == static_cast<int>(PredaExpressionTypes::Bracket))
		result.kind = RelayExprKind::Index;
	else if (expressionType == static_cast<int>(PredaExpressionTypes::Parentheses) ||
		expressionType == static_cast<int>(PredaExpressionTypes::Deploy))
		result.kind = RelayExprKind::Call;
	else if (expressionType == static_cast<int>(PredaExpressionTypes::Dot))
		result.kind = RelayExprKind::MemberAccess;
	else if (expressionType == static_cast<int>(PredaExpressionTypes::SurroundWithParentheses))
		result.kind = RelayExprKind::Group;
	else if (IsUnaryExpression(expressionType))
		result.kind = RelayExprKind::Unary;
	else if (IsBinaryExpression(expressionType))
		result.kind = RelayExprKind::Binary;
	else
	{
		result.kind = RelayExprKind::Opaque;
		result.opaqueReason =
			"unsupported PREDA expressionType " + std::to_string(expressionType);
		return result;
	}

	for (size_t i = 0; i < childExpressions.size(); ++i)
	{
		const bool propagateType =
			expressionType == static_cast<int>(PredaExpressionTypes::SurroundWithParentheses) &&
			childExpressions.size() == 1;
		result.children.push_back(BuildExpression(
			childExpressions[i],
			propagateType ? type : std::string()));
	}

	if (context->functionCallArguments() != nullptr)
	{
		for (PredaParser::ExpressionContext *argument :
			context->functionCallArguments()->expression())
		{
			result.children.push_back(BuildExpression(argument));
		}
	}
	if (expressionType == static_cast<int>(PredaExpressionTypes::Dot) &&
		context->identifier() != nullptr)
	{
		RelayExprIR member;
		member.kind = RelayExprKind::Identifier;
		member.text = context->identifier()->getText();
		member.location = GetLocation(context->identifier());
		result.children.push_back(std::move(member));
	}
	if (expressionType == static_cast<int>(PredaExpressionTypes::SurroundWithParentheses) &&
		result.children.size() == 1 &&
		result.children[0].kind == RelayExprKind::Opaque)
	{
		result.kind = RelayExprKind::Opaque;
		result.opaqueReason = result.children[0].opaqueReason;
	}
	return result;
}

RelayExprIR RelayProtocolCollector::BuildSourceExpression(
	antlr4::ParserRuleContext *sourceContext,
	PredaParser::ExpressionContext *expression,
	const std::string &text,
	const std::string &type) const
{
	if (expression != nullptr)
		return BuildExpression(expression, type);

	RelayExprIR result;
	result.text = text.empty() && sourceContext != nullptr
		? sourceContext->getText()
		: text;
	result.type = type;
	result.location = GetLocation(sourceContext);
	if (dynamic_cast<PredaParser::IdentifierContext *>(sourceContext) != nullptr)
		result.kind = RelayExprKind::Identifier;
	else
	{
		result.kind = RelayExprKind::Opaque;
		result.opaqueReason = "expression has no PREDA ExpressionContext";
	}
	return result;
}

std::vector<BranchCondition> RelayProtocolCollector::CollectBranches(
	PredaParser::RelayStatementContext *context) const
{
	std::vector<std::vector<BranchCondition>> innerToOuter;
	for (antlr4::tree::ParseTree *parent = context == nullptr ? nullptr : context->parent;
		parent != nullptr;
		parent = parent->parent)
	{
		if (dynamic_cast<PredaParser::RelayLambdaDefinitionContext *>(parent) != nullptr ||
			dynamic_cast<PredaParser::FunctionDefinitionContext *>(parent) != nullptr)
		{
			break;
		}

		std::vector<BranchCondition> armConditions;
		if (auto *ifBlock = dynamic_cast<PredaParser::IfWithBlockContext *>(parent))
		{
			BranchCondition guard;
			guard.condition = BuildExpression(ifBlock->expression(), "bool");
			guard.polarity = true;
			guard.arm = "if";
			guard.location = GetLocation(ifBlock);
			armConditions.push_back(std::move(guard));
		}
		else if (auto *elseIfBlock = dynamic_cast<PredaParser::ElseIfWithBlockContext *>(parent))
		{
			auto *ifStatement = dynamic_cast<PredaParser::IfStatementContext *>(elseIfBlock->parent);
			if (ifStatement != nullptr)
			{
				BranchCondition first;
				first.condition = BuildExpression(ifStatement->ifWithBlock()->expression(), "bool");
				first.polarity = false;
				first.arm = "else_if";
				first.location = GetLocation(elseIfBlock);
				armConditions.push_back(std::move(first));
				for (PredaParser::ElseIfWithBlockContext *candidate : ifStatement->elseIfWithBlock())
				{
					BranchCondition guard;
					guard.condition = BuildExpression(candidate->expression(), "bool");
					guard.polarity = candidate == elseIfBlock;
					guard.arm = "else_if";
					guard.location = GetLocation(elseIfBlock);
					armConditions.push_back(std::move(guard));
					if (candidate == elseIfBlock)
						break;
				}
			}
		}
		else if (auto *elseBlock = dynamic_cast<PredaParser::ElseWithBlockContext *>(parent))
		{
			auto *ifStatement = dynamic_cast<PredaParser::IfStatementContext *>(elseBlock->parent);
			if (ifStatement != nullptr)
			{
				BranchCondition first;
				first.condition = BuildExpression(ifStatement->ifWithBlock()->expression(), "bool");
				first.polarity = false;
				first.arm = "else";
				first.location = GetLocation(elseBlock);
				armConditions.push_back(std::move(first));
				for (PredaParser::ElseIfWithBlockContext *candidate : ifStatement->elseIfWithBlock())
				{
					BranchCondition guard;
					guard.condition = BuildExpression(candidate->expression(), "bool");
					guard.polarity = false;
					guard.arm = "else";
					guard.location = GetLocation(elseBlock);
					armConditions.push_back(std::move(guard));
				}
			}
		}
		if (!armConditions.empty())
			innerToOuter.push_back(std::move(armConditions));
	}

	std::vector<BranchCondition> result;
	for (auto group = innerToOuter.rbegin(); group != innerToOuter.rend(); ++group)
		result.insert(result.end(), group->begin(), group->end());
	return result;
}

LoopProtocol RelayProtocolCollector::BuildForLoop(
	PredaParser::ForStatementContext *context) const
{
	LoopProtocol result;
	result.kind = "for";
	result.location = GetLocation(context);

	PredaParser::ExpressionContext *initializerExpression = context->firstExpression;
	PredaParser::LocalVariableDeclarationContext *declaration =
		context->localVariableDeclaration();
	if (declaration != nullptr)
	{
		result.inductionVariable = declaration->identifier()->getText();
		initializerExpression = declaration->expression();
	}

	result.initializer = initializerExpression != nullptr
		? BuildExpression(initializerExpression)
		: MissingLoopExpression("for-loop initializer omitted", result.location);
	result.condition = context->secondExpression != nullptr
		? BuildExpression(context->secondExpression, "bool")
		: MissingLoopExpression("for-loop condition omitted", result.location);
	result.update = context->thirdExpression != nullptr
		? BuildExpression(context->thirdExpression)
		: MissingLoopExpression("for-loop update omitted", result.location);

	if (initializerExpression != nullptr && IsPrimaryLiteral(initializerExpression))
		result.initialValue = StripGroups(initializerExpression)->getText();

	PredaParser::ExpressionContext *condition = StripGroups(context->secondExpression);
	PredaParser::ExpressionContext *update = StripGroups(context->thirdExpression);
	const bool initializerSupported =
		declaration != nullptr &&
		!result.inductionVariable.empty() &&
		!result.initialValue.empty();
	bool conditionSupported = false;
	bool updateSupported = false;
	bool incrementing = false;
	bool decrementing = false;
	if (condition != nullptr && condition->expression().size() == 2)
	{
		const int kind = condition->expressionType;
		if (kind == static_cast<int>(PredaExpressionTypes::LessThan) ||
			kind == static_cast<int>(PredaExpressionTypes::GreaterThan))
		{
			const std::string variable = PrimaryIdentifier(condition->expression(0));
			if (variable == result.inductionVariable &&
				IsPrimaryLiteral(condition->expression(1)))
			{
				result.comparison = ExpressionOperator(kind);
				result.boundValue = StripGroups(condition->expression(1))->getText();
				conditionSupported = true;
			}
		}
	}

	if (update != nullptr)
	{
		const int kind = update->expressionType;
		if ((kind == static_cast<int>(PredaExpressionTypes::PostIncrement) ||
			kind == static_cast<int>(PredaExpressionTypes::PreIncrement)) &&
			PrimaryIdentifier(update->expression(0)) == result.inductionVariable)
		{
			result.step = "+1";
			incrementing = true;
			updateSupported = true;
		}
		else if ((kind == static_cast<int>(PredaExpressionTypes::PostDecrement) ||
			kind == static_cast<int>(PredaExpressionTypes::PreDecrement)) &&
			PrimaryIdentifier(update->expression(0)) == result.inductionVariable)
		{
			result.step = "-1";
			decrementing = true;
			updateSupported = true;
		}
	}

	bool bodyWritesInductionVariable = false;
	for (PredaParser::StatementContext *statement : context->statement())
	{
		if (!result.inductionVariable.empty() &&
			ParseTreeWritesIdentifier(statement, result.inductionVariable))
		{
			bodyWritesInductionVariable = true;
		}
		if (ParseTreeHasEarlyLoopExit(statement))
			result.bodyMayExitEarly = true;
	}

	const bool increasingBound = result.comparison == "<";
	const bool decreasingBound = result.comparison == ">";
	const bool directionSupported =
		(increasingBound && incrementing) ||
		(decreasingBound && decrementing);
	result.staticallyBounded =
		initializerSupported &&
		conditionSupported &&
		updateSupported &&
		directionSupported &&
		!bodyWritesInductionVariable;
	if (!result.staticallyBounded)
	{
		if (!initializerSupported)
		{
			result.opaqueReason =
				"loop initializer is not a canonical local declaration with a literal value";
		}
		else if (!conditionSupported)
		{
			result.opaqueReason =
				"loop condition is not a strict induction-variable comparison against a literal";
		}
		else if (!updateSupported)
		{
			result.opaqueReason =
				"loop update is not a canonical ++ or -- of the induction variable";
		}
		else if (!directionSupported)
		{
			result.opaqueReason =
				"loop update does not progress toward its strict bound";
		}
		else
		{
			result.opaqueReason =
				"loop body writes or shadows the induction variable";
		}
	}
	return result;
}

std::vector<LoopProtocol> RelayProtocolCollector::CollectLoops(
	PredaParser::RelayStatementContext *context) const
{
	std::vector<LoopProtocol> innerToOuter;
	for (antlr4::tree::ParseTree *parent = context == nullptr ? nullptr : context->parent;
		parent != nullptr;
		parent = parent->parent)
	{
		if (dynamic_cast<PredaParser::RelayLambdaDefinitionContext *>(parent) != nullptr ||
			dynamic_cast<PredaParser::FunctionDefinitionContext *>(parent) != nullptr)
		{
			break;
		}
		if (auto *forStatement = dynamic_cast<PredaParser::ForStatementContext *>(parent))
		{
			innerToOuter.push_back(BuildForLoop(forStatement));
		}
		else if (auto *whileStatement = dynamic_cast<PredaParser::WhileStatementContext *>(parent))
		{
			LoopProtocol loop;
			loop.kind = "while";
			loop.location = GetLocation(whileStatement);
			loop.initializer = MissingLoopExpression("while-loop has no initializer", loop.location);
			loop.condition = BuildExpression(whileStatement->expression(), "bool");
			loop.update = MissingLoopExpression("while-loop has no explicit update", loop.location);
			loop.opaqueReason = "while-loop bound is not statically established";
			innerToOuter.push_back(std::move(loop));
		}
		else if (auto *doWhileStatement = dynamic_cast<PredaParser::DoWhileStatementContext *>(parent))
		{
			LoopProtocol loop;
			loop.kind = "do_while";
			loop.location = GetLocation(doWhileStatement);
			loop.initializer = MissingLoopExpression("do-while loop has no initializer", loop.location);
			loop.condition = BuildExpression(doWhileStatement->expression(), "bool");
			loop.update = MissingLoopExpression("do-while loop has no explicit update", loop.location);
			loop.opaqueReason = "do-while loop bound is not statically established";
			innerToOuter.push_back(std::move(loop));
		}
	}
	std::reverse(innerToOuter.begin(), innerToOuter.end());
	return innerToOuter;
}

FunctionProtocol &RelayProtocolCollector::GetOrCreateFunction(
	const std::string &contract,
	const std::string &function,
	const std::string &sourceFunctionId,
	const std::string &sourceFunctionSignature,
	uint64_t sourceFunctionOverloadIndex,
	ScopeType scope)
{
	for (FunctionProtocol &protocol : m_ir.functions)
	{
		if (protocol.sourceFunctionId == sourceFunctionId)
			return protocol;
	}
	FunctionProtocol protocol;
	protocol.contract = contract;
	protocol.function = function;
	protocol.sourceFunctionId = sourceFunctionId;
	protocol.sourceFunctionSignature = sourceFunctionSignature;
	protocol.sourceFunctionOverloadIndex = sourceFunctionOverloadIndex;
	protocol.scope = scope;
	protocol.root.kind = ProtocolNodeKind::Sequence;
	m_ir.functions.push_back(std::move(protocol));
	return m_ir.functions.back();
}

ProtocolNode RelayProtocolCollector::BuildProtocolNode(const RelaySite &site) const
{
	ProtocolNode call;
	call.kind = ProtocolNodeKind::Call;
	call.location = site.location;
	call.callee = site.handlerId;

	ProtocolNode current;
	current.kind = ProtocolNodeKind::Emit;
	current.location = site.location;
	current.relaySiteId = site.id;
	current.children.push_back(std::move(call));

	if (site.relayKind == RelayKind::Shards)
	{
		ProtocolNode parallel;
		parallel.kind = ProtocolNodeKind::Parallel;
		parallel.location = site.location;
		parallel.children.push_back(std::move(current));
		current = std::move(parallel);
	}
	struct Wrapper
	{
		int64_t startOffset;
		bool isBranch;
		size_t index;
	};
	std::vector<Wrapper> wrappers;
	for (size_t i = 0; i < site.branches.size(); ++i)
		wrappers.push_back({site.branches[i].location.startOffset, true, i});
	for (size_t i = 0; i < site.loops.size(); ++i)
		wrappers.push_back({site.loops[i].location.startOffset, false, i});
	std::stable_sort(wrappers.begin(), wrappers.end(), [](const Wrapper &left, const Wrapper &right) {
		if (left.startOffset != right.startOffset)
			return left.startOffset > right.startOffset;
		if (left.isBranch != right.isBranch)
			return left.isBranch;
		if (left.isBranch)
		{
			// A single else-if arm contributes multiple guards at the same
			// source location in outer-to-inner evaluation order. Wrappers are
			// applied inside-out, so equal-location branch guards must be
			// visited in reverse order to preserve that order in the IR tree.
			return left.index > right.index;
		}
		return left.index < right.index;
	});
	for (const Wrapper &wrapper : wrappers)
	{
		ProtocolNode parent;
		if (wrapper.isBranch)
		{
			const BranchCondition &branch = site.branches[wrapper.index];
			parent.kind = ProtocolNodeKind::Branch;
			parent.location = branch.location;
			parent.condition = branch.condition;
			parent.conditionPolarity = branch.polarity;
		}
		else
		{
			parent.kind = ProtocolNodeKind::Repeat;
			parent.location = site.loops[wrapper.index].location;
			parent.loop = site.loops[wrapper.index];
		}
		parent.children.push_back(std::move(current));
		current = std::move(parent);
	}
	return current;
}

std::string RelayProtocolCollector::CollectRelay(const RelaySiteInput &input)
{
	m_finalized = false;
	RelaySite site;
	site.id = "relay_site_" + std::to_string(m_ir.relaySites.size());
	site.sourceContract = input.sourceContract;
	site.sourceFunction = input.sourceFunction;
	site.sourceFunctionId = input.sourceFunctionId;
	site.sourceFunctionSignature = input.sourceFunctionSignature;
	site.sourceFunctionOverloadIndex = input.sourceFunctionOverloadIndex;
	site.sourceScope = input.sourceScope;
	site.location = GetLocation(input.context);
	site.relayKind = input.relayKind;
	site.targetScope = input.targetScope;
	site.targetFunction = input.targetFunction;
	if (input.targetExpression != nullptr)
		site.target = BuildExpression(input.targetExpression, input.targetType);
	else
	{
		site.target.kind = RelayExprKind::Keyword;
		site.target.text = input.targetText;
		site.target.type = input.targetType;
		site.target.location = GetLocation(
			input.context == nullptr ? nullptr : input.context->relayType());
	}
	for (const RelayArgumentInput &argumentInput : input.arguments)
	{
		RelayArgument argument;
		argument.type = argumentInput.type;
		argument.expression = BuildSourceExpression(
			argumentInput.sourceContext,
			argumentInput.expression,
			argumentInput.text,
			argumentInput.type);
		site.arguments.push_back(std::move(argument));
	}
	site.branches = CollectBranches(input.context);
	site.loops = CollectLoops(input.context);

	size_t handlerIndex = 0;
	if (input.lambdaHandler)
	{
		RelayHandler handler;
		handler.id = "relay_handler_" + std::to_string(m_ir.handlers.size());
		handler.kind = RelayHandlerKind::Lambda;
		handler.contract = input.sourceContract;
		handler.targetFunctionId = input.targetFunctionId;
		handler.targetFunctionSignature = input.targetFunctionSignature;
		handler.targetFunctionOverloadIndex =
			input.targetFunctionOverloadIndex;
		handler.scope = input.targetScope;
		handler.location = site.location;
		for (const RelayArgumentInput &argument : input.arguments)
			handler.parameterTypes.push_back(argument.type);
		handlerIndex = m_ir.handlers.size();
		m_ir.handlers.push_back(std::move(handler));
	}
	else
	{
		const std::string key = input.sourceContract + "|" + input.targetFunction +
			"|" + std::to_string(static_cast<uint32_t>(input.targetScope)) +
			"|" + std::to_string(input.opcode);
		auto existing = m_namedHandlers.find(key);
		if (existing != m_namedHandlers.end())
		{
			handlerIndex = existing->second;
		}
		else
		{
			RelayHandler handler;
			handler.id = "relay_handler_" + std::to_string(m_ir.handlers.size());
			handler.kind = RelayHandlerKind::Named;
			handler.contract = input.sourceContract;
			handler.name = input.targetFunction;
			handler.targetFunctionId = input.targetFunctionId;
			handler.targetFunctionSignature = input.targetFunctionSignature;
			handler.targetFunctionOverloadIndex =
				input.targetFunctionOverloadIndex;
			handler.scope = input.targetScope;
			handler.opcode = input.opcode;
			handler.resolved = true;
			handler.location = site.location;
			for (const RelayArgumentInput &argument : input.arguments)
				handler.parameterTypes.push_back(argument.type);
			handlerIndex = m_ir.handlers.size();
			m_ir.handlers.push_back(std::move(handler));
			m_namedHandlers.emplace(key, handlerIndex);
		}
	}
	site.handlerId = m_ir.handlers[handlerIndex].id;

	RelayProtocolEdge edge;
	edge.id = "relay_edge_" + std::to_string(m_ir.edges.size());
	edge.sourceFunction = input.sourceFunction;
	edge.sourceFunctionId = input.sourceFunctionId;
	edge.sourceFunctionSignature = input.sourceFunctionSignature;
	edge.sourceFunctionOverloadIndex = input.sourceFunctionOverloadIndex;
	edge.relaySiteId = site.id;
	edge.handlerId = site.handlerId;
	edge.resolved = m_ir.handlers[handlerIndex].resolved;

	const std::string siteId = site.id;
	FunctionProtocol &function = GetOrCreateFunction(
		input.sourceContract,
		input.sourceFunction,
		input.sourceFunctionId,
		input.sourceFunctionSignature,
		input.sourceFunctionOverloadIndex,
		input.sourceScope);
	function.relaySiteIds.push_back(site.id);
	function.root.children.push_back(BuildProtocolNode(site));
	m_ir.relaySites.push_back(std::move(site));
	m_ir.edges.push_back(std::move(edge));
	return siteId;
}

void RelayProtocolCollector::ResolveLambdaHandler(
	const std::string &relaySiteId,
	const std::string &handlerName,
	int64_t opcode,
	ScopeType scope,
	const std::vector<std::string> &parameterTypes,
	const std::string &targetFunctionId,
	const std::string &targetFunctionSignature,
	uint64_t targetFunctionOverloadIndex)
{
	for (RelaySite &site : m_ir.relaySites)
	{
		if (site.id != relaySiteId)
			continue;
		for (RelayHandler &handler : m_ir.handlers)
		{
			if (handler.id != site.handlerId)
				continue;
			handler.name = handlerName;
			handler.opcode = opcode;
			handler.scope = scope;
			handler.parameterTypes = parameterTypes;
			handler.targetFunctionId = targetFunctionId;
			handler.targetFunctionSignature = targetFunctionSignature;
			handler.targetFunctionOverloadIndex =
				targetFunctionOverloadIndex;
			handler.resolved = true;
			break;
		}
		for (RelayProtocolEdge &edge : m_ir.edges)
		{
			if (edge.relaySiteId == relaySiteId)
				edge.resolved = true;
		}
		break;
	}
}

void RelayProtocolCollector::SetFunctionRelayReachability(
	const std::string &targetFunctionId,
	bool mayEmitRelay,
	bool hasUnmodeledRelayReachableCall)
{
	for (RelayHandler &handler : m_ir.handlers)
	{
		if (handler.targetFunctionId != targetFunctionId)
			continue;
		handler.relayReachabilityKnown = true;
		handler.mayEmitRelay = mayEmitRelay;
	}
	for (FunctionProtocol &function : m_ir.functions)
	{
		if (function.sourceFunctionId == targetFunctionId)
		{
			function.hasUnmodeledRelayReachableCall =
				hasUnmodeledRelayReachableCall;
		}
	}
}

void RelayProtocolCollector::Finalize()
{
	if (m_finalized)
		return;
	for (FunctionProtocol &function : m_ir.functions)
	{
		if (!function.root.children.empty() &&
			function.root.children.back().kind == ProtocolNodeKind::End)
		{
			continue;
		}
		ProtocolNode end;
		end.kind = ProtocolNodeKind::End;
		function.root.children.push_back(std::move(end));
	}
	m_finalized = true;
}

void RelayProtocolCollector::BuildSummaries()
{
	transpiler::relay_protocol::analysis::RelaySummaryBuilder builder;
	builder.Build(m_ir);
}

} // namespace relay_protocol
} // namespace transpiler
