// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.

#include "gargantuan/animation/AnimationRuntime.hpp"
#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/classes/Attachment.hpp"
#include "gargantuan/classes/AttachmentMesh.hpp"
#include "gargantuan/classes/Animator.hpp"
#include "gargantuan/classes/Character.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/MeshPart.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/editor/EditorViewport.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/SourceMount.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/RenderExtractor.hpp"
#include "gargantuan/render/RenderProjection.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"
#include "gargantuan/runtime/SemanticSpatialResolver.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

namespace {
	using namespace gargantuan;

	void Check(bool Condition, const char *Message) {
		if (!Condition) throw std::runtime_error(Message);
	}

	template <typename Callback> void CheckInvalid(Callback Test, const char *Message) {
		try { Test(); }
		catch (const std::invalid_argument &) { return; }
		Check(false, Message);
	}

	const RenderObjectCreate *FindCreate(const RenderPublication &Publication, ObjectId Object) {
		const auto Found = std::ranges::find_if(Publication.Creates,
			[&](const RenderObjectCreate &Create) { return Create.Item.Object == Object; });
		return Found == Publication.Creates.end() ? nullptr : &*Found;
	}

	const RenderObjectUpdate *FindUpdate(const RenderPublication &Publication, ObjectId Object) {
		const auto Found = std::ranges::find_if(Publication.Updates,
			[&](const RenderObjectUpdate &Update) { return Update.Object == Object; });
		return Found == Publication.Updates.end() ? nullptr : &*Found;
	}

	bool Near(const glm::mat4 &Left, const glm::mat4 &Right) {
		for (glm::length_t Column = 0; Column < 4; ++Column)
			for (glm::length_t Row = 0; Row < 4; ++Row)
				if (std::abs(Left[Column][Row] - Right[Column][Row]) > 0.0002f) return false;
		return true;
	}

	void WriteText(const std::filesystem::path &Path, const std::string &Value) {
		std::ofstream Output(Path, std::ios::binary);
		Output << Value;
		Check(static_cast<bool>(Output), "fixture write succeeded");
	}

