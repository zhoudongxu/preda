#include "RelaySummaryBuilder.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
#include <set>
#include <unordered_map>
#include <utility>

namespace transpiler {
namespace relay_protocol {
namespace analysis {
namespace {

bool CheckedAdd(uint64_t left, uint64_t right, uint64_t &result)
{
	if (right > std::numeric_limits<uint64_t>::max() - left)
		return false;
	result = left + right;
	return true;
}

bool CheckedMultiply(uint64_t left, uint64_t right, uint64_t &result)
{
	if (left != 0 && right > std::numeric_limits<uint64_t>::max() / left)
		return false;
	result = left * right;
	return true;
}

bool ContainsOpaqueExpression(const RelayExprIR &expression)
{
	if (expression.kind == RelayExprKind::Opaque)
		return true;
	for (const RelayExprIR &child : expression.children)
	{
		if (ContainsOpaqueExpression(child))
			return true;
	}
	return false;
}

bool IsStaticTargetExpression(const RelayExprIR &expression)
{
	switch (expression.kind)
	{
	case RelayExprKind::Literal:
		return true;
	case RelayExprKind::Keyword:
		return expression.text == "global" || expression.text == "shards";
	case RelayExprKind::Group:
	case RelayExprKind::Unary:
	case RelayExprKind::Binary:
		if (expression.children.empty())
			return false;
		for (const RelayExprIR &child : expression.children)
		{
			if (!IsStaticTargetExpression(child))
				return false;
		}
		return true;
	case RelayExprKind::Identifier:
	case RelayExprKind::MemberAccess:
	case RelayExprKind::Index:
	case RelayExprKind::Call:
	case RelayExprKind::Opaque:
	default:
		return false;
	}
}

bool IsTargetKnownBeforeExecution(const RelaySite &site)
{
	switch (site.relayKind)
	{
	case RelayKind::Global:
	case RelayKind::Shards:
		return true;
	case RelayKind::Next:
		return false;
	case RelayKind::CustomScope:
	default:
		return IsStaticTargetExpression(site.target);
	}
}

RelayCardinalityExpr MakeSum(std::vector<RelayCardinalityExpr> terms)
{
	std::vector<RelayCardinalityExpr> flattened;
	uint64_t constant = 0;
	for (RelayCardinalityExpr &term : terms)
	{
		if (term.kind == RelayCardinalityExprKind::Sum)
		{
			for (RelayCardinalityExpr &child : term.children)
				flattened.push_back(std::move(child));
			continue;
		}
		if (term.kind == RelayCardinalityExprKind::Constant)
		{
			uint64_t next = 0;
			if (!CheckedAdd(constant, term.value, next))
			{
				return RelayCardinalityExpr::Unknown(
					"relay cardinality addition exceeds uint64 range");
			}
			constant = next;
			continue;
		}
		flattened.push_back(std::move(term));
	}

	if (constant != 0)
		flattened.push_back(RelayCardinalityExpr::Constant(constant));
	if (flattened.empty())
		return RelayCardinalityExpr::Constant(0);
	if (flattened.size() == 1)
		return std::move(flattened.front());

	RelayCardinalityExpr result;
	result.kind = RelayCardinalityExprKind::Sum;
	result.children = std::move(flattened);
	return result;
}

RelayCardinalityExpr MakeProduct(std::vector<RelayCardinalityExpr> factors)
{
	std::vector<RelayCardinalityExpr> flattened;
	uint64_t constant = 1;
	for (RelayCardinalityExpr &factor : factors)
	{
		if (factor.kind == RelayCardinalityExprKind::Constant &&
			factor.value == 0)
		{
			return RelayCardinalityExpr::Constant(0);
		}
		if (factor.kind == RelayCardinalityExprKind::Product)
		{
			for (RelayCardinalityExpr &child : factor.children)
				flattened.push_back(std::move(child));
			continue;
		}
		if (factor.kind == RelayCardinalityExprKind::Constant)
		{
			uint64_t next = 0;
			if (!CheckedMultiply(constant, factor.value, next))
			{
				return RelayCardinalityExpr::Unknown(
					"relay cardinality multiplication exceeds uint64 range");
			}
			constant = next;
			continue;
		}
		flattened.push_back(std::move(factor));
	}

	if (constant == 0)
		return RelayCardinalityExpr::Constant(0);
	if (constant != 1 || flattened.empty())
		flattened.push_back(RelayCardinalityExpr::Constant(constant));
	if (flattened.size() == 1)
		return std::move(flattened.front());

	RelayCardinalityExpr result;
	result.kind = RelayCardinalityExprKind::Product;
	result.children = std::move(flattened);
	return result;
}

RelayCardinalityExpr MakeIte(
	const RelayExprIR &predicate,
	bool polarity,
	RelayCardinalityExpr body)
{
	if (body.kind == RelayCardinalityExprKind::Constant && body.value == 0)
		return body;

	RelayCardinalityExpr result;
	result.kind = RelayCardinalityExprKind::Ite;
	result.predicate = predicate;
	if (polarity)
	{
		result.children.push_back(std::move(body));
		result.children.push_back(RelayCardinalityExpr::Constant(0));
	}
	else
	{
		result.children.push_back(RelayCardinalityExpr::Constant(0));
		result.children.push_back(std::move(body));
	}
	return result;
}

bool CardinalityContainsUnknown(const RelayCardinalityExpr &expression)
{
	if (expression.kind == RelayCardinalityExprKind::Unknown)
		return true;
	for (const RelayCardinalityExpr &child : expression.children)
	{
		if (CardinalityContainsUnknown(child))
			return true;
	}
	return false;
}

RelayDepthExpr MakeMaximum(std::vector<RelayDepthExpr> expressions)
{
	std::vector<RelayDepthExpr> flattened;
	uint64_t constant = 0;
	for (RelayDepthExpr &expression : expressions)
	{
		if (expression.kind == RelayDepthExprKind::Unknown)
			return expression;
		if (expression.kind == RelayDepthExprKind::Maximum)
		{
			for (RelayDepthExpr &child : expression.children)
				flattened.push_back(std::move(child));
			continue;
		}
		if (expression.kind == RelayDepthExprKind::Constant)
		{
			constant = std::max(constant, expression.value);
			continue;
		}
		flattened.push_back(std::move(expression));
	}

	if (constant != 0 || flattened.empty())
		flattened.push_back(RelayDepthExpr::Constant(constant));
	if (flattened.size() == 1)
		return std::move(flattened.front());

	RelayDepthExpr result;
	result.kind = RelayDepthExprKind::Maximum;
	result.children = std::move(flattened);
	return result;
}

RelayDepthExpr MakeSuccessor(RelayDepthExpr expression)
{
	if (expression.kind == RelayDepthExprKind::Unknown)
		return expression;
	if (expression.kind == RelayDepthExprKind::Constant)
	{
		if (expression.value == std::numeric_limits<uint64_t>::max())
		{
			return RelayDepthExpr::Unknown(
				"relay nesting depth exceeds uint64 range");
		}
		return RelayDepthExpr::Constant(expression.value + 1);
	}

	RelayDepthExpr result;
	result.kind = RelayDepthExprKind::Successor;
	result.children.push_back(std::move(expression));
	return result;
}

bool DepthContainsUnknown(const RelayDepthExpr &expression)
{
	if (expression.kind == RelayDepthExprKind::Unknown)
		return true;
	for (const RelayDepthExpr &child : expression.children)
	{
		if (DepthContainsUnknown(child))
			return true;
	}
	return false;
}

bool IsValidIntegerSuffix(const std::string &suffix)
{
	if (suffix.empty() || suffix == "ib")
		return true;
	if (suffix[0] != 'u' && suffix[0] != 'i')
		return false;
	const std::string width = suffix.substr(1);
	return width.empty() ||
		width == "8" ||
		width == "16" ||
		width == "32" ||
		width == "64" ||
		width == "128" ||
		width == "256" ||
		width == "512";
}

bool ParseUnsignedLiteral(const std::string &source, uint64_t &value)
{
	std::string text;
	text.reserve(source.size());
	for (unsigned char character : source)
	{
		if (std::isspace(character) == 0)
			text.push_back(static_cast<char>(character));
	}
	if (text.empty())
		return false;

	int base = 10;
	size_t digitsBegin = 0;
	size_t digitsEnd = 0;
	if (text.size() >= 2 && text[0] == '0' &&
		(text[1] == 'x' || text[1] == 'X'))
	{
		base = 16;
		digitsBegin = 2;
		digitsEnd = digitsBegin;
		while (digitsEnd < text.size() &&
			std::isxdigit(static_cast<unsigned char>(text[digitsEnd])) != 0)
		{
			++digitsEnd;
		}
	}
	else
	{
		while (digitsEnd < text.size() &&
			std::isdigit(static_cast<unsigned char>(text[digitsEnd])) != 0)
		{
			++digitsEnd;
		}
	}
	if (digitsEnd == digitsBegin ||
		!IsValidIntegerSuffix(text.substr(digitsEnd)))
	{
		return false;
	}

	const char *begin = text.data() + digitsBegin;
	const char *end = text.data() + digitsEnd;
	const std::from_chars_result parsed =
		std::from_chars(begin, end, value, base);
	return parsed.ec == std::errc() && parsed.ptr == end;
}

bool ComputeTripCount(
	const LoopProtocol &loop,
	uint64_t &tripCount,
	std::string &reason)
{
	if (!loop.staticallyBounded)
	{
		reason = loop.opaqueReason.empty()
			? "loop has no statically established finite bound"
			: loop.opaqueReason;
		return false;
	}

	uint64_t initial = 0;
	uint64_t bound = 0;
	if (!ParseUnsignedLiteral(loop.initialValue, initial) ||
		!ParseUnsignedLiteral(loop.boundValue, bound))
	{
		reason =
			"bounded loop literal cannot be represented as a uint64 trip count";
		return false;
	}

	if (loop.comparison == "<" && loop.step == "+1")
	{
		tripCount = initial < bound ? bound - initial : 0;
		return true;
	}
	if (loop.comparison == ">" && loop.step == "-1")
	{
		tripCount = initial > bound ? initial - bound : 0;
		return true;
	}

	reason = "loop does not have a supported strict unit-step trip count";
	return false;
}

bool NodeContainsOpaque(const ProtocolNode &node)
{
	if (node.kind == ProtocolNodeKind::Opaque)
		return true;
	if (node.kind == ProtocolNodeKind::Branch &&
		ContainsOpaqueExpression(node.condition))
	{
		return true;
	}
	if (node.kind == ProtocolNodeKind::Repeat &&
		(ContainsOpaqueExpression(node.loop.initializer) ||
			ContainsOpaqueExpression(node.loop.condition) ||
			ContainsOpaqueExpression(node.loop.update)))
	{
		return true;
	}
	for (const ProtocolNode &child : node.children)
	{
		if (NodeContainsOpaque(child))
			return true;
	}
	return false;
}

bool SiteContainsOpaque(const RelaySite &site)
{
	if (ContainsOpaqueExpression(site.target))
		return true;
	for (const RelayArgument &argument : site.arguments)
	{
		if (ContainsOpaqueExpression(argument.expression))
			return true;
	}
	for (const BranchCondition &branch : site.branches)
	{
		if (ContainsOpaqueExpression(branch.condition))
			return true;
	}
	for (const LoopProtocol &loop : site.loops)
	{
		if (ContainsOpaqueExpression(loop.initializer) ||
			ContainsOpaqueExpression(loop.condition) ||
			ContainsOpaqueExpression(loop.update))
		{
			return true;
		}
	}
	return false;
}

struct CardinalityResult
{
	RelayCardinalityExpr exact;
	RelayCardinalityExpr upperBound;
};

enum class DepthVisitState : uint8_t
{
	Unvisited,
	Visiting,
	Complete,
};

class SummaryBuildState
{
public:
	explicit SummaryBuildState(RelayProtocolIR &protocol)
		: m_protocol(protocol)
	{
		for (const RelaySite &site : protocol.relaySites)
			m_sites.emplace(site.id, &site);
		for (const RelayHandler &handler : protocol.handlers)
			m_handlers.emplace(handler.id, &handler);
		for (FunctionProtocol &function : protocol.functions)
			m_functions.emplace(function.sourceFunctionId, &function);
	}

