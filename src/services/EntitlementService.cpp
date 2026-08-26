#include "gargantuan/services/EntitlementService.hpp"

#include "gargantuan/Log.hpp"
#include "gargantuan/classes/Player.hpp"
#include "gargantuan/runtime/JobSystem.hpp"
#include "gargantuan/scripting/ScriptSecurity.hpp"
#include "gargantuan/scripting/StackValue.hpp"

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <variant>

namespace gargantuan {
	namespace {
		std::shared_ptr<Player> ReadPlayer(lua_State *L, int Index) {
			auto InstanceValue = StackValue<std::shared_ptr<Instance>>::From(L, Index);
			auto PlayerValue = std::dynamic_pointer_cast<Player>(InstanceValue);
			if (!PlayerValue) luaL_typeerror(L, Index, "Player");
			return PlayerValue;
		}

		EntitlementId ReadEntitlementId(lua_State *L, int Index) {
			size_t Length = 0;
			const auto *Value = luaL_checklstring(L, Index, &Length);
			auto Parsed = EntitlementId::Parse(std::string_view(Value, Length));
			if (!Parsed) throw std::invalid_argument("Entitlement ID is invalid");
			return std::move(*Parsed);
		}

		void PushStringField(lua_State *L, const char *Name, std::string_view Value) {
			lua_pushlstring(L, Value.data(), Value.size());
			lua_setfield(L, -2, Name);
		}

		void PushDecision(lua_State *L, const EntitlementDecision &Decision) {
			lua_createtable(L, 0, 4);
			PushStringField(L, "Status", GetEntitlementStatusName(Decision.Status));
			PushStringField(L, "EntitlementId", Decision.Entitlement.Value());
			lua_createtable(L, 0, 2);
			PushStringField(L, "Provider", Decision.Identity.Provider);
			PushStringField(L, "Subject", Decision.Identity.Subject);
			lua_setfield(L, -2, "Identity");
			if (Decision.ExpiresAt) {
				const auto Milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
											  Decision.ExpiresAt->time_since_epoch()
				)
											  .count();
				lua_pushnumber(L, static_cast<double>(Milliseconds));
				lua_setfield(L, -2, "ExpiresUnixMilliseconds");
			}
		}

		void PushDecisions(lua_State *L, const std::vector<EntitlementDecision> &Decisions) {
			lua_createtable(L, static_cast<int>(Decisions.size()), 0);
			for (std::size_t Index = 0; Index < Decisions.size(); ++Index) {
				PushDecision(L, Decisions[Index]);
				lua_rawseti(L, -2, static_cast<int>(Index + 1));
			}
		}

		bool ValidDecision(
			const EntitlementDecision &Decision, const PlayerIdentity &Identity, const EntitlementId &Entitlement
		) {
			if (Decision.Identity != Identity || Decision.Entitlement != Entitlement) return false;
			if (Decision.Status == EntitlementStatus::Unavailable && Decision.ExpiresAt) return false;
			try {
				ValidatePlayerIdentity(Decision.Identity);
			} catch (...) {
				return false;
			}
			switch (Decision.Status) {
			case EntitlementStatus::Granted:
			case EntitlementStatus::Denied:
			case EntitlementStatus::Unavailable:
				return true;
			}
			return false;
		}

		EntitlementDecision Unavailable(const PlayerIdentity &Identity, const EntitlementId &Entitlement) {
			return {EntitlementStatus::Unavailable, Entitlement, Identity, std::nullopt};
		}

		std::vector<EntitlementDecision>
		UnavailableMany(const PlayerIdentity &Identity, std::span<const EntitlementId> Entitlements) {
			std::vector<EntitlementDecision> Results;
			Results.reserve(Entitlements.size());
			for (const auto &Entitlement : Entitlements)
				Results.push_back(Unavailable(Identity, Entitlement));
			return Results;
		}

		void AddSaturated(std::uint64_t &Value, std::uint64_t Amount = 1) {
			Value = Amount > std::numeric_limits<std::uint64_t>::max() - Value
						? std::numeric_limits<std::uint64_t>::max()
						: Value + Amount;
		}

