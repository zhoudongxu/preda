#include "RelayManifestBinding.h"

#ifdef RPREDA_ENABLE_BOUND_RELAY_MANIFEST

#include <exception>
#include <limits>

#include "../../../SFC/core/rt/json.h"
#include "../../3rdParty/nlohmann/json.hpp"
#include "../../native/types/typetraits.h"

namespace rpreda
{
namespace
{

using Json = nlohmann::ordered_json;

std::string HashToString(const rvm::HashValue& hash)
{
	rt::String result;
	rvm::RvmTypeToString(hash, result);
	return std::string(result.GetString(), result.GetLength());
}

std::string HashToHex(const rvm::HashValue& hash)
{
	static constexpr char Digits[] = "0123456789abcdef";
	const auto *bytes =
		reinterpret_cast<const uint8_t *>(&hash);
	std::string result(sizeof(hash) * 2, '\0');
	for (size_t index = 0; index < sizeof(hash); ++index)
	{
		result[index * 2] = Digits[bytes[index] >> 4];
		result[index * 2 + 1] =
			Digits[bytes[index] & 0x0f];
	}
	return result;
}

bool WriteAtomically(
	const std::string& path,
	const std::string& contents,
	std::string& outError)
{
	const std::string temporaryPath = path + ".tmp";
	os::File::CreateDirectories(path.c_str(), true);
	os::File::Remove(temporaryPath.c_str());

	os::File file;
	if (!file.Open(
			temporaryPath.c_str(),
			os::File::Normal_Write,
			true))
	{
		outError = "Cannot open temporary relay manifest " +
			temporaryPath;
		return false;
	}

	file.Write(contents.c_str());
	if (file.ErrorOccured())
	{
		file.Close();
		os::File::Remove(temporaryPath.c_str());
		outError = "Cannot write temporary relay manifest " +
			temporaryPath;
		return false;
	}
	file.Close();

	// The temporary file is a sibling of the destination. On POSIX the first
	// branch inside MoveFile is a same-filesystem rename. The existing file
	// helper also supplies the repository's platform fallback behavior.
	if (!os::File::MoveFile(
		temporaryPath.c_str(),
		path.c_str(),
		true))
	{
		os::File::Remove(temporaryPath.c_str());
		outError = "Cannot publish relay manifest " + path;
		return false;
	}

	return true;
}

} // namespace

bool FinalizeAndPublishRelayManifest(
	const char* unboundManifestJson,
	const std::string& dbPath,
	const std::string& dapp,
	const std::string& contract,
	const std::string& transpilerVersion,
	const rvm::HashValue& intermediateHash,
	const rvm::ContractModuleID& moduleId,
	rvm::HashValue& outManifestHash,
	std::string& outError)
{
	outManifestHash = {};
	outError.clear();

	if (unboundManifestJson == nullptr)
	{
		outError = "Relay protocol manifest is unavailable";
		return false;
	}
	if (transpilerVersion.empty())
	{
		outError =
			"Compilation-time transpiler version is unavailable";
		return false;
	}

	Json manifest;
	try
	{
		manifest = Json::parse(unboundManifestJson);
	}
	catch (const std::exception& exception)
	{
		outError = "Cannot parse relay protocol manifest: ";
		outError += exception.what();
		return false;
	}

	if (!manifest.is_object())
	{
		outError = "Relay protocol manifest root is not an object";
		return false;
	}

	const std::string intermediateHashString =
		HashToString(intermediateHash);
	const std::string moduleIdString = HashToString(moduleId);

	Json binding = {
		{"dapp", dapp},
		{"contract", contract},
		{"transpiler_version", transpilerVersion},
		{"intermediate_hash", intermediateHashString},
		{"module_id", moduleIdString},
		{"module_hash_kind", "preda_module_id"},
		{"module_hash", moduleIdString},
		{"manifest_hash_algorithm", "sha256"},
		{"binding_complete", true},
	};
	manifest["artifact_binding"] = std::move(binding);

	// Hash a single, deterministic representation. The manifest_hash member
	// is deliberately absent here, preventing a self-referential digest.
	const std::string canonicalManifest = manifest.dump();
	if (canonicalManifest.size() >
		std::numeric_limits<uint32_t>::max())
	{
		outError = "Relay protocol manifest is too large to hash";
		return false;
	}
	oxd::SecuritySuite::Hash(
		canonicalManifest.data(),
		static_cast<uint32_t>(canonicalManifest.size()),
		&outManifestHash);
	manifest["artifact_binding"]["manifest_hash"] =
		HashToHex(outManifestHash);

	const std::string boundManifest = manifest.dump(2);
	const std::string relayProtocolDirectory =
		dbPath + "relay_protocol/";
	const std::string moduleManifestPath =
		relayProtocolDirectory + "by_module/" + moduleIdString +
		".relay_protocol.json";
	const std::string logicalManifestPath =
		relayProtocolDirectory + dapp + "." + contract +
		".relay_protocol.json";

	if (!WriteAtomically(
		logicalManifestPath,
		boundManifest,
		outError))
	{
		return false;
	}
	// Publish the identity-addressed copy last. If updating the human-facing
	// alias fails, the runtime-trusted copy remains untouched. Runtime lookup
	// never needs to trust the logical alias.
	if (!WriteAtomically(
		moduleManifestPath,
		boundManifest,
		outError))
	{
		return false;
	}

	return true;
}

} // namespace rpreda

#endif // RPREDA_ENABLE_BOUND_RELAY_MANIFEST
