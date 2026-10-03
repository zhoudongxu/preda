#pragma once

#include "../../runtime/relay_plan/BoundRelayManifest.h"
#include "../../native/abi/vm_interfaces.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace oxd {
namespace relay_optimization {

// These modes intentionally describe admission/order policies only.  They do
// not change target evaluation, relay construction, or worker ownership.
enum class RelaySchedulerMode : uint8_t
{
	FIFO,
	CertificateBlindPriority,
	CertificateGuidedPriority,
};

const char *ToString(RelaySchedulerMode value) noexcept;

bool ParseRelaySchedulerMode(
	const std::string &text,
	RelaySchedulerMode &out,
	std::string &error);

struct RelaySchedulerTask
{
	std::string moduleId;
	std::string functionId;
	std::string relaySiteId;
	std::string parentId;
	std::string stableId;
	std::string targetScopeKey;
	uint32_t sourceOpcode = 0;
	uint32_t relaySiteOrdinal = 0;

	uint64_t originateHeight = 0;
	uint64_t sourceOrder = 0;
	uint64_t estimatedWork = 0;
	uint32_t remainingDepth = 0;
	uint32_t originShard = 0;
	uint32_t destinationShard = 0;

	bool targetKnown = false;
	bool globalRelay = false;
	bool broadcastRelay = false;
	bool opaqueRelay = false;
	bool unsupportedRelay = false;
};

// The view is deliberately explicit.  A scheduler must not infer trust from
// the presence of a certificate pointer alone.
struct RelaySchedulerCertificateView
{
	const relay_plan::ManifestFunctionParallelCertificate *function = nullptr;
	bool bindingTrusted = false;
	bool functionOpcodeTrusted = false;
	bool optimizationEligible = false;
};

enum class RelayPairDecisionKind : uint8_t
{
	Independent,
	MustPrecede,
	MutuallyExclusive,
	Conflict,
	Unknown,
};

struct RelayPairDecision
{
	RelayPairDecisionKind kind = RelayPairDecisionKind::Unknown;
	// The flags are relative to the arguments passed to ClassifyRelayPair.
	bool leftMustPrecede = false;
	bool rightMustPrecede = false;
	bool certificateUsed = false;
};

enum class RelaySchedulerDecisionReason : uint8_t
{
	FIFO,
	Priority,
	EmptyReadySet,
	NoSafeCandidate,
	UntrustedCertificate,
	MissingCertificate,
	RuntimeTargetUnknown,
	RuntimeTargetAlias,
	ProtectedRelayKind,
	PairNotProved,
	DependencyBlocked,
};

struct RelaySchedulerDecision
{
	size_t selectedIndex = 0;
	bool reordered = false;
	bool certificateUsed = false;
	RelaySchedulerDecisionReason reason =
		RelaySchedulerDecisionReason::EmptyReadySet;
};

// Classify one pair using only trusted, proved certificate facts.  The order
// of the arguments is significant for MustPrecedeAB/BA.  This function is
// public so the audit and experiment harness can report why a candidate was
// admitted or kept on the FIFO path without duplicating scheduler logic.
RelayPairDecision ClassifyRelayPair(
	const RelaySchedulerTask &left,
	const RelaySchedulerTask &right,
	const RelaySchedulerCertificateView &view) noexcept;

const char *ToString(RelayPairDecisionKind value) noexcept;
const char *ToString(RelaySchedulerDecisionReason value) noexcept;

class RelayScheduler
{
public:
	explicit RelayScheduler(
		RelaySchedulerMode mode = RelaySchedulerMode::FIFO) noexcept
		: m_mode(mode)
	{
	}

	RelaySchedulerMode Mode() const noexcept
	{
		return m_mode;
	}

	// Returns the index into ready.  FIFO always returns zero.  Priority modes
	// may move a task only across a prefix that is safe for that mode.
	RelaySchedulerDecision Choose(
		const std::vector<RelaySchedulerTask> &ready,
		const RelaySchedulerCertificateView &view) const noexcept;

	// Stable priority order used by both priority modes.  A true result means
	// left should be considered before right when they are otherwise eligible.
	static bool HigherPriority(
		const RelaySchedulerTask &left,
		const RelaySchedulerTask &right) noexcept;

private:
	bool CanMoveBefore(
		const RelaySchedulerTask &candidate,
		const RelaySchedulerTask &blocked,
		const RelaySchedulerCertificateView &view,
		bool &certificateUsed,
		RelaySchedulerDecisionReason &reason) const noexcept;

	RelaySchedulerMode m_mode;
};

} // namespace relay_optimization
} // namespace oxd
