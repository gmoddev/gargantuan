// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/MeshPart.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/SourceMount.hpp"
#include "gargantuan/network/ReplicaApplier.hpp"
#include "gargantuan/network/ReplicationCoordinator.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/RenderExtractor.hpp"
#include "gargantuan/render/RenderProjection.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <chrono>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace {
	using namespace gargantuan;
	using namespace gargantuan::network;
	using Clock = std::chrono::steady_clock;

	struct Options {
		std::size_t Count = 128;
		std::size_t Changed = 16;
		std::size_t Frames = 8;
		std::string Mode = "both";
	};

	struct TemporaryRoot {
		std::filesystem::path Path;
		~TemporaryRoot() {
			std::error_code Ignored;
			const auto Parent = Path.lexically_normal().parent_path();
			const auto Temporary = std::filesystem::temp_directory_path(Ignored).lexically_normal();
			if (!Ignored && Parent == Temporary &&
				Path.filename().string().starts_with("gargantuan-runtime6-mesh-"))
				std::filesystem::remove_all(Path, Ignored);
		}
	};

	void Require(bool Condition, std::string_view Message) {
		if (!Condition) throw std::runtime_error("[Runtime6:RealisticPublication] " + std::string(Message));
	}

	double Milliseconds(Clock::duration Duration) {
		return std::chrono::duration<double, std::milli>(Duration).count();
	}

	bool EqualItem(const RenderItem &Left, const RenderItem &Right) {
		return Left.Object == Right.Object && Left.Geometry == Right.Geometry &&
			Left.ModelMatrix == Right.ModelMatrix && Left.InverseModelMatrix == Right.InverseModelMatrix &&
			Left.Color == Right.Color && Left.CastShadow == Right.CastShadow;
	}

	bool EqualMaterial(const RenderMaterialState &Left, const RenderMaterialState &Right) {
		return Left.Revision == Right.Revision && Left.BaseColorFactor == Right.BaseColorFactor &&
			Left.BaseColorTexture == Right.BaseColorTexture && Left.NormalTexture == Right.NormalTexture &&
			Left.Metallic == Right.Metallic && Left.Roughness == Right.Roughness &&
			Left.OpacityMode == Right.OpacityMode && Left.AlphaCutoff == Right.AlphaCutoff &&
			Left.DoubleSided == Right.DoubleSided && Left.ShadingModel == Right.ShadingModel &&
			Left.MetallicRoughnessTexture == Right.MetallicRoughnessTexture &&
			Left.EmissiveFactor == Right.EmissiveFactor &&
			Left.EmissiveTexture == Right.EmissiveTexture &&
			Left.OcclusionTexture == Right.OcclusionTexture &&
			Left.OcclusionStrength == Right.OcclusionStrength &&
			Left.NormalScale == Right.NormalScale && Left.TextureSamplers == Right.TextureSamplers;
	}

	bool EqualProjected(const RenderProjectedObject &Left, const RenderProjectedObject &Right) {
		if (!EqualItem(Left.Item, Right.Item) || Left.Mesh != Right.Mesh ||
			!EqualMaterial(Left.Material, Right.Material) || Left.Visible != Right.Visible ||
			static_cast<bool>(Left.Primitives) != static_cast<bool>(Right.Primitives))
			return false;
		if (!Left.Primitives) return true;
		if (Left.Primitives->size() != Right.Primitives->size()) return false;
		for (std::size_t Index = 0; Index < Left.Primitives->size(); ++Index) {
			const auto &A = Left.Primitives->at(Index);
			const auto &B = Right.Primitives->at(Index);
			if (A.FirstIndex != B.FirstIndex || A.IndexCount != B.IndexCount ||
				!EqualMaterial(A.Material, B.Material)) return false;
		}
		return true;
	}

	void Print(
		std::string_view Mode, std::string_view Phase, std::size_t Frame, std::size_t Count,
		std::size_t Changed, double SourceMs, double ProduceMs, double ApplyMs,
		double PublishMs, double ProjectionMs, const RenderPublication &Publication
	) {
		std::cout << Mode << ',' << Phase << ',' << Frame << ',' << Count << ',' << Changed << ','
			<< std::fixed << std::setprecision(6) << SourceMs << ',' << ProduceMs << ',' << ApplyMs
			<< ',' << PublishMs << ',' << ProjectionMs << ',' << Publication.Creates.size()
			<< ',' << Publication.Updates.size() << ',' << Publication.Removes.size()
			<< ',' << Publication.MeshCreates.size() << ',' << Publication.MeshRemoves.size()
			<< ',' << Publication.FullResync << '\n' << std::flush;
	}

	RenderPublicationPtr VerifyFullState(
		RenderPublisher &Publisher, RenderProjection &Projection, const Workspace &World,
		const std::vector<ObjectId> &LiveIds
	) {
		Require(Projection.GetObjects().size() == LiveIds.size(),
			"retained projection contains an extra or missing object before resync");
		const auto RetainedMeshCount = Projection.GetMeshCount();
		const auto RetainedTextureCount = Projection.GetTextureCount();
		std::unordered_map<ObjectId, RenderProjectedObject> Previous;
		Previous.reserve(LiveIds.size());
		for (const auto Object : LiveIds) {
			const auto Found = Projection.GetObjects().find(Object);
			Require(Found != Projection.GetObjects().end(), "projected live identity is absent");
			Previous.emplace(Object, Found->second);
		}
		Publisher.RequestFullResync();
		const auto Complete = Publisher.Publish(World, RenderCameraInput{}, 640, 360);
		Require(Complete->FullResync, "explicit full resync was not published");
		std::unordered_map<ObjectId, RenderProjectedObject> Reconstructed;
		Reconstructed.reserve(LiveIds.size());
		for (const auto &Create : Complete->Creates)
			Reconstructed.emplace(Create.Item.Object, RenderProjectedObject{
				Create.Item, Create.Mesh, Create.Material, Create.Visible, Create.Primitives});
		Require(Reconstructed.size() == LiveIds.size(), "full resync object count differs from live identities");
		for (const auto &[Object, Item] : Previous) {
			const auto Found = Reconstructed.find(Object);
			Require(Found != Reconstructed.end() && EqualProjected(Item, Found->second),
				"incremental object state differs from authoritative full resync");
		}
		RenderProjection Fresh;
		(void)Fresh.Apply(*Complete);
		Require(Fresh.GetObjects().size() == LiveIds.size(), "fresh projection rejects complete reconstruction");
		Require(Fresh.GetMeshCount() == RetainedMeshCount &&
			Fresh.GetTextureCount() == RetainedTextureCount,
			"retained resource counts differ from complete reconstruction");
		for (const auto Object : LiveIds)
			Require(EqualProjected(Projection.GetObjects().at(Object), Fresh.GetObjects().at(Object)),
				"fresh projection differs from retained incremental projection");
		(void)Projection.Apply(*Complete);
		return Complete;
	}

	void WriteMesh(const std::filesystem::path &Path, float X) {
		std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
		Require(Output.good(), "mesh fixture file could not be opened");
		Output << "v 0 0 0\nv " << X << " 0 0\nv 0 1 0\n"
			"vt 0 0\nvt 1 0\nvt 0 1\nf 1/1 2/2 3/3\n";
		Require(Output.good(), "mesh fixture file could not be written");
	}

	void RunMesh(const Options &Config) {
		ChangeJournal::Get().Clear();
		RenderDirtyAccumulator::Get().Clear();
		const TemporaryRoot Temporary{
			std::filesystem::temp_directory_path() /
				("gargantuan-runtime6-mesh-" + std::to_string(Clock::now().time_since_epoch().count()))
		};
		std::filesystem::create_directories(Temporary.Path / "assets");
		WriteMesh(Temporary.Path / "assets" / "shared.obj", 1.0f);
		DiskFilesystem Filesystem(Temporary.Path);
		SourceMount Mount(Filesystem);
		auto Game = std::make_shared<DataModel>();
		Game->Root = Temporary.Path;
		Game->Filesystem = &Filesystem;
		auto Assets = std::dynamic_pointer_cast<AssetService>(Game->GetService("AssetService"));
		auto World = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
		Require(Assets && World, "asset or Workspace service is unavailable");
		const auto Imported = Assets->ImportProjectAsset(Mount, "assets/shared.obj", AssetKind::Mesh);
		Require(Imported.Ok && Imported.Record, "canonical shared mesh import failed");
		const auto Reference = Imported.Record->Reference.Value;
		std::vector<std::shared_ptr<MeshPart>> Parts;
		std::vector<ObjectId> LiveIds;
		Parts.reserve(Config.Count);
		LiveIds.reserve(Config.Count);
		for (std::size_t Index = 0; Index < Config.Count; ++Index) {
			auto Value = std::make_shared<MeshPart>();
			Value->SetMesh(Reference);
			Value->SetAnchored(true);
			Value->SetCollisionMode(Enums::MeshCollisionMode::Box);
			Value->SetCFrame(CFrame(glm::vec3(static_cast<float>(Index), 0.0f, 0.0f)));
			Value->SetParent(World);
			LiveIds.push_back(Value->GetObjectId());
			Parts.push_back(std::move(Value));
		}
		RenderPublisher Publisher;
		auto MeshChanges = Assets->DrainMeshChanges();
		Publisher.SetAssetMeshChanges(std::move(MeshChanges.Creates), std::move(MeshChanges.Removes));
		RenderProjection Projection;
		const auto Initial = Publisher.Publish(*World, RenderCameraInput{}, 640, 360);
		Require(Initial->FullResync && Initial->Creates.size() == Config.Count &&
			Initial->MeshCreates.size() == 1, "initial MeshPart publication count is wrong");
		(void)Projection.Apply(*Initial);
		Require(Projection.GetObjects().size() == Config.Count, "initial MeshPart projection count is wrong");

		for (std::size_t Frame = 0; Frame < Config.Frames; ++Frame) {
			const auto SourceStart = Clock::now();
			for (std::size_t Index = 0; Index < Config.Changed; ++Index) {
				const auto PartIndex = (Frame * Config.Changed + Index) % Config.Count;
				auto CFrameValue = Parts[PartIndex]->GetCFrame();
				CFrameValue.Position.z = static_cast<float>(Frame + 1) * 0.25f;
				Parts[PartIndex]->SetCFrame(CFrameValue);
				if (Frame % 2 == 1)
					Parts[PartIndex]->SetColor(Color3(0.25f, 0.5f, static_cast<float>(Frame + 1) / 64.0f));
			}
			const auto SourceMs = Milliseconds(Clock::now() - SourceStart);
			const auto PublishStart = Clock::now();
			const auto Publication = Publisher.Publish(*World, RenderCameraInput{}, 640, 360);
			const auto PublishMs = Milliseconds(Clock::now() - PublishStart);
			Require(!Publication->FullResync && Publication->Creates.empty() &&
				Publication->Removes.empty() && Publication->Updates.size() == Config.Changed &&
				Publication->MeshCreates.empty() && Publication->MeshRemoves.empty(),
				"MeshPart incremental count differs from changed identities");
			const auto ProjectionStart = Clock::now();
			(void)Projection.Apply(*Publication);
			const auto ProjectionMs = Milliseconds(Clock::now() - ProjectionStart);
			if (Config.Changed == 0) {
				Require(Projection.GetObjects().size() == LiveIds.size(),
					"quiet MeshPart frame changed projected object count");
				for (const auto Object : LiveIds) {
					const auto Found = Projection.GetObjects().find(Object);
					Require(Found != Projection.GetObjects().end() && Found->second.Mesh &&
						*Found->second.Mesh == Initial->MeshCreates.front().Mesh,
						"quiet MeshPart frame changed a stable resource binding");
				}
			}
			for (const auto &Update : Publication->Updates) {
				const auto Found = Projection.GetObjects().find(Update.Object);
				Require(Found != Projection.GetObjects().end() &&
					EqualItem(Found->second.Item, Update.Item), "MeshPart projection lost final update");
			}
			Print("mesh", "steady", Frame, Config.Count, Config.Changed,
				SourceMs, 0.0, 0.0, PublishMs, ProjectionMs, *Publication);
		}
		(void)VerifyFullState(Publisher, Projection, *World, LiveIds);

		const auto BeforeMesh = Assets->ResolveMeshResource(Reference);
		Require(BeforeMesh && BeforeMesh->Value.Vertices && BeforeMesh->Value.Vertices->size() >= 2,
			"pre-reimport canonical mesh is unavailable");
		WriteMesh(Temporary.Path / "assets" / "shared.obj", 2.0f);
		const auto ReimportStart = Clock::now();
		const auto Reimported = Assets->ReimportProjectAsset(Mount, Reference);
		const auto ReimportMs = Milliseconds(Clock::now() - ReimportStart);
		Require(Reimported.Ok, "canonical mesh reimport failed");
		const auto AfterMesh = Assets->ResolveMeshResource(Reference);
		bool VertexContentChanged = AfterMesh && AfterMesh->Value.Vertices &&
			AfterMesh->Value.Vertices->size() != BeforeMesh->Value.Vertices->size();
		if (AfterMesh && AfterMesh->Value.Vertices &&
			AfterMesh->Value.Vertices->size() == BeforeMesh->Value.Vertices->size()) {
			for (std::size_t Index = 0; Index < AfterMesh->Value.Vertices->size(); ++Index)
				if (AfterMesh->Value.Vertices->at(Index).Position !=
					BeforeMesh->Value.Vertices->at(Index).Position) VertexContentChanged = true;
		}
		Require(AfterMesh && AfterMesh->Value.Vertices && AfterMesh->Value.Indices &&
			AfterMesh->ContentRevision > BeforeMesh->ContentRevision &&
			AfterMesh->Mesh != BeforeMesh->Mesh && VertexContentChanged,
			"canonical mesh reimport did not advance exact resource content and generation");
		MeshChanges = Assets->DrainMeshChanges();
		Require(MeshChanges.Creates.size() == 1 && MeshChanges.Removes.size() == 1,
			"mesh reimport did not replace exactly one canonical resource");
		Publisher.SetAssetMeshChanges(std::move(MeshChanges.Creates), std::move(MeshChanges.Removes));
		const auto PublishStart = Clock::now();
		const auto ReimportPublication = Publisher.Publish(*World, RenderCameraInput{}, 640, 360);
		const auto PublishMs = Milliseconds(Clock::now() - PublishStart);
		Require(!ReimportPublication->FullResync && ReimportPublication->Updates.size() == Config.Count &&
			ReimportPublication->MeshCreates.size() == 1 && ReimportPublication->MeshRemoves.size() == 1,
			"asset revision did not invalidate every exact MeshPart dependent");
		const auto &CreatedMesh = ReimportPublication->MeshCreates.front();
		Require(ReimportPublication->MeshRemoves.front().Mesh == BeforeMesh->Mesh &&
			CreatedMesh.Mesh == AfterMesh->Mesh &&
			CreatedMesh.TopologyRevision == AfterMesh->ContentRevision &&
			CreatedMesh.VertexRevision == AfterMesh->ContentRevision &&
			CreatedMesh.Vertices == AfterMesh->Value.Vertices &&
			CreatedMesh.Indices == AfterMesh->Value.Indices &&
			CreatedMesh.Bounds.Minimum == AfterMesh->Value.Bounds.Minimum &&
			CreatedMesh.Bounds.Maximum == AfterMesh->Value.Bounds.Maximum,
			"mesh revision publication differs from exact canonical resource backing");
		for (const auto &Update : ReimportPublication->Updates)
			Require(Update.Mesh == AfterMesh->Mesh,
				"asset revision left a MeshPart bound to stale resource identity");
		const auto ProjectionStart = Clock::now();
		(void)Projection.Apply(*ReimportPublication);
		const auto ProjectionMs = Milliseconds(Clock::now() - ProjectionStart);
		const auto *ProjectedBounds = Projection.GetMeshBounds(AfterMesh->Mesh);
		Require(Projection.GetMeshCount() == 1 && ProjectedBounds &&
			ProjectedBounds->Minimum == AfterMesh->Value.Bounds.Minimum &&
			ProjectedBounds->Maximum == AfterMesh->Value.Bounds.Maximum,
			"projection retained stale mesh geometry bounds after reimport");
		Print("mesh", "asset_revision", Config.Frames, Config.Count, Config.Count,
			ReimportMs, 0.0, 0.0, PublishMs, ProjectionMs, *ReimportPublication);
		const auto CompleteAfterReimport = VerifyFullState(Publisher, Projection, *World, LiveIds);
		Require(CompleteAfterReimport->MeshCreates.size() == 1 &&
			CompleteAfterReimport->MeshCreates.front().Mesh == AfterMesh->Mesh &&
			CompleteAfterReimport->MeshCreates.front().TopologyRevision == AfterMesh->ContentRevision &&
			CompleteAfterReimport->MeshCreates.front().Vertices == AfterMesh->Value.Vertices &&
			CompleteAfterReimport->MeshCreates.front().Indices == AfterMesh->Value.Indices,
			"full resync did not reproduce current canonical mesh revision and payload");
		auto Nested = std::make_shared<Folder>();
		Nested->SetParent(World);
		auto StageStart = Clock::now();
		Parts.front()->SetParent(Nested);
		auto StageSourceMs = Milliseconds(Clock::now() - StageStart);
		StageStart = Clock::now();
		auto NestedPublication = Publisher.Publish(*World, RenderCameraInput{}, 640, 360);
		auto StagePublishMs = Milliseconds(Clock::now() - StageStart);
		Require(!NestedPublication->FullResync && NestedPublication->Updates.size() == 1,
			"nested MeshPart reparent did not preserve render membership");
		StageStart = Clock::now();
		(void)Projection.Apply(*NestedPublication);
		auto StageProjectionMs = Milliseconds(Clock::now() - StageStart);
		Print("mesh", "nested_reparent", Config.Frames + 1, Config.Count, 1,
			StageSourceMs, 0.0, 0.0, StagePublishMs, StageProjectionMs, *NestedPublication);
		StageStart = Clock::now();
		Parts.front()->SetParent(Game);
		StageSourceMs = Milliseconds(Clock::now() - StageStart);
		StageStart = Clock::now();
		auto RemovedPublication = Publisher.Publish(*World, RenderCameraInput{}, 640, 360);
		StagePublishMs = Milliseconds(Clock::now() - StageStart);
		Require(!RemovedPublication->FullResync && RemovedPublication->Removes.size() == 1 &&
			RemovedPublication->Removes.front().Object == LiveIds.front(),
			"MeshPart moved outside WorldRoot was not removed");
		StageStart = Clock::now();
		(void)Projection.Apply(*RemovedPublication);
		StageProjectionMs = Milliseconds(Clock::now() - StageStart);
		Print("mesh", "outside_world", Config.Frames + 2, Config.Count, 1,
			StageSourceMs, 0.0, 0.0, StagePublishMs, StageProjectionMs, *RemovedPublication);
		StageStart = Clock::now();
		Parts.front()->SetParent(Nested);
		StageSourceMs = Milliseconds(Clock::now() - StageStart);
		StageStart = Clock::now();
		auto RestoredPublication = Publisher.Publish(*World, RenderCameraInput{}, 640, 360);
		StagePublishMs = Milliseconds(Clock::now() - StageStart);
		Require(!RestoredPublication->FullResync && RestoredPublication->Creates.size() == 1 &&
			RestoredPublication->Creates.front().Item.Object == LiveIds.front(),
			"nested MeshPart return did not restore the same ObjectId");
		StageStart = Clock::now();
		(void)Projection.Apply(*RestoredPublication);
		StageProjectionMs = Milliseconds(Clock::now() - StageStart);
		Print("mesh", "return_world", Config.Frames + 3, Config.Count, 1,
			StageSourceMs, 0.0, 0.0, StagePublishMs, StageProjectionMs, *RestoredPublication);
		(void)VerifyFullState(Publisher, Projection, *World, LiveIds);
		Game->Destroy();
	}

	void RunReplica(const Options &Config) {
		ChangeJournal::Get().Clear();
		RenderDirtyAccumulator::Get().Clear();
		auto Server = std::make_shared<DataModel>();
		auto ServerWorld = std::dynamic_pointer_cast<Workspace>(Server->GetService("Workspace"));
		Require(ServerWorld != nullptr, "server Workspace is unavailable");
		std::vector<std::shared_ptr<Part>> Parts;
		Parts.reserve(Config.Count);
		for (std::size_t Index = 0; Index < Config.Count; ++Index) {
			auto Value = std::make_shared<Part>();
			Value->SetCFrame(CFrame(glm::vec3(static_cast<float>(Index), 0.0f, 0.0f)));
			Value->SetParent(ServerWorld);
			Parts.push_back(std::move(Value));
		}
		ReplicationCoordinator Coordinator(Server);
		const ConnectionId Peer{701, 1};
		ReplicaApplier Replica;
		const auto Baseline = Coordinator.AddPeer(Peer, ReplicationEpoch(1));
		Require(Baseline.Succeeded() && Baseline.Frame &&
			Replica.ApplyFrame(*Baseline.Frame).Succeeded(), "replica baseline failed");
		auto ReplicaGame = std::dynamic_pointer_cast<DataModel>(Replica.GetReplicaRoot());
		Require(ReplicaGame != nullptr, "replica root is not a DataModel");
		// Snapshot loading reconstructs the child hierarchy, not ServiceProvider's
		// lookup cache. Find the replicated service without creating another one.
		auto ReplicaWorld = std::dynamic_pointer_cast<Workspace>(
			ReplicaGame->FindFirstChildOfClass("Workspace", false));
		Require(ReplicaWorld && ReplicaWorld->GetName() == "Workspace",
			"live replica Workspace is unavailable");
		std::vector<ObjectId> LiveIds;
		LiveIds.reserve(Config.Count);
		for (const auto &Value : Parts) {
			auto ReplicaValue = std::dynamic_pointer_cast<Part>(Replica.Resolve(Value->GetObjectId()));
			Require(ReplicaValue != nullptr, "baseline omitted a replicated Part");
			LiveIds.push_back(ReplicaValue->GetObjectId());
		}
		RenderPublisher Publisher;
		RenderProjection Projection;
		const auto Initial = Publisher.Publish(*ReplicaWorld, RenderCameraInput{}, 640, 360);
		Require(Initial->FullResync && Initial->Creates.size() == Config.Count,
			"replica full publication count is wrong");
		(void)Projection.Apply(*Initial);
		Require(Projection.GetObjects().size() == Config.Count, "replica projection count is wrong");

		for (std::size_t Frame = 0; Frame < Config.Frames; ++Frame) {
			const auto SourceStart = Clock::now();
			for (std::size_t Index = 0; Index < Config.Changed; ++Index) {
				const auto PartIndex = (Frame * Config.Changed + Index) % Config.Count;
				auto CFrameValue = Parts[PartIndex]->GetCFrame();
				CFrameValue.Position.z = static_cast<float>(Frame + 1) * 0.25f;
				Parts[PartIndex]->SetCFrame(CFrameValue);
			}
			const auto SourceMs = Milliseconds(Clock::now() - SourceStart);
			const auto ProduceStart = Clock::now();
			const auto Delta = Coordinator.ProduceIncremental(Peer);
			const auto ProduceMs = Milliseconds(Clock::now() - ProduceStart);
			double ApplyMs = 0.0;
			if (Config.Changed == 0) {
				Require(!Delta.Succeeded() && !Delta.Frame &&
					Delta.Error == "No replication changes are available" &&
					Delta.SelectedTransitions == 0 && Delta.JournalRecordsExamined == 0 &&
					Delta.EncodedFrame.empty() && !Delta.DeferredForBytes,
					"quiet replica frame did not return the exact no-change status");
			} else {
				Require(Delta.Succeeded() && Delta.Frame, "replica incremental production failed");
				const auto ApplyStart = Clock::now();
				const auto Applied = Replica.ApplyFrame(*Delta.Frame);
				ApplyMs = Milliseconds(Clock::now() - ApplyStart);
				Require(Applied.Succeeded(), "replica incremental apply failed");
			}
			const auto PublishStart = Clock::now();
			const auto Publication = Publisher.Publish(*ReplicaWorld, RenderCameraInput{}, 640, 360);
			const auto PublishMs = Milliseconds(Clock::now() - PublishStart);
			Require(!Publication->FullResync && Publication->Creates.empty() &&
				Publication->Removes.empty() && Publication->Updates.size() == Config.Changed &&
				Publication->MeshCreates.empty() && Publication->MeshRemoves.empty(),
				"receiver publication count differs from network-originated changes");
			const auto ProjectionStart = Clock::now();
			(void)Projection.Apply(*Publication);
			const auto ProjectionMs = Milliseconds(Clock::now() - ProjectionStart);
			if (Config.Changed == 0) {
				Require(Projection.GetObjects().size() == LiveIds.size(),
					"quiet replica frame changed projected object count");
				for (std::size_t Index = 0; Index < Config.Count; ++Index) {
					auto ReplicaValue = std::dynamic_pointer_cast<Part>(
						Replica.Resolve(Parts[Index]->GetObjectId()));
					Require(ReplicaValue && ReplicaValue->GetObjectId() == LiveIds[Index] &&
						Projection.GetObjects().contains(LiveIds[Index]),
						"quiet replica frame changed a stable local identity");
				}
			}
			for (std::size_t Index = 0; Index < Config.Changed; ++Index) {
				const auto PartIndex = (Frame * Config.Changed + Index) % Config.Count;
				auto ReplicaValue = std::dynamic_pointer_cast<Part>(Replica.Resolve(Parts[PartIndex]->GetObjectId()));
				Require(ReplicaValue && ReplicaValue->GetCFrame().FuzzyEq(Parts[PartIndex]->GetCFrame()),
					"receiver authoritative transform differs from source");
				const auto Projected = Projection.GetObjects().find(ReplicaValue->GetObjectId());
				Require(Projected != Projection.GetObjects().end() &&
					Projected->second.Item.ModelMatrix[3].z == ReplicaValue->GetRenderCFrame().Position.z,
					"receiver projected transform differs from committed replica");
			}
			Print("replica", "steady", Frame, Config.Count, Config.Changed,
				SourceMs, ProduceMs, ApplyMs, PublishMs, ProjectionMs, *Publication);
		}
		(void)VerifyFullState(Publisher, Projection, *ReplicaWorld, LiveIds);
		const auto OldSource = Parts.front()->GetObjectId();
		const auto OldLocal = LiveIds.front();
		const auto LifecycleStart = Clock::now();
		Parts.front()->Destroy();
		auto Replacement = std::make_shared<Part>();
		Replacement->SetParent(ServerWorld);
		Parts.front() = Replacement;
		const auto LifecycleSourceMs = Milliseconds(Clock::now() - LifecycleStart);
		const auto ProduceStart = Clock::now();
		const auto LifecycleDelta = Coordinator.ProduceIncremental(Peer);
		const auto LifecycleProduceMs = Milliseconds(Clock::now() - ProduceStart);
		Require(LifecycleDelta.Succeeded() && LifecycleDelta.Frame, "replica lifecycle production failed");
		const auto ApplyStart = Clock::now();
		const auto LifecycleApplied = Replica.ApplyFrame(*LifecycleDelta.Frame);
		const auto LifecycleApplyMs = Milliseconds(Clock::now() - ApplyStart);
		Require(LifecycleApplied.Succeeded() && !Replica.Resolve(OldSource),
			"replica lifecycle retained the removed source identity");
		auto NewReplica = std::dynamic_pointer_cast<Part>(Replica.Resolve(Replacement->GetObjectId()));
		Require(NewReplica != nullptr && NewReplica->GetObjectId() != OldLocal,
			"replica lifecycle did not establish a distinct local generation");
		LiveIds.front() = NewReplica->GetObjectId();
		const auto PublishStart = Clock::now();
		const auto LifecyclePublication = Publisher.Publish(*ReplicaWorld, RenderCameraInput{}, 640, 360);
		const auto LifecyclePublishMs = Milliseconds(Clock::now() - PublishStart);
		Require(!LifecyclePublication->FullResync &&
			LifecyclePublication->Removes.size() == 1 && LifecyclePublication->Creates.size() == 1 &&
			LifecyclePublication->Removes.front().Object == OldLocal &&
			LifecyclePublication->Creates.front().Item.Object == LiveIds.front(),
			"replica lifecycle publication did not replace the exact local identity");
		const auto ProjectionStart = Clock::now();
		(void)Projection.Apply(*LifecyclePublication);
		const auto LifecycleProjectionMs = Milliseconds(Clock::now() - ProjectionStart);
		Require(!Projection.GetObjects().contains(OldLocal) &&
			Projection.GetObjects().contains(LiveIds.front()),
			"replica lifecycle projection retained a stale local identity");
		Print("replica", "lifecycle", Config.Frames, Config.Count, 2,
			LifecycleSourceMs, LifecycleProduceMs, LifecycleApplyMs,
			LifecyclePublishMs, LifecycleProjectionMs, *LifecyclePublication);
		(void)VerifyFullState(Publisher, Projection, *ReplicaWorld, LiveIds);
		Replica.Reset();
		Server->Destroy();
	}

	std::size_t ParseCount(std::string_view Value, std::size_t Maximum) {
		std::uint64_t Parsed = 0;
		const auto [End, Error] = std::from_chars(Value.data(), Value.data() + Value.size(), Parsed);
		if (Error != std::errc{} || End != Value.data() + Value.size() || Parsed > Maximum)
			throw std::invalid_argument("probe argument is not a bounded integer");
		return static_cast<std::size_t>(Parsed);
	}
}

