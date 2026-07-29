#include "RelayProofRunner.h"

#include <algorithm>

namespace transpiler {
namespace relay_protocol {
namespace refinement {
namespace solver {
namespace {

bool ContainsUnknown(const FormulaExpr &formula)
{
	if (formula.kind == FormulaExprKind::Unknown ||
		formula.sort.kind == FormulaSortKind::Unknown)
	{
		return true;
	}
	for (const FormulaExpr &child : formula.children)
	{
		if (ContainsUnknown(child))
			return true;
	}
	return false;
}

bool FormulaEqual(
	const FormulaExpr &left,
	const FormulaExpr &right)
{
	if (left.kind != right.kind ||
		left.sort != right.sort ||
		left.text != right.text ||
		left.op != right.op ||
		left.symbolId != right.symbolId ||
		left.literalValue != right.literalValue ||
		left.unknownReason != right.unknownReason ||
		left.children.size() != right.children.size())
	{
		return false;
	}
	for (size_t index = 0; index < left.children.size(); ++index)
	{
		if (!FormulaEqual(
				left.children[index],
				right.children[index]))
		{
			return false;
		}
	}
	return true;
}

bool IsSemanticDefinitionKind(
	RelayProofObligationKind kind)
{
	switch (kind)
	{
	case RelayProofObligationKind::RelayTargetEquality:
	case RelayProofObligationKind::RelayArgumentEquality:
	case RelayProofObligationKind::RelayGuardNecessity:
	case RelayProofObligationKind::RelayCountEquality:
		return true;
	default:
		return false;
	}
}

bool IsInitiallySupportedSolverGoal(
	RelayProofObligationKind kind)
{
	switch (kind)
	{
	case RelayProofObligationKind::RelayCountUpperBound:
	case RelayProofObligationKind::TargetNonAliasCandidate:
	case RelayProofObligationKind::BooleanRefinement:
		return true;
	default:
		return false;
	}
}

bool ContainsId(
	const std::vector<std::string> &ids,
	const std::string &candidate)
{
	return std::find(ids.begin(), ids.end(), candidate) != ids.end();
}

RelaySolverResult MakeResult(
	const std::string &backend,
	RelaySolverStatus status,
	const std::string &reason)
{
	RelaySolverResult result;
	result.backend = backend;
	result.status = status;
	result.reason = reason;
	return result;
}

} // namespace

RelayProofRunner::RelayProofRunner(
	const RelaySolverBackend *backend,
	RelayProofRunnerOptions options)
	: m_backend(backend),
	  m_options(std::move(options))
{
}

void RelayProofRunner::Run(
	const std::vector<RelayRefinementSymbol> &symbols,
	const std::vector<RelayConstraint> &constraints,
	std::vector<RelayProofObligation> &obligations) const
{
	for (RelayProofObligation &obligation : obligations)
	{
		obligation.solverResult =
			RunOne(symbols, constraints, obligation);
	}
}

RelaySolverResult RelayProofRunner::RunOne(
	const std::vector<RelayRefinementSymbol> &symbols,
	const std::vector<RelayConstraint> &constraints,
	const RelayProofObligation &obligation) const
{
	const std::string backendName =
		m_backend == nullptr ? "none" : m_backend->GetName();

	if (obligation.status ==
		RelayProofObligationStatus::Unsupported)
	{
		return MakeResult(
			backendName,
			RelaySolverStatus::Unsupported,
			obligation.reason.empty()
				? "the proof obligation is unsupported"
				: obligation.reason);
	}

	// These relations are copied directly from compiler semantics. Reporting
	// a Z3 proof after asserting the same relation would be circular, so no
	// solver query is permitted for these kinds.
	if (IsSemanticDefinitionKind(obligation.kind) ||
		obligation.role ==
			RelayProofObligationRole::EstablishedByConstruction)
	{
		return MakeResult(
			"compiler",
			RelaySolverStatus::EstablishedByConstruction,
			"the relation is established directly by compiler "
			"construction; no solver query was run");
	}

	if (!IsInitiallySupportedSolverGoal(obligation.kind))
	{
		return MakeResult(
			backendName,
			RelaySolverStatus::Unsupported,
			"this proof-obligation kind is not an initial solver goal");
	}

	if (ContainsUnknown(obligation.goal))
	{
		return MakeResult(
			backendName,
			RelaySolverStatus::Unsupported,
			"the solver goal contains an Unknown formula or sort");
	}
	if (obligation.goal.sort != FormulaSort::Bool())
	{
		return MakeResult(
			backendName,
			RelaySolverStatus::EncodingError,
			"the solver goal is not Boolean");
	}

	if (m_backend == nullptr)
	{
		return MakeResult(
			"none",
			RelaySolverStatus::NotRun,
			m_options.disabledReason);
	}

	RelaySolverRequest request;
	request.symbols = &symbols;
	request.goal = &obligation.goal;
	request.timeoutMs = m_options.perObligationTimeoutMs;

	bool hasCountEquality = false;
	bool hasFirstTargetRelation = false;
	bool hasSecondTargetRelation = false;
	for (const RelayConstraint &constraint : constraints)
	{
		if (constraint.role == RelayConstraintRole::SolverGoal)
			continue;
		if (!obligation.sourceFunctionId.empty() &&
			!constraint.sourceFunctionId.empty() &&
			constraint.sourceFunctionId !=
				obligation.sourceFunctionId)
		{
			continue;
		}
		bool selected = false;
		switch (obligation.kind)
		{
		case RelayProofObligationKind::RelayCountUpperBound:
			selected =
				constraint.kind ==
					RelayConstraintKind::RelayCountEquality ||
				constraint.kind ==
					RelayConstraintKind::RelayCountNonNegative;
			if (constraint.kind ==
				RelayConstraintKind::RelayCountEquality)
			{
				hasCountEquality = true;
			}
			break;

		case RelayProofObligationKind::TargetNonAliasCandidate:
		{
			const bool firstSite =
				constraint.relaySiteId ==
					obligation.relaySiteId;
			const bool secondSite =
				constraint.relaySiteId ==
					obligation.relatedRelaySiteId;
			selected =
				(firstSite || secondSite) &&
				(constraint.kind ==
						RelayConstraintKind::RelayTargetRelation ||
					constraint.kind ==
						RelayConstraintKind::RelayGuardNecessity);
			if (constraint.kind ==
				RelayConstraintKind::RelayTargetRelation)
			{
				hasFirstTargetRelation =
					hasFirstTargetRelation || firstSite;
				hasSecondTargetRelation =
					hasSecondTargetRelation || secondSite;
			}
			break;
		}

		case RelayProofObligationKind::BooleanRefinement:
			selected =
				constraint.role ==
					RelayConstraintRole::SolverAssumption;
			break;

		default:
			break;
		}
		if (!selected)
			continue;

		if (ContainsId(obligation.constraintIds, constraint.id) ||
			FormulaEqual(constraint.formula, obligation.goal))
		{
			return MakeResult(
				backendName,
				RelaySolverStatus::EncodingError,
				"circular_assumption_rejected: solver goal was "
				"also selected as an assumption");
		}
		if (ContainsUnknown(constraint.formula))
		{
			return MakeResult(
				backendName,
				RelaySolverStatus::EncodingError,
				"selected solver assumption contains an Unknown "
				"formula or sort: " + constraint.id);
		}
		// Defense in depth: even a missing/misclassified ID cannot turn the
		// goal itself into a proof assumption.
		request.assumptions.push_back(&constraint);
	}

	if (obligation.kind ==
			RelayProofObligationKind::RelayCountUpperBound &&
		!hasCountEquality)
	{
		return MakeResult(
			backendName,
			RelaySolverStatus::Unsupported,
			"a relay-count upper bound requires an independent exact "
			"RelayCountEquality semantic definition");
	}
	if (obligation.kind ==
			RelayProofObligationKind::TargetNonAliasCandidate &&
		(!hasFirstTargetRelation ||
			!hasSecondTargetRelation))
	{
		return MakeResult(
			backendName,
			RelaySolverStatus::Unsupported,
			"target non-alias requires independent target relations "
			"for both relay sites");
	}

	return m_backend->Solve(request);
}

} // namespace solver
} // namespace refinement
} // namespace relay_protocol
} // namespace transpiler
