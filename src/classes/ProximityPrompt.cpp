#include "gargantuan/classes/ProximityPrompt.hpp"

#include "gargantuan/runtime/ProtocolInput.hpp"

namespace gargantuan {
	std::string ProximityPrompt::GetActionText() const {
		return ActionText;
	}

	void ProximityPrompt::SetActionText(std::string Value) {
		AssertCanMutate();
		ValidateProtocolString(Value, MaximumTextBytes, "ProximityPrompt ActionText");
		ValidatePropertyMutation("ActionText", Value);
		if (ActionText == Value) return;
		ActionText = std::move(Value);
		NotifyPropertyCommitted("ActionText");
	}

	std::string ProximityPrompt::GetObjectText() const {
		return ObjectText;
	}

	void ProximityPrompt::SetObjectText(std::string Value) {
		AssertCanMutate();
		ValidateProtocolString(Value, MaximumTextBytes, "ProximityPrompt ObjectText");
		ValidatePropertyMutation("ObjectText", Value);
		if (ObjectText == Value) return;
		ObjectText = std::move(Value);
		NotifyPropertyCommitted("ObjectText");
	}
}
