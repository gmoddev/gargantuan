#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/ModuleScript.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"
#include "gargantuan/runtime/DataModelRoot.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"
#include "gargantuan/runtime/JobSystem.hpp"
#include "gargantuan/runtime/MutationGateway.hpp"
#include "gargantuan/runtime/ObjectId.hpp"
#include "gargantuan/scripting/ModuleResolution.hpp"
#include "gargantuan/scripting/NativeCallback.hpp"

#include <atomic>
#include <iostream>
#include <lua.h>
#include <lualib.h>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
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

	void TestHierarchyAndDestruction() {
		using namespace gargantuan;
		auto parent = std::make_shared<Folder>();
		auto child = std::make_shared<Folder>();
		auto grandchild = std::make_shared<Folder>();
		child->SetParent(parent);
		grandchild->SetParent(child);
		CheckThrows<std::invalid_argument>([&] { parent->SetParent(parent); }, "self-parenting is rejected");
		CheckThrows<std::invalid_argument>([&] { parent->SetParent(grandchild); }, "descendant cycles are rejected");

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
		Check(!destroyed->Editable && !destroyed->Write, "Destroyed lifecycle metadata is read-only and non-editable");
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
				std::any_cast<std::string>(std::get<PropertyUpdatedChange>(records.front().Payload).Value) == "FromWorker",
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
		journal.SetCapacity(originalCapacity);
		journal.Clear();
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
	TestCheckedResolutionAndOwnedPaths();
	TestJobSystem();
	TestSchemaMetadata();
	TestMutationGateway();
	TestBoundedJournalCursor();
	TestLuauExceptionBoundary();
	if (Failures == 0) std::cout << "All foundation tests passed\n";
	return Failures == 0 ? 0 : 1;
}
