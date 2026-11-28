// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "gargantuan/filesystem/ProjectIdentity.hpp"
#include "gargantuan/runtime/ApplicationSession.hpp"
#include <filesystem>
#include <system_error>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace gargantuan::host {
	// Stable lock files permit recovery by OS handle release after a process exit.
	// Unlinking one while it is held would create two distinct lock identities.
	class WorldWriter final {
	  public:
		WorldWriter(const std::filesystem::path &Profile, ProjectId Save) {
			const auto Path = Profile / (Save.ToString() + ".writer");
#ifdef _WIN32
			Handle = CreateFileW(Path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
				FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
			if (Handle == INVALID_HANDLE_VALUE) {
				const auto Error = GetLastError();
				if (Error == ERROR_SHARING_VIOLATION || Error == ERROR_LOCK_VIOLATION) throw WorldInUse{};
				throw std::system_error(Error, std::system_category());
			}
			BY_HANDLE_FILE_INFORMATION Info{};
			if (!GetFileInformationByHandle(Handle, &Info) || (Info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
				CloseHandle(Handle);
				throw std::runtime_error("Invalid world writer file");
			}
#else
			Handle = open(Path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
			if (Handle < 0) throw std::system_error(errno, std::generic_category());
			struct stat Info{};
			if (fstat(Handle, &Info) != 0 || !S_ISREG(Info.st_mode)) {
				close(Handle);
				throw std::runtime_error("Invalid world writer file");
			}
			if (flock(Handle, LOCK_EX | LOCK_NB) != 0) {
				const int Error = errno;
				close(Handle);
				if (Error == EWOULDBLOCK) throw WorldInUse{};
				throw std::system_error(Error, std::generic_category(), "World writer admission failed");
			}
#endif
		}
		WorldWriter(const WorldWriter &) = delete;
		WorldWriter &operator=(const WorldWriter &) = delete;
		~WorldWriter() {
#ifdef _WIN32
			CloseHandle(Handle);
#else
			close(Handle);
#endif
		}
	  private:
#ifdef _WIN32
		HANDLE Handle = INVALID_HANDLE_VALUE;
#else
		int Handle = -1;
#endif
	};
}
