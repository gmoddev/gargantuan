// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
#include "gargantuan/editor/AnimationPreview.hpp"

#include "gargantuan/animation/AnimationTrack.hpp"
#include "gargantuan/classes/Animator.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/MeshPart.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <cmath>
#include <stdexcept>

namespace gargantuan {
	AnimationPreview::AnimationPreview(const AssetProjectSnapshot &Snapshot, std::string MeshReference) {
		World = std::make_shared<DataModel>();
		Assets = std::dynamic_pointer_cast<AssetService>(World->GetService("AssetService"));
		WorkspaceValue = std::dynamic_pointer_cast<Workspace>(World->GetService("Workspace"));
		if (!Assets || !WorkspaceValue) throw std::runtime_error("[Animation:Preview] Missing native services");
		Assets->LoadProjectAssetSnapshot(Snapshot);
		const auto Mesh = Assets->ResolveMeshResource(MeshReference);
		if (!Mesh || !Mesh->Value.Skeleton || !Mesh->Value.SkinInfluences)
			throw std::invalid_argument("[Animation:Preview] Select an available skinned Mesh");
		Rig = std::make_shared<MeshPart>();
		Rig->SetName("Animation Preview");
		Rig->SetMesh(std::move(MeshReference));
		Rig->SetSize({1.0f, 1.0f, 1.0f});
		Rig->SetAnchored(true);
		Rig->SetCanCollide(false);
		Rig->SetParent(WorkspaceValue);
		AnimatorValue = std::make_shared<Animator>();
		AnimatorValue->SetParent(Rig);
		Runtime = std::make_unique<AnimationRuntime>(Assets, AnimationRuntime::DiagnosticCallback{},
			AnimationRuntimeOptions{.ParallelPoseEvaluation = false});
		Runtime->RegisterAnimator(AnimatorValue);
	}

	AnimationPreview::~AnimationPreview() {
		if (Runtime) Runtime->Shutdown();
		if (World) World->Destroy();
	}

	void AnimationPreview::SelectClip(std::string Reference) {
		// Validate/create first. A rejected clip must not stop the previous one.
		auto Candidate = AnimatorValue->CreateTrack(Reference);
		if (Track) {
			Candidate->SetSpeed(Track->GetSpeed());
			Candidate->SetLooped(Track->GetLooped());
			Track->Stop();
		}
		Track = std::move(Candidate);
		Track->Play();
		Track->Pause();
		Step(0.0f);
	}

	void AnimationPreview::Step(float DeltaTime) {
		if (!std::isfinite(DeltaTime) || DeltaTime < 0.0f || DeltaTime > 60.0f)
			throw std::invalid_argument("[Animation:Preview] Invalid elapsed time");
		Runtime->Step(DeltaTime);
		// Several controls may arrive before one frame. Coalesce before entering
		// RenderPublisher, which accepts one final operation per object/publication.
		for (const auto &Remove : Runtime->GetPoseRemoves()) { PendingPose.reset(); PendingRemove = Remove; }
		for (const auto &Update : Runtime->GetPoseUpdates()) { PendingRemove.reset(); PendingPose = Update; }
	}

	std::optional<AnimationPoseSnapshot> AnimationPreview::GetPose() const {
		return Runtime->GetPose(Rig->GetObjectId());
	}

	RenderPublicationPtr AnimationPreview::Publish(
		const RenderCameraInput &Camera, std::uint32_t Width, std::uint32_t Height) {
		if (Width == 0 || Height == 0 || Width > 1024 || Height > 1024)
			throw std::invalid_argument("[Animation:Preview] Invalid viewport dimensions");
		auto Textures = Assets->DrainTextureChanges();
		Publisher.SetUiTextureChanges(std::move(Textures.Creates), std::move(Textures.Updates), std::move(Textures.Removes));
		auto Meshes = Assets->DrainMeshChanges();
		Publisher.SetAssetMeshChanges(std::move(Meshes.Creates), std::move(Meshes.Removes));
		if (PendingPose) Publisher.SetAnimationPoseChanges(std::span(&*PendingPose, 1), {});
		if (PendingRemove) Publisher.SetAnimationPoseChanges({}, std::span(&*PendingRemove, 1));
		PendingPose.reset(); PendingRemove.reset();
		return Publisher.Publish(*WorkspaceValue, Camera, Width, Height);
	}
}
