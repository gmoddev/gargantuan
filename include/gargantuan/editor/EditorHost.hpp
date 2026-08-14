// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/editor/EditorViewport.hpp"
#include "gargantuan/editor/SharedFrameRing.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"
#include "gargantuan/runtime/MutationGateway.hpp"

#include <cstddef>
#include <istream>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

namespace gargantuan {
	inline constexpr std::uint32_t EditorHostProtocolVersion = 1;
	inline constexpr std::size_t EditorHostMaximumRequestBytes = 1024 * 1024;
	inline constexpr std::size_t EditorHostMaximumResponseBytes = 8 * 1024 * 1024;
	inline constexpr std::uint32_t EditorHostMaximumViewportDimension = 1024;
	inline constexpr std::uint64_t EditorHostMaximumViewportPixels = 1024 * 1024;
	inline constexpr std::string_view EditorHostResponsePrefix = "GARGANTUAN_EDITOR/1 ";

	class EditorHost {
	  public:
		explicit EditorHost(std::string sessionToken);
		EditorHost(std::string sessionToken, ScriptSecurityContext studioSecurity);

		[[nodiscard]] std::string HandleRequest(std::string_view request);
		int Run(std::istream &input, std::ostream &output);

	  private:
		std::string SessionToken;
		std::unique_ptr<DiskFilesystem> Filesystem;
		std::shared_ptr<DataModel> World;
		std::optional<ChangeCursor> Cursor;
		MutationGateway Mutations;
		ScriptSecurityContext StudioSecurity = ScriptSecurityContext::StudioCoreUi();
		std::shared_ptr<Camera> ViewportCamera;
		std::unique_ptr<EditorViewportRenderer> ViewportRenderer;
		std::unique_ptr<SharedFrameRing> ViewportFrameRing;
		std::uint32_t ViewportWidth = 0;
		std::uint32_t ViewportHeight = 0;
		std::uint64_t ViewportFrameNumber = 0;
	};
}
