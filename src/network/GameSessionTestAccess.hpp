#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <span>
#include "gargantuan/network/CharacterNetwork.hpp"
#include "gargantuan/network/ReplicationCoordinator.hpp"
#include "gargantuan/network/RemoteManager.hpp"
#include "gargantuan/runtime/SpatialRegionIndex.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"

namespace gargantuan::network {
	class GameSession;
	struct ReplicationMetrics;
}

namespace gargantuan::network::detail {
	enum class StructuralCausalKind : std::uint8_t {
		Prepared, Accepted, Rejected, NoFrame, PendingAdded, PendingCancelled, PendingReplaced, PeerRemoved, Delivery, Retired,
	};
	enum class StructuralCausalReason : std::uint8_t {
		None, FilteredOrAlreadyCovered, NoLongerRequired, ObjectRetired, Replanned, GenerationRemoved,
	};
	struct StructuralPendingIdentity {
		std::uint64_t Token = 0;
		ObjectId Object;
		bool Enter = false;
	};
	struct StructuralCausalEvent {
		StructuralCausalKind Kind = StructuralCausalKind::NoFrame;
		StructuralCausalReason Reason = StructuralCausalReason::None;
		ConnectionId Connection;
		ObjectId SourceScope;
		std::uint64_t Sequence = 0, GrantToken = 0, CompleteBytes = 0;
		std::uint64_t FirstSent = 0, Acked = 0, AcceptedBefore = 0;
		bool Valid = true;
		std::uint64_t CursorBefore = 0, CursorAfter = 0, AcceptedRevision = 0;
		std::array<std::uint64_t, 2> Fingerprint{};
		StructuralPendingIdentity Pending;
		std::uint64_t ReplacementToken = 0;
		// Borrowed only during the synchronous callback; no payload or Instance
		// ownership crosses the Main-only evidence boundary.
		std::span<const StructuralPendingIdentity> ResolvedPending;
		std::span<const ObjectId> Entering, Leaving;
	};
	struct StructuralCausalEvidenceSink {
		void *Context = nullptr;
		void (*Record)(void *, const StructuralCausalEvent &) noexcept = nullptr;
	};
	inline thread_local StructuralCausalEvidenceSink *ActiveStructuralCausalEvidence = nullptr;
	inline void RecordStructuralCausal(const StructuralCausalEvent &Event) noexcept {
		if (ActiveStructuralCausalEvidence && ActiveStructuralCausalEvidence->Record)
			ActiveStructuralCausalEvidence->Record(ActiveStructuralCausalEvidence->Context, Event);
	}
	struct StructuralCausalSnapshot {
		ConnectionId Connection;
		ObjectId SourceScope;
		std::uint64_t JournalCursor = 0, JournalTail = 0, NextSequence = 0, AcceptedRevision = 0;
		std::uint64_t PendingTokenWatermark = 0;
		std::vector<StructuralPendingIdentity> Pending;
		std::uint64_t GrantToken = 0, GrantBytes = 0;
		std::uint64_t GrantSequence = 0, GrantAcceptedBefore = 0, GrantFirstSent = 0, GrantAcked = 0;
		std::array<std::uint64_t, 2> GrantFingerprint{};
		std::uint64_t CumulativeAccepted = 0, CumulativeFirstSent = 0, CumulativeAcked = 0;
	};
	struct StructuralDeliverySnapshot {
		ConnectionId Connection;
		std::uint64_t CumulativeAccepted = 0, CumulativeFirstSent = 0, CumulativeAcked = 0;
		std::uint64_t ActiveGrantToken = 0, ActiveGrantBytes = 0;
	};
	struct FrozenCessationQuote {
		std::unique_ptr<ReplicationCoordinator> Replication;
		std::map<ConnectionId, std::uint64_t> AcceptedUnretiredCompleteBytes;
		std::map<ConnectionId, std::size_t> MaximumFrameBytes;
	};
	struct JournalRequirement {
		ChangeCursor Cursor;
		ConnectionId Connection;
		bool Catalog = false;
		bool PreparedCommit = false;
		std::uint64_t NameCoalescingBegin = 0;
		bool PendingRelevance = false;
	};

	enum class GameSessionFailurePoint : std::uint8_t {
		None,
		TransportStart,
		SchedulerRegistration,
		ReplicationPeerCreation,
		LocalPlayerResolution,
		RemoteManagerPeerCreation,
		PredictedCharacterPeerCreation,
		RuntimeCallbackAttachment,
		ClientGraphSynchronization,
		ClientReadySerialization,
		ClientReadySchedulerAdmission,
		StructuralSchedulerAdmission,
	};

	class GameSessionTestAccess final {
	  public:
		static void SetFailurePoint(GameSession &Session, GameSessionFailurePoint Point);
		static void RequestSpatialValidation(GameSession &Session);
		[[nodiscard]] static bool VerifySpatialIndex(const GameSession &Session);
		[[nodiscard]] static std::optional<SpatialCellAddress> GetSpatialCellAddress(const GameSession &Session, ObjectId Object);
		[[nodiscard]] static CharacterNetworkMetrics GetCharacterMetrics(const GameSession &Session);
		[[nodiscard]] static bool RequestClientCharacterAction(GameSession &Session, std::uint32_t Token, std::uint64_t Tick);
		[[nodiscard]] static std::uint64_t GetReliableEventsAccepted(const GameSession &Session);
		[[nodiscard]] static RemoteMetrics GetRemoteMetrics(const GameSession &Session);
		[[nodiscard]] static std::vector<ConnectionId> GetConnections(const GameSession &Session);
		// Actual raw-history readers, not dependency/publication revision stamps.
		[[nodiscard]] static std::vector<JournalRequirement> GetJournalRequirements(const GameSession &Session);
		[[nodiscard]] static ReplicationMetrics GetReplicationMetrics(const GameSession &Session);
		[[nodiscard]] static std::optional<std::vector<StructuralCausalSnapshot>> CaptureStructuralCausalSnapshot(
			const GameSession &Session, std::string &Error);
		[[nodiscard]] static std::optional<std::vector<StructuralDeliverySnapshot>> GetStructuralDeliverySnapshots(
			const GameSession &Session, std::string &Error);
		// Main-only qualification capture. Accepted debt and the detached 3J
		// source are sampled without an intervening session step.
		[[nodiscard]] static std::optional<FrozenCessationQuote> CaptureFrozenCessationQuote(
			const GameSession &Session, std::string &Error);
	};
}
