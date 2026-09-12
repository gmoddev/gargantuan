#pragma once

// Private implementation included inside the coordinator namespace, after its
// schema/dependency helpers. No task, Instance or Lua callback survives a yield.
struct ReplicationCoordinator::PlanningContinuation {
	struct Routine {
		struct promise_type {
			std::exception_ptr Failure;
			Routine get_return_object() { return {std::coroutine_handle<promise_type>::from_promise(*this)}; }
			std::suspend_always initial_suspend() noexcept { return {}; }
			std::suspend_always final_suspend() noexcept { return {}; }
			std::suspend_always yield_value(int) noexcept { return {}; }
			void return_void() noexcept {}
			void unhandled_exception() noexcept { Failure = std::current_exception(); }
		};
		std::coroutine_handle<promise_type> Handle;
		Routine(std::coroutine_handle<promise_type> Value = {}) : Handle(Value) {}
		Routine(Routine &&Other) noexcept : Handle(std::exchange(Other.Handle, {})) {}
		Routine &operator=(Routine &&Other) noexcept {
			if (Handle) Handle.destroy();
			Handle = std::exchange(Other.Handle, {});
			return *this;
		}
		~Routine() { if (Handle) Handle.destroy(); }
		bool Resume() {
			Handle.resume();
			if (Handle.promise().Failure) std::rethrow_exception(Handle.promise().Failure);
			return !Handle.done();
		}
	};
	struct ReferenceFixup {
		ObjectId Referrer;
		const InstanceProperty *Property = nullptr; // Frozen schema, not a live Instance.
		ObjectId CurrentTarget;
	};
	struct Dependency { ObjectId Object; std::size_t Depth; };
	PeerState Working;
	std::shared_ptr<const PeerRelevanceSelection> Selection;
	ChangeCursor Revision;
	std::uint64_t AcceptedRevision = 0;
	std::size_t Records = 0;
	std::size_t Cost = 0;
	std::size_t CriticalTransitions = 0;
	std::size_t LargestGroup = 1;
	bool Ready = false;
	bool Installed = false;
	bool Disposing = false;
	bool Baseline = false;
	bool AwaitingSelection = false;
	std::deque<Dependency> Frontier;
	std::set<ObjectId> SeedsRequired;
	std::set<ObjectId> Entering, Leaving, Group;
	std::deque<ObjectId> GroupFrontier;
	std::map<ObjectId, std::size_t> EnterCosts, LeaveCosts;
	std::map<ObjectId, ReparentReplication> ParentMoves;
	std::map<ObjectId, std::vector<ObjectId>> EnterParentFixups, LeaveParentFixups;
	std::map<ObjectId, std::vector<ReferenceFixup>> RemovalReferences;
	std::vector<ReferenceFixup> Clears, Restores;
	Routine Work;
	static bool SameCursor(ChangeCursor Left, ChangeCursor Right) {
		return Left.Scope == Right.Scope && Left.NextSequence == Right.NextSequence;
	}

