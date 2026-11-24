#pragma once

#include <memory>

namespace gargantuan {
	class Instance;
	class LuaSourceContainer;
	class Player;
	class PlayerGui;

	[[nodiscard]] std::shared_ptr<PlayerGui> FindPlayerGuiScope(const Instance &Object);
	[[nodiscard]] std::shared_ptr<Player> FindPlayerGuiOwner(const PlayerGui &Container);
	[[nodiscard]] bool IsLocalPlayerGui(const PlayerGui &Container);
	[[nodiscard]] bool IsStarterGuiTemplate(const Instance &Object);
	[[nodiscard]] bool IsClientPlayerGuiSource(const LuaSourceContainer &Source);
}
