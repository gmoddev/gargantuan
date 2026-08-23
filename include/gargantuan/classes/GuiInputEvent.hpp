#pragma once

#include "gargantuan/classes/generated/GuiInputEvent.hpp"

namespace gargantuan {
	G_ENUM(GuiPointerType, Mouse, Touch, Pen);
	G_ENUM(GuiPointerButton, Primary, Secondary, Middle, None);
	G_ENUM(GuiEventPhase, Capture, Target, Bubble);

	class GuiRuntime;
	class GuiInputEvent : public Instance {
		I_GuiInputEvent;

		friend class GuiRuntime;

	  public:
		void Initialize(
			std::shared_ptr<Instance> TargetValue,
			int PointerIdValue,
			Vector2 PositionValue,
			Enums::GuiPointerType PointerTypeValue,
			Enums::GuiPointerButton ButtonValue
		) {
			Target = std::move(TargetValue);
			PointerId = PointerIdValue;
			Position = PositionValue;
			PointerType = PointerTypeValue;
			Button = ButtonValue;
		}
		void SetRouteState(std::shared_ptr<Instance> CurrentTargetValue, Enums::GuiEventPhase PhaseValue) {
			CurrentTarget = std::move(CurrentTargetValue);
			Phase = PhaseValue;
		}
	};
}
