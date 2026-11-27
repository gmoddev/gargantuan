#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/SpatialRegion.hpp"
#include "gargantuan/classes/Terrain.hpp"
#include "gargantuan/network/SpatialTransfer.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <array>
#include <iostream>
#include <stdexcept>

namespace {
	using namespace gargantuan;
	using namespace gargantuan::network;
	constexpr WireObjectId SourceKey{11, 1}, DestinationKey{12, 1}, SubjectKey{13, 1}, FloorKey{14, 1};
	constexpr ConnectionId Peer{2, 3}, OtherPeer{2, 4};
	const SpatialDestinationBounds Bounds{{-3, 0, -3}, {3, 6, 3}};

	void Check(bool Value, const char *Reason) {
		if (!Value) throw std::runtime_error(Reason);
	}

	struct Realm {
		std::shared_ptr<DataModel> Game = std::make_shared<DataModel>();
		std::shared_ptr<Workspace> Source = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
		std::shared_ptr<SpatialRegion> Destination = std::make_shared<SpatialRegion>();
		std::shared_ptr<Part> Actor = std::make_shared<Part>(), Floor = std::make_shared<Part>();
		SpatialTransferReservations Reservations;
		SpatialTransferAdapters Adapters;
		bool Installed{true}, Permission{true}, MissingFloor{};
		std::uint8_t FloorContent{19};
		int Applications{};
		Realm() {
			Destination->SetParent(Source);
			Actor->SetSize({1, 1, 1});
			Actor->SetCFrame(CFrame(10, 3, 0));
			Actor->SetParent(Source);
			Actor->SetLinearVelocity({2, 0, 0});
			Floor->SetSize({1, 1, 1});
			Floor->SetCFrame(CFrame(2.5f, 3, 0));
			Floor->SetAnchored(true);
			Floor->SetParent(Destination);
			Adapters.ResolveObject = [this](WireObjectId Id) -> std::optional<ObjectId> {
				if (Id == SourceKey) return Source->GetObjectId();
				if (Id == DestinationKey) return Destination->GetObjectId();
				if (Id == SubjectKey) return Actor->GetObjectId();
				return {};
			};
			Adapters.Resolve = [this](SpatialWireProvider Provider) -> std::optional<ObjectId> {
				if (Provider == SpatialWireProvider{DestinationKey, SpatialCollisionProvider::Terrain})
					return Destination->GetTerrain()->GetObjectId();
				if (!MissingFloor && Provider == SpatialWireProvider{FloorKey, SpatialCollisionProvider::Rigid})
					return Floor->GetObjectId();
				return {};
			};
			Adapters.Project = [this](
								   const SpatialProviderReceipt &Native, const SpatialDestinationBounds &
							   ) -> std::optional<SpatialAppliedBinding> {
				const bool Terrain = Native.Owner == Destination->GetTerrain()->GetObjectId();
				if (!Terrain && Native.Owner != Floor->GetObjectId()) return {};
				const std::array<std::uint8_t, 2> VerifiedBytes{
					std::uint8_t(Native.Provider), Terrain ? std::uint8_t(7) : FloorContent
				};
				return SpatialAppliedBinding{
					{Terrain ? DestinationKey : FloorKey, Native.Provider},
					AssetContentId::Hash(VerifiedBytes),
					1,
					1,
					Installed,
					Native.AuthoritativeEmpty
				};
			};
			Adapters.Reserve = [this](const SpatialTransferFrame &Frame, const SpatialDestinationReceipt &Receipt) {
				return Reservations.Reserve(
					Actor->GetObjectId(), Frame, Receipt, [this] { return Permission; }, {Destination, Floor}
				);
			};
		}
		~Realm() {
			Game->Destroy();
		}
		SpatialMovementRequest Movement() const {
			return {
				Actor->GetObjectId(),
				Source->GetObjectId(),
				Source->GetSpatialActivation(),
				CFrame(10, 3, 0),
				CFrame(0, 3, 0)
			};
		}
		bool Apply(const SpatialTransferFrame &, const SpatialDestinationReceipt &Receipt) {
			++Applications;
			return Destination->CommitSpatialTransfer(Actor, Movement(), Receipt).Succeeded();
		}
		bool AtDestination() const {
			return WorldRoot::GetEffectiveWorld(*Actor) == Destination;
		}
	};

