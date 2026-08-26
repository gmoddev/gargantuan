#include "gargantuan/Engine.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/Player.hpp"
#include "gargantuan/entitlements/EntitlementProvider.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/Renderer.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"
#include "gargantuan/services/EntitlementService.hpp"
#include "support/EntitlementProviderConformance.hpp"

#include <Luau/Compiler.h>
#include <lua.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
	using namespace std::chrono_literals;
	int Failures = 0;

	void Check(bool Condition, const char *Message) {
		if (Condition) return;
		std::cerr << "FAIL: " << Message << '\n';
		++Failures;
	}

	void Check(bool Condition, const std::string &Message) {
		Check(Condition, Message.c_str());
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

	class StartFailureProvider final : public VectorProvider {
	  public:
		[[nodiscard]] std::string_view Name() const override {
			return "start-failure";
		}
		[[nodiscard]] gargantuan::EntitlementProviderLifecycleResult
		Start(const gargantuan::EntitlementRequestContext &) override {
			return std::unexpected(
				gargantuan::EntitlementProviderError{gargantuan::EntitlementProviderErrorCode::Unavailable}
			);
		}
		[[nodiscard]] gargantuan::EntitlementProviderHealth GetHealth() const noexcept override {
			return gargantuan::EntitlementProviderHealth::Unavailable;
		}
	};

	class DeadlineFailureProvider final : public VectorProvider {
	  public:
		[[nodiscard]] std::string_view Name() const override {
			return "deadline-failure";
		}
		[[nodiscard]] gargantuan::EntitlementProviderResult Check(
			const gargantuan::EntitlementRequestContext &,
			const gargantuan::PlayerIdentity &,
			const gargantuan::EntitlementId &
		) override {
			return std::unexpected(
				gargantuan::EntitlementProviderError{gargantuan::EntitlementProviderErrorCode::DeadlineExceeded}
			);
		}
	};

	class CountingProvider final : public VectorProvider {
	  public:
		explicit CountingProvider(std::string ProviderName, gargantuan::EntitlementStatus GameBaseStatus)
			: ProviderName(std::move(ProviderName)), GameBaseStatus(GameBaseStatus) {}

		[[nodiscard]] std::string_view Name() const override {
			return ProviderName;
		}
		[[nodiscard]] gargantuan::EntitlementProviderResult Check(
			const gargantuan::EntitlementRequestContext &Context,
			const gargantuan::PlayerIdentity &Identity,
			const gargantuan::EntitlementId &Entitlement
		) override {
			Calls.fetch_add(1);
			if (Context.IsCancelled())
				return std::unexpected(
					gargantuan::EntitlementProviderError{gargantuan::EntitlementProviderErrorCode::Cancelled}
				);
			if (Entitlement.Value() == "game.base")
				return gargantuan::EntitlementDecision{GameBaseStatus, Entitlement, Identity, std::nullopt};
			return VectorProvider::Check(Context, Identity, Entitlement);
		}

		std::atomic<std::uint64_t> Calls{0};

	  private:
		std::string ProviderName;
		gargantuan::EntitlementStatus GameBaseStatus;
	};

	class BlockingProvider final : public VectorProvider {
	  public:
		[[nodiscard]] std::string_view Name() const override {
			return "blocking-custom";
		}
		[[nodiscard]] gargantuan::EntitlementProviderResult Check(
			const gargantuan::EntitlementRequestContext &Context,
			const gargantuan::PlayerIdentity &Identity,
			const gargantuan::EntitlementId &Entitlement
		) override {
			Entered.fetch_add(1);
			while (!Context.IsCancelled() && !Context.IsExpired())
				std::this_thread::sleep_for(1ms);
			return gargantuan::EntitlementDecision{
				gargantuan::EntitlementStatus::Granted, Entitlement, Identity, std::nullopt
			};
		}
		void Stop(const gargantuan::EntitlementRequestContext &) noexcept override {
			Stopped.store(true);
		}

		std::atomic<std::uint64_t> Entered{0};
		std::atomic<bool> Stopped{false};
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

	bool ExecuteAsyncLuau(
		const std::shared_ptr<gargantuan::DataModel> &World,
		const std::shared_ptr<gargantuan::EntitlementService> &Service,
		std::string_view Source
	) {
		gargantuan::ScriptEngine Engine(World);
		lua_State *Thread = lua_newthread(Engine.L);
		const auto ThreadReference = lua_ref(Engine.L, -1);
		lua_pop(Engine.L, 1);
		size_t BytecodeSize = 0;
		char *Bytecode = luau_compile(Source.data(), Source.size(), &Engine.CompileOptions, &BytecodeSize);
		if (!Bytecode) return false;
		const auto LoadStatus = luau_load(Thread, "entitlement-service-test", Bytecode, BytecodeSize, 0);
		std::free(Bytecode);
		int Status = LoadStatus == LUA_OK ? lua_resume(Thread, Engine.L, 0) : LoadStatus;
		const auto Deadline = std::chrono::steady_clock::now() + 5s;
		while (Status == LUA_YIELD && std::chrono::steady_clock::now() < Deadline) {
			Service->PumpAsyncCompletions();
			Status = lua_status(Thread);
			if (Status == LUA_YIELD) std::this_thread::sleep_for(1ms);
		}
		if (Status != LUA_OK)
			std::cerr << "LUAU ERROR: " << (lua_tostring(Thread, -1) ? lua_tostring(Thread, -1) : "timeout") << '\n';
		lua_unref(Engine.L, ThreadReference);
		Service->DetachAsyncRuntime();
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
			[&] { (void)Service->ConfigureProvider(std::make_shared<InvalidNameProvider>()); },
			"trusted provider names must be canonical"
		);
		VectorProvider ConformanceProvider;
		testing::RunEntitlementProviderConformance(
			ConformanceProvider,
			{
				{"steam", "76561198000000001"},
				GameBase,
				*EntitlementId::Parse("dlc.missing_pack"),
				*EntitlementId::Parse("feature.provider_unavailable"),
				*EntitlementId::Parse("feature.trial"),
			},
			[](std::string Failure) { Check(false, "provider conformance: " + Failure); }
		);
		Check(Service->ConfigureProvider(std::make_shared<VectorProvider>()), "valid provider candidate commits");
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
		Check(Service->ConfigureProvider(std::make_shared<DeadlineFailureProvider>()), "deadline provider commits");
		const auto MetricsBeforeDeadline = Service->GetProviderMetrics();
		Check(
			Service->Check(PlayerValue, GameBase).Status == EntitlementStatus::Unavailable &&
				Service->GetProviderMetrics().Timeouts == MetricsBeforeDeadline.Timeouts + 1,
			"provider deadline failure becomes Unavailable and increments timeout metrics"
		);
		Check(
			Service->ConfigureProvider(std::make_shared<InvalidProvider>()),
			"malformed provider can start for validation"
		);
		Check(
			Service->Check(PlayerValue, GameBase).Status == EntitlementStatus::Unavailable,
			"malformed provider output fails closed as Unavailable"
		);

		Check(Service->ConfigureProvider(nullptr), "explicit None provider replacement commits");
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
		Check(
			Service->ConfigureProvider(
				std::make_shared<LocalEntitlementProvider>(std::vector<LocalEntitlementGrant>{
					{{"custom-development", "player.one"}, GameBase, std::nullopt},
					{{"custom-development", "player.one"},
					 *EntitlementId::Parse("feature.expired"),
					 std::chrono::system_clock::now() - 1s},
				})
			),
			"local provider candidate commits"
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
			ExecuteAsyncLuau(World, Service, R"(
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

	void TestTrustedEngineBootstrapComposition() {
		using namespace gargantuan;
		auto World = std::make_shared<DataModel>();
		HeadlessRenderer Renderer(Vector2(320, 180));
		auto Provider = std::make_shared<CountingProvider>("custom-license-v3", EntitlementStatus::Granted);
		Engine Runtime(World, &Renderer, {}, {.Entitlements = Provider});
		auto PlayerValue = MakePlayer(World, {"steam", "76561198000000001"}, "BootstrapPlayer");
		const auto GameBase = *EntitlementId::Parse("game.base");
		Check(
			Runtime.Entitlements->GetProviderName() == "custom-license-v3" &&
				Runtime.Entitlements->Check(PlayerValue, GameBase).Status == EntitlementStatus::Granted,
			"trusted Engine bootstrap composes an unrelated custom provider before gameplay"
		);
		Runtime.Destroy();

		auto FailedWorld = std::make_shared<DataModel>();
		HeadlessRenderer FailedRenderer(Vector2(320, 180));
		Engine FailedRuntime(
			FailedWorld, &FailedRenderer, {}, {.Entitlements = std::make_shared<StartFailureProvider>()}
		);
		auto FailedPlayer = MakePlayer(FailedWorld, {"steam", "76561198000000002"}, "FailedBootstrapPlayer");
		Check(
			FailedRuntime.Entitlements->GetProviderName() == "none" &&
				FailedRuntime.Entitlements->Check(FailedPlayer, GameBase).Status == EntitlementStatus::Unavailable,
			"failed optional bootstrap provider preserves no-backend semantics"
		);
		FailedRuntime.Destroy();
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

	void TestGenerationSafeReplacementAndBounds() {
		using namespace gargantuan;
		auto World = std::make_shared<DataModel>();
		auto Service = std::dynamic_pointer_cast<EntitlementService>(World->GetService("EntitlementService"));
		auto PlayerValue = MakePlayer(World, {"steam", "76561198000000001"}, "SwapPlayer");
		const auto GameBase = *EntitlementId::Parse("game.base");

		auto First = std::make_shared<CountingProvider>("custom-license-v3", EntitlementStatus::Granted);
		Check(Service->ConfigureProvider(First), "custom backend provider commits through the public contract");
		const auto FirstGeneration = Service->GetProviderGeneration();
		Check(Service->Check(PlayerValue, GameBase).Status == EntitlementStatus::Granted, "custom provider grants");
		Check(
			Service->Check(PlayerValue, GameBase).Status == EntitlementStatus::Granted, "semantic cache preserves grant"
		);
		Check(First->Calls.load() == 1, "semantic cache avoids duplicate provider dispatch");

		Check(
			!Service->ConfigureProvider(std::make_shared<StartFailureProvider>()), "failed candidate does not commit"
		);
		Check(
			Service->GetProviderGeneration() == FirstGeneration && Service->GetProviderName() == "custom-license-v3",
			"failed candidate leaves the working generation published"
		);

		auto Second = std::make_shared<CountingProvider>("replacement-authority", EntitlementStatus::Denied);
		Check(Service->ConfigureProvider(Second), "healthy replacement commits");
		Check(Service->GetProviderGeneration() == FirstGeneration + 1, "generation advances exactly once on commit");
		Check(
			Service->Check(PlayerValue, GameBase).Status == EntitlementStatus::Denied && Second->Calls.load() == 1,
			"replacement invalidates cached grants"
		);

		auto Blocking = std::make_shared<BlockingProvider>();
		Check(Service->ConfigureProvider(Blocking), "blocking test provider commits");
		std::atomic<EntitlementStatus> OldCompletion{EntitlementStatus::Granted};
		Check(
			Service->BeginCheck(
				PlayerValue, GameBase, [&](EntitlementDecision Decision) { OldCompletion.store(Decision.Status); }
			),
			"in-flight request is admitted"
		);
		const auto EnterDeadline = std::chrono::steady_clock::now() + 2s;
		while (Blocking->Entered.load() == 0 && std::chrono::steady_clock::now() < EnterDeadline)
			std::this_thread::sleep_for(1ms);
		Check(Blocking->Entered.load() == 1, "old-generation request entered provider");
		Check(Service->ConfigureProvider(Second), "replacement during an in-flight request commits");
		const auto CompletionDeadline = std::chrono::steady_clock::now() + 2s;
		while (OldCompletion.load() == EntitlementStatus::Granted &&
			   std::chrono::steady_clock::now() < CompletionDeadline)
			std::this_thread::sleep_for(1ms);
		Check(OldCompletion.load() == EntitlementStatus::Unavailable, "old in-flight completion fails closed");
		Check(Blocking->Stopped.load(), "old provider receives bounded stop after publication");
		Check(
			Service->Check(PlayerValue, GameBase).Status == EntitlementStatus::Denied,
			"old completion cannot contaminate cache"
		);

		auto Saturated = std::make_shared<BlockingProvider>();
		Check(Service->ConfigureProvider(Saturated), "in-flight bound provider commits");
		std::atomic<std::uint64_t> BoundedCompletions{0};
		for (std::size_t Index = 0; Index < EntitlementService::MaximumInFlightRequests; ++Index)
			Check(
				Service->BeginCheck(
					PlayerValue,
					GameBase,
					[&](EntitlementDecision Decision) {
						if (Decision.Status == EntitlementStatus::Unavailable) BoundedCompletions.fetch_add(1);
					}
				),
				"request within the in-flight bound is admitted"
			);
		Check(
			!Service->BeginCheck(
				PlayerValue,
				GameBase,
				[&](EntitlementDecision Decision) {
					if (Decision.Status == EntitlementStatus::Unavailable) BoundedCompletions.fetch_add(1);
				}
			),
			"request beyond the in-flight bound fails closed"
		);
		Check(
			Service->GetProviderMetrics().InFlightRequests == EntitlementService::MaximumInFlightRequests,
			"in-flight metrics expose the bounded admission ceiling"
		);
		Check(Service->ConfigureProvider(Second), "replacement cancels the saturated generation");
		const auto BoundedDeadline = std::chrono::steady_clock::now() + 5s;
		while (BoundedCompletions.load() < EntitlementService::MaximumInFlightRequests + 1 &&
			   std::chrono::steady_clock::now() < BoundedDeadline)
			std::this_thread::sleep_for(1ms);
		Check(
			BoundedCompletions.load() == EntitlementService::MaximumInFlightRequests + 1 &&
				Service->GetProviderMetrics().InFlightRequests == 0,
			"bounded requests drain as Unavailable after generation replacement"
		);

		for (int Iteration = 0; Iteration < 32; ++Iteration) {
			auto Status = Iteration % 2 == 0 ? EntitlementStatus::Granted : EntitlementStatus::Denied;
			Check(
				Service->ConfigureProvider(
					std::make_shared<CountingProvider>("repeat-" + std::to_string(Iteration), Status)
				),
				"repeated bounded replacement commits"
			);
			Check(
				Service->Check(PlayerValue, GameBase).Status == Status,
				"repeated replacement publishes only the new authority"
			);
		}
		const auto Metrics = Service->GetProviderMetrics();
		Check(
			Metrics.ReplacementAttempts == Metrics.ReplacementCommits + Metrics.ReplacementFailures,
			"replacement metrics account for every bounded attempt"
		);
		Check(Metrics.InFlightRequests == 0, "replacement stress drains in-flight accounting");
	}

	void TestShutdownCancelsProviderWork() {
		using namespace gargantuan;
		auto World = std::make_shared<DataModel>();
		auto Service = std::dynamic_pointer_cast<EntitlementService>(World->GetService("EntitlementService"));
		auto PlayerValue = MakePlayer(World, {"steam", "76561198000000001"}, "ShutdownPlayer");
		const auto GameBase = *EntitlementId::Parse("game.base");
		auto Blocking = std::make_shared<BlockingProvider>();
		Check(Service->ConfigureProvider(Blocking), "shutdown test provider commits");
		std::atomic<EntitlementStatus> Completion{EntitlementStatus::Granted};
		Check(
			Service->BeginCheck(
				PlayerValue, GameBase, [&](EntitlementDecision Decision) { Completion.store(Decision.Status); }
			),
			"shutdown test request is admitted"
		);
		const auto EnterDeadline = std::chrono::steady_clock::now() + 2s;
		while (Blocking->Entered.load() == 0 && std::chrono::steady_clock::now() < EnterDeadline)
			std::this_thread::sleep_for(1ms);
		const auto StartedAt = std::chrono::steady_clock::now();
		Service->ShutdownProviderRuntime();
		Check(std::chrono::steady_clock::now() - StartedAt < 2s, "cooperative shutdown is bounded");
		Check(Completion.load() == EntitlementStatus::Unavailable, "shutdown work fails closed");
		Check(Blocking->Stopped.load(), "shutdown stops the active provider");
		Check(!Service->ConfigureProvider(std::make_shared<VectorProvider>()), "stopped runtime rejects replacement");
	}

	void TestPublicHeadersHaveNoNodeTransportDependency() {
		const auto SourceRoot = std::filesystem::path(GARGANTUAN_SOURCE_DIR);
		const auto Root = SourceRoot / "include/gargantuan";
		for (const auto &Path : {
				 Root / "identity/PlayerIdentity.hpp",
				 Root / "entitlements/EntitlementProvider.hpp",
				 Root / "services/EntitlementService.hpp",
				 Root / "runtime/EngineProviderConfiguration.hpp",
				 Root / "classes/Player.hpp",
				 Root / "Engine.hpp",
				 SourceRoot / "src/entitlements/EntitlementProvider.cpp",
				 SourceRoot / "src/services/EntitlementService.cpp",
				 SourceRoot / "src/Engine.cpp",
			 }) {
			std::ifstream Input(Path, std::ios::binary);
			Check(Input.good(), "public architecture guard source is readable");
			if (!Input) continue;
			const std::string Contents((std::istreambuf_iterator<char>(Input)), std::istreambuf_iterator<char>());
			for (const auto Forbidden : {
					 "gargantuan.node",
					 "gargantuan-node",
					 "NodeEntitlementProvider",
					 "entitlements.grpc.pb",
					 "entitlements.check",
					 "#include <grpc",
					 "#include \"grpc",
				 })
				Check(
					Contents.find(Forbidden) == std::string::npos,
					"public Engine semantic code contains no private backend transport dependency"
				);
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
	TestTrustedEngineBootstrapComposition();
	TestHeadlessRepeatedDataModelLifecycle();
	TestGenerationSafeReplacementAndBounds();
	TestShutdownCancelsProviderWork();
	TestPublicHeadersHaveNoNodeTransportDependency();
	if (Failures != 0) {
		std::cerr << Failures << " entitlement foundation test(s) failed\n";
		return 1;
	}
	std::cout << "Entitlement foundation tests passed\n";
	return 0;
}