int main(int ArgumentCount, char **Arguments) {
	try {
		Options Config;
		for (int Index = 1; Index < ArgumentCount; ++Index) {
			const std::string_view Argument(Arguments[Index]);
			if (Argument.starts_with("--count=")) Config.Count = ParseCount(Argument.substr(8), 512);
			else if (Argument.starts_with("--changed=")) Config.Changed = ParseCount(Argument.substr(10), 512);
			else if (Argument.starts_with("--frames=")) Config.Frames = ParseCount(Argument.substr(9), 60);
			else if (Argument.starts_with("--mode=")) Config.Mode = Argument.substr(7);
			else throw std::invalid_argument("unknown realistic publication probe argument");
		}
		if (Config.Count == 0 || Config.Changed > Config.Count ||
			Config.Frames == 0 || (Config.Mode != "both" && Config.Mode != "mesh" && Config.Mode != "replica"))
			throw std::invalid_argument("invalid bounded realistic publication workload");
		gargantuan::BootstrapNativeRuntimeSchema();
		std::cout << "mode,phase,frame,objects,changed,source_ms,produce_ms,apply_ms,publish_ms,"
			"projection_ms,creates,updates,removes,mesh_creates,mesh_removes,full_resync\n";
		if (Config.Mode == "both" || Config.Mode == "mesh") RunMesh(Config);
		if (Config.Mode == "both" || Config.Mode == "replica") RunReplica(Config);
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Runtime6:RealisticPublication] " << Error.what() << '\n';
		return 1;
	}
}
