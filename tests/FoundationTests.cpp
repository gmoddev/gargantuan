#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/Frame.hpp"
#include "gargantuan/classes/ModuleScript.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/WeldConstraint.hpp"
#include "gargantuan/editor/EditorHost.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"
#include "gargantuan/runtime/DataModelRoot.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"
#include "gargantuan/runtime/JobSystem.hpp"
#include "gargantuan/runtime/InProcessReplicationSession.hpp"
#include "gargantuan/runtime/MutationGateway.hpp"
#include "gargantuan/runtime/ObjectId.hpp"
#include "gargantuan/runtime/Snapshot.hpp"
#include "gargantuan/runtime/WireJournal.hpp"
#include "gargantuan/reflection/RuntimeSchema.hpp"
#include "gargantuan/scripting/ModuleResolution.hpp"
#include "gargantuan/scripting/NativeCallback.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <lua.h>
#include <lualib.h>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

namespace gargantuan {
	struct WorldRootTestAccess {
		static std::size_t BodyCount(const WorldRoot &world) { return world.PartBodies.size(); }
		static std::size_t JointCount(const WorldRoot &world) { return world.ConstraintJoints.size(); }
	};
}

namespace {
	template <typename Needle, typename Variant> struct VariantContains;
	template <typename Needle, typename... Values>
	struct VariantContains<Needle, std::variant<Values...>> : std::bool_constant<(std::is_same_v<Needle, Values> || ...)> {};
	static_assert(!VariantContains<std::any, gargantuan::WireValue>::value);
	static_assert(std::is_same_v<
		decltype(gargantuan::PropertyUpdatedChange::Value),
		gargantuan::WireValue
	>);

	int Failures = 0;

	void Check(bool condition, const char *message) {
		if (!condition) {
			std::cerr << "FAIL: " << message << '\n';
			++Failures;
		}
	}

	template <typename Exception, typename Callback> void CheckThrows(Callback callback, const char *message) {
		try {
			callback();
		} catch (const Exception &) {
			return;
		} catch (...) {
		}
		Check(false, message);
	}

	struct SchemaTestTypeA {};
	struct SchemaTestTypeB {};
	struct SchemaTestTypeC {};
	struct SchemaTestTypeD {};

	gargantuan::SchemaDefinition MakeSchemaDefinition(
		std::string name,
		std::optional<std::string> baseName = std::nullopt,
		std::optional<gargantuan::SchemaId> id = std::nullopt
	) {
		using namespace gargantuan;
		SchemaDefinition definition;
		definition.Namespace = "Test";
		definition.ClassName = std::move(name);
		definition.Id = id.value_or(SchemaId::FromNativeName(definition.Namespace, definition.ClassName));
		definition.DefinitionVersion = 1;
		definition.Provenance = SchemaProvenance::NativeEngine;
		definition.Superclass = std::move(baseName);
		definition.BaseSchemaId = definition.Superclass
			? std::optional(SchemaId::FromNativeName(definition.Namespace, *definition.Superclass))
			: std::nullopt;
		return definition;
	}

	gargantuan::InstanceProperty MakeReadOnlySchemaProperty(std::string name, std::string type = "string") {
		gargantuan::InstanceProperty property(std::move(name));
		property.SetReflectedTypedef(std::move(type)).SetEditable(false);
		property.Read = [](gargantuan::Instance *) -> std::any { return std::string("value"); };
		return property;
	}

	void TestHierarchyAndDestruction() {
		using namespace gargantuan;
		auto parent = std::make_shared<Folder>();
		auto child = std::make_shared<Folder>();
		auto grandchild = std::make_shared<Folder>();
		child->SetParent(parent);
		grandchild->SetParent(child);
		ChangeJournal::Get().Clear();
		CheckThrows<std::invalid_argument>([&] { parent->SetParent(parent); }, "self-parenting is rejected");
		CheckThrows<std::invalid_argument>([&] { parent->SetParent(grandchild); }, "descendant cycles are rejected");
		Check(ChangeJournal::Get().ReadSince(0).empty(), "rejected hierarchy cycles emit no committed records");

		int destroyingCalls = 0;
		auto childId = child->GetObjectId();
		child->Destroying->Connect([&](std::monostate) {
			++destroyingCalls;
			Check(!ObjectRegistry::Get().Lookup(childId), "lookup is invalid before destruction callbacks");
			child->Destroy();
		});
		child->Destroy();
		child->Destroy();
		Check(child->GetDestroyed(), "Destroy commits destroyed state before callbacks");
		Check(destroyingCalls == 1, "Destroy is reentrant-safe and idempotent");
		Check(!child->GetParent().has_value(), "Destroy detaches the child");

		auto retainedChild = std::make_shared<Folder>();
		{
			auto temporaryParent = std::make_shared<Folder>();
			retainedChild->SetParent(temporaryParent);
		}
		Check(!retainedChild->GetParent().has_value(), "expired parent references do not dangle");
	}

	void TestObjectIdsAndChanges() {
		using namespace gargantuan;
		ChangeJournal::Get().Clear();
		auto first = std::make_shared<Folder>();
		auto firstId = first->GetObjectId();
		Check(firstId.IsValid(), "ObjectId is allocated");
		Check(ObjectRegistry::Get().Lookup(firstId) == first, "live ObjectId lookup succeeds");
		first->Destroy();
		Check(!ObjectRegistry::Get().Lookup(firstId), "destroy invalidates ObjectId lookup");
		auto second = std::make_shared<Folder>();
		auto secondId = second->GetObjectId();
		Check(firstId != secondId, "reused slots receive a new generation");
		Check(!ObjectRegistry::Get().Lookup(firstId), "stale generation remains invalid");

		second->SetName("CommittedName");
		auto parent = std::make_shared<Folder>();
		second->SetParent(parent);
		second->Destroy();
		auto records = ChangeJournal::Get().ReadSince(0);
		bool sawProperty = false;
		bool sawReparent = false;
		bool sawDestroy = false;
		for (const auto &record : records) {
			sawProperty = sawProperty || std::holds_alternative<PropertyUpdatedChange>(record.Payload);
			sawReparent = sawReparent || std::holds_alternative<ObjectReparentedChange>(record.Payload);
			sawDestroy = sawDestroy || std::holds_alternative<ObjectDestroyedChange>(record.Payload);
		}
		for (std::size_t i = 1; i < records.size(); ++i)
			Check(records[i - 1].Sequence < records[i].Sequence, "change records are strictly ordered");
		Check(!records.empty(), "committed mutations produce change records");
		Check(sawProperty && sawReparent && sawDestroy, "journal represents property, reparent, and destroy commits");
	}

