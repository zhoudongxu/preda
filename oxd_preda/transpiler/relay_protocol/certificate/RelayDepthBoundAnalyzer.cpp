#include "RelayDepthBoundAnalyzer.h"

#include "../RelayProtocolIR.h"

#include <algorithm>
#include <limits>
#include <set>

namespace transpiler {
namespace relay_protocol {
namespace certificate {
namespace {

using analysis::RelayDepthExpr;
using analysis::RelayDepthExprKind;

bool IsUnknown(const RelayDepthExpr &expression)
{
	if (expression.kind == RelayDepthExprKind::Unknown)
		return true;
	for (const RelayDepthExpr &child : expression.children)
		if (IsUnknown(child))
			return true;
	return false;
}

RelayDepthExpr Maximum(std::vector<RelayDepthExpr> expressions)
{
	uint64_t constant = 0;
	std::vector<RelayDepthExpr> symbolic;
	for (RelayDepthExpr &expression : expressions)
	{
		if (IsUnknown(expression))
			return expression;
		if (expression.kind == RelayDepthExprKind::Constant)
			constant = std::max(constant, expression.value);
		else
			symbolic.push_back(std::move(expression));
	}
	if (constant != 0 || symbolic.empty())
		symbolic.push_back(RelayDepthExpr::Constant(constant));
	if (symbolic.size() == 1)
		return std::move(symbolic.front());
	RelayDepthExpr result;
	result.kind = RelayDepthExprKind::Maximum;
	result.children = std::move(symbolic);
	return result;
}

RelayDepthExpr Successor(RelayDepthExpr expression)
{
	if (IsUnknown(expression))
		return expression;
	if (expression.kind == RelayDepthExprKind::Constant)
	{
		if (expression.value == std::numeric_limits<uint64_t>::max())
			return RelayDepthExpr::Unknown("relay depth overflow");
		return RelayDepthExpr::Constant(expression.value + 1);
	}
	RelayDepthExpr result;
	result.kind = RelayDepthExprKind::Successor;
	result.children.push_back(std::move(expression));
	return result;
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
	for (const cfg::PredaFunctionCFG &function :
		protocol.controlFlow.functions)
	{
		if (function.functionId == functionId)
			return &function;
	}
	return nullptr;
}

const cfg::PredaFunctionGraphAnalysis *FindFunctionAnalysis(
	const RelayProtocolIR &protocol,
	const std::string &functionId)
{
	for (const cfg::PredaFunctionGraphAnalysis &analysis :
		protocol.controlFlow.relayIcfg.functionAnalyses)
	{
		if (analysis.functionId == functionId)
			return &analysis;
	}
	return nullptr;
}

const cfg::PredaCFGNode *FindNode(
	const cfg::PredaFunctionCFG &function,
	const cfg::NodeId &nodeId)
{
	for (const cfg::PredaCFGNode &node : function.nodes)
	{
		if (node.id == nodeId)
			return &node;
	}
	return nullptr;
}

bool NodeDominatesExit(
	const RelayProtocolIR &protocol,
	const cfg::PredaFunctionCFG &function,
	const cfg::NodeId &nodeId)
{
	const cfg::PredaFunctionGraphAnalysis *analysis =
		FindFunctionAnalysis(protocol, function.functionId);
	if (analysis == nullptr ||
		analysis->status != cfg::AnalysisStatus::Complete ||
		!analysis->dominanceComplete)
	{
		return false;
	}
	const auto exitDominators =
		analysis->dominators.find(function.exitNodeId);
	return exitDominators != analysis->dominators.end() &&
		std::find(
			exitDominators->second.begin(),
			exitDominators->second.end(),
			nodeId) != exitDominators->second.end();
}

bool FunctionMayBypassRelayOccurrence(
	const cfg::PredaFunctionCFG &function)
{
	return function.status != cfg::AnalysisStatus::Complete ||
		function.effect.status != cfg::AnalysisStatus::Complete ||
		function.effect.mayReturnEarly ||
		function.effect.mayBreak ||
		function.effect.mayContinue ||
		function.effect.mayAbortOrFail ||
		function.effect.mayCallUnknown ||
		function.effect.mayHaveExternalEffect;
}

std::set<std::string> SynchronousClosure(
	const RelayProtocolIR &protocol,
	const std::string &root)
{
	std::set<std::string> functions;
	functions.insert(root);
	const auto closure = protocol.controlFlow.relayIcfg
		.synchronousReachableFunctions.find(root);
	if (closure != protocol.controlFlow.relayIcfg
		.synchronousReachableFunctions.end())
		functions.insert(closure->second.begin(), closure->second.end());
	return functions;
}

class DepthState
{
public:
	explicit DepthState(const RelayProtocolIR &protocol)
		: m_protocol(protocol)
	{
	}

