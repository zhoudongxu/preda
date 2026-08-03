#include "RelayWorkBoundAnalyzer.h"

#include "../RelayProtocolIR.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>
#include <map>
#include <set>

namespace transpiler {
namespace relay_protocol {
namespace certificate {
namespace {

using analysis::RelayCardinalityExpr;
using analysis::RelayCardinalityExprKind;

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

RelayCardinalityExpr Sum(std::vector<RelayCardinalityExpr> terms)
{
	uint64_t constant = 0;
	std::vector<RelayCardinalityExpr> nonConstants;
	for (RelayCardinalityExpr &term : terms)
	{
		if (term.kind == RelayCardinalityExprKind::Unknown)
			return term;
		if (term.kind == RelayCardinalityExprKind::Constant)
		{
			uint64_t next = 0;
			if (!CheckedAdd(constant, term.value, next))
				return RelayCardinalityExpr::Unknown("relay work sum overflow");
			constant = next;
		}
		else
		{
			nonConstants.push_back(std::move(term));
		}
	}
	if (constant != 0 || nonConstants.empty())
		nonConstants.push_back(RelayCardinalityExpr::Constant(constant));
	if (nonConstants.size() == 1)
		return std::move(nonConstants.front());
	RelayCardinalityExpr result;
	result.kind = RelayCardinalityExprKind::Sum;
	result.children = std::move(nonConstants);
	return result;
}

RelayCardinalityExpr Product(
	uint64_t coefficient,
	RelayCardinalityExpr expression)
{
	if (coefficient == 0)
		return RelayCardinalityExpr::Constant(0);
	if (expression.kind == RelayCardinalityExprKind::Unknown)
		return expression;
	if (expression.kind == RelayCardinalityExprKind::Constant)
	{
		uint64_t product = 0;
		if (!CheckedMultiply(coefficient, expression.value, product))
			return RelayCardinalityExpr::Unknown("relay work product overflow");
		return RelayCardinalityExpr::Constant(product);
	}
	if (coefficient == 1)
		return expression;
	RelayCardinalityExpr result;
	result.kind = RelayCardinalityExprKind::Product;
	result.children.push_back(RelayCardinalityExpr::Constant(coefficient));
	result.children.push_back(std::move(expression));
	return result;
}

bool IsUnknown(const RelayCardinalityExpr &expression)
{
	if (expression.kind == RelayCardinalityExprKind::Unknown)
		return true;
	for (const RelayCardinalityExpr &child : expression.children)
		if (IsUnknown(child))
			return true;
	return false;
}

void SortUnique(std::vector<std::string> &values)
{
	std::sort(values.begin(), values.end());
	values.erase(std::unique(values.begin(), values.end()), values.end());
}

const FunctionProtocol *FindProtocolFunction(
	const RelayProtocolIR &protocol,
	const std::string &functionId)
{
	for (const FunctionProtocol &function : protocol.functions)
		if (function.sourceFunctionId == functionId)
			return &function;
	return nullptr;
}

const RelayHandler *FindHandler(
	const RelayProtocolIR &protocol,
	const std::string &handlerId)
{
	for (const RelayHandler &handler : protocol.handlers)
		if (handler.id == handlerId)
			return &handler;
	return nullptr;
}

const cfg::PredaFunctionCFG *FindCfgFunction(
	const RelayProtocolIR &protocol,
	const std::string &functionId)
{
	for (const cfg::PredaFunctionCFG &function : protocol.controlFlow.functions)
		if (function.functionId == functionId)
			return &function;
	return nullptr;
}

const cfg::PredaCFGNode *FindNode(
	const cfg::PredaFunctionCFG &function,
	const cfg::NodeId &nodeId)
{
	for (const cfg::PredaCFGNode &node : function.nodes)
		if (node.id == nodeId)
			return &node;
	return nullptr;
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
	const std::from_chars_result result =
		std::from_chars(begin, end, value, base);
	return result.ec == std::errc() && result.ptr == end;
}

bool LoopTripCount(const LoopProtocol &loop, uint64_t &tripCount)
{
	uint64_t initial = 0;
	uint64_t bound = 0;
	if (!loop.staticallyBounded ||
		!ParseUnsignedLiteral(loop.initialValue, initial) ||
		!ParseUnsignedLiteral(loop.boundValue, bound))
	{
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
	return false;
}

bool SiteOccurrenceUpper(const RelaySite &site, uint64_t &upper)
{
	upper = 1;
	for (const LoopProtocol &loop : site.loops)
	{
		uint64_t tripCount = 0;
		uint64_t next = 0;
		if (!LoopTripCount(loop, tripCount) ||
			!CheckedMultiply(upper, tripCount, next))
		{
			return false;
		}
		upper = next;
	}
	return true;
}

struct MicrotransactionSites
{
	bool supported = true;
	bool exact = true;
	std::vector<std::pair<const RelaySite *, uint64_t>> sites;
	std::vector<std::string> evidence;
	std::string reason;
};

struct DirectWorkResult
{
	RelayCardinalityExpr exact = RelayCardinalityExpr::Constant(0);
	RelayCardinalityExpr upper = RelayCardinalityExpr::Constant(0);
	bool exactKnown = true;
	bool supported = true;
	std::vector<std::string> evidence;
	std::string reason;
};

struct PhysicalResult
{
	bool supported = true;
	uint64_t constant = 0;
	uint64_t shardCoefficient = 0;
	std::vector<std::string> evidence;
	std::string reason;
};

class WorkState
{
public:
	explicit WorkState(const RelayProtocolIR &protocol)
		: m_protocol(protocol)
	{
	}

	MicrotransactionSites CollectSites(const std::string &rootFunctionId)
	{
		MicrotransactionSites result;
		std::set<std::string> visiting;
		CollectFunction(rootFunctionId, visiting, result);
		std::sort(result.evidence.begin(), result.evidence.end());
		result.evidence.erase(
			std::unique(result.evidence.begin(), result.evidence.end()),
			result.evidence.end());
		return result;
	}

	DirectWorkResult DirectWork(const std::string &rootFunctionId)
	{
		std::set<std::string> visiting;
		return BuildDirect(rootFunctionId, visiting);
	}

	RelayCardinalityExpr TransitiveUpper(
		const std::string &rootFunctionId,
		std::vector<std::string> &evidence,
		std::string &reason)
	{
		std::set<std::string> visiting;
		return BuildTransitive(rootFunctionId, visiting, evidence, reason);
	}

	PhysicalResult PhysicalUpper(const std::string &rootFunctionId)
	{
		std::set<std::string> visiting;
		return BuildPhysical(rootFunctionId, visiting);
	}

private:
	const cfg::PredaFunctionGraphAnalysis *FunctionAnalysis(
		const std::string &functionId) const
	{
		for (const cfg::PredaFunctionGraphAnalysis &analysis :
			m_protocol.controlFlow.relayIcfg.functionAnalyses)
			if (analysis.functionId == functionId)
				return &analysis;
		return nullptr;
	}

	bool LocalCountExactUnderCfg(
		const cfg::PredaFunctionCFG &function,
		const FunctionProtocol &local,
		std::string &reason) const
	{
		const cfg::PredaFunctionGraphAnalysis *analysis =
			FunctionAnalysis(function.functionId);
		if (function.status == cfg::AnalysisStatus::Unsupported ||
			function.status == cfg::AnalysisStatus::Unknown ||
			function.effect.status == cfg::AnalysisStatus::Unsupported ||
			function.effect.status == cfg::AnalysisStatus::Unknown ||
			analysis == nullptr ||
			analysis->status == cfg::AnalysisStatus::Unsupported ||
			analysis->status == cfg::AnalysisStatus::Unknown ||
			!analysis->reachabilityComplete)
		{
			reason =
				"local relay count requires a complete CFG and reachability analysis";
			return false;
		}
		for (const cfg::PredaCFGNode &node : function.nodes)
		{
			if (!node.supported || node.kind == cfg::PredaCFGNodeKind::Opaque)
			{
				reason = "CFG contains an unsupported node";
				return false;
			}
		}
		for (const cfg::PredaCFGEdge &edge : function.edges)
		{
			if (!edge.supported || edge.kind == cfg::PredaCFGEdgeKind::Unknown)
			{
				reason = "CFG contains an unsupported control-flow edge";
				return false;
			}
		}
		if (function.effect.mayReturnEarly ||
			function.effect.mayBreak ||
			function.effect.mayContinue ||
			function.effect.mayAbortOrFail ||
			function.effect.mayCallUnknown ||
			function.effect.mayHaveExternalEffect)
		{
			reason =
				"CFG contains an early-exit, failure, or unknown path that may bypass a relay";
			return false;
		}

		const auto entryReachable =
			analysis->reachableNodes.find(function.entryNodeId);
		if (entryReachable == analysis->reachableNodes.end())
		{
			reason = "CFG entry reachability facts are unavailable";
			return false;
		}
		std::map<std::string, size_t> cfgOccurrences;
		for (const cfg::PredaCFGNode &node : function.nodes)
		{
			if (node.kind != cfg::PredaCFGNodeKind::RelayEmit)
				continue;
			if (std::find(
					entryReachable->second.begin(),
					entryReachable->second.end(),
					node.id) == entryReachable->second.end())
			{
				reason = "FunctionProtocol contains a relay site unreachable in the CFG";
				return false;
			}
			++cfgOccurrences[node.relaySiteId];
		}
		if (cfgOccurrences.size() != local.relaySiteIds.size())
		{
			reason = "FunctionProtocol and CFG relay-site sets do not match";
			return false;
		}
		for (const std::string &siteId : local.relaySiteIds)
		{
			const auto occurrence = cfgOccurrences.find(siteId);
			if (occurrence == cfgOccurrences.end() || occurrence->second != 1)
			{
				reason =
					"FunctionProtocol relay site has no unique reachable CFG node: " +
					siteId;
				return false;
			}
		}
		return true;
	}

	bool LocalSiteUpper(
		const cfg::PredaFunctionCFG &function,
		uint64_t &upper) const
	{
		if (function.effect.mayCallUnknown)
			return false;
		const auto unresolved = m_protocol.controlFlow.callGraph.analysis
			.hasUnresolvedOutgoing.find(function.functionId);
		if (unresolved != m_protocol.controlFlow.callGraph.analysis
			.hasUnresolvedOutgoing.end() && unresolved->second)
		{
			return false;
		}
		upper = 0;
		for (const RelaySite &site : m_protocol.relaySites)
		{
			if (site.sourceFunctionId != function.functionId)
				continue;
			uint64_t occurrenceUpper = 0;
			uint64_t next = 0;
			if (!SiteOccurrenceUpper(site, occurrenceUpper) ||
				!CheckedAdd(upper, occurrenceUpper, next))
			{
				return false;
			}
			upper = next;
		}
		return true;
	}

	bool CallIsUnconditional(
		const std::string &functionId,
		const cfg::NodeId &callNodeId) const
	{
		const cfg::PredaFunctionCFG *function =
			FindCfgFunction(m_protocol, functionId);
		const cfg::PredaFunctionGraphAnalysis *analysis =
			FunctionAnalysis(functionId);
		if (function == nullptr || analysis == nullptr ||
			analysis->status != cfg::AnalysisStatus::Complete ||
			!analysis->dominanceComplete)
		{
			return false;
		}
		const auto exitDominators =
			analysis->dominators.find(function->exitNodeId);
		return exitDominators != analysis->dominators.end() &&
			std::find(
				exitDominators->second.begin(),
				exitDominators->second.end(),
				callNodeId) != exitDominators->second.end();
	}

	DirectWorkResult BuildDirect(
		const std::string &functionId,
		std::set<std::string> &visiting)
	{
		DirectWorkResult result;
		if (!visiting.insert(functionId).second)
		{
			result.supported = false;
			result.exactKnown = false;
			result.exact = RelayCardinalityExpr::Unknown(
				"recursive synchronous call graph");
			result.upper = result.exact;
			result.reason =
				"recursive synchronous call graph has no finite direct-work proof";
			return result;
		}
		const cfg::PredaFunctionCFG *function =
			FindCfgFunction(m_protocol, functionId);
		if (function == nullptr ||
			function->status == cfg::AnalysisStatus::Unsupported ||
			function->status == cfg::AnalysisStatus::Unknown)
		{
			result.supported = false;
			result.exactKnown = false;
			result.exact = RelayCardinalityExpr::Unknown("function CFG is incomplete");
			result.upper = result.exact;
			result.reason = "function CFG is missing or incomplete: " + functionId;
			visiting.erase(functionId);
			return result;
		}
		result.evidence.push_back(functionId);
		const FunctionProtocol *local =
			FindProtocolFunction(m_protocol, functionId);
		if (local != nullptr)
		{
			result.exact = local->summary.relayCount;
			result.upper = local->summary.relayCountUpperBound;
			result.exactKnown = !IsUnknown(result.exact);
			result.supported = !IsUnknown(result.upper);
			if (!result.supported)
			{
				uint64_t cfgUpper = 0;
				if (LocalSiteUpper(*function, cfgUpper))
				{
					result.upper = RelayCardinalityExpr::Constant(cfgUpper);
					result.supported = true;
					result.reason =
						"finite CFG relay-site occurrence upper bound; exact listener protocol is incomplete";
				}
			}
			std::string cfgReason;
			if (result.exactKnown &&
				!LocalCountExactUnderCfg(*function, *local, cfgReason))
			{
				result.exactKnown = false;
				result.exact = RelayCardinalityExpr::Unknown(cfgReason);
				result.reason = cfgReason;
			}
		}
		else
		{
			for (const RelaySite &site : m_protocol.relaySites)
			{
				if (site.sourceFunctionId == functionId)
				{
					result.supported = false;
					result.exactKnown = false;
					result.exact = RelayCardinalityExpr::Unknown(
						"relay-owning FunctionProtocol is unavailable");
					result.upper = result.exact;
					break;
				}
			}
		}
		if (!result.supported)
		{
			result.reason = "local direct relay count has no finite upper bound";
			visiting.erase(functionId);
			return result;
		}

		for (const cfg::PredaInterproceduralEdge &edge :
			m_protocol.controlFlow.relayIcfg.interproceduralEdges)
		{
			if (edge.kind != cfg::InterproceduralEdgeKind::SyncCall ||
				edge.sourceFunctionId != functionId)
				continue;
			const cfg::PredaCFGNode *callNode =
				FindNode(*function, edge.sourceNodeId);
			if (!edge.resolved || callNode == nullptr ||
				!callNode->enclosingLoopIds.empty())
			{
				result.supported = false;
				result.exactKnown = false;
				result.reason =
					"unresolved or loop-indexed synchronous call: " + edge.id;
				break;
			}
			DirectWorkResult callee = BuildDirect(
				edge.targetFunctionId, visiting);
			if (!callee.supported)
			{
				result = std::move(callee);
				break;
			}
			result.upper = Sum({result.upper, callee.upper});
			result.evidence.push_back(edge.id);
			result.evidence.insert(
				result.evidence.end(),
				callee.evidence.begin(), callee.evidence.end());
			if (result.exactKnown && callee.exactKnown &&
				CallIsUnconditional(functionId, edge.sourceNodeId))
			{
				result.exact = Sum({result.exact, callee.exact});
			}
			else
			{
				result.exactKnown = false;
				result.exact = RelayCardinalityExpr::Unknown(
					"conditional synchronous call requires composed path cardinality");
				result.reason =
					"conditional synchronous call requires composed path cardinality";
			}
		}
		visiting.erase(functionId);
		return result;
	}

	void CollectFunction(
		const std::string &functionId,
		std::set<std::string> &visiting,
		MicrotransactionSites &result)
	{
		if (!result.supported)
			return;
		if (!visiting.insert(functionId).second)
		{
			result.supported = false;
			result.reason = "recursive synchronous call graph has no finite work proof";
			return;
		}
		const cfg::PredaFunctionCFG *function =
			FindCfgFunction(m_protocol, functionId);
		const auto unresolved = m_protocol.controlFlow.callGraph.analysis
			.hasUnresolvedOutgoing.find(functionId);
		if (function == nullptr ||
			function->status == cfg::AnalysisStatus::Unsupported ||
			function->status == cfg::AnalysisStatus::Unknown ||
			function->effect.mayCallUnknown ||
			(unresolved != m_protocol.controlFlow.callGraph.analysis
				.hasUnresolvedOutgoing.end() && unresolved->second))
		{
			result.supported = false;
			result.reason = "function CFG is missing or incomplete: " + functionId;
			visiting.erase(functionId);
			return;
		}
		result.evidence.push_back(functionId);
		for (const RelaySite &site : m_protocol.relaySites)
		{
			if (site.sourceFunctionId != functionId)
				continue;
			uint64_t upper = 0;
			if (!SiteOccurrenceUpper(site, upper))
			{
				result.supported = false;
				result.reason =
					"relay site has no finite occurrence upper bound: " + site.id;
				break;
			}
			result.sites.emplace_back(&site, upper);
			result.exact = result.exact && site.branches.empty() &&
				site.loops.empty();
		}
		for (const cfg::PredaInterproceduralEdge &edge :
			m_protocol.controlFlow.relayIcfg.interproceduralEdges)
		{
			if (edge.kind != cfg::InterproceduralEdgeKind::SyncCall ||
				edge.sourceFunctionId != functionId)
			{
				continue;
			}
			if (!edge.resolved)
			{
				result.supported = false;
				result.reason = "unresolved synchronous call: " + edge.id;
				break;
			}
			const cfg::PredaCFGNode *callNode =
				FindNode(*function, edge.sourceNodeId);
			if (callNode == nullptr || !callNode->enclosingLoopIds.empty())
			{
				result.supported = false;
				result.reason =
					"synchronous call occurrence is missing or loop-indexed: " + edge.id;
				break;
			}
			result.evidence.push_back(edge.id);
			// A branch-guarded call contributes safely to an upper bound, but
			// not to an exact direct count without a composed symbolic path.
			if (!callNode->predecessors.empty())
			{
				for (const cfg::NodeId &predecessor : callNode->predecessors)
				{
					const cfg::PredaCFGNode *pre = FindNode(*function, predecessor);
					if (pre != nullptr && pre->kind == cfg::PredaCFGNodeKind::Branch)
						result.exact = false;
				}
			}
			CollectFunction(edge.targetFunctionId, visiting, result);
		}
		visiting.erase(functionId);
	}

	RelayCardinalityExpr BuildTransitive(
		const std::string &functionId,
		std::set<std::string> &visiting,
		std::vector<std::string> &evidence,
		std::string &reason)
	{
		if (!visiting.insert(functionId).second)
		{
			reason = "async relay handler recursion has no finite work proof";
			return RelayCardinalityExpr::Unknown(reason);
		}
		MicrotransactionSites direct = CollectSites(functionId);
		if (!direct.supported)
		{
			reason = direct.reason;
			visiting.erase(functionId);
			return RelayCardinalityExpr::Unknown(reason);
		}
		evidence.insert(
			evidence.end(), direct.evidence.begin(), direct.evidence.end());
		std::vector<RelayCardinalityExpr> terms;
		for (const auto &occurrence : direct.sites)
		{
			const RelaySite &site = *occurrence.first;
			const RelayHandler *handler = FindHandler(m_protocol, site.handlerId);
			if (handler == nullptr || !handler->resolved ||
				handler->targetFunctionId.empty())
			{
				reason = "relay handler is unresolved: " + site.handlerId;
				visiting.erase(functionId);
				return RelayCardinalityExpr::Unknown(reason);
			}
			if (site.relayKind == RelayKind::Shards &&
				(!handler->relayReachabilityKnown || handler->mayEmitRelay))
			{
				reason =
					"all-shards handler emits relays, but broadcast-clone logical subtree collapse is not modeled";
				visiting.erase(functionId);
				return RelayCardinalityExpr::Unknown(reason);
			}
			RelayCardinalityExpr descendant = BuildTransitive(
				handler->targetFunctionId, visiting, evidence, reason);
			if (IsUnknown(descendant))
			{
				// A handler known not to relay has zero descendants even if no
				// FunctionProtocol was needed for it.
				if (handler->relayReachabilityKnown && !handler->mayEmitRelay)
					descendant = RelayCardinalityExpr::Constant(0);
				else
				{
					visiting.erase(functionId);
					return descendant;
				}
			}
			terms.push_back(Product(
				occurrence.second,
				Sum({RelayCardinalityExpr::Constant(1), std::move(descendant)})));
		}
		visiting.erase(functionId);
		return Sum(std::move(terms));
	}

	PhysicalResult BuildPhysical(
		const std::string &functionId,
		std::set<std::string> &visiting)
	{
		PhysicalResult result;
		if (!visiting.insert(functionId).second)
		{
			result.supported = false;
			result.reason = "async relay handler recursion has no finite route bound";
			return result;
		}
		MicrotransactionSites direct = CollectSites(functionId);
		if (!direct.supported)
		{
			result.supported = false;
			result.reason = direct.reason;
			visiting.erase(functionId);
			return result;
		}
		result.evidence = direct.evidence;
		for (const auto &occurrence : direct.sites)
		{
			const RelaySite &site = *occurrence.first;
			const RelayHandler *handler = FindHandler(m_protocol, site.handlerId);
			if (handler == nullptr || !handler->resolved ||
				handler->targetFunctionId.empty())
			{
				result.supported = false;
				result.reason = "relay handler is unresolved: " + site.handlerId;
				break;
			}
			PhysicalResult descendant = BuildPhysical(
				handler->targetFunctionId, visiting);
			if (!descendant.supported)
			{
				if (handler->relayReachabilityKnown && !handler->mayEmitRelay)
					descendant = PhysicalResult();
				else
				{
					result = std::move(descendant);
					break;
				}
			}
			result.evidence.insert(
				result.evidence.end(),
				descendant.evidence.begin(), descendant.evidence.end());
			uint64_t next = 0;
			if (site.relayKind == RelayKind::Shards)
			{
				if (descendant.shardCoefficient != 0)
				{
					result.supported = false;
					result.reason =
						"broadcast descendants require a nonlinear active-shard bound";
					break;
				}
				uint64_t perShard = 0;
				if (!CheckedAdd(uint64_t(1), descendant.constant, perShard) ||
					!CheckedMultiply(occurrence.second, perShard, perShard) ||
					!CheckedAdd(result.shardCoefficient, perShard, next))
				{
					result.supported = false;
					result.reason = "physical broadcast route bound overflow";
					break;
				}
				result.shardCoefficient = next;
			}
			else
			{
				uint64_t perOccurrence = 0;
				uint64_t contribution = 0;
				if (!CheckedAdd(uint64_t(1), descendant.constant, perOccurrence) ||
					!CheckedMultiply(occurrence.second, perOccurrence, contribution) ||
					!CheckedAdd(result.constant, contribution, next))
				{
					result.supported = false;
					result.reason = "physical route constant overflow";
					break;
				}
				result.constant = next;
				if (!CheckedMultiply(
						occurrence.second,
						descendant.shardCoefficient,
						contribution) ||
					!CheckedAdd(result.shardCoefficient, contribution, next))
				{
					result.supported = false;
					result.reason = "physical route coefficient overflow";
					break;
				}
				result.shardCoefficient = next;
			}
		}
		visiting.erase(functionId);
		return result;
	}

	const RelayProtocolIR &m_protocol;
};

std::string BoundId(const std::string &functionId, const std::string &kind)
{
	return "parallel.bound." + functionId + "." + kind;
}

} // namespace

void RelayWorkBoundAnalyzer::Analyze(
	const RelayProtocolIR &protocol,
	FunctionParallelRelayCertificate &certificate) const
{
	certificate.directWorkBound.id = BoundId(
		certificate.sourceFunctionId, "direct_logical_work");
	certificate.transitiveWorkBound.id = BoundId(
		certificate.sourceFunctionId, "transitive_logical_work");
	certificate.physicalRouteWork.id = BoundId(
		certificate.sourceFunctionId, "physical_route_work");

	WorkState state(protocol);
	DirectWorkResult directBound = state.DirectWork(
		certificate.sourceFunctionId);
	SortUnique(directBound.evidence);
	if (!directBound.supported)
	{
		certificate.directWorkBound.exact = directBound.exact;
		certificate.directWorkBound.upperBound = directBound.upper;
		certificate.directWorkBound.reason = directBound.reason;
	}
	else
	{
		certificate.directWorkBound.exact = directBound.exact;
		certificate.directWorkBound.upperBound = directBound.upper;
		certificate.directWorkBound.supportingCfgFactIds =
			directBound.evidence;
		certificate.directWorkBound.status = directBound.exactKnown
			? CertificateStatus::Complete
			: CertificateStatus::Conservative;
		certificate.directWorkBound.reason = directBound.exactKnown
			? "direct logical relay count composed with unconditional synchronous helpers"
			: (directBound.reason.empty()
				? "finite direct logical upper bound; exact occurrence is not CFG-proved"
				: directBound.reason);
	}

	PhysicalResult physical = state.PhysicalUpper(
		certificate.sourceFunctionId);
	SortUnique(physical.evidence);
	if (!physical.supported)
	{
		certificate.physicalRouteWork.reason = physical.reason;
	}
	else
	{
		certificate.physicalRouteWork.status = CertificateStatus::Conservative;
		certificate.physicalRouteWork.constantTerm = physical.constant;
		certificate.physicalRouteWork.activeShardCountCoefficient =
			physical.shardCoefficient;
		certificate.physicalRouteWork.supportingCfgFactIds = physical.evidence;
		if (physical.shardCoefficient == 0)
		{
			certificate.physicalRouteWork.boundKind =
				PhysicalWorkBoundKind::Constant;
			certificate.physicalRouteWork.expression =
				std::to_string(physical.constant);
		}
		else
		{
			certificate.physicalRouteWork.boundKind =
				PhysicalWorkBoundKind::ParameterizedUpperBound;
			certificate.physicalRouteWork.expression =
				std::to_string(physical.constant) + " + " +
				std::to_string(physical.shardCoefficient) +
				" * active_shard_count";
		}
		certificate.physicalRouteWork.reason =
			"whole relay-tree physical route upper bound";
	}

	std::vector<std::string> transitiveEvidence;
	std::string transitiveReason;
	RelayCardinalityExpr transitive = state.TransitiveUpper(
		certificate.sourceFunctionId,
		transitiveEvidence,
		transitiveReason);
	if (IsUnknown(transitive))
	{
		certificate.transitiveWorkBound.exact = transitive;
		certificate.transitiveWorkBound.upperBound = transitive;
		certificate.transitiveWorkBound.reason = transitiveReason;
		return;
	}
	SortUnique(transitiveEvidence);
	certificate.transitiveWorkBound.upperBound = transitive;
	certificate.transitiveWorkBound.supportingCfgFactIds =
		std::move(transitiveEvidence);
	if (transitive.kind == RelayCardinalityExprKind::Constant &&
		transitive.value == 0)
	{
		certificate.transitiveWorkBound.exact = transitive;
		certificate.transitiveWorkBound.status = CertificateStatus::Complete;
		certificate.transitiveWorkBound.reason = "relay tree has zero logical work";
	}
	else
	{
		certificate.transitiveWorkBound.status = CertificateStatus::Conservative;
		certificate.transitiveWorkBound.reason =
			"finite async relay-tree logical work upper bound";
		// Exact transitive work requires occurrence-sensitive branch composition;
		// retain Unknown unless the whole tree is trivially empty.
		certificate.transitiveWorkBound.exact = RelayCardinalityExpr::Unknown(
			"transitive exact work is not inferred without occurrence-indexed paths");
	}
}

} // namespace certificate
} // namespace relay_protocol
} // namespace transpiler
