#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace transpiler {
namespace relay_protocol {
namespace analysis {

enum class RelayDependencyClass : uint8_t
{
	Constant,
	TransactionArgument,
	CurrentScopeKey,
	CurrentScopeState,
	LocalDerived,
	LoopVariable,
	ExternalCallResult,
	Opaque,
};

// The numeric order is the availability lattice. Combining expression
// dependencies takes the latest (largest) stage required by any operand.
enum class RelayAvailabilityStage : uint8_t
{
	CompileTime = 0,
	AdmissionTime = 1,
	AfterScopeLoad = 2,
	DuringExecution = 3,
	Unknown = 4,
};

struct RelayExpressionDependency
{
	std::vector<RelayDependencyClass> classes;
	RelayAvailabilityStage earliestAvailability =
		RelayAvailabilityStage::Unknown;
	bool admissionTimeEvaluable = false;
	std::string reason;
};

} // namespace analysis
} // namespace relay_protocol
} // namespace transpiler
