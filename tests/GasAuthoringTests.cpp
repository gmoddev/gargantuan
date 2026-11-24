// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.

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
#include "gargantuan/classes/TextLabel.hpp"
#include "gargantuan/classes/WeldConstraint.hpp"
#include "gargantuan/editor/EditorHost.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/Project.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"
#include "gargantuan/runtime/MutationGateway.hpp"
#include "gargantuan/services/AssetService.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
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
			std::filesystem::remove(Root / ".gargantuan/project.instance.json", Ignored);
			std::filesystem::remove(Root / ".gargantuan/project.json", Ignored);
			std::filesystem::remove(Root / ".gargantuan", Ignored);
			std::filesystem::remove(Root, Ignored);
		}
	};

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
		TestLegacySoundRollOff();
		TestModel();
		TestPlace();
		std::cout << "[GAS:Authoring] Model and place integration tests passed\n";
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[GAS:Authoring] " << Error.what() << '\n';
		return 1;
	}
}
