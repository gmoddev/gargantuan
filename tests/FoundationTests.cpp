#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/ModuleScript.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"
#include "gargantuan/runtime/DataModelRoot.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"
#include "gargantuan/runtime/JobSystem.hpp"
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
	TestLuauExceptionBoundary();
	if (Failures == 0) std::cout << "All foundation tests passed\n";
	return Failures == 0 ? 0 : 1;
}
