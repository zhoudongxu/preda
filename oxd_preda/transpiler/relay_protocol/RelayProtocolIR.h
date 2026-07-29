#pragma once

#include "RelayExprIR.h"
#include "analysis/RelayExpressionDependency.h"
#include "analysis/RelayProtocolSummary.h"
#include "refinement/RelayConstraint.h"
#include "refinement/RelayFormulaIR.h"
#include "refinement/RelayProofObligation.h"
#include "refinement/RelayRefinementSymbol.h"
#include "../transpiler/PredaCommon.h"

#include <cstdint>
#include <string>
#include <vector>

namespace transpiler {
namespace relay_protocol {

enum class RelayKind : uint8_t
{
	CustomScope,
	Global,
	Shards,
	Next,
};

enum class RelayHandlerKind : uint8_t
{
	Named,
	Lambda,
};

enum class ProtocolNodeKind : uint8_t
{
	End,
	Emit,
	Branch,
	Sequence,
	Parallel,
	Repeat,
	Call,
	Opaque,
};

struct RelayArgument
{
	RelayExprIR expression;
	std::string type;
	analysis::RelayExpressionDependency dependency;
	refinement::FormulaExpr refinementFormula;
};

struct BranchCondition
{
	RelayExprIR condition;
	bool polarity = true;
	std::string arm;
	SourceLocation location;
	refinement::FormulaExpr refinementFormula;
};

struct LoopProtocol
{
	std::string kind;
	SourceLocation location;
	RelayExprIR initializer;
	RelayExprIR condition;
	RelayExprIR update;
	bool staticallyBounded = false;
	std::string inductionVariable;
	std::string initialValue;
	std::string comparison;
	std::string boundValue;
	std::string step;
	std::string opaqueReason;
	bool bodyMayExitEarly = false;
};

struct RelaySite
{
	std::string id;
	std::string sourceContract;
	std::string sourceFunction;
	std::string sourceFunctionId;
	std::string sourceFunctionSignature;
	uint64_t sourceFunctionOverloadIndex = 0;
	ScopeType sourceScope = ScopeType::None;
	SourceLocation location;
	RelayKind relayKind = RelayKind::CustomScope;
	RelayExprIR target;
	analysis::RelayExpressionDependency targetDependency;
	refinement::FormulaExpr refinementTargetFormula;
	ScopeType targetScope = ScopeType::None;
	std::string targetFunction;
	std::vector<RelayArgument> arguments;
	std::vector<BranchCondition> branches;
	std::vector<LoopProtocol> loops;
	std::string handlerId;
};

struct RelayHandler
{
	std::string id;
	RelayHandlerKind kind = RelayHandlerKind::Named;
	std::string contract;
	std::string name;
	std::string targetFunctionId;
	std::string targetFunctionSignature;
	uint64_t targetFunctionOverloadIndex = 0;
	ScopeType scope = ScopeType::None;
	int64_t opcode = -1;
	bool resolved = false;
	bool relayReachabilityKnown = false;
	bool mayEmitRelay = false;
	SourceLocation location;
	std::vector<std::string> parameterTypes;
};

struct RelayProtocolEdge
{
	std::string id;
	std::string sourceFunction;
	std::string sourceFunctionId;
	std::string sourceFunctionSignature;
	uint64_t sourceFunctionOverloadIndex = 0;
	std::string relaySiteId;
	std::string handlerId;
	bool resolved = false;
};

struct ProtocolNode
{
	ProtocolNodeKind kind = ProtocolNodeKind::End;
	SourceLocation location;
	std::string relaySiteId;
	std::string callee;
	RelayExprIR condition;
	bool conditionPolarity = true;
	LoopProtocol loop;
	std::string opaqueReason;
	std::vector<ProtocolNode> children;
};

struct FunctionProtocol
{
	std::string contract;
	std::string function;
	std::string sourceFunctionId;
	std::string sourceFunctionSignature;
	uint64_t sourceFunctionOverloadIndex = 0;
	ScopeType scope = ScopeType::None;
	ProtocolNode root;
	std::vector<std::string> relaySiteIds;
	bool hasUnmodeledRelayReachableCall = false;
	analysis::RelayProtocolSummary summary;
};

struct RelayProtocolIR
{
	uint32_t schemaVersion = 4;
	std::string dapp;
	std::string contract;
	std::vector<RelaySite> relaySites;
	std::vector<RelayHandler> handlers;
	std::vector<RelayProtocolEdge> edges;
	std::vector<FunctionProtocol> functions;
	std::vector<refinement::RelayRefinementSymbol> refinementSymbols;
	std::vector<refinement::RelayConstraint> refinementConstraints;
	std::vector<refinement::RelayProofObligation>
		refinementProofObligations;

	void Reset(const std::string &dappName, const std::string &contractName)
	{
		schemaVersion = 4;
		dapp = dappName;
		contract = contractName;
		relaySites.clear();
		handlers.clear();
		edges.clear();
		functions.clear();
		refinementSymbols.clear();
		refinementConstraints.clear();
		refinementProofObligations.clear();
	}
};

} // namespace relay_protocol
} // namespace transpiler
