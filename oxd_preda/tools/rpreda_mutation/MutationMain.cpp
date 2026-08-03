#include "MutationEngine.h"

#include "nlohmann/json.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace {

using Json = nlohmann::ordered_json;
using rpreda::mutation::AllKnownMutationKinds;
using rpreda::mutation::AllMutationKinds;
using rpreda::mutation::MutationGenerationStatus;
using rpreda::mutation::MutationKind;
using rpreda::mutation::MutationOptions;
using rpreda::mutation::MutationRecord;
using rpreda::mutation::SourceLocation;

struct Arguments
{
	std::filesystem::path source;
	std::filesystem::path manifest;
	std::filesystem::path output;
	MutationOptions options;
	bool listKinds = false;
};

std::string ReadFile(const std::filesystem::path &path)
{
	std::ifstream input(path, std::ios::binary);
	if (!input)
		throw std::runtime_error("cannot open " + path.string());
	std::ostringstream contents;
	contents << input.rdbuf();
	if (!input.good() && !input.eof())
		throw std::runtime_error("cannot read " + path.string());
	return contents.str();
}

void WriteFile(const std::filesystem::path &path, const std::string &contents)
{
	std::filesystem::create_directories(path.parent_path());
	const std::filesystem::path temporary = path.string() + ".tmp";
	{
		std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
		if (!output)
			throw std::runtime_error("cannot create " + temporary.string());
		output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
		if (!output)
			throw std::runtime_error("cannot write " + temporary.string());
	}
	std::error_code ignored;
	std::filesystem::remove(path, ignored);
	std::filesystem::rename(temporary, path);
}

Json LocationJson(const SourceLocation &location)
{
	return Json{
		{"line", location.line},
		{"column", location.column},
		{"end_line", location.endLine},
		{"end_column", location.endColumn},
		{"start_offset", location.startOffset},
		{"end_offset", location.endOffset},
		{"byte_start_offset", location.byteStartOffset},
		{"byte_end_offset", location.byteEndOffset},
	};
}

std::string ValueAfter(
	int &index,
	int argc,
	char **argv,
	const std::string &option)
{
	if (index + 1 >= argc)
		throw std::runtime_error(option + " requires a value");
	return argv[++index];
}

std::vector<MutationKind> ParseKinds(const std::string &value)
{
	std::vector<MutationKind> result;
	std::istringstream input(value);
	std::string item;
	while (std::getline(input, item, ','))
	{
		const auto kind = rpreda::mutation::ParseMutationKind(item);
		if (!kind.has_value())
			throw std::runtime_error("unknown mutation kind '" + item + "'");
		result.push_back(*kind);
	}
	return result;
}

Arguments ParseArguments(int argc, char **argv)
{
	Arguments result;
	for (int index = 1; index < argc; ++index)
	{
		const std::string option = argv[index];
		if (option == "--source")
			result.source = ValueAfter(index, argc, argv, option);
		else if (option == "--manifest")
			result.manifest = ValueAfter(index, argc, argv, option);
		else if (option == "--output")
			result.output = ValueAfter(index, argc, argv, option);
		else if (option == "--seed")
			result.options.seed = std::stoull(ValueAfter(index, argc, argv, option));
		else if (option == "--max-per-kind")
			result.options.maxMutantsPerKind = static_cast<size_t>(
				std::stoull(ValueAfter(index, argc, argv, option)));
		else if (option == "--kinds")
			result.options.enabledKinds =
				ParseKinds(ValueAfter(index, argc, argv, option));
		else if (option == "--include-inapplicable")
			result.options.includeUnsupported = true;
		else if (option == "--list-kinds")
			result.listKinds = true;
		else if (option == "--help" || option == "-h")
		{
			std::cout
				<< "Usage: rpreda_mutation --source contract.prd "
				   "--manifest contract.relay_protocol.json --output DIR "
				   "[--seed 88] [--max-per-kind 32] "
				   "[--kinds KindA,KindB] [--include-inapplicable]\n";
			std::exit(0);
		}
		else
			throw std::runtime_error("unknown option '" + option + "'");
	}
	if (result.listKinds)
		return result;
	if (result.source.empty() || result.manifest.empty() || result.output.empty())
		throw std::runtime_error("--source, --manifest, and --output are required");
	return result;
}

