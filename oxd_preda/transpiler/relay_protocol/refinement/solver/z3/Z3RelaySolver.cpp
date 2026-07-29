#include "Z3RelaySolver.h"

#include "Z3FormulaEncoder.h"
#include "Z3ModelDecoder.h"
#include "Z3SymbolEncoder.h"

#include <z3++.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <set>
#include <string>

namespace transpiler {
namespace relay_protocol {
namespace refinement {
namespace solver {
namespace z3_backend {
namespace {

using Clock = std::chrono::steady_clock;

uint64_t ElapsedMilliseconds(
	const Clock::time_point &start)
{
	return static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(
			Clock::now() - start)
			.count());
}

void SetElapsed(
	RelaySolverResult &result,
	const Clock::time_point &start)
{
	result.elapsedTimeMs = ElapsedMilliseconds(start);
}

RelaySolverResult EncodingFailure(
	const Clock::time_point &start,
	const std::string &reason,
	RelaySolverResult result = RelaySolverResult())
{
	result.backend = "z3";
	result.status = RelaySolverStatus::EncodingError;
	result.reason = reason;
	SetElapsed(result, start);
	return result;
}

bool FormulaStructurallyEqual(
	const FormulaExpr &left,
	const FormulaExpr &right)
{
	if (left.kind != right.kind ||
		left.sort != right.sort ||
		left.op != right.op ||
		left.symbolId != right.symbolId ||
		left.literalValue != right.literalValue ||
		left.children.size() != right.children.size())
	{
		return false;
	}
	for (size_t index = 0; index < left.children.size(); ++index)
	{
		if (!FormulaStructurallyEqual(
				left.children[index],
				right.children[index]))
		{
			return false;
		}
	}
	return true;
}

unsigned Z3TimeoutValue(uint64_t timeoutMs)
{
	const uint64_t maximum =
		static_cast<uint64_t>(
			std::numeric_limits<unsigned>::max());
	return static_cast<unsigned>(
		std::min(timeoutMs, maximum));
}

void ConfigureTimeout(
	::z3::solver &solver,
	::z3::context &context,
	uint64_t timeoutMs)
{
	if (timeoutMs == 0)
		return;
	::z3::params parameters(context);
	parameters.set("timeout", Z3TimeoutValue(timeoutMs));
	solver.set(parameters);
}

uint64_t RemainingTimeout(
	const Clock::time_point &start,
	uint64_t timeoutMs)
{
	if (timeoutMs == 0)
		return 0;
	const uint64_t elapsed = ElapsedMilliseconds(start);
	return elapsed >= timeoutMs ? 0 : timeoutMs - elapsed;
}

class ScopedSolverFrame
{
public:
	explicit ScopedSolverFrame(::z3::solver &solver)
		: m_solver(solver)
	{
		m_solver.push();
	}

