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
#include "PooledServiceDiagnostics.hpp"

namespace gargantuan::network {
	class GameSession;
	class ReplicaApplier;
	struct ReplicationMetrics;
}

namespace gargantuan::network::detail {
	struct ClientReplicaIdentity {
		ObjectId Source, Local;
		ReplicationEpoch Epoch;
	};
	struct PooledPostpassPeer {
		ConnectionId Connection;
		bool Ready = false, FeedbackComplete = false, Healthy = false;
		PooledExecutionState Execution;
		ReliableServiceAcceptedBytes Accepted, ReconciledAccepted;
		std::uint64_t FirstSent = 0, Acked = 0, Retired = 0, ReliableAcked = 0;
		std::uint64_t DebtToken = 0, Debt = 0, OrdinaryDebt = 0, OrdinaryUnsent = 0;
		std::uint64_t NativePending = 0, NativeUnacked = 0, NativeGrantToken = 0;
	};
	struct PooledPostpassSnapshot {
		ReliableByteAdmissionMetrics Admission;
		std::vector<PooledPostpassPeer> Peers;
	};
	enum class StructuralCausalKind : std::uint8_t {
		Prepared, Accepted, Rejected, NoFrame, PendingAdded, PendingCancelled, PendingReplaced, PeerRemoved, Delivery, Retired,
		PlanningInstalled,
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
		bool HasUnresolvedPlanning = false;
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
		// Main-owned qualification observation only. Source and local identity
		// domains stay distinct; no lookup by Name/path or persistent reverse map.
		[[nodiscard]] static std::optional<std::vector<ClientReplicaIdentity>> ReadClientReplicaIdentities(
			const GameSession &Session, std::span<const std::shared_ptr<Instance>> Observed);
		[[nodiscard]] static std::optional<std::vector<ClientReplicaIdentity>> ReadClientReplicaIdentities(
			const ReplicaApplier &Replica, std::span<const std::shared_ptr<Instance>> Observed);
		// Bounded forward checks at an immutable observation cut, after the
		// independently serviced source-map discovery. No reverse index or poll.
		[[nodiscard]] static bool VerifyClientReplicaIdentities(const GameSession &Session,
			std::span<const std::shared_ptr<Instance>> Observed, std::span<const ClientReplicaIdentity> Identities);
		[[nodiscard]] static bool VerifyClientReplicaIdentities(const ReplicaApplier &Replica,
			std::span<const std::shared_ptr<Instance>> Observed, std::span<const ClientReplicaIdentity> Identities);
		// Copies the actual ledger AFTER the production admission pass. Never
		// polls native feedback, advances clocks or reconciles/mutates ownership.
		[[nodiscard]] static std::optional<PooledPostpassSnapshot> ReadPooledPostpass(const GameSession &Session);
		// Internal A/B fixture only. No runtime configuration or wire policy.
		static void SetIncrementalEncodingOptimizationsEnabled(ReplicationCoordinator &Coordinator, bool Enabled) {
			Coordinator.IncrementalEncodingOptimizationsEnabled = Enabled;
		}
		static void SetFailurePoint(GameSession &Session, GameSessionFailurePoint Point);
		// Trusted source scope only; affects future enrollment, not acceptance or
		// clocks already owned by queued/native messages. No budget reset.
		[[nodiscard]] static bool SetOrdinarySourceNormal(GameSession &Session, bool Normal);
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
			const GameSession &Session, std::string &Error, std::uint64_t NativeDeadlineMicroseconds = 0);
		// Recovery source validation reads only journal/sequence/pending identity.
		// Native feedback may be transiently incomplete while a send is pending.
		[[nodiscard]] static std::optional<std::vector<StructuralCausalSnapshot>> CaptureStructuralSourceSnapshot(
			const GameSession &Session, std::string &Error);
		[[nodiscard]] static std::optional<std::vector<StructuralDeliverySnapshot>> GetStructuralDeliverySnapshots(
			const GameSession &Session, std::string &Error);
		// Main-only qualification capture. Accepted debt and the detached 3J
		// source are sampled without an intervening session step.
		[[nodiscard]] static std::optional<FrozenCessationQuote> CaptureFrozenCessationQuote(
			const GameSession &Session, std::string &Error);
	};
}
