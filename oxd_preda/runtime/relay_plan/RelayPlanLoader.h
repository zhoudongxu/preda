#pragma once

#include "BoundRelayManifest.h"

#include "../../native/abi/vm_types.h"

#include <memory>
#include <string>

namespace oxd {
namespace relay_plan {

// Stateless parser/verifier. Cache ownership belongs to RelayPlanRegistry so
// parsing and hashing can occur outside the registry's global mutex.
class RelayPlanLoader
{
public:
	explicit RelayPlanLoader(std::string relayProtocolRoot);

	std::shared_ptr<RelayPlanLoadResult> Load(
		const ExpectedArtifactBinding &expected) const;

	// SHA-256 of compact ordered JSON after removing
	// artifact_binding.manifest_hash.
	static std::string ComputeManifestSelfHash(
		const std::string &manifestText,
		std::string *error = nullptr);
	static std::string RuntimeHashIdentity(const rvm::HashValue &hash);
	static std::string NormalizeIdentity(std::string value);

private:
	std::string m_relayProtocolRoot;
};

} // namespace relay_plan
} // namespace oxd
