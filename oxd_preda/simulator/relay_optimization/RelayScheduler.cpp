#include "RelayScheduler.h"

#include <algorithm>
#include <cctype>

namespace oxd {
namespace relay_optimization {
namespace {

std::string Lower(std::string value)
{
	std::transform(
		value.begin(),
		value.end(),
		value.begin(),
		[](unsigned char c)
		{
			return static_cast<char>(std::tolower(c));
		});
	return value;
}

bool Protected(const RelaySchedulerTask &task) noexcept
{
	return task.globalRelay || task.broadcastRelay || task.opaqueRelay ||
		task.unsupportedRelay;
}

bool SameSitePair(
	const std::string &left,
	const std::string &right,
	const relay_plan::ManifestRelayPairCertificate &pair) noexcept
{
	return (pair.siteA == left && pair.siteB == right) ||
		(pair.siteA == right && pair.siteB == left);
}

RelayPairDecision FromCertificate(
	const RelaySchedulerTask &left,
	const RelaySchedulerTask &right,
	const RelaySchedulerCertificateView &view) noexcept
{
	RelayPairDecision result;
	if(!view.function || !view.bindingTrusted ||
		!view.functionOpcodeTrusted || !view.optimizationEligible)
	{
		return result;
	}
	if(!view.function->sourceFunctionId.empty() &&
		(!left.functionId.empty() &&
			left.functionId != view.function->sourceFunctionId) )
	{
		return result;
	}
	if(!view.function->sourceFunctionId.empty() &&
		(!right.functionId.empty() &&
			right.functionId != view.function->sourceFunctionId) )
	{
		return result;
	}

	for(const auto &pair : view.function->pairRelations)
	{
		if(!SameSitePair(left.relaySiteId, right.relaySiteId, pair))
			continue;
		if(pair.status != relay_plan::ParallelCertificateStatus::Proved)
			return result;

		result.certificateUsed = true;
		const bool sameOrientation =
			pair.siteA == left.relaySiteId &&
			pair.siteB == right.relaySiteId;
		switch(pair.relation)
		{
		case relay_plan::RelayPairCertificateRelation::CoEmissionIndependent:
			result.kind = RelayPairDecisionKind::Independent;
			return result;
		case relay_plan::RelayPairCertificateRelation::MutuallyExclusive:
			result.kind = RelayPairDecisionKind::MutuallyExclusive;
			return result;
		case relay_plan::RelayPairCertificateRelation::MustPrecedeAB:
			result.kind = RelayPairDecisionKind::MustPrecede;
			result.leftMustPrecede = sameOrientation;
			result.rightMustPrecede = !sameOrientation;
			return result;
		case relay_plan::RelayPairCertificateRelation::MustPrecedeBA:
			result.kind = RelayPairDecisionKind::MustPrecede;
			result.leftMustPrecede = !sameOrientation;
			result.rightMustPrecede = sameOrientation;
			return result;
		case relay_plan::RelayPairCertificateRelation::ProvedMayAlias:
		case relay_plan::RelayPairCertificateRelation::PotentialConflict:
			result.kind = RelayPairDecisionKind::Conflict;
			return result;
		case relay_plan::RelayPairCertificateRelation::Unknown:
			return result;
		}
	}
	return result;
}

} // namespace

const char *ToString(RelaySchedulerMode value) noexcept
{
	switch(value)
	{
	case RelaySchedulerMode::FIFO:
		return "fifo";
	case RelaySchedulerMode::CertificateBlindPriority:
		return "certificate_blind_priority";
	case RelaySchedulerMode::CertificateGuidedPriority:
		return "certificate_guided_priority";
	}
	return "fifo";
}

bool ParseRelaySchedulerMode(
	const std::string &text,
	RelaySchedulerMode &out,
	std::string &error)
{
	const std::string value = Lower(text);
	if(value == "fifo" || value == "baseline")
		out = RelaySchedulerMode::FIFO;
	else if(value == "certificate_blind_priority" ||
		value == "blind_priority")
	{
		out = RelaySchedulerMode::CertificateBlindPriority;
	}
	else if(value == "certificate_guided_priority" ||
		value == "certificate_guided")
	{
		out = RelaySchedulerMode::CertificateGuidedPriority;
	}
	else
	{
		error =
			"expected fifo, certificate_blind_priority, or "
			"certificate_guided_priority";
		return false;
	}
	return true;
}

const char *ToString(RelayPairDecisionKind value) noexcept
{
	switch(value)
	{
	case RelayPairDecisionKind::Independent:
		return "independent";
	case RelayPairDecisionKind::MustPrecede:
		return "must_precede";
	case RelayPairDecisionKind::MutuallyExclusive:
		return "mutually_exclusive";
	case RelayPairDecisionKind::Conflict:
		return "conflict";
	case RelayPairDecisionKind::Unknown:
		return "unknown";
	}
	return "unknown";
}

const char *ToString(RelaySchedulerDecisionReason value) noexcept
{
	switch(value)
	{
	case RelaySchedulerDecisionReason::FIFO:
		return "fifo";
	case RelaySchedulerDecisionReason::Priority:
		return "priority";
	case RelaySchedulerDecisionReason::EmptyReadySet:
		return "empty_ready_set";
	case RelaySchedulerDecisionReason::NoSafeCandidate:
		return "no_safe_candidate";
	case RelaySchedulerDecisionReason::UntrustedCertificate:
		return "untrusted_certificate";
	case RelaySchedulerDecisionReason::MissingCertificate:
		return "missing_certificate";
	case RelaySchedulerDecisionReason::RuntimeTargetUnknown:
		return "runtime_target_unknown";
	case RelaySchedulerDecisionReason::RuntimeTargetAlias:
		return "runtime_target_alias";
	case RelaySchedulerDecisionReason::ProtectedRelayKind:
		return "protected_relay_kind";
	case RelaySchedulerDecisionReason::PairNotProved:
		return "pair_not_proved";
	case RelaySchedulerDecisionReason::DependencyBlocked:
		return "dependency_blocked";
	}
	return "no_safe_candidate";
}

RelayPairDecision ClassifyRelayPair(
	const RelaySchedulerTask &left,
	const RelaySchedulerTask &right,
	const RelaySchedulerCertificateView &view) noexcept
{
	if(Protected(left) || Protected(right))
	{
		return RelayPairDecision{
			RelayPairDecisionKind::Conflict,
			false,
			false,
			false};
	}

	const RelayPairDecision certificate =
		FromCertificate(left, right, view);
	// A proved order relation is a dependency even when both tasks happen to
	// carry the same runtime target key.  It must be retained for auditability.
	if(certificate.kind == RelayPairDecisionKind::MustPrecede)
		return certificate;

	if(!left.targetKnown || !right.targetKnown)
		return certificate.certificateUsed
			? RelayPairDecision{
				RelayPairDecisionKind::Unknown,
				false,
				false,
				certificate.certificateUsed}
			: RelayPairDecision{};
	if(left.targetScopeKey == right.targetScopeKey)
	{
		return RelayPairDecision{
			RelayPairDecisionKind::Conflict,
			false,
			false,
			certificate.certificateUsed};
	}
	return certificate;
}

bool RelayScheduler::HigherPriority(
	const RelaySchedulerTask &left,
	const RelaySchedulerTask &right) noexcept
{
	if(left.remainingDepth != right.remainingDepth)
		return left.remainingDepth > right.remainingDepth;
	if(left.estimatedWork != right.estimatedWork)
		return left.estimatedWork > right.estimatedWork;
	if(left.originateHeight != right.originateHeight)
		return left.originateHeight < right.originateHeight;
	if(left.sourceOrder != right.sourceOrder)
		return left.sourceOrder < right.sourceOrder;
	if(left.stableId != right.stableId)
		return left.stableId < right.stableId;
	return left.relaySiteId < right.relaySiteId;
}

bool RelayScheduler::CanMoveBefore(
	const RelaySchedulerTask &candidate,
	const RelaySchedulerTask &blocked,
	const RelaySchedulerCertificateView &view,
	bool &certificateUsed,
	RelaySchedulerDecisionReason &reason) const noexcept
{
	// Selection is local to one destination queue.  Cross-shard admission and
	// route classification stay in the Native Engine's existing path.
	if(candidate.destinationShard != blocked.destinationShard)
	{
		reason = RelaySchedulerDecisionReason::DependencyBlocked;
		return false;
	}
	// Preserve PREDA's block admission order. A priority policy may reorder
	// only tasks from the same originate height and may not move a non-global
	// origin ahead of a global-origin relay at that height.
	if(candidate.originateHeight != blocked.originateHeight)
	{
		reason = RelaySchedulerDecisionReason::DependencyBlocked;
		return false;
	}
	if(blocked.originShard == rvm::GlobalShard &&
		candidate.originShard != rvm::GlobalShard)
	{
		reason = RelaySchedulerDecisionReason::DependencyBlocked;
		return false;
	}
	if(Protected(candidate) || Protected(blocked))
	{
		reason = RelaySchedulerDecisionReason::ProtectedRelayKind;
		return false;
	}
	if(!candidate.targetKnown || !blocked.targetKnown)
	{
		reason = RelaySchedulerDecisionReason::RuntimeTargetUnknown;
		return false;
	}

	if(m_mode == RelaySchedulerMode::CertificateBlindPriority)
	{
		if(candidate.targetScopeKey == blocked.targetScopeKey)
		{
			reason = RelaySchedulerDecisionReason::RuntimeTargetAlias;
			return false;
		}
		return true;
	}

	if(!view.function)
	{
		reason = RelaySchedulerDecisionReason::MissingCertificate;
		return false;
	}
	if(!view.bindingTrusted || !view.functionOpcodeTrusted ||
		!view.optimizationEligible)
	{
		reason = RelaySchedulerDecisionReason::UntrustedCertificate;
		return false;
	}
	const RelayPairDecision pair =
		ClassifyRelayPair(blocked, candidate, view);
	certificateUsed = certificateUsed || pair.certificateUsed;
	switch(pair.kind)
	{
	case RelayPairDecisionKind::Independent:
		return true;
	case RelayPairDecisionKind::MustPrecede:
		if(pair.rightMustPrecede)
			return true;
		reason = RelaySchedulerDecisionReason::DependencyBlocked;
		return false;
	case RelayPairDecisionKind::Conflict:
		reason = RelaySchedulerDecisionReason::RuntimeTargetAlias;
		return false;
	case RelayPairDecisionKind::MutuallyExclusive:
	case RelayPairDecisionKind::Unknown:
		reason = RelaySchedulerDecisionReason::PairNotProved;
		return false;
	}
	return false;
}

RelaySchedulerDecision RelayScheduler::Choose(
	const std::vector<RelaySchedulerTask> &ready,
	const RelaySchedulerCertificateView &view) const noexcept
{
	RelaySchedulerDecision result;
	if(ready.empty())
		return result;
	result.selectedIndex = 0;
	result.reason =
		m_mode == RelaySchedulerMode::FIFO
			? RelaySchedulerDecisionReason::FIFO
			: RelaySchedulerDecisionReason::NoSafeCandidate;
	if(m_mode == RelaySchedulerMode::FIFO || ready.size() == 1)
		return result;

	const bool trustedCertificate =
		view.function && view.bindingTrusted &&
		view.functionOpcodeTrusted && view.optimizationEligible;
	RelaySchedulerDecisionReason lastReason = result.reason;
	size_t best = 0;
	for(size_t i = 1; i < ready.size(); ++i)
	{
		bool candidateCertificateUsed = false;
		bool canBypass = true;
		for(size_t prefix = 0; prefix < i; ++prefix)
		{
			RelaySchedulerDecisionReason reason =
				RelaySchedulerDecisionReason::NoSafeCandidate;
			if(!CanMoveBefore(
				ready[i],
				ready[prefix],
				view,
				candidateCertificateUsed,
				reason))
			{
				canBypass = false;
				lastReason = reason;
				break;
			}
		}
		if(canBypass && HigherPriority(ready[i], ready[best]))
		{
			best = i;
			result.certificateUsed =
				result.certificateUsed || candidateCertificateUsed;
		}
	}
	result.selectedIndex = best;
	result.reordered = best != 0;
	if(result.reordered)
	{
		result.reason = RelaySchedulerDecisionReason::Priority;
		return result;
	}
	if(m_mode == RelaySchedulerMode::CertificateGuidedPriority &&
		!trustedCertificate)
	{
		result.reason = view.function
			? RelaySchedulerDecisionReason::UntrustedCertificate
			: RelaySchedulerDecisionReason::MissingCertificate;
	}
	else
	{
		result.reason = lastReason;
	}
	return result;
}

} // namespace relay_optimization
} // namespace oxd