	void Build()
	{
		for (FunctionProtocol &function : m_protocol.functions)
		{
			RelayProtocolSummary summary;
			const CardinalityResult cardinality =
				BuildCardinality(function.root);
			summary.relayCount = cardinality.exact;
			summary.relayCountUpperBound = cardinality.upperBound;
			summary.maxDepth = BuildFunctionDepth(function.sourceFunctionId);
			BuildLocalFacts(function, summary);
			SetAnalysisStatus(summary);
			function.summary = std::move(summary);
		}
	}

private:
	CardinalityResult BuildCardinality(const ProtocolNode &node) const
	{
		switch (node.kind)
		{
		case ProtocolNodeKind::End:
		case ProtocolNodeKind::Call:
			return {
				RelayCardinalityExpr::Constant(0),
				RelayCardinalityExpr::Constant(0),
			};

		case ProtocolNodeKind::Emit:
		{
			std::vector<RelayCardinalityExpr> exactTerms;
			std::vector<RelayCardinalityExpr> upperTerms;
			exactTerms.push_back(RelayCardinalityExpr::Constant(1));
			upperTerms.push_back(RelayCardinalityExpr::Constant(1));
			for (const ProtocolNode &child : node.children)
			{
				// The current Emit->Call edge names the asynchronous handler.
				// It is used for depth, not for the source function's direct
				// relay count.
				if (child.kind == ProtocolNodeKind::Call)
					continue;
				CardinalityResult continuation = BuildCardinality(child);
				exactTerms.push_back(std::move(continuation.exact));
				upperTerms.push_back(std::move(continuation.upperBound));
			}
			return {
				MakeSum(std::move(exactTerms)),
				MakeSum(std::move(upperTerms)),
			};
		}

		case ProtocolNodeKind::Sequence:
		case ProtocolNodeKind::Parallel:
		{
			std::vector<RelayCardinalityExpr> exactTerms;
			std::vector<RelayCardinalityExpr> upperTerms;
			for (const ProtocolNode &child : node.children)
			{
				CardinalityResult childResult = BuildCardinality(child);
				exactTerms.push_back(std::move(childResult.exact));
				upperTerms.push_back(std::move(childResult.upperBound));
			}
			return {
				MakeSum(std::move(exactTerms)),
				MakeSum(std::move(upperTerms)),
			};
		}

		case ProtocolNodeKind::Branch:
		{
			std::vector<RelayCardinalityExpr> exactTerms;
			std::vector<RelayCardinalityExpr> upperTerms;
			for (const ProtocolNode &child : node.children)
			{
				CardinalityResult childResult = BuildCardinality(child);
				exactTerms.push_back(std::move(childResult.exact));
				upperTerms.push_back(std::move(childResult.upperBound));
			}
			RelayCardinalityExpr bodyExact =
				MakeSum(std::move(exactTerms));
			RelayCardinalityExpr bodyUpper =
				MakeSum(std::move(upperTerms));
			if (ContainsOpaqueExpression(node.condition))
			{
				return {
					RelayCardinalityExpr::Unknown(
						"branch predicate is opaque"),
					std::move(bodyUpper),
				};
			}
			return {
				MakeIte(
					node.condition,
					node.conditionPolarity,
					std::move(bodyExact)),
				std::move(bodyUpper),
			};
		}

		case ProtocolNodeKind::Repeat:
		{
			std::vector<RelayCardinalityExpr> exactTerms;
			std::vector<RelayCardinalityExpr> upperTerms;
			for (const ProtocolNode &child : node.children)
			{
				CardinalityResult childResult = BuildCardinality(child);
				exactTerms.push_back(std::move(childResult.exact));
				upperTerms.push_back(std::move(childResult.upperBound));
			}
			RelayCardinalityExpr bodyExact =
				MakeSum(std::move(exactTerms));
			RelayCardinalityExpr bodyUpper =
				MakeSum(std::move(upperTerms));

			uint64_t tripCount = 0;
			std::string reason;
			if (!ComputeTripCount(node.loop, tripCount, reason))
			{
				return {
					RelayCardinalityExpr::Unknown(reason),
					RelayCardinalityExpr::Unknown(reason),
				};
			}

			RelayCardinalityExpr upper = MakeProduct({
				RelayCardinalityExpr::Constant(tripCount),
				std::move(bodyUpper),
			});
			if (tripCount == 0)
			{
				return {
					CardinalityContainsUnknown(bodyExact)
						? RelayCardinalityExpr::Unknown(
							"zero-trip loop body contains an unknown protocol node")
						: RelayCardinalityExpr::Constant(0),
					std::move(upper),
				};
			}
			if (node.loop.bodyMayExitEarly)
			{
				return {
					RelayCardinalityExpr::Unknown(
						"bounded loop body may exit before all iterations"),
					std::move(upper),
				};
			}
			return {
				MakeProduct({
					RelayCardinalityExpr::Constant(tripCount),
					std::move(bodyExact),
				}),
				std::move(upper),
			};
		}

		case ProtocolNodeKind::Opaque:
			return {
				RelayCardinalityExpr::Unknown(
					node.opaqueReason.empty()
						? "opaque protocol node"
						: node.opaqueReason),
				RelayCardinalityExpr::Unknown(
					node.opaqueReason.empty()
						? "opaque protocol node has no finite relay-count bound"
						: node.opaqueReason),
			};
		}

		return {
			RelayCardinalityExpr::Unknown("unsupported protocol node"),
			RelayCardinalityExpr::Unknown(
				"unsupported protocol node has no relay-count bound"),
		};
	}

