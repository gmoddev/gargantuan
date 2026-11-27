#include "gargantuan/runtime/SpatialDomainMember.hpp"
#include "gargantuan/classes/BasePart.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/SpatialRegion.hpp"
#include "gargantuan/classes/ViewportFrame.hpp"
#include "gargantuan/render/RenderDirtyAccumulator.hpp"

#include <stdexcept>
#include <vector>

namespace gargantuan {
	struct SpatialDomainMember::State {
		std::weak_ptr<SpatialRegion> Region;
		std::weak_ptr<WorldRoot> Active;
		std::vector<SignalConnection::Pointer> Connections;
		~State() {
			for (auto &Connection : Connections)
				if (Connection) Connection->Disconnect();
		}
	};

	SpatialDomainMember::SpatialDomainMember() = default;
	SpatialDomainMember::~SpatialDomainMember() = default;

	std::optional<std::shared_ptr<SpatialRegion>> SpatialDomainMember::GetAssignedRegion() const {
		if (Binding)
			if (auto R = Binding->Region.lock(); R && !R->IsDestroying() && !R->GetDestroyed()) return R;
		return std::nullopt;
	}

	std::shared_ptr<WorldRoot> SpatialDomainMember::ResolveAssignedWorld(const Instance &Root) const {
		auto Target = GetAssignedRegion().value_or(nullptr);
		if (!Target || Root.GetDestroyed() || Root.IsDestroying()) return {};
		const auto World = Root.GetDataModel();
		if (!World || World != Target->GetDataModel() || ViewportFrame::FindContaining(Root)) return {};
		return Target;
	}

	const Instance *SpatialDomainMember::GetAssignmentRoot(const Instance &Value) {
		for (const Instance *Current = &Value; Current;) {
			if (const auto *Member = dynamic_cast<const SpatialDomainMember *>(Current);
				Member && Member->HasAssignedRegion())
				return Current;
			if (dynamic_cast<const WorldRoot *>(Current) || dynamic_cast<const ViewportFrame *>(Current)) break;
			Current = Current->ParentReference.lock().get();
		}
		return nullptr;
	}

	void SpatialDomainMember::Refresh(const std::shared_ptr<Instance> &Root) {
		if (!Binding) return;
		const auto Before = Binding->Active.lock();
		const auto After = ResolveAssignedWorld(*Root);
		if (Before == After) return;
		WorldRoot::MoveSpatialMembers(Root, Before, After);
		if (Before) Before->ExplicitSpatialRoots.erase(Root->GetObjectId());
		if (After) After->ExplicitSpatialRoots.insert_or_assign(Root->GetObjectId(), Root);
		Binding->Active = After;
	}

	void SpatialDomainMember::SetAssignedRegion(Instance &Owner, std::optional<std::shared_ptr<SpatialRegion>> Value) {
		Owner.AssertCanMutate();
		Owner.ValidatePropertyMutation("SpatialRegion", Value);
		auto Region = Value.value_or(nullptr);
		const auto Realm = Owner.GetDataModel();
		if (Region && Realm && Realm->HasDefaultNetworkSpatialProfile())
			throw std::invalid_argument("[Spatial:Network] This session protocol requires default-domain membership");
		if (Region && (Region->GetDestroyed() || Region->IsDestroying() ||
					   (Realm && Region->GetDataModel() && Region->GetDataModel() != Realm)))
			throw std::invalid_argument("[Spatial:Membership] A region must be live and in the same DataModel");
		if ((!Binding && !Region) || (Binding && Region && Binding->Region.lock() == Region)) return;
		const auto Root = Owner.shared_from_this();
		const auto Before = Binding ? Binding->Active.lock() : WorldRoot::GetEffectiveWorld(Owner);
		auto Next = Region ? std::make_unique<State>() : nullptr;
		if (Next) {
			Next->Region = Region;
			Next->Active = Before;
			auto Changed = [Weak = Owner.weak_from_this()](auto) {
				if (const auto R = Weak.lock()) dynamic_cast<SpatialDomainMember &>(*R).Refresh(R);
			};
			Next->Connections.push_back(Owner.AncestryChanged->Connect(Changed));
			Next->Connections.push_back(Owner.Destroying->Connect(Changed));
			Next->Connections.push_back(Region->AncestryChanged->Connect(Changed));
			Next->Connections.push_back(Region->Destroying->Connect(Changed));
			Next->Connections.push_back(
				Owner.DescendantAdded->Connect([Weak = Owner.weak_from_this()](const auto &Child) {
					const auto R = Weak.lock();
					if (!R || GetAssignmentRoot(*Child) != R.get()) return;
					auto &Assigned = dynamic_cast<SpatialDomainMember &>(*R);
					if (Assigned.Binding)
						if (auto W = Assigned.Binding->Active.lock()) W->AdmitSpatialMember(Child);
				})
			);
			Next->Connections.push_back(
				Owner.DescendantRemoved->Connect([Weak = Owner.weak_from_this()](const auto &Child) {
					const auto R = Weak.lock();
					if (!R) return;
					auto &Assigned = dynamic_cast<SpatialDomainMember &>(*R);
					if (Assigned.Binding)
						if (auto W = Assigned.Binding->Active.lock()) W->RemoveSpatialMember(Child);
				})
			);
		}
		auto Previous = std::move(Binding);
		Binding = std::move(Next);
		try {
			const auto After = WorldRoot::GetEffectiveWorld(Owner);
			WorldRoot::MoveSpatialMembers(Root, Before, After);
			if (Before && Previous) Before->ExplicitSpatialRoots.erase(Root->GetObjectId());
			if (Binding) {
				Binding->Active = After;
				if (After) After->ExplicitSpatialRoots.insert_or_assign(Root->GetObjectId(), Root);
			}
		} catch (...) {
			Binding = std::move(Previous);
			throw;
		}
		Owner.NotifyPropertyCommitted("SpatialRegion");
	}

