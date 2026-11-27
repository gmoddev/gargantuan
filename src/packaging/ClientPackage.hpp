#pragma once

#include "gargantuan/assets/AssetTypes.hpp"
#include "gargantuan/packaging/RuntimeDistribution.hpp"

#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gargantuan {
	class BaseFilesystem;
	class DataModel;

	namespace client_package {
		inline constexpr std::string_view CodePath = "content/client-code.json";
		inline constexpr std::string_view CatalogPath = "content/assets/catalog.json";
		inline constexpr std::size_t MaximumCodeBytes = 16 * 1024 * 1024;
		inline constexpr std::size_t MaximumCodeSourceBytes = 64 * 1024;
		using CodeKey = std::pair<std::vector<std::string>, std::string>;
		using CodeMap = std::map<CodeKey, std::string>;

		[[nodiscard]] std::vector<AssetId> ReadSharedAssets(BaseFilesystem &Filesystem);
		[[nodiscard]] std::string CaptureCode(const DataModel &World);
		[[nodiscard]] std::vector<RuntimeDistributionScript> CaptureEligibleScripts(const DataModel &World);
		[[nodiscard]] bool ValidCodePathSegment(std::string_view Value);
		[[nodiscard]] CodeMap DecodeCode(std::string_view Bytes);
		void ValidateCode(std::string_view Bytes);
		[[nodiscard]] std::size_t HydrateCode(std::string_view Bytes, const DataModel &World);
	}
}
