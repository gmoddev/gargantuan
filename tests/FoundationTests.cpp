#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/Frame.hpp"
#include "gargantuan/classes/ModuleScript.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/Script.hpp"
#include "gargantuan/classes/WeldConstraint.hpp"
#include "gargantuan/datatypes/Vector2.hpp"
#include "gargantuan/editor/EditorHost.hpp"
#include "gargantuan/editor/PlaySession.hpp"
#include "gargantuan/editor/EditorViewport.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/Project.hpp"
#include "gargantuan/network/ReplicaApplier.hpp"
#include "gargantuan/network/ReplicationCoordinator.hpp"
#include "gargantuan/render/RenderExtractor.hpp"
#include "gargantuan/render/RenderProjection.hpp"
#include "gargantuan/render/Renderer.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"
#include "gargantuan/runtime/AttributeValidation.hpp"
#include "gargantuan/runtime/DataModelRoot.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"
#include "gargantuan/runtime/JobSystem.hpp"
#include "gargantuan/runtime/InProcessReplicationSession.hpp"
#include "gargantuan/runtime/MutationGateway.hpp"
#include "gargantuan/runtime/ObjectId.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"
#include "gargantuan/runtime/Snapshot.hpp"
#include "gargantuan/runtime/TagIndex.hpp"
#include "gargantuan/runtime/WireCodec.hpp"
#include "gargantuan/runtime/WireJournal.hpp"
#ifdef GARGANTUAN_WITH_GLAZE_SERIALIZATION_PROTOTYPE
#include "serialization/GlazePrototype.hpp"
#endif
#include "gargantuan/reflection/PreRunRegistration.hpp"
#include "gargantuan/reflection/RuntimeSchema.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/scripting/ModuleResolution.hpp"
#include "gargantuan/scripting/NativeCallback.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"
#include "gargantuan/services/ProcessService.hpp"
#include "gargantuan/services/Tags.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <lua.h>
#include <luacode.h>
#include <lualib.h>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <sstream>
#include <set>
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
	static_assert(std::is_abstract_v<gargantuan::BaseRenderer>);

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

	gargantuan::SchemaExtensionDefinition MakeSchemaExtensionDefinition(
		std::string schemaNamespace,
		std::string name,
		std::vector<gargantuan::SchemaExtensionProperty> properties = {
			{.Name = "Damage", .Type = gargantuan::SchemaExtensionPropertyType::Integer, .DefaultValue = 0},
		}
	) {
		using namespace gargantuan;
		return {
			.Id = SchemaId::FromExtensionName(schemaNamespace, name),
			.Namespace = std::move(schemaNamespace),
			.Name = std::move(name),
			.DefinitionVersion = 1,
			.Provenance = SchemaProvenance::Game,
			.OriginDetail = ".gargantuan/prerun.luau",
			.Properties = std::move(properties),
		};
	}

	std::string ReadSerializationFixture(std::string_view Name) {
		const auto Path = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "serialization" / Name;
		std::ifstream Input(Path, std::ios::binary);
		if (!Input) throw std::runtime_error("Unable to open serialization fixture: " + Path.string());
		std::string Contents((std::istreambuf_iterator<char>(Input)), std::istreambuf_iterator<char>());
		while (!Contents.empty() && (Contents.back() == '\n' || Contents.back() == '\r')) Contents.pop_back();
		return Contents;
	}

	gargantuan::SchemaClassDefinition MakeCustomClassDefinition(
		std::string schemaNamespace,
		std::string name,
		std::vector<gargantuan::SchemaClassProperty> properties = {
			{.Name = "Health", .Type = gargantuan::SchemaExtensionPropertyType::Integer, .DefaultValue = 100},
		}
	) {
		using namespace gargantuan;
		return {
			.Id = SchemaId::FromCustomClassName(schemaNamespace, name),
			.Namespace = std::move(schemaNamespace),
			.ClassName = std::move(name),
			.DefinitionVersion = 1,
			.Provenance = SchemaProvenance::Game,
			.ConstructionKind = SchemaClassConstructionKind::CustomData,
			.ProjectSubclassPolicy = CustomSubclassPolicy::DataOnly,
			.Constructor = nullptr,
			.DeclaredCustomProperties = std::move(properties),
			.OriginDetail = ".gargantuan/prerun.luau",
		};
	}

	gargantuan::SchemaClassDefinition MakeCustomSubclassableHost(std::string name) {
		auto Definition = MakeSchemaDefinition(std::move(name));
		Definition.ProjectSubclassPolicy = gargantuan::CustomSubclassPolicy::DataOnly;
		Definition.Constructor = []() -> std::shared_ptr<gargantuan::Instance> {
			return std::make_shared<gargantuan::Folder>();
		};
		return Definition;
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

	void TestServiceProviderSemantics() {
		using namespace gargantuan;
		auto Execute = [](const std::shared_ptr<DataModel> &World, std::string_view Source, const char *Name) {
			ScriptEngine Engine(World);
			size_t BytecodeSize = 0;
			char *Bytecode = luau_compile(Source.data(), Source.size(), &Engine.CompileOptions, &BytecodeSize);
			if (!Bytecode) return false;
			const auto LoadStatus = luau_load(Engine.L, Name, Bytecode, BytecodeSize, 0);
			std::free(Bytecode);
			const auto Status = LoadStatus == LUA_OK ? lua_pcall(Engine.L, 0, 0, 0) : LoadStatus;
			if (Status != LUA_OK)
				std::cerr << "SERVICE LUAU ERROR: " <<
					(lua_tostring(Engine.L, -1) ? lua_tostring(Engine.L, -1) : "unknown Luau error") << '\n';
			return Status == LUA_OK;
		};

		auto DirectFirstWorld = std::make_shared<DataModel>();
		Check(!DirectFirstWorld->FindService("Tags"), "FindService remains non-constructing before direct service access");
		Check(Execute(DirectFirstWorld, R"(
			local Direct = game.Tags
			assert(Direct ~= nil and Direct:IsA("Tags"))
			assert(game:FindService("Tags") == Direct)
			assert(game:GetService("Tags") == Direct)
			Direct.Name = "RenamedTags"
			assert(game.Tags == Direct)
		)", "service-direct-first"),
			"direct DataModel access lazily constructs the canonical service and survives visible Name changes");
		auto DirectFirstService = DirectFirstWorld->FindService("Tags");
		Check(DirectFirstService && (*DirectFirstService)->GetName() == "RenamedTags" &&
			DirectFirstWorld->GetService("Tags") == *DirectFirstService,
			"direct-first, FindService, and GetService preserve canonical identity");

		auto GetFirstWorld = std::make_shared<DataModel>();
		Check(Execute(GetFirstWorld, R"(
			local FromGet = game:GetService("Tags")
			assert(FromGet ~= nil and FromGet:IsA("Tags"))
			assert(game.Tags == FromGet)
			assert(game:GetService("Tags") == FromGet)
		)", "service-get-first"),
			"GetService-first access and canonical direct access preserve identity");
		auto GetFirstService = GetFirstWorld->FindService("Tags");
		Check(GetFirstService && DirectFirstService && *GetFirstService != *DirectFirstService &&
			(*GetFirstService)->GetDataModel() == GetFirstWorld && (*DirectFirstService)->GetDataModel() == DirectFirstWorld,
			"service singletons are isolated between DataModels");

		auto OrdinaryWorld = std::make_shared<DataModel>();
		auto OrdinaryChild = std::make_shared<Folder>();
		OrdinaryChild->SetName("ReplicatedStorage");
		OrdinaryChild->SetParent(OrdinaryWorld);
		auto ServiceNameDecoy = std::make_shared<Folder>();
		ServiceNameDecoy->SetName("Tags");
		ServiceNameDecoy->SetParent(OrdinaryWorld);
		Check(Execute(OrdinaryWorld, R"(
			local Ordinary = game.ReplicatedStorage
			assert(Ordinary == game:FindFirstChild("ReplicatedStorage"))
			assert(game:FindService("ReplicatedStorage") == nil)
			assert(not pcall(function() game:GetService("ReplicatedStorage") end))
			assert(game.DefinitelyUnknown == nil)
			assert(game:FindService("DefinitelyUnknown") == nil)
			assert(not pcall(function() game:GetService("DefinitelyUnknown") end))
			local Decoy = game:FindFirstChild("Tags")
			local Canonical = game.Tags
			assert(Canonical:IsA("Tags") and Canonical ~= Decoy)
		)", "service-unknown-and-decoy"),
			"unknown names remain safe ordinary members while registered service identity ignores child Name decoys");
		Check(!OrdinaryWorld->FindService("ReplicatedStorage") && OrdinaryWorld->FindService("Tags") &&
			OrdinaryWorld->GetService("Tags") != ServiceNameDecoy,
			"unregistered child names never enter the canonical service registry");

		auto LoadedWorld = std::make_shared<DataModel>();
		auto LoadedTags = std::make_shared<Tags>();
		LoadedTags->SetName("LoadedTags");
		LoadedTags->SetParent(LoadedWorld);
		Check(!LoadedWorld->FindService("Tags") && LoadedWorld->GetService("Tags") == LoadedTags &&
			LoadedWorld->ResolveService("Tags") == LoadedTags && LoadedTags->GetName() == "LoadedTags",
			"an already-instantiated registered service in the provider scope becomes canonical by schema identity");

		auto DetachedWorld = std::make_shared<DataModel>();
		auto DetachedTags = std::make_shared<Tags>();
		DetachedTags->SetName("Tags");
		auto ConstructedTags = DetachedWorld->GetService("Tags");
		Check(ConstructedTags != DetachedTags && ConstructedTags->GetDataModel() == DetachedWorld,
			"a detached service object cannot become canonical by class or visible Name alone");
		ConstructedTags->SetParent(nullptr);
		Check(!DetachedWorld->FindService("Tags"), "detaching a canonical service clears non-constructing lookup state");
		auto ReplacementTags = DetachedWorld->GetService("Tags");
		Check(ReplacementTags != ConstructedTags && ReplacementTags->GetParent() &&
			ReplacementTags->GetParent()->get() == DetachedWorld.get(),
			"GetService replaces detached canonical state through the registered construction path");
		ReplacementTags->Destroy();
		Check(!DetachedWorld->FindService("Tags") && DetachedWorld->GetService("Tags") != ReplacementTags,
			"destroyed service state is discarded before lazy reconstruction");

		auto ForeignWorld = std::make_shared<DataModel>();
		auto ForeignTags = ForeignWorld->GetService("Tags");
		auto LocalWorld = std::make_shared<DataModel>();
		CheckThrows<std::invalid_argument>([&] { ForeignTags->SetParent(LocalWorld); },
			"a service owned by another DataModel cannot migrate into the local provider");
		Check(LocalWorld->GetService("Tags") != ForeignTags,
			"wrong-scope service objects cannot become canonical accidentally");

		std::weak_ptr<Instance> ReleasedService;
		{
			auto LifetimeWorld = std::make_shared<DataModel>();
			ReleasedService = LifetimeWorld->GetService("Tags");
			Check(!ReleasedService.expired(), "the ServiceProvider owns its constructed service lifetime");
		}
		Check(ReleasedService.expired(), "destroying the DataModel releases its canonical service ownership");
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

		auto parent = std::make_shared<Folder>();
		auto game = std::make_shared<DataModel>();
		auto workspace = std::dynamic_pointer_cast<Workspace>(game->GetService("Workspace"));
		parent->SetParent(workspace);
		ChangeJournal::Get().Clear();
		auto cursor = ChangeJournal::Get().CreateCursor(game->GetObjectId());
		second->SetParent(parent);
		second->SetName("CommittedName");
		second->Destroy();
		auto records = ChangeJournal::Get().Read(cursor).Records;
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

		auto foreignGame = std::make_shared<DataModel>();
		auto foreignWorkspace = std::dynamic_pointer_cast<Workspace>(foreignGame->GetService("Workspace"));
		auto owned = std::make_shared<Folder>();
		owned->SetParent(workspace);
		CheckThrows<std::invalid_argument>([&] { owned->SetParent(foreignWorkspace); },
			"an adopted Instance cannot migrate to a different DataModel");
		owned->SetParent(nullptr);
		Check(owned->GetDataModel() == game, "Parent nil retains the originating DataModel association");
		CheckThrows<std::invalid_argument>([&] { owned->SetParent(foreignWorkspace); },
			"unparenting cannot turn an adopted Instance into a cross-DataModel migration loophole");

		auto detachedRoot = std::make_shared<Folder>();
		auto detachedPart0 = std::make_shared<Part>();
		auto detachedPart1 = std::make_shared<Part>();
		auto detachedWeld = std::make_shared<WeldConstraint>();
		detachedPart0->SetParent(detachedRoot);
		detachedPart1->SetParent(detachedRoot);
		detachedWeld->SetPart0(detachedPart0);
		detachedWeld->SetPart1(detachedPart1);
		detachedWeld->SetParent(detachedRoot);
		detachedRoot->SetParent(workspace);
		Check(detachedPart0->GetDataModel() == game && detachedPart1->GetDataModel() == game &&
			detachedWeld->GetDataModel() == game,
			"first adoption validates and adopts a detached constraint subtree atomically");
		const auto OwnedBeforeDetachedDestroy = game->GetOwnedInstanceCount();
		detachedRoot->Destroy();
		Check(game->GetOwnedInstanceCount() + 4 == OwnedBeforeDetachedDestroy &&
			!detachedPart0->GetParent() && !detachedPart1->GetParent(),
			"destroying an adopted subtree releases hierarchy and DataModel ownership for every node");

		auto invalidRoot = std::make_shared<Folder>();
		auto invalidWeld = std::make_shared<WeldConstraint>();
		auto outsidePart = std::make_shared<Part>();
		invalidWeld->SetPart0(outsidePart);
		invalidWeld->SetParent(invalidRoot);
		CheckThrows<std::invalid_argument>([&] { invalidRoot->SetParent(workspace); },
			"first adoption rejects detached object references outside the adopted subtree");
		Check(!invalidRoot->GetParent() && !invalidRoot->GetDataModel() && !invalidWeld->GetDataModel(),
			"failed subtree adoption publishes no partial hierarchy ownership");

		auto deepRoot = std::make_shared<Folder>();
		auto deepLeaf = deepRoot;
		for (std::size_t Depth = 1; Depth < MaximumProtocolJsonDepth; ++Depth) {
			auto Child = std::make_shared<Folder>();
			Child->SetParent(deepLeaf);
			deepLeaf = std::move(Child);
		}
		CheckThrows<std::length_error>([&] { deepRoot->SetParent(workspace); },
			"first adoption rejects a subtree that exceeds the hierarchy depth limit");
		Check(!deepRoot->GetParent() && !deepRoot->GetDataModel(),
			"depth-limit rejection leaves the detached subtree unowned and unpublished");
	}

	void TestWorldRootConstraintValidation() {
		using namespace gargantuan;
		auto game = std::make_shared<DataModel>();
		auto workspace = std::dynamic_pointer_cast<Workspace>(game->GetService("Workspace"));
		Check(workspace != nullptr, "WorldRoot regression fixture obtains Workspace");
		if (!workspace) return;
		auto adoptedPart = std::make_shared<Part>();
		const auto bodyCountBeforeAdoption = WorldRootTestAccess::BodyCount(*workspace);
		adoptedPart->SetParent(workspace);
		Check(WorldRootTestAccess::BodyCount(*workspace) == bodyCountBeforeAdoption + 1,
			"first-adopted Part establishes its physics body");
		adoptedPart->SetParent(nullptr);
		Check(!adoptedPart->GetParent() && adoptedPart->GetDataModel() == game &&
			WorldRootTestAccess::BodyCount(*workspace) == bodyCountBeforeAdoption,
			"Parent nil removes physics presence while retaining runtime DataModel ownership");
		adoptedPart->SetParent(workspace);
		Check(WorldRootTestAccess::BodyCount(*workspace) == bodyCountBeforeAdoption + 1,
			"same-DataModel reparent restores physics presence");
		adoptedPart->Destroy();

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
		RuntimeSchemaRegistry AggregatePayload;
		auto LargeItems = [] {
			std::vector<SchemaEnumItem> Items;
			for (std::size_t Index = 0; Index < MaximumCustomEnumItems; ++Index) {
				auto Suffix = std::to_string(Index);
				Items.push_back({std::string(MaximumCustomEnumItemNameBytes - Suffix.size(), 'x') + Suffix,
					static_cast<std::int32_t>(Index)});
			}
			return Items;
		};
		AggregatePayload.RegisterEnum(MakeSchemaEnumDefinition("Game", "LargeA", 1, LargeItems()));
		AggregatePayload.RegisterEnum(MakeSchemaEnumDefinition("Game", "LargeB", 1, LargeItems()));
		CheckThrows<std::invalid_argument>(
			[&] { AggregatePayload.RegisterEnum(MakeSchemaEnumDefinition("Game", "LargeC", 1, LargeItems())); },
			"canonical registry enforces the aggregate custom schema payload limit"
		);

		const WireSchemaEnumValue value{enumId, 1, 1};
		using Json = nlohmann::ordered_json;
		const auto encodedText = EncodeWireValueJson(WireValue(value));
		Check(encodedText.has_value(), "custom enum WireValue JSON encodes through the public codec boundary");
		auto encoded = Json::parse(*encodedText);
		const auto decoded = DecodeWireValueJson(encoded.dump());
		Check(
			decoded && std::get_if<WireSchemaEnumValue>(&*decoded) &&
				*std::get_if<WireSchemaEnumValue>(&*decoded) == value,
			"custom enum value round-trips with stable schema identity and definition version"
		);
		auto malformedWire = encoded;
		malformedWire["SchemaId"] = "not-a-schema-id";
		Check(!DecodeWireValueJson(malformedWire.dump()), "malformed custom enum wire identity is rejected");
		auto OversizedUnsignedWire = encoded;
		OversizedUnsignedWire["Value"] = std::numeric_limits<std::uint64_t>::max();
		Check(!DecodeWireValueJson(OversizedUnsignedWire.dump()), "oversized unsigned custom enum wire values are rejected before narrowing");
		Check(DecodeWireObjectIdJson(Json{{"Slot", std::numeric_limits<std::uint32_t>::max()}, {"Generation", 1}}.dump()) &&
			!DecodeWireObjectIdJson(Json{{"Slot", std::uint64_t{4294967296}}, {"Generation", 1}}.dump()),
			"wire uint32 decoding accepts the exact maximum and rejects maximum plus one before narrowing");
		auto MaximumWire = encoded;
		MaximumWire["Value"] = std::numeric_limits<std::int32_t>::max();
		Check(DecodeWireValueJson(MaximumWire.dump()).has_value(), "signed 32-bit maximum custom enum wire value is accepted");
		auto MinimumWire = encoded;
		MinimumWire["Value"] = std::numeric_limits<std::int32_t>::min();
		Check(DecodeWireValueJson(MinimumWire.dump()).has_value(), "signed 32-bit minimum custom enum wire value is accepted");
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
		auto MutationTarget = std::make_shared<Folder>();
		Check(
			MutationTarget->ApplyPropertyMutation(
				"Name", std::string("Denied"), Enums::Permission::None, ScriptSecurityContext::PreRunRegistration()
			) == MutationStatus::Unauthorized,
			"DefineSchema alone cannot cross the DataModel mutation boundary"
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
		RuntimeSchemaLifecycle ExactSourceLimit;
		PreparePreRun(ExactSourceLimit);
		ExecutePreRunRegistration(
			ExactSourceLimit, authority, std::string(MaximumPreRunSourceBytes, ' '), "exact-source-limit"
		);
		Check(ExactSourceLimit.HasCandidate(), "PreRun source exactly at its byte limit is accepted");
		ExactSourceLimit.AbortCandidate(authority);

		auto BuildDefinitionLimitSource = [](std::size_t Count) {
			std::string Source;
			for (std::size_t Index = 0; Index < Count; ++Index) {
				Source += "Schema:RegisterEnum({Namespace='Game',Name='Limit" +
					std::to_string(Index) + "',Version=1,Items={Value=0}})\n";
			}
			return Source;
		};
		RuntimeSchemaLifecycle ExactDefinitionLimit;
		PreparePreRun(ExactDefinitionLimit);
		ExecutePreRunRegistration(
			ExactDefinitionLimit, authority, BuildDefinitionLimitSource(MaximumCustomEnumDefinitions), "exact-definition-limit"
		);
		Check(ExactDefinitionLimit.HasCandidate(), "PreRun enum count exactly at its limit is accepted");
		ExactDefinitionLimit.AbortCandidate(authority);

		const auto TooManyDefinitions = BuildDefinitionLimitSource(MaximumCustomEnumDefinitions + 1);
		RuntimeSchemaLifecycle definitionLimit;
		PreparePreRun(definitionLimit);
		CheckThrows<PreRunRegistrationError>(
			[&] { ExecutePreRunRegistration(definitionLimit, authority, TooManyDefinitions, "definition-limit"); },
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
		RuntimeSchemaLifecycle ExactItemLimit;
		PreparePreRun(ExactItemLimit);
		const auto ExactItems = "local items = {}\nfor index = 1, " +
			std::to_string(MaximumCustomEnumItems) +
			" do items['Item' .. index] = index end\nSchema:RegisterEnum({Namespace='Game',Name='ExactItems',Version=1,Items=items})";
		ExecutePreRunRegistration(ExactItemLimit, authority, ExactItems, "exact-item-limit");
		Check(ExactItemLimit.HasCandidate(), "PreRun enum item count exactly at its limit is accepted");
		ExactItemLimit.AbortCandidate(authority);

		auto BuildAggregatePayloadSource = [](std::size_t ShortItemCount) {
			return "local global = 0\nfor definition = 0, 15 do\n"
				"local items = {}\nfor item = 0, 255 do\nlocal itemName\n"
				"if global < " + std::to_string(ShortItemCount) +
				" then itemName = string.format('Item%07d', item) else itemName = string.format('Item%08d', item) end\n"
				"items[itemName] = item\nglobal += 1\nend\n"
				"Schema:RegisterEnum({Namespace='Game',Name='Payload' .. definition,Version=1,Items=items})\nend";
		};
		RuntimeSchemaLifecycle ExactAggregatePayload;
		PreparePreRun(ExactAggregatePayload);
		ExecutePreRunRegistration(
			ExactAggregatePayload, authority, BuildAggregatePayloadSource(198), "exact-aggregate-payload"
		);
		Check(ExactAggregatePayload.HasCandidate(), "PreRun aggregate payload exactly at its byte limit is accepted");
		ExactAggregatePayload.AbortCandidate(authority);
		RuntimeSchemaLifecycle OversizedAggregatePayload;
		PreparePreRun(OversizedAggregatePayload);
		CheckThrows<PreRunRegistrationError>(
			[&] {
				ExecutePreRunRegistration(
					OversizedAggregatePayload, authority, BuildAggregatePayloadSource(197), "oversized-aggregate-payload"
				);
			},
			"PreRun aggregate payload one byte over its limit is rejected"
		);

		for (const auto Source : {
			"Schema:RegisterEnum({Namespace='Game',Name='StringVersion',Version='1',Items={Value=1}})",
			"Schema:RegisterEnum({Namespace='Game',Name='StringItem',Version=1,Items={Value='1'}})",
		}) {
			RuntimeSchemaLifecycle StrictTypes;
			PreparePreRun(StrictTypes);
			CheckThrows<PreRunRegistrationError>(
				[&] { ExecutePreRunRegistration(StrictTypes, authority, Source, "strict-registration-types"); },
				"PreRun registration rejects numeric strings without coercion"
			);
			Check(!StrictTypes.HasCandidate(), "type rejection aborts the complete candidate");
		}

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
		PreparePreRun(transactional);
		const std::string RuntimeFailureBatch = R"(
			Schema:RegisterEnum({ Namespace="Game", Name="RuntimeA", Version=1, Items={ One=1 } })
			Schema:RegisterEnum({ Namespace="Game", Name="RuntimeB", Version=1, Items={ Two=2 } })
			error("abort after valid registrations")
		)";
		CheckThrows<PreRunRegistrationError>(
			[&] { ExecutePreRunRegistration(transactional, authority, RuntimeFailureBatch, "runtime-failure-batch"); },
			"a runtime failure after valid registrations aborts the complete PreRun transaction"
		);
		Check(
			transactional.GetActiveRegistry() == previous &&
				transactional.GetActiveGeneration() == previousGeneration &&
				previous->FindEnumByName("Game.RuntimeA") == nullptr && previous->FindEnumByName("Game.RuntimeB") == nullptr,
			"runtime failure leaks no registered definitions and does not advance generation"
		);
	}

	void TestClassExtensionSchema() {
		using namespace gargantuan;
		const auto extensionId = SchemaId::FromExtensionName("Game.Combat", "CombatProperties");
		Check(extensionId == SchemaId::FromExtensionName("Game.Combat", "CombatProperties"),
			"extension SchemaId is deterministic");
		Check(extensionId != SchemaId::FromNativeName("Game.Combat", "CombatProperties") &&
			extensionId != SchemaId::FromEnumName("Game.Combat", "CombatProperties"),
			"class, enum, and extension SchemaIds use distinct identity domains");

		RuntimeSchemaRegistry registry;
		registry.RegisterNative<SchemaTestTypeA>(MakeSchemaDefinition("Host"));
		registry.RegisterNative<SchemaTestTypeB>(MakeSchemaDefinition("Derived", "Host"));
		registry.RegisterNative<SchemaTestTypeC>(MakeSchemaDefinition("Unrelated"));
		registry.RegisterExtension(MakeSchemaExtensionDefinition(
			"Game.Combat", "CombatProperties", {
				{.Name = "Team", .Type = SchemaExtensionPropertyType::String, .DefaultValue = std::string()},
				{.Name = "Damage", .Type = SchemaExtensionPropertyType::Integer, .DefaultValue = 0},
			}
		), "Test.Host");
		registry.RegisterExtension(MakeSchemaExtensionDefinition(
			"Package.Vehicle", "VehicleProperties", {
				{.Name = "Fuel", .Type = SchemaExtensionPropertyType::Number, .DefaultValue = 1.0},
			}
		), "Test.Host");
		FreezeSchemaRegistry(registry);
		auto *extension = registry.FindExtensionById(extensionId);
		Check(extension && extension->TargetClassId == SchemaId::FromNativeName("Test", "Host") &&
			extension->Properties[0].Name == "Damage" && extension->Properties[1].Name == "Team",
			"extension target identity and property order are canonicalized in the frozen registry");
		Check(registry.IsExtensionApplicableToClass(extensionId, SchemaId::FromNativeName("Test", "Host")) &&
			registry.IsExtensionApplicableToClass(extensionId, SchemaId::FromNativeName("Test", "Derived")) &&
			!registry.IsExtensionApplicableToClass(extensionId, SchemaId::FromNativeName("Test", "Unrelated")),
			"extension applicability follows class inheritance without changing extension identity");
		Check(registry.FindApplicableExtensions(SchemaId::FromNativeName("Test", "Derived")).size() == 2,
			"multiple independent extensions apply deterministically to one derived class");
		Check(registry.FindClassById(extensionId) == nullptr && registry.FindEnumById(extensionId) == nullptr,
			"typed class and enum lookup reject extension definitions");

		RuntimeSchemaRegistry missingTarget;
		missingTarget.RegisterNative<SchemaTestTypeA>(MakeSchemaDefinition("Host"));
		CheckThrows<std::invalid_argument>([&] {
			missingTarget.RegisterExtension(MakeSchemaExtensionDefinition("Game", "Missing"), "Test.Absent");
		}, "extension registration rejects a nonexistent target");

		RuntimeSchemaRegistry wrongTarget;
		wrongTarget.RegisterNative<SchemaTestTypeA>(MakeSchemaDefinition("Host"));
		wrongTarget.RegisterEnum(MakeSchemaEnumDefinition("Game", "State"));
		CheckThrows<std::invalid_argument>([&] {
			wrongTarget.RegisterExtension(MakeSchemaExtensionDefinition("Game", "WrongKind"), "Game.State");
		}, "extension registration rejects an enum target");
		wrongTarget.RegisterExtension(MakeSchemaExtensionDefinition("Game", "First"), "Test.Host");
		CheckThrows<std::invalid_argument>([&] {
			wrongTarget.RegisterExtension(MakeSchemaExtensionDefinition("Game", "Second"), "Game.First");
		}, "extension registration rejects an extension target");

		RuntimeSchemaRegistry duplicateProperty;
		duplicateProperty.RegisterNative<SchemaTestTypeA>(MakeSchemaDefinition("Host"));
		CheckThrows<std::invalid_argument>([&] {
			duplicateProperty.RegisterExtension(MakeSchemaExtensionDefinition("Game", "DuplicateProperty", {
				{.Name = "Damage", .Type = SchemaExtensionPropertyType::Integer, .DefaultValue = 0},
				{.Name = "Damage", .Type = SchemaExtensionPropertyType::Integer, .DefaultValue = 1},
			}), "Test.Host");
		}, "duplicate extension property names are rejected");

		RuntimeSchemaRegistry invalidDefault;
		invalidDefault.RegisterNative<SchemaTestTypeA>(MakeSchemaDefinition("Host"));
		CheckThrows<std::invalid_argument>([&] {
			invalidDefault.RegisterExtension(MakeSchemaExtensionDefinition("Game", "InvalidDefault", {
				{.Name = "Damage", .Type = SchemaExtensionPropertyType::Integer, .DefaultValue = std::string("0")},
			}), "Test.Host");
		}, "extension property defaults must exactly match their declared type");

		RuntimeSchemaRegistry nativeCollision;
		auto protectedTarget = MakeSchemaDefinition("Host");
		protectedTarget.Properties.emplace("Name", MakeReadOnlySchemaProperty("Name"));
		nativeCollision.RegisterNative<SchemaTestTypeA>(std::move(protectedTarget));
		nativeCollision.RegisterExtension(MakeSchemaExtensionDefinition("Game", "Collision", {
			{.Name = "Name", .Type = SchemaExtensionPropertyType::String, .DefaultValue = std::string()},
		}), "Test.Host");
		CheckThrows<std::invalid_argument>([&] { nativeCollision.Validate(); },
			"extension validation rejects shadowing a protected native member");

		const auto &authority = GetRuntimeSchemaBootstrapAuthority();
		auto Prepare = [&](RuntimeSchemaLifecycle &lifecycle) {
			lifecycle.BeginCandidate(authority);
			lifecycle.RegisterNative<SchemaTestTypeA>(authority, MakeSchemaDefinition("PreRunHost"));
			lifecycle.AdvanceRegistrationPhase(authority, RuntimeSchemaLifecyclePhase::CoreRegistration);
			lifecycle.AdvanceRegistrationPhase(authority, RuntimeSchemaLifecyclePhase::PreRunRegistration);
		};
		RuntimeSchemaLifecycle atomic;
		Prepare(atomic);
		CheckThrows<PreRunRegistrationError>([&] {
			ExecutePreRunRegistration(atomic, authority, R"(
				Schema:RegisterEnum({ Namespace="Game", Name="PublishedNever", Version=1, Items={ A=1 } })
				Schema:RegisterExtension({ Namespace="Game", Name="ValidNever", Version=1, Target="Test.PreRunHost",
					Properties={ Damage={ Type="Integer", Default=0 } } })
				Schema:RegisterExtension({ Namespace="Game", Name="Invalid", Version=1, Target="Test.Missing",
					Properties={ Damage={ Type="Integer", Default=0 } } })
			)", "extension-atomicity");
		}, "one invalid extension aborts enum and extension registration together");
		Check(!atomic.HasCandidate() && !atomic.HasActiveRegistry(),
			"failed mixed PreRun registration publishes no partial candidate");

		RuntimeSchemaLifecycle denied;
		Prepare(denied);
		CheckThrows<PreRunRegistrationError>([&] {
			ExecutePreRunRegistration(denied, authority, R"(
				Schema:RegisterExtension({ Namespace="Game", Name="Denied", Version=1, Target="Test.PreRunHost",
					Properties={ Damage={ Type="Integer", Default=0 } } })
			)", "extension-capability", {ScriptExecutionDomain::PreRun, {}});
		}, "PreRun domain without DefineSchema cannot register an extension");

		RuntimeSchemaLifecycle Malformed;
		Prepare(Malformed);
		try {
			ExecutePreRunRegistration(Malformed, authority, R"(
				Schema:RegisterExtension({ Namespace="Game", Name="BadDefault", Version=1, Target="Test.PreRunHost",
					Properties={ Damage={ Type="Integer", Default="0" } } })
			)", "extension-malformed-diagnostic");
			Check(false, "malformed extension registration is rejected");
		} catch (const PreRunRegistrationError &Error) {
			Check(Error.GetDiagnostic().Code == PreRunDiagnosticCode::SchemaRegistrationError &&
				Error.GetDiagnostic().Definition == "Game.BadDefault",
				"malformed extension registration reports its schema failure class and definition context");
		}

		RuntimeSchemaLifecycle LaterRuntimeFailure;
		Prepare(LaterRuntimeFailure);
		try {
			ExecutePreRunRegistration(LaterRuntimeFailure, authority, R"(
				Schema:RegisterExtension({ Namespace="Game", Name="ValidThenAbort", Version=1, Target="Test.PreRunHost",
					Properties={ Damage={ Type="Integer", Default=0 } } })
				error("abort after extension registration")
			)", "extension-runtime-diagnostic");
			Check(false, "runtime failure after extension registration is rejected");
		} catch (const PreRunRegistrationError &Error) {
			Check(Error.GetDiagnostic().Code == PreRunDiagnosticCode::RuntimeError &&
				Error.GetDiagnostic().Definition.empty(),
				"a later script failure is not misclassified as an extension registration failure");
		}
	}

	void TestCustomClassSchema() {
		using namespace gargantuan;
		const auto ClassId = SchemaId::FromCustomClassName("Game", "DamageableFolder");
		Check(ClassId == SchemaId::FromCustomClassName("Game", "DamageableFolder") &&
			ClassId != SchemaId::FromCustomClassName("Package", "DamageableFolder") &&
			ClassId != SchemaId::FromCustomClassName("Game", "OtherFolder"),
			"custom class identity is deterministic and namespace/name sensitive");
		Check(ClassId != SchemaId::FromNativeName("Game", "DamageableFolder") &&
			ClassId != SchemaId::FromEnumName("Game", "DamageableFolder") &&
			ClassId != SchemaId::FromExtensionName("Game", "DamageableFolder"),
			"native class, custom class, enum, and extension identities are domain separated");

		RuntimeSchemaRegistry Registry;
		Registry.RegisterNative<SchemaTestTypeA>(MakeCustomSubclassableHost("Host"));
		Registry.RegisterClass(MakeCustomClassDefinition("Game", "Child", {
			{.Name = "Faction", .Type = SchemaExtensionPropertyType::String, .DefaultValue = std::string()},
		}), "Game.Parent");
		Registry.RegisterClass(MakeCustomClassDefinition("Game", "Parent", {
			{.Name = "Health", .Type = SchemaExtensionPropertyType::Integer, .DefaultValue = 100},
			{.Name = "Enabled", .Type = SchemaExtensionPropertyType::Boolean, .DefaultValue = true},
		}), "Test.Host");
		FreezeSchemaRegistry(Registry);
		const auto *Parent = Registry.FindClassByName("Game.Parent");
		const auto *Child = Registry.FindClassByName("Game.Child");
		Check(Parent && Child && Parent->ConstructionKind == SchemaClassConstructionKind::CustomData &&
			Child->BaseSchemaId == Parent->Id && Child->NativeHostClassId == SchemaId::FromNativeName("Test", "Host") &&
			Child->InheritedClassIds.contains(Parent->Id) && Child->InheritedClassIds.contains(Child->Id),
			"whole-candidate validation resolves ordered custom inheritance to one approved native host");
		Check(Child && Child->AllProperties.contains("Health") && Child->AllProperties.contains("Faction") &&
			Child->AllProperties.at("Health")->DeclaringSchemaId == Parent->Id &&
			Registry.IsClassConstructible(*Child),
			"custom classes inherit bounded declarative properties without changing their declaring identity");

		RuntimeSchemaRegistry MissingBase;
		MissingBase.RegisterNative<SchemaTestTypeA>(MakeCustomSubclassableHost("Host"));
		MissingBase.RegisterClass(MakeCustomClassDefinition("Game", "MissingBase"), "Game.Absent");
		CheckThrows<std::invalid_argument>([&] { MissingBase.Validate(); },
			"custom class validation rejects a missing base");

		RuntimeSchemaRegistry ForbiddenBase;
		ForbiddenBase.RegisterNative<SchemaTestTypeA>(MakeSchemaDefinition("Forbidden"));
		ForbiddenBase.RegisterClass(MakeCustomClassDefinition("Game", "Denied"), "Test.Forbidden");
		CheckThrows<std::invalid_argument>([&] { ForbiddenBase.Validate(); },
			"custom class validation rejects a native base without data-only subclass policy");

		RuntimeSchemaRegistry Cycle;
		Cycle.RegisterNative<SchemaTestTypeA>(MakeCustomSubclassableHost("Host"));
		Cycle.RegisterClass(MakeCustomClassDefinition("Game", "A"), "Game.B");
		Cycle.RegisterClass(MakeCustomClassDefinition("Game", "B", {
			{.Name = "Other", .Type = SchemaExtensionPropertyType::Integer, .DefaultValue = 0},
		}), "Game.A");
		CheckThrows<std::invalid_argument>([&] { Cycle.Validate(); },
			"custom-on-custom inheritance cycles reject the complete candidate");

		RuntimeSchemaRegistry DuplicateProperty;
		DuplicateProperty.RegisterNative<SchemaTestTypeA>(MakeCustomSubclassableHost("Host"));
		CheckThrows<std::invalid_argument>([&] {
			DuplicateProperty.RegisterClass(MakeCustomClassDefinition("Game", "Duplicate", {
				{.Name = "Value", .Type = SchemaExtensionPropertyType::Integer, .DefaultValue = 0},
				{.Name = "Value", .Type = SchemaExtensionPropertyType::Integer, .DefaultValue = 1},
			}), "Test.Host");
		}, "custom class registration rejects duplicate property identities");

		RuntimeSchemaRegistry InvalidDefault;
		InvalidDefault.RegisterNative<SchemaTestTypeA>(MakeCustomSubclassableHost("Host"));
		CheckThrows<std::invalid_argument>([&] {
			InvalidDefault.RegisterClass(MakeCustomClassDefinition("Game", "InvalidDefault", {
				{.Name = "Value", .Type = SchemaExtensionPropertyType::Integer, .DefaultValue = std::string("0")},
			}), "Test.Host");
		}, "custom class defaults must exactly match their declared scalar type");

		RuntimeSchemaRegistry ProtectedNamespace;
		ProtectedNamespace.RegisterNative<SchemaTestTypeA>(MakeCustomSubclassableHost("Host"));
		CheckThrows<std::invalid_argument>([&] {
			ProtectedNamespace.RegisterClass(MakeCustomClassDefinition("Engine", "Impostor"), "Test.Host");
		}, "project classes cannot impersonate the protected Engine namespace");

		RuntimeSchemaRegistry NativeCollision;
		auto NativeHost = MakeCustomSubclassableHost("Host");
		NativeHost.Properties.emplace("Name", MakeReadOnlySchemaProperty("Name"));
		NativeCollision.RegisterNative<SchemaTestTypeA>(std::move(NativeHost));
		NativeCollision.RegisterClass(MakeCustomClassDefinition("Game", "Collision", {
			{.Name = "Name", .Type = SchemaExtensionPropertyType::String, .DefaultValue = std::string()},
		}), "Test.Host");
		CheckThrows<std::invalid_argument>([&] { NativeCollision.Validate(); },
			"custom classes cannot shadow inherited protected native members");

		RuntimeSchemaRegistry InheritedCollision;
		InheritedCollision.RegisterNative<SchemaTestTypeA>(MakeCustomSubclassableHost("Host"));
		InheritedCollision.RegisterClass(MakeCustomClassDefinition("Game", "Base", {
			{.Name = "Value", .Type = SchemaExtensionPropertyType::Integer, .DefaultValue = 0},
		}), "Test.Host");
		InheritedCollision.RegisterClass(MakeCustomClassDefinition("Game", "Derived", {
			{.Name = "Value", .Type = SchemaExtensionPropertyType::Integer, .DefaultValue = 1},
		}), "Game.Base");
		CheckThrows<std::invalid_argument>([&] { InheritedCollision.Validate(); },
			"custom classes cannot shadow inherited custom properties");

		RuntimeSchemaRegistry CanonicalCollision;
		CanonicalCollision.RegisterNative<SchemaTestTypeA>(MakeCustomSubclassableHost("Host"));
		CanonicalCollision.RegisterEnum(MakeSchemaEnumDefinition("Game", "SharedIdentity"));
		CheckThrows<std::invalid_argument>([&] {
			CanonicalCollision.RegisterClass(
				MakeCustomClassDefinition("Game", "SharedIdentity"), "Test.Host"
			);
		}, "custom class and enum canonical-name collisions fail independently of their separated SchemaIds");

		auto MakeProperties = [](std::size_t Count) {
			std::vector<SchemaClassProperty> Properties;
			Properties.reserve(Count);
			for (std::size_t Index = 0; Index < Count; ++Index)
				Properties.push_back({
					.Name = "Value" + std::to_string(Index),
					.Type = SchemaExtensionPropertyType::Integer,
					.DefaultValue = static_cast<int>(Index),
				});
			return Properties;
		};
		RuntimeSchemaRegistry ExactPropertyLimit;
		ExactPropertyLimit.RegisterNative<SchemaTestTypeA>(MakeCustomSubclassableHost("Host"));
		ExactPropertyLimit.RegisterClass(MakeCustomClassDefinition(
			"Game", "ExactProperties", MakeProperties(MaximumCustomClassProperties)
		), "Test.Host");
		FreezeSchemaRegistry(ExactPropertyLimit);
		Check(
			ExactPropertyLimit.FindClassByName("Game.ExactProperties")->DeclaredCustomProperties.size() ==
				MaximumCustomClassProperties,
			"custom class property count exactly at its canonical limit is accepted"
		);
		RuntimeSchemaRegistry PropertyOverflow;
		PropertyOverflow.RegisterNative<SchemaTestTypeA>(MakeCustomSubclassableHost("Host"));
		CheckThrows<std::invalid_argument>([&] {
			PropertyOverflow.RegisterClass(MakeCustomClassDefinition(
				"Game", "TooManyProperties", MakeProperties(MaximumCustomClassProperties + 1)
			), "Test.Host");
		}, "custom class property count over its canonical limit is rejected");

		RuntimeSchemaRegistry ExactClassLimit;
		ExactClassLimit.RegisterNative<SchemaTestTypeA>(MakeCustomSubclassableHost("Host"));
		for (std::size_t Index = 0; Index < MaximumCustomClassDefinitions; ++Index)
			ExactClassLimit.RegisterClass(
				MakeCustomClassDefinition("Game", "Limit" + std::to_string(Index)), "Test.Host"
			);
		FreezeSchemaRegistry(ExactClassLimit);
		Check(
			ExactClassLimit.EnumerateClasses().size() == MaximumCustomClassDefinitions + 1,
			"custom class definition count exactly at its canonical limit is accepted"
		);
		RuntimeSchemaRegistry ClassOverflow;
		ClassOverflow.RegisterNative<SchemaTestTypeA>(MakeCustomSubclassableHost("Host"));
		for (std::size_t Index = 0; Index < MaximumCustomClassDefinitions; ++Index)
			ClassOverflow.RegisterClass(
				MakeCustomClassDefinition("Game", "Limit" + std::to_string(Index)), "Test.Host"
			);
		CheckThrows<std::invalid_argument>([&] {
			ClassOverflow.RegisterClass(MakeCustomClassDefinition("Game", "Overflow"), "Test.Host");
		}, "custom class definition count over its canonical limit is rejected");

		RuntimeSchemaRegistry ExactDepth;
		ExactDepth.RegisterNative<SchemaTestTypeA>(MakeCustomSubclassableHost("Host"));
		std::string BaseName = "Test.Host";
		for (std::size_t Index = 0; Index < MaximumCustomClassInheritanceDepth - 1; ++Index) {
			const auto Name = "Depth" + std::to_string(Index);
			ExactDepth.RegisterClass(MakeCustomClassDefinition("Game", Name, {{
				.Name = "Value" + std::to_string(Index),
				.Type = SchemaExtensionPropertyType::Integer,
				.DefaultValue = 0,
			}}), BaseName);
			BaseName = "Game." + Name;
		}
		FreezeSchemaRegistry(ExactDepth);
		Check(
			ExactDepth.FindClassByName(BaseName) != nullptr,
			"custom inheritance depth exactly at its canonical limit is accepted"
		);
		RuntimeSchemaRegistry DepthOverflow;
		DepthOverflow.RegisterNative<SchemaTestTypeA>(MakeCustomSubclassableHost("Host"));
		BaseName = "Test.Host";
		for (std::size_t Index = 0; Index < MaximumCustomClassInheritanceDepth; ++Index) {
			const auto Name = "Depth" + std::to_string(Index);
			DepthOverflow.RegisterClass(MakeCustomClassDefinition("Game", Name, {{
				.Name = "Value" + std::to_string(Index),
				.Type = SchemaExtensionPropertyType::Integer,
				.DefaultValue = 0,
			}}), BaseName);
			BaseName = "Game." + Name;
		}
		CheckThrows<std::invalid_argument>([&] { DepthOverflow.Validate(); },
			"custom inheritance depth over its canonical limit is rejected");

		RuntimeSchemaRegistry EmbeddedNull;
		EmbeddedNull.RegisterNative<SchemaTestTypeA>(MakeCustomSubclassableHost("Host"));
		CheckThrows<std::invalid_argument>([&] {
			EmbeddedNull.RegisterClass(MakeCustomClassDefinition("Game", "EmbeddedNull", {{
				.Name = std::string("Bad\0Name", 8),
				.Type = SchemaExtensionPropertyType::Integer,
				.DefaultValue = 0,
			}}), "Test.Host");
		}, "custom class property names reject embedded nulls");

		const auto &Authority = GetRuntimeSchemaBootstrapAuthority();
		auto Prepare = [&](RuntimeSchemaLifecycle &Lifecycle) {
			Lifecycle.BeginCandidate(Authority);
			Lifecycle.RegisterNative<SchemaTestTypeA>(Authority, MakeCustomSubclassableHost("Host"));
			Lifecycle.AdvanceRegistrationPhase(Authority, RuntimeSchemaLifecyclePhase::CoreRegistration);
			Lifecycle.AdvanceRegistrationPhase(Authority, RuntimeSchemaLifecyclePhase::PreRunRegistration);
		};
		RuntimeSchemaLifecycle Denied;
		Prepare(Denied);
		CheckThrows<std::runtime_error>([&] {
			Denied.RegisterClass(Authority, MakeCustomClassDefinition("Game", "Denied"), "Test.Host",
				{ScriptExecutionDomain::PreRun, {}});
		}, "PreRun domain without DefineSchema cannot register a custom class");
		Check(!Denied.HasCandidate(), "capability-denied custom registration discards the hidden candidate");

		RuntimeSchemaLifecycle Atomic;
		Prepare(Atomic);
		ExecutePreRunRegistration(Atomic, Authority, R"(
			Schema:RegisterEnum({ Namespace="Game", Name="NeverPublished", Version=1, Items={ Value=0 } })
			Schema:RegisterClass({ Namespace="Game", Name="A", Version=1, Base="Game.B",
				Properties={ Value={ Type="Integer", Default=0 } } })
			Schema:RegisterClass({ Namespace="Game", Name="B", Version=1, Base="Game.A",
				Properties={ Other={ Type="Integer", Default=0 } } })
		)", "custom-class-cycle");
		Atomic.AdvanceRegistrationPhase(Authority, RuntimeSchemaLifecyclePhase::Validation);
		CheckThrows<std::runtime_error>([&] { Atomic.ValidateCandidate(Authority); },
			"late custom inheritance validation failure aborts the complete mixed candidate");
		Check(!Atomic.HasCandidate() && !Atomic.HasActiveRegistry(),
			"failed custom class candidate publishes no enum, class, generation, or partial state");

		auto BuildClassLimitSource = [](std::size_t Count) {
			return "for index = 1, " + std::to_string(Count) + " do "
				"Schema:RegisterClass({Namespace='Game',Name='Class' .. index,Version=1,Base='Test.Host',"
				"Properties={Value={Type='Integer',Default=0}}}) end";
		};
		RuntimeSchemaLifecycle ExactPreRunClassLimit;
		Prepare(ExactPreRunClassLimit);
		ExecutePreRunRegistration(
			ExactPreRunClassLimit, Authority,
			BuildClassLimitSource(MaximumCustomClassDefinitions), "exact-custom-class-limit"
		);
		Check(ExactPreRunClassLimit.HasCandidate(),
			"PreRun custom class count exactly at its facade limit is accepted");
		ExactPreRunClassLimit.AbortCandidate(Authority);

		RuntimeSchemaLifecycle PreRunClassOverflow;
		Prepare(PreRunClassOverflow);
		CheckThrows<PreRunRegistrationError>([&] {
			ExecutePreRunRegistration(
				PreRunClassOverflow, Authority,
				BuildClassLimitSource(MaximumCustomClassDefinitions + 1), "custom-class-limit-overflow"
			);
		}, "PreRun custom class count over its facade limit aborts registration");
		Check(!PreRunClassOverflow.HasCandidate(),
			"PreRun custom class count overflow leaks no earlier definitions");

		auto BuildPropertyLimitSource = [](std::size_t Count) {
			return "local properties = {} for index = 1, " + std::to_string(Count) + " do "
				"properties['Value' .. index] = {Type='Integer',Default=0} end "
				"Schema:RegisterClass({Namespace='Game',Name='PropertyLimit',Version=1,Base='Test.Host',"
				"Properties=properties})";
		};
		RuntimeSchemaLifecycle ExactPreRunPropertyLimit;
		Prepare(ExactPreRunPropertyLimit);
		ExecutePreRunRegistration(
			ExactPreRunPropertyLimit, Authority,
			BuildPropertyLimitSource(MaximumCustomClassProperties), "exact-custom-property-limit"
		);
		Check(ExactPreRunPropertyLimit.HasCandidate(),
			"PreRun custom property count exactly at its facade limit is accepted");
		ExactPreRunPropertyLimit.AbortCandidate(Authority);

		RuntimeSchemaLifecycle PreRunPropertyOverflow;
		Prepare(PreRunPropertyOverflow);
		CheckThrows<PreRunRegistrationError>([&] {
			ExecutePreRunRegistration(
				PreRunPropertyOverflow, Authority,
				BuildPropertyLimitSource(MaximumCustomClassProperties + 1), "custom-property-limit-overflow"
			);
		}, "PreRun custom property count over its facade limit aborts registration");
		Check(!PreRunPropertyOverflow.HasCandidate(),
			"PreRun custom property count overflow publishes no partial class");

		for (const auto Source : {
			"Schema:RegisterClass({Namespace='Game',Name='StringVersion',Version='1',Base='Test.Host',Properties={Value={Type='Integer',Default=0}}})",
			"Schema:RegisterClass({Namespace='Game',Name='Callback',Version=1,Base='Test.Host',Properties={Value={Type='Integer',Default=0}},Callback=function() end})",
		}) {
			RuntimeSchemaLifecycle StrictClassPayload;
			Prepare(StrictClassPayload);
			CheckThrows<PreRunRegistrationError>([&] {
				ExecutePreRunRegistration(
					StrictClassPayload, Authority, Source, "strict-custom-class-payload"
				);
			}, "PreRun custom class registration rejects wrong types and behavior fields");
			Check(!StrictClassPayload.HasCandidate(),
				"strict custom class payload rejection aborts the complete candidate");
		}
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

		for (const auto Runtime : {ScriptSecurityContext::ServerRuntime(), ScriptSecurityContext::ClientRuntime()}) {
			Check(
				Runtime.HasCapability(ScriptCapability::ReadDataModel) &&
					Runtime.HasCapability(ScriptCapability::MutateDataModel),
				"ordinary runtime scripts receive gameplay DataModel capabilities"
			);
			Check(
				!Runtime.HasCapability(ScriptCapability::EditorCommands) &&
					!Runtime.HasCapability(ScriptCapability::FilesystemRead) &&
					!Runtime.HasCapability(ScriptCapability::FilesystemWrite) &&
					!Runtime.HasCapability(ScriptCapability::ProcessControl) &&
					!Runtime.HasCapability(ScriptCapability::DefineSchema),
				"runtime scripts do not inherit editor, filesystem, process, or schema authority"
			);
		}
		auto Process = std::make_shared<ProcessService>();
		{
			ScriptSecurityScope RuntimeScope(ScriptSecurityContext::ClientRuntime());
			CheckThrows<std::runtime_error>(
				[&] { Process->FlushStdout(); }, "ProcessService enforces ProcessControl for ordinary runtime scripts"
			);
		}

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

		auto ProjectRoot = std::make_shared<DataModel>();
		auto parent = std::make_shared<Folder>();
		parent->SetParent(ProjectRoot);
		const auto parentId = parent->GetObjectId();
		journal.Clear();
		auto created = gateway.Apply(CreateObjectCommand{
			SchemaId::FromNativeName("Engine", "Folder"), 1, parentId, std::nullopt
		});
		Check(created.Succeeded() && created.Object.has_value(), "create command constructs an owned object");
		auto createdObject = ObjectRegistry::Get().Lookup(*created.Object);
		Check(createdObject && createdObject->GetParent() == parent, "created object is attached to its authoritative parent");
		auto otherParent = std::make_shared<Folder>();
		otherParent->SetParent(ProjectRoot);
		const auto otherParentId = otherParent->GetObjectId();
		journal.Clear();
		const auto ReparentCursor = journal.CreateCursor(ProjectRoot->GetObjectId());
		auto reparented = gateway.Apply(ReparentObjectCommand{*created.Object, otherParentId});
		Check(reparented.Succeeded() && createdObject->GetParent() == otherParent, "reparent command applies on Main");
		Check(journal.Read(ReparentCursor).Records.size() == 1,
			"reparent command emits one committed record");
		auto CycleResult = gateway.Apply(ReparentObjectCommand{otherParentId, *created.Object});
		Check(CycleResult.Status == MutationStatus::InvalidParent &&
			CycleResult.Message.find("hierarchy cycle") != std::string::npos,
			"reparent diagnostics distinguish hierarchy cycles");
		auto ForeignWorld = std::make_shared<DataModel>();
		auto ForeignParent = std::make_shared<Folder>();
		ForeignParent->SetParent(ForeignWorld);
		auto CrossScopeResult = gateway.Apply(ReparentObjectCommand{*created.Object, ForeignParent->GetObjectId()});
		Check(CrossScopeResult.Status == MutationStatus::InvalidParent &&
			CrossScopeResult.Message.find("different DataModel") != std::string::npos,
			"reparent diagnostics distinguish cross-DataModel targets");
		journal.Clear();
		auto destroyed = gateway.Apply(DestroyObjectCommand{*created.Object});
		Check(destroyed.Succeeded() && createdObject->GetDestroyed(), "destroy command applies on Main");
		Check(!ObjectRegistry::Get().Lookup(*created.Object), "destroy command invalidates lookup");

		auto ScriptValue = std::make_shared<ModuleScript>();
		ScriptValue->SetParent(ProjectRoot);
		const auto RevisionBeforeInvalidSource = ProjectRoot->GetAuthoritativeRevision();
		bool InvalidUtf8Rejected = false;
		try { ScriptValue->SetSource(std::string("\xC0", 1)); }
		catch (const std::invalid_argument &) { InvalidUtf8Rejected = true; }
		Check(InvalidUtf8Rejected && ScriptValue->GetSource().empty() &&
			ProjectRoot->GetAuthoritativeRevision() == RevisionBeforeInvalidSource,
			"script source rejects malformed UTF-8 without changing authoritative state");

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
		CheckThrows<std::invalid_argument>([&] { second->SetParent(otherScope); },
			"replicated Instances cannot migrate across DataModels");
		second->Destroy();
		Check(session->ApplyAvailable().Succeeded(), "scoped destruction applies in source order");
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
		const WireVector2 Offset{10.0f, 20.0f};
		Check(gateway.Apply(UpdateAttributeCommand{id, "Offset", WireValue(Offset)}).Succeeded() &&
			part->GetAttributeValue("Offset") == std::optional<WireValue>(Offset),
			"Vector2 attribute mutation retains its closed WireValue representation");
		const auto EncodedOffset = EncodeWireValueJson(WireValue(Offset));
		const auto DecodedOffset = EncodedOffset ? DecodeWireValueJson(*EncodedOffset) : SerializationResult<WireValue>(
			SerializationFailure(SerializationErrorCode::InternalFailure, "encode failed")
		);
		Check(DecodedOffset && *DecodedOffset == WireValue(Offset),
			"Vector2 WireValue encoding round-trips without a format change");
		const auto NativeOffset = DecodeNativeWireValue(WireValue(Offset));
		const auto *DecodedNativeOffset = NativeOffset ? std::any_cast<Vector2>(&*NativeOffset) : nullptr;
		Check(DecodedNativeOffset && DecodedNativeOffset->GetX() == 10.0f && DecodedNativeOffset->GetY() == 20.0f &&
			EncodeNativeWireValue(std::any(*DecodedNativeOffset)) == std::optional<WireValue>(Offset),
			"Vector2 tagged userdata materializes through the existing WireValue codec");
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
		Check(deserializedPart && deserializedPart->GetAttributeValue("Offset") == std::optional<WireValue>(Offset),
			"Vector2 attributes persist without changing their wire meaning");
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
		Check(snapshotPart != snapshot.Objects.end() && snapshotPart->Attributes.at("Offset") == WireValue(Offset),
			"snapshot contains Vector2 attribute state");
		auto parsedSnapshot = DeserializeSnapshot(SerializeSnapshot(snapshot));
		Check(parsedSnapshot.Succeeded() && LoadSnapshot(*parsedSnapshot.Value).Succeeded(), "attribute snapshot parses and loads");

		auto sessionStart = InProcessReplicationSession::Start(game);
		Check(sessionStart.Succeeded(), "attribute replication session starts from snapshot state");
		auto session = sessionStart.Session;
		auto receiverPart = session ? session->GetReceiverRoot()->FindFirstChild("AttributedPart", false) : nullptr;
		Check(receiverPart && receiverPart->GetAttributeValue("Persisted") == std::optional<WireValue>(std::string("value")),
			"initial attribute state replicates");
		Check(receiverPart && receiverPart->GetAttributeValue("Offset") == std::optional<WireValue>(Offset),
			"initial Vector2 attribute state replicates");
		Check(gateway.Apply(UpdateAttributeCommand{id, "Persisted", WireValue(std::string("updated"))}).Succeeded() &&
			session->ApplyAvailable().Succeeded(), "attribute update replicates through ordered journal");
		Check(receiverPart->GetAttributeValue("Persisted") == std::optional<WireValue>(std::string("updated")),
			"receiver observes replicated attribute update");
		const WireVector2 UpdatedOffset{-5.0f, 4.0f};
		Check(gateway.Apply(UpdateAttributeCommand{id, "Offset", WireValue(UpdatedOffset)}).Succeeded() &&
			session->ApplyAvailable().Succeeded() &&
			receiverPart->GetAttributeValue("Offset") == std::optional<WireValue>(UpdatedOffset),
			"Vector2 attribute journal updates replicate without coercion");
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
		auto DetachedTaggedOverflow = std::make_shared<Folder>();
		DetachedTaggedOverflow->SetDetachedTagsForAdoption({"DetachedDistinctOverflow"});
		const auto OwnedBeforeRejectedAdoption = distinctGame->GetOwnedInstanceCount();
		CheckThrows<std::length_error>([&] { DetachedTaggedOverflow->SetParent(distinctGame); },
			"detached tag adoption preflights the target DataModel distinct-tag limit");
		Check(!DetachedTaggedOverflow->GetDataModel() && !DetachedTaggedOverflow->GetParent() &&
			distinctGame->GetOwnedInstanceCount() == OwnedBeforeRejectedAdoption,
			"rejected detached tag adoption leaves ownership, hierarchy, and object count unchanged");

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
		CheckThrows<std::invalid_argument>([&] { movingAncestor->SetParent(otherGame); },
			"an adopted tagged subtree cannot migrate between DataModels");
		Check(game->Tags.Has(scope, movingDescendantId, "OldWorld", ScriptSecurityContext::CoreTrusted()),
			"rejected cross-DataModel parenting preserves the authoritative tag membership");

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

	void TestClassExtensionRuntime() {
		using namespace gargantuan;
		const auto temporaryRoot = std::filesystem::temp_directory_path() /
			("gargantuan-extension-runtime-" + std::to_string(
				std::chrono::steady_clock::now().time_since_epoch().count()
			));
		struct TemporaryExtensionCleanup {
			std::filesystem::path Root;
			~TemporaryExtensionCleanup() { std::filesystem::remove_all(Root); }
		} cleanup{temporaryRoot};
		std::filesystem::create_directories(temporaryRoot / ".gargantuan");
		std::ofstream preRun(temporaryRoot / ".gargantuan" / "prerun.luau", std::ios::binary);
		preRun << R"(
			Schema:RegisterExtension({
				Namespace = "Game.Combat",
				Name = "CombatProperties",
				Version = 1,
				Target = "Engine.BasePart",
				Properties = {
					Team = { Type = "String", Default = "" },
					Damage = { Type = "Integer", Default = 0 },
				},
			})
			Schema:RegisterExtension({
				Namespace = "Game.Data",
				Name = "FolderProperties",
				Version = 1,
				Target = "Engine.Folder",
				Properties = { Label = { Type = "String", Default = "" } },
			})
			Schema:RegisterClass({
				Namespace = "Game",
				Name = "CombatFolder",
				Version = 1,
				Base = "Game.DamageableFolder",
				Properties = { Faction = { Type = "String", Default = "Neutral" } },
			})
			Schema:RegisterClass({
				Namespace = "Game",
				Name = "DamageableFolder",
				Version = 1,
				Base = "Engine.Folder",
				Properties = {
					Enabled = { Type = "Boolean", Default = true },
					Health = { Type = "Integer", Default = 100 },
					Rating = { Type = "Number", Default = 1.5 },
					Title = { Type = "String", Default = "Unit" },
				},
			})
		)";
		for (std::size_t ExtensionIndex = 0; ExtensionIndex < 3; ++ExtensionIndex) {
			preRun << "Schema:RegisterExtension({ Namespace='Game.Bounds', Name='Dense" << ExtensionIndex <<
				"', Version=1, Target='Engine.BasePart', Properties={";
			const auto PropertyCount = ExtensionIndex < 2 ? MaximumExtensionProperties : 1;
			for (std::size_t PropertyIndex = 0; PropertyIndex < PropertyCount; ++PropertyIndex)
				preRun << "Value" << PropertyIndex << "={ Type='Integer', Default=0 },";
			preRun << "} })\n";
		}
		preRun.close();
		BootstrapProjectRuntimeSchema(temporaryRoot);
		const auto extensionId = SchemaId::FromExtensionName("Game.Combat", "CombatProperties");
		auto *extension = GetActiveRuntimeSchemaRegistry().FindExtensionById(extensionId);
		Check(extension && extension->TargetClassId == SchemaId::FromNativeName("Engine", "BasePart"),
			"project PreRun publishes one canonical extension targeting stable class identity");

		auto game = std::make_shared<DataModel>();
		game->SetArchivable(true);
		auto part = std::make_shared<Part>();
		part->SetName("ExtendedPart");
		part->SetParent(game);
		auto unrelated = std::make_shared<Folder>();
		unrelated->SetParent(game);
		auto BoundedPart = std::make_shared<Part>();
		for (std::size_t ExtensionIndex = 0; ExtensionIndex < 2; ++ExtensionIndex) {
			const auto BoundedExtensionId = SchemaId::FromExtensionName(
				"Game.Bounds", "Dense" + std::to_string(ExtensionIndex)
			);
			for (std::size_t PropertyIndex = 0; PropertyIndex < MaximumExtensionProperties; ++PropertyIndex)
				Check(BoundedPart->ApplyExtensionPropertyMutation(
					BoundedExtensionId,
					1,
					"Value" + std::to_string(PropertyIndex),
					1,
					ScriptSecurityContext::CoreTrusted()
				) == MutationStatus::Success, "extension runtime accepts its exact per-Instance override count limit");
		}
		Check(BoundedPart->GetExtensionPropertyOverrides().size() == 2,
			"exactly bounded extension overrides remain grouped by canonical extension identity");
		MutationGateway BoundedGateway;
		Check(BoundedGateway.Apply(UpdateExtensionPropertyCommand{
			BoundedPart->GetObjectId(), SchemaId::FromExtensionName("Game.Bounds", "Dense2"), 1, "Value0", 1
		}, ScriptSecurityContext::CoreTrusted()).Status == MutationStatus::ValidationFailed,
			"extension runtime rejects one override beyond its per-Instance count limit");
		BoundedPart->Destroy();
		Check(std::get<int>(part->GetExtensionPropertyValue(extensionId, "Damage")) == 0,
			"missing extension override reads the immutable schema default");
		CheckThrows<std::invalid_argument>([&] {
			static_cast<void>(unrelated->GetExtensionPropertyValue(extensionId, "Damage"));
		}, "unrelated class cannot read an inapplicable extension property");
		CheckThrows<std::runtime_error>([&] {
			static_cast<void>(part->GetExtensionPropertyValue(
				extensionId, "Damage", {ScriptExecutionDomain::Server, {}}
			));
		}, "extension property reads require explicit DataModel read authority");

		auto &journal = ChangeJournal::Get();
		journal.Clear();
		const auto scope = game->GetObjectId();
		auto cursor = journal.CreateCursor(scope);
		MutationGateway gateway;
		const auto defineOnly = ScriptSecurityContext::PreRunRegistration();
		Check(gateway.Apply(UpdateExtensionPropertyCommand{
			part->GetObjectId(), extensionId, 1, "Damage", 5
		}, defineOnly).Status == MutationStatus::Unauthorized,
			"DefineSchema does not imply extension-value mutation authority");
		Check(journal.Read(cursor).Records.empty(), "unauthorized extension mutation emits no journal record");
		Check(gateway.Apply(UpdateExtensionPropertyCommand{
			part->GetObjectId(), extensionId, 2, "Damage", 5
		}, ScriptSecurityContext::CoreTrusted()).Status == MutationStatus::InvalidProperty,
			"extension mutation rejects an incompatible definition version");
		Check(gateway.Apply(UpdateExtensionPropertyCommand{
			unrelated->GetObjectId(), extensionId, 1, "Damage", 5
		}, ScriptSecurityContext::CoreTrusted()).Status == MutationStatus::InvalidProperty,
			"extension mutation rejects a target-class mismatch");
		Check(gateway.Apply(UpdateExtensionPropertyCommand{
			part->GetObjectId(), extensionId, 1, "Damage", std::string("5")
		}, ScriptSecurityContext::CoreTrusted()).Status == MutationStatus::ValidationFailed,
			"extension mutation rejects a WireValue with the wrong declared type");
		auto mutation = gateway.Apply(UpdateExtensionPropertyCommand{
			part->GetObjectId(), extensionId, 1, "Damage", 5
		}, ScriptSecurityContext::CoreTrusted());
		Check(mutation.Succeeded() && std::get<int>(part->GetExtensionPropertyValue(extensionId, "Damage")) == 5,
			"authorized extension mutation commits authoritative Instance state");
		auto changes = journal.Read(cursor);
		Check(changes.Records.size() == 1 &&
			std::holds_alternative<ExtensionPropertyUpdatedChange>(changes.Records[0].Payload),
			"extension mutation emits one dedicated semantic journal record");

		auto snapshot = CaptureSnapshot(game);
		auto snapshotPart = std::find_if(snapshot.Objects.begin(), snapshot.Objects.end(), [](const auto &object) {
			return object.Name == "ExtendedPart";
		});
		Check(snapshotPart != snapshot.Objects.end() && snapshotPart->Extensions.size() == 1 &&
			std::get<int>(snapshotPart->Extensions[0].Properties.at("Damage")) == 5,
			"snapshot v5 carries sparse extension overrides with stable identity and version");
		auto parsedSnapshot = DeserializeSnapshot(SerializeSnapshot(snapshot));
		Check(parsedSnapshot.Succeeded(), "extension snapshot state round-trips through the bounded wire format");
		auto OversizedSnapshot = snapshot;
		auto OversizedPart = std::find_if(OversizedSnapshot.Objects.begin(), OversizedSnapshot.Objects.end(), [](const auto &Object) {
			return Object.Name == "ExtendedPart";
		});
		for (std::size_t ExtensionIndex = 0; ExtensionIndex < 2; ++ExtensionIndex) {
			SnapshotExtensionState State{
				SchemaId::FromExtensionName("Game.Bounds", "Dense" + std::to_string(ExtensionIndex)), 1, {}
			};
			for (std::size_t PropertyIndex = 0; PropertyIndex < MaximumExtensionProperties; ++PropertyIndex)
				State.Properties.emplace("Value" + std::to_string(PropertyIndex), 1);
			OversizedPart->Extensions.push_back(std::move(State));
		}
		std::sort(OversizedPart->Extensions.begin(), OversizedPart->Extensions.end(), [](const auto &Left, const auto &Right) {
			return Left.ExtensionSchemaId < Right.ExtensionSchemaId;
		});
		Check(!DeserializeSnapshot(SerializeSnapshot(OversizedSnapshot)).Succeeded(),
			"snapshot parsing rejects extension state beyond the canonical per-Instance override limit");

		auto started = InProcessReplicationSession::Start(game);
		Check(started.Succeeded(), "loopback replication materializes extension state from the initial snapshot");
		part->ApplyExtensionPropertyMutation(
			extensionId, 1, "Damage", 9, ScriptSecurityContext::CoreTrusted()
		);
		Check(started.Session->ApplyAvailable().Succeeded(), "extension property delta replicates through journal v6");
		auto receiverPart = started.Session->GetReceiverRoot()->FindFirstChild("ExtendedPart", false);
		Check(receiverPart && std::get<int>(receiverPart->GetExtensionPropertyValue(extensionId, "Damage")) == 9,
			"replicated receiver resolves extension meaning through the frozen schema");

		std::shared_ptr<Instance> serializableRoot = game;
		const auto serialized = InstanceSerialization::Serialize(InstanceSerialization::InstanceFormat::Json, serializableRoot);
		Check(serialized.find(extensionId.ToString()) != std::string::npos &&
			serialized.find("\"Version\":4") != std::string::npos,
			"persistence stores extension SchemaId, version, property identity, and WireValue");
		std::istringstream persistedStream(serialized);
		auto loaded = InstanceSerialization::Deserialize(
			InstanceSerialization::InstanceFormat::Json, persistedStream
		);
		auto loadedPart = loaded.Instance ? loaded.Instance->FindFirstChild("ExtendedPart", false) : nullptr;
		Check(loaded.Ok && loadedPart &&
			std::get<int>(loadedPart->GetExtensionPropertyValue(extensionId, "Damage")) == 9,
			"persistence reconstructs extension state under exact-version and target validation");

		part->ApplyExtensionPropertyMutation(
			extensionId, 1, "Damage", 0, ScriptSecurityContext::CoreTrusted()
		);
		Check(part->GetExtensionPropertyOverrides().empty(),
			"writing the schema default removes the physical override deterministically");
		const auto sparseSerialized = InstanceSerialization::Serialize(
			InstanceSerialization::InstanceFormat::Json, serializableRoot
		);
		Check(sparseSerialized.find(extensionId.ToString()) == std::string::npos,
			"default-valued extension state is omitted from canonical persistence");
		journal.Clear();
	}

	void TestProtocolInputHardening() {
		using namespace gargantuan;
		using namespace gargantuan::InstanceSerialization;
		using Json = nlohmann::ordered_json;

		Check(
			!DecodeWireObjectIdJson(Json{{"Slot", 1}, {"Generation", 1}, {"Extra", true}}.dump()),
			"wire ObjectIds reject unknown fields"
		);
		Check(!DecodeWireObjectIdJson(Json{{"Slot", 0}, {"Generation", 1}}.dump()), "wire ObjectIds reject zero slots");
		Check(
			!DecodeWireObjectIdJson(Json{{"Slot", std::uint64_t{1} << 32}, {"Generation", 1}}.dump()),
			"wire ObjectIds reject narrowing overflow"
		);
		Check(
			!DecodeWireValueJson(Json{{"Type", "Vector3"}, {"Value", Json::array({1.0, 2.0})}}.dump()),
			"WireValue rejects truncated compound values"
		);
		Check(
			!DecodeWireValueJson(Json{{"Type", "Bool"}, {"Value", true}, {"Capabilities", "all"}}.dump()),
			"WireValue rejects packet-supplied extra metadata"
		);
		Check(
			!DecodeWireValueJson(Json{{"Type", "Int"}, {"Value", std::uint64_t{1} << 32}}.dump()),
			"WireValue rejects signed integer narrowing overflow"
		);
		Check(
			!DecodeWireValueJson(Json{{"Type", "String"}, {"Value", std::string(MaximumProtocolStringBytes + 1, 'x')}}.dump()),
			"WireValue rejects oversized strings"
		);
		Check(
			!EncodeWireValueJson(WireFloat{std::numeric_limits<float>::infinity()}),
			"structured WireValue rejects non-finite numerics"
		);
		const auto TruncatedWireValue = DecodeWireValueJson("{");
		Check(
			!TruncatedWireValue && TruncatedWireValue.error().Code == SerializationErrorCode::TruncatedInput,
			"serializer-specific truncated-input exceptions normalize to Gargantuan errors"
		);
		const auto DuplicateWireValue = DecodeWireValueJson(R"({"Type":"Bool","Value":false,"Value":true})");
		Check(
			DuplicateWireValue && std::get_if<bool>(&*DuplicateWireValue) && *std::get_if<bool>(&*DuplicateWireValue),
			"the existing last-value-wins duplicate-field behavior is compatibility-locked explicitly"
		);

		Json Deep = nullptr;
		for (std::size_t Index = 0; Index <= MaximumProtocolJsonDepth; ++Index) Deep = Json::array({std::move(Deep)});
		Check(
			!DecodeWireValueJson(Deep.dump()),
			"protocol JSON rejects excessive nesting"
		);
		std::string DeepDocument(MaximumProtocolJsonDepth + 1, '[');
		DeepDocument += "null";
		DeepDocument.append(MaximumProtocolJsonDepth + 1, ']');
		Check(
			!DeserializeWireJournalRecords(DeepDocument).Succeeded(),
			"wire decoder rejects excessive nesting before JSON parsing"
		);
		Check(
			!DeserializeSnapshot(std::string(MaximumProtocolDocumentBytes + 1, ' ')).Succeeded(),
			"snapshot decoder rejects oversized documents before parsing"
		);
		Json OversizedJournal{{"Version", WireJournalFormatVersion}, {"Records", Json::array()}};
		for (std::size_t Index = 0; Index <= MaximumWireJournalRecords; ++Index)
			OversizedJournal["Records"].push_back(Json::object());
		Check(
			!DeserializeWireJournalRecords(OversizedJournal.dump()).Succeeded(),
			"wire journal decoder rejects oversized record batches"
		);

		auto PersistedPart = std::make_shared<Part>();
		std::shared_ptr<Instance> PersistedRoot = PersistedPart;
		auto PersistedDocument = Json::parse(Serialize(InstanceFormat::Json, PersistedRoot));
		PersistedDocument["Properties"]["Size"] = Json{{"Vector3", Json::array({1.0, 2.0})}};
		std::istringstream TruncatedPersistence(PersistedDocument.dump());
		auto TruncatedResult = Deserialize(InstanceFormat::Json, TruncatedPersistence);
		Check(!TruncatedResult.Ok, "persistence decoder rejects truncated compound values without throwing");

		auto FirstWorld = std::make_shared<DataModel>();
		auto FirstPart = std::make_shared<Part>();
		FirstPart->SetParent(FirstWorld);
		auto FirstWeld = std::make_shared<WeldConstraint>();
		FirstWeld->SetParent(FirstWorld);
		auto SecondWorld = std::make_shared<DataModel>();
		auto SecondPart = std::make_shared<Part>();
		SecondPart->SetParent(SecondWorld);
		std::optional<std::shared_ptr<BasePart>> ForeignReference = SecondPart;
		Check(
			FirstWeld->ApplyPropertyMutation(
				"Part0", ForeignReference, Enums::Permission::Engine, ScriptSecurityContext::CoreTrusted()
			) == MutationStatus::ValidationFailed,
			"authoritative object-reference mutation rejects another DataModel scope"
		);
		Check(!FirstWeld->GetPart0(), "rejected cross-scope reference does not commit");

		ScriptSecurityContext PeerSecurity{
			ScriptExecutionDomain::Client,
			{ScriptCapability::MutateDataModel},
		};
		auto PeerAuthority = MutationAuthorityContext::AuthenticatedPeer(PeerSecurity, FirstWorld->GetObjectId());
		MutationGateway Gateway;
		Check(
			Gateway.Apply(
				UpdatePropertyCommand{SecondPart->GetObjectId(), "Name", std::string("CrossScope")}, PeerAuthority
			).Status == MutationStatus::Rejected,
			"host-owned peer authority cannot mutate outside its assigned DataModel scope"
		);
		Check(
			Gateway.Apply(
				UpdatePropertyCommand{FirstPart->GetObjectId(), "Name", std::string("InScope")}, PeerAuthority
			).Succeeded(),
			"host-owned peer authority can carry an explicitly assigned in-scope capability context"
		);

		auto Source = std::make_shared<DataModel>();
		auto SourcePart = std::make_shared<Part>();
		SourcePart->SetParent(Source);
		auto CycleParent = std::make_shared<Folder>();
		CycleParent->SetParent(Source);
		auto CycleChild = std::make_shared<Folder>();
		CycleChild->SetParent(CycleParent);
		auto ReferenceWeld = std::make_shared<WeldConstraint>();
		ReferenceWeld->SetName("ReferenceWeld");
		ReferenceWeld->SetParent(Source);
		auto Started = InProcessReplicationSession::Start(Source);
		Check(Started.Succeeded(), "hostile batch test starts from a valid baseline");
		auto Session = Started.Session;
		auto ReceiverPart = std::dynamic_pointer_cast<Part>(Session->GetReceiverRoot()->Children.front());
		auto Cursor = Session->GetCursor();
		const auto Scope = WireObjectId::FromObjectId(Source->GetObjectId());
		const auto PartId = WireObjectId::FromObjectId(SourcePart->GetObjectId());
		std::vector<WireJournalRecord> RejectedBatch{
			{
				.Sequence = Cursor.NextSequence,
				.Scope = Scope,
				.Operation = WireJournalOperation::AttributeUpdate,
				.Object = PartId,
				.AttributeName = "Prefix",
				.Value = std::string("must-not-commit"),
			},
			{
				.Sequence = Cursor.NextSequence + 1,
				.Scope = Scope,
				.Operation = WireJournalOperation::TagRemoved,
				.Object = PartId,
				.TagName = "Absent",
			},
		};
		auto Rejected = Session->ApplyWireRecords(RejectedBatch);
		Check(
			Rejected.Status == ReplicationApplyStatus::ApplyRejected && Rejected.AppliedRecords == 0 &&
				Session->GetCursor().NextSequence == Cursor.NextSequence &&
				!ReceiverPart->GetAttributeValue("Prefix", ScriptSecurityContext::CoreTrusted()),
			"semantic rejection after a valid batch prefix commits no state or cursor prefix"
		);

		auto ReferenceCursor = Session->GetCursor();
		bool RejectedPrefixNotified = false;
		auto RejectedPrefixConnection = ReceiverPart->GetAttributeSignal(
			"ReferencePrefix", ScriptSecurityContext::CoreTrusted()
		)->Connect([&](std::monostate) { RejectedPrefixNotified = true; });
		std::vector<WireJournalRecord> WrongReferenceClassBatch{
			{
				.Sequence = ReferenceCursor.NextSequence,
				.Scope = Scope,
				.Operation = WireJournalOperation::AttributeUpdate,
				.Object = PartId,
				.AttributeName = "ReferencePrefix",
				.Value = 1,
			},
			{
				.Sequence = ReferenceCursor.NextSequence + 1,
				.Scope = Scope,
				.Operation = WireJournalOperation::PropertyUpdate,
				.Object = WireObjectId::FromObjectId(ReferenceWeld->GetObjectId()),
				.PropertyName = "Part0",
				.Value = WireObjectReference{WireObjectId::FromObjectId(CycleParent->GetObjectId())},
			},
		};
		auto WrongReferenceClass = Session->ApplyWireRecords(WrongReferenceClassBatch);
		Check(
			WrongReferenceClass.Status == ReplicationApplyStatus::ApplyRejected &&
				WrongReferenceClass.AppliedRecords == 0 &&
				Session->GetCursor().NextSequence == ReferenceCursor.NextSequence &&
				!ReceiverPart->GetAttributeValue("ReferencePrefix", ScriptSecurityContext::CoreTrusted()) &&
				!RejectedPrefixNotified,
			"reference class compatibility is preflighted before batch state, cursor, or notifications commit"
		);
		RejectedPrefixConnection->Disconnect();

		bool NotificationSawCommittedBatch = false;
		auto Connection = ReceiverPart->GetAttributeSignal(
			"First", ScriptSecurityContext::CoreTrusted()
		)->Connect([&](std::monostate) {
			NotificationSawCommittedBatch = ReceiverPart->GetAttributeValue(
				"Second", ScriptSecurityContext::CoreTrusted()
			).has_value();
		});
		std::vector<WireJournalRecord> ValidBatch{
			{
				.Sequence = Cursor.NextSequence,
				.Scope = Scope,
				.Operation = WireJournalOperation::AttributeUpdate,
				.Object = PartId,
				.AttributeName = "First",
				.Value = 1,
			},
			{
				.Sequence = Cursor.NextSequence + 1,
				.Scope = Scope,
				.Operation = WireJournalOperation::AttributeUpdate,
				.Object = PartId,
				.AttributeName = "Second",
				.Value = 2,
			},
		};
		Check(Session->ApplyWireRecords(ValidBatch).Succeeded(), "fully preflighted hostile-shaped batch commits");
		Check(NotificationSawCommittedBatch, "batch notifications observe the complete committed batch state");
		Connection->Disconnect();

		Cursor = Session->GetCursor();
		WireJournalRecord Cycle{
			.Sequence = Cursor.NextSequence,
			.Scope = Scope,
			.Operation = WireJournalOperation::Reparent,
			.Object = WireObjectId::FromObjectId(CycleParent->GetObjectId()),
			.Parent = WireObjectId::FromObjectId(CycleChild->GetObjectId()),
		};
		Check(
			Session->ApplyWireRecords({Cycle}).Status == ReplicationApplyStatus::ApplyRejected,
			"wire reparent preflight rejects hierarchy cycles"
		);

		auto SnapshotValue = CaptureSnapshot(Source);
		auto SnapshotCycle = SnapshotValue;
		auto ParentPosition = std::find_if(
			SnapshotCycle.Objects.begin(), SnapshotCycle.Objects.end(),
			[&](const SnapshotObject &Object) { return Object.Id == WireObjectId::FromObjectId(CycleParent->GetObjectId()); }
		);
		ParentPosition->Parent = WireObjectId::FromObjectId(CycleChild->GetObjectId());
		auto SnapshotRejected = LoadSnapshot(SnapshotCycle);
		Check(
			!SnapshotRejected.Succeeded() && !SnapshotRejected.Root && SnapshotRejected.Objects.empty(),
			"snapshot semantic preflight rejects cycles before publishing candidate state"
		);

		MutationGateway BoundedGateway;
		std::shared_ptr<MutationCompletion> OverflowCompletion;
		for (std::size_t Index = 0; Index <= MaximumPendingMutations; ++Index)
			OverflowCompletion = BoundedGateway.Submit(
				UpdatePropertyCommand{FirstPart->GetObjectId(), "Name", std::string("Queued")}
			);
		Check(
			OverflowCompletion->IsReady() && OverflowCompletion->Wait().Status == MutationStatus::Rejected,
			"mutation gateway rejects work beyond its fixed pending-command ceiling"
		);
	}

	void TestRenderBackendBoundary() {
		using namespace gargantuan;
		class RecordingRenderer final : public BaseRenderer {
		  public:
			void Draw(RenderSnapshotPtr Snapshot) override { LastSnapshot = std::move(Snapshot); }
			void Resize(int WidthValue, int HeightValue) override {
				Width = WidthValue;
				Height = HeightValue;
			}
			void Destroy() override { Destroyed = true; }
			[[nodiscard]] std::pair<std::uint32_t, std::uint32_t> GetViewportSize() const override {
				return {static_cast<std::uint32_t>(Width), static_cast<std::uint32_t>(Height)};
			}

			RenderSnapshotPtr LastSnapshot;
			int Width = 0;
			int Height = 0;
			bool Destroyed = false;
		};

		auto Published = std::make_shared<RenderSnapshot>();
		Published->Id = 1;
		Published->ViewportWidth = 320;
		Published->ViewportHeight = 200;
		RenderSnapshotPtr Immutable = Published;
		RecordingRenderer Renderer;
		Renderer.Resize(320, 200);
		Renderer.Draw(Immutable);
		Renderer.Destroy();
		Check(
			Renderer.LastSnapshot == Immutable && Renderer.GetViewportSize() == std::pair<std::uint32_t, std::uint32_t>{320, 200} &&
				Renderer.Destroyed,
			"backend-neutral renderer receives immutable snapshots and owns resize/shutdown behavior"
		);

		RenderProjection Projection;
		RenderSnapshot First;
		First.Id = 1;
		First.Items = {
			{.Object = {10, 1}, .Geometry = RenderGeometry::Block},
			{.Object = {11, 1}, .Geometry = RenderGeometry::Ball},
		};
		auto FirstChanges = Projection.Apply(First);
		Check(
			FirstChanges.Created == 2 && FirstChanges.Updated == 0 && FirstChanges.Removed == 0 &&
				Projection.GetSize() == 2,
			"render projection creates one disposable entry per ObjectId"
		);
		auto Unchanged = Projection.Apply(First);
		Check(
			Unchanged.Unchanged == 2 && Unchanged.Created == 0 && Unchanged.Updated == 0 && Unchanged.Removed == 0,
			"render projection preserves unchanged renderer-side objects"
		);

		RenderSnapshot Second;
		Second.Id = 2;
		Second.Items = {
			{.Object = {10, 1}, .Geometry = RenderGeometry::Cylinder, .Color = {0.5f, 0.25f, 0.75f, 1.0f}},
			{.Object = {11, 2}, .Geometry = RenderGeometry::Ball},
		};
		auto SecondChanges = Projection.Apply(Second);
		Check(
			SecondChanges.Created == 1 && SecondChanges.Updated == 1 && SecondChanges.Removed == 1 &&
				Projection.GetItem({11, 1}) == nullptr && Projection.GetItem({11, 2}) != nullptr,
			"generation reuse removes the stale ObjectId projection before retaining its replacement"
		);

		RenderSnapshot Duplicate = Second;
		Duplicate.Id = 3;
		Duplicate.Items.push_back(Duplicate.Items.front());
		CheckThrows<std::invalid_argument>(
			[&] { static_cast<void>(Projection.Apply(Duplicate)); },
			"render projection rejects duplicate ObjectIds"
		);
		Check(
			Projection.GetSize() == 2 && Projection.GetItem({10, 1}) != nullptr && Projection.GetItem({11, 2}) != nullptr,
			"invalid projection publication does not partially replace the previous projection"
		);
		Projection.Clear();
		Check(Projection.GetSize() == 0, "render projection shutdown releases every disposable entry");
	}

	void TestCustomClassRuntime() {
		using namespace gargantuan;
		const auto ParentId = SchemaId::FromCustomClassName("Game", "DamageableFolder");
		const auto ChildId = SchemaId::FromCustomClassName("Game", "CombatFolder");
		const auto FolderExtensionId = SchemaId::FromExtensionName("Game.Data", "FolderProperties");
		const auto *ParentDefinition = GetActiveRuntimeSchemaRegistry().FindClassById(ParentId);
		const auto *ChildDefinition = GetActiveRuntimeSchemaRegistry().FindClassById(ChildId);
		Check(ParentDefinition && ChildDefinition && ChildDefinition->BaseSchemaId == ParentId &&
			ChildDefinition->NativeHostClassId == SchemaId::FromNativeName("Engine", "Folder"),
			"published custom class identities resolve to their custom base and approved Folder host");

		auto Game = std::make_shared<DataModel>();
		Game->SetArchivable(true);
		auto Custom = InstanceClassRegistry::ConstructByName("Game.CombatFolder");
		Check(Custom && std::dynamic_pointer_cast<Folder>(Custom) &&
			Custom->GetClassName() == "Game.CombatFolder" &&
			Custom->IsA("Game.CombatFolder") && Custom->IsA("Game.DamageableFolder") &&
			Custom->IsA("Folder") && Custom->IsA("Instance") && !Custom->IsA("Part"),
			"custom construction preserves real schema identity, host behavior, and ID-based IsA ancestry");
		if (!Custom) return;
		Custom->SetArchivable(true);
		Custom->SetName("CustomCombatFolder");
		Custom->SetParent(Game);
		Check(std::get<int>(Custom->GetCustomClassPropertyValue(ParentId, "Health")) == 100 &&
			std::get<std::string>(Custom->GetCustomClassPropertyValue(ChildId, "Faction")) == "Neutral",
			"custom properties read immutable sparse defaults across custom inheritance");
		Check(std::get<std::string>(Custom->GetExtensionPropertyValue(FolderExtensionId, "Label")).empty(),
			"class extensions targeting a native ancestor apply to a custom class without copying definitions");

		auto &Journal = ChangeJournal::Get();
		Journal.Clear();
		auto Cursor = Journal.CreateCursor(Game->GetObjectId());
		MutationGateway Gateway;
		Check(Gateway.Apply(UpdatePropertyCommand{Custom->GetObjectId(), "Health", 75},
			ScriptSecurityContext::PreRunRegistration()).Status == MutationStatus::Unauthorized,
			"DefineSchema does not imply custom class runtime mutation authority");
		Check(Journal.Read(Cursor).Records.empty(), "unauthorized custom property mutation emits no journal record");
		Check(Gateway.Apply(UpdatePropertyCommand{Custom->GetObjectId(), "Health", std::string("75")},
			ScriptSecurityContext::CoreTrusted()).Status == MutationStatus::ValidationFailed,
			"custom property mutation rejects values outside its declared type");
		Check(Gateway.Apply(UpdatePropertyCommand{Custom->GetObjectId(), "Health", 75},
			ScriptSecurityContext::CoreTrusted()).Succeeded(),
			"MutationGateway commits an authorized custom property update");
		auto Changes = Journal.Read(Cursor);
		Check(Changes.Records.size() == 1 && std::holds_alternative<PropertyUpdatedChange>(Changes.Records[0].Payload) &&
			std::get<PropertyUpdatedChange>(Changes.Records[0].Payload).DeclaringClassSchemaId == ParentId,
			"custom property updates use the normal property journal with stable declaring identity");
		Check(Custom->ApplyAttributeMutation("Health", WireValue(12), ScriptSecurityContext::CoreTrusted()) ==
			MutationStatus::Success && Custom->GetAttributeValue("Health") == WireValue(12),
			"same-named Attributes remain independent dynamic state on custom Instances");
		Check(Gateway.Apply(AddTagCommand{Custom->GetObjectId(), "CustomEnemy"},
			ScriptSecurityContext::CoreTrusted()).Succeeded(),
			"custom Instances participate in the existing indexed tag architecture");

		auto Snapshot = CaptureSnapshot(Game);
		auto SnapshotCustom = std::find_if(Snapshot.Objects.begin(), Snapshot.Objects.end(), [](const auto &Object) {
			return Object.Name == "CustomCombatFolder";
		});
		Check(SnapshotCustom != Snapshot.Objects.end() && SnapshotCustom->ClassSchemaId == ChildId &&
			SnapshotCustom->ClassDefinitionVersion == 1 && SnapshotCustom->CustomProperties.size() == 1 &&
			std::get<int>(SnapshotCustom->CustomProperties[0].Properties.at("Health")) == 75,
			"snapshot v6 carries custom class and sparse property identity/version independently from Attributes");
		auto ParsedSnapshot = DeserializeSnapshot(SerializeSnapshot(Snapshot));
		Check(ParsedSnapshot.Succeeded() && LoadSnapshot(*ParsedSnapshot.Value).Succeeded(),
			"custom class snapshots validate and materialize against the active frozen schema");
		auto OversizedSnapshotVersion = nlohmann::ordered_json::parse(SerializeSnapshot(Snapshot));
		for (auto &Object : OversizedSnapshotVersion["Objects"])
			if (Object["Name"] == "CustomCombatFolder")
				Object["ClassDefinitionVersion"] = std::uint64_t{4294967297};
		Check(!DeserializeSnapshot(OversizedSnapshotVersion.dump()).Succeeded(),
			"custom class snapshots reject oversized definition versions before uint32 narrowing");

		auto Started = InProcessReplicationSession::Start(Game);
		Check(Started.Succeeded(), "loopback replication materializes custom class state from the initial snapshot");
		Check(Gateway.Apply(UpdatePropertyCommand{Custom->GetObjectId(), "Health", 60},
			ScriptSecurityContext::CoreTrusted()).Succeeded() && Started.Session->ApplyAvailable().Succeeded(),
			"custom class property deltas replicate through the canonical property journal");
		auto Receiver = Started.Session->GetReceiverRoot()->FindFirstChild("CustomCombatFolder", false);
		Check(Receiver && Receiver->GetClassName() == "Game.CombatFolder" &&
			std::get<int>(Receiver->GetCustomClassPropertyValue(ParentId, "Health")) == 60 &&
			Receiver->GetAttributeValue("Health") == WireValue(12),
			"replication reconstructs exact custom schema identity plus distinct custom and dynamic state");
		const auto AtomicCursor = Started.Session->GetCursor();
		const auto ReceiverCustomId = SnapshotCustom->Id;
		WireJournalRecord ValidThenRejected{
			.Sequence = AtomicCursor.NextSequence,
			.Scope = WireObjectId::FromObjectId(AtomicCursor.Scope),
			.Operation = WireJournalOperation::PropertyUpdate,
			.Object = ReceiverCustomId,
			.PropertyName = "Health",
			.DeclaringClassSchemaId = ParentId,
			.DefinitionVersion = 1,
			.Value = 59,
		};
		auto InvalidAfterValid = ValidThenRejected;
		InvalidAfterValid.Sequence++;
		InvalidAfterValid.DefinitionVersion = 2;
		InvalidAfterValid.Value = 58;
		Check(Started.Session->ApplyWireRecords({ValidThenRejected, InvalidAfterValid}).Status ==
			ReplicationApplyStatus::ApplyRejected &&
			Started.Session->GetCursor().NextSequence == AtomicCursor.NextSequence && Receiver &&
			std::get<int>(Receiver->GetCustomClassPropertyValue(ParentId, "Health")) == 60,
			"custom replication preflights a batch so malformed later schema state cannot partially mutate the receiver");
		auto DuplicateAfterValid = ValidThenRejected;
		DuplicateAfterValid.Sequence++;
		Check(Started.Session->ApplyWireRecords({ValidThenRejected, DuplicateAfterValid}).Status ==
			ReplicationApplyStatus::ApplyRejected &&
			Started.Session->GetCursor().NextSequence == AtomicCursor.NextSequence && Receiver &&
			std::get<int>(Receiver->GetCustomClassPropertyValue(ParentId, "Health")) == 60,
			"custom replication simulates effective values so a later semantic no-op cannot commit a batch prefix");
		auto OversizedJournalVersion = nlohmann::ordered_json::parse(
			SerializeWireJournalRecords({ValidThenRejected})
		);
		OversizedJournalVersion["Records"][0]["DefinitionVersion"] = std::uint64_t{4294967297};
		Check(!DeserializeWireJournalRecords(OversizedJournalVersion.dump()).Succeeded(),
			"custom property journal records reject oversized definition versions before uint32 narrowing");
		auto Created = Gateway.Apply(CreateObjectCommand{
			SchemaId::FromCustomClassName("Game", "CombatFolder"), 1, Game->GetObjectId(), std::nullopt
		},
			ScriptSecurityContext::CoreTrusted());
		Check(Created.Succeeded() && Created.Object && Gateway.Apply(UpdatePropertyCommand{
			*Created.Object, "Name", std::string("ReplicatedCustom")
		}, ScriptSecurityContext::CoreTrusted()).Succeeded() && Started.Session->ApplyAvailable().Succeeded(),
			"runtime construction and create journal accept a canonical custom class name under mutation authority");
		auto ReceiverCreated = Started.Session->GetReceiverRoot()->FindFirstChild("ReplicatedCustom", false);
		Check(ReceiverCreated && ReceiverCreated->GetClassName() == "Game.CombatFolder" &&
			std::get<int>(ReceiverCreated->GetCustomClassPropertyValue(ParentId, "Health")) == 100,
			"journal Create carries stable custom class identity/version and preserves sparse defaults");
		const auto ReceiverCursor = Started.Session->GetCursor();
		WireJournalRecord WrongKindCreate{
			.Sequence = ReceiverCursor.NextSequence,
			.Scope = WireObjectId::FromObjectId(ReceiverCursor.Scope),
			.Operation = WireJournalOperation::Create,
			.Object = {999999, 1},
			.ClassName = "Game.CombatFolder",
			.ClassSchemaId = SchemaId::FromEnumName("Game", "CombatState"),
			.DefinitionVersion = 1,
		};
		Check(Started.Session->ApplyWireRecords({WrongKindCreate}).Status == ReplicationApplyStatus::ApplyRejected &&
			Started.Session->GetCursor().NextSequence == ReceiverCursor.NextSequence,
			"replication rejects wrong-kind custom class identity without advancing receiver state");

		std::shared_ptr<Instance> Serializable = Game;
		const auto Serialized = InstanceSerialization::Serialize(InstanceSerialization::InstanceFormat::Json, Serializable);
		Check(Serialized.find(ChildId.ToString()) != std::string::npos &&
			Serialized.find(ParentId.ToString()) != std::string::npos && Serialized.find("\"Version\":4") != std::string::npos,
			"persistence v4 stores custom class, declaring property identity, and exact definition versions");
		auto OversizedPersistedVersion = nlohmann::ordered_json::parse(Serialized);
		for (auto &Child : OversizedPersistedVersion["Children"])
			if (Child["Name"] == "CustomCombatFolder")
				Child["ClassDefinitionVersion"] = std::uint64_t{4294967297};
		std::istringstream OversizedPersistedStream(OversizedPersistedVersion.dump());
		Check(!InstanceSerialization::Deserialize(
			InstanceSerialization::InstanceFormat::Json, OversizedPersistedStream
		).Ok, "custom class persistence rejects oversized definition versions before uint32 narrowing");
		std::istringstream Persisted(Serialized);
		auto Loaded = InstanceSerialization::Deserialize(InstanceSerialization::InstanceFormat::Json, Persisted);
		auto LoadedCustom = Loaded.Instance ? Loaded.Instance->FindFirstChild("CustomCombatFolder", false) : nullptr;
		Check(Loaded.Ok && LoadedCustom && LoadedCustom->GetClassName() == "Game.CombatFolder" &&
			std::get<int>(LoadedCustom->GetCustomClassPropertyValue(ParentId, "Health")) == 60,
			"persistence reconstructs the exact constructible custom class without base-class fallback");

		Check(Custom->ApplyExtensionPropertyMutation(FolderExtensionId, 1, "Label", WireValue(std::string("Network")),
			ScriptSecurityContext::CoreTrusted()) == MutationStatus::Success,
			"custom-class replication fixture commits extension state");
		auto RuntimeCustomClone = Custom->Clone();
		Check(RuntimeCustomClone && !RuntimeCustomClone->GetParent() &&
			RuntimeCustomClone->GetClassName() == "Game.CombatFolder" &&
			std::get<int>(RuntimeCustomClone->GetCustomClassPropertyValue(ParentId, "Health")) == 60 &&
			std::get<std::string>(RuntimeCustomClone->GetExtensionPropertyValue(FolderExtensionId, "Label")) == "Network" &&
			RuntimeCustomClone->GetAttributeValue("Health") == WireValue(12),
			"runtime Clone preserves exact custom class identity, sparse custom state, extensions, and Attributes");
		RuntimeCustomClone->SetParent(Game);
		Check(Game->Tags.Has(Game->GetObjectId(), RuntimeCustomClone->GetObjectId(), "CustomEnemy",
			ScriptSecurityContext::CoreTrusted()),
			"runtime custom Clone publishes copied Tags during first adoption");
		Check(RuntimeCustomClone->ApplyCustomClassPropertyMutation(ParentId, 1, "Health", WireValue(5),
			ScriptSecurityContext::CoreTrusted()) == MutationStatus::Success &&
			std::get<int>(Custom->GetCustomClassPropertyValue(ParentId, "Health")) == 60,
			"runtime custom Clone state is independent from its source");
		RuntimeCustomClone->Destroy();
		network::ReplicationCoordinator NetworkCoordinator(Game);
		auto NetworkBaseline = NetworkCoordinator.AddPeer({701, 1}, network::ReplicationEpoch(1));
		network::ReplicaApplier NetworkReplica;
		Check(NetworkBaseline.Succeeded() && NetworkReplica.ApplyFrame(*NetworkBaseline.Frame).Succeeded(),
			"game replication baseline materializes custom classes against exact schema compatibility");
		auto NetworkCustom = NetworkReplica.GetReplicaRoot()
			? NetworkReplica.GetReplicaRoot()->FindFirstChild("CustomCombatFolder", false) : nullptr;
		Check(NetworkCustom && NetworkCustom->GetClassName() == "Game.CombatFolder" &&
			std::get<int>(NetworkCustom->GetCustomClassPropertyValue(ParentId, "Health")) == 60 &&
			std::get<std::string>(NetworkCustom->GetExtensionPropertyValue(FolderExtensionId, "Label")) == "Network",
			"game replication preserves custom identity, sparse custom properties, and Class Extension state");
		Check(Gateway.Apply(UpdatePropertyCommand{Custom->GetObjectId(), "Health", 59},
			ScriptSecurityContext::CoreTrusted()).Succeeded() &&
			Custom->ApplyExtensionPropertyMutation(FolderExtensionId, 1, "Label", WireValue(std::string("Network2")),
				ScriptSecurityContext::CoreTrusted()) == MutationStatus::Success,
			"custom and extension incremental replication fixture mutates authoritative state");
		auto NetworkDelta = NetworkCoordinator.ProduceIncremental({701, 1});
		Check(NetworkDelta.Succeeded() && NetworkReplica.ApplyFrame(*NetworkDelta.Frame).Succeeded(),
			"game replication applies custom and extension changes through typed replication operations");
		NetworkCustom = NetworkReplica.GetReplicaRoot()
			? NetworkReplica.GetReplicaRoot()->FindFirstChild("CustomCombatFolder", false) : nullptr;
		Check(NetworkCustom && std::get<int>(NetworkCustom->GetCustomClassPropertyValue(ParentId, "Health")) == 59 &&
			std::get<std::string>(NetworkCustom->GetExtensionPropertyValue(FolderExtensionId, "Label")) == "Network2",
			"incremental game replication preserves exact custom and extension definition versions");

		Check(Gateway.Apply(UpdatePropertyCommand{Custom->GetObjectId(), "Health", 100},
			ScriptSecurityContext::CoreTrusted()).Succeeded() &&
			Custom->GetCustomClassPropertyOverrides().empty(),
			"writing a custom property default removes its physical override deterministically");
		Journal.Clear();
	}

	void TestSerializationGoldenFixtures() {
		using namespace gargantuan;
		using namespace gargantuan::InstanceSerialization;

		const auto ProjectMinimal = ReadSerializationFixture("project_v4_minimal.json");
		auto MinimalRoot = std::make_shared<DataModel>();
		MinimalRoot->SetName("FixtureProject");
		MinimalRoot->SetArchivable(true);
		std::shared_ptr<Instance> MinimalSerializable = MinimalRoot;
		Check(
			Serialize(InstanceFormat::Json, MinimalSerializable) == ProjectMinimal,
			"minimal project fixture locks deterministic persistence v4 output"
		);
		std::istringstream MinimalInput(ProjectMinimal);
		auto MinimalDecoded = Deserialize(InstanceFormat::Json, MinimalInput);
		std::shared_ptr<Instance> MinimalDecodedRoot = MinimalDecoded.Instance;
		Check(
			MinimalDecoded.Ok && MinimalDecodedRoot &&
				Serialize(InstanceFormat::Json, MinimalDecodedRoot) == ProjectMinimal,
			"minimal project fixture decodes to semantic state and re-encodes canonically"
		);

		const auto ProjectComplex = ReadSerializationFixture("project_v4_complex.json");
		std::istringstream ProjectInput(ProjectComplex);
		auto ProjectDecoded = Deserialize(InstanceFormat::Json, ProjectInput);
		std::shared_ptr<Instance> ProjectRoot = ProjectDecoded.Instance;
		const auto ProjectChild = ProjectRoot && ProjectRoot->Children.size() == 1
			? ProjectRoot->Children.front() : std::shared_ptr<Instance>{};
		Check(
			ProjectDecoded.Ok && ProjectChild && ProjectChild->GetName() == "FixtureCustom" &&
				ProjectChild->GetAttributeValue("Health") == std::optional<WireValue>(12),
			"complex project fixture decodes its child and Attribute semantic state"
		);
		// Project v4 child emission is gated by the root's non-persisted runtime Archivable flag.
		// Reapply that existing serializer precondition without changing the durable format.
		if (ProjectRoot) ProjectRoot->SetArchivable(true);
		const auto ProjectReencoded = ProjectRoot ? Serialize(InstanceFormat::Json, ProjectRoot) : std::string{};
		Check(
			ProjectDecoded.Ok && ProjectRoot && ProjectReencoded == ProjectComplex,
			"complex project fixture preserves Attributes, Tags, extensions, custom classes, and sparse state"
		);

		for (const auto Name : {"snapshot_v6_minimal.json", "snapshot_v6_complex.json"}) {
			const auto Fixture = ReadSerializationFixture(Name);
			auto Decoded = DeserializeSnapshot(Fixture);
			Check(
				Decoded.Succeeded() && SerializeSnapshot(*Decoded.Value) == Fixture,
				"snapshot fixtures decode to semantic state and re-encode canonically"
			);
			#ifdef GARGANTUAN_WITH_GLAZE_SERIALIZATION_PROTOTYPE
			auto GlazeDecoded = GlazePrototype::DecodeSnapshot(Fixture);
			auto GlazeEncoded = GlazeDecoded ? GlazePrototype::EncodeSnapshot(*GlazeDecoded) :
				SerializationResult<std::string>(std::unexpected(GlazeDecoded.error()));
			Check(
				GlazeDecoded.has_value() && GlazeEncoded.has_value() && *GlazeEncoded == Fixture,
				"Glaze Snapshot prototype preserves golden canonical bytes"
			);
			#endif
		}

		const auto JournalFixture = ReadSerializationFixture("journal_v6_representative.json");
		auto JournalDecoded = DeserializeWireJournalRecords(JournalFixture);
		Check(
			JournalDecoded.Succeeded() && SerializeWireJournalRecords(*JournalDecoded.Value) == JournalFixture,
			"journal fixture preserves custom, extension, enum, and Tag wire semantics"
		);
		#ifdef GARGANTUAN_WITH_GLAZE_SERIALIZATION_PROTOTYPE
		auto GlazeJournalDecoded = GlazePrototype::DecodeJournal(JournalFixture);
		auto GlazeJournalEncoded = GlazeJournalDecoded ? GlazePrototype::EncodeJournal(*GlazeJournalDecoded) :
			SerializationResult<std::string>(std::unexpected(GlazeJournalDecoded.error()));
		Check(
			GlazeJournalDecoded.has_value() && GlazeJournalEncoded.has_value() && *GlazeJournalEncoded == JournalFixture,
			"Glaze Journal prototype preserves golden canonical bytes"
		);
		#endif

		EditorHost Host("fixture-token");
		Check(
			Host.HandleRequest(ReadSerializationFixture("editorhost_v1_request.json")) ==
				ReadSerializationFixture("editorhost_v1_response.json"),
			"EditorHost request and response fixtures preserve the versioned JSON envelope"
		);
	}

	void TestProjectCreationJsonNames() {
		using Json = nlohmann::ordered_json;
		using namespace gargantuan;
		const auto TemporaryRoot = std::filesystem::temp_directory_path() /
			("gargantuan-project-name-" + std::to_string(
				std::chrono::steady_clock::now().time_since_epoch().count()
			));
		struct TemporaryProjectCleanup {
			std::filesystem::path Root;
			~TemporaryProjectCleanup() { std::filesystem::remove_all(Root); }
		} Cleanup{TemporaryRoot};

		const std::vector<std::string> Names{
			"Ordinary Project",
			"Quoted \"Project\"",
			R"(Backslash\Project)",
			"Unicode \xE2\x98\x83",
			"Line\nTab\tControl\x01",
		};
		for (std::size_t Index = 0; Index < Names.size(); ++Index) {
			const auto CaseRoot = TemporaryRoot / std::to_string(Index);
			std::filesystem::create_directories(CaseRoot);
			DiskFilesystem Filesystem(CaseRoot);
			auto Created = Project::fromInit(&Filesystem, Names[Index]);
			const auto Contents = Filesystem.ReadFileToString(Created.InstanceFilePath);
			const auto Document = Json::parse(Contents, nullptr, false);
			Check(
				!Document.is_discarded() && Document.is_object() &&
					Document.value("Name", std::string{}) == Names[Index],
				"project creation JSON round-trips names through the canonical encoder"
			);
		}
	}

	void TestAuthoritativeTransactions() {
		using namespace gargantuan;
		auto World = std::make_shared<DataModel>();
		World->SetArchivable(true);
		auto FirstParent = std::make_shared<Folder>();
		FirstParent->SetArchivable(true);
		FirstParent->SetParent(World);
		auto SecondParent = std::make_shared<Folder>();
		SecondParent->SetArchivable(true);
		SecondParent->SetParent(World);
		auto Target = std::make_shared<Part>();
		Target->SetArchivable(true);
		Target->SetName("Before");
		Target->SetParent(FirstParent);
		const auto Scope = World->GetObjectId();
		const auto TargetId = Target->GetObjectId();
		auto &Journal = ChangeJournal::Get();
		auto Cursor = Journal.CreateCursor(Scope);
		MutationGateway Gateway;
		const auto StartingRevision = World->GetAuthoritativeRevision();

		auto Began = World->Transactions.Begin(*World, 41, "Grouped Edit", TransactionOrigin::Studio);
		Check(
			Began.Succeeded() && Began.Id.IsValid() && Began.StartingRevision == StartingRevision,
			"engine issues a nonzero project-local transaction identity"
		);
		auto Authority = MutationAuthorityContext::Studio(ScriptSecurityContext::StudioCoreUi(), Scope, Began.Id, 41);
		Check(
			Gateway.Apply(UpdatePropertyCommand{TargetId, "Name", std::string("After")}, Authority).Succeeded() &&
				Gateway.Apply(UpdateAttributeCommand{TargetId, "Grouped", WireValue(7)}, Authority).Succeeded() &&
				Gateway.Apply(ReparentObjectCommand{TargetId, SecondParent->GetObjectId()}, Authority).Succeeded(),
			"current scalar, Attribute, and structural mutations participate in one explicit transaction"
		);
		Check(
			World->GetAuthoritativeRevision() == StartingRevision && Journal.Read(Cursor).Records.empty(),
			"open explicit transaction defers project revision and committed journal publication"
		);
		auto Committed = World->Transactions.Commit(*World, Began.Id, 41);
		Check(
			Committed.Succeeded() && Committed.ChangeCount == 3 &&
				World->GetAuthoritativeRevision() == StartingRevision + 1,
			"one grouped transaction advances the project revision exactly once"
		);
		auto Records = Journal.Read(Cursor);
		Check(
			Records.Records.size() == 3 && World->Transactions.GetCommitted().size() == 1 &&
				World->Transactions.GetCommitted().back().Changes.size() == 3,
			"transaction commit publishes all journal records and one semantic history entry"
		);
		Check(
			World->Transactions.Commit(*World, Began.Id, 41).Status == TransactionStatus::NotFound,
			"committed transaction identity cannot be committed twice"
		);
		auto CreatedForIdentity = Gateway.Apply(
			CreateObjectCommand{SchemaId::FromNativeName("Engine", "Folder"), 1, FirstParent->GetObjectId(), "IdentityHistory"},
			MutationAuthorityContext::Studio(ScriptSecurityContext::StudioCoreUi(), Scope, std::nullopt, 41)
		);
		Check(CreatedForIdentity.Succeeded() && CreatedForIdentity.Object,
			"ObjectId restoration fixture creates an authoritative object");
		const auto HistoricalCreatedId = *CreatedForIdentity.Object;
		Check(Gateway.Undo(*World, ScriptSecurityContext::StudioCoreUi()).Succeeded() &&
			!ObjectRegistry::Get().Lookup(HistoricalCreatedId),
			"Undo Create destroys the historical generation");
		auto ReusedSlotObject = std::make_shared<Folder>();
		ReusedSlotObject->SetArchivable(true);
		ReusedSlotObject->SetParent(FirstParent);
		Check(Gateway.Redo(*World, ScriptSecurityContext::StudioCoreUi()).Succeeded(),
			"Redo Create restores the captured semantic object");
		const auto RestoredCreatedId = World->Transactions.ResolveIdentity(HistoricalCreatedId);
		Check(RestoredCreatedId != HistoricalCreatedId && ObjectRegistry::Get().Lookup(RestoredCreatedId) &&
			!Gateway.Apply(DestroyObjectCommand{HistoricalCreatedId}, ScriptSecurityContext::CoreTrusted()).Succeeded(),
			"Redo Create allocates a fresh identity and the stale generation cannot affect it after slot reuse");
		Check(Gateway.Apply(DestroyObjectCommand{RestoredCreatedId},
			MutationAuthorityContext::Studio(ScriptSecurityContext::StudioCoreUi(), Scope, std::nullopt, 41)).Succeeded() &&
			Gateway.Undo(*World, ScriptSecurityContext::StudioCoreUi()).Succeeded(),
			"identity alias chain fixture restores a later Delete of a previously restored Create");
		const auto TwiceRestoredCreatedId = World->Transactions.ResolveIdentity(HistoricalCreatedId);
		Check(TwiceRestoredCreatedId != RestoredCreatedId &&
			Gateway.Apply(UpdatePropertyCommand{TwiceRestoredCreatedId, "Name", std::string("AliasBranch")},
				MutationAuthorityContext::Studio(ScriptSecurityContext::StudioCoreUi(), Scope, std::nullopt, 41)).Succeeded() &&
			World->Transactions.ResolveIdentity(HistoricalCreatedId) == TwiceRestoredCreatedId,
			"redo-branch truncation flattens retained identity aliases onto the current generation");

		const auto NoOpRevision = World->GetAuthoritativeRevision();
		const auto HistoryBeforeNoOp = World->Transactions.GetCommitted().size();
		auto NoOp = World->Transactions.Begin(*World, 41, "No-op", TransactionOrigin::Studio);
		auto NoOpAuthority = MutationAuthorityContext::Studio(
			ScriptSecurityContext::StudioCoreUi(), Scope, NoOp.Id, 41
		);
		Check(
			Gateway.Apply(UpdatePropertyCommand{TargetId, "Name", std::string("After")}, NoOpAuthority).Succeeded(),
			"explicit transaction accepts a validated no-op mutation"
		);
		auto NoOpCommit = World->Transactions.Commit(*World, NoOp.Id, 41);
		Check(
			NoOpCommit.Status == TransactionStatus::NoChanges && World->GetAuthoritativeRevision() == NoOpRevision &&
				World->Transactions.GetCommitted().size() == HistoryBeforeNoOp,
			"no-op transaction creates no history and advances no revision"
		);

		auto Owned = World->Transactions.Begin(*World, 41, "Owner Guard", TransactionOrigin::Studio);
		Check(
			World->Transactions.Commit(*World, Owned.Id, 99).Status == TransactionStatus::WrongOwner &&
				World->Transactions.Commit(*World, Owned.Id, 41).Status == TransactionStatus::NoChanges,
			"another session cannot commit an engine-issued transaction identity"
		);
		auto Expiring = World->Transactions.Begin(*World, 41, "Lifetime Guard", TransactionOrigin::Studio);
		Check(
			World->Transactions
						.ExpireOwner(*World, 41, std::chrono::steady_clock::now() + MaximumOpenTransactionLifetime)
						.Status == TransactionStatus::NoChanges &&
				!World->Transactions.IsOpen(Expiring.Id, 41),
			"bounded transaction lifetime terminates an abandoned empty group"
		);
		const auto ExpiringRevision = World->GetAuthoritativeRevision();
		auto ChangedExpiry = World->Transactions.Begin(*World, 41, "Changed Lifetime Guard", TransactionOrigin::Studio);
		auto ChangedExpiryAuthority = MutationAuthorityContext::Studio(
			ScriptSecurityContext::StudioCoreUi(), Scope, ChangedExpiry.Id, 41
		);
		Check(
			Gateway
				.Apply(UpdatePropertyCommand{TargetId, "Name", std::string("Expired Change")}, ChangedExpiryAuthority)
				.Succeeded(),
			"changed lifetime setup mutation succeeds"
		);
		auto ExpiredCommit = World->Transactions.ExpireOwner(
			*World, 41, std::chrono::steady_clock::now() + MaximumOpenTransactionLifetime
		);
		Check(
			ExpiredCommit.Status == TransactionStatus::Success &&
				World->GetAuthoritativeRevision() == ExpiringRevision + 1 &&
				!World->Transactions.IsOpen(ChangedExpiry.Id, 41),
			"expired commit-only grouping commits already-applied changes instead of implying rollback"
		);

		const auto RejectedRevision = World->GetAuthoritativeRevision();
		auto RejectedGroup = World->Transactions.Begin(*World, 41, "Rejected Later Change", TransactionOrigin::Studio);
		auto RejectedAuthority = MutationAuthorityContext::Studio(
			ScriptSecurityContext::StudioCoreUi(), Scope, RejectedGroup.Id, 41
		);
		Check(
			Gateway.Apply(
					   UpdatePropertyCommand{TargetId, "Name", std::string("Valid Before Rejection")}, RejectedAuthority
			)
					.Succeeded() &&
				!Gateway
					 .Apply(
						 UpdatePropertyCommand{TargetId, "MissingProperty", std::string("Rejected")}, RejectedAuthority
					 )
					 .Succeeded(),
			"a rejected later mutation leaves the explicit commit-only group open and deterministic"
		);
		auto RejectedCommit = World->Transactions.Commit(*World, RejectedGroup.Id, 41);
		Check(
			RejectedCommit.Status == TransactionStatus::Success && RejectedCommit.ChangeCount == 1 &&
				World->GetAuthoritativeRevision() == RejectedRevision + 1,
			"only successful changes enter and advance a commit-only transaction"
		);

		const auto ImplicitStart = World->GetAuthoritativeRevision();
		Check(
			Gateway.Apply(
					   UpdatePropertyCommand{TargetId, "Name", std::string("Implicit")},
					   MutationAuthorityContext::Studio(ScriptSecurityContext::StudioCoreUi(), Scope, std::nullopt, 41)
			)
					.Succeeded() &&
				World->GetAuthoritativeRevision() == ImplicitStart + 1 &&
				World->Transactions.GetCommitted().back().Label == "Set Name",
			"one-shot Studio mutation creates and commits one implicit transaction"
		);
		const auto BeforeHistoryExecution = World->GetAuthoritativeRevision();
		auto UndoImplicit = Gateway.Undo(*World, ScriptSecurityContext::StudioCoreUi());
		auto RedoImplicit = Gateway.Redo(*World, ScriptSecurityContext::StudioCoreUi());
		Check(UndoImplicit.Succeeded() && RedoImplicit.Succeeded() && Target->GetName() == "Implicit" &&
			World->GetAuthoritativeRevision() == BeforeHistoryExecution + 2 &&
			World->Transactions.GetStatus().CanUndo && !World->Transactions.GetStatus().CanRedo,
			"authoritative property Undo and Redo move the cursor and each advance monotonic revision once");
		Check(Gateway.Undo(*World, ScriptSecurityContext::StudioCoreUi()).Succeeded(),
			"branch-invalidation fixture moves behind the newest transaction");
		Check(Gateway.Apply(
			UpdatePropertyCommand{TargetId, "Name", std::string("Divergent")},
			MutationAuthorityContext::Studio(ScriptSecurityContext::StudioCoreUi(), Scope, std::nullopt, 41)
		).Succeeded() && !World->Transactions.GetStatus().CanRedo &&
			Gateway.Redo(*World, ScriptSecurityContext::StudioCoreUi()).Status == TransactionStatus::NothingToRedo,
			"new persistent authoring after Undo deterministically truncates the redo branch");

		for (std::size_t Index = 0; Index < MaximumRetainedTransactions + 4; ++Index)
			Check(
				Gateway
					.Apply(
						UpdatePropertyCommand{TargetId, "Name", std::string("Eviction-") + std::to_string(Index)},
						MutationAuthorityContext::Studio(ScriptSecurityContext::StudioCoreUi(), Scope, std::nullopt, 41)
					)
					.Succeeded(),
				"bounded history setup mutation succeeds"
			);
		Check(
			World->Transactions.GetCommitted().size() == MaximumRetainedTransactions &&
				World->Transactions.GetRetainedBytes() <= MaximumRetainedTransactionBytes,
			"history evicts oldest entries deterministically at the exact count bound"
		);
		for (std::size_t Index = 0; Index < 50; ++Index)
			Check(Gateway.Undo(*World, ScriptSecurityContext::StudioCoreUi()).Succeeded(),
				"bounded cursor stress Undo succeeds");
		for (std::size_t Index = 0; Index < 25; ++Index)
			Check(Gateway.Redo(*World, ScriptSecurityContext::StudioCoreUi()).Succeeded(),
				"bounded cursor stress Redo succeeds");
		Check(Gateway.Apply(
			UpdatePropertyCommand{TargetId, "Name", std::string("Stress-Divergence")},
			MutationAuthorityContext::Studio(ScriptSecurityContext::StudioCoreUi(), Scope, std::nullopt, 41)
		).Succeeded() && !World->Transactions.GetStatus().CanRedo &&
			World->Transactions.GetStatus().Cursor == World->Transactions.GetStatus().RetainedCount,
			"100-plus transaction stress preserves the cursor through Undo, Redo, branch truncation, and eviction");

		PropertyTransactionChange AggregatePrototype{
			TargetId,
			SchemaId::FromNativeName("Engine", "Instance"),
			1,
			"Name",
			WireValue(std::string(192, 'a')),
			WireValue(std::string(192, 'b')),
		};
		const auto AggregateChangeBytes = EstimateTransactionChangeBytes(AggregatePrototype);
		AuthoritativeTransactionHistory AggregateHistory(16, AggregateChangeBytes * 2);
		TransactionId FirstAggregateId;
		for (std::size_t Index = 0; Index < 3; ++Index) {
			auto Aggregate = AggregateHistory.Begin(
				*World, 73, "Aggregate Bound", TransactionOrigin::InternalAuthoring
			);
			if (Index == 0) FirstAggregateId = Aggregate.Id;
			AggregateHistory.RecordMutation(Aggregate.Id, 73, AggregatePrototype, {});
			Check(
				AggregateHistory.Commit(*World, Aggregate.Id, 73).Succeeded(),
				"aggregate history fixture commits a bounded semantic record"
			);
		}
		Check(
			AggregateHistory.GetCommitted().size() == 2 &&
				AggregateHistory.GetCommitted().front().Id != FirstAggregateId &&
				AggregateHistory.GetRetainedBytes() == AggregateChangeBytes * 2,
			"aggregate byte bound independently evicts the oldest transaction exactly"
		);

		auto DeleteRoot = std::make_shared<Folder>();
		DeleteRoot->SetArchivable(true);
		DeleteRoot->SetParent(FirstParent);
		auto DeleteChild = std::make_shared<Folder>();
		DeleteChild->SetArchivable(true);
		DeleteChild->SetParent(DeleteRoot);
		DeleteChild->ApplyAttributeMutation(
			"State", WireValue(std::string("retained")), ScriptSecurityContext::CoreTrusted()
		);
		World->Tags.Add(Scope, DeleteChild->GetObjectId(), "HistoryTag", ScriptSecurityContext::CoreTrusted());
		const auto DeleteStarted = std::chrono::steady_clock::now();
		Check(
			Gateway
				.Apply(
					DestroyObjectCommand{DeleteRoot->GetObjectId()},
					MutationAuthorityContext::Studio(ScriptSecurityContext::StudioCoreUi(), Scope, std::nullopt, 41)
				)
				.Succeeded(),
			"bounded subtree delete commits through an implicit transaction"
		);
		const auto DeleteElapsed =
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - DeleteStarted).count();
		const auto &DeleteRecord = World->Transactions.GetCommitted().back();
		const auto *DeleteChange = std::get_if<SubtreeTransactionChange>(&DeleteRecord.Changes.front());
		Check(
			DeleteChange && DeleteChange->Kind == SubtreeTransactionKind::Destroy &&
				DeleteChange->Objects.size() == 2 &&
				DeleteChange->PersistentSnapshot.find("HistoryTag") != std::string::npos &&
				DeleteChange->PersistentSnapshot.find("retained") != std::string::npos,
			"delete history owns a bounded persistent subtree snapshot and stable destroyed identities"
		);

		const auto CustomClassId = SchemaId::FromCustomClassName("Game", "CombatFolder");
		const auto ExtensionId = SchemaId::FromExtensionName("Game.Data", "FolderProperties");
		double UndoDuplicateElapsed = 0.0;
		double RedoDuplicateElapsed = 0.0;
		double UndoDeleteElapsed = 0.0;
		double RedoDeleteElapsed = 0.0;
		auto CustomSource = InstanceClassRegistry::ConstructByName("Game.CombatFolder");
		Check(CustomSource != nullptr, "transaction duplicate fixture constructs the active custom class");
		if (CustomSource) {
			CustomSource->SetArchivable(true);
			CustomSource->SetName("TransactionCustomSource");
			CustomSource->SetParent(FirstParent);
			Check(
				CustomSource->ApplyPropertyMutation(
					"Health", 73, Enums::Permission::None, ScriptSecurityContext::CoreTrusted()
				) == MutationStatus::Success &&
					CustomSource->ApplyAttributeMutation(
						"HistoryAttribute",
						WireValue(std::string("attribute-state")),
						ScriptSecurityContext::CoreTrusted()
					) == MutationStatus::Success &&
					CustomSource->ApplyExtensionPropertyMutation(
						ExtensionId,
						1,
						"Label",
						WireValue(std::string("extension-state")),
						ScriptSecurityContext::CoreTrusted()
					) == MutationStatus::Success,
				"transaction duplicate fixture establishes custom, Attribute, and extension state"
			);
			World->Tags.Add(
				Scope, CustomSource->GetObjectId(), "transaction-tag", ScriptSecurityContext::CoreTrusted()
			);
			auto CustomChild = std::make_shared<Folder>();
			CustomChild->SetArchivable(true);
			CustomChild->SetName("TransactionCustomChild");
			CustomChild->SetParent(CustomSource);
			const auto SourceId = CustomSource->GetObjectId();
			const auto ChildId = CustomChild->GetObjectId();
			auto Duplicated = Gateway.Apply(
				DuplicateObjectCommand{SourceId},
				MutationAuthorityContext::Studio(ScriptSecurityContext::StudioCoreUi(), Scope, std::nullopt, 41)
			);
			const auto &DuplicateRecord = World->Transactions.GetCommitted().back();
			const auto *DuplicateChange = std::get_if<SubtreeTransactionChange>(&DuplicateRecord.Changes.front());
			Check(
				Duplicated.Succeeded() && Duplicated.Object && *Duplicated.Object != SourceId && DuplicateChange &&
					DuplicateChange->Kind == SubtreeTransactionKind::Duplicate &&
					DuplicateChange->Objects.size() == 2 &&
					std::ranges::none_of(
						DuplicateChange->Objects, [&](ObjectId Id) { return Id == SourceId || Id == ChildId; }
					) &&
					DuplicateChange->PersistentSnapshot.find(CustomClassId.ToString()) != std::string::npos &&
					DuplicateChange->PersistentSnapshot.find("attribute-state") != std::string::npos &&
					DuplicateChange->PersistentSnapshot.find("transaction-tag") != std::string::npos &&
					DuplicateChange->PersistentSnapshot.find("extension-state") != std::string::npos &&
					DuplicateChange->PersistentSnapshot.find("TransactionCustomChild") != std::string::npos,
				"duplicate history captures fresh subtree identities plus custom, extension, Attribute, Tag, and "
				"descendant state"
			);
			if (DuplicateChange && Duplicated.Object) {
				const auto HistoricalDuplicate = *Duplicated.Object;
				const auto UndoDuplicateStarted = std::chrono::steady_clock::now();
				const auto UndoDuplicateResult = Gateway.Undo(*World, ScriptSecurityContext::StudioCoreUi());
				UndoDuplicateElapsed = std::chrono::duration<double, std::milli>(
					std::chrono::steady_clock::now() - UndoDuplicateStarted).count();
				Check(UndoDuplicateResult.Succeeded() &&
					!ObjectRegistry::Get().Lookup(HistoricalDuplicate),
					"Undo Duplicate destroys the captured duplicate generation");
				CustomSource->Destroy();
				const auto RedoDuplicateStarted = std::chrono::steady_clock::now();
				const auto RedoDuplicateResult = Gateway.Redo(*World, ScriptSecurityContext::StudioCoreUi());
				RedoDuplicateElapsed = std::chrono::duration<double, std::milli>(
					std::chrono::steady_clock::now() - RedoDuplicateStarted).count();
				Check(RedoDuplicateResult.Succeeded(),
					"Redo Duplicate restores captured state without depending on the source object");
				const auto RestoredId = World->Transactions.ResolveIdentity(HistoricalDuplicate);
				auto Restored = ObjectRegistry::Get().Lookup(RestoredId);
				Check(Restored && RestoredId != HistoricalDuplicate && Restored->FindFirstChild("TransactionCustomChild", false) &&
					Restored->ReadPropertyWireValue("Health") == std::optional<WireValue>(73) &&
					Restored->GetAttributeValue("HistoryAttribute", ScriptSecurityContext::CoreTrusted()) ==
						std::optional<WireValue>(std::string("attribute-state")) &&
					World->Tags.Has(Scope, RestoredId, "transaction-tag", ScriptSecurityContext::CoreTrusted()) &&
					Restored->GetExtensionPropertyValue(ExtensionId, "Label", ScriptSecurityContext::CoreTrusted()) ==
						WireValue(std::string("extension-state")),
					"Redo Duplicate allocates fresh identities and restores descendants, Attributes, Tags, extensions, and custom state");
				Check(Gateway.Apply(DestroyObjectCommand{RestoredId},
					MutationAuthorityContext::Studio(ScriptSecurityContext::StudioCoreUi(), Scope, std::nullopt, 41)).Succeeded(),
					"Delete restoration performance fixture destroys the restored custom subtree");
				const auto UndoDeleteStarted = std::chrono::steady_clock::now();
				const auto UndoDeleteResult = Gateway.Undo(*World, ScriptSecurityContext::StudioCoreUi());
				UndoDeleteElapsed = std::chrono::duration<double, std::milli>(
					std::chrono::steady_clock::now() - UndoDeleteStarted).count();
				const auto RedoDeleteStarted = std::chrono::steady_clock::now();
				const auto RedoDeleteResult = Gateway.Redo(*World, ScriptSecurityContext::StudioCoreUi());
				RedoDeleteElapsed = std::chrono::duration<double, std::milli>(
					std::chrono::steady_clock::now() - RedoDeleteStarted).count();
				Check(UndoDeleteResult.Succeeded() && RedoDeleteResult.Succeeded(),
					"small custom subtree Delete Undo/Redo performance cycle succeeds");
			}
		}

		auto MissingParent = std::make_shared<Folder>();
		MissingParent->SetArchivable(true);
		MissingParent->SetParent(World);
		auto StableParent = std::make_shared<Folder>();
		StableParent->SetArchivable(true);
		StableParent->SetParent(World);
		auto AtomicTarget = std::make_shared<Folder>();
		AtomicTarget->SetArchivable(true);
		AtomicTarget->SetParent(MissingParent);
		Check(Gateway.Apply(ReparentObjectCommand{AtomicTarget->GetObjectId(), StableParent->GetObjectId()},
			MutationAuthorityContext::Studio(ScriptSecurityContext::StudioCoreUi(), Scope, std::nullopt, 41)).Succeeded(),
			"failed-history atomicity fixture commits a reparent transaction");
		MissingParent->Destroy();
		const auto FailedUndoRevision = World->GetAuthoritativeRevision();
		const auto FailedUndoStatus = World->Transactions.GetStatus();
		auto FailedUndo = Gateway.Undo(*World, ScriptSecurityContext::StudioCoreUi());
		Check(FailedUndo.Status == TransactionStatus::ExecutionFailed &&
			World->GetAuthoritativeRevision() == FailedUndoRevision &&
			World->Transactions.GetStatus().Cursor == FailedUndoStatus.Cursor &&
			AtomicTarget->GetParent() && *AtomicTarget->GetParent() == StableParent,
			"incompatible Reparent Undo fails before mutation with cursor, revision, and hierarchy unchanged");

		auto MeasurePropertyEdits = [&](std::size_t Count, bool Explicit) {
			const auto Started = std::chrono::steady_clock::now();
			std::optional<TransactionResult> Group;
			if (Explicit)
				Group = World->Transactions.Begin(*World, 41, "Performance Sanity", TransactionOrigin::Studio);
			for (std::size_t Index = 0; Index < Count; ++Index) {
				auto PerformanceAuthority = MutationAuthorityContext::Studio(
					ScriptSecurityContext::StudioCoreUi(), Scope, Group ? std::optional(Group->Id) : std::nullopt, 41
				);
				Check(
					Gateway
						.Apply(
							UpdatePropertyCommand{
								TargetId,
								"Name",
								std::string("Performance-") + std::to_string(Count) + "-" + std::to_string(Index)
							},
							PerformanceAuthority
						)
						.Succeeded(),
					"transaction performance sanity mutation succeeds"
				);
			}
			if (Group)
				Check(
					World->Transactions.Commit(*World, Group->Id, 41).Succeeded(),
					"transaction performance sanity group commits"
				);
			return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Started).count();
		};
		const auto SinglePropertyElapsed = MeasurePropertyEdits(1, false);
		const auto TenPropertyElapsed = MeasurePropertyEdits(10, true);
		const auto HundredPropertyElapsed = MeasurePropertyEdits(100, true);
		const auto UndoHundredStarted = std::chrono::steady_clock::now();
		Check(Gateway.Undo(*World, ScriptSecurityContext::StudioCoreUi()).Succeeded(),
			"100-change transaction performance Undo succeeds");
		const auto UndoHundredElapsed = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - UndoHundredStarted).count();
		const auto RedoHundredStarted = std::chrono::steady_clock::now();
		Check(Gateway.Redo(*World, ScriptSecurityContext::StudioCoreUi()).Succeeded(),
			"100-change transaction performance Redo succeeds");
		const auto RedoHundredElapsed = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - RedoHundredStarted).count();
		std::cout << "METRIC transaction_single_property_ms " << SinglePropertyElapsed << '\n'
				  << "METRIC transaction_explicit_10_ms " << TenPropertyElapsed << '\n'
				  << "METRIC transaction_explicit_100_ms " << HundredPropertyElapsed << '\n'
				  << "METRIC transaction_delete_subtree_2_ms " << DeleteElapsed << '\n'
				  << "METRIC undo_property_transaction_100_ms " << UndoHundredElapsed << '\n'
				  << "METRIC redo_property_transaction_100_ms " << RedoHundredElapsed << '\n';
		std::cout << "METRIC undo_duplicate_subtree_2_ms " << UndoDuplicateElapsed << '\n'
				  << "METRIC redo_duplicate_subtree_2_ms " << RedoDuplicateElapsed << '\n'
				  << "METRIC undo_delete_subtree_2_ms " << UndoDeleteElapsed << '\n'
				  << "METRIC redo_delete_subtree_2_ms " << RedoDeleteElapsed << '\n';
	}

	void TestProjectRevisionPersistence() {
		using namespace gargantuan;
		const auto TemporaryRoot = std::filesystem::temp_directory_path() /
			("gargantuan-project-revision-" + std::to_string(
				std::chrono::steady_clock::now().time_since_epoch().count()
			));
		struct TemporaryProjectCleanup {
			std::filesystem::path Root;
			~TemporaryProjectCleanup() { std::filesystem::remove_all(Root); }
		} Cleanup{TemporaryRoot};
		std::filesystem::create_directories(TemporaryRoot);
		DiskFilesystem Filesystem(TemporaryRoot);
		auto ProjectState = Project::fromInit(&Filesystem, "RevisionWorld");
		auto World = ProjectState.DeserializeGame();
		World->InitializeLoadedProjectRevision();
		Check(World->GetAuthoritativeRevision() == DataModel::InitialProjectRevision,
			"loaded projects initialize the explicit authoritative revision");

		for (int Revision = 2; Revision <= 10; ++Revision)
			World->SetName("Revision" + std::to_string(Revision));
		Check(World->GetAuthoritativeRevision() == 10,
			"saved reflected mutations advance the project revision monotonically");
		auto Captured = ProjectState.CaptureGame(World, World->GetAuthoritativeRevision());
		ProjectState.PersistGameAtomically(Captured, [&] { World->SetName("Revision11"); });
		Check(Captured.Revision == 10 && World->GetAuthoritativeRevision() == 11,
			"mutation during persistence leaves the exact saved revision behind the authoritative revision");
		auto PersistedWorld = ProjectState.DeserializeGame();
		Check(PersistedWorld->GetName() == "Revision10" && World->GetName() == "Revision11",
			"the persistence snapshot is coherent and excludes a later mutation");

		const auto Original = Filesystem.ReadFileToString(ProjectState.InstanceFilePath);
		bool FailedBeforeReplace = false;
		try {
			auto FailureSnapshot = ProjectState.CaptureGame(World, World->GetAuthoritativeRevision());
			ProjectState.PersistGameAtomically(FailureSnapshot, [] {
				throw std::runtime_error("deterministic pre-replacement failure");
			});
		} catch (const std::runtime_error &) {
			FailedBeforeReplace = true;
		}
		Check(FailedBeforeReplace && Filesystem.ReadFileToString(ProjectState.InstanceFilePath) == Original,
			"failure before atomic replacement preserves the prior valid project file");
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
		auto PersistedNode = [](std::string Name, std::string ClassName, SchemaId ClassId, Json Children) {
			return Json{
				{"Name", std::move(Name)}, {"ClassName", std::move(ClassName)},
				{"ClassSchemaId", ClassId.ToString()}, {"ClassDefinitionVersion", 1},
				{"Properties", Json::object()}, {"Attributes", Json::object()},
				{"Extensions", Json::array()}, {"CustomProperties", Json::array()},
				{"Tags", Json::array()}, {"Children", std::move(Children)},
			};
		};
		Json WorkspaceChildren = Json::array({PersistedNode(
			"PickTarget", "Part", SchemaId::FromNativeName("Engine", "Part"), Json::array()
		)});
		Json ProjectChildren = Json::array({
			PersistedNode("Editable", "Folder", SchemaId::FromNativeName("Engine", "Folder"), Json::array()),
			PersistedNode("CustomEditable", "Game.StudioFolder",
				SchemaId::FromCustomClassName("Game", "StudioFolder"), Json::array()),
			PersistedNode("Workspace", "Workspace", SchemaId::FromNativeName("Engine", "Workspace"),
				std::move(WorkspaceChildren)),
		});
		auto ProjectDocument = PersistedNode(
			"EditorWorld", "DataModel", SchemaId::FromNativeName("Engine", "DataModel"), std::move(ProjectChildren)
		);
		ProjectDocument["Version"] = 4;
		project << ProjectDocument.dump();
		project.close();
		std::ofstream preRun(temporaryRoot / ".gargantuan" / "prerun.luau", std::ios::binary);
		preRun << R"(Schema:RegisterEnum({
			Namespace = "Game",
			Name = "CombatState",
			Version = 1,
			Items = { Idle = 0, Attacking = 1, Blocking = 2 },
		})
		Schema:RegisterExtension({
			Namespace = "Game.Combat",
			Name = "CombatProperties",
			Version = 1,
			Target = "Engine.BasePart",
			Properties = { Damage = { Type = "Integer", Default = 0 } },
		})
		Schema:RegisterClass({
			Namespace = "Game",
			Name = "StudioFolder",
			Version = 1,
			Base = "Engine.Folder",
			Properties = { Score = { Type = "Integer", Default = 10 } },
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
		Check(
			std::ranges::contains(handshake["Result"]["Capabilities"], "CreateProject") &&
				std::ranges::contains(handshake["Result"]["Capabilities"], "SaveProject") &&
				std::ranges::contains(handshake["Result"]["Capabilities"], "SaveProjectAs") &&
				std::ranges::contains(handshake["Result"]["Capabilities"], "AuthoritativeRevision") &&
				std::ranges::contains(handshake["Result"]["Capabilities"], "CreateInstance") &&
				std::ranges::contains(handshake["Result"]["Capabilities"], "DestroyInstance") &&
				std::ranges::contains(handshake["Result"]["Capabilities"], "DuplicateInstance") &&
				std::ranges::contains(handshake["Result"]["Capabilities"], "ReparentInstance") &&
				std::ranges::contains(handshake["Result"]["Capabilities"], "BeginTransaction") &&
				std::ranges::contains(handshake["Result"]["Capabilities"], "CommitTransaction") &&
				std::ranges::contains(handshake["Result"]["Capabilities"], "Undo") &&
				std::ranges::contains(handshake["Result"]["Capabilities"], "Redo") &&
				std::ranges::contains(handshake["Result"]["Capabilities"], "AuthoritativeHistoryStatus") &&
				std::ranges::contains(handshake["Result"]["Capabilities"], "ReadScriptSource") &&
				std::ranges::contains(handshake["Result"]["Capabilities"], "WriteScriptSource") &&
				std::ranges::contains(handshake["Result"]["Capabilities"], "PlaySession") &&
				std::ranges::contains(handshake["Result"]["Capabilities"], "DiagnosticStream") &&
				std::ranges::contains(handshake["Result"]["Capabilities"], "SendPlayInput") &&
				!std::ranges::contains(handshake["Result"]["Capabilities"], "AbortTransaction"),
			"EditorHost advertises persistence, structural authoring, and honest commit-only transaction capabilities"
		);
		auto SaveWithoutProject = call("SaveProject", Json::object(), "test-token");
		Check(!SaveWithoutProject["Ok"].get<bool>() && SaveWithoutProject["Error"]["Code"] == "NoProjectLoaded",
			"EditorHost returns a structured error when Save has no loaded project");
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
		for (const auto Method : {"Undo", "Redo"}) {
			Json RestrictedRequest{{"Version", EditorHostProtocolVersion}, {"RequestId", Method},
				{"SessionToken", "restricted-token"}, {"Method", Method}, {"Params", Json::object()}};
			auto RestrictedHistory = Json::parse(restrictedHost.HandleRequest(RestrictedRequest.dump()));
			Check(!RestrictedHistory["Ok"].get<bool>() && RestrictedHistory["Error"]["Code"] == "Unauthorized",
				"EditorHost history execution requires trusted authoring mutation capability");
		}
		for (const auto Method : {"GetScriptSource", "SetScriptSource"}) {
			Json RestrictedRequest{{"Version", EditorHostProtocolVersion}, {"RequestId", Method},
				{"SessionToken", "restricted-token"}, {"Method", Method}, {"Params", Json::object()}};
			auto RestrictedSource = Json::parse(restrictedHost.HandleRequest(RestrictedRequest.dump()));
			Check(!RestrictedSource["Ok"].get<bool>() && RestrictedSource["Error"]["Code"] == "Unauthorized",
				"EditorHost script source access requires trusted editor authority");
		}
		{
			Json RestrictedRequest{{"Version", EditorHostProtocolVersion}, {"RequestId", "StartPlaySession"},
				{"SessionToken", "restricted-token"}, {"Method", "StartPlaySession"}, {"Params", Json::object()}};
			auto RestrictedPlay = Json::parse(restrictedHost.HandleRequest(RestrictedRequest.dump()));
			Check(!RestrictedPlay["Ok"].get<bool>() && RestrictedPlay["Error"]["Code"] == "Unauthorized",
				"EditorHost Play requires trusted editor authority");
		}
		{
			Json RestrictedRequest{{"Version", EditorHostProtocolVersion}, {"RequestId", "CreateProject"},
				{"SessionToken", "restricted-token"}, {"Method", "CreateProject"},
				{"Params", {{"Destination", (temporaryRoot / "restricted-new").string()}, {"Name", "Restricted"}}}};
			auto RestrictedCreate = Json::parse(restrictedHost.HandleRequest(RestrictedRequest.dump()));
			Check(!RestrictedCreate["Ok"].get<bool>() && RestrictedCreate["Error"]["Code"] == "Unauthorized",
				"EditorHost project creation requires trusted EditorCommands authority");
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

		auto InvalidCreatePath = call("CreateProject", {
			{"Destination", "relative-project"}, {"Name", "Relative"}
		}, "test-token");
		Check(!InvalidCreatePath["Ok"].get<bool>() && InvalidCreatePath["Error"]["Code"] == "InvalidDestination",
			"CreateProject rejects non-absolute destinations");
		auto InvalidCreateName = call("CreateProject", {
			{"Destination", (temporaryRoot / "invalid-name").string()}, {"Name", "   "}
		}, "test-token");
		Check(!InvalidCreateName["Ok"].get<bool>() && InvalidCreateName["Error"]["Code"] == "InvalidProjectName",
			"CreateProject rejects non-visible project names");
		auto SpoofedCreateState = call("CreateProject", {
			{"Destination", (temporaryRoot / "spoofed-state").string()}, {"Name", "Spoofed"},
			{"AuthoritativeRevision", 99}
		}, "test-token");
		Check(!SpoofedCreateState["Ok"].get<bool>() && SpoofedCreateState["Error"]["Code"] == "MalformedRequest",
			"CreateProject rejects request-supplied revision authority");
		auto MissingParentCreate = call("CreateProject", {
			{"Destination", (temporaryRoot / "missing-parent" / "project").string()}, {"Name", "Missing Parent"}
		}, "test-token");
		Check(!MissingParentCreate["Ok"].get<bool>() && MissingParentCreate["Error"]["Code"] == "InvalidDestination",
			"CreateProject rejects a destination whose parent is absent");

		const auto EmptyDestination = temporaryRoot / "existing-empty";
		std::filesystem::create_directory(EmptyDestination);
		auto ExistingEmpty = call("CreateProject", {
			{"Destination", EmptyDestination.string()}, {"Name", "Existing Empty"}
		}, "test-token");
		Check(!ExistingEmpty["Ok"].get<bool>() && ExistingEmpty["Error"]["Code"] == "DestinationExists" &&
			std::filesystem::is_empty(EmptyDestination),
			"CreateProject conservatively preserves and rejects an existing empty directory");

		const auto NonemptyDestination = temporaryRoot / "existing-nonempty";
		std::filesystem::create_directory(NonemptyDestination);
		std::ofstream(NonemptyDestination / "user-file.txt") << "preserve";
		auto ExistingNonempty = call("CreateProject", {
			{"Destination", NonemptyDestination.string()}, {"Name", "Existing Nonempty"}
		}, "test-token");
		Check(!ExistingNonempty["Ok"].get<bool>() && ExistingNonempty["Error"]["Code"] == "DestinationExists" &&
			std::filesystem::is_regular_file(NonemptyDestination / "user-file.txt"),
			"CreateProject rejects an existing nonempty directory without touching user content");

		const auto FailedCreateDestination = temporaryRoot / "failed-create";
		host.SetPersistenceCheckpointForTesting([] { throw std::runtime_error("injected create failure"); });
		auto FailedCreate = call("CreateProject", {
			{"Destination", FailedCreateDestination.string()}, {"Name", "Failed Project"}
		}, "test-token");
		host.SetPersistenceCheckpointForTesting({});
		Check(!FailedCreate["Ok"].get<bool>() && FailedCreate["Error"]["Code"] == "PersistenceFailure" &&
			!std::filesystem::exists(FailedCreateDestination),
			"CreateProject removes only its owned staging artifacts after persistence failure");

		const auto CreatedRoot = temporaryRoot / "created-project";
		auto CreatedProject = call("CreateProject", {
			{"Destination", CreatedRoot.string()}, {"Name", "Created Project"}
		}, "test-token");
		Check(CreatedProject["Ok"].get<bool>() &&
			CreatedProject["Result"]["ProjectState"]["AuthoritativeRevision"] == DataModel::InitialProjectRevision &&
			CreatedProject["Result"]["ProjectState"]["PersistedRevision"] == DataModel::InitialProjectRevision &&
			!CreatedProject["Result"]["ProjectState"]["Dirty"].get<bool>() &&
			!CreatedProject["Result"]["ProjectState"]["History"]["CanUndo"].get<bool>() &&
			!CreatedProject["Result"]["ProjectState"]["History"]["CanRedo"].get<bool>(),
			"CreateProject activates a clean revision-one session with empty history");
		const auto CreatedProjectPath = CreatedRoot / ".gargantuan" / "project.instance.json";
		std::ifstream CreatedProjectStream(CreatedProjectPath, std::ios::binary);
		const auto CreatedProjectDocument = Json::parse(CreatedProjectStream);
		CreatedProjectStream.close();
		Check(CreatedProjectDocument["Version"] == 4 && CreatedProjectDocument["Name"] == "Created Project" &&
			CreatedProjectDocument["Children"].size() == 1 &&
			CreatedProjectDocument["Children"][0]["ClassName"] == "Workspace" &&
			CreatedProjectDocument["Children"][0]["Name"] == "Workspace",
			"CreateProject immediately persists the minimal DataModel plus Workspace in project format v4");
		auto ExistingProjectCreate = call("CreateProject", {
			{"Destination", CreatedRoot.string()}, {"Name", "Overwrite Attempt"}
		}, "test-token");
		Check(!ExistingProjectCreate["Ok"].get<bool>() && ExistingProjectCreate["Error"]["Code"] == "ExistingProject",
			"CreateProject never overwrites an existing Gargantuan project");
		auto CreatedSnapshot = call("GetSnapshot", Json::object(), "test-token");
		Check(CreatedSnapshot["Ok"].get<bool>() && CreatedSnapshot["Result"]["Snapshot"]["Objects"].size() == 2,
			"the minimum new project is immediately available through the normal authoritative snapshot path");
		auto CreatedWorkspace = std::find_if(
			CreatedSnapshot["Result"]["Snapshot"]["Objects"].begin(),
			CreatedSnapshot["Result"]["Snapshot"]["Objects"].end(),
			[](const Json &Object) { return Object["ClassName"] == "Workspace"; }
		);
		Check(CreatedWorkspace != CreatedSnapshot["Result"]["Snapshot"]["Objects"].end(),
			"the minimum new project exposes its canonical Workspace");
		if (CreatedWorkspace != CreatedSnapshot["Result"]["Snapshot"]["Objects"].end()) {
			auto NewFolder = call("CreateInstance", {
				{"ClassSchemaId", SchemaId::FromNativeName("Engine", "Folder").ToString()},
				{"DefinitionVersion", 1}, {"Parent", (*CreatedWorkspace)["Id"]}, {"Name", "First Folder"}
			}, "test-token");
			auto EditedCreatedState = call("GetProjectState", Json::object(), "test-token");
			Check(NewFolder["Ok"].get<bool>() &&
				EditedCreatedState["Result"]["AuthoritativeRevision"] == DataModel::InitialProjectRevision + 1 &&
				EditedCreatedState["Result"]["PersistedRevision"] == DataModel::InitialProjectRevision &&
				EditedCreatedState["Result"]["Dirty"].get<bool>() &&
				EditedCreatedState["Result"]["History"]["CanUndo"].get<bool>(),
				"a new project immediately supports normal structural authoring, revision, and history semantics");
			auto SavedCreatedProject = call("SaveProject", Json::object(), "test-token");
			Check(SavedCreatedProject["Ok"].get<bool>() &&
				!SavedCreatedProject["Result"]["Dirty"].get<bool>() &&
				SavedCreatedProject["Result"]["History"]["CanUndo"].get<bool>(),
				"saving a new project makes it clean without clearing authoritative history");
			auto ReopenedCreatedProject = call("OpenProject", {{"Root", CreatedRoot.string()}}, "test-token");
			auto ReopenedCreatedSnapshot = call("GetSnapshot", Json::object(), "test-token");
			Check(ReopenedCreatedProject["Ok"].get<bool>() &&
				!ReopenedCreatedProject["Result"]["ProjectState"]["Dirty"].get<bool>() &&
				!ReopenedCreatedProject["Result"]["ProjectState"]["History"]["CanUndo"].get<bool>() &&
				std::ranges::any_of(ReopenedCreatedSnapshot["Result"]["Snapshot"]["Objects"],
					[](const Json &Object) { return Object["Name"] == "First Folder"; }),
				"a saved new project reopens clean with persisted hierarchy and session-local history cleared");
			const auto FailedReplacementRoot = temporaryRoot / "failed-replacement";
			host.SetPersistenceCheckpointForTesting([] { throw std::runtime_error("injected replacement failure"); });
			auto FailedReplacement = call("CreateProject", {
				{"Destination", FailedReplacementRoot.string()}, {"Name", "Failed Replacement"}
			}, "test-token");
			host.SetPersistenceCheckpointForTesting({});
			auto StateAfterFailedReplacement = call("GetProjectState", Json::object(), "test-token");
			auto SnapshotAfterFailedReplacement = call("GetSnapshot", Json::object(), "test-token");
			Check(!FailedReplacement["Ok"].get<bool>() &&
				FailedReplacement["Error"]["Code"] == "PersistenceFailure" &&
				!std::filesystem::exists(FailedReplacementRoot) &&
				StateAfterFailedReplacement["Result"] == ReopenedCreatedProject["Result"]["ProjectState"] &&
				std::ranges::any_of(SnapshotAfterFailedReplacement["Result"]["Snapshot"]["Objects"],
					[](const Json &Object) { return Object["Name"] == "First Folder"; }),
				"failed CreateProject staging preserves the prior active session and removes no user destination");

			auto ScriptSnapshot = call("GetSnapshot", Json::object(), "test-token");
			auto ScriptWorkspace = std::find_if(
				ScriptSnapshot["Result"]["Snapshot"]["Objects"].begin(),
				ScriptSnapshot["Result"]["Snapshot"]["Objects"].end(),
				[](const Json &Object) { return Object["ClassName"] == "Workspace"; }
			);
			Check(ScriptWorkspace != ScriptSnapshot["Result"]["Snapshot"]["Objects"].end(),
				"script authoring fixture resolves the authoritative Workspace");
			if (ScriptWorkspace != ScriptSnapshot["Result"]["Snapshot"]["Objects"].end()) {
				auto CreatedScript = call("CreateInstance", {
					{"ClassSchemaId", SchemaId::FromNativeName("Engine", "Script").ToString()},
					{"DefinitionVersion", 1}, {"Parent", (*ScriptWorkspace)["Id"]}, {"Name", "AuthoringScript"}
				}, "test-token");
				Check(CreatedScript["Ok"].get<bool>(), "Script is constructible through normal structural authoring");
				(void)call("PollChanges", Json::object(), "test-token");
				const auto ScriptId = CreatedScript["Result"]["Object"];
				auto InitialSource = call("GetScriptSource", {{"Object", ScriptId}}, "test-token");
				Check(InitialSource["Ok"].get<bool>() && InitialSource["Result"]["Source"] == "" &&
					InitialSource["Result"]["SourceVersion"].get<int>() > 0,
					"new Script has empty authoritative source and a bounded conflict token");
				const auto InitialVersion = InitialSource["Result"]["SourceVersion"].get<int>();
				auto SourcePrivacySnapshot = call("GetSnapshot", Json::object(), "test-token");
				auto SourcePrivacyObject = std::find_if(
					SourcePrivacySnapshot["Result"]["Snapshot"]["Objects"].begin(),
					SourcePrivacySnapshot["Result"]["Snapshot"]["Objects"].end(),
					[&](const Json &Object) { return Object["Id"] == ScriptId; }
				);
				Check(SourcePrivacyObject != SourcePrivacySnapshot["Result"]["Snapshot"]["Objects"].end() &&
					!(*SourcePrivacyObject)["Properties"].contains("Source") &&
					(*SourcePrivacyObject)["Properties"].contains("SourceVersion"),
					"gameplay/editor snapshots expose only source invalidation state, never source text");
				auto GenericSourceWrite = call("SetProperty", {
					{"Object", ScriptId}, {"Property", "Source"},
					{"Value", {{"Type", "String"}, {"Value", "bypass"}}}
				}, "test-token");
				Check(!GenericSourceWrite["Ok"].get<bool>() && GenericSourceWrite["Error"]["Code"] == "Unauthorized",
					"generic property mutation cannot bypass source conflict semantics");
				const std::string SourceA =
					"-- unicode: \xF0\x9F\x9A\x80\n"
					"print(\"hello\")\n"
					"print(\"value\", 123, true)\n"
					"warn(\"warning\", 456)\n"
					"error(\"runtime failure\")\n";
				auto BeforeSourceState = call("GetProjectState", Json::object(), "test-token");
				auto SetSourceA = call("SetScriptSource", {
					{"Object", ScriptId}, {"ExpectedSourceVersion", InitialVersion}, {"Source", SourceA}
				}, "test-token");
				auto AfterSourceState = call("GetProjectState", Json::object(), "test-token");
				Check(SetSourceA["Ok"].get<bool>() &&
					SetSourceA["Result"]["AuthoritativeRevision"] ==
						BeforeSourceState["Result"]["AuthoritativeRevision"].get<std::uint64_t>() + 1 &&
					AfterSourceState["Result"]["History"]["UndoLabel"] == "Edit Script" &&
					AfterSourceState["Result"]["History"]["SemanticBytes"].get<std::uint64_t>() > 0,
					"one source commit advances the project revision exactly once");
				auto SourceChanges = call("PollChanges", Json::object(), "test-token");
				Check(SourceChanges["Ok"].get<bool>() && SourceChanges["Result"]["Records"].size() == 1 &&
					SourceChanges["Result"]["Records"][0]["Operation"] == "PropertyUpdate" &&
					SourceChanges["Result"]["Records"][0]["PropertyName"] == "SourceVersion" &&
					SourceChanges["Result"]["Records"][0].dump().find(SourceA) == std::string::npos,
					"source commit journals only its invalidation token, never source text");
				const auto SourceAVersion = SetSourceA["Result"]["SourceVersion"].get<int>();
				auto StaleWrite = call("SetScriptSource", {
					{"Object", ScriptId}, {"ExpectedSourceVersion", InitialVersion}, {"Source", "stale overwrite"}
				}, "test-token");
				auto AfterStale = call("GetScriptSource", {{"Object", ScriptId}}, "test-token");
				Check(!StaleWrite["Ok"].get<bool>() && StaleWrite["Error"]["Code"] == "SourceConflict" &&
					AfterStale["Result"]["Source"] == SourceA &&
					call("PollChanges", Json::object(), "test-token")["Result"]["Records"].empty(),
					"stale source token rejects without mutation, revision, or journal publication");
				auto StateBeforePlay = call("GetProjectState", Json::object(), "test-token")["Result"];
				auto StartPlay = call("StartPlaySession", Json::object(), "test-token");
				Check(StartPlay["Ok"].get<bool>() && StartPlay["Result"]["State"] == "Running" &&
					StartPlay["Result"]["LaunchAuthoritativeRevision"] == StateBeforePlay["AuthoritativeRevision"],
					"Play starts an isolated runtime from the current authoritative in-memory revision");
				const auto PlayId = StartPlay["Result"]["PlaySessionId"];
				Check(call("SendPlayInput", {
					{"PlaySessionId", PlayId}, {"Type", "Focus"}, {"Focused", true}
				}, "test-token")["Ok"].get<bool>(), "focused viewport input reaches the exact active Play session");
				auto RightDown = call("SendPlayInput", {
					{"PlaySessionId", PlayId}, {"Type", "PointerButton"},
					{"Button", "Right"}, {"State", "Pressed"}, {"X", 10.0}, {"Y", 20.0}
				}, "test-token");
				Check(RightDown["Ok"].get<bool>() && RightDown["Result"]["RelativePointerMode"] == true,
					"Play RMB down reaches the runtime camera and requests relative pointer capture");
				Check(call("SendPlayInput", {
					{"PlaySessionId", PlayId}, {"Type", "PointerMove"},
					{"X", 12.0}, {"Y", 19.0}, {"DeltaX", 2.0}, {"DeltaY", -1.0}
				}, "test-token")["Ok"].get<bool>(), "Play pointer movement reaches the active runtime exactly once");
				auto RightUp = call("SendPlayInput", {
					{"PlaySessionId", PlayId}, {"Type", "PointerButton"},
					{"Button", "Right"}, {"State", "Released"}, {"X", 12.0}, {"Y", 19.0}
				}, "test-token");
				Check(RightUp["Ok"].get<bool>() && RightUp["Result"]["RelativePointerMode"] == false,
					"Play RMB up reaches the runtime camera and releases relative pointer capture");
				(void)call("SendPlayInput", {
					{"PlaySessionId", PlayId}, {"Type", "PointerButton"},
					{"Button", "Right"}, {"State", "Pressed"}, {"X", 12.0}, {"Y", 19.0}
				}, "test-token");
				auto FocusLoss = call("SendPlayInput", {
					{"PlaySessionId", PlayId}, {"Type", "Focus"}, {"Focused", false}
				}, "test-token");
				Check(FocusLoss["Ok"].get<bool>() && FocusLoss["Result"]["RelativePointerMode"] == false,
					"Play focus loss clears runtime input and releases relative pointer capture");
				Check(!call("StartPlaySession", Json::object(), "test-token")["Ok"].get<bool>(),
					"a second Play request cannot start another local session");
				auto RejectedPlayMutation = call("SetScriptSource", {
					{"Object", ScriptId}, {"ExpectedSourceVersion", SourceAVersion}, {"Source", "return 1"}
				}, "test-token");
				Check(!RejectedPlayMutation["Ok"].get<bool>() && RejectedPlayMutation["Error"]["Code"] == "PlaySessionActive",
					"authoritative source mutation is disabled during Play");
				auto PlayDiagnostics = call("PollPlayDiagnostics", {{"PlaySessionId", PlayId}}, "test-token");
				const auto HasLuauError = [](const Json &Diagnostics) {
					return std::ranges::any_of(Diagnostics, [](const Json &Diagnostic) {
						return Diagnostic["Category"] == "Luau" && Diagnostic["Severity"] == "Error";
					});
				};
				const auto HasDiagnostic = [](const Json &Diagnostics, std::string_view Severity, std::string_view Message) {
					return std::ranges::any_of(Diagnostics, [&](const Json &Diagnostic) {
						return Diagnostic["Category"] == "Luau" &&
							Diagnostic["Severity"].get<std::string>() == Severity &&
							Diagnostic["Message"].get<std::string>() == Message;
					});
				};
				Check(PlayDiagnostics["Ok"].get<bool>() &&
					(HasLuauError(StartPlay["Result"]["Diagnostics"]) ||
						HasLuauError(PlayDiagnostics["Result"]["Diagnostics"])),
					"runtime Script errors are contained and reported through bounded diagnostics");
				Check(
					(HasDiagnostic(StartPlay["Result"]["Diagnostics"], "Information", "hello") ||
						HasDiagnostic(PlayDiagnostics["Result"]["Diagnostics"], "Information", "hello")) &&
					(HasDiagnostic(StartPlay["Result"]["Diagnostics"], "Information", "value\t123\ttrue") ||
						HasDiagnostic(PlayDiagnostics["Result"]["Diagnostics"], "Information", "value\t123\ttrue")) &&
					(HasDiagnostic(StartPlay["Result"]["Diagnostics"], "Warning", "warning\t456") ||
						HasDiagnostic(PlayDiagnostics["Result"]["Diagnostics"], "Warning", "warning\t456")),
					"Play transports print and warn through the ordered Luau diagnostic stream with distinct severity");
				auto StaleStop = call("StopPlaySession", {{"PlaySessionId", "999999"}}, "test-token");
				Check(!StaleStop["Ok"].get<bool>() && StaleStop["Error"]["Code"] == "StalePlaySession",
					"stale PlaySessionId cannot stop the owned runtime");
				auto StopPlay = call("StopPlaySession", {{"PlaySessionId", PlayId}}, "test-token");
				Check(StopPlay["Ok"].get<bool>() && StopPlay["Result"]["State"] == "Stopped" &&
					call("GetProjectState", Json::object(), "test-token")["Result"] == StateBeforePlay,
					"Stop destroys runtime state without changing authoring revision, dirty state, or history");
				for (int Cycle = 0; Cycle < 10; ++Cycle) {
					auto RepeatedStart = call("StartPlaySession", Json::object(), "test-token");
					Check(RepeatedStart["Ok"].get<bool>(), "repeated Play starts successfully");
					Check(call("StopPlaySession", {{"PlaySessionId", RepeatedStart["Result"]["PlaySessionId"]}}, "test-token")["Ok"].get<bool>(),
						"repeated Stop destroys the exact session");
				}
				Check(call("GetProjectState", Json::object(), "test-token")["Result"] == StateBeforePlay,
					"ten Play/Stop cycles preserve authoring state and history");
				auto NonScriptSource = call("GetScriptSource", {{"Object", (*ScriptWorkspace)["Id"]}}, "test-token");
				Check(!NonScriptSource["Ok"].get<bool>() && NonScriptSource["Error"]["Code"] == "NotScript",
					"source reads reject non-script objects");
				auto MaximumSource = std::string(MaximumScriptSourceBytes, 'x');
				auto MaximumWrite = call("SetScriptSource", {
					{"Object", ScriptId}, {"ExpectedSourceVersion", SourceAVersion}, {"Source", MaximumSource}
				}, "test-token");
				Check(MaximumWrite["Ok"].get<bool>(), "script source accepts the exact 64 KiB UTF-8 bound");
				auto OversizedWrite = call("SetScriptSource", {
					{"Object", ScriptId}, {"ExpectedSourceVersion", MaximumWrite["Result"]["SourceVersion"]},
					{"Source", std::string(MaximumScriptSourceBytes + 1, 'x')}
				}, "test-token");
				Check(!OversizedWrite["Ok"].get<bool>(), "script source rejects input above the UTF-8 byte bound");
				(void)call("PollChanges", Json::object(), "test-token");
				const auto BeforeSourceUndoRevision = call("GetProjectState", Json::object(), "test-token")
					["Result"]["AuthoritativeRevision"].get<std::uint64_t>();
				auto UndoSource = call("Undo", Json::object(), "test-token");
				(void)call("PollChanges", Json::object(), "test-token");
				auto AfterUndoSource = call("GetScriptSource", {{"Object", ScriptId}}, "test-token");
				auto RedoSource = call("Redo", Json::object(), "test-token");
				(void)call("PollChanges", Json::object(), "test-token");
				auto AfterRedoSource = call("GetScriptSource", {{"Object", ScriptId}}, "test-token");
				Check(UndoSource["Ok"].get<bool>() &&
					UndoSource["Result"]["ResultingRevision"] == BeforeSourceUndoRevision + 1 &&
					AfterUndoSource["Result"]["Source"] == SourceA &&
					RedoSource["Ok"].get<bool>() &&
					RedoSource["Result"]["ResultingRevision"] == BeforeSourceUndoRevision + 2 &&
					AfterRedoSource["Result"]["Source"] == MaximumSource,
					"authoritative Undo and Redo restore exact script source state");
				auto RestoreA = call("SetScriptSource", {
					{"Object", ScriptId},
					{"ExpectedSourceVersion", AfterRedoSource["Result"]["SourceVersion"]}, {"Source", SourceA}
				}, "test-token");
				Check(RestoreA["Ok"].get<bool>(), "representative source is restored before duplicate persistence proof");
				(void)call("PollChanges", Json::object(), "test-token");
				auto DuplicateScript = call("DuplicateInstance", {{"Object", ScriptId}}, "test-token");
				Check(DuplicateScript["Ok"].get<bool>(), "script Duplicate captures authoritative source state");
				(void)call("PollChanges", Json::object(), "test-token");
				auto DuplicateSource = call("GetScriptSource", {{"Object", DuplicateScript["Result"]["Object"]}}, "test-token");
				Check(call("SetProperty", {
					{"Object", DuplicateScript["Result"]["Object"]}, {"Property", "Name"},
					{"Value", {{"Type", "String"}, {"Value", "AuthoringScriptCopy"}}}
				}, "test-token")["Ok"].get<bool>(), "duplicated script can be renamed independently");
				const std::string SourceB = "local Duplicate = true\n";
				auto EditDuplicate = call("SetScriptSource", {
					{"Object", DuplicateScript["Result"]["Object"]},
					{"ExpectedSourceVersion", DuplicateSource["Result"]["SourceVersion"]}, {"Source", SourceB}
				}, "test-token");
				Check(DuplicateSource["Result"]["Source"] == SourceA && EditDuplicate["Ok"].get<bool>() &&
					call("GetScriptSource", {{"Object", ScriptId}}, "test-token")["Result"]["Source"] == SourceA,
					"duplicated script source is independent from its original");
				(void)call("PollChanges", Json::object(), "test-token");
				Check(call("SaveProject", Json::object(), "test-token")["Ok"].get<bool>(),
					"script source persists through normal project Save");
				Check(call("OpenProject", {{"Root", CreatedRoot.string()}}, "test-token")["Ok"].get<bool>(),
					"saved script project reopens");
				auto ReopenedScriptSnapshot = call("GetSnapshot", Json::object(), "test-token");
				auto OriginalReopened = std::find_if(
					ReopenedScriptSnapshot["Result"]["Snapshot"]["Objects"].begin(),
					ReopenedScriptSnapshot["Result"]["Snapshot"]["Objects"].end(),
					[](const Json &Object) { return Object["Name"] == "AuthoringScript"; }
				);
				Check(OriginalReopened != ReopenedScriptSnapshot["Result"]["Snapshot"]["Objects"].end() &&
					call("GetScriptSource", {{"Object", (*OriginalReopened)["Id"]}}, "test-token")["Result"]["Source"] == SourceA,
					"project v4 Save/reopen preserves exact invalid Unicode, multiline, and trailing-newline source");
				if (OriginalReopened != ReopenedScriptSnapshot["Result"]["Snapshot"]["Objects"].end()) {
					const auto ReopenedId = (*OriginalReopened)["Id"];
					const auto ReopenedSource = call("GetScriptSource", {{"Object", ReopenedId}}, "test-token");
					Check(call("DestroyInstance", {{"Object", ReopenedId}}, "test-token")["Ok"].get<bool>(),
						"script destruction succeeds before stale-generation source proof");
					(void)call("PollChanges", Json::object(), "test-token");
					const auto StaleRead = call("GetScriptSource", {{"Object", ReopenedId}}, "test-token");
					const auto StaleMutation = call("SetScriptSource", {
						{"Object", ReopenedId},
						{"ExpectedSourceVersion", ReopenedSource["Result"]["SourceVersion"]},
						{"Source", "must not target a reused slot"}
					}, "test-token");
					Check(!StaleRead["Ok"].get<bool>() && StaleRead["Error"]["Code"] == "StaleObject" &&
						!StaleMutation["Ok"].get<bool>() && StaleMutation["Error"]["Code"] == "StaleObject",
						"destroyed script generations cannot read or mutate a later slot occupant");
				}
			}
		}
		auto schema = call("GetSchema", Json::object(), "test-token");
		Check(
			schema["Ok"].get<bool>() && schema["Result"]["SchemaDiscoveryVersion"] == 5 &&
				!schema["Result"]["Classes"].empty() && !schema["Result"]["Definitions"].empty(),
			"EditorHost exposes versioned frozen schema discovery without removing the class adapter"
		);

		auto opened = call("OpenProject", {{"Root", temporaryRoot.string()}}, "test-token");
		Check(opened["Ok"].get<bool>() &&
			opened["Result"]["ProjectState"]["AuthoritativeRevision"] == DataModel::InitialProjectRevision &&
			opened["Result"]["ProjectState"]["PersistedRevision"] == DataModel::InitialProjectRevision &&
			!opened["Result"]["ProjectState"]["Dirty"].get<bool>(),
			"EditorHost opens a project clean with coherent authoritative and persisted revisions");
		auto projectSchema = call("GetSchema", Json::object(), "test-token");
		auto discoveredEnum = std::find_if(
			projectSchema["Result"]["Definitions"].begin(), projectSchema["Result"]["Definitions"].end(),
			[](const Json &definition) { return definition["CanonicalName"] == "Game.CombatState"; }
		);
		auto discoveredExtension = std::find_if(
			projectSchema["Result"]["Definitions"].begin(), projectSchema["Result"]["Definitions"].end(),
			[](const Json &definition) { return definition["CanonicalName"] == "Game.Combat.CombatProperties"; }
		);
		auto DiscoveredCustomClass = std::find_if(
			projectSchema["Result"]["Definitions"].begin(), projectSchema["Result"]["Definitions"].end(),
			[](const Json &Definition) { return Definition["CanonicalName"] == "Game.StudioFolder"; }
		);
		auto FindSchemaDefinition = [&](std::string_view CanonicalName) {
			return std::find_if(projectSchema["Result"]["Definitions"].begin(),
				projectSchema["Result"]["Definitions"].end(), [&](const Json &Definition) {
					return Definition["CanonicalName"].get<std::string>() == CanonicalName;
				});
		};
		auto PartSchema = FindSchemaDefinition("Engine.Part");
		auto BasePartSchema = FindSchemaDefinition("Engine.BasePart");
		auto CameraSchema = FindSchemaDefinition("Engine.Camera");
		auto WorkspaceSchema = FindSchemaDefinition("Engine.Workspace");
		auto FindProperty = [](const Json &Definition, std::string_view Name) -> const Json * {
			if (!Definition.contains("Properties")) return nullptr;
			auto Property = std::find_if(Definition["Properties"].begin(), Definition["Properties"].end(),
				[&](const Json &Candidate) { return Candidate["Name"].get<std::string>() == Name; });
			return Property == Definition["Properties"].end() ? nullptr : &*Property;
		};
		const auto *ShapeMetadata = PartSchema == projectSchema["Result"]["Definitions"].end()
			? nullptr : FindProperty(*PartSchema, "Shape");
		const auto *TransparencyMetadata = BasePartSchema == projectSchema["Result"]["Definitions"].end()
			? nullptr : FindProperty(*BasePartSchema, "Transparency");
		const auto *ColorMetadata = BasePartSchema == projectSchema["Result"]["Definitions"].end()
			? nullptr : FindProperty(*BasePartSchema, "Color");
		const auto *CFrameMetadata = BasePartSchema == projectSchema["Result"]["Definitions"].end()
			? nullptr : FindProperty(*BasePartSchema, "CFrame");
		const auto *CameraFieldOfView = CameraSchema == projectSchema["Result"]["Definitions"].end()
			? nullptr : FindProperty(*CameraSchema, "FieldOfView");
		const auto *CameraViewport = CameraSchema == projectSchema["Result"]["Definitions"].end()
			? nullptr : FindProperty(*CameraSchema, "ViewportSize");
		const auto *CurrentCameraMetadata = WorkspaceSchema == projectSchema["Result"]["Definitions"].end()
			? nullptr : FindProperty(*WorkspaceSchema, "CurrentCamera");
		std::set<std::string> NativeDataTypes;
		for (const auto &Definition : projectSchema["Result"]["Definitions"])
			if (Definition.value("Kind", "") == "Class")
				for (const auto &Property : Definition["Properties"])
					NativeDataTypes.insert(Property["DataType"].get<std::string>());
		Check(ShapeMetadata && (*ShapeMetadata)["DataType"] == "NativeEnum" &&
			(*ShapeMetadata)["WireType"] == "EnumItem" && (*ShapeMetadata)["EnumKind"] == "Native" &&
			(*ShapeMetadata)["EnumType"] == "PartType" && (*ShapeMetadata)["EnumItems"].size() >= 5 &&
			TransparencyMetadata && (*TransparencyMetadata)["NumericRange"]["Minimum"] == 0.0 &&
			(*TransparencyMetadata)["NumericRange"]["Maximum"] == 1.0 &&
			ColorMetadata && (*ColorMetadata)["CompoundType"] == "Color3" &&
			CFrameMetadata && (*CFrameMetadata)["EditorHint"] == "CFrameComponents" &&
			CameraFieldOfView && (*CameraFieldOfView)["Editable"].get<bool>() &&
			CameraViewport && !(*CameraViewport)["Editable"].get<bool>() &&
			CurrentCameraMetadata && (*CurrentCameraMetadata)["DataType"] == "ObjectReference" &&
			!(*CurrentCameraMetadata)["Editable"].get<bool>() && (*CurrentCameraMetadata)["Nullable"].get<bool>() &&
			(*CurrentCameraMetadata)["ObjectReferenceClassSchemaId"] ==
				SchemaId::FromNativeName("Engine", "Camera").ToString(),
			"EditorHost round-trips native editor semantics, ranges, exact enum identity, and safe reference constraints");
		Check(std::ranges::all_of(std::array{
			"Bool", "Integer", "Number", "String", "Vector2", "Vector3", "Color3", "CFrame",
			"NativeEnum", "ObjectReference",
		}, [&](std::string_view Type) { return NativeDataTypes.contains(std::string(Type)); }),
			"the frozen native schema emits metadata for the primitive and compound datatypes it currently declares");
		Check(
			projectSchema["Ok"].get<bool>() && discoveredEnum != projectSchema["Result"]["Definitions"].end() &&
				(*discoveredEnum)["Kind"] == "Enum" && (*discoveredEnum)["Provenance"] == "Game" &&
				(*discoveredEnum)["Items"].size() == 3,
			"EditorHost exposes immutable project enum metadata after PreRun publication"
		);
		Check(
			discoveredExtension != projectSchema["Result"]["Definitions"].end() &&
				(*discoveredExtension)["Kind"] == "Extension" &&
				(*discoveredExtension)["TargetClassSchemaId"] ==
					SchemaId::FromNativeName("Engine", "BasePart").ToString() &&
				(*discoveredExtension)["Properties"].size() == 1 &&
				(*discoveredExtension)["Properties"][0]["Default"]["Type"] == "Int",
			"EditorHost schema discovery v5 exposes immutable bounded extension metadata"
		);
		Check(DiscoveredCustomClass != projectSchema["Result"]["Definitions"].end() &&
			(*DiscoveredCustomClass)["Kind"] == "Class" &&
			(*DiscoveredCustomClass)["ConstructionKind"] == "CustomData" &&
			(*DiscoveredCustomClass)["NativeHostClassSchemaId"] ==
				SchemaId::FromNativeName("Engine", "Folder").ToString() &&
			(*DiscoveredCustomClass)["Properties"].size() == 1,
			"EditorHost schema discovery exposes immutable custom class construction and property metadata");
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
		auto ClosedSnapshot = call("GetSnapshot", Json::object(), "test-token");
		Check(
			!ClosedSnapshot["Ok"].get<bool>() && ClosedSnapshot["Error"]["Code"] == "ProjectRequired",
			"schema replacement closes the previous live world before running project PreRun"
		);
		auto Reopened = call("OpenProject", {{"Root", temporaryRoot.string()}}, "test-token");
		Check(Reopened["Ok"].get<bool>(), "EditorHost can construct a fresh world after a rejected replacement");
		auto snapshot = call("GetSnapshot", Json::object(), "test-token");
		Check(snapshot["Ok"].get<bool>(), "EditorHost returns a cursor-paired snapshot");
		auto &objects = snapshot["Result"]["Snapshot"]["Objects"];
		auto editable = std::find_if(objects.begin(), objects.end(), [](const Json &object) {
			return object["Name"] == "Editable";
		});
		auto extensionTarget = std::find_if(objects.begin(), objects.end(), [](const Json &object) {
			return object["Name"] == "PickTarget";
		});
		auto CustomEditable = std::find_if(objects.begin(), objects.end(), [](const Json &Object) {
			return Object["Name"] == "CustomEditable";
		});
		auto WorkspaceObject = std::find_if(objects.begin(), objects.end(), [](const Json &Object) {
			return Object["Name"] == "Workspace";
		});
		Check(editable != objects.end(), "EditorHost snapshot contains the project hierarchy");
		Check(CustomEditable != objects.end() && (*CustomEditable)["ClassName"] == "Game.StudioFolder" &&
			(*CustomEditable)["ClassSchemaId"] == SchemaId::FromCustomClassName("Game", "StudioFolder").ToString(),
			"EditorHost snapshot carries stable custom class identity/version");
		Check(WorkspaceObject != objects.end(), "EditorHost snapshot contains the protected Workspace service");
		Check(snapshot["Result"]["Snapshot"]["EditorPropertyValuesVersion"] == 1 &&
			extensionTarget != objects.end() && (*extensionTarget).contains("EditorProperties") &&
			(*extensionTarget)["EditorProperties"].contains("Shape") &&
			(*extensionTarget)["EditorProperties"].contains("CFrame"),
			"EditorHost snapshot carries a versioned authoritative native-property projection");
		if (extensionTarget != objects.end() && WorkspaceObject != objects.end() && ShapeMetadata &&
			PartSchema != projectSchema["Result"]["Definitions"].end() &&
			BasePartSchema != projectSchema["Result"]["Definitions"].end() &&
			WorkspaceSchema != projectSchema["Result"]["Definitions"].end()) {
			auto SetNative = [&](std::string_view Property, const Json &Value, const Json &DeclaringId,
				std::uint32_t DeclaringVersion = 1) {
				return call("SetProperty", {
					{"Object", (*extensionTarget)["Id"]}, {"ClassSchemaId", (*PartSchema)["SchemaId"]},
					{"ClassDefinitionVersion", (*PartSchema)["DefinitionVersion"]},
					{"DeclaringClassSchemaId", DeclaringId},
					{"DeclaringDefinitionVersion", DeclaringVersion},
					{"Property", Property}, {"Value", Value},
				}, "test-token");
			};
			const auto BeforeRejectedNative = (*extensionTarget)["EditorProperties"];
			auto StaleNative = SetNative("Shape", {{"Type", "EnumItem"}, {"Enum", "PartType"}, {"Value", "Ball"}},
				(*PartSchema)["SchemaId"], 2);
			auto MalformedNative = SetNative("CFrame", {{"Type", "CFrame"}, {"Value", {1.0, 2.0}}},
				(*BasePartSchema)["SchemaId"]);
			auto OutOfRangeNative = SetNative("Transparency", {{"Type", "Float"}, {"Value", 2.0}},
				(*BasePartSchema)["SchemaId"]);
			auto ReadOnlyNative = SetNative("Position", {{"Type", "Vector3"}, {"Value", {1.0, 2.0, 3.0}}},
				(*BasePartSchema)["SchemaId"]);
			auto WrongEnumType = SetNative("Shape",
				{{"Type", "EnumItem"}, {"Enum", "CameraType"}, {"Value", "Ball"}},
				(*PartSchema)["SchemaId"]);
			auto DisplayEnumIdentity = SetNative("Shape",
				{{"Type", "EnumItem"}, {"Enum", "PartType"}, {"Value", "Ball (1)"}},
				(*PartSchema)["SchemaId"]);
			auto ReadOnlyReference = call("SetProperty", {
				{"Object", (*WorkspaceObject)["Id"]},
				{"ClassSchemaId", (*WorkspaceSchema)["SchemaId"]},
				{"ClassDefinitionVersion", (*WorkspaceSchema)["DefinitionVersion"]},
				{"DeclaringClassSchemaId", (*WorkspaceSchema)["SchemaId"]},
				{"DeclaringDefinitionVersion", (*WorkspaceSchema)["DefinitionVersion"]},
				{"Property", "CurrentCamera"}, {"Value", {{"Type", "Null"}}},
			}, "test-token");
			auto AfterRejectedSnapshot = call("GetSnapshot", Json::object(), "test-token");
			auto AfterRejectedPart = std::find_if(AfterRejectedSnapshot["Result"]["Snapshot"]["Objects"].begin(),
				AfterRejectedSnapshot["Result"]["Snapshot"]["Objects"].end(), [&](const Json &Object) {
					return Object["Id"] == (*extensionTarget)["Id"];
				});
			Check(!StaleNative["Ok"].get<bool>() && StaleNative["Error"]["Code"] == "StaleSchema" &&
				!MalformedNative["Ok"].get<bool>() && !OutOfRangeNative["Ok"].get<bool>() &&
				!ReadOnlyNative["Ok"].get<bool>() && ReadOnlyNative["Error"]["Code"] == "ReadOnly" &&
				!WrongEnumType["Ok"].get<bool>() && !DisplayEnumIdentity["Ok"].get<bool>() &&
				!ReadOnlyReference["Ok"].get<bool>() && ReadOnlyReference["Error"]["Code"] == "ReadOnly" &&
				AfterRejectedPart != AfterRejectedSnapshot["Result"]["Snapshot"]["Objects"].end() &&
				(*AfterRejectedPart)["EditorProperties"] == BeforeRejectedNative &&
				call("PollChanges", Json::object(), "test-token")["Result"]["Records"].empty(),
				"malformed, out-of-range, stale, display-enum, and read-only native edits preserve authoritative state");

			Check(SetNative("Anchored", {{"Type", "Bool"}, {"Value", true}},
				(*BasePartSchema)["SchemaId"])["Ok"].get<bool>(), "Studio edits native Bool values");
			Check(SetNative("Transparency", {{"Type", "Float"}, {"Value", 0.25}},
				(*BasePartSchema)["SchemaId"])["Ok"].get<bool>(), "Studio edits bounded finite Number values");
			Check(SetNative("Size", {{"Type", "Vector3"}, {"Value", {2.0, 3.0, 4.0}}},
				(*BasePartSchema)["SchemaId"])["Ok"].get<bool>(), "Studio edits native Vector3 values");
			Check(SetNative("Color", {{"Type", "Color3"}, {"Value", {0.2, 0.4, 0.6}}},
				(*BasePartSchema)["SchemaId"])["Ok"].get<bool>(), "Studio edits bounded native Color3 values");
			Check(SetNative("CFrame", {{"Type", "CFrame"},
				{"Value", {0.0, 0.0, 0.0, 0.0, -1.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0}}},
				(*BasePartSchema)["SchemaId"])["Ok"].get<bool>(), "Studio edits bounded-component native CFrame values");
			Check(SetNative("Shape", {{"Type", "EnumItem"}, {"Enum", "PartType"}, {"Value", "Ball"}},
				(*PartSchema)["SchemaId"])["Ok"].get<bool>(), "Studio mutates native enums by canonical item identity");
			auto NativeChanges = call("PollChanges", Json::object(), "test-token");
			Check(NativeChanges["Result"]["Records"].size() == 6 &&
				std::ranges::all_of(NativeChanges["Result"]["Records"], [](const Json &Record) {
					return Record["Operation"] == "PropertyUpdate";
				}) && NativeChanges["Result"]["Records"].back()["Value"] ==
					Json{{"Type", "EnumItem"}, {"Enum", "PartType"}, {"Value", "Ball"}},
				"native editor mutations reconcile only through canonical journal WireValues");
			auto UndoNativeEnum = call("Undo", Json::object(), "test-token");
			auto UndoNativeChanges = call("PollChanges", Json::object(), "test-token");
			auto RedoNativeEnum = call("Redo", Json::object(), "test-token");
			auto RedoNativeChanges = call("PollChanges", Json::object(), "test-token");
			Check(UndoNativeEnum["Ok"].get<bool>() && RedoNativeEnum["Ok"].get<bool>() &&
				UndoNativeChanges["Result"]["Records"].size() == 1 &&
				UndoNativeChanges["Result"]["Records"][0]["Value"]["Value"] == "Block" &&
				RedoNativeChanges["Result"]["Records"].size() == 1 &&
				RedoNativeChanges["Result"]["Records"][0]["Value"]["Value"] == "Ball",
				"Undo and Redo restore exact native enum identity through the authoritative journal");
		}
		Json StructuralDestinationId;
		Json StructuralDuplicateId;
		if (WorkspaceObject != objects.end() && DiscoveredCustomClass != projectSchema["Result"]["Definitions"].end()) {
			auto BeforeCreate = call("GetProjectState", Json::object(), "test-token");
			auto Created = call("CreateInstance", {
				{"ClassSchemaId", (*DiscoveredCustomClass)["SchemaId"]},
				{"DefinitionVersion", 1}, {"Parent", (*WorkspaceObject)["Id"]}, {"Name", "StructuralSource"},
			}, "test-token");
			Check(Created["Ok"].get<bool>() &&
				Created["Result"]["AuthoritativeRevision"] ==
					BeforeCreate["Result"]["AuthoritativeRevision"].get<std::uint64_t>() + 1,
				"CreateInstance allocates authoritative identity and advances one project revision");
			const auto CreatedId = Created["Result"]["Object"];
			auto CreateChanges = call("PollChanges", Json::object(), "test-token");
			Check(std::ranges::any_of(CreateChanges["Result"]["Records"], [&](const Json &Record) {
				return Record["Operation"] == "Create" && Record["ObjectId"] == CreatedId &&
					Record["ClassSchemaId"] == (*DiscoveredCustomClass)["SchemaId"];
			}), "CreateInstance publishes stable identity and exact custom schema through the journal");

			Check(call("SetAttribute", {
				{"Object", CreatedId}, {"Attribute", "Health"}, {"Value", {{"Type", "Int"}, {"Value", 42}}},
			}, "test-token")["Ok"].get<bool>(), "structural source accepts persistent attribute state");
			Check(call("AddTag", {{"Object", CreatedId}, {"Tag", "StructuralTag"}}, "test-token")["Ok"].get<bool>(),
				"structural source accepts persistent tag state");
			Check(call("SetCustomProperty", {
				{"Object", CreatedId}, {"DeclaringClassSchemaId", (*DiscoveredCustomClass)["SchemaId"]},
				{"DefinitionVersion", 1}, {"Property", "Score"},
				{"Value", {{"Type", "Int"}, {"Value", 77}}},
			}, "test-token")["Ok"].get<bool>(), "structural source accepts custom class state");
			(void)call("PollChanges", Json::object(), "test-token");
			auto Child = call("CreateInstance", {
				{"ClassSchemaId", SchemaId::FromNativeName("Engine", "Folder").ToString()},
				{"DefinitionVersion", 1}, {"Parent", CreatedId}, {"Name", "StructuralChild"},
			}, "test-token");
			Check(Child["Ok"].get<bool>(), "CreateInstance supports an ordinary nested child");
			(void)call("PollChanges", Json::object(), "test-token");

			auto BeforeDuplicate = call("GetProjectState", Json::object(), "test-token");
			auto Duplicate = call("DuplicateInstance", {{"Object", CreatedId}}, "test-token");
			Check(Duplicate["Ok"].get<bool>() && Duplicate["Result"]["Object"] != CreatedId &&
				Duplicate["Result"]["AuthoritativeRevision"] ==
					BeforeDuplicate["Result"]["AuthoritativeRevision"].get<std::uint64_t>() + 1,
				"DuplicateInstance clones a subtree with fresh root identity as one project revision");
			StructuralDuplicateId = Duplicate["Result"]["Object"];
			auto DuplicateChanges = call("PollChanges", Json::object(), "test-token");
			Check(std::ranges::count_if(DuplicateChanges["Result"]["Records"], [](const Json &Record) {
				return Record["Operation"] == "Create";
			}) == 2 && std::ranges::any_of(DuplicateChanges["Result"]["Records"], [&](const Json &Record) {
				return Record["ObjectId"] == StructuralDuplicateId && Record["Operation"] == "TagAdded";
			}) && std::ranges::any_of(DuplicateChanges["Result"]["Records"], [&](const Json &Record) {
				return Record["ObjectId"] == StructuralDuplicateId && Record["Operation"] == "AttributeUpdate";
			}) && std::ranges::any_of(DuplicateChanges["Result"]["Records"], [&](const Json &Record) {
				return Record["ObjectId"] == StructuralDuplicateId && Record["Operation"] == "PropertyUpdate" &&
					Record.value("DeclaringClassSchemaId", "") == (*DiscoveredCustomClass)["SchemaId"];
			}), "DuplicateInstance journals its subtree, attributes, tags, and custom state atomically");

			auto Destination = call("CreateInstance", {
				{"ClassSchemaId", SchemaId::FromNativeName("Engine", "Folder").ToString()},
				{"DefinitionVersion", 1}, {"Parent", (*WorkspaceObject)["Id"]}, {"Name", "StructuralDestination"},
			}, "test-token");
			Check(Destination["Ok"].get<bool>(), "structural workflow creates a reparent destination");
			StructuralDestinationId = Destination["Result"]["Object"];
			(void)call("PollChanges", Json::object(), "test-token");
			auto BeforeReparent = call("GetProjectState", Json::object(), "test-token");
			Check(call("ReparentInstance", {
				{"Object", StructuralDuplicateId}, {"Parent", StructuralDestinationId},
			}, "test-token")["Ok"].get<bool>(), "ReparentInstance accepts a valid identity-based move");
			auto ReparentChanges = call("PollChanges", Json::object(), "test-token");
			Check(ReparentChanges["Result"]["Records"].size() == 1 &&
				ReparentChanges["Result"]["Records"][0]["Operation"] == "Reparent" &&
				ReparentChanges["Result"]["ProjectState"]["AuthoritativeRevision"] ==
					BeforeReparent["Result"]["AuthoritativeRevision"].get<std::uint64_t>() + 1,
				"ReparentInstance journals one committed move and advances one revision");

			auto BeforeCycle = call("GetProjectState", Json::object(), "test-token");
			Check(!call("ReparentInstance", {
				{"Object", StructuralDestinationId}, {"Parent", StructuralDuplicateId},
			}, "test-token")["Ok"].get<bool>() &&
				call("PollChanges", Json::object(), "test-token")["Result"]["Records"].empty() &&
				call("GetProjectState", Json::object(), "test-token")["Result"] == BeforeCycle["Result"],
				"cycle rejection leaves hierarchy, revision, and journal unchanged");

			auto BeforeDelete = call("GetProjectState", Json::object(), "test-token");
			Check(call("DestroyInstance", {{"Object", CreatedId}}, "test-token")["Ok"].get<bool>(),
				"DestroyInstance recursively destroys the authoritative subtree");
			auto DeleteChanges = call("PollChanges", Json::object(), "test-token");
			Check(DeleteChanges["Result"]["ProjectState"]["AuthoritativeRevision"] ==
				BeforeDelete["Result"]["AuthoritativeRevision"].get<std::uint64_t>() + 1 &&
				std::ranges::any_of(DeleteChanges["Result"]["Records"], [&](const Json &Record) {
					return Record["Operation"] == "Destroy" && Record["ObjectId"] == CreatedId;
				}), "recursive DestroyInstance is one logical revision with destruction records");

			const auto DeletedRevision = DeleteChanges["Result"]["ProjectState"]["AuthoritativeRevision"].get<std::uint64_t>();
			auto UndoDelete = call("Undo", Json::object(), "test-token");
			auto UndoDeleteChanges = call("PollChanges", Json::object(), "test-token");
			auto RestoredRoot = std::find_if(
				UndoDeleteChanges["Result"]["Records"].begin(), UndoDeleteChanges["Result"]["Records"].end(),
				[&](const Json &Record) {
					return Record["Operation"] == "Create" &&
						Record.value("ClassSchemaId", "") == (*DiscoveredCustomClass)["SchemaId"];
				}
			);
			Check(UndoDelete["Ok"].get<bool>() && UndoDelete["Result"]["ResultingRevision"] == DeletedRevision + 1 &&
				UndoDelete["Result"]["CanRedo"].get<bool>() &&
				RestoredRoot != UndoDeleteChanges["Result"]["Records"].end() && (*RestoredRoot)["ObjectId"] != CreatedId,
				"Undo Delete restores the semantic subtree with fresh authoritative identities and one revision");
			auto RedoDelete = call("Redo", Json::object(), "test-token");
			auto RedoDeleteChanges = call("PollChanges", Json::object(), "test-token");
			Check(RedoDelete["Ok"].get<bool>() && RedoDelete["Result"]["ResultingRevision"] == DeletedRevision + 2 &&
				std::ranges::any_of(RedoDeleteChanges["Result"]["Records"], [&](const Json &Record) {
					return Record["Operation"] == "Destroy" && Record["ObjectId"] == (*RestoredRoot)["ObjectId"];
				}), "Redo Delete destroys the currently restored generation rather than the stale historical identity");

			auto BeforeRejected = call("GetProjectState", Json::object(), "test-token");
			Check(!call("DestroyInstance", {{"Object", CreatedId}}, "test-token")["Ok"].get<bool>() &&
				!call("DuplicateInstance", {{"Object", CreatedId}}, "test-token")["Ok"].get<bool>() &&
				!call("ReparentInstance", {{"Object", CreatedId}, {"Parent", StructuralDestinationId}}, "test-token")["Ok"].get<bool>() &&
				!call("CreateInstance", {
					{"ClassSchemaId", SchemaId::FromNativeName("Engine", "Folder").ToString()},
					{"DefinitionVersion", 1}, {"Parent", CreatedId},
				}, "test-token")["Ok"].get<bool>() &&
				!call("DestroyInstance", {{"Object", (*WorkspaceObject)["Id"]}}, "test-token")["Ok"].get<bool>() &&
				!call("DuplicateInstance", {{"Object", (*WorkspaceObject)["Id"]}}, "test-token")["Ok"].get<bool>() &&
				!call("CreateInstance", {
					{"ClassSchemaId", SchemaId::FromNativeName("Engine", "DataModel").ToString()},
					{"DefinitionVersion", 1}, {"Parent", (*WorkspaceObject)["Id"]},
				}, "test-token")["Ok"].get<bool>() &&
				call("PollChanges", Json::object(), "test-token")["Result"]["Records"].empty() &&
				call("GetProjectState", Json::object(), "test-token")["Result"] == BeforeRejected["Result"],
				"stale and protected structural requests fail atomically without revision or journal changes");
		}
		if (editable != objects.end()) {
			if (!StructuralDestinationId.is_null()) {
				auto BeforeTransaction = call("GetProjectState", Json::object(), "test-token");
				auto OversizedLabel = call("BeginTransaction", {{"Label", std::string(129, 'x')}}, "test-token");
				auto SpoofedTransaction = call(
					"SetProperty",
					{
						{"Object", (*editable)["Id"]},
						{"Property", "Name"},
						{"Value", {{"Type", "String"}, {"Value", "Spoofed"}}},
						{"TransactionId", "18446744073709551615"},
					},
					"test-token"
				);
				Check(
					!OversizedLabel["Ok"].get<bool>() && OversizedLabel["Error"]["Code"] == "TransactionLimit" &&
						!SpoofedTransaction["Ok"].get<bool>() &&
						SpoofedTransaction["Error"]["Code"] == "TransactionNotFound" &&
						call("GetProjectState", Json::object(), "test-token")["Result"] == BeforeTransaction["Result"],
					"bounded labels and invented transaction identities fail without mutation authority"
				);
				auto BeganTransaction = call("BeginTransaction", {{"Label", "Protocol Group"}}, "test-token");
				auto UndoWhileOpen = call("Undo", Json::object(), "test-token");
				Check(
					BeganTransaction["Ok"].get<bool>() && BeganTransaction["Result"]["Status"] == "Open" &&
						!UndoWhileOpen["Ok"].get<bool>() && UndoWhileOpen["Error"]["Code"] == "TransactionOpen",
					"EditorHost issues an engine-owned explicit transaction identity"
				);
				const auto Transaction = BeganTransaction["Result"]["TransactionId"];
				auto SaveWhileOpen = call("SaveProject", Json::object(), "test-token");
				auto PlayWhileOpen = call("StartPlaySession", Json::object(), "test-token");
				Check(
					!SaveWhileOpen["Ok"].get<bool>() && SaveWhileOpen["Error"]["Code"] == "TransactionOpen" &&
					!PlayWhileOpen["Ok"].get<bool>() && PlayWhileOpen["Error"]["Code"] == "TransactionOpen",
					"Save and Play cannot observe an intermediate commit-only transaction state"
				);
				Check(
					call(
						"SetProperty",
						{
							{"Object", (*editable)["Id"]},
							{"Property", "Name"},
							{"Value", {{"Type", "String"}, {"Value", "GroupedThroughProtocol"}}},
							{"TransactionId", Transaction},
						},
						"test-token"
					)["Ok"]
							.get<bool>() &&
						call(
							"SetAttribute",
							{
								{"Object", (*editable)["Id"]},
								{"Attribute", "Grouped"},
								{"Value", {{"Type", "Int"}, {"Value", 9}}},
								{"TransactionId", Transaction},
							},
							"test-token"
						)["Ok"]
							.get<bool>() &&
						call(
							"ReparentInstance",
							{
								{"Object", (*editable)["Id"]},
								{"Parent", StructuralDestinationId},
								{"TransactionId", Transaction},
							},
							"test-token"
						)["Ok"]
							.get<bool>(),
					"EditorHost routes property, Attribute, and reparent requests into one explicit transaction"
				);
				Check(
					call("PollChanges", Json::object(), "test-token")["Result"]["Records"].empty() &&
						call("GetProjectState", Json::object(), "test-token")["Result"]["AuthoritativeRevision"] ==
							BeforeTransaction["Result"]["AuthoritativeRevision"],
					"open protocol transaction publishes neither journal records nor a project revision"
				);
				auto CommittedTransaction = call("CommitTransaction", {{"TransactionId", Transaction}}, "test-token");
				auto GroupedChanges = call("PollChanges", Json::object(), "test-token");
				Check(
					CommittedTransaction["Ok"].get<bool>() && CommittedTransaction["Result"]["ChangeCount"] == 3 &&
						CommittedTransaction["Result"]["ResultingRevision"] ==
							BeforeTransaction["Result"]["AuthoritativeRevision"].get<std::uint64_t>() + 1 &&
						GroupedChanges["Result"]["Records"].size() == 3,
					"CommitTransaction advances one revision and releases the grouped authoritative journal batch"
				);
				auto UndoGroup = call("Undo", Json::object(), "test-token");
				auto UndoGroupChanges = call("PollChanges", Json::object(), "test-token");
				auto RedoGroup = call("Redo", Json::object(), "test-token");
				auto RedoGroupChanges = call("PollChanges", Json::object(), "test-token");
				Check(UndoGroup["Ok"].get<bool>() && RedoGroup["Ok"].get<bool>() &&
					UndoGroupChanges["Result"]["Records"].size() == 3 &&
					RedoGroupChanges["Result"]["Records"].size() == 3 &&
					RedoGroup["Result"]["ResultingRevision"] ==
						CommittedTransaction["Result"]["ResultingRevision"].get<std::uint64_t>() + 2,
					"Undo and Redo execute a grouped property, Attribute, and reparent transaction as ordinary journal batches");
				Check(
					!call("CommitTransaction", {{"TransactionId", Transaction}}, "test-token")["Ok"].get<bool>(),
					"EditorHost rejects duplicate transaction commit"
				);
			}
			auto mutation = call("SetProperty", {
				{"Object", (*editable)["Id"]},
				{"Property", "Name"},
				{"Value", {{"Type", "String"}, {"Value", "EditedThroughProtocol"}}},
			}, "test-token");
			Check(mutation["Ok"].get<bool>(), "EditorHost applies a typed property mutation through the gateway");
			auto changes = call("PollChanges", Json::object(), "test-token");
			Check(
				changes["Ok"].get<bool>() && changes["Result"]["Records"].size() == 1 &&
					changes["Result"]["Records"][0]["Operation"] == "PropertyUpdate" &&
					changes["Result"]["ProjectState"]["Dirty"].get<bool>() &&
					changes["Result"]["ProjectState"]["AuthoritativeRevision"] >
						changes["Result"]["ProjectState"]["PersistedRevision"],
				"EditorHost publishes the committed mutation as one journal record"
			);
			auto UndoProperty = call("Undo", Json::object(), "test-token");
			(void)call("PollChanges", Json::object(), "test-token");
			Check(UndoProperty["Ok"].get<bool>() && UndoProperty["Result"]["CanRedo"].get<bool>(),
				"property Undo moves the authoritative cursor without decrementing project revision");
			Check(call("SetProperty", {
				{"Object", (*editable)["Id"]}, {"Property", "Name"},
				{"Value", {{"Type", "String"}, {"Value", "DivergentPropertyEdit"}}},
			}, "test-token")["Ok"].get<bool>(), "a divergent property edit commits after Undo");
			(void)call("PollChanges", Json::object(), "test-token");
			auto InvalidatedRedo = call("Redo", Json::object(), "test-token");
			Check(!InvalidatedRedo["Ok"].get<bool>() && InvalidatedRedo["Error"]["Code"] == "NothingToRedo" &&
				!call("GetProjectState", Json::object(), "test-token")["Result"]["History"]["CanRedo"].get<bool>(),
				"a new persistent transaction after Undo truncates the redo branch");
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
		if (extensionTarget != objects.end() &&
			discoveredExtension != projectSchema["Result"]["Definitions"].end()) {
			auto extensionMutation = call("SetExtensionProperty", {
				{"Object", (*extensionTarget)["Id"]},
				{"ExtensionSchemaId", (*discoveredExtension)["SchemaId"]},
				{"DefinitionVersion", 1},
				{"Property", "Damage"},
				{"Value", {{"Type", "Int"}, {"Value", 12}}},
			}, "test-token");
			Check(extensionMutation["Ok"].get<bool>(),
				"EditorHost applies extension edits through the authoritative mutation gateway");
			auto extensionChanges = call("PollChanges", Json::object(), "test-token");
			Check(extensionChanges["Ok"].get<bool>() &&
				extensionChanges["Result"]["Records"].size() == 1 &&
				extensionChanges["Result"]["Records"][0]["Operation"] == "ExtensionPropertyUpdate" &&
				extensionChanges["Result"]["Records"][0]["ExtensionSchemaId"] ==
					(*discoveredExtension)["SchemaId"],
				"EditorHost publishes one identity- and version-bearing extension journal record");
			auto ExtensionDuplicate = call("DuplicateInstance", {{"Object", (*extensionTarget)["Id"]}}, "test-token");
			Check(ExtensionDuplicate["Ok"].get<bool>(),
				"DuplicateInstance accepts an Instance with extension state");
			auto ExtensionDuplicateId = ExtensionDuplicate["Result"]["Object"];
			auto ExtensionDuplicateChanges = call("PollChanges", Json::object(), "test-token");
			Check(std::ranges::any_of(ExtensionDuplicateChanges["Result"]["Records"], [&](const Json &Record) {
				return Record["ObjectId"] == ExtensionDuplicateId &&
					Record["Operation"] == "ExtensionPropertyUpdate" &&
					Record["ExtensionSchemaId"] == (*discoveredExtension)["SchemaId"];
			}), "DuplicateInstance independently clones persistent Class Extension overrides");
			Check(call("DestroyInstance", {{"Object", ExtensionDuplicateId}}, "test-token")["Ok"].get<bool>(),
				"extension duplicate cleanup uses authoritative DestroyInstance");
			(void)call("PollChanges", Json::object(), "test-token");
			auto wrongVersion = call("SetExtensionProperty", {
				{"Object", (*extensionTarget)["Id"]},
				{"ExtensionSchemaId", (*discoveredExtension)["SchemaId"]},
				{"DefinitionVersion", 2},
				{"Property", "Damage"},
				{"Value", {{"Type", "Int"}, {"Value", 13}}},
			}, "test-token");
			Check(!wrongVersion["Ok"].get<bool>(),
				"EditorHost rejects extension definition-version mismatch");
			auto OversizedExtensionVersion = call("SetExtensionProperty", {
				{"Object", (*extensionTarget)["Id"]},
				{"ExtensionSchemaId", (*discoveredExtension)["SchemaId"]},
				{"DefinitionVersion", std::uint64_t{4294967297}},
				{"Property", "Damage"},
				{"Value", {{"Type", "Int"}, {"Value", 13}}},
			}, "test-token");
			Check(!OversizedExtensionVersion["Ok"].get<bool>() &&
				OversizedExtensionVersion["Error"]["Code"] == "MalformedRequest",
				"EditorHost rejects extension definition versions outside uint32 before conversion");
			auto rejectedChanges = call("PollChanges", Json::object(), "test-token");
			Check(rejectedChanges["Ok"].get<bool>() && rejectedChanges["Result"]["Records"].empty(),
				"rejected extension edit emits no journal record");
		}
		if (CustomEditable != objects.end() && DiscoveredCustomClass != projectSchema["Result"]["Definitions"].end()) {
			auto CustomMutation = call("SetCustomProperty", {
				{"Object", (*CustomEditable)["Id"]},
				{"DeclaringClassSchemaId", (*DiscoveredCustomClass)["SchemaId"]},
				{"DefinitionVersion", 1}, {"Property", "Score"},
				{"Value", {{"Type", "Int"}, {"Value", 20}}},
			}, "test-token");
			Check(CustomMutation["Ok"].get<bool>(),
				"EditorHost routes custom property edits through authoritative schema-aware mutation");
			auto CustomChanges = call("PollChanges", Json::object(), "test-token");
			Check(CustomChanges["Ok"].get<bool>() && CustomChanges["Result"]["Records"].size() == 1 &&
				CustomChanges["Result"]["Records"][0]["Operation"] == "PropertyUpdate" &&
				CustomChanges["Result"]["Records"][0]["DeclaringClassSchemaId"] ==
					(*DiscoveredCustomClass)["SchemaId"],
				"EditorHost publishes a stable declaring-class identity on custom property journal updates");
			auto WrongCustomVersion = call("SetCustomProperty", {
				{"Object", (*CustomEditable)["Id"]},
				{"DeclaringClassSchemaId", (*DiscoveredCustomClass)["SchemaId"]},
				{"DefinitionVersion", 2}, {"Property", "Score"},
				{"Value", {{"Type", "Int"}, {"Value", 21}}},
			}, "test-token");
			Check(!WrongCustomVersion["Ok"].get<bool>() &&
				call("PollChanges", Json::object(), "test-token")["Result"]["Records"].empty(),
				"EditorHost rejects custom class version mismatch without journaling");
			auto OversizedCustomVersion = call("SetCustomProperty", {
				{"Object", (*CustomEditable)["Id"]},
				{"DeclaringClassSchemaId", (*DiscoveredCustomClass)["SchemaId"]},
				{"DefinitionVersion", std::uint64_t{4294967297}}, {"Property", "Score"},
				{"Value", {{"Type", "Int"}, {"Value", 21}}},
			}, "test-token");
			Check(!OversizedCustomVersion["Ok"].get<bool>() &&
				OversizedCustomVersion["Error"]["Code"] == "MalformedRequest" &&
				call("PollChanges", Json::object(), "test-token")["Result"]["Records"].empty(),
				"EditorHost rejects custom definition versions outside uint32 without mutation or journaling");
		}

		auto Save = call("SaveProject", Json::object(), "test-token");
		Check(Save["Ok"].get<bool>() && !Save["Result"]["Dirty"].get<bool>() &&
			Save["Result"]["PersistedRevision"] == Save["Result"]["AuthoritativeRevision"],
			"Save records the exact persisted revision and leaves an unchanged project clean");
		const auto SavedRevision = Save["Result"]["PersistedRevision"].get<std::uint64_t>();
		Check(Save["Result"]["History"]["CanUndo"].get<bool>(),
			"Save preserves authoritative Undo history");
		auto UndoAfterSave = call("Undo", Json::object(), "test-token");
		auto UndoAfterSaveChanges = call("PollChanges", Json::object(), "test-token");
		Check(UndoAfterSave["Ok"].get<bool>() &&
			UndoAfterSaveChanges["Result"]["ProjectState"]["AuthoritativeRevision"] == SavedRevision + 1 &&
			UndoAfterSaveChanges["Result"]["ProjectState"]["PersistedRevision"] == SavedRevision &&
			UndoAfterSaveChanges["Result"]["ProjectState"]["Dirty"].get<bool>(),
			"Undo after Save advances monotonic revision, preserves PersistedRevision, and becomes dirty");
		auto RedoAfterSave = call("Redo", Json::object(), "test-token");
		auto RedoAfterSaveChanges = call("PollChanges", Json::object(), "test-token");
		Check(RedoAfterSave["Ok"].get<bool>() &&
			RedoAfterSaveChanges["Result"]["ProjectState"]["AuthoritativeRevision"] == SavedRevision + 2 &&
			RedoAfterSaveChanges["Result"]["ProjectState"]["PersistedRevision"] == SavedRevision &&
			RedoAfterSaveChanges["Result"]["ProjectState"]["Dirty"].get<bool>(),
			"Redo after Save independently advances revision without changing the persistence checkpoint");
		Save = call("SaveProject", Json::object(), "test-token");
		Check(Save["Ok"].get<bool>() && !Save["Result"]["Dirty"].get<bool>(),
			"Save after Undo/Redo persists the final authoritative state without clearing history");
		{
			std::ifstream SavedProject(temporaryRoot / ".gargantuan" / "project.instance.json", std::ios::binary);
			Json SavedDocument;
			SavedProject >> SavedDocument;
			Check(SavedDocument["Version"] == 4,
				"Save preserves the current project instance format version");
		}
		{
			DiskFilesystem ReopenFilesystem(temporaryRoot);
			auto ReopenProject = Project::fromExisting(&ReopenFilesystem);
			auto ReopenedWorld = ReopenProject.DeserializeGame();
			auto Destination = ReopenedWorld->FindFirstDescendant("StructuralDestination");
			auto SurvivingDuplicate = Destination ? Destination->FindFirstChild("StructuralSource", false) : nullptr;
			auto ReopenedPart = std::dynamic_pointer_cast<Part>(ReopenedWorld->FindFirstDescendant("PickTarget"));
			Check(Destination && SurvivingDuplicate &&
				SurvivingDuplicate->GetAttributeValue("Health", ScriptSecurityContext::CoreTrusted()) ==
					std::optional<WireValue>(std::int32_t{42}) &&
				ReopenedWorld->Tags.Has(ReopenedWorld->GetObjectId(), SurvivingDuplicate->GetObjectId(),
					"StructuralTag", ScriptSecurityContext::CoreTrusted()) &&
				SurvivingDuplicate->FindFirstChild("StructuralChild", false),
				"Save/reopen preserves duplicate subtree, reparent, state, and prior deletion");
			Check(ReopenedPart && ReopenedPart->GetAnchored(),
				"Save/reopen preserves authoritative native Bool edits");
			Check(ReopenedPart && std::abs(ReopenedPart->GetTransparency() - 0.25f) < 0.0001f,
				"Save/reopen preserves authoritative native Number edits");
			Check(ReopenedPart && ReopenedPart->GetSize() == glm::vec3(2.0f, 3.0f, 4.0f),
				"Save/reopen preserves authoritative native compound edits");
			Check(ReopenedPart && ReopenedPart->GetShape() == Enums::PartType::Ball,
				"Save/reopen preserves authoritative native enum edits");
			ReopenedWorld->Destroy();
		}
		const auto SaveAsRoot = temporaryRoot.parent_path() /
			(temporaryRoot.filename().string() + "-save-as");
		struct SaveAsCleanup {
			std::filesystem::path Root;
			~SaveAsCleanup() { std::filesystem::remove_all(Root); }
		} SaveAsCleanupState{SaveAsRoot};
		const auto ExpectedSaveAsRoot = std::filesystem::weakly_canonical(SaveAsRoot);
		auto SaveAs = call("SaveProjectAs", {{"Destination", SaveAsRoot.string()}}, "test-token");
		Check(SaveAs["Ok"].get<bool>() &&
			SaveAs["Result"]["CurrentDestination"] == ExpectedSaveAsRoot.generic_string() &&
			std::filesystem::is_regular_file(SaveAsRoot / ".gargantuan" / "project.instance.json") &&
			std::filesystem::is_regular_file(SaveAsRoot / ".gargantuan" / "prerun.luau"),
			"Save As persists the existing project format and adopts the destination only after success");
		if (editable != objects.end()) {
			Check(call("SetProperty", {
				{"Object", (*editable)["Id"]}, {"Property", "Name"},
				{"Value", {{"Type", "String"}, {"Value", "DirtyAfterSaveAs"}}},
			}, "test-token")["Ok"].get<bool>(), "post-Save-As mutation succeeds");
			(void)call("PollChanges", Json::object(), "test-token");
		}
		auto StateBeforeFailedSaveAs = call("GetProjectState", Json::object(), "test-token");
		const auto InvalidSaveAsDestination = SaveAsRoot / "not-a-directory";
		{
			std::ofstream InvalidDestinationFile(InvalidSaveAsDestination);
			InvalidDestinationFile << "occupied";
		}
		auto FailedSaveAs = call("SaveProjectAs", {{"Destination", InvalidSaveAsDestination.string()}}, "test-token");
		auto StateAfterFailedSaveAs = call("GetProjectState", Json::object(), "test-token");
		Check(!FailedSaveAs["Ok"].get<bool>() && FailedSaveAs["Error"]["Code"] == "InvalidDestination" &&
			StateAfterFailedSaveAs["Result"]["CurrentDestination"] == ExpectedSaveAsRoot.generic_string() &&
			StateAfterFailedSaveAs["Result"] == StateBeforeFailedSaveAs["Result"] &&
			StateAfterFailedSaveAs["Result"]["Dirty"].get<bool>(),
			"failed Save As preserves the prior destination, persisted revision, and dirty state");
		if (editable != objects.end()) {
			auto BeforeRaceState = call("GetProjectState", Json::object(), "test-token");
			host.SetPersistenceCheckpointForTesting([&] {
				auto DuringSaveMutation = call("SetProperty", {
					{"Object", (*editable)["Id"]}, {"Property", "Name"},
					{"Value", {{"Type", "String"}, {"Value", "MutationDuringSave"}}},
				}, "test-token");
				Check(DuringSaveMutation["Ok"].get<bool>(),
					"deterministic mutation commits while the captured save is before replacement");
			});
			auto RacedSave = call("SaveProject", Json::object(), "test-token");
			host.SetPersistenceCheckpointForTesting({});
			const auto CapturedRevision = BeforeRaceState["Result"]["AuthoritativeRevision"].get<std::uint64_t>();
			Check(RacedSave["Ok"].get<bool>() && RacedSave["Result"]["PersistedRevision"] == CapturedRevision &&
				RacedSave["Result"]["AuthoritativeRevision"] == CapturedRevision + 1 &&
				RacedSave["Result"]["Dirty"].get<bool>(),
				"Save reports persisted revision N and remains dirty when a mutation commits at N+1");
			std::ifstream RacedFile(SaveAsRoot / ".gargantuan" / "project.instance.json", std::ios::binary);
			std::stringstream RacedContents;
			RacedContents << RacedFile.rdbuf();
			Check(RacedContents.str().find("MutationDuringSave") == std::string::npos,
				"raced Save file contains exactly the captured revision rather than later state");
			(void)call("PollChanges", Json::object(), "test-token");
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

		auto Abandoned = call("BeginTransaction", {{"Label", "Replaced Session"}}, "test-token");
		const auto AbandonedId = Abandoned["Result"]["TransactionId"];
		auto ReplacedProject = call("OpenProject", {{"Root", SaveAsRoot.string()}}, "test-token");
		Check(
			Abandoned["Ok"].get<bool>() &&
				ReplacedProject["Ok"].get<bool>() &&
				!ReplacedProject["Result"]["ProjectState"]["History"]["CanUndo"].get<bool>() &&
				!ReplacedProject["Result"]["ProjectState"]["History"]["CanRedo"].get<bool>() &&
				!call("CommitTransaction", {{"TransactionId", AbandonedId}}, "test-token")["Ok"].get<bool>(),
			"project replacement clears history/cursor and old transaction identity cannot affect the new project"
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

	void TestLuauEmbeddingCompatibility() {
		using namespace gargantuan;
		static_assert(LUA_VECTOR_DOUBLE == 0);
		static_assert(LUA_VECTOR_SIZE == 3);
		static_assert(std::is_same_v<LUA_VECTOR_TYPE, float>);

		auto game = std::make_shared<DataModel>();
		std::vector<std::pair<std::string, std::string>> RuntimeDiagnostics;
		ScriptEngine engine(game, [&](std::string Severity, std::string Message) {
			RuntimeDiagnostics.emplace_back(std::move(Severity), std::move(Message));
		});
		lua_State *L = engine.L;
		Check(engine.CompileOptions.vectorPrecision == 0,
			"Luau compiler vector precision matches the float-vector VM");

		auto Load = [&](lua_State *State, std::string_view Source, const char *Name) -> int {
			size_t BytecodeSize = 0;
			char *Bytecode = luau_compile(Source.data(), Source.size(), &engine.CompileOptions, &BytecodeSize);
			if (!Bytecode) return LUA_ERRSYNTAX;
			const auto Status = luau_load(State, Name, Bytecode, BytecodeSize, 0);
			std::free(Bytecode);
			return Status;
		};

		lua_settop(L, 0);
		Check(Load(L, R"(
			local Hostile = setmetatable({}, { __tostring = function() error("must not run") end })
			print("hello")
			print("value", 123, true, nil, game.Workspace, Vector3.new(1, 2, 3), Hostile)
			warn("warning", 456)
			print("unicode ✓\nsecond line")
			print(string.rep("x", 4096))
			print(string.char(255))
			local Many = {}
			for Index = 1, 70 do Many[Index] = Index end
			print(table.unpack(Many))
		)", "luau-runtime-diagnostics") == LUA_OK && lua_pcall(L, 0, 0, 0) == LUA_OK,
			"print and warn format supported values without invoking hostile __tostring code");
		const bool DiagnosticFormattingOk = RuntimeDiagnostics.size() == 7 && RuntimeDiagnostics[0] == std::pair(
			std::string("Information"), std::string("hello")) &&
			RuntimeDiagnostics[1].first == "Information" && RuntimeDiagnostics[1].second.find("value\t123\ttrue\tnil") == 0 &&
			RuntimeDiagnostics[1].second.find("Vector3(1, 2, 3)") != std::string::npos &&
			RuntimeDiagnostics[1].second.ends_with("<table>") &&
			RuntimeDiagnostics[2] == std::pair(std::string("Warning"), std::string("warning\t456"));
		if (!DiagnosticFormattingOk) for (const auto &[Severity, Message] : RuntimeDiagnostics)
			std::cerr << "DIAGNOSTIC " << Severity << " | " << Message << '\n';
		Check(DiagnosticFormattingOk,
			"runtime diagnostics preserve Luau category formatting and print/warn severity");
		Check(RuntimeDiagnostics[3].second == "unicode ✓\nsecond line" &&
			RuntimeDiagnostics[4].second.size() <= 2048 && RuntimeDiagnostics[4].second.ends_with("...<truncated>") &&
			RuntimeDiagnostics[5].second == "<invalid utf-8>" &&
			RuntimeDiagnostics[6].second.ends_with("<arguments truncated>"),
			"runtime diagnostics preserve multiline UTF-8 and bound or sanitize unsafe strings");

		lua_settop(L, 0);
		Check(Load(L, "return Vector3.new(1, 0, 0):Cross(Vector3.new(0, 1, 0)), type(Vector3.zero)",
			"luau-compatibility") == LUA_OK, "Luau compiler bytecode loads into the matching VM");
		Check(lua_pcall(L, 0, 2, 0) == LUA_OK, "compiled Luau executes successfully");
		const auto *Vector = lua_tovector(L, -2);
		Check(Vector && Vector[0] == 0.0f && Vector[1] == 0.0f && Vector[2] == 1.0f,
			"native Vector3 values retain three float components");
		Check(std::string_view(lua_tostring(L, -1)) == "vector", "Vector3 retains Luau's native vector representation");

		lua_settop(L, 0);
		Check(Load(L, R"(
			local Value = 2 * Vector2.new(1, 2)
			local InvalidOk = pcall(function() return "wrong" * Vector2.new(1, 2) end)
			local OrderingOk = pcall(function() return Vector2.zero < Vector2.one end)
			return Value.X, Value.Y, Vector2.new(1, 2):Cross(Vector2.new(3, 4)),
				Vector2.new(1, 2):FuzzyEq(Vector2.new(1.00001, 1.99999), 0.0001),
				InvalidOk, OrderingOk, type(Value)
		)", "luau-vector2-safety") == LUA_OK && lua_pcall(L, 0, 7, 0) == LUA_OK,
			"Vector2 arithmetic executes through the real Luau userdata boundary");
		Check(lua_tonumber(L, -7) == 2.0 && lua_tonumber(L, -6) == 4.0 && lua_tonumber(L, -5) == -2.0,
			"Vector2 scalar-left multiplication and Cross return exact expected values");
		Check(lua_toboolean(L, -4) && !lua_toboolean(L, -3) && !lua_toboolean(L, -2) &&
			std::string_view(lua_tostring(L, -1)) == "userdata",
			"Vector2 FuzzyEq succeeds while invalid multiplication and ordering fail as Luau errors");

		auto Vector2AttributePart = std::make_shared<Folder>();
		Vector2AttributePart->SetParent(game);
		StackValue<std::shared_ptr<Folder>>::Push(L, Vector2AttributePart);
		lua_setglobal(L, "Vector2AttributePart");
		lua_settop(L, 0);
		Check(Load(L, R"(
			Vector2AttributePart:SetAttribute("Offset", Vector2.new(10, 20))
			local Value = Vector2AttributePart:GetAttribute("Offset")
			return Value.X, Value.Y
		)", "luau-vector2-attribute") == LUA_OK && lua_pcall(L, 0, 2, 0) == LUA_OK,
			"Luau stores and retrieves a Vector2 Attribute through MutationGateway");
		Check(lua_tonumber(L, -2) == 10.0 && lua_tonumber(L, -1) == 20.0 &&
			Vector2AttributePart->GetAttributeValue("Offset") == std::optional<WireValue>(WireVector2{10.0f, 20.0f}),
			"Luau Vector2 Attribute access retains exact component values");

		auto WorkspaceValue = std::dynamic_pointer_cast<Workspace>(game->GetService("Workspace"));
		lua_settop(L, 0);
		Check(Load(L, "print(game.Workspace, Enum.PartType.Block, Vector2.new(1, 2))", "luau-diagnostic-userdata") == LUA_OK &&
			lua_pcall(L, 0, 0, 0) == LUA_OK && RuntimeDiagnostics.back().second == "<Instance>\t<EnumItem>\t<Vector2>",
			"runtime diagnostics safely label Instance, EnumItem, and supported userdata values");
		auto KnownPart = std::make_shared<Part>();
		KnownPart->SetName("KnownPart");
		KnownPart->SetArchivable(true);
		KnownPart->SetSize({4.0f, 1.0f, 4.0f});
		KnownPart->SetColor(Color3(0.2f, 0.4f, 0.8f));
		KnownPart->SetParent(WorkspaceValue);
		Check(KnownPart->ApplyAttributeMutation("CloneHealth", WireValue(75), ScriptSecurityContext::CoreTrusted()) ==
			MutationStatus::Success && game->Tags.Add(game->GetObjectId(), KnownPart->GetObjectId(), "CloneTag",
			ScriptSecurityContext::CoreTrusted()), "clone fixture carries Attribute and Tag state");
		lua_settop(L, 0);
		Check(Load(L, R"(
			local Workspace = game.Workspace
			assert(Workspace ~= nil, "game.Workspace is missing")
			local Missing = Workspace:FindFirstChild("DefinitelyMissing")
			assert(Missing == nil and not Missing)
			assert(Workspace:FindFirstChildOfClass("Folder") == nil)
			assert(Workspace:FindFirstChildWhichIsA("Folder") == nil)
			assert(Workspace:FindFirstDescendant("DefinitelyMissing") == nil)
			assert(Workspace:FindFirstDescendantOfClass("Folder") == nil)
			assert(Workspace:FindFirstDescendantWhichIsA("Folder") == nil)
			assert(Workspace:FindFirstAncestor("DefinitelyMissing") == nil)
			assert(Workspace:FindFirstAncestorOfClass("Folder") == nil)
			assert(Workspace:FindFirstAncestorWhichIsA("Folder") == nil)
			assert(Instance.new("Part").Parent == nil)
			assert(Instance.new("WeldConstraint").Part0 == nil)
			local Found = Workspace:FindFirstChild("KnownPart")
			assert(Found ~= nil and Found:IsA("Part"))
			Workspace.Name = "RenamedWorkspace"
			assert(game.Workspace:IsA("Workspace"))
		)", "luau-nullable-instance") == LUA_OK && lua_pcall(L, 0, 0, 0) == LUA_OK,
			"nullable Instance-returning Luau APIs push nil and valid lookups remain usable");

		lua_settop(L, 0);
		Check(Load(L, R"(
			local FolderValue = Instance.new("Folder")
			local PartValue = Instance.new("Part")
			PartValue.Name = "RuntimePart"
			PartValue.Size = Vector3.new(4, 1, 4)
			PartValue.CFrame = CFrame.new(0, 3, 0)
			PartValue.Anchored = true
			PartValue.Parent = FolderValue
			FolderValue.Parent = game.Workspace
			return PartValue, FolderValue
		)", "luau-runtime-adoption") == LUA_OK && lua_pcall(L, 0, 2, 0) == LUA_OK,
			"detached Luau Instance subtrees can be first-adopted into the runtime DataModel");
		Check(WorkspaceValue->FindFirstChildOfClass("Folder", false) != nullptr,
			"runtime first adoption publishes the detached subtree into Workspace");
		Check(WorldRootTestAccess::BodyCount(*WorkspaceValue) == 2,
			"first-adopted Part enters runtime physics alongside the known Part");
		auto AdoptedPart = StackValue<std::shared_ptr<Instance>>::From(L, -2);
		RenderExtractor RuntimeExtractor;
		auto RuntimeSnapshot = RuntimeExtractor.Extract(
			*WorkspaceValue, MakeRenderCameraInput(*WorkspaceValue->GetCurrentCamera()), 320, 200
		);
		Check(AdoptedPart && std::ranges::contains(RuntimeSnapshot->Items, AdoptedPart->GetObjectId(), &RenderItem::Object),
			"first-adopted Part enters runtime render extraction with stable identity");

		lua_settop(L, 0);
		const auto CloneLoadStatus = Load(L, R"(
			local Original = game.Workspace.KnownPart
			local Copy = Original:Clone()
			assert(Copy ~= Original and Copy.Parent == nil)
			assert(Copy.Name == Original.Name and Copy.Size == Original.Size)
			assert(Copy.Color.R == Original.Color.R and Copy.Color.G == Original.Color.G and Copy.Color.B == Original.Color.B)
			assert(Copy:GetAttribute("CloneHealth") == 75)
			Copy.Name = "RuntimeCopy"
			Copy.CFrame = CFrame.new(5, 3, 0)
			Copy.Parent = game.Workspace
			assert(game.Workspace:FindFirstChild("RuntimeCopy") == Copy)
			return Copy
		)", "luau-runtime-clone");
		const auto CloneCallStatus = CloneLoadStatus == LUA_OK ? lua_pcall(L, 0, 1, 0) : CloneLoadStatus;
		if (CloneCallStatus != LUA_OK) std::cerr << "CLONE ERROR: " << lua_tostring(L, -1) << '\n';
		Check(CloneCallStatus == LUA_OK,
			"Instance:Clone returns an independent detached Part that can be first-adopted");
		auto RuntimeCopy = CloneCallStatus == LUA_OK ? StackValue<std::shared_ptr<Instance>>::From(L, -1) : nullptr;
		Check(RuntimeCopy && RuntimeCopy != KnownPart && RuntimeCopy->GetObjectId() != KnownPart->GetObjectId() &&
			KnownPart->GetName() == "KnownPart" && RuntimeCopy->GetName() == "RuntimeCopy" &&
			RuntimeCopy->GetAttributeValue("CloneHealth") == WireValue(75) &&
			game->Tags.Has(game->GetObjectId(), RuntimeCopy->GetObjectId(), "CloneTag",
				ScriptSecurityContext::CoreTrusted()),
			"adopted clone identity and mutable state are independent from the source");
		auto CloneSnapshot = RuntimeExtractor.Extract(
			*WorkspaceValue, MakeRenderCameraInput(*WorkspaceValue->GetCurrentCamera()), 320, 200
		);
		Check(std::ranges::contains(CloneSnapshot->Items, RuntimeCopy->GetObjectId(), &RenderItem::Object) &&
			WorldRootTestAccess::BodyCount(*WorkspaceValue) == 3,
			"adopted Part clone receives independent render and physics resources");

		auto CloneFolder = std::make_shared<Folder>();
		auto ClonePartA = std::make_shared<Part>();
		auto ClonePartB = std::make_shared<Part>();
		auto CloneWeld = std::make_shared<WeldConstraint>();
		for (const auto &Node : std::vector<std::shared_ptr<Instance>>{CloneFolder, ClonePartA, ClonePartB, CloneWeld})
			Node->SetArchivable(true);
		CloneFolder->SetName("CloneFolder");
		ClonePartA->SetName("A");
		ClonePartB->SetName("B");
		CloneWeld->SetName("Weld");
		ClonePartA->SetParent(CloneFolder);
		ClonePartB->SetParent(CloneFolder);
		CloneWeld->SetPart0(ClonePartA);
		CloneWeld->SetPart1(ClonePartB);
		CloneWeld->SetParent(CloneFolder);
		CloneFolder->SetParent(WorkspaceValue);
		auto FolderCopy = CloneFolder->Clone();
		Check(FolderCopy && !FolderCopy->GetParent() && FolderCopy->GetChildren().size() == 3,
			"subtree Clone preserves descendants while leaving its root detached");
		FolderCopy->SetParent(WorkspaceValue);
		auto CopiedA = std::dynamic_pointer_cast<Part>(FolderCopy->FindFirstChild("A", false));
		auto CopiedB = std::dynamic_pointer_cast<Part>(FolderCopy->FindFirstChild("B", false));
		auto CopiedWeld = std::dynamic_pointer_cast<WeldConstraint>(FolderCopy->FindFirstChild("Weld", false));
		Check(CopiedA && CopiedB && CopiedWeld && CopiedWeld->GetPart0() == CopiedA && CopiedWeld->GetPart1() == CopiedB &&
			CopiedA != ClonePartA && CopiedB != ClonePartB,
			"subtree Clone remaps internal object references to fresh descendants");
		FolderCopy->Destroy();
		Check(CloneFolder->FindFirstChild("A", false) == ClonePartA,
			"destroying an adopted clone removes only the clone subtree");

		auto ScriptSource = std::make_shared<Script>();
		ScriptSource->SetArchivable(true);
		ScriptSource->SetName("CloneScript");
		ScriptSource->SetSource("return 21\n");
		ScriptSource->SetSource("return 42\n");
		int SourceDestroyCallbacks = 0;
		auto SourceConnection = ScriptSource->Destroying->Connect([&](std::monostate) { ++SourceDestroyCallbacks; });
		auto ScriptCopy = std::dynamic_pointer_cast<Script>(ScriptSource->Clone());
		Check(ScriptCopy && ScriptCopy->GetSource() == ScriptSource->GetSource() &&
			ScriptCopy->GetSourceVersion() > 0 && ScriptCopy->GetSourceVersion() != ScriptSource->GetSourceVersion() &&
			ScriptCopy->Thread == nullptr &&
			ScriptCopy->Bytecode.empty(),
			"Script Clone preserves source while resetting source-version, execution, and bytecode state");
		ScriptCopy->Destroy();
		Check(SourceDestroyCallbacks == 0,
			"Clone creates fresh signals and does not copy source subscribers");
		SourceConnection->Disconnect();
		auto ModuleSource = std::make_shared<ModuleScript>();
		ModuleSource->SetArchivable(true);
		ModuleSource->SetSource("return { Value = 42 }\n");
		auto ModuleCopy = std::dynamic_pointer_cast<ModuleScript>(ModuleSource->Clone());
		Check(ModuleCopy && ModuleCopy->GetSource() == ModuleSource->GetSource() && ModuleCopy->Bytecode.empty(),
			"ModuleScript Clone preserves exact source without copying compiled runtime state");

		auto MeasurePart = std::make_shared<Part>();
		MeasurePart->SetArchivable(true);
		auto PartCloneStart = std::chrono::steady_clock::now();
		auto MeasuredPartCopy = MeasurePart->Clone();
		const auto PartCloneMilliseconds = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - PartCloneStart).count();
		auto MeasureTree = std::make_shared<Folder>();
		MeasureTree->SetArchivable(true);
		for (int Index = 1; Index < 100; ++Index) {
			auto Child = std::make_shared<Folder>();
			Child->SetArchivable(true);
			Child->SetParent(MeasureTree);
		}
		auto TreeCloneStart = std::chrono::steady_clock::now();
		auto MeasuredTreeCopy = MeasureTree->Clone();
		const auto TreeCloneMilliseconds = std::chrono::duration<double, std::milli>(
			std::chrono::steady_clock::now() - TreeCloneStart).count();
		Check(MeasuredPartCopy && MeasuredTreeCopy && MeasuredTreeCopy->GetDescendants().size() == 99,
			"representative one-Part and 100-object Clone measurements produce complete detached results");
		std::cout << "METRIC clone_part_ms " << PartCloneMilliseconds << '\n';
		std::cout << "METRIC clone_subtree_100_ms " << TreeCloneMilliseconds << '\n';

		lua_settop(L, 0);
		Check(Load(L, R"(
			local NonArchivable = Instance.new("Part")
			local ArchivableOk, ArchivableError = pcall(function() NonArchivable:Clone() end)
			local ProtectedOk, ProtectedError = pcall(function() game:Clone() end)
			local Stale = Instance.new("Part")
			Stale.Archivable = true
			Stale:Destroy()
			local StaleOk, StaleError = pcall(function() Stale:Clone() end)
			return ArchivableOk, ArchivableError, ProtectedOk, ProtectedError, StaleOk, StaleError
		)", "luau-clone-errors") == LUA_OK && lua_pcall(L, 0, 6, 0) == LUA_OK,
			"Clone denial paths remain ordinary bounded Luau errors");
		Check(!lua_toboolean(L, -6) && std::string_view(lua_tostring(L, -5)).find("Archivable") != std::string_view::npos &&
			!lua_toboolean(L, -4) && std::string_view(lua_tostring(L, -3)).find("protected") != std::string_view::npos &&
			!lua_toboolean(L, -2) && std::string_view(lua_tostring(L, -1)).find("destroyed") != std::string_view::npos,
			"Clone explains non-archivable, protected, and stale source failures");
		auto AtomicCloneSource = std::make_shared<Folder>();
		AtomicCloneSource->SetArchivable(true);
		auto NonArchivableDescendant = std::make_shared<Part>();
		NonArchivableDescendant->SetParent(AtomicCloneSource);
		CheckThrows<std::invalid_argument>([&] { (void)AtomicCloneSource->Clone(); },
			"Clone rejects a non-Archivable descendant before constructing a partial result");
		Check(AtomicCloneSource->GetChildren().size() == 1 &&
			AtomicCloneSource->GetChildren()[0] == NonArchivableDescendant,
			"failed subtree Clone leaves the complete source hierarchy unchanged");

		lua_settop(L, 0);
		Check(Load(L, R"(
			local WrongTypeOk, WrongTypeError = pcall(function()
				game.Workspace.KnownPart.Size = "bad"
			end)
			local PartValue = Instance.new("Part")
			PartValue.Parent = game.Workspace
			PartValue:Destroy()
			local StaleOk, StaleError = pcall(function() PartValue.Name = "AfterDestroy" end)
			local InvalidClassOk, InvalidClassError = pcall(function()
				Instance.new("DefinitelyNotAClass")
			end)
			local ProtectedClassOk, ProtectedClassError = pcall(function()
				Instance.new("Workspace")
			end)
			return WrongTypeOk, WrongTypeError, StaleOk, StaleError,
				InvalidClassOk, InvalidClassError, ProtectedClassOk, ProtectedClassError
		)", "luau-instance-errors") == LUA_OK && lua_pcall(L, 0, 8, 0) == LUA_OK,
			"Instance type, stale identity, and class-validation failures remain bounded Luau errors");
		Check(!lua_toboolean(L, -8) && std::string_view(lua_tostring(L, -7)).find("Size") != std::string_view::npos,
			"wrong property type identifies the property");
		Check(!lua_toboolean(L, -6) && std::string_view(lua_tostring(L, -5)).find("destroyed") != std::string_view::npos,
			"destroyed userdata reports stale lifetime rather than a contradictory type");
		Check(!lua_toboolean(L, -4) && std::string_view(lua_tostring(L, -3)).find("Unknown instance class") != std::string_view::npos,
			"Instance.new rejects unknown classes cleanly");
		Check(!lua_toboolean(L, -2) && std::string_view(lua_tostring(L, -1)).find("cannot be constructed") != std::string_view::npos,
			"Instance.new rejects protected service classes cleanly");

		lua_settop(L, 0);
		Check(Load(L, "local =", "luau-syntax-error") != LUA_OK, "Luau syntax errors fail during bytecode loading");
		lua_settop(L, 0);
		Check(Load(L, "error('runtime failure')", "luau-runtime-error") == LUA_OK,
			"valid runtime-error source compiles");
		Check(lua_pcall(L, 0, 0, 0) != LUA_OK, "Luau runtime errors remain protected-call failures");

		lua_settop(L, 0);
		Check(Load(L,
			"return coroutine.create(function() coroutine.yield('paused'); return 'done' end)",
			"luau-coroutine") == LUA_OK && lua_pcall(L, 0, 1, 0) == LUA_OK,
			"Luau coroutine source compiles and returns a thread");
		lua_State *Coroutine = lua_tothread(L, -1);
		Check(Coroutine && lua_resume(Coroutine, L, 0) == LUA_YIELD &&
			std::string_view(lua_tostring(Coroutine, -1)) == "paused", "Luau coroutine yields through the native API");
		lua_settop(Coroutine, 0);
		Check(lua_resume(Coroutine, L, 0) == LUA_OK && std::string_view(lua_tostring(Coroutine, -1)) == "done",
			"Luau coroutine resumes to completion");

		lua_settop(L, 0);
		lua_State *Sandboxed = lua_newthread(L);
		const int ThreadReference = lua_ref(L, -1);
		lua_pop(L, 1);
		luaL_sandboxthread(Sandboxed);
		Check(Load(Sandboxed, "return 42", "luau-sandbox-thread") == LUA_OK &&
			lua_resume(Sandboxed, L, 0) == LUA_OK && lua_tonumber(Sandboxed, -1) == 42,
			"sandboxed Luau threads execute compiled bytecode");
		Check(lua_unref(L, ThreadReference) == LUA_NOREF,
			"Luau 0.734 thread reference cleanup returns LUA_NOREF");

		lua_settop(L, 0);
		Check(Load(L, R"(
			LuauTaskResult = 0
			task.defer(function() LuauTaskResult += 1 end)
			task.delay(0, function() LuauTaskResult += 2 end)
			task.spawn(function() task.wait(0); LuauTaskResult += 4 end)
		)", "luau-task") == LUA_OK && lua_pcall(L, 0, 0, 0) == LUA_OK,
			"task scheduling source executes");
		engine.Threads.Step();
		engine.Threads.Step();
		lua_getglobal(L, "LuauTaskResult");
		Check(lua_tonumber(L, -1) == 7, "task.defer, task.delay, and task.wait resume correctly");

		lua_settop(L, 0);
		lua_pushcfunction(L, [](lua_State *State) {
			lua_pushboolean(
				State,
				GetCurrentScriptSecurityContext().HasCapability(ScriptCapability::MutateDataModel)
			);
			return 1;
		}, "HasTaskMutationAuthority");
		lua_setglobal(L, "HasTaskMutationAuthority");
		{
			ScriptSecurityScope Scope({ScriptExecutionDomain::Client, {ScriptCapability::NetworkReceive}});
			Check(Load(L, R"(
				DeferredTaskCanMutate = nil
				DelayedTaskCanMutate = nil
				WaitTaskCanMutate = nil
				task.defer(function() DeferredTaskCanMutate = HasTaskMutationAuthority() end)
				task.delay(0, function() DelayedTaskCanMutate = HasTaskMutationAuthority() end)
				task.spawn(function()
					task.wait(0)
					WaitTaskCanMutate = HasTaskMutationAuthority()
				end)
			)", "luau-task-security") == LUA_OK && lua_pcall(L, 0, 0, 0) == LUA_OK,
				"restricted task scheduling source executes");
		}
		engine.Threads.Step();
		engine.Threads.Step();
		for (const char *Name : {"DeferredTaskCanMutate", "DelayedTaskCanMutate", "WaitTaskCanMutate"}) {
			lua_getglobal(L, Name);
			Check(!lua_toboolean(L, -1), "task continuation preserves its originating restricted capability context");
			lua_pop(L, 1);
		}

		lua_settop(L, 0);
		lua_pushcfunction(
			L,
			[](lua_State *State) {
				lua_pushboolean(
					State, GetCurrentScriptSecurityContext().HasCapability(ScriptCapability::MutateDataModel)
				);
				return 1;
			},
			"HasSignalMutationAuthority"
		);
		lua_setglobal(L, "HasSignalMutationAuthority");
		{
			ScriptSecurityScope Scope({ScriptExecutionDomain::Client, {ScriptCapability::NetworkReceive}});
			Check(
				Load(
					L,
					R"(
				SecuritySignal = Signal.new()
				SignalCallbackCanMutate = nil
				SecuritySignal:Connect(function()
					SignalCallbackCanMutate = HasSignalMutationAuthority()
				end)
			)",
					"luau-signal-security-connect"
				) == LUA_OK &&
					lua_pcall(L, 0, 0, 0) == LUA_OK,
				"restricted signal callback source connects"
			);
		}
		Check(
			Load(L, "SecuritySignal:Fire()", "luau-signal-security-fire") == LUA_OK && lua_pcall(L, 0, 0, 0) == LUA_OK,
			"restricted signal callback fires from a trusted native entry"
		);
		lua_getglobal(L, "SignalCallbackCanMutate");
		Check(!lua_toboolean(L, -1), "signal callback preserves its originating restricted capability context");
		lua_pop(L, 1);

		lua_settop(L, 0);
		const auto SignalLoadStatus = Load(L, R"(
			local SignalValue = Signal.new()
			local Result = 0
			local Connection = SignalValue:Connect(function(Value) Result += Value end)
			SignalValue:Fire(2)
			Connection:Disconnect()
			SignalValue:Fire(4)
			SignalValue:Once(function(Value) Result += Value end)
			SignalValue:Fire(8)
			SignalValue:Fire(16)
			SignalResult = Result
			Connection = nil
		)", "luau-signal");
		const auto SignalExecutionStatus = SignalLoadStatus == LUA_OK ? lua_pcall(L, 0, 0, 0) : SignalLoadStatus;
		if (SignalExecutionStatus != LUA_OK)
			std::cerr << "Luau Signal regression diagnostic: " <<
				(lua_tostring(L, -1) ? lua_tostring(L, -1) : "unknown Luau error") << '\n';
		Check(SignalExecutionStatus == LUA_OK,
			"Signal callbacks and userdata garbage collection execute without native failure");
		lua_gc(L, LUA_GCCOLLECT, 0);
		lua_getglobal(L, "SignalResult");
		Check(lua_tonumber(L, -1) == 10, "Signal Connect, Once, Fire, and Disconnect preserve callback semantics");
		lua_settop(L, 0);
	}

	void TestPlayDiagnosticBounds() {
		using namespace gargantuan;
		auto World = std::make_shared<DataModel>();
		World->MarkPersistenceSubtreeArchivable();
		auto SpamScript = std::make_shared<Script>();
		SpamScript->SetArchivable(true);
		SpamScript->SetName("DiagnosticSpam");
		SpamScript->SetSource(R"(
			for Index = 1, 300 do print("spam", Index) end
			warn("warning after spam")
			error("error after spam")
		)");
		auto Workspace = World->GetService("Workspace");
		Workspace->SetArchivable(true);
		SpamScript->SetParent(Workspace);
		std::shared_ptr<Instance> Root = World;
		const auto BeforePlay = InstanceSerialization::Serialize(InstanceSerialization::InstanceFormat::Json, Root);
		PlaySession Session(
			{7001}, BeforePlay, InstanceSerialization::InstanceFormat::Json,
			std::filesystem::temp_directory_path(), 64, 64, 1
		);
		auto RuntimeSpam = std::dynamic_pointer_cast<Script>(Session.GetWorld()->FindFirstChild("DiagnosticSpam", true));
		Check(RuntimeSpam && RuntimeSpam->GetEnabled() && RuntimeSpam->GetSource().find("warning after spam") != std::string::npos,
			"Play diagnostic fixture retains its enabled Script source");
		for (int Step = 0; Step < 4; ++Step) Session.Step();
		auto Diagnostics = Session.DrainDiagnostics();
		Check(Diagnostics.size() == PlaySession::MaximumDiagnostics &&
			std::ranges::is_sorted(Diagnostics, {}, &PlayDiagnostic::Sequence) &&
			Diagnostics.front().Sequence > 1,
			"runtime diagnostic spam evicts oldest records from the bounded Play queue without blocking");
		Check(std::ranges::any_of(Diagnostics, [](const PlayDiagnostic &Diagnostic) {
			return Diagnostic.Severity == "Warning" && Diagnostic.Message == "warning after spam";
		}) && std::ranges::any_of(Diagnostics, [](const PlayDiagnostic &Diagnostic) {
			return Diagnostic.Severity == "Error" && Diagnostic.Message.find("error after spam") != std::string::npos;
		}), "warning and runtime error diagnostics remain classified after bounded log pressure");
		Session.Stop();
		auto StopDiagnostics = Session.DrainDiagnostics();
		Check(StopDiagnostics.size() == 1 && StopDiagnostics[0].Category == "Runtime" &&
			StopDiagnostics[0].Message == "Play session stopped",
			"stopped Play session retains only its own final queued lifecycle diagnostic after a drain");
		const auto AfterPlay = InstanceSerialization::Serialize(InstanceSerialization::InstanceFormat::Json, Root);
		Check(AfterPlay == BeforePlay,
			"runtime diagnostics do not mutate authoring hierarchy or persistent state");
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
	TestServiceProviderSemantics();
	TestObjectIdsAndChanges();
	TestWorldRootConstraintValidation();
	TestCheckedResolutionAndOwnedPaths();
	TestJobSystem();
	TestSchemaMetadata();
	TestRuntimeSchemaRegistry();
	TestRuntimeSchemaLifecycle();
	TestCustomEnumPreRun();
	TestClassExtensionSchema();
	TestCustomClassSchema();
	TestRenderSnapshotExtraction();
	TestRenderBackendBoundary();
	TestScriptSecurityModel();
	TestMutationGateway();
	TestInstanceAttributes();
	TestInstanceTags();
	TestBoundedJournalCursor();
	TestSnapshotBaseline();
	TestWireJournalAndLoopbackReplication();
	TestProtocolInputHardening();
	TestSharedFrameRing();
	TestClassExtensionRuntime();
	TestCustomClassRuntime();
	TestSerializationGoldenFixtures();
	TestProjectCreationJsonNames();
	TestAuthoritativeTransactions();
	TestProjectRevisionPersistence();
	TestEditorHostProtocol();
	TestLuauExceptionBoundary();
	TestLuauEmbeddingCompatibility();
	TestPlayDiagnosticBounds();
	if (Failures == 0) std::cout << "All foundation tests passed\n";
	return Failures == 0 ? 0 : 1;
}
