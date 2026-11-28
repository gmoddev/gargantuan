#pragma once

#if defined(_WIN32)
#include "gargantuan/packaging/PackageBuilder.hpp"

#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace gargantuan::windows_package_publication {
	namespace fs = std::filesystem;

	class OutputParents final {
	  public:
		void Acquire(const fs::path &Parent) {
			auto Current = Parent.root_path();
			const auto Relative = Parent.relative_path();
			if (Relative.empty()) Hold(Current);
			for (const auto &Component : Relative) {
				Current /= Component;
				Hold(Current);
			}
		}
	  private:
		struct Close {
			void operator()(void *Handle) const noexcept { CloseHandle(Handle); }
		};
		std::vector<std::unique_ptr<void, Close>> Handles;

		void Hold(const fs::path &Directory) {
			// Pin each existing component before opening its child. The package may create and rename its own children.
			const auto Handle = CreateFileW(Directory.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
				nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
			if (Handle == INVALID_HANDLE_VALUE)
				throw fs::filesystem_error("Could not open package output parent", Directory,
					std::error_code(static_cast<int>(GetLastError()), std::system_category()));
			std::unique_ptr<void, Close> Owner(Handle);
			BY_HANDLE_FILE_INFORMATION Info{};
			if (!GetFileInformationByHandle(Handle, &Info))
				throw fs::filesystem_error("Could not inspect package output parent", Directory,
					std::error_code(static_cast<int>(GetLastError()), std::system_category()));
			if (!(Info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (Info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
				throw std::runtime_error("Package output parent is unavailable or redirected");
			Handles.push_back(std::move(Owner));
		}
	};

	// The kernel deletes this file when its owning process exits, including on termination.
	// CREATE_NEW also refuses a directory lease left by an older packager.
	class Writer final {
	  public:
		explicit Writer(const fs::path &Destination) {
			auto Path = Destination;
			Path += L".gpk-publish";
			Handle = CreateFileW(Path.c_str(), DELETE, 0, nullptr, CREATE_NEW,
				FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
			if (Handle == INVALID_HANDLE_VALUE) {
				const auto Error = GetLastError();
				if (Error == ERROR_FILE_EXISTS || Error == ERROR_ALREADY_EXISTS || Error == ERROR_SHARING_VIOLATION)
					throw RuntimeDistributionError("PackageDestinationBusy", "Another publication owns this output.");
				throw fs::filesystem_error("Could not acquire package publication", Path,
					std::error_code(static_cast<int>(Error), std::system_category()));
			}
		}
		Writer(const Writer &) = delete;
		Writer &operator=(const Writer &) = delete;
		~Writer() { CloseHandle(Handle); }
	  private:
		HANDLE Handle = INVALID_HANDLE_VALUE;
	};

	inline void Move(const fs::path &From, const fs::path &To, const PackageCancellationToken *Cancellation = nullptr) {
		using namespace std::chrono;
		const auto Deadline = steady_clock::now() + seconds(2);
		auto Delay = milliseconds(10);
		for (;;) {
			if (Cancellation && Cancellation->IsCancelled()) throw std::runtime_error("Package build cancelled");
			// No REPLACE_EXISTING. A foreign path appearing at the destination must survive.
			if (MoveFileExW(From.c_str(), To.c_str(), MOVEFILE_WRITE_THROUGH)) return;
			const auto Error = GetLastError();
			const auto Remaining = duration_cast<milliseconds>(Deadline - steady_clock::now());
			if ((Error != ERROR_SHARING_VIOLATION && Error != ERROR_LOCK_VIOLATION && Error != ERROR_ACCESS_DENIED) ||
				Remaining.count() <= 0)
				throw fs::filesystem_error("Move package directory", From, To,
					std::error_code(static_cast<int>(Error), std::system_category()));
			// NTFS also returns ACCESS_DENIED for a directory with an open child, even with FILE_SHARE_DELETE.
			std::this_thread::sleep_for((std::min)(Delay, Remaining));
			Delay = (std::min)(Delay * 2, milliseconds(100));
		}
	}

	inline fs::path Journal(const fs::path &Destination) {
		auto Path = Destination;
		return Path += L".gpk-recovery";
	}

	inline bool HasEntry(const fs::path &Path) {
		return fs::symlink_status(Path).type() != fs::file_type::not_found;
	}

	inline bool IsDigest(const std::string &Value) {
		return Value.size() == 64 && Value.find_first_not_of("0123456789abcdef") == std::string::npos;
	}

	template<class UniquePath>
	void Retire(const fs::path &Directory, UniquePath &&Unique) {
		// First detach the entire record. A crash or a partial cleanup can never expose a half-deleted rollback target.
		const auto Retired = Unique(Directory, "gargantuan-package-backup");
		Move(Directory, Retired);
		fs::remove_all(Retired);
	}

	template<class Identify, class IsRedirected, class UniquePath>
	void Recover(const fs::path &Destination, Identify &&Fingerprint, IsRedirected &&IsLink, UniquePath &&Unique) {
		const auto Directory = Journal(Destination);
		if (!HasEntry(Directory)) return;
		const auto Previous = Directory / L"old";
		const auto Receipt = Directory / L"receipt";
		const auto Conflict = [] { throw std::runtime_error("The package replacement recovery state is unrecognized or conflicts with the destination"); };
		if (IsLink(Directory) || !fs::is_directory(Directory)) Conflict();
		for (const auto &Entry : fs::directory_iterator(Directory)) {
			if ((Entry.path() != Receipt && Entry.path() != Previous) || IsLink(Entry.path())) Conflict();
		}
		if (!fs::is_regular_file(Receipt) || fs::file_size(Receipt) > 160) Conflict();
		std::ifstream Input(Receipt, std::ios::binary);
		std::string Kind, Before, After;
		if (!std::getline(Input, Kind) || !std::getline(Input, Before) || !std::getline(Input, After) ||
			Input.peek() != std::char_traits<char>::eof() || Kind != "GargantuanPackageReplacement1" ||
			(Before != "empty" && !IsDigest(Before)) || !IsDigest(After)) Conflict();
		Input.close();
		const bool HasPrevious = HasEntry(Previous);
		if (HasPrevious && Fingerprint(Previous) != Before) Conflict();
		if (!HasEntry(Destination)) {
			if (!HasPrevious) Conflict();
			Move(Previous, Destination);
		} else {
			const auto Visible = Fingerprint(Destination);
			if ((HasPrevious && Visible != After) || (!HasPrevious && Visible != Before && Visible != After)) Conflict();
		}
		Retire(Directory, Unique);
	}

	template<class UniquePath>
	bool Install(const fs::path &Destination, fs::path &Candidate, const std::string &Before,
		const std::string &After, const PackageCancellationToken &Cancellation, UniquePath &&Unique) {
		if (Before.empty()) {
			Move(Candidate, Destination, &Cancellation);
			Candidate.clear();
			return true;
		}
		const auto Directory = Journal(Destination);
		const auto Previous = Directory / L"old";
		const auto Pending = Unique(Destination, "gargantuan-package-receipt");
		if (!fs::create_directory(Pending)) throw std::runtime_error("Cannot reserve a package replacement receipt");
		try {
			std::ofstream Receipt(Pending / L"receipt", std::ios::binary | std::ios::trunc);
			Receipt << "GargantuanPackageReplacement1\n" << Before << '\n' << After << '\n';
			Receipt.close();
			if (!Receipt) throw std::runtime_error("Could not stage the package replacement receipt");
			Move(Pending, Directory, &Cancellation);
		} catch (...) {
			std::error_code Ignored;
			fs::remove_all(Pending, Ignored);
			throw;
		}
		try {
			Move(Destination, Previous, &Cancellation);
		} catch (...) {
			Retire(Directory, Unique);
			throw;
		}
		try {
			Move(Candidate, Destination, &Cancellation);
			Candidate.clear();
		} catch (...) {
			// Rollback remains mandatory after cancellation. If blocked, retain the durable receipt and previous package.
			try { Move(Previous, Destination); }
			catch (const fs::filesystem_error &) {
				throw RuntimeDistributionError("PublicationRecoveryRequired",
					"The previous package and its receipt were retained for the next authorized publication.");
			}
			Retire(Directory, Unique);
			throw;
		}
		try {
			Retire(Directory, Unique);
			return true;
		} catch (const fs::filesystem_error &) {
			return false;
		}
	}
}
#endif
