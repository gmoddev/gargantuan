#include "gargantuan/editor/EditorHost.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"

#include <chrono>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
	using Json = nlohmann::ordered_json;

	void Require(bool Condition, std::string Message) {
		if (!Condition) throw std::runtime_error(std::move(Message));
	}

	const Json &FindDefinition(const Json &Schema, std::string_view Name) {
		for (const auto &Definition : Schema.at("Result").at("Definitions"))
			if (Definition.at("CanonicalName").get<std::string>() == std::string(Name)) return Definition;
		throw std::runtime_error("[Physics:EditorHost] Missing schema definition " + std::string(Name));
	}

	const Json &FindProperty(const Json &Definition, std::string_view Name) {
		for (const auto &Property : Definition.at("Properties"))
			if (Property.at("Name").get<std::string>() == std::string(Name)) return Property;
		throw std::runtime_error("[Physics:EditorHost] Missing schema property " + std::string(Name));
	}

	Json FindObject(const Json &Snapshot, std::string_view Name) {
		for (const auto &Object : Snapshot.at("Result").at("Snapshot").at("Objects"))
			if (Object.at("Name").get<std::string>() == std::string(Name)) return Object;
		throw std::runtime_error("[Physics:EditorHost] Missing snapshot object " + std::string(Name));
	}

	bool SameFields(const Json &Left, const Json &Right, std::initializer_list<std::string_view> Names) {
		for (const auto Name : Names) {
			const auto Key = std::string(Name);
			if (!Left.contains(Key) || !Right.contains(Key) || Left.at(Key) != Right.at(Key)) return false;
		}
		return true;
	}

	void TestPhysicsPropertyAuthoring() {
		using namespace gargantuan;
		const auto TemporaryRoot = std::filesystem::temp_directory_path() /
			("gargantuan-physics-editorhost-" +
				std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		struct Cleanup {
			std::filesystem::path Root;
			~Cleanup() { std::error_code Error; std::filesystem::remove_all(Root, Error); }
		} CleanupValue{TemporaryRoot};
		std::filesystem::create_directories(TemporaryRoot);
		EditorHost Host("physics-editorhost-test");
		std::size_t Sequence = 0;
		auto Call = [&](std::string Method, Json Parameters) {
			return Json::parse(Host.HandleRequest(Json{
				{"Version", 1}, {"RequestId", std::to_string(++Sequence)},
				{"SessionToken", "physics-editorhost-test"},
				{"Method", std::move(Method)}, {"Params", std::move(Parameters)},
			}.dump()));
		};
		auto CheckSuccess = [&](std::string Method, Json Parameters) {
			auto Response = Call(std::move(Method), std::move(Parameters));
			Require(Response.at("Ok").get<bool>(),
				"[Physics:EditorHost] Command rejected: " + Response.dump());
			return Response;
		};
		const auto ProjectRoot = TemporaryRoot / "Project";
		CheckSuccess("CreateProject", {{"Destination", ProjectRoot.generic_string()}, {"Name", "Physics editor fixture"}});
		const auto Schema = CheckSuccess("GetSchema", Json::object());
		const auto &PartSchema = FindDefinition(Schema, "Engine.Part");
		const auto &BasePartSchema = FindDefinition(Schema, "Engine.BasePart");
		const auto &JointSchema = FindDefinition(Schema, "Engine.JointConstraint");
		for (const auto Name : {"CollisionGroup", "CollisionMask", "MovingBodyCCD", "LinearDamping", "AngularDamping", "GravityScale", "SleepEnabled", "Kinematic"}) {
			const auto &Property = FindProperty(BasePartSchema, Name);
			Require(Property.at("Editable") == true && Property.at("AtomicBatchWritable") == true &&
				Property.at("Persistence") == "Saved" && Property.at("Replication") == "Replicated",
				"[Physics:EditorHost] BasePart physics property is not authorable and durable: " + std::string(Name));
		}
		Require(FindProperty(BasePartSchema, "CollisionGroup").at("NumericRange").at("Maximum") == 15 &&
			FindProperty(BasePartSchema, "CollisionMask").at("NumericRange").at("Maximum") == 65535 &&
			FindProperty(BasePartSchema, "LinearDamping").at("NumericRange").at("Minimum") == 0,
			"[Physics:EditorHost] Collision and damping bounds are visible to inspection");
		for (const auto Name : {"RestLength", "SpringFrequency", "MotorSpeed", "MaximumMotorForce"}) {
			const auto &Property = FindProperty(JointSchema, Name);
			Require(Property.at("Editable") == true && Property.at("AtomicBatchWritable") == true &&
				Property.at("Persistence") == "Saved" && Property.at("Replication") == "Replicated",
				"[Physics:EditorHost] Joint parameter is not authorable and durable: " + std::string(Name));
		}
		Require(FindProperty(JointSchema, "JointType").at("Editable") == true &&
			FindProperty(JointSchema, "JointType").at("AtomicBatchWritable") == false,
			"[Physics:EditorHost] Validated JointType setter is exposed outside prepared batch");

		const auto InitialSnapshot = CheckSuccess("GetSnapshot", Json::object());
		const auto Workspace = FindObject(InitialSnapshot, "Workspace");
		const auto Scope = InitialSnapshot.at("Result").at("Snapshot").at("Cursor").at("Scope");
		auto Create = [&](const Json &Definition, std::string Name) {
			return CheckSuccess("CreateInstance", {
				{"ClassSchemaId", Definition.at("SchemaId")},
				{"DefinitionVersion", Definition.at("DefinitionVersion")},
				{"Parent", Workspace.at("Id")}, {"Name", std::move(Name)},
			}).at("Result").at("Object");
		};
		const auto PartId = Create(PartSchema, "AuthoredPhysicsPart");
		const auto JointId = Create(JointSchema, "AuthoredPhysicsJoint");
		const auto JointType = CheckSuccess("SetProperty", {
			{"Object", JointId},
			{"ClassSchemaId", JointSchema.at("SchemaId")},
			{"ClassDefinitionVersion", JointSchema.at("DefinitionVersion")},
			{"DeclaringClassSchemaId", JointSchema.at("SchemaId")},
			{"DeclaringDefinitionVersion", JointSchema.at("DefinitionVersion")},
			{"Property", "JointType"},
			{"Value", {{"Type", "String"}, {"Value", "Distance"}}},
		});
		Require(JointType.at("Result").at("Status") == "Success",
			"[Physics:EditorHost] JointType committed through the ordinary mutation command");
		auto Write = [](const Json &Object, const Json &Definition, std::string Name, Json Value) {
			return Json{{"Object", Object},
				{"DeclaringClassSchemaId", Definition.at("SchemaId")},
				{"DeclaringDefinitionVersion", Definition.at("DefinitionVersion")},
				{"Property", std::move(Name)}, {"Value", std::move(Value)}};
		};
		auto Batch = [&](Json Writes) {
			const auto State = CheckSuccess("GetProjectState", Json::object());
			return Call("SetPropertyBatch", {
				{"PropertyBatchVersion", 1}, {"Scope", Scope},
				{"ExpectedRevision", State.at("Result").at("AuthoritativeRevision")},
				{"Writes", std::move(Writes)},
			});
		};
		const auto GoodBatch = Batch(Json::array({
			Write(PartId, BasePartSchema, "CollisionGroup", {{"Type", "Int"}, {"Value", 5}}),
			Write(PartId, BasePartSchema, "CollisionMask", {{"Type", "Int"}, {"Value", 31}}),
			Write(PartId, BasePartSchema, "MovingBodyCCD", {{"Type", "Bool"}, {"Value", true}}),
			Write(PartId, BasePartSchema, "LinearDamping", {{"Type", "Float"}, {"Value", 0.25}}),
			Write(PartId, BasePartSchema, "AngularDamping", {{"Type", "Float"}, {"Value", 0.125}}),
			Write(PartId, BasePartSchema, "GravityScale", {{"Type", "Float"}, {"Value", 0.5}}),
			Write(PartId, BasePartSchema, "SleepEnabled", {{"Type", "Bool"}, {"Value", false}}),
			Write(JointId, JointSchema, "RestLength", {{"Type", "Float"}, {"Value", 2.0}}),
			Write(JointId, JointSchema, "SpringFrequency", {{"Type", "Float"}, {"Value", 4.0}}),
		}));
		Require(GoodBatch.at("Ok") == true && GoodBatch.at("Result").at("ChangedWriteCount") == 9,
			"[Physics:EditorHost] One prepared command commits BasePart and Joint parameters together: " + GoodBatch.dump());
		const auto AppliedSnapshot = CheckSuccess("GetSnapshot", Json::object());
		const auto AppliedPart = FindObject(AppliedSnapshot, "AuthoredPhysicsPart").at("EditorProperties");
		const auto AppliedJoint = FindObject(AppliedSnapshot, "AuthoredPhysicsJoint").at("EditorProperties");
		Require(AppliedPart.at("CollisionGroup").at("Value") == 5 &&
			AppliedPart.at("CollisionMask").at("Value") == 31 &&
			AppliedPart.at("MovingBodyCCD").at("Value") == true &&
			AppliedPart.at("LinearDamping").at("Value") == 0.25 &&
			AppliedPart.at("AngularDamping").at("Value") == 0.125 &&
			AppliedPart.at("GravityScale").at("Value") == 0.5 &&
			AppliedPart.at("SleepEnabled").at("Value") == false &&
			AppliedJoint.at("JointType").at("Value") == "Distance" &&
			AppliedJoint.at("RestLength").at("Value") == 2.0 &&
			AppliedJoint.at("SpringFrequency").at("Value") == 4.0,
			"[Physics:EditorHost] Editor inspection returns committed physics properties");
		(void)CheckSuccess("PollChanges", Json::object());
		const auto BeforeRejected = CheckSuccess("GetProjectState", Json::object()).at("Result");
		const auto InvalidBatch = Batch(Json::array({
			Write(PartId, BasePartSchema, "CollisionGroup", {{"Type", "Int"}, {"Value", 6}}),
			Write(PartId, BasePartSchema, "MovingBodyCCD", {{"Type", "Bool"}, {"Value", false}}),
			Write(JointId, JointSchema, "SpringFrequency", {{"Type", "Float"}, {"Value", 16.0}}),
		}));
		Require(InvalidBatch.at("Ok") == false && InvalidBatch.at("Error").at("Code") == "ValidationFailed",
			"[Physics:EditorHost] Out-of-range joint parameter rejects a mixed prepared batch: " + InvalidBatch.dump());
		const auto RejectedChanges = CheckSuccess("PollChanges", Json::object());
		const auto RejectedSnapshot = CheckSuccess("GetSnapshot", Json::object());
		Require(CheckSuccess("GetProjectState", Json::object()).at("Result") == BeforeRejected &&
			RejectedChanges.at("Result").at("Records").empty() &&
			FindObject(RejectedSnapshot, "AuthoredPhysicsPart").at("EditorProperties") == AppliedPart &&
			FindObject(RejectedSnapshot, "AuthoredPhysicsJoint").at("EditorProperties") == AppliedJoint,
			"[Physics:EditorHost] Failed batch leaves revision, journal, and both inspected objects unchanged");
		const auto InvalidGroup = Batch(Json::array({
			Write(PartId, BasePartSchema, "CollisionGroup", {{"Type", "Int"}, {"Value", 16}}),
			Write(JointId, JointSchema, "RestLength", {{"Type", "Float"}, {"Value", 3.0}}),
		}));
		Require(InvalidGroup.at("Ok") == false && InvalidGroup.at("Error").at("Code") == "ValidationFailed" &&
			CheckSuccess("GetProjectState", Json::object()).at("Result") == BeforeRejected &&
			FindObject(CheckSuccess("GetSnapshot", Json::object()), "AuthoredPhysicsJoint").at("EditorProperties") == AppliedJoint,
			"[Physics:EditorHost] Out-of-range collision group rejects without a partial joint edit");

		CheckSuccess("SaveProject", Json::object());
		CheckSuccess("OpenProject", {{"Root", ProjectRoot.generic_string()}});
		const auto ReopenedSnapshot = CheckSuccess("GetSnapshot", Json::object());
		Require(SameFields(FindObject(ReopenedSnapshot, "AuthoredPhysicsPart").at("EditorProperties"),
			AppliedPart, {"CollisionGroup", "CollisionMask", "MovingBodyCCD", "LinearDamping", "AngularDamping", "GravityScale", "SleepEnabled"}) &&
			SameFields(FindObject(ReopenedSnapshot, "AuthoredPhysicsJoint").at("EditorProperties"),
				AppliedJoint, {"JointType", "RestLength", "SpringFrequency"}),
			"[Physics:EditorHost] Saved physics properties survive EditorHost reopen");
	}
}

int main() {
#ifdef _WIN32
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
		TestPhysicsPropertyAuthoring();
		std::cout << "[Physics:EditorHost] Physics property authoring passed\n";
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Physics:EditorHost] " << Error.what() << '\n';
		return 1;
	}
}
