#include "RelayConstraintGenerator.h"

#include "RelayFormulaBuilder.h"
#include "RelayRefinementSymbolTable.h"

#include "../analysis/RelayCardinalityExpr.h"

#include <algorithm>
#include <map>
#include <utility>

namespace transpiler {
namespace relay_protocol {
namespace refinement {
namespace {

using analysis::RelayCardinalityExpr;
using analysis::RelayCardinalityExprKind;

FormulaExpr SymbolFormula(
	const RelayRefinementSymbol &symbol,
	const SourceLocation &location = SourceLocation())
{
	return FormulaExpr::Symbol(
		symbol.id,
		symbol.sort,
		location.IsValid() ? location : symbol.location,
		symbol.sourceName);
}

FormulaExpr BinaryFormula(
	const char *op,
	FormulaExpr left,
	FormulaExpr right,
	const SourceLocation &location = SourceLocation())
{
	return FormulaExpr::Binary(
		op,
		std::move(left),
		std::move(right),
		FormulaSort::Bool(),
		location);
}

FormulaExpr Implication(
	FormulaExpr antecedent,
	FormulaExpr consequent,
	const SourceLocation &location)
{
	return BinaryFormula(
		"implies",
		std::move(antecedent),
		std::move(consequent),
		location);
}

FormulaExpr UnsupportedGoal(
	const std::string &text,
	const SourceLocation &location,
	const std::string &reason)
{
	return FormulaExpr::Unknown(text, location, reason);
}

bool ContainsUnknown(const FormulaExpr &formula)
{
	// Unknown is an explicit owning formula node. A child with sort Unknown
	// is still usable as the opaque container operand of a supported
	// ArrayLength node; the parent's BV32 result is fully typed. Other
	// operations reject incompatible child sorts in RelayFormulaBuilder.
	if (formula.IsUnknown())
		return true;
	for (const FormulaExpr &child : formula.children)
	{
		if (ContainsUnknown(child))
			return true;
	}
	return false;
}

bool HasSnapshot(const FormulaExpr &formula)
{
	return !formula.IsUnknown() ||
		!formula.text.empty() ||
		formula.location.IsValid() ||
		!formula.unknownReason.empty();
}

std::string FormulaFailureReason(
	const FormulaExpr &formula,
	const std::string &fallback)
{
	if (!formula.unknownReason.empty())
		return formula.unknownReason;
	if (!formula.sort.IsKnown())
		return "formula has an unknown sort";
	return fallback;
}

std::string TargetConstraintId(const RelaySite &site)
{
	return "rpreda.constraint.target_relation.site." + site.id;
}

std::string ArgumentConstraintId(
	const RelaySite &site,
	size_t argumentIndex)
{
	return "rpreda.constraint.argument_relation.site." + site.id +
		".arg." + std::to_string(argumentIndex);
}

std::string GuardConstraintId(const RelaySite &site)
{
	return "rpreda.constraint.guard_necessity.site." + site.id;
}

std::string CountConstraintId(
	const FunctionProtocol &function,
	const char *suffix)
{
	return "rpreda.constraint.count." +
		function.sourceFunctionId + "." + suffix;
}

std::string TargetObligationId(const RelaySite &site)
{
	return "rpreda.obligation.target_equality.site." + site.id;
}

std::string ArgumentObligationId(
	const RelaySite &site,
	size_t argumentIndex)
{
	return "rpreda.obligation.argument_equality.site." + site.id +
		".arg." + std::to_string(argumentIndex);
}

std::string GuardObligationId(
	const RelaySite &site,
	const char *suffix)
{
	return "rpreda.obligation.guard." + std::string(suffix) +
		".site." + site.id;
}

std::string CountObligationId(
	const FunctionProtocol &function,
	const char *suffix)
{
	return "rpreda.obligation.count." +
		function.sourceFunctionId + "." + suffix;
}

std::string NonAliasObligationId(
	const std::string &firstSiteId,
	const std::string &secondSiteId)
{
	return "rpreda.obligation.target_non_alias.site." +
		firstSiteId + ".site." + secondSiteId;
}

RelayProofObligationRole ObligationRoleForKind(
	RelayProofObligationKind kind)
{
	switch (kind)
	{
	case RelayProofObligationKind::RelayTargetEquality:
	case RelayProofObligationKind::RelayArgumentEquality:
	case RelayProofObligationKind::RelayGuardNecessity:
	case RelayProofObligationKind::RelayCountEquality:
		return RelayProofObligationRole::
			EstablishedByConstruction;
	default:
		return RelayProofObligationRole::SolverGoal;
	}
}

RelayProofObligation MakeUnsupportedObligation(
	const std::string &id,
	RelayProofObligationKind kind,
	const std::string &functionId,
	const std::string &siteId,
	int64_t argumentIndex,
	const SourceLocation &location,
	const std::string &sourceText,
	const std::string &reason)
{
	RelayProofObligation obligation;
	obligation.id = id;
	obligation.kind = kind;
	obligation.role = ObligationRoleForKind(kind);
	obligation.status = RelayProofObligationStatus::Unsupported;
	obligation.sourceFunctionId = functionId;
	obligation.relaySiteId = siteId;
	obligation.argumentIndex = argumentIndex;
	obligation.location = location;
	obligation.reason = reason;
	obligation.goal =
		UnsupportedGoal(sourceText, location, reason);
	return obligation;
}

RelayProofObligation MakeGeneratedObligation(
	const std::string &id,
	RelayProofObligationKind kind,
	const std::string &functionId,
	const std::string &siteId,
	int64_t argumentIndex,
	const SourceLocation &location,
	const std::string &constraintId,
	const FormulaExpr &goal)
{
	RelayProofObligation obligation;
	obligation.id = id;
	obligation.kind = kind;
	obligation.role = ObligationRoleForKind(kind);
	obligation.status = RelayProofObligationStatus::Generated;
	obligation.sourceFunctionId = functionId;
	obligation.relaySiteId = siteId;
	obligation.argumentIndex = argumentIndex;
	obligation.location = location;
	if (!constraintId.empty())
		obligation.constraintIds.push_back(constraintId);
	obligation.goal = goal;
	return obligation;
}

const FormulaExpr *FindBranchSnapshot(
	const RelayProtocolIR &protocol,
	const std::string &functionId,
	const RelayExprIR &predicate)
{
	for (const RelaySite &site : protocol.relaySites)
	{
		if (site.sourceFunctionId != functionId)
			continue;
		for (const BranchCondition &branch : site.branches)
		{
			const bool sameOffsets =
				predicate.location.startOffset >= 0 &&
				branch.condition.location.startOffset >= 0 &&
				predicate.location.startOffset ==
					branch.condition.location.startOffset &&
				predicate.location.endOffset ==
					branch.condition.location.endOffset;
			const bool sameFallbackIdentity =
				predicate.text == branch.condition.text &&
				predicate.location.line == branch.condition.location.line &&
				predicate.location.column ==
					branch.condition.location.column;
			if ((sameOffsets || sameFallbackIdentity) &&
				HasSnapshot(branch.refinementFormula))
			{
				return &branch.refinementFormula;
			}
		}
	}
	return nullptr;
}

FormulaExpr BuildPredicateFormula(
	const RelayExprIR &predicate,
	const std::string &functionId,
	const RelayProtocolIR &protocol,
	const RelayRefinementSymbolTable &symbols,
	const RelayFormulaBuilder &formulaBuilder)
{
	const FormulaExpr *snapshot =
		FindBranchSnapshot(protocol, functionId, predicate);
	if (snapshot != nullptr)
		return *snapshot;
	return formulaBuilder.Build(
		predicate,
		symbols.MakeResolver(functionId));
}

FormulaExpr TranslateCardinality(
	const RelayCardinalityExpr &cardinality,
	const FunctionProtocol &function,
	const RelayProtocolIR &protocol,
	const RelayRefinementSymbolTable &symbols,
	const RelayFormulaBuilder &formulaBuilder)
{
	switch (cardinality.kind)
	{
	case RelayCardinalityExprKind::Constant:
		return FormulaExpr::IntLiteral(
			std::to_string(cardinality.value));

	case RelayCardinalityExprKind::Sum:
	case RelayCardinalityExprKind::Product:
	{
		std::vector<FormulaExpr> children;
		children.reserve(cardinality.children.size());
		for (const RelayCardinalityExpr &child : cardinality.children)
		{
			FormulaExpr translated = TranslateCardinality(
				child,
				function,
				protocol,
				symbols,
				formulaBuilder);
			if (ContainsUnknown(translated))
			{
				return UnsupportedGoal(
					std::string(),
					function.root.location,
					FormulaFailureReason(
						translated,
						"relay cardinality child is unsupported"));
			}
			children.push_back(std::move(translated));
		}
		return FormulaExpr::Nary(
			cardinality.kind == RelayCardinalityExprKind::Sum
				? "+"
				: "*",
			std::move(children),
			FormulaSort::Int(),
			function.root.location);
	}

	case RelayCardinalityExprKind::Ite:
	{
		if (cardinality.children.size() != 2)
		{
			return UnsupportedGoal(
				cardinality.predicate.text,
				cardinality.predicate.location,
				"relay cardinality ite does not have two result arms");
		}
		FormulaExpr condition = BuildPredicateFormula(
			cardinality.predicate,
			function.sourceFunctionId,
			protocol,
			symbols,
			formulaBuilder);
		FormulaExpr thenValue = TranslateCardinality(
			cardinality.children[0],
			function,
			protocol,
			symbols,
			formulaBuilder);
		FormulaExpr elseValue = TranslateCardinality(
			cardinality.children[1],
			function,
			protocol,
			symbols,
			formulaBuilder);
		if (ContainsUnknown(condition) ||
			condition.sort != FormulaSort::Bool() ||
			ContainsUnknown(thenValue) ||
			ContainsUnknown(elseValue) ||
			thenValue.sort != FormulaSort::Int() ||
			elseValue.sort != FormulaSort::Int())
		{
			return UnsupportedGoal(
				cardinality.predicate.text,
				cardinality.predicate.location,
				"relay cardinality ite contains an unsupported "
				"predicate or arm");
		}
		return FormulaExpr::Ite(
			std::move(condition),
			std::move(thenValue),
			std::move(elseValue),
			FormulaSort::Int(),
			cardinality.predicate.location);
	}

	case RelayCardinalityExprKind::Unknown:
		return UnsupportedGoal(
			std::string(),
			function.root.location,
			cardinality.reason.empty()
				? "relay cardinality is unknown"
				: cardinality.reason);
	}

	return UnsupportedGoal(
		std::string(),
		function.root.location,
		"unsupported relay cardinality expression kind");
}

FormulaExpr BuildGuard(
	const RelaySite &site,
	const RelayFormulaBuilder &formulaBuilder,
	const RelayRefinementSymbolTable &symbols)
{
	std::vector<FormulaExpr> predicates;
	predicates.reserve(site.branches.size());
	for (const BranchCondition &branch : site.branches)
	{
		FormulaExpr predicate = HasSnapshot(branch.refinementFormula)
			? branch.refinementFormula
			: formulaBuilder.Build(
				branch.condition,
				symbols.MakeResolver(site.sourceFunctionId));
		if (ContainsUnknown(predicate) ||
			predicate.sort != FormulaSort::Bool())
		{
			return UnsupportedGoal(
				branch.condition.text,
				branch.condition.location,
				FormulaFailureReason(
					predicate,
					"branch predicate is not a supported Bool formula"));
		}
		if (!branch.polarity)
		{
			predicate = FormulaExpr::Unary(
				"!",
				std::move(predicate),
				FormulaSort::Bool(),
				branch.location,
				"!" + branch.condition.text);
		}
		predicates.push_back(std::move(predicate));
	}
	if (predicates.empty())
		return FormulaExpr::BoolLiteral(true);
	if (predicates.size() == 1)
		return std::move(predicates.front());
	return FormulaExpr::Nary(
		"&&",
		std::move(predicates),
		FormulaSort::Bool(),
		site.location);
}

bool IsLoopInstanceAmbiguous(
	const RelaySite &site,
	const analysis::RelayExpressionDependency & /*dependency*/)
{
	// FormulaIR deliberately has one actual target/argument symbol per static
	// RelaySite, but a loop can produce multiple dynamic occurrences with
	// different values.  Without an occurrence index or a quantified
	// schematic semantics, asserting one equality for every execution would
	// be unsound even when the first listener visit sees a stable value.
	return !site.loops.empty();
}

struct SiteFormulaFacts
{
	const RelaySite *site = nullptr;
	RelayRefinementSymbol emitted;
	RelayRefinementSymbol actualTarget;
	bool targetSupported = false;
};

} // namespace

RelayConstraintGenerationResult RelayConstraintGenerator::Generate(
	const RelayProtocolIR &protocol,
	RelayRefinementSymbolTable &symbols,
	const RelayFormulaBuilder &formulaBuilder) const
{
	RelayConstraintGenerationResult result;
	std::map<std::string, SiteFormulaFacts> siteFacts;

	for (const RelaySite &site : protocol.relaySites)
	{
		const RelayRefinementSymbol emitted =
			symbols.EnsureRelayEmission(site);
		const RelayRefinementSymbol actualTarget =
			symbols.EnsureActualTarget(site);
		SiteFormulaFacts facts;
		facts.site = &site;
		facts.emitted = emitted;
		facts.actualTarget = actualTarget;

		FormulaExpr target = HasSnapshot(site.refinementTargetFormula)
			? site.refinementTargetFormula
			: formulaBuilder.Build(
				site.target,
				symbols.MakeResolver(site.sourceFunctionId));
		const bool targetLoopAmbiguous =
			IsLoopInstanceAmbiguous(site, site.targetDependency);
		const bool targetSupported =
			!targetLoopAmbiguous &&
			!ContainsUnknown(target) &&
			actualTarget.sort.IsKnown() &&
			target.sort == actualTarget.sort;

		if (targetSupported)
		{
			FormulaExpr equality = BinaryFormula(
				"==",
				SymbolFormula(actualTarget, site.location),
				std::move(target),
				site.location);
			FormulaExpr relation = Implication(
				SymbolFormula(emitted, site.location),
				std::move(equality),
				site.location);
			RelayConstraint constraint;
			constraint.id = TargetConstraintId(site);
			constraint.kind =
				RelayConstraintKind::RelayTargetRelation;
			constraint.sourceFunctionId = site.sourceFunctionId;
			constraint.relaySiteId = site.id;
			constraint.formula = relation;
			constraint.location = site.location;
			result.constraints.push_back(constraint);
			result.proofObligations.push_back(
				MakeGeneratedObligation(
					TargetObligationId(site),
					RelayProofObligationKind::RelayTargetEquality,
					site.sourceFunctionId,
					site.id,
					-1,
					site.location,
					constraint.id,
					relation));
			facts.targetSupported = true;
		}
		else
		{
			std::string reason;
			if (targetLoopAmbiguous)
			{
				reason =
					"one actual target symbol cannot represent all "
					"loop-variable-dependent executions of this relay site";
			}
			else if (ContainsUnknown(target))
			{
				reason = FormulaFailureReason(
					target,
					"relay target formula is unsupported");
			}
			else if (!actualTarget.sort.IsKnown())
			{
				reason =
					"actual relay target has an unknown formula sort";
			}
			else
			{
				reason =
					"actual relay target and target expression have "
					"incompatible formula sorts";
			}
			RelayProofObligation obligation =
				MakeUnsupportedObligation(
					TargetObligationId(site),
					RelayProofObligationKind::RelayTargetEquality,
					site.sourceFunctionId,
					site.id,
					-1,
					site.location,
					site.target.text,
					reason);
			// Retain a structurally translated candidate when only a nested
			// term, a loop occurrence, or solver sort support prevents this
			// phase from asserting it. A truly unknown target remains an
			// owning Unknown formula with its source diagnostics.
			if (!target.IsUnknown() &&
				actualTarget.sort.IsKnown() &&
				target.sort == actualTarget.sort)
			{
				obligation.goal = Implication(
					SymbolFormula(emitted, site.location),
					BinaryFormula(
						"==",
						SymbolFormula(actualTarget, site.location),
						target,
						site.location),
					site.location);
			}
			result.proofObligations.push_back(
				std::move(obligation));
		}

		for (size_t argumentIndex = 0;
			argumentIndex < site.arguments.size();
			++argumentIndex)
		{
			const RelayArgument &argument =
				site.arguments[argumentIndex];
			const RelayRefinementSymbol actualArgument =
				symbols.EnsureActualArgument(site, argumentIndex);
			FormulaExpr value = HasSnapshot(argument.refinementFormula)
				? argument.refinementFormula
				: formulaBuilder.Build(
					argument.expression,
					symbols.MakeResolver(site.sourceFunctionId));
			const bool loopAmbiguous =
				IsLoopInstanceAmbiguous(site, argument.dependency);
			const bool supported =
				!loopAmbiguous &&
				!ContainsUnknown(value) &&
				actualArgument.sort.IsKnown() &&
				value.sort == actualArgument.sort;
			if (supported)
			{
				FormulaExpr equality = BinaryFormula(
					"==",
					SymbolFormula(
						actualArgument,
						argument.expression.location),
					std::move(value),
					argument.expression.location);
				FormulaExpr relation = Implication(
					SymbolFormula(emitted, site.location),
					std::move(equality),
					site.location);
				RelayConstraint constraint;
				constraint.id =
					ArgumentConstraintId(site, argumentIndex);
				constraint.kind =
					RelayConstraintKind::RelayArgumentRelation;
				constraint.sourceFunctionId =
					site.sourceFunctionId;
				constraint.relaySiteId = site.id;
				constraint.argumentIndex =
					static_cast<int64_t>(argumentIndex);
				constraint.formula = relation;
				constraint.location =
					argument.expression.location;
				result.constraints.push_back(constraint);
				result.proofObligations.push_back(
					MakeGeneratedObligation(
						ArgumentObligationId(
							site,
							argumentIndex),
						RelayProofObligationKind::
							RelayArgumentEquality,
						site.sourceFunctionId,
						site.id,
						static_cast<int64_t>(
							argumentIndex),
						argument.expression.location,
						constraint.id,
						relation));
			}
			else
			{
				std::string reason;
				if (loopAmbiguous)
				{
					reason =
						"one actual argument symbol cannot represent all "
						"loop-variable-dependent executions of this relay site";
				}
				else if (ContainsUnknown(value))
				{
					reason = FormulaFailureReason(
						value,
						"relay argument formula is unsupported");
				}
				else if (!actualArgument.sort.IsKnown())
				{
					reason =
						"actual relay argument has an unknown formula sort";
				}
				else
				{
					reason =
						"actual relay argument and source expression have "
						"incompatible formula sorts";
				}
				RelayProofObligation obligation =
					MakeUnsupportedObligation(
						ArgumentObligationId(
							site,
							argumentIndex),
						RelayProofObligationKind::
							RelayArgumentEquality,
						site.sourceFunctionId,
						site.id,
						static_cast<int64_t>(
							argumentIndex),
						argument.expression.location,
						argument.expression.text,
						reason);
				if (!value.IsUnknown() &&
					actualArgument.sort.IsKnown() &&
					value.sort == actualArgument.sort)
				{
					obligation.goal = Implication(
						SymbolFormula(emitted, site.location),
						BinaryFormula(
							"==",
							SymbolFormula(
								actualArgument,
								argument.expression.location),
							value,
							argument.expression.location),
						site.location);
				}
				result.proofObligations.push_back(
					std::move(obligation));
			}
		}

		FormulaExpr guard =
			BuildGuard(site, formulaBuilder, symbols);
		const bool guardLoopAmbiguous =
			!site.loops.empty() && !site.branches.empty();
		if (!guardLoopAmbiguous &&
			!ContainsUnknown(guard) &&
			guard.sort == FormulaSort::Bool())
		{
			FormulaExpr necessity = Implication(
				SymbolFormula(emitted, site.location),
				std::move(guard),
				site.location);
			RelayConstraint constraint;
			constraint.id = GuardConstraintId(site);
			constraint.kind =
				RelayConstraintKind::RelayGuardNecessity;
			constraint.sourceFunctionId = site.sourceFunctionId;
			constraint.relaySiteId = site.id;
			constraint.formula = necessity;
			constraint.location = site.location;
			result.constraints.push_back(constraint);
			result.proofObligations.push_back(
				MakeGeneratedObligation(
					GuardObligationId(site, "necessity"),
					RelayProofObligationKind::
						RelayGuardNecessity,
					site.sourceFunctionId,
					site.id,
					-1,
					site.location,
					constraint.id,
					necessity));
		}
		else
		{
			const std::string reason = guardLoopAmbiguous
				? "one site-level emission Boolean cannot represent "
					"per-iteration branch guards at a repeated relay site"
				: FormulaFailureReason(
					guard,
					"enclosing branch guard cannot be translated");
			result.proofObligations.push_back(
				MakeUnsupportedObligation(
					GuardObligationId(site, "necessity"),
					RelayProofObligationKind::
						RelayGuardNecessity,
					site.sourceFunctionId,
					site.id,
					-1,
					site.location,
					std::string(),
					reason));
		}

		result.proofObligations.push_back(
			MakeUnsupportedObligation(
				GuardObligationId(site, "equivalence"),
				RelayProofObligationKind::RelayGuardEquivalence,
				site.sourceFunctionId,
				site.id,
				-1,
				site.location,
				std::string(),
				"the current protocol IR is not a complete CFG and "
				"cannot exclude early return, failure, opaque control, "
				"unmodeled relay-reachable calls, or unknown loop behavior"));

		siteFacts.emplace(site.id, std::move(facts));
	}

	for (const FunctionProtocol &function : protocol.functions)
	{
		const RelayRefinementSymbol directCount =
			symbols.EnsureDirectRelayCount(function);
		const FormulaExpr countSymbol =
			SymbolFormula(directCount, function.root.location);
		FormulaExpr exact = TranslateCardinality(
			function.summary.relayCount,
			function,
			protocol,
			symbols,
			formulaBuilder);
		if (!ContainsUnknown(exact) &&
			exact.sort == FormulaSort::Int())
		{
			FormulaExpr equality = BinaryFormula(
				"==",
				countSymbol,
				std::move(exact),
				function.root.location);
			RelayConstraint constraint;
			constraint.id =
				CountConstraintId(function, "equality");
			constraint.kind =
				RelayConstraintKind::RelayCountEquality;
			constraint.sourceFunctionId =
				function.sourceFunctionId;
			constraint.formula = equality;
			constraint.location = function.root.location;
			result.constraints.push_back(constraint);
			result.proofObligations.push_back(
				MakeGeneratedObligation(
					CountObligationId(function, "equality"),
					RelayProofObligationKind::
						RelayCountEquality,
					function.sourceFunctionId,
					std::string(),
					-1,
					function.root.location,
					constraint.id,
					equality));
		}
		else
		{
			const std::string reason = FormulaFailureReason(
				exact,
				"exact direct relay count is unsupported");
			result.proofObligations.push_back(
				MakeUnsupportedObligation(
					CountObligationId(function, "equality"),
					RelayProofObligationKind::
						RelayCountEquality,
					function.sourceFunctionId,
					std::string(),
					-1,
					function.root.location,
					std::string(),
					reason));
		}

		const std::string nonNegativeConstraintId =
			CountConstraintId(function, "non_negative");
		const FormulaExpr nonNegativeFormula = BinaryFormula(
			">=",
			countSymbol,
			FormulaExpr::IntLiteral("0"),
			function.root.location);
		RelayConstraint nonNegative;
		nonNegative.id = nonNegativeConstraintId;
		nonNegative.kind =
			RelayConstraintKind::RelayCountNonNegative;
		nonNegative.role =
			RelayConstraintRole::SolverAssumption;
		nonNegative.sourceFunctionId =
			function.sourceFunctionId;
		nonNegative.formula = nonNegativeFormula;
		nonNegative.location = function.root.location;
		result.constraints.push_back(std::move(nonNegative));

		FormulaExpr upper = TranslateCardinality(
			function.summary.relayCountUpperBound,
			function,
			protocol,
			symbols,
			formulaBuilder);
		if (!ContainsUnknown(upper) &&
			upper.sort == FormulaSort::Int())
		{
			FormulaExpr bound = BinaryFormula(
				"<=",
				countSymbol,
				std::move(upper),
				function.root.location);
			RelayConstraint constraint;
			constraint.id =
				CountConstraintId(function, "upper_bound");
			constraint.kind =
				RelayConstraintKind::RelayCountUpperBound;
			// This formula is the goal to be checked. It is deliberately
			// excluded from the solver assumption set.
			constraint.role =
				RelayConstraintRole::SolverGoal;
			constraint.sourceFunctionId =
				function.sourceFunctionId;
			constraint.formula = bound;
			constraint.location = function.root.location;
			result.constraints.push_back(constraint);
			result.proofObligations.push_back(
				MakeGeneratedObligation(
					CountObligationId(function, "upper_bound"),
					RelayProofObligationKind::
						RelayCountUpperBound,
					function.sourceFunctionId,
					std::string(),
					-1,
					function.root.location,
					constraint.id,
					bound));
		}
		else
		{
			const std::string reason = FormulaFailureReason(
				upper,
				"finite direct relay-count upper bound is unavailable");
			result.proofObligations.push_back(
				MakeUnsupportedObligation(
					CountObligationId(function, "upper_bound"),
					RelayProofObligationKind::
						RelayCountUpperBound,
					function.sourceFunctionId,
					std::string(),
					-1,
					function.root.location,
					std::string(),
					reason));
		}

		std::vector<std::string> siteIds =
			function.relaySiteIds;
		std::sort(siteIds.begin(), siteIds.end());
		siteIds.erase(
			std::unique(siteIds.begin(), siteIds.end()),
			siteIds.end());
		for (size_t firstIndex = 0;
			firstIndex < siteIds.size();
			++firstIndex)
		{
			for (size_t secondIndex = firstIndex + 1;
				secondIndex < siteIds.size();
				++secondIndex)
			{
				const auto first = siteFacts.find(
					siteIds[firstIndex]);
				const auto second = siteFacts.find(
					siteIds[secondIndex]);
				if (first == siteFacts.end() ||
					second == siteFacts.end())
				{
					continue;
				}
				const SiteFormulaFacts &firstFacts =
					first->second;
				const SiteFormulaFacts &secondFacts =
					second->second;
				const std::string obligationId =
					NonAliasObligationId(
						siteIds[firstIndex],
						siteIds[secondIndex]);
				const SourceLocation location =
					firstFacts.site->location;
				const bool supported =
					firstFacts.targetSupported &&
					secondFacts.targetSupported &&
					firstFacts.actualTarget.sort.IsKnown() &&
					firstFacts.actualTarget.sort ==
						secondFacts.actualTarget.sort;
				if (!supported)
				{
					RelayProofObligation obligation =
						MakeUnsupportedObligation(
							obligationId,
							RelayProofObligationKind::
								TargetNonAliasCandidate,
							function.sourceFunctionId,
							siteIds[firstIndex],
							-1,
							location,
							std::string(),
							"relay targets are unavailable or have "
							"incompatible formula sorts");
					obligation.relatedRelaySiteId =
						siteIds[secondIndex];
					result.proofObligations.push_back(
						std::move(obligation));
					continue;
				}

				FormulaExpr bothEmitted = FormulaExpr::Nary(
					"&&",
					{
						SymbolFormula(
							firstFacts.emitted,
							firstFacts.site->location),
						SymbolFormula(
							secondFacts.emitted,
							secondFacts.site->location),
					},
					FormulaSort::Bool(),
					location);
				FormulaExpr distinctTargets = BinaryFormula(
					"!=",
					SymbolFormula(
						firstFacts.actualTarget,
						firstFacts.site->location),
					SymbolFormula(
						secondFacts.actualTarget,
						secondFacts.site->location),
					location);
				FormulaExpr goal = Implication(
					std::move(bothEmitted),
					std::move(distinctTargets),
					location);
				RelayProofObligation obligation =
					MakeGeneratedObligation(
						obligationId,
						RelayProofObligationKind::
							TargetNonAliasCandidate,
						function.sourceFunctionId,
						siteIds[firstIndex],
						-1,
						location,
						std::string(),
						goal);
				obligation.relatedRelaySiteId =
					siteIds[secondIndex];
				result.proofObligations.push_back(
					std::move(obligation));
			}
		}
	}

	return result;
}

} // namespace refinement
} // namespace relay_protocol
} // namespace transpiler