	void TestWorldRootConstraintValidation() {
		using namespace gargantuan;
		auto game = std::make_shared<DataModel>();
		auto workspace = std::dynamic_pointer_cast<Workspace>(game->GetService("Workspace"));
		Check(workspace != nullptr, "WorldRoot regression fixture obtains Workspace");
		if (!workspace) return;

		auto unrelated = std::make_shared<Folder>();
		unrelated->SetParent(workspace);
		const auto bodiesBeforeRemoval = WorldRootTestAccess::BodyCount(*workspace);
		const auto jointsBeforeRemoval = WorldRootTestAccess::JointCount(*workspace);
		unrelated->SetParent(nullptr);
		Check(
			WorldRootTestAccess::BodyCount(*workspace) == bodiesBeforeRemoval &&
				WorldRootTestAccess::JointCount(*workspace) == jointsBeforeRemoval,
			"removing an unrelated descendant does not enter physics teardown paths"
		);

		auto bothMissing = std::make_shared<WeldConstraint>();
		bothMissing->SetParent(workspace);
		Check(WorldRootTestAccess::JointCount(*workspace) == 0, "constraint with both endpoints missing is rejected");
		bothMissing->SetParent(nullptr);

		auto part0 = std::make_shared<Part>();
		auto part1 = std::make_shared<Part>();
		part0->SetParent(workspace);
		part1->SetParent(workspace);

		auto part0Missing = std::make_shared<WeldConstraint>();
		part0Missing->SetPart1(part1);
		part0Missing->SetParent(workspace);
		Check(WorldRootTestAccess::JointCount(*workspace) == 0, "constraint with only Part0 missing is rejected");
		part0Missing->SetParent(nullptr);

		auto part1Missing = std::make_shared<WeldConstraint>();
		part1Missing->SetPart0(part0);
		part1Missing->SetParent(workspace);
		Check(WorldRootTestAccess::JointCount(*workspace) == 0, "constraint with only Part1 missing is rejected");
		part1Missing->SetParent(nullptr);

		auto valid = std::make_shared<WeldConstraint>();
		valid->SetPart0(part0);
		valid->SetPart1(part1);
		valid->SetParent(workspace);
		auto [activePart0, activePart1] = valid->GetActiveParts();
		Check(activePart0 == part0 && activePart1 == part1, "WeldConstraint resolves both distinct endpoints");
		Check(WorldRootTestAccess::JointCount(*workspace) == 1, "constraint with valid endpoints creates one joint");
		valid->SetParent(nullptr);
		Check(WorldRootTestAccess::JointCount(*workspace) == 0, "removing a valid constraint tears down its joint");

		auto selfConstraint = std::make_shared<WeldConstraint>();
		selfConstraint->SetPart0(part0);
		selfConstraint->SetPart1(part0);
		selfConstraint->SetParent(workspace);
		Check(WorldRootTestAccess::JointCount(*workspace) == 0, "self constraints are rejected");
		selfConstraint->SetParent(nullptr);
	}

	void TestCheckedResolutionAndOwnedPaths() {
		using namespace gargantuan;
		auto folder = std::make_shared<Folder>();
		CheckThrows<std::invalid_argument>([&] { ResolveRequiredModule(folder); }, "wrong require target is rejected");
		auto module = std::make_shared<ModuleScript>();
		Check(ResolveRequiredModule(module) == module, "ModuleScript require target is accepted");
		auto game = PrepareDataModelRoot(folder);
		Check(game != nullptr && folder->GetParent().has_value(), "standalone non-DataModel root is wrapped safely");
		Check(PrepareDataModelRoot(game) == game, "DataModel root preserves checked identity");

		std::string source = "Owned";
		InstanceSerialization::DeserializationState state;
		state.CurrentPath.push_back(source);
		source.assign("Changed");
		Check(state.CurrentPath.back() == "Owned", "deserialization paths own retained strings");
	}

	void TestJobSystem() {
		using namespace gargantuan;
		JobSystem jobs(3);
		auto group = std::make_shared<JobGroup>();
		std::atomic<int> count = 0;
		for (int i = 0; i < 100; ++i) jobs.Submit([&] { ++count; }, group);
		group->Wait();
		Check(count == 100, "jobs execute exactly once and group waits");

		auto exceptionGroup = std::make_shared<JobGroup>();
		jobs.Submit([] { throw std::runtime_error("contained"); }, exceptionGroup);
		exceptionGroup->Wait();
		Check(exceptionGroup->GetFirstException() != nullptr, "job exceptions are contained in their group");

		auto folder = std::make_shared<Folder>();
		auto affinityGroup = std::make_shared<JobGroup>();
		jobs.Submit([folder] { folder->SetName("Illegal"); }, affinityGroup);
		affinityGroup->Wait();
		Check(affinityGroup->GetFirstException() != nullptr, "worker hierarchy/property mutation is rejected");

		std::vector<std::thread> submitters;
		auto concurrentGroup = std::make_shared<JobGroup>();
		for (int i = 0; i < 4; ++i) {
			submitters.emplace_back([&] {
				for (int j = 0; j < 25; ++j) jobs.Submit([&] { ++count; }, concurrentGroup);
			});
		}
		for (auto &thread : submitters) thread.join();
		concurrentGroup->Wait();
		Check(count == 200, "concurrent submission is safe");
		jobs.Shutdown(true);
		CheckThrows<std::runtime_error>([&] { jobs.Submit([] {}); }, "submission after shutdown is rejected");

		JobSystem drainingJobs(2);
		auto drainingGroup = std::make_shared<JobGroup>();
		std::atomic<int> drainedCount = 0;
		for (int i = 0; i < 50; ++i) drainingJobs.Submit([&] { ++drainedCount; }, drainingGroup);
		drainingJobs.Shutdown(true);
		Check(drainingGroup->IsComplete() && drainedCount == 50, "draining shutdown completes queued jobs");
	}

	void TestSchemaMetadata() {
		using namespace gargantuan;
		auto *destroyed = InstanceClassRegistry::GetDefinitionByName("Instance")->AllProperties.at("Destroyed");
		auto *name = InstanceClassRegistry::GetDefinitionByName("Instance")->AllProperties.at("Name");
		auto *archivable = InstanceClassRegistry::GetDefinitionByName("Instance")->AllProperties.at("Archivable");
		Check(!destroyed->Editable && !destroyed->Write, "Destroyed lifecycle metadata is read-only and non-editable");
		Check(
			name->ReplicationPolicy == InstanceProperty::Replication::FutureReplicated,
			"Name is selected for replication by explicit schema metadata"
		);
		Check(
			archivable->ReplicationPolicy == InstanceProperty::Replication::None,
			"Archivable remains excluded from replication by explicit schema metadata"
		);
		InstanceProperty property("SchemaProbe");
		property.SetSerializable().SetReplication(InstanceProperty::Replication::FutureReplicated).SetAuthority(
			InstanceProperty::Authority::Any
		);
		Check(property.PersistencePolicy == InstanceProperty::Persistence::Saved, "serializable maps to saved persistence");
		Check(
			property.ReplicationPolicy == InstanceProperty::Replication::FutureReplicated,
			"replication metadata is retained"
		);
		Check(property.WriteAuthority == InstanceProperty::Authority::Any, "authority metadata is retained");
		Check(
			name->RequiredReadCapability == ScriptCapability::ReadDataModel &&
				name->RequiredWriteCapability == ScriptCapability::MutateDataModel,
			"property metadata carries enforceable read and mutation capabilities"
		);
		Check(
			name->ReadDomains.Contains(ScriptExecutionDomain::Studio) &&
				name->WriteDomains.Contains(ScriptExecutionDomain::Server),
			"property metadata represents domains as independent set membership"
		);
	}

