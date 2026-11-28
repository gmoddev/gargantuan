// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "StandalonePlayer.hpp"
#include "PrivateServerChannel.hpp"
#include "packaging/PrivateServerSnapshot.hpp"
#include "gargantuan/filesystem/Paths.hpp"
#include "gargantuan/packaging/PackageBuilder.hpp"
#include "gargantuan/runtime/ApplicationSession.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cstring>

namespace gargantuan::host {
	// Configuration is admitted once by the native launcher. The creator can
	// select private play but cannot provide a program, arguments or a socket.
	class PrivateServerProcess final {
	  public:
		PrivateServerProcess(std::shared_ptr<PrivateServerSnapshot> Snapshot, const PrivateServerPackage &Server, const std::filesystem::path &Profile, const WorldSlot *Slot)
			: Snapshot(std::move(Snapshot)) {
			const auto Executable = ExecutablePath(this->Snapshot->Root);
			Endpoint = {"127.0.0.1", static_cast<std::uint16_t>(49152 + SDL_rand(16384))};
			const std::string Address = Endpoint.Host + ":" + std::to_string(Endpoint.Port);
			const auto Path = Paths::ToUtf8(Executable);
			std::vector<const char *> Arguments{Path.c_str(), "--owned-session"};
			const auto Directory = Paths::ToUtf8(Profile);
			if (Slot) Arguments.insert(Arguments.end(), {"--owned-world", Directory.c_str(), Slot->SaveId.c_str(),
				Slot->WorldId.c_str(), Slot->Name.c_str(), Slot->Seed.c_str(), Slot->Baseline.c_str()});
			Arguments.insert(Arguments.end(), {"--bind", Address.c_str(), "--expected-manifest-sha256", Server.Manifest.c_str(), nullptr});
			Channel.Start(Executable, Arguments);
			Deadline = Clock::now() + std::chrono::seconds(10);
		}
		PrivateServerProcess(const PrivateServerProcess &) = delete;
		PrivateServerProcess &operator=(const PrivateServerProcess &) = delete;
		std::optional<PlayerAdmission> PollListening() {
			try {
				if (!Read(Greeting.data(), Greeting.size())) return {};
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
			if (std::memcmp(Greeting.data(), "GPC1", 4) != 0) throw SessionAdmissionFailure::NotAuthorized;
			PlayerAdmission Grant{Endpoint, {}, network::TransportServerPublicKey{}};
			std::copy_n(Greeting.begin() + 4, 32, Grant.ServerKey->begin());
			std::copy_n(Greeting.begin() + 36, 32, Grant.Proof.begin());
			Greeting.fill({});
			Send('A');
			return Grant;
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
			if (!Closing) {
				if (!PreparingLeave) {
					Send('D');
					Deadline = Clock::now() + std::chrono::seconds(6);
					PreparingLeave = true;
				}
				std::byte Result{};
				if (!Read(&Result, 1)) return false;
				PreparingLeave = false;
				if (Result == std::byte{'U'}) throw SessionAdmissionFailure::UnknownOutcome;
				if (Result == std::byte{'F'}) throw SessionAdmissionFailure::SaveFailed;
				if (Result != std::byte{'S'}) throw SessionAdmissionFailure::Disconnected;
			}
			return Close();
		}
		bool Close() {
			if (!Closing) {
				Send('X');
				Closing = true;
				Deadline = Clock::now() + std::chrono::seconds(6);
			}
			int Code = -1;
			if (Channel.Exited(&Code)) {
				if (Code != 0) throw SessionAdmissionFailure::SaveFailed;
				return true;
			}
			if (Clock::now() >= Deadline) throw SessionAdmissionFailure::UnknownOutcome;
			return false;
		}
		[[nodiscard]] bool IsClosing() const { return Closing; }
		[[nodiscard]] bool Exited() const { return Channel.Exited(); }
	  private:
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
		bool Read(std::byte *Data, std::size_t Size) {
			int Code = 0;
			if (Channel.Exited(&Code)) {
				if (Code == 27) throw WorldInUse{};
				if (Code == 28) throw SessionAdmissionFailure::Incompatible;
				throw SessionAdmissionFailure::Disconnected;
			}
			if (Clock::now() >= Deadline) throw SessionAdmissionFailure::TimedOut;
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
		PrivateServerChannel Channel;
		network::TransportEndpoint Endpoint;
		std::array<std::byte, 68> Greeting{};
		std::array<std::byte, 9> Receipt{};
		std::array<std::byte, 68> InputFrame{};
		std::size_t Offset = 0, ExpectedSize = 0;
		std::uint64_t Sequence = 0, PendingHold = 0;
		Clock::time_point Deadline;
		bool PreparingLeave = false, Closing = false;
	};
}
