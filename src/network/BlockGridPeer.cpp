// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "BlockGridPeer.hpp"
#include "gargantuan/classes/BlockGrid.hpp"
#include "gargantuan/classes/WorldRoot.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <stdexcept>

namespace gargantuan::network {
	namespace {
		AssetContentId ReceiptContent(const AssetContentId& Cells,std::uint64_t Motion,std::uint64_t Fence) {
			std::array<std::uint8_t,32+16> Bytes{};
			std::ranges::copy(Cells.Bytes,Bytes.begin());
			for (unsigned I=0;I<8;++I) {
				Bytes[32+I]=static_cast<std::uint8_t>(Motion>>(I*8));
				Bytes[40+I]=static_cast<std::uint8_t>(Fence>>(I*8));
			}
			return AssetContentId::Hash(Bytes);
		}
		AssetContentId PlacementDigest(const BlockGrid &Grid, const CFrame &Pose) {
			std::array<std::uint8_t, 13 * sizeof(float) + 2> Bytes{};
			std::size_t Cursor{};
			auto Put = [&](float Value) {
				const auto Bits = std::bit_cast<std::uint32_t>(Value == 0.f ? 0.f : Value);
				for (unsigned Shift = 0; Shift < 32; Shift += 8)
					Bytes[Cursor++] = static_cast<std::uint8_t>(Bits >> Shift);
			};
			for (int Axis = 0; Axis < 3; ++Axis)
				Put(Pose.Position[Axis]);
			for (int Column = 0; Column < 3; ++Column)
				for (int Row = 0; Row < 3; ++Row)
					Put(Pose.Rotation[Column][Row]);
			Put(Grid.GetCellSize());
			Bytes[Cursor++] = Grid.GetCanCollide();
			Bytes[Cursor] = Grid.GetCanQuery();
			return AssetContentId::Hash(Bytes);
		}
	}
	BlockGridPeer::BlockGridPeer(bool IsServer, std::uint64_t SessionEpoch, ObjectId GrantedDomain,
		std::shared_ptr<const blockgrid::SharedAssetPins> Pins, std::uint64_t Generation)
		: Server(IsServer), Epoch(SessionEpoch), Domain(GrantedDomain), GrantGeneration(Generation), SharedAssets(std::move(Pins)) {}
	std::uint64_t BlockGridPeer::Token() {
		if (!NextToken) throw std::overflow_error("[BlockGrid:Network] Transfer tokens exhausted");
		return NextToken++;
	}
	void BlockGridPeer::Track(ObjectId Grid) {
		if (Server || Grids.contains(Grid)) return;
		if (Grids.size() >= MaximumGrids) throw std::length_error("[BlockGrid:Network] Grid shell budget exhausted");
		Grids.emplace(Grid, Entry{.Subscription = Token()});
	}
	void BlockGridPeer::Forget(ObjectId Grid) {
		if (Active && Active->Offer.Scope.Grid == Grid) {
			Active.reset();
			Receiver.Cancel();
		}
		if (const auto It = Grids.find(Grid); It != Grids.end()) {
			const auto Shell = It->second.Replica.lock();
			Grids.erase(It);
			if (Shell && !Shell->IsDestroying() && !Shell->GetDestroyed()) Shell->RetireReplicaContent();
		}
	}
	void BlockGridPeer::Retire() {
		while (!Grids.empty())
			Forget(Grids.begin()->first);
	}
	bool BlockGridPeer::IsInstalled(ObjectId Source, const BlockGrid &Grid) const {
		const auto Found = Grids.find(Source);
		return Server && Found!=Grids.end() && Found->second.Owner.lock().get()==&Grid && Grid.EnsureAssetBindings() && FencesInstalled(Source);
	}
	GridTransferFence BlockGridPeer::CurrentFence(ObjectId Id,const BlockGrid& Grid,const Resolve& Find) const {
		const auto Stamp=Grid.GetPublicationTransfer();
		if (!Stamp) return {};
		const auto Other=Stamp->Grids[Stamp->Grids[0]==Id?1:0];
		// A withheld participant never discloses its ID. The known grid may render, but cannot attest the paired landing.
		return {Stamp->WorldRevision,Grids.contains(Other) && Find(Other)?Other:ObjectId{}};
	}
	bool BlockGridPeer::FencesInstalled(ObjectId Source,const SpatialDestinationBounds* Bounds) const {
		std::array<ObjectId,MaximumGrids> Seen{};
		std::size_t Count{};
		auto Id=Source;
		for (;;) {
			if (std::find(Seen.begin(),Seen.begin()+Count,Id)!=Seen.begin()+Count) return true;
			const auto It=Grids.find(Id);
			if (It==Grids.end() || !It->second.Known.Bytes || It->second.Unavailable || Count==Seen.size()) return false;
			const auto& E=It->second;
			const auto Grid=Server?E.Owner.lock():E.Replica.lock();
			if (!Grid || !E.Known.Coverage || !E.WireMembership || E.LocalMembership!=Grid->GetDomainMembershipRevision()) return false;
			const auto Local=Bounds?GridLocalBounds(Grid->GetCFrame(),Grid->GetCellSize(),*Bounds):std::nullopt;
			if (Bounds && (!Local || !E.Known.Coverage->Contains(*Local))) return false;
			if (Active && Active->Offer.Fence.Other==Id && !Active->Offer.RetainsController) return false;
			if (Active && Active->Offer.Scope.Grid==Id) {
				// An independent camera can refresh its supplemental pages while the
				// installed controller rectangle and all authority stamps still agree.
				const auto& Offer=Active->Offer;
				if (!Local || !Offer.RetainsController || !E.Known.Coverage->Rectangle || Offer.Fence!=E.Fence || Offer.Membership!=E.WireMembership ||
					Offer.Configuration!=E.Configuration || Offer.MotionRevision!=E.MotionRevision ||
					Offer.Placement!=E.Placement || !blockgrid::ReplicaCoverage{.Rectangle=E.Known.Coverage->Rectangle}.Contains(*Local) ||
					(Active->Content.Coverage && (Active->Content.Coverage->Rectangle!=E.Known.Coverage->Rectangle ||
					!Active->Content.Coverage->Contains(*Local)))) return false;
			}
			if (Server) {
				if (E.SourceRevision!=Grid->GetField().GetPublicRevision() || E.Configuration!=Grid->GetConfigurationRevision() ||
					E.MotionRevision!=Grid->GetAssemblyRevision()) return false;
				const auto Stamp=Grid->GetPublicationTransfer();
				if ((Stamp?Stamp->WorldRevision:0)!=E.Fence.Generation) return false;
			} else {
				const auto Projection=Grid->GetAssemblyProjection();
				if (!Grid->HasReplicaField() || !Projection || Projection->Revision!=E.MotionRevision ||
					PlacementDigest(*Grid,Grid->GetCFrame())!=E.Placement) return false;
			}
			for (const auto& [Other,Peer]:Grids)
				if (Peer.Fence.Other==Id && Peer.Fence.Generation>E.Fence.Generation) return false;
			Seen[Count++]=Id;
			if (!E.Fence.Generation) return true;
			if (!E.Fence.Other.IsValid()) return false;
			const auto Other=Grids.find(E.Fence.Other);
			if (Other==Grids.end() || Other->second.Fence.Generation<E.Fence.Generation) return false;
			Id=E.Fence.Other;
		}
	}
	std::optional<SpatialAppliedBinding> BlockGridPeer::GetBinding(ObjectId Source, const BlockGrid &Grid, bool Empty) const {
		return Binding(Source,Grid,Empty,nullptr);
	}
	std::optional<SpatialAppliedBinding> BlockGridPeer::GetBinding(ObjectId Source,const BlockGrid& Grid,bool Empty,
		const SpatialDestinationBounds& Bounds) const {
		return Binding(Source,Grid,Empty,&Bounds);
	}
	std::optional<SpatialAppliedBinding> BlockGridPeer::Binding(ObjectId Source,const BlockGrid& Grid,bool Empty,
		const SpatialDestinationBounds* Bounds) const {
		const auto Found = Grids.find(Source);
		if (Found == Grids.end()) return {};
		const auto &Entry = Found->second;
		if (!Entry.Known.Coverage || (!Bounds && !Entry.Known.Coverage->Whole) ||
			!FencesInstalled(Source,Bounds) || !Grid.EnsureAssetBindings()) return {};
		if (Server) {
			if (Entry.Owner.lock().get()!=&Grid) return {};
		} else {
			const auto Motion = Grid.GetAssemblyProjection();
			if (Entry.Replica.lock().get()!=&Grid || !Grid.HasReplicaField() || !Motion || Motion->Revision!=Entry.MotionRevision ||
				PlacementDigest(Grid,Grid.GetCFrame())!=Entry.Placement) return {};
		}
		if (Bounds && Entry.Known.Coverage->Rectangle) {
			const auto Window=SelectGridCoverage(Grid.GetField(),Grid.GetCFrame(),Grid.GetCellSize(),GridInterest{.Destination=*Bounds});
			if (!Window.Rectangle || !Entry.Known.Coverage->Contains(*Window.Rectangle)) return {};
			const auto Revision=Grid.GetField().GetPublicRevision();
			if (Entry.ReceiptFieldRevision!=Revision || Entry.ReceiptRectangle!=Window.Rectangle ||
				Entry.ReceiptMotionRevision!=Entry.MotionRevision || Entry.ReceiptFenceGeneration!=Entry.Fence.Generation) {
				const auto Exact=CaptureGridReplica(Grid.GetField(),Window,{});
				if (!Exact) return {};
				Entry.ReceiptRectangle=Window.Rectangle;
				Entry.ReceiptFieldRevision=Revision;
				Entry.ReceiptMotionRevision=Entry.MotionRevision;
				Entry.ReceiptFenceGeneration=Entry.Fence.Generation;
				Entry.ReceiptDigest=ReceiptContent(Exact->Digest,Entry.MotionRevision,Entry.Fence.Generation);
			}
			// Both peers derive this immutable content identity from precisely the
			// requested footprint. Other views have their own delivery revisions.
			return SpatialAppliedBinding{{WireObjectId::FromObjectId(Source),SpatialCollisionProvider::BlockGrid},
				Entry.ReceiptDigest,Entry.WireMembership,Entry.Configuration,true,Empty};
		}
		return SpatialAppliedBinding{{WireObjectId::FromObjectId(Source),SpatialCollisionProvider::BlockGrid},
			ReceiptContent(Entry.Known.Digest,Entry.MotionRevision,Entry.Fence.Generation),Entry.Revision,Entry.Configuration,true,Empty};
	}
	void BlockGridPeer::Receive(const GridStreamMessage &Message, const Resolve &Find, std::uint64_t Tick) {
		if (Message.Scope.Epoch != Epoch || Message.Scope.Domain != Domain || Message.Scope.GrantGeneration != GrantGeneration)
			throw std::invalid_argument("[BlockGrid:Network] Transfer outside the session grant");
		if (Server)
			ReceiveServer(Message, Find);
		else
			ReceiveClient(Message, Find, Tick);
	}
	void BlockGridPeer::ReceiveServer(const GridStreamMessage &M, const Resolve &Find) {
		if (M.Kind != GridStreamKind::Request && M.Kind != GridStreamKind::Installed)
			throw std::invalid_argument("[BlockGrid:Network] Message has no server-side consumer");
		const auto Id = M.Scope.Grid;
		if (!Find(Id)) {
			Forget(Id);
			return;
		} // A reliable reply can cross a later structural departure.
		auto Found = Grids.find(Id);
		if (M.Kind == GridStreamKind::Request) {
			if (M.Base.IsValid() || M.Serial != M.Subscription)
				throw std::invalid_argument("[BlockGrid:Network] Invalid fresh subscription");
			if (Found != Grids.end()) {
				if (M.Subscription <= Found->second.Subscription) return;
				Forget(Id);
			}
			if (Grids.size() >= MaximumGrids)
				throw std::length_error("[BlockGrid:Network] Grid request budget exhausted");
			Grids.emplace(Id, Entry{.Subscription = M.Subscription, .Requested = true});
			return;
		}
		if (Found == Grids.end() || Found->second.Subscription != M.Subscription) return;
		if (M.Serial <= Found->second.LastSerial) return;
		if (!Active || !Active->Offered || Active->Offer.Scope != M.Scope || Active->Offer.Serial != M.Serial ||
			Active->Offer.Revision != M.Revision || Active->Offer.Target != M.Target ||
			Active->Offer.Configuration != M.Configuration || Active->Offer.Placement != M.Placement ||
			Active->Offer.MotionRevision != M.MotionRevision || Active->Offer.Membership!=M.Membership ||
			Active->Offer.RetainsController!=M.RetainsController || Active->Offer.Fence!=M.Fence ||
			Active->Cursor != Active->Offer.Size - Active->Offer.Suffix)
			throw std::invalid_argument("[BlockGrid:Network] Invalid installation acknowledgment");
		Found->second.Known = std::move(Active->Content);
		Found->second.Revision = M.Revision;
		Found->second.Configuration = M.Configuration;
		Found->second.MotionRevision = M.MotionRevision;
		Found->second.LocalMembership=Found->second.WireMembership=M.Membership;
		Found->second.Placement = M.Placement;
		Found->second.LastSerial = M.Serial;
		Found->second.Fence=M.Fence;
		Found->second.SourceRevision=Active->SourceRevision;
		Active.reset();
	}
	void BlockGridPeer::ReceiveClient(const GridStreamMessage &M, const Resolve &Find, std::uint64_t Tick) {
		if (M.Kind != GridStreamKind::Offer && M.Kind != GridStreamKind::Fragment && M.Kind != GridStreamKind::Forget &&
			M.Kind != GridStreamKind::Unavailable)
			throw std::invalid_argument("[BlockGrid:Network] Message has no client-side consumer");
		auto Found = Grids.find(M.Scope.Grid);
		if (Found == Grids.end() || Found->second.Subscription != M.Subscription) return;
		auto Grid = Find(M.Scope.Grid);
		if (!Grid) {
			Forget(M.Scope.Grid);
			return;
		}
		auto &Entry = Found->second;
		if (Entry.LocalMembership && Entry.LocalMembership!=Grid->GetDomainMembershipRevision()) {
			Forget(M.Scope.Grid);
			Track(M.Scope.Grid);
			return;
		}
		Entry.Replica = Grid;
		if (M.Kind == GridStreamKind::Forget) {
			if (M.Serial < Entry.LastSerial) return;
			if (Active && (Active->Offer.Scope != M.Scope || Active->Offer.Serial != M.Serial))
				throw std::invalid_argument("[BlockGrid:Network] Cancellation does not identify the live offer");
			if (!Active && M.Serial != Entry.LastSerial)
				throw std::invalid_argument("[BlockGrid:Network] Unknown cancellation");
			Active.reset();
			Receiver.Cancel();
			Grid->RetireReplicaContent();
			Entry.Known = {};
			Entry.Revision = Entry.Configuration = Entry.MotionRevision = 0;
			Entry.LastSerial = M.Serial;
			return;
		}
		if (!Entry.Requested || M.Serial <= Entry.LastSerial)
			throw std::invalid_argument("[BlockGrid:Network] Unrequested transfer");
		if (M.Kind == GridStreamKind::Unavailable) {
			if (Active) throw std::invalid_argument("[BlockGrid:Network] Availability changed during an active offer");
			Grid->RetireReplicaContent();
			Entry.Known = {};
			Entry.Revision = Entry.Configuration = Entry.MotionRevision = 0;
			Entry.LastSerial = M.Serial;
			return;
		}
		if (M.Kind == GridStreamKind::Offer) {
			if (M.RetainsController && (!Entry.Known.Coverage || !Entry.Known.Coverage->Rectangle ||
				M.Base!=Entry.Known.Digest || M.Membership!=Entry.WireMembership || M.Configuration!=Entry.Configuration ||
				M.MotionRevision!=Entry.MotionRevision || M.Placement!=Entry.Placement || M.Fence!=Entry.Fence))
				throw std::invalid_argument("[BlockGrid:Network] Unverified controller baseline");
			if (Active || M.Revision < Entry.Revision || M.Configuration < Entry.Configuration || M.Membership<Entry.WireMembership ||
				M.MotionRevision < Entry.MotionRevision ||
				(M.Revision == Entry.Revision && M.Configuration == Entry.Configuration &&
				 M.MotionRevision == Entry.MotionRevision && M.Fence==Entry.Fence))
				throw std::invalid_argument("[BlockGrid:Network] Unexpected revision");
			Active.emplace(Transfer{.Offer = M, .ProgressTick = Tick});
			Entry.LocalMembership=Grid->GetDomainMembershipRevision();
			if (Grid->HasReplicaField() && Entry.Known.Bytes && Entry.Known.Digest == M.Target && M.Base == M.Target &&
				M.Prefix == M.Size && M.Size == Entry.Known.Bytes->size()) {
				Active->Content = Entry.Known;
				return;
			}
			Receiver.Begin(M, M.Scope, &Entry.Known);
		} else {
			if (!Active || Active->Installed) throw std::invalid_argument("[BlockGrid:Network] Unsolicited fragment");
			Receiver.Append(M);
			Active->ProgressTick = Tick;
		}
		if (Receiver.Complete()) {
			auto Result = Receiver.Finish();
			if (Active->Offer.RetainsController && Result.Content.Coverage->Rectangle!=Entry.Known.Coverage->Rectangle)
				throw std::invalid_argument("[BlockGrid:Network] Retained controller rectangle changed");
			Active->PendingField.emplace(std::move(Result.Field));
			Active->Content = std::move(Result.Content);
		}
	}
	void BlockGridPeer::Prune(const Resolve &Find) {
		for (auto It = Grids.begin(); It != Grids.end();) {
			const auto Current=It++;
			const auto Id=Current->first;
			const auto Grid=Find(Id);
			if (!Grid) Forget(Id);
			else if (!Server && Current->second.LocalMembership && Current->second.LocalMembership!=Grid->GetDomainMembershipRevision()) {
				Forget(Id);
				Track(Id);
			}
		}
	}
	void BlockGridPeer::Pump(const Resolve &Find, const Send &Queue, std::uint64_t Tick,const GridInterest& Interest) {
		Prune(Find);
		if (Active && Tick - Active->ProgressTick > 600)
			throw std::runtime_error("[BlockGrid:Network] Grid transfer timed out");
		if (!Server) {
			if (Active && Active->Content.Bytes) {
				const auto &Offer = Active->Offer;
				const auto Grid = Find(Offer.Scope.Grid);
				if (!Grid || !Offer.Assembly || PlacementDigest(*Grid, Offer.Assembly->Pose) != Offer.Placement) return;
				if (Active->PendingField) {
					Grid->InstallReplicaField(std::move(*Active->PendingField),Active->Content.Coverage);
					Active->PendingField.reset();
				}
				(void)Grid->ApplyReplicaAssemblyProjection(*Offer.Assembly);
				const auto World = WorldRoot::GetEffectiveWorld(*Grid);
				if (!World || !Grid->EnsureAssetBindings() || !World->VerifyReplicaGridInstallation(*Grid)) return;
				Active->Installed = true;
				auto Ack = Offer;
				Ack.Kind = GridStreamKind::Installed;
				if (!Queue(Ack)) return;
				auto &Entry = Grids.at(Offer.Scope.Grid);
				Entry.Known = std::move(Active->Content);
				Entry.Revision = Offer.Revision;
				Entry.Configuration = Offer.Configuration;
				Entry.MotionRevision = Offer.MotionRevision;
				Entry.WireMembership=Offer.Membership;
				Entry.Placement = Offer.Placement;
				Entry.Fence=Offer.Fence;
				Entry.LastSerial = Offer.Serial;
				Active.reset();
			}
			for (auto &[Id, Entry] : Grids) {
				if (Entry.Requested) continue;
				GridStreamMessage Request{
					.Kind = GridStreamKind::Request,
					.Scope = {Epoch, Id, Domain, GrantGeneration},
					.Subscription = Entry.Subscription,
					.Serial = Entry.Subscription
				};
				if (Queue(Request)) Entry.Requested = true;
				break;
			}
			return;
		}
		if (!Active && !Grids.empty()) {
			auto It = Grids.upper_bound(LastServed);
			for (std::size_t I = 0; I < Grids.size(); ++I) {
				if (It == Grids.end()) It = Grids.begin();
				auto &[Id, Entry] = *It++;
				auto Grid = Find(Id);
				if (!Grid) continue;
				Entry.Owner=Grid;
				const auto &Field = Grid->GetField();
				const auto Revision = Field.GetPublicRevision();
				auto Coverage=SelectGridCoverage(Field,Grid->GetCFrame(),Grid->GetCellSize(),Interest);
				if (Interest.ControlledMotion && Coverage.Rectangle &&
					(!Entry.Known.Coverage || Entry.Known.Coverage->Rectangle!=Coverage.Rectangle))
					Coverage.Origins.clear();
				const auto Fence=CurrentFence(Id,*Grid,Find);
				const auto Membership=Grid->GetDomainMembershipRevision();
				const bool SameLifetime=Membership==Entry.WireMembership;
				const bool SameSelection=Entry.Known.Coverage && *Entry.Known.Coverage==Coverage;
				const bool SameContent=SameSelection && Entry.SourceRevision==Revision;
				const bool Ready=Grid->EnsureAssetBindings();
				if (Entry.Unavailable && Entry.SourceRevision==Revision && Entry.DeniedCoverage && *Entry.DeniedCoverage==Coverage) continue;
				const auto MotionRevision=Grid->GetAssemblyRevision();
				if (Ready && SameLifetime && SameContent && Grid->GetConfigurationRevision()==Entry.Configuration && Fence==Entry.Fence &&
					(MotionRevision==Entry.MotionRevision || Tick-Entry.MotionTick<3)) continue;
				std::optional<GridReplicaContent> Next;
				if (Ready) {
					if (SameContent) Next=Entry.Known;
					else Next=CaptureGridReplica(Field,Coverage,[&](const blockgrid::Field& Schema) {
						return blockgrid::AdmitsAssetPins(Schema,SharedAssets.get());
					});
				}
				if (!Next) {
					if (Entry.Unavailable) {
						Entry.SourceRevision=Revision;
						Entry.DeniedCoverage=Ready?std::make_shared<const blockgrid::ReplicaCoverage>(Coverage):nullptr;
						continue;
					}
					GridStreamMessage Closed{.Kind = GridStreamKind::Unavailable, .Scope = {Epoch, Id, Domain, GrantGeneration},
						.Subscription = Entry.Subscription, .Serial = Token()};
					if (Queue(Closed)) {
						Entry.Unavailable = true;
						Entry.Known = {};
						Entry.Configuration = Entry.Revision = Entry.MotionRevision = 0;
						Entry.LastSerial = Closed.Serial;
						Entry.SourceRevision=Revision;
						Entry.DeniedCoverage=Ready?std::make_shared<const blockgrid::ReplicaCoverage>(Coverage):nullptr;
						LastServed = Id;
					}
					return;
				}
				Entry.Unavailable = false;
				Entry.DeniedCoverage.reset();
				const bool SameProjection=Entry.Known.Bytes && Next->Digest==Entry.Known.Digest;
				if (SameProjection) {
					Entry.SourceRevision=Revision;
					if (SameLifetime && Grid->GetConfigurationRevision()==Entry.Configuration && Fence==Entry.Fence &&
						(MotionRevision==Entry.MotionRevision || Tick-Entry.MotionTick<3)) continue;
					Next=Entry.Known;
				}
				const auto Motion = Grid->GetAssemblyProjection();
				if (!Motion) continue;
				if ((!SameProjection || !SameLifetime) && Entry.Revision==std::numeric_limits<std::uint64_t>::max())
					throw std::overflow_error("[BlockGrid:Network] View revision exhausted");
				auto Offer = MakeGridOffer(
					{Epoch, Id, Domain, GrantGeneration},
					Entry.Subscription,
					Token(),
					Entry.Revision+unsigned(!SameProjection || !SameLifetime),
					Grid->GetConfigurationRevision(),
					PlacementDigest(*Grid, Motion->Pose),
					*Motion,
					*Next,
					&Entry.Known
				);
				Offer.Fence=Fence;
				Offer.Membership=Membership;
				Offer.RetainsController=Interest.ControlledMotion && !Interest.Destination && SameLifetime && Entry.Known.Coverage &&
					Coverage.Rectangle && Entry.Known.Coverage->Rectangle==Coverage.Rectangle && Entry.SourceRevision==Revision &&
					Offer.Configuration==Entry.Configuration && Offer.MotionRevision==Entry.MotionRevision &&
					Offer.Placement==Entry.Placement && Fence==Entry.Fence;
				Active.emplace(
					Transfer{.Offer=Offer,.Content=std::move(*Next),.Cursor=NextGridLiteral(Offer,Offer.Prefix).first,
						.ProgressTick=Tick,.SourceRevision=Revision}
				);
				LastServed = Id;
				Entry.MotionTick = Tick;
				break;
			}
		}
		if (!Active) return;
		auto &Transfer = *Active;
		const auto Grid = Find(Transfer.Offer.Scope.Grid);
		if (!Grid) return;
		// A current controller offer owns a bounded snapshot of the secondary
		// hints at admission. A moving camera coalesces behind it. Domain grants,
		// controller rectangles, configuration, content and bilateral fences still revoke it.
		const bool ControllerOffer=Interest.ControlledMotion && !Interest.Destination && Transfer.Content.Coverage->Rectangle;
		const auto Desired=SelectGridCoverage(Grid->GetField(),Grid->GetCFrame(),Grid->GetCellSize(),
			ControllerOffer?GridInterest{.ControlledMotion=Interest.ControlledMotion}:Interest);
		const bool SameController=ControllerOffer && Desired.Rectangle==Transfer.Content.Coverage->Rectangle;
		bool Current=Grid->GetDomainMembershipRevision()==Transfer.Offer.Membership &&
			Grid->GetConfigurationRevision()==Transfer.Offer.Configuration && Grid->EnsureAssetBindings() &&
			(SameController || *Transfer.Content.Coverage==Desired) && Transfer.Offer.Fence==CurrentFence(Transfer.Offer.Scope.Grid,*Grid,Find);
		if (Current && Grid->GetField().GetPublicRevision()!=Transfer.SourceRevision) {
			const auto Updated=CaptureGridReplica(Grid->GetField(),*Transfer.Content.Coverage,[&](const blockgrid::Field& Schema) {
				return blockgrid::AdmitsAssetPins(Schema,SharedAssets.get());
			});
			Current=Updated && Updated->Digest==Transfer.Content.Digest;
			if (Current) Transfer.SourceRevision=Grid->GetField().GetPublicRevision();
		}
		if (!Current) {
			if (Transfer.Offered) {
				auto Cancel = Transfer.Offer;
				Cancel.Kind = GridStreamKind::Forget;
				if (!Queue(Cancel)) return;
				auto &Entry = Grids.at(Cancel.Scope.Grid);
				Entry.Known = {};
				Entry.Configuration = Entry.Revision = Entry.MotionRevision = 0;
				Entry.LastSerial = Cancel.Serial;
			}
			Active.reset();
			return;
		}
		if (!Transfer.Offered) {
			if (Queue(Transfer.Offer)) {
				Transfer.Offered = true;
				Transfer.ProgressTick = Tick;
			}
			return;
		}
		const auto End = Transfer.Offer.Size - Transfer.Offer.Suffix;
		if (Transfer.Cursor == End) return;
		auto Fragment = Transfer.Offer;
		Fragment.Kind = GridStreamKind::Fragment;
		Fragment.Offset = static_cast<std::uint32_t>(Transfer.Cursor);
		const auto Count = std::min<std::size_t>(MaximumGridStreamFragment, NextGridLiteral(Transfer.Offer,Transfer.Cursor).second);
		Fragment.Fragment = std::span(*Transfer.Content.Bytes).subspan(Transfer.Cursor, Count);
		if (Queue(Fragment)) {
			Transfer.Cursor = NextGridLiteral(Transfer.Offer,Transfer.Cursor+Count).first;
			Transfer.ProgressTick = Tick;
		}
	}
}
