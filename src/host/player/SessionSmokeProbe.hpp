// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "gargantuan/Engine.hpp"
#include "gargantuan/network/GameSession.hpp"

namespace gargantuan::host {
	class SessionSmokeProbe final {
	  public:
		void SubmitMovement(Engine &Runtime) {
			if (MovementSubmitted) return;
			const auto Player = Runtime.Players->GetLocalPlayer();
			if (!Player || !(*Player)->GetCharacter()) return;
			(void)Runtime.ProcessEvent(KeyEvent{.Device = {1}, .Physical = PhysicalKey::W,
				.Logical = LogicalKey::W, .State = ButtonState::Pressed});
			MovementSubmitted = true;
		}
		bool Completed(Engine &Runtime, const network::GameSession &Session, bool Headless) const {
			if (!Runtime.CharacterControl->GetAttributeValue("SessionSmokeComplete")) return false;
			const auto Metrics = Session.GetMetrics();
			return Metrics.ActionsPresented >= 1 && Metrics.ActionPresentationStops >= 1 &&
				(Headless || Runtime.Workspace->GetCurrentCamera()->GetCameraType() == Enums::CameraType::Scriptable);
		}
	  private:
		bool MovementSubmitted = false;
	};
}
