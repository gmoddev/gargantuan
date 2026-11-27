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
	BlockGridPeer::BlockGridPeer(bool IsServer, std::uint64_t SessionEpoch, ObjectId GrantedDomain)
		: Server(IsServer), Epoch(SessionEpoch), Domain(GrantedDomain) {}
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
		return Server && Found != Grids.end() && Found->second.Known.Bytes &&
			   Found->second.Revision == Grid.GetField().GetPublicRevision() &&
			   Found->second.Configuration == Grid.GetConfigurationRevision() && Grid.EnsureAssetBindings() &&
			   Found->second.MotionRevision == Grid.GetAssemblyRevision();
	}
	void BlockGridPeer::Receive(const GridStreamMessage &Message, const Resolve &Find, std::uint64_t Tick) {
		if (Message.Scope.Epoch != Epoch || Message.Scope.Domain != Domain)
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
			Active->Offer.MotionRevision != M.MotionRevision ||
			Active->Cursor != Active->Offer.Size - Active->Offer.Suffix)
			throw std::invalid_argument("[BlockGrid:Network] Invalid installation acknowledgment");
		Found->second.Known = std::move(Active->Content);
		Found->second.Revision = M.Revision;
		Found->second.Configuration = M.Configuration;
		Found->second.MotionRevision = M.MotionRevision;
		Found->second.Placement = M.Placement;
		Found->second.LastSerial = M.Serial;
		Active.reset();
	}
	void BlockGridPeer::ReceiveClient(const GridStreamMessage &M, const Resolve &Find, std::uint64_t Tick) {
		if (M.Kind != GridStreamKind::Offer && M.Kind != GridStreamKind::Fragment && M.Kind != GridStreamKind::Forget)
			throw std::invalid_argument("[BlockGrid:Network] Message has no client-side consumer");
		auto Found = Grids.find(M.Scope.Grid);
		if (Found == Grids.end() || Found->second.Subscription != M.Subscription) return;
		auto Grid = Find(M.Scope.Grid);
		if (!Grid) {
			Forget(M.Scope.Grid);
			return;
		}
		auto &Entry = Found->second;
		Entry.Replica = Grid;
		if (M.Kind == GridStreamKind::Forget) {
			if (M.Serial < Entry.LastSerial) return;
			if (Active && (Active->Offer.Scope != M.Scope || Active->Offer.Serial != M.Serial))
				throw std::invalid_argument("[BlockGrid:Network] Cancellation does not identify the live offer");
			if (!Active && M.Serial != Entry.LastSerial)
				throw std::invalid_argument("[BlockGrid:Network] Unknown cancellation");
			Active.reset();
			Receiver.Cancel();
			Entry.Known = {};
			Entry.Revision = Entry.Configuration = Entry.MotionRevision = 0;
			Entry.LastSerial = M.Serial;
			return;
		}
		if (!Entry.Requested || M.Serial <= Entry.LastSerial)
			throw std::invalid_argument("[BlockGrid:Network] Unrequested transfer");
		if (M.Kind == GridStreamKind::Offer) {
			if (Active || M.Revision < Entry.Revision || M.Configuration < Entry.Configuration ||
				M.MotionRevision < Entry.MotionRevision ||
				(M.Revision == Entry.Revision && M.Configuration == Entry.Configuration &&
				 M.MotionRevision == Entry.MotionRevision))
				throw std::invalid_argument("[BlockGrid:Network] Unexpected revision");
			Active.emplace(Transfer{.Offer = M, .ProgressTick = Tick});
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
			Active->PendingField.emplace(std::move(Result.Field));
			Active->Content = std::move(Result.Content);
		}
	}
	void BlockGridPeer::Pump(const Resolve &Find, const Send &Queue, std::uint64_t Tick) {
		for (auto It = Grids.begin(); It != Grids.end();) {
			const auto Id = (It++)->first;
			if (!Find(Id)) Forget(Id);
		}
		if (Active && Tick - Active->ProgressTick > 600)
			throw std::runtime_error("[BlockGrid:Network] Grid transfer timed out");
		if (!Server) {
			if (Active && Active->Content.Bytes) {
				const auto &Offer = Active->Offer;
				const auto Grid = Find(Offer.Scope.Grid);
				if (!Grid || !Offer.Assembly || PlacementDigest(*Grid, Offer.Assembly->Pose) != Offer.Placement) return;
				if (Active->PendingField) {
					Grid->InstallReplicaField(std::move(*Active->PendingField));
					Active->PendingField.reset();
				}
				(void)Grid->ApplyReplicaAssemblyProjection(*Offer.Assembly);
				const auto World = WorldRoot::GetEffectiveWorld(*Grid);
				const std::array Required{Grid->GetObjectId()};
				const auto Position = Grid->GetCFrame().Position;
				if (!World || !Grid->EnsureAssetBindings() ||
					!World->PrepareSpatialDestination({Position, Position}, Required))
					return;
				Active->Installed = true;
				auto Ack = Offer;
				Ack.Kind = GridStreamKind::Installed;
				if (!Queue(Ack)) return;
				auto &Entry = Grids.at(Offer.Scope.Grid);
				Entry.Known = std::move(Active->Content);
				Entry.Revision = Offer.Revision;
				Entry.Configuration = Offer.Configuration;
				Entry.MotionRevision = Offer.MotionRevision;
				Entry.Placement = Offer.Placement;
				Entry.LastSerial = Offer.Serial;
				Active.reset();
			}
			for (auto &[Id, Entry] : Grids) {
				if (Entry.Requested) continue;
				GridStreamMessage Request{
					.Kind = GridStreamKind::Request,
					.Scope = {Epoch, Id, Domain},
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
				const bool SameStructure = Grid->GetField().GetPublicRevision() == Entry.Revision &&
										   Grid->GetConfigurationRevision() == Entry.Configuration;
				if (SameStructure &&
					(Entry.MotionRevision == Grid->GetAssemblyRevision() || Tick - Entry.MotionTick < 3))
					continue;
				if (!Grid->EnsureAssetBindings()) continue;
				const auto Motion = Grid->GetAssemblyProjection();
				if (!Motion) continue;
				auto Next = Entry.Known.Bytes && Grid->GetField().GetPublicRevision() == Entry.Revision
								? Entry.Known
								: CaptureGridReplica(Grid->GetField());
				auto Offer = MakeGridOffer(
					{Epoch, Id, Domain},
					Entry.Subscription,
					Token(),
					Grid->GetField().GetPublicRevision(),
					Grid->GetConfigurationRevision(),
					PlacementDigest(*Grid, Motion->Pose),
					*Motion,
					Next,
					&Entry.Known
				);
				Active.emplace(
					Transfer{.Offer = Offer, .Content = std::move(Next), .Cursor = Offer.Prefix, .ProgressTick = Tick}
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
		if (Grid->GetConfigurationRevision() != Transfer.Offer.Configuration || !Grid->EnsureAssetBindings()) {
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
		const auto Count = std::min<std::size_t>(MaximumGridStreamFragment, End - Transfer.Cursor);
		Fragment.Fragment = std::span(*Transfer.Content.Bytes).subspan(Transfer.Cursor, Count);
		if (Queue(Fragment)) {
			Transfer.Cursor += Count;
			Transfer.ProgressTick = Tick;
		}
	}
}
