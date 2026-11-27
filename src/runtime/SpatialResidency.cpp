#include "gargantuan/runtime/SpatialResidency.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Script.hpp"
#include "gargantuan/classes/SpatialRegion.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace gargantuan {
	namespace {
		constexpr std::size_t MaximumHolders = 64, MaximumPerAudience = 8, MaximumRequiredOwners = 64,
							  MaximumCapturesPerPump = 4;
		constexpr auto MaximumDuration = std::chrono::seconds(60);
		bool ValidDuration(std::chrono::milliseconds Duration) {
			return Duration.count() > 0 && Duration <= MaximumDuration;
		}
		bool ValidPriority(SpatialDemandPriority Priority) {
			return Priority <= SpatialDemandPriority::Interactive;
		}
		bool ValidArea(const SpatialDestinationBounds &Bounds) {
			for (unsigned I = 0; I < 3; ++I)
				if (!std::isfinite(Bounds.Minimum[I]) || !std::isfinite(Bounds.Maximum[I]) ||
					Bounds.Minimum[I] >= Bounds.Maximum[I])
					return false;
			return true;
		}
		std::optional<std::vector<ObjectId>> CanonicalOwners(std::span<const ObjectId> Required) {
			if (Required.size() > MaximumRequiredOwners) return {};
			std::vector<ObjectId> Owners(Required.begin(), Required.end());
			std::ranges::sort(Owners);
			if (std::ranges::any_of(Owners, [](ObjectId Id) { return !Id.IsValid(); }) ||
				std::adjacent_find(Owners.begin(), Owners.end()) != Owners.end())
				return {};
			return Owners;
		}
		struct Demand {
			SpatialDestinationBounds Bounds;
			std::vector<ObjectId> Required;
			std::shared_ptr<const SpatialDestinationReceipt> Receipt;
			SpatialCollisionCondition Condition{SpatialCollisionCondition::Pending};
			std::uint64_t Revision{}, LastCapture{};
		};
		bool CurrentIssuer(const Instance &Owner, std::uint64_t Epoch) {
			const auto *ScriptOwner = dynamic_cast<const Script *>(&Owner);
			return !ScriptOwner || ScriptOwner->IsCurrentExecution(Epoch);
		}
		void Main() {
			if (GetCurrentExecutionDomain() != ExecutionDomain::Main)
				throw std::logic_error("[Spatial:Residency] Main owns collision requests");
		}
	}
	struct SpatialCollisionLease::State {
		std::weak_ptr<WorldRoot> World;
		std::weak_ptr<Instance> Owner;
		ObjectId Realm;
		SpatialResidencyAudience Audience;
		std::uint64_t Activation{};
		SpatialDemandPriority Priority{};
		SpatialLeaseLife Life{SpatialLeaseLife::Active};
		SpatialResidencyClock::time_point Deadline;
		std::shared_ptr<Demand> Request;
		std::uint64_t Revision{1}, Observed{};
		void Close(SpatialLeaseLife Reason) {
			if (Life != SpatialLeaseLife::Active) return;
			++Revision;
			Life = Reason;
			Request.reset();
		}
		SpatialCollisionSnapshot Inspect(SpatialResidencyClock::time_point Now) {
			if (Life == SpatialLeaseLife::Active) {
				const auto Target = World.lock();
				const auto Issuer = Owner.lock();
				if (!Target || !Issuer || Target->GetDestroyed() || Target->IsDestroying() || Issuer->GetDestroyed() ||
					Issuer->IsDestroying() || Target->GetSpatialActivation() != Activation ||
					Target->GetReplicationScopeId() != Realm || Issuer->GetReplicationScopeId() != Realm ||
					!CurrentIssuer(*Issuer, Audience.Epoch) ||
					(!dynamic_cast<DataModel *>(Issuer.get()) && !SpatialRegion::FindAttachedWorld(*Issuer)))
					Close(SpatialLeaseLife::Stale);
				else if (Now >= Deadline)
					Close(SpatialLeaseLife::Expired);
			}
			if (Request && Request->Revision != Observed) {
				Observed = Request->Revision;
				++Revision;
			}
			return {Life, Request ? Request->Condition : SpatialCollisionCondition::Pending, Revision};
		}
	};
	struct SpatialCollisionQueue {
		struct AudienceEpoch {
			std::weak_ptr<Instance> Owner;
			ObjectId Id;
			std::uint64_t Epoch{};
			bool Retired{};
		};
		std::vector<AudienceEpoch> Audiences;
		std::vector<std::weak_ptr<SpatialCollisionLease::State>> Holders;
		bool Pumping{};
		std::uint64_t Sequence{};
		std::shared_ptr<Demand>
		FindDemand(const SpatialDestinationBounds &Bounds, std::vector<ObjectId> Required) const {
			for (const auto &Weak : Holders)
				if (const auto Holder = Weak.lock(); Holder && Holder->Request && Holder->Request->Bounds == Bounds &&
													 Holder->Request->Required == Required)
					return Holder->Request;
			auto Result = std::make_shared<Demand>();
			Result->Bounds = Bounds;
			Result->Required = std::move(Required);
			Result->LastCapture = Sequence;
			return Result;
		}
		void Sweep(SpatialResidencyClock::time_point Now) {
			std::erase_if(Audiences, [](const auto &Entry) {
				const auto Owner = Entry.Owner.lock();
				return !Owner || Owner->GetDestroyed() || Owner->IsDestroying();
			});
			std::erase_if(Holders, [&](const auto &Weak) {
				const auto Holder = Weak.lock();
				return !Holder || Holder->Inspect(Now).Life != SpatialLeaseLife::Active;
			});
		}
	};
	SpatialCollisionLease::SpatialCollisionLease(std::shared_ptr<State> Value) : Holder(std::move(Value)) {}
	SpatialCollisionLease::~SpatialCollisionLease() = default;
	SpatialCollisionSnapshot SpatialCollisionLease::GetStatus(SpatialResidencyClock::time_point Now) const {
		Main();
		return Holder->Inspect(Now);
	}
	std::shared_ptr<const SpatialDestinationReceipt>
	SpatialCollisionLease::GetReceipt(SpatialResidencyClock::time_point Now) const {
		return GetStatus(Now).Collision == SpatialCollisionCondition::Ready ? Holder->Request->Receipt : nullptr;
	}
	bool SpatialCollisionLease::Renew(SpatialResidencyClock::time_point Now, std::chrono::milliseconds Duration) {
		if (!ValidDuration(Duration) || GetStatus(Now).Life != SpatialLeaseLife::Active ||
			Now > SpatialResidencyClock::time_point::max() - Duration)
			return false;
		Holder->Deadline = Now + Duration;
		return true;
	}
	bool SpatialCollisionLease::SetPriority(SpatialResidencyClock::time_point Now, SpatialDemandPriority Priority) {
		if (!ValidPriority(Priority) || GetStatus(Now).Life != SpatialLeaseLife::Active) return false;
		Holder->Priority = Priority;
		return true;
	}
	bool SpatialCollisionLease::SetArea(
		SpatialResidencyClock::time_point Now,
		const SpatialDestinationBounds &Bounds,
		std::span<const ObjectId> RequiredOwners
	) {
		if (GetStatus(Now).Life != SpatialLeaseLife::Active || !ValidArea(Bounds)) return false;
		auto Owners = CanonicalOwners(RequiredOwners);
		if (!Owners) return false;
		const auto World = Holder->World.lock();
		const auto Queue = World ? World->CollisionDemand : nullptr;
		if (!Queue) return false;
		auto Next = Queue->FindDemand(Bounds, std::move(*Owners));
		if (Next != Holder->Request) {
			Holder->Request = std::move(Next);
			Holder->Observed = Holder->Request->Revision;
			++Holder->Revision;
		}
		return true;
	}
	void SpatialCollisionLease::Release() {
		Main();
		Holder->Close(SpatialLeaseLife::Released);
	}

	std::expected<std::shared_ptr<SpatialCollisionLease>, SpatialRequestFailure> WorldRoot::RequestSpatialCollision(
		SpatialResidencyAudience Audience,
		const SpatialDestinationBounds &Bounds,
		std::span<const ObjectId> RequiredOwners,
		SpatialDemandPriority Priority,
		std::chrono::milliseconds Duration,
		SpatialResidencyClock::time_point Now
	) {
		using enum SpatialRequestFailure;
		if (GetCurrentExecutionDomain() != ExecutionDomain::Main || !Audience.Owner.IsValid() || !Audience.Epoch ||
			!ValidArea(Bounds) || !ValidDuration(Duration) ||
			Now > SpatialResidencyClock::time_point::max() - Duration || !ValidPriority(Priority) ||
			RequiredOwners.size() > MaximumRequiredOwners)
			return std::unexpected(Invalid);
		const auto Realm = SpatialRegion::FindAttachedWorld(*this);
		const auto Issuer = ObjectRegistry::Get().Lookup(Audience.Owner);
		if (!Realm || !SpatialActivation || !Issuer || Issuer->GetDestroyed() || Issuer->IsDestroying() ||
			Issuer->GetReplicationScopeId() != Realm->GetObjectId() || !CurrentIssuer(*Issuer, Audience.Epoch) ||
			(Issuer != Realm && !SpatialRegion::FindAttachedWorld(*Issuer)))
			return std::unexpected(Unavailable);
		auto Owners = CanonicalOwners(RequiredOwners);
		if (!Owners) return std::unexpected(Invalid);
		if (!CollisionDemand) CollisionDemand = std::make_shared<SpatialCollisionQueue>();
		auto &Queue = *CollisionDemand;
		Queue.Sweep(Now);
		const auto Saved = std::ranges::find(
			Queue.Audiences, Audience.Owner, &SpatialCollisionQueue::AudienceEpoch::Id
		);
		if (Saved != Queue.Audiences.end() &&
			(Audience.Epoch < Saved->Epoch || (Audience.Epoch == Saved->Epoch && Saved->Retired)))
			return std::unexpected(Stale);
		if (Saved == Queue.Audiences.end() && Queue.Audiences.size() >= 128) return std::unexpected(Unavailable);
		std::size_t AudienceHolders{};
		for (const auto &Weak : Queue.Holders)
			if (const auto Holder = Weak.lock()) {
				if (Holder->Audience.Owner == Audience.Owner) {
					if (Holder->Audience.Epoch != Audience.Epoch) return std::unexpected(Stale);
					++AudienceHolders;
				}
			}
		if (AudienceHolders >= MaximumPerAudience) return std::unexpected(Budget);
		if (Queue.Holders.size() >= MaximumHolders) return std::unexpected(Unavailable);
		auto Shared = Queue.FindDemand(Bounds, std::move(*Owners));
		auto Holder = std::make_shared<SpatialCollisionLease::State>();
		Holder->World = std::static_pointer_cast<WorldRoot>(shared_from_this());
		Holder->Owner = Issuer;
		Holder->Realm = Realm->GetObjectId();
		Holder->Audience = Audience;
		Holder->Activation = SpatialActivation;
		Holder->Priority = Priority;
		Holder->Deadline = Now + Duration;
		Holder->Request = std::move(Shared);
		if (Saved == Queue.Audiences.end())
			Queue.Audiences.push_back({Issuer, Audience.Owner, Audience.Epoch, false});
		else {
			Saved->Epoch = Audience.Epoch;
			Saved->Retired = false;
		}
		Queue.Holders.push_back(Holder);
		SpatialDemandScheduled = true;
		return std::shared_ptr<SpatialCollisionLease>(new SpatialCollisionLease(std::move(Holder)));
	}
	SpatialDemandStatistics WorldRoot::GetSpatialDemandStatistics(SpatialResidencyClock::time_point Now) const {
		Main();
		SpatialDemandStatistics Result;
		if (!CollisionDemand) return Result;
		for (const auto &Entry : CollisionDemand->Holders) {
			const auto Holder = Entry.lock();
			if (!Holder) continue;
			const auto Status = Holder->Inspect(Now);
			if (Status.Life != SpatialLeaseLife::Active) continue;
			switch (Status.Collision) {
			case SpatialCollisionCondition::Pending: ++Result.Pending; break;
			case SpatialCollisionCondition::Ready: ++Result.Ready; break;
			case SpatialCollisionCondition::Unavailable: ++Result.Unavailable; break;
			}
		}
		return Result;
	}

	SpatialCollisionWork
	WorldRoot::PumpSpatialCollision(SpatialResidencyClock::time_point Now, std::size_t MaximumCaptures) {
		Main();
		const auto Queue = CollisionDemand;
		if (!Queue || Queue->Pumping) return {};
		Queue->Sweep(Now);
		if (Queue->Holders.empty()) {
			SpatialDemandScheduled = false;
			return {};
		}
		Queue->Pumping = true;
		struct Guard {
			bool &Pumping;
			~Guard() {
				Pumping = false;
			}
		} Clear{Queue->Pumping};
		struct Candidate {
			std::shared_ptr<Demand> Request;
			SpatialDemandPriority Priority;
		};
		std::vector<Candidate> Requests;
		for (const auto &Weak : Queue->Holders)
			if (const auto Holder = Weak.lock()) {
				const auto It = std::ranges::find(Requests, Holder->Request, &Candidate::Request);
				if (It == Requests.end())
					Requests.push_back({Holder->Request, Holder->Priority});
				else
					It->Priority = std::max(It->Priority, Holder->Priority);
			}
		// Oldest request goes first. Highest surviving priority breaks equal-age ties.
		std::stable_sort(Requests.begin(), Requests.end(), [](const Candidate &A, const Candidate &B) {
			if (A.Request->LastCapture != B.Request->LastCapture)
				return A.Request->LastCapture < B.Request->LastCapture;
			return A.Priority > B.Priority;
		});
		SpatialCollisionWork Work{Queue->Holders.size(), Requests.size(), 0};
		const auto Limit = std::min({MaximumCaptures, MaximumCapturesPerPump, Requests.size()});
		for (std::size_t I = 0; I < Limit; ++I) {
			auto &Request = *Requests[I].Request;
			const auto Authority = GetSpatialActivation();
			auto Prepared = PrepareSpatialDestination(Request.Bounds, Request.Required);
			++Work.Captures;
			if (CollisionDemand != Queue || GetSpatialActivation() != Authority || GetDestroyed() || IsDestroying())
				return Work;
			const auto Condition = Prepared ? SpatialCollisionCondition::Ready : SpatialCollisionCondition::Unavailable;
			if (Request.Condition != Condition || (Prepared && (!Request.Receipt || *Request.Receipt != *Prepared))) {
				++Request.Revision;
				Request.Condition = Condition;
				Request.Receipt = Prepared ? std::make_shared<const SpatialDestinationReceipt>(std::move(*Prepared))
										   : nullptr;
			}
			Request.LastCapture = ++Queue->Sequence;
		}
		return Work;
	}
	void WorldRoot::RetireSpatialAudience(SpatialResidencyAudience Audience) {
		Main();
		if (!CollisionDemand) return;
		const auto Entry = std::ranges::find(
			CollisionDemand->Audiences, Audience.Owner, &SpatialCollisionQueue::AudienceEpoch::Id
		);
		if (Entry == CollisionDemand->Audiences.end() || Entry->Epoch != Audience.Epoch) return;
		Entry->Retired = true;
		for (const auto &Weak : CollisionDemand->Holders)
			if (const auto Holder = Weak.lock(); Holder && Holder->Audience == Audience)
				Holder->Close(SpatialLeaseLife::Stale);
	}
	void WorldRoot::RetireSpatialCollision() {
		SpatialDemandScheduled = false;
		const auto Queue = std::move(CollisionDemand);
		if (!Queue) return;
		for (const auto &Weak : Queue->Holders)
			if (const auto Holder = Weak.lock()) Holder->Close(SpatialLeaseLife::Stale);
	}
}