	void TestIncompatibleReimportFence(const std::filesystem::path &Root) {
		const auto FenceRoot = Root / "fence";
		std::filesystem::create_directories(FenceRoot / "assets");
		std::filesystem::copy_file(Root / "assets" / "accessory.obj",
			FenceRoot / "assets" / "accessory.obj");
		std::ifstream Source(GARGANTUAN_ANIMATION_PREVIEW_FIXTURE, std::ios::binary);
		auto RigSource = nlohmann::ordered_json::parse(Source);
		WriteText(FenceRoot / "assets" / "fenced-rig.gltf", RigSource.dump());
		DiskFilesystem Filesystem(FenceRoot);
		SourceMount Mount(Filesystem);
		auto Game = std::make_shared<DataModel>();
		Game->Root = FenceRoot;
		Game->Filesystem = &Filesystem;
		auto World = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
		auto Assets = std::dynamic_pointer_cast<AssetService>(Game->GetService("AssetService"));
		const auto RigImport = Assets->ImportProjectAsset(Mount, "assets/fenced-rig.gltf", AssetKind::Mesh);
		const auto AccessoryImport = Assets->ImportProjectAsset(Mount, "assets/accessory.obj", AssetKind::Mesh);
		Check(RigImport.Ok && AccessoryImport.Ok && AccessoryImport.Record,
			"compatibility fence fixtures import");
		std::string RigReference;
		for (const auto &Record : RigImport.Records)
			if (Record.Kind == AssetKind::Mesh) RigReference = Record.Reference.Value;
		Check(!RigReference.empty(), "compatibility fence fixture has a Mesh");
		const auto OriginalRig = Assets->ResolveMeshResource(RigReference);
		Check(OriginalRig && OriginalRig->Value.Skeleton,
			"compatibility fence fixture has a canonical skeleton");
		auto Rig = std::make_shared<MeshPart>();
		Rig->SetMesh(RigReference);
		Rig->SetAnchored(true);
		Rig->SetCanCollide(false);
		Rig->SetParent(World);
		auto Socket = std::make_shared<Attachment>();
		Socket->SetJointPath("BeaconRoot/BeaconTip");
		Socket->SetParent(Rig);
		auto Nested = std::make_shared<Attachment>();
		Nested->SetCFrame(CFrame(0.2f, 0.0f, 0.0f));
		Nested->SetParent(Socket);
		auto Visual = std::make_shared<AttachmentMesh>();
		Visual->SetMesh(AccessoryImport.Record->Reference.Value);
		Visual->SetParent(Nested);
		auto Spatial = std::make_shared<SemanticSpatialResolver>(Assets);
		const auto IsBound = [&] {
			const auto Resolved = Spatial->ResolveAttachment(Visual);
			return Resolved && Resolved->Animated;
		};
		RenderPublisher Publisher;
		Publisher.SetSpatialResolver(Spatial);
		auto MeshChanges = Assets->DrainMeshChanges();
		Publisher.SetAssetMeshChanges(std::move(MeshChanges.Creates), std::move(MeshChanges.Removes));
		const auto Initial = Publisher.Publish(*World, RenderCameraInput{}, 640, 360);
		Check(FindCreate(*Initial, Visual->GetObjectId()) && IsBound(),
			"nested lazy socket binds to the original canonical skeleton");

		// Keep the exact path while changing bind translation, which changes the
		// skeleton compatibility identity without changing the socket spelling.
		RigSource["nodes"][1]["translation"][1] = 1.25;
		WriteText(FenceRoot / "assets" / "fenced-rig.gltf", RigSource.dump());
		const auto Reimport = Assets->ReimportProjectAsset(Mount, RigReference);
		const auto ChangedRig = Assets->ResolveMeshResource(RigReference);
		Check(Reimport.Ok && ChangedRig && ChangedRig->Value.Skeleton &&
			ChangedRig->Value.Skeleton->CompatibilityId != OriginalRig->Value.Skeleton->CompatibilityId,
			"same-named joint reimport changes the canonical compatibility identity");
		auto ReimportMeshes = Assets->DrainMeshChanges();
		Publisher.SetAssetMeshChanges(std::move(ReimportMeshes.Creates), std::move(ReimportMeshes.Removes));
		Spatial->Step();
		const auto Incompatible = Publisher.Publish(*World, RenderCameraInput{}, 640, 360);
		Check(std::ranges::any_of(Incompatible->Removes, [&](const RenderObjectRemove &Remove) {
			return Remove.Object == Visual->GetObjectId();
		}) && !IsBound(),
			"incompatible reimport removes the visual instead of rebinding a same-named joint");
		Visual->SetColor(Color3(0.5f, 0.5f, 0.5f));
		Spatial->Step();
		const auto MaterialRefresh = Publisher.Publish(*World, RenderCameraInput{}, 640, 360);
		Check(!FindCreate(*MaterialRefresh, Visual->GetObjectId()) && !IsBound(),
			"material reindex preserves the incompatible-skeleton fence");
		Publisher.RequestFullResync();
		const auto FullRefresh = Publisher.Publish(*World, RenderCameraInput{}, 640, 360);
		Check(FullRefresh->FullResync && !FindCreate(*FullRefresh, Visual->GetObjectId()) && !IsBound(),
			"full render resync preserves the incompatible-skeleton fence");
		Socket->SetJointPath("");
		Socket->SetJointPath("BeaconRoot/BeaconTip");
		Spatial->Step();
		const auto Reauthored = Publisher.Publish(*World, RenderCameraInput{}, 640, 360);
		Check(FindCreate(*Reauthored, Visual->GetObjectId()) && IsBound(),
			"explicit valid socket reauthoring permits binding to the new skeleton");
	}
}

