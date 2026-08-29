// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/Sky.hpp"
#include "gargantuan/environment/EnvironmentSemantics.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/RenderExtractor.hpp"
#include "gargantuan/render/RenderProjection.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "gargantuan/services/Lighting.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <type_traits>

namespace {
	int Failures = 0;

	void Check(bool Condition, const char *Message) {
		if (Condition) return;
		std::cerr << "[Render:PublicationTest] FAIL: " << Message << '\n';
		++Failures;
	}

	template <typename Exception, typename Callback> void CheckThrows(Callback Test, const char *Message) {
		try { Test(); }
		catch (const Exception &) { return; }
		catch (...) {}
		Check(false, Message);
	}

	gargantuan::RenderFrameState MakeFrame() {
		return {.ViewportWidth = 640, .ViewportHeight = 360, .DpiScale = 1.5f};
	}

	gargantuan::RenderItem MakeItem(gargantuan::ObjectId Object, float X = 0.0f) {
		auto Item = gargantuan::RenderItem{.Object = Object};
		Item.ModelMatrix[3].x = X;
		Item.InverseModelMatrix[3].x = -X;
		return Item;
	}

	void TestProjectionOrderingAndIdentity() {
		using namespace gargantuan;
		RenderProjection Projection;
		RenderPublication Initial{
			.Id = 1, .FullResync = true, .Frame = MakeFrame(),
			.Creates = {{MakeItem({10, 1})}, {MakeItem({11, 1})}},
		};
		auto Changes = Projection.Apply(Initial);
		Check(Changes.Created == 2 && Projection.GetSize() == 2, "full resync creates a deterministic projection");

		RenderPublication Delta{
			.Id = 2, .BaseId = 1, .Frame = MakeFrame(),
			.Creates = {{MakeItem({11, 2})}},
			.Updates = {{{10, 1}, RenderUpdateDomain::Transform, MakeItem({10, 1}, 4.0f)}},
			.Removes = {{{11, 1}}},
		};
		Changes = Projection.Apply(Delta);
		Check(
			Changes.Created == 1 && Changes.Updated == 1 && Changes.Removed == 1 &&
			Projection.GetItem({11, 1}) == nullptr && Projection.GetItem({11, 2}) != nullptr,
			"remove, recreate, and update preserve generation-safe identity"
		);

		auto Stale = Delta;
		Stale.Id = 3;
		CheckThrows<std::invalid_argument>([&] { (void)Projection.Apply(Stale); }, "stale publication bases are rejected");
		auto Replay = Delta;
		Replay.BaseId = 2;
		CheckThrows<std::invalid_argument>([&] { (void)Projection.Apply(Replay); }, "publication identities must increase");
		auto Duplicate = RenderPublication{.Id = 3, .BaseId = 2, .Frame = MakeFrame()};
		Duplicate.Updates = {
			{{10, 1}, RenderUpdateDomain::Transform, MakeItem({10, 1}, 5.0f)},
			{{10, 1}, RenderUpdateDomain::Material, MakeItem({10, 1}, 5.0f)},
		};
		CheckThrows<std::invalid_argument>([&] { (void)Projection.Apply(Duplicate); }, "duplicate updates are rejected atomically");
		Check(Projection.GetItem({10, 1})->ModelMatrix[3].x == 4.0f, "rejected updates do not mutate the projection");

		RenderProjection Restarted;
		auto Restart = Initial;
		Restart.Id = 50;
		Restart.Creates = {{MakeItem({10, 1}, 4.0f)}, {MakeItem({11, 2})}};
		Check(Restarted.Apply(Restart).Created == 2 && Restarted.GetSize() == 2, "a renderer restart accepts a deterministic full resync");
	}