	void Retain(ReplicationCoordinator &Owner, std::size_t Count = 1) {
		if (Count > MaximumPlanningRecordsPerPeer - Records || Count > MaximumPlanningRecords - Owner.PlanningRecordCount)
			throw std::runtime_error("Structural planning continuation record limit exceeded");
		Records += Count;
		Owner.PlanningRecordCount += Count;
		Owner.Metrics.PlanningRecords = Owner.PlanningRecordCount;
		Owner.Metrics.PlanningRecordsHighWater = std::max<std::uint64_t>(Owner.Metrics.PlanningRecordsHighWater, Owner.PlanningRecordCount);
		Owner.Metrics.PlanningPeerRecordsHighWater = std::max<std::uint64_t>(Owner.Metrics.PlanningPeerRecordsHighWater, Records);
		runtime_detail::MaximumWork(runtime_detail::WorkCounter::PlanningRecordsHighWater, Owner.PlanningRecordCount);
	}
	bool Valid(const ReplicationCoordinator &Owner, const PeerState &Peer) const {
		return Selection == Peer.PlanningSelection && SameCursor(Revision, Owner.PlanningCursor) &&
			AcceptedRevision == Peer.AcceptedRevision && Peer.AcceptedRevision != std::numeric_limits<std::uint64_t>::max();
	}
	void AddPending(ReplicationCoordinator &Owner, const PeerState &Peer, ObjectId Object,
		PendingTransitionKind Kind, bool Critical, std::uint64_t Tick) {
		if (Working.PendingTransitions.size() >= Owner.Configuration.MaximumPendingTransitionsPerPeer)
			throw std::runtime_error("Peer structural pending transition limit exceeded");
		if (Working.NextPendingToken == std::numeric_limits<std::uint64_t>::max())
			throw std::runtime_error("Structural pending transition token is exhausted");
		const auto Old = Peer.PendingTransitions.find(Object);
		const auto Since = Old == Peer.PendingTransitions.end() ? Tick : Old->second.PendingSinceTick;
		Retain(Owner, 2);
		const PendingTransition Value{Kind, Since, Working.NextPendingToken++, Critical};
		Working.PendingTransitions.emplace(Object, Value);
		if (Critical) ++CriticalTransitions;
		(Critical ? Working.CriticalQueue : Working.OrdinaryQueue).push_back({Object, Value.Token});
		runtime_detail::CountWork(runtime_detail::WorkCounter::CandidateAttempts);
		runtime_detail::CountWork(Old == Peer.PendingTransitions.end() ? runtime_detail::WorkCounter::CandidateInserted : runtime_detail::WorkCounter::AlreadyPending);
	}
	void EnqueueGroup(ReplicationCoordinator &Owner, ObjectId Object, PendingTransitionKind Kind) {
		const auto Pending = Working.PendingTransitions.find(Object);
		if (Pending == Working.PendingTransitions.end() || Pending->second.Kind != Kind || Group.contains(Object)) return;
		if (Group.size() == std::min(Owner.Configuration.PeerQuantum, MaximumRelevanceTransitionsPerFrame))
			throw std::runtime_error("Atomic replication dependency group exceeds the transition work limit");
		// The reusable group/frontier capacity is reserved once, independently of
		// the number of candidate attempts; clearing it is also charged below.
		Group.insert(Object);
		GroupFrontier.push_back(Object);
	}
	Routine Run(ReplicationCoordinator &Owner, PeerState &Peer, std::uint64_t Tick) {
		Working.View.Connection = Peer.View.Connection;
		Working.View.Epoch = Peer.View.Epoch;
		Working.NextPendingToken = Peer.NextPendingToken;
		Baseline = Peer.View.KnownObjects.empty();
		Retain(Owner, Peer.DesiredObjects.size() + Peer.RequiredObjects.size() + 2 * Owner.Configuration.PeerQuantum +
			Peer.PendingTransitions.size() + Peer.CriticalQueue.size() + Peer.OrdinaryQueue.size() + Peer.LeavingDependents.size());
		for (const auto &[Object, Dependents] : Peer.LeavingDependents) {
			co_yield 0;
			(void)Object;
			for (const auto Dependent : Dependents) { co_yield 0; (void)Dependent; Retain(Owner); }
		}
		// Accepted state is read in place. The owner validates AcceptedRevision
		// before every service slice and cancels this coroutine before peer erase.
		// Reference vectors are indexed afresh after each yield: a journal commit
		// may replace storage without changing its dependency/reference meaning.
		if (Peer.AcceptedParents.size() != Peer.View.KnownObjects.size())
			throw std::runtime_error("Known structural object has no accepted ancestry metadata");
		const bool ReuseClosure = Selection == Peer.ResolvedSelection && SameCursor(Peer.DesiredDependencyCursor, Owner.DependencyCursor);
		if (ReuseClosure) {
			SaturatingAdd(Owner.Metrics.DependencyPlanCacheHits, 1);
			for (const auto Object : Peer.DesiredObjects) { co_yield 0; Retain(Owner); Working.DesiredObjects.insert(Object); }
			for (const auto Object : Peer.RequiredObjects) { co_yield 0; Retain(Owner); Working.RequiredObjects.insert(Object); }
		} else {
			SaturatingAdd(Owner.Metrics.DependencyPlanRebuilds, 1);
			for (const auto Object : Selection->RequiredObjects) { co_yield 0; Retain(Owner); SeedsRequired.insert(Object); }
			for (int Pass = 0; Pass != 2; ++Pass) {
				auto &Closure = Pass == 0 ? Working.DesiredObjects : Working.RequiredObjects;
				const auto &Seeds = Pass == 0 ? Selection->DesiredObjects : Selection->RequiredObjects;
				for (const auto Object : Seeds) { co_yield 0; Retain(Owner); Frontier.push_back({Object, 0}); runtime_detail::CountWork(runtime_detail::WorkCounter::DependencySeeds); }
				for (const auto Object : Selection->RequiredObjects) { co_yield 0; Retain(Owner); Frontier.push_back({Object, 0}); runtime_detail::CountWork(runtime_detail::WorkCounter::DependencySeeds); }
				if (Pass == 1 && Frontier.empty()) { Retain(Owner); Frontier.push_back({Owner.SourceRoot->GetObjectId(), 0}); }
				while (!Frontier.empty()) {
					co_yield 0;
					const auto [Object, Depth] = Frontier.front();
					Frontier.pop_front();
					if (!Object.IsValid() || Closure.contains(Object)) { runtime_detail::CountWork(runtime_detail::WorkCounter::DependencyDuplicates); continue; }
					auto Found = Owner.Catalog.find(Object);
					if (Found == Owner.Catalog.end() || Owner.RetiredObjects.contains(Object)) {
						if (SeedsRequired.contains(Object)) {
							// 3E peer evaluation and planning have independent rotating
							// service points. A destroyed required Character can remain in
							// an older immutable selection until 3E revisits this peer.
							// Nothing from that incomplete projection is publishable.
							AwaitingSelection = true;
							co_return;
						}
						continue;
					}
					if (Depth > MaximumDependencyClosureDepth || Closure.size() == MaximumPeerDesiredObjects)
						throw std::runtime_error("Replication dependency closure limit exceeded");
					Retain(Owner);
					Closure.insert(Object);
					if (Found->second->Publication.Parent) {
						Retain(Owner); Frontier.push_back({*Found->second->Publication.Parent, Depth + 1});
						runtime_detail::CountWork(runtime_detail::WorkCounter::DependencyEdges);
					}
					for (std::size_t Index = 0;; ++Index) {
						co_yield 0;
						// No borrowed property node/iterator is used across a yield.
						const auto &Entry = Owner.Catalog.at(Object);
						if (Index == Entry.ReferenceProperties.size()) break;
						const auto &[Name, Value] = *Entry.ReferenceProperties[Index];
						const auto *Reference = std::get_if<WireObjectReference>(&Value);
						if (Reference && IsHardReference(Entry->Publication, Name)) {
							Retain(Owner); Frontier.push_back({Reference->Object.ToObjectId(), Depth + 1});
							runtime_detail::CountWork(runtime_detail::WorkCounter::DependencyEdges);
						}
					}
				}
			}
		}
		runtime_detail::CountWork(runtime_detail::WorkCounter::DiscoveryPeers);
		for (const auto Object : Working.DesiredObjects) {
			co_yield 0;
			runtime_detail::CountWork(runtime_detail::WorkCounter::DesiredExamined);
			if (Peer.View.Knows(Object)) {
				Retain(Owner); Working.View.RelevantObjects.insert(Object);
				runtime_detail::CountWork(runtime_detail::WorkCounter::AlreadyKnown);
			} else if (!Owner.RetiredObjects.contains(Object))
				AddPending(Owner, Peer, Object, PendingTransitionKind::Enter, Working.RequiredObjects.contains(Object), Tick);
		}
		for (const auto &[Object, Accepted] : Peer.AcceptedParents) {
			co_yield 0;
			runtime_detail::CountWork(runtime_detail::WorkCounter::KnownExamined);
			if (!Working.DesiredObjects.contains(Object))
				AddPending(Owner, Peer, Object, PendingTransitionKind::Leave, Owner.RetiredObjects.contains(Object), Tick);
		}
		for (const auto &[Object, Transition] : Working.PendingTransitions) {
			co_yield 0;
			runtime_detail::CountWork(runtime_detail::WorkCounter::PendingExamined);
			if (Transition.Kind != PendingTransitionKind::Leave) continue;
			const auto Parent = Peer.AcceptedParents.at(Object).Parent;
			auto AddReverse = [&](ObjectId Target) {
				const auto Pending = Working.PendingTransitions.find(Target);
				if (Pending != Working.PendingTransitions.end() && Pending->second.Kind == PendingTransitionKind::Leave) {
					Retain(Owner, 2); Working.LeavingDependents[Target].push_back(Object);
				}
			};
			AddReverse(Parent);
			for (std::size_t Index = 0;; ++Index) {
				co_yield 0;
				const auto &Entry = Owner.Catalog.at(Object);
				if (Index == Entry.ReferenceProperties.size()) break;
				const auto &[Name, Value] = *Entry.ReferenceProperties[Index];
				if (IsHardReference(Entry->Publication, Name)) AddReverse(std::get<WireObjectReference>(Value).Object.ToObjectId());
			}
		}
		if (!Baseline) {
			for (const auto Referrer : Working.DesiredObjects) {
				co_yield 0;
				runtime_detail::CountWork(runtime_detail::WorkCounter::FixupReferrers);
				if (!Peer.View.Knows(Referrer)) continue;
				const auto Parent = Owner.Catalog.at(Referrer)->Publication.Parent;
				const auto AcceptedParent = Peer.AcceptedParents.at(Referrer).Parent;
				if (AcceptedParent != Parent.value_or(ObjectId{})) {
					Retain(Owner); ParentMoves.emplace(Referrer, ReparentReplication{Referrer, Parent});
					if (Parent && !Peer.View.Knows(*Parent)) {
						Retain(Owner, 3); ++EnterCosts[*Parent]; EnterParentFixups[*Parent].push_back(Referrer);
					}
					if (AcceptedParent.IsValid()) {
						Retain(Owner, 3); ++LeaveCosts[AcceptedParent]; LeaveParentFixups[AcceptedParent].push_back(Referrer);
					}
				}
				for (std::size_t Index = 0;; ++Index) {
					co_yield 0;
					const auto &Entry = Owner.Catalog.at(Referrer);
					if (Index == Entry.ReferenceProperties.size()) break;
					const auto &[Name, Value] = *Entry.ReferenceProperties[Index];
					(void)Name;
					Retain(Owner); ++EnterCosts[std::get<WireObjectReference>(Value).Object.ToObjectId()];
					runtime_detail::CountWork(runtime_detail::WorkCounter::FixupProperties);
				}
			}
			for (const auto &[Referrer, Accepted] : Peer.AcceptedParents) {
				co_yield 0;
				runtime_detail::CountWork(runtime_detail::WorkCounter::AcceptedReferenceObjectsExamined);
				for (std::size_t Index = 0;; ++Index) {
					co_yield 0;
					if (Index == Accepted.References.size()) break;
					const auto [Property, Target] = Accepted.References[Index];
					runtime_detail::CountWork(runtime_detail::WorkCounter::AcceptedReferencesExamined);
					const auto Pending = Working.PendingTransitions.find(Target);
					if (Pending == Working.PendingTransitions.end() || Pending->second.Kind != PendingTransitionKind::Leave) continue;
					const auto &Value = Owner.Catalog.at(Referrer)->Publication.Properties.at(Property->Name);
					const auto *Reference = std::get_if<WireObjectReference>(&Value);
					Retain(Owner, 2);
					RemovalReferences[Target].push_back({Referrer, Property, Reference ? Reference->Object.ToObjectId() : ObjectId{}});
					runtime_detail::CountWork(runtime_detail::WorkCounter::RemovalReferencesPlanned);
				}
			}
		}
		if (Baseline && Working.CriticalQueue.size() > Owner.Configuration.PeerQuantum)
			throw std::runtime_error("Critical structural bootstrap exceeds its bounded transition quantum");
		const auto Limit = Peer.PlanningFrameLimit ? Peer.PlanningFrameLimit : Owner.Configuration.PeerQuantum;
		for (int Pass = 0; Pass != (Baseline && Working.PendingTransitions.size() > Limit ? 1 : 2); ++Pass) {
			const auto &Queue = Pass == 0 ? Working.CriticalQueue : Working.OrdinaryQueue;
			for (const auto Candidate : Queue) {
				co_yield 0;
				if (Cost == Limit) break;
				const auto Kind = Working.PendingTransitions.at(Candidate.Object).Kind;
				auto &Selected = Kind == PendingTransitionKind::Enter ? Entering : Leaving;
				if (Selected.contains(Candidate.Object)) continue;
				while (!Group.empty()) { co_yield 0; Group.erase(Group.begin()); }
				EnqueueGroup(Owner, Candidate.Object, Kind);
				runtime_detail::CountWork(runtime_detail::WorkCounter::DependencyGroupsBuilt);
				while (!GroupFrontier.empty()) {
					co_yield 0;
					const auto Object = GroupFrontier.front(); GroupFrontier.pop_front();
					if (Kind == PendingTransitionKind::Leave) {
						if (auto Found = Working.LeavingDependents.find(Object); Found != Working.LeavingDependents.end())
							for (const auto Dependent : Found->second) { co_yield 0; EnqueueGroup(Owner, Dependent, Kind); }
						if (auto Found = RemovalReferences.find(Object); Found != RemovalReferences.end())
							for (const auto &Reference : Found->second) {
								co_yield 0;
								if (!Reference.Property->Nullable || Reference.Property->MaterializationDependencyPolicy == InstanceProperty::MaterializationDependency::Hard)
									EnqueueGroup(Owner, Reference.Referrer, Kind);
							}
					} else {
						const auto Parent = Owner.Catalog.at(Object)->Publication.Parent;
						if (Parent && !Peer.View.Knows(*Parent)) EnqueueGroup(Owner, *Parent, Kind);
						for (std::size_t Index = 0;; ++Index) {
							co_yield 0;
							const auto &Entry = Owner.Catalog.at(Object);
							if (Index == Entry.ReferenceProperties.size()) break;
							const auto &[Name, Value] = *Entry.ReferenceProperties[Index];
							const auto Target = std::get<WireObjectReference>(Value).Object.ToObjectId();
							if (IsHardReference(Entry->Publication, Name) && !Peer.View.Knows(Target)) EnqueueGroup(Owner, Target, Kind);
						}
					}
				}
				bool Blocked = false;
				std::size_t NewWork = 0, Disappearing = 0;
				for (const auto Object : Group) {
					co_yield 0;
					if (Selected.contains(Object)) continue;
					++NewWork;
					const auto &Costs = Kind == PendingTransitionKind::Enter ? EnterCosts : LeaveCosts;
					if (auto Found = Costs.find(Object); Found != Costs.end()) NewWork += Found->second;
					if (Kind != PendingTransitionKind::Leave) continue;
					if (auto Moves = LeaveParentFixups.find(Object); Moves != LeaveParentFixups.end())
						for (const auto Child : Moves->second) {
							co_yield 0;
							const auto Parent = ParentMoves.at(Child).Parent;
							if (Parent && !Peer.View.Knows(*Parent) && !Entering.contains(*Parent)) Blocked = true;
						}
					if (auto Found = RemovalReferences.find(Object); Found != RemovalReferences.end())
						for (const auto &Reference : Found->second) {
							co_yield 0;
							if (Leaving.contains(Reference.Referrer) || Group.contains(Reference.Referrer)) continue;
							++NewWork;
							const auto ReferrerLeave = Working.PendingTransitions.find(Reference.Referrer);
							if (ReferrerLeave != Working.PendingTransitions.end() && ReferrerLeave->second.Kind == PendingTransitionKind::Leave) ++Disappearing;
							const bool Hard = !Reference.Property->Nullable || Reference.Property->MaterializationDependencyPolicy == InstanceProperty::MaterializationDependency::Hard;
							if (Hard && Reference.CurrentTarget.IsValid() &&
								((!Peer.View.Knows(Reference.CurrentTarget) && !Entering.contains(Reference.CurrentTarget)) || Leaving.contains(Reference.CurrentTarget) || Group.contains(Reference.CurrentTarget))) Blocked = true;
						}
				}
				if (Blocked) continue;
				if (NewWork > Owner.Configuration.PeerQuantum) {
					if (NewWork - Disappearing <= Owner.Configuration.PeerQuantum) continue;
					throw std::runtime_error("Structural dependency group and reference fixups exceed the peer quantum");
				}
				if (NewWork > Limit - Cost) continue;
				LargestGroup = std::max(LargestGroup, NewWork);
				for (const auto Object : Group) { co_yield 0; if (!Selected.contains(Object)) { Retain(Owner); Selected.insert(Object); } }
				Cost += NewWork;
				runtime_detail::CountWork(runtime_detail::WorkCounter::PlanningCompletedGroups);
			}
		}
		for (const auto Target : Leaving) {
			co_yield 0;
			if (auto Found = RemovalReferences.find(Target); Found != RemovalReferences.end())
				for (const auto &Reference : Found->second) {
					co_yield 0;
					if (!Leaving.contains(Reference.Referrer)) { Retain(Owner); Clears.push_back(Reference); }
				}
		}
		if (!Baseline) for (const auto Referrer : Working.DesiredObjects) {
			co_yield 0;
			runtime_detail::CountWork(runtime_detail::WorkCounter::RestoreFixupReferrers);
			if (Entering.contains(Referrer) || !Peer.View.Knows(Referrer)) continue;
			for (std::size_t Index = 0;; ++Index) {
				co_yield 0;
				const auto &Entry = Owner.Catalog.at(Referrer);
				if (Index == Entry.ReferenceProperties.size()) break;
				const auto &[Name, Value] = *Entry.ReferenceProperties[Index];
				runtime_detail::CountWork(runtime_detail::WorkCounter::RestoreFixupProperties);
				const auto Target = std::get<WireObjectReference>(Value).Object.ToObjectId();
				if (Entering.contains(Target)) { Retain(Owner); Restores.push_back({Referrer, FindNativeProperty(Entry->Publication, Name), Target}); }
			}
		}
		Ready = true;
	}
	Routine Dispose() {
		// Pop nested records before destroying their owning node: one giant
		// reverse-reference vector must not turn cancellation into a drain loop.
		for (auto *Map : {&Working.LeavingDependents, &EnterParentFixups, &LeaveParentFixups})
			for (auto &[Object, Values] : *Map) {
				co_yield 0;
				(void)Object;
				while (!Values.empty()) { co_yield 0; Values.pop_back(); }
			}
		for (auto &[Object, Values] : RemovalReferences) {
			co_yield 0;
			(void)Object;
			while (!Values.empty()) { co_yield 0; Values.pop_back(); }
		}
#define GARGANTUAN_DRAIN_PLANNING(Value) while (!(Value).empty()) { co_yield 0; (Value).erase((Value).begin()); }
		GARGANTUAN_DRAIN_PLANNING(Working.View.RelevantObjects)
		GARGANTUAN_DRAIN_PLANNING(Working.DesiredObjects)
		GARGANTUAN_DRAIN_PLANNING(Working.RequiredObjects)
		GARGANTUAN_DRAIN_PLANNING(Working.PendingTransitions)
		GARGANTUAN_DRAIN_PLANNING(Working.CriticalQueue)
		GARGANTUAN_DRAIN_PLANNING(Working.OrdinaryQueue)
		GARGANTUAN_DRAIN_PLANNING(Working.LeavingDependents)
		GARGANTUAN_DRAIN_PLANNING(Frontier)
		GARGANTUAN_DRAIN_PLANNING(SeedsRequired)
		GARGANTUAN_DRAIN_PLANNING(Entering)
		GARGANTUAN_DRAIN_PLANNING(Leaving)
		GARGANTUAN_DRAIN_PLANNING(Group)
		GARGANTUAN_DRAIN_PLANNING(GroupFrontier)
		GARGANTUAN_DRAIN_PLANNING(EnterCosts)
		GARGANTUAN_DRAIN_PLANNING(LeaveCosts)
		GARGANTUAN_DRAIN_PLANNING(ParentMoves)
		GARGANTUAN_DRAIN_PLANNING(EnterParentFixups)
		GARGANTUAN_DRAIN_PLANNING(LeaveParentFixups)
		GARGANTUAN_DRAIN_PLANNING(RemovalReferences)
#undef GARGANTUAN_DRAIN_PLANNING
		while (!Clears.empty()) { co_yield 0; Clears.pop_back(); }
		while (!Restores.empty()) { co_yield 0; Restores.pop_back(); }
	}
};