	struct Pair {
		Realm Authority, Replica;
		SpatialTransferServer Server;
		SpatialTransferReplica Client;
		SpatialTransferScope Scope{6, 7, 8, 9, SubjectKey, SourceKey, DestinationKey};
		SpatialTransferClock::time_point Now{SpatialTransferClock::now()};
		SpatialTransferStep Begin() {
			const std::array Owners{Authority.Floor->GetObjectId()};
			return Server.Begin(
				Peer,
				Scope,
				Authority.Actor,
				Authority.Destination,
				Authority.Movement(),
				Bounds,
				Owners,
				Authority.Adapters,
				Now
			);
		}
		SpatialTransferStep ServerReceive(const SpatialTransferFrame &Frame) {
			return Server.Receive(Peer, Scope, Frame, Authority.Adapters, Now);
		}
		SpatialTransferStep ClientReceive(const SpatialTransferFrame &Frame) {
			return Client.Receive(
				Scope, Frame, Replica.Destination, Replica.Adapters, [this](const auto &Message, const auto &Receipt) {
					return Replica.Apply(Message, Receipt);
				}
			);
		}
	};
	SpatialTransferFrame Packet(const SpatialTransferStep &Step, SpatialTransferPhase Phase) {
		Check(Step.Reply && Step.Reply->Phase == Phase, "unexpected transfer phase");
		const auto Bytes = EncodeSpatialTransfer(*Step.Reply);
		Check(
			!Bytes.empty() && Bytes.size() <= MaximumSpatialTransferBytes && IsSpatialTransfer(Bytes), "wire envelope"
		);
		auto Parsed = DecodeSpatialTransfer(Bytes);
		Check(
			Parsed && Parsed->Phase == Phase && Parsed->Collision == Step.Reply->Collision, "bounded wire round-trip"
		);
		return *Parsed;
	}

	void CheckedHandoff() {
		using enum SpatialTransferPhase;
		Pair P;
		const auto ServerId = P.Authority.Actor->GetObjectId(), ClientId = P.Replica.Actor->GetObjectId();
		Check(ServerId != ClientId, "separate native realms must use independent handles");
		const auto OfferPacket = Packet(P.Begin(), Offer);
		Check(
			OfferPacket.Owners.size() == 2 && P.Authority.Reservations.HoldsMember(ServerId) &&
				P.Authority.Reservations.HoldsProvider(P.Authority.Floor->GetObjectId()) &&
				P.Authority.Reservations.HoldsBounds(P.Authority.Destination->GetObjectId(), Bounds) &&
				!P.Authority.Reservations.HoldsBounds(P.Authority.Destination->GetObjectId(), {{5, 0, 5}, {7, 3, 7}}) &&
				!P.Authority.Reservations.HoldsBounds(P.Authority.Source->GetObjectId(), Bounds),
			"server holds only the reserved domain footprint"
		);
		const auto ReadyPacket = Packet(P.ClientReceive(OfferPacket), Ready);
		Check(
			!P.Replica.Reservations.HoldsMember(ClientId), "receipt preparation alone cannot reserve or move replica"
		);
		auto Forged = ReadyPacket;
		Forged.Phase = Held;
		Check(!P.ServerReceive(Forged).Reply && !P.Authority.AtDestination(), "out-of-order hold admitted a transfer");
		Check(
			!P.Server.Receive(OtherPeer, P.Scope, ReadyPacket, P.Authority.Adapters, P.Now).Reply,
			"connection generation mismatch admitted a reply"
		);
		auto Changed = ReadyPacket;
		Changed.DestinationPose.Components[0] += 1;
		Check(!P.ServerReceive(Changed).Reply, "reply altered the destination");
		const auto ReservePacket = Packet(P.ServerReceive(ReadyPacket), Reserve);
		const auto HeldPacket = Packet(P.ClientReceive(ReservePacket), Held);
		Check(
			P.Replica.Reservations.HoldsMember(ClientId) && !P.Authority.AtDestination() && !P.Replica.AtDestination(),
			"replica hold must precede either move"
		);
		const auto CommitPacket = Packet(P.ServerReceive(HeldPacket), Commit);
		Check(
			P.Authority.AtDestination() && !P.Replica.AtDestination() && P.Authority.Actor->GetObjectId() == ServerId,
			"server commit must consume its native admission once"
		);
		(void)Packet(P.ServerReceive(HeldPacket), Commit);
		const auto Ack = Packet(P.ClientReceive(CommitPacket), Applied);
		(void)Packet(P.ClientReceive(CommitPacket), Applied);
		Check(
			P.Replica.Applications == 1 && P.Replica.AtDestination() && P.Replica.Actor->GetObjectId() == ClientId &&
				P.Authority.Actor->GetLinearVelocity().x == 2.f && P.Replica.Actor->GetLinearVelocity().x == 2.f &&
				!P.Replica.Reservations.HoldsMember(ClientId),
			"replica must apply exactly once and release its lease"
		);
		Check(
			P.ServerReceive(Ack).Status == SpatialTransferStatus::Applied &&
				!P.Authority.Reservations.HoldsMember(ServerId),
			"terminal acknowledgment releases the authority hold"
		);
	}