	void TestRuntimeSchemaRegistry() {
		using namespace gargantuan;

		constexpr auto expectedPartId = SchemaId::FromNativeName("Engine", "Part");
		auto *partDefinition = InstanceClassRegistry::GetDefinitionByName("Part");
		auto *qualifiedPartDefinition = InstanceClassRegistry::GetDefinitionByName("Engine.Part");
		Check(partDefinition && partDefinition->Id == expectedPartId, "native classes have deterministic SchemaIds");
		Check(qualifiedPartDefinition == partDefinition, "canonical and compatibility class lookup share one definition");
		Check(
			InstanceClassRegistry::GetDefinitionBySchemaId(expectedPartId) == partDefinition,
			"schema lookup by native ID succeeds"
		);
		Check(
			InstanceClassRegistry::GetDefinitionBySchemaId(SchemaId::FromParts(0x1234, 0x5678)) == nullptr,
			"unknown SchemaId lookup fails safely"
		);
		auto serializedId = expectedPartId.ToString();
		Check(SchemaId::Parse(serializedId) == expectedPartId, "SchemaId has a deterministic serialized form");
		Check(!SchemaId::Parse(std::string(32, 'z')), "malformed SchemaId fails safely");
		Check(!SchemaId{}.IsValid() && !SchemaId::Parse(std::string(32, '0')), "zero SchemaId is explicitly invalid");

		RuntimeSchemaRegistry firstConstruction;
		RuntimeSchemaRegistry secondConstruction;
		firstConstruction.RegisterNative<SchemaTestTypeA>(MakeSchemaDefinition("Stable"));
		secondConstruction.RegisterNative<SchemaTestTypeA>(MakeSchemaDefinition("Stable"));
		Check(
			firstConstruction.FindByName("Test.Stable")->Id == secondConstruction.FindByName("Test.Stable")->Id,
			"SchemaId survives repeated registry construction"
		);
		Check(
			SchemaId::FromNativeName("Engine", "Part") != SchemaId::FromNativeName("Engine", "Folder"),
			"distinct native classes have distinct SchemaIds"
		);

		RuntimeSchemaRegistry duplicateIds;
		const auto sharedId = SchemaId::FromParts(1, 1);
		duplicateIds.RegisterNative<SchemaTestTypeA>(MakeSchemaDefinition("First", std::nullopt, sharedId));
		CheckThrows<std::invalid_argument>(
			[&] { duplicateIds.RegisterNative<SchemaTestTypeB>(MakeSchemaDefinition("Second", std::nullopt, sharedId)); },
			"duplicate SchemaIds are rejected"
		);

		RuntimeSchemaRegistry duplicateNames;
		duplicateNames.RegisterNative<SchemaTestTypeA>(
			MakeSchemaDefinition("SameName", std::nullopt, SchemaId::FromParts(2, 1))
		);
		CheckThrows<std::invalid_argument>(
			[&] {
				duplicateNames.RegisterNative<SchemaTestTypeB>(
					MakeSchemaDefinition("SameName", std::nullopt, SchemaId::FromParts(2, 2))
				);
			},
			"duplicate canonical names are rejected"
		);

		RuntimeSchemaRegistry invalidIdentity;
		auto invalidDefinition = MakeSchemaDefinition("InvalidId");
		invalidDefinition.Id = {};
		CheckThrows<std::invalid_argument>(
			[&] { invalidIdentity.RegisterNative<SchemaTestTypeA>(std::move(invalidDefinition)); },
			"invalid default SchemaIds are rejected"
		);

		RuntimeSchemaRegistry invalidMembers;
		auto wrongOwner = MakeSchemaDefinition("WrongOwner");
		auto wrongOwnerProperty = MakeReadOnlySchemaProperty("Value");
		wrongOwnerProperty.DeclaringSchemaId = SchemaId::FromParts(9, 9);
		wrongOwner.Properties.emplace("Value", std::move(wrongOwnerProperty));
		CheckThrows<std::invalid_argument>(
			[&] { invalidMembers.RegisterNative<SchemaTestTypeA>(std::move(wrongOwner)); },
			"members with a different declaring SchemaId are rejected"
		);
		auto unsupportedMember = MakeSchemaDefinition("UnsupportedMember");
		unsupportedMember.Properties.emplace("Value", MakeReadOnlySchemaProperty("Value", ""));
		CheckThrows<std::invalid_argument>(
			[&] { invalidMembers.RegisterNative<SchemaTestTypeB>(std::move(unsupportedMember)); },
			"members without reflected value types are rejected"
		);
		auto memberCollision = MakeSchemaDefinition("MemberCollision");
		memberCollision.Properties.emplace("Value", MakeReadOnlySchemaProperty("Value"));
		memberCollision.Methods.emplace("Value", UserdataMethod<Instance>{.Call = [](lua_State *, Instance *) { return 0; }});
		CheckThrows<std::invalid_argument>(
			[&] { invalidMembers.RegisterNative<SchemaTestTypeC>(std::move(memberCollision)); },
			"property and method name collisions are rejected"
		);

		RuntimeSchemaRegistry missingBase;
		missingBase.RegisterNative<SchemaTestTypeA>(MakeSchemaDefinition("Child", "Missing"));
		CheckThrows<std::invalid_argument>([&] { missingBase.Validate(); }, "missing schema base classes are rejected");

		RuntimeSchemaRegistry inheritanceCycle;
		inheritanceCycle.RegisterNative<SchemaTestTypeA>(MakeSchemaDefinition("CycleA", "CycleB"));
		inheritanceCycle.RegisterNative<SchemaTestTypeB>(MakeSchemaDefinition("CycleB", "CycleA"));
		CheckThrows<std::invalid_argument>([&] { inheritanceCycle.Validate(); }, "schema inheritance cycles are rejected");

		RuntimeSchemaRegistry orderedOne;
		RuntimeSchemaRegistry orderedTwo;
		orderedOne.RegisterNative<SchemaTestTypeA>(MakeSchemaDefinition("Zulu"));
		orderedOne.RegisterNative<SchemaTestTypeB>(MakeSchemaDefinition("Alpha"));
		orderedTwo.RegisterNative<SchemaTestTypeC>(MakeSchemaDefinition("Alpha"));
		orderedTwo.RegisterNative<SchemaTestTypeD>(MakeSchemaDefinition("Zulu"));
		auto firstOrder = orderedOne.Enumerate();
		auto secondOrder = orderedTwo.Enumerate();
		Check(
			firstOrder.size() == 2 && secondOrder.size() == 2 &&
				firstOrder[0]->CanonicalName == "Test.Alpha" && firstOrder[1]->CanonicalName == "Test.Zulu" &&
				firstOrder[0]->CanonicalName == secondOrder[0]->CanonicalName &&
				firstOrder[1]->CanonicalName == secondOrder[1]->CanonicalName,
			"schema enumeration is deterministic and independent of registration order"
		);

		auto *instanceDefinition = InstanceClassRegistry::GetDefinitionByName("Instance");
		auto *basePartDefinition = InstanceClassRegistry::GetDefinitionByName("BasePart");
		Check(
			partDefinition && basePartDefinition && partDefinition->BaseSchemaId == basePartDefinition->Id,
			"schema inheritance uses stable base identity"
		);
		Check(
			partDefinition && instanceDefinition && partDefinition->AllProperties.at("Name")->DeclaringSchemaId ==
				instanceDefinition->Id,
			"inherited members retain their declaring schema identity"
		);
		auto *cframe = partDefinition ? partDefinition->AllProperties.at("CFrame") : nullptr;
		Check(
			cframe && cframe->PersistencePolicy == InstanceProperty::Persistence::Saved &&
				cframe->ReplicationPolicy == InstanceProperty::Replication::FutureReplicated,
			"canonical schema exposes serialization and replication metadata"
		);
		Check(
			partDefinition && partDefinition->DefinitionVersion == 1 &&
				partDefinition->Provenance == SchemaProvenance::NativeEngine &&
				partDefinition->Namespace == "Engine",
			"native schema version, provenance, and namespace are populated"
		);

		auto part = std::make_shared<Part>();
		Check(part->IsA("Part") && part->IsA("BasePart") && part->IsA("Instance"), "existing IsA behavior remains compatible");
		ScriptSecurityContext noMutation{ScriptExecutionDomain::Studio, {ScriptCapability::ReadDataModel}};
		Check(
			part->ApplyPropertyMutation("Name", std::string("Denied"), Enums::Permission::None, noMutation) ==
				MutationStatus::Unauthorized,
			"schema-backed reflected mutation still enforces its native capability boundary"
		);
	}