	void TestDeformableAndGuiContracts() {
		using namespace gargantuan;
		RenderProjection Projection;
		auto Vertices = std::make_shared<const std::vector<RenderVertex>>(std::vector<RenderVertex>(8));
		auto Indices = std::make_shared<const std::vector<std::uint32_t>>(std::vector<std::uint32_t>{0, 1, 2, 2, 3, 0});
		auto AtlasPixels = std::make_shared<const std::vector<std::uint8_t>>(std::vector<std::uint8_t>(4 * 4 * 4, 255));
		RenderPublication Initial{.Id = 1, .FullResync = true, .Frame = MakeFrame()};
		Initial.MeshCreates.push_back({
			{1, 1}, 1, 1, Vertices, Indices, {glm::vec3(-1.0f), glm::vec3(1.0f)},
		});
		Initial.TextureCreates.push_back({{7, 1}, 1, 4, 4, RenderTextureFormat::Rgba8Unorm, AtlasPixels});
		auto Changes = Projection.Apply(Initial);
		Check(
			Changes.MeshesCreated == 1 && Changes.VertexUploadBytes ==
				8 * sizeof(RenderVertex) + 6 * sizeof(std::uint32_t),
			"stable topology creation reports exact initial upload bytes"
		);
		Check(
			Changes.TexturesCreated == 1 && Changes.TextureUploadBytes == AtlasPixels->size(),
			"texture creation establishes stable renderer-neutral residency"
		);

		RenderPublication BindObject{.Id = 2, .BaseId = 1, .Frame = MakeFrame()};
		RenderMaterialState TexturedMaterial;
		TexturedMaterial.BaseColorTexture = RenderTextureIdentity{7, 1};
		BindObject.Creates.push_back({MakeItem({20, 1}), RenderMeshIdentity{1, 1}, TexturedMaterial});
		Changes = Projection.Apply(BindObject);
		Check(Changes.Created == 1, "objects may bind resident generation-safe meshes and textures");

		RenderPublication MissingMesh{.Id = 3, .BaseId = 2, .Frame = MakeFrame()};
		MissingMesh.Creates.push_back({MakeItem({21, 1}), RenderMeshIdentity{99, 1}});
		CheckThrows<std::invalid_argument>(
			[&] { (void)Projection.Apply(MissingMesh); },
			"object creation rejects a missing mesh identity"
		);
		RenderPublication MissingTexture{.Id = 3, .BaseId = 2, .Frame = MakeFrame()};
		RenderMaterialState MissingTextureMaterial;
		MissingTextureMaterial.BaseColorTexture = RenderTextureIdentity{99, 1};
		MissingTexture.Creates.push_back({MakeItem({21, 1}), std::nullopt, MissingTextureMaterial});
		CheckThrows<std::invalid_argument>(
			[&] { (void)Projection.Apply(MissingTexture); },
			"object creation rejects a material texture without residency"
		);
		RenderPublication ReferencedRemoval{.Id = 3, .BaseId = 2, .Frame = MakeFrame()};
		ReferencedRemoval.MeshRemoves.push_back({{1, 1}});
		CheckThrows<std::invalid_argument>(
			[&] { (void)Projection.Apply(ReferencedRemoval); },
			"mesh removal rejects identities retained by projected objects"
		);
		RenderPublication ReferencedTextureRemoval{.Id = 3, .BaseId = 2, .Frame = MakeFrame()};
		ReferencedTextureRemoval.TextureRemoves.push_back({{7, 1}});
		CheckThrows<std::invalid_argument>(
			[&] { (void)Projection.Apply(ReferencedTextureRemoval); },
			"texture removal rejects identities retained by projected materials"
		);

		auto DirtyVertices = std::make_shared<const std::vector<RenderVertex>>(std::vector<RenderVertex>(2));
		RenderPublication Delta{.Id = 3, .BaseId = 2, .Frame = MakeFrame()};
		Delta.MeshVertexUpdates.push_back({
			{1, 1}, 2, 3, DirtyVertices, {glm::vec3(-1.0f), glm::vec3(1.0f)},
		});
		auto AtlasPatch = std::make_shared<const std::vector<std::uint8_t>>(std::vector<std::uint8_t>(2 * 2 * 4, 127));
		Delta.TextureUpdates.push_back({{7, 1}, 2, 1, 1, 2, 2, AtlasPatch});
		RenderUiBatch Batch;
		Batch.Texture = RenderTextureIdentity{7, 1};
		Batch.Clip = RenderUiClipRect{0.0f, 0.0f, 100.0f, 50.0f};
		Batch.Layer = 4;
		Batch.Opacity = 0.75f;
		Batch.Vertices.resize(4);
		Batch.Indices = {0, 1, 2, 2, 3, 0};
		Delta.SharedUi = std::make_shared<const RenderUiFrame>(RenderUiFrame{640, 360, 1.5f, {Batch}});
		Delta.UiChanged = true;
		Changes = Projection.Apply(Delta);
		Check(
			Changes.MeshesUpdated == 1 && Changes.VertexUploadBytes == 2 * sizeof(RenderVertex),
			"bounded deformable dirty ranges upload only the published vertex slice"
		);
		Check(
			Changes.UiBatches == 1 && Changes.UiVertices == 4 && Changes.UiIndices == 6 &&
			&Projection.GetUi() == Delta.SharedUi.get(),
			"renderer-neutral immutable GUI frames preserve geometry and shared ownership without copying"
		);
		Check(
			Changes.TexturesUpdated == 1 && Changes.TextureUploadBytes == AtlasPatch->size(),
			"atlas-like subregion updates preserve texture identity and report exact upload bytes"
		);
		RenderPublication NoUiDelta{.Id = 4, .BaseId = 3, .Frame = MakeFrame()};
		Changes = Projection.Apply(NoUiDelta);
		Check(
			Changes.UiBatches == 0 && Projection.GetUi().Batches.size() == 1,
			"an incremental publication without a UI update preserves committed UI state"
		);
		RenderPublication ClearUi{.Id = 5, .BaseId = 4, .Frame = MakeFrame(), .UiChanged = true};
		ClearUi.Ui = {640, 360, 1.5f, {}};
		Changes = Projection.Apply(ClearUi);
		Check(Changes.UiBatches == 0 && Projection.GetUi().Batches.empty(), "an explicit empty UI update clears committed UI state");

		RenderPublication OutOfRange{.Id = 6, .BaseId = 5, .Frame = MakeFrame()};
		OutOfRange.MeshVertexUpdates.push_back({
			{1, 1}, 3, 7, DirtyVertices, {glm::vec3(-1.0f), glm::vec3(1.0f)},
		});
		CheckThrows<std::invalid_argument>([&] { (void)Projection.Apply(OutOfRange); }, "out-of-range dirty geometry is rejected");

		auto InvalidVertices = std::vector<RenderVertex>(1);
		InvalidVertices[0].Position.x = std::numeric_limits<float>::quiet_NaN();
		RenderPublication Invalid{.Id = 6, .BaseId = 5, .Frame = MakeFrame()};
		Invalid.MeshVertexUpdates.push_back({
			{1, 1}, 3, 0, std::make_shared<const std::vector<RenderVertex>>(std::move(InvalidVertices)),
			{glm::vec3(0.0f), glm::vec3(1.0f)},
		});
		CheckThrows<std::invalid_argument>([&] { (void)Projection.Apply(Invalid); }, "NaN deformable geometry is rejected");

		RenderPublication InvalidTexture{.Id = 6, .BaseId = 5, .Frame = MakeFrame()};
		InvalidTexture.TextureUpdates.push_back({{7, 1}, 3, 3, 3, 2, 2, AtlasPatch});
		CheckThrows<std::invalid_argument>(
			[&] { (void)Projection.Apply(InvalidTexture); },
			"out-of-range texture updates are rejected atomically"
		);

		RenderPublication TopologyChange{.Id = 6, .BaseId = 5, .Frame = MakeFrame()};
		TopologyChange.MeshRemoves.push_back({{1, 1}});
		TopologyChange.MeshCreates.push_back({
			{1, 2}, 2, 1, Vertices, Indices, {glm::vec3(-1.0f), glm::vec3(1.0f)},
		});
		TopologyChange.Updates.push_back({
			{20, 1}, RenderUpdateDomain::Geometry, MakeItem({20, 1}), RenderMeshIdentity{1, 2},
		});
		Changes = Projection.Apply(TopologyChange);
		Check(
			Changes.MeshesRemoved == 1 && Changes.MeshesCreated == 1 && Changes.Updated == 1,
			"topology replacement uses a new mesh generation and rebinds the projected object"
		);

		RenderProjection Restarted;
		RenderPublication Restart{.Id = 100, .FullResync = true, .Frame = MakeFrame()};
		Restart.TextureCreates.push_back({{7, 1}, 1, 4, 4, RenderTextureFormat::Rgba8Unorm, AtlasPixels});
		Restart.Ui = {640, 360, 1.5f, {Batch}};
		Check(
			Restarted.Apply(Restart).TexturesCreated == 1 && Restarted.GetTextureCount() == 1,
			"renderer restart reconstructs texture residency from a full publication"
		);
	}

