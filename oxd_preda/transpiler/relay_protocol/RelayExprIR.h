#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace transpiler {
namespace relay_protocol {

struct SourceLocation
{
	uint32_t line = 0;
	uint32_t column = 0;
	uint32_t endLine = 0;
	uint32_t endColumn = 0;
	int64_t startOffset = -1;
	int64_t endOffset = -1;

	bool IsValid() const
	{
		return line != 0;
	}
};

enum class RelayExprKind : uint8_t
{
	Identifier,
	Literal,
	Keyword,
	MemberAccess,
	Index,
	Unary,
	Binary,
	Call,
	Group,
	Opaque,
};

// This is an owning representation. It deliberately keeps no ANTLR context,
// token, ConcreteTypePtr, or string_view alive after compilation.
struct RelayExprIR
{
	RelayExprKind kind = RelayExprKind::Opaque;
	std::string text;
	std::string type;
	std::string op;
	SourceLocation location;
	std::vector<RelayExprIR> children;
	std::string opaqueReason;
};

} // namespace relay_protocol
} // namespace transpiler
