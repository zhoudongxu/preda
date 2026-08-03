#pragma once

#include "../RelayExprIR.h"
#include "../analysis/RelayCardinalityExpr.h"
#include "../analysis/RelayDepthExpr.h"
#include "../refinement/solver/RelaySolverResult.h"

#include <cstdint>
#include <string>
#include <vector>

namespace transpiler {
namespace relay_protocol {
namespace certificate {

enum class RelayPairRelation : uint8_t
{
	MutuallyExclusive,
	MustPrecedeAB,
	MustPrecedeBA,
	CoEmissionIndependent,
	ProvedMayAlias,
	PotentialConflict,
	Unknown,
};

enum class CertificateStatus : uint8_t
{
	Complete,
	Conservative,
	Unknown,
	Unsupported,
};

enum class CertificatePropertyStatus : uint8_t
{
	Proved,
	Conservative,
	Unknown,
	Unsupported,
};

enum class PhysicalWorkBoundKind : uint8_t
{
	Constant,
	ParameterizedUpperBound,
	Unknown,
};

struct RelayPairCertificate
{
	std::string id;
	std::string sourceFunctionId;
	std::string siteA;
	std::string siteB;
	RelayPairRelation relation = RelayPairRelation::Unknown;
	CertificatePropertyStatus status = CertificatePropertyStatus::Unknown;
	std::string reason;
	std::vector<std::string> supportingCfgFactIds;
	std::vector<std::string> supportingConstraintIds;
	std::vector<std::string> supportingSolverResultIds;
	refinement::solver::RelaySolverResult feasibilityResult;
	refinement::solver::RelaySolverResult relationResult;
	std::vector<refinement::solver::RelayCounterexampleValue> counterexample;
	SourceLocation siteALocation;
	SourceLocation siteBLocation;
};

struct RelayLogicalWorkBound
{
	std::string id;
	CertificateStatus status = CertificateStatus::Unknown;
	analysis::RelayCardinalityExpr exact =
		analysis::RelayCardinalityExpr::Unknown("not analyzed");
	analysis::RelayCardinalityExpr upperBound =
		analysis::RelayCardinalityExpr::Unknown("not analyzed");
	std::string reason;
	std::vector<std::string> supportingCfgFactIds;
	std::vector<std::string> supportingConstraintIds;
	std::vector<std::string> supportingSolverResultIds;
};

struct RelayPhysicalWorkBound
{
	std::string id;
	CertificateStatus status = CertificateStatus::Unknown;
	PhysicalWorkBoundKind boundKind = PhysicalWorkBoundKind::Unknown;
	uint64_t constantTerm = 0;
	uint64_t activeShardCountCoefficient = 0;
	std::string expression;
	std::string reason;
	std::vector<std::string> supportingCfgFactIds;
	std::vector<std::string> supportingConstraintIds;
	std::vector<std::string> supportingSolverResultIds;
};

struct RelayTreeDepthBound
{
	std::string id;
	CertificateStatus status = CertificateStatus::Unknown;
	analysis::RelayDepthExpr exact =
		analysis::RelayDepthExpr::Unknown("not analyzed");
	analysis::RelayDepthExpr upperBound =
		analysis::RelayDepthExpr::Unknown("not analyzed");
	std::string reason;
	std::vector<std::string> supportingCfgFactIds;
	std::vector<std::string> supportingConstraintIds;
	std::vector<std::string> supportingSolverResultIds;
};

struct FunctionParallelRelayCertificate
{
	std::string sourceFunctionId;
	CertificateStatus status = CertificateStatus::Unknown;
	std::string reason;
	std::vector<RelayPairCertificate> pairRelations;
	RelayLogicalWorkBound directWorkBound;
	RelayLogicalWorkBound transitiveWorkBound;
	RelayPhysicalWorkBound physicalRouteWork;
	RelayTreeDepthBound depthBound;
};

struct ParallelRelayCertificate
{
	uint32_t extensionSchemaVersion = 1;
	std::vector<FunctionParallelRelayCertificate> functions;

	void Reset()
	{
		extensionSchemaVersion = 1;
		functions.clear();
	}
};

} // namespace certificate
} // namespace relay_protocol
} // namespace transpiler