	RelayDepthExpr BuildFunctionDepth(const std::string &functionId)
	{
		const auto cached = m_depthMemo.find(functionId);
		if (cached != m_depthMemo.end())
			return cached->second;

		DepthVisitState &state = m_depthStates[functionId];
		if (state == DepthVisitState::Visiting)
		{
			return RelayDepthExpr::Unknown(
				"recursive relay handler cycle reaches " + functionId);
		}

		const auto function = m_functions.find(functionId);
		if (function == m_functions.end())
		{
			return RelayDepthExpr::Unknown(
				"relay handler function protocol is unavailable: " +
				functionId);
		}
		if (function->second->hasUnmodeledRelayReachableCall)
		{
			return RelayDepthExpr::Unknown(
				"function has an ordinary call to relay-reachable code "
				"that is not represented in the relay protocol: " +
				functionId);
		}

		state = DepthVisitState::Visiting;
		RelayDepthExpr result = BuildNodeDepth(function->second->root);
		state = DepthVisitState::Complete;
		m_depthMemo.emplace(functionId, result);
		return result;
	}

	RelayDepthExpr BuildNodeDepth(const ProtocolNode &node)
	{
		switch (node.kind)
		{
		case ProtocolNodeKind::End:
		case ProtocolNodeKind::Call:
			return RelayDepthExpr::Constant(0);

		case ProtocolNodeKind::Emit:
		{
			RelayDepthExpr emittedDepth =
				BuildEmittedRelayDepth(node.relaySiteId);
			std::vector<RelayDepthExpr> depths;
			depths.push_back(std::move(emittedDepth));
			for (const ProtocolNode &child : node.children)
			{
				if (child.kind != ProtocolNodeKind::Call)
					depths.push_back(BuildNodeDepth(child));
			}
			return MakeMaximum(std::move(depths));
		}

		case ProtocolNodeKind::Branch:
		case ProtocolNodeKind::Sequence:
		case ProtocolNodeKind::Parallel:
		{
			std::vector<RelayDepthExpr> depths;
			for (const ProtocolNode &child : node.children)
				depths.push_back(BuildNodeDepth(child));
			return MakeMaximum(std::move(depths));
		}

		case ProtocolNodeKind::Repeat:
		{
			std::vector<RelayDepthExpr> depths;
			for (const ProtocolNode &child : node.children)
				depths.push_back(BuildNodeDepth(child));
			RelayDepthExpr bodyDepth =
				MakeMaximum(std::move(depths));

			uint64_t tripCount = 0;
			std::string reason;
			if (ComputeTripCount(node.loop, tripCount, reason) &&
				tripCount == 0)
			{
				if (DepthContainsUnknown(bodyDepth))
					return bodyDepth;
				return RelayDepthExpr::Constant(0);
			}
			return bodyDepth;
		}

		case ProtocolNodeKind::Opaque:
			return RelayDepthExpr::Unknown(
				node.opaqueReason.empty()
					? "opaque protocol node"
					: node.opaqueReason);
		}
		return RelayDepthExpr::Unknown("unsupported protocol node");
	}