ReplicationScheduleResult ReplicationCoordinator::RegisterPeerPlanned(ConnectionId Connection, ReplicationEpoch Epoch,
	std::shared_ptr<const PeerRelevanceSelection> Selection) {
	if (!SourceRoot || !Connection.IsValid() || !Epoch.IsValid() || !Selection) return {"Invalid replication peer or source"};
	if (Peers.size() + DetachedPlanning.size() >= 1'024) return {"Structural planning peer limit exceeded"};
	if (Peers.contains(Connection) || DetachedPlanning.contains(Connection)) return {"Replication peer identity is already registered or retiring"};
	for (const auto &[Existing, State] : Peers) {
		(void)State;
		if (Existing.Slot == Connection.Slot) return {"A live replication peer already owns this connection slot"};
	}
	std::string Error;
	if (!RefreshCatalog(Error)) return {std::move(Error)};
	PeerState Peer{{Connection, Epoch}, CatalogCursor, ReliableReplicationSequence(1)};
	Peer.PolicyManaged = true;
	Peer.ExplicitSchedulerCommit = true;
	Peer.Planned = true;
	Peers.emplace(Connection, std::move(Peer));
	auto Result = RequestPlanning(Connection, std::move(Selection), LatestSchedulingTick);
	if (!Result.Succeeded()) RemovePeer(Connection);
	return Result;
}

ReplicationScheduleResult ReplicationCoordinator::RequestPlanning(ConnectionId Connection,
	std::shared_ptr<const PeerRelevanceSelection> Selection, std::uint64_t SimulationTick) {
	auto Found = Peers.find(Connection);
	if (Found == Peers.end() || !Found->second.Planned || !Selection) return {"Invalid structural planning peer"};
	auto &Peer = Found->second;
	if (!Peer.PlanningError.empty()) return {Peer.PlanningError};
	if (Peer.PreparedCommit) return {"A structural frame is awaiting scheduler acceptance"};
	if (Selection->DesiredObjects.size() > MaximumPeerDesiredObjects || Selection->RequiredObjects.size() > MaximumPeerDesiredObjects)
		return {"Replication relevance selection exceeds its object limit"};
	LatestSchedulingTick = std::max(LatestSchedulingTick, SimulationTick);
	std::string Error;
	if (!RefreshCatalog(Error)) return {std::move(Error)};
	const auto InputRecords = Selection->DesiredObjects.size() + Selection->RequiredObjects.size();
	const auto WithoutInput = PlanningRecordCount - Peer.PlanningInputRecords;
	if (InputRecords > MaximumPlanningRecords - WithoutInput) return {"Structural planning input record limit exceeded"};
	PlanningRecordCount = WithoutInput + InputRecords;
	Peer.PlanningInputRecords = InputRecords;
	Metrics.PlanningRecords = PlanningRecordCount;
	Metrics.PlanningRecordsHighWater = std::max<std::uint64_t>(Metrics.PlanningRecordsHighWater, PlanningRecordCount);
	if (Selection != Peer.ResolvedSelection) Peer.ResolvedSelection.reset();
	if (Selection != Peer.PlanningSelection) Peer.PlanningAwaitingSelection = false;
	Peer.PlanningSelection = std::move(Selection);
	if (Peer.Planning || (!Peer.PlanningAwaitingSelection && (Peer.PlanningSelection != Peer.ResolvedSelection || !Peer.PendingTransitions.empty() ||
		!PlanningContinuation::SameCursor(Peer.DesiredDependencyCursor, DependencyCursor)))) {
		if (!PlanningPeers.contains(Connection)) Peer.LastPlanningServiceTick = std::max<std::uint64_t>(1, SimulationTick);
		PlanningPeers.insert(Connection);
	}
	return {};
}

bool ReplicationCoordinator::IsPlanningReady(ConnectionId Connection) const {
	const auto Found = Peers.find(Connection);
	if (Found == Peers.end()) return false;
	const auto &Peer = Found->second;
	return !Peer.Planned || (Peer.Planning && Peer.Planning->Ready && !Peer.Planning->Disposing && Peer.Planning->Valid(*this, Peer));
}

void ReplicationCoordinator::ProcessPlanning(std::uint64_t SimulationTick) {
	if (SimulationTick > PlanningTick) {
		PlanningTick = SimulationTick;
		PlanningRemaining = Configuration.PlanningWorkPerTick;
	}
	if (PlanningRemaining == 0 || PlanningPeers.empty()) return;
	runtime_detail::WorkScope PlanningWork(runtime_detail::WorkPhase::StructuralPlanning);
	const auto Started = std::chrono::steady_clock::now();
	std::string CatalogError;
	RefreshCatalog(CatalogError);
	// At most one rotation per call. Remaining allowance is shared by repeated
	// calls in this tick; it cannot be refilled by a peer or an older tick.
	const auto Opportunities = PlanningPeers.size();
	// Do not advance the peer cursor with only the visit unit left. Calling that
	// a service opportunity would let a small budget skip the same peer forever.
	for (std::size_t Index = 0; Index < Opportunities && PlanningRemaining >= 2 && !PlanningPeers.empty(); ++Index) {
		auto Position = PlanningPeers.upper_bound(PlanningAfter);
		if (Position == PlanningPeers.end()) Position = PlanningPeers.begin();
		const auto Connection = *Position;
		PlanningAfter = Connection;
		auto Found = Peers.find(Connection);
		--PlanningRemaining;
		SaturatingAdd(Metrics.PlanningWork, 1);
		runtime_detail::CountWork(runtime_detail::WorkCounter::PlanningCharged);
		if (Found == Peers.end()) {
			const auto Detached = DetachedPlanning.find(Connection);
			if (Detached == DetachedPlanning.end()) { PlanningPeers.erase(Connection); continue; }
			const auto Allowance = std::min(PlanningRemaining, Configuration.PlanningPeerQuantum);
			std::size_t Used = 0;
			bool Running = true;
			{
				runtime_detail::WorkScope CleanupWork(runtime_detail::WorkPhase::PlanningCleanup);
				while (Used < Allowance && Running) { ++Used; Running = Detached->second->Work.Resume(); }
			}
			PlanningRemaining -= Used;
			SaturatingAdd(Metrics.PlanningWork, Used);
			runtime_detail::CountWork(runtime_detail::WorkCounter::PlanningCharged, Used);
			runtime_detail::RecordWorkUnits(runtime_detail::WorkPhase::StructuralPlanning, Used + 1);
			if (!Running) {
				PlanningRecordCount -= Detached->second->Records;
				DetachedPlanning.erase(Detached); PlanningPeers.erase(Connection);
			}
			continue;
		}
		auto &Peer = Found->second;
		if (!CatalogError.empty()) Peer.PlanningError = CatalogError;
		if (!Peer.PlanningError.empty() || Peer.PreparedCommit) { PlanningPeers.erase(Connection); continue; }
		const auto Invalid = runtime_detail::MeasureWork(runtime_detail::WorkPhase::PlanningValidation, [&] {
			return Peer.Planning && !Peer.Planning->Disposing && !Peer.Planning->Valid(*this, Peer);
		});
		if (Invalid) {
			Peer.Planning->Work = {};
			Peer.Planning->Ready = false;
			Peer.Planning->Disposing = true;
			Peer.Planning->Work = Peer.Planning->Dispose();
			SaturatingAdd(Metrics.PlanningInvalidations, 1);
		}
		if (!Peer.Planning) {
			try {
				if (Peer.AcceptedRevision == std::numeric_limits<std::uint64_t>::max())
					throw std::runtime_error("Accepted structural planning revision exhausted");
				Peer.Planning = std::make_shared<PlanningContinuation>();
				Peer.Planning->Selection = Peer.PlanningSelection;
				// Charge the lease even if only the peer-visit unit remains and this
				// coroutine cannot start until a later tick. A newer input may replace
				// the peer's lease before then; the old one must remain accounted.
				Peer.Planning->Retain(*this, Peer.PlanningSelection->DesiredObjects.size() + Peer.PlanningSelection->RequiredObjects.size());
				Peer.Planning->Revision = PlanningCursor;
				Peer.Planning->AcceptedRevision = Peer.AcceptedRevision;
				Peer.Planning->Work = Peer.Planning->Run(*this, Peer, PlanningTick);
			} catch (const std::exception &Failure) {
				Peer.PlanningError = Failure.what(); PlanningPeers.erase(Connection); continue;
			}
		}
		auto &Plan = *Peer.Planning;
		if (Plan.Ready) { PlanningPeers.erase(Connection); continue; }
		const auto Allowance = std::min(PlanningRemaining, Configuration.PlanningPeerQuantum);
		std::size_t Used = 0;
		SaturatingAdd(Metrics.PlanningServiceOpportunities, 1);
		if (Peer.LastPlanningServiceTick != 0) {
			const auto Gap = PlanningTick >= Peer.LastPlanningServiceTick ? PlanningTick - Peer.LastPlanningServiceTick : 0;
			Metrics.PlanningMaximumServiceGapTicks = std::max(Metrics.PlanningMaximumServiceGapTicks, Gap);
			runtime_detail::MaximumWork(runtime_detail::WorkCounter::PlanningServiceGap, Gap);
		}
		Peer.LastPlanningServiceTick = PlanningTick;
		try {
			bool Running = true;
			{
				// Scope ends before returning to Main; never retain a timing scope across a coroutine yield.
				runtime_detail::WorkScope ResumeWork(Plan.Disposing ? runtime_detail::WorkPhase::PlanningCleanup : runtime_detail::WorkPhase::PlanningResume);
				while (Used < Allowance && Running) { ++Used; Running = Plan.Work.Resume(); }
			}
			SaturatingAdd(Metrics.PlanningResumes, 1);
			runtime_detail::CountWork(runtime_detail::WorkCounter::PlanningResumes);
			if (!Running) {
				if (Plan.Disposing) {
					PlanningRecordCount -= Plan.Records;
					Peer.Planning.reset();
					if (Peer.PlanningAwaitingSelection || (Peer.PendingTransitions.empty() && Peer.PlanningSelection == Peer.ResolvedSelection &&
						PlanningContinuation::SameCursor(Peer.DesiredDependencyCursor, DependencyCursor))) {
						PlanningPeers.erase(Connection); Peer.LastPlanningServiceTick = 0;
					}
				} else if (Plan.AwaitingSelection) {
					Peer.PlanningAwaitingSelection = true;
					Plan.Work = {}; Plan.Disposing = true; Plan.Work = Plan.Dispose();
					SaturatingAdd(Metrics.PlanningInvalidations, 1);
				} else {
					runtime_detail::WorkScope InstallWork(runtime_detail::WorkPhase::PlanningInstall);
					const auto CountWithoutPeer = PendingTransitionCount - Peer.PendingTransitions.size();
					if (Plan.Working.PendingTransitions.size() > Configuration.MaximumPendingTransitions - CountWithoutPeer)
						throw std::runtime_error("Session structural pending transition limit exceeded");
					PendingTransitionCount = CountWithoutPeer + Plan.Working.PendingTransitions.size();
					Peer.DesiredObjects.swap(Plan.Working.DesiredObjects);
					Peer.RequiredObjects.swap(Plan.Working.RequiredObjects);
					Peer.PendingTransitions.swap(Plan.Working.PendingTransitions);
					Peer.CriticalQueue.swap(Plan.Working.CriticalQueue);
					Peer.OrdinaryQueue.swap(Plan.Working.OrdinaryQueue);
					Peer.LeavingDependents.swap(Plan.Working.LeavingDependents);
					Peer.View.RelevantObjects.swap(Plan.Working.View.RelevantObjects);
					Peer.NextPendingToken = Plan.Working.NextPendingToken;
					Peer.DesiredDependencyCursor = DependencyCursor;
					Peer.ResolvedSelection = Plan.Selection;
					Plan.Installed = true;
					SaturatingAdd(Metrics.PlanningReadyBatches, 1);
					runtime_detail::CountWork(runtime_detail::WorkCounter::PlanningReadyBatches);
					if (Plan.Cost == 0) {
						Plan.Work = {}; Plan.Ready = false; Plan.Disposing = true; Plan.Work = Plan.Dispose();
					} else { PlanningPeers.erase(Connection); Peer.LastPlanningServiceTick = 0; }
				}
			}
		} catch (const std::exception &Failure) {
			Peer.PlanningError = Failure.what();
			PlanningPeers.erase(Connection);
		}
		PlanningRemaining -= Used;
		SaturatingAdd(Metrics.PlanningWork, Used);
		runtime_detail::CountWork(runtime_detail::WorkCounter::PlanningCharged, Used);
		runtime_detail::RecordWorkUnits(runtime_detail::WorkPhase::StructuralPlanning, Used + 1);
		Metrics.PlanningMaximumPeerSlice = std::max<std::uint64_t>(Metrics.PlanningMaximumPeerSlice, Used);
	}
	Metrics.PlanningRecords = PlanningRecordCount;
	Metrics.PlanningMaximumTickWork = std::max<std::uint64_t>(Metrics.PlanningMaximumTickWork, Configuration.PlanningWorkPerTick - PlanningRemaining);
	SaturatingAdd(Metrics.PlanningCpuNanoseconds, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Started).count()));
}