	void TestScriptSecurityModel() {
		using namespace gargantuan;
		Check(GetScriptExecutionDomainName(ScriptExecutionDomain::Core) == "Core", "Core script domain is represented");
		Check(GetScriptExecutionDomainName(ScriptExecutionDomain::Studio) == "Studio", "Studio script domain is represented");
		Check(GetScriptExecutionDomainName(ScriptExecutionDomain::Server) == "Server", "Server script domain is represented");
		Check(GetScriptExecutionDomainName(ScriptExecutionDomain::Client) == "Client", "Client script domain is represented");

		ScriptSecurityContext coreWithoutCapabilities{ScriptExecutionDomain::Core, {}};
		ScriptSecurityContext serverMutator{
			ScriptExecutionDomain::Server,
			{ScriptCapability::MutateDataModel},
		};
		ScriptSecurityContext clientProcessOnly{
			ScriptExecutionDomain::Client,
			{ScriptCapability::ProcessControl},
		};
		Check(
			!coreWithoutCapabilities.HasCapability(ScriptCapability::MutateDataModel),
			"Core domain does not imply mutation capability"
		);
		Check(
			!clientProcessOnly.HasCapability(ScriptCapability::ReadDataModel),
			"capabilities are independent rather than a numeric privilege hierarchy"
		);

		auto folder = std::make_shared<Folder>();
		const auto id = folder->GetObjectId();
		MutationGateway gateway;
		auto deniedCore = gateway.Apply(
			UpdatePropertyCommand{id, "Name", std::string("DeniedCore")},
			coreWithoutCapabilities
		);
		Check(deniedCore.Status == MutationStatus::Unauthorized, "native gateway enforces explicit capabilities for Core");
		auto allowedServer = gateway.Apply(
			UpdatePropertyCommand{id, "Name", std::string("AllowedServer")},
			serverMutator
		);
		Check(
			allowedServer.Succeeded() && folder->GetName() == "AllowedServer",
			"Server domain can mutate only when explicitly granted MutateDataModel"
		);
		auto deniedClient = gateway.Apply(
			UpdatePropertyCommand{id, "Name", std::string("DeniedClient")},
			clientProcessOnly
		);
		Check(deniedClient.Status == MutationStatus::Unauthorized, "unrelated ProcessControl does not grant mutation");
		folder->Destroy();
	}

	void TestMutationGateway() {
		using namespace gargantuan;
		auto &journal = ChangeJournal::Get();
		journal.Clear();
		auto folder = std::make_shared<Folder>();
		const auto id = folder->GetObjectId();
		journal.Clear();

		MutationGateway gateway;
		JobSystem jobs(2);
		auto submitted = std::make_shared<JobGroup>();
		std::shared_ptr<MutationCompletion> completion;
		jobs.Submit(
			[&] { completion = gateway.Submit(UpdatePropertyCommand{id, "Name", std::string("FromWorker")}); },
			submitted
		);
		submitted->Wait();
		Check(folder->GetName() != "FromWorker", "worker submission does not mutate authoritative state");
		Check(journal.ReadSince(0).empty(), "queued command is not recorded before commit");
		Check(gateway.Drain() == 1, "Main drains one queued mutation");
		Check(completion->Wait().Succeeded(), "queued property mutation succeeds");
		Check(folder->GetName() == "FromWorker", "Main applies worker property mutation");
		auto records = journal.ReadSince(0);
		Check(records.size() == 1, "successful property command emits exactly one change record");
		Check(
			std::holds_alternative<PropertyUpdatedChange>(records.front().Payload) &&
				std::get<PropertyUpdatedChange>(records.front().Payload).PropertyName == "Name" &&
				std::get<std::string>(std::get<PropertyUpdatedChange>(records.front().Payload).Value) == "FromWorker",
			"property command records the committed property value"
		);

		journal.Clear();
		auto invalid = gateway.Apply(UpdatePropertyCommand{id, "Missing", std::string("value")});
		Check(invalid.Status == MutationStatus::InvalidProperty, "invalid property command is rejected");
		Check(journal.ReadSince(0).empty(), "invalid property command emits no record");
		auto wrongType = gateway.Apply(UpdatePropertyCommand{id, "Name", 42});
		Check(wrongType.Status == MutationStatus::ValidationFailed, "wrong native property type is rejected");
		Check(journal.ReadSince(0).empty(), "wrong native property type emits no record");

		auto *nameProperty = const_cast<InstanceProperty *>(folder->FindProperty("Name"));
		const auto originalValidator = nameProperty->Validate;
		nameProperty->Validate = [](const std::any &value) {
			auto *name = std::any_cast<std::string>(&value);
			return name && *name != "Rejected";
		};
		auto validation = gateway.Apply(UpdatePropertyCommand{id, "Name", std::string("Rejected")});
		Check(validation.Status == MutationStatus::ValidationFailed, "schema validation rejects command value");
		Check(journal.ReadSince(0).empty(), "validation failure emits no record");
		nameProperty->Validate = originalValidator;

		const auto originalPermission = nameProperty->WritePermission;
		nameProperty->WritePermission = Enums::Permission::Engine;
		auto unauthorized = gateway.Apply(UpdatePropertyCommand{id, "Name", std::string("Unauthorized")});
		Check(unauthorized.Status == MutationStatus::Unauthorized, "property permission rejects command origin");
		Check(journal.ReadSince(0).empty(), "unauthorized command emits no record");
		nameProperty->WritePermission = originalPermission;

		auto readOnly = gateway.Apply(UpdatePropertyCommand{id, "Destroyed", true});
		Check(readOnly.Status == MutationStatus::ReadOnly, "read-only property command is rejected");
		Check(journal.ReadSince(0).empty(), "read-only command emits no record");

		MutationResult bypass;
		auto bypassGroup = std::make_shared<JobGroup>();
		jobs.Submit([&] { bypass = gateway.Apply(UpdatePropertyCommand{id, "Name", std::string("Bypass")}); }, bypassGroup);
		bypassGroup->Wait();
		Check(bypass.Status == MutationStatus::WrongExecutionDomain, "worker direct apply is unauthorized");
		Check(journal.ReadSince(0).empty(), "unauthorized worker apply emits no record");

		auto parent = std::make_shared<Folder>();
		const auto parentId = parent->GetObjectId();
		journal.Clear();
		auto created = gateway.Apply(CreateObjectCommand{"Folder", parentId});
		Check(created.Succeeded() && created.Object.has_value(), "create command constructs an owned object");
		auto createdObject = ObjectRegistry::Get().Lookup(*created.Object);
		Check(createdObject && createdObject->GetParent() == parent, "created object is attached to its authoritative parent");
		auto otherParent = std::make_shared<Folder>();
		const auto otherParentId = otherParent->GetObjectId();
		journal.Clear();
		auto reparented = gateway.Apply(ReparentObjectCommand{*created.Object, otherParentId});
		Check(reparented.Succeeded() && createdObject->GetParent() == otherParent, "reparent command applies on Main");
		Check(journal.ReadSince(0).size() == 1, "reparent command emits one committed record");
		journal.Clear();
		auto destroyed = gateway.Apply(DestroyObjectCommand{*created.Object});
		Check(destroyed.Succeeded() && createdObject->GetDestroyed(), "destroy command applies on Main");
		Check(!ObjectRegistry::Get().Lookup(*created.Object), "destroy command invalidates lookup");

		folder->Destroy();
		journal.Clear();
		auto dead = gateway.Apply(UpdatePropertyCommand{id, "Name", std::string("Dead")});
		Check(dead.Status == MutationStatus::StaleObject, "destroyed object command is rejected as stale");
		Check(journal.ReadSince(0).empty(), "destroyed object command emits no record");
		jobs.Shutdown(true);
	}

