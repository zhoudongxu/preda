#pragma once

#include "MutationKind.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rpreda {
namespace mutation {

struct SourceLocation
{
	uint32_t line = 0;
	uint32_t column = 0;
	uint32_t endLine = 0;
	uint32_t endColumn = 0;
	// Compiler/ANTLR offsets are Unicode code-point offsets and remain the
	// externally reported source coordinates.
	int64_t startOffset = -1;
	int64_t endOffset = -1;
	// Edits are applied to UTF-8 bytes through an explicit source map.
	int64_t byteStartOffset = -1;
	int64_t byteEndOffset = -1;

	bool IsValidFor(const std::string &source) const noexcept
	{
		return byteStartOffset >= 0 && byteEndOffset >= byteStartOffset &&
			static_cast<uint64_t>(byteEndOffset) < source.size();
	}
};

struct SourceEdit
{
	int64_t startOffset = -1;
	int64_t endOffset = -1; // Inclusive. Insertions use endOffset < startOffset.
	std::string replacement;
	std::string expected;
};

enum class MutationGenerationStatus : uint8_t
{
	Generated,
	Unsupported,
};

struct MutationRecord
{
	std::string mutationId;
	MutationKind mutationType = MutationKind::RelayTargetReplace;
	MutationGenerationStatus status = MutationGenerationStatus::Unsupported;
	std::string contract;
	std::string sourceFunctionId;
	std::vector<std::string> relaySiteIds;
	SourceLocation location;
	std::string description;
	std::string reason;
	std::string originalFragment;
	std::string mutatedFragment;
	std::string originalCode;
	std::string mutatedCode;
	std::vector<SourceEdit> edits;
};

struct MutationOptions
{
	uint64_t seed = 88;
	// Zero means no per-kind limit. The default controls combinatorial pairs
	// while still exercising every site-oriented operator.
	size_t maxMutantsPerKind = 32;
	// Inapplicable operators are not mutants and are excluded by default.
	// Enabling this writes explicit generation diagnostics for corpus audits.
	bool includeUnsupported = false;
	std::vector<MutationKind> enabledKinds;
};

struct MutationGenerationResult
{
	uint32_t schemaVersion = 1;
	uint64_t seed = 88;
	std::string contract;
	std::string sourceDigest;
	std::string manifestDigest;
	std::vector<MutationRecord> mutations;
	std::vector<std::string> diagnostics;
};

class MutationEngine
{
public:
	MutationGenerationResult Generate(
		const std::string &sourceCode,
		const std::string &relayManifestJson,
		const MutationOptions &options = MutationOptions()) const;

	static bool ApplyEdits(
		const std::string &sourceCode,
		const std::vector<SourceEdit> &edits,
		std::string &mutatedCode,
		std::string &error);
};

const char *ToString(MutationGenerationStatus status);

} // namespace mutation
} // namespace rpreda
