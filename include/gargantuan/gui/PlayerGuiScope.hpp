#pragma once

#include <memory>
#include <vector>

namespace gargantuan {
	class Instance;
	class LuaSourceContainer;
	class Player;
	class PlayerGui;
	class ScreenGui;

	struct StarterGuiTemplate {
		std::shared_ptr<ScreenGui> Root;
		std::vector<std::shared_ptr<Instance>> Descendants;
	};

	[[nodiscard]] std::vector<StarterGuiTemplate> GetStarterGuiTemplates(const std::shared_ptr<Instance> &Starter, bool ResetOnly);

	[[nodiscard]] std::shared_ptr<PlayerGui> FindPlayerGuiScope(const Instance &Object);
	[[nodiscard]] std::shared_ptr<Player> FindPlayerGuiOwner(const PlayerGui &Container);
	[[nodiscard]] bool IsLocalPlayerGui(const PlayerGui &Container);
	[[nodiscard]] bool IsStarterGuiTemplate(const Instance &Object);
	[[nodiscard]] bool IsClientPlayerGuiSource(const LuaSourceContainer &Source);
}
