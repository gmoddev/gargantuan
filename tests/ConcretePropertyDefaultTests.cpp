#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Frame.hpp"
#include "gargantuan/classes/ImageLabel.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/ScrollingFrame.hpp"
#include "gargantuan/classes/TextBox.hpp"
#include "gargantuan/classes/TextButton.hpp"
#include "gargantuan/classes/UIListLayout.hpp"
#include "gargantuan/services/Workspace.hpp"
#include "gargantuan/editor/EditorHost.hpp"
#include "gargantuan/runtime/WireCodec.hpp"
#include "gargantuan/runtime/InProcessReplicationSession.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"
#include "../src/runtime/PreparedPropertyCommit.hpp"
#include "../src/serialization/JsonCodec.hpp"
#include <iostream>
#include <fstream>
#include <chrono>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace {
	using namespace gargantuan;
	using Json = nlohmann::ordered_json;
	void Require(bool Value, std::string_view Message) {
		if (!Value) throw std::runtime_error(std::string(Message));
	}

	// No authored overrides: proves construction and resolution inherit an
	// override across an additional native class boundary without a copied table.
	class InheritedLabel final : public TextLabel {};

	const Json &Definition(const Json &Schema, SchemaId Id) {
		for (const auto &Entry : Schema.at("Definitions"))
			if (Entry.at("SchemaId") == Id.ToString()) return Entry;
		throw std::runtime_error("missing discovered definition");
	}
	const Json &DeclaredProperty(const Json &Schema, const InstanceProperty &Property) {
		const auto &Owner = Definition(Schema, Property.DeclaringSchemaId);
		Require(Owner.at("DefinitionVersion") == Property.DeclaringDefinitionVersion, "declaring version");
		for (const auto &Entry : Owner.at("Properties"))
			if (Entry.at("Name") == Property.Name) return Entry;
		throw std::runtime_error("missing declared discovery property");
	}
	WireValue DiscoveredDefault(const Json &Schema, const SchemaClassDefinition &Class, const InstanceProperty &Property) {
		const auto *Current = &Definition(Schema, Class.Id);
		Require(Current->at("DefinitionVersion") == Class.DefinitionVersion, "concrete discovery version");
		for (;;) {
			if (Current->contains("DefaultOverrides")) {
				for (const auto &Override : Current->at("DefaultOverrides")) {
					if (Override.at("Property") != Property.Name) continue;
					Require(Override.at("DeclaringClassSchemaId") == Property.DeclaringSchemaId.ToString() &&
						Override.at("DeclaringDefinitionVersion") == Property.DeclaringDefinitionVersion,
						"override retains declaring identity/version");
					Require(Override.at("CanonicalName") == Definition(Schema, Property.DeclaringSchemaId).at("CanonicalName").get<std::string>() + "." + Property.Name,
						"canonical override property identity");
					const auto Decoded = JsonCodec::DecodeWireValue(Override.at("Default"));
					Require(Decoded.has_value(), "override normal wire decode");
					return *Decoded;
				}
			}
			if (Current->at("SchemaId") == Property.DeclaringSchemaId.ToString()) break;
			const auto Base = SchemaId::Parse(Current->at("BaseSchemaId").get<std::string>());
			Require(Base.has_value(), "discovery base identity");
			Current = &Definition(Schema, *Base);
		}
		const auto Decoded = JsonCodec::DecodeWireValue(DeclaredProperty(Schema, Property).at("Default"));
		Require(Decoded.has_value(), "declared normal wire decode");
		return *Decoded;
	}

	void TestConstructionAndDiscovery() {
		EditorHost Host("concrete-defaults");
		const auto Response = Json::parse(Host.HandleRequest(Json{{"Version", 1}, {"RequestId", "schema"},
			{"SessionToken", "concrete-defaults"}, {"Method", "GetSchema"}, {"Params", {{"SchemaDiscoveryVersion", 7}}}}.dump()));
		Require(Response.at("Ok").get<bool>(), "production GetSchema");
		const auto &Schema = Response.at("Result");
		Require(Schema.at("SchemaDiscoveryVersion") == 7 && Schema.contains("Classes"), "v7 with legacy Classes view");
		const auto &Registry = GetActiveRuntimeSchemaRegistry();
		const auto World = std::make_shared<DataModel>();
		const auto Revision = World->GetAuthoritativeRevision();
		const auto History = World->Transactions.GetCommitted();
		const auto Journal = ChangeJournal::Get().CreateCursor();
		std::vector<std::shared_ptr<Instance>> Direct{
			std::make_shared<TextLabel>(), std::make_shared<ImageLabel>(), std::make_shared<TextBox>(),
			std::make_shared<TextButton>(), std::make_shared<ScrollingFrame>(), std::make_shared<Frame>(),
			std::make_shared<Part>(), std::make_shared<UIListLayout>(), std::make_shared<InheritedLabel>()};
		Require(ChangeJournal::Get().Read(Journal).Records.empty() && World->GetAuthoritativeRevision() == Revision &&
			World->Transactions.GetCommitted() == History, "direct initialization publishes no journal, revision, or history");
		std::size_t Checked = 0;
		for (const auto &Object : Direct) {
			const auto *Class = InstanceClassRegistry::GetDefinition(Object.get());
			Require(Class != nullptr, "direct object's concrete registry class");
			const auto BeforeConstruction = ChangeJournal::Get().CreateCursor();
			const auto Constructed = InstanceClassRegistry::Construct(*Class);
			Require(Constructed != nullptr, "registry construction");
			Require(ChangeJournal::Get().Read(BeforeConstruction).Records.empty() && World->GetAuthoritativeRevision() == Revision &&
				World->Transactions.GetCommitted() == History, "registry initialization publishes no journal, revision, or history");
			for (const auto &[Name, Property] : Class->AllProperties) {
				if (Property->PersistencePolicy != InstanceProperty::Persistence::Saved ||
					!PreparedPropertyCommit::SupportsProperty(*Property)) continue;
				const auto *Effective = Registry.ResolveEffectivePropertyDefault(Class->Id, Class->DefinitionVersion,
					Property->DeclaringSchemaId, Property->DeclaringDefinitionVersion, Name);
				const auto Label = Class->ClassName + "." + Name;
				Require(Effective != nullptr, "effective native default exists: " + Label);
				const auto Wire = EncodePropertyDefault(*Property, *Effective);
				Require(Wire.has_value(), "effective default encodes: " + Label);
				Require(Object->ReadPropertyWireValue(Name) == Wire, "direct construction: " + Label);
				Require(Constructed->ReadPropertyWireValue(Name) == Wire, "registry construction: " + Label);
				Require(DiscoveredDefault(Schema, *Class, *Property) == *Wire, "production discovery: " + Label);
				const auto &Declared = DeclaredProperty(Schema, *Property);
				Require(Declared.at("AtomicBatchWritable").get<bool>(), "prepared eligibility preserved");
				Require(JsonCodec::DecodeWireValue(Declared.at("Default")) == EncodePropertyDefault(*Property, Property->Unmodified),
					"declared fallback remains unchanged");
				Require(Class->AllProperties.at(Name) == Registry.FindClassById(Property->DeclaringSchemaId)->AllProperties.at(Name),
					"one declaring property node, no shadow property");
				Require(!Registry.ResolveEffectivePropertyDefault(Class->Id, Class->DefinitionVersion + 1,
					Property->DeclaringSchemaId, Property->DeclaringDefinitionVersion, Name), "stale concrete version denied");
				Require(!Registry.ResolveEffectivePropertyDefault(Class->Id, Class->DefinitionVersion,
					Property->DeclaringSchemaId, Property->DeclaringDefinitionVersion + 1, Name), "stale declaring version denied");
				++Checked;
			}
			Require(Object->PropertyChangedSignals.empty() && Constructed->PropertyChangedSignals.empty(),
				"construction creates no property notification signals");
			Constructed->Destroy();
		}
		Require(std::make_shared<TextLabel>()->GetBackgroundTransparency() == 1.0f &&
			std::make_shared<TextBox>()->GetBackgroundTransparency() == 0.0f &&
			std::make_shared<TextButton>()->GetBackgroundTransparency() == 0.0f &&
			std::make_shared<InheritedLabel>()->GetBackgroundTransparency() == 1.0f,
			"nearest override wins and inherited override survives");
		Require(std::make_shared<TextBox>()->GetGuiState() == Enums::GuiState::Idle &&
			std::make_shared<TextButton>()->GetGuiState() == Enums::GuiState::Idle &&
			std::make_shared<ScrollingFrame>()->GetGuiState() == Enums::GuiState::Idle,
			"transient handwritten interaction initialization survives");
		Require(std::make_shared<Workspace>()->GetCurrentCamera() != nullptr, "contextual camera construction survives");
		std::size_t Count = 0, NativeValueBytes = 0, WireBytes = 0, NameBytes = 0;
		auto LegacySize = Schema;
		for (auto &Entry : LegacySize["Definitions"]) Entry.erase("DefaultOverrides");
		for (const auto *Class : Registry.EnumerateClasses()) {
			for (const auto &Override : Class->DefaultOverrides) {
				++Count;
				const auto *Property = Class->AllProperties.at(Override.Property);
				Require(Property->PersistencePolicy == InstanceProperty::Persistence::Saved &&
					PreparedPropertyCommit::SupportsProperty(*Property), "all migrated overrides remain prepared-safe persistent properties");
				WireBytes += EncodeWireValueJson(*EncodePropertyDefault(*Property, Override.Value))->size();
				NameBytes += Override.Property.size();
				NativeValueBytes += Override.Value.type() == typeid(Color3) ? sizeof(Color3) :
					Override.Value.type() == typeid(bool) ? sizeof(bool) : sizeof(float);
			}
		}
		Require(Count == 16, "exact migrated override count");
		// Older consumers can still project Classes; its defaults retain v6 meaning.
		for (const auto &Class : Schema.at("Classes")) {
			if (Class.at("Name") != "TextLabel") continue;
			for (const auto &Property : Class.at("Properties"))
				if (Property.at("Name") == "BackgroundTransparency")
					Require(JsonCodec::DecodeWireValue(Property.at("Default")) == std::optional<WireValue>(WireFloat{0}),
						"legacy declared-default adapter unchanged");
		}
		std::cout << "[Schema:ConcreteDefaults] Properties=" << Checked << " Overrides=" << Count
			<< " RecordBytes=" << sizeof(SchemaPropertyDefaultOverride) << " NativeValueBytes=" << NativeValueBytes
			<< " PropertyNameBytes=" << NameBytes << " ValueJsonBytes=" << WireBytes
			<< " DiscoveryIncreaseBytes=" << Schema.dump().size() - LegacySize.dump().size()
			<< " ClassVectorBytes=" << sizeof(std::vector<SchemaPropertyDefaultOverride>) * Registry.EnumerateClasses().size() << '\n';
		for (const auto &Object : Direct) Object->Destroy();
		World->Destroy();
	}

	void TestPersistence() {
		for (const auto &ClassName : {"TextLabel", "ImageLabel", "TextBox", "TextButton", "ScrollingFrame"}) {
			for (const auto Mode : {0, 1, 2}) {
				auto Object = InstanceClassRegistry::ConstructByName(ClassName);
				Object->SetArchivable(true);
				const auto *Class = InstanceClassRegistry::GetDefinition(Object.get());
				for (const auto &Override : Class->DefaultOverrides) {
					const auto *Property = Object->FindProperty(Override.Property);
					if (Mode == 1) Property->Write(Object.get(), Property->Unmodified);
					if (Mode == 2) {
						if (Override.Value.type() == typeid(float)) Property->Write(Object.get(), 0.4f);
						else if (Override.Value.type() == typeid(Color3)) Property->Write(Object.get(), Color3(0.3f, 0.4f, 0.5f));
						else if (Override.Value.type() == typeid(bool)) Property->Write(Object.get(), !std::any_cast<bool>(Override.Value));
						else if (Override.Value.type() == typeid(Enums::InputSink)) Property->Write(Object.get(), Enums::InputSink::None);
						else Property->Write(Object.get(), Enums::TextXAlignment::Right);
					}
				}
				const auto Saved = InstanceSerialization::Serialize(InstanceSerialization::InstanceFormat::Json, Object);
				const auto Document = Json::parse(Saved);
				std::istringstream Input(Saved);
				auto Loaded = InstanceSerialization::Deserialize(InstanceSerialization::InstanceFormat::Json, Input);
				Require(Loaded.Ok && Loaded.Instance, "saved GUI values reopen");
				const auto Clone = Object->Clone();
				Require(Clone != nullptr, "GUI clone succeeds");
				for (const auto &[Name, Property] : Class->AllProperties) {
					if (Property->PersistencePolicy != InstanceProperty::Persistence::Saved || !Property->Read || !Property->Write) continue;
					Require(Document.at("Properties").contains(Name), "saved defaults are never elided");
					Require(Loaded.Instance->ReadPropertyWireValue(Name) == Object->ReadPropertyWireValue(Name), "saved property exact fidelity");
					Require(Clone->ReadPropertyWireValue(Name) == Object->ReadPropertyWireValue(Name), "clone exact fidelity");
				}
				Clone->Destroy(); Loaded.Instance->Destroy(); Object->Destroy();
			}
		}
	}

	void TestNativeReset() {
		auto World = std::make_shared<DataModel>();
		std::size_t Cases = 0;
		for (const auto &ClassName : {"TextLabel", "ImageLabel", "TextBox", "TextButton", "ScrollingFrame",
			"DefaultInheritedLabel", "Frame", "Part"}) {
			auto Object = InstanceClassRegistry::ConstructByName(ClassName);
			Object->SetParent(World);
			const auto *Class = InstanceClassRegistry::GetDefinition(Object.get());
			std::vector<std::string> Names;
			for (const auto &Override : Class->DefaultOverrides) Names.push_back(Override.Property);
			if (Names.empty()) Names.push_back(std::string(ClassName) == "Part" ? "Shape" : "BackgroundTransparency");
			if (std::string(ClassName) == "Frame") Names.push_back("Name");
			for (const auto &Name : Names) {
				const auto *Property = Object->FindProperty(Name);
				const auto *Default = GetActiveRuntimeSchemaRegistry().ResolveEffectivePropertyDefault(
					Class->Id, Class->DefinitionVersion, Property->DeclaringSchemaId, Property->DeclaringDefinitionVersion, Name);
				Require(Default != nullptr, "reset default resolves");
				const auto Expected = EncodePropertyDefault(*Property, *Default);
				WireValue Manual = WireFloat{0.5f};
				if (Name == "Name") Manual = std::string("ManualName");
				else if (Default->type() == typeid(bool)) Manual = !std::any_cast<bool>(*Default);
				else if (Default->type() == typeid(Color3)) Manual = WireColor3{0.3f, 0.4f, 0.5f};
				else if (Name == "InputSink") Manual = WireEnumItem{"InputSink", "None"};
				else if (Name == "TextXAlignment") Manual = WireEnumItem{"TextXAlignment", "Right"};
				else if (Name == "Shape") Manual = WireEnumItem{"PartType", "Ball"};
				Require(Object->ApplyPropertyWireMutation(Name, Manual, Enums::Permission::Engine) == MutationStatus::Success,
					"manual value applies before native reset");
				const auto Revision = World->GetAuthoritativeRevision();
				const auto Cursor = ChangeJournal::Get().CreateCursor(World->GetObjectId());
				std::size_t Notifications = 0;
				Object->GetPropertyChangedSignal(Name)->Connect([&](std::monostate) {
					++Notifications;
					Require(Object->ReadPropertyWireValue(Name) == Expected, "native observer sees committed reset value");
					Require(World->GetAuthoritativeRevision() == Revision + 1, "native observer sees new revision");
				});
				Object->ResetPropertyToDefault(Name);
				Require(Object->ReadPropertyWireValue(Name) == Expected && Object->FindProperty(Name) == Property,
					"native reset uses effective value and original declaring identity");
				const auto Published = ChangeJournal::Get().Read(Cursor);
				Require(Published.Records.size() == 1 && std::holds_alternative<PropertyUpdatedChange>(Published.Records[0].Payload),
					"native reset publishes one ordinary property record");
				Object->ResetPropertyToDefault(Name);
				Require(Notifications == 1 && World->GetAuthoritativeRevision() == Revision + 1 &&
					ChangeJournal::Get().Read(Cursor).Records.size() == 1, "already-default reset is a normal no-op, including enums");
				Require(World->Transactions.GetCommitted().empty(), "runtime reset preserves ordinary runtime history policy");
				Object->GetPropertyChangedSignal(Name)->DisconnectAll();
				++Cases;
			}
			Object->Destroy();
		}
		auto Label = std::make_shared<TextBox>(); Label->SetParent(World);
		const auto Reject = [&](const std::shared_ptr<Instance> &Object, const std::string &Name) {
			const auto Value = Object->ReadPropertyWireValue(Name);
			const auto Revision = World->GetAuthoritativeRevision();
			const auto Cursor = ChangeJournal::Get().CreateCursor(World->GetObjectId());
			bool Rejected = false;
			try { Object->ResetPropertyToDefault(Name); } catch (const std::exception &) { Rejected = true; }
			Require(Rejected && Object->ReadPropertyWireValue(Name) == Value && World->GetAuthoritativeRevision() == Revision &&
				ChangeJournal::Get().Read(Cursor).Records.empty(), "unsupported reset fails before publication");
		};
		Reject(Label, "GuiState"); Reject(Label, "Parent"); Reject(Label, "Destroyed"); Reject(Label, "Missing");
		auto WorkspaceObject = std::make_shared<Workspace>(); WorkspaceObject->SetParent(World);
		const auto Camera = WorkspaceObject->GetCurrentCamera();
		Reject(WorkspaceObject, "CurrentCamera");
		Require(WorkspaceObject->GetCurrentCamera() == Camera, "contextual camera is retained");
		Label->SetBackgroundTransparency(0.5f);
		{
			ExecutionDomainScope Worker(ExecutionDomain::Worker);
			Reject(Label, "BackgroundTransparency");
		}
		{
			ScriptSecurityScope Restricted({ScriptExecutionDomain::Studio, ScriptCapabilitySet{ScriptCapability::ReadDataModel}});
			Reject(Label, "BackgroundTransparency");
		}
		Label->Destroy(); Reject(Label, "BackgroundTransparency");
		World->Destroy();
		std::cout << "[Schema:ResetDefaults] NativeCases=" << Cases << '\n';
	}

	void TestEditorReset() {
		const auto Root = std::filesystem::temp_directory_path() /
			("gargantuan-reset-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		struct Cleanup {
			std::filesystem::path Root;
			~Cleanup() { std::error_code Error; std::filesystem::remove_all(Root, Error); }
		} CleanupValue{Root};
		std::filesystem::create_directories(Root / ".gargantuan");
		const auto Node = [](const std::string &Name, const std::string &Class, Json Children) {
			return Json{{"Name", Name}, {"ClassName", Class}, {"ClassSchemaId", SchemaId::FromNativeName("Engine", Class).ToString()},
				{"ClassDefinitionVersion", 1}, {"Properties", Json::object()}, {"Attributes", Json::object()},
				{"Extensions", Json::array()}, {"CustomProperties", Json::array()}, {"Tags", Json::array()}, {"Children", Children}};
		};
		Json Children = Json::array();
		for (std::size_t Index = 0; Index < 257; ++Index)
			Children.push_back(Node("Target" + std::to_string(Index), Index % 2 == 0 ? "TextLabel" : "TextBox", Json::array()));
		auto Document = Node("ResetWorld", "DataModel", Json::array({Node("Workspace", "Workspace", std::move(Children))}));
		Document["Version"] = 4;
		std::ofstream(Root / ".gargantuan" / "project.instance.json", std::ios::binary) << Document.dump();
		std::ofstream(Root / ".gargantuan" / "prerun.luau", std::ios::binary) <<
			"Schema:RegisterClass({ Namespace = 'Game', Name = 'ResetFolder', Version = 1, Base = 'Engine.Folder', "
			"Properties = { Health = { Type = 'Integer', Default = 100 } } })";
		EditorHost Host("reset-token");
		const auto Call = [&](std::string Method, Json Params) {
			return Json::parse(Host.HandleRequest(Json{{"Version", 1}, {"RequestId", "reset"}, {"SessionToken", "reset-token"},
				{"Method", Method}, {"Params", std::move(Params)}}.dump()));
		};
		Require(Call("OpenProject", {{"Root", Root.string()}}).at("Ok").get<bool>(), "reset project opens");
		const auto Snapshot = Call("GetSnapshot", Json::object());
		const auto Scope = Snapshot.at("Result").at("Snapshot").at("Cursor").at("Scope");
		const auto ScopeId = JsonCodec::DecodeObjectId(Scope);
		auto World = std::dynamic_pointer_cast<DataModel>(ObjectRegistry::Get().Lookup(ScopeId->ToObjectId()));
		Require(World != nullptr, "reset world resolves");
		auto Custom = InstanceClassRegistry::ConstructByName("Game.ResetFolder");
		Require(Custom != nullptr, "custom reset exclusion fixture constructs");
		Custom->SetParent(World);
		for (const auto &Name : {"Name", "Health"}) {
			const auto Before = Custom->ReadPropertyWireValue(Name);
			const auto Revision = World->GetAuthoritativeRevision();
			const auto Cursor = ChangeJournal::Get().CreateCursor(World->GetObjectId());
			bool Rejected = false;
			try { Custom->ResetPropertyToDefault(Name); } catch (const std::exception &) { Rejected = true; }
			Require(Rejected && Custom->ReadPropertyWireValue(Name) == Before && World->GetAuthoritativeRevision() == Revision &&
				ChangeJournal::Get().Read(Cursor).Records.empty(), "custom declared and inherited contextual defaults remain unsupported");
		}
		Custom->Destroy(); Custom.reset();
		const auto Legacy = Call("GetSchema", Json::object()).at("Result");
		Require(Legacy.at("SchemaDiscoveryVersion") == 6, "parameterless discovery supports exact-version older readers");
		for (const auto &Entry : Legacy.at("Definitions")) Require(!Entry.contains("DefaultOverrides"), "legacy discovery has no effective-default contract");
		Require(!Call("GetSchema", {{"SchemaDiscoveryVersion", 8}}).at("Ok").get<bool>(), "unknown discovery version fails closed");
		const auto Schema = Call("GetSchema", {{"SchemaDiscoveryVersion", 7}}).at("Result");
		Require(Schema.at("SchemaDiscoveryVersion") == 7, "explicit v7 enables authoritative effective defaults");
		std::vector<std::shared_ptr<Instance>> Targets;
		for (std::size_t Index = 0; Index < 257; ++Index) Targets.push_back(World->FindFirstChild("Target" + std::to_string(Index), true));
		const auto Write = [&](const std::shared_ptr<Instance> &Object, std::string Name = "BackgroundTransparency") {
			const auto *Class = InstanceClassRegistry::GetDefinition(Object.get());
			const auto *Property = Object->FindProperty(Name);
			const auto &Metadata = DeclaredProperty(Schema, *Property);
			Require(Definition(Schema, Class->Id).at("ConstructionKind") == "Native" &&
				Metadata.at("AtomicBatchWritable").get<bool>() && Metadata.at("Persistence") == "Saved",
				"complete selection preflight requires native persistent batch-writable metadata");
			return Json{{"Object", JsonCodec::EncodeObjectId(WireObjectId::FromObjectId(Object->GetObjectId()))},
				{"DeclaringClassSchemaId", Property->DeclaringSchemaId.ToString()}, {"DeclaringDefinitionVersion", Property->DeclaringDefinitionVersion},
				{"Property", Name}, {"Value", JsonCodec::EncodeWireValue(DiscoveredDefault(Schema, *Class, *Property))}};
		};
		const auto Params = [&](Json Writes) {
			return Json{{"PropertyBatchVersion", 1}, {"Scope", Scope}, {"ExpectedRevision", World->GetAuthoritativeRevision()}, {"Writes", Writes}};
		};
		const auto State = [&] {
			return std::tuple{SerializeSnapshot(CaptureSnapshot(World)), World->GetAuthoritativeRevision(),
				World->Transactions.GetCommitted(), World->Transactions.GetStatus().Cursor,
				ChangeJournal::Get().CreateCursor(World->GetObjectId()).NextSequence};
		};
		const auto Reject = [&](Json Parameters, std::string_view Code) {
			const auto Before = State();
			const auto Result = Call("SetPropertyBatch", std::move(Parameters));
			Require(!Result.at("Ok").get<bool>() && Result.at("Error").at("Code").get<std::string>() == Code && State() == Before,
				"whole reset rejects without state/history/publication: " + Result.dump());
		};
		// Single editor reset uses ordinary assignment and its existing history.
		Require(Targets[0]->ApplyPropertyWireMutation("BackgroundTransparency", WireFloat{0.5f}, Enums::Permission::Engine) == MutationStatus::Success, "single manual value");
		auto Single = Write(Targets[0]);
		const auto *SingleClass = InstanceClassRegistry::GetDefinition(Targets[0].get());
		Single["ClassSchemaId"] = SingleClass->Id.ToString(); Single["ClassDefinitionVersion"] = SingleClass->DefinitionVersion;
		Single["ExpectedRevision"] = World->GetAuthoritativeRevision();
		auto Stale = Single; Stale["ClassDefinitionVersion"] = 2;
		const auto BeforeStale = State();
		Require(!Call("SetProperty", Stale).at("Ok").get<bool>() && State() == BeforeStale, "stale concrete version rejected before single reset");
		const auto SingleHistory = World->Transactions.GetCommitted().size();
		Require(Call("SetProperty", Single).at("Ok").get<bool>() && World->Transactions.GetCommitted().size() == SingleHistory + 1,
			"single reset records one ordinary authoring action");
		Require(Call("Undo", Json::object()).at("Ok").get<bool>() && Targets[0]->ReadPropertyWireValue("BackgroundTransparency") == WireValue(WireFloat{0.5f}), "single Undo restores manual value");
		Require(Call("Redo", Json::object()).at("Ok").get<bool>() && Targets[0]->ReadPropertyWireValue("BackgroundTransparency") == WireValue(WireFloat{1.0f}), "single Redo reapplies captured effective value");
		Single["ExpectedRevision"] = World->GetAuthoritativeRevision();
		const auto BeforeSingleNoOp = State();
		Require(Call("SetProperty", Single).at("Ok").get<bool>() && State() == BeforeSingleNoOp, "single already-default creates no history");
		// Common, mixed, partially-default and wholly-default selections; each
		// target still contributes intent, and the coordinator normalizes no-ops.
		for (const auto Values : {std::pair{0.5f, 0.5f}, std::pair{0.25f, 0.75f}, std::pair{1.0f, 0.5f}, std::pair{1.0f, 0.0f}}) {
			Targets[0]->ApplyPropertyWireMutation("BackgroundTransparency", WireFloat{Values.first}, Enums::Permission::Engine);
			Targets[1]->ApplyPropertyWireMutation("BackgroundTransparency", WireFloat{Values.second}, Enums::Permission::Engine);
			const auto Changes = (Values.first != 1.0f ? 1u : 0u) + (Values.second != 0.0f ? 1u : 0u);
			const auto Revision = World->GetAuthoritativeRevision();
			const auto History = World->Transactions.GetCommitted().size();
			const auto Cursor = ChangeJournal::Get().CreateCursor(World->GetObjectId());
			const auto Result = Call("SetPropertyBatch", Params(Json::array({Write(Targets[0]), Write(Targets[1])})));
			Require(Result.at("Ok").get<bool>() && Result.at("Result").at("ChangedWriteCount") == Changes &&
				Targets[0]->ReadPropertyWireValue("BackgroundTransparency") == WireValue(WireFloat{1.0f}) &&
				Targets[1]->ReadPropertyWireValue("BackgroundTransparency") == WireValue(WireFloat{0.0f}), "per-target effective reset in one gesture");
			Require(World->GetAuthoritativeRevision() == Revision + (Changes ? 1 : 0) &&
				World->Transactions.GetCommitted().size() == History + (Changes ? 1 : 0) &&
				ChangeJournal::Get().Read(Cursor).Records.size() == Changes, "reset no-op/history/journal normalization");
		}
		Json Maximum = Json::array();
		for (std::size_t Index = 0; Index < 256; ++Index) {
			Targets[Index]->ApplyPropertyWireMutation("BackgroundTransparency", WireFloat{0.5f}, Enums::Permission::Engine);
			Maximum.push_back(Write(Targets[Index]));
		}
		auto TooMany = Maximum; TooMany.push_back(Write(Targets[256])); Reject(Params(TooMany), "ResourceLimit");
		auto Invalid = Params(Maximum); Invalid["Writes"][1]["DeclaringDefinitionVersion"] = 2; Reject(Invalid, "StaleSchema");
		Invalid = Params(Maximum); Invalid["ExpectedRevision"] = World->GetAuthoritativeRevision() + 1; Reject(Invalid, "Conflict");
		Invalid = Params(Maximum); Invalid["Writes"][1]["Property"] = "GuiState";
		Invalid["Writes"][1]["Value"] = JsonCodec::EncodeWireValue(WireEnumItem{"GuiState", "Idle"}); Reject(Invalid, "ReadOnly");
		auto Replica = InProcessReplicationSession::Start(World);
		Require(Replica.Succeeded(), "reset replication baseline");
		const auto Revision = World->GetAuthoritativeRevision();
		const auto History = World->Transactions.GetCommitted().size();
		std::size_t Notifications = 0;
		bool Restoring = false;
		bool ObservationsValid = true;
		Targets[0]->GetPropertyChangedSignal("BackgroundTransparency")->Connect([&](std::monostate) {
			++Notifications;
			for (std::size_t Index = 0; Index < 256; ++Index)
				ObservationsValid &= Targets[Index]->ReadPropertyWireValue("BackgroundTransparency") ==
					WireValue(WireFloat{Restoring ? 0.5f : (Index % 2 == 0 ? 1.0f : 0.0f)});
			ObservationsValid &= World->GetAuthoritativeRevision() == Revision + Notifications;
		});
		const auto Result = Call("SetPropertyBatch", Params(Maximum));
		Require(Result.at("Ok").get<bool>() && Result.at("Result").at("ChangedWriteCount") == 256 &&
			World->Transactions.GetCommitted().size() == History + 1, "maximum reset is one action");
		Require(Replica.Session->ApplyAvailable().AppliedRecords == 256, "reset uses normal replication records");
		for (std::size_t Index = 0; Index < 256; ++Index)
			Require(Replica.Session->ResolveReceiver(WireObjectId::FromObjectId(Targets[Index]->GetObjectId()))->ReadPropertyWireValue("BackgroundTransparency") ==
				Targets[Index]->ReadPropertyWireValue("BackgroundTransparency"), "replica has each effective default");
		Restoring = true; Require(Call("Undo", Json::object()).at("Ok").get<bool>(), "whole reset Undo");
		Restoring = false; Require(Call("Redo", Json::object()).at("Ok").get<bool>(), "whole reset Redo");
		Require(ObservationsValid && Notifications == 3 && World->Transactions.GetCommitted().size() == History + 1 &&
			Replica.Session->ApplyAvailable().AppliedRecords == 512, "replay preserves one history action and ordinary replication");
		Targets[0]->GetPropertyChangedSignal("BackgroundTransparency")->DisconnectAll();
		Replica.Session->GetReceiverRoot()->Destroy(); Replica.Session.reset();
		// Old clients can still use the unversioned Name compatibility operation.
		Require(Call("SetProperty", {{"Object", Maximum[0]["Object"]}, {"Property", "Name"},
			{"Value", JsonCodec::EncodeWireValue(std::string("LegacyEdited"))}}).at("Ok").get<bool>(), "legacy editing survives v7 opt-in");
		const auto Delayed = Params(Maximum);
		Require(Call("OpenProject", {{"Root", Root.string()}}).at("Ok").get<bool>(), "project replacement retires reset cache");
		Require(Call("GetSnapshot", Json::object()).at("Ok").get<bool>(), "replacement snapshot");
		const auto Rejected = Call("SetPropertyBatch", Delayed);
		Require(!Rejected.at("Ok").get<bool>() && Rejected.at("Error").at("Code") == "StaleProject", "prior-scope reset cannot use replacement schema");
		std::cout << "[Schema:ResetDefaults] EditorSingle=1 SelectionModes=4 MaximumWrites=256 RejectedWrites=257 passed\n";
	}

	void TestRejectedMetadata() {
		const auto Check = [](SchemaClassDefinition Definition, bool ConstrainValue = false) {
			RuntimeSchemaRegistry Candidate;
			Candidate.RegisterNative<Instance>(Instance::CLASS_DEFINITION);
			Candidate.RegisterNative<GuiBase>(GuiBase::CLASS_DEFINITION);
			Candidate.RegisterNative<GuiBase2d>(GuiBase2d::CLASS_DEFINITION);
			auto Gui = GuiObject::CLASS_DEFINITION;
			if (ConstrainValue) {
				Gui.Properties.at("BackgroundTransparency").SetNumericRange(0.0, 1.0);
				Gui.Properties.at("BackgroundTransparency").Validate = [](const std::any &Value) {
					const auto *Number = std::any_cast<float>(&Value);
					return Number && *Number != 0.5f;
				};
			}
			Candidate.RegisterNative<GuiObject>(std::move(Gui));
			bool Rejected = false;
			try { Candidate.RegisterNative<TextLabel>(std::move(Definition)); Candidate.Validate(); }
			catch (const std::invalid_argument &) { Rejected = true; }
			Require(Rejected && !Candidate.IsValidated(), "invalid override rejects unpublished candidate");
		};
		auto Invalid = TextLabel::CLASS_DEFINITION;
		Invalid.DefaultOverrides[0].Property = "Unknown"; Check(Invalid);
		Invalid = TextLabel::CLASS_DEFINITION; Invalid.DefaultOverrides[0].Value = 1.0; Check(Invalid);
		Invalid = TextLabel::CLASS_DEFINITION; Invalid.DefaultOverrides[0].Value = std::numeric_limits<float>::infinity(); Check(Invalid);
		Invalid = TextLabel::CLASS_DEFINITION; Invalid.DefaultOverrides[0].Value = 2.0f; Check(Invalid, true);
		Invalid = TextLabel::CLASS_DEFINITION; Invalid.DefaultOverrides[0].Value = 0.5f; Check(Invalid, true);
		Invalid = TextLabel::CLASS_DEFINITION; Invalid.DefaultOverrides.push_back(Invalid.DefaultOverrides[0]); Check(Invalid);
		Invalid = TextLabel::CLASS_DEFINITION; Invalid.DefaultOverrides[0].DeclaringDefinitionVersion = 2; Check(Invalid);
		Invalid = TextLabel::CLASS_DEFINITION; Invalid.DefaultOverrides[0].DeclaringClassSchemaId = TextLabel::CLASS_DEFINITION.Id; Check(Invalid);
		Invalid = TextLabel::CLASS_DEFINITION; Invalid.DefaultOverrides[0].Property = "Text"; Check(Invalid);
		Invalid = TextLabel::CLASS_DEFINITION; Invalid.DefaultOverrides[0].Property = "GuiState";
		Invalid.DefaultOverrides[0].Value = Enums::GuiState::Idle; Check(Invalid);
		Invalid = TextLabel::CLASS_DEFINITION; Invalid.DefaultOverrides[0].Property = "InputSink";
		Invalid.DefaultOverrides[0].Value = static_cast<Enums::InputSink>(9999); Check(Invalid);
		Invalid = TextLabel::CLASS_DEFINITION; Invalid.DefaultOverrides.resize(MaximumClassDefaultOverrides + 1, Invalid.DefaultOverrides[0]); Check(Invalid);
	}
}

int main() {
	try {
		SchemaClassDefinition Child;
		Child.Id = SchemaId::FromNativeName("Engine", "DefaultInheritedLabel");
		Child.ClassName = "DefaultInheritedLabel";
		Child.Superclass = "TextLabel";
		Child.BaseSchemaId = TextLabel::CLASS_DEFINITION.Id;
		Child.Constructor = []() -> std::shared_ptr<Instance> { return std::make_shared<InheritedLabel>(); };
		InstanceClassRegistry::Register<InheritedLabel>(std::move(Child));
		BootstrapNativeRuntimeSchema();
		TestRejectedMetadata();
		TestConstructionAndDiscovery();
		TestPersistence();
		TestNativeReset();
		TestEditorReset();
		std::cout << "[Schema:ConcreteDefaults] passed\n";
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Schema:ConcreteDefaults] " << Error.what() << '\n';
		return 1;
	}
}
