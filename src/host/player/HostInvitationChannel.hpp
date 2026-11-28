// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include <charconv>
#include <array>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <string>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/socket.h>
#endif

namespace gargantuan::host {
	// The launcher supplies a private, inherited message channel. No package
	// path, authored stream or process log is a destination for guest proofs.
	class HostInvitationChannel final {
	  public:
		static std::optional<HostInvitationChannel> Open(std::string_view Value) {
			std::uintptr_t Native{};
			const auto Parsed = std::from_chars(Value.data(), Value.data() + Value.size(), Native);
			if (Parsed.ec != std::errc{} || Parsed.ptr != Value.data() + Value.size() || Native <= 2) return {};
#ifdef _WIN32
			HANDLE Handle = reinterpret_cast<HANDLE>(Native);
			DWORD Flags{};
			if (GetFileType(Handle) != FILE_TYPE_PIPE || !GetNamedPipeInfo(Handle, &Flags, nullptr, nullptr, nullptr) ||
				!(Flags & PIPE_TYPE_MESSAGE) || !(Flags & PIPE_SERVER_END)) return {};
			DWORD Mode = PIPE_READMODE_MESSAGE | PIPE_NOWAIT;
			if (!SetNamedPipeHandleState(Handle, &Mode, nullptr, nullptr) ||
				!SetHandleInformation(Handle, HANDLE_FLAG_INHERIT, 0)) return {};
#else
			if (Native > std::numeric_limits<int>::max()) return {};
			const auto Descriptor = static_cast<int>(Native);
			int Type{};
			socklen_t Size = sizeof(Type);
			sockaddr_storage Peer{};
			socklen_t PeerSize = sizeof(Peer);
			if (getsockopt(Descriptor, SOL_SOCKET, SO_TYPE, &Type, &Size) != 0 || Type != SOCK_DGRAM ||
				getpeername(Descriptor, reinterpret_cast<sockaddr *>(&Peer), &PeerSize) != 0 || Peer.ss_family != AF_UNIX) return {};
			const auto Flags = fcntl(Descriptor, F_GETFD);
			if (Flags < 0 || fcntl(Descriptor, F_SETFD, Flags | FD_CLOEXEC) != 0) return {};
#endif
			return HostInvitationChannel{Native};
		}
		bool Publish(std::string_view Frame) const {
			if (Frame.empty() || Frame.size() > 4096) return false;
#ifdef _WIN32
			DWORD Written{};
			return WriteFile(reinterpret_cast<HANDLE>(Native), Frame.data(), static_cast<DWORD>(Frame.size()), &Written, nullptr) &&
				Written == Frame.size();
#else
			return send(static_cast<int>(Native), Frame.data(), Frame.size(), MSG_DONTWAIT) == static_cast<ssize_t>(Frame.size());
#endif
		}
		// Empty means no message yet. A closed or malformed channel is retired
		// without interrupting an already admitted game.
		std::optional<std::string> Poll() const {
			std::array<char, 4096> Bytes;
#ifdef _WIN32
			const auto Handle = reinterpret_cast<HANDLE>(Native);
			DWORD Available{}, Size{};
			if (!PeekNamedPipe(Handle, nullptr, 0, nullptr, &Available, &Size) || Size > Bytes.size()) return {};
			if (!Available) return std::string{};
			DWORD Count{};
			if (!Size || !ReadFile(Handle, Bytes.data(), Size, &Count, nullptr) || Count != Size) return {};
#else
			const auto Count = recv(static_cast<int>(Native), Bytes.data(), Bytes.size(), MSG_DONTWAIT | MSG_TRUNC);
			if (Count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return std::string{};
			if (Count <= 0 || static_cast<std::size_t>(Count) > Bytes.size()) return {};
#endif
			return std::string(Bytes.data(), static_cast<std::size_t>(Count));
		}
	  private:
		explicit HostInvitationChannel(std::uintptr_t Value) : Native(Value) {}
		std::uintptr_t Native;
	};
}
