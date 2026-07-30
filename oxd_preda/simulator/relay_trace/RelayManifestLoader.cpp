#include "RelayManifestLoader.h"

#include "../../3rdParty/nlohmann/json.hpp"
#include "../../../oxd_libsec/oxd_libsec.h"
#include "../../native/types/typetraits.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace oxd {
namespace relay_trace {
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

std::string NormalizeIdentity(std::string value)
{
	value.erase(
		std::remove_if(
			value.begin(),
			value.end(),
			[](unsigned char c) { return std::isspace(c) != 0; }),
		value.end());
	std::transform(
		value.begin(),
		value.end(),
		value.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	if (value.size() > 2 && value[0] == '0' && value[1] == 'x')
		value.erase(0, 2);
	return value;
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
		NormalizeIdentity(expected) == NormalizeIdentity(actual);
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

TraceSourceLocation ParseLocation(const Json &value)
{
	TraceSourceLocation result;
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
			const uint64_t parsed = std::stoull(value->get<std::string>(), &consumed);
			if (consumed == value->get_ref<const std::string &>().size())
				return parsed;
		}
		catch (...)
		{
		}
	}
	return std::nullopt;
}

std::string CacheKey(const ExpectedArtifactBinding &expected)
{
	std::ostringstream key;
	key << NormalizeIdentity(expected.identity.moduleId) << '|'
		<< expected.manifestPath << '\x1f'
		<< expected.identity.dapp << '\x1f'
		<< expected.identity.contract << '\x1f'
		<< expected.identity.transpilerVersion << '\x1f'
		<< expected.identity.intermediateHash << '\x1f'
		<< expected.identity.moduleId << '\x1f'
		<< expected.identity.moduleHashKind << '\x1f'
		<< expected.identity.moduleHash << '\x1f'
		<< NormalizeIdentity(expected.identity.manifestHash) << '\x1f'
		<< NormalizeIdentity(expected.trustedManifestHash) << '\x1f'
		<< expected.requireTranspilerVersion << '\x1f'
		<< expected.requireIntermediateHash << '\x1f'
		<< expected.requireModuleHash << '\x1f'
		<< expected.requireManifestSelfHash;
	return key.str();
}

struct ParsedCandidate
{
	ManifestLoadStatus status = ManifestLoadStatus::ManifestParseError;
	std::string diagnostic;
	std::string sourcePath;
	ArtifactIdentity observedBinding;
	std::shared_ptr<LoadedRelayManifest> manifest;
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
		result.diagnostic = "manifest is empty or unreadable: " + path.string();
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
			"manifest JSON parse failed: " + std::string(exception.what());
		return result;
	}

