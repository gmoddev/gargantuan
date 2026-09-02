#include "gargantuan/services/Players.hpp"

#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/ModuleScript.hpp"
#include "gargantuan/classes/Player.hpp"
#include "gargantuan/classes/Script.hpp"
#include "gargantuan/filesystem/Paths.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace gargantuan {
	namespace {
		std::string ReadRuntimeModule(std::string_view Name) {
			const auto ExecutableDirectory = Paths::GetExecutableDirectory();
			const auto Candidate = ExecutableDirectory / "runtime" / Name;
			{
				std::ifstream Input(Candidate, std::ios::binary);
				if (!Input.is_open())
					throw std::runtime_error("[Player:Runtime] Missing shipped Luau module: " + std::string(Name));
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
		std::vector<std::shared_ptr<Player>> Result;
		for (const auto &Child : GetChildren())
			if (auto Value = std::dynamic_pointer_cast<Player>(Child);
				Value && !Value->GetDestroyed() && !Value->IsDestroying())
				Result.push_back(std::move(Value));
		std::ranges::sort(Result, [](const auto &Left, const auto &Right) {
			if (Left->GetPlayerId() != Right->GetPlayerId()) return Left->GetPlayerId() < Right->GetPlayerId();
			return Left->GetObjectId() < Right->GetObjectId();
		});
		return Result;
	}

	void Players::InitializeLocalPlayer() {
		if (LocalPlayerValue && !LocalPlayerValue->GetDestroyed() && !LocalPlayerValue->IsDestroying()) return;
		auto Value = std::make_shared<Player>();
		Value->InitializeIdentity(1);
		Value->InitializeAuthenticationIdentity({"local", "player-1"});
		Value->SetName("Player1");
		Value->SetParent(shared_from_this());
		LocalPlayerValue = Value;
		NotifyPropertyCommitted("LocalPlayer");
		PlayerAdded->Fire(Value);
	}

	std::shared_ptr<Player> Players::CreateSessionPlayer(PlayerIdentity Identity) {
		ValidatePlayerIdentity(Identity);
		if (GetPlayers().size() >= 512 || NextSessionPlayerId <= 0)
			throw std::length_error("[Player:Session] Player capacity is exhausted");
		auto Value = std::make_shared<Player>();
		Value->InitializeIdentity(NextSessionPlayerId++);
		Value->InitializeAuthenticationIdentity(std::move(Identity));
		Value->SetName("Player" + std::to_string(Value->GetPlayerId()));
		Value->SetParent(shared_from_this());
		PlayerAdded->Fire(Value);
		return Value;
	}

	bool Players::RemoveSessionPlayer(const std::shared_ptr<Player> &Value) {
		if (!Value || Value->GetDestroyed() || Value->IsDestroying() ||
			Value->GetParent().value_or(nullptr).get() != this)
			return false;
		if (LocalPlayerValue == Value) ClearTrustedLocalPlayer();
		PlayerRemoving->Fire(Value);
		Value->ShutdownCharacter();
		if (!Value->GetDestroyed() && !Value->IsDestroying()) Value->Destroy();
		return true;
	}

	bool Players::SetTrustedLocalPlayer(const std::shared_ptr<Player> &Value) {
		if (!Value || Value->GetDestroyed() || Value->IsDestroying() ||
			Value->GetParent().value_or(nullptr).get() != this)
			return false;
		if (LocalPlayerValue == Value) return true;
		if (LocalPlayerValue) return false;
		LocalPlayerValue = Value;
		NotifyPropertyCommitted("LocalPlayer");
		PlayerAdded->Fire(Value);
		return true;
	}

	void Players::ClearTrustedLocalPlayer() {
		if (!LocalPlayerValue) return;
		LocalPlayerValue.reset();
		NotifyPropertyCommitted("LocalPlayer");
	}

	bool Players::IsRuntimeModule(ObjectId Object) const {
		if (!RuntimeModules || !Object.IsValid()) return false;
		if (RuntimeModules->GetObjectId() == Object) return true;
		for (const auto &Descendant : RuntimeModules->GetDescendants())
			if (Descendant->GetObjectId() == Object) return true;
		return false;
	}

	std::shared_ptr<Script> Players::StartDefaultRuntime(RuntimeMode Mode) {
		if (RuntimeStarted) return nullptr;
		if (!GetDefaultControllerEnabled() && !GetDefaultCameraEnabled()) return nullptr;
		std::vector<std::string> Names;
		std::string BootstrapName;
		Enums::RunContext RunContext = Enums::RunContext::Client;
		if (Mode == RuntimeMode::NetworkServer) {
			if (!GetDefaultControllerEnabled()) return nullptr;
			Names = {"DefaultCharacterAssembly", "DefaultNetworkLocomotion"};
			BootstrapName = "DefaultNetworkServerRuntime";
			RunContext = Enums::RunContext::Server;
		} else if (Mode == RuntimeMode::NetworkClient) {
			Names = {"DefaultActionMap", "DefaultNetworkLocomotion", "DefaultNetworkCharacterRuntime", "DefaultCamera"};
			BootstrapName = "DefaultNetworkClientRuntime";
		} else {
			Names = {"DefaultActionMap", "DefaultCharacterRuntime", "DefaultLocomotion", "DefaultCamera"};
			BootstrapName = "DefaultPlayerRuntime";
		}
		std::vector<std::string> Sources;
		Sources.reserve(Names.size());
		for (const auto &Name : Names)
			Sources.push_back(ReadRuntimeModule(Name + ".luau"));
		const auto BootstrapSource = ReadRuntimeModule(BootstrapName + ".luau");

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
		Bootstrap->SetName(BootstrapName);
		Bootstrap->SetArchivable(false);
		Bootstrap->SetRunContext(RunContext);
		Bootstrap->SetSource(BootstrapSource);
		Bootstrap->SetParent(Container);
		RuntimeStarted = true;
		return Bootstrap;
	}

	void Players::ShutdownRuntime() {
		if (!RuntimeStarted && GetPlayers().empty() && !RuntimeModules) return;
		RuntimeStarted = false;
		if (RuntimeModules && !RuntimeModules->GetDestroyed() && !RuntimeModules->IsDestroying())
			RuntimeModules->Destroy();
		RuntimeModules.reset();
		const auto Values = GetPlayers();
		for (const auto &Value : Values)
			(void)RemoveSessionPlayer(Value);
		ClearTrustedLocalPlayer();
	}
}
