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
#include <utility>

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
		SelectClip(AnimationPreviewClipSlot::Primary, std::move(Reference));
	}

	void AnimationPreview::SelectClip(AnimationPreviewClipSlot Slot, std::string Reference) {
		if (Slot != AnimationPreviewClipSlot::Primary && Slot != AnimationPreviewClipSlot::Secondary)
			throw std::invalid_argument("[Animation:Preview] Invalid clip slot");
		if (Slot == AnimationPreviewClipSlot::Secondary && !Track)
			throw std::logic_error("[Animation:Preview] Select a primary clip first");
		auto &Selected = Slot == AnimationPreviewClipSlot::Primary ? Track : SecondaryTrack;
		// Validate/create first. A rejected clip must not stop the previous one.
		auto Candidate = AnimatorValue->CreateTrack(Reference);
		if (Selected) {
			Candidate->SetSpeed(Selected->GetSpeed());
			Candidate->SetLooped(Selected->GetLooped());
		}
		const auto NextBlendWeight = Slot == AnimationPreviewClipSlot::Secondary && !SecondaryTrack
			? 0.5f : BlendWeight;
		Candidate->SetWeight(Slot == AnimationPreviewClipSlot::Secondary
			? NextBlendWeight : SecondaryTrack ? 1.0f - BlendWeight : 1.0f);
		Candidate->Play();
		Candidate->Pause();
		if (Selected) {
			Selected->SetRootMotionEnabled(false);
			Selected->Stop();
		}
		Selected = std::move(Candidate);
		if (Slot == AnimationPreviewClipSlot::Secondary) {
			BlendWeight = NextBlendWeight;
			Track->SetWeight(1.0f - BlendWeight);
		}
		Step(0.0f);
	}

	void AnimationPreview::ClearSecondaryClip() {
		if (!SecondaryTrack) return;
		SecondaryTrack->SetRootMotionEnabled(false);
		SecondaryTrack->Stop();
		SecondaryTrack.reset();
		BlendWeight = 0.0f;
		if (Track) Track->SetWeight(1.0f);
		Step(0.0f);
	}

	void AnimationPreview::SetBlendWeight(float Weight) {
		if (!std::isfinite(Weight) || Weight < 0.0f || Weight > 1.0f)
			throw std::invalid_argument("[Animation:Preview] Blend weight must be finite and within 0..1");
		if (!Track || !SecondaryTrack)
			throw std::logic_error("[Animation:Preview] Both clips are required for blending");
		Track->SetWeight(1.0f - Weight);
		SecondaryTrack->SetWeight(Weight);
		BlendWeight = Weight;
		Step(0.0f);
	}

	std::shared_ptr<AnimationTrack> AnimationPreview::GetTrack(AnimationPreviewClipSlot Slot) const {
		switch (Slot) {
			case AnimationPreviewClipSlot::Primary: return Track;
			case AnimationPreviewClipSlot::Secondary: return SecondaryTrack;
		}
		throw std::invalid_argument("[Animation:Preview] Invalid clip slot");
	}

	void AnimationPreview::Step(float DeltaTime) {
		if (!std::isfinite(DeltaTime) || DeltaTime < 0.0f || DeltaTime > 60.0f)
			throw std::invalid_argument("[Animation:Preview] Invalid elapsed time");
		// Preview is visual only, including when a native caller retains a track.
		if (Track && Track->GetRootMotionEnabled()) Track->SetRootMotionEnabled(false);
		if (SecondaryTrack && SecondaryTrack->GetRootMotionEnabled()) SecondaryTrack->SetRootMotionEnabled(false);
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
		// EditorViewportRenderer::Resize clears its projection. The next publication
		// must recreate the complete scene rather than refer to that retired base.
		if (PublishedWidth != Width || PublishedHeight != Height)
			Publisher.RequestFullResync();
		auto Textures = Assets->DrainTextureChanges();
		Publisher.SetUiTextureChanges(std::move(Textures.Creates), std::move(Textures.Updates), std::move(Textures.Removes));
		auto Meshes = Assets->DrainMeshChanges();
		Publisher.SetAssetMeshChanges(std::move(Meshes.Creates), std::move(Meshes.Removes));
		if (PendingPose) Publisher.SetAnimationPoseChanges(std::span(&*PendingPose, 1), {});
		if (PendingRemove) Publisher.SetAnimationPoseChanges({}, std::span(&*PendingRemove, 1));
		PendingPose.reset(); PendingRemove.reset();
		auto Publication = Publisher.Publish(*WorkspaceValue, Camera, Width, Height);
		PublishedWidth = Width;
		PublishedHeight = Height;
		return Publication;
	}
}
