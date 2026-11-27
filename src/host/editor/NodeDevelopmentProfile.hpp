#pragma once

#include "gargantuan/editor/EditorHost.hpp"

namespace gargantuan::host {
	[[nodiscard]] EditorNodeDevelopmentPersistence LoadStudioNodeDevelopmentProfile(std::string_view ProfilePath);
}
