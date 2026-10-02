#pragma once

#include "../src/network/RecoveryCausalEvidence.hpp"
#include <iostream>

namespace gargantuan::test {

	inline bool RunRecoveryCausalEvidenceTests() {
		using namespace network;
		using namespace network::detail;
		std::size_t Cases = 0, Failures = 0;
		auto Test = [&](const char *Name, auto Body) {
			++Cases;
			if (!Body()) { ++Failures; std::cerr << "[Recovery:CausalEvidence] FAIL " << Name << '\n'; }
		};
		const ConnectionId Peer{1, 1};
		const std::array<std::uint64_t, 2> Hash{123, 456};
		auto Fence = [&] {
			return RecoveryPeerFence{Peer, 10, 20, 1, 4, 512, {2}, {}};
		};
		auto Frame = [&](std::uint64_t Sequence, std::uint64_t Before, std::uint64_t After,
			std::vector<std::uint64_t> Tokens = {}, std::uint64_t Bytes = 77) {
			return RecoveryPreparedFrame{Peer, Sequence, Bytes, Hash,
				{Before, After, RecoveryCoverageDisposition::AcceptedFrame, std::move(Tokens)}};
		};
		auto Accept = [&](RecoveryCausalEvidence &Audit, const RecoveryPreparedFrame &Value, std::uint64_t Token) {
			return Audit.ObservePrepared(Value) && Audit.ObserveAccepted(Peer, Value.Sequence, Token,
				Value.CompleteBytes, Value.Fingerprint);
		};
		auto Retire = [&](RecoveryCausalEvidence &Audit, std::uint64_t Token, std::uint64_t Bytes = 77) {
			return Audit.ObserveDelivery(Peer, Token, Bytes, Bytes) && Audit.ObserveRetired(Peer, Token, Bytes);
		};
		Test("R1 frozen baseline completes only after exact delivery and retirement", [&] {
			RecoveryCausalEvidence Audit(32, 1000, 100, 4);
			return Audit.CapturePeer(Fence()) && !Audit.Represented() &&
				Accept(Audit, Frame(1, 10, 20, {2}), 1) && Audit.Represented() && !Audit.Converged() &&
				Retire(Audit, 1) && Audit.Converged() && Audit.ReferenceBytes(Peer) == 512;
		});
		Test("R2 post-cessation Leave keeps its own exact byte obligation", [&] {
			RecoveryCausalEvidence Audit(32, 1000, 100, 4);
			return Audit.CapturePeer(Fence()) && Audit.ObservePending(Peer, 4) &&
				Accept(Audit, Frame(1, 10, 10, {4}, 118), 1) && !Audit.Represented() && Retire(Audit, 1, 118) &&
				Audit.ObserveCancellation(Peer, 2, RecoveryCancellationDisposition::CurrentStateSuperseded) &&
				Audit.ObserveCoverage(Peer, {10, 20, RecoveryCoverageDisposition::NotRelevant, {}}) &&
				Audit.Converged() && Audit.Totals().RetiredBytes == 118;
		});
		Test("R3 live Enter may also represent cessation journal coverage exactly once", [&] {
			RecoveryCausalEvidence Audit(32, 1000, 100, 4);
			return Audit.CapturePeer(Fence()) && Audit.ObservePending(Peer, 4) &&
				Accept(Audit, Frame(1, 10, 20, {2, 4}), 1) && Retire(Audit, 1) &&
				Audit.Converged() && Audit.Totals().AcceptedBytes == 77;
		});
		Test("R4 unaccepted Enter Leave cancellation creates no wire bytes", [&] {
			RecoveryCausalEvidence Audit(32, 1000, 100, 4);
			return Audit.CapturePeer(Fence()) && Audit.ObservePending(Peer, 4) &&
				Audit.ObserveCancellation(Peer, 4, RecoveryCancellationDisposition::CurrentStateSuperseded) &&
				Accept(Audit, Frame(1, 10, 20, {2}), 1) && Retire(Audit, 1) &&
				Audit.Totals().AcceptedFrames == 1;
		});
		Test("R4 accepted Enter cannot be cancelled by subsequent Leave", [&] {
			RecoveryCausalEvidence Audit(32, 1000, 100, 4);
			return Audit.CapturePeer(Fence()) && Accept(Audit, Frame(1, 10, 20, {2}), 1) &&
				!Audit.ObserveCancellation(Peer, 2, RecoveryCancellationDisposition::CurrentStateSuperseded) &&
				!Audit.Converged();
		});
		Test("R5 oscillation replacements retain the original baseline ownership", [&] {
			RecoveryCausalEvidence Audit(32, 1000, 1, 4);
			if (!Audit.CapturePeer(Fence()) || !Audit.ObserveCoverage(Peer,
				{10, 20, RecoveryCoverageDisposition::AcceptedRepresentation, {}})) return false;
			std::uint64_t Prior = 2;
			for (std::uint64_t Token = 4; Token < 104; ++Token) {
				if (!Audit.ObserveReplacement(Peer, Prior, Token) || Audit.Represented()) return false;
				Prior = Token;
			}
			return Accept(Audit, Frame(1, 20, 20, {Prior}), 1) && Retire(Audit, 1) && Audit.Converged();
		});
		Test("R5 bounded audit fails closed rather than truncating oscillation", [&] {
			RecoveryCausalEvidence Audit(32, 2, 1, 4);
			return Audit.CapturePeer(Fence()) && Audit.ObserveReplacement(Peer, 2, 4) &&
				!Audit.ObserveReplacement(Peer, 4, 5) && !Audit.Converged();
		});
		Test("R6 rejected coalesced candidate cannot advance coverage", [&] {
			RecoveryCausalEvidence Audit(32, 1000, 100, 4);
			return Audit.CapturePeer(Fence()) && Audit.ObservePrepared(Frame(1, 10, 20, {2})) &&
				Audit.ObserveRejected(Peer, 1) && !Audit.Represented() &&
				Accept(Audit, Frame(1, 10, 20, {2}), 2) && Retire(Audit, 2) && Audit.Converged();
		});
		Test("R6 no-frame represented prefix is legal but cannot swallow pending", [&] {
			RecoveryCausalEvidence Audit(32, 1000, 100, 4);
			return Audit.CapturePeer(Fence()) && Audit.ObserveCoverage(Peer,
				{10, 20, RecoveryCoverageDisposition::AcceptedRepresentation, {}}) && !Audit.Represented();
		});
		Test("R7 later live work does not reopen the finite recovery cut", [&] {
			RecoveryCausalEvidence Audit(32, 1000, 100, 4);
			return Audit.CapturePeer(Fence()) && Accept(Audit, Frame(1, 10, 20, {2}), 1) && Retire(Audit, 1) &&
				Audit.ObservePending(Peer, 4) && Accept(Audit, Frame(2, 20, 21, {4}), 2) &&
				Audit.Converged() && Audit.Totals().AcceptedBytes == 154 && Audit.Totals().RetiredBytes == 77;
		});
		Test("R7 healthy live service cannot hide an unresolved baseline", [&] {
			RecoveryCausalEvidence Audit(32, 1000, 100, 4);
			return Audit.CapturePeer(Fence()) && Audit.ObservePending(Peer, 4) &&
				Accept(Audit, Frame(1, 10, 10, {4}), 1) && Retire(Audit, 1) && !Audit.Represented();
		});
		Test("R8 disconnect conserves terminal bytes but never passes recovery", [&] {
			RecoveryCausalEvidence Audit(32, 1000, 100, 4);
			return Audit.CapturePeer(Fence()) && Accept(Audit, Frame(1, 10, 20, {2}), 1) &&
				Audit.ObserveDisconnect(Peer, 77) && !Audit.Converged() &&
				Audit.Totals().TerminalReleasedBytes == 77 && Audit.Totals().RetiredBytes == 0;
		});
		Test("R8 a stale or replaced generation cannot inherit the fence", [&] {
			RecoveryCausalEvidence Audit(32, 1000, 100, 4);
			return Audit.CapturePeer(Fence()) && !Audit.ObservePending({1, 2}, 4) && !Audit.Converged();
		});
		Test("R9 verifier is provider independent for the same causal trace", [&] {
			RecoveryCausalEvidence Local(32, 1000, 100, 4), Node(32, 1000, 100, 4);
			for (auto *Audit : {&Local, &Node})
				if (!Audit->CapturePeer(Fence()) || !Accept(*Audit, Frame(1, 10, 20, {2}), 1) || !Retire(*Audit, 1)) return false;
			return Local.Converged() && Node.Converged() && Local.Totals().RetiredBytes == Node.Totals().RetiredBytes;
		});
		Test("R10 cessation debt belongs to the prefix even if cursor is caught up", [&] {
			RecoveryCausalEvidence Audit(32, 1000, 100, 4);
			auto Initial = Fence();
			Initial.JournalCursor = 20; Initial.NextSequence = 2; Initial.PendingTokens.clear();
			Initial.ExistingGrants.push_back({8, 1, 77, Hash, 50, 25});
			return Audit.CapturePeer(Initial) && Audit.Represented() && !Audit.Converged() && Retire(Audit, 8) &&
				Audit.Converged() && Audit.Totals().FirstSentBytes == 77 && Audit.Totals().AckedBytes == 77;
		});
		Test("R10 exact accepted hash mismatch fails", [&] {
			RecoveryCausalEvidence Audit(32, 1000, 100, 4);
			return Audit.CapturePeer(Fence()) && Audit.ObservePrepared(Frame(1, 10, 20, {2})) &&
				!Audit.ObserveAccepted(Peer, 1, 1, 77, {456, 123});
		});
		Test("R10 missing prepared fingerprint fails", [&] {
			RecoveryCausalEvidence Audit(32, 1000, 100, 4);
			auto Value = Frame(1, 10, 20, {2}); Value.Fingerprint = {};
			return Audit.CapturePeer(Fence()) && !Audit.ObservePrepared(Value);
		});
		Test("R10 skipped or overlapping source cursor fails", [&] {
			RecoveryCausalEvidence Audit(32, 1000, 100, 4);
			return Audit.CapturePeer(Fence()) && !Audit.ObserveCoverage(Peer,
				{11, 20, RecoveryCoverageDisposition::NotRelevant, {}});
		});
		Test("R10 premature retirement cannot fabricate ACK", [&] {
			RecoveryCausalEvidence Audit(32, 1000, 100, 4);
			return Audit.CapturePeer(Fence()) && Accept(Audit, Frame(1, 10, 20, {2}), 1) &&
				Audit.ObserveDelivery(Peer, 1, 77, 76) && !Audit.ObserveRetired(Peer, 1, 77);
		});
		Test("R10 duplicate retirement fails", [&] {
			RecoveryCausalEvidence Audit(32, 1000, 100, 4);
			return Audit.CapturePeer(Fence()) && Accept(Audit, Frame(1, 10, 20, {2}), 1) && Retire(Audit, 1) &&
				!Audit.ObserveRetired(Peer, 1, 77);
		});
		Test("R10 prefix receipt remains exact while new live debt exists", [&] {
			RecoveryCausalEvidence Audit(32, 1000, 100, 4);
			if (!Audit.CapturePeer(Fence()) || !Accept(Audit, Frame(1, 10, 20, {2}), 1) ||
				!Audit.ObserveDelivery(Peer, 1, 40, 20)) return false;
			const auto Partial = Audit.Snapshot(Peer);
			if (!Partial || Partial->Converged || Partial->CutGrantToken != 1 || Partial->CutSequence != 1 ||
				Partial->Prefix.AcceptedBytes != 77 || Partial->Prefix.FirstSentBytes != 40 ||
				Partial->Prefix.AckedBytes != 20 || Partial->UnresolvedBaselineTokens != 0) return false;
			if (!Retire(Audit, 1) || !Accept(Audit, Frame(2, 20, 21), 2)) return false;
			const auto Result = Audit.Snapshot(Peer);
			return Result && Result->Converged && Result->Prefix.AcceptedBytes == 77 &&
				Result->Prefix.RetiredBytes == 77 && Result->All.AcceptedBytes == 154 &&
				Result->All.RetiredBytes == 77 && Result->OutstandingGrants == 1;
		});
		Test("R10 canonical maximum complete group cannot be exceeded", [&] {
			RecoveryCausalEvidence Audit(32, 1000, 100, 4);
			return Audit.CapturePeer(Fence()) &&
				!Audit.ObservePrepared(Frame(1, 10, 20, {2}, MaximumReliableServiceGroupBytes + 1));
		});
		Test("R10 duplicate grant token cannot be reused after retirement", [&] {
			RecoveryCausalEvidence Audit(32, 1000, 100, 4);
			return Audit.CapturePeer(Fence()) && Accept(Audit, Frame(1, 10, 20, {2}), 1) && Retire(Audit, 1) &&
				Audit.ObservePrepared(Frame(2, 20, 21)) && !Audit.ObserveAccepted(Peer, 2, 1, 77, Hash);
		});
		Test("R10 second active peer grant remains forbidden", [&] {
			RecoveryCausalEvidence Audit(32, 1000, 100, 4);
			return Audit.CapturePeer(Fence()) && Accept(Audit, Frame(1, 10, 20, {2}), 1) &&
				Audit.ObservePrepared(Frame(2, 20, 21)) && !Audit.ObserveAccepted(Peer, 2, 2, 77, Hash);
		});
		Test("R10 externally invalid native feedback revokes a prior convergence", [&] {
			RecoveryCausalEvidence Audit(32, 1000, 100, 4);
			return Audit.CapturePeer(Fence()) && Accept(Audit, Frame(1, 10, 20, {2}), 1) && Retire(Audit, 1) &&
				Audit.Converged() && !Audit.RejectEvidence("native feedback invalid") && !Audit.Converged();
		});
		Test("R10 source snapshot corroborates event derived state", [&] {
			RecoveryCausalEvidence Audit(32, 1000, 100, 4);
			return Audit.CapturePeer(Fence()) && Audit.ValidateSourceSnapshot(Peer, 10, 1, {2}) &&
				Audit.ObserveReplacement(Peer, 2, 4) && Audit.ValidateSourceSnapshot(Peer, 10, 1, {4});
		});
		Test("R10 a missing cancellation event is detected not inferred", [&] {
			RecoveryCausalEvidence Audit(32, 1000, 100, 4);
			return Audit.CapturePeer(Fence()) && !Audit.ValidateSourceSnapshot(Peer, 10, 1, {});
		});
		Test("R10 a missing cursor coverage event is detected not inferred", [&] {
			RecoveryCausalEvidence Audit(32, 1000, 100, 4);
			return Audit.CapturePeer(Fence()) && !Audit.ValidateSourceSnapshot(Peer, 20, 1, {2});
		});
		std::cout << "[Recovery:CausalEvidence] cases=" << Cases << " failures=" << Failures << '\n';
		return Failures == 0;
	}
}