	RelayDepthExpr BuildEmittedRelayDepth(const std::string &relaySiteId)
	{
		const auto site = m_sites.find(relaySiteId);
		if (site == m_sites.end())
		{
			return RelayDepthExpr::Unknown(
				"Emit node references an unknown relay site: " +
				relaySiteId);
		}

		const auto handler = m_handlers.find(site->second->handlerId);
		if (handler == m_handlers.end())
		{
			return RelayDepthExpr::Unknown(
				"relay site references an unknown handler: " +
				site->second->handlerId);
		}
		const RelayHandler &target = *handler->second;
		if (!target.resolved)
		{
			return RelayDepthExpr::Unknown(
				"relay handler is unresolved: " + target.id);
		}

		if (!target.targetFunctionId.empty())
		{
			const auto function = m_functions.find(target.targetFunctionId);
			if (function != m_functions.end())
				return MakeSuccessor(
					BuildFunctionDepth(target.targetFunctionId));
		}

		if (target.relayReachabilityKnown && !target.mayEmitRelay)
			return RelayDepthExpr::Constant(1);

		return RelayDepthExpr::Unknown(
			target.relayReachabilityKnown
				? "relay handler may emit another relay but no function protocol is available: " +
					target.id
				: "relay reachability is unknown for handler: " + target.id);
	}

