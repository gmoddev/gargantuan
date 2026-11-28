// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <memory>
#include <map>
#include <vector>
#include <string>
#include <string_view>

namespace gargantuan {
	class DataModel;
	class Instance;
	class Script;
	struct AssetRuntimeSnapshot;
	struct RuntimePackagePayload;
	namespace application_template {
		[[nodiscard]] std::string WorldBaseline(const RuntimePackagePayload &Package);
		inline constexpr std::string_view Path = "content/application.json";
		inline constexpr std::size_t MaximumBytes = 2 * 1024 * 1024;
		struct Graph {
			std::shared_ptr<DataModel> World;
			std::shared_ptr<Script> Entry;
		};
		[[nodiscard]] const Instance *FindFolder(const DataModel &World);
		struct SourceBinding { std::vector<std::string> Path; std::string Class, Source; };
		using Bindings = std::map<std::string, SourceBinding, std::less<>>;
		[[nodiscard]] Bindings GetBindings(std::string_view Template);
		[[nodiscard]] std::string Extract(std::string &Project, std::string_view SourceId);
		void Validate(std::string_view Template);
		[[nodiscard]] Graph Load(std::string_view Template);
		[[nodiscard]] AssetRuntimeSnapshot SelectAssets(std::string_view Template, const AssetRuntimeSnapshot &PackageAssets);
	}
}