int main() {
	using namespace gargantuan;
	const auto Root = std::filesystem::temp_directory_path() /
		("gargantuan-attachment-mesh-" +
			std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	struct Cleanup {
		std::filesystem::path Path;
		~Cleanup() { std::error_code Ignored; std::filesystem::remove_all(Path, Ignored); }
	} CleanupValue{Root};
	try {
		BootstrapNativeRuntimeSchema();
		std::filesystem::create_directories(Root / "assets");
		WriteText(Root / "assets" / "accessory.obj",
			"v -0.5 -0.5 0\nv 0.5 -0.5 0\nv 0 0.5 0\nf 1 2 3\n");
		std::filesystem::copy_file(GARGANTUAN_ANIMATION_PREVIEW_FIXTURE, Root / "assets" / "rig.gltf");
		DiskFilesystem Filesystem(Root);
		SourceMount Mount(Filesystem);
		auto Game = std::make_shared<DataModel>();
		Game->Root = Root;
		Game->Filesystem = &Filesystem;
		auto World = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
		auto Assets = std::dynamic_pointer_cast<AssetService>(Game->GetService("AssetService"));
		Check(World && Assets, "canonical services exist");
		const auto AccessoryImport = Assets->ImportProjectAsset(Mount, "assets/accessory.obj", AssetKind::Mesh);
		const auto RigImport = Assets->ImportProjectAsset(Mount, "assets/rig.gltf", AssetKind::Mesh);
		Check(AccessoryImport.Ok && AccessoryImport.Record && RigImport.Ok,
			"rigid accessory and canonical skinned rig import");
		std::string RigReference;
		std::string ClipReference;
		for (const auto &Record : RigImport.Records)
			if (Record.Kind == AssetKind::Mesh) RigReference = Record.Reference.Value;
			else if (Record.Kind == AssetKind::Animation) ClipReference = Record.Reference.Value;
		Check(!RigReference.empty() && !ClipReference.empty(), "rig fixture has a skinned mesh and clip");

		auto CharacterValue = std::make_shared<Character>();
		CharacterValue->SetCFrame(CFrame(0.0f, 0.0f, -4.0f));
		CharacterValue->SetParent(World);
		auto Rig = std::make_shared<MeshPart>();
		Rig->SetMesh(RigReference);
		Rig->SetAnchored(true);
		Rig->SetCanCollide(false);
		Rig->SetCFrame(CFrame(0.0f, 0.0f, -4.0f));
		Rig->SetParent(CharacterValue);
		CharacterValue->SetRootPart(Rig);
		auto Socket = std::make_shared<Attachment>();
		Socket->SetJointPath("BeaconRoot/BeaconTip");
		Socket->SetParent(Rig);
		auto Visual = std::make_shared<AttachmentMesh>();
		CheckInvalid([&] { Visual->SetMesh("assets/accessory.obj"); },
			"raw filesystem paths are not visual asset references");
		CheckInvalid([&] { Visual->SetSize({0.0f, 1.0f, 1.0f}); },
			"zero visual scale is rejected");
		CheckInvalid([&] { Visual->SetTransparency(2.0f); },
			"out-of-range transparency is rejected");
		Visual->SetMesh(AccessoryImport.Record->Reference.Value);
		Visual->SetCFrame(CFrame(0.0f, 0.0f, 2.0f));
		Visual->SetSize({1.0f, 2.0f, 1.0f});
		Visual->SetParent(Socket);
		auto Spatial = std::make_shared<SemanticSpatialResolver>(Assets);
		// Match EditorHost's lazy resolver: the producer discovers the visual;
		// this test does not manually register the socket or accessory.
		RenderPublisher Publisher;
		Publisher.SetSpatialResolver(Spatial);
		auto MeshChanges = Assets->DrainMeshChanges();
		Publisher.SetAssetMeshChanges(std::move(MeshChanges.Creates), std::move(MeshChanges.Removes));
		const auto Camera = MakeLookAtRenderCameraInput({0, 0, 0}, {0, 0, -4}, {0, 1, 0});
		auto First = Publisher.Publish(*World, Camera, 640, 360);
		const auto FirstVisual = FindCreate(*First, Visual->GetObjectId());
		const auto Semantic = Spatial->ResolveAttachment(Visual);
		Check(First->FullResync && First->VisualAttachmentsComplete && FirstVisual && FirstVisual->Mesh &&
			Semantic && Semantic->Animated && FirstVisual->Item.Object == Visual->GetObjectId(),
			"rigid visual publishes at a live canonical joint socket with its own picking identity");
		const auto Expected = Semantic->Matrix * glm::scale(glm::mat4(1.0f), Visual->GetSize());
		Check(Near(FirstVisual->Item.ModelMatrix, Expected),
			"visual publication preserves the semantic matrix including scale");
		RenderProjection Projection;
		(void)Projection.Apply(*First);
		Check(Projection.GetItem(Visual->GetObjectId()) && Projection.AreVisualAttachmentsComplete(),
			"renderer-owned projection receives an immutable visual value");
		RenderPublication PickOnly = *First;
		std::erase_if(PickOnly.Creates, [&](const RenderObjectCreate &Create) {
			return Create.Item.Object != Visual->GetObjectId();
		});
		RenderProjection PickProjection;
		(void)PickProjection.Apply(PickOnly);
		const auto Clip = PickOnly.Frame.Camera.ViewProjectionMatrix *
			FirstVisual->Item.ModelMatrix * glm::vec4(0, 0, 0, 1);
		const auto PickX = ((Clip.x / Clip.w) * 0.5f + 0.5f) * 640.0f - 0.5f;
		const auto PickY = (0.5f - (Clip.y / Clip.w) * 0.5f) * 360.0f - 0.5f;
		const auto Pick = PickEditorViewport(PickProjection, PickX, PickY);
		Check(Pick && Pick->Object == Visual->GetObjectId(),
			"viewport picking returns the accessory identity rather than its socket or owner");

		const auto BeforeRevision = Game->GetAuthoritativeRevision();
		const auto BeforeJournal = ChangeJournal::Get().GetRetentionWindow(Game->GetObjectId()).NextSequence;
		Spatial->Step();
		auto Idle = Publisher.Publish(*World, Camera, 640, 360);
		Check(!Idle->FullResync && !FindUpdate(*Idle, Visual->GetObjectId()) &&
			Game->GetAuthoritativeRevision() == BeforeRevision &&
			ChangeJournal::Get().GetRetentionWindow(Game->GetObjectId()).NextSequence == BeforeJournal,
			"idle socket publication does not modify authored state or history");
		Rig->SetCFrame(CFrame(1.0f, 0.0f, -4.0f));
		Spatial->Step();
		auto Moved = Publisher.Publish(*World, Camera, 640, 360);
		const auto MovedVisual = FindUpdate(*Moved, Visual->GetObjectId());
		Check(!Moved->FullResync && MovedVisual &&
			std::abs(MovedVisual->Item.ModelMatrix[3].x - FirstVisual->Item.ModelMatrix[3].x - 1.0f) < 0.0002f &&
			Visual->GetCFrame().Position.z == 2.0f,
			"static RootPart motion updates the accessory incrementally without changing its authored offset");
		(void)Projection.Apply(*Idle);
		(void)Projection.Apply(*Moved);
		Rig->SetSize({2.0f, 1.0f, 1.0f});
		auto ResizedOwner = Publisher.Publish(*World, Camera, 640, 360);
		const auto ResizedVisual = FindUpdate(*ResizedOwner, Visual->GetObjectId());
		Check(ResizedVisual,
			"owner Size dirtiness reaches the dependent visual through the reverse index");
		const auto BeforeSocketOffset = ResizedVisual->Item.ModelMatrix;
		Socket->SetCFrame(CFrame(0.25f, 0.0f, 0.0f));
		Spatial->Step();
		auto OffsetSocket = Publisher.Publish(*World, Camera, 640, 360);
		const auto OffsetItem = FindUpdate(*OffsetSocket, Visual->GetObjectId());
		Check(OffsetItem && !Near(OffsetItem->Item.ModelMatrix, BeforeSocketOffset) &&
			Near(OffsetItem->Item.ModelMatrix,
				Spatial->ResolveAttachment(Visual)->Matrix * glm::scale(glm::mat4(1.0f), Visual->GetSize())),
			"lazy registered parent CFrame invalidates the visual's cached semantic matrix");
		Socket->SetJointPath("BeaconRoot");
		Spatial->Step();
		auto SwitchedSocket = Publisher.Publish(*World, Camera, 640, 360);
		const auto SwitchedItem = FindUpdate(*SwitchedSocket, Visual->GetObjectId());
		Check(SwitchedItem && !Near(SwitchedItem->Item.ModelMatrix, OffsetItem->Item.ModelMatrix) &&
			Near(SwitchedItem->Item.ModelMatrix,
				Spatial->ResolveAttachment(Visual)->Matrix * glm::scale(glm::mat4(1.0f), Visual->GetSize())),
			"lazy registered parent JointPath switch updates the visual on an incremental publication");
		Socket->SetJointPath("BeaconRoot/BeaconTip");
		Socket->SetCFrame(CFrame());
		Spatial->Step();
		(void)Publisher.Publish(*World, Camera, 640, 360);
		auto AnimatorValue = std::make_shared<Animator>();
		AnimatorValue->SetParent(Rig);
		AnimationRuntime Animation(Assets);
		Animation.RegisterAnimator(AnimatorValue);
		auto Track = AnimatorValue->CreateTrack(ClipReference);
		Track->Play();
		Track->SetTimePosition(0.0f);
		auto AnimatedSpatial = std::make_shared<SemanticSpatialResolver>(Assets, &Animation);
		AnimatedSpatial->RegisterAttachment(Socket);
		AnimatedSpatial->RegisterAttachment(Visual);
		Publisher.SetSpatialResolver(AnimatedSpatial);
		Animation.Step(0.0f);
		AnimatedSpatial->Step();
		Publisher.SetAnimationPoseChanges(Animation.GetPoseUpdates(), Animation.GetPoseRemoves());
		Animation.ClearChanges();
		auto PoseStart = Publisher.Publish(*World, Camera, 640, 360);
		const auto PoseStartItem = FindCreate(*PoseStart, Visual->GetObjectId());
		Check(PoseStartItem, "animated resolver publishes the initial socket pose");
		const auto PoseHistoryRevision = Game->GetAuthoritativeRevision();
		const auto PoseHistorySequence = ChangeJournal::Get().GetRetentionWindow(Game->GetObjectId()).NextSequence;
		Track->SetTimePosition(0.5f);
		Animation.Step(0.0f);
		AnimatedSpatial->Step();
		Publisher.SetAnimationPoseChanges(Animation.GetPoseUpdates(), Animation.GetPoseRemoves());
		Animation.ClearChanges();
		auto PoseMoved = Publisher.Publish(*World, Camera, 640, 360);
		const auto PoseMovedItem = FindUpdate(*PoseMoved, Visual->GetObjectId());
		Check(PoseMovedItem && !Near(PoseStartItem->Item.ModelMatrix, PoseMovedItem->Item.ModelMatrix) &&
			Game->GetAuthoritativeRevision() == PoseHistoryRevision &&
			ChangeJournal::Get().GetRetentionWindow(Game->GetObjectId()).NextSequence == PoseHistorySequence,
			"animated socket follows pose without persisting transient motion or changing authored history");
		const auto SemanticBeforePresentation = AnimatedSpatial->ResolveAttachment(Visual);
		CharacterValue->ApplyRuntimePresentationOffset(CFrame(0.0f, 0.0f, 1.0f));
		auto Presented = Publisher.Publish(*World, Camera, 640, 360);
		const auto PresentedItem = FindUpdate(*Presented, Visual->GetObjectId());
		const auto SemanticAfterPresentation = AnimatedSpatial->ResolveAttachment(Visual);
		Check(PresentedItem && SemanticBeforePresentation && SemanticAfterPresentation &&
			Near(SemanticBeforePresentation->Matrix, SemanticAfterPresentation->Matrix) &&
			std::abs(PresentedItem->Item.ModelMatrix[3].z - PoseMovedItem->Item.ModelMatrix[3].z - 1.0f) < 0.0002f &&
			Game->GetAuthoritativeRevision() == PoseHistoryRevision,
			"presentation offset moves only the visual projection, preserving semantic and authored transforms");
		CharacterValue->ApplyRuntimePresentationOffset(std::nullopt);
		(void)Publisher.Publish(*World, Camera, 640, 360);
		Visual->SetColor(Color3(0.25f, 0.5f, 1.0f));
		Visual->SetTransparency(0.5f);
		Visual->SetCastShadow(false);
		auto Styled = Publisher.Publish(*World, Camera, 640, 360);
		const auto StyledItem = FindUpdate(*Styled, Visual->GetObjectId());
		Check(StyledItem && !StyledItem->Item.CastShadow &&
			std::abs(StyledItem->Item.Color.a - 0.5f) < 0.0002f,
			"authoritative color, opacity, and shadow changes update immutable visual values");
		auto Alternate = std::make_shared<AttachmentMesh>();
		Alternate->SetMesh(AccessoryImport.Record->Reference.Value);
		Alternate->SetVisible(false);
		Alternate->SetParent(Socket);
		AnimatedSpatial->RegisterAttachment(Alternate);
		AnimatedSpatial->Step();
		Visual->SetVisible(false);
		Alternate->SetVisible(true);
		auto Hidden = Publisher.Publish(*World, Camera, 640, 360);
		Check(std::ranges::any_of(Hidden->Removes, [&](const RenderObjectRemove &Remove) {
			return Remove.Object == Visual->GetObjectId();
		}) && FindCreate(*Hidden, Alternate->GetObjectId()),
			"authoritative sibling variant selection replaces the projected accessory");
		Alternate->SetVisible(false);
		Visual->SetVisible(true);
		Alternate->Destroy();
		Visual->SetMesh("asset://00000000000000000000000000000002");
		auto Missing = Publisher.Publish(*World, Camera, 640, 360);
		Check(!FindCreate(*Missing, Visual->GetObjectId()) && !Missing->Diagnostics.empty(),
			"missing asset fails closed with a bounded extraction diagnostic");
		Visual->SetMesh(ClipReference);
		auto WrongKind = Publisher.Publish(*World, Camera, 640, 360);
		Check(!FindCreate(*WrongKind, Visual->GetObjectId()) && !WrongKind->Diagnostics.empty(),
			"an Animation reference cannot be rendered as a rigid Mesh");
		Visual->SetMesh(AccessoryImport.Record->Reference.Value);
		auto Restored = Publisher.Publish(*World, Camera, 640, 360);
		Check(FindCreate(*Restored, Visual->GetObjectId()),
			"restoring a canonical mesh reference republishes the visible accessory");
		WriteText(Root / "assets" / "accessory.obj",
			"v -0.5 -0.5 0\nv 1 -0.5 0\nv 0 0.5 0\nf 1 2 3\n");
		Check(Assets->ReimportProjectAsset(Mount, AccessoryImport.Record->Reference.Value).Ok,
			"canonical accessory reimport succeeds");
		auto ReimportMeshes = Assets->DrainMeshChanges();
		Publisher.SetAssetMeshChanges(std::move(ReimportMeshes.Creates), std::move(ReimportMeshes.Removes));
		auto Reimported = Publisher.Publish(*World, Camera, 640, 360);
		Check(FindUpdate(*Reimported, Visual->GetObjectId()) &&
			FindUpdate(*Reimported, Visual->GetObjectId())->Mesh &&
			Reimported->VisualAttachmentsComplete,
			"reimport dirties the dependent visual without rescanning the scene");
		Visual->SetMaterial("asset://00000000000000000000000000000001");
		auto MissingMaterial = Publisher.Publish(*World, Camera, 640, 360);
		Check(std::ranges::any_of(MissingMaterial->Removes, [&](const RenderObjectRemove &Remove) {
			return Remove.Object == Visual->GetObjectId();
		}), "missing material removes the prior accessory instead of retaining stale state");
		Visual->SetMaterial("");
		Socket->SetJointPath("BeaconRoot/Missing");
		auto InvalidSocket = Publisher.Publish(*World, Camera, 640, 360);
		Check(!FindCreate(*InvalidSocket, Visual->GetObjectId()) && !InvalidSocket->Diagnostics.empty(),
			"unavailable joint path fails closed");
		Socket->SetJointPath("BeaconRoot/BeaconTip");
		AnimatedSpatial->Step();
		Check(FindCreate(*Publisher.Publish(*World, Camera, 640, 360), Visual->GetObjectId()),
			"restored socket republishes without changing physics authority");

		Visual->SetArchivable(true);
		std::shared_ptr<Instance> PersistedRoot = Visual;
		const auto Serialized = InstanceSerialization::Serialize(
			InstanceSerialization::InstanceFormat::Json, PersistedRoot);
		std::istringstream Input(Serialized);
		const auto Reopened = InstanceSerialization::Deserialize(
			InstanceSerialization::InstanceFormat::Json, Input);
		const auto ReopenedVisual = std::dynamic_pointer_cast<AttachmentMesh>(Reopened.Instance);
		Check(Reopened.Ok && ReopenedVisual &&
			ReopenedVisual->GetMesh() == Visual->GetMesh() &&
			ReopenedVisual->GetSize() == Visual->GetSize() &&
			Serialized.find("WorldCFrame") == std::string::npos,
			"save and reopen preserves only authored visual and socket properties");

		Visual->SetParent(nullptr);
		auto Detached = Publisher.Publish(*World, Camera, 640, 360);
		Check(std::ranges::any_of(Detached->Removes, [&](const RenderObjectRemove &Remove) {
			return Remove.Object == Visual->GetObjectId();
		}), "reparenting outside the world removes the accessory by generation-safe identity");
		Visual->SetParent(Socket);
		AnimatedSpatial->Step();
		Check(FindCreate(*Publisher.Publish(*World, Camera, 640, 360), Visual->GetObjectId()),
			"reattaching a live accessory recreates its projection");
		Visual->Destroy();
		auto Destroyed = Publisher.Publish(*World, Camera, 640, 360);
		Check(std::ranges::any_of(Destroyed->Removes, [&](const RenderObjectRemove &Remove) {
			return Remove.Object == Visual->GetObjectId();
		}), "destroying an accessory removes the stale projection");

		// Full discovery is bounded by the native scene bound, not by the number
		// of ordinary objects preceding an accessory. Incremental frames use the
		// reverse dependency index and do no whole-scene visual traversal.
		for (std::size_t Index = 0; Index < 8192; ++Index) {
			auto Ordinary = std::make_shared<Folder>();
			Ordinary->SetParent(World);
		}
		Publisher.RequestFullResync();
		auto Large = Publisher.Publish(*World, Camera, 640, 360);
		Check(Large->VisualAttachmentsComplete && Publisher.GetLastProfile().VisualTraversalObjects >= 8192,
			"large ordinary scene with zero accessories remains a complete publication");
		auto LargeIdle = Publisher.Publish(*World, Camera, 640, 360);
		Check(!LargeIdle->FullResync && Publisher.GetLastProfile().VisualTraversalObjects == 0 &&
			Publisher.GetLastProfile().VisualDependencyExpansions == 0,
			"large ordinary scene performs no per-frame visual scan");
		auto FirstBounded = std::make_shared<AttachmentMesh>();
		FirstBounded->SetMesh(AccessoryImport.Record->Reference.Value);
		FirstBounded->SetParent(Socket);
		AnimatedSpatial->RegisterAttachment(FirstBounded);
		AnimatedSpatial->Step();
		auto BeforeOverflow = Publisher.Publish(*World, Camera, 640, 360);
		Check(FindCreate(*BeforeOverflow, FirstBounded->GetObjectId()),
			"an indexed visual remains publishable after a large zero-accessory scene");
		Publisher.RequestFullResync();
		auto LargeWithVisual = Publisher.Publish(*World, Camera, 640, 360);
		Check(LargeWithVisual->FullResync && LargeWithVisual->VisualAttachmentsComplete &&
			FindCreate(*LargeWithVisual, FirstBounded->GetObjectId()),
			"full discovery finds a visual after thousands of ordinary scene objects");
		std::vector<std::shared_ptr<AttachmentMesh>> Many;
		Many.reserve(1024);
		for (std::size_t Index = 0; Index < 1024; ++Index) {
			auto Extra = std::make_shared<AttachmentMesh>();
			Extra->SetMesh(AccessoryImport.Record->Reference.Value);
			Extra->SetParent(Socket);
			Many.push_back(std::move(Extra));
		}
		auto Overflow = Publisher.Publish(*World, Camera, 640, 360);
		Check(Overflow->FullResync && !Overflow->VisualAttachmentsComplete &&
			!FindCreate(*Overflow, FirstBounded->GetObjectId()) &&
			std::ranges::none_of(Overflow->Creates, [&](const RenderObjectCreate &Create) {
				return std::ranges::any_of(Many, [&](const auto &Extra) {
					return Create.Item.Object == Extra->GetObjectId();
				});
			}), "visual bound omits the entire subset and explicitly marks publication incomplete");
		RenderProjection OverflowProjection;
		(void)OverflowProjection.Apply(*LargeWithVisual);
		(void)OverflowProjection.Apply(*Overflow);
		Check(!OverflowProjection.GetItem(FirstBounded->GetObjectId()) &&
			!OverflowProjection.AreVisualAttachmentsComplete(),
			"overflow full resync removes an accessory from the previous projection");
		Many.back()->Destroy();
		Many.pop_back();
		AnimatedSpatial->Step();
		auto Recovered = Publisher.Publish(*World, Camera, 640, 360);
		Check(Recovered->FullResync && Recovered->VisualAttachmentsComplete &&
			FindCreate(*Recovered, FirstBounded->GetObjectId()),
			"falling below the visual bound recovers complete publication");

		// AssetService must treat AttachmentMesh as a canonical live reference
		// for both source-group deletion and generated-child retirement.
		WriteText(Root / "assets" / "deletable.obj",
			"v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
		const auto Deletable = Assets->ImportProjectAsset(Mount, "assets/deletable.obj", AssetKind::Mesh);
		Check(Deletable.Ok && Deletable.Record, "independent deletion fixture imports");
		std::ifstream RigSource(GARGANTUAN_ANIMATION_PREVIEW_FIXTURE, std::ios::binary);
		auto MaterialSource = nlohmann::ordered_json::parse(RigSource);
		MaterialSource["materials"] = nlohmann::ordered_json::array({
			{{"name", "Base"}, {"pbrMetallicRoughness", {{"baseColorFactor", {1.0, 1.0, 1.0, 1.0}}}}},
			{{"name", "Orphan"}, {"pbrMetallicRoughness", {{"baseColorFactor", {1.0, 0.0, 1.0, 1.0}}}}},
		});
		MaterialSource["meshes"][0]["primitives"][0]["material"] = 0;
		WriteText(Root / "assets" / "material-source.gltf", MaterialSource.dump());
		const auto MaterialImport = Assets->ImportProjectAsset(
			Mount, "assets/material-source.gltf", AssetKind::Mesh);
		Check(MaterialImport.Ok && MaterialImport.Record,
			"generated-material retirement fixture imports");
		std::string OrphanMaterial;
		for (const auto &Record : MaterialImport.Records)
			if (Record.Kind == AssetKind::Material && Record.Name == "Orphan")
				OrphanMaterial = Record.Reference.Value;
		Check(!OrphanMaterial.empty(), "fixture exposes a generated orphan Material");
		auto GuardVisual = std::make_shared<AttachmentMesh>();
		GuardVisual->SetMesh(Deletable.Record->Reference.Value);
		GuardVisual->SetMaterial(OrphanMaterial);
		GuardVisual->SetParent(Socket);
		const auto BlockedMeshDelete = Assets->DeleteProjectAsset(Deletable.Record->Reference.Value);
		const auto BlockedMaterialDelete = Assets->DeleteProjectAsset(MaterialImport.Record->Reference.Value);
		Check(!BlockedMeshDelete.Ok && BlockedMeshDelete.Diagnostic.Code == "AssetReferenced" &&
			!BlockedMaterialDelete.Ok && BlockedMaterialDelete.Diagnostic.Code == "AssetReferenced",
			"AttachmentMesh Mesh and explicit Material refs both block source-group deletion");
		MaterialSource["materials"].erase(MaterialSource["materials"].begin() + 1);
		WriteText(Root / "assets" / "material-source.gltf", MaterialSource.dump());
		const auto BlockedRetirement = Assets->ReimportProjectAsset(Mount, MaterialImport.Record->Reference.Value);
		Check(!BlockedRetirement.Ok && BlockedRetirement.Diagnostic.Code == "AssetReferenced" &&
			Assets->GetAsset(OrphanMaterial).has_value(),
			"reimport cannot retire a generated Material referenced only by AttachmentMesh");
		GuardVisual->SetMaterial("");
		const auto ReleasedRetirement = Assets->ReimportProjectAsset(Mount, MaterialImport.Record->Reference.Value);
		Check(ReleasedRetirement.Ok && !Assets->GetAsset(OrphanMaterial),
			"generated Material retires after the visual releases its reference");
		GuardVisual->Destroy();
		Check(Assets->DeleteProjectAsset(Deletable.Record->Reference.Value).Ok,
			"source group can be deleted after the visual is destroyed");
		TestIncompatibleReimportFence(Root);
		std::cout << "[Creator:AttachmentMesh] All focused checks passed\n";
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Creator:AttachmentMesh] FAIL: " << Error.what() << '\n';
		return 1;
	}
}
