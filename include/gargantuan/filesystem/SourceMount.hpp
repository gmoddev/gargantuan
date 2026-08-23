#pragma once

#include "gargantuan/filesystem/BaseFilesystem.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"

#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace gargantuan {
	inline constexpr std::size_t MaximumSourceMountTraversalDepth = 32;
	inline constexpr std::size_t MaximumSourceMountEntries = 16 * 1024;
	inline constexpr std::size_t MaximumSourceMountPathBytes = 4096;
	inline constexpr std::size_t MaximumSourceMountFileBytes = MaximumProtocolDocumentBytes;
	inline constexpr std::size_t MaximumSourceMountAggregateBytes = 32 * 1024 * 1024;
	inline constexpr std::size_t MaximumSourceMountObjects = MaximumPersistenceObjects;
	inline constexpr std::size_t MaximumSourceMountNestedInstanceDepth = 32;

	struct SourceMountLimits {
		std::size_t TraversalDepth = MaximumSourceMountTraversalDepth;
		std::size_t Entries = MaximumSourceMountEntries;
		std::size_t PathBytes = MaximumSourceMountPathBytes;
		std::size_t FileBytes = MaximumSourceMountFileBytes;
		std::size_t AggregateBytes = MaximumSourceMountAggregateBytes;
		std::size_t Objects = MaximumSourceMountObjects;
		std::size_t NestedInstanceDepth = MaximumSourceMountNestedInstanceDepth;
	};

	enum class SourceMountErrorCode {
		InvalidPath,
		AbsolutePath,
		PathEscape,
		PathTooLong,
		Missing,
		WrongType,
		LinkNotAllowed,
		Inaccessible,
		TraversalDepthLimit,
		EntryLimit,
		FileSizeLimit,
		AggregateByteLimit,
		ObjectLimit,
		NestedInstanceDepthLimit,
		MalformedInstance,
		RecursiveMount,
		MissingParent,
		SynchronizationInProgress,
		CommitFailure,
	};

	[[nodiscard]] std::string_view GetSourceMountErrorCodeName(SourceMountErrorCode Code);

	struct SourceMountError {
		SourceMountErrorCode Code = SourceMountErrorCode::Inaccessible;
		std::filesystem::path RelativePath;
		std::string Message;

		[[nodiscard]] std::string Format() const;
	};

	template <typename Value> using SourceMountResult = std::expected<Value, SourceMountError>;
	using SourceMountStatus = SourceMountResult<void>;

	struct SourceMountBudget {
		std::size_t Entries = 0;
		std::size_t Objects = 0;
		std::size_t ImportedBytes = 0;
	};

	struct SourceMountEntry {
		std::string Name;
		std::filesystem::path RelativePath;
		FileType Type = FileType::Unknown;
	};

	class SourceMount final {
	  public:
		explicit SourceMount(BaseFilesystem &Filesystem, SourceMountLimits Limits = {});

		[[nodiscard]] const std::filesystem::path &GetCanonicalRoot() const { return CanonicalRoot; }
		[[nodiscard]] const SourceMountLimits &GetLimits() const { return Limits; }

		[[nodiscard]] SourceMountResult<std::vector<SourceMountEntry>> GetChildren(
			const std::filesystem::path &RelativeDirectory,
			std::size_t Depth,
			SourceMountBudget &Budget
		) const;
		[[nodiscard]] SourceMountResult<std::string> ReadFile(
			const std::filesystem::path &RelativeFile,
			std::size_t MaximumBytes,
			SourceMountBudget &Budget
		) const;
		[[nodiscard]] SourceMountStatus ReserveObjects(
			const std::filesystem::path &RelativePath,
			std::size_t Count,
			SourceMountBudget &Budget
		) const;
		[[nodiscard]] SourceMountStatus ValidateNestedInstanceDepth(
			const std::filesystem::path &RelativePath,
			std::size_t Depth
		) const;

	  private:
		struct ResolvedPath {
			std::filesystem::path Relative;
			std::filesystem::path Canonical;
			FileType Type = FileType::Unknown;
		};

		BaseFilesystem *Filesystem;
		std::filesystem::path CanonicalRoot;
		SourceMountLimits Limits;

		[[nodiscard]] SourceMountResult<ResolvedPath> ResolveExisting(
			const std::filesystem::path &RelativePath,
			FileType ExpectedType = FileType::Unknown
		) const;
	};
}
