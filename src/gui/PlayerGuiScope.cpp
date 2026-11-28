#include "gargantuan/gui/PlayerGuiScope.hpp"
#include "gargantuan/classes/ScreenGui.hpp"
#include "gargantuan/gui/GuiLimits.hpp"
#include "gargantuan/reflection/InstanceClassRegistry.hpp"
#include <stdexcept>
#include <unordered_set>

#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/ModuleScript.hpp"
#include "gargantuan/classes/Player.hpp"
#include "gargantuan/classes/PlayerGui.hpp"
#include "gargantuan/classes/Script.hpp"
#include "gargantuan/services/Players.hpp"
#include "gargantuan/services/StarterGui.hpp"

namespace gargantuan {
	std::vector<StarterGuiTemplate> GetStarterGuiTemplates(const std::shared_ptr<Instance> &Starter, bool ResetOnly) {
		constexpr std::size_t MaximumTemplateObjects = 1024;
		if (!Starter || Starter->GetDestroyed() || Starter->IsDestroying()) return {};
		std::vector<StarterGuiTemplate> Templates;
		std::size_t TotalObjects = 0;
		for (const auto &Child : Starter->GetChildren()) {
			const auto Root = std::dynamic_pointer_cast<ScreenGui>(Child);
			if (!Root || !Root->GetArchivable() || Root->GetDestroyed() || Root->IsDestroying() ||
				(ResetOnly && !Root->GetResetOnSpawn())) continue;
			if (Templates.size() == GuiLimits::MaximumRoots || TotalObjects == MaximumTemplateObjects)
				throw std::length_error("StarterGui exceeds the template root or object limit");
			std::vector<std::shared_ptr<Instance>> Nodes;
			std::vector<std::shared_ptr<Instance>> Pending{Root};
			while (!Pending.empty()) {
				auto Node = std::move(Pending.back());
				Pending.pop_back();
				const auto Children = Node->GetChildren();
				++TotalObjects;
				if (Children.size() + Pending.size() > MaximumTemplateObjects - TotalObjects)
					throw std::length_error("StarterGui exceeds the template object limit");
				if (Node != Root) Nodes.push_back(Node);
				Pending.insert(Pending.end(), Children.rbegin(), Children.rend());
			}
			Templates.emplace_back(Root, std::move(Nodes));
		}
		for (const auto &[Root, SourceNodes] : Templates) {
			std::unordered_set<Instance *> Members{Root.get()};
			for (const auto &Node : SourceNodes) Members.insert(Node.get());
			for (auto *Node : Members) {
				const auto *Definition = InstanceClassRegistry::GetDefinition(Node);
				if (!Definition || !Definition->EditorVisible || !InstanceClassRegistry::IsConstructible(*Definition) ||
					!Node->GetArchivable() || Node->GetDestroyed() || Node->IsDestroying())
					throw std::invalid_argument("StarterGui template contains a descendant that cannot be cloned");
				for (const auto &[Name, Property] : Definition->AllProperties) {
					if (Name == "Parent" || !Property->ReadObjectReference) continue;
					const auto Target = Property->ReadObjectReference(Node);
					if (Target && !Members.contains(Target.get()) &&
						(Target == Starter || IsStarterGuiTemplate(*Target)))
						throw std::invalid_argument("StarterGui references must stay within one ScreenGui template");
				}
			}
		}
		return Templates;
	}

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
