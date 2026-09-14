#pragma once

#include "gargantuan/network/Connection.hpp"
#include "gargantuan/network/ReliableServiceProfile.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>

namespace gargantuan::network::detail {

// Main-owned byte resource accounting only. It retains neither messages nor
// object identities, plans, callbacks or authoritative application state.
class ReliableByteAdmission {
public:
	using Metrics = ReliableByteAdmissionMetrics;
	struct Reservation {
		std::uint64_t Token = 0;
		ConnectionId Connection;
		std::uint64_t Bytes = 0;
		auto operator<=>(const Reservation &) const = default;
	};
	// GameSession verifies generation, native receipts and aggregate accounting.
	// This value carries resource evidence only, never transport handles.
	struct ServiceObservation {
		std::uint64_t ObservedAtMicroseconds = 0;
		bool Qualified = false;
		bool Available = false;
		std::uint64_t OrdinaryDebt = 0;
	};
	explicit ReliableByteAdmission(ReliableServiceProfile Value) : Profile(Value) {
		if (!Profile.IsValid()) throw std::invalid_argument(std::string(Profile.ValidationError()));
	}
	bool BeginStep(std::uint64_t Microseconds) {
		if (Active || Step == std::numeric_limits<std::uint64_t>::max() || !Advance(Microseconds)) return false;
		++Step; AggregateExposure = 0; FeedbackComplete = true; Unobserved = Peers.size(); OrdinaryFunding.reset();
		for (auto &[Id, Value] : Peers) { (void)Id; Value.Seen = false; Value.Exposure.reset(); Value.Service.reset(); }
		return true;
	}
	bool ObserveService(ConnectionId Id, std::optional<ServiceObservation> Service) {
		if (!Profile.IsPooled() || !Observe(Id, Service ? std::optional<std::uint64_t>(0) : std::nullopt)) return false;
		return RefreshService(Id, Service);
	}
	bool RefreshService(ConnectionId Id, std::optional<ServiceObservation> Service) {
		auto Found = Peers.find(Id);
		if (!Profile.IsPooled() || Found == Peers.end() || !Found->second.Seen || Active) return false;
		auto &Value = Found->second;
		Value.Service = Service;
		if (Value.OwnsGrant && !Value.Debt && Service && !Service->OrdinaryDebt) {
			Value.OwnsGrant = false; --Totals.ActiveDrainGrants;
		}
		return true;
	}
	void SetOrdinaryFunding(std::uint64_t Bytes) { OrdinaryFunding = Bytes; }
	bool Retire(ConnectionId Id, std::uint64_t Token, std::uint64_t Bytes) {
		auto Found = Peers.find(Id);
		if (!Profile.IsPooled() || Found == Peers.end() || !Token || !Bytes ||
			Found->second.DebtToken != Token || Found->second.Debt != Bytes ||
			Bytes > std::numeric_limits<std::uint64_t>::max() - Totals.VerifiedAttributedRetirement) return false;
		Found->second.Debt = Found->second.DebtToken = 0;
		Totals.OutstandingBytes -= Bytes;
		Totals.VerifiedAttributedRetirement += Bytes;
		return true;
	}
	bool TerminalRelease(ConnectionId Id) {
		auto Found = Peers.find(Id);
		if (!Profile.IsPooled() || Found == Peers.end() || (Active && Active->Connection == Id)) return false;
		const auto Bytes = Found->second.Debt;
		if (Bytes > std::numeric_limits<std::uint64_t>::max() - Totals.TerminalReleasedBytes) return false;
		Totals.TerminalReleasedBytes += Bytes; Totals.OutstandingBytes -= Bytes;
		if (Found->second.OwnsGrant) --Totals.ActiveDrainGrants;
		Found->second.OwnsGrant = false;
		Found->second.Debt = Found->second.DebtToken = 0;
		Remove(Id);
		return true;
	}
	bool Observe(ConnectionId Id, std::optional<std::uint64_t> Exposure) {
		if (!Step || !Id.IsValid()) return false;
		auto Found = Peers.find(Id);
		if (Found == Peers.end()) {
			if (Peers.size() >= Profile.MaximumConnections) return false;
			// Do not permit a stale generation to coexist with a reconnected slot.
			for (const auto &[Other, Value] : Peers) { (void)Value; if (Other.Slot == Id.Slot) return false; }
			Found = Peers.try_emplace(Id).first;
			Found->second.Credit.Updated = Now;
			++Unobserved;
		}
		auto &Value = Found->second;
		if (Value.Seen) return false;
		Value.Seen = true; Value.Exposure = Exposure; --Unobserved;
		if (!Exposure) FeedbackComplete = false;
		else {
			Add(AggregateExposure, *Exposure);
			Totals.PeerBacklogHighWater = std::max(Totals.PeerBacklogHighWater, *Exposure);
			Totals.GlobalBacklogHighWater = std::max(Totals.GlobalBacklogHighWater, AggregateExposure);
		}
		return true;
	}
	std::uint64_t Allowance(ConnectionId Id, std::uint64_t Microseconds) {
		if (Active || !Advance(Microseconds)) return 0;
		auto Found = Peers.find(Id);
		if (Found == Peers.end()) return 0;
		auto &Value = Found->second;
		Value.DemandStep = Step;
		if (!Value.WaitSince) Value.WaitSince = Now;
		Refill(Value.Credit, Profile.IsPooled() ? Profile.Pooled.PeerCreditRate : Profile.PeerStructuralRate(), PeerCap());
		Totals.PeerCreditHighWater = std::max(Totals.PeerCreditHighWater, Value.Credit.Bytes);
		if (!FeedbackComplete || Unobserved || !Value.Seen || !Value.Exposure) {
			Add(Totals.FeedbackDeferrals, 1); ReleaseWait(Id); return 0;
		}
		if (Profile.IsPooled()) {
			if (!Value.Service || !Value.Service->Available || Value.Service->ObservedAtMicroseconds > Now ||
				Now - Value.Service->ObservedAtMicroseconds > Profile.Pooled.FeedbackFreshnessMicroseconds) {
				Add(Totals.FeedbackDeferrals, 1); ReleaseWait(Id); return 0;
			}
			if (Value.OwnsGrant || Totals.ActiveDrainGrants >= Profile.Pooled.MaximumDrainGrants ||
				Profile.Pooled.RequalificationMicroseconds > std::numeric_limits<std::uint64_t>::max() - Now ||
				(!Value.Service->Qualified && Now < Value.NextQualification)) {
				Add(Totals.GrantDeferrals, 1); ReleaseWait(Id); return 0;
			}
		}
		const auto PeerRoom = Profile.IsPooled() ? MaximumReliableServiceGroupBytes :
			Room(*Value.Exposure, Profile.PeerBacklog - Profile.GameplayBurst, Value.Backlogged);
		const auto GlobalRoom = Profile.IsPooled() ? FundedRoom() : Room(AggregateExposure,
			Profile.GlobalBacklog - Profile.GameplayBurst * Profile.MaximumConnections, GlobalBacklogged);
		if (PeerRoom < Value.Required || GlobalRoom < Value.Required) {
			if (Profile.IsPooled()) Add(Totals.FundedDeferrals, 1);
			Add(Totals.BacklogDeferrals, 1); ReleaseWait(Id); return 0;
		}
		if (Value.Credit.Bytes < Value.Required) {
			Add(Totals.CreditDeferrals, 1); ReleaseWait(Id); return 0;
		}
		if (GlobalWait && *GlobalWait != Id) { Add(Totals.FairnessDeferrals, 1); return 0; }
		if (Global.Bytes < Value.Required) {
			GlobalWait = Id; Add(Totals.CreditDeferrals, 1); return 0;
		}
		return std::min({Value.Credit.Bytes, Global.Bytes, PeerRoom, GlobalRoom, MaximumReliableServiceGroupBytes});
	}
	void DeferSize(ConnectionId Id, std::uint64_t CompleteBytes) {
		auto Found = Peers.find(Id);
		if (Found == Peers.end() || CompleteBytes < MinimumFrameBytes || CompleteBytes > MaximumReliableServiceGroupBytes) return;
		Found->second.Required = CompleteBytes;
		Add(Totals.SizeDeferrals, 1); Add(Totals.DeferredBytes, CompleteBytes);
		// Earmark global replenishment for this eligible peer before small peers
		// can consume every subsequent refill. Local blockage never owns the turn.
		(void)Allowance(Id, Now);
	}
	std::optional<Reservation> Reserve(ConnectionId Id, std::uint64_t Bytes) {
		if (Bytes < MinimumFrameBytes || Bytes > Allowance(Id, Now) || NextToken == std::numeric_limits<std::uint64_t>::max()) return {};
		if (Profile.IsPooled() && Bytes > std::numeric_limits<std::uint64_t>::max() - Totals.AcceptedBytes) return {};
		auto &Value = Peers.at(Id);
		Value.Credit.Bytes -= Bytes; Global.Bytes -= Bytes;
		if (!Profile.IsPooled()) { Add(*Value.Exposure, Bytes); Add(AggregateExposure, Bytes); }
		Totals.PeerBacklogHighWater = std::max(Totals.PeerBacklogHighWater, *Value.Exposure);
		Totals.GlobalBacklogHighWater = std::max(Totals.GlobalBacklogHighWater, AggregateExposure);
		Active = Reservation{++NextToken, Id, Bytes}; Add(Totals.ReservedBytes, Bytes);
		return Active;
	}
	bool Commit(Reservation Receipt) {
		if (!Active || *Active != Receipt) return false;
		auto &Value = Peers.at(Receipt.Connection);
		if (Profile.IsPooled()) {
			Value.Debt = Receipt.Bytes; Value.DebtToken = Receipt.Token;
			Value.OwnsGrant = true;
			Totals.OutstandingBytes += Receipt.Bytes; ++Totals.ActiveDrainGrants;
			Totals.OutstandingHighWater = std::max(Totals.OutstandingHighWater, Totals.OutstandingBytes);
			Totals.DrainGrantsHighWater = std::max(Totals.DrainGrantsHighWater, Totals.ActiveDrainGrants);
			if (!Value.Service->Qualified) {
				Add(Totals.QualificationGrants, 1);
			}
			Value.NextQualification = Now + Profile.Pooled.RequalificationMicroseconds;
		}
		if (Value.WaitSince) Totals.MaximumAdmissionWaitMicroseconds = std::max(
			Totals.MaximumAdmissionWaitMicroseconds, Now - *Value.WaitSince);
		Value.WaitSince.reset(); Value.Required = MinimumFrameBytes;
		Add(Totals.AcceptedBytes, Receipt.Bytes); Active.reset(); ReleaseWait(Receipt.Connection); return true;
	}
	bool Rollback(Reservation Receipt) {
		if (!Active || *Active != Receipt) return false;
		auto &Value = Peers.at(Receipt.Connection);
		Value.Credit.Bytes += std::min(Receipt.Bytes, PeerCap() - Value.Credit.Bytes);
		Global.Bytes += std::min(Receipt.Bytes, GlobalCap() - Global.Bytes);
		if (!Profile.IsPooled()) { *Value.Exposure -= Receipt.Bytes; AggregateExposure -= Receipt.Bytes; }
		Add(Totals.RolledBackBytes, Receipt.Bytes); Active.reset(); return true;
	}
	void NoWork(ConnectionId Id) {
		if (auto Found = Peers.find(Id); Found != Peers.end()) {
			Found->second.Required = MinimumFrameBytes; Found->second.WaitSince.reset(); Found->second.DemandStep = 0;
		}
		ReleaseWait(Id);
	}
	void Remove(ConnectionId Id) {
		if (Active && Active->Connection == Id) (void)Rollback(*Active);
		if (auto Found = Peers.find(Id); Found != Peers.end()) {
			// Accepted pooled obligations require explicit irreversible terminal
			// evidence. A generic disconnect request cannot erase them.
			if (Profile.IsPooled() && Found->second.OwnsGrant) return;
			if (Found->second.Exposure) AggregateExposure -= std::min(AggregateExposure, *Found->second.Exposure);
			if (!Found->second.Seen && Unobserved) --Unobserved;
			Peers.erase(Found);
		}
		ReleaseWait(Id);
	}
	void EndStep() {
		Totals.OldestWaitMicroseconds = 0;
		for (auto &[Id, Value] : Peers) {
			if (Value.DemandStep != Step) NoWork(Id);
			if (Value.WaitSince) Totals.OldestWaitMicroseconds = std::max(Totals.OldestWaitMicroseconds, Now - *Value.WaitSince);
		}
		if (Peers.empty()) Totals.OldestWaitMicroseconds = 0;
	}
	[[nodiscard]] const Metrics &GetMetrics() const { return Totals; }
	[[nodiscard]] std::size_t PeerCount() const { return Peers.size(); }
	[[nodiscard]] std::size_t LogicalBytes() const { return sizeof(*this) + Peers.size() * sizeof(decltype(Peers)::value_type); }
	[[nodiscard]] std::uint64_t GlobalCredit() const { return Global.Bytes; }
	[[nodiscard]] std::uint64_t Debt(ConnectionId Id) const { auto Found = Peers.find(Id); return Found == Peers.end() ? 0 : Found->second.Debt; }
	[[nodiscard]] std::uint64_t DebtToken(ConnectionId Id) const { auto Found = Peers.find(Id); return Found == Peers.end() ? 0 : Found->second.DebtToken; }
private:
	static constexpr std::uint64_t MinimumFrameBytes = 36 + ReliableServiceEnvelopeBytes;
	struct Bucket { std::uint64_t Bytes = 0, Remainder = 0, Updated = 0; };
	struct Peer {
		Bucket Credit;
		std::optional<std::uint64_t> Exposure, WaitSince;
		std::uint64_t Required = MinimumFrameBytes, DemandStep = 0;
		bool Seen = false, Backlogged = false;
		std::optional<ServiceObservation> Service;
		std::uint64_t Debt = 0, DebtToken = 0, NextQualification = 0;
		bool OwnsGrant = false;
	};
	ReliableServiceProfile Profile;
	std::map<ConnectionId, Peer> Peers;
	Bucket Global;
	Metrics Totals;
	std::optional<ConnectionId> GlobalWait;
	std::optional<Reservation> Active;
	std::optional<std::uint64_t> OrdinaryFunding;
	std::uint64_t Now = 0, Step = 0, NextToken = 0, AggregateExposure = 0;
	std::size_t Unobserved = 0;
	bool Initialized = false, FeedbackComplete = false, GlobalBacklogged = false;
	static void Add(std::uint64_t &Value, std::uint64_t Amount) { Value += std::min(Amount, std::numeric_limits<std::uint64_t>::max() - Value); }
	void ReleaseWait(ConnectionId Id) { if (GlobalWait == Id) GlobalWait.reset(); }
	std::uint64_t PeerCap() const { return Profile.IsPooled() ? Profile.Pooled.PeerBurstCap : Profile.PeerBurst; }
	std::uint64_t GlobalCap() const { return Profile.IsPooled() ? Profile.Pooled.GlobalBurstCap : Profile.GlobalBurst; }
	std::uint64_t FundedRoom() const {
		if (!OrdinaryFunding) return 0;
		const auto &P = Profile.Pooled;
		const auto Window = P.QueueWindowMicroseconds;
		const auto Funded = P.BackendCap * Window / 1'000'000;
		const auto Transport = P.RequiredTransportReserve * Window / 1'000'000;
		// Ordinary exposure is already an upper bound of the two independently
		// floored aggregate ledgers, computed by GameSession for the whole server.
		const auto Ordinary = std::max(*OrdinaryFunding, P.GameplayBurst + P.ControlBurst);
		if (Ordinary > Funded || Transport > Funded - Ordinary || Totals.OutstandingBytes > Funded - Ordinary - Transport) return 0;
		return Funded - Ordinary - Transport - Totals.OutstandingBytes;
	}
	std::uint64_t Room(std::uint64_t Exposure, std::uint64_t High, bool &Latched) {
		if (Exposure > High) Latched = true;
		if (Latched && Exposure <= High - std::min(High / 4, MaximumReliableServiceGroupBytes / 4)) Latched = false;
		return Latched ? 0 : High - std::min(High, Exposure);
	}
	bool Advance(std::uint64_t Time) {
		if (Initialized && Time < Now) return false;
		Now = Time;
		if (!Initialized) { Initialized = true; Global.Updated = Now; }
		Refill(Global, Profile.IsPooled() ? Profile.Pooled.GlobalCreditRate : Profile.GlobalStructuralRate(), GlobalCap());
		Totals.GlobalCreditHighWater = std::max(Totals.GlobalCreditHighWater, Global.Bytes);
		return true;
	}
	void Refill(Bucket &Value, std::uint64_t Rate, std::uint64_t Cap) {
		const auto Elapsed = Now - Value.Updated; Value.Updated = Now;
		const auto Remaining = Cap - Value.Bytes;
		const auto Seconds = Elapsed / 1'000'000;
		if (Seconds > Remaining / Rate) { Value.Bytes = Cap; Value.Remainder = 0; return; }
		// Profile bounds prove Rate*999999 + 999999 fits uint64, even globally.
		const auto Fraction = (Elapsed % 1'000'000) * Rate + Value.Remainder;
		const auto Added = Seconds * Rate + Fraction / 1'000'000;
		Value.Bytes += std::min(Remaining, Added);
		Value.Remainder = Value.Bytes == Cap ? 0 : Fraction % 1'000'000;
	}
};
}