	void Failures() {
		using enum SpatialTransferPhase;
		{
			Pair P;
			const auto OfferPacket = Packet(P.Begin(), Offer);
			auto Bytes = EncodeSpatialTransfer(OfferPacket);
			Bytes.push_back(std::byte{0});
			Check(!DecodeSpatialTransfer(Bytes), "trailing wire bytes accepted");
			Bytes.pop_back();
			Bytes.pop_back();
			Check(!DecodeSpatialTransfer(Bytes), "truncated provider generation accepted");
			auto Invalid = OfferPacket;
			Invalid.Owners.push_back(Invalid.Owners.back());
			Check(EncodeSpatialTransfer(Invalid).empty(), "duplicate owner accepted");
			Invalid = OfferPacket;
			Invalid.SourcePose.Components[3] = 0.f;
			Check(EncodeSpatialTransfer(Invalid).empty(), "nonrigid source pose accepted");
			P.Replica.MissingFloor = true;
			(void)Packet(P.ClientReceive(OfferPacket), Cancel);
			Check(!P.Replica.AtDestination(), "missing provider declared empty");
		}
		{
			Pair P;
			const auto OfferPacket = Packet(P.Begin(), Offer);
			P.Replica.FloorContent = 20;
			(void)Packet(P.ClientReceive(OfferPacket), Cancel);
			Check(!P.Replica.AtDestination(), "different installed content shared portable receipt");
		}
		{
			Pair P;
			const auto OfferPacket = Packet(P.Begin(), Offer);
			P.Replica.Installed = false;
			(void)Packet(P.ClientReceive(OfferPacket), Cancel);
		}
		{
			Pair P;
			const auto OfferPacket = Packet(P.Begin(), Offer);
			const auto ReadyPacket = Packet(P.ClientReceive(OfferPacket), Ready);
			const auto ReservePacket = Packet(P.ServerReceive(ReadyPacket), Reserve);
			P.Replica.Floor->SetCFrame(CFrame(1.5f, 3, 0));
			(void)Packet(P.ClientReceive(ReservePacket), Cancel);
			Check(
				!P.Replica.Reservations.HoldsMember(P.Replica.Actor->GetObjectId()),
				"stale native replica receipt kept a hold"
			);
		}
		{
			Pair P;
			const auto OfferPacket = Packet(P.Begin(), Offer);
			const auto ReadyPacket = Packet(P.ClientReceive(OfferPacket), Ready);
			const auto Reserved = Packet(P.ServerReceive(ReadyPacket), Reserve);
			P.Replica.Permission = false;
			(void)Packet(P.ClientReceive(Reserved), Cancel);
		}
		{
			Pair P;
			const auto OfferPacket = Packet(P.Begin(), Offer);
			const auto ReadyPacket = Packet(P.ClientReceive(OfferPacket), Ready);
			const auto Reserved = Packet(P.ServerReceive(ReadyPacket), Reserve);
			const auto HeldPacket = Packet(P.ClientReceive(Reserved), Held);
			P.Authority.Floor->SetCFrame(CFrame(0, 3, 0));
			(void)Packet(P.ServerReceive(HeldPacket), Cancel);
			Check(!P.Authority.AtDestination(), "changed authoritative occupancy allowed a move");
		}
		{
			Pair P;
			const auto OfferPacket = Packet(P.Begin(), Offer);
			(void)Packet(P.ClientReceive(OfferPacket), Ready);
			++P.Scope.FocusEpoch;
			Check(
				P.Client.Poll(P.Scope, P.Replica.Adapters).Status == SpatialTransferStatus::Canceled &&
					P.Server.Poll(Peer, P.Scope, P.Authority.Adapters, P.Now).Status ==
						SpatialTransferStatus::Canceled &&
					!P.ClientReceive(OfferPacket).Reply,
				"focus change reused the old grant"
			);
			++P.Scope.SessionEpoch;
			P.Server.Reset();
			P.Client.Reset();
			Check(!P.ClientReceive(OfferPacket).Reply, "old packet crossed a reconnect");
		}
		{
			Pair P;
			const auto OfferPacket = Packet(P.Begin(), Offer);
			P.Now += std::chrono::seconds(4);
			const auto Expired = P.Server.Poll(Peer, P.Scope, P.Authority.Adapters, P.Now);
			Check(
				Expired.Status == SpatialTransferStatus::Expired && !P.Authority.AtDestination(),
				"expired offer admitted a move"
			);
		}
		{
			Pair P;
			const auto OfferPacket = Packet(P.Begin(), Offer);
			const auto ReadyPacket = Packet(P.ClientReceive(OfferPacket), Ready);
			const auto Reserved = Packet(P.ServerReceive(ReadyPacket), Reserve);
			const auto HeldPacket = Packet(P.ClientReceive(Reserved), Held);
			const auto CommitPacket = Packet(P.ServerReceive(HeldPacket), Commit);
			P.Replica.Reservations.Reset();
			const auto Rejected = Packet(P.ClientReceive(CommitPacket), Cancel);
			Check(
				P.ServerReceive(Rejected).Status == SpatialTransferStatus::CommittedUnconfirmed &&
					P.Authority.AtDestination() && !P.Replica.AtDestination(),
				"lost controlling replica must require authoritative reconciliation"
			);
		}
		{
			Pair P;
			const auto OfferPacket = Packet(P.Begin(), Offer);
			const auto ReadyPacket = Packet(P.ClientReceive(OfferPacket), Ready);
			const auto Reserved = Packet(P.ServerReceive(ReadyPacket), Reserve);
			const auto HeldPacket = Packet(P.ClientReceive(Reserved), Held);
			const auto CommitPacket = Packet(P.ServerReceive(HeldPacket), Commit);
			const auto Result = P.Client.Receive(
				P.Scope,
				CommitPacket,
				P.Replica.Destination,
				P.Replica.Adapters,
				[&](const auto &Frame, const auto &Receipt) {
					P.Client.Reset();
					return P.Replica.Apply(Frame, Receipt);
				}
			);
			Check(
				Result.Status == SpatialTransferStatus::Applied && !Result.Reply && P.Replica.AtDestination(),
				"reentrant session shutdown during apply lost the native result"
			);
		}
	}
}
int main() try {
	BootstrapNativeRuntimeSchema();
	CheckedHandoff();
	Failures();
	std::cout << "[Network:SpatialTransfer] Independent native realms, portable proof, bilateral holds and revocation "
				 "passed.\n";
	return 0;
} catch (const std::exception &Error) {
	std::cerr << "[Network:SpatialTransfer] " << Error.what() << '\n';
	return 1;
}