		PlayerIdentity RequirePlayerIdentity(
			const EntitlementService &Service, const std::shared_ptr<Player> &PlayerValue, bool &Available
		) {
			if (!PlayerValue || PlayerValue->GetDestroyed() || PlayerValue->IsDestroying())
				throw std::invalid_argument("Entitlement check requires a live Player");
			if (PlayerValue->GetDataModel().get() != Service.GetDataModel().get())
				throw std::invalid_argument("Entitlement check Player belongs to another DataModel");
			const auto Identity = PlayerValue->GetAuthenticationIdentity();
			if (!Identity) {
				Available = false;
				return {"local", "identity-unavailable"};
			}
			ValidatePlayerIdentity(*Identity);
			Available = true;
			return *Identity;
		}
	}

	struct EntitlementService::State final {
		struct CacheKey final {
			PlayerIdentity Identity;
			std::string Entitlement;
			std::uint64_t Generation = 0;
			auto operator<=>(const CacheKey &) const = default;
		};

		struct CacheEntry final {
			EntitlementDecision Decision;
			std::chrono::steady_clock::time_point ExpiresAt;
			std::uint64_t Sequence = 0;
		};

		struct Dispatch final {
			std::shared_ptr<IEntitlementProvider> Provider;
			EntitlementCancellationToken ProviderCancellation;
			std::uint64_t Generation = 0;
			std::chrono::steady_clock::time_point Deadline;
			bool Reserved = false;
		};

		struct PendingLuaRequest final {
			lua_State *Thread = nullptr;
			lua_State *Main = nullptr;
			int ThreadReference = LUA_NOREF;
			std::uint64_t RuntimeGeneration = 0;
			EntitlementCancellationToken Cancellation;
			ScriptSecurityContext SecurityContext;
		};

		using LuaResult = std::variant<EntitlementDecision, std::vector<EntitlementDecision>>;
		struct LuaCompletion final {
			std::uint64_t RequestId = 0;
			std::uint64_t RuntimeGeneration = 0;
			LuaResult Result;
		};

		State() : Provider(std::make_shared<NoneEntitlementProvider>()), Workers(2) {}

		std::mutex ReplacementMutex;
		mutable std::mutex ProviderMutex;
		std::shared_ptr<IEntitlementProvider> Provider;
		EntitlementCancellationToken ProviderCancellation;
		std::uint64_t ProviderGeneration = 1;
		std::size_t OutstandingRequests = 0;
		bool Stopping = false;
		std::map<CacheKey, CacheEntry> Cache;
		std::uint64_t NextCacheSequence = 1;
		EntitlementProviderMetrics Metrics;
		JobSystem Workers;

		std::mutex LuaMutex;
		lua_State *LuaMain = nullptr;
		bool LuaAttached = false;
		std::uint64_t LuaRuntimeGeneration = 1;
		std::uint64_t NextLuaRequestId = 1;
		std::unordered_map<std::uint64_t, PendingLuaRequest> PendingLuaRequests;
		std::deque<LuaCompletion> LuaCompletions;
	};

	namespace {
		void RecordDecision(EntitlementService::State &Runtime, EntitlementStatus Status) {
			switch (Status) {
			case EntitlementStatus::Granted:
				AddSaturated(Runtime.Metrics.Granted);
				break;
			case EntitlementStatus::Denied:
				AddSaturated(Runtime.Metrics.Denied);
				break;
			case EntitlementStatus::Unavailable:
				AddSaturated(Runtime.Metrics.Unavailable);
				break;
			}
		}

		void RecordUnavailableChecks(EntitlementService::State &Runtime, std::size_t Count) {
			std::scoped_lock Lock(Runtime.ProviderMutex);
			AddSaturated(Runtime.Metrics.SemanticChecks, Count);
			for (std::size_t Index = 0; Index < Count; ++Index)
				RecordDecision(Runtime, EntitlementStatus::Unavailable);
		}

		void EvictOldestCacheEntry(EntitlementService::State &Runtime) {
			if (Runtime.Cache.size() < EntitlementService::MaximumCacheEntries) return;
			auto Oldest = Runtime.Cache.begin();
			for (auto Iterator = std::next(Runtime.Cache.begin()); Iterator != Runtime.Cache.end(); ++Iterator)
				if (Iterator->second.Sequence < Oldest->second.Sequence) Oldest = Iterator;
			Runtime.Cache.erase(Oldest);
		}

		void CacheDecision(
			EntitlementService::State &Runtime,
			const PlayerIdentity &Identity,
			const EntitlementDecision &Decision,
			std::uint64_t Generation
		) {
			if (Decision.Status == EntitlementStatus::Unavailable) return;
			auto CacheExpiry = std::chrono::steady_clock::now() + EntitlementService::SemanticCacheLifetime;
			if (Decision.Status == EntitlementStatus::Granted && Decision.ExpiresAt) {
				const auto Remaining = *Decision.ExpiresAt - std::chrono::system_clock::now();
				if (Remaining <= std::chrono::system_clock::duration::zero()) return;
				CacheExpiry = std::min(
					CacheExpiry,
					std::chrono::steady_clock::now() +
						std::chrono::duration_cast<std::chrono::steady_clock::duration>(Remaining)
				);
			}
			const EntitlementService::State::CacheKey Key{Identity, Decision.Entitlement.Value(), Generation};
			if (!Runtime.Cache.contains(Key)) EvictOldestCacheEntry(Runtime);
			if (Runtime.NextCacheSequence == std::numeric_limits<std::uint64_t>::max()) {
				Runtime.Cache.clear();
				Runtime.NextCacheSequence = 1;
			}
			Runtime.Cache.insert_or_assign(
				Key, EntitlementService::State::CacheEntry{Decision, CacheExpiry, Runtime.NextCacheSequence++}
			);
		}

		std::optional<EntitlementDecision> FindCachedDecision(
			EntitlementService::State &Runtime, const PlayerIdentity &Identity, const EntitlementId &Entitlement
		) {
			const EntitlementService::State::CacheKey Key{Identity, Entitlement.Value(), Runtime.ProviderGeneration};
			const auto Match = Runtime.Cache.find(Key);
			if (Match == Runtime.Cache.end()) return std::nullopt;
			if (Match->second.ExpiresAt <= std::chrono::steady_clock::now()) {
				Runtime.Cache.erase(Match);
				return std::nullopt;
			}
			return Match->second.Decision;
		}

		EntitlementService::State::Dispatch
		ReserveDispatch(EntitlementService::State &Runtime, std::size_t SemanticChecks) {
			EntitlementService::State::Dispatch Dispatch;
			std::scoped_lock Lock(Runtime.ProviderMutex);
			AddSaturated(Runtime.Metrics.SemanticChecks, SemanticChecks);
			if (Runtime.Stopping || Runtime.OutstandingRequests >= EntitlementService::MaximumInFlightRequests)
				return Dispatch;
			++Runtime.OutstandingRequests;
			Runtime.Metrics.InFlightRequests = Runtime.OutstandingRequests;
			Dispatch.Provider = Runtime.Provider;
			Dispatch.ProviderCancellation = Runtime.ProviderCancellation;
			Dispatch.Generation = Runtime.ProviderGeneration;
			Dispatch.Deadline = std::chrono::steady_clock::now() + EntitlementService::DefaultDeadline;
			Dispatch.Reserved = true;
			return Dispatch;
		}

		void ReleaseDispatch(EntitlementService::State &Runtime) {
			std::scoped_lock Lock(Runtime.ProviderMutex);
			if (Runtime.OutstandingRequests > 0) --Runtime.OutstandingRequests;
			Runtime.Metrics.InFlightRequests = Runtime.OutstandingRequests;
		}

		EntitlementDecision ExecuteSingle(
			EntitlementService::State &Runtime,
			EntitlementService::State::Dispatch Dispatch,
			const PlayerIdentity &Identity,
			const EntitlementId &Entitlement,
			const EntitlementCancellationToken &Cancellation
		) {
			const auto Fallback = Unavailable(Identity, Entitlement);
			if (!Dispatch.Reserved || !Dispatch.Provider) {
				std::scoped_lock Lock(Runtime.ProviderMutex);
				RecordDecision(Runtime, EntitlementStatus::Unavailable);
				return Fallback;
			}
			const EntitlementRequestContext Context{Cancellation, Dispatch.Deadline, Dispatch.ProviderCancellation};
			EntitlementProviderResult ProviderResult = std::unexpected(
				EntitlementProviderError{EntitlementProviderErrorCode::Unavailable}
			);
			const auto StartedAt = std::chrono::steady_clock::now();
			try {
				if (!Context.IsCancelled() && !Context.IsExpired()) {
					{
						std::scoped_lock Lock(Runtime.ProviderMutex);
						AddSaturated(Runtime.Metrics.ProviderCalls);
					}
					ProviderResult = Dispatch.Provider->Check(Context, Identity, Entitlement);
				}
			} catch (...) {
				ProviderResult = std::unexpected(EntitlementProviderError{EntitlementProviderErrorCode::Internal});
			}
			const auto Elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
									 std::chrono::steady_clock::now() - StartedAt
			)
									 .count();
			std::scoped_lock Lock(Runtime.ProviderMutex);
			if (Runtime.OutstandingRequests > 0) --Runtime.OutstandingRequests;
			Runtime.Metrics.InFlightRequests = Runtime.OutstandingRequests;
			const auto ElapsedValue = static_cast<std::uint64_t>(std::max<std::int64_t>(0, Elapsed));
			AddSaturated(Runtime.Metrics.TotalProviderLatencyMicroseconds, ElapsedValue);
			Runtime.Metrics.MaximumProviderLatencyMicroseconds = std::max(
				Runtime.Metrics.MaximumProviderLatencyMicroseconds, ElapsedValue
			);
			if (!ProviderResult || Context.IsCancelled() || Context.IsExpired() || Runtime.Stopping ||
				Runtime.ProviderGeneration != Dispatch.Generation ||
				!ValidDecision(*ProviderResult, Identity, Entitlement)) {
				if (Context.IsExpired() ||
					(!ProviderResult && ProviderResult.error().Code == EntitlementProviderErrorCode::DeadlineExceeded))
					AddSaturated(Runtime.Metrics.Timeouts);
				RecordDecision(Runtime, EntitlementStatus::Unavailable);
				return Fallback;
			}
			CacheDecision(Runtime, Identity, *ProviderResult, Dispatch.Generation);
			RecordDecision(Runtime, ProviderResult->Status);
			return std::move(*ProviderResult);
		}

		std::vector<EntitlementDecision> ExecuteMany(
			EntitlementService::State &Runtime,
			EntitlementService::State::Dispatch Dispatch,
			const PlayerIdentity &Identity,
			const std::vector<EntitlementId> &Entitlements,
			const EntitlementCancellationToken &Cancellation
		) {
			auto Fallback = UnavailableMany(Identity, Entitlements);
			if (!Dispatch.Reserved || !Dispatch.Provider) {
				std::scoped_lock Lock(Runtime.ProviderMutex);
				for (std::size_t Index = 0; Index < Entitlements.size(); ++Index)
					RecordDecision(Runtime, EntitlementStatus::Unavailable);
				return Fallback;
			}
			const EntitlementRequestContext Context{Cancellation, Dispatch.Deadline, Dispatch.ProviderCancellation};
			EntitlementProviderBatchResult ProviderResult = std::unexpected(
				EntitlementProviderError{EntitlementProviderErrorCode::Unavailable}
			);
			const auto StartedAt = std::chrono::steady_clock::now();
			try {
				if (!Context.IsCancelled() && !Context.IsExpired()) {
					{
						std::scoped_lock Lock(Runtime.ProviderMutex);
						AddSaturated(Runtime.Metrics.ProviderCalls);
					}
					ProviderResult = Dispatch.Provider->CheckMany(Context, Identity, Entitlements);
				}
			} catch (...) {
				ProviderResult = std::unexpected(EntitlementProviderError{EntitlementProviderErrorCode::Internal});
			}
			const auto Elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
									 std::chrono::steady_clock::now() - StartedAt
			)
									 .count();
			std::scoped_lock Lock(Runtime.ProviderMutex);
			if (Runtime.OutstandingRequests > 0) --Runtime.OutstandingRequests;
			Runtime.Metrics.InFlightRequests = Runtime.OutstandingRequests;
			const auto ElapsedValue = static_cast<std::uint64_t>(std::max<std::int64_t>(0, Elapsed));
			AddSaturated(Runtime.Metrics.TotalProviderLatencyMicroseconds, ElapsedValue);
			Runtime.Metrics.MaximumProviderLatencyMicroseconds = std::max(
				Runtime.Metrics.MaximumProviderLatencyMicroseconds, ElapsedValue
			);
			bool Valid = ProviderResult && !Context.IsCancelled() && !Context.IsExpired() && !Runtime.Stopping &&
						 Runtime.ProviderGeneration == Dispatch.Generation &&
						 ProviderResult->size() == Entitlements.size();
			if (Valid)
				for (std::size_t Index = 0; Index < Entitlements.size(); ++Index)
					if (!ValidDecision((*ProviderResult)[Index], Identity, Entitlements[Index])) {
						Valid = false;
						break;
					}
			if (!Valid) {
				if (Context.IsExpired() ||
					(!ProviderResult && ProviderResult.error().Code == EntitlementProviderErrorCode::DeadlineExceeded))
					AddSaturated(Runtime.Metrics.Timeouts);
				for (std::size_t Index = 0; Index < Entitlements.size(); ++Index)
					RecordDecision(Runtime, EntitlementStatus::Unavailable);
				return Fallback;
			}
			for (const auto &Decision : *ProviderResult) {
				CacheDecision(Runtime, Identity, Decision, Dispatch.Generation);
				RecordDecision(Runtime, Decision.Status);
			}
			return std::move(*ProviderResult);
		}

		std::pair<std::uint64_t, std::uint64_t> RegisterLuaRequest(
			EntitlementService::State &Runtime, lua_State *L, const EntitlementCancellationToken &Cancellation
		) {
			lua_State *Main = lua_mainthread(L);
			if (L == Main) throw std::runtime_error("Entitlement async checks require a yieldable Luau coroutine");
			lua_pushthread(L);
			lua_xmove(L, Main, 1);
			const auto ThreadReference = lua_ref(Main, -1);
			lua_pop(Main, 1);
			std::scoped_lock Lock(Runtime.LuaMutex);
			if (!Runtime.LuaAttached) {
				Runtime.LuaAttached = true;
				Runtime.LuaMain = Main;
			} else if (Runtime.LuaMain != Main) {
				lua_unref(Main, ThreadReference);
				throw std::runtime_error("EntitlementService cannot span multiple Luau VMs");
			}
			if (Runtime.PendingLuaRequests.size() >= EntitlementService::MaximumInFlightRequests ||
				Runtime.NextLuaRequestId == std::numeric_limits<std::uint64_t>::max()) {
				lua_unref(Main, ThreadReference);
				throw std::length_error("Entitlement async request bound is exhausted");
			}
			const auto RequestId = Runtime.NextLuaRequestId++;
			const auto RuntimeGeneration = Runtime.LuaRuntimeGeneration;
			Runtime.PendingLuaRequests.emplace(
				RequestId,
				EntitlementService::State::PendingLuaRequest{
					L, Main, ThreadReference, RuntimeGeneration, Cancellation, GetCurrentScriptSecurityContext()
				}
			);
			return {RequestId, RuntimeGeneration};
		}

		void QueueLuaCompletion(
			EntitlementService::State &Runtime,
			std::uint64_t RequestId,
			std::uint64_t RuntimeGeneration,
			EntitlementService::State::LuaResult Result
		) {
			std::scoped_lock Lock(Runtime.LuaMutex);
			if (!Runtime.LuaAttached || Runtime.LuaRuntimeGeneration != RuntimeGeneration ||
				!Runtime.PendingLuaRequests.contains(RequestId))
				return;
			Runtime.LuaCompletions.push_back({RequestId, RuntimeGeneration, std::move(Result)});
		}
	}

	EntitlementService::EntitlementService() : Runtime(std::make_unique<State>()) {}

	EntitlementService::~EntitlementService() {
		ShutdownProviderRuntime();
	}

	bool EntitlementService::ConfigureProvider(std::shared_ptr<IEntitlementProvider> Value) {
		if (!Value) Value = std::make_shared<NoneEntitlementProvider>();
		ValidateIdentityProviderName(Value->Name());
		std::scoped_lock ReplacementLock(Runtime->ReplacementMutex);
		{
			std::scoped_lock Lock(Runtime->ProviderMutex);
			AddSaturated(Runtime->Metrics.ReplacementAttempts);
			if (Runtime->Stopping) {
				AddSaturated(Runtime->Metrics.ReplacementFailures);
				return false;
			}
		}
		const EntitlementRequestContext StartContext{{}, std::chrono::steady_clock::now() + DefaultDeadline, {}};
		bool Ready = false;
		try {
			const auto Result = Value->Start(StartContext);
			Ready = Result.has_value() && !StartContext.IsCancelled() && !StartContext.IsExpired() &&
					Value->GetHealth() == EntitlementProviderHealth::Ready;
		} catch (...) {
			Ready = false;
		}
		if (!Ready) {
			Value->Stop(StartContext);
			std::scoped_lock Lock(Runtime->ProviderMutex);
			AddSaturated(Runtime->Metrics.ReplacementFailures);
			return false;
		}

		std::shared_ptr<IEntitlementProvider> OldProvider;
		EntitlementCancellationToken OldCancellation;
		bool StoppedBeforeCommit = false;
		{
			std::scoped_lock Lock(Runtime->ProviderMutex);
			if (Runtime->Stopping) {
				AddSaturated(Runtime->Metrics.ReplacementFailures);
				StoppedBeforeCommit = true;
			} else if (Runtime->ProviderGeneration == std::numeric_limits<std::uint64_t>::max()) {
				AddSaturated(Runtime->Metrics.ReplacementFailures);
				StoppedBeforeCommit = true;
			} else {
				OldProvider = std::move(Runtime->Provider);
				OldCancellation = Runtime->ProviderCancellation;
				Runtime->Provider = std::move(Value);
				Runtime->ProviderCancellation = {};
				++Runtime->ProviderGeneration;
				Runtime->Cache.clear();
				AddSaturated(Runtime->Metrics.ReplacementCommits);
			}
		}
		if (StoppedBeforeCommit) {
			Value->Stop(StartContext);
			return false;
		}
		OldCancellation.Cancel();
		const EntitlementRequestContext StopContext{
			{}, std::chrono::steady_clock::now() + DefaultDeadline, OldCancellation
		};
		if (OldProvider) OldProvider->Stop(StopContext);
		return true;
	}

	void EntitlementService::ShutdownProviderRuntime() {
		if (!Runtime) return;
		DetachAsyncRuntime();
		std::shared_ptr<IEntitlementProvider> Provider;
		EntitlementCancellationToken ProviderCancellation;
		{
			std::scoped_lock ReplacementLock(Runtime->ReplacementMutex);
			{
				std::scoped_lock Lock(Runtime->ProviderMutex);
				if (Runtime->Stopping) return;
				Runtime->Stopping = true;
				Provider = Runtime->Provider;
				ProviderCancellation = Runtime->ProviderCancellation;
				Runtime->Cache.clear();
			}
		}
		ProviderCancellation.Cancel();
		const EntitlementRequestContext StopContext{
			{}, std::chrono::steady_clock::now() + DefaultDeadline, ProviderCancellation
		};
		if (Provider) Provider->Stop(StopContext);
		Runtime->Workers.Shutdown(true);
	}

	std::uint64_t EntitlementService::GetProviderGeneration() const {
		std::scoped_lock Lock(Runtime->ProviderMutex);
		return Runtime->ProviderGeneration;
	}

	std::string EntitlementService::GetProviderName() const {
		std::scoped_lock Lock(Runtime->ProviderMutex);
		return Runtime->Provider ? std::string(Runtime->Provider->Name()) : "none";
	}

	EntitlementProviderHealth EntitlementService::GetProviderHealth() const {
		std::scoped_lock Lock(Runtime->ProviderMutex);
		return Runtime->Provider ? Runtime->Provider->GetHealth() : EntitlementProviderHealth::Unavailable;
	}

	EntitlementProviderMetrics EntitlementService::GetProviderMetrics() const {
		std::scoped_lock Lock(Runtime->ProviderMutex);
		return Runtime->Metrics;
	}

	EntitlementDecision EntitlementService::Check(
		const std::shared_ptr<Player> &PlayerValue,
		const EntitlementId &Entitlement,
		const EntitlementCancellationToken &Cancellation
	) const {
		bool IdentityAvailable = false;
		const auto Identity = RequirePlayerIdentity(*this, PlayerValue, IdentityAvailable);
		if (!IdentityAvailable || Cancellation.IsCancelled()) {
			RecordUnavailableChecks(*Runtime, 1);
			return Unavailable(Identity, Entitlement);
		}
		{
			std::scoped_lock Lock(Runtime->ProviderMutex);
			AddSaturated(Runtime->Metrics.SemanticChecks);
			if (auto Cached = FindCachedDecision(*Runtime, Identity, Entitlement)) {
				AddSaturated(Runtime->Metrics.CacheHits);
				RecordDecision(*Runtime, Cached->Status);
				return *Cached;
			}
			AddSaturated(Runtime->Metrics.CacheMisses);
		}
		auto Dispatch = ReserveDispatch(*Runtime, 0);
		return ExecuteSingle(*Runtime, std::move(Dispatch), Identity, Entitlement, Cancellation);
	}

	std::vector<EntitlementDecision> EntitlementService::CheckMany(
		const std::shared_ptr<Player> &PlayerValue,
		std::span<const EntitlementId> Entitlements,
		const EntitlementCancellationToken &Cancellation
	) const {
		if (Entitlements.empty() || Entitlements.size() > MaximumBatchSize)
			throw std::length_error("Entitlement batch must contain between 1 and 32 IDs");
		bool IdentityAvailable = false;
		const auto Identity = RequirePlayerIdentity(*this, PlayerValue, IdentityAvailable);
		if (!IdentityAvailable || Cancellation.IsCancelled()) {
			RecordUnavailableChecks(*Runtime, Entitlements.size());
			return UnavailableMany(Identity, Entitlements);
		}
		{
			std::scoped_lock Lock(Runtime->ProviderMutex);
			AddSaturated(Runtime->Metrics.SemanticChecks, Entitlements.size());
			std::vector<EntitlementDecision> Cached;
			Cached.reserve(Entitlements.size());
			bool AllCached = true;
			for (const auto &Entitlement : Entitlements) {
				if (auto Decision = FindCachedDecision(*Runtime, Identity, Entitlement)) {
					Cached.push_back(*Decision);
				} else {
					AllCached = false;
				}
			}
			if (AllCached) {
				AddSaturated(Runtime->Metrics.CacheHits, Entitlements.size());
				for (const auto &Decision : Cached)
					RecordDecision(*Runtime, Decision.Status);
				return Cached;
			}
			AddSaturated(Runtime->Metrics.CacheMisses, Entitlements.size());
		}
		auto Dispatch = ReserveDispatch(*Runtime, 0);
		return ExecuteMany(
			*Runtime,
			std::move(Dispatch),
			Identity,
			std::vector<EntitlementId>(Entitlements.begin(), Entitlements.end()),
			Cancellation
		);
	}

	bool EntitlementService::BeginCheck(
		const std::shared_ptr<Player> &PlayerValue,
		const EntitlementId &Entitlement,
		CheckCompletion Completion,
		const EntitlementCancellationToken &Cancellation
	) const {
		if (!Completion) throw std::invalid_argument("Entitlement completion callback is required");
		bool IdentityAvailable = false;
		const auto Identity = RequirePlayerIdentity(*this, PlayerValue, IdentityAvailable);
		if (!IdentityAvailable) {
			RecordUnavailableChecks(*Runtime, 1);
			Completion(Unavailable(Identity, Entitlement));
			return true;
		}
		if (Cancellation.IsCancelled()) {
			RecordUnavailableChecks(*Runtime, 1);
			Completion(Unavailable(Identity, Entitlement));
			return true;
		}
		std::optional<EntitlementDecision> CachedDecision;
		{
			std::scoped_lock Lock(Runtime->ProviderMutex);
			AddSaturated(Runtime->Metrics.SemanticChecks);
			if (auto Cached = FindCachedDecision(*Runtime, Identity, Entitlement)) {
				AddSaturated(Runtime->Metrics.CacheHits);
				RecordDecision(*Runtime, Cached->Status);
				CachedDecision = std::move(*Cached);
			} else {
				AddSaturated(Runtime->Metrics.CacheMisses);
			}
		}
		if (CachedDecision) {
			Completion(std::move(*CachedDecision));
			return true;
		}
		auto Dispatch = ReserveDispatch(*Runtime, 0);
		if (!Dispatch.Reserved) {
			{
				std::scoped_lock Lock(Runtime->ProviderMutex);
				RecordDecision(*Runtime, EntitlementStatus::Unavailable);
			}
			Completion(Unavailable(Identity, Entitlement));
			return false;
		}
		try {
			Runtime->Workers.Submit([Runtime = Runtime.get(),
									 Dispatch = std::move(Dispatch),
									 Identity,
									 Entitlement,
									 Cancellation,
									 Completion = std::move(Completion)]() mutable {
				Completion(ExecuteSingle(*Runtime, std::move(Dispatch), Identity, Entitlement, Cancellation));
			});
			return true;
		} catch (...) {
			ReleaseDispatch(*Runtime);
			{
				std::scoped_lock Lock(Runtime->ProviderMutex);
				RecordDecision(*Runtime, EntitlementStatus::Unavailable);
			}
			Completion(Unavailable(Identity, Entitlement));
			return false;
		}
	}

	bool EntitlementService::BeginCheckMany(
		const std::shared_ptr<Player> &PlayerValue,
		std::span<const EntitlementId> Entitlements,
		CheckManyCompletion Completion,
		const EntitlementCancellationToken &Cancellation
	) const {
		if (!Completion) throw std::invalid_argument("Entitlement completion callback is required");
		if (Entitlements.empty() || Entitlements.size() > MaximumBatchSize)
			throw std::length_error("Entitlement batch must contain between 1 and 32 IDs");
		bool IdentityAvailable = false;
		const auto Identity = RequirePlayerIdentity(*this, PlayerValue, IdentityAvailable);
		const std::vector<EntitlementId> Values(Entitlements.begin(), Entitlements.end());
		if (!IdentityAvailable) {
			RecordUnavailableChecks(*Runtime, Values.size());
			Completion(UnavailableMany(Identity, Values));
			return true;
		}
		if (Cancellation.IsCancelled()) {
			RecordUnavailableChecks(*Runtime, Values.size());
			Completion(UnavailableMany(Identity, Values));
			return true;
		}
		std::optional<std::vector<EntitlementDecision>> CachedDecisions;
		{
			std::scoped_lock Lock(Runtime->ProviderMutex);
			AddSaturated(Runtime->Metrics.SemanticChecks, Values.size());
			std::vector<EntitlementDecision> Cached;
			Cached.reserve(Values.size());
			bool AllCached = true;
			for (const auto &Entitlement : Values) {
				if (auto Decision = FindCachedDecision(*Runtime, Identity, Entitlement)) {
					Cached.push_back(*Decision);
				} else {
					AllCached = false;
				}
			}
			if (AllCached) {
				AddSaturated(Runtime->Metrics.CacheHits, Values.size());
				for (const auto &Decision : Cached)
					RecordDecision(*Runtime, Decision.Status);
				CachedDecisions = std::move(Cached);
			} else
				AddSaturated(Runtime->Metrics.CacheMisses, Values.size());
		}
		if (CachedDecisions) {
			Completion(std::move(*CachedDecisions));
			return true;
		}
		auto Dispatch = ReserveDispatch(*Runtime, 0);
		if (!Dispatch.Reserved) {
			{
				std::scoped_lock Lock(Runtime->ProviderMutex);
				for (std::size_t Index = 0; Index < Values.size(); ++Index)
					RecordDecision(*Runtime, EntitlementStatus::Unavailable);
			}
			Completion(UnavailableMany(Identity, Values));
			return false;
		}
		try {
			Runtime->Workers.Submit([Runtime = Runtime.get(),
									 Dispatch = std::move(Dispatch),
									 Identity,
									 Values,
									 Cancellation,
									 Completion = std::move(Completion)]() mutable {
				Completion(ExecuteMany(*Runtime, std::move(Dispatch), Identity, Values, Cancellation));
			});
			return true;
		} catch (...) {
			ReleaseDispatch(*Runtime);
			{
				std::scoped_lock Lock(Runtime->ProviderMutex);
				for (std::size_t Index = 0; Index < Values.size(); ++Index)
					RecordDecision(*Runtime, EntitlementStatus::Unavailable);
			}
			Completion(UnavailableMany(Identity, Values));
			return false;
		}
	}

	std::size_t EntitlementService::PumpAsyncCompletions() {
		std::vector<std::pair<State::PendingLuaRequest, State::LuaResult>> Ready;
		{
			std::scoped_lock Lock(Runtime->LuaMutex);
			while (!Runtime->LuaCompletions.empty()) {
				auto Completion = std::move(Runtime->LuaCompletions.front());
				Runtime->LuaCompletions.pop_front();
				if (Completion.RuntimeGeneration != Runtime->LuaRuntimeGeneration) continue;
				auto Pending = Runtime->PendingLuaRequests.find(Completion.RequestId);
				if (Pending == Runtime->PendingLuaRequests.end()) continue;
				Ready.emplace_back(std::move(Pending->second), std::move(Completion.Result));
				Runtime->PendingLuaRequests.erase(Pending);
			}
		}
		for (auto &[Request, Result] : Ready) {
			try {
				if (auto *Decision = std::get_if<EntitlementDecision>(&Result))
					PushDecision(Request.Thread, *Decision);
				else
					PushDecisions(Request.Thread, std::get<std::vector<EntitlementDecision>>(Result));
				ScriptSecurityScope SecurityScope(std::move(Request.SecurityContext));
				const auto Status = lua_resume(Request.Thread, Request.Main, 1);
				if (Status != LUA_OK && Status != LUA_YIELD)
					LOG_ERROR(
						Lua, "[Backend:Entitlements] async coroutine failed: %s", lua_tostring(Request.Thread, -1)
					);
			} catch (const std::exception &Error) {
				LOG_ERROR(Lua, "[Backend:Entitlements] async completion failed: %s", Error.what());
			}
			lua_unref(Request.Main, Request.ThreadReference);
		}
		return Ready.size();
	}

	void EntitlementService::DetachAsyncRuntime() {
		if (!Runtime) return;
		std::vector<State::PendingLuaRequest> Pending;
		{
			std::scoped_lock Lock(Runtime->LuaMutex);
			if (!Runtime->LuaAttached && Runtime->PendingLuaRequests.empty()) return;
			Runtime->LuaAttached = false;
			Runtime->LuaMain = nullptr;
			Runtime->LuaRuntimeGeneration = Runtime->LuaRuntimeGeneration == std::numeric_limits<std::uint64_t>::max()
												? 1
												: Runtime->LuaRuntimeGeneration + 1;
			Runtime->LuaCompletions.clear();
			Pending.reserve(Runtime->PendingLuaRequests.size());
			for (auto &[RequestId, Request] : Runtime->PendingLuaRequests) {
				(void)RequestId;
				Pending.push_back(std::move(Request));
			}
			Runtime->PendingLuaRequests.clear();
		}
		for (auto &Request : Pending) {
			Request.Cancellation.Cancel();
			lua_unref(Request.Main, Request.ThreadReference);
		}
	}

	int EntitlementService::CheckAsync(lua_State *L, Instance *InstanceValue) {
		auto *Service = dynamic_cast<EntitlementService *>(InstanceValue);
		if (!Service) throw std::invalid_argument("EntitlementService receiver is invalid");
		auto PlayerValue = ReadPlayer(L, 2);
		auto Entitlement = ReadEntitlementId(L, 3);
		EntitlementCancellationToken Cancellation;
		const auto [RequestId, RuntimeGeneration] = RegisterLuaRequest(*Service->Runtime, L, Cancellation);
		(void)Service->BeginCheck(
			PlayerValue,
			Entitlement,
			[Runtime = Service->Runtime.get(), RequestId, RuntimeGeneration](EntitlementDecision Decision) mutable {
				QueueLuaCompletion(*Runtime, RequestId, RuntimeGeneration, std::move(Decision));
			},
			Cancellation
		);
		return lua_yield(L, 0);
	}

	int EntitlementService::CheckManyAsync(lua_State *L, Instance *InstanceValue) {
		auto *Service = dynamic_cast<EntitlementService *>(InstanceValue);
		if (!Service) throw std::invalid_argument("EntitlementService receiver is invalid");
		auto PlayerValue = ReadPlayer(L, 2);
		luaL_checktype(L, 3, LUA_TTABLE);
		const auto Count = lua_objlen(L, 3);
		if (Count == 0 || Count > MaximumBatchSize)
			throw std::length_error("Entitlement batch must contain between 1 and 32 IDs");
		std::vector<EntitlementId> Entitlements;
		Entitlements.reserve(Count);
		for (std::size_t Index = 1; Index <= Count; ++Index) {
			lua_rawgeti(L, 3, static_cast<int>(Index));
			Entitlements.push_back(ReadEntitlementId(L, -1));
			lua_pop(L, 1);
		}
		EntitlementCancellationToken Cancellation;
		const auto [RequestId, RuntimeGeneration] = RegisterLuaRequest(*Service->Runtime, L, Cancellation);
		(void)Service->BeginCheckMany(
			PlayerValue,
			Entitlements,
			[Runtime = Service->Runtime.get(), RequestId, RuntimeGeneration](
				std::vector<EntitlementDecision> Decisions
			) mutable { QueueLuaCompletion(*Runtime, RequestId, RuntimeGeneration, std::move(Decisions)); },
			Cancellation
		);
		return lua_yield(L, 0);
	}
}