	void TestIncrementalPublisher() {
		using namespace gargantuan;
		ChangeJournal::Get().Clear();
		RenderDirtyAccumulator::Get().Clear();
		auto Game = std::make_shared<DataModel>();
		auto World = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
		auto PartValue = std::make_shared<Part>();
		PartValue->SetParent(World);
		const auto Object = PartValue->GetObjectId();
		RenderPublisher Publisher;
		const auto Camera = MakeLookAtRenderCameraInput(
			glm::vec3(0.0f, 0.0f, 10.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f)
		);

		auto First = Publisher.Publish(*World, Camera, 640, 360);
		Check(First->FullResync && First->Creates.size() == 1, "first publication is a complete reconstructable resync");
		auto Unchanged = Publisher.Publish(*World, Camera, 640, 360);
		Check(
			Unchanged->Creates.empty() && Unchanged->Updates.empty() && Unchanged->Removes.empty(),
			"unchanged objects are not republished"
		);
		PartValue->SetName("IrrelevantToRendering");
		auto Irrelevant = Publisher.Publish(*World, Camera, 640, 360);
		Check(
			Irrelevant->Creates.empty() && Irrelevant->Updates.empty() && Irrelevant->Removes.empty(),
			"irrelevant property mutations do not dirty render publication"
		);

		PartValue->SetCFrame(CFrame(glm::vec3(2.0f, 0.0f, 0.0f)));
		auto Transform = Publisher.Publish(*World, Camera, 640, 360);
		Check(
			Transform->Updates.size() == 1 && Transform->Updates[0].Object == Object &&
			HasRenderUpdateDomain(Transform->Updates[0].Domains, RenderUpdateDomain::Transform) &&
			!HasRenderUpdateDomain(Transform->Updates[0].Domains, RenderUpdateDomain::Material),
			"transform-only mutations publish one classified update"
		);

		PartValue->SetColor(Color3(0.25f, 0.5f, 0.75f));
		auto Material = Publisher.Publish(*World, Camera, 640, 360);
		Check(
			Material->Updates.size() == 1 &&
			HasRenderUpdateDomain(Material->Updates[0].Domains, RenderUpdateDomain::Material) &&
			!HasRenderUpdateDomain(Material->Updates[0].Domains, RenderUpdateDomain::Transform),
			"material-only mutations publish one classified update"
		);

		for (std::size_t Index = 0; Index < 100; ++Index)
			PartValue->SetCFrame(CFrame(glm::vec3(static_cast<float>(Index), 0.0f, 0.0f)));
		for (std::size_t Index = 0; Index < 20; ++Index)
			PartValue->SetColor(Color3(static_cast<float>(Index) / 20.0f, 0.5f, 0.75f));
		auto Coalesced = Publisher.Publish(*World, Camera, 640, 360);
		Check(
			Coalesced->Updates.size() == 1 &&
			HasRenderUpdateDomain(Coalesced->Updates[0].Domains, RenderUpdateDomain::Transform) &&
			HasRenderUpdateDomain(Coalesced->Updates[0].Domains, RenderUpdateDomain::Material) &&
			Coalesced->Updates[0].Item.ModelMatrix[3].x == 99.0f,
			"redundant transform and material writes coalesce into one final-state update"
		);

		auto CreatedAndUpdated = std::make_shared<Part>();
		CreatedAndUpdated->SetParent(World);
		CreatedAndUpdated->SetCFrame(CFrame(glm::vec3(7.0f, 0.0f, 0.0f)));
		auto Created = Publisher.Publish(*World, Camera, 640, 360);
		Check(
			Created->Creates.size() == 1 && Created->Creates[0].Item.Object == CreatedAndUpdated->GetObjectId() &&
			Created->Creates[0].Item.ModelMatrix[3].x == 7.0f,
			"create plus updates publishes one create with final initial state"
		);

		CreatedAndUpdated->SetCFrame(CFrame(glm::vec3(8.0f, 0.0f, 0.0f)));
		CreatedAndUpdated->Destroy();
		auto UpdatedAndRemoved = Publisher.Publish(*World, Camera, 640, 360);
		Check(
			UpdatedAndRemoved->Updates.empty() && UpdatedAndRemoved->Removes.size() == 1 &&
			UpdatedAndRemoved->Removes[0].Object == CreatedAndUpdated->GetObjectId(),
			"update plus remove publishes only the removal"
		);

		auto Remaining = std::make_shared<Part>();
		Remaining->SetParent(World);
		auto RemainingCreated = Publisher.Publish(*World, Camera, 640, 360);
		Check(RemainingCreated->Creates.size() == 1, "replacement fixture publishes one create");

		auto Ephemeral = std::make_shared<Part>();
		Ephemeral->SetParent(World);
		const auto EphemeralObject = Ephemeral->GetObjectId();
		Ephemeral->Destroy();
		auto Cancelled = Publisher.Publish(*World, Camera, 640, 360);
		Check(
			Cancelled->Creates.empty() && Cancelled->Updates.empty() && Cancelled->Removes.empty() &&
			Publisher.GetPublishedObjectCount() == 2,
			"create plus remove before publication produces no projected operation"
		);
		(void)EphemeralObject;

		PartValue->Destroy();
		auto Removed = Publisher.Publish(*World, Camera, 640, 360);
		Check(Removed->Removes.size() == 1 && Removed->Removes[0].Object == Object, "destroy publishes one removal");
		Publisher.RequestFullResync();
		auto Resync = Publisher.Publish(*World, Camera, 640, 360);
		Check(
			Resync->FullResync && Resync->Creates.size() == 1 &&
			Resync->Creates[0].Item.Object == Remaining->GetObjectId(),
			"explicit restart resync reconstructs current truth"
		);
		auto SecondGame = std::make_shared<DataModel>();
		auto SecondWorld = std::dynamic_pointer_cast<Workspace>(SecondGame->GetService("Workspace"));
		auto Switched = Publisher.Publish(*SecondWorld, Camera, 640, 360);
		Check(Switched->FullResync && Switched->Creates.empty(), "switching publication scope performs a full resync");
		auto Returned = Publisher.Publish(*World, Camera, 640, 360);
		Check(
			Returned->FullResync && Returned->Creates.size() == 1,
			"returning to a prior publication scope retains the consumer and performs a full resync"
		);
		SecondGame->Destroy();
		Game->Destroy();
	}