Json RecordJson(
	const MutationRecord &record,
	const std::filesystem::path &originalPath,
	const std::filesystem::path &mutatedPath)
{
	Json edits = Json::array();
	for (const auto &edit : record.edits)
	{
		edits.push_back(Json{
			{"byte_start_offset", edit.startOffset},
			{"byte_end_offset", edit.endOffset},
			{"expected_original", edit.expected},
			{"replacement", edit.replacement},
		});
	}
	return Json{
		{"mutation_id", record.mutationId},
		{"mutation_type", rpreda::mutation::ToString(record.mutationType)},
		{"generation_status", rpreda::mutation::ToString(record.status)},
		{"contract", record.contract},
		{"source_function_id", record.sourceFunctionId},
		{"relay_site_ids", record.relaySiteIds},
		{"location", LocationJson(record.location)},
		{"description", record.description},
		{"reason", record.reason},
		{"original_fragment", record.originalFragment},
		{"mutated_fragment", record.mutatedFragment},
		{"original_code_path", originalPath.generic_string()},
		{"mutated_code_path", mutatedPath.empty()
			? std::string()
			: mutatedPath.generic_string()},
		{"edits", std::move(edits)},
	};
}

} // namespace

int main(int argc, char **argv)
{
	try
	{
		const Arguments arguments = ParseArguments(argc, argv);
		if (arguments.listKinds)
		{
			for (MutationKind kind : AllKnownMutationKinds)
				std::cout << rpreda::mutation::ToString(kind) << '\n';
			return 0;
		}

		const std::string source = ReadFile(arguments.source);
		const std::string manifest = ReadFile(arguments.manifest);
		const auto result = rpreda::mutation::MutationEngine().Generate(
			source, manifest, arguments.options);

		std::filesystem::create_directories(arguments.output / "mutants");
		Json records = Json::array();
		std::map<std::string, uint64_t> counts;
		if (arguments.options.enabledKinds.empty())
		{
			// Preserve the original 13-key default report. Semantic operators
			// are opt-in and appear only when --kinds explicitly selects them.
			for (MutationKind kind : AllMutationKinds)
				counts[rpreda::mutation::ToString(kind)] = 0;
		}
		else
		{
			for (MutationKind kind : arguments.options.enabledKinds)
				counts[rpreda::mutation::ToString(kind)] = 0;
		}
		for (const MutationRecord &record : result.mutations)
		{
			const std::filesystem::path directory =
				arguments.output / "mutants" / record.mutationId;
			const std::filesystem::path originalPath = directory / "original.prd";
			std::filesystem::path mutatedPath;
			WriteFile(originalPath, record.originalCode);
			if (record.status == MutationGenerationStatus::Generated)
			{
				mutatedPath = directory / arguments.source.filename();
				WriteFile(mutatedPath, record.mutatedCode);
				++counts[rpreda::mutation::ToString(record.mutationType)];
			}
			const Json metadata = RecordJson(record, originalPath, mutatedPath);
			WriteFile(directory / "mutation.json", metadata.dump(2) + "\n");
			records.push_back(metadata);
		}

		Json root{
			{"schema_version", result.schemaVersion},
			{"seed", result.seed},
			{"contract", result.contract},
			{"source_path", std::filesystem::absolute(arguments.source).generic_string()},
			{"manifest_path", std::filesystem::absolute(arguments.manifest).generic_string()},
			{"source_digest", result.sourceDigest},
			{"manifest_digest", result.manifestDigest},
			{"operator_counts", counts},
			{"diagnostics", result.diagnostics},
			{"mutations", std::move(records)},
		};
		WriteFile(arguments.output / "mutation_index.json", root.dump(2) + "\n");
		std::cout << "Generated " << result.mutations.size()
			<< " mutation records in " << arguments.output << '\n';
		if (result.contract.empty() && !result.diagnostics.empty())
			return 3;
		return 0;
	}
	catch (const std::exception &exception)
	{
		std::cerr << "rpreda_mutation: " << exception.what() << '\n';
		return 2;
	}
}