	const uint32_t schema = root.value("schema_version", uint32_t(0));
	if (schema != 5)
	{
		result.status = ManifestLoadStatus::ManifestSchemaUnsupported;
		result.diagnostic =
			"runtime trace requires relay manifest schema version 5";
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
	binding.manifestHash = JsonIdentity(bindingJson, "manifest_hash");
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
		(expected.requireManifestSelfHash && binding.manifestHash.empty());
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
		RelayManifestLoader::ComputeManifestSelfHash(text, &selfHashError);
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

	auto manifest = std::make_shared<LoadedRelayManifest>();
	manifest->schemaVersion = schema;
	manifest->sourcePath = path.string();
	manifest->binding = binding;
	manifest->recomputedManifestHash = selfHash;

	std::unordered_map<std::string, std::pair<uint32_t, bool>> handlers;
	auto handlersIt = root.find("handlers");
	if (handlersIt != root.end() && handlersIt->is_array())
	{
		for (const Json &handler : *handlersIt)
		{
			const std::string id = handler.value("id", std::string());
			const int64_t opcode = handler.value("opcode", int64_t(-1));
			if (!id.empty() && opcode >= 0)
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

		ManifestRelaySite site;
		site.ordinal = siteJson["ordinal"].get<RelaySiteOrdinal>();
		site.id = siteJson.value("id", std::string());
		site.sourceFunctionId =
			siteJson.value("source_function_id", std::string());
		site.handlerId = siteJson.value("handler_id", std::string());
		site.relayKind =
			ParseRelayKind(siteJson.value("relay_kind", std::string()));
		site.targetScope =
			ParseScopeKind(siteJson.value("target_scope", std::string()));
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
		manifest->sitesByOrdinal.emplace(site.ordinal, std::move(site));
	}

	auto functionsIt = root.find("functions");
	if (functionsIt != root.end() && functionsIt->is_array())
	{
		for (const Json &functionJson : *functionsIt)
		{
			ManifestFunctionSummary function;
			function.sourceFunctionId =
				functionJson.value("source_function_id", std::string());
			auto summary = functionJson.find("summary");
			if (summary != functionJson.end() && summary->is_object())
			{
				if (summary->contains("relay_count"))
					function.exactDirectRelayCount =
						ParseConstant((*summary)["relay_count"]);
				if (summary->contains("relay_count_upper_bound"))
					function.directRelayCountUpperBound =
						ParseConstant((*summary)["relay_count_upper_bound"]);
				if (summary->contains("max_depth"))
				{
					auto value = ParseConstant((*summary)["max_depth"]);
					if (value && *value <= std::numeric_limits<uint32_t>::max())
						function.maximumDepth =
							static_cast<uint32_t>(*value);
				}
				function.hasOpaque = summary->value("has_opaque", false);
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
				}
			}
			if (!function.sourceFunctionId.empty())
			{
				const std::string sourceFunctionId =
					function.sourceFunctionId;
				// Schema v5 names this field exported_opcode. Accept opcode as
				// a compatibility fallback for early development manifests.
				auto opcode = functionJson.find("exported_opcode");
				if (opcode == functionJson.end())
					opcode = functionJson.find("opcode");
				if (opcode != functionJson.end() &&
					(opcode->is_number_unsigned() ||
					 opcode->is_number_integer()))
				{
					std::optional<uint64_t> rawOpcode;
					if (opcode->is_number_unsigned())
					{
						rawOpcode = opcode->get<uint64_t>();
					}
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
						auto inserted =
							manifest->functionIdByOpcode.emplace(
							static_cast<uint32_t>(*rawOpcode),
							sourceFunctionId);
						if (!inserted.second &&
							inserted.first->second !=
								sourceFunctionId)
						{
							result.status =
								ManifestLoadStatus::
									ManifestParseError;
							result.diagnostic =
								"exported opcode is mapped to multiple "
								"source functions";
							return result;
						}
					}
				}
				auto inserted =
					manifest->functionsById.emplace(
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
	}

	auto refinementIt = root.find("refinement");
	if (refinementIt != root.end() && refinementIt->is_object())
	{
		auto obligations = refinementIt->find("proof_obligations");
		if (obligations != refinementIt->end() && obligations->is_array())
		{
			for (const Json &obligation : *obligations)
			{
				if (obligation.value("kind", std::string()) !=
					"TargetNonAliasCandidate")
				{
					continue;
				}
				ManifestNonAliasProof proof;
				proof.obligationId =
					obligation.value("id", std::string());
				proof.leftRelaySiteId =
					obligation.value("relay_site_id", std::string());
				proof.rightRelaySiteId =
					obligation.value(
						"related_relay_site_id",
						std::string());
				auto solver = obligation.find("solver_result");
				proof.solverProved =
					solver != obligation.end() &&
					solver->is_object() &&
					solver->value("status", std::string()) == "Proved";
				if (!proof.leftRelaySiteId.empty() &&
					!proof.rightRelaySiteId.empty())
				{
					manifest->nonAliasProofs.push_back(std::move(proof));
				}
			}
		}
	}

	result.status = ManifestLoadStatus::Loaded;
	result.manifest = std::move(manifest);
	result.diagnostic = "manifest loaded and artifact binding verified";
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

RelayManifestLoader::RelayManifestLoader(std::string relayProtocolRoot)
	: m_relayProtocolRoot(std::move(relayProtocolRoot))
{
}

std::shared_ptr<const RelayManifestLoadResult> RelayManifestLoader::Load(
	const ExpectedArtifactBinding &expected)
{
	const std::string key = CacheKey(expected);
	std::lock_guard<std::mutex> lock(m_mutex);
	auto cached = m_cache.find(key);
	if (cached != m_cache.end())
		return cached->second;

	std::shared_ptr<RelayManifestLoadResult> result = LoadUncached(expected);
	m_cache.emplace(key, result);
	return result;
}

std::shared_ptr<RelayManifestLoadResult>
RelayManifestLoader::LoadUncached(
	const ExpectedArtifactBinding &expected) const
{
	const auto begin = std::chrono::steady_clock::now();
	auto result = std::make_shared<RelayManifestLoadResult>();
	result->expectedBinding = expected.identity;

	std::vector<std::filesystem::path> candidates;
	if (!expected.manifestPath.empty())
	{
		candidates.emplace_back(expected.manifestPath);
	}
	else if (!expected.identity.moduleId.empty())
	{
		// The link/deploy binder publishes this identity-addressed file.
		// Looking it up directly avoids allowing an unrelated module's
		// malformed or stale sidecar to determine this module's diagnosis.
		candidates.emplace_back(
			std::filesystem::path(m_relayProtocolRoot) /
			"by_module" /
			(expected.identity.moduleId + ".relay_protocol.json"));
	}
	else
	{
		const std::filesystem::path directory =
			std::filesystem::path(m_relayProtocolRoot) / "by_module";
		std::error_code error;
		if (std::filesystem::is_directory(directory, error))
		{
			for (const auto &entry :
				std::filesystem::directory_iterator(directory, error))
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
		if (!candidates.empty())
			result->sourcePath = candidates.front().string();
		ParsedCandidate best;
		best.status = ManifestLoadStatus::ManifestNotFound;
		for (const std::filesystem::path &candidate : candidates)
		{
			std::error_code existsError;
			if (!std::filesystem::is_regular_file(candidate, existsError))
				continue;
			ParsedCandidate parsed = ParseCandidate(candidate, expected);
			if (parsed.status == ManifestLoadStatus::Loaded)
			{
				result->status = parsed.status;
				result->diagnostic = std::move(parsed.diagnostic);
				result->sourcePath = std::move(parsed.sourcePath);
				result->observedBinding =
					std::move(parsed.observedBinding);
				result->manifest = std::move(parsed.manifest);
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
			result->status = ManifestLoadStatus::ManifestNotFound;
			result->diagnostic =
				"no relay manifest candidate exists for the deployed module";
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
		std::chrono::duration<double, std::milli>(end - begin).count();
	return result;
}

void RelayManifestLoader::Invalidate(const std::string &moduleId)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	const std::string prefix = NormalizeIdentity(moduleId) + "|";
	for (auto it = m_cache.begin(); it != m_cache.end();)
	{
		if (it->first.compare(0, prefix.size(), prefix) == 0)
			it = m_cache.erase(it);
		else
			++it;
	}
}

void RelayManifestLoader::Clear()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_cache.clear();
}

std::string RelayManifestLoader::ComputeManifestSelfHash(
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
				*error = "artifact_binding is missing while computing self hash";
			return {};
		}
		binding->erase("manifest_hash");
		const std::string canonical = root.dump();
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
			*error =
				"manifest self hash failed: " +
				std::string(exception.what());
		return {};
	}
}

std::string RelayManifestLoader::RuntimeHashIdentity(
	const rvm::HashValue &hash)
{
	rt::String result;
	rvm::RvmTypeToString(hash, result);
	return std::string(result.GetString(), result.GetLength());
}

} // namespace relay_trace
} // namespace oxd
