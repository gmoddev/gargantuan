// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "ServerHost.hpp"
#include "host/common/LocalGuestGrant.hpp"
#include "gargantuan/runtime/WorldCatalog.hpp"
#include "gargantuan/filesystem/ProjectIdentity.hpp"
#include "gargantuan/packaging/RuntimePackageCrypto.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
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
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace gargantuan::host {
	// The parent owns the opposite ends of two inherited anonymous pipes.
	// Standard output is separated before Engine or any authored Script starts.
	class PrivateServerControl final : public IManagedServerLifecycle, public IPrivateGameplayControl, public network::IGameAdmissionProvider {
	  public:
		PrivateServerControl(std::filesystem::path Directory, std::optional<WorldSlot> Slot, unsigned Seats = 1)
			: Profile(std::move(Directory)), Selected(std::move(Slot)), Seats(Seats) {
			if (Seats == 0 || Seats > Grants.size()) return;
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
			const auto Flags = fcntl(Copy, F_GETFL, 0);
			if (Flags < 0 || fcntl(Copy, F_SETFL, Flags | O_NONBLOCK) < 0) return;
			if (dup2(STDERR_FILENO, STDOUT_FILENO) < 0) return;
#endif
			for (unsigned I = 0; I < Seats; ++I)
				if (GenerateSecureRandomBytes(Grants[I].Proof) != RuntimePackageCryptoStatus::Ok) return;
			Current = true;
			Service = std::jthread([this](std::stop_token Stop) { Pump(Stop); });
		}
		~PrivateServerControl() override {
			Stopping.store(true, std::memory_order_release);
			Service.request_stop();
#if defined(_WIN32)
			while (Service.joinable() && WaitForSingleObject(Service.native_handle(), 10) == WAIT_TIMEOUT)
				(void)CancelSynchronousIo(Service.native_handle());
#endif
			if (Service.joinable()) Service.join();
			if (Channel) std::fclose(Channel);
		}
		bool Begin(network::GameAdmissionRequest Request, network::GameAdmissionCompletion Complete) override {
			if (!Current || !Ready || Closing || Prepare || (Seats > 1 && ListingClosed)) return false;
			for (unsigned Seat = 0; Seat < Seats; ++Seat) {
				auto &Grant = Grants[Seat];
				unsigned Difference = 0;
				for (std::size_t I = 0; I < Grant.Proof.size(); ++I)
					Difference |= Grant.Proof[I] ^ std::to_integer<unsigned>(Request.Proof[I]);
				if (Difference) continue;
				if ((Seat && std::chrono::steady_clock::now() >= Grant.Expires) || Grant.RequestId) {
					Complete(std::nullopt);
					return true;
				}
				Grant.RequestId = Request.RequestId;
				Complete(Seats == 1 ? PlayerIdentity{"private-session", "owner"} :
					PlayerIdentity{"independent-session", SessionId + "/" + std::to_string(Request.RequestId)});
				return true;
			}
			return false;
		}
		void Release(std::uint64_t RequestId) noexcept override {
			for (unsigned Seat = 1; Seat < Seats; ++Seat) {
				auto &Grant = Grants[Seat];
				if (Grant.RequestId == RequestId) Grant.Retired = true;
			}
		}
		void ServiceGuestInvitations() override {
			if (Seats == 1 || !OwnerReady || Closing || Prepare || ListingClosed) return;
			const auto Now = std::chrono::steady_clock::now();
			for (unsigned Seat = 1; Seat < Seats; ++Seat) {
				auto &Grant = Grants[Seat];
				if (!Grant.Retired && (Grant.RequestId || Now + std::chrono::seconds(10) < Grant.Expires)) continue;
				if (Grant.Pending.load(std::memory_order_acquire)) continue;
				if (GenerateSecureRandomBytes(Grant.Proof) != RuntimePackageCryptoStatus::Ok) { Current = false; return; }
				Grant.RequestId = 0;
				Grant.Retired = false;
				Grant.Expires = Now + std::chrono::seconds(60);
				Grant.Frame[0] = 'G';
				Grant.Frame[1] = static_cast<unsigned char>(Seat);
				const auto Expires = static_cast<std::uint64_t>(InvitationTimeMillis() + 55'000);
				for (unsigned I = 0; I < 8; ++I) Grant.Frame[2 + I] = static_cast<unsigned char>(Expires >> (8 * I));
				std::copy(Grant.Proof.begin(), Grant.Proof.end(), Grant.Frame.begin() + 10);
				Grant.Pending.store(true, std::memory_order_release);
			}
		}
		ManagedServerSnapshot Snapshot() const noexcept override { return {Current, Ready, Closing}; }
		void OnListening(ServerListeningEvidence Info) noexcept override {
			Key = Info.ServerPublicKey;
			Port = Info.Endpoint.Port;
			Listening.store(true, std::memory_order_release);
		}
		std::uint64_t GetHoldRequest() const noexcept override { return Requested.load(); }
		bool AwaitingOwnerReady() const noexcept override { return Seats > 1 && !OwnerReady.load(); }
		void ConfirmOwnerReady() noexcept override { OwnerReady.store(true); }
		bool IsPreparingLeave() const noexcept override { return Prepare.load(); }
		void ReportLeave(char Result) noexcept override {
			// A failed or uncertain drain may resume the owner's gameplay for
			// recovery, but it must not admit new guests into that world.
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
			if (Stopping.load(std::memory_order_acquire)) return false;
#if defined(_WIN32)
			return std::fwrite(Bytes, 1, Size, Channel) == Size && std::fflush(Channel) == 0;
#else
			auto *Data = static_cast<const unsigned char *>(Bytes);
			const int Descriptor = fileno(Channel);
			std::size_t Sent = 0;
			while (Sent < Size) {
				if (Stopping.load(std::memory_order_acquire)) return false;
				pollfd Output{Descriptor, POLLOUT, 0};
				const int Ready = poll(&Output, 1, 10);
				if (Ready < 0 && errno == EINTR) continue;
				if (Ready <= 0) {
					if (Ready == 0) continue;
					return false;
				}
				if (!(Output.revents & POLLOUT)) return false;
				const auto Written = write(Descriptor, Data + Sent, Size - Sent);
				if (Written < 0 && (errno == EINTR || errno == EAGAIN)) continue;
				if (Written <= 0) return false;
				Sent += static_cast<std::size_t>(Written);
			}
			return true;
#endif
		}
		bool WriteState(char Code) {
			SharedHostFrame Frame{static_cast<unsigned char>(Code)};
			return Write(Frame.data(), Seats == 1 ? 1 : Frame.size());
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
			bool AnnouncedOwner = false;
			while (!Stop.stop_requested()) {
				if (!Sent && Listening.load(std::memory_order_acquire)) {
					const std::array<unsigned char, 2> Address{static_cast<unsigned char>(Port), static_cast<unsigned char>(Port >> 8)};
					if (!Write(Seats == 1 ? "GPC2" : "GPH2", 4) || !Write(Address.data(), Address.size()) || !Write(Key.data(), Key.size())) break;
					for (unsigned I = 0; I < Seats; ++I)
						if (!Write(Grants[I].Proof.data(), Grants[I].Proof.size())) { Current = false; return; }
					Sent = true;
				}
				if (!AnnouncedOwner && OwnerReady.load()) {
					if (!WriteState('J')) break;
					AnnouncedOwner = true;
				}
				if (AnnouncedOwner && !ListingClosed) {
					for (unsigned Seat = 1; Seat < Seats; ++Seat) {
						auto &Grant = Grants[Seat];
						if (!Grant.Pending.load(std::memory_order_acquire)) continue;
						if (!Write(Grant.Frame.data(), Grant.Frame.size())) { Current = false; return; }
						Grant.Frame.fill(0);
						Grant.Pending.store(false, std::memory_order_release);
					}
				}
				if (const auto Ack = Applied.exchange(0)) {
					// The dedicated pipe identifies this process generation. The
					// sequence and effective state identify its completed command.
					std::array<unsigned char, 9> Receipt{'H'};
					for (unsigned I = 0; I < 8; ++I) Receipt[I + 1] = static_cast<unsigned char>(Ack >> (I * 8));
					if (!Write(Receipt.data(), Receipt.size())) break;
				}
				if (const char Result = DrainReceipt.exchange(0)) {
					if (!WriteState(Result)) break;
				}
				const auto Command = ReadCommand();
				if (Command == 'X') { Closing = true; return; }
				if (Command == 'A' && Sent && !Ready) Ready = true;
				else if (Command == 'D' && Ready) { ListingClosed = true; Prepare = true; }
				else if ((Command == 'P' || Command == 'R') && Ready && Seats == 1) {
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
		struct Grant {
			std::array<unsigned char, network::GameAdmissionProofBytes> Proof{};
			std::uint64_t RequestId{};
			bool Retired{};
			std::chrono::steady_clock::time_point Expires = std::chrono::steady_clock::now() + std::chrono::seconds(60);
			SharedHostFrame Frame{};
			std::atomic<bool> Pending{};
		};
		std::array<Grant, 8> Grants;
		const unsigned Seats;
		const std::string SessionId = ProjectId::New().ToString();
		std::atomic<bool> ListingClosed{};
		network::TransportServerPublicKey Key{};
		std::uint16_t Port{};
		std::FILE *Channel = nullptr;
		std::atomic<bool> Prepare{};
		std::atomic<char> DrainReceipt{};
		std::atomic<bool> Current{}, Ready{}, Closing{}, Listening{}, OwnerReady{};
		std::atomic<bool> Stopping{};
		std::uint64_t Sequence = 0;
		std::atomic<std::uint64_t> Requested{}, Applied{};
		std::jthread Service;
	};
}
