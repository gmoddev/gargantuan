// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "ServerHost.hpp"
#include "gargantuan/runtime/WorldCatalog.hpp"
#include "gargantuan/packaging/RuntimePackageCrypto.hpp"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <poll.h>
#include <unistd.h>
#endif

namespace gargantuan::host {
	// The parent owns the opposite ends of two inherited anonymous pipes.
	// Standard output is separated before Engine or any authored Script starts.
	class PrivateServerControl final : public IManagedServerLifecycle, public IPrivateGameplayControl, public network::IGameAdmissionProvider {
	  public:
		PrivateServerControl(std::filesystem::path Directory, std::optional<WorldSlot> Slot)
			: Profile(std::move(Directory)), Selected(std::move(Slot)) {
#if defined(_WIN32)
			if (GetFileType(GetStdHandle(STD_INPUT_HANDLE)) != FILE_TYPE_PIPE ||
				GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) != FILE_TYPE_PIPE) return;
			const auto Copy = _dup(_fileno(stdout));
			if (Copy < 0) return;
			Channel = _fdopen(Copy, "wb");
			if (!Channel) { _close(Copy); return; }
			if (_setmode(Copy, _O_BINARY) < 0 || _dup2(_fileno(stderr), _fileno(stdout)) < 0) return;
			if (!SetStdHandle(STD_OUTPUT_HANDLE, GetStdHandle(STD_ERROR_HANDLE))) return;
#else
			const auto Copy = dup(STDOUT_FILENO);
			if (Copy < 0) return;
			Channel = fdopen(Copy, "wb");
			if (!Channel) { close(Copy); return; }
			if (dup2(STDERR_FILENO, STDOUT_FILENO) < 0) return;
#endif
			if (GenerateSecureRandomBytes(Proof) != RuntimePackageCryptoStatus::Ok) return;
			Current = true;
			Service = std::jthread([this](std::stop_token Stop) { Pump(Stop); });
		}
		~PrivateServerControl() override {
			Service.request_stop();
			if (Service.joinable()) Service.join();
			if (Channel) std::fclose(Channel);
		}
		bool Begin(network::GameAdmissionRequest Request, network::GameAdmissionCompletion Complete) override {
			if (!Current || Closing || Claimed) return false;
			unsigned Difference = 0;
			for (std::size_t I = 0; I < Proof.size(); ++I)
				Difference |= Proof[I] ^ std::to_integer<unsigned>(Request.Proof[I]);
			if (Difference || Claimed.exchange(true)) return false;
			Complete(PlayerIdentity{"private-session", "owner"});
			return true;
		}
		void Release(std::uint64_t) noexcept override {}
		ManagedServerSnapshot Snapshot() const noexcept override { return {Current, Ready, Closing}; }
		void OnListening(ServerListeningEvidence Info) noexcept override {
			Key = Info.ServerPublicKey;
			Listening.store(true, std::memory_order_release);
		}
		std::uint64_t GetHoldRequest() const noexcept override { return Requested.load(); }
		bool IsPreparingLeave() const noexcept override { return Prepare.load(); }
		void ReportLeave(char Result) noexcept override {
			if (Result != 'S') Prepare = false;
			DrainReceipt = Result;
		}
		void AcknowledgeHold(std::uint64_t Request) noexcept override { Applied.store(Request); }
		const WorldSlot *GetWorldSlot() const noexcept override { return Selected ? &*Selected : nullptr; }
		const std::filesystem::path &GetProfile() const noexcept override { return Profile; }
		void OnDraining() noexcept override { Closing = true; }
		void OnStopped() noexcept override {}
	  private:
		bool Write(const void *Bytes, std::size_t Size) {
			return std::fwrite(Bytes, 1, Size, Channel) == Size && std::fflush(Channel) == 0;
		}
		int ReadCommand() {
			unsigned char Command{};
#if defined(_WIN32)
			const auto Input = GetStdHandle(STD_INPUT_HANDLE);
			DWORD Available{}, Read{};
			if (!PeekNamedPipe(Input, nullptr, 0, nullptr, &Available, nullptr)) return -1;
			if (!Available) return 0;
			if (!ReadFile(Input, &Command, 1, &Read, nullptr) || Read != 1) return -1;
#else
			pollfd Input{STDIN_FILENO, POLLIN, 0};
			const int Result = poll(&Input, 1, 0);
			if (Result < 0 && errno == EINTR) return 0;
			if (Result < 0) return -1;
			if (!Result) return 0;
			if (read(STDIN_FILENO, &Command, 1) != 1) return -1;
#endif
			return Command;
		}
		void Pump(std::stop_token Stop) {
			bool Sent = false;
			while (!Stop.stop_requested()) {
				if (!Sent && Listening.load(std::memory_order_acquire)) {
					if (!Write("GPC1", 4) || !Write(Key.data(), Key.size()) || !Write(Proof.data(), Proof.size())) break;
					Sent = true;
				}
				if (const auto Ack = Applied.exchange(0)) {
					// The dedicated pipe identifies this process generation. The
					// sequence and effective state identify its completed command.
					std::array<unsigned char, 9> Receipt{'H'};
					for (unsigned I = 0; I < 8; ++I) Receipt[I + 1] = static_cast<unsigned char>(Ack >> (I * 8));
					if (!Write(Receipt.data(), Receipt.size())) break;
				}
				if (const char Result = DrainReceipt.exchange(0)) {
					if (!Write(&Result, 1)) break;
				}
				const auto Command = ReadCommand();
				if (Command == 'X') { Closing = true; return; }
				if (Command == 'A' && Sent && !Ready) Ready = true;
				else if (Command == 'D' && Ready) Prepare = true;
				else if ((Command == 'P' || Command == 'R') && Ready) {
					if (++Sequence > (UINT64_MAX >> 1)) break;
					Requested.store((Sequence << 1) | static_cast<unsigned>(Command == 'P'));
				}
				else if (Command != 0) break;
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
			Current = false;
		}
		std::filesystem::path Profile;
		std::optional<WorldSlot> Selected;
		std::array<unsigned char, 32> Proof{};
		network::TransportServerPublicKey Key{};
		std::FILE *Channel = nullptr;
		std::atomic<bool> Prepare{};
		std::atomic<char> DrainReceipt{};
		std::atomic<bool> Current{}, Ready{}, Closing{}, Listening{}, Claimed{};
		std::uint64_t Sequence = 0;
		std::atomic<std::uint64_t> Requested{}, Applied{};
		std::jthread Service;
	};
}
