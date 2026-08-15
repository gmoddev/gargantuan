#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/reflection/InstanceClassRegistry.hpp"
#include "gargantuan/reflection/PreRunRegistration.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"

#include <chrono>
#include <any>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>

int main() {
	using namespace gargantuan;
	const auto root = std::filesystem::temp_directory_path() /
		("gargantuan-prerun-bootstrap-" + std::to_string(
			std::chrono::steady_clock::now().time_since_epoch().count()
		));
	struct Cleanup {
		std::filesystem::path Root;
		~Cleanup() { std::filesystem::remove_all(Root); }
	} cleanup{root};
	std::filesystem::create_directories(root / ".gargantuan");
	std::ofstream source(root / ".gargantuan" / "prerun.luau", std::ios::binary);
	source << R"(Schema:RegisterEnum({
		Namespace = "Game",
		Name = "BootstrapState",
		Version = 1,
		Items = { Ready = 1 },
	})
	Schema:RegisterExtension({
		Namespace = "Game.Bootstrap",
		Name = "PartState",
		Version = 1,
		Target = "Engine.BasePart",
		Properties = { Damage = { Type = "Integer", Default = 0 } },
	})
	Schema:RegisterClass({
		Namespace = "Game",
		Name = "AChild",
		Version = 1,
		Base = "Game.ZParent",
		Properties = { Aggressive = { Type = "Boolean", Default = true } },
	})
	Schema:RegisterClass({
		Namespace = "Game",
		Name = "ZParent",
		Version = 1,
		Base = "Engine.Folder",
		Properties = {
			Health = { Type = "Integer", Default = 100 },
			Nameplate = { Type = "String", Default = "" },
		},
	})
	Schema:RegisterClass({
		Namespace = "Game",
		Name = "Folder",
		Version = 1,
		Base = "Engine.Folder",
		Properties = { Marker = { Type = "Integer", Default = 0 } },
	})
	Schema:RegisterClass({
		Namespace = "Game",
		Name = "DataModel",
		Version = 1,
		Base = "Engine.Folder",
		Properties = { Marker = { Type = "Integer", Default = 0 } },
	}))";
	source.close();

	try {
		BootstrapProjectRuntimeSchema(root);
		const auto &lifecycle = GetRuntimeSchemaLifecycle();
		if (lifecycle.GetActiveGeneration() != 1 ||
			lifecycle.GetActiveRegistry()->FindEnumByName("Game.BootstrapState") == nullptr ||
			lifecycle.GetActiveRegistry()->FindExtensionByName("Game.Bootstrap.PartState") == nullptr ||
			lifecycle.GetActiveRegistry()->FindClassByName("Game.ZParent") == nullptr ||
			lifecycle.GetActiveRegistry()->FindClassByName("Game.AChild") == nullptr) {
			std::cerr << "Project startup did not publish one complete generation\n";
			return 1;
		}
		auto world = std::make_shared<DataModel>();
		if (!world) return 1;
		auto custom = InstanceClassRegistry::ConstructByName("Game.AChild");
		if (!custom || custom->GetClassName() != "Game.AChild" || !custom->IsA("Game.AChild") ||
			!custom->IsA("Game.ZParent") || !custom->IsA("Engine.Folder") ||
			!custom->IsA("Engine.Instance") || custom->IsA("Engine.Part")) {
			std::cerr << "Custom class construction or schema inheritance is invalid\n";
			return 1;
		}
		auto *health = custom->FindProperty("Health");
		auto *aggressive = custom->FindProperty("Aggressive");
		if (!health || !aggressive || std::any_cast<int>(health->Read(custom.get())) != 100 ||
			!std::any_cast<bool>(aggressive->Read(custom.get()))) {
			std::cerr << "Custom class inherited defaults are invalid\n";
			return 1;
		}
		if (custom->ApplyPropertyMutation("Health", 75, Enums::Permission::None,
			ScriptSecurityContext::CoreTrusted()) != MutationStatus::Success ||
			std::any_cast<int>(health->Read(custom.get())) != 75) {
			std::cerr << "Custom class property mutation was rejected\n";
			return 1;
		}
		auto collidingName = InstanceClassRegistry::ConstructByName("Game.Folder");
		if (!collidingName) {
			std::cerr << "Namespaced short-name collision class was not constructible\n";
			return 1;
		}
		std::shared_ptr<Instance> serializableCollision = collidingName;
		const auto serializedCollision = InstanceSerialization::Serialize(
			InstanceSerialization::InstanceFormat::Json, serializableCollision
		);
		if (serializedCollision.find(
			SchemaId::FromCustomClassName("Game", "Folder").ToString()
		) == std::string::npos) {
			std::cerr << "Namespaced short-name collision did not serialize through stable base identity\n";
			return 1;
		}
		auto spoofedDataModel = InstanceClassRegistry::ConstructByName("Game.DataModel");
		if (!spoofedDataModel || spoofedDataModel->GetDataModel() ||
			spoofedDataModel->GetReplicationScopeId().IsValid()) {
			std::cerr << "Project class short name granted native DataModel lifecycle semantics\n";
			return 1;
		}

		std::ofstream ExactSource(root / ".gargantuan" / "prerun.luau", std::ios::binary | std::ios::trunc);
		ExactSource << std::string(MaximumPreRunSourceBytes, ' ');
		ExactSource.close();
		auto ExactRead = ReadProjectPreRunSource(root);
		if (!ExactRead || ExactRead->size() != MaximumPreRunSourceBytes) {
			std::cerr << "PreRun source at the exact byte limit was rejected\n";
			return 1;
		}
		std::ofstream OversizedSource(root / ".gargantuan" / "prerun.luau", std::ios::binary | std::ios::trunc);
		OversizedSource << std::string(MaximumPreRunSourceBytes + 1, ' ');
		OversizedSource.close();
		try {
			static_cast<void>(ReadProjectPreRunSource(root));
			std::cerr << "PreRun source over the byte limit was accepted\n";
			return 1;
		} catch (const PreRunRegistrationError &Error) {
			if (Error.GetDiagnostic().Code != PreRunDiagnosticCode::SourceTooLarge) return 1;
		}

		const auto ExternalSource = root.parent_path() / (root.filename().string() + "-external.luau");
		struct ExternalCleanup {
			std::filesystem::path Path;
			~ExternalCleanup() { std::filesystem::remove(Path); }
		} ExternalCleanupGuard{ExternalSource};
		std::ofstream External(ExternalSource, std::ios::binary);
		External << "error('outside project')";
		External.close();
		std::filesystem::remove(root / ".gargantuan" / "prerun.luau");
		std::error_code SymlinkError;
		std::filesystem::create_symlink(ExternalSource, root / ".gargantuan" / "prerun.luau", SymlinkError);
		if (!SymlinkError) {
			try {
				static_cast<void>(ReadProjectPreRunSource(root));
				std::cerr << "PreRun source symlink escaped the project root\n";
				return 1;
			} catch (const PreRunRegistrationError &) {
			}
		} else {
			std::cerr << "PreRun symlink confinement test skipped: " << SymlinkError.message() << '\n';
		}
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
	std::cout << "PreRun bootstrap ordering passed\n";
	return 0;
}
