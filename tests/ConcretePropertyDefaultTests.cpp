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
#include "../src/runtime/PreparedPropertyCommit.hpp"
#include "../src/serialization/JsonCodec.hpp"
#include <iostream>
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
			{"SessionToken", "concrete-defaults"}, {"Method", "GetSchema"}, {"Params", Json::object()}}.dump()));
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
		std::cout << "[Schema:ConcreteDefaults] passed\n";
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Schema:ConcreteDefaults] " << Error.what() << '\n';
		return 1;
	}
}
