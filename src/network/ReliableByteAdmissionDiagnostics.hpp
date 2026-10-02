#pragma once

#include "gargantuan/network/Connection.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace gargantuan::network::detail {
	// Qualification-only identity of an already encoded candidate. No payload is
	// retained and this never participates in admission or transport decisions.
	[[nodiscard]] inline std::array<std::uint64_t, 2> ExactCandidateFingerprint(
		std::span<const std::byte> Bytes) noexcept {
		std::uint64_t First = 0xcbf29ce484222325ULL;
		std::uint64_t Second = 0x84222325cbf29ce4ULL;
		for (const auto Byte : Bytes) {
			const auto Value = static_cast<std::uint8_t>(Byte);
			First = (First ^ Value) * 0x100000001b3ULL;
			Second = (Second ^ (Value + 1U)) * 0x100000001b3ULL;
		}
		return {First, Second};
	}
	// Read-only qualification evidence. This sink has no authority over credit,
	// ordering, grant ownership, or transport service.
	enum class AdmissionEvidenceKind : std::uint8_t {
		ExactDemand,
		CreditEligible,
		EligibilityInterrupted,
		GrantAccepted,
		ReservationRolledBack,
		DemandDisposed,
	};
	enum class AdmissionEvidenceReason : std::uint8_t {
		None,
		NoWork,
		Unexamined,
		Replaced,
		Rollback,
		GenerationRemoved,
		TerminalRelease,
		FeedbackUnavailable,
	};
	struct AdmissionEvidenceEvent {
		AdmissionEvidenceKind Kind = AdmissionEvidenceKind::ExactDemand;
		AdmissionEvidenceReason Reason = AdmissionEvidenceReason::None;
		ConnectionId Connection;
		std::uint64_t DemandId = 0, EligibilityEpisode = 0, GrantToken = 0;
		std::uint64_t ExactBytes = 0, AtMicroseconds = 0, CreditThresholdAtMicroseconds = 0;
		std::uint64_t EligibleSinceMicroseconds = 0, PeerCreditBytes = 0, GlobalCreditBytes = 0;
		std::uint64_t ActiveGrants = 0, GrantDeferrals = 0, FundedDeferrals = 0;
		std::uint64_t CreditDeferrals = 0, FairnessDeferrals = 0;
		std::array<std::uint64_t, 2> ExactCandidateFingerprint{};
	};
	struct AdmissionEvidenceSink {
		void *Context = nullptr;
		void (*Record)(void *, const AdmissionEvidenceEvent &) noexcept = nullptr;
	};
	inline thread_local AdmissionEvidenceSink *ActiveAdmissionEvidence = nullptr;
}
