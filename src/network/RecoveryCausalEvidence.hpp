#pragma once

#include "gargantuan/network/Connection.hpp"
#include "gargantuan/network/ReliableServiceProfile.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace gargantuan::network::detail {

	// Observer facts, never authority to advance a production cursor or cancel work.
	// The producer must establish the named semantic disposition. This verifier
	// checks causal continuity and ownership, not the semantic truth of an enum.
	enum class RecoveryCoverageDisposition : std::uint8_t {
		AcceptedFrame, AcceptedRepresentation, NotRelevant, NonReplicated,
		// Mixed legitimate dispositions: emitted only at the actual zero-operation
		// producer commit, not inferred from a sampled aggregate lag counter.
		SourceValidatedNoFrame,
	};
	enum class RecoveryCancellationDisposition : std::uint8_t {
		CurrentStateSuperseded,
	};
	struct RecoveryCoverage {
		std::uint64_t Before = 0, After = 0;
		RecoveryCoverageDisposition Disposition = RecoveryCoverageDisposition::AcceptedFrame;
		std::vector<std::uint64_t> ResolvedPendingTokens;
	};
	struct RecoveryPreparedFrame {
		ConnectionId Connection;
		std::uint64_t Sequence = 0, CompleteBytes = 0;
		std::array<std::uint64_t, 2> Fingerprint{};
		RecoveryCoverage Coverage;
	};
	struct RecoveryAcceptedGrant {
		std::uint64_t GrantToken = 0, Sequence = 0, CompleteBytes = 0;
		std::array<std::uint64_t, 2> Fingerprint{};
		std::uint64_t FirstSent = 0, Acked = 0;
	};
	struct RecoveryPeerFence {
		ConnectionId Connection;
		std::uint64_t JournalCursor = 0, JournalFence = 0, NextSequence = 0;
		// Exclusive watermark: all captured pending tokens must be below it.
		std::uint64_t PendingTokenWatermark = 0, ReferenceBytes = 0;
		std::vector<std::uint64_t> PendingTokens;
		std::vector<RecoveryAcceptedGrant> ExistingGrants;
	};
	struct RecoveryEvidenceTotals {
		std::uint64_t AcceptedBytes = 0, FirstSentBytes = 0, AckedBytes = 0;
		std::uint64_t RetiredBytes = 0, TerminalReleasedBytes = 0;
		std::uint64_t AcceptedFrames = 0, RetiredFrames = 0;
	};
	struct RecoveryPeerEvidence {
		ConnectionId Connection;
		std::uint64_t JournalCursor = 0, JournalFence = 0, NextSequence = 0, ReferenceBytes = 0;
		std::size_t UnresolvedBaselineTokens = 0, PendingTokens = 0, OutstandingGrants = 0;
		bool Represented = false, Converged = false, Disconnected = false;
		std::uint64_t CutGrantToken = 0, CutSequence = 0;
		// Both counters start with exactly the outstanding accepted debt at t0.
		// Prefix accepted totals freeze at the representation cut. Later delivery
		// only advances prefix counters for grants owned by that finite cut.
		RecoveryEvidenceTotals All, Prefix;
	};

	// Run/source-scope owned, bounded read-only qualification state. A new run or
	// DataModel scope needs a new instance. No wire/public epoch is introduced.
	class RecoveryCausalEvidence {
	  public:
		RecoveryCausalEvidence(std::size_t MaximumPeersValue, std::size_t MaximumEventsValue,
			std::size_t MaximumPendingTokensValue, std::size_t MaximumGrantsValue)
			: MaximumPeers(MaximumPeersValue), MaximumEvents(MaximumEventsValue),
			  MaximumPendingTokens(MaximumPendingTokensValue), MaximumGrants(MaximumGrantsValue) {}

		[[nodiscard]] const std::string &Failure() const { return Error; }
		bool RejectEvidence(const char *Reason) { return Fail(Reason); }
		[[nodiscard]] const RecoveryEvidenceTotals &Totals() const { return Count; }
		[[nodiscard]] std::size_t Events() const { return EventCount; }
		[[nodiscard]] std::optional<RecoveryPeerEvidence> Snapshot(ConnectionId Connection) const {
			const auto Found = Peers.find(Connection);
			if (Found == Peers.end()) return {};
			const auto &Peer = Found->second;
			bool Complete = Peer.Cut.has_value() && !Peer.Disconnected && Error.empty();
			for (const auto &[Token, Grant] : Peer.Grants) {
				(void)Grant;
				if (Peer.Cut && Token <= *Peer.Cut) Complete = false;
			}
			return RecoveryPeerEvidence{Connection, Peer.Cursor, Peer.Fence, Peer.NextSequence, Peer.ReferenceBytes,
				Peer.BaselinePending.size(), Peer.Pending.size(), Peer.Grants.size(),
				Peer.Cut.has_value() && !Peer.Disconnected && Error.empty(), Complete, Peer.Disconnected,
				Peer.Cut.value_or(0), Peer.CutSequence, Peer.Count, Peer.Prefix};
		}
		[[nodiscard]] std::uint64_t ReferenceBytes(ConnectionId Connection) const {
			const auto Found = Peers.find(Connection);
			return Found == Peers.end() ? 0 : Found->second.ReferenceBytes;
		}
		[[nodiscard]] bool Represented() const {
			if (!Error.empty() || Peers.empty()) return false;
			for (const auto &[Connection, Peer] : Peers) {
				(void)Connection;
				if (!Peer.Cut || Peer.Disconnected) return false;
			}
			return true;
		}
		[[nodiscard]] bool Converged() const {
			if (!Represented()) return false;
			for (const auto &[Connection, Peer] : Peers) {
				(void)Connection;
				for (const auto &[Token, Grant] : Peer.Grants) {
					(void)Grant;
					if (Token <= *Peer.Cut) return false;
				}
			}
			return true;
		}

		bool CapturePeer(const RecoveryPeerFence &Fence) {
			if (!Event()) return false;
			if (!Fence.Connection.IsValid() || Peers.contains(Fence.Connection) ||
				Peers.size() >= MaximumPeers || Fence.JournalCursor > Fence.JournalFence ||
				Fence.NextSequence == 0 || Fence.PendingTokenWatermark == 0 ||
				Fence.PendingTokens.size() > MaximumPendingTokens - PendingCount ||
				Fence.ExistingGrants.size() > 1 ||
				Fence.ExistingGrants.size() > MaximumGrants - GrantCount)
				return Fail("invalid or unbounded recovery fence");
			for (const auto &[Connection, Peer] : Peers) {
				(void)Peer;
				if (Connection.Slot == Fence.Connection.Slot) return Fail("peer slot reused inside recovery fence");
			}
			PeerState Peer;
			Peer.Cursor = Fence.JournalCursor;
			Peer.Fence = Fence.JournalFence;
			Peer.NextSequence = Fence.NextSequence;
			Peer.PendingWatermark = Fence.PendingTokenWatermark;
			Peer.LastPendingToken = Fence.PendingTokenWatermark - 1;
			Peer.ReferenceBytes = Fence.ReferenceBytes;
			for (const auto Token : Fence.PendingTokens) {
				if (Token == 0 || Token >= Fence.PendingTokenWatermark || !Peer.Pending.insert(Token).second)
					return Fail("invalid captured pending token");
				Peer.BaselinePending.insert(Token);
			}
			for (const auto &Grant : Fence.ExistingGrants) {
				if (!ValidGrant(Grant) || Grant.Sequence >= Fence.NextSequence ||
					Grant.GrantToken <= Peer.LastGrantToken ||
					( Peer.LastExistingSequence != 0 && Grant.Sequence <= Peer.LastExistingSequence))
					return Fail("invalid captured accepted grant");
				Peer.LastGrantToken = Grant.GrantToken;
				Peer.LastExistingSequence = Grant.Sequence;
				Peer.Grants.emplace(Grant.GrantToken, Grant);
				if (!Add(Count.AcceptedBytes, Grant.CompleteBytes) || !Add(Count.FirstSentBytes, Grant.FirstSent) ||
					!Add(Count.AckedBytes, Grant.Acked) || !Add(Count.AcceptedFrames, 1)) return false;
				Peer.Count.AcceptedBytes += Grant.CompleteBytes;
				Peer.Count.FirstSentBytes += Grant.FirstSent;
				Peer.Count.AckedBytes += Grant.Acked;
				++Peer.Count.AcceptedFrames;
			}
			PendingCount += Peer.Pending.size();
			GrantCount += Peer.Grants.size();
			Seal(Peer);
			Peers.emplace(Fence.Connection, std::move(Peer));
			return true;
		}

		bool ObservePending(ConnectionId Connection, std::uint64_t Token) {
			auto *Peer = FindEvent(Connection);
			if (!Peer) return false;
			if (Peer->Prepared || Token <= Peer->LastPendingToken || PendingCount >= MaximumPendingTokens)
				return Fail("invalid or unbounded new pending token");
			Peer->LastPendingToken = Token;
			Peer->Pending.insert(Token);
			++PendingCount;
			return true;
		}
		bool ValidateSourceSnapshot(ConnectionId Connection, std::uint64_t Cursor, std::uint64_t NextSequence,
			const std::vector<std::uint64_t> &PendingTokens) {
			auto *Peer = FindEvent(Connection);
			if (!Peer) return false;
			if (Peer->Prepared || Cursor != Peer->Cursor || NextSequence != Peer->NextSequence ||
				PendingTokens.size() != Peer->Pending.size()) return Fail("source snapshot differs from causal evidence");
			const std::set<std::uint64_t> Observed(PendingTokens.begin(), PendingTokens.end());
			if (Observed != Peer->Pending || Observed.size() != PendingTokens.size())
				return Fail("source pending snapshot differs from causal evidence");
			return true;
		}
		bool ObserveCancellation(ConnectionId Connection, std::uint64_t Token,
			RecoveryCancellationDisposition Disposition) {
			auto *Peer = FindEvent(Connection);
			if (!Peer) return false;
			if (Peer->Prepared || Disposition != RecoveryCancellationDisposition::CurrentStateSuperseded ||
				!Peer->Pending.erase(Token)) return Fail("unowned or invalid pending cancellation");
			--PendingCount;
			Peer->BaselinePending.erase(Token);
			Seal(*Peer);
			return true;
		}
		bool ObserveReplacement(ConnectionId Connection, std::uint64_t OldToken, std::uint64_t NewToken) {
			auto *Peer = FindEvent(Connection);
			if (!Peer) return false;
			if (Peer->Prepared || NewToken <= Peer->LastPendingToken || !Peer->Pending.erase(OldToken))
				return Fail("unowned or stale pending replacement");
			Peer->Pending.insert(NewToken);
			Peer->LastPendingToken = NewToken;
			if (Peer->BaselinePending.erase(OldToken)) Peer->BaselinePending.insert(NewToken);
			return true;
		}
		bool ObservePrepared(const RecoveryPreparedFrame &Frame) {
			auto *Peer = FindEvent(Frame.Connection);
			if (!Peer) return false;
			if (Peer->Prepared || Frame.Sequence != Peer->NextSequence || Frame.CompleteBytes == 0 ||
				Frame.CompleteBytes > MaximumReliableServiceGroupBytes ||
				Missing(Frame.Fingerprint) || Frame.Coverage.Disposition != RecoveryCoverageDisposition::AcceptedFrame ||
				!ValidCoverage(*Peer, Frame.Coverage)) return Fail("invalid exact prepared frame");
			Peer->Prepared = Frame;
			return true;
		}
		bool ObserveRejected(ConnectionId Connection, std::uint64_t Sequence) {
			auto *Peer = FindEvent(Connection);
			if (!Peer) return false;
			if (!Peer->Prepared || Peer->Prepared->Sequence != Sequence) return Fail("unowned rejected preparation");
			Peer->Prepared.reset();
			return true;
		}
		bool ObserveAccepted(ConnectionId Connection, std::uint64_t Sequence, std::uint64_t Token,
			std::uint64_t Bytes, std::array<std::uint64_t, 2> Fingerprint) {
			auto *Peer = FindEvent(Connection);
			if (!Peer) return false;
			if (!Peer->Prepared || Sequence != Peer->Prepared->Sequence || Bytes != Peer->Prepared->CompleteBytes ||
				Fingerprint != Peer->Prepared->Fingerprint || Token <= Peer->LastGrantToken ||
				!Peer->Grants.empty() ||
				GrantCount >= MaximumGrants || Sequence == std::numeric_limits<std::uint64_t>::max())
				return Fail("accepted frame differs from exact preparation or grant ownership");
			if (!Add(Count.AcceptedBytes, Bytes) || !Add(Count.AcceptedFrames, 1)) return false;
			Peer->Count.AcceptedBytes += Bytes;
			++Peer->Count.AcceptedFrames;
			Peer->Grants.emplace(Token, RecoveryAcceptedGrant{Token, Sequence, Bytes, Fingerprint});
			++GrantCount;
			Peer->LastGrantToken = Token;
			Peer->NextSequence = Sequence + 1;
			ApplyCoverage(*Peer, Peer->Prepared->Coverage);
			Peer->Prepared.reset();
			Seal(*Peer);
			return true;
		}
		bool ObserveCoverage(ConnectionId Connection, const RecoveryCoverage &Coverage) {
			auto *Peer = FindEvent(Connection);
			if (!Peer) return false;
			if (Peer->Prepared || Coverage.Disposition == RecoveryCoverageDisposition::AcceptedFrame ||
				(Coverage.Disposition != RecoveryCoverageDisposition::AcceptedRepresentation &&
				 Coverage.Disposition != RecoveryCoverageDisposition::NotRelevant &&
				 Coverage.Disposition != RecoveryCoverageDisposition::NonReplicated &&
				 Coverage.Disposition != RecoveryCoverageDisposition::SourceValidatedNoFrame) ||
				!Coverage.ResolvedPendingTokens.empty() || Coverage.After == Coverage.Before ||
				!ValidCoverage(*Peer, Coverage)) return Fail("invalid no-frame source coverage");
			ApplyCoverage(*Peer, Coverage);
			Seal(*Peer);
			return true;
		}
		bool ObserveDelivery(ConnectionId Connection, std::uint64_t Token, std::uint64_t FirstSent, std::uint64_t Acked) {
			auto *Peer = FindEvent(Connection);
			if (!Peer) return false;
			auto Found = Peer->Grants.find(Token);
			if (Found == Peer->Grants.end() || FirstSent < Found->second.FirstSent || Acked < Found->second.Acked ||
				Acked > FirstSent || FirstSent > Found->second.CompleteBytes) return Fail("invalid grant delivery evidence");
			if (!Add(Count.FirstSentBytes, FirstSent - Found->second.FirstSent) ||
				!Add(Count.AckedBytes, Acked - Found->second.Acked)) return false;
			Peer->Count.FirstSentBytes += FirstSent - Found->second.FirstSent;
			Peer->Count.AckedBytes += Acked - Found->second.Acked;
			if (Peer->Cut && Token <= *Peer->Cut) {
				Peer->Prefix.FirstSentBytes += FirstSent - Found->second.FirstSent;
				Peer->Prefix.AckedBytes += Acked - Found->second.Acked;
			}
			Found->second.FirstSent = FirstSent;
			Found->second.Acked = Acked;
			return true;
		}
		bool ObserveRetired(ConnectionId Connection, std::uint64_t Token, std::uint64_t Bytes) {
			auto *Peer = FindEvent(Connection);
			if (!Peer) return false;
			const auto Found = Peer->Grants.find(Token);
			if (Found == Peer->Grants.end() || Bytes != Found->second.CompleteBytes ||
				Found->second.Acked != Bytes || Found->second.FirstSent != Bytes)
				return Fail("retirement lacks exact first-send and ACK evidence");
			if (!Add(Count.RetiredBytes, Bytes) || !Add(Count.RetiredFrames, 1)) return false;
			Peer->Count.RetiredBytes += Bytes;
			++Peer->Count.RetiredFrames;
			if (Peer->Cut && Token <= *Peer->Cut) {
				Peer->Prefix.RetiredBytes += Bytes;
				++Peer->Prefix.RetiredFrames;
			}
			Peer->Grants.erase(Found);
			--GrantCount;
			return true;
		}
		bool ObserveDisconnect(ConnectionId Connection, std::uint64_t ReleasedBytes) {
			auto *Peer = FindEvent(Connection);
			if (!Peer) return false;
			std::uint64_t Outstanding = 0;
			for (const auto &[Token, Grant] : Peer->Grants) {
				(void)Token;
				if (!Add(Outstanding, Grant.CompleteBytes)) return false;
			}
			if (Outstanding != ReleasedBytes || !Add(Count.TerminalReleasedBytes, ReleasedBytes))
				return Fail("terminal release does not conserve exact ownership");
			Peer->Count.TerminalReleasedBytes += ReleasedBytes;
			for (const auto &[Token, Grant] : Peer->Grants)
				if (Peer->Cut && Token <= *Peer->Cut) Peer->Prefix.TerminalReleasedBytes += Grant.CompleteBytes;
			GrantCount -= Peer->Grants.size();
			PendingCount -= Peer->Pending.size();
			Peer->Grants.clear();
			Peer->Pending.clear();
			Peer->BaselinePending.clear();
			Peer->Prepared.reset();
			Peer->Disconnected = true;
			return true; // Correct cleanup is not successful recovery.
		}

	  private:
		struct PeerState {
			std::uint64_t Cursor = 0, Fence = 0, NextSequence = 0, PendingWatermark = 0;
			std::uint64_t LastPendingToken = 0, LastGrantToken = 0, LastExistingSequence = 0, ReferenceBytes = 0;
			std::set<std::uint64_t> Pending;
			std::set<std::uint64_t> BaselinePending;
			std::optional<RecoveryPreparedFrame> Prepared;
			std::map<std::uint64_t, RecoveryAcceptedGrant> Grants;
			std::optional<std::uint64_t> Cut;
			std::uint64_t CutSequence = 0;
			RecoveryEvidenceTotals Count, Prefix;
			bool Disconnected = false;
		};
		std::size_t MaximumPeers, MaximumEvents, MaximumPendingTokens, MaximumGrants;
		std::size_t EventCount = 0, PendingCount = 0, GrantCount = 0;
		std::map<ConnectionId, PeerState> Peers;
		RecoveryEvidenceTotals Count;
		std::string Error;
		bool Fail(const char *Reason) { if (Error.empty()) Error = Reason; return false; }
		bool Event() {
			if (!Error.empty()) return false;
			if (EventCount >= MaximumEvents) return Fail("recovery evidence event bound exceeded");
			++EventCount;
			return true;
		}
		PeerState *FindEvent(ConnectionId Connection) {
			if (!Event()) return nullptr;
			const auto Found = Peers.find(Connection);
			if (Found == Peers.end() || Found->second.Disconnected) {
				Fail("unknown or terminal recovery generation");
				return nullptr;
			}
			return &Found->second;
		}
		bool Add(std::uint64_t &Value, std::uint64_t Increment) {
			if (Increment > std::numeric_limits<std::uint64_t>::max() - Value) return Fail("recovery evidence sum overflow");
			Value += Increment;
			return true;
		}
		static bool Missing(std::array<std::uint64_t, 2> Fingerprint) { return Fingerprint == std::array<std::uint64_t, 2>{}; }
		static bool ValidGrant(const RecoveryAcceptedGrant &Grant) {
			return Grant.GrantToken != 0 && Grant.Sequence != 0 && Grant.CompleteBytes != 0 &&
				Grant.CompleteBytes <= MaximumReliableServiceGroupBytes &&
				!Missing(Grant.Fingerprint) && Grant.Acked <= Grant.FirstSent && Grant.FirstSent <= Grant.CompleteBytes;
		}
		bool ValidCoverage(const PeerState &Peer, const RecoveryCoverage &Coverage) const {
			if (Coverage.Before != Peer.Cursor || Coverage.After < Coverage.Before ||
				Coverage.ResolvedPendingTokens.size() > Peer.Pending.size()) return false;
			std::set<std::uint64_t> Seen;
			for (const auto Token : Coverage.ResolvedPendingTokens)
				if (!Peer.Pending.contains(Token) || !Seen.insert(Token).second) return false;
			return true;
		}
		void ApplyCoverage(PeerState &Peer, const RecoveryCoverage &Coverage) {
			Peer.Cursor = Coverage.After;
			for (const auto Token : Coverage.ResolvedPendingTokens) {
				Peer.Pending.erase(Token);
				Peer.BaselinePending.erase(Token);
				--PendingCount;
			}
		}
		static void Seal(PeerState &Peer) {
			if (Peer.Cut || Peer.Cursor < Peer.Fence || Peer.Prepared) return;
			if (!Peer.BaselinePending.empty()) return;
			Peer.Cut = Peer.LastGrantToken;
			Peer.CutSequence = Peer.NextSequence - 1;
			Peer.Prefix = Peer.Count;
		}
	};
}
