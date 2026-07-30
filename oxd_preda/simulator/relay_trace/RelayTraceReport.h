#pragma once

#include "RelayTraceTypes.h"

#include <mutex>
#include <string>

namespace oxd {
namespace relay_trace {

struct RelayTraceSerializedReport
{
	std::string contents;
	double elapsedTimeMs = 0.0;
};

class RelayTraceReport
{
public:
	RelayTraceSerializedReport BuildJson(
		const RelayTraceSnapshot &snapshot,
		bool pretty = true) const;
	RelayTraceSerializedReport BuildJsonLines(
		const RelayTraceSnapshot &snapshot) const;

	bool WriteJson(
		const std::string &path,
		const RelayTraceSnapshot &snapshot,
		std::string *error = nullptr) const;
	bool WriteJsonLines(
		const std::string &path,
		const RelayTraceSnapshot &snapshot,
		std::string *error = nullptr) const;

private:
	bool WriteAtomically(
		const std::string &path,
		const std::string &contents,
		std::string *error) const;

	mutable std::mutex m_writeMutex;
};

} // namespace relay_trace
} // namespace oxd
