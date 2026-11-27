// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.

#include "gargantuan/Engine.hpp"
#include "gargantuan/assets/GasDependencies.hpp"
#include "gargantuan/assets/GasInstanceSerialization.hpp"
#include "gargantuan/classes/Animator.hpp"
#include "gargantuan/classes/Attachment.hpp"
#include "gargantuan/classes/Character.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/Frame.hpp"
#include "gargantuan/classes/JointConstraint.hpp"
#include "gargantuan/classes/MeshPart.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/Script.hpp"
#include "gargantuan/classes/Sound.hpp"
#include "gargantuan/classes/Terrain.hpp"
#include "gargantuan/classes/SpatialRegion.hpp"
#include "serialization/JsonCodec.hpp"
#include "gargantuan/classes/TextLabel.hpp"
#include "gargantuan/classes/WeldConstraint.hpp"
#include "gargantuan/editor/EditorHost.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/Project.hpp"
#include "gargantuan/filesystem/SourceMount.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"
#include "gargantuan/runtime/MutationGateway.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "gargantuan/services/Workspace.hpp"
#include "terrain/ScenePersistence.hpp"
#include "terrain/RenderStage.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
	using namespace gargantuan;
	constexpr std::string_view ScriptSource = "script:SetAttribute('Ran', true)\n";
	constexpr std::string_view LabelText = "GAS authoring fixture";

	void Require(bool Condition, std::string_view Message) {
		if (!Condition) throw std::runtime_error(std::string(Message));
	}

	template <typename Type>
	std::shared_ptr<Type> Create(std::string Name, const std::shared_ptr<Instance> &Parent) {
		auto Result = std::make_shared<Type>();
		Result->SetName(std::move(Name));
		Result->SetArchivable(true);
		Result->SetParent(Parent);
		return Result;
	}

	template <typename Type>
	std::shared_ptr<Type> Find(Instance &Root, std::string_view Name) {
		auto Result = std::dynamic_pointer_cast<Type>(Root.FindFirstDescendant(std::string(Name)));
		Require(Result != nullptr, std::string("Scene is missing ") + std::string(Name));
		return Result;
	}

	struct Scene {
		std::shared_ptr<Part> PartA;
		std::shared_ptr<Part> PartB;
		std::shared_ptr<Frame> Panel;
		std::shared_ptr<Script> Logic;
		std::shared_ptr<Character> Rig;

		[[nodiscard]] std::array<std::shared_ptr<Instance>, 5> Roots() const { return {PartA, PartB, Panel, Logic, Rig}; }
	};

	Scene CreateScene(const std::shared_ptr<DataModel> &World, const std::shared_ptr<Instance> &Parent) {
		Scene Result{Create<Part>("PartA", Parent), Create<Part>("PartB", Parent),
			Create<Frame>("Panel", Parent), Create<Script>("Logic", Parent), Create<Character>("Rig", Parent)};
		Result.PartA->SetFriction(0.125f);
		Result.PartB->SetRestitution(0.75f);
		const auto AttachmentA = Create<Attachment>("MountA", Result.PartA);
		const auto AttachmentB = Create<Attachment>("MountB", Result.PartB);
		const auto NearbySound = Create<Sound>("NearbySound", AttachmentA);
		NearbySound->SetRollOffMinDistance(1.0f);
		NearbySound->SetRollOffMaxDistance(5.0f);
		const auto DistantSound = Create<Sound>("DistantSound", AttachmentB);
		DistantSound->SetRollOffMaxDistance(200.0f);
		DistantSound->SetRollOffMinDistance(125.0f);
		const auto Weld = Create<WeldConstraint>("BridgeWeld", Result.PartA);
		Weld->SetPart0(Result.PartA);
		Weld->SetPart1(Result.PartB);
		const auto Joint = Create<JointConstraint>("BridgeJoint", Result.PartA);
		Joint->SetJointType("Hinge");
		Joint->SetAttachment0(AttachmentA);
		Joint->SetAttachment1(AttachmentB);
		Create<TextLabel>("Message", Result.Panel)->SetText(std::string(LabelText));
		Result.Rig->SetRootPart(Create<Part>("RigBody", Result.Rig));
		Result.Logic->SetSource(std::string(ScriptSource));
		const auto Security = ScriptSecurityContext::CoreTrusted();
		Require(Result.PartA->ApplyAttributeMutation("Channel", WireValue{std::string("Relay")}, Security) ==
			MutationStatus::Success, "Author fixture attribute");
		Require(World->Tags.Add(World->GetObjectId(), Result.PartA->GetObjectId(), "Portable", Security),
			"Author fixture tag");
		return Result;
	}

	void VerifyScene(const Scene &Restored, const std::shared_ptr<DataModel> &World) {
		Require(Restored.PartA->GetFriction() == 0.125f && Restored.PartB->GetRestitution() == 0.75f,
			"Restore authored rigid surface response");
		const auto Weld = Find<WeldConstraint>(*Restored.PartA, "BridgeWeld");
		const auto Joint = Find<JointConstraint>(*Restored.PartA, "BridgeJoint");
		const auto AttachmentA = Find<Attachment>(*Restored.PartA, "MountA");
		const auto AttachmentB = Find<Attachment>(*Restored.PartB, "MountB");
		Require(Weld->GetPart0() == Restored.PartA && Weld->GetPart1() == Restored.PartB,
			"Restore weld links across roots");
		Require(Joint->GetAttachment0() == AttachmentA && Joint->GetAttachment1() == AttachmentB &&
			Joint->GetJointType() == "Hinge", "Restore joint settings and attachment links across roots");
		const auto NearbySound = Find<Sound>(*AttachmentA, "NearbySound");
		const auto DistantSound = Find<Sound>(*AttachmentB, "DistantSound");
		Require(NearbySound->GetRollOffMinDistance() == 1.0f && NearbySound->GetRollOffMaxDistance() == 5.0f &&
			DistantSound->GetRollOffMinDistance() == 125.0f && DistantSound->GetRollOffMaxDistance() == 200.0f,
			"Restore Sound rolloff ranges below and above the construction defaults");
		Require(Find<TextLabel>(*Restored.Panel, "Message")->GetText() == LabelText, "Restore nested UI and text");
		Require(Restored.Rig->GetRootPart() == Find<Part>(*Restored.Rig, "RigBody"), "Restore Character root body reference");
		const auto Security = ScriptSecurityContext::CoreTrusted();
		Require(Restored.PartA->GetAttributeValues(Security).at("Channel") == WireValue{std::string("Relay")},
			"Restore attribute type and value");
		if (World) {
			Require(World->Tags.Has(World->GetObjectId(), Restored.PartA->GetObjectId(), "Portable", Security),
				"Restore tag into destination scope");
		} else {
			Require(Restored.PartA->GetDetachedTagsForAdoption() == std::vector<std::string>{"Portable"},
				"Keep parsed tag on detached candidate");
		}
		Require(Restored.Logic->GetSource() == ScriptSource &&
			!Restored.Logic->GetAttributeValues(Security).contains("Ran"), "Keep restored source inert");
	}

	Scene SceneUnder(Instance &Parent) {
		return {Find<Part>(Parent, "PartA"), Find<Part>(Parent, "PartB"), Find<Frame>(Parent, "Panel"),
			Find<Script>(Parent, "Logic"), Find<Character>(Parent, "Rig")};
	}

	struct PublicationState {
		std::uint64_t Revision;
		std::uint64_t NextJournalSequence;
		std::size_t OwnedInstances;
		std::size_t History;
		std::size_t OpenTransactions;
		std::size_t Children;
		bool operator==(const PublicationState &) const = default;
	};

	PublicationState Published(const DataModel &World, const Instance &Parent) {
		return {World.GetAuthoritativeRevision(), ChangeJournal::Get().CreateCursor(World.GetObjectId()).NextSequence,
			World.GetOwnedInstanceCount(), World.Transactions.GetCommitted().size(), World.Transactions.GetOpenCount(),
			Parent.Children.size()};
	}

	class ScopedFixtureGraph {
	  public:
		explicit ScopedFixtureGraph(std::shared_ptr<Instance> Root) : Roots{std::move(Root)} {}
		explicit ScopedFixtureGraph(std::span<const std::shared_ptr<Instance>> Forest) : Roots(Forest.begin(), Forest.end()) {}
		~ScopedFixtureGraph() { Destroy(); }
		ScopedFixtureGraph(const ScopedFixtureGraph &) = delete;
		ScopedFixtureGraph &operator=(const ScopedFixtureGraph &) = delete;

		void Destroy() {
			// Constraints can retain their ancestor Parts. Run the same teardown as an owning host.
			for (const auto &Root : Roots)
				if (Root) Root->Destroy();
			Roots.clear();
		}

	  private:
		std::vector<std::shared_ptr<Instance>> Roots;
	};

	struct ScopedFixtureConnection {
		SignalConnection::Pointer Connection;
		~ScopedFixtureConnection() { Connection->Disconnect(); }
	};

	class ScopedJournalCapacity {
	  public:
		explicit ScopedJournalCapacity(std::size_t Capacity) { ChangeJournal::Get().SetCapacity(Capacity); }
		~ScopedJournalCapacity() { ChangeJournal::Get().SetCapacity(PreviousCapacity); }
		ScopedJournalCapacity(const ScopedJournalCapacity &) = delete;
		ScopedJournalCapacity &operator=(const ScopedJournalCapacity &) = delete;

	  private:
		const std::size_t PreviousCapacity = ChangeJournal::Get().GetCapacity();
	};

	void TestLegacySoundRollOff() {
		using Json = nlohmann::ordered_json;
		const auto Read = [](Json Properties) {
			std::istringstream Input(Json{
				{"Version", 0}, {"Name", "Cue"}, {"ClassName", "Sound"},
				{"Properties", std::move(Properties)}, {"Children", Json::array()},
			}.dump());
			return InstanceSerialization::Deserialize(InstanceSerialization::InstanceFormat::Json, Input);
		};
		const auto Verify = [&](Json Properties, float Minimum, float Maximum) {
			const auto State = Read(std::move(Properties));
			const ScopedFixtureGraph Lifetime(State.Instance);
			const auto Value = std::dynamic_pointer_cast<Sound>(State.Instance);
			Require(State.Ok && Value && Value->GetRollOffMinDistance() == Minimum &&
				Value->GetRollOffMaxDistance() == Maximum, "Restore legacy JSON Sound rolloff distances");
		};
		Verify({{"RollOffMinDistance", {{"Float", 1.0}}}, {"RollOffMaxDistance", {{"Float", 5.0}}}}, 1.0f, 5.0f);
		Verify({{"RollOffMinDistance", {{"Float", 125.0}}}, {"RollOffMaxDistance", {{"Float", 200.0}}}}, 125.0f, 200.0f);
		Verify({{"RollOffMinDistance", {{"Float", 20.0}}}}, 20.0f, 100.0f);
		Verify({{"RollOffMaxDistance", {{"Float", 20.0}}}}, 10.0f, 20.0f);
		Require(!Read({{"RollOffMinDistance", {{"Float", 20.0}}}, {"RollOffMaxDistance", {{"Float", 20.0}}}}).Ok &&
			!Read({{"RollOffMaxDistance", {{"Float", 5.0}}}}).Ok,
			"Reject equal Sound rolloff distances and a legacy maximum below the default minimum");
	}

	void TestModel() {
		auto SourceWorld = std::make_shared<DataModel>();
		const ScopedFixtureGraph SourceLifetime(SourceWorld);
		const auto Source = CreateScene(SourceWorld, SourceWorld->GetService("Workspace"));
		const auto Roots = Source.Roots();
		Require(!InstanceSerialization::CaptureGasDocument(std::span(Roots).first(1), Gas::DocumentKind::Model),
			"Reject export that would silently lose an external reference");
		const auto Captured = InstanceSerialization::CaptureGasDocument(Roots, Gas::DocumentKind::Model);
		Require(Captured.has_value(), "Capture several arbitrary model roots");
		const auto Bytes = Gas::Encode(*Captured);
		Require(Bytes.has_value(), "Encode captured model");
		const auto Parsed = Gas::Decode(*Bytes);
		Require(Parsed.has_value() && *Parsed == *Captured, "Decode the captured forest canonically");

		const auto Staged = InstanceSerialization::MaterializeGasDocument(*Parsed, true);
		const ScopedFixtureGraph StagedLifetime(Staged.Roots);
		Require(Staged.Succeeded() && Staged.Roots.size() == 5, "Materialize detached roots before publication");
		const Scene Detached{std::dynamic_pointer_cast<Part>(Staged.Roots[0]),
			std::dynamic_pointer_cast<Part>(Staged.Roots[1]), std::dynamic_pointer_cast<Frame>(Staged.Roots[2]),
			std::dynamic_pointer_cast<Script>(Staged.Roots[3]), std::dynamic_pointer_cast<Character>(Staged.Roots[4])};
		Require(Detached.PartA && Detached.PartB && Detached.Panel && Detached.Logic && Detached.Rig &&
			Detached.PartA->GetObjectId() != Source.PartA->GetObjectId() && !Detached.PartA->GetDataModel() &&
			Detached.Logic->GetSourceId() == Source.Logic->GetSourceId(), "Stage isolated roots in document order");
		VerifyScene(Detached, {});

		auto Destination = std::make_shared<DataModel>();
		const ScopedFixtureGraph DestinationLifetime(Destination);
		Destination->InitializeLoadedProjectRevision();
		const auto Parent = Destination->GetService("Workspace");
		const auto Studio = ScriptSecurityContext::StudioCoreUi();
		const auto Authority = MutationAuthorityContext::Studio(Studio, Destination->GetObjectId());
		const auto Before = Published(*Destination, *Parent);
		MutationGateway Gateway;
		const auto Document = std::make_shared<const Gas::Document>(*Parsed);
		const ImportGasModelCommand Command{Document, Parent->GetObjectId(), Before.Revision};
		Require(Gateway.Apply(Command, ScriptSecurityContext::CoreTrusted()).Status == MutationStatus::Unauthorized,
			"Require Studio authoring authority for model import");
		auto OtherParent = Command;
		OtherParent.Parent = SourceWorld->GetService("Workspace")->GetObjectId();
		Require(Gateway.Apply(OtherParent, Authority).Status == MutationStatus::InvalidParent,
			"Reject a parent in another DataModel");
		auto Stale = Command;
		++Stale.ExpectedRevision;
		Require(Gateway.Apply(Stale, Authority).Status == MutationStatus::Conflict, "Reject stale optimistic revision");

		auto Bad = std::make_shared<Gas::Document>(*Parsed);
		auto Failure = Command;
		Failure.DocumentValue = Bad;
		Bad->Nodes.front().Class = {0x1234, 0x5678};
		Require(Gateway.Apply(Failure, Authority).Status == MutationStatus::InvalidClass, "Reject unknown model class");
		*Bad = *Parsed;
		const std::array<std::uint8_t, 1> AssetBytes{42};
		Bad->Dependencies.push_back({{1, 2}, AssetContentId::Hash(AssetBytes)});
		Require(Gateway.Apply(Failure, Authority).Status == MutationStatus::ValidationFailed, "Reject unresolved asset pin");
		*Bad = *Parsed;
		const auto WeldNode = std::ranges::find(Bad->Nodes, std::string("BridgeWeld"), &Gas::Node::Name);
		const auto ScriptNode = std::ranges::find(Bad->Nodes, std::string("Logic"), &Gas::Node::Name);
		Require(WeldNode != Bad->Nodes.end() && ScriptNode != Bad->Nodes.end(), "Find document targets");
		const auto ScriptIndex = static_cast<std::uint32_t>(ScriptNode - Bad->Nodes.begin() + 1);
		WeldNode->SavedProperties.at("Part1") = Gas::LocalReference{ScriptIndex};
		Require(Gateway.Apply(Failure, Authority).Status == MutationStatus::ValidationFailed,
			"Reject wrong-class reference during detached materialization");
		*Bad = *Parsed;
		const auto SoundNode = std::ranges::find(Bad->Nodes, std::string("NearbySound"), &Gas::Node::Name);
		Require(SoundNode != Bad->Nodes.end(), "Find persisted Sound in the model fixture");
		SoundNode->SavedProperties.at("RollOffMinDistance") = WireValue{6.0f};
		Require(Gateway.Apply(Failure, Authority).Status == MutationStatus::ValidationFailed,
			"Reject an invalid Sound rolloff pair during detached model import");
		Require(Published(*Destination, *Parent) == Before, "Invalid imports leave graph, history and journal untouched");
		std::size_t AddedSignals = 0;
		std::size_t RemovedSignals = 0;
		bool ListenerSawCommit = false;
		const ScopedFixtureConnection Added{Parent->ChildAdded->Connect([&](const std::shared_ptr<Instance> &) {
			++AddedSignals;
			const auto Observed = Published(*Destination, *Parent);
			ListenerSawCommit = Observed.Revision == Before.Revision + 1 &&
				Observed.History == Before.History + 1 && Observed.NextJournalSequence > Before.NextJournalSequence;
		})};
		const ScopedFixtureConnection Removed{Parent->ChildRemoved->Connect([&](const std::shared_ptr<Instance> &) {
			++RemovedSignals;
		})};
		{
			const ScopedJournalCapacity Capacity(1);
			Require(!Gateway.Apply(Command, Authority).Succeeded() && Published(*Destination, *Parent) == Before &&
				Destination->Tags.GetTagged(Destination->GetObjectId(), "Portable", Studio).empty() &&
				AddedSignals == 0 && RemovedSignals == 0,
				"Rollback a failed journal reservation before publishing hierarchy signals");
		}

		const auto Imported = Gateway.Apply(Command, Authority);
		Added.Connection->Disconnect();
		Removed.Connection->Disconnect();
		Require(Imported.Succeeded() && Imported.Object.has_value(), "Commit an authorized model forest");
		Require(AddedSignals == 1 && RemovedSignals == 0 && ListenerSawCommit,
			"Native hierarchy listeners observe the committed history and journal");
		const auto ImportedRoot = std::dynamic_pointer_cast<Folder>(ObjectRegistry::Get().Lookup(*Imported.Object));
		Require(ImportedRoot && ImportedRoot->GetDataModel() == Destination && ImportedRoot->Children.size() == 5 &&
			ImportedRoot->Children.front()->GetName() == "PartA" && ImportedRoot->Children.back()->GetName() == "Rig",
			"Adopt forest under one ordered Folder in the destination scope");
		const auto After = Published(*Destination, *Parent);
		Require(After.Revision == Before.Revision + 1 && After.History == Before.History + 1 &&
			After.NextJournalSequence > Before.NextJournalSequence && After.OpenTransactions == 0,
			"Publish one authoritative revision and one history transaction");
		const auto ImportedScene = SceneUnder(*ImportedRoot);
		Require(ImportedScene.PartA->GetObjectId() != Source.PartA->GetObjectId() &&
			ImportedScene.Logic->GetSourceId() != Source.Logic->GetSourceId(),
			"Allocate fresh Instance and Script resource identities");
		VerifyScene(ImportedScene, Destination);
		const auto ImportedPartId = ImportedScene.PartA->GetObjectId();
		const auto ImportedScriptId = ImportedScene.Logic->GetSourceId();

		Require(Gateway.Undo(*Destination, Studio).Succeeded() && !ObjectRegistry::Get().Lookup(*Imported.Object) &&
			!ObjectRegistry::Get().Lookup(ImportedPartId) && Parent->Children.size() == Before.Children,
			"Undo retires the entire forest and its old generation IDs");
		Require(Gateway.Redo(*Destination, Studio).Succeeded(), "Redo restores model forest as one subtree");
		const auto RestoredRoot = ObjectRegistry::Get().Lookup(Destination->Transactions.ResolveIdentity(*Imported.Object));
		Require(RestoredRoot != nullptr, "Resolve recreated root through existing history remapping");
		const auto Restored = SceneUnder(*RestoredRoot);
		Require(Restored.PartA->GetObjectId() != ImportedPartId && Restored.Logic->GetSourceId() == ImportedScriptId,
			"Redo generates fresh live identities and preserves the imported Script resource identity");
		VerifyScene(Restored, Destination);

		auto Full = std::make_shared<DataModel>();
		const ScopedFixtureGraph FullLifetime(Full);
		const auto FullParent = Full->GetService("Workspace");
		const auto Core = ScriptSecurityContext::CoreTrusted();
		for (std::size_t Index = 0; Index < MaximumDistinctTagsPerDataModel; ++Index) {
			if (Index % MaximumTagsPerInstance == 0) Create<Folder>("TagGroup", FullParent);
			Require(Full->Tags.Add(Full->GetObjectId(), FullParent->Children.back()->GetObjectId(),
				"Existing" + std::to_string(Index), Core), "Fill destination tag index to its limit");
		}
		const auto FullBefore = Published(*Full, *FullParent);
		Require(!Gateway.Apply(ImportGasModelCommand{Document, FullParent->GetObjectId(), FullBefore.Revision},
			MutationAuthorityContext::Studio(Studio, Full->GetObjectId())).Succeeded() &&
			Published(*Full, *FullParent) == FullBefore && Full->Tags.GetTagged(Full->GetObjectId(), "Portable", Core).empty(),
			"Roll back an authorized import whose candidate exceeds the destination tag limit");
	}

	class TestProjectDirectory {
	  public:
		const std::filesystem::path Root = std::filesystem::temp_directory_path() / ("gas-" + ProjectId::New().ToString());

		TestProjectDirectory() { Require(std::filesystem::create_directory(Root), "Create isolated project directory"); }
		~TestProjectDirectory() {
			std::error_code Ignored;
			std::filesystem::remove_all(Root, Ignored);
		}
	};

	template <typename Brush>
	void CommitTerrainBrush(terrain::Field &Field, const Brush &Edit) {
		const auto Get = [](auto Value) {
			if (!Value) throw std::runtime_error("[Terrain:Persistence] Fixture brush failed: " +
				std::string(terrain::GetFieldErrorText(Value.error())));
			return std::move(*Value);
		};
		const auto Bounds = Get(terrain::GetBrushBounds(Field.GetGrid(), Edit));
		const auto Keys = Get(terrain::GetRegionChunks(Bounds));
		auto Snapshot = Get(Field.Capture(Keys));
		auto Candidate = Get(terrain::Field::Prepare(std::move(Snapshot), Edit));
		(void)Get(Field.Commit(std::move(Candidate)));
	}

	std::shared_ptr<Workspace> GetWorkspace(DataModel &World) {
		auto Space = std::dynamic_pointer_cast<Workspace>(World.GetService("Workspace"));
		Require(Space && Space->GetTerrain(), "Find the canonical Workspace.Terrain");
		return Space;
	}

	void MakeTerrainProject(const std::filesystem::path &Root, bool PackageFixture = false, bool NetworkFixture = false) {
		DiskFilesystem Filesystem(Root);
		auto ProjectValue = Project::fromInit(&Filesystem, "Terrain persistence qualification");
		const auto World = ProjectValue.DeserializeGame();
		const ScopedFixtureGraph Lifetime(World);
		World->InitializeLoadedProjectRevision();
		const auto Space = GetWorkspace(*World);
		auto Field = std::make_unique<terrain::Field>(terrain::GridSpec{-1});
		using enum terrain::BrushMode;
		CommitTerrainBrush(*Field, terrain::SphereBrush{{-.22, 8.23, 8.6}, 3.72, Add, 2});
		CommitTerrainBrush(*Field, terrain::SphereBrush{{-.22, 8.23, 8.6}, 3.8, Paint, 5});
		if (NetworkFixture) {
			const auto ScriptValue = Create<Script>("TerrainServerQualification", Space);
			ScriptValue->SetSource(R"(
				local Terrain = game.Workspace.Terrain
				local Params = RaycastParams.new()
				Params.IncludeTerrain = true
				local Hit = game.Workspace:Raycast(Vector3.new(-0.5, 14, 8.5), Vector3.new(0, -14, 0), Params)
				assert(Hit and Hit.Instance == Terrain, "server terrain must be installed before Luau starts")
				print("[Terrain:NetworkFixture] Server ray hit the authoritative terrain")
			)");
		} else if (PackageFixture) {
			const auto ScriptValue = Create<Script>("TerrainPackageQualification", Space);
			ScriptValue->SetSource(R"(
				local Terrain = game.Workspace.Terrain
				assert(Terrain:GetGridInfo().SampleSpacing == 0.5)
				local Min = Vector3.new(-1, 16, 17)
				local MaxExclusive = Vector3.new(0, 17, 18)
				local Samples = Terrain:ReadRegion(Min, MaxExclusive)
				assert(buffer.readi16(Samples, 0) < 0 and buffer.readu32(Samples, 2) == 5)
				local Params = RaycastParams.new()
				Params.IncludeTerrain = true
				local Hit = game.Workspace:Raycast(Vector3.new(-0.5, 14, 8.5), Vector3.new(0, -14, 0), Params)
				assert(Hit and Hit.Instance == Terrain)
				local Ticket = Terrain:PaintBall(Vector3.new(-0.22, 8.23, 8.6), 1.5, 2)
				for Step = 1, 400 do
					task.wait()
					local Receipt = Terrain:GetEditStatus(Ticket)
					if Receipt.Status ~= "Pending" then
						assert(Receipt.Status == "Committed" and Receipt.CollisionReady)
						Samples = Terrain:ReadRegion(Min, MaxExclusive)
						assert(buffer.readu32(Samples, 2) == 2)
						print("[Terrain:Package] Restored sample spacing, material, collision and gameplay editing")
						return
					end
				end
				error("[Terrain:Package] Runtime edit timed out")
			)");
		} else {
			CommitTerrainBrush(*Field, terrain::BoxBrush{{-37, 8, 9}, {6.1, 4.9, 4.8}, Add, 3});
			CommitTerrainBrush(*Field, terrain::SphereBrush{{-36, 8, 9}, 2.6, Subtract, 0});
			CommitTerrainBrush(*Field, terrain::SphereBrush{{106.3, 13, -90}, 2.8, Add, 7});
		}
		Space->RestoreTerrainField(std::move(Field));
		ProjectValue.PersistGameAtomically(ProjectValue.CaptureGame(World, World->GetAuthoritativeRevision()));
	}

	void MakeTerrainPaletteProject(const std::filesystem::path &Root) {
		MakeTerrainProject(Root, true);
		DiskFilesystem Filesystem(Root);
		auto ProjectValue = Project::FromGasOrigin(&Filesystem);
		const auto World = ProjectValue.DeserializeGame();
		const ScopedFixtureGraph Lifetime(World);
		World->InitializeLoadedProjectRevision();
		const auto Space = GetWorkspace(*World);
		std::filesystem::create_directories(Root / "assets");
		{
			std::ofstream Source(Root / "assets/terrain-tint.gltf", std::ios::binary);
			Source << R"({
				"asset":{"version":"2.0"},
				"buffers":[{"byteLength":42,"uri":"data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIA"}],
				"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6}],
				"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3",
					"min":[0,0,0],"max":[1,1,0]},{"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}],
				"materials":[{"name":"TerrainTint","pbrMetallicRoughness":{"baseColorFactor":[0.25,0.5,0.75,1]}}],
				"meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1,"material":0}]}],
				"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0
			})";
			Require(Source.good(), "Write the canonical terrain material source fixture");
		}
		auto Assets = std::dynamic_pointer_cast<AssetService>(World->GetService("AssetService"));
		SourceMount Mount(Filesystem);
		auto Imported = Assets->ImportProjectAsset(Mount, "assets/terrain-tint.gltf", AssetKind::Mesh);
		const auto Material = std::ranges::find(Imported.Records, AssetKind::Material, &AssetRecord::Kind);
		Require(Imported.Ok && Material != Imported.Records.end(), "Import project material through the asset catalog");
		nlohmann::ordered_json Bindings = nlohmann::ordered_json::array({{{"Id", 5}, {"Asset", Material->Id.ToString()}}});
		Require(Space->ApplyAttributeMutation("TerrainMaterialBindings", WireValue{Bindings.dump()}) == MutationStatus::Success,
			"Persist the stable numeric terrain material binding");
		const auto PriorManifest = ProjectValue.GetPersistedTerrainManifest();
		const auto Captured = ProjectValue.CaptureGame(World, World->GetAuthoritativeRevision());
		Require(Captured.Terrain && Captured.Terrain->ManifestDigest == PriorManifest,
			"The project palette leaves the canonical terrain field bytes intact");
		ProjectValue.PersistGameAtomically(Captured);
	}

	void TestTerrainProjectPalette() {
		using Json = nlohmann::ordered_json;
		const TestProjectDirectory Directory;
		MakeTerrainPaletteProject(Directory.Root);
		{
			EditorHost Host("terrain-palette-test");
			std::uint64_t Request = 0;
			const auto Call = [&](std::string_view Method, const Json &Parameters = Json::object()) {
				return Json::parse(Host.HandleRequest(Json{
					{"Version", EditorHostProtocolVersion}, {"RequestId", std::to_string(++Request)},
					{"SessionToken", "terrain-palette-test"}, {"Method", Method}, {"Params", Parameters},
				}.dump()));
			};
			const auto Handshake = Call("Handshake");
			Require(std::ranges::any_of(Handshake.at("Result").at("Capabilities"),
				[](const Json &Capability) { return Capability == "TerrainMaterialBindingsV1"; }),
				"Studio discovers compatible terrain material binding support");
			const auto Opened = Call("OpenProject", {{"Root", Directory.Root.generic_string()}});
			const auto Snapshot = Call("GetSnapshot");
			const auto &Objects = Snapshot.at("Result").at("Snapshot").at("Objects");
			const auto Target = std::ranges::find_if(Objects, [](const Json &Object) {
				return Object.at("ClassName") == "Workspace";
			});
			Require(Target != Objects.end(), "Resolve the canonical Workspace by the project snapshot");
			const auto Revision = Opened.at("Result").at("ProjectState").at("AuthoritativeRevision");
			Json Change{{"Object", Target->at("Id")}, {"Attribute", "TerrainMaterialBindings"},
				{"Value", {{"Type", "String"}, {"Value", "[]"}}},
				{"ExpectedRevision", Revision.get<std::uint64_t>() + 1}};
			Require(Call("SetAttribute", Change).at("Error").at("Code") == "Conflict",
				"A stale Studio revision cannot change the project palette");
			Change["ExpectedRevision"] = Revision;
			Require(Call("SetAttribute", Change).at("Ok") == true && Call("Undo").at("Ok") == true &&
				Call("SaveProject").at("Ok") == true, "Studio can edit, undo and save palette bindings");
		}
		DiskFilesystem Filesystem(Directory.Root);
		const auto World = Project::FromGasOrigin(&Filesystem).DeserializeGame();
		const ScopedFixtureGraph Lifetime(World);
		const auto Space = GetWorkspace(*World);
		Require(Space->GetAttributeValue("TerrainMaterialBindings").has_value(), "Project terrain palette reopens");
		HeadlessRenderer Renderer(Vector2(64, 64));
		Engine Runtime(World, &Renderer, nullptr);
		const auto RenderState = Space->GetTerrainRenderState();
		constexpr std::array<std::uint8_t, 4> Tint{64, 128, 191, 255};
		Require(RenderState && std::ranges::any_of(RenderState->Chunks, [&](const auto &Chunk) {
			return std::ranges::any_of(*Chunk.second.Resource.Vertices, [&](const auto &Vertex) { return Vertex.Tint == Tint; });
		}) && Space->IsTerrainCollisionReady(), "Engine publishes project terrain tint and retains native collision");
		Runtime.Destroy();
	}

	void TestTerrainProjectGrid() {
		using Json = nlohmann::ordered_json;
		const TestProjectDirectory Directory;
		EditorHost Host("terrain-grid-test");
		std::uint64_t Request = 0;
		const auto Call = [&](std::string_view Method, const Json &Parameters = Json::object()) {
			return Json::parse(Host.HandleRequest(Json{
				{"Version", EditorHostProtocolVersion}, {"RequestId", std::to_string(++Request)},
				{"SessionToken", "terrain-grid-test"}, {"Method", Method}, {"Params", Parameters},
			}.dump()));
		};
		const auto InvalidRoot = Directory.Root / "invalid";
		const auto Invalid = Call("CreateProject", {{"Destination", InvalidRoot.generic_string()},
			{"Name", "Invalid terrain grid"}, {"TerrainSampleSpacing", 0.25}});
		Require(Invalid.at("Error").at("Code") == "InvalidTerrainSpacing" && !std::filesystem::exists(InvalidRoot),
			"An unsupported authored terrain spacing leaves the destination untouched");
		for (const auto &[Name, Spacing] : std::array{
				std::pair{"half", 0.5}, std::pair{"default", 1.0}, std::pair{"double", 2.0}}) {
			const auto Root = (Directory.Root / Name).generic_string();
			Json Params{{"Destination", Root}, {"Name", "Terrain grid profile"}};
			if (Spacing != 1.0) Params["TerrainSampleSpacing"] = Spacing;
			Require(Call("CreateProject", Params).at("Ok") == true &&
				Call("GetTerrainAuthoringState").at("Result").at("SampleSpacing") == Spacing,
				"Creator selects the exact terrain spacing at project creation");
			Require(Call("SaveProject").at("Ok") == true && Call("OpenProject", {{"Root", Root}}).at("Ok") == true &&
				Call("GetTerrainAuthoringState").at("Result").at("SampleSpacing") == Spacing,
				"Empty saved terrain retains its authored sample spacing when reopened");
		}
	}

	void TestTerrainPlace() {
		using Json = nlohmann::ordered_json;
		using Clock = std::chrono::steady_clock;
		const auto MillisecondsSince = [](Clock::time_point Started) {
			return std::chrono::duration<double, std::milli>(Clock::now() - Started).count();
		};
		const TestProjectDirectory Directory;
		const auto FirstSaveStarted = Clock::now();
		MakeTerrainProject(Directory.Root);
		const auto FirstSaveMs = MillisecondsSince(FirstSaveStarted);
		DiskFilesystem Filesystem(Directory.Root);
		const auto Metadata = Directory.Root / ".gargantuan/project.json";
		const auto Backup = Directory.Root / ".gargantuan/project.json.saved";
		std::filesystem::rename(Metadata, Backup);
		bool MissingIdentityRejected = false;
		try { (void)Project::fromExisting(&Filesystem); }
		catch (const std::runtime_error &) { MissingIdentityRejected = true; }
		Require(MissingIdentityRejected && !std::filesystem::exists(Metadata),
			"Terrain project admission cannot mint a replacement identity");
		std::filesystem::rename(Backup, Metadata);
		auto Origin = Project::FromGasOrigin(&Filesystem);
		const auto ReopenStarted = Clock::now();
		const auto World = Origin.DeserializeGame();
		const auto ReopenMs = MillisecondsSince(ReopenStarted);
		const ScopedFixtureGraph WorldLifetime(World);
		World->InitializeLoadedProjectRevision();
		const auto &Field = GetWorkspace(*World)->GetTerrain()->GetField();
		Require(Field.GetGrid().GetSpacing() == .5 && Field.Read({-1, 16, 17})->Material == 5 &&
			Field.Read({-83, 16, 18})->Material == 3 && Field.Read({-72, 16, 18})->Density > 0 &&
			Field.Read({213, 26, -180})->Material == 7 && Field.Read({0, 0, 0}) == terrain::AirSample,
			"Reopen the authored lattice, negative samples, excavated cave and distant terrain");
		const auto First = Origin.CaptureGame(World, World->GetAuthoritativeRevision(), true);
		Require(First.Terrain && First.Terrain->ManifestDigest == Origin.GetPersistedTerrainManifest(),
			"Capture reproduces the persisted canonical field bytes");
		std::istringstream Detached(First.Contents);
		Require(!InstanceSerialization::Deserialize(Origin.InstanceFileFormat, Detached).Ok,
			"Reject a terrain scene without its authenticated project closure");
		auto Tampered = Json::parse(First.Contents);
		Tampered["Version"] = 5;
		Require(!InstanceSerialization::InspectTerrainManifest(Tampered.dump()), "Reject a manifest on a legacy scene version");
		Tampered["Version"] = InstanceSerialization::TerrainSceneVersion;
		Tampered.erase("TerrainManifest");
		Require(!InstanceSerialization::InspectTerrainManifest(Tampered.dump()), "Reject a terrain scene missing its manifest digest");
		Origin.PersistGameAtomically(First);
		const auto IncrementalStarted = Clock::now();
		const auto Unchanged = Origin.CaptureGame(World, World->GetAuthoritativeRevision(), true);
		Require(Unchanged.Terrain && Unchanged.Terrain->Artifacts.size() == 1 &&
			Unchanged.Terrain->ReusedChunks == First.Terrain->Index.Chunks.size(),
			"Second save reuses every unchanged terrain chunk");
		Origin.PersistGameAtomically(Unchanged);
		const auto IncrementalMs = MillisecondsSince(IncrementalStarted);
		std::cout << "[Terrain:Persistence] chunks=" << First.Terrain->Index.Chunks.size()
			<< " bytes=" << First.Terrain->ClosureBytes << " setup_and_first_save_ms=" << FirstSaveMs
			<< " scene_reopen_ms=" << ReopenMs << " unchanged_save_ms=" << IncrementalMs << '\n';
		const auto PublishedContents = Filesystem.ReadFileToString(Origin.InstanceFilePath);
		auto CompetingSave = Unchanged;
		CompetingSave.Contents += '\n';
		bool IdentityChangedRejected = false;
		try {
			Origin.PersistGameAtomically(CompetingSave, [&] {
				std::filesystem::rename(Metadata, Backup);
				std::ofstream ProcessRace(Metadata, std::ios::binary);
				ProcessRace << Json{{"Version", 1}, {"ProjectId", ProjectId::New().ToString()}}.dump();
				Require(ProcessRace.good(), "Write independently replaced fixture metadata");
			});
		} catch (const std::runtime_error &) {
			IdentityChangedRejected = true;
		}
		Require(std::filesystem::remove(Metadata), "Remove independently replaced fixture metadata");
		std::filesystem::rename(Backup, Metadata);
		Require(IdentityChangedRejected && Filesystem.ReadFileToString(Origin.InstanceFilePath) == PublishedContents,
			"Scene replacement is denied if the destination identity changes after capture");
		std::optional<terrain::TerrainCapture> GasTerrain;
		const auto GasPlace = Origin.CaptureGasPlace(World, &GasTerrain);
		Require(GasPlace && GasTerrain && Gas::GetFormatVersion(*GasPlace) == Gas::TerrainPlaceFormatVersion,
			"Capture authenticated versioned terrain GASP");
		Origin.PersistTerrainArtifacts(*GasTerrain);
		auto Place = Gas::Decode(Gas::Encode(*GasPlace).value()).value();
		Require(Place.Place->TerrainManifest == Origin.GetPersistedTerrainManifest(), "Persist a pinned GASP terrain digest");
		const auto Replacement = Origin.LoadGasPlace(Place);
		const ScopedFixtureGraph ReplacementLifetime(Replacement);
		Replacement->InitializeLoadedProjectRevision();
		const auto Rebound = Origin.CaptureGame(Replacement, 1);
		Require(Rebound.Terrain && Rebound.Terrain->ManifestDigest == First.Terrain->ManifestDigest &&
			GetWorkspace(*Replacement)->GetTerrain()->GetField().GetLifetime() != Field.GetLifetime(),
			"GASP replacement restores exact content in a new field lifetime");
		const auto Chunk = terrain::GetTerrainArtifactPath(First.Terrain->Artifacts.front().Id).value();
		const auto ChunkPath = Directory.Root / Chunk;
		auto Accepts = [&] {
			try {
				const auto Candidate = Origin.DeserializeGame();
				Candidate->Destroy();
				return true;
			} catch (const std::exception &) { return false; }
		};
		Require(std::filesystem::remove(ChunkPath) && !Accepts(), "Missing terrain chunk fails closed");
		bool FailedIncremental = false;
		try { Origin.PersistGameAtomically(Unchanged); }
		catch (const std::runtime_error &) { FailedIncremental = true; }
		Require(FailedIncremental && Origin.GetPersistedTerrainManifest() == Place.Place->TerrainManifest,
			"A failed incremental write preserves the previous scene root");
		{
			const std::ofstream Broken(ChunkPath, std::ios::binary);
			Require(Broken.good(), "Create a corrupt terrain artifact fixture");
		}
		Require(!Accepts(), "Corrupt terrain chunk fails closed");
		Require(std::filesystem::remove(ChunkPath), "Remove the corrupt terrain artifact fixture");
		Origin.PersistGameAtomically(First);
		Require(Accepts(), "A verified complete closure reopens again");
		Require(std::filesystem::remove(Origin.InstanceFilePath), "Remove the fixture scene to exercise GASP restore");
		Origin.RestoreGasPlace(Replacement, Place.Place->PreRunSource);
		Require(Accepts() && Origin.GetPersistedTerrainManifest() == Place.Place->TerrainManifest,
			"Restore JSON from terrain GASP while preserving its verified closure");

		EditorHost Host("terrain-persistence-test");
		std::vector<ObjectId> PreviewedTerrain;
		std::vector<std::uint8_t> ViewportPixels;
		std::uint64_t Request = 0;
		const auto Call = [&](std::string_view Method, const Json &Parameters) {
			return Json::parse(Host.HandleRequest(Json{
				{"Version", EditorHostProtocolVersion}, {"RequestId", std::to_string(++Request)},
				{"SessionToken", "terrain-persistence-test"}, {"Method", Method}, {"Params", Parameters},
			}.dump()));
		};
		Require(Call("OpenProject", {{"Root", Directory.Root.generic_string()}}).at("Ok") == true,
			"Open terrain project through EditorHost");
		if (LatestFrameMailbox::IsSupported()) {
			Host.SetViewportCaptureForTesting([&](RenderPublicationPtr Publication) {
				if (Publication->TerrainDraws && !Publication->TerrainDraws->empty()) {
					const auto Owner = Publication->TerrainDraws->front().Item.Object;
					Require(Owner.IsValid() && std::ranges::all_of(*Publication->TerrainDraws,
						[&](const auto &Draw) { return Draw.Item.Object == Owner; }),
						"Editor terrain draws share one canonical semantic owner");
					PreviewedTerrain.push_back(Owner);
				}
				const auto Width = Publication->Frame.ViewportWidth, Height = Publication->Frame.ViewportHeight;
				ViewportPixels.resize(static_cast<std::size_t>(Width) * Height * 4);
				return EditorViewportCaptureView{.Width = Width, .Height = Height, .BgraPixels = ViewportPixels};
			});
			Require(Call("OpenViewportTransport", {{"Transport", "LatestFrameMailbox"}, {"Version", 2},
				{"PixelFormat", "BGRA8"}}).at("Ok") == true &&
				Call("ConfigureViewport", {{"Width", 200}, {"Height", 120}}).at("Ok") == true &&
				Call("StartViewportPresentation", {{"FrameRateMode", "Adaptive"}}).at("Ok") == true,
				"Start an editor presentation for the reopened terrain");
			Host.PumpViewportPresentationForTesting();
			Require(PreviewedTerrain.size() == 1, "The restored terrain is included in the first edit presentation");
		}
		Require(Call("GetSnapshot", Json::object()).at("Ok") == true && Call("SaveProject", Json::object()).at("Ok") == true,
			"Save the reopened terrain using the ordinary EditorHost route");
		const auto Copy = Directory.Root / "save-as";
		Require(Call("SaveProjectAs", {{"Destination", Copy.generic_string()}}).at("Ok") == true,
			"Save As captures a complete terrain closure");
		DiskFilesystem CopyFilesystem(Copy);
		const auto CopyProject = Project::FromGasOrigin(&CopyFilesystem);
		Require(CopyProject.Identity == Origin.Identity && CopyProject.GetPersistedTerrainManifest() == Place.Place->TerrainManifest,
			"Save As keeps project lineage and the canonical terrain digest");
		const auto Exported = Call("ExportGasPlace", Json::object());
		const auto State = Call("GetProjectState", Json::object());
		Require(Exported.at("Ok") == true, "Export terrain GASP from the adopted destination");
		const Json OpenParameters{{"ExpectedRevision", State.at("Result").at("AuthoritativeRevision")},
			{"DataBase64", Exported.at("Result").at("DataBase64")}};
		Require(std::filesystem::remove(Copy / Chunk), "Remove a referenced GASP terrain chunk");
		Require(Call("OpenGasPlace", OpenParameters).at("Ok") == false &&
			Call("GetProjectState", Json::object()).at("Result") == State.at("Result"),
			"Missing GASP terrain leaves the active project scope intact");
		CopyProject.PersistGameAtomically(First);
		Require(Call("OpenGasPlace", OpenParameters).at("Ok") == true, "Replace the live editor project using terrain GASP");
		if (LatestFrameMailbox::IsSupported()) {
			Host.PumpViewportPresentationForTesting();
			Require(PreviewedTerrain.size() == 2 && PreviewedTerrain.front() != PreviewedTerrain.back(),
				"GASP replacement republishes terrain with the new scene owner");
			Host.SetViewportCaptureForTesting({});
		}
	}

	void TestEditorTerrainBrush() {
		using Json = nlohmann::ordered_json;
		const TestProjectDirectory Directory;
		DiskFilesystem Filesystem(Directory.Root);
		const auto ProjectValue = Project::fromInit(&Filesystem, "Terrain brush qualification");
		EditorHost Host("terrain-brush-test");
		std::uint64_t Request = 0;
		const auto Call = [&](std::string_view Method, const Json &Parameters = Json::object()) {
			return Json::parse(Host.HandleRequest(Json{
				{"Version", EditorHostProtocolVersion}, {"RequestId", std::to_string(++Request)},
				{"SessionToken", "terrain-brush-test"}, {"Method", Method}, {"Params", Parameters},
			}.dump()));
		};
		const auto ReadState = [&] { return Call("GetTerrainAuthoringState").at("Result"); };
		Require(Call("OpenProject", {{"Root", Directory.Root.generic_string()}}).at("Ok") == true &&
			Call("GetSnapshot").at("Ok") == true, "Open terrain brush project");
		const auto Initial = ReadState();
		const auto Scope = Initial.at("Scope");
		Require(Initial.at("TerrainRevision") == "0" && Initial.at("EditingAvailable") == true,
			"Empty project exposes canonical terrain authoring state");
		std::uint32_t TerrainFrames = 0;
		std::vector<std::uint8_t> Pixels;
		if (LatestFrameMailbox::IsSupported()) {
			Host.SetViewportCaptureForTesting([&](RenderPublicationPtr Publication) {
				if (Publication->TerrainDraws && !Publication->TerrainDraws->empty()) ++TerrainFrames;
				const auto Width = Publication->Frame.ViewportWidth, Height = Publication->Frame.ViewportHeight;
				Pixels.resize(static_cast<std::size_t>(Width) * Height * 4);
				return EditorViewportCaptureView{.Width = Width, .Height = Height, .BgraPixels = Pixels};
			});
			Require(Call("OpenViewportTransport", {{"Transport", "LatestFrameMailbox"}, {"Version", 2},
				{"PixelFormat", "BGRA8"}}).at("Ok") == true &&
				Call("ConfigureViewport", {{"Width", 64}, {"Height", 64}}).at("Ok") == true &&
				Call("StartViewportPresentation", {{"FrameRateMode", "Adaptive"}}).at("Ok") == true,
				"Set up a bounded edit viewport");
		}
		const auto Submit = [&](std::string_view Method, Json Parameters) {
			const auto State = ReadState();
			Parameters["Scope"] = Scope;
			Parameters["ExpectedTerrainRevision"] = State.at("TerrainRevision");
			Parameters["ExpectedRevision"] = State.at("ProjectState").at("AuthoritativeRevision");
			const auto Response = Call(Method, Parameters);
			Require(Response.at("Ok") == true && Response.at("Result").at("Status") == "Pending",
				"Terrain command returns an asynchronous ticket");
			return Response.at("Result").at("Ticket");
		};
		const auto Await = [&](const Json &Ticket) {
			const auto Until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
			while (std::chrono::steady_clock::now() < Until) {
				const auto Reply = Call("GetTerrainEditStatus", {{"Scope", Scope}, {"Ticket", Ticket}});
				Require(Reply.at("Ok") == true, "Terrain ticket stays available in its project scope");
				if (Reply.at("Result").at("Status") != "Pending") return Reply.at("Result");
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
			throw std::runtime_error("The EditorHost terrain ticket did not finish");
		};
		const Json Fill{{"Shape", "Sphere"}, {"Mode", "Add"}, {"Center", {-0.2, 9.2, 10.1}},
			{"Radius", 3.2}, {"Material", 1}};
		auto Oversized = Fill;
		Oversized["Radius"] = 11.0;
		Require(Call("SubmitTerrainBrush", {{"Scope", Scope}, {"ExpectedTerrainRevision", "0"},
			{"Brush", Oversized}}).at("Error").at("Code") == "TerrainBrushLimit",
			"Brush footprint fails before admission without reducing resolution");
		const auto FillTicket = Submit("SubmitTerrainBrush", {{"Brush", Fill}});
		Require(Call("SaveProject").at("Error").at("Code") == "TerrainEditPending",
			"Pending terrain cannot race scene persistence");
		const auto Filled = Await(FillTicket);
		Require(Filled.at("Status") == "Committed" && Filled.at("TerrainRevision") == "1" &&
			Filled.at("ProjectState").at("Dirty") == true && Filled.at("CanUndo") == true &&
			Filled.at("ProjectState").at("History").at("CanUndo") == false,
			"Fill publishes authority and isolated terrain history");
		if (LatestFrameMailbox::IsSupported()) {
			Host.PumpViewportPresentationForTesting();
			Require(TerrainFrames == 1, "Brush refreshes the committed edit viewport");
		}
		const Json Ray{{"Scope", Scope}, {"ExpectedTerrainRevision", "1"},
			{"Origin", {-0.2, 18.0, 10.1}}, {"Direction", {0.0, -18.0, 0.0}}};
		const auto Hit = Call("RaycastTerrainSurface", Ray);
		Require(Hit.at("Ok") == true && Hit.at("Result").at("Hit").at("Position")[1].get<double>() > 11.0,
			"Terrain surface targeting sees the admitted native collider");
		Require(Call("SaveProject").at("Ok") == true, "Save the filled terrain");
		const auto FilledDigest = ProjectValue.GetPersistedTerrainManifest();
		Require(FilledDigest.has_value(), "Fill persisted as a canonical terrain manifest");
		Require(Call("SubmitTerrainBrush", {{"Scope", Scope}, {"ExpectedTerrainRevision", "0"},
			{"Brush", Fill}}).at("Error").at("Code") == "Conflict",
			"Older terrain revisions cannot overwrite a committed brush");

		Json Paint{{"Shape", "Box"}, {"Mode", "Paint"}, {"Center", {-0.2, 9.2, 10.1}},
			{"HalfExtent", {1.3, 1.3, 1.3}}, {"Material", 5}};
		Require(Await(Submit("SubmitTerrainBrush", {{"Brush", Paint}})).at("TerrainRevision") == "2" &&
			Call("SaveProject").at("Ok") == true, "Commit and save bounded Box paint");
		const auto PaintedDigest = ProjectValue.GetPersistedTerrainManifest();
		Require(PaintedDigest && PaintedDigest != FilledDigest, "Paint changes the canonical terrain closure");
		const auto Undone = Await(Submit("UndoTerrainEdit", Json::object()));
		Require(Undone.at("TerrainRevision") == "3" && Undone.at("CanRedo") == true &&
			Call("SaveProject").at("Ok") == true && ProjectValue.GetPersistedTerrainManifest() == FilledDigest,
			"Undo restores the exact previously saved sample bytes");
		const auto Redone = Await(Submit("RedoTerrainEdit", Json::object()));
		Require(Redone.at("TerrainRevision") == "4" && Redone.at("CanUndo") == true &&
			Call("SaveProject").at("Ok") == true && ProjectValue.GetPersistedTerrainManifest() == PaintedDigest,
			"Redo restores the exact painted sample bytes");

		Paint["Center"] = {90.0, 9.2, 10.1};
		const auto NoChange = Await(Submit("SubmitTerrainBrush", {{"Brush", Paint}}));
		Require(NoChange.at("Status") == "Committed" && NoChange.at("TerrainRevision") == "4" &&
			NoChange.at("ProjectState").at("Dirty") == false && NoChange.at("CanUndo") == true,
			"An empty paint remains a no-op and preserves terrain undo");
		const auto Pending = Submit("SubmitTerrainBrush", {{"Brush", Fill}});
		Require(Call("CancelTerrainEdit", {{"Scope", Scope}, {"Ticket", Pending}}).at("Result").at("Status") ==
			"CancelRequested" && Await(Pending).at("Status") == "Canceled" && ReadState().at("TerrainRevision") == "4",
			"Cancel ends pending work without changing the field");
		const auto Game = std::dynamic_pointer_cast<DataModel>(ObjectRegistry::Get().Lookup(
			JsonCodec::DecodeObjectId(Scope).value().ToObjectId()));
		const auto Space = GetWorkspace(*Game);
		const auto Domain = std::make_shared<SpatialRegion>();
		Domain->SetParent(Space);
		Space->SetCurrentSpatialRegion(Domain);
		const auto Local = ReadState();
		const auto LocalScope = Local.at("Scope");
		Require(LocalScope != Scope && Local.at("TerrainRevision") == "0" && Local.at("CanUndo") == false,
			"an independently selected terrain starts empty and cannot replay the default terrain history");
		Require(Call("RaycastTerrainSurface", Ray).at("Error").at("Code") == "StaleTerrainScope",
			"an old default viewport ray cannot target the new domain");
		const auto LocalSubmit = Call("SubmitTerrainBrush", {{"Scope", LocalScope}, {"ExpectedTerrainRevision", "0"},
			{"ExpectedRevision", Local.at("ProjectState").at("AuthoritativeRevision")}, {"Brush", Fill}});
		Require(LocalSubmit.at("Ok") == true, "selected domain admits a local brush");
		const auto DomainTicket = LocalSubmit.at("Result").at("Ticket");
		Space->SetCurrentSpatialRegion(std::nullopt);
		Require(Call("CancelTerrainEdit", {{"Scope", Scope}, {"Ticket", DomainTicket}}).at("Ok") == false,
			"a same-number ticket in another terrain cannot cancel the editing domain");
		const auto Limit = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		Json LocalReceipt;
		do {
			LocalReceipt = Call("GetTerrainEditStatus", {{"Scope", LocalScope}, {"Ticket", DomainTicket}}).at("Result");
			if (LocalReceipt.at("Status") != "Pending") break;
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		} while (std::chrono::steady_clock::now() < Limit);
		Require(LocalReceipt.at("Status") == "Committed" && LocalReceipt.at("TerrainRevision") == "1" &&
			ReadState().at("TerrainRevision") == "4" && ReadState().at("CanUndo") == false,
			"an edit can finish on its addressed domain after the camera returns to Workspace");
		Domain->Destroy();
		Require(Call("GetTerrainEditStatus", {{"Scope", LocalScope}, {"Ticket", DomainTicket}}).at("Ok") == false,
			"a retired domain receipt cannot resolve to another terrain");
		Require(Call("OpenProject", {{"Root", Directory.Root.generic_string()}}).at("Ok") == true &&
			ReadState().at("CanUndo") == false &&
			Call("GetTerrainEditStatus", {{"Scope", Scope}, {"Ticket", Pending}}).at("Error").at("Code") == "StaleTerrainScope",
			"Reopening clears terrain history and old scope tickets");
	}

	void TestRuntimeGeneratedTerrainSave() {
		const TestProjectDirectory Directory;
		DiskFilesystem Filesystem(Directory.Root);
		auto ProjectValue = Project::fromInit(&Filesystem, "Runtime terrain qualification");
		const auto World = ProjectValue.DeserializeGame();
		const ScopedFixtureGraph Lifetime(World);
		World->InitializeLoadedProjectRevision();
		const auto ScriptValue = Create<Script>("RuntimeTerrainSaveQualification", GetWorkspace(*World));
		ScriptValue->SetSource(R"(
			local Terrain = game.Workspace.Terrain
			local Center = Vector3.new(12, 10.1, 12)
			local function Await(Ticket)
				for Step = 1, 1000 do
					task.wait()
					local Receipt = Terrain:GetEditStatus(Ticket)
					if Receipt.Status ~= "Pending" then
						assert(Receipt.Status == "Committed" and Receipt.CollisionReady, Receipt.Status)
						return
					end
				end
				error("[Terrain:Persistence] Timed out waiting for a public terrain edit")
			end
			Await(Terrain:FillBall(Center, 3.21, 4))
			local Paint = Terrain:PaintBall(Center, 2.1, 7)
			assert(Terrain:GetEditStatus(Paint).Status == "Pending")
			script:SetAttribute("PaintPending", true)
			Await(Paint)
			script:SetAttribute("PaintCommitted", true)
		)");
		HeadlessRenderer Renderer(Vector2(64.f, 64.f));
		Engine Runtime(World, &Renderer, {}, EngineProviderConfiguration{.AudioEnabled = false});
		Runtime.ProcessService->Alive = true;
		const auto AwaitAttribute = [&](std::string_view Name) {
			for (int Frame = 0; Frame < 1200 && ScriptValue->Status != ScriptStatus::Error; ++Frame) {
				Runtime.Step();
				if (const auto Attribute = ScriptValue->GetAttributeValue(std::string(Name));
					Attribute && std::holds_alternative<bool>(*Attribute) && std::get<bool>(*Attribute)) return;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			throw std::runtime_error("[Terrain:Persistence] " + ScriptValue->ErrorMessage);
		};
		AwaitAttribute("PaintPending");
		const auto Before = ProjectValue.CaptureGame(World, World->GetAuthoritativeRevision(), true);
		ProjectValue.PersistGameAtomically(Before);
		const auto First = ProjectValue.DeserializeGame();
		const ScopedFixtureGraph FirstLifetime(First);
		Require(GetWorkspace(*First)->GetTerrain()->GetField().Read({12, 10, 12})->Material == 4,
			"Save from a live Engine retains the last committed material while paint is pending");
		AwaitAttribute("PaintCommitted");
		const auto After = ProjectValue.CaptureGame(World, World->GetAuthoritativeRevision(), true);
		ProjectValue.PersistGameAtomically(After);
		const auto Second = ProjectValue.DeserializeGame();
		const ScopedFixtureGraph SecondLifetime(Second);
		Require(GetWorkspace(*Second)->GetTerrain()->GetField().Read({12, 10, 12})->Material == 7 &&
			After.Terrain && Before.Terrain && After.Terrain->ManifestDigest != Before.Terrain->ManifestDigest,
			"Save publishes the next committed gameplay edit through the ordinary project closure");
		Runtime.Destroy();
	}

	void TestTerrainSceneCapacity() {
		terrain::Field Field;
		for (std::int64_t Index = 0; Index < 129; ++Index) {
			const std::array Writes{terrain::SampleWrite{{64 * Index, 0, 0}, {-16384, 3}}};
			const std::array Keys{terrain::GetChunkKey(Writes.front().Point)};
			auto Edit = terrain::Field::Prepare(Field.Capture(Keys).value(), Writes).value();
			Require(Field.Commit(std::move(Edit)).has_value(), "Seed a separated contour owner");
		}
		const auto Identity = ProjectId::New();
		const auto Capture = terrain::CaptureTerrain(Field, Identity, terrain::GetTerrainDatasetId(Identity)).value();
		Require(Capture.Index.Chunks.size() == 129, "Encode a valid native directory above the offline contour profile");
		bool Refused = false;
		try {
			(void)terrain::LoadSceneTerrain(Identity, Capture.ManifestDigest,
				[&](const terrain::TerrainArtifactId &Id, std::size_t Maximum) {
					return terrain::ReadCapturedTerrainArtifact(Capture.Artifacts, Id, Maximum);
				});
		} catch (const std::runtime_error &Error) {
			Refused = std::string_view(Error.what()).find("contour candidates") != std::string_view::npos;
		}
		Require(Refused, "A valid native closure that cannot start is refused at project and package admission");
	}

	void TestEditorTransfer(const std::filesystem::path &Root) {
		using Json = nlohmann::ordered_json;
		EditorHost Host("gas-authoring-test");
		std::uint64_t Request = 0;
		const auto Call = [&](std::string_view Method, const Json &Parameters) {
			return Json::parse(Host.HandleRequest(Json{
				{"Version", EditorHostProtocolVersion}, {"RequestId", std::to_string(++Request)},
				{"SessionToken", "gas-authoring-test"}, {"Method", Method}, {"Params", Parameters},
			}.dump()));
		};
		const auto Opened = Call("OpenProject", {{"Root", Root.generic_string()}});
		Require(Opened.at("Ok") == true, "EditorHost opens the restored JSON project");
		const auto Snapshot = Call("GetSnapshot", Json::object());
		const auto &Objects = Snapshot.at("Result").at("Snapshot").at("Objects");
		const auto Workspace = std::ranges::find_if(Objects, [](const Json &Object) {
			return Object.at("ClassName") == "Workspace";
		});
		const auto Panel = std::ranges::find_if(Objects, [](const Json &Object) {
			return Object.at("Name") == "Panel";
		});
		Require(Workspace != Objects.end() && Panel != Objects.end(), "Find scoped model and parent identities");
		const auto Revision = Opened.at("Result").at("ProjectState").at("AuthoritativeRevision");
		std::vector<std::uint8_t> Pixels;
		std::vector<bool> FullCaptures;
		if (LatestFrameMailbox::IsSupported()) {
			Host.SetViewportCaptureForTesting([&](RenderPublicationPtr Publication) {
				FullCaptures.push_back(Publication->FullResync);
				const auto Width = Publication->Frame.ViewportWidth;
				const auto Height = Publication->Frame.ViewportHeight;
				Pixels.resize(static_cast<std::size_t>(Width) * Height * 4);
				return EditorViewportCaptureView{.Width = Width, .Height = Height, .BgraPixels = Pixels};
			});
			Require(Call("OpenViewportTransport", {{"Transport", "LatestFrameMailbox"}, {"Version", 2},
				{"PixelFormat", "BGRA8"}}).at("Ok") == true &&
				Call("ConfigureViewport", {{"Width", 200}, {"Height", 120}}).at("Ok") == true &&
				Call("StartViewportPresentation", {{"FrameRateMode", "Adaptive"}}).at("Ok") == true,
				"Start a viewport consumer before replacing the GAS document");
			Host.PumpViewportPresentationForTesting();
			Require(FullCaptures == std::vector{true}, "Publish the first document's asset residency");
		}
		const auto Model = Call("ExportGasModel", {{"Roots", Json::array({Panel->at("Id")})}});
		const auto Place = Call("ExportGasPlace", Json::object());
		Require(Model.at("Ok") == true && Place.at("Ok") == true, "Export both GAS profiles through EditorHost");
		const auto WrongKind = Call("ImportGasModel", {
			{"Parent", Workspace->at("Id")}, {"ExpectedRevision", Revision},
			{"DataBase64", Place.at("Result").at("DataBase64")},
		});
		Require(WrongKind.at("Ok") == false, "Reject place bytes at the model endpoint");
		const auto Imported = Call("ImportGasModel", {
			{"Parent", Workspace->at("Id")}, {"ExpectedRevision", Revision},
			{"DataBase64", Model.at("Result").at("DataBase64")},
		});
		Require(Imported.at("Ok") == true, "Insert a model from chunked Base64");
		const auto CurrentRevision = Imported.at("Result").at("ProjectState").at("AuthoritativeRevision");
		const auto Reopened = Call("OpenGasPlace", {{"ExpectedRevision", CurrentRevision},
			{"DataBase64", Place.at("Result").at("DataBase64")}});
		Require(Reopened.at("Ok") == true &&
			Reopened.at("Result").at("Root") != Opened.at("Result").at("Root"),
			"Open a staged place through EditorHost in a new scope");
		Require(Call("GetSnapshot", Json::object()).at("Ok") == true, "Refresh the snapshot after the place changes scope");
		if (LatestFrameMailbox::IsSupported()) {
			Host.PumpViewportPresentationForTesting();
			Require(FullCaptures == std::vector{true, true},
				"A replacement GASP republishes its assets without reusing the previous document's texture residency");
			Host.SetViewportCaptureForTesting({});
		}
	}

	void TestPlace() {
		const TestProjectDirectory Directory;
		DiskFilesystem Filesystem(Directory.Root);
		const auto ProjectValue = Project::fromInit(&Filesystem, "GAS place");
		auto Origin = Project::FromGasOrigin(&Filesystem);
		Require(Origin.Identity == ProjectValue.Identity, "Read durable project identity from the selected origin");
		const auto World = Origin.DeserializeGame();
		ScopedFixtureGraph SourceLifetime(World);
		World->InitializeLoadedProjectRevision();
		const auto Workspace = World->GetService("Workspace");
		const auto Source = CreateScene(World, Workspace);
		const auto Captured = Origin.CaptureGasPlace(World);
		Require(Captured.has_value() && Captured->Place && Captured->Place->Identity == Origin.Identity,
			"Capture persistent world using the existing project origin");
		const auto Bytes = Gas::Encode(*Captured);
		Require(Bytes.has_value(), "Encode selected place");
		const auto Place = Gas::Decode(*Bytes);
		Require(Place.has_value(), "Parse binary place");
		auto InvalidPlace = *Place;
		const auto SoundNode = std::ranges::find(InvalidPlace.Nodes, std::string("DistantSound"), &Gas::Node::Name);
		Require(SoundNode != InvalidPlace.Nodes.end(), "Find persisted Sound in the place fixture");
		SoundNode->SavedProperties.at("RollOffMaxDistance") = WireValue{100.0f};
		const auto Rejected = InstanceSerialization::MaterializeGasDocument(InvalidPlace, true);
		Require(!Rejected.Succeeded() && Rejected.Roots.empty(), "Reject an invalid Sound rolloff pair during place load");
		const auto Restored = Origin.LoadGasPlace(*Place);
		ScopedFixtureGraph RestoredLifetime(Restored);
		Restored->InitializeLoadedProjectRevision();
		const auto RestoredWorkspace = Restored->GetService("Workspace");
		const auto RestoredScene = SceneUnder(*RestoredWorkspace);
		Require(Restored != World && RestoredWorkspace != Workspace && Restored->GetName() == World->GetName() &&
			RestoredScene.Logic->GetSourceId() == Source.Logic->GetSourceId() &&
			RestoredScene.Logic->GetObjectId() != Source.Logic->GetObjectId(),
			"Reconstitute canonical services and durable Script identity in a new DataModel scope");
		VerifyScene(RestoredScene, Restored);

		const auto &TrustedPreRun = Place->Place->PreRunSource;
		bool RefusedExisting = false;
		try { Origin.RestoreGasPlace(Restored, TrustedPreRun); }
		catch (const std::invalid_argument &) { RefusedExisting = true; }
		Require(RefusedExisting && std::filesystem::remove(Origin.InstanceFilePath),
			"Explicit restore preserves an existing Instance file");
		Origin.RestoreGasPlace(Restored, TrustedPreRun);
		const auto JsonWorld = Origin.DeserializeGame();
		ScopedFixtureGraph JsonLifetime(JsonWorld);
		const auto JsonScene = SceneUnder(*JsonWorld->GetService("Workspace"));
		Require(JsonScene.Logic->GetSourceId() == Source.Logic->GetSourceId(), "Keep durable identity through the JSON bridge");
		VerifyScene(JsonScene, JsonWorld);
		SourceLifetime.Destroy();
		RestoredLifetime.Destroy();
		JsonLifetime.Destroy();
		TestEditorTransfer(Directory.Root);
	}

	void TestAnimatedModel(const std::filesystem::path &Origin, std::string_view Name) {
		BootstrapProjectRuntimeSchema(Origin);
		DiskFilesystem Filesystem(Origin);
		auto ProjectValue = Project::fromExisting(&Filesystem);
		const auto SourceWorld = ProjectValue.DeserializeGame();
		const ScopedFixtureGraph SourceLifetime(SourceWorld);
		const auto Source = Find<MeshPart>(*SourceWorld, Name);
		const auto SourceAssets = std::dynamic_pointer_cast<AssetService>(SourceWorld->GetService("AssetService"));
		Require(SourceAssets != nullptr, "Load fixture asset catalog");
		const auto Mesh = SourceAssets->ResolveMeshResource(Source->GetMesh());
		Require(Mesh && Mesh->Value.Skeleton, "Resolve fixture rig");
		std::optional<AssetRecord> Animation;
		for (const auto &Asset : SourceAssets->GetCatalog(false)) {
			if (Asset.Kind != AssetKind::Animation) continue;
			const auto Clip = SourceAssets->ResolveAnimation(Asset.Reference.Value);
			if (Clip && Clip->Value.SkeletonCompatibilityId == Mesh->Value.Skeleton->CompatibilityId) {
				Animation = Asset;
				break;
			}
		}
		Require(Animation.has_value(), "Resolve a fixture animation for the rig");

		const std::array<std::shared_ptr<Instance>, 1> Roots{Source};
		auto Model = InstanceSerialization::CaptureGasDocument(Roots, Gas::DocumentKind::Model);
		Require(Model.has_value(), "Capture the fixture mesh and Animator");
		Model->Dependencies.push_back({Animation->Id, Animation->ContentId});
		auto Pins = Gas::ResolveDependencies(*Model, SourceAssets.get(), false);
		Require(Pins && Pins->size() >= 2, "Resolve reflected mesh and explicitly selected animation pins");
		Model->Dependencies = std::move(*Pins);
		const auto Bytes = Gas::Encode(*Model);
		Require(Bytes.has_value(), "Encode an asset-bound fixture model");
		const auto Parsed = Gas::Decode(*Bytes);
		Require(Parsed.has_value(), "Decode an asset-bound fixture model");

		const auto Destination = std::make_shared<DataModel>();
		const ScopedFixtureGraph DestinationLifetime(Destination);
		Destination->InitializeLoadedProjectRevision();
		const auto Assets = std::dynamic_pointer_cast<AssetService>(Destination->GetService("AssetService"));
		Require(Assets != nullptr, "Find the new world's existing asset service");
		const auto Parent = Destination->GetService("Workspace");
		const auto Authority = MutationAuthorityContext::Studio(
			ScriptSecurityContext::StudioCoreUi(), Destination->GetObjectId());
		const auto Document = std::make_shared<const Gas::Document>(*Parsed);
		MutationGateway Gateway;
		const auto Before = Published(*Destination, *Parent);
		Require(!Gateway.Apply(ImportGasModelCommand{Document, Parent->GetObjectId(), Before.Revision},
			Authority).Succeeded() && Published(*Destination, *Parent) == Before, "Reject missing model assets atomically");
		Assets->LoadProjectAssetSnapshot(SourceAssets->CaptureProjectAssets());
		const auto Imported = Gateway.Apply(ImportGasModelCommand{Document, Parent->GetObjectId(),
			Destination->GetAuthoritativeRevision()}, Authority);
		Require(Imported.Succeeded() && Imported.Object.has_value(), "Insert fixture into a fresh project with its catalog");
		const auto Rebuilt = std::dynamic_pointer_cast<MeshPart>(ObjectRegistry::Get().Lookup(*Imported.Object));
		Require(Rebuilt && Rebuilt != Source && Rebuilt->GetObjectId() != Source->GetObjectId() &&
			Rebuilt->GetMesh() == Source->GetMesh(), "Preserve model mesh and allocate new identity");
		const auto AnimatorValue = std::dynamic_pointer_cast<Animator>(Rebuilt->FindFirstChildOfClass("Animator", false));
		Require(AnimatorValue && AnimatorValue->CreateTrack(Animation->Reference.Value),
			"Imported Animator can use the pinned canonical rig and animation");
		std::cout << "[GAS:Authoring] Animated model SHA-256 " << AssetContentId::Hash(*Bytes).ToString() << '\n';
	}
}

int main(int ArgumentCount, char **Arguments) {
	if (!SDL_Init(0)) {
		std::cerr << "[GAS:Authoring] SDL initialization failed\n";
		return 1;
	}
	struct SdlLifetime {
		~SdlLifetime() { SDL_Quit(); }
	} Sdl;
	try {
		if (ArgumentCount == 4 && std::string_view(Arguments[1]) == "--model-fixture") {
			TestAnimatedModel(std::filesystem::absolute(Arguments[2]), Arguments[3]);
			return 0;
		}
		BootstrapNativeRuntimeSchema();
		const bool NetworkTerrainFixture = ArgumentCount == 3 && std::string_view(Arguments[1]) == "--terrain-network-fixture";
		if (ArgumentCount == 3 && std::string_view(Arguments[1]) == "--terrain-material-fixture") {
			const auto Root = std::filesystem::absolute(Arguments[2]);
			Require(std::filesystem::is_directory(Root) && std::filesystem::is_empty(Root),
				"Terrain material fixture requires an empty project directory");
			MakeTerrainPaletteProject(Root);
			return 0;
		}
		if (NetworkTerrainFixture || (ArgumentCount == 3 && std::string_view(Arguments[1]) == "--terrain-package-fixture")) {
			const auto Root = std::filesystem::absolute(Arguments[2]);
			Require(std::filesystem::is_directory(Root) && std::filesystem::is_empty(Root),
				"Terrain package fixture requires an empty project directory");
			MakeTerrainProject(Root, true, NetworkTerrainFixture);
			return 0;
		}
		TestLegacySoundRollOff();
		TestModel();
		TestPlace();
		TestTerrainPlace();
		TestTerrainProjectPalette();
		TestTerrainProjectGrid();
		TestEditorTerrainBrush();
		TestRuntimeGeneratedTerrainSave();
		TestTerrainSceneCapacity();
		std::cout << "[GAS:Authoring] Model and place integration tests passed\n";
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[GAS:Authoring] " << Error.what() << '\n';
		return 1;
	}
}
