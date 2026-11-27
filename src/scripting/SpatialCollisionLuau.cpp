#include "gargantuan/classes/Player.hpp"
#include "gargantuan/classes/Script.hpp"
#include "gargantuan/classes/SpatialRegion.hpp"
#include "gargantuan/datatypes/Vector3.hpp"
#include "gargantuan/network/SpatialTransferProtection.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"
#include "gargantuan/runtime/SpatialResidency.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"
#include "gargantuan/scripting/Userdata.hpp"
#include "gargantuan/services/Players.hpp"

#include <cmath>
#include <stdexcept>
#include <string>

namespace gargantuan {
	G_USERDATA_DECL(
		SpatialLeaseUserdata, std::weak_ptr<DataModel> Realm; SpatialResidencyAudience Audience;
		std::shared_ptr<SpatialCollisionLease> Holder;
	);
	G_USERDATA_DECL(
		SpatialTransferUserdata, std::weak_ptr<DataModel> Realm; SpatialResidencyAudience Issuer;
		network::SpatialTransferTicket Ticket;
	);
	namespace {
		struct Access {
			std::shared_ptr<DataModel> Realm;
			SpatialResidencyAudience Audience;
		};
		std::optional<Access> CurrentAccess(lua_State *L) {
			const auto &Security = GetCurrentScriptSecurityContext();
			const auto Engine = ScriptEngine::Get(L);
			if (GetCurrentExecutionDomain() != ExecutionDomain::Main || !Engine || Engine->IsStopping() ||
				!Engine->DataModel || Engine->Mode == RuntimeMode::NetworkClient ||
				Security.Domain != ScriptExecutionDomain::Server ||
				!Security.HasCapability(ScriptCapability::ReadDataModel) || !Security.IssuingScript.IsValid() ||
				!Security.ExecutionGeneration)
				return {};
			const auto Owner = std::dynamic_pointer_cast<Script>(ObjectRegistry::Get().Lookup(Security.IssuingScript));
			if (!Owner || !Owner->IsCurrentExecution(Security.ExecutionGeneration) ||
				SpatialRegion::FindAttachedWorld(*Owner) != Engine->DataModel)
				return {};
			return Access{Engine->DataModel, {Security.IssuingScript, Security.ExecutionGeneration}};
		}
		SpatialCollisionLease &Owned(lua_State *L, SpatialLeaseUserdata *Handle) {
			const auto Allowed = CurrentAccess(L);
			if (!Allowed || !Handle || !Handle->Holder || Allowed->Realm != Handle->Realm.lock() ||
				Allowed->Audience != Handle->Audience)
				throw std::runtime_error("[Spatial:Residency] This execution cannot use this collision request");
			return *Handle->Holder;
		}
		std::optional<std::chrono::milliseconds> Duration(lua_State *L, int Index) {
			if (lua_type(L, Index) != LUA_TNUMBER) return {};
			const auto Seconds = lua_tonumber(L, Index);
			if (!std::isfinite(Seconds) || Seconds <= 0 || Seconds > 60) return {};
			return std::chrono::milliseconds(static_cast<int>(std::ceil(Seconds * 1000.)));
		}
		std::optional<SpatialDemandPriority> Priority(lua_State *L, int Index) {
			if (lua_type(L, Index) != LUA_TSTRING) return {};
			std::size_t Count{};
			const auto Text = lua_tolstring(L, Index, &Count);
			const std::string_view Name(Text, Count);
			if (Name == "Interactive") return SpatialDemandPriority::Interactive;
			if (Name == "Normal") return SpatialDemandPriority::Normal;
			if (Name == "Background") return SpatialDemandPriority::Background;
			return {};
		}
		const char *Life(SpatialLeaseLife Value) {
			switch (Value) {
			case SpatialLeaseLife::Active:
				return "Active";
			case SpatialLeaseLife::Released:
				return "Released";
			case SpatialLeaseLife::Expired:
				return "Expired";
			case SpatialLeaseLife::Stale:
				return "Stale";
			}
			return "Stale";
		}
		const char *Condition(SpatialCollisionCondition Value) {
			switch (Value) {
			case SpatialCollisionCondition::Pending:
				return "Pending";
			case SpatialCollisionCondition::Ready:
				return "Ready";
			case SpatialCollisionCondition::Unavailable:
				return "Unavailable";
			}
			return "Unavailable";
		}
		const char *TransferDisposition(network::SpatialTransferStatus Status) {
			using enum network::SpatialTransferStatus;
			switch (Status) {
			case Progress:
				return "Pending";
			case Applied:
				return "Applied";
			case CommittedUnconfirmed:
				return "CommittedUnconfirmed";
			case Canceled:
				return "Canceled";
			case Stale:
				return "Stale";
			case Denied:
				return "Denied";
			case Busy:
				return "Busy";
			case Expired:
				return "Expired";
			}
			return "Stale";
		}
		Access TransferAccess(lua_State *L, const SpatialTransferUserdata *Handle) {
			const auto Allowed = CurrentAccess(L);
			if (!Allowed || !Handle || !Handle->Ticket.Result || Allowed->Realm != Handle->Realm.lock() ||
				Allowed->Audience != Handle->Issuer ||
				!GetCurrentScriptSecurityContext().HasCapability(ScriptCapability::MutateDataModel))
				throw std::runtime_error("[Spatial:Transfer] This execution cannot use this movement request");
			return *Allowed;
		}
		int TransferStatus(lua_State *L, SpatialTransferUserdata *Handle) {
			(void)TransferAccess(L, Handle);
			lua_pushstring(L, TransferDisposition(*Handle->Ticket.Result));
			return 1;
		}
		int CancelTransfer(lua_State *L, SpatialTransferUserdata *Handle) {
			const auto Allowed = TransferAccess(L, Handle);
			if (*Handle->Ticket.Result == network::SpatialTransferStatus::Progress)
				if (const auto Host = Allowed.Realm->GetSpatialTransferProtection();
					Host && Host->CancelCreatorTransfer)
					Host->CancelCreatorTransfer(Handle->Ticket);
			lua_pushstring(L, TransferDisposition(*Handle->Ticket.Result));
			return 1;
		}
		int Status(lua_State *L, SpatialLeaseUserdata *Handle) {
			const auto Now = Owned(L, Handle).GetStatus(SpatialResidencyClock::now());
			lua_createtable(L, 0, 3);
			lua_pushstring(L, Life(Now.Life));
			lua_setfield(L, -2, "Life");
			lua_pushstring(L, Condition(Now.Collision));
			lua_setfield(L, -2, "Collision");
			const auto Revision = std::to_string(Now.Revision);
			lua_pushlstring(L, Revision.data(), Revision.size());
			lua_setfield(L, -2, "Revision");
			lua_setreadonly(L, -1, true);
			return 1;
		}
		int SetArea(lua_State *L, SpatialLeaseUserdata *Handle) {
			auto &Lease = Owned(L, Handle);
			const SpatialDestinationBounds Bounds{CheckStackValue<glm::vec3>(L, 2), CheckStackValue<glm::vec3>(L, 3)};
			lua_pushboolean(L, Lease.SetArea(SpatialResidencyClock::now(), Bounds));
			return 1;
		}
		int Renew(lua_State *L, SpatialLeaseUserdata *Handle) {
			auto &Lease = Owned(L, Handle);
			const auto Seconds = Duration(L, 2);
			lua_pushboolean(L, Seconds && Lease.Renew(SpatialResidencyClock::now(), *Seconds));
			return 1;
		}
		int SetPriority(lua_State *L, SpatialLeaseUserdata *Handle) {
			auto &Lease = Owned(L, Handle);
			const auto Selected = Priority(L, 2);
			lua_pushboolean(L, Selected && Lease.SetPriority(SpatialResidencyClock::now(), *Selected));
			return 1;
		}
		int Release(lua_State *L, SpatialLeaseUserdata *Handle) {
			Owned(L, Handle).Release();
			return 0;
		}
		int Refuse(lua_State *L, const char *Reason) {
			lua_pushnil(L);
			lua_pushstring(L, Reason);
			return 2;
		}
		const char *Failure(SpatialRequestFailure Value) {
			switch (Value) {
			case SpatialRequestFailure::Invalid:
				return "Invalid";
			case SpatialRequestFailure::Unavailable:
				return "Unavailable";
			case SpatialRequestFailure::Budget:
				return "Budget";
			case SpatialRequestFailure::Stale:
				return "Stale";
			}
			return "Unavailable";
		}
	}
	G_USERDATA_IMPL(
		SpatialLeaseUserdata,
		.Tag = UserdataTag::SpatialCollisionLease,
		.Type = "SpatialCollisionLease",
		.Methods = {
			{"GetStatus", {.Call = &Status}},
			{"SetArea", {.Call = &SetArea}},
			{"SetPriority", {.Call = &SetPriority}},
			{"Renew", {.Call = &Renew}},
			{"Release", {.Call = &Release}}
		}
	);
	G_USERDATA_IMPL(
		SpatialTransferUserdata,
		.Tag = UserdataTag::SpatialTransferTicket,
		.Type = "SpatialTransfer",
		.Methods = {{"GetStatus", {.Call = &TransferStatus}}, {"Cancel", {.Call = &CancelTransfer}}}
	);
	void RegisterSpatialCollisionUserdata(lua_State *L) {
		SpatialLeaseUserdata::CreateUserdataMetatable(L);
		SpatialTransferUserdata::CreateUserdataMetatable(L);
	}
	int Player::RequestSpatialTransfer(lua_State *L, Instance *Value) {
		const auto Allowed = CurrentAccess(L);
		const auto Engine = ScriptEngine::Get(L);
		auto *Owner = dynamic_cast<Player *>(Value);
		if (!Allowed || !Owner || !Engine ||
			!GetCurrentScriptSecurityContext().HasCapability(ScriptCapability::MutateDataModel) ||
			Owner->GetDataModel() != Allowed->Realm || Owner->GetDestroyed() || Owner->IsDestroying())
			return Refuse(L, "Unavailable");
		const auto Destination = CheckStackValue<std::shared_ptr<WorldRoot>>(L, 2);
		const auto Pose = CheckStackValue<CFrame>(L, 3);
		const SpatialDestinationBounds Bounds{CheckStackValue<glm::vec3>(L, 4), CheckStackValue<glm::vec3>(L, 5)};
		if (!Destination || SpatialRegion::FindAttachedWorld(*Destination) != Allowed->Realm ||
			!Destination->GetSpatialActivation())
			return Refuse(L, "Unavailable");
		for (unsigned I = 0; I < 3; ++I)
			if (!std::isfinite(Bounds.Minimum[I]) || !std::isfinite(Bounds.Maximum[I]) ||
				Bounds.Minimum[I] >= Bounds.Maximum[I] || double(Bounds.Maximum[I]) - Bounds.Minimum[I] > 512.)
				return Refuse(L, "Invalid");
		network::SpatialTransferTicket Ticket;
		if (Engine->Mode == RuntimeMode::NetworkServer) {
			const auto Host = Allowed->Realm->GetSpatialTransferProtection();
			if (!Host || !Host->RequestCreatorTransfer) return Refuse(L, "Unavailable");
			auto Started = Host->RequestCreatorTransfer(
				*Owner, Destination->GetObjectId(), Pose, Bounds, Allowed->Audience
			);
			if (!Started) return Refuse(L, TransferDisposition(Started.error()));
			Ticket = std::move(*Started);
		} else if (Engine->Mode == RuntimeMode::Offline && !Allowed->Realm->HasDefaultNetworkSpatialProfile()) {
			const auto PlayersService = std::dynamic_pointer_cast<Players>(Owner->GetParent().value_or(nullptr));
			if (!PlayersService || PlayersService->GetLocalPlayer().value_or(nullptr).get() != Owner)
				return Refuse(L, "Unavailable");
			const auto Actor = network::GetSpatialTransferSubject(*Owner);
			const auto Source = Actor ? WorldRoot::GetEffectiveWorld(*Actor) : nullptr;
			const auto Proof = Destination->PrepareSpatialDestination(Bounds);
			if (!Source || Source == Destination || !Proof ||
				!Destination
					 ->CommitSpatialTransfer(
						 Actor,
						 {Actor->GetObjectId(),
						  Source->GetObjectId(),
						  Source->GetSpatialActivation(),
						  Actor->GetCFrame(),
						  Pose},
						 *Proof,
						 {}
					 )
					 .Succeeded())
				return Refuse(L, "Denied");
			Ticket.Result = std::make_shared<const network::SpatialTransferStatus>(
				network::SpatialTransferStatus::Applied
			);
		} else
			return Refuse(L, "Unavailable");
		StackValue<SpatialTransferUserdata>::Push(L, {{}, Allowed->Realm, Allowed->Audience, std::move(Ticket)});
		lua_pushnil(L);
		return 2;
	}
	int WorldRoot::RequestCollisionArea(lua_State *L, Instance *Value) {
		const auto Allowed = CurrentAccess(L);
		if (!Allowed) return Refuse(L, "Unavailable");
		const auto Target = dynamic_cast<WorldRoot *>(Value);
		if (!Target || Target->GetDestroyed() || Target->IsDestroying() ||
			SpatialRegion::FindAttachedWorld(*Target) != Allowed->Realm)
			return Refuse(L, "Unavailable");
		const SpatialDestinationBounds Bounds{CheckStackValue<glm::vec3>(L, 2), CheckStackValue<glm::vec3>(L, 3)};
		const auto Time = lua_isnoneornil(L, 4) ? std::optional(std::chrono::milliseconds(5000)) : Duration(L, 4);
		const auto Rank = lua_isnoneornil(L, 5) ? std::optional(SpatialDemandPriority::Normal) : Priority(L, 5);
		if (!Time || !Rank) return Refuse(L, "Invalid");
		auto Lease = Target->RequestSpatialCollision(
			Allowed->Audience, Bounds, {}, *Rank, *Time, SpatialResidencyClock::now()
		);
		if (!Lease) return Refuse(L, Failure(Lease.error()));
		StackValue<SpatialLeaseUserdata>::Push(L, {{}, Allowed->Realm, Allowed->Audience, std::move(*Lease)});
		lua_pushnil(L);
		return 2;
	}
}
