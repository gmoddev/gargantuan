// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "BlockGridPeer.hpp"
#include "gargantuan/classes/BlockGrid.hpp"
#include "gargantuan/classes/WorldRoot.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace gargantuan::network {
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
		Grids.erase(Grid);
	}
	bool BlockGridPeer::IsInstalled(ObjectId Grid, std::uint64_t PublicRevision) const {
		const auto Found = Grids.find(Grid);
		return Found != Grids.end() && Found->second.Known.Bytes && Found->second.Revision == PublicRevision;
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
		if (!Active || !Active->Offered || Active->Offer.Scope != M.Scope || Active->Offer.Serial != M.Serial ||
			Active->Offer.Revision != M.Revision || Active->Offer.Target != M.Target ||
			Active->Cursor != Active->Offer.Size - Active->Offer.Suffix)
			throw std::invalid_argument("[BlockGrid:Network] Invalid installation acknowledgment");
		Found->second.Known = std::move(Active->Content);
		Found->second.Revision = M.Revision;
		Found->second.LastSerial = M.Serial;
		Active.reset();
	}
	void BlockGridPeer::ReceiveClient(const GridStreamMessage &M, const Resolve &Find, std::uint64_t Tick) {
		if (M.Kind != GridStreamKind::Offer && M.Kind != GridStreamKind::Fragment)
			throw std::invalid_argument("[BlockGrid:Network] Message has no client-side consumer");
		auto Found = Grids.find(M.Scope.Grid);
		if (Found == Grids.end() || Found->second.Subscription != M.Subscription) return;
		auto Grid = Find(M.Scope.Grid);
		if (!Grid) {
			Forget(M.Scope.Grid);
			return;
		}
		auto &Entry = Found->second;
		if (!Entry.Requested || M.Serial <= Entry.LastSerial)
			throw std::invalid_argument("[BlockGrid:Network] Unrequested transfer");
		if (M.Kind == GridStreamKind::Offer) {
			if (Active || M.Revision <= Entry.Revision)
				throw std::invalid_argument("[BlockGrid:Network] Unexpected revision");
			Receiver.Begin(M, M.Scope, &Entry.Known);
			Active.emplace(Transfer{.Offer = M, .ProgressTick = Tick});
		} else {
			if (!Active || Active->Installed) throw std::invalid_argument("[BlockGrid:Network] Unsolicited fragment");
			Receiver.Append(M);
			Active->ProgressTick = Tick;
		}
		if (Receiver.Complete()) {
			auto Result = Receiver.Finish();
			Grid->InstallReplicaField(std::move(Result.Field));
			const auto World = WorldRoot::GetEffectiveWorld(*Grid);
			const std::array Required{Grid->GetObjectId()};
			const auto Position = Grid->GetCFrame().Position;
			if (!World || !World->PrepareSpatialDestination({Position, Position}, Required))
				throw std::runtime_error("[BlockGrid:Network] Replica collision installation failed");
			Active->Content = std::move(Result.Content);
			Active->Installed = true;
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
			if (Active && Active->Installed) {
				const auto &Offer = Active->Offer;
				auto Ack = Offer;
				Ack.Kind = GridStreamKind::Installed;
				if (!Queue(Ack)) return;
				auto &Entry = Grids.at(Offer.Scope.Grid);
				Entry.Known = std::move(Active->Content);
				Entry.Revision = Offer.Revision;
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
				const auto &[Id, Entry] = *It++;
				auto Grid = Find(Id);
				if (!Grid || Grid->GetField().GetPublicRevision() == Entry.Revision) continue;
				auto Next = CaptureGridReplica(Grid->GetField());
				auto Offer = MakeGridOffer(
					{Epoch, Id, Domain},
					Entry.Subscription,
					Token(),
					Grid->GetField().GetPublicRevision(),
					Next,
					&Entry.Known
				);
				Active.emplace(
					Transfer{.Offer = Offer, .Content = std::move(Next), .Cursor = Offer.Prefix, .ProgressTick = Tick}
				);
				LastServed = Id;
				break;
			}
		}
		if (!Active) return;
		auto &Transfer = *Active;
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
