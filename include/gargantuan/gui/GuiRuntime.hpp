// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.

#pragma once

#include "gargantuan/classes/GuiInputEvent.hpp"
#include "gargantuan/gui/GuiResources.hpp"
#include "gargantuan/platform/HostEvent.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace gargantuan {
	class DataModel;
	class GuiObject;
	class RenderPublisher;

	enum class GuiPointerAction : std::uint8_t { Move, Down, Up, Cancel };

	struct GuiPointerInput {
		int PointerId = 0;
		Enums::GuiPointerType Type = Enums::GuiPointerType::Mouse;
		Enums::GuiPointerButton Button = Enums::GuiPointerButton::Primary;
		GuiPointerAction Action = GuiPointerAction::Move;
		Vector2 PhysicalPosition;
	};

	class GuiRuntime final {
	  public:
		GuiRuntime(std::shared_ptr<DataModel> World, std::filesystem::path DefaultFontPath);
		~GuiRuntime();
		GuiRuntime(const GuiRuntime &) = delete;
		GuiRuntime &operator=(const GuiRuntime &) = delete;

		static GuiRuntime *Find(const Instance &Object);

		void SetViewport(GuiViewportConfiguration Configuration);
		[[nodiscard]] bool Reconcile();
		void Publish(RenderPublisher &Publisher);
		[[nodiscard]] bool ProcessEvent(const HostEvent &Event);
		[[nodiscard]] bool ProcessPointer(const GuiPointerInput &Input);
		void RequestFocus(ObjectId Object);
		void ReleaseFocus(ObjectId Object);
		void CapturePointer(int PointerId, ObjectId Object);
		void ReleasePointer(int PointerId, ObjectId Object);
		void ClearTransientState();

		void RegisterImage(std::string LogicalId, std::uint32_t Width, std::uint32_t Height,
			std::span<const std::uint8_t> Rgba8);

		[[nodiscard]] std::shared_ptr<const GuiLayoutSnapshot> GetCommittedLayout() const;
		[[nodiscard]] std::shared_ptr<const GuiPresentationSnapshot> GetCommittedPresentation() const;
		[[nodiscard]] std::shared_ptr<const GuiAccessibilitySnapshot> GetAccessibilitySnapshot() const;
		[[nodiscard]] GuiRuntimeProfile GetLastProfile() const;
		[[nodiscard]] std::vector<GuiDiagnostic> GetDiagnostics() const;

	  private:
		struct Impl;
		std::unique_ptr<Impl> State;
	};
}