	void TestBoundedJournalCursor() {
		using namespace gargantuan;
		auto &journal = ChangeJournal::Get();
		const auto originalCapacity = journal.GetCapacity();
		journal.Clear();
		journal.SetCapacity(2);
		auto folder = std::make_shared<Folder>();
		const auto id = folder->GetObjectId();
		auto cursor = journal.CreateCursor();
		folder->SetName("One");
		folder->SetName("Two");
		auto available = journal.Read(cursor, 1);
		Check(available.Status == ChangeReadStatus::Available, "live cursor reads retained changes");
		Check(available.Records.size() == 1, "cursor read respects its record bound");
		folder->SetName("Three");
		folder->SetName("Four");
		auto stale = journal.Read(cursor);
		Check(stale.Status == ChangeReadStatus::ResnapshotRequired, "evicted cursor requires resnapshot");
		const auto beforeClear = journal.CreateCursor().NextSequence;
		journal.Clear();
		folder->SetName("Five");
		Check(
			journal.CreateCursor().NextSequence > beforeClear,
			"clearing retained records does not reuse journal sequence numbers"
		);
		journal.SetCapacity(originalCapacity);
		journal.Clear();
	}

	void TestSnapshotBaseline() {
		using namespace gargantuan;
		auto &journal = ChangeJournal::Get();
		journal.Clear();
		auto game = std::make_shared<DataModel>();
		game->SetName("SnapshotGame");
		auto first = std::make_shared<Part>();
		first->SetName("First");
		first->SetTransparency(0.25f);
		first->SetParent(game);
		auto second = std::make_shared<Part>();
		second->SetName("Second");
		second->SetParent(game);
		auto weld = std::make_shared<WeldConstraint>();
		weld->SetName("Link");
		weld->SetPart0(first);
		weld->SetPart1(second);
		weld->SetParent(game);
		auto frame = std::make_shared<Frame>();
		frame->SetName("Panel");
		frame->SetAutomaticSize(Enums::AutomaticSize::XY);
		frame->SetPosition(UDim2(0.5f, 8, 1.0f, -4));
		frame->SetParent(game);

		auto snapshot = CaptureSnapshot(game);
		Check(snapshot.Cursor.Scope == game->GetObjectId(), "snapshot cursor identifies its DataModel scope");
		CheckThrows<std::runtime_error>(
			[&] { (void)CaptureSnapshot(first); },
			"snapshot capture requires the DataModel scope root"
		);
		const auto serialized = SerializeSnapshot(snapshot);
		const auto repeated = SerializeSnapshot(CaptureSnapshot(game));
		Check(serialized == repeated, "the same DataModel produces a deterministic snapshot");
		Check(serialized.find("ObjectReference") != std::string::npos, "snapshot uses an explicit reference wire type");
		Check(serialized.find("\"Slot\"") != std::string::npos, "snapshot uses explicit serialized ObjectIds");

		auto parsed = DeserializeSnapshot(serialized);
		Check(parsed.Succeeded(), "serialized snapshot parses successfully");
		Check(SerializeSnapshot(*parsed.Value) == serialized, "snapshot wire document round-trips deterministically");
		auto loaded = LoadSnapshot(*parsed.Value);
		Check(loaded.Succeeded(), "parsed snapshot materializes successfully");
		Check(loaded.Root && loaded.Root->GetName() == "SnapshotGame", "snapshot restores its root");
		auto loadedWeld = std::dynamic_pointer_cast<WeldConstraint>(loaded.Root->FindFirstChild("Link", false));
		auto loadedFirst = std::dynamic_pointer_cast<Part>(loaded.Root->FindFirstChild("First", false));
		auto loadedSecond = std::dynamic_pointer_cast<Part>(loaded.Root->FindFirstChild("Second", false));
		auto loadedFrame = std::dynamic_pointer_cast<Frame>(loaded.Root->FindFirstChild("Panel", false));
		Check(loadedWeld && loadedFirst && loadedSecond && loadedFrame, "snapshot restores hierarchy and classes");
		Check(
			loadedWeld->GetPart0() == loadedFirst && loadedWeld->GetPart1() == loadedSecond,
			"object references survive snapshot serialization and materialization"
		);
		Check(loadedFirst->GetTransparency() == 0.25f, "closed wire values restore serializable properties");
		Check(loadedFrame->GetAutomaticSize() == Enums::AutomaticSize::XY, "wire enums restore by type and item");
		Check(
			loadedFrame->GetPosition().X.Scale == 0.5f && loadedFrame->GetPosition().X.Offset == 8 &&
				loadedFrame->GetPosition().Y.Scale == 1.0f && loadedFrame->GetPosition().Y.Offset == -4,
			"compound wire values restore without native type erasure"
		);

		journal.Clear();
		auto transition = CaptureSnapshot(game);
		first->SetTransparency(0.5f);
		auto incremental = journal.Read(transition.Cursor);
		Check(incremental.Status == ChangeReadStatus::Available, "snapshot cursor transitions to journal consumption");
		Check(incremental.Records.size() == 1, "post-snapshot mutation appears exactly once after the cursor");

		const auto originalCapacity = journal.GetCapacity();
		journal.SetCapacity(1);
		first->SetTransparency(0.6f);
		first->SetTransparency(0.7f);
		auto evicted = journal.Read(transition.Cursor);
		Check(evicted.Status == ChangeReadStatus::ResnapshotRequired, "stale snapshot cursor requires resnapshot");
		journal.SetCapacity(originalCapacity);

		auto dangling = snapshot;
		auto danglingWeld = std::find_if(
			dangling.Objects.begin(), dangling.Objects.end(), [](const SnapshotObject &object) { return object.Name == "Link"; }
		);
		auto &reference = std::get<WireObjectReference>(danglingWeld->Properties.at("Part0"));
		reference.Object = {999999, 1};
		auto rejected = LoadSnapshot(dangling);
		Check(!rejected.Succeeded(), "dangling serialized references cannot resolve");
		auto wrongScope = snapshot;
		wrongScope.Cursor.Scope = second->GetObjectId();
		Check(!LoadSnapshot(wrongScope).Succeeded(), "snapshot root must match its declared replication scope");
		Check(!loaded.Resolve({999999, 1}), "unknown or stale snapshot IDs resolve to null");
		const auto loadedFirstId = snapshot.Objects[1].Id;
		loadedFirst->Destroy();
		Check(!loaded.Resolve(loadedFirstId), "destroyed snapshot objects no longer resolve");
		journal.Clear();
	}

