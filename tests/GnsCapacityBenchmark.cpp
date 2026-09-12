// Backend-only discriminator, not a GameSession/GRPL correctness fixture.
// Same pinned GNS, flags, IP loopback and message-size envelope as the adapter.
// Rate overrides exist only in this executable, never in the production adapter.
#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
std::int64_t Now() { return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count(); }
HSteamNetConnection Server = 0, Client = 0;
bool ServerReady = false, ClientReady = false, AcceptFailed = false;
void Status(SteamNetConnectionStatusChangedCallback_t *Value) {
	if (Value->m_info.m_eState == k_ESteamNetworkingConnectionState_Connecting && Value->m_info.m_hListenSocket) {
		Server = Value->m_hConn;
		AcceptFailed = SteamNetworkingSockets()->AcceptConnection(Server) != k_EResultOK;
	}
	if (Value->m_info.m_eState == k_ESteamNetworkingConnectionState_Connected) {
		if (Value->m_hConn == Server) ServerReady = true;
		if (Value->m_hConn == Client) ClientReady = true;
	}
}
struct Probe { std::uint64_t Kind, Id; std::int64_t Sent; };
struct Case {
	const char *Name;
	std::size_t Chunk = 470000, Total = 1450000;
	int Rate = 0, Offered = 0;
	bool Rpc = true, Before = false;
};
double Percentile(std::vector<double> Values, double Fraction) {
	if (Values.empty()) return -1;
	std::sort(Values.begin(), Values.end());
	return Values[static_cast<std::size_t>((Values.size() - 1) * Fraction)];
}
void Run(Case Work) {
	Server = Client = 0; ServerReady = ClientReady = AcceptFailed = false;
	std::array<SteamNetworkingConfigValue_t, 6> Options;
	Options[0].SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, reinterpret_cast<void *>(Status));
	Options[1].SetInt32(k_ESteamNetworkingConfig_SendBufferSize, 8 * 1024 * 1024);
	Options[2].SetInt32(k_ESteamNetworkingConfig_RecvBufferSize, 8 * 1024 * 1024);
	Options[3].SetInt32(k_ESteamNetworkingConfig_RecvMaxMessageSize, 512 * 1024);
	Options[4].SetInt32(k_ESteamNetworkingConfig_SendRateMin, Work.Rate);
	Options[5].SetInt32(k_ESteamNetworkingConfig_SendRateMax, Work.Rate);
	const int OptionCount = Work.Rate ? 6 : 4;
	SteamNetworkingIPAddr Address;
	Address.Clear(); Address.ParseString("127.0.0.1");
	HSteamListenSocket Listener = 0;
	for (std::uint16_t Port = 39500; Port < 39600 && !Listener; ++Port) {
		Address.m_port = Port;
		Listener = SteamNetworkingSockets()->CreateListenSocketIP(Address, OptionCount, Options.data());
	}
	if (!Listener) throw std::runtime_error("listen");
	Client = SteamNetworkingSockets()->ConnectByIPAddress(Address, OptionCount, Options.data());
	const auto ConnectDeadline = Clock::now() + 5s;
	while ((!ClientReady || !ServerReady) && Clock::now() < ConnectDeadline) {
		SteamNetworkingSockets()->RunCallbacks(); std::this_thread::sleep_for(1ms);
	}
	if (AcceptFailed || !ClientReady || !ServerReady) throw std::runtime_error("connect/accept");
	const auto Start = Now();
	std::uint64_t Submitted = 0, Delivered = 0, Messages = 0, Received = 0;
	std::uint64_t Requests = 0, Responses = 0, UnreliableSent = 0, UnreliableReceived = 0;
	std::int64_t LastDelivery = Start, LastResponse = Start, MaxResponseGap = 0, LastSample = 0;
	std::int64_t LastRequest = 0, LastUnreliable = 0, MaxQueue = 0, PeakPending = 0, EndOfferPending = 0;
	std::vector<double> Rtt, ReceiveAge, UnreliableLatency;
	Rtt.reserve(128); ReceiveAge.reserve(8192); UnreliableLatency.reserve(512);
	auto Send = [&](HSteamNetConnection Connection, std::uint64_t Kind, std::uint64_t Id,
		std::size_t Bytes, int Flags, std::int64_t Sent = 0) {
		std::vector<std::byte> Payload(Bytes);
		const Probe Header{Kind, Id, Sent ? Sent : Now()};
		std::memcpy(Payload.data(), &Header, sizeof(Header));
		if (SteamNetworkingSockets()->SendMessageToConnection(Connection, Payload.data(),
			static_cast<std::uint32_t>(Payload.size()), Flags, nullptr) != k_EResultOK) throw std::runtime_error("send");
		if (Connection == Server && Flags == k_nSteamNetworkingSend_Reliable) { Submitted += Bytes; ++Messages; }
	};
	auto Drain = [&](HSteamNetConnection Connection) {
		for (std::size_t I = 0; I < 256; ++I) {
			SteamNetworkingMessage_t *Message = nullptr;
			const auto Count = SteamNetworkingSockets()->ReceiveMessagesOnConnection(Connection, &Message, 1);
			if (!Count) break;
			if (Count < 0 || !Message) throw std::runtime_error("receive");
			Probe Header{};
			if (Message->m_cbSize < sizeof(Header)) throw std::runtime_error("short probe");
			std::memcpy(&Header, Message->m_pData, sizeof(Header));
			const auto Time = Now();
			if (ReceiveAge.size() < 8192) ReceiveAge.push_back(
				(SteamNetworkingUtils()->GetLocalTimestamp() - Message->m_usecTimeReceived) / 1000.0);
			if (Connection == Server && Header.Kind == 2)
				Send(Server, 3, Header.Id, 115, k_nSteamNetworkingSend_Reliable, Header.Sent);
			if (Connection == Client && (Message->m_nFlags & k_nSteamNetworkingSend_Reliable)) {
				Delivered += Message->m_cbSize; ++Received; LastDelivery = Time;
				if (Header.Kind == 3) {
					++Responses;
					if (Rtt.size() < 128) Rtt.push_back((Time - Header.Sent) / 1000.0);
					MaxResponseGap = std::max(MaxResponseGap, Time - LastResponse); LastResponse = Time;
				}
			}
			if (Connection == Client && Header.Kind == 4) {
				++UnreliableReceived;
				if (UnreliableLatency.size() < 512) UnreliableLatency.push_back((Time - Header.Sent) / 1000.0);
			}
			Message->Release();
		}
	};
	if (Work.Rpc && Work.Before) {
		Send(Client, 2, ++Requests, 115, k_nSteamNetworkingSend_Reliable);
		// Wait only until the response has been submitted. It may still be in GNS.
		while (Messages == 0 && Now() - Start < 1000000) {
			SteamNetworkingSockets()->RunCallbacks(); Drain(Server); std::this_thread::sleep_for(1ms);
		}
		if (!Messages) throw std::runtime_error("early request");
	}
	std::size_t StructuralSubmitted = 0;
	const auto OfferEnd = Start + (Work.Offered ? 6000000 : Work.Total ? 0 : 2000000);
	const auto Deadline = Start + 25000000;
	while (Now() < Deadline) {
		const auto Time = Now();
		SteamNetworkingSockets()->RunCallbacks();
		const auto Allowed = Work.Offered
			? std::min<std::size_t>(Work.Total, static_cast<std::size_t>((Time - Start) * Work.Offered / 1000000)) : Work.Total;
		while (StructuralSubmitted < Allowed) {
			const auto Size = std::min(Work.Chunk, Work.Total - StructuralSubmitted);
			if (Work.Offered && Size > Allowed - StructuralSubmitted) break;
			Send(Server, 1, StructuralSubmitted, Size, k_nSteamNetworkingSend_Reliable);
			StructuralSubmitted += Size;
		}
		const unsigned RequestLimit = Work.Offered ? 60u : Work.Total ? 1u : 20u;
		if (Work.Rpc && Requests < RequestLimit && Time - LastRequest >= 100000) {
			Send(Client, 2, ++Requests, 115, k_nSteamNetworkingSend_Reliable); LastRequest = Time;
		}
		if (Time - LastUnreliable >= 100000 && Time - Start < 8000000) {
			Send(Server, 4, ++UnreliableSent, 115, k_nSteamNetworkingSend_Unreliable); LastUnreliable = Time;
		}
		Drain(Server); Drain(Client);
		SteamNetConnectionRealTimeStatus_t Status{};
		if (SteamNetworkingSockets()->GetConnectionRealTimeStatus(Server, &Status, 0, nullptr) != k_EResultOK)
			throw std::runtime_error("status");
		PeakPending = std::max<std::int64_t>(PeakPending, Status.m_cbPendingReliable);
		MaxQueue = std::max(MaxQueue, Status.m_usecQueueTime);
		if (Time < OfferEnd) EndOfferPending = Status.m_cbPendingReliable;
		if (Time - LastSample >= 100000) {
			std::cout << "[Network:CapacitySample] case=" << Work.Name << " us=" << Time - Start
				<< " submitted=" << Submitted << " delivered=" << Delivered << " pending=" << Status.m_cbPendingReliable
				<< " unacked=" << Status.m_cbSentUnackedReliable << " rate=" << Status.m_nSendRateBytesPerSecond
				<< " queueUs=" << Status.m_usecQueueTime << " outBps=" << Status.m_flOutBytesPerSec << '\n';
			LastSample = Time;
		}
		if (Time >= OfferEnd && Submitted == Delivered && Requests == Responses && Status.m_cbPendingReliable == 0) break;
		std::this_thread::sleep_for(1ms);
	}
	std::cout << "[Network:CapacityResult] case=" << Work.Name << " overrideRate=" << Work.Rate
		<< " chunk=" << Work.Chunk << " offered=" << Work.Offered << " submitted=" << Submitted << " delivered=" << Delivered
		<< " messages=" << Messages << " received=" << Received << " seconds=" << (LastDelivery - Start) / 1000000.0
		<< " effectiveBps=" << Delivered * 1000000.0 / std::max<std::int64_t>(1, LastDelivery - Start)
		<< " pendingHigh=" << PeakPending << " offerEndPending=" << EndOfferPending << " queueUsHigh=" << MaxQueue
		<< " requests=" << Requests << " responses=" << Responses << " rpcP50Ms=" << Percentile(Rtt, .5)
		<< " rpcP99Ms=" << Percentile(Rtt, .99) << " rpcMaxMs=" << Percentile(Rtt, 1)
		<< " responseGapMaxMs=" << MaxResponseGap / 1000.0 << " receiveAgeMaxMs=" << Percentile(ReceiveAge, 1)
		<< " unreliableSent=" << UnreliableSent << " unreliableReceived=" << UnreliableReceived
		<< " unreliableMaxMs=" << Percentile(UnreliableLatency, 1) << '\n';
	if (Submitted != Delivered || Messages != Received || Requests != Responses) throw std::runtime_error("did not drain");
	SteamNetworkingSockets()->CloseConnection(Client, 0, "capacity complete", false);
	SteamNetworkingSockets()->CloseConnection(Server, 0, "capacity complete", false);
	SteamNetworkingSockets()->CloseListenSocket(Listener);
}
}
int main() {
	SteamNetworkingErrMsg Error{};
	if (!GameNetworkingSockets_Init(nullptr, Error)) return 2;
	try {
		Run({.Name = "gameplay", .Total = 0});
		Run({.Name = "structure", .Rpc = false});
		Run({.Name = "burst-then-rpc"});
		Run({.Name = "rpc-then-burst", .Before = true});
		Run({.Name = "small-messages", .Chunk = 16000});
		Run({.Name = "rate-1m", .Rate = 1024 * 1024});
		Run({.Name = "rate-4m", .Rate = 4 * 1024 * 1024});
		for (const int Rate : {128 * 1024, 256 * 1024, 512 * 1024})
			Run({.Name = Rate == 128 * 1024 ? "paced-128k" : Rate == 256 * 1024 ? "paced-256k" : "paced-512k",
				.Chunk = 16384, .Total = static_cast<std::size_t>(Rate) * 6, .Offered = Rate});
	} catch (const std::exception &Error) {
		std::cerr << "[Network:CapacityFailure] " << Error.what() << '\n'; GameNetworkingSockets_Kill(); return 1;
	}
	GameNetworkingSockets_Kill();
}
