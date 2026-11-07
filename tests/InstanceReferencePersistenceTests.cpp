#include "gargantuan/assets/InstanceSerialization.hpp"
#include "assets/PreparedInstanceSerialization.hpp"
#include "gargantuan/classes/Attachment.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/JointConstraint.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/WeldConstraint.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"

#include <chrono>
#include <cstdint>
#include <future>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
	using namespace gargantuan;
	using Json = nlohmann::ordered_json;

	int Failures = 0;
	void Check(bool Condition, std::string_view Message) {
		if (Condition) return;
		std::cerr << "FAIL: " << Message << '\n';
		++Failures;
	}

	Json &ChildNamed(Json &Parent, std::string_view Name) {
		for (auto &Child : Parent["Children"])
			if (Child["Name"].get<std::string>() == Name) return Child;
		throw std::runtime_error("Missing fixture child");
	}

	InstanceSerialization::DeserializationState Decode(const Json &Document, bool Clone = false) {
		std::istringstream Input(Document.dump());
		return Clone ? InstanceSerialization::DeserializeClone(InstanceSerialization::InstanceFormat::Json, Input) :
			InstanceSerialization::Deserialize(InstanceSerialization::InstanceFormat::Json, Input);
	}

	void TestDocumentLocalReferences() {
		auto Root = std::make_shared<Folder>();
		Root->SetName("Assembly");
		Root->SetArchivable(true);
		auto Weld = std::make_shared<WeldConstraint>();
		Weld->SetName("ForwardWeld");
		Weld->SetArchivable(true);
		Weld->SetParent(Root);
		auto Joint = std::make_shared<JointConstraint>();
		Joint->SetName("ForwardJoint");
		Joint->SetArchivable(true);
		Joint->SetJointType("Hinge");
		Joint->SetParent(Root);
		auto PartA = std::make_shared<Part>();
		PartA->SetName("PartA");
		PartA->SetArchivable(true);
		PartA->SetParent(Root);
		auto PartB = std::make_shared<Part>();
		PartB->SetName("PartB");
		PartB->SetArchivable(true);
		PartB->SetParent(Root);
		auto AttachmentA = std::make_shared<Attachment>();
		AttachmentA->SetName("AttachmentA");
		AttachmentA->SetArchivable(true);
		AttachmentA->SetParent(PartA);
		auto AttachmentB = std::make_shared<Attachment>();
		AttachmentB->SetName("AttachmentB");
		AttachmentB->SetArchivable(true);
		AttachmentB->SetParent(PartB);
		Weld->SetPart0(PartA);
		Weld->SetPart1(PartB);
		Joint->SetAttachment0(AttachmentA);
		Joint->SetAttachment1(AttachmentB);

		std::shared_ptr<Instance> Serializable = Root;
		const auto Encoded = InstanceSerialization::Serialize(InstanceSerialization::InstanceFormat::Json, Serializable);
		auto Document = Json::parse(Encoded);
		Check(Document["Version"] == 5 && Document["NodeId"] == 1 &&
			ChildNamed(Document, "ForwardWeld")["NodeId"] < ChildNamed(Document, "PartA")["NodeId"] &&
			ChildNamed(Document, "ForwardWeld")["Properties"]["Part0"]["InstanceReference"] ==
				ChildNamed(Document, "PartA")["NodeId"],
			"v5 writes deterministic preorder IDs and forward reference values");
		auto Loaded = Decode(Document);
		auto LoadedWeld = Loaded.Instance ? std::dynamic_pointer_cast<WeldConstraint>(
			Loaded.Instance->FindFirstDescendant("ForwardWeld")) : nullptr;
		auto LoadedJoint = Loaded.Instance ? std::dynamic_pointer_cast<JointConstraint>(
			Loaded.Instance->FindFirstDescendant("ForwardJoint")) : nullptr;
		auto LoadedA = Loaded.Instance ? std::dynamic_pointer_cast<Part>(
			Loaded.Instance->FindFirstDescendant("PartA")) : nullptr;
		auto LoadedB = Loaded.Instance ? std::dynamic_pointer_cast<Part>(
			Loaded.Instance->FindFirstDescendant("PartB")) : nullptr;
		auto LoadedAttachmentA = Loaded.Instance ? std::dynamic_pointer_cast<Attachment>(
			Loaded.Instance->FindFirstDescendant("AttachmentA")) : nullptr;
		Check(Loaded.Ok && LoadedWeld && LoadedJoint && LoadedA && LoadedB && LoadedAttachmentA &&
			LoadedWeld->GetPart0() == LoadedA && LoadedWeld->GetPart1() == LoadedB &&
			LoadedJoint->GetAttachment0() && *LoadedJoint->GetAttachment0() == LoadedAttachmentA &&
			LoadedA != PartA && LoadedA->GetObjectId() != PartA->GetObjectId(),
			"reopen remaps forward part and attachment references to new runtime identities");

		auto Cloned = std::dynamic_pointer_cast<Folder>(Root->Clone());
		auto CloneWeld = Cloned ? std::dynamic_pointer_cast<WeldConstraint>(
			Cloned->FindFirstDescendant("ForwardWeld")) : nullptr;
		auto CloneA = Cloned ? std::dynamic_pointer_cast<Part>(Cloned->FindFirstDescendant("PartA")) : nullptr;
		Check(Cloned && CloneWeld && CloneA && CloneWeld->GetPart0() == CloneA && CloneA != PartA,
			"clone remaps internal references within its new document");

		auto NullWeld = std::make_shared<WeldConstraint>();
		NullWeld->SetName("NullWeld");
		NullWeld->SetParent(Root);
		auto External = std::make_shared<Part>();
		auto ExternalWeld = std::make_shared<WeldConstraint>();
		ExternalWeld->SetName("ExternalWeld");
		ExternalWeld->SetPart0(External);
		ExternalWeld->SetParent(Root);
		auto Hidden = std::make_shared<Folder>();
		Hidden->SetName("Unarchivable");
		Hidden->SetArchivable(false);
		Hidden->SetParent(Root);
		auto Omitted = std::make_shared<Part>();
		Omitted->SetName("Omitted");
		Omitted->SetParent(Hidden);
		ExternalWeld->SetPart1(Omitted);
		Serializable = Root;
		auto Partial = Json::parse(InstanceSerialization::Serialize(
			InstanceSerialization::InstanceFormat::Json, Serializable));
		Check(ChildNamed(Partial, "NullWeld")["Properties"]["Part0"]["InstanceReference"].is_null() &&
			ChildNamed(Partial, "ExternalWeld")["Properties"]["Part0"]["InstanceReference"].is_null() &&
			ChildNamed(Partial, "ExternalWeld")["Properties"]["Part1"]["InstanceReference"].is_null() &&
			ChildNamed(Partial, "Unarchivable")["Children"].empty(),
			"nullable external, null, and omitted descendant references encode as explicit null");
		auto PartialLoaded = Decode(Partial);
		auto LoadedExternal = PartialLoaded.Instance ? std::dynamic_pointer_cast<WeldConstraint>(
			PartialLoaded.Instance->FindFirstDescendant("ExternalWeld")) : nullptr;
		Check(PartialLoaded.Ok && LoadedExternal && !LoadedExternal->GetPart0() && !LoadedExternal->GetPart1(),
			"subtree reopen never resolves references outside its serialized document");

		auto Bad = Document;
		ChildNamed(Bad, "PartA")["NodeId"] = Bad["NodeId"];
		Check(!Decode(Bad).Ok, "duplicate document-local node ID rejects");
		Bad = Document;
		ChildNamed(Bad, "PartA").erase("NodeId");
		Check(!Decode(Bad).Ok, "missing document-local node ID rejects");
		Bad = Document;
		ChildNamed(Bad, "PartA")["NodeId"] = MaximumPersistenceObjects + 1;
		Check(!Decode(Bad).Ok, "out-of-range document-local node ID rejects");
		Bad = Document;
		ChildNamed(Bad, "ForwardWeld")["Properties"]["Part0"]["InstanceReference"] = MaximumPersistenceObjects;
		Check(!Decode(Bad).Ok, "reference target absent from document rejects");
		Bad = Document;
		ChildNamed(Bad, "ForwardWeld")["Properties"]["Part0"]["InstanceReference"] = Bad["NodeId"];
		Check(!Decode(Bad).Ok, "wrong-class reference target rejects through reflected validation");
		Bad = Document;
		ChildNamed(Bad, "ForwardWeld")["Properties"]["Part0"]["InstanceReference"] = -1;
		Check(!Decode(Bad).Ok, "negative reference target rejects");
		Bad = Document;
		ChildNamed(Bad, "ForwardWeld")["Properties"].erase("Part0");
		Check(!Decode(Bad).Ok, "v5 missing explicit reference property rejects");
		Bad = Document;
		auto &InvalidChild = ChildNamed(Bad, "PartA");
		const auto *DataModelClass = InstanceClassRegistry::GetDefinitionByName("DataModel");
		if (!DataModelClass) throw std::runtime_error("DataModel reflection definition is absent");
		InvalidChild["ClassName"] = "DataModel";
		InvalidChild["ClassSchemaId"] = DataModelClass->Id.ToString();
		InvalidChild["ClassDefinitionVersion"] = DataModelClass->DefinitionVersion;
		InvalidChild["Children"] = Json::array();
		auto InvalidParenting = Decode(Bad);
		Check(!InvalidParenting.Ok && !InvalidParenting.Instance &&
			InvalidParenting.PendingTags.empty() && InvalidParenting.DocumentNodes.empty() &&
			InvalidParenting.PendingReferences.empty(),
			"failed child parenting rolls back every pending materialized node and tag owner");
		Bad = Document;
		Bad["Version"] = 6;
		Check(!Decode(Bad).Ok, "unsupported future persistence version rejects");
	}

	void TestPreparedSchemaGeneration() {
		auto Root = std::make_shared<Folder>();
		Root->SetName("PreparedGeneration");
		Root->SetArchivable(true);
		std::shared_ptr<Instance> Serializable = Root;
		const auto Encoded = InstanceSerialization::Serialize(
			InstanceSerialization::InstanceFormat::Json, Serializable);
		const std::vector<std::uint8_t> Bytes(Encoded.begin(), Encoded.end());
		Root.reset();
		Serializable.reset();

		auto CapturedSchema = GetRuntimeSchemaLifecycle().GetActiveRegistry();
		std::promise<void> WorkerReady;
		auto Ready = WorkerReady.get_future();
		std::promise<void> ReleaseWorker;
		auto Released = ReleaseWorker.get_future().share();
		auto Worker = std::async(std::launch::async,
			[Bytes, Schema = CapturedSchema, &WorkerReady, Released] {
				ExecutionDomainScope WorkerDomain(ExecutionDomain::Worker);
				auto Prepared = InstanceSerialization::Internal::PrepareDetachedJson(Bytes, 1, Schema);
				WorkerReady.set_value();
				Released.wait();
				return Prepared;
			});
		if (Ready.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
			ReleaseWorker.set_value();
			(void)Worker.get();
			throw std::runtime_error("worker did not prepare the pinned schema document");
		}
		CapturedSchema.reset();
		try {
			BootstrapPackagedRuntimeSchema(std::nullopt);
		} catch (...) {
			ReleaseWorker.set_value();
			Worker.wait();
			throw;
		}
		ReleaseWorker.set_value();
		auto Prepared = Worker.get();
		if (!Prepared) throw std::runtime_error("worker rejected the pinned schema document: " + Prepared.error());
		auto Materialized = InstanceSerialization::Internal::MaterializeDetachedJson(*Prepared);
		Check(!Materialized.Ok && !Materialized.Instance && Materialized.ObjectsDecoded == 0 &&
			!Materialized.Errors.empty(),
			"prepared content from an old owned schema generation rejects before Main materialization");
	}
}

int main() {
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
		TestDocumentLocalReferences();
		TestPreparedSchemaGeneration();
	} catch (const std::exception &Error) {
		std::cerr << "[Persistence:Reference] " << Error.what() << '\n';
		return 1;
	}
	if (Failures == 0) std::cout << "Instance reference persistence tests passed\n";
	return Failures == 0 ? 0 : 1;
}
