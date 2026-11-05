#include "gargantuan/classes/InputAction.hpp"

#include "gargantuan/classes/InputBinding.hpp"
#include "gargantuan/input/InputConstruction.hpp"

#include <array>

namespace gargantuan {
	Enums::InputActionType InputAction::GetType() const {
		return ActionType;
	}
	void InputAction::SetType(Enums::InputActionType Value) {
		AssertCanMutate();
		ValidatePropertyMutation("Type", Value);
		if (Value != Enums::InputActionType::Bool && Value != Enums::InputActionType::Scalar &&
			Value != Enums::InputActionType::Vector2)
			throw std::invalid_argument("[Input:Authoring] Action type is invalid");
		if (ActionType == Value) return;
		ActionType = Value;
		NotifyPropertyCommitted("Type");
	}

	bool InputAction::IsDown() {
		return RuntimeDown;
	}
	float InputAction::GetValue() {
		return RuntimeScalar;
	}
	Vector2 InputAction::GetVector() {
		return RuntimeValue;
	}
	void InputAction::CommitState(Vector2 Value, bool Down, float Scalar) {
		RuntimeValue = Value;
		RuntimeScalar = Scalar;
		RuntimeDown = Down;
	}

	int InputAction::AddBinding(lua_State *L, Instance *InstanceValue) {
		auto *Action = static_cast<InputAction *>(InstanceValue);
		Action->AssertCanMutate();
		RequireInputMutation();
		const auto Key = ReadInputValue<Enums::KeyCode>(L, 2);
		ValidateInputOptions(L, 3, {"Modifiers", "Enabled", "ExactModifiers", "Scale", "Priority", "Consume"});
		auto Binding = std::make_shared<InputBinding>();
		Binding->SetName("Binding");
		Binding->SetKeyCode(Key);
		if (auto Value = ReadInputOption<bool>(L, 3, "Enabled")) Binding->SetEnabled(*Value);
		if (auto Value = ReadInputOption<bool>(L, 3, "ExactModifiers")) Binding->SetExactModifiers(*Value);
		if (auto Value = ReadInputOption<float>(L, 3, "Scale")) Binding->SetScale(*Value);
		if (auto Value = ReadInputOption<int>(L, 3, "Priority")) Binding->SetPriority(*Value);
		if (auto Value = ReadInputOption<bool>(L, 3, "Consume")) Binding->SetConsume(*Value);
		if (!lua_isnoneornil(L, 3)) {
			lua_getfield(L, 3, "Modifiers");
			if (!lua_isnil(L, -1)) {
				if (!lua_istable(L, -1)) throw std::invalid_argument("[Input:Authoring] Modifiers must be an array");
				const auto Table = lua_absindex(L, -1);
				const auto Count = lua_objlen(L, Table);
				if (Count > InputBinding::MaximumModifiers)
					throw std::length_error("[Input:Authoring] At most four modifiers are supported");
				std::size_t Entries = 0;
				lua_pushnil(L);
				while (lua_next(L, Table)) {
					if (lua_type(L, -2) != LUA_TNUMBER || lua_tonumber(L, -2) < 1 ||
						lua_tonumber(L, -2) > static_cast<double>(Count) ||
						std::trunc(lua_tonumber(L, -2)) != lua_tonumber(L, -2))
						throw std::invalid_argument("[Input:Authoring] Modifiers must be a dense array");
					++Entries;
					lua_pop(L, 1);
				}
				if (Entries != Count) throw std::invalid_argument("[Input:Authoring] Modifiers must be a dense array");
				using Setter = void (InputBinding::*)(Enums::KeyCode);
				constexpr std::array<Setter, 4> Setters{
					&InputBinding::SetModifier1,
					&InputBinding::SetModifier2,
					&InputBinding::SetModifier3,
					&InputBinding::SetModifier4
				};
				for (std::size_t Index = 0; Index < Count; ++Index) {
					lua_rawgeti(L, Table, static_cast<int>(Index + 1));
					const auto Modifier = ReadInputValue<Enums::KeyCode>(L, -1);
					lua_pop(L, 1);
					if (!IsInputModifier(Modifier))
						throw std::invalid_argument("[Input:Authoring] Modifier must be a left/right modifier key");
					(Binding.get()->*Setters[Index])(Modifier);
				}
			}
			lua_pop(L, 1);
		}
		if (auto Error = Binding->ValidateDefinition(); !Error.empty()) throw std::invalid_argument(Error);
		Binding->SetParent(Action->shared_from_this());
		return StackValue<std::shared_ptr<InputBinding>>::Push(L, Binding);
	}
}
