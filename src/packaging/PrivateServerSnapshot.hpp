// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <filesystem>
#include <memory>
#include <string>

namespace gargantuan {
	struct PackageInspection;
	struct PackageCancellationToken;
	class PrivateServerSnapshot final {
	  public:
		[[nodiscard]] static std::shared_ptr<PrivateServerSnapshot> Stage(const std::filesystem::path &Source,
			const std::string &ExpectedManifest, const PackageInspection &Client, const std::filesystem::path &Profile, PackageCancellationToken Token);
		~PrivateServerSnapshot();
		PrivateServerSnapshot(const PrivateServerSnapshot &) = delete;
		PrivateServerSnapshot &operator=(const PrivateServerSnapshot &) = delete;
		const std::filesystem::path Root;
	  private:
		explicit PrivateServerSnapshot(std::filesystem::path Directory) : Root(std::move(Directory)) {}
	};
}
