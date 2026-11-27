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
	class AnimationRuntime;
	class DataModel;
	class GuiObject;
	class RenderPublisher;

	enum class GuiPointerAction : std::uint8_t { Move, Down, Up, Cancel };
	enum class GuiRuntimeMode : std::uint8_t { Runtime, AuthoringPreview };

	struct GuiPointerInput {
		int PointerId = 0;
		Enums::GuiPointerType Type = Enums::GuiPointerType::Mouse;
		Enums::GuiPointerButton Button = Enums::GuiPointerButton::Primary;
		GuiPointerAction Action = GuiPointerAction::Move;
		Vector2 PhysicalPosition;
	};

	class GuiRuntime final {
	  public:
		GuiRuntime(std::shared_ptr<DataModel> World, std::filesystem::path DefaultFontPath,
			GuiRuntimeMode Mode = GuiRuntimeMode::Runtime);
		~GuiRuntime();
		GuiRuntime(const GuiRuntime &) = delete;
		GuiRuntime &operator=(const GuiRuntime &) = delete;

		static GuiRuntime *Find(const Instance &Object);

		void SetViewport(GuiViewportConfiguration Configuration);
		[[nodiscard]] bool Reconcile(const AnimationRuntime *Animation = nullptr);
		[[nodiscard]] std::span<const ObjectId> GetVisibleViewportAnimationObjects() const;
		void Publish(RenderPublisher &Publisher, std::shared_ptr<const RenderUiFrame> PlacedFrame = {},
			std::uint64_t PlacementGeneration = 0, GuiTextureChanges ViewportTextures = {});
		[[nodiscard]] std::vector<RenderTextureRemove> GetPreviewTextureRemovals() const;
		[[nodiscard]] bool ProcessEvent(const HostEvent &Event);
		[[nodiscard]] bool ProcessPointer(const GuiPointerInput &Input);
		[[nodiscard]] std::optional<HostCommand> SynchronizeTextInput();
		void RequestFocus(ObjectId Object);
		void ReleaseFocus(ObjectId Object);
		void CapturePointer(int PointerId, ObjectId Object);
		void ReleasePointer(int PointerId, ObjectId Object);
		void ClearTransientState();

		[[nodiscard]] std::string RegisterImage(std::string LogicalId, std::uint32_t Width, std::uint32_t Height,
			std::span<const std::uint8_t> Rgba8);

		[[nodiscard]] std::shared_ptr<const GuiLayoutSnapshot> GetCommittedLayout() const;
		[[nodiscard]] std::shared_ptr<const GuiLayoutSnapshot> GetPresentedLayout() const;
		[[nodiscard]] bool IsPresentedLayoutCurrent() const;
		[[nodiscard]] std::shared_ptr<const GuiPresentationSnapshot> GetCommittedPresentation() const;
		[[nodiscard]] std::shared_ptr<const GuiAccessibilitySnapshot> GetAccessibilitySnapshot() const;
		[[nodiscard]] GuiRuntimeProfile GetLastProfile() const;
		[[nodiscard]] std::vector<GuiDiagnostic> GetDiagnostics() const;

	  private:
		struct Impl;
		std::unique_ptr<Impl> State;
	};
}
