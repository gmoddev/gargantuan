#include "gargantuan/gui/PlayerGuiScope.hpp"

#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/ModuleScript.hpp"
#include "gargantuan/classes/Player.hpp"
#include "gargantuan/classes/PlayerGui.hpp"
#include "gargantuan/classes/Script.hpp"
#include "gargantuan/services/Players.hpp"
#include "gargantuan/services/StarterGui.hpp"

namespace gargantuan {
	std::shared_ptr<PlayerGui> FindPlayerGuiScope(const Instance &Object) {
		for (auto Parent = Object.GetParent().value_or(nullptr); Parent; Parent = Parent->GetParent().value_or(nullptr))
			if (auto Gui = std::dynamic_pointer_cast<PlayerGui>(Parent)) return Gui;
		return nullptr;
	}

	std::shared_ptr<Player> FindPlayerGuiOwner(const PlayerGui &Container) {
		auto Owner = std::dynamic_pointer_cast<Player>(Container.GetParent().value_or(nullptr));
		if (!Owner || Owner->GetDestroyed() || Owner->IsDestroying()) return nullptr;
		return Owner;
	}

	bool IsLocalPlayerGui(const PlayerGui &Container) {
		const auto Owner = FindPlayerGuiOwner(Container);
		const auto World = Owner ? Owner->GetDataModel() : nullptr;
		const auto Service = World ? std::dynamic_pointer_cast<Players>(World->FindFirstChildOfClass("Players", false)) : nullptr;
		return Service && Service->GetLocalPlayer().value_or(nullptr) == Owner;
	}

	bool IsStarterGuiTemplate(const Instance &Object) {
		for (auto Parent = Object.GetParent().value_or(nullptr); Parent; Parent = Parent->GetParent().value_or(nullptr))
			if (dynamic_cast<StarterGui *>(Parent.get())) return true;
		return false;
	}

	bool IsClientPlayerGuiSource(const LuaSourceContainer &Source) {
		if (const auto Candidate = dynamic_cast<const Script *>(&Source))
			return Candidate->GetRunContext() == Enums::RunContext::Client;
		return dynamic_cast<const ModuleScript *>(&Source) != nullptr;
	}
}