	void BuildLocalFacts(
		const FunctionProtocol &function,
		RelayProtocolSummary &summary) const
	{
		summary.relaySiteIds = function.relaySiteIds;
		std::sort(
			summary.relaySiteIds.begin(),
			summary.relaySiteIds.end());
		summary.relaySiteIds.erase(
			std::unique(
				summary.relaySiteIds.begin(),
				summary.relaySiteIds.end()),
			summary.relaySiteIds.end());

		std::set<ScopeType> scopeKinds;
		std::set<RelayFanoutKind> fanoutKinds;
		summary.targetsKnownBeforeExecution = true;
		summary.hasOpaque = NodeContainsOpaque(function.root);
		for (const std::string &siteId : summary.relaySiteIds)
		{
			const auto site = m_sites.find(siteId);
			if (site == m_sites.end())
			{
				summary.targetsKnownBeforeExecution = false;
				summary.hasOpaque = true;
				continue;
			}
			scopeKinds.insert(site->second->targetScope);
			fanoutKinds.insert(
				site->second->relayKind == RelayKind::Shards
					? RelayFanoutKind::AllShards
					: RelayFanoutKind::SingleTarget);
			summary.targetsKnownBeforeExecution =
				summary.targetsKnownBeforeExecution &&
				IsTargetKnownBeforeExecution(*site->second);
			summary.hasOpaque =
				summary.hasOpaque || SiteContainsOpaque(*site->second);
		}
		summary.targetScopeKinds.assign(
			scopeKinds.begin(),
			scopeKinds.end());
		summary.fanoutKinds.assign(
			fanoutKinds.begin(),
			fanoutKinds.end());
		summary.hasUnmodeledRelayReachableCall =
			function.hasUnmodeledRelayReachableCall;

		if (summary.relaySiteIds.size() <= 1)
		{
			summary.ordering.status = RelayOrderingStatus::Trivial;
			summary.ordering.reason =
				"at most one distinct relay site occurs in this function";
		}
		else
		{
			summary.ordering.status = RelayOrderingStatus::Unknown;
			summary.ordering.reason =
				"listener discovery order is not a proved execution order";
		}
		// In particular, do not create must-precede edges from Sequence child
		// indexes or source offsets. A future CFG pass may add proved edges.
		summary.ordering.mustPrecede.clear();
	}

