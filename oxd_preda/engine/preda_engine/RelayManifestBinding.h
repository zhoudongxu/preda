#pragma once

#ifdef RPREDA_ENABLE_BOUND_RELAY_MANIFEST

#include <string>

#include "../../native/abi/vm_types.h"

namespace rpreda
{

// Completes the compiler-produced relay manifest once the PREDA module
// identity is available and publishes both the logical-name alias and the
// module-addressed copy. The returned hash is the SHA-256 of the ordered JSON
// with artifact_binding.manifest_hash removed.
bool FinalizeAndPublishRelayManifest(
	const char* unboundManifestJson,
	const std::string& dbPath,
	const std::string& dapp,
	const std::string& contract,
	const std::string& transpilerVersion,
	const rvm::HashValue& intermediateHash,
	const rvm::ContractModuleID& moduleId,
	rvm::HashValue& outManifestHash,
	std::string& outError);

} // namespace rpreda

#endif // RPREDA_ENABLE_BOUND_RELAY_MANIFEST
