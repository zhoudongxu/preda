#pragma once

#include "../RelayExprIR.h"

#include <cstdint>
#include <string>
#include <vector>

namespace transpiler {
namespace relay_protocol {
namespace analysis {

// A tagged, owning arithmetic expression.  Both the exact relay count and its
// upper bound use this representation so JSON consumers never have to switch
// between scalar and null types.
enum class RelayCardinalityExprKind : uint8_t
{
	Constant,
	Sum,
	Product,
	Ite,
	Unknown,
};

struct RelayCardinalityExpr
{
	RelayCardinalityExprKind kind = RelayCardinalityExprKind::Unknown;
	uint64_t value = 0;
	RelayExprIR predicate;
	std::vector<RelayCardinalityExpr> children;
	std::string reason;

	static RelayCardinalityExpr Constant(uint64_t constant)
	{
		RelayCardinalityExpr result;
		result.kind = RelayCardinalityExprKind::Constant;
		result.value = constant;
		return result;
	}

	static RelayCardinalityExpr Unknown(const std::string &why)
	{
		RelayCardinalityExpr result;
		result.kind = RelayCardinalityExprKind::Unknown;
		result.reason = why;
		return result;
	}
};

} // namespace analysis
} // namespace relay_protocol
} // namespace transpiler
