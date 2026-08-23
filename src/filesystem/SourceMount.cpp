#include "gargantuan/filesystem/SourceMount.hpp"

#include "gargantuan/filesystem/Paths.hpp"

#include <algorithm>
#include <format>
#include <stdexcept>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace gargantuan {
	namespace {
		bool IsCanonicalPathWithin(
			const std::filesystem::path &CanonicalRoot,
			const std::filesystem::path &CanonicalCandidate
		) {
			auto RootIterator = CanonicalRoot.begin();
			auto CandidateIterator = CanonicalCandidate.begin();
			for (; RootIterator != CanonicalRoot.end(); ++RootIterator, ++CandidateIterator) {
				if (CandidateIterator == CanonicalCandidate.end() || *RootIterator != *CandidateIterator) return false;
			}
			return true;
		}

		bool IsLinkOrReparsePoint(const std::filesystem::path &Path, std::error_code &Error) {
			const auto Status = std::filesystem::symlink_status(Path, Error);
			if (Error) return false;
			if (std::filesystem::is_symlink(Status)) return true;
#if defined(_WIN32)
			const auto Attributes = GetFileAttributesW(Path.c_str());
			if (Attributes != INVALID_FILE_ATTRIBUTES && (Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return true;
#endif
			return false;
		}

		SourceMountError MakeError(
			SourceMountErrorCode Code,
			const std::filesystem::path &RelativePath,
			std::string Message
		) {
			return {Code, RelativePath, std::move(Message)};
		}

		std::filesystem::path NormalizedRelative(const std::filesystem::path &Path) {
			return Path.empty() ? std::filesystem::path(".") : Path.lexically_normal();
		}
	}

	std::string_view GetSourceMountErrorCodeName(SourceMountErrorCode Code) {
		switch (Code) {
			case SourceMountErrorCode::InvalidPath: return "InvalidPath";
			case SourceMountErrorCode::AbsolutePath: return "AbsolutePath";
			case SourceMountErrorCode::PathEscape: return "PathEscape";
			case SourceMountErrorCode::PathTooLong: return "PathTooLong";
			case SourceMountErrorCode::Missing: return "Missing";
			case SourceMountErrorCode::WrongType: return "WrongType";
			case SourceMountErrorCode::LinkNotAllowed: return "LinkNotAllowed";
			case SourceMountErrorCode::Inaccessible: return "Inaccessible";
			case SourceMountErrorCode::TraversalDepthLimit: return "TraversalDepthLimit";
			case SourceMountErrorCode::EntryLimit: return "EntryLimit";
			case SourceMountErrorCode::FileSizeLimit: return "FileSizeLimit";
			case SourceMountErrorCode::AggregateByteLimit: return "AggregateByteLimit";
			case SourceMountErrorCode::ObjectLimit: return "ObjectLimit";
			case SourceMountErrorCode::NestedInstanceDepthLimit: return "NestedInstanceDepthLimit";
			case SourceMountErrorCode::MalformedInstance: return "MalformedInstance";
			case SourceMountErrorCode::RecursiveMount: return "RecursiveMount";
			case SourceMountErrorCode::MissingParent: return "MissingParent";
			case SourceMountErrorCode::SynchronizationInProgress: return "SynchronizationInProgress";
			case SourceMountErrorCode::CommitFailure: return "CommitFailure";
		}
		return "Unknown";
	}

	std::string SourceMountError::Format() const {
		const auto Path = RelativePath.empty() ? std::string(".") : Paths::ToUtf8(RelativePath);
		return std::format("{} at '{}': {}", GetSourceMountErrorCodeName(Code), Path, Message);
	}

	SourceMount::SourceMount(BaseFilesystem &FilesystemValue, SourceMountLimits LimitsValue)
		: Filesystem(&FilesystemValue), Limits(LimitsValue) {
		if (Limits.TraversalDepth == 0 || Limits.Entries == 0 || Limits.PathBytes == 0 || Limits.FileBytes == 0 ||
			Limits.AggregateBytes == 0 || Limits.Objects == 0 || Limits.NestedInstanceDepth == 0)
			throw std::invalid_argument("SourceMount limits must all be positive");
		if (Limits.TraversalDepth > MaximumSourceMountTraversalDepth || Limits.Entries > MaximumSourceMountEntries ||
			Limits.PathBytes > MaximumSourceMountPathBytes || Limits.FileBytes > MaximumSourceMountFileBytes ||
			Limits.AggregateBytes > MaximumSourceMountAggregateBytes || Limits.Objects > MaximumSourceMountObjects ||
			Limits.NestedInstanceDepth > MaximumSourceMountNestedInstanceDepth)
			throw std::invalid_argument("SourceMount limits cannot exceed the engine hard limits");

		std::error_code FilesystemError;
		CanonicalRoot = std::filesystem::canonical(FilesystemValue.Root, FilesystemError);
		if (FilesystemError || !std::filesystem::is_directory(CanonicalRoot, FilesystemError) || FilesystemError)
			throw std::invalid_argument("SourceMount root must resolve to an accessible directory");
	}

	SourceMountResult<SourceMount::ResolvedPath> SourceMount::ResolveExisting(
		const std::filesystem::path &RelativePath,
		FileType ExpectedType
	) const {
		const auto EncodedPath = Paths::ToUtf8(RelativePath);
		if (EncodedPath.find('\0') != std::string::npos)
			return std::unexpected(MakeError(SourceMountErrorCode::InvalidPath, RelativePath, "path contains a null byte"));
		if (EncodedPath.size() > Limits.PathBytes)
			return std::unexpected(MakeError(SourceMountErrorCode::PathTooLong, RelativePath, "path exceeds its byte limit"));
		if (RelativePath.is_absolute() || RelativePath.has_root_name() || RelativePath.has_root_directory())
			return std::unexpected(MakeError(SourceMountErrorCode::AbsolutePath, RelativePath, "source paths must be relative"));

		std::size_t LexicalDepth = 0;
		for (const auto &Component : RelativePath) {
			if (Component.empty() || Component == ".") continue;
			if (Component == "..") {
				if (LexicalDepth == 0)
					return std::unexpected(MakeError(SourceMountErrorCode::PathEscape, RelativePath, "path escapes the mount root"));
				--LexicalDepth;
			} else {
				++LexicalDepth;
			}
		}
		if (LexicalDepth > Limits.TraversalDepth)
			return std::unexpected(MakeError(
				SourceMountErrorCode::TraversalDepthLimit, RelativePath,
				"source path exceeds its traversal depth limit"
			));

		const auto Normalized = NormalizedRelative(RelativePath);
		auto Current = CanonicalRoot;
		for (const auto &Component : Normalized) {
			if (Component.empty() || Component == ".") continue;
			Current /= Component;
			std::error_code LinkError;
			if (IsLinkOrReparsePoint(Current, LinkError))
				return std::unexpected(MakeError(
					SourceMountErrorCode::LinkNotAllowed, Normalized,
					"symbolic links and reparse points are not allowed in source mounts"
				));
			if (LinkError && LinkError != std::errc::no_such_file_or_directory)
				return std::unexpected(MakeError(SourceMountErrorCode::Inaccessible, Normalized, "source path metadata is inaccessible"));
		}

		std::error_code CanonicalError;
		const auto Canonical = std::filesystem::canonical(CanonicalRoot / Normalized, CanonicalError);
		if (CanonicalError) {
			const auto Code = CanonicalError == std::errc::no_such_file_or_directory
				? SourceMountErrorCode::Missing : SourceMountErrorCode::Inaccessible;
			return std::unexpected(MakeError(Code, Normalized, Code == SourceMountErrorCode::Missing
				? "source path does not exist" : "source path could not be resolved"));
		}
		if (!IsCanonicalPathWithin(CanonicalRoot, Canonical))
			return std::unexpected(MakeError(SourceMountErrorCode::PathEscape, Normalized, "canonical path escapes the mount root"));

		FileType Type = FileType::Unknown;
		try {
			Type = Filesystem->Type(Canonical);
		} catch (...) {
			return std::unexpected(MakeError(SourceMountErrorCode::Inaccessible, Normalized, "source path metadata could not be read"));
		}
		if (ExpectedType != FileType::Unknown && Type != ExpectedType)
			return std::unexpected(MakeError(SourceMountErrorCode::WrongType, Normalized,
				ExpectedType == FileType::Directory ? "source path is not a directory" : "source path is not a regular file"));
		if (Type == FileType::Unknown)
			return std::unexpected(MakeError(SourceMountErrorCode::WrongType, Normalized, "unsupported filesystem entry type"));

		return ResolvedPath{Normalized, Canonical, Type};
	}

	SourceMountResult<std::vector<SourceMountEntry>> SourceMount::GetChildren(
		const std::filesystem::path &RelativeDirectory,
		std::size_t Depth,
		SourceMountBudget &Budget
	) const {
		if (Depth > Limits.TraversalDepth)
			return std::unexpected(MakeError(
				SourceMountErrorCode::TraversalDepthLimit, RelativeDirectory, "directory traversal exceeds its depth limit"
			));
		auto Directory = ResolveExisting(RelativeDirectory, FileType::Directory);
		if (!Directory) return std::unexpected(Directory.error());

		std::vector<DirectoryEntry> BackendEntries;
		try {
			BackendEntries = Filesystem->GetChildren(Directory->Canonical);
		} catch (...) {
			return std::unexpected(MakeError(
				SourceMountErrorCode::Inaccessible, Directory->Relative, "directory entries could not be enumerated"
			));
		}
		if (BackendEntries.size() > Limits.Entries - std::min(Budget.Entries, Limits.Entries))
			return std::unexpected(MakeError(SourceMountErrorCode::EntryLimit, Directory->Relative, "source entry limit exceeded"));

		std::vector<SourceMountEntry> Entries;
		Entries.reserve(BackendEntries.size());
		for (const auto &BackendEntry : BackendEntries) {
			const auto Name = Paths::ToUtf8(BackendEntry.Path.filename());
			if (Name.empty() || Name == "." || Name == ".." || Name.find('/') != std::string::npos ||
				Name.find('\\') != std::string::npos || Name.find('\0') != std::string::npos)
				return std::unexpected(MakeError(SourceMountErrorCode::InvalidPath, Directory->Relative, "directory contains an invalid entry name"));
			try {
				ValidateProtocolString(Name, Limits.PathBytes, "SourceMount entry name");
			} catch (...) {
				return std::unexpected(MakeError(SourceMountErrorCode::InvalidPath, Directory->Relative, "directory contains an invalid entry name"));
			}
			const auto ChildRelative = Directory->Relative / std::filesystem::u8path(Name);
			auto Child = ResolveExisting(ChildRelative);
			if (!Child) return std::unexpected(Child.error());
			Entries.push_back({Name, Child->Relative, Child->Type});
		}
		auto RevalidatedDirectory = ResolveExisting(Directory->Relative, FileType::Directory);
		if (!RevalidatedDirectory || RevalidatedDirectory->Canonical != Directory->Canonical)
			return std::unexpected(RevalidatedDirectory
				? MakeError(SourceMountErrorCode::Inaccessible, Directory->Relative, "source directory changed during enumeration")
				: RevalidatedDirectory.error());
		Budget.Entries += Entries.size();
		std::ranges::sort(Entries, {}, &SourceMountEntry::Name);
		return Entries;
	}

	SourceMountResult<std::string> SourceMount::ReadFile(
		const std::filesystem::path &RelativeFile,
		std::size_t MaximumBytes,
		SourceMountBudget &Budget
	) const {
		auto File = ResolveExisting(RelativeFile, FileType::File);
		if (!File) return std::unexpected(File.error());
		const auto EffectiveMaximum = std::min(MaximumBytes, Limits.FileBytes);
		try {
			auto Handle = Filesystem->Open(File->Canonical, FileOpen::Read);
			const auto DeclaredSize = Handle->Size();
			if (DeclaredSize > EffectiveMaximum)
				return std::unexpected(MakeError(SourceMountErrorCode::FileSizeLimit, File->Relative, "source file exceeds its byte limit"));
			if (DeclaredSize > Limits.AggregateBytes - std::min(Budget.ImportedBytes, Limits.AggregateBytes))
				return std::unexpected(MakeError(SourceMountErrorCode::AggregateByteLimit, File->Relative, "aggregate imported byte limit exceeded"));
			std::string Contents(DeclaredSize + 1, '\0');
			const auto BytesRead = Handle->Read(Contents.data(), Contents.size());
			Handle->Close();
			if (BytesRead != DeclaredSize)
				return std::unexpected(MakeError(SourceMountErrorCode::Inaccessible, File->Relative, "source changed or failed during bounded read"));
			auto RevalidatedFile = ResolveExisting(File->Relative, FileType::File);
			if (!RevalidatedFile || RevalidatedFile->Canonical != File->Canonical)
				return std::unexpected(RevalidatedFile
					? MakeError(SourceMountErrorCode::Inaccessible, File->Relative, "source file changed during bounded read")
					: RevalidatedFile.error());
			Contents.resize(BytesRead);
			Budget.ImportedBytes += BytesRead;
			return Contents;
		} catch (...) {
			return std::unexpected(MakeError(SourceMountErrorCode::Inaccessible, File->Relative, "source file could not be read"));
		}
	}

	SourceMountStatus SourceMount::ReserveObjects(
		const std::filesystem::path &RelativePath,
		std::size_t Count,
		SourceMountBudget &Budget
	) const {
		if (Count > Limits.Objects - std::min(Budget.Objects, Limits.Objects))
			return std::unexpected(MakeError(SourceMountErrorCode::ObjectLimit, RelativePath, "imported object limit exceeded"));
		Budget.Objects += Count;
		return {};
	}

	SourceMountStatus SourceMount::ValidateNestedInstanceDepth(
		const std::filesystem::path &RelativePath,
		std::size_t Depth
	) const {
		if (Depth > Limits.NestedInstanceDepth)
			return std::unexpected(MakeError(
				SourceMountErrorCode::NestedInstanceDepthLimit, RelativePath,
				"nested Instance hierarchy exceeds its depth limit"
			));
		return {};
	}
}
