#include "gargantuan/filesystem/BaseFilesystem.hpp"

#include <format>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace gargantuan {
	FileType BaseFilesystem::Type(const std::filesystem::path &path) const {
		return Metadata(path).Type;
	}

	void BaseFilesystem::Copy(const std::filesystem::path &source, const std::filesystem::path &destination) {
		if (!Exists(source)) {
			throw std::runtime_error(
				std::format("File {} does not exist", source.string())
			);
		}

		if (Exists(destination)) {
			throw std::runtime_error(
				std::format("Cannot copy to existing destination {}", destination.string())
			);
		}

		if (Type(source) != FileType::File) {
			throw std::runtime_error("Unsupported source file type");
		}

		auto sourceHandle = Open(source, FileOpen::Read);
		auto destinationHandle = Open(destination, FileOpen::Write);

		size_t sourceSize = sourceHandle->Size();
		std::vector<char> sourceContents(sourceSize);

		size_t bytesRead = sourceHandle->Read(
			sourceContents.data(),
			sourceContents.size()
		);

		if (bytesRead != sourceSize) {
			throw std::runtime_error(
				std::format(
					"Failed to read complete source file {}: expected {} bytes, read {}",
					source.string(),
					sourceSize,
					bytesRead
				)
			);
		}

		size_t bytesWritten = destinationHandle->Write(
			sourceContents.data(),
			bytesRead
		);

		if (bytesWritten != bytesRead) {
			throw std::runtime_error(
				std::format(
					"Failed to write complete destination file {}: expected {} bytes, wrote {}",
					destination.string(),
					bytesRead,
					bytesWritten
				)
			);
		}

		sourceHandle->Close();
		destinationHandle->Close();
	}

	void BaseFilesystem::Move(const std::filesystem::path &source, const std::filesystem::path &destination) {
		if (!Exists(source)) {
			throw std::runtime_error(
				std::format("File {} does not exist", source.string())
			);
		}

		if (Exists(destination)) {
			throw std::runtime_error(
				std::format("Cannot move to existing destination {}", destination.string())
			);
		}

		if (Type(source) != FileType::File) {
			throw std::runtime_error("Unsupported source file type");
		}

		auto sourceHandle = Open(source, FileOpen::Read);
		auto destinationHandle = Open(destination, FileOpen::Write);

		size_t sourceSize = sourceHandle->Size();
		std::vector<char> sourceContents(sourceSize);

		size_t bytesRead = sourceHandle->Read(
			sourceContents.data(),
			sourceContents.size()
		);

		if (bytesRead != sourceSize) {
			throw std::runtime_error(
				std::format(
					"Failed to read complete source file {}: expected {} bytes, read {}",
					source.string(),
					sourceSize,
					bytesRead
				)
			);
		}

		size_t bytesWritten = destinationHandle->Write(
			sourceContents.data(),
			bytesRead
		);

		if (bytesWritten != bytesRead) {
			throw std::runtime_error(
				std::format(
					"Failed to write complete destination file {}: expected {} bytes, wrote {}",
					destination.string(),
					bytesRead,
					bytesWritten
				)
			);
		}

		sourceHandle->Close();
		destinationHandle->Close();

		Remove(source);
	}

	std::string BaseFilesystem::ReadFileToString(const std::filesystem::path &path) {
		if (!Exists(path)) {
			throw std::runtime_error(
				std::format("File {} does not exist", path.string())
			);
		}

		if (Type(path) != FileType::File) {
			throw std::runtime_error(
				std::format("{} is not a file", path.string())
			);
		}

		auto handle = Open(path, FileOpen::Read);

		std::string result;
		result.resize(handle->Size());

		size_t bytesRead = handle->Read(
			result.data(),
			result.size()
		);

		result.resize(bytesRead);
		handle->Close();

		return result;
	}

	void BaseFilesystem::WriteStringToFile(const std::filesystem::path &path, std::string contents) {
		if (Exists(path) && Type(path) != FileType::File) {
			throw std::runtime_error(
				std::format("{} is not a file", path.string())
			);
		}

		auto handle = Open(path, FileOpen::Write);

		size_t bytesWritten = handle->Write(
			contents.data(),
			contents.size()
		);

		if (bytesWritten != contents.size()) {
			throw std::runtime_error(
				std::format(
					"Failed to write complete file {}: expected {} bytes, wrote {}",
					path.string(),
					contents.size(),
					bytesWritten
				)
			);
		}

		handle->Close();
	}

	std::stringstream BaseFilesystem::ReadFileToStringStream(const std::filesystem::path &path) {
		return std::stringstream(ReadFileToString(path));
	}

	void BaseFilesystem::WriteStringStreamToFile(
		const std::filesystem::path &path,
		std::ostringstream contents
	) {
		WriteStringToFile(path, contents.str());
	}
}
