#include "PredaCFGBuilder.h"

#include "PredaCallGraphAnalysis.h"
#include "PredaEffectAnalysis.h"
#include "RelayICFGBuilder.h"
#include "../RelayProtocolIR.h"
#include "../metrics/RelayAnalysisMetrics.h"
#include "../refinement/RelayFormulaBuilder.h"
#include "../../transpiler/PredaTranspiler.h"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace transpiler {
namespace relay_protocol {
namespace cfg {
namespace {

SourceLocation GetLocation(const antlr4::ParserRuleContext *context)
{
	SourceLocation result;
	if (context == nullptr || context->start == nullptr)
		return result;
	result.line = static_cast<uint32_t>(context->start->getLine());
	result.column = static_cast<uint32_t>(
		context->start->getCharPositionInLine());
	result.startOffset = static_cast<int64_t>(
		context->start->getStartIndex());
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

bool Contains(
	const SourceLocation &outer,
	const SourceLocation &inner)
{
	return outer.startOffset >= 0 && inner.startOffset >= 0 &&
		inner.startOffset >= outer.startOffset &&
		inner.endOffset <= outer.endOffset;
}

const char *NodeKindToken(PredaCFGNodeKind kind)
{
	switch (kind)
	{
	case PredaCFGNodeKind::Entry: return "entry";
	case PredaCFGNodeKind::Exit: return "exit";
	case PredaCFGNodeKind::Basic: return "basic";
	case PredaCFGNodeKind::Branch: return "branch";
	case PredaCFGNodeKind::LoopHeader: return "loop_header";
	case PredaCFGNodeKind::LoopLatch: return "loop_latch";
	case PredaCFGNodeKind::Break: return "break";
	case PredaCFGNodeKind::Continue: return "continue";
	case PredaCFGNodeKind::Return: return "return";
	case PredaCFGNodeKind::SynchronousCall: return "sync_call";
	case PredaCFGNodeKind::RelayEmit: return "relay_emit";
	case PredaCFGNodeKind::AbortOrFailure: return "abort";
	case PredaCFGNodeKind::Opaque: return "opaque";
	default: return "unknown";
	}
}

const char *ExpressionOperator(int expressionType)
{
	static const char *operators[] = {
		"++", "--", "[]", "()", ".", "group", "++", "--",
		"+", "-", "!", "~", "deploy", "*", "/", "%", "+", "-",
		"<<", ">>", "<", ">", "<=", ">=", "==", "!=", "&", "^",
		"|", "&&", "||", "?:", "=", "+=", "-=", "*=", "/=", "%=",
		"<<=", ">>=", "&=", "^=", "|=", "primary",
	};
	if (expressionType < 0 ||
		expressionType >= static_cast<int>(
			sizeof(operators) / sizeof(operators[0])))
	{
		return "";
	}
	return operators[expressionType];
}

bool IsUnary(int expressionType)
{
	return (expressionType >= 0 && expressionType <= 1) ||
		(expressionType >= 6 && expressionType <= 11);
}

bool IsBinary(int expressionType)
{
	return (expressionType >= 13 && expressionType <= 30) ||
		(expressionType >= 32 && expressionType <= 42);
}

bool ContainsExpressionKind(
	PredaParser::ExpressionContext *expression,
	PredaExpressionTypes kind)
{
	if (expression == nullptr)
		return false;
	if (expression->expressionType == static_cast<int>(kind))
		return true;
	for (PredaParser::ExpressionContext *child : expression->expression())
	{
		if (ContainsExpressionKind(child, kind))
			return true;
	}
	if (expression->functionCallArguments() != nullptr)
	{
		for (PredaParser::ExpressionContext *argument :
			expression->functionCallArguments()->expression())
		{
			if (ContainsExpressionKind(argument, kind))
				return true;
		}
	}
	return false;
}

bool MayHaveImplicitRuntimeFailure(
	PredaParser::ExpressionContext *expression)
{
	if (expression == nullptr)
		return false;
	const int kind = expression->expressionType;
	if (kind == static_cast<int>(PredaExpressionTypes::Bracket) ||
		kind == static_cast<int>(PredaExpressionTypes::Divide) ||
		kind == static_cast<int>(PredaExpressionTypes::Modulo) ||
		kind == static_cast<int>(PredaExpressionTypes::AssignmentDivide) ||
		kind == static_cast<int>(PredaExpressionTypes::AssignmentModulo))
	{
		return true;
	}
	for (PredaParser::ExpressionContext *child : expression->expression())
	{
		if (MayHaveImplicitRuntimeFailure(child))
			return true;
	}
	if (expression->functionCallArguments() != nullptr)
	{
		for (PredaParser::ExpressionContext *argument :
			expression->functionCallArguments()->expression())
		{
			if (MayHaveImplicitRuntimeFailure(argument))
				return true;
		}
	}
	return false;
}

RelayExprIR BuildSyntaxExpression(
	PredaParser::ExpressionContext *context,
	const std::string &type = std::string())
{
	RelayExprIR result;
	result.type = type;
	result.location = GetLocation(context);
	if (context == nullptr)
	{
		result.opaqueReason = "missing PREDA expression";
		return result;
	}
	result.text = context->getText();
	result.op = ExpressionOperator(context->expressionType);
	const int expressionType = context->expressionType;
	if (expressionType == static_cast<int>(PredaExpressionTypes::Primary))
	{
		PredaParser::PrimaryExpressionContext *primary =
			context->primaryExpression();
		if (primary == nullptr)
		{
			result.opaqueReason = "primary expression has no primary node";
			return result;
		}
		if (primary->identifier() != nullptr)
			result.kind = RelayExprKind::Identifier;
		else if (primary->fundamentalTypeName() != nullptr ||
			primary->builtInContainerTypeName() != nullptr)
			result.kind = RelayExprKind::Keyword;
		else
			result.kind = RelayExprKind::Literal;
		return result;
	}
	if (expressionType ==
		static_cast<int>(PredaExpressionTypes::TernaryConditional))
	{
		result.kind = RelayExprKind::Opaque;
		result.opaqueReason =
			"ternary control expression is not represented structurally";
		return result;
	}
	if (expressionType == static_cast<int>(PredaExpressionTypes::Bracket))
		result.kind = RelayExprKind::Index;
	else if (expressionType ==
			static_cast<int>(PredaExpressionTypes::Parentheses) ||
		expressionType == static_cast<int>(PredaExpressionTypes::Deploy))
		result.kind = RelayExprKind::Call;
	else if (expressionType == static_cast<int>(PredaExpressionTypes::Dot))
		result.kind = RelayExprKind::MemberAccess;
	else if (expressionType == static_cast<int>(
			PredaExpressionTypes::SurroundWithParentheses))
		result.kind = RelayExprKind::Group;
	else if (IsUnary(expressionType))
		result.kind = RelayExprKind::Unary;
	else if (IsBinary(expressionType))
		result.kind = RelayExprKind::Binary;
	else
	{
		result.kind = RelayExprKind::Opaque;
		result.opaqueReason = "unsupported PREDA expression kind";
		return result;
	}
	for (PredaParser::ExpressionContext *child : context->expression())
		result.children.push_back(BuildSyntaxExpression(child));
	if (context->functionCallArguments() != nullptr)
	{
		for (PredaParser::ExpressionContext *argument :
			context->functionCallArguments()->expression())
		{
			result.children.push_back(BuildSyntaxExpression(argument));
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
	return result;
}

struct MutationRange
{
	SourceLocation location;
	bool alsoReads = false;
};

void CollectMutationRanges(
	PredaParser::ExpressionContext *expression,
	std::vector<MutationRange> &ranges)
{
	if (expression == nullptr)
		return;
	const int kind = expression->expressionType;
	const bool increment =
		kind == static_cast<int>(PredaExpressionTypes::PostIncrement) ||
		kind == static_cast<int>(PredaExpressionTypes::PostDecrement) ||
		kind == static_cast<int>(PredaExpressionTypes::PreIncrement) ||
		kind == static_cast<int>(PredaExpressionTypes::PreDecrement);
	const bool assignment =
		kind >= static_cast<int>(PredaExpressionTypes::Assignment) &&
		kind <= static_cast<int>(PredaExpressionTypes::AssignmentBitwiseOr);
	if ((increment || assignment) && !expression->expression().empty())
	{
		MutationRange range;
		range.location = GetLocation(expression->expression(0));
		range.alsoReads = increment ||
			kind != static_cast<int>(PredaExpressionTypes::Assignment);
		ranges.push_back(range);
	}
	for (PredaParser::ExpressionContext *child : expression->expression())
		CollectMutationRanges(child, ranges);
	if (expression->functionCallArguments() != nullptr)
	{
		for (PredaParser::ExpressionContext *argument :
			expression->functionCallArguments()->expression())
		{
			CollectMutationRanges(argument, ranges);
		}
	}
}

std::string DirectPrimaryIdentifier(
	PredaParser::ExpressionContext *expression)
{
	while (expression != nullptr &&
		expression->expressionType == static_cast<int>(
			PredaExpressionTypes::SurroundWithParentheses) &&
		expression->expression().size() == 1)
	{
		expression = expression->expression(0);
	}
	if (expression == nullptr ||
		expression->expressionType !=
			static_cast<int>(PredaExpressionTypes::Primary) ||
		expression->primaryExpression() == nullptr ||
		expression->primaryExpression()->identifier() == nullptr)
	{
		return std::string();
	}
	return expression->primaryExpression()->identifier()->getText();
}

struct LoopTarget
{
	NodeId breakTarget;
	NodeId continueTarget;
	std::string loopId;
	std::string inductionVariable;
};

class FunctionBuilder
{
public:
	FunctionBuilder(
		const PredaCFGFunctionInput &input,
		const PredaCFGBuilderInput &allInput,
		const RelayProtocolIR &protocol)
		: m_input(input), m_allInput(allInput), m_protocol(protocol)
	{
		m_cfg.functionId = input.functionId;
		m_cfg.function = input.function;
		m_cfg.signature = input.signature;
		m_cfg.scope = input.scope;
		m_cfg.generatedRelayLambda = input.generatedRelayLambda;
		m_cfg.location = input.location;
	}

	PredaFunctionCFG Build()
	{
		PredaCFGNode entry;
		entry.kind = PredaCFGNodeKind::Entry;
		entry.location = m_input.location;
		m_cfg.entryNodeId = AddNode(std::move(entry));

		PredaCFGNode exit;
		exit.kind = PredaCFGNodeKind::Exit;
		exit.location = m_input.location;
		m_cfg.exitNodeId = AddNode(std::move(exit));

		std::vector<NodeId> tails{m_cfg.entryNodeId};
		tails = BuildStatements(m_input.statements, tails);
		ConnectAll(tails, m_cfg.exitNodeId, PredaCFGEdgeKind::Fallthrough);

		PredaCFGRegion functionRegion;
		functionRegion.id = "region::" + m_cfg.functionId + "::function";
		functionRegion.kind = RegionKind::Function;
		functionRegion.sourceFunctionId = m_cfg.functionId;
		functionRegion.location = m_cfg.location;
		for (const PredaCFGNode &node : m_cfg.nodes)
			functionRegion.nodeIds.push_back(node.id);
		m_cfg.regions.push_back(std::move(functionRegion));

		for (const PredaCFGNode &node : m_cfg.nodes)
		{
			if (node.kind == PredaCFGNodeKind::SynchronousCall)
			{
				PredaCFGRegion region;
				region.id = "region::" + node.id + "::call";
				region.kind = RegionKind::SynchronousCallSite;
				region.sourceFunctionId = m_cfg.functionId;
				region.location = node.location;
				region.nodeIds.push_back(node.id);
				region.callSiteNodeId = node.id;
				m_cfg.regions.push_back(std::move(region));
			}
		}
		return std::move(m_cfg);
	}

private:
	const PredaCFGFunctionInput &m_input;
	const PredaCFGBuilderInput &m_allInput;
	const RelayProtocolIR &m_protocol;
	PredaFunctionCFG m_cfg;
	std::map<NodeId, size_t> m_nodeIndex;
	std::vector<LoopTarget> m_loops;
	std::set<std::pair<int64_t, int64_t>> m_emittedCallSites;
	bool m_creatingFailureSink = false;
	uint64_t m_nodeOrdinal = 0;
	uint64_t m_edgeOrdinal = 0;
	uint64_t m_regionOrdinal = 0;

	void MarkConservative(const std::string &reason)
	{
		if (m_cfg.status == AnalysisStatus::Complete)
			m_cfg.status = AnalysisStatus::Conservative;
		if (std::find(
				m_cfg.completenessReasons.begin(),
				m_cfg.completenessReasons.end(),
				reason) == m_cfg.completenessReasons.end())
		{
			m_cfg.completenessReasons.push_back(reason);
		}
	}

	void MarkUnsupported(const std::string &reason)
	{
		m_cfg.status = AnalysisStatus::Unsupported;
		if (std::find(
				m_cfg.completenessReasons.begin(),
				m_cfg.completenessReasons.end(),
				reason) == m_cfg.completenessReasons.end())
		{
			m_cfg.completenessReasons.push_back(reason);
		}
	}

	NodeId AddNode(PredaCFGNode node)
	{
		node.sourceFunctionId = m_cfg.functionId;
		node.enclosingLoopIds.clear();
		for (const LoopTarget &loop : m_loops)
			node.enclosingLoopIds.push_back(loop.loopId);
		const int64_t offset = node.location.startOffset;
		node.id = "cfg::" + m_cfg.functionId + "::" +
			std::to_string(offset) + "::" + NodeKindToken(node.kind) +
			"::" + std::to_string(m_nodeOrdinal++);
		m_nodeIndex[node.id] = m_cfg.nodes.size();
		m_cfg.nodes.push_back(std::move(node));
		const NodeId id = m_cfg.nodes.back().id;
		if (m_cfg.nodes.back().directEffect.mayAbortOrFail &&
			!m_creatingFailureSink &&
			!m_cfg.exitNodeId.empty() && id != m_cfg.exitNodeId)
		{
			PredaCFGNode failure;
			failure.kind = PredaCFGNodeKind::AbortOrFailure;
			failure.location = m_cfg.nodes[m_nodeIndex.at(id)].location;
			failure.directEffect.mayAbortOrFail = true;
			failure.directEffect.status = AnalysisStatus::Conservative;
			failure.directEffect.reason =
				"terminal failure or exceptional sink";
			m_creatingFailureSink = true;
			const NodeId failureId = AddNode(std::move(failure));
			m_creatingFailureSink = false;
			AddEdge(
				id,
				failureId,
				PredaCFGEdgeKind::ExceptionalOrFailure,
				true,
				"conservative terminal failure path");
		}
		return id;
	}

	PredaCFGNode &Node(const NodeId &id)
	{
		return m_cfg.nodes[m_nodeIndex.at(id)];
	}

	void AddEdge(
		const NodeId &source,
		const NodeId &target,
		PredaCFGEdgeKind kind,
		bool supported = true,
		const std::string &reason = std::string())
	{
		if (source.empty() || target.empty())
			return;
		PredaCFGEdge edge;
		edge.id = "cfg_edge::" + m_cfg.functionId + "::" +
			std::to_string(m_edgeOrdinal++);
		edge.source = source;
		edge.target = target;
		edge.kind = kind;
		edge.supported = supported;
		edge.reason = reason;
		m_cfg.edges.push_back(edge);
		Node(source).successors.push_back(target);
		Node(target).predecessors.push_back(source);
	}

	void ConnectAll(
		const std::vector<NodeId> &sources,
		const NodeId &target,
		PredaCFGEdgeKind kind,
		bool supported = true,
		const std::string &reason = std::string())
	{
		for (const NodeId &source : sources)
			AddEdge(source, target, kind, supported, reason);
	}

	std::vector<NodeId> BuildStatements(
		const std::vector<PredaParser::StatementContext *> &statements,
		std::vector<NodeId> tails)
	{
		for (PredaParser::StatementContext *statement : statements)
			tails = BuildStatement(statement, tails);
		return tails;
	}

	const PredaCFGConditionInput *FindCondition(
		PredaParser::ExpressionContext *expression) const
	{
		const SourceLocation location = GetLocation(expression);
		for (const PredaCFGConditionInput &condition :
			m_allInput.conditions)
		{
			if (condition.functionId == m_cfg.functionId &&
				condition.location.startOffset == location.startOffset &&
				condition.location.endOffset == location.endOffset)
			{
				return &condition;
			}
		}
		return nullptr;
	}

	void SetCondition(
		PredaCFGNode &node,
		PredaParser::ExpressionContext *expression)
	{
		node.hasCondition = expression != nullptr;
		if (expression == nullptr)
		{
			node.sourceExpression.kind = RelayExprKind::Literal;
			node.sourceExpression.text = "true";
			node.sourceExpression.type = "bool";
			node.sourceExpression.location = node.location;
			node.condition = refinement::FormulaExpr::BoolLiteral(
				true,
				"true",
				node.location);
			node.hasCondition = true;
			return;
		}
		const PredaCFGConditionInput *captured = FindCondition(expression);
		if (captured != nullptr)
		{
			node.sourceExpression = captured->expression;
			node.condition = captured->formula;
		}
		else
		{
			node.sourceExpression = BuildSyntaxExpression(expression, "bool");
			node.condition = refinement::RelayFormulaBuilder().Build(
				node.sourceExpression);
		}
		if (node.condition.IsUnknown())
			MarkConservative("control condition formula is unknown");
	}

	void ApplyExpressionEffects(
		PredaCFGNode &node,
		PredaParser::ExpressionContext *expression,
		bool localDeclaration = false)
	{
		if (localDeclaration)
			node.directEffect.writesLocalState = true;
		if (expression == nullptr)
			return;
		if (MayHaveImplicitRuntimeFailure(expression))
		{
			node.directEffect.mayAbortOrFail = true;
			if (node.directEffect.status == AnalysisStatus::Complete)
				node.directEffect.status = AnalysisStatus::Conservative;
			node.directEffect.reason =
				"index or division expression may fail at runtime";
			MarkConservative(node.directEffect.reason);
		}
		std::vector<MutationRange> mutations;
		CollectMutationRanges(expression, mutations);
		const SourceLocation range = GetLocation(expression);
		for (const PredaCFGIdentifierUseInput &use :
			m_allInput.identifierUses)
		{
			if (use.functionId != m_cfg.functionId ||
				!Contains(range, use.location) || use.functionSymbol)
			{
				continue;
			}
			bool written = false;
			bool alsoReads = true;
			for (const MutationRange &mutation : mutations)
			{
				if (Contains(mutation.location, use.location))
				{
					written = true;
					alsoReads = mutation.alsoReads;
					break;
				}
			}
			for (const PredaCFGCallFactInput &call : m_allInput.calls)
			{
				if (call.edge.caller == m_cfg.functionId &&
					call.edge.kind !=
						PredaCallKind::CompilerGeneratedHelper &&
					Contains(range, call.edge.callSite) &&
					Contains(call.edge.callSite, use.location))
				{
					// Function constness constrains the callee's current scope,
					// not every reference-like argument.  Until parameter-level
					// alias facts are persisted, conservatively treat state
					// values passed to any non-constructor call as possibly
					// written.  This deliberately over-approximates pure calls.
					written = true;
					alsoReads = true;
					break;
				}
			}
			if (use.stateVariable)
			{
				if (!written || alsoReads)
				{
					node.directEffect.readStateVariables.insert(
						use.stateVariableId);
					if (use.stateScope == ScopeType::Global)
						node.directEffect.readsGlobalState = true;
					else
						node.directEffect.readsCurrentScopeState = true;
				}
				if (written)
				{
					node.directEffect.writtenStateVariables.insert(
						use.stateVariableId);
					if (use.stateScope == ScopeType::Global)
						node.directEffect.writesGlobalState = true;
					else
						node.directEffect.writesCurrentScopeState = true;
				}
			}
			else if (written)
			{
				node.directEffect.writesLocalState = true;
				for (const LoopTarget &loop : m_loops)
				{
					if (!loop.inductionVariable.empty() &&
						loop.inductionVariable == use.name)
					{
						node.directEffect.modifiesLoopInductionVariable =
							true;
					}
				}
			}
		}
	}

	void ApplyIdentifierReadEffects(
		PredaCFGNode &node,
		const SourceLocation &range)
	{
		for (const PredaCFGIdentifierUseInput &use :
			m_allInput.identifierUses)
		{
			if (use.functionId != m_cfg.functionId ||
				!use.stateVariable || use.functionSymbol ||
				!Contains(range, use.location))
			{
				continue;
			}
			node.directEffect.readStateVariables.insert(
				use.stateVariableId);
			if (use.stateScope == ScopeType::Global)
				node.directEffect.readsGlobalState = true;
			else
				node.directEffect.readsCurrentScopeState = true;
		}
	}

	std::vector<const PredaCallEdge *> CallsIn(
		PredaParser::ExpressionContext *expression) const
	{
		std::vector<const PredaCallEdge *> result;
		if (expression == nullptr)
			return result;
		const SourceLocation range = GetLocation(expression);
		for (const PredaCFGCallFactInput &call : m_allInput.calls)
		{
			if (call.edge.caller == m_cfg.functionId &&
				Contains(range, call.edge.callSite))
			{
				result.push_back(&call.edge);
			}
		}
		std::stable_sort(
			result.begin(),
			result.end(),
			[](const PredaCallEdge *left, const PredaCallEdge *right)
			{
				const bool leftContainsRight =
					Contains(left->callSite, right->callSite) &&
					(left->callSite.startOffset !=
						right->callSite.startOffset ||
					 left->callSite.endOffset !=
						right->callSite.endOffset);
				const bool rightContainsLeft =
					Contains(right->callSite, left->callSite) &&
					(left->callSite.startOffset !=
						right->callSite.startOffset ||
					 left->callSite.endOffset !=
						right->callSite.endOffset);
				// Arguments and receivers are evaluated before invoking the
				// syntactically enclosing call.  Disjoint sibling calls have
				// no proved relative order and are handled below as Unknown.
				if (leftContainsRight)
					return false;
				if (rightContainsLeft)
					return true;
				if (left->callSite.startOffset != right->callSite.startOffset)
					return left->callSite.startOffset < right->callSite.startOffset;
				return left->callSite.endOffset < right->callSite.endOffset;
			});
		return result;
	}

	std::vector<NodeId> BuildCalls(
		PredaParser::ExpressionContext *expression,
		std::vector<NodeId> tails,
		bool incomingOrderProved = true)
	{
		std::vector<const PredaCallEdge *> calls = CallsIn(expression);
		bool ambiguousOrder = false;
		for (size_t first = 0; first < calls.size(); ++first)
		{
			for (size_t second = first + 1; second < calls.size(); ++second)
			{
				if (!Contains(calls[first]->callSite, calls[second]->callSite) &&
					!Contains(calls[second]->callSite, calls[first]->callSite))
				{
					ambiguousOrder = true;
				}
			}
		}
		if (ambiguousOrder)
		{
			MarkConservative(
				"multiple calls in one expression have no proved evaluation order");
		}
		bool firstEmittedCall = true;
		for (const PredaCallEdge *call : calls)
		{
			const std::pair<int64_t, int64_t> key{
				call->callSite.startOffset,
				call->callSite.endOffset};
			if (!m_emittedCallSites.insert(key).second)
				continue;
			PredaCFGNode node;
			node.location = call->callSite;
			node.calleeFunctionId = call->callee;
			if (call->kind == PredaCallKind::Synchronous && call->resolved)
			{
				node.kind = PredaCFGNodeKind::SynchronousCall;
			}
			else if (call->kind == PredaCallKind::RuntimeHelper)
			{
				node.kind = PredaCFGNodeKind::AbortOrFailure;
				node.directEffect.mayAbortOrFail = true;
				node.directEffect.mayHaveExternalEffect = true;
				if (!call->resolved)
				{
					node.directEffect.mayCallUnknown = true;
					node.directEffect.status = AnalysisStatus::Unknown;
				}
				else
				{
					node.directEffect.status = AnalysisStatus::Conservative;
				}
				MarkConservative(
					"runtime helper effects are conservatively modeled");
			}
			else if (call->kind == PredaCallKind::CompilerGeneratedHelper)
			{
				node.kind = PredaCFGNodeKind::Basic;
			}
			else
			{
				node.kind = PredaCFGNodeKind::Opaque;
				node.supported = false;
				node.unsupportedReason = call->unresolvedReason.empty()
					? "call has no local PREDA body"
					: call->unresolvedReason;
				node.directEffect.mayCallUnknown = true;
				node.directEffect.mayAbortOrFail = true;
				node.directEffect.mayHaveExternalEffect = true;
				node.directEffect.status = AnalysisStatus::Unknown;
				MarkConservative(node.unsupportedReason);
			}
			ApplyExpressionEffects(node, expression);
			const NodeId id = AddNode(std::move(node));
			const bool orderProved =
				!ambiguousOrder &&
				(!firstEmittedCall || incomingOrderProved);
			ConnectAll(
				tails,
				id,
				orderProved
					? PredaCFGEdgeKind::Fallthrough
					: PredaCFGEdgeKind::Unknown,
				orderProved,
				orderProved
					? std::string()
					: "no proved evaluation order for these calls");
			tails = {id};
			firstEmittedCall = false;
		}
		return tails;
	}

	std::vector<NodeId> BuildBasicExpression(
		PredaParser::ExpressionContext *expression,
		const SourceLocation &location,
		std::vector<NodeId> tails,
		bool localDeclaration,
		bool omitBasicForDirectCall)
	{
		std::vector<NodeId> afterCalls = BuildCalls(expression, tails);
		const bool directCall = expression != nullptr &&
			expression->expressionType ==
				static_cast<int>(PredaExpressionTypes::Parentheses) &&
			!CallsIn(expression).empty();
		if (omitBasicForDirectCall && directCall && !localDeclaration)
			return afterCalls;
		PredaCFGNode node;
		node.kind = PredaCFGNodeKind::Basic;
		node.location = location;
		node.sourceExpression = BuildSyntaxExpression(expression);
		ApplyExpressionEffects(node, expression, localDeclaration);
		if (ContainsExpressionKind(expression, PredaExpressionTypes::Deploy))
		{
			node.kind = PredaCFGNodeKind::AbortOrFailure;
			node.calleeFunctionId = "runtime::deploy";
			node.directEffect.mayAbortOrFail = true;
			node.directEffect.mayHaveExternalEffect = true;
			node.directEffect.status = AnalysisStatus::Conservative;
			MarkConservative("deploy has runtime and external effects");
		}
		else if (node.sourceExpression.kind == RelayExprKind::Opaque)
		{
			node.kind = PredaCFGNodeKind::Opaque;
			node.supported = false;
			node.unsupportedReason = node.sourceExpression.opaqueReason.empty()
				? "expression shape is not modeled"
				: node.sourceExpression.opaqueReason;
			node.directEffect.status = AnalysisStatus::Conservative;
			node.directEffect.reason = node.unsupportedReason;
			MarkConservative(node.unsupportedReason);
		}
		const NodeId id = AddNode(std::move(node));
		ConnectAll(afterCalls, id, PredaCFGEdgeKind::Fallthrough);
		return {id};
	}

	std::string RelaySiteId(
		PredaParser::RelayStatementContext *context) const
	{
		const SourceLocation location = GetLocation(context);
		for (const RelaySite &site : m_protocol.relaySites)
		{
			if (site.sourceFunctionId == m_cfg.functionId &&
				site.location.startOffset == location.startOffset &&
				site.location.endOffset == location.endOffset)
			{
				return site.id;
			}
		}
		return std::string();
	}

	void AddRegion(
		RegionKind kind,
		const SourceLocation &location,
		size_t firstNodeIndex,
		const std::string &suffix)
	{
		PredaCFGRegion region;
		region.id = "region::" + m_cfg.functionId + "::" + suffix + "::" +
			std::to_string(m_regionOrdinal++);
		region.kind = kind;
		region.sourceFunctionId = m_cfg.functionId;
		region.location = location;
		for (size_t index = firstNodeIndex;
			index < m_cfg.nodes.size();
			++index)
		{
			region.nodeIds.push_back(m_cfg.nodes[index].id);
		}
		m_cfg.regions.push_back(std::move(region));
	}

	std::vector<NodeId> BuildIf(
		PredaParser::IfStatementContext *context,
		std::vector<NodeId> tails)
	{
		PredaParser::IfWithBlockContext *first = context->ifWithBlock();
		tails = BuildCalls(first->expression(), tails);
		PredaCFGNode branch;
		branch.kind = PredaCFGNodeKind::Branch;
		branch.location = GetLocation(first->expression());
		SetCondition(branch, first->expression());
		ApplyExpressionEffects(branch, first->expression());
		NodeId currentBranch = AddNode(std::move(branch));
		ConnectAll(tails, currentBranch, PredaCFGEdgeKind::Fallthrough);

		PredaCFGNode joinNode;
		joinNode.kind = PredaCFGNodeKind::Basic;
		joinNode.location = GetLocation(context);
		const NodeId join = AddNode(std::move(joinNode));

		auto buildArm = [&](
			const NodeId &conditionNode,
			const std::vector<PredaParser::StatementContext *> &statements,
			const SourceLocation &location)
		{
			const size_t firstNode = m_cfg.nodes.size();
			std::vector<NodeId> armTails;
			if (statements.empty())
			{
				AddEdge(
					conditionNode,
					join,
					PredaCFGEdgeKind::TrueBranch);
			}
			else
			{
				armTails = BuildStatements(statements, {});
				const NodeId armEntry =
					m_cfg.nodes[firstNode].id;
				AddEdge(
					conditionNode,
					armEntry,
					PredaCFGEdgeKind::TrueBranch);
				ConnectAll(
					armTails,
					join,
					PredaCFGEdgeKind::Fallthrough);
			}
			AddRegion(
				RegionKind::BranchArm,
				location,
				firstNode,
				"branch_arm");
		};

		buildArm(
			currentBranch,
			first->statement(),
			GetLocation(first));

		for (PredaParser::ElseIfWithBlockContext *elseIf :
			context->elseIfWithBlock())
		{
			const size_t firstConditionCall = m_cfg.nodes.size();
			std::vector<NodeId> conditionTails =
				BuildCalls(elseIf->expression(), {});
			PredaCFGNode next;
			next.kind = PredaCFGNodeKind::Branch;
			next.location = GetLocation(elseIf->expression());
			SetCondition(next, elseIf->expression());
			ApplyExpressionEffects(next, elseIf->expression());
			const NodeId nextId = AddNode(std::move(next));
			if (!conditionTails.empty())
			{
				AddEdge(
					currentBranch,
					m_cfg.nodes[firstConditionCall].id,
					PredaCFGEdgeKind::FalseBranch);
				ConnectAll(
					conditionTails,
					nextId,
					PredaCFGEdgeKind::Fallthrough);
			}
			else
			{
				AddEdge(
					currentBranch,
					nextId,
					PredaCFGEdgeKind::FalseBranch);
			}
			currentBranch = nextId;
			buildArm(
				currentBranch,
				elseIf->statement(),
				GetLocation(elseIf));
		}

		PredaParser::ElseWithBlockContext *otherwise =
			context->elseWithBlock();
		if (otherwise == nullptr || otherwise->statement().empty())
		{
			AddEdge(
				currentBranch,
				join,
				PredaCFGEdgeKind::FalseBranch);
		}
		else
		{
			const size_t firstNode = m_cfg.nodes.size();
			std::vector<NodeId> elseTails =
				BuildStatements(otherwise->statement(), {});
			AddEdge(
				currentBranch,
				m_cfg.nodes[firstNode].id,
				PredaCFGEdgeKind::FalseBranch);
			ConnectAll(
				elseTails,
				join,
				PredaCFGEdgeKind::Fallthrough);
			AddRegion(
				RegionKind::BranchArm,
				GetLocation(otherwise),
				firstNode,
				"else_arm");
		}
		return {join};
	}

	std::vector<NodeId> BuildWhile(
		PredaParser::WhileStatementContext *context,
		std::vector<NodeId> tails)
	{
		const size_t firstConditionCall = m_cfg.nodes.size();
		tails = BuildCalls(context->expression(), tails);
		const bool hasConditionCalls =
			m_cfg.nodes.size() != firstConditionCall;
		PredaCFGNode headerNode;
		headerNode.kind = PredaCFGNodeKind::LoopHeader;
		headerNode.location = GetLocation(context->expression());
		SetCondition(headerNode, context->expression());
		ApplyExpressionEffects(headerNode, context->expression());
		const NodeId header = AddNode(std::move(headerNode));
		ConnectAll(tails, header, PredaCFGEdgeKind::Fallthrough);
		const NodeId conditionEntry = hasConditionCalls
			? m_cfg.nodes[firstConditionCall].id
			: header;

		PredaCFGNode latchNode;
		latchNode.kind = PredaCFGNodeKind::LoopLatch;
		latchNode.location = GetLocation(context);
		const NodeId latch = AddNode(std::move(latchNode));
		PredaCFGNode afterNode;
		afterNode.kind = PredaCFGNodeKind::Basic;
		afterNode.location = GetLocation(context);
		const NodeId after = AddNode(std::move(afterNode));

		LoopTarget loop;
		loop.loopId = "loop::" + header;
		loop.breakTarget = after;
		loop.continueTarget = conditionEntry;
		for (size_t index = firstConditionCall;
			index <= m_nodeIndex.at(header);
			++index)
		{
			m_cfg.nodes[index].enclosingLoopIds.push_back(loop.loopId);
		}
		Node(latch).enclosingLoopIds.push_back(loop.loopId);
		m_loops.push_back(loop);
		const size_t firstBodyNode = m_cfg.nodes.size();
		std::vector<NodeId> bodyTails =
			BuildStatements(context->statement(), {});
		if (context->statement().empty())
			AddEdge(header, latch, PredaCFGEdgeKind::TrueBranch);
		else
			AddEdge(
				header,
				m_cfg.nodes[firstBodyNode].id,
				PredaCFGEdgeKind::TrueBranch);
		ConnectAll(bodyTails, latch, PredaCFGEdgeKind::Fallthrough);
		AddEdge(latch, conditionEntry, PredaCFGEdgeKind::LoopBack);
		m_loops.pop_back();
		AddEdge(header, after, PredaCFGEdgeKind::FalseBranch);
		AddRegion(
			RegionKind::LoopBody,
			GetLocation(context),
			firstBodyNode,
			"while_body");
		return {after};
	}

	std::vector<NodeId> BuildFor(
		PredaParser::ForStatementContext *context,
		std::vector<NodeId> tails)
	{
		std::string inductionVariable;
		if (context->localVariableDeclaration() != nullptr)
		{
			PredaParser::LocalVariableDeclarationContext *declaration =
				context->localVariableDeclaration();
			inductionVariable = declaration->identifier()->getText();
			tails = BuildBasicExpression(
				declaration->expression(),
				GetLocation(declaration),
				tails,
				true,
				false);
		}
		else if (context->firstExpression != nullptr)
		{
			tails = BuildBasicExpression(
				context->firstExpression,
				GetLocation(context->firstExpression),
				tails,
				false,
				false);
		}

		const size_t firstConditionCall = m_cfg.nodes.size();
		tails = BuildCalls(context->secondExpression, tails);
		const bool hasConditionCalls =
			m_cfg.nodes.size() != firstConditionCall;
		PredaCFGNode headerNode;
		headerNode.kind = PredaCFGNodeKind::LoopHeader;
		headerNode.location = context->secondExpression == nullptr
			? GetLocation(context)
			: GetLocation(context->secondExpression);
		SetCondition(headerNode, context->secondExpression);
		ApplyExpressionEffects(headerNode, context->secondExpression);
		const NodeId header = AddNode(std::move(headerNode));
		ConnectAll(tails, header, PredaCFGEdgeKind::Fallthrough);
		const NodeId conditionEntry = hasConditionCalls
			? m_cfg.nodes[firstConditionCall].id
			: header;

		const size_t firstUpdateCall = m_cfg.nodes.size();
		std::vector<NodeId> updateTails =
			BuildCalls(context->thirdExpression, {});
		const bool hasUpdateCalls =
			m_cfg.nodes.size() != firstUpdateCall;
		PredaCFGNode latchNode;
		latchNode.kind = PredaCFGNodeKind::LoopLatch;
		latchNode.location = context->thirdExpression == nullptr
			? GetLocation(context)
			: GetLocation(context->thirdExpression);
		latchNode.sourceExpression =
			BuildSyntaxExpression(context->thirdExpression);
		ApplyExpressionEffects(latchNode, context->thirdExpression);
		if (!inductionVariable.empty() &&
			context->thirdExpression != nullptr &&
			!context->thirdExpression->expression().empty() &&
			DirectPrimaryIdentifier(
				context->thirdExpression->expression(0)) == inductionVariable)
		{
			latchNode.directEffect.modifiesLoopInductionVariable = true;
		}
		const NodeId latch = AddNode(std::move(latchNode));
		if (hasUpdateCalls)
			ConnectAll(updateTails, latch, PredaCFGEdgeKind::Fallthrough);
		PredaCFGNode afterNode;
		afterNode.kind = PredaCFGNodeKind::Basic;
		afterNode.location = GetLocation(context);
		const NodeId after = AddNode(std::move(afterNode));

		LoopTarget loop;
		loop.loopId = "loop::" + header;
		loop.breakTarget = after;
		loop.continueTarget = hasUpdateCalls
			? m_cfg.nodes[firstUpdateCall].id
			: latch;
		loop.inductionVariable = inductionVariable;
		for (size_t index = firstConditionCall;
			index <= m_nodeIndex.at(header);
			++index)
		{
			m_cfg.nodes[index].enclosingLoopIds.push_back(loop.loopId);
		}
		for (size_t index = firstUpdateCall;
			index <= m_nodeIndex.at(latch);
			++index)
		{
			m_cfg.nodes[index].enclosingLoopIds.push_back(loop.loopId);
		}
		m_loops.push_back(loop);
		const size_t firstBodyNode = m_cfg.nodes.size();
		std::vector<NodeId> bodyTails =
			BuildStatements(context->statement(), {});
		if (context->statement().empty())
			AddEdge(
				header,
				loop.continueTarget,
				PredaCFGEdgeKind::TrueBranch);
		else
			AddEdge(
				header,
				m_cfg.nodes[firstBodyNode].id,
				PredaCFGEdgeKind::TrueBranch);
		ConnectAll(
			bodyTails,
			loop.continueTarget,
			PredaCFGEdgeKind::Fallthrough);
		AddEdge(latch, conditionEntry, PredaCFGEdgeKind::LoopBack);
		m_loops.pop_back();
		if (context->secondExpression != nullptr)
			AddEdge(header, after, PredaCFGEdgeKind::FalseBranch);
		AddRegion(
			RegionKind::LoopBody,
			GetLocation(context),
			firstBodyNode,
			"for_body");
		return {after};
	}

	std::vector<NodeId> BuildDoWhile(
		PredaParser::DoWhileStatementContext *context,
		std::vector<NodeId> tails)
	{
		PredaCFGNode headerNode;
		headerNode.kind = PredaCFGNodeKind::LoopHeader;
		headerNode.location = GetLocation(context);
		const NodeId header = AddNode(std::move(headerNode));
		ConnectAll(tails, header, PredaCFGEdgeKind::Fallthrough);

		const size_t firstConditionCall = m_cfg.nodes.size();
		std::vector<NodeId> conditionTails =
			BuildCalls(context->expression(), {});
		const bool hasConditionCalls =
			m_cfg.nodes.size() != firstConditionCall;
		PredaCFGNode latchNode;
		latchNode.kind = PredaCFGNodeKind::LoopLatch;
		latchNode.location = GetLocation(context->expression());
		SetCondition(latchNode, context->expression());
		ApplyExpressionEffects(latchNode, context->expression());
		const NodeId latch = AddNode(std::move(latchNode));
		const NodeId conditionEntry = hasConditionCalls
			? m_cfg.nodes[firstConditionCall].id
			: latch;
		if (hasConditionCalls)
			ConnectAll(
				conditionTails,
				latch,
				PredaCFGEdgeKind::Fallthrough);
		PredaCFGNode afterNode;
		afterNode.kind = PredaCFGNodeKind::Basic;
		afterNode.location = GetLocation(context);
		const NodeId after = AddNode(std::move(afterNode));

		LoopTarget loop;
		loop.loopId = "loop::" + header;
		loop.breakTarget = after;
		loop.continueTarget = conditionEntry;
		Node(header).enclosingLoopIds.push_back(loop.loopId);
		for (size_t index = firstConditionCall;
			index <= m_nodeIndex.at(latch);
			++index)
		{
			m_cfg.nodes[index].enclosingLoopIds.push_back(loop.loopId);
		}
		m_loops.push_back(loop);
		const size_t firstBodyNode = m_cfg.nodes.size();
		std::vector<NodeId> bodyTails =
			BuildStatements(context->statement(), {header});
		ConnectAll(bodyTails, conditionEntry, PredaCFGEdgeKind::Fallthrough);
		AddEdge(latch, header, PredaCFGEdgeKind::LoopBack);
		AddEdge(latch, after, PredaCFGEdgeKind::FalseBranch);
		m_loops.pop_back();
		AddRegion(
			RegionKind::LoopBody,
			GetLocation(context),
			firstBodyNode,
			"do_while_body");
		return {after};
	}

	std::vector<NodeId> BuildStatement(
		PredaParser::StatementContext *statement,
		std::vector<NodeId> tails)
	{
		if (statement == nullptr)
			return tails;
		if (statement->ifStatement() != nullptr)
			return BuildIf(statement->ifStatement(), tails);
		if (statement->whileStatement() != nullptr)
			return BuildWhile(statement->whileStatement(), tails);
		if (statement->forStatement() != nullptr)
			return BuildFor(statement->forStatement(), tails);
		if (statement->doWhileStatement() != nullptr)
			return BuildDoWhile(statement->doWhileStatement(), tails);
		if (statement->userBlockStatement() != nullptr)
		{
			return BuildStatements(
				statement->userBlockStatement()->statement(),
				tails);
		}
		if (statement->variableDeclarationStatement() != nullptr)
		{
			PredaParser::LocalVariableDeclarationContext *declaration =
				statement->variableDeclarationStatement()
					->localVariableDeclaration();
			return BuildBasicExpression(
				declaration->expression(),
				GetLocation(statement),
				tails,
				true,
				false);
		}
		if (statement->expressionStatement() != nullptr)
		{
			PredaParser::ExpressionContext *expression =
				statement->expressionStatement()->expression();
			return BuildBasicExpression(
				expression,
				GetLocation(statement),
				tails,
				false,
				true);
		}
		if (statement->returnStatement() != nullptr)
		{
			PredaParser::ReturnStatementContext *returnStatement =
				statement->returnStatement();
			tails = BuildCalls(returnStatement->expression(), tails);
			PredaCFGNode node;
			node.kind = PredaCFGNodeKind::Return;
			node.location = GetLocation(returnStatement);
			node.directEffect.mayReturnEarly = true;
			node.sourceExpression =
				BuildSyntaxExpression(returnStatement->expression());
			ApplyExpressionEffects(node, returnStatement->expression());
			const NodeId id = AddNode(std::move(node));
			ConnectAll(tails, id, PredaCFGEdgeKind::Fallthrough);
			AddEdge(id, m_cfg.exitNodeId, PredaCFGEdgeKind::ReturnExit);
			return {};
		}
		if (statement->breakStatement() != nullptr)
		{
			PredaCFGNode node;
			node.kind = PredaCFGNodeKind::Break;
			node.location = GetLocation(statement);
			node.directEffect.mayBreak = true;
			const NodeId id = AddNode(std::move(node));
			ConnectAll(tails, id, PredaCFGEdgeKind::Fallthrough);
			if (m_loops.empty())
			{
				MarkUnsupported("break has no enclosing loop target");
				AddEdge(
					id,
					m_cfg.exitNodeId,
					PredaCFGEdgeKind::Unknown,
					false,
					"break target is unresolved");
			}
			else
			{
				AddEdge(
					id,
					m_loops.back().breakTarget,
					PredaCFGEdgeKind::BreakExit);
			}
			return {};
		}
		if (statement->continueStatement() != nullptr)
		{
			PredaCFGNode node;
			node.kind = PredaCFGNodeKind::Continue;
			node.location = GetLocation(statement);
			node.directEffect.mayContinue = true;
			const NodeId id = AddNode(std::move(node));
			ConnectAll(tails, id, PredaCFGEdgeKind::Fallthrough);
			if (m_loops.empty())
			{
				MarkUnsupported("continue has no enclosing loop target");
				AddEdge(
					id,
					m_cfg.exitNodeId,
					PredaCFGEdgeKind::Unknown,
					false,
					"continue target is unresolved");
			}
			else
			{
				AddEdge(
					id,
					m_loops.back().continueTarget,
					PredaCFGEdgeKind::ContinueBack);
			}
			return {};
		}
		if (statement->relayStatement() != nullptr)
		{
			PredaParser::RelayStatementContext *relay =
				statement->relayStatement();
			// Every target, named argument, and lambda capture initializer is
			// evaluated before the asynchronous emit.  PREDA lowering does not
			// prove a relative order across distinct operands, so cross-operand
			// call edges are explicitly Unknown.
			std::vector<PredaParser::ExpressionContext *> operands;
			if (relay->relayType()->expression() != nullptr)
				operands.push_back(relay->relayType()->expression());
			if (relay->functionCallArguments() != nullptr)
			{
				for (PredaParser::ExpressionContext *argument :
					relay->functionCallArguments()->expression())
				{
					operands.push_back(argument);
				}
			}
			if (relay->relayLambdaDefinition() != nullptr)
			{
				for (PredaParser::RelayLambdaParameterContext *parameter :
					relay->relayLambdaDefinition()->relayLambdaParameter())
				{
					if (parameter->expression() != nullptr)
						operands.push_back(parameter->expression());
				}
			}
			bool sawOperandCall = false;
			for (PredaParser::ExpressionContext *operand : operands)
			{
				const bool hasCalls = !CallsIn(operand).empty();
				tails = BuildCalls(
					operand,
					tails,
					!sawOperandCall);
				if (hasCalls && sawOperandCall)
				{
					MarkConservative(
						"relay operand call order is not proved");
				}
				sawOperandCall = sawOperandCall || hasCalls;
			}
			PredaCFGNode node;
			node.kind = PredaCFGNodeKind::RelayEmit;
			node.location = GetLocation(relay);
			node.relaySiteId = RelaySiteId(relay);
			node.directEffect.mayEmitRelay = true;
			node.directEffect.mayAbortOrFail = true;
			for (PredaParser::ExpressionContext *operand : operands)
				ApplyExpressionEffects(node, operand);
			// Caret captures (`^state`) have no ExpressionContext in the
			// grammar.  The exact semantic identifier-use facts still carry
			// their source range, so include all state reads in the relay
			// statement range to preserve capture effects.
			ApplyIdentifierReadEffects(node, GetLocation(relay));
			if (node.relaySiteId.empty())
			{
				node.supported = false;
				node.unsupportedReason =
					"relay statement has no collected RelaySite";
				MarkUnsupported(node.unsupportedReason);
			}
			const NodeId id = AddNode(std::move(node));
			ConnectAll(tails, id, PredaCFGEdgeKind::Fallthrough);
			MarkConservative(
				"relay emission may fail through the PREDA runtime");
			return {id};
		}

		PredaCFGNode opaque;
		opaque.kind = PredaCFGNodeKind::Opaque;
		opaque.location = GetLocation(statement);
		opaque.supported = false;
		opaque.unsupportedReason =
			"unsupported PREDA statement parse-tree alternative";
		opaque.directEffect.mayCallUnknown = true;
		opaque.directEffect.mayAbortOrFail = true;
		opaque.directEffect.mayHaveExternalEffect = true;
		opaque.directEffect.status = AnalysisStatus::Unknown;
		const NodeId id = AddNode(std::move(opaque));
		ConnectAll(
			tails,
			id,
			PredaCFGEdgeKind::Unknown,
			false,
			"unsupported statement");
		MarkUnsupported("unsupported PREDA statement parse-tree alternative");
		return {id};
	}
};

} // namespace

PredaControlFlowIR PredaCFGBuilder::Build(
	const PredaCFGBuilderInput &input,
	const RelayProtocolIR &protocol) const
{
	PredaControlFlowIR result;
	{
		metrics::ScopedRelayAnalysisPhase timer(
			metrics::RelayAnalysisPhase::CFGConstruction);
		for (const PredaCFGFunctionInput &function : input.functions)
		{
			result.functions.push_back(
				FunctionBuilder(function, input, protocol).Build());
			result.callGraph.functions.push_back(function.functionId);
		}
	}
	{
		metrics::ScopedRelayAnalysisPhase timer(
			metrics::RelayAnalysisPhase::CallGraphConstruction);
		for (const PredaCFGCallFactInput &call : input.calls)
			result.callGraph.edges.push_back(call.edge);
		for (const PredaFunctionCFG &function : result.functions)
		{
			for (const PredaCFGNode &node : function.nodes)
			{
				if (node.calleeFunctionId != "runtime::deploy")
					continue;
				PredaCallEdge deploy;
				deploy.id = "call::deploy::" + node.id;
				deploy.caller = function.functionId;
				deploy.callee = node.calleeFunctionId;
				deploy.kind = PredaCallKind::RuntimeHelper;
				deploy.callSite = node.location;
				deploy.sourceText = node.sourceExpression.text;
				deploy.resolved = true;
				result.callGraph.edges.push_back(std::move(deploy));
			}
		}
		for (const RelayProtocolEdge &relay : protocol.edges)
		{
			PredaCallEdge edge;
			edge.id = "call::relay::" + relay.id;
			edge.caller = relay.sourceFunctionId;
			edge.kind = PredaCallKind::Relay;
			edge.resolved = relay.resolved;
			for (const RelaySite &site : protocol.relaySites)
			{
				if (site.id == relay.relaySiteId)
				{
					edge.callSite = site.location;
					break;
				}
			}
			for (const RelayHandler &handler : protocol.handlers)
			{
				if (handler.id == relay.handlerId)
				{
					edge.callee = handler.targetFunctionId;
					break;
				}
			}
			if (!edge.resolved)
				edge.unresolvedReason = "relay handler is unresolved";
			result.callGraph.edges.push_back(std::move(edge));
		}
		PredaCallGraphAnalyzer::Analyze(
			result.callGraph,
			result.functions);
	}
	{
		metrics::ScopedRelayAnalysisPhase timer(
			metrics::RelayAnalysisPhase::EffectAnalysis);
		result.regionEffects = PredaEffectAnalysis::Build(
			result.functions,
			result.callGraph,
			protocol);
	}
	{
		metrics::ScopedRelayAnalysisPhase timer(
			metrics::RelayAnalysisPhase::ICFGConstruction);
		result.relayIcfg = RelayICFGBuilder::Build(
			result.functions,
			result.callGraph,
			protocol);
	}
	return result;
}

} // namespace cfg
} // namespace relay_protocol
} // namespace transpiler
