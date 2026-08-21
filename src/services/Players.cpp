#include "gargantuan/services/Players.hpp"

#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/ModuleScript.hpp"
#include "gargantuan/classes/Player.hpp"
#include "gargantuan/classes/Script.hpp"
#include "gargantuan/filesystem/Paths.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace gargantuan {
	namespace {
		std::string ReadRuntimeModule(std::string_view Name) {
			const auto ExecutableDirectory = Paths::GetExecutableDirectory();
			const std::array Candidates = {
				ExecutableDirectory / "runtime" / Name,
				ExecutableDirectory.parent_path() / "runtime" / Name,
			};
			for (const auto &Candidate : Candidates) {
				std::ifstream Input(Candidate, std::ios::binary);
				if (!Input.is_open()) continue;
				Input.seekg(0, std::ios::end);
				const auto Size = Input.tellg();
				if (Size < 0 || static_cast<std::size_t>(Size) > MaximumScriptSourceBytes)
					throw std::runtime_error("[Player:Runtime] Shipped Luau module exceeds its source bound");
				Input.seekg(0, std::ios::beg);
				std::string Source(static_cast<std::size_t>(Size), '\0');
				Input.read(Source.data(), static_cast<std::streamsize>(Size));
				if (!Input) throw std::runtime_error("[Player:Runtime] Failed to read shipped Luau module");
				return Source;
			}
			throw std::runtime_error("[Player:Runtime] Missing shipped Luau module: " + std::string(Name));
		}
	}

	Players::~Players() {
		ShutdownRuntime();
	}

	std::optional<std::shared_ptr<Player>> Players::GetLocalPlayer() const {
		return LocalPlayerValue && !LocalPlayerValue->GetDestroyed() && !LocalPlayerValue->IsDestroying()
				   ? std::optional(LocalPlayerValue)
				   : std::nullopt;
	}

	std::vector<std::shared_ptr<Player>> Players::GetPlayers() {
		if (!LocalPlayerValue || LocalPlayerValue->GetDestroyed() || LocalPlayerValue->IsDestroying()) return {};
		return {LocalPlayerValue};
	}

	void Players::InitializeLocalPlayer() {
		if (LocalPlayerValue && !LocalPlayerValue->GetDestroyed() && !LocalPlayerValue->IsDestroying()) return;
		auto Value = std::make_shared<Player>();
		Value->InitializeIdentity(1);
		Value->SetName("Player1");
		Value->SetParent(shared_from_this());
		LocalPlayerValue = Value;
		NotifyPropertyCommitted("LocalPlayer");
		PlayerAdded->Fire(Value);
	}

	void Players::StartDefaultRuntime() {
		if (RuntimeStarted) return;
		if (!GetDefaultControllerEnabled() && !GetDefaultCameraEnabled()) return;
		constexpr std::array Names = {"DefaultActionMap", "DefaultPlayerController", "DefaultCamera"};
		std::array<std::string, Names.size()> Sources;
		for (std::size_t Index = 0; Index < Names.size(); ++Index)
			Sources[Index] = ReadRuntimeModule(std::string(Names[Index]) + ".luau");
		const auto BootstrapSource = ReadRuntimeModule("DefaultPlayerRuntime.luau");

		auto Container = std::make_shared<Folder>();
		Container->SetName("PlayerRuntimeModules");
		Container->SetArchivable(false);
		Container->SetParent(shared_from_this());
		RuntimeModules = Container;

		for (std::size_t Index = 0; Index < Names.size(); ++Index) {
			auto Module = std::make_shared<ModuleScript>();
			Module->SetName(Names[Index]);
			Module->SetArchivable(false);
			Module->SetSource(std::move(Sources[Index]));
			Module->SetParent(Container);
		}

		auto Bootstrap = std::make_shared<Script>();
		Bootstrap->SetName("DefaultPlayerRuntime");
		Bootstrap->SetArchivable(false);
		Bootstrap->SetRunContext(Enums::RunContext::Client);
		Bootstrap->SetSource(BootstrapSource);
		Bootstrap->SetParent(Container);
		RuntimeStarted = true;
	}

	void Players::ShutdownRuntime() {
		if (!RuntimeStarted && !LocalPlayerValue && !RuntimeModules) return;
		RuntimeStarted = false;
		if (RuntimeModules && !RuntimeModules->GetDestroyed() && !RuntimeModules->IsDestroying())
			RuntimeModules->Destroy();
		RuntimeModules.reset();
		auto Value = std::move(LocalPlayerValue);
		if (Value) {
			PlayerRemoving->Fire(Value);
			Value->ShutdownCharacter();
			if (!Value->GetDestroyed() && !Value->IsDestroying()) Value->Destroy();
			NotifyPropertyCommitted("LocalPlayer");
		}
	}
}
