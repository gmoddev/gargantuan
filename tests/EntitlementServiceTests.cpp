#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/Player.hpp"
#include "gargantuan/entitlements/EntitlementProvider.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"
#include "gargantuan/services/EntitlementService.hpp"

#include <Luau/Compiler.h>
#include <lua.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
	using namespace std::chrono_literals;
	int Failures = 0;

	void Check(bool Condition, const char *Message) {
		if (Condition) return;
		std::cerr << "FAIL: " << Message << '\n';
		++Failures;
	}

	template <typename Exception, typename Function> void CheckThrows(Function &&Callback, const char *Message) {
		try {
			Callback();
		} catch (const Exception &) {
			return;
		} catch (...) {}
		Check(false, Message);
	}

	class VectorProvider : public gargantuan::IEntitlementProvider {
	  public:
		[[nodiscard]] std::string_view Name() const override {
			return "vector";
		}

		[[nodiscard]] gargantuan::EntitlementProviderResult Check(
			const gargantuan::EntitlementRequestContext &Context,
			const gargantuan::PlayerIdentity &Identity,
			const gargantuan::EntitlementId &Entitlement
		) override {
			using namespace gargantuan;
			if (Context.IsCancelled())
				return std::unexpected(EntitlementProviderError{EntitlementProviderErrorCode::Cancelled});
			if (Context.IsExpired())
				return std::unexpected(EntitlementProviderError{EntitlementProviderErrorCode::DeadlineExceeded});
			if (Identity.Provider != "steam")
				return EntitlementDecision{EntitlementStatus::Unavailable, Entitlement, Identity, std::nullopt};
			if (Entitlement.Value() == "game.base")
				return EntitlementDecision{EntitlementStatus::Granted, Entitlement, Identity, std::nullopt};
			if (Entitlement.Value() == "dlc.missing_pack")
				return EntitlementDecision{EntitlementStatus::Denied, Entitlement, Identity, std::nullopt};
			if (Entitlement.Value() == "feature.trial")
				return EntitlementDecision{
					EntitlementStatus::Granted,
					Entitlement,
					Identity,
					std::chrono::system_clock::time_point(4'102'444'800s),
				};
			return EntitlementDecision{EntitlementStatus::Unavailable, Entitlement, Identity, std::nullopt};
		}
	};

	class InvalidProvider final : public gargantuan::IEntitlementProvider {
	  public:
		[[nodiscard]] std::string_view Name() const override {
			return "invalid-response";
		}
		[[nodiscard]] gargantuan::EntitlementProviderResult Check(
			const gargantuan::EntitlementRequestContext &,
			const gargantuan::PlayerIdentity &Identity,
			const gargantuan::EntitlementId &
		) override {
			return gargantuan::EntitlementDecision{
				gargantuan::EntitlementStatus::Granted,
				*gargantuan::EntitlementId::Parse("game.wrong"),
				Identity,
				std::nullopt,
			};
		}
	};

	class InvalidNameProvider final : public VectorProvider {
	  public:
		[[nodiscard]] std::string_view Name() const override {
			return "Invalid Provider";
		}
	};

	std::shared_ptr<gargantuan::Player> MakePlayer(
		const std::shared_ptr<gargantuan::DataModel> &World,
		gargantuan::PlayerIdentity Identity,
		std::string Name = "EntitlementPlayer"
	) {
		auto PlayerValue = std::make_shared<gargantuan::Player>();
		PlayerValue->InitializeAuthenticationIdentity(std::move(Identity));
		PlayerValue->SetName(std::move(Name));
		PlayerValue->SetParent(World);
		return PlayerValue;
	}

	bool ExecuteLuau(const std::shared_ptr<gargantuan::DataModel> &World, std::string_view Source) {
		gargantuan::ScriptEngine Engine(World);
		size_t BytecodeSize = 0;
		char *Bytecode = luau_compile(Source.data(), Source.size(), &Engine.CompileOptions, &BytecodeSize);
		if (!Bytecode) return false;
		const auto LoadStatus = luau_load(Engine.L, "entitlement-service-test", Bytecode, BytecodeSize, 0);
		std::free(Bytecode);
		const auto Status = LoadStatus == LUA_OK ? lua_pcall(Engine.L, 0, 0, 0) : LoadStatus;
		if (Status != LUA_OK)
			std::cerr << "LUAU ERROR: " << (lua_tostring(Engine.L, -1) ? lua_tostring(Engine.L, -1) : "unknown")
					  << '\n';
		return Status == LUA_OK;
	}

	void TestSemanticValidationAndVectors() {
		using namespace gargantuan;
		const auto VectorPath = std::filesystem::path(GARGANTUAN_SOURCE_DIR) / "tests/conformance/entitlements-v1.json";
		std::ifstream Input(VectorPath);
		nlohmann::json Vectors;
		Input >> Vectors;
		Check(Vectors["version"] == 1, "conformance vector version is stable");
		Check(
			Vectors["limits"]["maximum_identity_provider_bytes"] == PlayerIdentity::MaximumProviderBytes &&
				Vectors["limits"]["maximum_identity_subject_bytes"] == PlayerIdentity::MaximumSubjectBytes &&
				Vectors["limits"]["maximum_entitlement_id_bytes"] == EntitlementId::MaximumBytes &&
				Vectors["limits"]["maximum_batch_count"] == EntitlementService::MaximumBatchSize,
			"conformance vector limits match the engine contract"
		);
		for (const auto &Malformed : Vectors["malformed_entitlement_ids"])
			Check(!EntitlementId::Parse(Malformed.get<std::string>()), "malformed entitlement vector is rejected");
		Check(EntitlementId::Parse("game.base").has_value(), "canonical entitlement ID is accepted");
		Check(
			!EntitlementId::Parse(std::string(EntitlementId::MaximumBytes + 1, 'a')),
			"oversized entitlement ID is rejected"
		);
		CheckThrows<std::invalid_argument>(
			[] { ValidatePlayerIdentity({"Steam", "subject"}); }, "noncanonical identity provider is rejected"
		);
		CheckThrows<std::invalid_argument>(
			[] { ValidatePlayerIdentity({"steam", std::string(PlayerIdentity::MaximumSubjectBytes + 1, 'x')}); },
			"oversized identity subject is rejected"
		);
	}

	void TestProviderConformanceAndService() {
		using namespace gargantuan;
		auto World = std::make_shared<DataModel>();
		auto Service = std::dynamic_pointer_cast<EntitlementService>(World->GetService("EntitlementService"));
		auto PlayerValue = MakePlayer(World, {"steam", "76561198000000001"});
		const auto GameBase = *EntitlementId::Parse("game.base");
		Check(
			Service && World->GetService("EntitlementService") == Service,
			"EntitlementService is canonical and DataModel-scoped"
		);
		Check(Service->GetProviderName() == "none", "offline service starts with an explicit None provider");
		Check(
			Service->Check(PlayerValue, GameBase).Status == EntitlementStatus::Unavailable,
			"no-backend mode reports Unavailable"
		);

		const auto InitialGeneration = Service->GetProviderGeneration();
		CheckThrows<std::invalid_argument>(
			[&] { Service->ConfigureProvider(std::make_shared<InvalidNameProvider>()); },
			"trusted provider names must be canonical"
		);
		Service->ConfigureProvider(std::make_shared<VectorProvider>());
		Check(
			Service->GetProviderGeneration() == InitialGeneration + 1 && Service->GetProviderName() == "vector",
			"trusted provider replacement advances the provider generation"
		);
		Check(
			Service->Check(PlayerValue, GameBase).Status == EntitlementStatus::Granted, "provider grant is preserved"
		);
		Check(
			Service->Check(PlayerValue, *EntitlementId::Parse("dlc.missing_pack")).Status == EntitlementStatus::Denied,
			"authoritative provider denial is distinct from outage"
		);
		auto Trial = Service->Check(PlayerValue, *EntitlementId::Parse("feature.trial"));
		Check(
			Trial.Status == EntitlementStatus::Granted && Trial.ExpiresAt.has_value(),
			"expiring grants preserve neutral expiry"
		);

		const std::vector Batch{
			GameBase,
			*EntitlementId::Parse("dlc.missing_pack"),
			*EntitlementId::Parse("feature.provider_unavailable"),
		};
		const auto Decisions = Service->CheckMany(PlayerValue, Batch);
		Check(
			Decisions.size() == 3 && Decisions[0].Status == EntitlementStatus::Granted &&
				Decisions[1].Status == EntitlementStatus::Denied &&
				Decisions[2].Status == EntitlementStatus::Unavailable,
			"bounded batch retains individual decision semantics"
		);
		std::vector<EntitlementId> Oversized(EntitlementService::MaximumBatchSize + 1, GameBase);
		CheckThrows<std::length_error>(
			[&] { (void)Service->CheckMany(PlayerValue, Oversized); }, "oversized batch is rejected"
		);

		EntitlementCancellationToken Cancellation;
		Cancellation.Cancel();
		Check(
			Service->Check(PlayerValue, GameBase, Cancellation).Status == EntitlementStatus::Unavailable,
			"cancellation cannot become a denial or grant"
		);
		Service->ConfigureProvider(std::make_shared<InvalidProvider>());
		Check(
			Service->Check(PlayerValue, GameBase).Status == EntitlementStatus::Unavailable,
			"malformed provider output fails closed as Unavailable"
		);

		Service->ConfigureProvider(nullptr);
		Check(
			Service->GetProviderName() == "none" &&
				Service->Check(PlayerValue, GameBase).Status == EntitlementStatus::Unavailable,
			"resetting provider restores offline behavior"
		);
	}

	void TestLocalProviderIdentityAndLuauAuthority() {
		using namespace gargantuan;
		auto World = std::make_shared<DataModel>();
		auto Service = std::dynamic_pointer_cast<EntitlementService>(World->GetService("EntitlementService"));
		auto PlayerValue = MakePlayer(World, {"custom-development", "player.one"});
		const auto GameBase = *EntitlementId::Parse("game.base");
		Service->ConfigureProvider(
			std::make_shared<LocalEntitlementProvider>(std::vector<LocalEntitlementGrant>{
				{{"custom-development", "player.one"}, GameBase, std::nullopt},
				{{"custom-development", "player.one"},
				 *EntitlementId::Parse("feature.expired"),
				 std::chrono::system_clock::now() - 1s},
			})
		);
		Check(
			Service->Check(PlayerValue, GameBase).Status == EntitlementStatus::Granted,
			"configured local grant supports offline development"
		);
		Check(
			Service->Check(PlayerValue, *EntitlementId::Parse("dlc.unconfigured")).Status == EntitlementStatus::Denied,
			"configured local provider authoritatively denies absent grants"
		);
		auto Expired = Service->Check(PlayerValue, *EntitlementId::Parse("feature.expired"));
		Check(
			Expired.Status == EntitlementStatus::Denied && Expired.ExpiresAt.has_value(),
			"configured local provider preserves expiry while denying expired grants"
		);
		CheckThrows<std::logic_error>(
			[&] { PlayerValue->InitializeAuthenticationIdentity({"steam", "76561198000000001"}); },
			"trusted Player identity is immutable once initialized"
		);
		auto ForeignWorld = std::make_shared<DataModel>();
		auto ForeignPlayer = MakePlayer(ForeignWorld, {"custom-development", "player.one"});
		CheckThrows<std::invalid_argument>(
			[&] { (void)Service->Check(ForeignPlayer, GameBase); }, "cross-DataModel Player identity is rejected"
		);

		Check(
			ExecuteLuau(World, R"(
				local Entitlements = game:GetService("EntitlementService")
				local Player = game:FindFirstChild("EntitlementPlayer")
				assert(Entitlements.ConfigureProvider == nil)
				local Granted = Entitlements:CheckAsync(Player, "game.base")
				assert(Granted.Status == "Granted")
				assert(Granted.EntitlementId == "game.base")
				assert(Granted.Identity.Provider == "custom-development")
				assert(Granted.Identity.Subject == "player.one")
				Granted.Identity.Subject = "spoofed-copy"
				assert(Entitlements:CheckAsync(Player, "game.base").Identity.Subject == "player.one")
				local Many = Entitlements:CheckManyAsync(Player, { "game.base", "dlc.unconfigured" })
				assert(#Many == 2 and Many[1].Status == "Granted" and Many[2].Status == "Denied")
				assert(not pcall(function() Entitlements:CheckAsync(Player, "Steam.480") end))
				assert(not pcall(function() Entitlements:CheckAsync(game, "game.base") end))
			)"),
			"Luau can ask but cannot replace providers, supply identity, or bypass semantic validation"
		);
	}

	void TestHeadlessRepeatedDataModelLifecycle() {
		using namespace gargantuan;
		const auto GameBase = *EntitlementId::Parse("game.base");
		for (int Iteration = 0; Iteration < 8; ++Iteration) {
			auto World = std::make_shared<DataModel>();
			auto Service = std::dynamic_pointer_cast<EntitlementService>(World->GetService("EntitlementService"));
			auto PlayerValue = MakePlayer(World, {"local", "headless-player"}, "HeadlessPlayer");
			Check(
				Service->Check(PlayerValue, GameBase).Status == EntitlementStatus::Unavailable,
				"repeated headless lifecycle retains offline semantics"
			);
		}
		auto World = std::make_shared<DataModel>();
		auto Service = std::dynamic_pointer_cast<EntitlementService>(World->GetService("EntitlementService"));
		auto IdentitylessPlayer = std::make_shared<Player>();
		IdentitylessPlayer->SetName("IdentitylessPlayer");
		IdentitylessPlayer->SetParent(World);
		Check(
			Service->Check(IdentitylessPlayer, GameBase).Status == EntitlementStatus::Unavailable,
			"missing authenticated identity cannot become a grant or denial"
		);
	}

	void TestPublicHeadersHaveNoNodeTransportDependency() {
		const auto Root = std::filesystem::path(GARGANTUAN_SOURCE_DIR) / "include/gargantuan";
		for (const auto &Path : {
				 Root / "identity/PlayerIdentity.hpp",
				 Root / "entitlements/EntitlementProvider.hpp",
				 Root / "services/EntitlementService.hpp",
				 Root / "classes/Player.hpp",
			 }) {
			std::ifstream Input(Path, std::ios::binary);
			const std::string Contents((std::istreambuf_iterator<char>(Input)), std::istreambuf_iterator<char>());
			Check(
				Contents.find("protobuf") == std::string::npos,
				"public entitlement headers contain no protobuf dependency"
			);
			Check(
				Contents.find("gargantuan-node") == std::string::npos,
				"public entitlement headers contain no Node dependency"
			);
			Check(Contents.find("grpc") == std::string::npos, "public entitlement headers contain no gRPC dependency");
		}
	}
}

int main() {
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
	} catch (const std::exception &Exception) {
		std::cerr << "Runtime schema bootstrap failed: " << Exception.what() << '\n';
		return 1;
	}
	TestSemanticValidationAndVectors();
	TestProviderConformanceAndService();
	TestLocalProviderIdentityAndLuauAuthority();
	TestHeadlessRepeatedDataModelLifecycle();
	TestPublicHeadersHaveNoNodeTransportDependency();
	if (Failures != 0) {
		std::cerr << Failures << " entitlement foundation test(s) failed\n";
		return 1;
	}
	std::cout << "Entitlement foundation tests passed\n";
	return 0;
}