	void TestDirtyAccumulatorBoundsAndConsumers() {
		using namespace gargantuan;
		RenderDirtyLimits Limits;
		Limits.MaximumScopes = 2;
		Limits.MaximumConsumers = 2;
		Limits.MaximumDistinctObjects = 2;
		Limits.MaximumDeformableBytes = 64;
		Limits.MaximumUiBytes = 64;
		Limits.MaximumPublicationBytes = 4096;
		RenderDirtyAccumulator Accumulator(Limits);
		const ObjectId Scope{100, 1};
		const auto FirstConsumer = Accumulator.CreateConsumer();
		const auto SecondConsumer = Accumulator.CreateConsumer();

		for (std::size_t Index = 0; Index < 100; ++Index)
			Accumulator.Mark(Scope, {1, 1}, RenderUpdateDomain::Transform);
		auto FirstCapture = Accumulator.Capture(Scope, FirstConsumer);
		auto SecondCapture = Accumulator.Capture(Scope, SecondConsumer);
		Check(
			FirstCapture.Records.size() == 1 && SecondCapture.Records.size() == 1 &&
			Accumulator.GetPendingObjectCount(Scope) == 1,
			"raw mutation volume coalesces independently for multiple render consumers"
		);
		Accumulator.Acknowledge(FirstCapture);
		Check(
			Accumulator.GetPendingObjectCount(Scope) == 1,
			"one renderer acknowledgement does not starve another active renderer"
		);
		Accumulator.Acknowledge(SecondCapture);
		Check(Accumulator.GetPendingObjectCount(Scope) == 0, "successful acknowledgement clears consumed state");

		Accumulator.Mark(Scope, {1, 1}, RenderUpdateDomain::Transform);
		auto FailedBuildCapture = Accumulator.Capture(Scope, FirstConsumer);
		auto RetriedCapture = Accumulator.Capture(Scope, FirstConsumer);
		Check(
			FailedBuildCapture.Records.size() == 1 && RetriedCapture.Records.size() == 1,
			"an unacknowledged failed publication preserves dirty state for retry"
		);
		Accumulator.Acknowledge(RetriedCapture);

		Accumulator.Mark(Scope, {1, 1}, RenderUpdateDomain::Transform);
		Accumulator.Mark(Scope, {2, 1}, RenderUpdateDomain::Transform);
		Accumulator.Mark(Scope, {3, 1}, RenderUpdateDomain::Transform);
		auto DistinctOverflow = Accumulator.Capture(Scope, FirstConsumer);
		Check(
			DistinctOverflow.FullResyncRequired && DistinctOverflow.Records.size() == 2,
			"distinct-object safety limit requests full resync without partial third-object publication"
		);

		Accumulator.Clear();
		Accumulator.Mark(Scope, {1, 1}, RenderUpdateDomain::DeformableVertices, 65);
		Accumulator.MarkUi(Scope, 65);
		auto ByteOverflow = Accumulator.Capture(Scope, FirstConsumer);
		Check(
			ByteOverflow.FullResyncRequired && ByteOverflow.UiDirty,
			"deformable and UI dirty-byte safety limits request full resync"
		);

		Accumulator.Clear();
		Accumulator.Mark(Scope, {2, 1}, RenderUpdateDomain::Material);
		Accumulator.Mark(Scope, {1, 1}, RenderUpdateDomain::Transform);
		auto Ordered = Accumulator.Capture(Scope, FirstConsumer);
		Check(
			Ordered.Records.size() == 2 && Ordered.Records[0].Object == ObjectId{1, 1} &&
			Ordered.Records[1].Object == ObjectId{2, 1},
			"dirty publication ordering is deterministic by generation-safe ObjectId"
		);
		Check(
			RenderDirtyAccumulator::Classify(PropertyUpdatedChange{"Name", std::string("Ignored"), false}) ==
				RenderUpdateDomain::None,
			"semantic classification ignores non-render properties"
		);

		CheckThrows<std::length_error>(
			[&] { (void)Accumulator.CreateConsumer(); },
			"render dirty consumer identities are bounded with the configured scope budget"
		);
		Accumulator.Clear();
		const ObjectId SecondScope{101, 1};
		const ObjectId OverflowScope{102, 1};
		Accumulator.Mark(Scope, {1, 1}, RenderUpdateDomain::Transform);
		Accumulator.Mark(SecondScope, {2, 1}, RenderUpdateDomain::Transform);
		auto ScopeOverflow = Accumulator.Capture(OverflowScope, FirstConsumer);
		Check(
			ScopeOverflow.FullResyncRequired && Accumulator.GetPendingObjectCount(OverflowScope) == 0,
			"scope exhaustion requests global recovery without allocating an unbounded scope"
		);
		Accumulator.Acknowledge(ScopeOverflow);
		auto OtherConsumerRecovery = Accumulator.Capture(Scope, SecondConsumer);
		Check(
			OtherConsumerRecovery.FullResyncRequired,
			"one consumer cannot clear another consumer's global recovery epoch"
		);
		Accumulator.Acknowledge(OtherConsumerRecovery);
		auto Recovered = Accumulator.Capture(Scope, FirstConsumer);
		Check(!Recovered.FullResyncRequired, "global recovery completes after every live consumer acknowledges it");
		Accumulator.ReleaseConsumer(FirstConsumer);
		Accumulator.ReleaseConsumer(SecondConsumer);
	}

