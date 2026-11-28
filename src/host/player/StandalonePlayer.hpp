#pragma once
#include <filesystem>

namespace gargantuan {
	class ApplicationPresentation;
	class BaseRenderer;
	struct RuntimePackagePayload;
	namespace host {
		int RunStandalonePlayer(ApplicationPresentation &App, BaseRenderer &Device,
			const RuntimePackagePayload &Package, const std::filesystem::path &Root, const std::filesystem::path &Profile, int MaximumFrames);
	}
}
