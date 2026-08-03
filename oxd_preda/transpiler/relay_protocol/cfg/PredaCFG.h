#pragma once

#include "../RelayExprIR.h"
#include "../refinement/RelayFormulaIR.h"
#include "../../transpiler/PredaCommon.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace transpiler {
namespace relay_protocol {
namespace cfg {

using NodeId = std::string;
using FunctionId = std::string;
using StateVariableId = std::string;

enum class AnalysisStatus : uint8_t
{
	Complete,
	Conservative,
	Unsupported,
	Unknown,
};

enum class PredaCFGNodeKind : uint8_t
{
	Entry,
	Exit,
	Basic,
	Branch,
	LoopHeader,
	LoopLatch,
	Break,
	Continue,
	Return,
	SynchronousCall,
	RelayEmit,
	AbortOrFailure,
	Opaque,
};

enum class PredaCFGEdgeKind : uint8_t
{
	Fallthrough,
	TrueBranch,
	FalseBranch,
	LoopBack,
	BreakExit,
	ContinueBack,
	ReturnExit,
	CallToCallee,
	ReturnToCaller,
	ExceptionalOrFailure,
	Unknown,
};

enum class PredaCallKind : uint8_t
{
	Synchronous,
	Relay,
	ExternalUnknown,
	CompilerGeneratedHelper,
	RuntimeHelper,
};

enum class RegionKind : uint8_t
{
	Function,
	BranchArm,
	LoopBody,
	SynchronousCallSite,
	RelayHandler,
	RelayRegion,
};

enum class InterproceduralEdgeKind : uint8_t
{
	SyncCall,
	SyncReturn,
	AsyncRelaySpawn,
};

enum class RelaySiteRelationStatus : uint8_t
{
	CoReachable,
	MutuallyExclusive,
	Unknown,
};

struct EffectSummary
{
	bool readsCurrentScopeState = false;
	bool writesCurrentScopeState = false;
	bool readsGlobalState = false;
	bool writesGlobalState = false;
	bool writesLocalState = false;
	bool modifiesLoopInductionVariable = false;
	bool mayEmitRelay = false;
	bool mayCallUnknown = false;
	bool mayReturnEarly = false;
	bool mayBreak = false;
	bool mayContinue = false;
	bool mayAbortOrFail = false;
	bool mayHaveExternalEffect = false;
	std::set<StateVariableId> readStateVariables;
	std::set<StateVariableId> writtenStateVariables;
	AnalysisStatus status = AnalysisStatus::Complete;
	std::string reason;
};

struct PredaCFGNode
{
	NodeId id;
	PredaCFGNodeKind kind = PredaCFGNodeKind::Opaque;
	FunctionId sourceFunctionId;
	SourceLocation location;
	std::string relaySiteId;
	FunctionId calleeFunctionId;
	bool hasCondition = false;
	refinement::FormulaExpr condition;
	RelayExprIR sourceExpression;
	std::vector<NodeId> successors;
	std::vector<NodeId> predecessors;
	std::vector<std::string> enclosingLoopIds;
	EffectSummary directEffect;
	bool supported = true;
	std::string unsupportedReason;
};

struct PredaCFGEdge
{
	std::string id;
	NodeId source;
	NodeId target;
	PredaCFGEdgeKind kind = PredaCFGEdgeKind::Unknown;
	bool supported = true;
	std::string reason;
};

struct PredaCFGRegion
{
	std::string id;
	RegionKind kind = RegionKind::Function;
	FunctionId sourceFunctionId;
	SourceLocation location;
	std::vector<NodeId> nodeIds;
	std::string relaySiteId;
	NodeId callSiteNodeId;
};

struct PredaFunctionCFG
{
	FunctionId functionId;
	std::string function;
	std::string signature;
	ScopeType scope = ScopeType::None;
	bool generatedRelayLambda = false;
	SourceLocation location;
	NodeId entryNodeId;
	NodeId exitNodeId;
	std::vector<PredaCFGNode> nodes;
	std::vector<PredaCFGEdge> edges;
	std::vector<PredaCFGRegion> regions;
	EffectSummary effect;
	AnalysisStatus status = AnalysisStatus::Complete;
	std::vector<std::string> completenessReasons;
};

struct PredaCallEdge
{
	std::string id;
	FunctionId caller;
	FunctionId callee;
	PredaCallKind kind = PredaCallKind::ExternalUnknown;
	SourceLocation callSite;
	std::string sourceText;
	bool resolved = false;
	bool calleeIsConst = false;
	std::string unresolvedReason;
};

struct PredaCallGraphAnalysis
{
	std::vector<std::vector<FunctionId>> stronglyConnectedComponents;
	std::vector<FunctionId> recursiveFunctions;
	std::vector<std::vector<FunctionId>> topologicalComponents;
	std::map<FunctionId, std::vector<FunctionId>> reverseCallers;
	std::map<FunctionId, bool> hasUnresolvedOutgoing;
	std::map<FunctionId, bool> relayReachable;
};

struct PredaSynchronousCallGraph
{
	std::vector<FunctionId> functions;
	std::vector<PredaCallEdge> edges;
	PredaCallGraphAnalysis analysis;
};

struct PredaRegionEffect
{
	std::string regionId;
	RegionKind kind = RegionKind::Function;
	FunctionId sourceFunctionId;
	std::string relaySiteId;
	NodeId callSiteNodeId;
	std::vector<NodeId> nodeIds;
	EffectSummary effect;
};

struct PredaFunctionGraphAnalysis
{
	FunctionId functionId;
	AnalysisStatus status = AnalysisStatus::Complete;
	std::vector<std::string> completenessReasons;
	bool dominanceComplete = true;
	bool postDominanceComplete = true;
	bool reachabilityComplete = true;
	std::map<NodeId, std::vector<NodeId>> dominators;
	std::map<NodeId, std::vector<NodeId>> postDominators;
	std::map<NodeId, std::vector<NodeId>> controlDependents;
	std::map<NodeId, std::vector<NodeId>> reachableNodes;
	std::map<NodeId, std::vector<std::string>> loopNesting;
};

struct PredaInterproceduralEdge
{
	std::string id;
	InterproceduralEdgeKind kind = InterproceduralEdgeKind::SyncCall;
	FunctionId sourceFunctionId;
	NodeId sourceNodeId;
	FunctionId targetFunctionId;
	NodeId targetNodeId;
	std::string relaySiteId;
	bool resolved = false;
	std::string reason;
};

struct RelaySiteRelation
{
	std::string firstRelaySiteId;
	std::string secondRelaySiteId;
	RelaySiteRelationStatus status = RelaySiteRelationStatus::Unknown;
	std::string reason;
};

struct RelayICFG
{
	std::vector<PredaInterproceduralEdge> interproceduralEdges;
	std::vector<PredaFunctionGraphAnalysis> functionAnalyses;
	std::vector<PredaFunctionGraphAnalysis>
		synchronousCompositionAnalyses;
	std::vector<RelaySiteRelation> relaySiteRelations;
	std::map<FunctionId, std::vector<FunctionId>> synchronousReachableFunctions;
	std::map<FunctionId, std::vector<FunctionId>> asyncReachableHandlers;
	std::vector<std::vector<FunctionId>> synchronousSccs;
};

struct PredaControlFlowIR
{
	std::vector<PredaFunctionCFG> functions;
	PredaSynchronousCallGraph callGraph;
	std::vector<PredaRegionEffect> regionEffects;
	RelayICFG relayIcfg;

	void Reset()
	{
		functions.clear();
		callGraph = PredaSynchronousCallGraph();
		regionEffects.clear();
		relayIcfg = RelayICFG();
	}
};

} // namespace cfg
} // namespace relay_protocol
} // namespace transpiler
