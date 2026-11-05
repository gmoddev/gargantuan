#pragma once

#include "gargantuan/classes/generated/InputBinding.hpp"
#include "gargantuan/input/InputDefinitions.hpp"

#include <array>
#include <string>

namespace gargantuan {
	class InputBinding : public Instance {
		I_InputBinding;

	  public:
		static constexpr std::size_t MaximumModifiers = 4;
		[[nodiscard]] const std::array<Enums::KeyCode, MaximumModifiers> &GetModifiers() const {
			return Modifiers;
		}
		[[nodiscard]] std::string ValidateDefinition() const;

	  private:
		Enums::InputBindingType BindingType = Enums::InputBindingType::Key;
		Enums::KeyCode PrimaryKey = Enums::KeyCode::None;
		Enums::UserInputType MouseInputType = Enums::UserInputType::None;
		std::array<Enums::KeyCode, MaximumModifiers> Modifiers{
			Enums::KeyCode::None, Enums::KeyCode::None, Enums::KeyCode::None, Enums::KeyCode::None
		};
		void SetModifier(std::size_t Index, Enums::KeyCode Value);
	};
}
