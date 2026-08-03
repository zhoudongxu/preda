#include "RelayPlanLoader.h"

#include "../../3rdParty/nlohmann/json.hpp"
#include "../../native/types/typetraits.h"
#include "../../../oxd_libsec/oxd_libsec.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace oxd {
namespace relay_plan {
namespace {

using Json = nlohmann::ordered_json;

std::string ReadFile(const std::filesystem::path &path)
{
	std::ifstream input(path, std::ios::binary);
	if (!input)
		return {};
	std::ostringstream contents;
	contents << input.rdbuf();
	return contents.str();
}

std::string BytesToHex(const uint8_t *data, size_t size)
{
	static constexpr char Digits[] = "0123456789abcdef";
	std::string result(size * 2, '\0');
	for (size_t index = 0; index < size; ++index)
	{
		result[index * 2] = Digits[data[index] >> 4];
		result[index * 2 + 1] = Digits[data[index] & 0x0f];
	}
	return result;
}

std::string JsonIdentity(const Json &object, const char *field)
{
	auto found = object.find(field);
	if (found == object.end() || found->is_null())
		return {};
	if (found->is_string())
		return found->get<std::string>();
	return found->dump();
}

bool ExactIdentityMatches(
	const std::string &expected,
	const std::string &actual)
{
	return expected.empty() || expected == actual;
}

bool DigestMatches(const std::string &expected, const std::string &actual)
{
	return expected.empty() ||
		RelayPlanLoader::NormalizeIdentity(expected) ==
			RelayPlanLoader::NormalizeIdentity(actual);
}

RelayKind ParseRelayKind(const std::string &value)
{
	if (value == "custom_scope") return RelayKind::CustomScope;
	if (value == "global") return RelayKind::Global;
	if (value == "shards" || value == "all_shards")
		return RelayKind::AllShards;
	if (value == "next" || value == "deferred")
		return RelayKind::DeferredNext;
	return RelayKind::Unknown;
}

ScopeKind ParseScopeKind(const std::string &value)
{
	if (value == "none") return ScopeKind::None;
	if (value == "global") return ScopeKind::Global;
	if (value == "shard") return ScopeKind::Shard;
	if (value == "address") return ScopeKind::Address;
	if (value == "uint32") return ScopeKind::Uint32;
	if (value == "uint64") return ScopeKind::Uint64;
	if (value == "uint96") return ScopeKind::Uint96;
	if (value == "uint128") return ScopeKind::Uint128;
	if (value == "uint160") return ScopeKind::Uint160;
	if (value == "uint256") return ScopeKind::Uint256;
	if (value == "uint512") return ScopeKind::Uint512;
	return ScopeKind::Unknown;
}

FanoutKind ParseFanoutKind(const std::string &value)
{
	if (value == "single_target") return FanoutKind::SingleTarget;
	if (value == "all_shards") return FanoutKind::AllShards;
	return FanoutKind::Unknown;
}

RelaySourceLocation ParseLocation(const Json &value)
{
	RelaySourceLocation result;
	if (!value.is_object())
		return result;
	result.line = value.value("line", int64_t(0));
	result.column = value.value("column", int64_t(0));
	result.endLine = value.value("end_line", int64_t(0));
	result.endColumn = value.value("end_column", int64_t(0));
	result.startOffset = value.value("start_offset", int64_t(-1));
	result.endOffset = value.value("end_offset", int64_t(-1));
	return result;
}

bool ParseCertificateStatus(
	const Json &value,
	ParallelCertificateStatus &out)
{
	if (!value.is_string())
		return false;
	const std::string status = value.get<std::string>();
	if (status == "Proved")
		out = ParallelCertificateStatus::Proved;
	else if (status == "Complete")
		out = ParallelCertificateStatus::Complete;
	else if (status == "Conservative")
		out = ParallelCertificateStatus::Conservative;
	else if (status == "Unknown")
		out = ParallelCertificateStatus::Unknown;
	else if (status == "Unsupported")
		out = ParallelCertificateStatus::Unsupported;
	else
		return false;
	return true;
}

bool ParsePairCertificateRelation(
	const Json &value,
	RelayPairCertificateRelation &out)
{
	if (!value.is_string())
		return false;
	const std::string relation = value.get<std::string>();
	if (relation == "MutuallyExclusive")
		out = RelayPairCertificateRelation::MutuallyExclusive;
	else if (relation == "MustPrecedeAB")
		out = RelayPairCertificateRelation::MustPrecedeAB;
	else if (relation == "MustPrecedeBA")
		out = RelayPairCertificateRelation::MustPrecedeBA;
	else if (relation == "CoEmissionIndependent")
		out = RelayPairCertificateRelation::CoEmissionIndependent;
	else if (relation == "ProvedMayAlias")
	{
		out = RelayPairCertificateRelation::ProvedMayAlias;
	}
	else if (relation == "PotentialConflict" ||
		relation == "MayAliasOrConflict")
	{
		out = RelayPairCertificateRelation::PotentialConflict;
	}
	else if (relation == "Unknown")
		out = RelayPairCertificateRelation::Unknown;
	else
		return false;
	return true;
}

bool ParseUniqueStringArray(
	const Json &object,
	const char *field,
	std::vector<std::string> &out,
	std::string &error)
{
	auto found = object.find(field);
	if (found == object.end() || !found->is_array())
	{
		error = std::string(field) + " array is missing";
		return false;
	}
	std::set<std::string> unique;
	for (const Json &value : *found)
	{
		if (!value.is_string() || value.get<std::string>().empty())
		{
			error = std::string(field) +
				" contains an empty or non-string reference";
			return false;
		}
		const std::string reference = value.get<std::string>();
		if (!unique.insert(reference).second)
		{
			error = std::string(field) +
				" contains a duplicate reference: " + reference;
			return false;
		}
		out.push_back(reference);
	}
	return true;
}

void CollectStructuredIds(const Json &value, std::set<std::string> &ids)
{
	if (value.is_array())
	{
		for (const Json &child : value)
			CollectStructuredIds(child, ids);
		return;
	}
	if (!value.is_object())
		return;
	for (auto field = value.begin(); field != value.end(); ++field)
	{
		const bool identityField = field.key() == "id" ||
			(field.key().size() >= 3 &&
			 field.key().compare(
				 field.key().size() - 3,
				 3,
				 "_id") == 0);
		if (identityField &&
			field->is_string() && !field->get<std::string>().empty())
		{
			ids.insert(field->get<std::string>());
		}
		CollectStructuredIds(*field, ids);
	}
}

struct SolverProofReference
{
	std::string kind;
	std::string solverStatus;
	bool duplicate = false;
};

using SolverProofReferences =
	std::unordered_map<std::string, SolverProofReference>;

bool ValidateEvidenceReferences(
	const std::vector<std::string> &cfgReferences,
	const std::vector<std::string> &constraintReferences,
	const std::vector<std::string> &solverReferences,
	const std::set<std::string> &cfgIds,
	const std::set<std::string> &constraintIds,
	const SolverProofReferences &solverProofs,
	std::string &error)
{
	for (const std::string &reference : cfgReferences)
	{
		if (cfgIds.find(reference) == cfgIds.end())
		{
			error = "supporting CFG fact does not exist: " + reference;
			return false;
		}
	}
	for (const std::string &reference : constraintReferences)
	{
		if (constraintIds.find(reference) == constraintIds.end())
		{
			error = "supporting constraint does not exist: " + reference;
			return false;
		}
	}
	for (const std::string &reference : solverReferences)
	{
		auto found = solverProofs.find(reference);
		if (found == solverProofs.end() || found->second.duplicate)
		{
			error = "supporting solver result is missing or ambiguous: " +
				reference;
			return false;
		}
	}
	return true;
}

std::optional<uint64_t> ParseConstant(const Json &expression)
{
	if (!expression.is_object() ||
		expression.value("kind", std::string()) != "constant")
	{
		return std::nullopt;
	}
	auto value = expression.find("value");
	if (value == expression.end())
		return std::nullopt;
	if (value->is_number_unsigned())
		return value->get<uint64_t>();
	if (value->is_number_integer())
	{
		const int64_t integer = value->get<int64_t>();
		if (integer >= 0)
			return static_cast<uint64_t>(integer);
	}
	if (value->is_string())
	{
		try
		{
			size_t consumed = 0;
			const std::string text = value->get<std::string>();
			const uint64_t parsed = std::stoull(text, &consumed);
			if (consumed == text.size())
				return parsed;
		}
		catch (...)
		{
		}
	}
	return std::nullopt;
}

std::string CardinalityKind(const Json &expression)
{
	if (!expression.is_object())
		return {};
	return expression.value("kind", std::string());
}

bool ParseWorkCertificate(
	const Json &object,
	bool requireMachineBound,
	const std::set<std::string> &cfgIds,
	const std::set<std::string> &constraintIds,
	const SolverProofReferences &solverProofs,
	ManifestWorkCertificate &out,
	std::string &error)
{
	if (!object.is_object())
	{
		error = "work certificate is not an object";
		return false;
	}
	out.certificateId = object.value("certificate_id", std::string());
	if (out.certificateId.empty())
	{
		error = "work certificate id is empty";
		return false;
	}
	auto status = object.find("status");
	if (status == object.end() ||
		!ParseCertificateStatus(*status, out.status))
	{
		error = "work certificate has an invalid status";
		return false;
	}
	if (out.status == ParallelCertificateStatus::Proved)
	{
		error = "work certificate status must not be Proved";
		return false;
	}
	auto reason = object.find("reason");
	if (reason == object.end() || !reason->is_string())
	{
		error = "work certificate reason is missing or non-string";
		return false;
	}
	out.reason = reason->get<std::string>();

	auto exact = object.find("exact");
	auto upper = object.find("upper_bound");
	if (!requireMachineBound &&
		(exact == object.end() || !exact->is_object() ||
		 upper == object.end() || !upper->is_object()))
	{
		error =
			"work certificate exact/upper_bound expressions are missing";
		return false;
	}
	if (exact != object.end() && exact->is_object())
	{
		out.exact = ParseConstant(*exact);
		out.exactExpressionJson = exact->dump();
		out.expressionKind = CardinalityKind(*exact);
	}
	if (upper != object.end() && upper->is_object())
	{
		out.upperBound = ParseConstant(*upper);
		out.upperBoundExpressionJson = upper->dump();
		if (out.expressionKind.empty())
			out.expressionKind = CardinalityKind(*upper);
	}

	if (requireMachineBound)
	{
		auto boundKind = object.find("bound_kind");
		auto constantTerm = object.find("constant_term");
		auto coefficient =
			object.find("active_shard_count_coefficient");
		auto expression = object.find("expression");
		if (boundKind == object.end() || !boundKind->is_string() ||
			constantTerm == object.end() ||
			!constantTerm->is_number_unsigned() ||
			coefficient == object.end() ||
			!coefficient->is_number_unsigned() ||
			expression == object.end() || !expression->is_string())
		{
			error =
				"physical work certificate has malformed machine bound";
			return false;
		}
		const std::string kind = boundKind->get<std::string>();
		if (kind == "Constant")
			out.boundKind = ManifestWorkBoundKind::Constant;
		else if (kind == "ParameterizedUpperBound")
			out.boundKind =
				ManifestWorkBoundKind::ParameterizedUpperBound;
		else if (kind == "Unknown")
			out.boundKind = ManifestWorkBoundKind::Unknown;
		else
		{
			error = "physical work certificate has unknown bound_kind";
			return false;
		}
		out.constantTerm = constantTerm->get<uint64_t>();
		out.activeShardCountCoefficient =
			coefficient->get<uint64_t>();
		out.expression = expression->get<std::string>();
		out.parameterized = out.boundKind ==
			ManifestWorkBoundKind::ParameterizedUpperBound;
		if (out.boundKind == ManifestWorkBoundKind::Constant &&
			out.activeShardCountCoefficient != 0)
		{
			error =
				"constant physical work bound has a shard coefficient";
			return false;
		}
	}
	else
	{
		const std::string exactKind = CardinalityKind(*exact);
		const std::string upperKind = CardinalityKind(*upper);
		out.parameterized =
			(!out.exact && !exactKind.empty() && exactKind != "unknown") ||
			(!out.upperBound && !upperKind.empty() &&
			 upperKind != "unknown");
		out.boundKind = out.exact || out.upperBound
			? ManifestWorkBoundKind::Constant
			: ManifestWorkBoundKind::Unknown;
	}

	if (!ParseUniqueStringArray(
			object,
			"supporting_cfg_fact_ids",
			out.supportingCfgFactIds,
			error) ||
		!ParseUniqueStringArray(
			object,
			"supporting_constraint_ids",
			out.supportingConstraintIds,
			error) ||
		!ParseUniqueStringArray(
			object,
			"supporting_solver_result_ids",
			out.supportingSolverResultIds,
			error))
	{
		return false;
	}
	return ValidateEvidenceReferences(
		out.supportingCfgFactIds,
		out.supportingConstraintIds,
		out.supportingSolverResultIds,
		cfgIds,
		constraintIds,
		solverProofs,
		error);
}

bool ParsePairCertificate(
	const Json &object,
	const BoundRelayManifest &manifest,
	const std::set<std::string> &cfgIds,
	const std::set<std::string> &constraintIds,
	const SolverProofReferences &solverProofs,
	ManifestRelayPairCertificate &out,
	std::string &error)
{
	if (!object.is_object())
	{
		error = "pair certificate is not an object";
		return false;
	}
	out.certificateId = object.value("certificate_id", std::string());
	out.siteA = object.value("site_a", std::string());
	out.siteB = object.value("site_b", std::string());
	out.reason = object.value("reason", std::string());
	if (out.certificateId.empty() || out.siteA.empty() ||
		out.siteB.empty() || out.siteA == out.siteB)
	{
		error =
			"pair certificate has an empty id/site or repeats one site";
		return false;
	}
	if (manifest.ordinalBySiteId.find(out.siteA) ==
			manifest.ordinalBySiteId.end() ||
		manifest.ordinalBySiteId.find(out.siteB) ==
			manifest.ordinalBySiteId.end())
	{
		error = "pair certificate references an unknown relay site";
		return false;
	}
	auto relation = object.find("relation");
	if (relation == object.end() ||
		!ParsePairCertificateRelation(*relation, out.relation))
	{
		error = "pair certificate has an invalid relation";
		return false;
	}
	auto status = object.find("status");
	if (status == object.end() ||
		!ParseCertificateStatus(*status, out.status))
	{
		error = "pair certificate has an invalid status";
		return false;
	}
	if (out.status == ParallelCertificateStatus::Complete)
	{
		error = "pair certificate status must not be Complete";
		return false;
	}
	auto reason = object.find("reason");
	if (reason == object.end() || !reason->is_string())
	{
		error = "pair certificate reason is missing or non-string";
		return false;
	}
	out.reason = reason->get<std::string>();
	if (!ParseUniqueStringArray(
			object,
			"supporting_cfg_fact_ids",
			out.supportingCfgFactIds,
			error) ||
		!ParseUniqueStringArray(
			object,
			"supporting_constraint_ids",
			out.supportingConstraintIds,
			error) ||
		!ParseUniqueStringArray(
			object,
			"supporting_solver_result_ids",
			out.supportingSolverResultIds,
			error))
	{
		return false;
	}
	if (!ValidateEvidenceReferences(
			out.supportingCfgFactIds,
			out.supportingConstraintIds,
			out.supportingSolverResultIds,
			cfgIds,
			constraintIds,
			solverProofs,
			error))
	{
		return false;
	}

	const bool strongRelation =
		out.relation ==
			RelayPairCertificateRelation::MutuallyExclusive ||
		out.relation ==
			RelayPairCertificateRelation::MustPrecedeAB ||
		out.relation ==
			RelayPairCertificateRelation::MustPrecedeBA ||
		out.relation ==
			RelayPairCertificateRelation::CoEmissionIndependent ||
		out.relation ==
			RelayPairCertificateRelation::ProvedMayAlias;
	if (strongRelation &&
		out.status != ParallelCertificateStatus::Proved)
	{
		error = "strong pair certificate relation is not Proved";
		return false;
	}
	if (out.relation ==
			RelayPairCertificateRelation::PotentialConflict &&
		out.status == ParallelCertificateStatus::Proved)
	{
		error = "PotentialConflict certificate must not claim Proved status";
		return false;
	}
	if ((out.relation ==
			 RelayPairCertificateRelation::MustPrecedeAB ||
		 out.relation ==
			 RelayPairCertificateRelation::MustPrecedeBA) &&
		out.supportingCfgFactIds.empty())
	{
		error = "MustPrecede certificate has no supporting CFG fact";
		return false;
	}

	auto hasSolverEvidence = [&](const char *kind, const char *status)
	{
		for (const std::string &reference : out.supportingSolverResultIds)
		{
			auto found = solverProofs.find(reference);
			if (found != solverProofs.end() &&
				found->second.kind == kind &&
				found->second.solverStatus == status)
			{
				return true;
			}
		}
		return false;
	};
	if (out.relation ==
			RelayPairCertificateRelation::MutuallyExclusive &&
		!hasSolverEvidence("RelayMutualExclusion", "Proved"))
	{
		error = "MutuallyExclusive certificate lacks a Proved "
			"RelayMutualExclusion obligation";
		return false;
	}
	if (out.relation ==
			RelayPairCertificateRelation::CoEmissionIndependent &&
		(!hasSolverEvidence("RelayMutualExclusion", "Disproved") ||
		 !hasSolverEvidence("RelayTargetIndependence", "Proved")))
	{
		error = "CoEmissionIndependent certificate lacks a co-emission "
			"SAT witness or Proved RelayTargetIndependence obligation";
		return false;
	}

	auto counterexample = object.find("counterexample");
	if (counterexample == object.end())
	{
		error = "pair certificate counterexample field is missing";
		return false;
	}
	if (out.relation ==
			RelayPairCertificateRelation::ProvedMayAlias)
	{
		const bool counterexampleEmpty = counterexample->is_null() ||
			(counterexample->is_string() &&
			 counterexample->get<std::string>().empty()) ||
			(counterexample->is_array() && counterexample->empty()) ||
			(counterexample->is_object() && counterexample->empty());
		if (!hasSolverEvidence("RelayMutualExclusion", "Disproved") ||
			!hasSolverEvidence("RelayTargetIndependence", "Disproved") ||
			counterexampleEmpty)
		{
			error = "ProvedMayAlias certificate lacks co-emission/alias "
				"counterexample evidence";
			return false;
		}
	}
	out.counterexampleJson = counterexample->dump();
	auto location = object.find("location");
	if (location == object.end() || !location->is_object())
	{
		error = "pair certificate location object is missing";
		return false;
	}
	auto locationA = location->find("site_a");
	auto locationB = location->find("site_b");
	if (locationA == location->end() || !locationA->is_object() ||
		locationB == location->end() || !locationB->is_object())
	{
		error = "pair certificate location lacks site_a/site_b";
		return false;
	}
	out.locationA = ParseLocation(*locationA);
	out.locationB = ParseLocation(*locationB);
	return true;
}

bool ParseParallelCertificate(
	const Json &root,
	const std::set<std::string> &cfgIds,
	const std::set<std::string> &constraintIds,
	const SolverProofReferences &solverProofs,
	BoundRelayManifest &manifest,
	std::string &error)
{
	auto section = root.find("parallel_certificate");
	if (section == root.end())
		return true;
	if (!section->is_object() ||
		!section->contains("extension_schema_version") ||
		!(*section)["extension_schema_version"].is_number_unsigned() ||
		(*section)["extension_schema_version"].get<uint32_t>() != 1)
	{
		error =
			"parallel_certificate extension schema version is unsupported";
		return false;
	}
	manifest.parallelCertificateExtensionSchemaVersion = 1;
	auto functions = section->find("functions");
	if (functions == section->end() || !functions->is_array())
	{
		error = "parallel_certificate functions array is missing";
		return false;
	}
	std::set<std::string> certificateIds;
	for (const Json &functionJson : *functions)
	{
		if (!functionJson.is_object())
		{
			error = "parallel certificate function is not an object";
			return false;
		}
		ManifestFunctionParallelCertificate function;
		function.sourceFunctionId = functionJson.value(
			"source_function_id",
			std::string());
		if (function.sourceFunctionId.empty() ||
			manifest.functionsById.find(function.sourceFunctionId) ==
				manifest.functionsById.end())
		{
			error =
				"parallel certificate references an unknown source function";
			return false;
		}
		auto status = functionJson.find("certificate_status");
		if (status == functionJson.end() ||
			!ParseCertificateStatus(*status, function.status))
		{
			error = "parallel certificate function has an invalid status";
			return false;
		}
		if (function.status == ParallelCertificateStatus::Proved)
		{
			error = "function certificate status must not be Proved";
			return false;
		}
		auto reason = functionJson.find("reason");
		if (reason == functionJson.end() || !reason->is_string())
		{
			error = "parallel certificate function reason is missing";
			return false;
		}
		function.reason = reason->get<std::string>();

		auto pairs = functionJson.find("pair_relations");
		if (pairs == functionJson.end() || !pairs->is_array())
		{
			error = "parallel certificate pair_relations array is missing";
			return false;
		}
		for (const Json &pairJson : *pairs)
		{
			ManifestRelayPairCertificate pair;
			if (!ParsePairCertificate(
					pairJson,
					manifest,
					cfgIds,
					constraintIds,
					solverProofs,
					pair,
					error))
			{
				return false;
			}
			if (!certificateIds.insert(pair.certificateId).second)
			{
				error = "parallel certificate id is duplicated: " +
					pair.certificateId;
				return false;
			}
			function.pairRelations.push_back(std::move(pair));
		}

		auto parseWork = [&](const char *preferred,
						 const char *fallback,
						 bool requireMachineBound,
						 ManifestWorkCertificate &destination)
		{
			auto work = functionJson.find(preferred);
			if (work != functionJson.end() && fallback != nullptr)
			{
				auto alias = functionJson.find(fallback);
				if (alias != functionJson.end() && *alias != *work)
				{
					error = std::string("parallel certificate aliases disagree: ") +
						preferred + " and " + fallback;
					return false;
				}
			}
			if (work == functionJson.end() && fallback != nullptr)
				work = functionJson.find(fallback);
			if (work == functionJson.end())
			{
				error = std::string("parallel certificate ") +
					preferred + " object is missing";
				return false;
			}
			return ParseWorkCertificate(
				*work,
				requireMachineBound,
				cfgIds,
				constraintIds,
				solverProofs,
				destination,
				error);
		};
		if (!parseWork(
				"direct_logical_work",
				"direct_work_bound",
				false,
				function.directLogicalWork) ||
			!parseWork(
				"transitive_logical_work",
				"transitive_work_bound",
				false,
				function.transitiveLogicalWork) ||
			!parseWork(
				"physical_route_work",
				nullptr,
				true,
				function.physicalRouteWork) ||
			!parseWork(
				"relay_tree_depth",
				"depth_bound",
				false,
				function.relayTreeDepth))
		{
			return false;
		}

		const ManifestWorkCertificate *workCertificates[] = {
			&function.directLogicalWork,
			&function.transitiveLogicalWork,
			&function.physicalRouteWork,
			&function.relayTreeDepth,
		};
		for (const ManifestWorkCertificate *work : workCertificates)
		{
			if (!certificateIds.insert(work->certificateId).second)
			{
				error = "parallel certificate id is duplicated: " +
					work->certificateId;
				return false;
			}
		}

		auto inserted = manifest.parallelCertificatesByFunction.emplace(
			function.sourceFunctionId,
			std::move(function));
		if (!inserted.second)
		{
			error =
				"parallel certificate source function is duplicated";
			return false;
		}
	}
	return true;
}

struct PropertyEvidenceRecord
{
	PropertyEvidence evidence;
	bool present = false;
	bool duplicate = false;
};

using EvidenceByFunction =
	std::unordered_map<std::string, PropertyEvidenceRecord>;

PropertyEvidence ParseEvidence(const Json &obligation)
{
	PropertyEvidence evidence;
	evidence.propertyId = obligation.value("id", std::string());
	evidence.obligationStatus =
		obligation.value("status", std::string());
	evidence.proofRole =
		obligation.value("proof_role", std::string());

	auto solver = obligation.find("solver_result");
	if (solver != obligation.end() && solver->is_object())
	{
		evidence.backend = solver->value("backend", std::string());
		evidence.solverStatus =
			solver->value("status", std::string());
		auto assumptions = solver->find("assumption_constraint_ids");
		if (assumptions != solver->end() && assumptions->is_array())
		{
			for (const Json &assumption : *assumptions)
			{
				if (assumption.is_string())
				evidence.assumptionConstraintIds.push_back(
					assumption.get<std::string>());
			}
		}
	}
	return evidence;
}

void AddEvidence(
	EvidenceByFunction &records,
	const std::string &functionId,
	PropertyEvidence evidence)
{
	if (functionId.empty())
		return;
	PropertyEvidenceRecord &record = records[functionId];
	if (record.present)
	{
		record.duplicate = true;
		return;
	}
	record.present = true;
	record.evidence = std::move(evidence);
}

bool IsExactCountEvidenceAccepted(const PropertyEvidenceRecord *record)
{
	return record != nullptr &&
		record->present &&
		!record->duplicate &&
		!record->evidence.propertyId.empty() &&
		record->evidence.obligationStatus == "Generated" &&
		record->evidence.proofRole == "EstablishedByConstruction" &&
		record->evidence.backend == "compiler" &&
		record->evidence.solverStatus == "EstablishedByConstruction";
}

bool IsUpperBoundEvidenceAccepted(const PropertyEvidenceRecord *record)
{
	return record != nullptr &&
		record->present &&
		!record->duplicate &&
		!record->evidence.propertyId.empty() &&
		record->evidence.obligationStatus == "Generated" &&
		record->evidence.proofRole == "SolverGoal" &&
		record->evidence.solverStatus == "Proved";
}

const PropertyEvidenceRecord *FindEvidence(
	const EvidenceByFunction &records,
	const std::string &functionId)
{
	auto found = records.find(functionId);
	return found == records.end() ? nullptr : &found->second;
}

std::string EvidenceFailure(
	const PropertyEvidenceRecord *record,
	const char *property)
{
	if (record == nullptr || !record->present)
		return std::string(property) + " evidence is missing";
	if (record->duplicate)
		return std::string(property) + " evidence is duplicated";
	if (record->evidence.obligationStatus != "Generated")
		return std::string(property) +
			" obligation is not Generated";
	if (record->evidence.solverStatus.empty())
		return std::string(property) +
			" has Generated-only evidence with no solver result";
	return std::string(property) + " proof status is " +
		record->evidence.solverStatus;
}

void AppendReason(std::string &destination, const std::string &reason)
{
	if (reason.empty())
		return;
	if (!destination.empty())
		destination += "; ";
	destination += reason;
}

void BuildFunctionPlans(
	BoundRelayManifest &manifest,
	const EvidenceByFunction &exactEvidence,
	const EvidenceByFunction &upperEvidence)
{
	for (const auto &siteEntry : manifest.sitesByOrdinal)
	{
		const BoundRelaySite &site = siteEntry.second;
		auto functionIt =
			manifest.functionsById.find(site.sourceFunctionId);
		if (functionIt == manifest.functionsById.end())
			continue;

		FunctionRelayPlan &function = functionIt->second;
		switch (site.relayKind)
		{
		case RelayKind::Global:
			function.mayGlobal = true;
			break;
		case RelayKind::AllShards:
			function.mayBroadcast = true;
			break;
		case RelayKind::CustomScope:
			function.mayCustom = true;
			break;
		case RelayKind::DeferredNext:
			function.mayDeferred = true;
			break;
		case RelayKind::Unknown:
			function.hasUnknownRelayKind = true;
			break;
		}
	}

	for (auto &entry : manifest.functionsById)
	{
		FunctionRelayPlan &function = entry.second;
		function.bindingTrusted = manifest.bindingTrusted;
		function.functionOpcodeTrusted =
			function.hasExportedOpcode &&
			manifest.functionIdByOpcode.find(function.exportedOpcode) !=
				manifest.functionIdByOpcode.end() &&
			manifest.functionIdByOpcode.at(function.exportedOpcode) ==
				function.sourceFunctionId;

		const PropertyEvidenceRecord *exact =
			FindEvidence(exactEvidence, function.sourceFunctionId);
		if (function.exactDirectRelayCount)
		{
			if (function.hasUnmodeledRelayReachableCall)
			{
				function.directCount.ineligibleReason =
					"function has an unmodeled relay-reachable call";
			}
			else if (IsExactCountEvidenceAccepted(exact))
			{
				function.directCount.kind =
					CountPlanKind::ExactConstant;
				function.directCount.value =
					*function.exactDirectRelayCount;
				function.directCount.evidence = exact->evidence;
				function.directCount.sourcePropertyId =
					exact->evidence.propertyId;
			}
			else
			{
				function.directCount.ineligibleReason =
					EvidenceFailure(exact, "exact relay count");
				if (exact != nullptr)
					function.directCount.evidence = exact->evidence;
			}
		}
		else if (
			!function.relayCountExpressionKind.empty() &&
			function.relayCountExpressionKind != "unknown")
		{
			function.directCount.kind =
				CountPlanKind::SymbolicUnsupportedAtRuntime;
			function.directCount.ineligibleReason =
				"exact relay count is symbolic at runtime";
		}
		else
		{
			function.directCount.ineligibleReason =
				"exact relay count is unknown";
		}

		const PropertyEvidenceRecord *upper =
			FindEvidence(upperEvidence, function.sourceFunctionId);
		if (function.directRelayCountUpperBound)
		{
			if (function.hasUnmodeledRelayReachableCall)
			{
				function.directCountUpperBound.ineligibleReason =
					"function has an unmodeled relay-reachable call";
			}
			else if (IsUpperBoundEvidenceAccepted(upper))
			{
				function.directCountUpperBound.kind =
					CountPlanKind::ProvedConstantUpperBound;
				function.directCountUpperBound.value =
					*function.directRelayCountUpperBound;
				function.directCountUpperBound.evidence =
					upper->evidence;
				function.directCountUpperBound.sourcePropertyId =
					upper->evidence.propertyId;
			}
			else
			{
				function.directCountUpperBound.ineligibleReason =
					EvidenceFailure(
						upper,
						"relay count upper bound");
				if (upper != nullptr)
					function.directCountUpperBound.evidence =
						upper->evidence;
			}
		}
		else if (
			!function.relayCountUpperBoundExpressionKind.empty() &&
			function.relayCountUpperBoundExpressionKind != "unknown")
		{
			function.directCountUpperBound.kind =
				CountPlanKind::SymbolicUnsupportedAtRuntime;
			function.directCountUpperBound.ineligibleReason =
				"relay count upper bound is symbolic at runtime";
		}
		else
		{
			function.directCountUpperBound.ineligibleReason =
				"relay count upper bound is unknown";
		}

		function.fanoutEligible = true;
		for (FanoutKind fanout : function.fanout)
		{
			if (fanout == FanoutKind::Unknown)
			{
				function.fanoutEligible = false;
				break;
			}
		}

		const CountPlan &selected =
			function.directCount.IsUsableForReserve()
				? function.directCount
				: function.directCountUpperBound;
		function.reserveEligible =
			manifest.bindingTrusted &&
			function.functionOpcodeTrusted &&
			selected.IsUsableForReserve();
		function.fanoutEligible =
			manifest.bindingTrusted &&
			function.functionOpcodeTrusted &&
			function.fanoutEligible;
		function.optimizationEligible = function.reserveEligible;

		if (!manifest.bindingTrusted)
			AppendReason(
				function.ineligibleReason,
				"artifact binding lacks an independently trusted manifest hash");
		if (!function.functionOpcodeTrusted)
			AppendReason(
				function.ineligibleReason,
				"function/exported-opcode identity is unavailable or ambiguous");
		if (!selected.IsUsableForReserve())
		{
			AppendReason(
				function.ineligibleReason,
				function.directCount.ineligibleReason);
			AppendReason(
				function.ineligibleReason,
				function.directCountUpperBound.ineligibleReason);
		}
	}
}

struct ParsedCandidate
{
	ManifestLoadStatus status = ManifestLoadStatus::ManifestParseError;
	std::string diagnostic;
	std::string sourcePath;
	ArtifactIdentity observedBinding;
	std::shared_ptr<BoundRelayManifest> manifest;
};

ParsedCandidate ParseCandidateUnchecked(
	const std::filesystem::path &path,
	const ExpectedArtifactBinding &expected)
{
	ParsedCandidate result;
	result.sourcePath = path.string();
	const std::string text = ReadFile(path);
	if (text.empty())
	{
		result.diagnostic =
			"manifest is empty or unreadable: " + path.string();
		return result;
	}

	Json root;
	try
	{
		root = Json::parse(text);
	}
	catch (const std::exception &exception)
	{
		result.diagnostic =
			"manifest JSON parse failed: " +
			std::string(exception.what());
		return result;
	}

	const uint32_t schema = root.value("schema_version", uint32_t(0));
	if (schema != 5)
	{
		result.status = ManifestLoadStatus::ManifestSchemaUnsupported;
		result.diagnostic =
			"runtime relay plans require relay manifest schema version 5";
		return result;
	}

	auto bindingIt = root.find("artifact_binding");
	if (bindingIt == root.end() || !bindingIt->is_object())
	{
		result.status = ManifestLoadStatus::ManifestBindingMissing;
		result.diagnostic = "artifact_binding object is missing";
		return result;
	}
	const Json &bindingJson = *bindingIt;

	ArtifactIdentity binding;
	binding.dapp = JsonIdentity(bindingJson, "dapp");
	binding.contract = JsonIdentity(bindingJson, "contract");
	binding.transpilerVersion =
		JsonIdentity(bindingJson, "transpiler_version");
	binding.intermediateHash =
		JsonIdentity(bindingJson, "intermediate_hash");
	binding.moduleId = JsonIdentity(bindingJson, "module_id");
	binding.moduleHashKind =
		JsonIdentity(bindingJson, "module_hash_kind");
	binding.moduleHash = JsonIdentity(bindingJson, "module_hash");
	binding.manifestHashAlgorithm =
		JsonIdentity(bindingJson, "manifest_hash_algorithm");
	binding.manifestHash =
		JsonIdentity(bindingJson, "manifest_hash");
	binding.bindingComplete =
		bindingJson.value("binding_complete", false);
	result.observedBinding = binding;

	const bool requiredMissing =
		binding.dapp.empty() ||
		binding.contract.empty() ||
		binding.moduleId.empty() ||
		!binding.bindingComplete ||
		binding.moduleHashKind != "preda_module_id" ||
		binding.manifestHashAlgorithm != "sha256" ||
		(expected.requireTranspilerVersion &&
			binding.transpilerVersion.empty()) ||
		(expected.requireIntermediateHash &&
			binding.intermediateHash.empty()) ||
		(expected.requireModuleHash && binding.moduleHash.empty()) ||
		(expected.requireManifestSelfHash &&
			binding.manifestHash.empty());
	if (requiredMissing)
	{
		result.status = ManifestLoadStatus::ManifestBindingMissing;
		result.diagnostic =
			"artifact_binding is missing one or more required identity fields";
		return result;
	}

	if (!ExactIdentityMatches(expected.identity.dapp, binding.dapp) ||
		!ExactIdentityMatches(
			expected.identity.contract,
			binding.contract) ||
		!ExactIdentityMatches(
			expected.identity.transpilerVersion,
			binding.transpilerVersion) ||
		!ExactIdentityMatches(
			expected.identity.intermediateHash,
			binding.intermediateHash) ||
		!ExactIdentityMatches(
			expected.identity.moduleId,
			binding.moduleId) ||
		(!expected.identity.moduleHashKind.empty() &&
		 expected.identity.moduleHashKind != binding.moduleHashKind) ||
		!ExactIdentityMatches(
			expected.identity.moduleHash,
			binding.moduleHash))
	{
		result.status = ManifestLoadStatus::ManifestBindingMismatch;
		result.diagnostic =
			"artifact_binding does not match the deployed module identity";
		return result;
	}

	std::string selfHashError;
	const std::string selfHash =
		RelayPlanLoader::ComputeManifestSelfHash(text, &selfHashError);
	if (selfHash.empty() ||
		(expected.requireManifestSelfHash &&
			!DigestMatches(binding.manifestHash, selfHash)) ||
		!DigestMatches(
			expected.identity.manifestHash,
			binding.manifestHash) ||
		(!expected.trustedManifestHash.empty() &&
			(!DigestMatches(
				expected.trustedManifestHash,
				binding.manifestHash) ||
			 !DigestMatches(
				expected.trustedManifestHash,
				selfHash))))
	{
		result.status = ManifestLoadStatus::ManifestHashMismatch;
		result.diagnostic = selfHashError.empty()
			? "manifest self hash or trusted manifest hash does not match"
			: selfHashError;
		return result;
	}

	auto manifest = std::make_shared<BoundRelayManifest>();
	manifest->schemaVersion = schema;
	manifest->sourcePath = path.string();
	manifest->binding = binding;
	manifest->recomputedManifestHash = selfHash;
	manifest->bindingTrusted =
		!expected.trustedManifestHash.empty() &&
		expected.requireTranspilerVersion &&
		!expected.identity.transpilerVersion.empty() &&
		expected.requireIntermediateHash &&
		!expected.identity.intermediateHash.empty() &&
		expected.requireModuleHash &&
		!expected.identity.moduleHash.empty() &&
		!expected.identity.moduleId.empty() &&
		expected.requireManifestSelfHash;

	std::unordered_map<std::string, std::pair<uint32_t, bool>> handlers;
	auto handlersIt = root.find("handlers");
	if (handlersIt != root.end() && handlersIt->is_array())
	{
		for (const Json &handler : *handlersIt)
		{
			const std::string id =
				handler.value("id", std::string());
			const int64_t opcode =
				handler.value("opcode", int64_t(-1));
			if (!id.empty() && opcode >= 0 &&
				static_cast<uint64_t>(opcode) <=
					std::numeric_limits<uint32_t>::max())
			{
				handlers[id] = {
					static_cast<uint32_t>(opcode),
					handler.value("resolved", false),
				};
			}
		}
	}

	auto sitesIt = root.find("relay_sites");
	if (sitesIt == root.end() || !sitesIt->is_array())
	{
		result.status = ManifestLoadStatus::ManifestParseError;
		result.diagnostic = "relay_sites array is missing";
		return result;
	}
	for (const Json &siteJson : *sitesIt)
	{
		if (!siteJson.contains("ordinal") ||
			!siteJson["ordinal"].is_number_unsigned())
		{
			result.status = ManifestLoadStatus::ManifestParseError;
			result.diagnostic =
				"schema-v5 relay site is missing an unsigned ordinal";
			return result;
		}

		BoundRelaySite site;
		site.ordinal =
			siteJson["ordinal"].get<RelaySiteOrdinal>();
		site.id = siteJson.value("id", std::string());
		site.sourceFunctionId =
			siteJson.value("source_function_id", std::string());
		site.handlerId =
			siteJson.value("handler_id", std::string());
		site.relayKind = ParseRelayKind(
			siteJson.value("relay_kind", std::string()));
		site.targetScope = ParseScopeKind(
			siteJson.value("target_scope", std::string()));
		if (siteJson.contains("location"))
			site.location = ParseLocation(siteJson["location"]);

		auto handler = handlers.find(site.handlerId);
		if (handler != handlers.end())
		{
			site.expectedOpcode = handler->second.first;
			site.handlerResolved = handler->second.second;
		}
		if (site.id.empty() ||
			manifest->sitesByOrdinal.find(site.ordinal) !=
				manifest->sitesByOrdinal.end() ||
			manifest->ordinalBySiteId.find(site.id) !=
				manifest->ordinalBySiteId.end())
		{
			result.status = ManifestLoadStatus::ManifestParseError;
			result.diagnostic =
				"relay site id/ordinal is empty or duplicated";
			return result;
		}
		manifest->ordinalBySiteId.emplace(site.id, site.ordinal);
		manifest->sitesByOrdinal.emplace(
			site.ordinal,
			std::move(site));
	}

	auto functionsIt = root.find("functions");
	if (functionsIt != root.end() && functionsIt->is_array())
	{
		for (const Json &functionJson : *functionsIt)
		{
			FunctionRelayPlan function;
			function.sourceFunctionId = functionJson.value(
				"source_function_id",
				std::string());
			auto summary = functionJson.find("summary");
			if (summary != functionJson.end() && summary->is_object())
			{
				if (summary->contains("relay_count"))
				{
					const Json &count = (*summary)["relay_count"];
					function.relayCountExpressionKind =
						CardinalityKind(count);
					function.exactDirectRelayCount =
						ParseConstant(count);
				}
				if (summary->contains("relay_count_upper_bound"))
				{
					const Json &upper =
						(*summary)["relay_count_upper_bound"];
					function.relayCountUpperBoundExpressionKind =
						CardinalityKind(upper);
					function.directRelayCountUpperBound =
						ParseConstant(upper);
				}
				if (summary->contains("max_depth"))
				{
					auto value =
						ParseConstant((*summary)["max_depth"]);
					if (value &&
						*value <=
							std::numeric_limits<uint32_t>::max())
					{
						function.maximumDepth =
							static_cast<uint32_t>(*value);
					}
				}
				function.hasOpaque =
					summary->value("has_opaque", false);
				function.hasUnmodeledRelayReachableCall =
					summary->value(
						"has_unmodeled_relay_reachable_call",
						false);
				if (summary->contains("relay_site_set") &&
					(*summary)["relay_site_set"].is_array())
				{
					function.relaySiteIds =
						(*summary)["relay_site_set"].
							get<std::vector<std::string>>();
				}
				if (summary->contains("fanout") &&
					(*summary)["fanout"].is_array())
				{
					function.fanoutKinds =
						(*summary)["fanout"].
							get<std::vector<std::string>>();
					for (const std::string &fanout :
						function.fanoutKinds)
					{
						function.fanout.push_back(
							ParseFanoutKind(fanout));
					}
				}
			}

			auto opcode = functionJson.find("exported_opcode");
			if (opcode == functionJson.end())
				opcode = functionJson.find("opcode");
			if (opcode != functionJson.end() &&
				(opcode->is_number_unsigned() ||
				 opcode->is_number_integer()))
			{
				std::optional<uint64_t> rawOpcode;
				if (opcode->is_number_unsigned())
					rawOpcode = opcode->get<uint64_t>();
				else
				{
					const int64_t signedOpcode =
						opcode->get<int64_t>();
					if (signedOpcode >= 0)
						rawOpcode =
							static_cast<uint64_t>(signedOpcode);
				}
				if (rawOpcode &&
					*rawOpcode <=
						std::numeric_limits<uint32_t>::max())
				{
					function.exportedOpcode =
						static_cast<uint32_t>(*rawOpcode);
					function.hasExportedOpcode = true;
				}
			}

			if (function.sourceFunctionId.empty())
				continue;
			const std::string sourceFunctionId =
				function.sourceFunctionId;
			if (function.hasExportedOpcode)
			{
				auto inserted =
					manifest->functionIdByOpcode.emplace(
						function.exportedOpcode,
						sourceFunctionId);
				if (!inserted.second &&
					inserted.first->second != sourceFunctionId)
				{
					result.status =
						ManifestLoadStatus::ManifestParseError;
					result.diagnostic =
						"exported opcode is mapped to multiple "
						"source functions";
					return result;
				}
			}
			auto inserted = manifest->functionsById.emplace(
				sourceFunctionId,
				std::move(function));
			if (!inserted.second)
			{
				result.status =
					ManifestLoadStatus::ManifestParseError;
				result.diagnostic =
					"source function id is duplicated";
				return result;
			}
		}
	}

	EvidenceByFunction exactEvidence;
	EvidenceByFunction upperEvidence;
	std::set<std::string> cfgIds;
	auto controlFlow = root.find("control_flow");
	if (controlFlow != root.end())
		CollectStructuredIds(*controlFlow, cfgIds);
	std::set<std::string> constraintIds;
	SolverProofReferences solverProofs;
	auto refinementIt = root.find("refinement");
	if (refinementIt != root.end() && refinementIt->is_object())
	{
		auto constraints = refinementIt->find("constraints");
		if (constraints != refinementIt->end() &&
			constraints->is_array())
		{
			for (const Json &constraint : *constraints)
			{
				if (!constraint.is_object())
					continue;
				const std::string id =
					constraint.value("id", std::string());
				if (!id.empty())
					constraintIds.insert(id);
			}
		}
		auto obligations =
			refinementIt->find("proof_obligations");
		if (obligations != refinementIt->end() &&
			obligations->is_array())
		{
			for (const Json &obligation : *obligations)
			{
				const std::string obligationId =
					obligation.value("id", std::string());
				const std::string kind =
					obligation.value("kind", std::string());
				const std::string functionId =
					obligation.value(
						"source_function_id",
						std::string());
				if (!obligationId.empty())
				{
					SolverProofReference reference;
					reference.kind = kind;
					auto solver = obligation.find("solver_result");
					if (solver != obligation.end() && solver->is_object())
					{
						reference.solverStatus = solver->value(
							"status",
							std::string());
					}
					auto inserted = solverProofs.emplace(
						obligationId,
						std::move(reference));
					if (!inserted.second)
						inserted.first->second.duplicate = true;
				}
				if (kind == "RelayCountEquality")
				{
					AddEvidence(
						exactEvidence,
						functionId,
						ParseEvidence(obligation));
				}
				else if (kind == "RelayCountUpperBound")
				{
					AddEvidence(
						upperEvidence,
						functionId,
						ParseEvidence(obligation));
				}
				else if (kind == "TargetNonAliasCandidate")
				{
					ManifestNonAliasProof proof;
					proof.obligationId =
						obligation.value("id", std::string());
					proof.leftRelaySiteId =
						obligation.value(
							"relay_site_id",
							std::string());
					proof.rightRelaySiteId =
						obligation.value(
							"related_relay_site_id",
							std::string());
					auto solver =
						obligation.find("solver_result");
					proof.solverProved =
						solver != obligation.end() &&
						solver->is_object() &&
						solver->value(
							"status",
							std::string()) == "Proved";
					if (!proof.leftRelaySiteId.empty() &&
						!proof.rightRelaySiteId.empty())
					{
						manifest->nonAliasProofs.push_back(
							std::move(proof));
					}
				}
			}
		}
	}

	std::string certificateError;
	if (!ParseParallelCertificate(
			root,
			cfgIds,
			constraintIds,
			solverProofs,
			*manifest,
			certificateError))
	{
		result.status = ManifestLoadStatus::ManifestParseError;
		result.diagnostic =
			"parallel_certificate parse failed: " + certificateError;
		return result;
	}

	BuildFunctionPlans(*manifest, exactEvidence, upperEvidence);
	result.status = ManifestLoadStatus::Loaded;
	result.manifest = std::move(manifest);
	result.diagnostic = result.manifest->bindingTrusted
		? "manifest loaded and artifact binding verified"
		: "manifest loaded for observation, but optimization trust is "
		  "unavailable without complete independent binding metadata";
	return result;
}

ParsedCandidate ParseCandidate(
	const std::filesystem::path &path,
	const ExpectedArtifactBinding &expected)
{
	try
	{
		return ParseCandidateUnchecked(path, expected);
	}
	catch (const std::exception &exception)
	{
		ParsedCandidate result;
		result.status = ManifestLoadStatus::ManifestParseError;
		result.sourcePath = path.string();
		result.diagnostic =
			"manifest schema parse failed for " + path.string() +
			": " + exception.what();
		return result;
	}
	catch (...)
	{
		ParsedCandidate result;
		result.status = ManifestLoadStatus::ManifestParseError;
		result.sourcePath = path.string();
		result.diagnostic =
			"manifest schema parse failed for " + path.string() +
			": unknown exception";
		return result;
	}
}

int StatusPriority(ManifestLoadStatus status)
{
	switch (status)
	{
	case ManifestLoadStatus::ManifestHashMismatch: return 6;
	case ManifestLoadStatus::ManifestBindingMismatch: return 5;
	case ManifestLoadStatus::ManifestBindingMissing: return 4;
	case ManifestLoadStatus::ManifestSchemaUnsupported: return 3;
	case ManifestLoadStatus::ManifestParseError: return 2;
	case ManifestLoadStatus::ManifestNotFound: return 1;
	case ManifestLoadStatus::Loaded: return 7;
	}
	return 0;
}

} // namespace

RelayPlanLoader::RelayPlanLoader(std::string relayProtocolRoot)
	: m_relayProtocolRoot(std::move(relayProtocolRoot))
{
}

std::shared_ptr<RelayPlanLoadResult> RelayPlanLoader::Load(
	const ExpectedArtifactBinding &expected) const
{
	const auto begin = std::chrono::steady_clock::now();
	auto result = std::make_shared<RelayPlanLoadResult>();
	result->expectedBinding = expected.identity;

	std::vector<std::filesystem::path> candidates;
	if (!expected.manifestPath.empty())
	{
		candidates.emplace_back(expected.manifestPath);
	}
	else if (!expected.identity.moduleId.empty())
	{
		candidates.emplace_back(
			std::filesystem::path(m_relayProtocolRoot) /
			"by_module" /
			(expected.identity.moduleId +
				".relay_protocol.json"));
	}
	else
	{
		const std::filesystem::path directory =
			std::filesystem::path(m_relayProtocolRoot) /
			"by_module";
		std::error_code error;
		if (std::filesystem::is_directory(directory, error))
		{
			for (const auto &entry :
				std::filesystem::directory_iterator(
					directory,
					error))
			{
				if (!error && entry.is_regular_file() &&
					entry.path().extension() == ".json")
				{
					candidates.push_back(entry.path());
				}
			}
		}
		std::sort(candidates.begin(), candidates.end());
	}

	if (candidates.empty())
	{
		result->status = ManifestLoadStatus::ManifestNotFound;
		result->diagnostic =
			"no relay manifest candidate exists for the deployed module";
	}
	else
	{
		result->sourcePath = candidates.front().string();
		ParsedCandidate best;
		best.status = ManifestLoadStatus::ManifestNotFound;
		for (const std::filesystem::path &candidate : candidates)
		{
			std::error_code existsError;
			if (!std::filesystem::is_regular_file(
					candidate,
					existsError))
			{
				continue;
			}
			ParsedCandidate parsed =
				ParseCandidate(candidate, expected);
			if (parsed.status == ManifestLoadStatus::Loaded)
			{
				result->status = parsed.status;
				result->diagnostic =
					std::move(parsed.diagnostic);
				result->sourcePath =
					std::move(parsed.sourcePath);
				result->observedBinding =
					std::move(parsed.observedBinding);
				result->manifest =
					std::move(parsed.manifest);
				break;
			}
			if (StatusPriority(parsed.status) >
				StatusPriority(best.status))
			{
				best = std::move(parsed);
			}
		}
		if (result->status != ManifestLoadStatus::Loaded &&
			best.status == ManifestLoadStatus::ManifestNotFound)
		{
			result->status =
				ManifestLoadStatus::ManifestNotFound;
			result->diagnostic =
				"no relay manifest candidate exists for the "
				"deployed module";
		}
		else if (result->status != ManifestLoadStatus::Loaded)
		{
			result->status = best.status;
			result->diagnostic = best.diagnostic;
			result->sourcePath = best.sourcePath;
			result->observedBinding = best.observedBinding;
		}
	}

	const auto end = std::chrono::steady_clock::now();
	result->elapsedTimeMs =
		std::chrono::duration<double, std::milli>(
			end - begin).count();
	return result;
}

std::string RelayPlanLoader::ComputeManifestSelfHash(
	const std::string &manifestText,
	std::string *error)
{
	try
	{
		Json root = Json::parse(manifestText);
		auto binding = root.find("artifact_binding");
		if (binding == root.end() || !binding->is_object())
		{
			if (error)
				*error =
					"artifact_binding is missing while computing self hash";
			return {};
		}
		binding->erase("manifest_hash");
		const std::string canonical = root.dump();
		if (canonical.size() >
			std::numeric_limits<uint32_t>::max())
		{
			if (error)
				*error = "manifest is too large to hash";
			return {};
		}
		std::array<uint8_t, oxd::SecuritySuite::HASHSIZE> digest{};
		oxd::SecuritySuite::Hash(
			canonical.data(),
			static_cast<uint32_t>(canonical.size()),
			digest.data());
		return BytesToHex(digest.data(), digest.size());
	}
	catch (const std::exception &exception)
	{
		if (error)
		{
			*error =
				"manifest self hash failed: " +
				std::string(exception.what());
		}
		return {};
	}
}

std::string RelayPlanLoader::RuntimeHashIdentity(
	const rvm::HashValue &hash)
{
	rt::String result;
	rvm::RvmTypeToString(hash, result);
	return std::string(result.GetString(), result.GetLength());
}

std::string RelayPlanLoader::NormalizeIdentity(std::string value)
{
	value.erase(
		std::remove_if(
			value.begin(),
			value.end(),
			[](unsigned char character) {
				return std::isspace(character) != 0;
			}),
		value.end());
	std::transform(
		value.begin(),
		value.end(),
		value.begin(),
		[](unsigned char character) {
			return static_cast<char>(std::tolower(character));
		});
	if (value.size() > 2 && value[0] == '0' && value[1] == 'x')
		value.erase(0, 2);
	return value;
}

} // namespace relay_plan
} // namespace oxd