ReplicationProduceResult ReplicationCoordinator::ProducePlannedFrame(ConnectionId Connection, ReplicationMessageKind Kind,
	std::size_t MaximumTransitions, std::uint64_t SimulationTick, std::size_t MaximumFrameBytes) {
	auto &Peer = Peers.at(Connection);
	if (!Peer.PlanningError.empty()) return {{}, Peer.PlanningError};
	if (Peer.PreparedCommit) return {{}, "A structural frame is awaiting scheduler acceptance"};
	if (MaximumTransitions == 0 || MaximumTransitions > MaximumReplicationOperationsPerFrame ||
		MaximumFrameBytes == 0 || MaximumFrameBytes > MaximumReplicationFrameBytes)
		return {{}, "Structural transition work limit is invalid"};
	std::string Error;
	if (!RefreshCatalog(Error)) return {{}, std::move(Error)};
	if (!IsPlanningReady(Connection)) return {{}, "No replication relevance changes are available"};
	auto &Plan = *Peer.Planning;
	if ((Kind == ReplicationMessageKind::Baseline) != Plan.Baseline || Plan.Cost == 0 || Plan.Cost > MaximumTransitions)
		return {{}, "No replication relevance changes are available"};
	// This is the existing 3J selection point. All planning has completed; no
	// traversal can discover an extra member after the budget decision here.
	const auto Started = std::chrono::steady_clock::now();
	std::optional<runtime_detail::WorkScope> FrameWork;
	FrameWork.emplace(runtime_detail::WorkPhase::StructuralFrameBuild);
	auto CandidateMetrics = Metrics;
	std::set<ObjectId> Requested;
	ReplicationFrame Frame{ReplicationProtocolVersion, Kind, Peer.View.Epoch, Peer.NextSequence};
	if (Plan.Baseline) Frame.Schema = CaptureReplicationSchemaCompatibility();
	Frame.Operations.reserve(Plan.Cost);
	for (const auto &Reference : Plan.Clears) {
		WireValue Value = std::monostate{};
		if (Reference.CurrentTarget.IsValid() && !Plan.Leaving.contains(Reference.CurrentTarget) &&
			(Peer.View.Knows(Reference.CurrentTarget) || Plan.Entering.contains(Reference.CurrentTarget)))
			Value = WireObjectReference{WireObjectId::FromObjectId(Reference.CurrentTarget)};
		Frame.Operations.push_back({Frame.Epoch, PropertyReplicationUpdate{Reference.Referrer, Reference.Property->Name, Value}});
		SaturatingAdd(CandidateMetrics.SoftReferenceFixups, 1);
	}
	auto CurrentParent = [&](ObjectId Object) -> std::optional<ObjectId> {
		const auto Found = Catalog.find(Object);
		return Found == Catalog.end() ? std::nullopt : Found->second->Publication.Parent;
	};
	auto AcceptedParent = [&](ObjectId Object) -> std::optional<ObjectId> {
		const auto Found = Peer.AcceptedParents.find(Object);
		return Found != Peer.AcceptedParents.end() && Found->second.Parent.IsValid() ? std::optional(Found->second.Parent) : std::nullopt;
	};
	for (const auto &[Object, Depth] : OrderByAncestry(Plan.Entering, CurrentParent, true)) {
		(void)Depth;
		if (!Catalog.contains(Object) || RetiredObjects.contains(Object)) return {{}, "Cannot publish a stale authoritative object"};
		auto Publish = MakePeerPublish(Object, Peer.View, Plan.Entering, Plan.Leaving, CandidateMetrics, Requested);
		if (!PublishReferencesKnown(Peer.View, Publish, Plan.Entering)) return {{}, "Hard materialization dependency is not available"};
		Frame.Operations.push_back({Frame.Epoch, FinalizePeerPublish(std::move(Publish))});
	}
	std::set<ObjectId> ParentFixups;
	for (const auto Parent : Plan.Entering)
		if (const auto Found = Plan.EnterParentFixups.find(Parent); Found != Plan.EnterParentFixups.end())
			ParentFixups.insert(Found->second.begin(), Found->second.end());
	for (const auto Parent : Plan.Leaving)
		if (const auto Found = Plan.LeaveParentFixups.find(Parent); Found != Plan.LeaveParentFixups.end())
			ParentFixups.insert(Found->second.begin(), Found->second.end());
	for (const auto &[Object, Depth] : OrderByAncestry(ParentFixups, CurrentParent, true)) {
		(void)Depth; Frame.Operations.push_back({Frame.Epoch, Plan.ParentMoves.at(Object)});
	}
	std::uint64_t Destroyed = 0, Unpublished = 0;
	for (const auto &[Object, Depth] : OrderByAncestry(Plan.Leaving, AcceptedParent, false)) {
		(void)Depth;
		if (RetiredObjects.contains(Object)) { Frame.Operations.push_back({Frame.Epoch, DestroyReplication{Object}}); ++Destroyed; }
		else { Frame.Operations.push_back({Frame.Epoch, UnpublishReplication{Object}}); ++Unpublished; }
	}
	for (const auto &Reference : Plan.Restores) {
		Frame.Operations.push_back({Frame.Epoch, PropertyReplicationUpdate{Reference.Referrer, Reference.Property->Name,
			WireObjectReference{WireObjectId::FromObjectId(Reference.CurrentTarget)}}});
		SaturatingAdd(CandidateMetrics.SoftReferenceFixups, 1);
	}
	if (Frame.Operations.empty() || Frame.Operations.size() > Plan.Cost || Frame.Operations.size() > MaximumTransitions)
		return {{}, "Replication relevance frame exceeded its selected work limit"};
	FrameWork.reset();
	auto Encoded = runtime_detail::MeasureWork(runtime_detail::WorkPhase::StructuralValidationEncode, [&] { return EncodeReplicationFrame(Frame); });
	if (!Encoded) return {{}, Encoded.error().Format()};
	if (Encoded->size() > MaximumFrameBytes) {
		const auto Minimum = Plan.Baseline ? std::max(Plan.LargestGroup, GetPendingCriticalTransitionCount(Connection)) : Plan.LargestGroup;
		const auto Reduced = std::max(Minimum, Plan.Cost / 2);
		if (Reduced >= Plan.Cost) return {{}, "Atomic structural group exceeds the negotiated reliable message limit"};
		Peer.PlanningFrameLimit = Reduced;
		Plan.Work = {};
		Plan.Ready = false;
		Plan.Disposing = true;
		Plan.Work = Plan.Dispose();
		PlanningPeers.insert(Connection);
		return {{}, "No replication relevance changes are available"};
	}
	auto Next = Peer.NextSequence.TryNext();
	if (!Next) return {{}, "Reliable replication sequence is exhausted"};
	if (Peer.PublicationJournalEnds.size() + Plan.Entering.size() > MaximumPeerDesiredObjects)
		return {{}, "Publication journal watermark limit exceeded"};
	std::uint64_t DeadlineMisses = 0;
	for (const auto Object : Plan.Entering) {
		const auto Since = Peer.PendingTransitions.at(Object).PendingSinceTick;
		if (SimulationTick >= Since && SimulationTick - Since > Configuration.TransitionDeadlineTicks) ++DeadlineMisses;
	}
	const auto Count = Frame.Operations.size();
	PreparedStructuralCommit Commit{
		.Sequence = Frame.Sequence, .NextSequence = *Next,
		.JournalCursor = Plan.Baseline ? std::optional(CatalogCursor) : std::nullopt,
		.PublicationJournalEnd = CatalogCursor.NextSequence,
		.Entering = std::vector<ObjectId>(Plan.Entering.begin(), Plan.Entering.end()),
		.Leaving = std::vector<ObjectId>(Plan.Leaving.begin(), Plan.Leaving.end()),
		.PublishedObjects = Plan.Entering.size(), .UnpublishedObjects = Unpublished, .DestroyedObjects = Destroyed,
		.DeadlineMisses = DeadlineMisses, .TransitionCount = Count,
		.Parents = CaptureAcceptedParents(Peer, Frame, CatalogCursor.NextSequence),
	};
	SaturatingAdd(CandidateMetrics.StructuralSchedulingTicks, 1);
	SaturatingAdd(CandidateMetrics.StructuralTransitionsOffered, Peer.PendingTransitions.size());
	SaturatingAdd(CandidateMetrics.StructuralTransitionsSelected, Count);
	SaturatingAdd(CandidateMetrics.StructuralTransitionsPrepared, Count);
	SaturatingAdd(CandidateMetrics.StructuralTransitionsEncoded, Count);
	SaturatingAdd(CandidateMetrics.StructuralBytesEncoded, Encoded->size());
	SaturatingAdd(CandidateMetrics.OperationsGenerated, Count);
	SaturatingAdd(CandidateMetrics.RelevanceTransitions, Plan.Entering.size() + Plan.Leaving.size());
	SaturatingAdd(CandidateMetrics.PeerMaterializationPlans, 1);
	SaturatingAdd(CandidateMetrics.StructuralSelectionCpuNanoseconds, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Started).count()));
	if (Plan.Baseline) { SaturatingAdd(CandidateMetrics.BaselineObjects, Plan.Entering.size()); SaturatingAdd(CandidateMetrics.BaselineBytes, Encoded->size()); }
	else SaturatingAdd(CandidateMetrics.IncrementalBytes, Encoded->size());
	RequestedTemplates.merge(Requested);
	Metrics = CandidateMetrics;
	Peer.PreparedCommit = std::move(Commit);
	return {std::move(Frame), {}, Count};
}
