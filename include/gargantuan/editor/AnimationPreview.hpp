// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
#pragma once

#include "gargantuan/animation/AnimationRuntime.hpp"
#include "gargantuan/render/RenderExtractor.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace gargantuan {
	enum class AnimationPreviewClipSlot { Primary, Secondary };

	class DataModel;
	class Workspace;
	class AssetService;
	class MeshPart;
	class Animator;
	class AnimationTrack;

	// A disposable native-only view, never a clone of project scripts or gameplay.
	class AnimationPreview final {
	  public:
		AnimationPreview(const AssetProjectSnapshot &Assets, std::string MeshReference);
		~AnimationPreview();
		// Selection starts a paused clip at time zero; replacement keeps that slot's speed and loop setting.
		// Rejected references leave both selected tracks and their blend unchanged.
		void SelectClip(std::string Reference);
		void SelectClip(AnimationPreviewClipSlot Slot, std::string Reference);
		void ClearSecondaryClip();
		// With two clips, effective weights are 1 - Weight (primary) and Weight (secondary).
		void SetBlendWeight(float Weight);
		[[nodiscard]] float GetBlendWeight() const { return SecondaryTrack ? BlendWeight : 0.0f; }
		void Step(float DeltaTime);
		[[nodiscard]] RenderPublicationPtr Publish(
			const RenderCameraInput &Camera, std::uint32_t Width, std::uint32_t Height);
		[[nodiscard]] const std::shared_ptr<AnimationTrack> &GetTrack() const { return Track; }
		[[nodiscard]] std::shared_ptr<AnimationTrack> GetTrack(AnimationPreviewClipSlot Slot) const;
		[[nodiscard]] const std::shared_ptr<DataModel> &GetWorld() const { return World; }
		[[nodiscard]] std::optional<AnimationPoseSnapshot> GetPose() const;

	  private:
		std::shared_ptr<DataModel> World;
		std::shared_ptr<Workspace> WorkspaceValue;
		std::shared_ptr<AssetService> Assets;
		std::shared_ptr<MeshPart> Rig;
		std::shared_ptr<Animator> AnimatorValue;
		std::shared_ptr<AnimationTrack> Track;
		std::shared_ptr<AnimationTrack> SecondaryTrack;
		float BlendWeight = 0.0f;
		std::unique_ptr<AnimationRuntime> Runtime;
		std::optional<RenderAnimationPoseState> PendingPose;
		std::optional<RenderAnimationPoseRemove> PendingRemove;
		RenderPublisher Publisher;
		std::uint32_t PublishedWidth = 0;
		std::uint32_t PublishedHeight = 0;
	};
}
