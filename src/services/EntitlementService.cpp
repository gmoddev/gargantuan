#include "gargantuan/services/EntitlementService.hpp"

#include "gargantuan/classes/Player.hpp"
#include "gargantuan/scripting/StackValue.hpp"

#include <lua.h>
#include <lualib.h>

#include <chrono>
#include <limits>
#include <stdexcept>
#include <string_view>

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
	}

	EntitlementService::EntitlementService() : Provider(std::make_shared<NoneEntitlementProvider>()) {}

	void EntitlementService::ConfigureProvider(std::shared_ptr<IEntitlementProvider> Value) {
		if (!Value) Value = std::make_shared<NoneEntitlementProvider>();
		ValidateIdentityProviderName(Value->Name());
		std::scoped_lock Lock(ProviderMutex);
		if (ProviderGeneration == std::numeric_limits<std::uint64_t>::max())
			throw std::overflow_error("Entitlement provider generation is exhausted");
		Provider = std::move(Value);
		++ProviderGeneration;
	}

	std::uint64_t EntitlementService::GetProviderGeneration() const {
		std::scoped_lock Lock(ProviderMutex);
		return ProviderGeneration;
	}

	std::string EntitlementService::GetProviderName() const {
		std::scoped_lock Lock(ProviderMutex);
		return std::string(Provider->Name());
	}

	EntitlementDecision EntitlementService::Check(
		const std::shared_ptr<Player> &PlayerValue,
		const EntitlementId &Entitlement,
		const EntitlementCancellationToken &Cancellation
	) const {
		if (!PlayerValue || PlayerValue->GetDestroyed() || PlayerValue->IsDestroying())
			throw std::invalid_argument("Entitlement check requires a live Player");
		if (PlayerValue->GetDataModel().get() != GetDataModel().get())
			throw std::invalid_argument("Entitlement check Player belongs to another DataModel");
		const auto Identity = PlayerValue->GetAuthenticationIdentity();
		if (!Identity) return Unavailable({"local", "identity-unavailable"}, Entitlement);
		ValidatePlayerIdentity(*Identity);

		std::shared_ptr<IEntitlementProvider> SelectedProvider;
		{
			std::scoped_lock Lock(ProviderMutex);
			SelectedProvider = Provider;
		}
		const EntitlementRequestContext Context{
			Cancellation,
			std::chrono::steady_clock::now() + DefaultDeadline,
		};
		try {
			auto Result = SelectedProvider->Check(Context, *Identity, Entitlement);
			if (!Result || Context.IsCancelled() || Context.IsExpired() ||
				!ValidDecision(*Result, *Identity, Entitlement))
				return Unavailable(*Identity, Entitlement);
			return std::move(*Result);
		} catch (...) {
			return Unavailable(*Identity, Entitlement);
		}
	}

	std::vector<EntitlementDecision> EntitlementService::CheckMany(
		const std::shared_ptr<Player> &PlayerValue,
		std::span<const EntitlementId> Entitlements,
		const EntitlementCancellationToken &Cancellation
	) const {
		if (Entitlements.empty() || Entitlements.size() > MaximumBatchSize)
			throw std::length_error("Entitlement batch must contain between 1 and 32 IDs");
		if (!PlayerValue || PlayerValue->GetDestroyed() || PlayerValue->IsDestroying())
			throw std::invalid_argument("Entitlement check requires a live Player");
		if (PlayerValue->GetDataModel().get() != GetDataModel().get())
			throw std::invalid_argument("Entitlement check Player belongs to another DataModel");
		const auto Identity = PlayerValue->GetAuthenticationIdentity();
		std::vector<EntitlementDecision> UnavailableResults;
		UnavailableResults.reserve(Entitlements.size());
		const auto FallbackIdentity = Identity.value_or(PlayerIdentity{"local", "identity-unavailable"});
		for (const auto &Entitlement : Entitlements)
			UnavailableResults.push_back(Unavailable(FallbackIdentity, Entitlement));
		if (!Identity) return UnavailableResults;
		ValidatePlayerIdentity(*Identity);

		std::shared_ptr<IEntitlementProvider> SelectedProvider;
		{
			std::scoped_lock Lock(ProviderMutex);
			SelectedProvider = Provider;
		}
		const EntitlementRequestContext Context{
			Cancellation,
			std::chrono::steady_clock::now() + DefaultDeadline,
		};
		try {
			auto Result = SelectedProvider->CheckMany(Context, *Identity, Entitlements);
			if (!Result || Context.IsCancelled() || Context.IsExpired() || Result->size() != Entitlements.size())
				return UnavailableResults;
			for (std::size_t Index = 0; Index < Result->size(); ++Index)
				if (!ValidDecision((*Result)[Index], *Identity, Entitlements[Index])) return UnavailableResults;
			return std::move(*Result);
		} catch (...) {
			return UnavailableResults;
		}
	}

	int EntitlementService::CheckAsync(lua_State *L, Instance *InstanceValue) {
		auto *Service = dynamic_cast<EntitlementService *>(InstanceValue);
		if (!Service) throw std::invalid_argument("EntitlementService receiver is invalid");
		auto Decision = Service->Check(ReadPlayer(L, 2), ReadEntitlementId(L, 3));
		PushDecision(L, Decision);
		return 1;
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
		auto Decisions = Service->CheckMany(PlayerValue, Entitlements);
		lua_createtable(L, static_cast<int>(Decisions.size()), 0);
		for (std::size_t Index = 0; Index < Decisions.size(); ++Index) {
			PushDecision(L, Decisions[Index]);
			lua_rawseti(L, -2, static_cast<int>(Index + 1));
		}
		return 1;
	}
}
