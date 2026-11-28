#include "gargantuan/network/GameNetworkingSocketsTransport.hpp"
#include "gargantuan/network/GameSession.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {
	using namespace gargantuan::network;
	using namespace std::chrono_literals;

	std::string Hex(const TransportServerPublicKey &Key) {
		constexpr char Digits[] = "0123456789abcdef";
		std::string Result;
		Result.reserve(64);
		for (const auto Value : Key) {
			const auto Byte = std::to_integer<unsigned>(Value);
			Result += Digits[Byte >> 4];
			Result += Digits[Byte & 15];
		}
		return Result;
	}

	std::optional<TransportServerPublicKey> ParseKey(std::string_view Value) {
		if (Value.size() != 64) return std::nullopt;
		TransportServerPublicKey Key{};
		for (std::size_t Index = 0; Index < Key.size(); ++Index) {
			const auto Digit = [](char Character) {
				if (Character >= '0' && Character <= '9') return Character - '0';
				if (Character >= 'a' && Character <= 'f') return Character - 'a' + 10;
				return -1;
			};
			const auto Upper = Digit(Value[2 * Index]);
			const auto Lower = Digit(Value[2 * Index + 1]);
			if (Upper < 0 || Lower < 0) return std::nullopt;
			Key[Index] = static_cast<std::byte>((Upper << 4) | Lower);
		}
		return Key;
	}

	TransportStartConfiguration Configuration(TransportRole Role, std::uint16_t Port) {
		return {
			.Role = Role,
			.Endpoint = {"127.0.0.1", Port},
			.AdvertisedLimits = GameSessionConfiguration::DefaultLimits(),
		};
	}

	int Server(const std::filesystem::path &Directory) {
		std::unique_ptr<GameNetworkingSocketsTransport> Transport;
		std::uint16_t Port = 0;
		for (std::uint16_t Candidate = 39600; Candidate < 39700; ++Candidate) {
			auto CandidateTransport = std::make_unique<GameNetworkingSocketsTransport>();
			auto Start = Configuration(TransportRole::Server, Candidate);
			Start.RequireServerIdentity = true;
			if (CandidateTransport->Start(Start).Succeeded()) {
				Transport = std::move(CandidateTransport);
				Port = Candidate;
				break;
			}
		}
		if (!Transport) return 1;
		const auto Key = Transport->LocalServerPublicKey();
		if (!Port || !Key) return 1;
		{
			std::ofstream Ready(Directory / "ready.tmp", std::ios::binary | std::ios::trunc);
			if (!Ready) return 1;
			Ready << Port << '\n' << Hex(*Key) << '\n';
			if (!Ready) return 1;
		}
		std::filesystem::rename(Directory / "ready.tmp", Directory / "ready");
		std::size_t Messages = 0;
		const auto Deadline = std::chrono::steady_clock::now() + 15s;
		while (std::chrono::steady_clock::now() < Deadline &&
			!std::filesystem::exists(Directory / "stop")) {
			std::array<TransportEvent, 32> Events;
			for (std::size_t Index = 0, Count = Transport->PollEvents(Events); Index < Count; ++Index) {
				if (const auto *Message = std::get_if<ReceivedMessageEvent>(&Events[Index])) {
					if (Message->Payload == std::vector<std::byte>{std::byte{0x47}})
						++Messages;
				}
			}
			{
				std::ofstream Count(Directory / "messages", std::ios::binary | std::ios::trunc);
				if (!Count) return 1;
				Count << Messages << '\n';
			}
			std::this_thread::sleep_for(5ms);
		}
		(void)Transport->Stop({DisconnectReason::LocalShutdown, "Identity fixture stopped"});
		return std::filesystem::exists(Directory / "stop") ? 0 : 1;
	}

	int Client(std::uint16_t Port, const TransportServerPublicKey &Expected, bool ShouldConnect) {
		GameNetworkingSocketsTransport Transport;
		auto Start = Configuration(TransportRole::Client, Port);
		Start.ExpectedServerPublicKey = Expected;
		if (!Transport.Start(Start).Succeeded()) return 1;
		bool Connected = false;
		bool RejectedForIdentity = false;
		std::size_t Sent = 0;
		const auto Deadline = std::chrono::steady_clock::now() + 4s;
		while (std::chrono::steady_clock::now() < Deadline) {
			std::array<TransportEvent, 32> Events;
			for (std::size_t Index = 0, Count = Transport.PollEvents(Events); Index < Count; ++Index) {
				if (const auto *Closed = std::get_if<DisconnectedEvent>(&Events[Index]))
					RejectedForIdentity = Closed->Information.Reason == DisconnectReason::AuthenticationFailure;
				const auto *Changed = std::get_if<ConnectionStateEvent>(&Events[Index]);
				if (!Changed || Changed->Current != ConnectionState::Connected) continue;
				Connected = true;
				if (Transport.AuthenticatedServerPublicKey(Changed->Connection) != Expected) return 1;
				const auto Message = MakeNetworkMessageIntent(Changed->Connection,
					DeliveryMode::ReliableOrdered, TrafficClass::ReliableApplication, std::monostate{},
					{std::byte{0x47}}, Start.AdvertisedLimits);
				if (!Message || !Transport.Send(*Message).Succeeded()) return 1;
				++Sent;
			}
			if (Connected || RejectedForIdentity) {
				// Keep the sender alive while the real reliable frame reaches the peer.
				if (Connected) {
					for (int Retry = 0; Retry < 100; ++Retry) {
						(void)Transport.PollEvents(Events);
						std::this_thread::sleep_for(5ms);
					}
				}
				break;
			}
			std::this_thread::sleep_for(5ms);
		}
		(void)Transport.Stop({DisconnectReason::LocalShutdown, "Identity fixture stopped"});
		return Connected == ShouldConnect && (ShouldConnect || RejectedForIdentity) &&
			Sent == (ShouldConnect ? 1u : 0u) ? 0 : 1;
	}
}

int main(int Count, char **Arguments) {
#if defined(_WIN32)
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
	try {
		if (Count == 3 && std::string_view(Arguments[1]) == "server")
			return Server(Arguments[2]);
		if (Count == 5 && std::string_view(Arguments[1]) == "client") {
			const auto PortValue = std::stoi(Arguments[2]);
			const auto Key = ParseKey(Arguments[3]);
			if (PortValue < 1 || PortValue > 65535 || !Key) return 2;
			const std::string_view Mode = Arguments[4];
			if (Mode != "accept" && Mode != "reject") return 2;
			return Client(static_cast<std::uint16_t>(PortValue), *Key, Mode == "accept");
		}
	} catch (...) { return 1; }
	return 2;
}
