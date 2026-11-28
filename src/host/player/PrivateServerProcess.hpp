// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "StandalonePlayer.hpp"
#include "PrivateServerChannel.hpp"
#include "host/common/LocalGuestGrant.hpp"
#include "packaging/PrivateServerSnapshot.hpp"
#include "gargantuan/filesystem/Paths.hpp"
#include "gargantuan/filesystem/ProjectIdentity.hpp"
#include "serialization/JsonCodec.hpp"
#include "gargantuan/packaging/PackageBuilder.hpp"
#include "gargantuan/runtime/ApplicationSession.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>

namespace gargantuan::host {
	// Configuration is admitted once by the native launcher. The creator can
	// select private play but cannot provide a program, arguments or a socket.
	class PrivateServerProcess final {
	  public:
		PrivateServerProcess(std::shared_ptr<PrivateServerSnapshot> Snapshot, const PrivateServerPackage &Server,
			const std::filesystem::path &Profile, const WorldSlot *Slot, unsigned Capacity)
			: Snapshot(std::move(Snapshot)), Seats(Capacity ? Capacity : 1) {
			if (Seats > 8 || (Capacity && (!Server.Listener || !Server.Invitations))) throw SessionAdmissionFailure::UnsupportedTopology;
			const auto Executable = ExecutablePath(this->Snapshot->Root);
			Endpoint = Capacity ? *Server.Listener : network::TransportEndpoint{"127.0.0.1", static_cast<std::uint16_t>(49152 + SDL_rand(16384))};
			const auto Address = AddressText();
			const auto Path = Paths::ToUtf8(Executable);
			const auto Count = std::to_string(Seats);
			std::vector<const char *> Arguments{Path.c_str(), Capacity ? "--owned-multiplayer-session" : "--owned-session"};
			if (Capacity) Arguments.push_back(Count.c_str());
			const auto Directory = Paths::ToUtf8(Profile);
			if (Slot) Arguments.insert(Arguments.end(), {"--owned-world", Directory.c_str(), Slot->SaveId.c_str(),
				Slot->WorldId.c_str(), Slot->Name.c_str(), Slot->Seed.c_str(), Slot->Baseline.c_str()});
			Arguments.insert(Arguments.end(), {"--bind", Address.c_str(), "--expected-manifest-sha256", Server.Manifest.c_str(), nullptr});
			Channel.Start(Executable, Arguments, Server.RuntimeEnrollment);
			Deadline = Clock::now() + std::chrono::seconds(10);
		}
		PrivateServerProcess(const PrivateServerProcess &) = delete;
		~PrivateServerProcess() { WithdrawListing(); }
		PrivateServerProcess &operator=(const PrivateServerProcess &) = delete;
		std::optional<PlayerAdmission> PollListening() {
			try {
				if (!Read(Greeting.data(), 38 + Seats * 32)) return {};
			} catch (SessionAdmissionFailure Failure) {
				// Pipe EOF can precede the OS process-exit notification. Read the
				// actual startup result before classifying a writer-lock conflict.
				if (Failure != SessionAdmissionFailure::Disconnected) throw;
				int Code = 0;
				if (!Channel.Exited(&Code) && Clock::now() < Deadline) return {};
				if (Code == 27) throw WorldInUse{};
				if (Code == 28) throw SessionAdmissionFailure::Incompatible;
				throw;
			}
			Endpoint.Port = std::to_integer<std::uint16_t>(Greeting[4]) |
				(std::to_integer<std::uint16_t>(Greeting[5]) << 8);
			if (!Endpoint.Port || std::memcmp(Greeting.data(), Seats == 1 ? "GPC2" : "GPH2", 4) != 0) throw SessionAdmissionFailure::NotAuthorized;
			PlayerAdmission Grant{Endpoint, {}, network::TransportServerPublicKey{}};
			std::copy_n(Greeting.begin() + 6, 32, Grant.ServerKey->begin());
			std::copy_n(Greeting.begin() + 38, 32, Grant.Proof.begin());
			std::fill_n(Greeting.begin() + 38, 32, std::byte{});
			Send('A');
			Deadline = Clock::now() + std::chrono::seconds(10);
			return Grant;
		}
		bool OwnerReady() {
			SharedHostFrame Message{};
			if (!Read(reinterpret_cast<std::byte *>(Message.data()), Message.size())) return false;
			if (Message[0] != 'J') throw SessionAdmissionFailure::Disconnected;
			return true;
		}
	  private:
		static std::string Hex(std::span<const std::byte> Bytes) {
			constexpr char Digits[] = "0123456789abcdef";
			std::string Result;
			Result.reserve(Bytes.size() * 2);
			for (const auto Byte : Bytes) {
				const auto Value = std::to_integer<unsigned>(Byte);
				Result += Digits[Value >> 4];
				Result += Digits[Value & 15];
			}
			return Result;
		}
	  public:
		void PublishGuests(const HostInvitationChannel &Launcher, const std::string &Project, const std::string &Manifest) {
			const auto Milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(Expires - Clock::now()).count();
			if (Milliseconds <= 0) throw SessionAdmissionFailure::TimedOut;
			const auto Expiry = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() + Milliseconds;
			auto Guests = JsonCodec::Json::array();
			for (unsigned Seat = 1; Seat < Seats; ++Seat)
				Guests.push_back(Hex(std::span(Greeting).subspan(38 + Seat * 32, 32)));
			ListingId = ProjectId::New().ToString();
			Withdrawal = JsonCodec::Json{{"Version", 1}, {"Action", "Withdraw"}, {"Source", "LAN"}, {"SessionID", ListingId}}.dump();
			const auto Frame = JsonCodec::Json{{"Version", 1}, {"Action", "Publish"}, {"Source", "LAN"}, {"SessionID", ListingId},
				{"ProjectID", Project}, {"PackageSHA256", Manifest}, {"Endpoint", AddressText()}, {"ExpiresUnixMillis", Expiry},
				{"ServerPublicKeyHex", Hex(std::span(Greeting).subspan(6, 32))}, {"GuestProofs", std::move(Guests)}}.dump();
			Greeting.fill({});
			if (!Launcher.Publish(Frame)) throw SessionAdmissionFailure::Unavailable;
			ListingChannel = Launcher;
		}
		void ServiceGuestInvitations() {
			if (Seats == 1 || !ListingChannel || PreparingLeave || Closing) return;
			const auto Now = static_cast<std::uint64_t>(InvitationTimeMillis());
			for (auto &Update : Undelivered) {
				if (!Update.Frame.empty() && (Update.Expiry <= Now || ListingChannel->Publish(Update.Frame)))
					Update = {};
			}
			for (unsigned I = 1; I < Seats; ++I) {
				SharedHostFrame Frame{};
				if (!Read(reinterpret_cast<std::byte *>(Frame.data()), Frame.size(), false)) return;
				if (Frame[0] != 'G' || !Frame[1] || Frame[1] >= Seats) throw SessionAdmissionFailure::Disconnected;
				std::uint64_t Expiry = 0;
				for (unsigned K = 0; K < 8; ++K) Expiry |= std::uint64_t(Frame[2 + K]) << (8 * K);
				if (Expiry <= Now) continue;
				auto Update = JsonCodec::Json{{"Version", 1}, {"Action", "Refresh"}, {"Source", "LAN"},
					{"SessionID", ListingId}, {"Seat", Frame[1]}, {"ExpiresUnixMillis", Expiry},
					{"GuestProof", Hex(std::as_bytes(std::span(Frame)).subspan(10))}}.dump();
				auto &Pending = Undelivered[Frame[1]];
				Pending = {};
				if (!ListingChannel->Publish(Update)) Pending = {Expiry, std::move(Update)};
			}
		}
		void WithdrawListing() noexcept {
			if (ListingChannel && ListingChannel->Publish(Withdrawal)) ListingChannel.reset();
		}
		bool ApplyHold(bool Held) {
			if (!PendingHold) {
				Send(Held ? 'P' : 'R');
				PendingHold = (++Sequence << 1) | static_cast<unsigned>(Held);
				Deadline = Clock::now() + std::chrono::seconds(3);
			}
			if (!Read(Receipt.data(), Receipt.size())) return false;
			std::uint64_t Value = 0;
			for (unsigned I = 0; I < 8; ++I)
				Value |= std::uint64_t(std::to_integer<unsigned>(Receipt[I + 1])) << (8 * I);
			if (Receipt[0] != std::byte{'H'} || Value != PendingHold) throw SessionAdmissionFailure::Disconnected;
			PendingHold = 0;
			return ((Value & 1) != 0) == Held;
		}
		bool Leave() {
			WithdrawListing();
			if (!Closing) {
				if (!PreparingLeave) {
					Send('D');
					std::clog << "[Lifecycle:PrivateHost] Awaiting authoritative write drain" << std::endl;
					Deadline = Clock::now() + std::chrono::seconds(6);
					PreparingLeave = true;
				}
				SharedHostFrame Frame{};
				if (!Read(reinterpret_cast<std::byte *>(Frame.data()), Seats == 1 ? 1 : Frame.size())) return false;
				if (Seats > 1 && Frame[0] == 'G') return false;
				PreparingLeave = false;
				if (Frame[0] == 'U') throw SessionAdmissionFailure::UnknownOutcome;
				if (Frame[0] == 'F') throw SessionAdmissionFailure::SaveFailed;
				if (Frame[0] != 'S') throw SessionAdmissionFailure::Disconnected;
				LeaveConfirmed = true;
			}
			return Close();
		}
		bool Close() {
			WithdrawListing();
			const auto CompleteExit = [&](int Code) {
				// Bootstrap rejected the world before it could own or write it.
				if (Code == 27 || Code == 28) return true;
				if (Code == 29) throw SessionAdmissionFailure::SaveFailed;
				if (Code != 0 || (!Closing && !LeaveConfirmed)) throw SessionAdmissionFailure::UnknownOutcome;
				return true;
			};
			int Code = -1;
			if (Channel.Exited(&Code)) return CompleteExit(Code);
			if (!Closing) {
				Send('X');
				Closing = true;
				Deadline = Clock::now() + std::chrono::seconds(6);
			}
			if (Channel.Exited(&Code)) return CompleteExit(Code);
			if (Clock::now() >= Deadline) throw SessionAdmissionFailure::UnknownOutcome;
			return false;
		}
		[[nodiscard]] bool IsClosing() const { return Closing; }
		[[nodiscard]] bool IsAwaitingLeave() const { return PreparingLeave || Closing; }
		[[nodiscard]] bool Exited() const { return Channel.Exited(); }
	  private:
		std::string AddressText() const {
			return (Endpoint.Host.find(':') == std::string::npos ? Endpoint.Host : "[" + Endpoint.Host + "]") + ":" + std::to_string(Endpoint.Port);
		}
		static std::filesystem::path ExecutablePath(const std::filesystem::path &Root) {
#ifdef _WIN32
			return Root / "GargantuanServer.exe";
#else
			return Root / "GargantuanServer";
#endif
		}
		using Clock = std::chrono::steady_clock;
		void Send(char Command) {
			Channel.Send(Command);
		}
		bool Read(std::byte *Data, std::size_t Size, bool TimeBound = true) {
			int Code = 0;
			if (Channel.Exited(&Code)) {
				if (Code == 27) throw WorldInUse{};
				if (Code == 28) throw SessionAdmissionFailure::Incompatible;
				throw SessionAdmissionFailure::Disconnected;
			}
			if (TimeBound && Clock::now() >= Deadline) throw SessionAdmissionFailure::TimedOut;
			if (ExpectedSize && Size != ExpectedSize) throw SessionAdmissionFailure::Disconnected;
			if (Size > InputFrame.size() || Offset >= Size) throw SessionAdmissionFailure::Disconnected;
			ExpectedSize = Size;
			Offset += Channel.Read(std::span(InputFrame).subspan(Offset, Size - Offset));
			if (Offset < Size) return false;
			std::copy_n(InputFrame.begin(), Size, Data);
			InputFrame.fill({});
			ExpectedSize = Offset = 0;
			return true;
		}
		std::shared_ptr<PrivateServerSnapshot> Snapshot;
		std::optional<HostInvitationChannel> ListingChannel;
		struct GuestUpdate { std::uint64_t Expiry{}; std::string Frame; };
		std::array<GuestUpdate, 8> Undelivered{};
		std::string ListingId, Withdrawal;
		PrivateServerChannel Channel;
		network::TransportEndpoint Endpoint;
		const unsigned Seats;
		const Clock::time_point Expires = Clock::now() + std::chrono::seconds(55);
		std::array<std::byte, 38 + 8 * 32> Greeting{};
		std::array<std::byte, 9> Receipt{};
		std::array<std::byte, 38 + 8 * 32> InputFrame{};
		std::size_t Offset = 0, ExpectedSize = 0;
		std::uint64_t Sequence = 0, PendingHold = 0;
		Clock::time_point Deadline;
		bool PreparingLeave = false, LeaveConfirmed = false, Closing = false;
	};
}
