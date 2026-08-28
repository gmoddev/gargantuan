#include "gargantuan/classes/Animator.hpp"

#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/MeshPart.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "gargantuan/scripting/StackValue.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_map>

#include <lua.h>
#include <lualib.h>

namespace gargantuan {
	Animator::Animator() {
		Destroying->Connect([this](std::monostate) { InvalidateTracks(); });
	}

	Animator::~Animator() {
		InvalidateTracks();
	}

	std::shared_ptr<AnimationTrack> Animator::CreateTrack(std::string_view AnimationReference) {
		if (GetDestroyed() || IsDestroying())
			throw std::runtime_error("[Animation:Animator] Animator is destroyed");
		const auto Parsed = AssetReference::Parse(AnimationReference);
		if (!Parsed)
			throw std::invalid_argument("[Animation:Animator] LoadAnimation requires a strict asset:// reference");
		const auto Parent = GetParent();
		const auto Mesh = Parent ? std::dynamic_pointer_cast<MeshPart>(*Parent) : nullptr;
		if (!Mesh || Mesh->GetDestroyed() || Mesh->IsDestroying())
			throw std::runtime_error("[Animation:Animator] Animator must be a direct child of a live MeshPart");
		auto DataModelValue = GetDataModel();
		auto Assets = DataModelValue
			? std::dynamic_pointer_cast<AssetService>(DataModelValue->GetService("AssetService")) : nullptr;
		const auto Rig = Assets ? Assets->ResolveMeshResource(Mesh->GetMesh()) : std::nullopt;
		const auto Clip = Assets ? Assets->ResolveAnimation(AnimationReference) : std::nullopt;
		if (!Rig || !Rig->Value.Skeleton || !Rig->Value.Skeleton->Joints || !Rig->Value.SkinInfluences)
			throw std::runtime_error("[Animation:Animator] Parent MeshPart does not resolve to a skinned Mesh asset");
		if (!Clip)
			throw std::runtime_error("[Animation:Animator] Animation asset is missing or unavailable");
		if (Clip->Value.SkeletonCompatibilityId != Rig->Value.Skeleton->CompatibilityId)
			throw std::runtime_error("[Animation:Animator] Animation and Mesh skeleton compatibility identities differ");

		std::erase_if(Tracks, [](const auto &Track) {
			return !Track || (Track.use_count() == 1 &&
				Track->GetPlaybackState() == Enums::AnimationPlaybackState::Stopped &&
				!Track->HoldsNaturalEndPose());
		});
		if (Tracks.size() >= MaximumTracks)
			throw std::length_error("[Animation:Animator] Animator track capacity is exhausted");
		if (NextCreationSequence == std::numeric_limits<std::uint64_t>::max())
			throw std::overflow_error("[Animation:Animator] track creation sequence is exhausted");

		std::unordered_map<std::string_view, std::int32_t> ClipTracks;
		ClipTracks.reserve(Clip->Value.Tracks->size());
		for (std::size_t Index = 0; Index < Clip->Value.Tracks->size(); ++Index)
			ClipTracks.emplace((*Clip->Value.Tracks)[Index].JointPath, static_cast<std::int32_t>(Index));
		std::vector<std::int32_t> JointTrackIndices(Rig->Value.Skeleton->Joints->size(), -1);
		for (std::size_t Joint = 0; Joint < Rig->Value.Skeleton->Joints->size(); ++Joint)
			if (const auto Found = ClipTracks.find((*Rig->Value.Skeleton->Joints)[Joint].Path);
				Found != ClipTracks.end()) JointTrackIndices[Joint] = Found->second;

		auto Owner = std::dynamic_pointer_cast<Animator>(shared_from_this());
		auto Track = std::make_shared<AnimationTrack>(Owner, *Clip, std::move(JointTrackIndices),
			NextCreationSequence++);
		Tracks.push_back(Track);
		return Track;
	}

	void Animator::InvalidateTracks() {
		for (auto &Track : Tracks) if (Track) Track->InvalidateRuntime();
		Tracks.clear();
	}

	int Animator::LoadAnimation(lua_State *L, Instance *InstanceValue) {
		auto *AnimatorValue = dynamic_cast<Animator *>(InstanceValue);
		if (!AnimatorValue) {
			luaL_error(L, "LoadAnimation receiver is not an Animator");
			return 0;
		}
		const auto Reference = CheckStackValue<std::string_view>(L, 2);
		return StackValue<std::shared_ptr<AnimationTrack>>::Push(L, AnimatorValue->CreateTrack(Reference));
	}
}
