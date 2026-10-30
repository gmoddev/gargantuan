#include "gargantuan/classes/ModuleScript.hpp"
#include "gargantuan/runtime/ServicePolicy.hpp"

namespace gargantuan {
	ModuleScript::ModuleScript() {
		GetPropertyChangedSignal("Source")->Connect([this](std::monostate) {
			RefreshReplicatedSource(true);
		});
		AncestryChanged->Connect([this](const auto &) {
			RefreshReplicatedSource(false);
		});
	}

	void ModuleScript::RefreshReplicatedSource(bool SourceChanged) {
		if (GetDestroyed() || IsDestroying()) return;
		const bool Shared = GetInstanceServicePolicy(*this) == SchemaServicePolicy::Replicated;
		const bool Changed = Shared != SourceWasShared || (Shared && SourceChanged);
		SourceWasShared = Shared;
		if (Changed) NotifyPropertyCommitted("ReplicatedSource");
	}

	std::string ModuleScript::GetReplicatedSource() const {
		return GetInstanceServicePolicy(*this) == SchemaServicePolicy::Replicated ? GetSource() : std::string{};
	}

	void ModuleScript::SetReplicatedSource(std::string Value) {
		ApplyReplicatedSource(std::move(Value));
	}
}
