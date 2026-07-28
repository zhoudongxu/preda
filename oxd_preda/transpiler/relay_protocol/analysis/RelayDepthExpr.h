#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace transpiler {
namespace relay_protocol {
namespace analysis {

enum class RelayDepthExprKind : uint8_t
{
	Constant,
	Maximum,
	Successor,
	Unknown,
};

struct RelayDepthExpr
{
	RelayDepthExprKind kind = RelayDepthExprKind::Unknown;
	uint64_t value = 0;
	std::vector<RelayDepthExpr> children;
	std::string reason;

	static RelayDepthExpr Constant(uint64_t constant)
	{
		RelayDepthExpr result;
		result.kind = RelayDepthExprKind::Constant;
		result.value = constant;
		return result;
	}

	static RelayDepthExpr Unknown(const std::string &why)
	{
		RelayDepthExpr result;
		result.kind = RelayDepthExprKind::Unknown;
		result.reason = why;
		return result;
	}
};

} // namespace analysis
} // namespace relay_protocol
} // namespace transpiler