	void TestAnimationPaletteContracts() {
		using namespace gargantuan;
		const ObjectId Object{30, 1};
		const RenderMeshIdentity SourceMesh{30, 1};
		auto Vertices = std::make_shared<const std::vector<RenderVertex>>(std::vector<RenderVertex>(3));
		auto Indices = std::make_shared<const std::vector<std::uint32_t>>(
			std::vector<std::uint32_t>{0, 1, 2});
		auto Influences = std::make_shared<std::vector<RenderSkinInfluence>>(3);
		for (std::size_t Index = 0; Index < Influences->size(); ++Index) {
			Influences->at(Index).Joints = {0, 1, 0, 0};
			Influences->at(Index).Weights = {0.25f, 0.75f, 0.0f, 0.0f};
		}
		RenderSkeletonIdentity Skeleton;
		Skeleton.Bytes[0] = 1;
		auto Palette = std::make_shared<std::vector<RenderSkinPaletteEntry>>(2);
		Palette->at(1).PositionMatrix[3].x = 0.5f;

		RenderProjection Projection;
		RenderPublication Initial{.Id = 1, .FullResync = true, .Frame = MakeFrame()};
		Initial.MeshCreates.push_back({SourceMesh, 1, 1, Vertices, Indices,
			{glm::vec3(0.0f), glm::vec3(1.0f)}, Influences, Skeleton, 2});
		Initial.Creates.push_back({MakeItem(Object), SourceMesh});
		Initial.AnimationPoseUpdates.push_back({
			.Object = Object,
			.SourceMesh = SourceMesh,
			.PoseRevision = 1,
			.Palette = {Skeleton, Palette},
		});
		const auto Changes = Projection.Apply(Initial);
		Check(Changes.AnimationPosesUpdated == 1 &&
			Changes.PaletteUploadBytes == 2 * sizeof(RenderSkinPaletteEntry) &&
			Projection.GetAnimationPose(Object) &&
			Projection.GetAnimationPose(Object)->PoseRevision == 1,
			"GPU pose projection retains one coherent source mesh and exact palette bytes");

		auto InvalidUpdate = [&](std::uint64_t Id = 2) {
			RenderPublication Publication{.Id = Id, .BaseId = 1, .Frame = MakeFrame()};
			Publication.AnimationPoseUpdates.push_back({
				.Object = Object,
				.SourceMesh = SourceMesh,
				.PoseRevision = 2,
				.Palette = {Skeleton, Palette},
			});
			return Publication;
		};

		auto Oversized = InvalidUpdate();
		Oversized.AnimationPoseUpdates[0].Palette.Entries =
			std::make_shared<const std::vector<RenderSkinPaletteEntry>>(
				MaximumRenderSkinPaletteEntries + 1);
		CheckThrows<std::invalid_argument>([&] { (void)Projection.Apply(Oversized); },
			"projection rejects palettes above the 256-joint contract");

		auto CountMismatch = InvalidUpdate();
		CountMismatch.AnimationPoseUpdates[0].Palette.Entries =
			std::make_shared<const std::vector<RenderSkinPaletteEntry>>(1);
		CheckThrows<std::invalid_argument>([&] { (void)Projection.Apply(CountMismatch); },
			"projection rejects palette counts that differ from source skeleton topology");

		auto NonFinite = InvalidUpdate();
		auto NonFiniteEntries = std::make_shared<std::vector<RenderSkinPaletteEntry>>(*Palette);
		NonFiniteEntries->at(1).NormalMatrix[0][0] = std::numeric_limits<float>::quiet_NaN();
		NonFinite.AnimationPoseUpdates[0].Palette.Entries = NonFiniteEntries;
		CheckThrows<std::invalid_argument>([&] { (void)Projection.Apply(NonFinite); },
			"projection rejects non-finite position or normal palette matrices");

		auto Stale = InvalidUpdate();
		Stale.AnimationPoseUpdates[0].PoseRevision = 1;
		CheckThrows<std::invalid_argument>([&] { (void)Projection.Apply(Stale); },
			"projection rejects stale per-rig pose revisions");

		auto MissingSource = InvalidUpdate();
		MissingSource.AnimationPoseUpdates[0].SourceMesh = {99, 1};
		CheckThrows<std::invalid_argument>([&] { (void)Projection.Apply(MissingSource); },
			"projection rejects a palette whose source mesh residency is missing");

		auto Incompatible = InvalidUpdate();
		Incompatible.AnimationPoseUpdates[0].Palette.Skeleton.Bytes[0] = 2;
		CheckThrows<std::invalid_argument>([&] { (void)Projection.Apply(Incompatible); },
			"projection rejects a palette whose skeleton identity is incompatible with its mesh");

		auto UnexpectedPosedMesh = InvalidUpdate();
		UnexpectedPosedMesh.AnimationPoseUpdates[0].PosedMesh = {31, 1};
		CheckThrows<std::invalid_argument>([&] { (void)Projection.Apply(UnexpectedPosedMesh); },
			"GPU palette mode rejects transient CPU posed-mesh identities");

		RenderProjection MalformedMeshProjection;
		auto MalformedInfluences = std::make_shared<std::vector<RenderSkinInfluence>>(*Influences);
		MalformedInfluences->at(0).Joints[0] = 2;
		RenderPublication MalformedMesh{.Id = 1, .FullResync = true, .Frame = MakeFrame()};
		MalformedMesh.MeshCreates.push_back({SourceMesh, 1, 1, Vertices, Indices,
			{glm::vec3(0.0f), glm::vec3(1.0f)}, MalformedInfluences, Skeleton, 2});
		CheckThrows<std::invalid_argument>([&] { (void)MalformedMeshProjection.Apply(MalformedMesh); },
			"renderer validation rejects integer joint indices outside the declared palette range");
		Check(Projection.GetAnimationPose(Object) && Projection.GetAnimationPose(Object)->PoseRevision == 1,
			"rejected animation publications leave the previous coherent pose atomically intact");
	}

