#pragma once

#include "gargantuan/platform/HostEvent.hpp"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_properties.h>

#include <cstdint>
#include <optional>

namespace gargantuan {
	[[nodiscard]] std::optional<HostEvent> TranslateSDLEvent(const SDL_Event &Event);
	[[nodiscard]] bool ConfigureSDLTextInputProperties(SDL_PropertiesID Properties, const SetTextInputState &State);

	class SDLHost final {
	  public:
		[[nodiscard]] bool PollEvent(HostEvent &Event);
		void Apply(const HostCommand &Command) const;

	  private:
		std::uint32_t ActiveWindowId = 0;
	};
}