	void TestWireJournalAndLoopbackReplication() {
		using namespace gargantuan;
		auto &journal = ChangeJournal::Get();
		const auto originalCapacity = journal.GetCapacity();
		journal.Clear();
		journal.SetCapacity(64);

		auto source = std::make_shared<DataModel>();
		source->SetName("SourceGame");
		auto first = std::make_shared<Part>();
		first->SetName("First");
		first->SetTransparency(0.1f);
		first->SetParent(source);
		auto weld = std::make_shared<WeldConstraint>();
		weld->SetName("Link");
		weld->SetPart0(first);
		weld->SetParent(source);

		auto started = InProcessReplicationSession::Start(source);
		Check(started.Succeeded(), "loopback session loads a serialized snapshot baseline");
		auto session = started.Session;
		Check(session && session->GetReceiverRoot(), "loopback session owns a receiver root");
		Check(session->GetReceiverRoot() != source, "receiver state is distinct from authoritative source state");
		Check(
			session->ApplyAvailable().Status == ReplicationApplyStatus::NoChanges,
			"snapshot plus zero deltas is already synchronized"
		);
		first->SetArchivable(true);
		Check(
			session->ApplyAvailable().Status == ReplicationApplyStatus::NoChanges,
			"properties without replication metadata do not enter the scoped wire stream"
		);
		auto otherScope = std::make_shared<DataModel>();
		otherScope->SetName("OtherGame");
		auto otherPart = std::make_shared<Part>();
		otherPart->SetName("OtherPart");
		otherPart->SetParent(otherScope);
		Check(
			session->ApplyAvailable().Status == ReplicationApplyStatus::NoChanges,
			"mutations in another DataModel do not enter this replication session"
		);

		auto second = std::make_shared<Part>();
		second->SetName("Second");
		second->SetTransparency(0.4f);
		second->SetParent(source);
		weld->SetPart1(second);
		first->SetTransparency(0.75f);

		auto applied = session->ApplyAvailable();
		Check(applied.Succeeded() && applied.AppliedRecords > 0, "ordered wire journal deltas apply successfully");
		auto receiverFirst = std::dynamic_pointer_cast<Part>(
			session->GetReceiverRoot()->FindFirstChild("First", false)
		);
		auto receiverSecond = std::dynamic_pointer_cast<Part>(
			session->GetReceiverRoot()->FindFirstChild("Second", false)
		);
		auto receiverWeld = std::dynamic_pointer_cast<WeldConstraint>(
			session->GetReceiverRoot()->FindFirstChild("Link", false)
		);
		Check(receiverFirst && receiverFirst->GetTransparency() == 0.75f, "property delta reproduces source state");
		Check(receiverSecond && receiverSecond->GetTransparency() == 0.4f, "create and pre-parent properties reproduce source state");
		Check(receiverWeld && receiverWeld->GetPart1() == receiverSecond, "create-before-reference ordering resolves correctly");

		const auto cursor = session->GetCursor();
		const auto scope = WireObjectId::FromObjectId(cursor.Scope);
		WireJournalRecord duplicate{
			.Sequence = cursor.NextSequence - 1,
			.Scope = scope,
			.Operation = WireJournalOperation::Destroy,
			.Object = WireObjectId::FromObjectId(first->GetObjectId()),
		};
		Check(
			session->ApplyWireRecords({duplicate}).Status == ReplicationApplyStatus::DuplicateRecord,
			"duplicate records are explicitly rejected"
		);
		auto outOfOrder = duplicate;
		outOfOrder.Sequence = cursor.NextSequence + 1;
		Check(
			session->ApplyWireRecords({outOfOrder}).Status == ReplicationApplyStatus::OutOfOrderRecord,
			"out-of-order records are rejected"
		);
		auto wrongScope = duplicate;
		wrongScope.Sequence = cursor.NextSequence;
		wrongScope.Scope = WireObjectId::FromObjectId(otherScope->GetObjectId());
		Check(
			session->ApplyWireRecords({wrongScope}).Status == ReplicationApplyStatus::ApplyRejected,
			"records from another replication scope are rejected"
		);

		const auto secondWireId = WireObjectId::FromObjectId(second->GetObjectId());
		second->SetParent(otherScope);
		Check(session->ApplyAvailable().Succeeded(), "cross-scope removal applies in source order");
		Check(!session->ResolveReceiver(secondWireId), "leaving the scope invalidates receiver reference lookup");
		WireJournalRecord staleReference{
			.Sequence = session->GetCursor().NextSequence,
			.Scope = scope,
			.Operation = WireJournalOperation::PropertyUpdate,
			.Object = WireObjectId::FromObjectId(weld->GetObjectId()),
			.PropertyName = "Part1",
			.Value = WireObjectReference{secondWireId},
		};
		Check(
			session->ApplyWireRecords({staleReference}).Status == ReplicationApplyStatus::ApplyRejected,
			"later references to destroyed objects are rejected"
		);
		weld->SetPart1(std::nullopt);

		auto encodedRecords = std::vector{
			WireJournalRecord{
				.Sequence = 1,
				.Scope = scope,
				.Operation = WireJournalOperation::PropertyUpdate,
				.Object = WireObjectId::FromObjectId(first->GetObjectId()),
				.PropertyName = "Transparency",
				.Value = WireFloat{0.5f},
			}
		};
		const auto serializedRecords = SerializeWireJournalRecords(encodedRecords);
		auto parsedRecords = DeserializeWireJournalRecords(serializedRecords);
		Check(parsedRecords.Succeeded(), "wire journal records round-trip through the shared WireValue codec");
		Check(serializedRecords.find("\"Type\":\"Float\"") != std::string::npos, "journal property values use WireValue encoding");
		Check(serializedRecords.find("\"Scope\"") != std::string::npos, "journal records serialize their replication scope");
		Check(
			!DeserializeWireJournalRecords("{\"Version\":999,\"Records\":[]}").Succeeded(),
			"unknown wire journal envelope versions fail closed"
		);
		Check(
			!DeserializeWireJournalRecords(
				"{\"Version\":2,\"Records\":[{\"Version\":3,\"Sequence\":1,\"Scope\":{\"Slot\":1,\"Generation\":1},\"Operation\":\"Destroy\",\"ObjectId\":{\"Slot\":1,\"Generation\":1}}]}"
			).Succeeded(),
			"unknown wire journal record versions fail closed"
		);

		journal.Clear();
		auto staleStarted = InProcessReplicationSession::Start(source);
		Check(staleStarted.Succeeded(), "stale-cursor probe session starts");
		journal.SetCapacity(1);
		first->SetTransparency(0.2f);
		first->SetTransparency(0.3f);
		Check(
			staleStarted.Session->ApplyAvailable().Status == ReplicationApplyStatus::ResnapshotRequired,
			"evicted replication cursor requires a new snapshot"
		);

		const auto sourceName = first->GetName();
		receiverFirst->SetName("ReceiverOnly");
		Check(first->GetName() == sourceName, "receiver mutation cannot mutate authoritative source state");
		journal.SetCapacity(originalCapacity);
		journal.Clear();
	}