	void TestEnvironmentLightingFoundation() {
		using namespace gargantuan;
		ChangeJournal::Get().Clear();
		RenderDirtyAccumulator::Get().Clear();
		auto Game = std::make_shared<DataModel>();
		auto World = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
		auto LightingValue = std::dynamic_pointer_cast<Lighting>(Game->GetService("Lighting"));
		auto Assets = std::dynamic_pointer_cast<AssetService>(Game->GetService("AssetService"));
		Check(
			LightingValue && Game->FindService("Lighting") && *Game->FindService("Lighting") == LightingValue,
			"Lighting is one discoverable canonical service"
		);
		const Color3 DefaultAmbient = LightingValue->GetAmbient();
		Check(
			LightingValue->GetClockTime() == 12.0f && LightingValue->GetBrightness() == 1.0f &&
				DefaultAmbient.R == 0.2f && DefaultAmbient.G == 0.2f && DefaultAmbient.B == 0.2f,
			"Lighting defaults are deterministic"
		);
		CheckThrows<std::invalid_argument>(
			[&] { LightingValue->SetClockTime(24.0f); }, "ClockTime rejects the excluded upper bound"
		);
		CheckThrows<std::invalid_argument>(
			[&] { LightingValue->SetBrightness(9.0f); }, "Brightness rejects out-of-range values"
		);
		CheckThrows<std::invalid_argument>(
			[&] { LightingValue->SetExposureCompensation(std::numeric_limits<float>::quiet_NaN()); },
			"environment properties reject NaN"
		);
		CheckThrows<std::invalid_argument>(
			[&] { LightingValue->SetFogStart(1001.0f); }, "FogStart rejects an inverted fog interval"
		);

		const auto Sunrise = ComputeEnvironmentSunState(6.0f, 2.0f, Color3(1.0f, 0.5f, 0.25f));
		const auto Noon = ComputeEnvironmentSunState(12.0f, 2.0f, Color3(1.0f, 0.5f, 0.25f));
		const auto Sunset = ComputeEnvironmentSunState(18.0f, 2.0f, Color3(1.0f, 0.5f, 0.25f));
		Check(
			std::abs(Sunrise.Direction.x - 1.0f) < 1e-5f && std::abs(Noon.Direction.y - 1.0f) < 1e-5f &&
				std::abs(Sunset.Direction.x + 1.0f) < 1e-5f && Noon.Intensity == 2.0f,
			"ClockTime derives the documented sunrise, noon, and sunset vectors"
		);
		Check(
			ComputeEnvironmentExposure(-2.0f) == 0.25f && ComputeEnvironmentExposure(2.0f) == 4.0f,
			"exposure compensation uses powers of two"
		);

		const std::vector<std::uint8_t> RedPixels(4 * 4 * 4, 64);
		const std::vector<std::uint8_t> BluePixels(4 * 4 * 4, 192);
		const auto Red = Assets->RegisterMemoryImage("EnvironmentRed", 4, 4, RedPixels);
		const auto Blue = Assets->RegisterMemoryImage("EnvironmentBlue", 4, 4, BluePixels);
		auto FirstSky = std::make_shared<Sky>();
		FirstSky->SetSkyboxPositiveX(Red);
		FirstSky->SetSkyboxNegativeX(Red);
		FirstSky->SetSkyboxPositiveY(Red);
		FirstSky->SetSkyboxNegativeY(Red);
		FirstSky->SetSkyboxPositiveZ(Red);
		FirstSky->SetSkyboxNegativeZ(Red);
		FirstSky->SetParent(LightingValue);

		RenderPublisher Publisher;
		auto TextureChanges = Assets->DrainTextureChanges();
		Publisher.SetUiTextureChanges(
			std::move(TextureChanges.Creates), std::move(TextureChanges.Updates), std::move(TextureChanges.Removes)
		);
		const auto Camera = MakeLookAtRenderCameraInput(
			glm::vec3(0.0f, 0.0f, 10.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f)
		);
		auto Initial = Publisher.Publish(*World, Camera, 640, 360);
		Check(
			Initial->FullResync && Initial->EnvironmentChanged && Initial->Frame.Environment.Sky &&
				Initial->Frame.Environment.Sky->FaceDimension == 4,
			"a coherent six-face Sky is included in the initial immutable publication"
		);
		RenderProjection Projection;
		Check(
			Projection.Apply(*Initial).EnvironmentsUpdated == 1,
			"projection applies initial environment state with resident Sky textures"
		);

		for (std::size_t Index = 0; Index < 100; ++Index)
			LightingValue->SetClockTime(static_cast<float>(Index % 24));
		LightingValue->SetAmbient(Color3(0.1f, 0.15f, 0.2f));
		LightingValue->SetExposureCompensation(1.0f);
		LightingValue->SetFogEnabled(true);
		LightingValue->SetFogStart(10.0f);
		LightingValue->SetFogEnd(100.0f);
		auto Animated = Publisher.Publish(*World, Camera, 640, 360);
		Check(
			Animated->EnvironmentChanged && Animated->Creates.empty() && Animated->Updates.empty() &&
				Animated->Removes.empty() && Animated->Frame.Environment.ExposureMultiplier == 2.0f &&
				Animated->Frame.Environment.Fog.Enabled,
			"many environment edits coalesce into one environment-only final-state publication"
		);
		(void)Projection.Apply(*Animated);
		auto Static = Publisher.Publish(*World, Camera, 640, 360);
		Check(
			!Static->EnvironmentChanged && Static->Creates.empty() && Static->Updates.empty(),
			"static environment frames publish no environment or object delta"
		);
		(void)Projection.Apply(*Static);

		const auto Missing = AssetReference::FromAssetId(AssetId::New()).Value;
		FirstSky->SetSkyboxPositiveX(Missing);
		auto Broken = Publisher.Publish(*World, Camera, 640, 360);
		Check(
			!Broken->EnvironmentChanged && Broken->Frame.Environment.Sky && Broken->Diagnostics.size() >= 2,
			"an incoherent Sky retains its same-source last-known-good state with bounded diagnostics"
		);
		(void)Projection.Apply(*Broken);

		FirstSky->SetEnabled(false);
		auto Fallback = Publisher.Publish(*World, Camera, 640, 360);
		Check(
			Fallback->EnvironmentChanged && !Fallback->Frame.Environment.Sky,
			"disabling the effective Sky publishes the authored background fallback"
		);
		(void)Projection.Apply(*Fallback);

		auto SecondSky = std::make_shared<Sky>();
		SecondSky->SetSkyboxPositiveX(Blue);
		SecondSky->SetSkyboxNegativeX(Blue);
		SecondSky->SetSkyboxPositiveY(Blue);
		SecondSky->SetSkyboxNegativeY(Blue);
		SecondSky->SetSkyboxPositiveZ(Blue);
		SecondSky->SetSkyboxNegativeZ(Blue);
		SecondSky->SetParent(LightingValue);
		FirstSky->SetSkyboxPositiveX(Red);
		FirstSky->SetEnabled(true);
		auto Multiple = Publisher.Publish(*World, Camera, 640, 360);
		const auto RedResource = Assets->ResolveImage(Red);
		Check(
			Multiple->Frame.Environment.Sky && RedResource &&
				Multiple->Frame.Environment.Sky->Faces[0].Texture == RedResource->Texture &&
				std::ranges::any_of(
					Multiple->Diagnostics,
					[](const auto &Diagnostic) {
						return Diagnostic.Message.find("Multiple enabled") != std::string::npos;
					}
				),
			"multiple enabled Skies select the lowest ObjectId and emit a diagnostic"
		);
		(void)Projection.Apply(*Multiple);

		const auto PreviousFaceRevision = Multiple->Frame.Environment.Sky->Faces[0].ContentRevision;
		const std::vector<std::uint8_t> ReimportedRedPixels(4 * 4 * 4, 80);
		Check(
			Assets->RegisterMemoryImage("EnvironmentRed", 4, 4, ReimportedRedPixels) == Red,
			"memory-image reimport preserves the Sky AssetId"
		);
		TextureChanges = Assets->DrainTextureChanges();
		Publisher.SetUiTextureChanges(
			std::move(TextureChanges.Creates), std::move(TextureChanges.Updates), std::move(TextureChanges.Removes)
		);
		auto Reimported = Publisher.Publish(*World, Camera, 640, 360);
		Check(
			Reimported->EnvironmentChanged && Reimported->TextureCreates.empty() &&
				Reimported->TextureUpdates.size() == 1 && Reimported->Creates.empty() && Reimported->Updates.empty() &&
				Reimported->Removes.empty() && Reimported->Frame.Environment.Sky &&
				Reimported->Frame.Environment.Sky->Faces[0].ContentRevision == PreviousFaceRevision + 1,
			"successful Sky face reimport publishes one texture/environment update and no object work"
		);
		(void)Projection.Apply(*Reimported);

		const std::vector<std::uint8_t> InvalidPixels(4, 0);
		CheckThrows<std::invalid_argument>(
			[&] { (void)Assets->RegisterMemoryImage("EnvironmentRed", 4, 4, InvalidPixels); },
			"failed Sky reimport rejects invalid image bytes"
		);
		auto FailedReimport = Publisher.Publish(*World, Camera, 640, 360);
		Check(
			!FailedReimport->EnvironmentChanged && FailedReimport->TextureUpdates.empty(),
			"failed Sky reimport leaves the published environment and resources unchanged"
		);
		(void)Projection.Apply(*FailedReimport);

		Publisher.RequestFullResync();
		auto Restart = Publisher.Publish(*World, Camera, 640, 360);
		Check(
			Restart->FullResync && Restart->EnvironmentChanged && Restart->Frame.Environment.Sky &&
				Restart->Frame.Environment.Sky->Faces[0].ContentRevision == PreviousFaceRevision + 1 &&
				Restart->TextureCreates.size() >= 2,
			"renderer restart full resync recreates current environment and Sky residency without a DataModel mutation"
		);
		RenderProjection RestartedProjection;
		Check(
			RestartedProjection.Apply(*Restart).EnvironmentsUpdated == 1,
			"fresh renderer projection accepts the complete environment resync"
		);
		Game->Destroy();
	}
}

static_assert(std::is_same_v<gargantuan::RenderPublicationPtr, std::shared_ptr<const gargantuan::RenderPublication>>);
static_assert(!std::is_pointer_v<decltype(gargantuan::RenderObjectCreate::Item)>);

int main() {
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
		TestProjectionOrderingAndIdentity();
		TestDeformableAndGuiContracts();
		TestAnimationPaletteContracts();
		TestIncrementalPublisher();
		TestDirtyAccumulatorBoundsAndConsumers();
		TestEnvironmentLightingFoundation();
	} catch (const std::exception &Error) {
		std::cerr << "[Render:PublicationTest] unexpected exception: " << Error.what() << '\n';
		return 1;
	}
	if (Failures != 0) return 1;
	std::cout << "[Render:PublicationTest] All incremental publication tests passed\n";
	return 0;
}
