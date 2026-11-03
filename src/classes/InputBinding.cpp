#include "gargantuan/classes/InputBinding.hpp"

#include <cmath>
#include <stdexcept>

namespace gargantuan {
	Enums::InputBindingType InputBinding::GetType() const {
		return BindingType;
	}
	void InputBinding::SetType(Enums::InputBindingType Value) {
		AssertCanMutate();
		ValidatePropertyMutation("Type", Value);
		if (Value != Enums::InputBindingType::Key && Value != Enums::InputBindingType::MouseButton &&
			Value != Enums::InputBindingType::PointerDelta)
			throw std::invalid_argument("[Input:Authoring] Binding type is invalid");
		if (BindingType == Value) return;
		BindingType = Value;
		NotifyPropertyCommitted("Type");
	}

	Enums::KeyCode InputBinding::GetKeyCode() const {
		return PrimaryKey;
	}
	void InputBinding::SetKeyCode(Enums::KeyCode Value) {
		AssertCanMutate();
		ValidatePropertyMutation("KeyCode", Value);
		if (!Enum::fromType<Enums::KeyCode>()->FromValue(static_cast<int>(Value)))
			throw std::invalid_argument("[Input:Authoring] Primary key is invalid");
		if (PrimaryKey == Value) return;
		PrimaryKey = Value;
		NotifyPropertyCommitted("KeyCode");
	}

	Enums::UserInputType InputBinding::GetInputType() const {
		return MouseInputType;
	}
	void InputBinding::SetInputType(Enums::UserInputType Value) {
		AssertCanMutate();
		ValidatePropertyMutation("InputType", Value);
		if (!Enum::fromType<Enums::UserInputType>()->FromValue(static_cast<int>(Value)))
			throw std::invalid_argument("[Input:Authoring] Input type is invalid");
		if (MouseInputType == Value) return;
		MouseInputType = Value;
		NotifyPropertyCommitted("InputType");
	}

	Enums::KeyCode InputBinding::GetModifier1() const {
		return Modifiers[0];
	}
	Enums::KeyCode InputBinding::GetModifier2() const {
		return Modifiers[1];
	}
	Enums::KeyCode InputBinding::GetModifier3() const {
		return Modifiers[2];
	}
	Enums::KeyCode InputBinding::GetModifier4() const {
		return Modifiers[3];
	}
	void InputBinding::SetModifier1(Enums::KeyCode Value) {
		SetModifier(0, Value);
	}
	void InputBinding::SetModifier2(Enums::KeyCode Value) {
		SetModifier(1, Value);
	}
	void InputBinding::SetModifier3(Enums::KeyCode Value) {
		SetModifier(2, Value);
	}
	void InputBinding::SetModifier4(Enums::KeyCode Value) {
		SetModifier(3, Value);
	}

	void InputBinding::SetModifier(std::size_t Index, Enums::KeyCode Value) {
		AssertCanMutate();
		const auto Property = "Modifier" + std::to_string(Index + 1);
		ValidatePropertyMutation(Property, Value);
		if (Value != Enums::KeyCode::None && !IsInputModifier(Value))
			throw std::invalid_argument("[Input:Authoring] Modifier must be None or a left/right modifier key");
		if (Modifiers[Index] == Value) return;
		Modifiers[Index] = Value;
		NotifyPropertyCommitted(Property);
	}

	std::string InputBinding::ValidateDefinition() const {
		if (!std::isfinite(GetScale()) || !std::isfinite(GetVectorScale().GetX()) ||
			!std::isfinite(GetVectorScale().GetY()))
			return "[Input:Compile] Binding scales must be finite";
		if (GetType() != Enums::InputBindingType::Key && GetExactModifiers())
			return "[Input:Compile] Exact modifiers require a key binding";
		if (GetType() == Enums::InputBindingType::Key) {
			if (GetKeyCode() == Enums::KeyCode::None ||
				!Enum::fromType<Enums::KeyCode>()->FromValue(static_cast<int>(GetKeyCode())))
				return "[Input:Compile] Key binding requires a supported primary key";
		} else if (GetType() == Enums::InputBindingType::MouseButton) {
			if (GetInputType() != Enums::UserInputType::MouseButton1 &&
				GetInputType() != Enums::UserInputType::MouseButton2 &&
				GetInputType() != Enums::UserInputType::MouseButton3)
				return "[Input:Compile] Mouse binding requires a mouse button";
		} else if (GetType() != Enums::InputBindingType::PointerDelta) {
			return "[Input:Compile] Binding type is invalid";
		}
		for (std::size_t Index = 0; Index < Modifiers.size(); ++Index) {
			const auto Value = Modifiers[Index];
			if (Value == Enums::KeyCode::None) continue;
			if (!IsInputModifier(Value)) return "[Input:Compile] Modifier is invalid";
			if (GetType() != Enums::InputBindingType::Key) return "[Input:Compile] Modifiers require a key binding";
			if (Value == GetKeyCode()) return "[Input:Compile] Modifier cannot equal the primary key";
			for (std::size_t Previous = 0; Previous < Index; ++Previous)
				if (Modifiers[Previous] == Value) return "[Input:Compile] Duplicate modifiers are invalid";
		}
		return {};
	}
}