	void SetAnalysisStatus(RelayProtocolSummary &summary) const
	{
		const bool countUnknown =
			CardinalityContainsUnknown(summary.relayCount);
		const bool upperUnknown =
			CardinalityContainsUnknown(summary.relayCountUpperBound);
		const bool depthUnknown =
			DepthContainsUnknown(summary.maxDepth);
		const bool orderingUnknown =
			summary.ordering.status == RelayOrderingStatus::Unknown;

		if (!countUnknown &&
			!upperUnknown &&
			!depthUnknown &&
			!summary.hasOpaque &&
			!summary.hasUnmodeledRelayReachableCall &&
			summary.targetsKnownBeforeExecution &&
			!orderingUnknown)
		{
			summary.analysisStatus = RelayAnalysisStatus::Exact;
		}
		else if (countUnknown && upperUnknown && depthUnknown)
		{
			summary.analysisStatus = RelayAnalysisStatus::Unknown;
		}
		else
		{
			summary.analysisStatus = RelayAnalysisStatus::Conservative;
		}
	}

	RelayProtocolIR &m_protocol;
	std::unordered_map<std::string, const RelaySite *> m_sites;
	std::unordered_map<std::string, const RelayHandler *> m_handlers;
	std::unordered_map<std::string, FunctionProtocol *> m_functions;
	std::unordered_map<std::string, RelayDepthExpr> m_depthMemo;
	std::unordered_map<std::string, DepthVisitState> m_depthStates;
};

} // namespace

void RelaySummaryBuilder::Build(RelayProtocolIR &protocol) const
{
	SummaryBuildState state(protocol);
	state.Build();
}

} // namespace analysis
} // namespace relay_protocol
} // namespace transpiler