	RelayDepthExpr Build(
		const std::string &functionId,
		std::vector<std::string> &evidence,
		bool &exact,
		std::string &reason)
	{
		std::set<std::string> visiting;
		return BuildFunction(functionId, visiting, evidence, exact, reason);
	}

private:
	RelayDepthExpr BuildFunction(
		const std::string &functionId,
		std::set<std::string> &visiting,
		std::vector<std::string> &evidence,
		bool &exact,
		std::string &reason)
	{
		if (!visiting.insert(functionId).second)
		{
			reason = "recursive async relay handler cycle has no finite depth proof";
			return RelayDepthExpr::Unknown(reason);
		}
		const std::set<std::string> closure =
			SynchronousClosure(m_protocol, functionId);
		for (const std::string &recursive :
			m_protocol.controlFlow.callGraph.analysis.recursiveFunctions)
		{
			if (closure.find(recursive) != closure.end())
			{
				reason =
					"synchronous recursion prevents a finite composed depth proof";
				visiting.erase(functionId);
				return RelayDepthExpr::Unknown(reason);
			}
		}

		std::vector<RelayDepthExpr> siteDepths;
		bool closureHasRelay = false;
		bool closureOccurrenceExact = true;
		for (const std::string &member : closure)
		{
			evidence.push_back(member);
			const cfg::PredaFunctionCFG *function =
				FindCfgFunction(m_protocol, member);
			if (function == nullptr)
			{
				reason = "synchronous closure contains a missing CFG: " + member;
				visiting.erase(functionId);
				return RelayDepthExpr::Unknown(reason);
			}
			closureOccurrenceExact = closureOccurrenceExact &&
				!FunctionMayBypassRelayOccurrence(*function);
		}
		for (const cfg::PredaInterproceduralEdge &edge :
			m_protocol.controlFlow.relayIcfg.interproceduralEdges)
		{
			if (edge.kind != cfg::InterproceduralEdgeKind::SyncCall ||
				closure.find(edge.sourceFunctionId) == closure.end())
			{
				continue;
			}
			const cfg::PredaFunctionCFG *caller =
				FindCfgFunction(m_protocol, edge.sourceFunctionId);
			const cfg::PredaCFGNode *callNode = caller == nullptr
				? nullptr
				: FindNode(*caller, edge.sourceNodeId);
			if (!edge.resolved || caller == nullptr || callNode == nullptr)
			{
				reason =
					"synchronous closure contains an unresolved call occurrence: " +
					edge.id;
				visiting.erase(functionId);
				return RelayDepthExpr::Unknown(reason);
			}
			if (!callNode->enclosingLoopIds.empty() ||
				!NodeDominatesExit(m_protocol, *caller, callNode->id))
			{
				closureOccurrenceExact = false;
			}
		}
		for (const RelaySite &site : m_protocol.relaySites)
		{
			if (closure.find(site.sourceFunctionId) == closure.end())
				continue;
			closureHasRelay = true;
			exact = exact && site.branches.empty() && site.loops.empty();
			const RelayHandler *handler =
				FindHandler(m_protocol, site.handlerId);
			if (handler == nullptr || !handler->resolved ||
				handler->targetFunctionId.empty())
			{
				reason = "relay handler is unresolved: " + site.handlerId;
				visiting.erase(functionId);
				return RelayDepthExpr::Unknown(reason);
			}
			RelayDepthExpr child;
			if (handler->relayReachabilityKnown && !handler->mayEmitRelay)
			{
				child = RelayDepthExpr::Constant(0);
			}
			else
			{
				child = BuildFunction(
					handler->targetFunctionId,
					visiting,
					evidence,
					exact,
					reason);
			}
			if (IsUnknown(child))
			{
				visiting.erase(functionId);
				return child;
			}
			siteDepths.push_back(Successor(std::move(child)));
		}
		if (closureHasRelay && !closureOccurrenceExact)
			exact = false;
		visiting.erase(functionId);
		return Maximum(std::move(siteDepths));
	}

	const RelayProtocolIR &m_protocol;
};

} // namespace

void RelayDepthBoundAnalyzer::Analyze(
	const RelayProtocolIR &protocol,
	FunctionParallelRelayCertificate &certificate) const
{
	certificate.depthBound.id =
		"parallel.bound." + certificate.sourceFunctionId + ".relay_tree_depth";
	std::vector<std::string> evidence;
	bool exact = true;
	std::string reason;
	RelayDepthExpr depth = DepthState(protocol).Build(
		certificate.sourceFunctionId, evidence, exact, reason);
	std::sort(evidence.begin(), evidence.end());
	evidence.erase(std::unique(evidence.begin(), evidence.end()), evidence.end());
	certificate.depthBound.supportingCfgFactIds = std::move(evidence);
	certificate.depthBound.upperBound = depth;
	if (IsUnknown(depth))
	{
		certificate.depthBound.exact = depth;
		certificate.depthBound.reason = reason;
		return;
	}
	if (exact)
	{
		certificate.depthBound.exact = depth;
		certificate.depthBound.status = CertificateStatus::Complete;
		certificate.depthBound.reason =
			"exact relay-tree depth over the synchronous closure and async handlers";
	}
	else
	{
		certificate.depthBound.exact = RelayDepthExpr::Unknown(
			"conditional relay occurrence prevents an exact depth value");
		certificate.depthBound.status = CertificateStatus::Conservative;
		certificate.depthBound.reason =
			"finite relay-tree depth upper bound over composed handlers";
	}
}

} // namespace certificate
} // namespace relay_protocol
} // namespace transpiler
