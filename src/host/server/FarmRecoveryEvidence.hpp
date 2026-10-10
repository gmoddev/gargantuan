#pragma once

#include "FarmAdmissionEvidence.hpp"
#include "network/GameSessionTestAccess.hpp"
#include "network/RecoveryCausalEvidence.hpp"

#include <chrono>
#include <map>
#include <sstream>

namespace gargantuan::host::detail {

	// Read-only Main-thread adapter. Events contain identity/coverage, never a
	// second payload queue. Formatting and exclusive file writes occur after the
	// measured workload. Every borrowed source span is copied within the callback.
	class FarmRecoveryEvidence final {
		using Event = network::detail::StructuralCausalEvent;
		using Kind = network::detail::StructuralCausalKind;
		using Snapshot = network::detail::StructuralCausalSnapshot;
		struct OwnedEvent {
			Event Value;
			std::uint64_t AtMicroseconds = 0;
			std::vector<network::detail::StructuralPendingIdentity> Pending;
			std::vector<ObjectId> Entering, Leaving;
		};
		static constexpr std::size_t MaximumEvents = 65'536;
		static constexpr std::size_t MaximumIdentities = 1'048'576;
		// In addition to trace events, the verifier counts 32 captures and the
		// 32-peer source checks at the service snapshot and final deadline.
		network::detail::RecoveryCausalEvidence Verifier{32, MaximumEvents + 3 * 32, MaximumIdentities, 4};
		std::vector<Snapshot> Initial;
		std::vector<Snapshot> FinalSource;
		std::vector<OwnedEvent> Events;
		std::vector<network::FrozenJournalQuoteFrame> Reference;
		std::map<network::ConnectionId, std::array<std::uint64_t, 3>> LastDelivery;
		ObjectId Scope;
		std::uint64_t IdentityCount = 0;
		std::string Error;
		bool AllocationFailed = false, Dumped = false;
		network::detail::StructuralCausalEvidenceSink Sink{this, Record};
		network::detail::StructuralCausalEvidenceSink *Previous = nullptr;
		static void Record(void *Context, const Event &Value) noexcept {
			auto &Self = *static_cast<FarmRecoveryEvidence *>(Context);
			try { Self.Observe(Value); } catch (...) { Self.AllocationFailed = true; }
		}
		void Observe(const Event &Value) {
			using namespace network::detail;
			if (!Failure().empty()) return;
			if (Value.Valid && Value.SourceScope == Scope && Value.Kind == Kind::Delivery) {
				const std::array Current{Value.GrantToken, Value.FirstSent, Value.Acked};
				if (LastDelivery[Value.Connection] == Current) return;
				LastDelivery[Value.Connection] = Current;
			}
			const auto Identities = Value.ResolvedPending.size() + Value.Entering.size() + Value.Leaving.size();
			if (Events.size() == MaximumEvents || Identities > MaximumIdentities - IdentityCount) {
				Error = "causal recovery evidence bound exceeded"; return;
			}
			IdentityCount += Identities;
			OwnedEvent Copy{Value, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count()),
				{Value.ResolvedPending.begin(), Value.ResolvedPending.end()},
				{Value.Entering.begin(), Value.Entering.end()}, {Value.Leaving.begin(), Value.Leaving.end()}};
			// Do not retain dangling borrowed spans even though formatting uses owners.
			Copy.Value.ResolvedPending = {}; Copy.Value.Entering = {}; Copy.Value.Leaving = {};
			Events.push_back(std::move(Copy));
			if (!Value.Valid || Value.SourceScope != Scope) { Error = "invalid causal scope or native delivery"; return; }
			RecoveryCoverage Coverage{Value.CursorBefore, Value.CursorAfter};
			for (const auto &Pending : Value.ResolvedPending) Coverage.ResolvedPendingTokens.push_back(Pending.Token);
			switch (Value.Kind) {
			case Kind::Prepared:
				Verifier.ObservePrepared({Value.Connection, Value.Sequence, Value.CompleteBytes, Value.Fingerprint, Coverage}); break;
			case Kind::Accepted:
				Verifier.ObserveAccepted(Value.Connection, Value.Sequence, Value.GrantToken, Value.CompleteBytes, Value.Fingerprint); break;
			case Kind::Rejected: Verifier.ObserveRejected(Value.Connection, Value.Sequence); break;
			case Kind::NoFrame:
				if (Value.Reason != StructuralCausalReason::FilteredOrAlreadyCovered) { Error = "unexplained no-frame coverage"; break; }
				Coverage.Disposition = RecoveryCoverageDisposition::SourceValidatedNoFrame;
				Verifier.ObserveCoverage(Value.Connection, Coverage); break;
			case Kind::PendingAdded: Verifier.ObservePending(Value.Connection, Value.Pending.Token); break;
			case Kind::PendingCancelled:
				if (Value.Reason == StructuralCausalReason::None) { Error = "unexplained pending cancellation"; break; }
				Verifier.ObserveCancellation(Value.Connection, Value.Pending.Token,
					RecoveryCancellationDisposition::CurrentStateSuperseded); break;
			case Kind::PendingReplaced:
				Verifier.ObserveReplacement(Value.Connection, Value.Pending.Token, Value.ReplacementToken); break;
			case Kind::PlanningInstalled:
				Verifier.ObservePlanningInstalled(Value.Connection, Coverage.ResolvedPendingTokens); break;
			case Kind::Delivery: Verifier.ObserveDelivery(Value.Connection, Value.GrantToken, Value.FirstSent, Value.Acked); break;
			case Kind::Retired: Verifier.ObserveRetired(Value.Connection, Value.GrantToken, Value.CompleteBytes); break;
			case Kind::PeerRemoved: Error = "required recovery generation disconnected"; break;
			}
		}
	  public:
		explicit FarmRecoveryEvidence(std::vector<Snapshot> Captured) : Initial(std::move(Captured)) {
			using namespace network::detail;
			if (Initial.size() != 32) throw std::invalid_argument("recovery requires 32 causal snapshots");
			Scope = Initial.front().SourceScope;
			for (const auto &Peer : Initial) {
				if (Peer.SourceScope != Scope) throw std::invalid_argument("recovery source scopes differ");
				RecoveryPeerFence Fence{Peer.Connection, Peer.JournalCursor, Peer.JournalTail, Peer.NextSequence,
					Peer.PendingTokenWatermark};
				Fence.HasUnresolvedPlanning = Peer.HasUnresolvedPlanning;
				for (const auto &Pending : Peer.Pending) Fence.PendingTokens.push_back(Pending.Token);
				if (Peer.GrantBytes) Fence.ExistingGrants.push_back({Peer.GrantToken, Peer.GrantSequence,
					Peer.GrantBytes, Peer.GrantFingerprint, Peer.GrantFirstSent, Peer.GrantAcked});
				Verifier.CapturePeer(Fence);
			}
			Events.reserve(MaximumEvents);
			Previous = ActiveStructuralCausalEvidence;
			ActiveStructuralCausalEvidence = &Sink;
		}
		~FarmRecoveryEvidence() { Detach(); }
		FarmRecoveryEvidence(const FarmRecoveryEvidence &) = delete;
		FarmRecoveryEvidence &operator=(const FarmRecoveryEvidence &) = delete;
		void Detach() {
			if (network::detail::ActiveStructuralCausalEvidence == &Sink)
				network::detail::ActiveStructuralCausalEvidence = Previous;
		}
		[[nodiscard]] std::string Failure() const {
			if (AllocationFailed) return "causal recovery observation allocation failed";
			return Error.empty() ? Verifier.Failure() : Error;
		}
		[[nodiscard]] const auto &Audit() const { return Verifier; }
		[[nodiscard]] const auto &Peers() const { return Initial; }
		[[nodiscard]] std::size_t EventCount() const { return Events.size(); }
		[[nodiscard]] std::uint64_t InitialDebt() const {
			std::uint64_t Total = 0;
			for (const auto &Peer : Initial) Total += Peer.GrantBytes;
			return Total;
		}
		void ReferenceFrame(const network::FrozenJournalQuoteFrame &Frame) {
			if (Reference.size() >= MaximumEvents) { Error = "reference frame evidence bound exceeded"; return; }
			Reference.push_back(Frame);
		}
		void ValidateSource(const network::GameSession &Session) {
			if (!Failure().empty()) return;
			std::string CaptureError;
			auto Current = network::detail::GameSessionTestAccess::CaptureStructuralCausalSnapshot(Session, CaptureError);
			if (!Current) { Error = CaptureError.empty() ? "causal source capture failed" : CaptureError; return; }
			if (Current->size() != Initial.size()) { Error = "causal source peer set changed"; return; }
			for (const auto &Peer : *Current) {
				if (Peer.SourceScope != Scope) { Error = "causal source scope changed"; return; }
				std::vector<std::uint64_t> Tokens;
				for (const auto &Pending : Peer.Pending) Tokens.push_back(Pending.Token);
				Verifier.ValidateSourceSnapshot(Peer.Connection, Peer.JournalCursor, Peer.NextSequence, Tokens);
			}
			FinalSource = std::move(*Current);
		}
		[[nodiscard]] bool Converged() const { return Failure().empty() && Verifier.Converged(); }
		void Dump(const FarmAdmissionEvidence &Writer, std::string_view Run, std::string_view Case) {
			if (Dumped) return;
			Writer.WriteRecovery(Case, [&](auto &Output) {
				Output.Write("format=GargantuanRecoveryCausalV1\trun=" + std::string(Run) + "\tcase=" + std::string(Case) + "\n");
				for (const auto &Peer : Initial) {
					std::ostringstream Line;
					Line << "capture\t" << Peer.Connection.Slot << '\t' << Peer.Connection.Generation
						<< '\t' << Scope.Slot << '\t' << Scope.Generation << '\t' << Peer.JournalCursor
						<< '\t' << Peer.JournalTail << '\t' << Peer.NextSequence << '\t' << Peer.PendingTokenWatermark
						<< '\t' << Peer.GrantToken << '\t' << Peer.GrantSequence << '\t' << Peer.GrantBytes
						<< '\t' << Peer.GrantFingerprint[0] << '\t' << Peer.GrantFingerprint[1]
						<< '\t' << Peer.GrantFirstSent << '\t' << Peer.GrantAcked
						<< '\t' << Peer.CumulativeAccepted << '\t' << Peer.CumulativeFirstSent
						<< '\t' << Peer.CumulativeAcked << '\t' << Peer.GrantAcceptedBefore << '\t' << Peer.HasUnresolvedPlanning;
					for (const auto &Pending : Peer.Pending) Line << '\t' << Pending.Token << ':' << Pending.Object.Slot
						<< ':' << Pending.Object.Generation << ':' << Pending.Enter;
					Line << '\n'; Output.Write(Line.str());
				}
				for (const auto &Stored : Events) {
					const auto &E = Stored.Value;
					std::ostringstream Line;
					Line << "event\t" << static_cast<unsigned>(E.Kind) << '\t' << static_cast<unsigned>(E.Reason)
						<< '\t' << Stored.AtMicroseconds << '\t' << E.Connection.Slot << '\t' << E.Connection.Generation
						<< '\t' << E.Sequence << '\t' << E.GrantToken << '\t' << E.CompleteBytes
						<< '\t' << E.CursorBefore << '\t' << E.CursorAfter << '\t' << E.Fingerprint[0] << '\t' << E.Fingerprint[1]
						<< '\t' << E.FirstSent << '\t' << E.Acked << '\t' << E.Pending.Token << '\t' << E.ReplacementToken
						<< '\t' << E.Pending.Object.Slot << '\t' << E.Pending.Object.Generation << '\t' << E.Pending.Enter
						<< '\t' << E.SourceScope.Slot << '\t' << E.SourceScope.Generation << '\t' << E.Valid << '\t' << E.AcceptedBefore;
					for (const auto &Pending : Stored.Pending) Line << "\tp:" << Pending.Token << ':' << Pending.Object.Slot
						<< ':' << Pending.Object.Generation << ':' << Pending.Enter;
					for (const auto Id : Stored.Entering) Line << "\te:" << Id.Slot << ':' << Id.Generation;
					for (const auto Id : Stored.Leaving) Line << "\tl:" << Id.Slot << ':' << Id.Generation;
					Line << '\n'; Output.Write(Line.str());
				}
				for (const auto &Frame : Reference) {
					std::ostringstream Line;
					Line << "quote\t" << Frame.Connection.Slot << '\t' << Frame.Connection.Generation << '\t'
						<< Frame.Sequence.Value() << '\t' << Frame.CompleteBytes << '\t'
						<< Frame.Fingerprint[0] << '\t' << Frame.Fingerprint[1] << '\t'
						<< Frame.CursorBefore << '\t' << Frame.CursorAfter << '\n';
					Output.Write(Line.str());
				}
				for (const auto &Peer : FinalSource) {
					std::ostringstream Line;
					Line << "source\t" << Peer.Connection.Slot << '\t' << Peer.Connection.Generation << '\t'
						<< Peer.JournalCursor << '\t' << Peer.NextSequence;
					for (const auto &Pending : Peer.Pending) Line << '\t' << Pending.Token;
					Line << '\n'; Output.Write(Line.str());
				}
				Output.Write("end\t" + std::to_string(Events.size()) + "\t" + (Failure().empty() ? "0\n" : "1\n"));
			});
			Dumped = true;
		}
	};
}
