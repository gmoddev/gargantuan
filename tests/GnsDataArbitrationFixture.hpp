#pragma once
#include <clientlib/steamnetworkingsockets_connections.h>
#include "ReliableServiceFeedback.hpp"
#include <array>
#include <vector>

namespace SteamNetworkingSocketsLib {
// Exercise the real encrypted UDP serializer and its existing segment owners.
// Manual polling excludes the actor; synthetic times qualify selection, not OS
// service latency. No alternative packet collector exists in this fixture.
struct GargantuanDataArbitrationTest {
	static bool ThirtyOneRetained() {
		// Raw native unit arrangement only: no GameSession, native32 campaign,
		// client workload, admission bypass claim or measured F1 certification.
		auto *Api = SteamNetworkingSockets();
		std::array<HSteamNetConnection, 32> Senders{}, Receivers{};
		std::array<unsigned, 31> RetryPackets{};
		std::vector<unsigned char> Retained(21216, 0x31), Ordinary(20480, 0x57);
		bool Passed = true;
		auto Now = SteamNetworkingSockets_GetLocalTimestamp() + 1000000;
		for (unsigned Flow = 0; Flow < Senders.size(); ++Flow) {
			if (!Api->CreateSocketPair(&Senders[Flow], &Receivers[Flow], true, nullptr, nullptr)) { Passed = false; break; }
			if (Flow < 31) Passed = Api->SendMessageToConnection(Senders[Flow], Retained.data(), int(Retained.size()),
				k_nSteamNetworkingSend_ReliableNoNagle, nullptr) == k_EResultOK && Passed;
			SteamNetworkingGlobalLock Global("DataArbitrationThirtyOneArrange");
			ConnectionScopeLock Lock;
			auto *Connection = GetConnectionByHandle(Senders[Flow], Lock);
			if (!Connection) { Passed = false; break; }
			auto &State = Connection->m_senderState;
			Connection->m_sendRateData.m_flCurrentSendRateUsed = 18.0f * 1024 * 1024;
			for (unsigned Packet = 0; Packet < 32 && !State.m_messagesQueued.empty(); ++Packet) {
				Connection->SNP_TokenBucket_Accumulate(Now += 1000);
				Passed = Connection->m_pTransport->SendDataPacket(Now) && Passed;
			}
			Passed = State.m_messagesQueued.empty() && Passed;
			for (auto &Flight : State.m_mapInFlightPacketsByPktNum)
				Connection->SNP_QueueReliableSegmentsForRetry(Flight.second, Flight.first, "Thirty-one retained arrange");
			State.GargantuanDataArbitrationEnabled = true;
			std::uint64_t Cutoff = 0;
			(void)Connection->GargantuanPlanData(Now, nullptr, Cutoff);
		}
		if (Passed) {
			Passed = GargantuanBeginReliableSubmissionMetadata(false, 0, 0, 0, true, GargantuanReliableServiceClock(), 6);
			Passed = Api->SendMessageToConnection(Senders[31], Ordinary.data(), int(Ordinary.size()),
				k_nSteamNetworkingSend_ReliableNoNagle, nullptr) == k_EResultOK && Passed;
			GargantuanEndReliableRetirementAttribution();
		}
		const auto Started = Now;
		SteamNetworkingMicroseconds Completed = 0;
		unsigned Serviced = 0;
		for (unsigned Opportunity = 0; Passed && Opportunity < 512 && (!Completed || Serviced < 31); ++Opportunity) {
			Now += 65; // Declared deterministic actor opportunity, not measured OS latency.
			Serviced = 0;
			for (unsigned Flow = 0; Flow < Senders.size(); ++Flow) {
				SteamNetworkingGlobalLock Global("DataArbitrationThirtyOneSelect");
				ConnectionScopeLock Lock;
				auto *Connection = GetConnectionByHandle(Senders[Flow], Lock);
				if (!Connection) { Passed = false; break; }
				auto &State = Connection->m_senderState;
				std::uint64_t Cutoff = 0;
				const auto Selected = Connection->GargantuanPlanData(Now, nullptr, Cutoff);
				if (Selected != GargantuanDataClass::None) {
					const auto UniqueBefore = State.GargantuanFeedback.UniqueReliableStreamBytesFirstSent;
					const auto RetryBefore = State.GargantuanFeedback.ReliableStreamBytesRetransmitted;
					Connection->SNP_TokenBucket_Accumulate(Now);
					Passed = Connection->m_pTransport->SendDataPacket(Now) && Passed;
					if (Flow < 31) {
						Passed = Selected == GargantuanDataClass::Retained &&
							State.GargantuanFeedback.UniqueReliableStreamBytesFirstSent == UniqueBefore &&
							State.GargantuanFeedback.ReliableStreamBytesRetransmitted > RetryBefore && Passed;
						++RetryPackets[Flow];
						for (auto &Flight : State.m_mapInFlightPacketsByPktNum)
							Connection->SNP_QueueReliableSegmentsForRetry(Flight.second, Flight.first, "Continuous thirty-one retries");
					} else {
						Passed = Selected == GargantuanDataClass::Protected &&
							State.GargantuanFeedback.ReliableStreamBytesRetransmitted == RetryBefore && Passed;
						if (!State.GargantuanProtectedOrdinary.LastPendingMessage() && !Completed) Completed = Now;
					}
				}
				Passed = !State.GargantuanFeedback.Invalid && Passed;
				if (Flow < 31 && RetryPackets[Flow]) ++Serviced;
			}
		}
		const auto Completion = Completed ? 37324 + Completed - Started : 0;
		Passed = Completed && Completion <= 50000 && Serviced == 31 && Passed;
		for (unsigned Flow = 0; Flow < Senders.size(); ++Flow) {
			if (Senders[Flow]) Api->CloseConnection(Senders[Flow], 0, "Thirty-one fixture complete", false);
			if (Receivers[Flow]) Api->CloseConnection(Receivers[Flow], 0, "Thirty-one fixture complete", false);
		}
		std::fprintf(stderr, "[Gns:DataArbitration] NativeSelectorThirtyOneRetries=%s ordinary_with_prefix_us=%lld retry_peers_serviced=%u synthetic_actor_opportunities=1 physical_f1_qualified=0\n",
			Passed ? "PASS" : "FAIL", static_cast<long long>(Completion), Serviced);
		return Passed;
	}
	static bool Run() {
		auto *Api = SteamNetworkingSockets();
		HSteamNetConnection Sender = 0, Receiver = 0;
		if (!Api->CreateSocketPair(&Sender, &Receiver, true, nullptr, nullptr)) return false;
		std::vector<unsigned char> Retry(4000, 0x31), Ordinary(10240, 0x57), Control(10240, 0x68), Structural(4000, 0x79);
		bool Passed = Api->SendMessageToConnection(Sender, Retry.data(), int(Retry.size()),
			k_nSteamNetworkingSend_ReliableNoNagle, nullptr) == k_EResultOK;
		std::fprintf(stderr, "[Gns:DataArbitration] setup sender=%u receiver=%u submitted=%d\n", Sender, Receiver, Passed);
		unsigned ProtectedPackets = 0, RetryPackets = 0, ConsecutiveProtected = 0;
		{
			SteamNetworkingGlobalLock Global("DataArbitrationFixture");
			ConnectionScopeLock Lock;
			auto *Connection = GetConnectionByHandle(Sender, Lock);
			if (!Connection) Passed = false;
			else {
				auto &State = Connection->m_senderState;
				Connection->m_sendRateData.m_flCurrentSendRateUsed = 18.0f * 1024 * 1024;
				auto Now = SteamNetworkingSockets_GetLocalTimestamp();
				for (unsigned Packet = 0; Packet < 8 && !State.m_messagesQueued.empty(); ++Packet) {
					Connection->SNP_TokenBucket_Accumulate(Now += 1000);
					Passed = Connection->m_pTransport->SendDataPacket(Now) && Passed;
				}
				Passed = State.m_messagesQueued.empty() && State.GargantuanFeedback.UniqueReliableStreamBytesFirstSent >= Retry.size() && Passed;
				std::fprintf(stderr, "[Gns:DataArbitration] setup queued_empty=%d unique=%llu invalid=%d\n", State.m_messagesQueued.empty(),
					static_cast<unsigned long long>(State.GargantuanFeedback.UniqueReliableStreamBytesFirstSent), State.GargantuanFeedback.Invalid);
				for (auto &Packet : State.m_mapInFlightPacketsByPktNum)
					Connection->SNP_QueueReliableSegmentsForRetry(Packet.second, Packet.first, "Arbitration fixture NACK");
				State.GargantuanDataArbitrationEnabled = true;
				const auto Tokens = Connection->m_sendRateData.m_flTokenBucket;
				Connection->m_sendRateData.m_flTokenBucket = -1;
				auto &ProBudget = GargantuanProtectedWirePacer(1300);
				auto &RetryBudget = GargantuanRetainedWirePacer(1300);
				(void)ProBudget.EligibleAt(Now, Sender); (void)RetryBudget.EligibleAt(Now, Sender);
				std::uint64_t Cutoff = 0; SteamNetworkingMicroseconds Eligible = -1;
				const auto Other = Sender ^ 1u;
				Passed = Connection->GargantuanPlanData(Now, nullptr, Cutoff, &Eligible) == GargantuanDataClass::None &&
					Eligible == 0 && ProBudget.EligibleAt(Now, Other) <= Now && RetryBudget.EligibleAt(Now, Other) <= Now && Passed;
				ProBudget.Forget(Other); RetryBudget.Forget(Other);
				Connection->m_sendRateData.m_flTokenBucket = Tokens;
			}
		}
		Passed = Api->ConfigureConnectionLanes(Sender, 2, nullptr, nullptr) == k_EResultInvalidParam && Passed;
		const auto Accepted = GargantuanReliableServiceClock();
		Passed = GargantuanBeginReliableSubmissionMetadata(false, 0, 0, 0, true, Accepted, 6) && Passed;
		Passed = Api->SendMessageToConnection(Sender, Ordinary.data(), int(Ordinary.size()),
			k_nSteamNetworkingSend_ReliableNoNagle, nullptr) == k_EResultOK && Passed;
		GargantuanEndReliableRetirementAttribution();
		Passed = GargantuanBeginReliableSubmissionMetadata(false, 0, 0, 0, true, Accepted, 4) && Passed;
		Passed = Api->SendMessageToConnection(Sender, Control.data(), int(Control.size()),
			k_nSteamNetworkingSend_ReliableNoNagle, nullptr) == k_EResultOK && Passed;
		GargantuanEndReliableRetirementAttribution();
		{
			SteamNetworkingGlobalLock Global("DataArbitrationFixture");
			ConnectionScopeLock Lock;
			auto *Connection = GetConnectionByHandle(Sender, Lock);
			if (!Connection) Passed = false;
			else {
				auto &State = Connection->m_senderState;
				auto Now = SteamNetworkingSockets_GetLocalTimestamp() + 10000;
				std::fprintf(stderr, "[Gns:DataArbitration] enrolled cutoff=%llu queued_empty=%d invalid=%d protected_invalid=%d\n",
					static_cast<unsigned long long>(State.GargantuanProtectedOrdinary.LastPendingMessage()), State.m_messagesQueued.empty(),
					State.GargantuanFeedback.Invalid, State.GargantuanProtectedOrdinary.EvidenceInvalid);
				for (unsigned Packet = 0; Packet < 80 && State.GargantuanProtectedOrdinary.LastPendingMessage(); ++Packet) {
					const auto BeforeUnique = State.GargantuanFeedback.UniqueReliableStreamBytesFirstSent;
					const auto BeforeRetry = State.GargantuanFeedback.ReliableStreamBytesRetransmitted;
					Connection->SNP_TokenBucket_Accumulate(Now += 1000);
					Passed = Connection->m_pTransport->SendDataPacket(Now) && Passed;
					const bool Unique = State.GargantuanFeedback.UniqueReliableStreamBytesFirstSent > BeforeUnique;
					const bool Retried = State.GargantuanFeedback.ReliableStreamBytesRetransmitted > BeforeRetry;
					Passed = !(Unique && Retried) && Passed; // Native packet class boundary.
					if (Unique) { ++ProtectedPackets; ++ConsecutiveProtected; }
					if (Retried) { ++RetryPackets; ConsecutiveProtected = 0; }
					Passed = ConsecutiveProtected <= GargantuanProtectedPacketQuantum && Passed;
					// Keep positive retry demand continuously ready, using the pinned
					// NACK/refcount path rather than reconstructing segment ownership.
					for (auto &Flight : State.m_mapInFlightPacketsByPktNum)
						Connection->SNP_QueueReliableSegmentsForRetry(Flight.second, Flight.first, "Continuous fixture NACK");
				}
				Passed = !State.GargantuanProtectedOrdinary.LastPendingMessage() &&
					ProtectedPackets > 5 && RetryPackets > 0 && !State.GargantuanFeedback.Invalid && Passed;
				std::fprintf(stderr, "[Gns:DataArbitration] ordinary_stage passed=%d cutoff=%llu invalid=%d\n", Passed,
					static_cast<unsigned long long>(State.GargantuanProtectedOrdinary.LastPendingMessage()), State.GargantuanFeedback.Invalid);
			}
		}
		Passed = GargantuanBeginReliableRetirementAttribution(999, GargantuanReliableServiceClock()) && Passed;
		Passed = Api->SendMessageToConnection(Sender, Structural.data(), int(Structural.size()),
			k_nSteamNetworkingSend_ReliableNoNagle, nullptr) == k_EResultOK && Passed;
		GargantuanEndReliableRetirementAttribution();
		{
			SteamNetworkingGlobalLock Global("DataArbitrationStructuralFixture");
			ConnectionScopeLock Lock;
			auto *Connection = GetConnectionByHandle(Sender, Lock);
			if (!Connection) Passed = false;
			else {
				auto &State = Connection->m_senderState;
				const auto BeforeRetry = State.GargantuanFeedback.ReliableStreamBytesRetransmitted;
				auto Now = SteamNetworkingSockets_GetLocalTimestamp() + 100000;
				for (unsigned Packet = 0; Packet < 8 && State.GargantuanFeedback.StructuralLastCompletedGrantToken != 999; ++Packet) {
					Connection->SNP_TokenBucket_Accumulate(Now += 1000);
					Passed = Connection->m_pTransport->SendDataPacket(Now) && Passed;
				}
				Passed = State.GargantuanFeedback.StructuralLastCompletedGrantToken == 999 &&
					State.GargantuanFeedback.StructuralLastCompletedGrantBytes == Structural.size() &&
					State.GargantuanFeedback.ReliableStreamBytesRetransmitted == BeforeRetry &&
					!State.m_listReadyRetryReliableRange.IsEmpty() && !State.GargantuanFeedback.Invalid && Passed;
				std::fprintf(stderr, "[Gns:DataArbitration] structural_stage passed=%d bytes=%llu retry_delta=%llu invalid=%d\n", Passed,
					static_cast<unsigned long long>(State.GargantuanFeedback.StructuralLastCompletedGrantBytes),
					static_cast<unsigned long long>(State.GargantuanFeedback.ReliableStreamBytesRetransmitted - BeforeRetry), State.GargantuanFeedback.Invalid);
			}
		}
		// Drain actual UDP packets, then require application FIFO and byte equality.
		unsigned Delivered = 0;
		for (unsigned Pass = 0; Pass < 100 && Delivered < 4; ++Pass) {
			SteamNetworkingSockets_Poll(0);
			SteamNetworkingMessage_t *Message = nullptr;
			while (Api->ReceiveMessagesOnConnection(Receiver, &Message, 1) == 1) {
				const auto &Expected = Delivered == 0 ? Retry : (Delivered == 1 ? Ordinary : (Delivered == 2 ? Control : Structural));
				Passed = Delivered < 4 && Message->m_cbSize == int(Expected.size()) &&
					std::memcmp(Message->m_pData, Expected.data(), Expected.size()) == 0 && Passed;
				++Delivered; Message->Release();
			}
		}
		Passed = Delivered == 4 && Passed;
		Api->CloseConnection(Sender, 0, "Arbitration fixture complete", false);
		Api->CloseConnection(Receiver, 0, "Arbitration fixture complete", false);
		std::fprintf(stderr, "[Gns:DataArbitration] NativeSelectorContinuousRetryFIFO=%s protected_packets=%u retry_packets=%u delivered=%u\n",
			Passed ? "PASS" : "FAIL", ProtectedPackets, RetryPackets, Delivered);
		return Passed;
	}
};
}
