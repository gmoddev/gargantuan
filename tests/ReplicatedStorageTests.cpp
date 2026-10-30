#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/ModuleScript.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/Player.hpp"
#include "gargantuan/classes/Script.hpp"
#include "gargantuan/network/ReplicaApplier.hpp"
#include "gargantuan/network/ReplicationCoordinator.hpp"
#include "gargantuan/network/ReplicationRelevance.hpp"
#include "gargantuan/reflection/InstanceClassRegistry.hpp"
#include "gargantuan/reflection/PreRunRegistration.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"
#include "gargantuan/runtime/MutationGateway.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

int main() {
	using namespace gargantuan;
	const auto Root = std::filesystem::temp_directory_path() /
					  ("gargantuan-shared-service-" +
					   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	struct Cleanup {
		std::filesystem::path Root;
		~Cleanup() {
			std::error_code Ignored;
			std::filesystem::remove_all(Root, Ignored);
		}
	} CleanupValue{Root};
	try {
		std::filesystem::create_directories(Root / ".gargantuan");
		std::ofstream Source(Root / ".gargantuan" / "prerun.luau");
		Source << R"(
Schema:RegisterClass({Namespace="Game", Name="SharedInventory", Version=1, Base="Engine.Folder",
    Service=true, Replicated=true, Properties={}})
Schema:RegisterClass({Namespace="Game", Name="PrivateInventory", Version=1, Base="Engine.Folder",
    Service=true, Properties={Count={Type="Integer", Default=0}}})
Schema:RegisterClass({Namespace="Game", Name="OrdinaryFolder", Version=1, Base="Engine.Folder",
    Properties={Count={Type="Integer", Default=0}}})
)";
		Source.close();
		BootstrapProjectRuntimeSchema(Root);
		auto World = std::make_shared<DataModel>();
		World->MarkPersistenceSubtreeArchivable();
		World->InitializeLoadedProjectRevision();
		std::size_t Checks = 0;
		auto Check = [&](bool Condition, const char *Message) {
			if (!Condition) throw std::runtime_error(Message);
			++Checks;
		};
		auto Shared = World->GetService("ReplicatedStorage");
		Check(
			Shared && Shared->GetClassName() == "ReplicatedStorage" && World->GetService("ReplicatedStorage") == Shared,
			"Built-in ReplicatedStorage is one canonical service"
		);
		Check(Shared->GetArchivable(), "Service created after project persistence initialization remains archivable");
		auto Custom = World->GetService("Game.SharedInventory");
		Check(
			Custom && Custom->GetClassName() == "Game.SharedInventory" && Custom->IsA("Engine.Folder") &&
				World->GetService("Game.SharedInventory") == Custom,
			"Custom service uses stable custom identity and existing data-only host"
		);
		Check(
			InstanceClassRegistry::GetDefinition(Custom.get())->ServicePolicy == SchemaServicePolicy::Replicated &&
				InstanceClassRegistry::GetDefinition(Shared.get())->ServicePolicy == SchemaServicePolicy::Replicated,
			"Native and custom service replication policy lives in frozen schema"
		);
		auto Private = World->GetService("Game.PrivateInventory");
		Check(
			InstanceClassRegistry::GetDefinition(Private.get())->ServicePolicy == SchemaServicePolicy::ServerOnly,
			"Custom services default to server-only policy"
		);
		Check(
			World->IsProtectedService(Custom) && World->IsProtectedService(Shared) &&
				World->IsProtectedService(Private),
			"All service roots use existing protected lifecycle"
		);
		MutationGateway Mutations;
		Check(
			!Mutations.Apply(DestroyObjectCommand{Custom->GetObjectId()}, ScriptSecurityContext::StudioCoreUi())
				 .Succeeded(),
			"Studio cannot destroy custom service root"
		);
		Check(
			!Mutations
				 .Apply(
					 ReparentObjectCommand{Shared->GetObjectId(), Private->GetObjectId()},
					 ScriptSecurityContext::StudioCoreUi()
				 )
				 .Succeeded(),
			"Studio cannot reparent built-in service root"
		);
		Check(
			!Mutations
				 .Apply(
					 CreateObjectCommand{
						 InstanceClassRegistry::GetDefinition(Custom.get())->Id, 1, World->GetObjectId()
					 },
					 ScriptSecurityContext::StudioCoreUi()
				 )
				 .Succeeded(),
			"Studio cannot duplicate singleton construction"
		);
		bool OrdinaryRejected = false;
		try {
			(void)World->GetService("Game.OrdinaryFolder");
		} catch (const std::runtime_error &) {
			OrdinaryRejected = true;
		}
		Check(OrdinaryRejected, "Ordinary custom class does not gain service authority");
		auto Module = std::make_shared<ModuleScript>();
		Module->SetName("SharedRules");
		Module->SetSource("return { Value = 7 }");
		Module->SetParent(Custom);
		auto Secret = std::make_shared<ModuleScript>();
		Secret->SetName("PrivateRules");
		Secret->SetSource("return 'server-only'");
		Secret->SetParent(Private);
		Check(
			Module->GetReplicatedSource() == Module->GetSource() && Secret->GetReplicatedSource().empty(),
			"Only explicitly shared module source is projected"
		);
		const auto Cursor = ChangeJournal::Get().CreateCursor(World->GetObjectId());
		const auto Revision = World->GetAuthoritativeRevision();
		Check(
			Mutations
					.Apply(
						UpdatePropertyCommand{Module->GetObjectId(), "Name", std::string("client-write")},
						ScriptSecurityContext::ClientRuntime()
					)
					.Status == MutationStatus::Unauthorized,
			"Captured client authority cannot rename shared state"
		);
		Check(
			Mutations.Apply(DestroyObjectCommand{Module->GetObjectId()}, ScriptSecurityContext::ClientRuntime())
					.Status == MutationStatus::Unauthorized,
			"Client cannot destroy shared descendants"
		);
		Check(
			Module->ApplyAttributeMutation("Client", WireValue(true), ScriptSecurityContext::ClientRuntime()) ==
				MutationStatus::Unauthorized,
			"Client cannot mutate shared attributes through the direct native boundary"
		);
		{
			ScriptSecurityScope Client(ScriptSecurityContext::ClientRuntime());
			bool Denied = false;
			try {
				Module->SetParent(Private);
			} catch (const std::runtime_error &) {
				Denied = true;
			}
			Check(
				Denied && Module->GetParent() == Custom, "Client cannot move shared content through native parenting"
			);
			Check(!World->FindService("Game.PrivateInventory"), "Client cannot resolve a server-only custom service");
		}
		Check(
			World->GetAuthoritativeRevision() == Revision &&
				ChangeJournal::Get().CreateCursor(World->GetObjectId()).NextSequence == Cursor.NextSequence,
			"Rejected client writes publish no state, revision or journal changes"
		);
		auto StoredPart = std::make_shared<Part>();
		StoredPart->SetCFrame(CFrame(100000, 0, 0));
		StoredPart->SetParent(Custom);
		auto PlayerValue = std::make_shared<Player>();
		PlayerValue->SetParent(World);
		using namespace gargantuan::network;
		const ConnectionId Connection{510, 1};
		ReplicationRelevance Relevance(World);
		Check(
			Relevance.AddPeer(Connection, PlayerValue->GetObjectId()) && Relevance.Update(1),
			"Service uses ordinary server-owned relevance"
		);
		const auto *Selection = Relevance.GetSelection(Connection);
		Check(
			Selection && std::ranges::binary_search(Selection->DesiredObjects, StoredPart->GetObjectId()) &&
				!std::ranges::binary_search(Selection->DesiredObjects, Secret->GetObjectId()) &&
				std::ranges::binary_search(Selection->RequiredObjects, Custom->GetObjectId()),
			"Shared contents ignore spatial distance, service root bootstraps, private contents stay excluded"
		);
		ReplicationCoordinator Coordinator(World);
		ReplicaApplier Replica;
		auto Baseline = Coordinator.AddPeer(Connection, ReplicationEpoch(1), *Selection);
		Check(
			Baseline.Succeeded() && Replica.ApplyFrame(*Baseline.Frame).Succeeded(),
			"Shared service materializes through the real replication codec/applier"
		);
		auto ClientModule = std::dynamic_pointer_cast<ModuleScript>(Replica.Resolve(Module->GetObjectId()));
		Check(
			ClientModule && ClientModule->GetSource() == Module->GetSource() &&
				ClientModule->GetSourceVersion() == Module->GetSourceVersion() &&
				!Replica.Resolve(Secret->GetObjectId()),
			"Client receives exact shared module source/version and no private module"
		);
		Module->SetSource("return { Value = 8 }");
		auto Update = Coordinator.ProduceIncremental(Connection);
		Check(
			Update.Succeeded() && Replica.ApplyFrame(*Update.Frame).Succeeded() &&
				ClientModule->GetSource() == Module->GetSource() &&
				ClientModule->GetSourceVersion() == Module->GetSourceVersion(),
			"Source updates use ordinary journal replication without advancing the received version twice"
		);
		ReplicaApplier LateReplica;
		auto Late = Coordinator.AddPeer({511, 1}, ReplicationEpoch(2), *Selection);
		Check(
			Late.Succeeded() && LateReplica.ApplyFrame(*Late.Frame).Succeeded(),
			"Late peer receives a valid current baseline"
		);
		auto LateModule = std::dynamic_pointer_cast<ModuleScript>(LateReplica.Resolve(Module->GetObjectId()));
		Check(
			LateModule && LateModule->GetSource() == Module->GetSource(),
			"Late materialization receives current module contents"
		);
		Check(
			ClientModule->BytecodeCompileStatus == BytecodeCompileStatus::Uncompiled,
			"Replicating a module does not execute or compile it"
		);
		{
			auto ClientWorld = std::dynamic_pointer_cast<DataModel>(Replica.GetReplicaRoot());
			auto StoredScript = std::make_shared<Script>();
			StoredScript->SetSource("error('stored scripts must not execute')");
			StoredScript->SetRunContext(Enums::RunContext::Client);
			StoredScript->SetParent(ClientWorld->GetService("Game.SharedInventory"));
			auto Probe = std::make_shared<Script>();
			Probe->SetName("ClientStorageProbe");
			Probe->SetRunContext(Enums::RunContext::Client);
			Probe->SetSource(R"(
local Rules = require("@game/Game.SharedInventory/SharedRules")
assert(Rules.Value == 8)
local Shared = game:GetService("Game.SharedInventory")
assert(not pcall(function() Shared.Name = "client-rename" end))
assert(not pcall(function() game:GetService("Game.PrivateInventory") end))
)");
			Probe->SetParent(ClientWorld);
			ScriptEngine Scripts(ClientWorld, {}, RuntimeMode::NetworkClient);
			Scripts.ScriptQueue.insert(StoredScript);
			Scripts.Step();
			Check(StoredScript->Status == ScriptStatus::Idle, "Stored client-context Script remains dormant");
			Scripts.RunBootstrapScript(Probe);
			Check(
				Probe->Status == ScriptStatus::Finished,
				"Actual client Luau can explicitly require shared module without gaining mutation/private-service "
				"authority"
			);
		}
		std::shared_ptr<Instance> SavedWorld = World;
		const auto Serialized = InstanceSerialization::Serialize(
			InstanceSerialization::InstanceFormat::Json, SavedWorld
		);
		Check(
			Serialized.find("ReplicatedSource") == std::string::npos,
			"Shared source projection is not a second persisted source"
		);
		std::istringstream Input(Serialized);
		auto Restored = InstanceSerialization::Deserialize(InstanceSerialization::InstanceFormat::Json, Input);
		auto RestoredWorld = std::dynamic_pointer_cast<DataModel>(Restored.Instance);
		Check(Restored.Ok && RestoredWorld, "Saved service hierarchy deserializes without a schema migration");
		auto RestoredModule = std::dynamic_pointer_cast<ModuleScript>(
			RestoredWorld->GetService("Game.SharedInventory")->FindFirstChild("SharedRules", false)
		);
		if (!RestoredModule || RestoredModule->GetSource() != Module->GetSource()) {
			for (const auto &Child : RestoredWorld->Children)
				std::cout << "[Storage:Persistence] root-child=" << Child->GetClassName()
						  << " name=" << Child->GetName() << " descendants=" << Child->GetDescendants().size() << '\n';
			std::cout << "[Storage:Persistence] module=" << static_cast<bool>(RestoredModule)
					  << " source-bytes=" << (RestoredModule ? RestoredModule->GetSource().size() : 0) << '\n';
		}
		Check(
			RestoredModule && RestoredModule->GetSource() == Module->GetSource(),
			"Save/reopen reuses canonical service identity and canonical module source"
		);
		RestoredWorld->Destroy();
		const auto OldReplicaId = ClientModule->GetObjectId();
		Module->SetParent(Private);
		Check(Relevance.Update(10), "Private reparent refreshes existing relevance");
		auto Hidden = Coordinator.UpdateRelevance(Connection, *Relevance.GetSelection(Connection));
		Check(
			Hidden.Succeeded() && Replica.ApplyFrame(*Hidden.Frame).Succeeded() &&
				!Replica.Resolve(Module->GetObjectId()) && ClientModule->GetDestroyed(),
			"Private reparent unpublishes the old replica without destroying the server module"
		);
		Module->SetParent(Custom);
		Check(Relevance.Update(20), "Shared reparent refreshes existing relevance");
		auto Reentered = Coordinator.UpdateRelevance(Connection, *Relevance.GetSelection(Connection));
		Check(
			Reentered.Succeeded() && Replica.ApplyFrame(*Reentered.Frame).Succeeded(),
			"Reentry uses the ordinary publication path"
		);
		auto ReenteredModule = std::dynamic_pointer_cast<ModuleScript>(Replica.Resolve(Module->GetObjectId()));
		Check(
			ReenteredModule && ReenteredModule->GetObjectId() != OldReplicaId &&
				ReenteredModule->GetSource() == Module->GetSource(),
			"Reentry retains source identity but creates a fresh local generation and current source"
		);
		Module->Destroy();
		Check(Relevance.Update(30), "Destroy refreshes existing relevance");
		auto Destroyed = Coordinator.UpdateRelevance(Connection, *Relevance.GetSelection(Connection));
		Check(
			Destroyed.Succeeded() && Replica.ApplyFrame(*Destroyed.Frame).Succeeded() &&
				!Replica.Resolve(Module->GetObjectId()),
			"Authoritative destruction retires shared materialization"
		);
		Replica.Reset();
		LateReplica.Reset();
		World->Destroy();
		const auto Generation = GetRuntimeSchemaLifecycle().GetActiveGeneration();
		for (const auto *Fields : {"Service=1", "Replicated=true", "Service=true, Replicated='yes'"}) {
			std::ofstream Invalid(Root / ".gargantuan" / "prerun.luau", std::ios::trunc);
			Invalid << "Schema:RegisterClass({Namespace='Game',Name='InvalidService',Version=1,Base='Engine.Folder',"
					<< Fields << ",Properties={Marker={Type='Integer',Default=0}}})";
			Invalid.close();
			bool Rejected = false;
			try {
				BootstrapProjectRuntimeSchema(Root);
			} catch (const std::exception &) {
				Rejected = true;
			}
			Check(
				Rejected && GetRuntimeSchemaLifecycle().GetActiveGeneration() == Generation &&
					GetActiveRuntimeSchemaRegistry().FindClassByName("Game.InvalidService") == nullptr,
				"Invalid service toggle aborts the whole candidate without publishing or changing generation"
			);
		}
		std::cout << "[Storage:Service] PASS checks=" << Checks << '\n';
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Storage:Service] FAIL " << Error.what() << '\n';
		return 1;
	}
}