	void TestSharedFrameRing() {
		using namespace gargantuan;
		if (!SharedFrameRing::IsSupported()) return;
		SharedFrameRing ring;
		Check(!ring.GetName().empty(), "shared frame ring has an unguessable session name");
		Check(
			ring.GetMappingBytes() == SharedFrameRingLayout::MappingBytes,
			"shared frame ring allocation is fixed and bounded"
		);
		std::vector<std::uint8_t> pixels(4 * 4 * 3, 0x5a);
		for (std::uint64_t sequence = 1; sequence <= 100; ++sequence) {
			Check(
				ring.Publish(4, 4, pixels, sequence) == sequence,
				"shared frame publication sequence is monotonic"
			);
		}
		Check(ring.GetLatestSequence() == 100, "shared frame ring retains the latest publication sequence");
		CheckThrows<std::invalid_argument>(
			[&] { ring.Publish(4, 4, std::span<const std::uint8_t>(pixels.data(), pixels.size() - 1), 101); },
			"shared frame ring rejects impossible payload sizes"
		);
		ring.Close();
		CheckThrows<std::runtime_error>(
			[&] { ring.Publish(4, 4, pixels, 102); },
			"closed shared frame rings reject publication"
		);
	}

	void TestEditorHostProtocol() {
		using Json = nlohmann::ordered_json;
		using namespace gargantuan;
		const auto temporaryRoot = std::filesystem::temp_directory_path() /
			("gargantuan-editor-host-" + std::to_string(
				std::chrono::steady_clock::now().time_since_epoch().count()
			));
		struct TemporaryProjectCleanup {
			std::filesystem::path Root;
			~TemporaryProjectCleanup() { std::filesystem::remove_all(Root); }
		} cleanup{temporaryRoot};
		std::filesystem::create_directories(temporaryRoot / ".gargantuan");
		std::ofstream project(temporaryRoot / ".gargantuan" / "project.instance.json", std::ios::binary);
		project << R"({"Version":0,"Name":"EditorWorld","ClassName":"DataModel","Properties":{},"Children":[{"Name":"Editable","ClassName":"Folder","Properties":{},"Children":[]},{"Name":"Workspace","ClassName":"Workspace","Properties":{},"Children":[{"Name":"PickTarget","ClassName":"Part","Properties":{},"Children":[]}]}]})";
		project.close();

		EditorHost host("test-token");
		std::size_t requestNumber = 0;
		auto call = [&](std::string method, Json parameters, std::string token) {
			Json request{
				{"Version", EditorHostProtocolVersion},
				{"RequestId", std::to_string(++requestNumber)},
				{"SessionToken", std::move(token)},
				{"Method", std::move(method)},
				{"Params", std::move(parameters)},
			};
			return Json::parse(host.HandleRequest(request.dump()));
		};

		auto unauthorized = call("Handshake", Json::object(), "wrong-token");
		Check(!unauthorized["Ok"].get<bool>() && unauthorized["Error"]["Code"] == "Unauthorized", "EditorHost rejects the wrong launch token");
		auto handshake = call("Handshake", Json::object(), "test-token");
		Check(handshake["Ok"].get<bool>() && handshake["Result"]["ProtocolVersion"] == 1, "EditorHost negotiates protocol version 1");
		Check(
			handshake["Result"]["StudioExecutionDomain"] == "Studio" &&
				handshake["Result"]["StudioCapabilities"] == Json::array({
					"ReadDataModel", "MutateDataModel", "EditorCommands", "SelectionAccess", "ViewportControl"
				}),
			"EditorHost exposes the narrow Studio-domain capability grant"
		);
		EditorHost restrictedHost("restricted-token", {
			ScriptExecutionDomain::Studio,
			{ScriptCapability::ReadDataModel},
		});
		for (const auto method : {
			"OpenViewportTransport", "CloseViewportTransport", "ConfigureViewport",
			"SetViewportCamera", "CaptureViewport", "PickViewport",
		}) {
			Json restrictedRequest{
				{"Version", EditorHostProtocolVersion},
				{"RequestId", method},
				{"SessionToken", "restricted-token"},
				{"Method", method},
				{"Params", Json::object()},
			};
			auto restrictedViewport = Json::parse(restrictedHost.HandleRequest(restrictedRequest.dump()));
			Check(
				!restrictedViewport["Ok"].get<bool>() && restrictedViewport["Error"]["Code"] == "Unauthorized",
				"EditorHost enforces ViewportControl for every viewport method"
			);
		}
		auto sharedTransport = std::find_if(
			handshake["Result"]["ViewportTransports"].begin(),
			handshake["Result"]["ViewportTransports"].end(),
			[](const Json &transport) { return transport["Name"] == "SharedMemoryRing"; }
		);
		Check(
			SharedFrameRing::IsSupported()
				? sharedTransport != handshake["Result"]["ViewportTransports"].end()
				: sharedTransport == handshake["Result"]["ViewportTransports"].end(),
			"EditorHost advertises shared-memory viewport transport only when supported"
		);
		auto invalidTransport = call("OpenViewportTransport", {
			{"Transport", "SharedMemoryRing"}, {"Version", 999}, {"PixelFormat", "RGB8"}
		}, "test-token");
		Check(
			!invalidTransport["Ok"].get<bool>() &&
				invalidTransport["Error"]["Code"] == "UnsupportedViewportTransport",
			"EditorHost rejects unknown shared-memory transport versions"
		);
		if (SharedFrameRing::IsSupported()) {
			auto openedTransport = call("OpenViewportTransport", {
				{"Transport", "SharedMemoryRing"}, {"Version", 1}, {"PixelFormat", "RGB8"}
			}, "test-token");
			Check(
				openedTransport["Ok"].get<bool>() &&
					openedTransport["Result"]["SlotCount"] == SharedFrameRingLayout::SlotCount &&
					openedTransport["Result"]["MappingBytes"] == SharedFrameRingLayout::MappingBytes,
				"EditorHost opens the fixed-capacity shared-memory viewport ring"
			);
			auto closedTransport = call("CloseViewportTransport", Json::object(), "test-token");
			Check(closedTransport["Ok"].get<bool>(), "EditorHost closes the shared-memory viewport ring explicitly");
		}
		auto schema = call("GetSchema", Json::object(), "test-token");
		Check(schema["Ok"].get<bool>() && !schema["Result"]["Classes"].empty(), "EditorHost exposes reflected class schemas");

		auto opened = call("OpenProject", {{"Root", temporaryRoot.string()}}, "test-token");
		Check(opened["Ok"].get<bool>(), "EditorHost opens a project without starting the engine loop");
		auto snapshot = call("GetSnapshot", Json::object(), "test-token");
		Check(snapshot["Ok"].get<bool>(), "EditorHost returns a cursor-paired snapshot");
		auto &objects = snapshot["Result"]["Snapshot"]["Objects"];
		auto editable = std::find_if(objects.begin(), objects.end(), [](const Json &object) {
			return object["Name"] == "Editable";
		});
		Check(editable != objects.end(), "EditorHost snapshot contains the project hierarchy");
		if (editable != objects.end()) {
			auto mutation = call("SetProperty", {
				{"Object", (*editable)["Id"]},
				{"Property", "Name"},
				{"Value", {{"Type", "String"}, {"Value", "EditedThroughProtocol"}}},
			}, "test-token");
			Check(mutation["Ok"].get<bool>(), "EditorHost applies a typed property mutation through the gateway");
			auto changes = call("PollChanges", Json::object(), "test-token");
			Check(
				changes["Ok"].get<bool>() && changes["Result"]["Records"].size() == 1 &&
					changes["Result"]["Records"][0]["Operation"] == "PropertyUpdate",
				"EditorHost publishes the committed mutation as one journal record"
			);
		}

		auto captureBeforeConfiguration = call("CaptureViewport", Json::object(), "test-token");
		Check(
			!captureBeforeConfiguration["Ok"].get<bool>() &&
				captureBeforeConfiguration["Error"]["Code"] == "ViewportRequired",
			"viewport capture requires bounded configuration"
		);
		auto oversizedViewport = call("ConfigureViewport", {{"Width", 2048}, {"Height", 2048}}, "test-token");
		Check(
			!oversizedViewport["Ok"].get<bool>() && oversizedViewport["Error"]["Code"] == "ViewportTooLarge",
			"viewport configuration enforces dimension and pixel bounds"
		);
		auto configuredViewport = call("ConfigureViewport", {{"Width", 320}, {"Height", 200}}, "test-token");
		Check(
			configuredViewport["Ok"].get<bool>() && configuredViewport["Result"]["Format"] == "RGB8",
			"EditorHost negotiates a bounded RGB viewport surface"
		);
		auto invalidCamera = call("SetViewportCamera", {
			{"Position", {0.0, 0.0, 10.0}}, {"Target", {0.0, 0.0, 10.0}}
		}, "test-token");
		Check(
			!invalidCamera["Ok"].get<bool>() && invalidCamera["Error"]["Code"] == "InvalidCamera",
			"viewport camera rejects coincident position and target"
		);
		auto camera = call("SetViewportCamera", {
			{"Position", {0.0, 0.0, 10.0}}, {"Target", {0.0, 0.0, 0.0}}, {"FieldOfView", 70.0}
		}, "test-token");
		Check(camera["Ok"].get<bool>(), "EditorHost applies an absolute viewport camera command");
		auto cameraChanges = call("PollChanges", Json::object(), "test-token");
		Check(
			cameraChanges["Ok"].get<bool>() && cameraChanges["Result"]["Records"].empty(),
			"viewport camera session state does not enter the document journal"
		);
		auto picked = call("PickViewport", {{"X", 159.5}, {"Y", 99.5}}, "test-token");
		auto pickTarget = std::find_if(objects.begin(), objects.end(), [](const Json &object) {
			return object["Name"] == "PickTarget";
		});
		Check(
			picked["Ok"].get<bool>() && pickTarget != objects.end() &&
				picked["Result"]["Object"] == (*pickTarget)["Id"],
			"viewport picking returns the selected Part ObjectId"
		);

		auto malformed = Json::parse(host.HandleRequest(std::string(EditorHostMaximumRequestBytes + 1, 'x')));
		Check(!malformed["Ok"].get<bool>() && malformed["Error"]["Code"] == "MalformedRequest", "EditorHost rejects oversized direct requests before parsing");
	}

