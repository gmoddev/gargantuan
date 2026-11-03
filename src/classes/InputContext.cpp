#include "gargantuan/classes/InputContext.hpp"

#include "gargantuan/classes/InputAction.hpp"
#include "gargantuan/input/InputConstruction.hpp"

namespace gargantuan {
	int InputContext::AddAction(lua_State *L, Instance *InstanceValue) {
		auto *Context = static_cast<InputContext *>(InstanceValue);
		Context->AssertCanMutate();
		RequireInputMutation();
		const auto Name = ReadInputValue<std::string>(L, 2);
		ValidateInputName(Name);
		ValidateInputOptions(L, 3, {"Type", "Enabled"});
		auto Action = std::make_shared<InputAction>();
		Action->SetName(Name);
		if (auto Value = ReadInputOption<Enums::InputActionType>(L, 3, "Type")) Action->SetType(*Value);
		if (auto Value = ReadInputOption<bool>(L, 3, "Enabled")) Action->SetEnabled(*Value);
		Action->SetParent(Context->shared_from_this());
		return StackValue<std::shared_ptr<InputAction>>::Push(L, Action);
	}
}