	~ScopedSolverFrame()
	{
		try
		{
			m_solver.pop();
		}
		catch (...)
		{
			// Destructors must not mask the original Z3 result or exception.
		}
	}

private:
	::z3::solver &m_solver;
};

} // namespace

std::string Z3RelaySolver::GetName() const
{
	return "z3";
}

RelaySolverResult Z3RelaySolver::Solve(
	const RelaySolverRequest &request) const
{
	const Clock::time_point start = Clock::now();
	RelaySolverResult result;
	result.backend = GetName();

	if (request.symbols == nullptr)
	{
		return EncodingFailure(
			start,
			"solver request has no refinement symbol table");
	}
	if (request.goal == nullptr)
	{
		return EncodingFailure(
			start,
			"solver request has no goal formula");
	}
	if (request.goal->sort != FormulaSort::Bool())
	{
		return EncodingFailure(
			start,
			"solver goal must have Bool sort");
	}

	try
	{
		::z3::context context;
		Z3SymbolEncoder symbolEncoder(
			context,
			*request.symbols);
		if (!symbolEncoder.IsValid())
		{
			return EncodingFailure(
				start,
				"invalid refinement symbol table: " +
				symbolEncoder.GetValidationError());
		}
		Z3FormulaEncoder formulaEncoder(
			context,
			symbolEncoder);
		::z3::solver solver(context);
		ConfigureTimeout(
			solver,
			context,
			request.timeoutMs);

		std::set<std::string> seenAssumptionIds;
		for (const RelayConstraint *assumption :
			request.assumptions)
		{
			if (assumption == nullptr)
			{
				return EncodingFailure(
					start,
					"solver request contains a null assumption",
					std::move(result));
			}
			if (assumption->role ==
				RelayConstraintRole::SolverGoal)
			{
				return EncodingFailure(
					start,
					"circular_assumption_rejected: constraint '" +
					assumption->id +
					"' is classified as a SolverGoal",
					std::move(result));
			}
			if (FormulaStructurallyEqual(
					assumption->formula,
					*request.goal))
			{
				return EncodingFailure(
					start,
					"circular_assumption_rejected: constraint '" +
					assumption->id +
					"' is structurally identical to the solver goal",
					std::move(result));
			}
			if (assumption->formula.sort != FormulaSort::Bool())
			{
				return EncodingFailure(
					start,
					"solver assumption '" + assumption->id +
					"' must have Bool sort",
					std::move(result));
			}

			const Z3ExprEncodingResult encoded =
				formulaEncoder.Encode(assumption->formula);
			if (!encoded.Succeeded())
			{
				return EncodingFailure(
					start,
					"cannot encode solver assumption '" +
					assumption->id + "': " + encoded.reason,
					std::move(result));
			}
			solver.add(*encoded.value);
			if (seenAssumptionIds.insert(
					assumption->id).second)
			{
				result.assumptionConstraintIds.push_back(
					assumption->id);
			}
		}

		// Stage one checks the assumptions independently of the goal. This
		// prevents vacuous proofs from an inconsistent compiler fact set.
		const ::z3::check_result assumptionsStatus =
			solver.check();
		if (assumptionsStatus == ::z3::unsat)
		{
			result.status =
				RelaySolverStatus::InconsistentAssumptions;
			result.reason =
				"solver assumptions are inconsistent";
			SetElapsed(result, start);
			return result;
		}
		if (assumptionsStatus == ::z3::unknown)
		{
			result.status = RelaySolverStatus::Unknown;
			result.reason =
				"Z3 returned unknown while checking assumptions: " +
				solver.reason_unknown();
			SetElapsed(result, start);
			return result;
		}

		const Z3ExprEncodingResult encodedGoal =
			formulaEncoder.Encode(*request.goal);
		if (!encodedGoal.Succeeded())
		{
			result.status = RelaySolverStatus::EncodingError;
			result.reason =
				"cannot encode solver goal: " +
				encodedGoal.reason;
			SetElapsed(result, start);
			return result;
		}

		if (request.timeoutMs != 0)
		{
			const uint64_t remaining =
				RemainingTimeout(start, request.timeoutMs);
			if (remaining == 0)
			{
				result.status = RelaySolverStatus::Unknown;
				result.reason =
					"per-obligation timeout was exhausted after "
					"checking assumption consistency";
				SetElapsed(result, start);
				return result;
			}
			ConfigureTimeout(solver, context, remaining);
		}

		ScopedSolverFrame frame(solver);
		solver.add(!*encodedGoal.value);
		const ::z3::check_result counterexampleStatus =
			solver.check();
		if (counterexampleStatus == ::z3::unsat)
		{
			result.status = RelaySolverStatus::Proved;
			result.reason =
				"the assumptions and negated goal are unsatisfiable";
			SetElapsed(result, start);
			return result;
		}
		if (counterexampleStatus == ::z3::unknown)
		{
			result.status = RelaySolverStatus::Unknown;
			result.reason =
				"Z3 returned unknown while checking the negated goal: " +
				solver.reason_unknown();
			SetElapsed(result, start);
			return result;
		}

		const ::z3::model model = solver.get_model();
		Z3ModelDecoder decoder(symbolEncoder);
		Z3ModelDecodeResult decoded = decoder.Decode(
			model,
			request.assumptions,
			*request.goal);
		if (!decoded.Succeeded())
		{
			result.status = RelaySolverStatus::EncodingError;
			result.reason = decoded.reason;
			SetElapsed(result, start);
			return result;
		}

		result.status = RelaySolverStatus::Disproved;
		result.reason =
			"Z3 found a model satisfying the assumptions and "
			"negated goal";
		result.projectedCounterexample =
			std::move(decoded.values);
		SetElapsed(result, start);
		return result;
	}
	catch (const ::z3::exception &error)
	{
		result.status = RelaySolverStatus::EncodingError;
		result.reason =
			std::string("Z3 backend failure: ") + error.msg();
	}
	catch (const std::exception &error)
	{
		result.status = RelaySolverStatus::EncodingError;
		result.reason =
			std::string("solver backend failure: ") +
			error.what();
	}
	catch (...)
	{
		result.status = RelaySolverStatus::EncodingError;
		result.reason =
			"solver backend failed with an unknown exception";
	}
	SetElapsed(result, start);
	return result;
}

} // namespace z3_backend
} // namespace solver
} // namespace refinement
} // namespace relay_protocol
} // namespace transpiler