	void TestLuauExceptionBoundary() {
		lua_State *L = luaL_newstate();
		lua_pushcfunction(L, [](lua_State *state) {
			return gargantuan::InvokeNativeCallback(state, []() -> int { throw std::runtime_error("native failure"); });
		}, "throwing_native");
		int status = lua_pcall(L, 0, 0, 0);
		Check(status != LUA_OK, "native exception becomes a Luau error");
		Check(std::string(lua_tostring(L, -1)) == "native failure", "Luau error preserves native diagnostic");
		lua_pop(L, 1);
		lua_pushcfunction(L, [](lua_State *state) {
			return gargantuan::InvokeNativeCallback(state, [state]() -> int {
				luaL_error(state, "direct Luau error");
				return 0;
			});
		}, "direct_luau_error");
		status = lua_pcall(L, 0, 0, 0);
		Check(status != LUA_OK, "existing Luau errors pass through the native guard");
		Check(std::string(lua_tostring(L, -1)) == "direct Luau error", "native guard preserves Luau diagnostics");
		lua_close(L);
	}
}

int main() {
	TestHierarchyAndDestruction();
	TestObjectIdsAndChanges();
	TestWorldRootConstraintValidation();
	TestCheckedResolutionAndOwnedPaths();
	TestJobSystem();
	TestSchemaMetadata();
	TestRuntimeSchemaRegistry();
	TestScriptSecurityModel();
	TestMutationGateway();
	TestBoundedJournalCursor();
	TestSnapshotBaseline();
	TestWireJournalAndLoopbackReplication();
	TestSharedFrameRing();
	TestEditorHostProtocol();
	TestLuauExceptionBoundary();
	if (Failures == 0) std::cout << "All foundation tests passed\n";
	return Failures == 0 ? 0 : 1;
}
