// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
#pragma once

#include "gargantuan/animation/AnimationRuntime.hpp"
#include "gargantuan/render/RenderExtractor.hpp"

#include <memory>
#include <string>

namespace gargantuan {
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
		void SelectClip(std::string Reference);
		void Step(float DeltaTime);
		[[nodiscard]] RenderPublicationPtr Publish(
			const RenderCameraInput &Camera, std::uint32_t Width, std::uint32_t Height);
		[[nodiscard]] const std::shared_ptr<AnimationTrack> &GetTrack() const { return Track; }
		[[nodiscard]] const std::shared_ptr<DataModel> &GetWorld() const { return World; }
		[[nodiscard]] std::optional<AnimationPoseSnapshot> GetPose() const;

	  private:
		std::shared_ptr<DataModel> World;
		std::shared_ptr<Workspace> WorkspaceValue;
		std::shared_ptr<AssetService> Assets;
		std::shared_ptr<MeshPart> Rig;
		std::shared_ptr<Animator> AnimatorValue;
		std::shared_ptr<AnimationTrack> Track;
		std::unique_ptr<AnimationRuntime> Runtime;
		std::optional<RenderAnimationPoseState> PendingPose;
		std::optional<RenderAnimationPoseRemove> PendingRemove;
		RenderPublisher Publisher;
	};
}
