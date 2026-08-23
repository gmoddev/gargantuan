// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/RenderExtractor.hpp"
#include "gargantuan/render/RenderProjection.hpp"
#include "gargantuan/services/Workspace.hpp"

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
}

static_assert(std::is_same_v<gargantuan::RenderPublicationPtr, std::shared_ptr<const gargantuan::RenderPublication>>);
static_assert(!std::is_pointer_v<decltype(gargantuan::RenderObjectCreate::Item)>);

int main() {
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
		TestProjectionOrderingAndIdentity();
		TestDeformableAndGuiContracts();
		TestIncrementalPublisher();
		TestDirtyAccumulatorBoundsAndConsumers();
	} catch (const std::exception &Error) {
		std::cerr << "[Render:PublicationTest] unexpected exception: " << Error.what() << '\n';
		return 1;
	}
	if (Failures != 0) return 1;
	std::cout << "[Render:PublicationTest] All incremental publication tests passed\n";
	return 0;
}
