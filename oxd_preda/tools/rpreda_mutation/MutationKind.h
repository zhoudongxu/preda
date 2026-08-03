#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace rpreda {
namespace mutation {

enum class MutationKind : uint8_t
{
	RelayTargetReplace,
	RelayTargetSwap,
	RelayArgumentReplace,
	RelayDelete,
	RelayDuplicate,
	RelayOrderSwap,
	GuardNegate,
	HandlerReplace,
	RelayKindChange,
	BroadcastToSingle,
	SingleToBroadcast,
	IntroduceAlias,
	IntroduceRelayRecursion,
	// Semantic source mutations are opt-in.  They deliberately do not belong
	// to AllMutationKinds so the original 13-operator default study remains
	// byte-for-byte reproducible unless --kinds explicitly selects them.
	TargetArithmeticPerturb,
	TargetVariableSwap,
	ArgumentArithmeticPerturb,
	GuardBoundaryChange,
};

inline constexpr std::array<MutationKind, 13> AllMutationKinds = {
	MutationKind::RelayTargetReplace,
	MutationKind::RelayTargetSwap,
	MutationKind::RelayArgumentReplace,
	MutationKind::RelayDelete,
	MutationKind::RelayDuplicate,
	MutationKind::RelayOrderSwap,
	MutationKind::GuardNegate,
	MutationKind::HandlerReplace,
	MutationKind::RelayKindChange,
	MutationKind::BroadcastToSingle,
	MutationKind::SingleToBroadcast,
	MutationKind::IntroduceAlias,
	MutationKind::IntroduceRelayRecursion,
};

inline constexpr std::array<MutationKind, 4> AllSemanticMutationKinds = {
	MutationKind::TargetArithmeticPerturb,
	MutationKind::TargetVariableSwap,
	MutationKind::ArgumentArithmeticPerturb,
	MutationKind::GuardBoundaryChange,
};

inline constexpr std::array<MutationKind, 17> AllKnownMutationKinds = {
	MutationKind::RelayTargetReplace,
	MutationKind::RelayTargetSwap,
	MutationKind::RelayArgumentReplace,
	MutationKind::RelayDelete,
	MutationKind::RelayDuplicate,
	MutationKind::RelayOrderSwap,
	MutationKind::GuardNegate,
	MutationKind::HandlerReplace,
	MutationKind::RelayKindChange,
	MutationKind::BroadcastToSingle,
	MutationKind::SingleToBroadcast,
	MutationKind::IntroduceAlias,
	MutationKind::IntroduceRelayRecursion,
	MutationKind::TargetArithmeticPerturb,
	MutationKind::TargetVariableSwap,
	MutationKind::ArgumentArithmeticPerturb,
	MutationKind::GuardBoundaryChange,
};

inline const char *ToString(MutationKind kind)
{
	switch (kind)
	{
	case MutationKind::RelayTargetReplace: return "RelayTargetReplace";
	case MutationKind::RelayTargetSwap: return "RelayTargetSwap";
	case MutationKind::RelayArgumentReplace: return "RelayArgumentReplace";
	case MutationKind::RelayDelete: return "RelayDelete";
	case MutationKind::RelayDuplicate: return "RelayDuplicate";
	case MutationKind::RelayOrderSwap: return "RelayOrderSwap";
	case MutationKind::GuardNegate: return "GuardNegate";
	case MutationKind::HandlerReplace: return "HandlerReplace";
	case MutationKind::RelayKindChange: return "RelayKindChange";
	case MutationKind::BroadcastToSingle: return "BroadcastToSingle";
	case MutationKind::SingleToBroadcast: return "SingleToBroadcast";
	case MutationKind::IntroduceAlias: return "IntroduceAlias";
	case MutationKind::IntroduceRelayRecursion:
		return "IntroduceRelayRecursion";
	case MutationKind::TargetArithmeticPerturb:
		return "TargetArithmeticPerturb";
	case MutationKind::TargetVariableSwap:
		return "TargetVariableSwap";
	case MutationKind::ArgumentArithmeticPerturb:
		return "ArgumentArithmeticPerturb";
	case MutationKind::GuardBoundaryChange:
		return "GuardBoundaryChange";
	}
	return "Unknown";
}

inline std::optional<MutationKind> ParseMutationKind(const std::string &name)
{
	for (MutationKind kind : AllKnownMutationKinds)
	{
		if (name == ToString(kind))
			return kind;
	}
	return std::nullopt;
}

} // namespace mutation
} // namespace rpreda
