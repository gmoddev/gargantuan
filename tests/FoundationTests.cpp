#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/Frame.hpp"
#include "gargantuan/classes/ModuleScript.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/WeldConstraint.hpp"
#include "gargantuan/editor/EditorHost.hpp"
#include "gargantuan/editor/EditorViewport.hpp"
#include "gargantuan/render/RenderExtractor.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"
#include "gargantuan/runtime/AttributeValidation.hpp"
#include "gargantuan/runtime/DataModelRoot.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"
#include "gargantuan/runtime/JobSystem.hpp"
#include "gargantuan/runtime/InProcessReplicationSession.hpp"
#include "gargantuan/runtime/MutationGateway.hpp"
#include "gargantuan/runtime/ObjectId.hpp"
#include "gargantuan/runtime/Snapshot.hpp"
#include "gargantuan/runtime/TagIndex.hpp"
#include "gargantuan/runtime/WireCodec.hpp"
#include "gargantuan/runtime/WireJournal.hpp"
#include "gargantuan/reflection/PreRunRegistration.hpp"
#include "gargantuan/reflection/RuntimeSchema.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/scripting/ModuleResolution.hpp"
#include "gargantuan/scripting/NativeCallback.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <lua.h>
#include <lualib.h>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <sstream>
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
	template <typename T>
	concept ExposesGpuMesh = requires(T &value) { value.GetMesh(); };
	static_assert(!ExposesGpuMesh<gargantuan::Part>);
	template <typename T>
	concept ExposesMutableAttributeStorage = requires(T &value) { value.Attributes; };
	static_assert(!ExposesMutableAttributeStorage<gargantuan::Instance>);
	static_assert(std::is_same_v<
		gargantuan::RenderSnapshotPtr,
		std::shared_ptr<const gargantuan::RenderSnapshot>
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

	gargantuan::SchemaClassDefinition MakeSchemaDefinition(
		std::string name,
		std::optional<std::string> baseName = std::nullopt,
		std::optional<gargantuan::SchemaId> id = std::nullopt
	) {
		using namespace gargantuan;
		SchemaClassDefinition definition;
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

	gargantuan::SchemaEnumDefinition MakeSchemaEnumDefinition(
		std::string schemaNamespace,
		std::string name,
		std::uint32_t version = 1,
		std::vector<gargantuan::SchemaEnumItem> items = {
			{"Idle", 0}, {"Attacking", 1}, {"Blocking", 2},
		}
	) {
		using namespace gargantuan;
		SchemaEnumDefinition definition;
		definition.Id = SchemaId::FromEnumName(schemaNamespace, name);
		definition.Namespace = std::move(schemaNamespace);
		definition.Name = std::move(name);
		definition.DefinitionVersion = version;
		definition.Provenance = SchemaProvenance::Game;
		definition.OriginDetail = ".gargantuan/prerun.luau";
		definition.Items = std::move(items);
		return definition;
	}

	gargantuan::InstanceProperty MakeReadOnlySchemaProperty(std::string name, std::string type = "string") {
		gargantuan::InstanceProperty property(std::move(name));
		property.SetReflectedTypedef(std::move(type)).SetEditable(false);
		property.Read = [](gargantuan::Instance *) -> std::any { return std::string("value"); };
		return property;
	}

	void FreezeSchemaRegistry(gargantuan::RuntimeSchemaRegistry &registry) {
		registry.Validate();
		registry.Freeze();
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
		FreezeSchemaRegistry(firstConstruction);
		FreezeSchemaRegistry(secondConstruction);
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
		FreezeSchemaRegistry(orderedOne);
		FreezeSchemaRegistry(orderedTwo);
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

	void TestRuntimeSchemaLifecycle() {
		using namespace gargantuan;
		const auto &authority = GetRuntimeSchemaBootstrapAuthority();

		static_assert(std::is_same_v<
			decltype(std::declval<const RuntimeSchemaRegistry &>().FindByName(std::string_view{})),
			const SchemaClassDefinition *
		>);

		RuntimeSchemaRegistry directRegistry;
		directRegistry.RegisterNative<SchemaTestTypeA>(MakeSchemaDefinition("Direct"));
		CheckThrows<std::logic_error>(
			[&] { static_cast<void>(directRegistry.FindByName("Test.Direct")); },
			"candidate definitions are not observable before freeze"
		);
		directRegistry.Validate();
		CheckThrows<std::logic_error>(
			[&] { static_cast<void>(directRegistry.FindByName("Test.Direct")); },
			"validated candidate definitions remain hidden before freeze"
		);
		directRegistry.Freeze();
		const auto *stableDefinition = directRegistry.FindByName("Test.Direct");
		Check(stableDefinition != nullptr, "frozen registry lookup succeeds");
		Check(
			stableDefinition == directRegistry.FindByName("Test.Direct"),
			"frozen compatibility definitions remain stable"
		);
		CheckThrows<std::logic_error>([&] { directRegistry.Freeze(); }, "a frozen registry cannot freeze twice");
		CheckThrows<std::logic_error>(
			[&] { directRegistry.RegisterNative<SchemaTestTypeB>(MakeSchemaDefinition("Late")); },
			"post-freeze registration is rejected"
		);

		RuntimeSchemaLifecycle invalidTransition;
		invalidTransition.BeginCandidate(authority);
		CheckThrows<std::logic_error>(
			[&] {
				invalidTransition.AdvanceRegistrationPhase(
					authority, RuntimeSchemaLifecyclePhase::PreRunRegistration
				);
			},
			"runtime schema lifecycle rejects phase jumps"
		);
		invalidTransition.AdvanceRegistrationPhase(authority, RuntimeSchemaLifecyclePhase::CoreRegistration);
		CheckThrows<std::logic_error>(
			[&] {
				invalidTransition.RegisterNative<SchemaTestTypeA>(authority, MakeSchemaDefinition("OutsideNative"));
			},
			"native registration outside its phase is rejected"
		);
		invalidTransition.AbortCandidate(authority);

		RuntimeSchemaLifecycle freezeBeforeValidation;
		freezeBeforeValidation.BeginCandidate(authority);
		freezeBeforeValidation.RegisterNative<SchemaTestTypeA>(authority, MakeSchemaDefinition("Unvalidated"));
		freezeBeforeValidation.AdvanceRegistrationPhase(authority, RuntimeSchemaLifecyclePhase::CoreRegistration);
		freezeBeforeValidation.AdvanceRegistrationPhase(authority, RuntimeSchemaLifecyclePhase::PreRunRegistration);
		freezeBeforeValidation.AdvanceRegistrationPhase(authority, RuntimeSchemaLifecyclePhase::Validation);
		CheckThrows<std::runtime_error>(
			[&] { freezeBeforeValidation.FreezeCandidate(authority); },
			"freeze before whole-candidate validation is rejected"
		);
		Check(
			freezeBeforeValidation.GetPhase() == RuntimeSchemaLifecyclePhase::Bootstrap &&
				!freezeBeforeValidation.HasCandidate(),
			"failed freeze discards the unpublished candidate"
		);

		auto PublishSingle = [&](RuntimeSchemaLifecycle &lifecycle, std::string name, std::uint32_t version) {
			lifecycle.BeginCandidate(authority);
			auto definition = MakeSchemaDefinition(std::move(name));
			definition.DefinitionVersion = version;
			lifecycle.RegisterNative<SchemaTestTypeA>(authority, std::move(definition));
			lifecycle.AdvanceRegistrationPhase(authority, RuntimeSchemaLifecyclePhase::CoreRegistration);
			lifecycle.AdvanceRegistrationPhase(authority, RuntimeSchemaLifecyclePhase::PreRunRegistration);
			lifecycle.AdvanceRegistrationPhase(authority, RuntimeSchemaLifecyclePhase::Validation);
			lifecycle.ValidateCandidate(authority);
			lifecycle.FreezeCandidate(authority);
			lifecycle.PublishCandidate(authority);
		};

		RuntimeSchemaLifecycle lifecycle;
		Check(
			lifecycle.GetActiveGeneration() == InvalidRuntimeSchemaGeneration && !lifecycle.HasActiveRegistry(),
			"unpublished schema lifecycle starts with invalid generation zero"
		);
		PublishSingle(lifecycle, "PublishedOne", 7);
		const auto firstActive = lifecycle.GetActiveRegistry();
		const auto firstGeneration = lifecycle.GetActiveGeneration();
		Check(
			lifecycle.GetPhase() == RuntimeSchemaLifecyclePhase::Runtime && firstActive->IsFrozen(),
			"valid bootstrap publishes one frozen active registry"
		);
		Check(firstGeneration == 1, "first successful schema publication receives generation one");
		Check(
			firstActive->FindByName("Test.PublishedOne")->DefinitionVersion == 7 && firstGeneration != 7,
			"definition version remains independent from registry generation"
		);

		lifecycle.BeginCandidate(authority);
		lifecycle.RegisterNative<SchemaTestTypeB>(authority, MakeSchemaDefinition("Broken", "Missing"));
		lifecycle.AdvanceRegistrationPhase(authority, RuntimeSchemaLifecyclePhase::CoreRegistration);
		lifecycle.AdvanceRegistrationPhase(authority, RuntimeSchemaLifecyclePhase::PreRunRegistration);
		lifecycle.AdvanceRegistrationPhase(authority, RuntimeSchemaLifecyclePhase::Validation);
		CheckThrows<std::runtime_error>(
			[&] { lifecycle.ValidateCandidate(authority); },
			"one invalid candidate definition aborts publication"
		);
		Check(
			lifecycle.GetActiveRegistry() == firstActive && lifecycle.GetActiveGeneration() == firstGeneration,
			"candidate failure preserves the previous active registry and generation"
		);
		Check(
			firstActive->FindByName("Test.Broken") == nullptr,
			"candidate failure leaks no definitions into the active registry"
		);

		PublishSingle(lifecycle, "PublishedTwo", 1);
		Check(
			lifecycle.GetActiveGeneration() == firstGeneration + 1 &&
				lifecycle.GetActiveRegistry()->FindByName("Test.PublishedTwo") != nullptr,
			"a successful complete replacement increments generation"
		);

		Check(
			GetRuntimeSchemaLifecycle().GetPhase() == RuntimeSchemaLifecyclePhase::Runtime &&
				GetRuntimeSchemaLifecycle().GetActiveGeneration() != InvalidRuntimeSchemaGeneration,
			"native bootstrap reaches the frozen runtime phase with a valid generation"
		);
		CheckThrows<std::logic_error>(
			[] { BootstrapNativeRuntimeSchema(); },
			"published native runtime schema cannot be reopened by bootstrap"
		);
	}

	void TestCustomEnumPreRun() {
		using namespace gargantuan;
		const auto &authority = GetRuntimeSchemaBootstrapAuthority();
		const auto enumId = SchemaId::FromEnumName("Game", "CombatState");
		Check(enumId == SchemaId::FromEnumName("Game", "CombatState"), "custom enum SchemaId is deterministic");
		Check(enumId != SchemaId::FromEnumName("Game", "MovementState"), "custom enum names domain-separate identity");
		Check(enumId != SchemaId::FromEnumName("Tools", "CombatState"), "custom enum namespaces domain-separate identity");
		Check(enumId != SchemaId::FromNativeName("Game", "CombatState"), "class and enum SchemaIds use distinct domains");
		Check(
			WireSchemaEnumValue{enumId, 1, 1} !=
				WireSchemaEnumValue{SchemaId::FromEnumName("Game", "MovementState"), 1, 1},
			"enum item identity includes the owning enum SchemaId"
		);

		RuntimeSchemaRegistry registry;
		registry.RegisterNative<SchemaTestTypeA>(MakeSchemaDefinition("Host"));
		registry.RegisterEnum(MakeSchemaEnumDefinition("Game", "CombatState"));
		registry.Validate();
		registry.Freeze();
		const auto *customEnum = registry.FindEnumById(enumId);
		Check(
			customEnum && customEnum->CanonicalName == "Game.CombatState" &&
				customEnum->DefinitionVersion == 1 && customEnum->Provenance == SchemaProvenance::Game,
			"canonical registry exposes frozen custom enum identity, version, and provenance"
		);
		Check(
			customEnum && customEnum->Items == std::vector<SchemaEnumItem>({
				{"Attacking", 1}, {"Blocking", 2}, {"Idle", 0},
			}),
			"custom enum item enumeration is deterministic"
		);
		Check(registry.FindClassById(enumId) == nullptr, "typed class lookup rejects enum definitions");
		Check(registry.FindEnumById(SchemaId::FromNativeName("Test", "Host")) == nullptr,
			"typed enum lookup rejects class definitions");

		RuntimeSchemaRegistry registrationOrder;
		registrationOrder.RegisterEnum(MakeSchemaEnumDefinition("Game", "CombatState"));
		registrationOrder.RegisterNative<SchemaTestTypeB>(MakeSchemaDefinition("Host"));
		registrationOrder.Validate();
		registrationOrder.Freeze();
		auto names = [](const RuntimeSchemaRegistry &source) {
			std::vector<std::string> result;
			for (const auto *definition : source.EnumerateDefinitions())
				result.emplace_back(GetSchemaDefinitionCanonicalName(*definition));
			return result;
		};
		Check(names(registry) == names(registrationOrder), "schema enumeration is independent of registration order");

		RuntimeSchemaRegistry canonicalCollision;
		auto collidingClass = MakeSchemaDefinition("CombatState");
		collidingClass.Namespace = "Game";
		collidingClass.Id = SchemaId::FromNativeName("Game", "CombatState");
		canonicalCollision.RegisterNative<SchemaTestTypeA>(std::move(collidingClass));
		CheckThrows<std::invalid_argument>(
			[&] { canonicalCollision.RegisterEnum(MakeSchemaEnumDefinition("Game", "CombatState")); },
			"class and enum canonical-name collision is rejected rather than load-order resolved"
		);
		RuntimeSchemaRegistry itemCollision;
		CheckThrows<std::invalid_argument>(
			[&] { itemCollision.RegisterEnum(MakeSchemaEnumDefinition("Game", "Alias", 1, {{"A", 1}, {"B", 1}})); },
			"custom enum numeric aliases are rejected"
		);
		RuntimeSchemaRegistry invalidUtf8;
		auto malformedName = MakeSchemaEnumDefinition("Game", "Malformed");
		malformedName.Items[0].Name = std::string("\xc0", 1);
		CheckThrows<std::invalid_argument>(
			[&] { invalidUtf8.RegisterEnum(std::move(malformedName)); },
			"custom enum registration rejects malformed UTF-8"
		);

		const WireSchemaEnumValue value{enumId, 1, 1};
		const auto encoded = EncodeWireValue(WireValue(value));
		const auto decoded = DecodeWireValue(encoded);
		Check(
			decoded && std::get_if<WireSchemaEnumValue>(&*decoded) &&
				*std::get_if<WireSchemaEnumValue>(&*decoded) == value,
			"custom enum value round-trips with stable schema identity and definition version"
		);
		auto malformedWire = encoded;
		malformedWire["SchemaId"] = "not-a-schema-id";
		Check(!DecodeWireValue(malformedWire), "malformed custom enum wire identity is rejected");
		Check(FormatSchemaEnumValue(value, registry) == "Game.CombatState.Attacking",
			"custom enum values format deterministically through the frozen registry");
		WireJournalRecord replicatedValue{
			.Version = WireJournalFormatVersion,
			.Sequence = 1,
			.Scope = {1, 1},
			.Operation = WireJournalOperation::PropertyUpdate,
			.Object = {2, 1},
			.PropertyName = "State",
			.Value = WireValue(value),
		};
		const auto replicatedRoundTrip = DeserializeWireJournalRecords(
			SerializeWireJournalRecords({replicatedValue})
		);
		Check(
			replicatedRoundTrip.Succeeded() && replicatedRoundTrip.Value->size() == 1 &&
				std::get<WireSchemaEnumValue>(*replicatedRoundTrip.Value->front().Value) == value,
			"loopback journal wire transport retains typed custom enum identity"
		);
		if (replicatedRoundTrip.Succeeded())
			static_cast<void>(ValidateSchemaEnumValue(
				std::get<WireSchemaEnumValue>(*replicatedRoundTrip.Value->front().Value), registry
			));
		CheckThrows<std::invalid_argument>(
			[&] { static_cast<void>(ValidateSchemaEnumValue({enumId, 2, 1}, registry)); },
			"custom enum materialization fails closed on definition-version mismatch"
		);
		CheckThrows<std::invalid_argument>(
			[&] { static_cast<void>(ValidateSchemaEnumValue({SchemaId::FromParts(99, 99), 1, 1}, registry)); },
			"custom enum materialization fails closed on missing definitions"
		);
		CheckThrows<std::invalid_argument>(
			[&] { static_cast<void>(ValidateSchemaEnumValue({SchemaId::FromNativeName("Test", "Host"), 1, 1}, registry)); },
			"custom enum materialization fails closed on wrong-kind definitions"
		);
		CheckThrows<std::invalid_argument>(
			[&] { static_cast<void>(ValidateSchemaEnumValue({enumId, 1, 999}, registry)); },
			"custom enum materialization fails closed on unknown items"
		);
		CheckThrows<std::invalid_argument>(
			[&] { static_cast<void>(ValidateAttributeValue(WireValue(value))); },
			"enum-valued Attributes remain explicitly unsupported"
		);

		auto PreparePreRun = [&](RuntimeSchemaLifecycle &lifecycle) {
			lifecycle.BeginCandidate(authority);
			lifecycle.RegisterNative<SchemaTestTypeA>(authority, MakeSchemaDefinition("PreRunHost"));
			lifecycle.AdvanceRegistrationPhase(authority, RuntimeSchemaLifecyclePhase::CoreRegistration);
			lifecycle.AdvanceRegistrationPhase(authority, RuntimeSchemaLifecyclePhase::PreRunRegistration);
		};
		const std::string validSource = R"(
			assert(os == nil and io == nil and debug == nil and require == nil)
			Schema:RegisterEnum({
				Namespace = "Game",
				Name = "CombatState",
				Version = 1,
				Items = { Idle = 0, Attacking = 1, Blocking = 2 },
			})
		)";
		RuntimeSchemaLifecycle successful;
		PreparePreRun(successful);
		ExecutePreRunRegistration(successful, authority, validSource, "valid-prerun");
		successful.AdvanceRegistrationPhase(authority, RuntimeSchemaLifecyclePhase::Validation);
		successful.ValidateCandidate(authority);
		successful.FreezeCandidate(authority);
		successful.PublishCandidate(authority);
		Check(
			successful.GetActiveRegistry()->FindEnumByName("Game.CombatState") != nullptr &&
				successful.GetActiveGeneration() == 1,
			"bounded authorized PreRun registers and atomically publishes a custom enum"
		);
		Check(
			!ScriptSecurityContext::PreRunRegistration().HasCapability(ScriptCapability::MutateDataModel),
			"PreRun definition capability does not grant DataModel mutation authority"
		);
		CheckThrows<std::logic_error>(
			[&] { successful.RegisterEnum(authority, MakeSchemaEnumDefinition("Game", "Late"), ScriptSecurityContext::PreRunRegistration()); },
			"post-freeze schema registration fails even with DefineSchema capability"
		);

		for (const auto domain : {ScriptExecutionDomain::PreRun, ScriptExecutionDomain::Core,
			ScriptExecutionDomain::Studio, ScriptExecutionDomain::Server, ScriptExecutionDomain::Client}) {
			RuntimeSchemaLifecycle denied;
			PreparePreRun(denied);
			CheckThrows<PreRunRegistrationError>(
				[&] { ExecutePreRunRegistration(denied, authority, validSource, "unauthorized-prerun", {domain, {}}); },
				"execution domain without DefineSchema cannot register custom schema"
			);
			Check(!denied.HasCandidate() && !denied.HasActiveRegistry(),
				"unauthorized PreRun aborts the complete hidden candidate");
		}

		RuntimeSchemaLifecycle runaway;
		PreparePreRun(runaway);
		try {
			ExecutePreRunRegistration(runaway, authority, "while true do end", "runaway-prerun");
			Check(false, "runaway PreRun is interrupted");
		} catch (const PreRunRegistrationError &error) {
			Check(error.GetDiagnostic().Code == PreRunDiagnosticCode::ExecutionBudgetExceeded,
				"runaway PreRun reports a structured execution-budget diagnostic");
		}
		Check(!runaway.HasCandidate(), "runaway PreRun publishes no candidate state");

		RuntimeSchemaLifecycle allocation;
		PreparePreRun(allocation);
		try {
			ExecutePreRunRegistration(
				allocation, authority,
				"local value = string.rep('x', 20000000)",
				"allocation-prerun"
			);
			Check(false, "excessive PreRun allocation is rejected");
		} catch (const PreRunRegistrationError &error) {
			Check(error.GetDiagnostic().Code == PreRunDiagnosticCode::MemoryBudgetExceeded,
				"excessive PreRun allocation reports a structured memory-budget diagnostic");
		}
		Check(!allocation.HasCandidate(), "allocation failure publishes no candidate state");

		RuntimeSchemaLifecycle sourceLimit;
		PreparePreRun(sourceLimit);
		try {
			ExecutePreRunRegistration(
				sourceLimit, authority, std::string(MaximumPreRunSourceBytes + 1, ' '), "oversized-prerun"
			);
			Check(false, "oversized PreRun source is rejected");
		} catch (const PreRunRegistrationError &error) {
			Check(error.GetDiagnostic().Code == PreRunDiagnosticCode::SourceTooLarge,
				"oversized PreRun source reports a structured source-limit diagnostic");
		}

		std::string tooManyDefinitions;
		for (std::size_t index = 0; index <= MaximumCustomEnumDefinitions; ++index) {
			tooManyDefinitions += "Schema:RegisterEnum({Namespace='Game',Name='Limit" +
				std::to_string(index) + "',Version=1,Items={Value=0}})\n";
		}
		RuntimeSchemaLifecycle definitionLimit;
		PreparePreRun(definitionLimit);
		CheckThrows<PreRunRegistrationError>(
			[&] { ExecutePreRunRegistration(definitionLimit, authority, tooManyDefinitions, "definition-limit"); },
			"PreRun definition-count overflow aborts registration"
		);
		Check(!definitionLimit.HasCandidate(), "definition-count overflow leaks no earlier definitions");

		std::string tooManyItems = "local items = {}\n";
		tooManyItems += "for index = 0, " + std::to_string(MaximumCustomEnumItems) +
			" do items['Item' .. index] = index end\n";
		tooManyItems += "Schema:RegisterEnum({Namespace='Game',Name='TooManyItems',Version=1,Items=items})";
		RuntimeSchemaLifecycle itemLimit;
		PreparePreRun(itemLimit);
		CheckThrows<PreRunRegistrationError>(
			[&] { ExecutePreRunRegistration(itemLimit, authority, tooManyItems, "item-limit"); },
			"PreRun enum-item overflow aborts registration"
		);
		Check(!itemLimit.HasCandidate(), "enum-item overflow publishes no partial definition");

		RuntimeSchemaLifecycle transactional;
		PreparePreRun(transactional);
		ExecutePreRunRegistration(transactional, authority, validSource, "initial-prerun");
		transactional.AdvanceRegistrationPhase(authority, RuntimeSchemaLifecyclePhase::Validation);
		transactional.ValidateCandidate(authority);
		transactional.FreezeCandidate(authority);
		transactional.PublishCandidate(authority);
		const auto previous = transactional.GetActiveRegistry();
		const auto previousGeneration = transactional.GetActiveGeneration();
		PreparePreRun(transactional);
		const std::string invalidBatch = R"(
			Schema:RegisterEnum({ Namespace="Game", Name="A", Version=1, Items={ One=1 } })
			Schema:RegisterEnum({ Namespace="Game", Name="B", Version=1, Items={ Two=2 } })
			Schema:RegisterEnum({ Namespace="Game", Name="C", Version=1, Items={ X=3, Y=3 } })
		)";
		CheckThrows<PreRunRegistrationError>(
			[&] { ExecutePreRunRegistration(transactional, authority, invalidBatch, "invalid-batch"); },
			"one malformed custom enum aborts the complete PreRun transaction"
		);
		Check(
			transactional.GetActiveRegistry() == previous &&
				transactional.GetActiveGeneration() == previousGeneration &&
				previous->FindEnumByName("Game.A") == nullptr && previous->FindEnumByName("Game.B") == nullptr,
			"failed replacement leaks no definitions and does not advance registry generation"
		);
	}

	void TestRenderSnapshotExtraction() {
		using namespace gargantuan;
		auto game = std::make_shared<DataModel>();
		auto workspace = std::dynamic_pointer_cast<Workspace>(game->GetService("Workspace"));
		Check(workspace != nullptr, "render extraction fixture obtains Workspace");

		auto primary = std::make_shared<Part>();
		primary->SetName("PrimaryRenderPart");
		primary->SetSize({2.0f, 4.0f, 6.0f});
		primary->SetColor(Color3(0.25f, 0.5f, 0.75f));
		primary->SetTransparency(0.2f);
		primary->SetShape(Enums::PartType::Wedge);
		primary->SetCFrame(CFrame(glm::vec3(0.0f, 0.0f, 0.0f)));
		primary->SetParent(workspace);
		const auto primaryId = primary->GetObjectId();

		auto secondary = std::make_shared<Part>();
		secondary->SetName("SecondaryRenderPart");
		secondary->SetCFrame(CFrame(glm::vec3(5.0f, 0.0f, 0.0f)));
		secondary->SetParent(workspace);
		const auto secondaryId = secondary->GetObjectId();

		auto camera = std::make_shared<Camera>();
		camera->SetCFrame(CFrame::lookAt({0.0f, 0.0f, 10.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}));
		camera->SetFieldOfView(60.0f);

		RenderExtractor extractor;
		auto invalidCamera = MakeRenderCameraInput(*camera);
		invalidCamera.LookDirection = {};
		CheckThrows<std::invalid_argument>(
			[&] { static_cast<void>(extractor.Extract(*workspace, invalidCamera, 320, 200)); },
			"invalid camera prevents RenderSnapshot publication"
		);
		Check(
			extractor.GetLastSnapshotId() == InvalidRenderSnapshotId,
			"failed extraction does not consume a RenderSnapshot identity"
		);
		CheckThrows<std::invalid_argument>(
			[&] { static_cast<void>(extractor.Extract(*workspace, MakeRenderCameraInput(*camera), 0, 200)); },
			"invalid viewport dimensions prevent RenderSnapshot publication"
		);

		auto first = extractor.Extract(*workspace, MakeRenderCameraInput(*camera), 320, 200);
		Check(
			first && first->Id == 1 && first->ViewportWidth == 320 && first->ViewportHeight == 200,
			"valid world produces a complete first RenderSnapshot"
		);
		Check(
			first->Camera.Position == glm::vec3(0.0f, 0.0f, 10.0f) &&
				std::abs(first->Camera.VerticalFieldOfView - 60.0f) < 1e-5f,
			"camera values are extracted into owned snapshot state"
		);
		Check(first->Items.size() == 2, "all valid primitive render items are extracted");
		Check(
			std::ranges::is_sorted(first->Items, {}, &RenderItem::Object),
			"RenderSnapshot items have deterministic ObjectId ordering"
		);
		auto primaryItem = std::ranges::find(first->Items, primaryId, &RenderItem::Object);
		Check(
			primaryItem != first->Items.end() && primaryItem->Geometry == RenderGeometry::Wedge &&
				primaryItem->Object == primaryId && primaryItem->Color == glm::vec4(0.25f, 0.5f, 0.75f, 0.8f),
			"primitive geometry, visual state, and stable picking identity are extracted"
		);
		const auto firstPrimaryModel = primaryItem->ModelMatrix;
		const auto firstCameraPosition = first->Camera.Position;

		primary->SetCFrame(CFrame(glm::vec3(2.0f, 3.0f, 4.0f)));
		primary->SetColor(Color3(1.0f, 0.0f, 0.0f));
		camera->SetCFrame(CFrame::lookAt({0.0f, 5.0f, 15.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}));
		Check(
			primaryItem->ModelMatrix == firstPrimaryModel && primaryItem->Color == glm::vec4(0.25f, 0.5f, 0.75f, 0.8f) &&
				first->Camera.Position == firstCameraPosition,
			"runtime and camera mutation cannot alter an already published RenderSnapshot"
		);

		auto second = extractor.Extract(*workspace, MakeRenderCameraInput(*camera), 320, 200);
		Check(second->Id == 2, "successful RenderSnapshot extraction increments frame identity");
		auto updatedPrimary = std::ranges::find(second->Items, primaryId, &RenderItem::Object);
		Check(
			updatedPrimary != second->Items.end() && updatedPrimary->ModelMatrix != firstPrimaryModel &&
				second->Camera.Position != firstCameraPosition,
			"a later snapshot observes newly committed runtime state"
		);

		primary->SetCFrame(CFrame(glm::vec3(0.0f, 0.0f, 0.0f)));
		auto visibleSnapshot = extractor.Extract(
			*workspace,
			MakeLookAtRenderCameraInput({0.0f, 0.0f, 10.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, 60.0f),
			320,
			200
		);
		auto pick = PickEditorViewport(*visibleSnapshot, 159.5f, 99.5f);
		Check(pick && pick->Object == primaryId, "snapshot picking returns the stable ObjectId for the visible item");

		primary->Destroy();
		Check(!ObjectRegistry::Get().Lookup(primaryId), "destroyed render item identity is stale in the ObjectRegistry");
		Check(
			std::ranges::find(visibleSnapshot->Items, primaryId, &RenderItem::Object) != visibleSnapshot->Items.end(),
			"destroying an Instance does not invalidate an in-flight RenderSnapshot"
		);
		pick = PickEditorViewport(*visibleSnapshot, 159.5f, 99.5f);
		Check(
			pick && pick->Object == primaryId,
			"the displayed snapshot retains its frame-local picking identity after runtime destruction"
		);
		auto afterDestroy = extractor.Extract(*workspace, MakeRenderCameraInput(*camera), 320, 200);
		Check(
			std::ranges::find(afterDestroy->Items, primaryId, &RenderItem::Object) == afterDestroy->Items.end(),
			"a newly extracted snapshot excludes destroyed objects"
		);

		secondary->SetSize({0.0f, 1.0f, 1.0f});
		auto invalidItem = extractor.Extract(*workspace, MakeRenderCameraInput(*camera), 320, 200);
		Check(
			std::ranges::find(invalidItem->Items, secondaryId, &RenderItem::Object) == invalidItem->Items.end() &&
				!invalidItem->Diagnostics.empty(),
			"singular item transforms are rejected with an explicit extraction diagnostic"
		);

		const auto identityBeforeWorkerAttempt = extractor.GetLastSnapshotId();
		{
			ExecutionDomainScope worker(ExecutionDomain::Worker);
			CheckThrows<std::logic_error>(
				[&] { static_cast<void>(extractor.Extract(*workspace, MakeRenderCameraInput(*camera), 320, 200)); },
				"RenderSnapshot extraction is confined to the authoritative Main domain"
			);
		}
		Check(
			extractor.GetLastSnapshotId() == identityBeforeWorkerAttempt,
			"rejected off-domain extraction does not publish a snapshot"
		);
	}

	void TestScriptSecurityModel() {
		using namespace gargantuan;
		Check(GetScriptExecutionDomainName(ScriptExecutionDomain::Core) == "Core", "Core script domain is represented");
		Check(GetScriptExecutionDomainName(ScriptExecutionDomain::PreRun) == "PreRun", "PreRun script domain is represented");
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
			!coreWithoutCapabilities.HasCapability(ScriptCapability::DefineSchema) &&
				ScriptSecurityContext::PreRunRegistration().HasCapability(ScriptCapability::DefineSchema),
			"schema definition authority is an explicit capability rather than a domain rank"
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

	void TestInstanceAttributes() {
		using namespace gargantuan;
		auto &journal = ChangeJournal::Get();
		journal.Clear();
		auto game = std::make_shared<DataModel>();
		game->SetArchivable(true);
		auto part = std::make_shared<Part>();
		part->SetName("AttributedPart");
		part->SetParent(game);
		const auto id = part->GetObjectId();
		const auto schemaGeneration = GetRuntimeSchemaLifecycle().GetActiveGeneration();
		auto *instanceSchema = InstanceClassRegistry::GetDefinitionByName("Instance");
		Check(instanceSchema && instanceSchema->AllMethods.contains("SetAttribute") &&
			instanceSchema->AllMethods.contains("GetAttribute") && instanceSchema->AllMethods.contains("GetAttributes") &&
			instanceSchema->AllMethods.contains("GetAttributeChangedSignal"),
			"frozen Instance schema describes attribute behavior without dynamic definitions");
		MutationGateway gateway;
		journal.Clear();
		auto attributeCursor = journal.CreateCursor(game->GetObjectId());
		auto AttributeRecordCount = [&] { return journal.Read(attributeCursor).Records.size(); };

		int signalCalls = 0;
		part->GetAttributeSignal("Health")->Connect([&](std::monostate) { ++signalCalls; });
		auto created = gateway.Apply(UpdateAttributeCommand{id, "Health", WireValue(100)});
		Check(created.Succeeded(), "attribute creation succeeds through MutationGateway");
		Check(part->GetAttributeValue("Health") == std::optional<WireValue>(100), "attribute read returns committed value");
		Check(part->GetAttributeValues().size() == 1, "attribute enumeration returns committed state");
		Check(signalCalls == 1 && AttributeRecordCount() == 1, "attribute creation signals and journals exactly once");

		auto updated = gateway.Apply(UpdateAttributeCommand{id, "Health", WireValue(75)});
		Check(updated.Succeeded() && part->GetAttributeValue("Health") == std::optional<WireValue>(75), "attribute update replaces its value");
		Check(signalCalls == 2 && AttributeRecordCount() == 2, "attribute update signals and journals exactly once");
		auto noOp = gateway.Apply(UpdateAttributeCommand{id, "Health", WireValue(75)});
		Check(noOp.Succeeded() && signalCalls == 2 && AttributeRecordCount() == 2, "identical attribute assignment is a true no-op");
		auto removed = gateway.Apply(UpdateAttributeCommand{id, "Health", std::nullopt});
		Check(removed.Succeeded() && !part->GetAttributeValue("Health"), "nil attribute mutation removes the value");
		Check(signalCalls == 3 && AttributeRecordCount() == 3, "attribute removal signals and journals exactly once");
		auto removeNoOp = gateway.Apply(UpdateAttributeCommand{id, "Health", std::nullopt});
		Check(removeNoOp.Succeeded() && signalCalls == 3 && AttributeRecordCount() == 3, "removing a missing attribute is a no-op");

		auto ExpectRejected = [&](std::string name, std::optional<WireValue> value, const char *message) {
			const auto before = part->GetAttributeValues();
			const auto records = AttributeRecordCount();
			auto result = gateway.Apply(UpdateAttributeCommand{id, std::move(name), std::move(value)});
			Check(!result.Succeeded() && part->GetAttributeValues() == before && AttributeRecordCount() == records &&
				signalCalls == 3, message);
		};
		ExpectRejected("", WireValue(true), "empty attribute name is rejected atomically");
		ExpectRejected(std::string(MaximumAttributeNameBytes + 1, 'a'), WireValue(true), "oversized attribute name is rejected");
		ExpectRejected(std::string("\xc3\x28", 2), WireValue(true), "malformed UTF-8 attribute name is rejected");
		ExpectRejected("Reference", WireValue(WireObjectReference{WireObjectId::FromObjectId(id)}), "unsupported reference attribute is rejected");
		ExpectRejected("Infinite", WireValue(std::numeric_limits<double>::infinity()), "non-finite attribute number is rejected");
		ExpectRejected("Large", WireValue(std::string(MaximumAttributeValueBytes + 1, 'x')), "oversized attribute value is rejected");

		auto signalBounded = std::make_shared<Folder>();
		for (std::size_t index = 0; index < MaximumAttributeSignalsPerInstance; ++index)
			(void)signalBounded->GetAttributeSignal("Signal" + std::to_string(index));
		CheckThrows<std::invalid_argument>(
			[&] { (void)signalBounded->GetAttributeSignal("SignalOverflow"); },
			"per-name attribute signal creation is bounded"
		);
		Check(signalBounded->GetAttributeSignal("Signal0") != nullptr,
			"existing attribute signals remain accessible at the creation limit");

		auto counted = std::make_shared<Folder>();
		const auto countedId = counted->GetObjectId();
		for (std::size_t index = 0; index < MaximumAttributesPerInstance; ++index)
			Check(gateway.Apply(UpdateAttributeCommand{countedId, "A" + std::to_string(index), WireValue(true)}).Succeeded(),
				"attributes up to the count limit are accepted");
		Check(!gateway.Apply(UpdateAttributeCommand{countedId, "Overflow", WireValue(true)}).Succeeded(),
			"attribute count limit is enforced");
		Check(counted->GetAttributeValues().size() == MaximumAttributesPerInstance, "count rejection preserves prior attributes");

		auto aggregate = std::make_shared<Folder>();
		const auto aggregateId = aggregate->GetObjectId();
		bool aggregateRejected = false;
		for (std::size_t index = 0; index < MaximumAttributesPerInstance; ++index) {
			auto result = gateway.Apply(UpdateAttributeCommand{
				aggregateId, "B" + std::to_string(index), WireValue(std::string(1024, 'b'))
			});
			if (!result.Succeeded()) { aggregateRejected = true; break; }
		}
		Check(aggregateRejected, "aggregate attribute byte limit is enforced");
		Check(ValidateAttributeCollection(aggregate->GetAttributeValues()) <= MaximumAttributeBytesPerInstance,
			"aggregate rejection preserves a valid collection");

		ScriptSecurityContext readOnly{ScriptExecutionDomain::Studio, {ScriptCapability::ReadDataModel}};
		ScriptSecurityContext writeOnly{ScriptExecutionDomain::Studio, {ScriptCapability::MutateDataModel}};
		Check(
			gateway.Apply(UpdateAttributeCommand{id, "Denied", WireValue(true)}, readOnly).Status == MutationStatus::Unauthorized,
			"attribute write requires MutateDataModel"
		);
		CheckThrows<std::runtime_error>([&] { (void)part->GetAttributeValues(writeOnly); }, "attribute read requires ReadDataModel");
		Check(GetRuntimeSchemaLifecycle().GetActiveGeneration() == schemaGeneration, "attribute mutation does not change frozen schema generation");

		Check(gateway.Apply(UpdateAttributeCommand{id, "Persisted", WireValue(std::string("value"))}).Succeeded(),
			"persistence test attribute is accepted");
		std::shared_ptr<Instance> persistenceRoot = game;
		auto serialized = InstanceSerialization::Serialize(InstanceSerialization::InstanceFormat::Json, persistenceRoot);
		Check(serialized == InstanceSerialization::Serialize(InstanceSerialization::InstanceFormat::Json, persistenceRoot),
			"attribute persistence output is deterministic");
		std::istringstream serializedInput(serialized);
		auto deserialized = InstanceSerialization::Deserialize(InstanceSerialization::InstanceFormat::Json, serializedInput);
		Check(deserialized.Ok, "attribute project persistence round-trips");
		auto deserializedPart = deserialized.Instance ? deserialized.Instance->FindFirstChild("AttributedPart", false) : nullptr;
		Check(deserializedPart && deserializedPart->GetAttributeValue("Persisted") == std::optional<WireValue>(std::string("value")),
			"deserialized Instance restores attributes");
		auto versionOne = nlohmann::ordered_json::parse(serialized);
		versionOne["Version"] = 1;
		std::function<void(nlohmann::ordered_json &)> RemoveTags = [&](nlohmann::ordered_json &node) {
			node.erase("Tags");
			for (auto &child : node["Children"]) RemoveTags(child);
		};
		RemoveTags(versionOne);
		std::istringstream versionOneInput(versionOne.dump());
		auto loadedVersionOne = InstanceSerialization::Deserialize(InstanceSerialization::InstanceFormat::Json, versionOneInput);
		auto versionOnePart = loadedVersionOne.Instance
			? loadedVersionOne.Instance->FindFirstChild("AttributedPart", false)
			: nullptr;
		Check(loadedVersionOne.Ok && versionOnePart && versionOnePart->GetAttributeValue("Persisted") ==
			std::optional<WireValue>(std::string("value")), "project version 1 retains attributes and defaults tags empty");
		auto versionZero = versionOne;
		versionZero["Version"] = 0;
		std::function<void(nlohmann::ordered_json &)> RemoveAttributes = [&](nlohmann::ordered_json &node) {
			node.erase("Attributes");
			for (auto &child : node["Children"]) RemoveAttributes(child);
		};
		RemoveAttributes(versionZero);
		std::istringstream versionZeroInput(versionZero.dump());
		auto loadedVersionZero = InstanceSerialization::Deserialize(InstanceSerialization::InstanceFormat::Json, versionZeroInput);
		auto versionZeroPart = loadedVersionZero.Instance
			? loadedVersionZero.Instance->FindFirstChild("AttributedPart", false)
			: nullptr;
		Check(loadedVersionZero.Ok && versionZeroPart && versionZeroPart->GetAttributeValues().empty(),
			"project version 0 deterministically defaults attributes and tags empty");
		auto malformedDocument = nlohmann::ordered_json::parse(serialized);
		for (auto &child : malformedDocument["Children"]) {
			if (child["Name"] == "AttributedPart") child["Attributes"]["Bad"] = {{"Type", "Double"}, {"Value", "not-number"}};
		}
		std::istringstream malformedInput(malformedDocument.dump());
		Check(!InstanceSerialization::Deserialize(InstanceSerialization::InstanceFormat::Json, malformedInput).Ok,
			"malformed persisted attribute data is rejected");

		auto snapshot = CaptureSnapshot(game);
		auto snapshotPart = std::find_if(snapshot.Objects.begin(), snapshot.Objects.end(), [](const SnapshotObject &object) {
			return object.Name == "AttributedPart";
		});
		Check(snapshotPart != snapshot.Objects.end() && snapshotPart->Attributes.at("Persisted") == WireValue(std::string("value")),
			"snapshot contains initial attribute state");
		auto parsedSnapshot = DeserializeSnapshot(SerializeSnapshot(snapshot));
		Check(parsedSnapshot.Succeeded() && LoadSnapshot(*parsedSnapshot.Value).Succeeded(), "attribute snapshot parses and loads");

		auto sessionStart = InProcessReplicationSession::Start(game);
		Check(sessionStart.Succeeded(), "attribute replication session starts from snapshot state");
		auto session = sessionStart.Session;
		auto receiverPart = session ? session->GetReceiverRoot()->FindFirstChild("AttributedPart", false) : nullptr;
		Check(receiverPart && receiverPart->GetAttributeValue("Persisted") == std::optional<WireValue>(std::string("value")),
			"initial attribute state replicates");
		Check(gateway.Apply(UpdateAttributeCommand{id, "Persisted", WireValue(std::string("updated"))}).Succeeded() &&
			session->ApplyAvailable().Succeeded(), "attribute update replicates through ordered journal");
		Check(receiverPart->GetAttributeValue("Persisted") == std::optional<WireValue>(std::string("updated")),
			"receiver observes replicated attribute update");
		Check(gateway.Apply(UpdateAttributeCommand{id, "Persisted", std::nullopt}).Succeeded() &&
			session->ApplyAvailable().Succeeded() && !receiverPart->GetAttributeValue("Persisted"),
			"attribute removal replicates");
		const auto duplicateAttributeCursor = session->GetCursor();
		WireJournalRecord duplicateAttribute{
			.Sequence = duplicateAttributeCursor.NextSequence,
			.Scope = WireObjectId::FromObjectId(duplicateAttributeCursor.Scope),
			.Operation = WireJournalOperation::AttributeUpdate,
			.Object = WireObjectId::FromObjectId(id),
			.AttributeName = "Persisted",
			.Value = WireValue(std::monostate{}),
		};
		auto duplicateAttributeResult = session->ApplyWireRecords({duplicateAttribute});
		Check(duplicateAttributeResult.Status == ReplicationApplyStatus::ApplyRejected &&
			session->GetCursor().NextSequence == duplicateAttributeCursor.NextSequence,
			"semantic no-op attribute replication is rejected without advancing the cursor");

		part->Destroy();
		journal.Clear();
		Check(gateway.Apply(UpdateAttributeCommand{id, "Dead", WireValue(true)}).Status == MutationStatus::StaleObject,
			"stale attribute target is rejected");
		Check(journal.ReadSince(0).empty(), "stale attribute target produces no journal record");
	}

	void TestInstanceTags() {
		using namespace gargantuan;
		auto &journal = ChangeJournal::Get();
		journal.Clear();
		auto game = std::make_shared<DataModel>();
		game->SetArchivable(true);
		auto first = std::make_shared<Folder>();
		auto second = std::make_shared<Folder>();
		auto third = std::make_shared<Folder>();
		first->SetName("FirstTagged");
		second->SetName("SecondTagged");
		third->SetName("ThirdTagged");
		first->SetParent(game);
		second->SetParent(game);
		third->SetParent(game);
		const auto scope = game->GetObjectId();
		const auto firstId = first->GetObjectId();
		const auto secondId = second->GetObjectId();
		const auto thirdId = third->GetObjectId();
		const auto schemaGeneration = GetRuntimeSchemaLifecycle().GetActiveGeneration();
		auto *tagsSchema = InstanceClassRegistry::GetDefinitionByName("Tags");
		Check(tagsSchema && tagsSchema->AllMethods.contains("GetTagged") && tagsSchema->AllMethods.contains("GetTaggedAll"),
			"frozen schema describes the bounded Tags service API");
		Check(game->GetService("Tags")->GetClassName() == "Tags", "DataModel constructs the scoped Tags service from frozen schema");

		MutationGateway gateway;
		journal.Clear();
		auto cursor = journal.CreateCursor(scope);
		auto RecordCount = [&] { return journal.Read(cursor).Records.size(); };
		Check(gateway.Apply(AddTagCommand{firstId, "Enemy"}).Succeeded(), "tag add succeeds through MutationGateway");
		Check(gateway.Apply(AddTagCommand{secondId, "Enemy"}).Succeeded() &&
			gateway.Apply(AddTagCommand{secondId, "Alive"}).Succeeded() &&
			gateway.Apply(AddTagCommand{thirdId, "Alive"}).Succeeded(), "multiple bounded memberships are accepted");
		Check(game->Tags.Has(scope, firstId, "Enemy", ScriptSecurityContext::CoreTrusted()), "tag membership lookup succeeds");
		Check(game->Tags.GetTags(scope, secondId, ScriptSecurityContext::CoreTrusted()) == std::vector<std::string>({"Alive", "Enemy"}),
			"object-side tags enumerate in deterministic name order");
		auto expectedEnemies = std::vector<ObjectId>({firstId, secondId});
		std::sort(expectedEnemies.begin(), expectedEnemies.end());
		Check(game->Tags.GetTagged(scope, "Enemy", ScriptSecurityContext::CoreTrusted()) == expectedEnemies,
			"reverse query returns deterministic ObjectId order");
		Check(game->Tags.GetTaggedAll(scope, {"Enemy", "Alive"}, ScriptSecurityContext::CoreTrusted()) == std::vector<ObjectId>({secondId}),
			"multi-tag query intersects indexed candidate sets");
		Check(game->Tags.GetTagged(scope, "Unknown", ScriptSecurityContext::CoreTrusted()).empty(), "unknown tag query is empty");
		auto otherGame = std::make_shared<DataModel>();
		auto foreign = std::make_shared<Folder>();
		foreign->SetParent(otherGame);
		Check(gateway.Apply(AddTagCommand{foreign->GetObjectId(), "Enemy"}).Succeeded() &&
			otherGame->Tags.GetTagged(otherGame->GetObjectId(), "Enemy", ScriptSecurityContext::CoreTrusted()) ==
				std::vector<ObjectId>({foreign->GetObjectId()}) &&
			game->Tags.GetTagged(scope, "Enemy", ScriptSecurityContext::CoreTrusted()) == expectedEnemies,
			"tag indexes remain isolated between DataModels");
		Check(
			gateway.Apply(AddTagCommand{foreign->GetObjectId(), "CrossScope", scope}).Status == MutationStatus::Rejected &&
			!otherGame->Tags.Has(otherGame->GetObjectId(), foreign->GetObjectId(), "CrossScope", ScriptSecurityContext::CoreTrusted()),
			"scope-bound tag commands cannot mutate another DataModel"
		);
		const auto recordsBeforeNoOps = RecordCount();
		Check(gateway.Apply(AddTagCommand{firstId, "Enemy"}).Succeeded() &&
			gateway.Apply(RemoveTagCommand{firstId, "Absent"}).Succeeded() && RecordCount() == recordsBeforeNoOps,
			"duplicate add and absent remove are journal-free no-ops");

		auto ExpectRejected = [&](MutationCommand command, const char *message) {
			const auto before = game->Tags.GetTags(scope, firstId, ScriptSecurityContext::CoreTrusted());
			const auto records = RecordCount();
			Check(!gateway.Apply(std::move(command)).Succeeded() &&
				game->Tags.GetTags(scope, firstId, ScriptSecurityContext::CoreTrusted()) == before && RecordCount() == records, message);
		};
		ExpectRejected(AddTagCommand{firstId, ""}, "empty tag is rejected atomically");
		ExpectRejected(AddTagCommand{firstId, std::string(MaximumTagNameBytes + 1, 'x')}, "oversized tag is rejected atomically");
		ExpectRejected(AddTagCommand{firstId, std::string("\xc3\x28", 2)}, "malformed UTF-8 tag is rejected atomically");

		auto counted = std::make_shared<Folder>();
		counted->SetParent(game);
		const auto countedId = counted->GetObjectId();
		for (std::size_t index = 0; index < MaximumTagsPerInstance; ++index)
			Check(gateway.Apply(AddTagCommand{countedId, "Tag" + std::to_string(index)}).Succeeded(), "tags up to the per-object limit are accepted");
		Check(!gateway.Apply(AddTagCommand{countedId, "Overflow"}).Succeeded() &&
			game->Tags.GetTags(scope, countedId, ScriptSecurityContext::CoreTrusted()).size() == MaximumTagsPerInstance,
			"per-object tag limit rejection preserves prior membership");
		CheckThrows<std::invalid_argument>([&] {
			(void)game->Tags.GetTaggedAll(
				scope,
				std::vector<std::string>(MaximumTagsPerQuery + 1, "Enemy"),
				ScriptSecurityContext::CoreTrusted()
			);
		}, "multi-tag query input is bounded before allocation/intersection work");

		auto distinctGame = std::make_shared<DataModel>();
		MutationGateway distinctGateway;
		std::vector<std::shared_ptr<Folder>> distinctObjects;
		for (std::size_t objectIndex = 0; objectIndex < MaximumDistinctTagsPerDataModel / MaximumTagsPerInstance; ++objectIndex) {
			auto object = std::make_shared<Folder>();
			object->SetParent(distinctGame);
			for (std::size_t tagIndex = 0; tagIndex < MaximumTagsPerInstance; ++tagIndex) {
				const auto ordinal = objectIndex * MaximumTagsPerInstance + tagIndex;
				Check(distinctGateway.Apply(AddTagCommand{object->GetObjectId(), "Distinct" + std::to_string(ordinal)}).Succeeded(),
					"distinct tags up to the DataModel limit are accepted");
			}
			distinctObjects.push_back(std::move(object));
		}
		auto distinctOverflow = std::make_shared<Folder>();
		distinctOverflow->SetParent(distinctGame);
		Check(!distinctGateway.Apply(AddTagCommand{distinctOverflow->GetObjectId(), "DistinctOverflow"}).Succeeded(),
			"distinct tag limit is enforced at the authoritative index");

		ScriptSecurityContext readOnly{ScriptExecutionDomain::Studio, {ScriptCapability::ReadDataModel}};
		ScriptSecurityContext writeOnly{ScriptExecutionDomain::Studio, {ScriptCapability::MutateDataModel}};
		Check(gateway.Apply(AddTagCommand{thirdId, "Denied"}, readOnly).Status == MutationStatus::Unauthorized,
			"tag mutation requires MutateDataModel");
		CheckThrows<std::runtime_error>([&] { (void)game->Tags.GetTagged(scope, "Alive", writeOnly); },
			"tag query requires ReadDataModel");
		Check(GetRuntimeSchemaLifecycle().GetActiveGeneration() == schemaGeneration,
			"tag changes do not alter frozen schema generation");

		std::shared_ptr<Instance> persistenceRoot = game;
		auto serialized = InstanceSerialization::Serialize(InstanceSerialization::InstanceFormat::Json, persistenceRoot);
		Check(serialized == InstanceSerialization::Serialize(InstanceSerialization::InstanceFormat::Json, persistenceRoot),
			"tag persistence is deterministic");
		std::istringstream persistedInput(serialized);
		auto deserialized = InstanceSerialization::Deserialize(InstanceSerialization::InstanceFormat::Json, persistedInput);
		Check(deserialized.Ok, "tag project persistence round-trips");
		auto loadedGame = std::dynamic_pointer_cast<DataModel>(deserialized.Instance);
		auto loadedFirst = loadedGame ? loadedGame->FindFirstChild("FirstTagged", false) : nullptr;
		Check(loadedGame && loadedFirst && loadedGame->Tags.Has(loadedGame->GetObjectId(), loadedFirst->GetObjectId(), "Enemy", ScriptSecurityContext::CoreTrusted()),
			"deserialization rebuilds the reverse tag index");
		auto malformed = nlohmann::ordered_json::parse(serialized);
		for (auto &child : malformed["Children"]) if (child["Name"] == "FirstTagged") child["Tags"] = {"Enemy", "Enemy"};
		std::istringstream malformedInput(malformed.dump());
		Check(!InstanceSerialization::Deserialize(InstanceSerialization::InstanceFormat::Json, malformedInput).Ok,
			"duplicate persisted tag membership is rejected");

		auto snapshot = CaptureSnapshot(game);
		auto snapshotFirst = std::find_if(snapshot.Objects.begin(), snapshot.Objects.end(), [](const SnapshotObject &object) { return object.Name == "FirstTagged"; });
		Check(snapshotFirst != snapshot.Objects.end() && snapshotFirst->Tags == std::vector<std::string>({"Enemy"}),
			"snapshot captures canonical tag membership");
		auto parsed = DeserializeSnapshot(SerializeSnapshot(snapshot));
		Check(parsed.Succeeded() && LoadSnapshot(*parsed.Value).Succeeded(), "tag snapshot parses and rebuilds indexes");

		auto sessionStart = InProcessReplicationSession::Start(game);
		Check(sessionStart.Succeeded(), "tag replication starts from snapshot membership");
		auto receiverGame = sessionStart.Session ? std::dynamic_pointer_cast<DataModel>(sessionStart.Session->GetReceiverRoot()) : nullptr;
		auto receiverFirst = receiverGame ? receiverGame->FindFirstChild("FirstTagged", false) : nullptr;
		Check(receiverGame && receiverFirst && receiverGame->Tags.Has(receiverGame->GetObjectId(), receiverFirst->GetObjectId(), "Enemy", ScriptSecurityContext::CoreTrusted()),
			"initial tag membership replicates");
		Check(gateway.Apply(AddTagCommand{firstId, "Boss"}).Succeeded() && sessionStart.Session->ApplyAvailable().Succeeded(),
			"tag add replicates through the ordered journal");
		Check(receiverGame->Tags.Has(receiverGame->GetObjectId(), receiverFirst->GetObjectId(), "Boss", ScriptSecurityContext::CoreTrusted()),
			"receiver reverse index observes replicated add");
		Check(gateway.Apply(RemoveTagCommand{firstId, "Boss"}).Succeeded() && sessionStart.Session->ApplyAvailable().Succeeded() &&
			!receiverGame->Tags.Has(receiverGame->GetObjectId(), receiverFirst->GetObjectId(), "Boss", ScriptSecurityContext::CoreTrusted()),
			"tag removal replicates");

		auto ancestor = std::make_shared<Folder>();
		auto descendant = std::make_shared<Folder>();
		ancestor->SetParent(game);
		descendant->SetParent(ancestor);
		const auto descendantId = descendant->GetObjectId();
		Check(gateway.Apply(AddTagCommand{descendantId, "Temporary"}).Succeeded(), "descendant tag fixture is indexed");
		ancestor->Destroy();
		Check(game->Tags.GetTagged(scope, "Temporary", ScriptSecurityContext::CoreTrusted()).empty(),
			"ancestor destruction removes descendant reverse entries");

		auto movingAncestor = std::make_shared<Folder>();
		auto movingDescendant = std::make_shared<Folder>();
		movingAncestor->SetParent(game);
		movingDescendant->SetParent(movingAncestor);
		const auto movingDescendantId = movingDescendant->GetObjectId();
		Check(gateway.Apply(AddTagCommand{movingDescendantId, "OldWorld"}).Succeeded(),
			"cross-scope subtree fixture is tagged");
		movingAncestor->SetParent(otherGame);
		movingAncestor->SetParent(game);
		Check(!game->Tags.Has(scope, movingDescendantId, "OldWorld", ScriptSecurityContext::CoreTrusted()),
			"moving a subtree between DataModels cleans descendant tag membership before it can resurface");

		first->Destroy();
		auto replacement = std::make_shared<Folder>();
		replacement->SetParent(game);
		const auto replacementId = replacement->GetObjectId();
		Check(replacementId.Slot == firstId.Slot && replacementId.Generation != firstId.Generation &&
			!game->Tags.Has(scope, replacementId, "Enemy", ScriptSecurityContext::CoreTrusted()),
			"generation-reused slots do not inherit old tags");
		Check(sessionStart.Session->ApplyAvailable().Succeeded(), "tagged destroy and subsequent generation-safe create replicate");
		Check(game->Tags.GetTagged(scope, "Enemy", ScriptSecurityContext::CoreTrusted()) == std::vector<ObjectId>({secondId}),
			"destroy removes authoritative reverse membership");
		Check(receiverGame->Tags.GetTagged(receiverGame->GetObjectId(), "Enemy", ScriptSecurityContext::CoreTrusted()).size() == 1,
			"receiver query cannot return destroyed membership");

		auto duplicateSessionStart = InProcessReplicationSession::Start(game);
		Check(duplicateSessionStart.Succeeded(), "duplicate tag replication fixture starts");
		const auto duplicateCursor = duplicateSessionStart.Session->GetCursor();
		WireJournalRecord duplicateTag{
			.Sequence = duplicateCursor.NextSequence,
			.Scope = WireObjectId::FromObjectId(duplicateCursor.Scope),
			.Operation = WireJournalOperation::TagAdded,
			.Object = WireObjectId::FromObjectId(secondId),
			.TagName = "Enemy",
		};
		auto duplicateResult = duplicateSessionStart.Session->ApplyWireRecords({duplicateTag});
		Check(duplicateResult.Status == ReplicationApplyStatus::ApplyRejected &&
			duplicateSessionStart.Session->GetCursor().NextSequence == duplicateCursor.NextSequence,
			"semantically duplicate tag replication is rejected without advancing the cursor");
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
		std::ofstream preRun(temporaryRoot / ".gargantuan" / "prerun.luau", std::ios::binary);
		preRun << R"(Schema:RegisterEnum({
			Namespace = "Game",
			Name = "CombatState",
			Version = 1,
			Items = { Idle = 0, Attacking = 1, Blocking = 2 },
		}))";
		preRun.close();
		const auto rejectedRoot = temporaryRoot / "rejected";
		std::filesystem::create_directories(rejectedRoot / ".gargantuan");
		std::ofstream rejectedProject(rejectedRoot / ".gargantuan" / "project.instance.json", std::ios::binary);
		rejectedProject << R"({"Version":0,"Name":"RejectedWorld","ClassName":"DataModel","Properties":{},"Children":[]})";
		rejectedProject.close();
		std::ofstream rejectedPreRun(rejectedRoot / ".gargantuan" / "prerun.luau", std::ios::binary);
		rejectedPreRun << R"(Schema:RegisterEnum({ Namespace="Game", Name="Broken", Version=1, Items={ A=1, B=1 } }))";
		rejectedPreRun.close();

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
		Check(
			schema["Ok"].get<bool>() && schema["Result"]["SchemaDiscoveryVersion"] == 2 &&
				!schema["Result"]["Classes"].empty() && !schema["Result"]["Definitions"].empty(),
			"EditorHost exposes versioned frozen schema discovery without removing the class adapter"
		);

		auto opened = call("OpenProject", {{"Root", temporaryRoot.string()}}, "test-token");
		Check(opened["Ok"].get<bool>(), "EditorHost opens a project without starting the engine loop");
		auto projectSchema = call("GetSchema", Json::object(), "test-token");
		auto discoveredEnum = std::find_if(
			projectSchema["Result"]["Definitions"].begin(), projectSchema["Result"]["Definitions"].end(),
			[](const Json &definition) { return definition["CanonicalName"] == "Game.CombatState"; }
		);
		Check(
			projectSchema["Ok"].get<bool>() && discoveredEnum != projectSchema["Result"]["Definitions"].end() &&
				(*discoveredEnum)["Kind"] == "Enum" && (*discoveredEnum)["Provenance"] == "Game" &&
				(*discoveredEnum)["Items"].size() == 3,
			"EditorHost exposes immutable project enum metadata after PreRun publication"
		);
		const auto publishedGeneration = projectSchema["Result"]["RegistryGeneration"];
		auto rejectedOpen = call("OpenProject", {{"Root", rejectedRoot.string()}}, "test-token");
		Check(
			!rejectedOpen["Ok"].get<bool>() && rejectedOpen["Error"]["Code"] == "RequestRejected",
			"EditorHost reports failed PreRun project bootstrap as a structured error"
		);
		auto preservedSchema = call("GetSchema", Json::object(), "test-token");
		Check(
			preservedSchema["Result"]["RegistryGeneration"] == publishedGeneration &&
				std::any_of(
					preservedSchema["Result"]["Definitions"].begin(), preservedSchema["Result"]["Definitions"].end(),
					[](const Json &definition) { return definition["CanonicalName"] == "Game.CombatState"; }
				),
			"failed project PreRun preserves the prior frozen schema and generation"
		);
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
			auto attributeMutation = call("SetAttribute", {
				{"Object", (*editable)["Id"]},
				{"Attribute", "Health"},
				{"Value", {{"Type", "Double"}, {"Value", 100.0}}},
			}, "test-token");
			Check(attributeMutation["Ok"].get<bool>(), "EditorHost applies a bounded attribute mutation through the gateway");
			auto attributeChanges = call("PollChanges", Json::object(), "test-token");
			Check(
				attributeChanges["Ok"].get<bool>() && attributeChanges["Result"]["Records"].size() == 1 &&
					attributeChanges["Result"]["Records"][0]["Operation"] == "AttributeUpdate" &&
					attributeChanges["Result"]["Records"][0]["AttributeName"] == "Health",
				"EditorHost publishes one dedicated attribute journal record"
			);
			auto unsupportedAttribute = call("SetAttribute", {
				{"Object", (*editable)["Id"]},
				{"Attribute", "Reference"},
				{"Value", {{"Type", "ObjectReference"}, {"Value", (*editable)["Id"]}}},
			}, "test-token");
			Check(!unsupportedAttribute["Ok"].get<bool>(), "EditorHost rejects unsupported attribute WireValues");
			auto rejectedAttributeChanges = call("PollChanges", Json::object(), "test-token");
			Check(rejectedAttributeChanges["Ok"].get<bool>() && rejectedAttributeChanges["Result"]["Records"].empty(),
				"rejected EditorHost attribute mutation emits no journal record");
			auto addTag = call("AddTag", {{"Object", (*editable)["Id"]}, {"Tag", "Enemy"}}, "test-token");
			Check(addTag["Ok"].get<bool>(), "EditorHost applies tag addition through the mutation gateway");
			auto tagAdded = call("PollChanges", Json::object(), "test-token");
			Check(tagAdded["Ok"].get<bool>() && tagAdded["Result"]["Records"].size() == 1 &&
				tagAdded["Result"]["Records"][0]["Operation"] == "TagAdded" &&
				tagAdded["Result"]["Records"][0]["TagName"] == "Enemy",
				"EditorHost publishes one semantic TagAdded record");
			auto removeTag = call("RemoveTag", {{"Object", (*editable)["Id"]}, {"Tag", "Enemy"}}, "test-token");
			Check(removeTag["Ok"].get<bool>(), "EditorHost applies tag removal through the mutation gateway");
			auto tagRemoved = call("PollChanges", Json::object(), "test-token");
			Check(tagRemoved["Ok"].get<bool>() && tagRemoved["Result"]["Records"].size() == 1 &&
				tagRemoved["Result"]["Records"][0]["Operation"] == "TagRemoved",
				"EditorHost publishes one semantic TagRemoved record");
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
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
	} catch (const std::exception &exception) {
		std::cerr << "Runtime schema bootstrap failed: " << exception.what() << '\n';
		return 1;
	}

	TestHierarchyAndDestruction();
	TestObjectIdsAndChanges();
	TestWorldRootConstraintValidation();
	TestCheckedResolutionAndOwnedPaths();
	TestJobSystem();
	TestSchemaMetadata();
	TestRuntimeSchemaRegistry();
	TestRuntimeSchemaLifecycle();
	TestCustomEnumPreRun();
	TestRenderSnapshotExtraction();
	TestScriptSecurityModel();
	TestMutationGateway();
	TestInstanceAttributes();
	TestInstanceTags();
	TestBoundedJournalCursor();
	TestSnapshotBaseline();
	TestWireJournalAndLoopbackReplication();
	TestSharedFrameRing();
	TestEditorHostProtocol();
	TestLuauExceptionBoundary();
	if (Failures == 0) std::cout << "All foundation tests passed\n";
	return Failures == 0 ? 0 : 1;
}