	void WorldRoot::MoveSpatialMembers(
		const std::shared_ptr<Instance> &Root,
		const std::shared_ptr<WorldRoot> &Before,
		const std::shared_ptr<WorldRoot> &After
	) {
		if (Before == After) return;
		if ((Before && Before->SteppingPhysics) || (After && After->SteppingPhysics))
			throw std::runtime_error("[Spatial:Membership] Cannot change membership inside a physics step");
		struct Item {
			std::shared_ptr<Instance> Owner;
			std::optional<PhysicsBodyState> Body;
		};
		std::vector<Item> Items;
		const auto Collect = [&](const std::shared_ptr<Instance> &O) {
			std::optional<PhysicsBodyState> S;
			if (Before)
				if (auto It = Before->PartBodies.find(O->GetObjectId()); It != Before->PartBodies.end())
					S = Before->Physics.GetBodyState(It->second);
			Items.push_back({O, std::move(S)});
		};
		Collect(Root);
		for (std::size_t I = 0; I < Items.size(); ++I) {
			const auto Current = Items[I].Owner;
			for (const auto &Child : Current->Children) {
				if (dynamic_cast<WorldRoot *>(Child.get()) || dynamic_cast<ViewportFrame *>(Child.get())) continue;
				if (auto *Own = dynamic_cast<SpatialDomainMember *>(Child.get()); Own && Own->HasAssignedRegion())
					continue;
				Collect(Child);
			}
		}
		const auto Install = [&](const std::shared_ptr<WorldRoot> &W) {
			if (!W || W->ShuttingDownPhysics) return;
			for (const auto &Entry : Items) {
				W->AdmitSpatialMember(Entry.Owner);
				if (const auto Part = std::dynamic_pointer_cast<BasePart>(Entry.Owner)) {
					const auto It = W->PartBodies.find(Part->GetObjectId());
					if (It == W->PartBodies.end())
						throw std::runtime_error("[Spatial:Membership] Rigid body admission failed");
					if (Entry.Body && !Entry.Body->Description.Anchored) {
						if (!W->Physics.SetLinearVelocity(It->second, Entry.Body->LinearVelocity).Succeeded() ||
							!W->Physics.SetAngularVelocity(It->second, Entry.Body->AngularVelocity).Succeeded() ||
							!W->Physics.SetAwake(It->second, Entry.Body->Awake).Succeeded())
							throw std::runtime_error("[Spatial:Membership] Rigid body state admission failed");
						if (auto S = W->Physics.GetBodyState(It->second)) Part->PublishedPhysicsState = *S;
					}
				}
			}
		};
		const auto Retire = [&](const std::shared_ptr<WorldRoot> &W) {
			if (W && !W->ShuttingDownPhysics)
				for (auto It = Items.rbegin(); It != Items.rend(); ++It)
					W->RemoveSpatialMember(It->Owner);
		};
		Retire(Before);
		try {
			Install(After);
		} catch (...) {
			Retire(After);
			Install(Before);
			throw;
		}
		const auto Scope = Root->GetReplicationScopeId();
		if (Scope.IsValid())
			RenderDirtyAccumulator::Get().RequestFullResync(Scope, "Spatial domain membership changed");
	}
}
