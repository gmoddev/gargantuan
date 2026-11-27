#include "gargantuan/classes/BlockGrid.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/KinematicCharacter.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/SpatialRegion.hpp"
#include "gargantuan/classes/Terrain.hpp"
#include "gargantuan/network/SpatialTransfer.hpp"
#include "../src/network/SpatialAudience.hpp"
#include "gargantuan/network/SpatialTransferProtection.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/services/Workspace.hpp"
#include "gargantuan/terrain/Field.hpp"

#include <array>
#include <iostream>
#include <stdexcept>

namespace {
	using namespace gargantuan;
	using namespace gargantuan::network;
	constexpr WireObjectId SourceKey{11, 1}, DestinationKey{12, 1}, SubjectKey{13, 1}, FloorKey{14, 1};
	constexpr WireObjectId ControllerKey{15, 1};
	constexpr ConnectionId Peer{2, 3}, OtherPeer{2, 4};
	const SpatialDestinationBounds Bounds{{-3, 0, -3}, {3, 6, 3}};

	void Check(bool Value, const char *Reason) {
		if (!Value) throw std::runtime_error(Reason);
	}

	void CheckAudienceFrames() {
		const ObjectId Domain{13,1}, Wall{14,1};
		SpatialAudienceFrame Stage{SpatialAudienceAction::Stage, 3, 9, Domain, Bounds, true, {Domain,Wall}};
		const auto Bytes = EncodeSpatialAudience(Stage);
		const auto Copy = DecodeSpatialAudience(Bytes);
		Check(Copy && Copy->Destination == Domain && Copy->Serial == 9 && Copy->Objects == Stage.Objects &&
			Copy->Bounds == Bounds && Copy->EmptyTerrain, "bounded audience manifest round trip");
		for (std::size_t N = 0; N < Bytes.size(); ++N)
			Check(!DecodeSpatialAudience(std::span(Bytes).first(N)), "truncated staging manifest");
		auto Bad = Bytes;
		Bad.push_back(std::byte{0});
		Check(!DecodeSpatialAudience(Bad), "trailing staging payload");
		Stage.Objects = {Wall, Domain};
		Check(!ValidSpatialAudience(Stage), "unsorted staging grants");
		Stage.Objects = {Wall};
		Check(!ValidSpatialAudience(Stage), "destination absent from grant");
		Stage.Objects = {Domain, Domain};
		Check(!ValidSpatialAudience(Stage), "repeated identities");
		Stage.Objects = {Domain};
		Stage.Bounds.Maximum.x = Stage.Bounds.Minimum.x + 513;
		Check(!ValidSpatialAudience(Stage), "oversize landing volume");
		Stage.Action = SpatialAudienceAction::Release;
		Check(!ValidSpatialAudience(Stage), "release cannot add members");
		Stage.Objects.clear();
		Check(DecodeSpatialAudience(EncodeSpatialAudience(Stage)).has_value(), "release round trip");
	}

	struct Realm {
		std::shared_ptr<DataModel> Game = std::make_shared<DataModel>();
		std::shared_ptr<Workspace> Source = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
		std::shared_ptr<SpatialRegion> Destination = std::make_shared<SpatialRegion>();
		std::shared_ptr<Part> Actor = std::make_shared<Part>(), Floor = std::make_shared<Part>();
		std::shared_ptr<KinematicCharacter> Avatar;
		SpatialTransferReservations Reservations;
		std::shared_ptr<SpatialTransferProtection> Protection;
		std::weak_ptr<const std::function<bool()>> LastReservation;
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
				if (Id == ControllerKey && Avatar) return Avatar->GetObjectId();
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
				auto &Book = Protection ? Protection->Reservations : Reservations;
				auto Hold = Book.Reserve(
					Actor->GetObjectId(), Frame, Receipt, [this] { return Permission; }, {Destination, Floor}
				);
				LastReservation = Hold;
				return Hold;
			};
		}
		void AddController() {
			Avatar = std::make_shared<KinematicCharacter>();
			Avatar->SetParent(Source);
			Avatar->SetCFrame(Actor->GetCFrame());
			Actor->SetParent(Avatar);
			Avatar->SetRootPart(Actor);
			Avatar->SetCapsuleRadius(1.f);
			Avatar->SetCapsuleHeight(4.f);
			Avatar->ApplyRuntimeControllerFacts({1, 0, 2}, {0, 1, 0}, true);
			Avatar->ApplyRuntimePresentationOffset(CFrame(.25f, 0, 0));
		}
		void Protect() {
			Actor->SetAnchored(true);
			Actor->SetLinearVelocity({0, 0, 0});
			Protection = Game->AcquireSpatialTransferProtection([this](ObjectId Id) {
				return Id == Actor->GetObjectId();
			});
		}
		~Realm() {
			Game->Destroy();
		}
		SpatialMovementRequest Movement() const {
			return {
				Actor->GetObjectId(),
				Source->GetObjectId(),
				Source->GetSpatialActivation(),
				Actor->GetCFrame(),
				CFrame(0, 3, 0)
			};
		}
		bool Apply(
			const SpatialTransferFrame &, const SpatialDestinationReceipt &Receipt, const SpatialTransferLease &Lease
		) {
			++Applications;
			return Destination->CommitSpatialTransfer(Actor, Movement(), Receipt, Lease).Succeeded();
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
				Scope,
				Frame,
				Replica.Destination,
				Replica.Adapters,
				[this](const auto &Message, const auto &Receipt, const auto &Lease) {
					return Replica.Apply(Message, Receipt, Lease);
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

	template <class F> bool Denied(F Action) {
		try {
			Action();
			return false;
		} catch (const std::exception &) {
			return true;
		}
	}
	void PreventBypasses() {
		using enum SpatialTransferPhase;
		Pair P;
		auto Shell = std::make_shared<Folder>();
		Shell->SetParent(P.Authority.Source);
		P.Authority.Actor->SetParent(Shell);
		auto Grid = std::make_shared<BlockGrid>();
		Grid->SetCFrame(CFrame(45, 0, 0));
		Grid->SetParent(P.Authority.Destination);
		auto Far = std::make_shared<Part>();
		Far->SetAnchored(true);
		Far->SetCFrame(CFrame(30, 1, 0));
		Far->SetParent(P.Authority.Destination);
		P.Authority.Protect();
		P.Replica.Protect();
		const std::array Required{P.Authority.Floor->GetObjectId()};
		const auto Native = P.Authority.Destination->PrepareSpatialDestination(Bounds, Required).value();
		const auto Fabricated = std::make_shared<const std::function<bool()>>([] { return true; });
		Check(
			!P.Authority.Destination
				 ->CommitSpatialTransfer(P.Authority.Actor, P.Authority.Movement(), Native, Fabricated)
				 .Succeeded(),
			"a lookalike grant admitted a controlled subject"
		);
		Check(
			Denied([&] { P.Authority.Actor->SetSpatialRegion(P.Authority.Destination); }) &&
				Denied([&] { P.Authority.Actor->SetCFrame(CFrame(5, 1, 0)); }) &&
				Denied([&] { P.Authority.Actor->ApplyImpulse({3, 0, 0}); }) &&
				Denied([&] { Shell->SetParent(P.Authority.Destination); }),
			"controlled root bypassed placement or ancestor admission"
		);
		const auto OfferPacket = Packet(P.Begin(), Offer);
		Check(
			!P.Authority.Destination
				 ->CommitSpatialTransfer(
					 P.Authority.Actor, P.Authority.Movement(), Native, P.Authority.LastReservation.lock()
				 )
				 .Succeeded(),
			"an authentic resource hold authorized a transfer before the controlling replica was ready"
		);
		Check(
			Denied([&] { P.Authority.Floor->SetSize({8, 8, 8}); }) && Denied([&] { P.Authority.Floor->Destroy(); }) &&
				!P.Authority.Floor->GetDestroyed() && Denied([&] { P.Authority.Destination->SetParent(nullptr); }) &&
				Denied([&] { Far->SetCFrame(CFrame(0, 3, 0)); }),
			"reserved provider, domain or incoming geometry bypassed admission"
		);
		Far->SetCFrame(CFrame(35, 1, 0));
		Check(
			Denied([&] { Grid->SetCanCollide(false); }) &&
				Denied([&] { P.Authority.Destination->RestoreTerrainField(std::make_unique<terrain::Field>()); }),
			"native terrain or private grid publication replaced a reserved closure"
		);
		auto Detached = std::make_shared<Part>();
		Detached->SetAnchored(true);
		Detached->SetCFrame(CFrame(0, 3, 0));
		Detached->SetSpatialRegion(P.Authority.Destination);
		Check(
			Denied([&] { Detached->SetParent(P.Authority.Source); }) && !Detached->GetParent(),
			"detached explicit membership entered a held footprint via Workspace"
		);
		const auto ReadyPacket = Packet(P.ClientReceive(OfferPacket), Ready);
		const auto Reserved = Packet(P.ServerReceive(ReadyPacket), Reserve);
		const auto Hold = Packet(P.ClientReceive(Reserved), Held);
		const auto Grant = Packet(P.ServerReceive(Hold), Commit);
		const auto Ack = Packet(P.ClientReceive(Grant), Applied);
		Check(
			P.ServerReceive(Ack).Status == SpatialTransferStatus::Applied && P.Authority.AtDestination() &&
				P.Replica.AtDestination(),
			"authentic bilateral grant failed on a protected actor"
		);
		P.Authority.Floor->SetSize({2, 2, 2});
		Pair Dynamic;
		auto Outside = std::make_shared<Part>();
		Outside->SetCFrame(CFrame(60, 0, 0));
		Outside->SetParent(Dynamic.Authority.Destination);
		Dynamic.Authority.Protect();
		Check(
			Dynamic.Begin().Status == SpatialTransferStatus::Denied && !Dynamic.Authority.AtDestination(),
			"unsupported independent dynamics promised immutability"
		);
	}

	void CharacterAdmission() {
		using enum SpatialTransferPhase;
		{
			Pair P;
			P.Authority.AddController();
			P.Replica.AddController();
			P.Scope.Controller = ControllerKey;
			P.Authority.Protect();
			P.Replica.Protect();
			const auto AuthorityPose = CFrame(12.25f,3,0)*CFrame::Angles(.13f,.31f,-.22f);
			const glm::vec3 Velocity(1.007f,-3.270000219f,.083f), Floor = glm::normalize(glm::vec3(.035f,1.f,-.093f));
			P.Authority.Avatar->ApplyRuntimeTransform(AuthorityPose);
			P.Authority.Avatar->ApplyRuntimeControllerFacts(Velocity,Floor,true);
			P.Replica.Avatar->ApplyRuntimeControllerFacts(glm::round(Velocity*64.f)/64.f,glm::round(Floor*32767.f)/32767.f,true);
			const auto OfferPacket = Packet(P.Begin(),Offer);
			Check(OfferPacket.ControllerVelocity==Velocity && OfferPacket.FloorNormal==Floor && OfferPacket.Grounded,
				"reliable controller source facts did not survive the transfer codec");
			Check(InstallSpatialControllerCheckpoint(OfferPacket,*P.Replica.Destination,P.Replica.Adapters) &&
				P.Replica.Actor->GetCFrame().Position!=AuthorityPose.Position, "an unbound native caller published a checkpoint");
			P.Replica.Adapters.AuthorizeControllerCheckpoint = [&](const SpatialTransferFrame &Frame) {
				return P.Replica.Permission && Frame.Scope == P.Scope && Frame.Serial == OfferPacket.Serial;
			};
			auto WrongGeometry = OfferPacket;
			WrongGeometry.SubjectGeometry = WrongGeometry.Collision;
			Check(InstallSpatialControllerCheckpoint(WrongGeometry,*P.Replica.Destination,P.Replica.Adapters) &&
				P.Replica.Actor->GetCFrame().Position!=AuthorityPose.Position,"a different subject geometry received a source correction");
			const SpatialDestinationBounds Headroom{{9.8f,4.15f,-.75f},{11.2f,5.4f,.75f}};
			const SpatialWireProvider SourceTerrain{SourceKey, SpatialCollisionProvider::Terrain};
			const auto Terrain = P.Replica.Source->GetTerrain();
			auto SecondAdapters = P.Replica.Adapters;
			SecondAdapters.ResolveObject = [&](WireObjectId Id) -> std::optional<ObjectId> {
				return Id == FloorKey ? P.Replica.Floor->GetObjectId() : P.Replica.Adapters.ResolveObject(Id);
			};
			SecondAdapters.Resolve = [&](SpatialWireProvider Key) -> std::optional<ObjectId> {
				return Key == SourceTerrain ? std::optional(Terrain->GetObjectId()) : std::nullopt;
			};
			SecondAdapters.Project = [&](const SpatialProviderReceipt &Native, const SpatialDestinationBounds &) -> std::optional<SpatialAppliedBinding> {
				if (Native.Owner != Terrain->GetObjectId() || !Native.AuthoritativeEmpty) return {};
				return SpatialAppliedBinding{SourceTerrain, AssetContentId::Hash(std::array<std::uint8_t,1>{7}), 1, 1, true, true};
			};
			SecondAdapters.Reserve = [&](const SpatialTransferFrame &Frame, const SpatialDestinationReceipt &Receipt) {
				return P.Replica.Protection->Reservations.Reserve(P.Replica.Floor->GetObjectId(), Frame, Receipt, [] { return true; });
			};
			SpatialTransferServer Second;
			const SpatialMovementRequest Neighbour{P.Replica.Floor->GetObjectId(), P.Replica.Destination->GetObjectId(),
				P.Replica.Destination->GetSpatialActivation(), P.Replica.Floor->GetCFrame(), CFrame(10.5f,4.75f,0)};
			(void)Packet(Second.Begin(OtherPeer, {6,7,12,10,FloorKey,DestinationKey,SourceKey}, P.Replica.Floor,
				P.Replica.Source, Neighbour, Headroom, {}, SecondAdapters, P.Now), Offer);
			Check(InstallSpatialControllerCheckpoint(OfferPacket,*P.Replica.Destination,P.Replica.Adapters) &&
				P.Replica.Actor->GetCFrame().Position==glm::vec3(10,3,0), "source correction crossed another capsule reservation");
			(void)Packet(Second.Cancel(), Cancel);
			bool Observed{};
			const auto Observation = P.Replica.Actor->GetPropertyChangedSignal("CFrame")->Connect([&](std::monostate) {
				const auto Hit = P.Replica.Source->ResolveRaycast(AuthorityPose.Position + glm::vec3(0,3,0),{0,-6,0});
				Observed = Hit.Instance==P.Replica.Actor && !P.Replica.Source->ResolveRaycast({10,6,0},{0,-6,0}).HasHit() &&
					P.Replica.Avatar->GetCFrame().Rotation==AuthorityPose.Rotation && P.Replica.Avatar->GetVelocity()==Velocity &&
					P.Replica.Avatar->GetFloorNormal()==Floor && P.Replica.Avatar->GetGrounded();
			});
			Check(!InstallSpatialControllerCheckpoint(OfferPacket,*P.Replica.Destination,P.Replica.Adapters) && Observed &&
				P.Replica.Actor->GetCFrame().Rotation==AuthorityPose.Rotation && P.Replica.Actor->GetCFrame().Position==AuthorityPose.Position &&
				P.Replica.Avatar->GetVelocity()==Velocity && P.Replica.Avatar->GetFloorNormal()==Floor,
				"fenced source checkpoint failed to recover a delayed pose and compact controller facts");
			Observation->Disconnect();
			const auto ReadyPacket = Packet(P.ClientReceive(OfferPacket),Ready);
			auto Changed = ReadyPacket;
			Changed.ControllerVelocity.x += .001f;
			Check(!P.ServerReceive(Changed).Reply,"modified controller facts advanced a grant");
			const auto Reserved=Packet(P.ServerReceive(ReadyPacket),Reserve);
			P.Replica.Avatar->ApplyRuntimeControllerFacts(Velocity+glm::vec3(.001f,0,0),Floor,true);
			(void)Packet(P.ClientReceive(Reserved),Cancel);
			Check(!P.Replica.AtDestination(),"a later local change within the same compact velocity bin went undetected");
			P.Replica.Avatar->ApplyRuntimeTransform(CFrame(10,3,0));
			const auto Revoke = P.Replica.Actor->GetPropertyChangedSignal("CFrame")->Connect([&](std::monostate) {
				P.Replica.Permission = false;
			});
			Check(InstallSpatialControllerCheckpoint(OfferPacket,*P.Replica.Destination,P.Replica.Adapters) &&
				!P.Replica.Permission && !P.Replica.AtDestination(), "a checkpoint callback retired its grant and still produced readiness");
			Revoke->Disconnect();
		}
		{
			Pair P;
			P.Authority.AddController();
			P.Replica.AddController();
			P.Scope.Controller = ControllerKey;
			P.Authority.Floor->SetCFrame(CFrame(0, 4.55f, 0));
			P.Authority.Floor->SetCollisionGroup(7);
			P.Authority.Floor->SetCollisionMask(1 << 7);
			Check(
				P.Begin().Status == SpatialTransferStatus::Denied && !P.Authority.AtDestination() &&
					P.Authority.Avatar->GetGrounded(),
				"root box missed the controller's occupied headroom or a foreign collision group"
			);
		}
		{
			Pair P;
			P.Authority.AddController();
			P.Replica.AddController();
			P.Scope.Controller = ControllerKey;
			const auto Sibling = std::make_shared<Part>();
			Sibling->SetParent(P.Authority.Avatar);
			Check(
				P.Begin().Status == SpatialTransferStatus::Denied && !P.Authority.AtDestination(),
				"a multi-body avatar was split"
			);
		}
		{
			Pair P;
			P.Authority.AddController();
			P.Replica.AddController();
			P.Scope.Controller = ControllerKey;
			P.Replica.Avatar->SetCapsuleHeight(5.f);
			const auto OfferPacket = Packet(P.Begin(), Offer);
			Check(
				P.ClientReceive(OfferPacket).Status != SpatialTransferStatus::Progress,
				"different installed capsule was marked ready"
			);
		}
		{
			Pair P;
			P.Authority.AddController();
			P.Replica.AddController();
			P.Scope.Controller = ControllerKey;
			P.Authority.Protect();
			P.Replica.Protect();
			const SpatialDestinationBounds TooSmall{{-.6f, 2.4f, -.6f}, {.6f, 3.6f, .6f}};
			const auto Tiny = P.Authority.Destination->PrepareSpatialDestination(TooSmall).value();
			Check(
				!P.Authority.Destination->CheckSpatialLanding(P.Authority.Actor, P.Authority.Movement(), Tiny)
					 .Succeeded(),
				"partial capsule coverage accepted"
			);
			const auto OfferPacket = Packet(P.Begin(), Offer);
			const auto ReadyPacket = Packet(P.ClientReceive(OfferPacket), Ready);
			const auto ReservePacket = Packet(P.ServerReceive(ReadyPacket), Reserve);
			const auto HeldPacket = Packet(P.ClientReceive(ReservePacket), Held);
			const auto Committed = Packet(P.ServerReceive(HeldPacket), Commit);
			const auto Ack = Packet(P.ClientReceive(Committed), Applied);
			Check(P.ServerReceive(Ack).Status == SpatialTransferStatus::Applied, "character acknowledgment");
			for (const auto *Side : {&P.Authority, &P.Replica})
				Check(
					Side->AtDestination() && Side->Avatar->GetCFrame().FuzzyEq(Side->Actor->GetCFrame()) &&
						Side->Actor->GetParent().value_or(nullptr) == Side->Avatar && !Side->Avatar->GetGrounded() &&
						Side->Avatar->GetFloorNormal() == glm::vec3(0) &&
						Side->Avatar->GetVelocity() == glm::vec3(1, 0, 2) &&
						Side->Avatar->GetPresentationCFrame().FuzzyEq(Side->Actor->GetCFrame()),
					"admitted move must preserve character identity/velocity and retire old floor and presentation "
					"state"
				);
		}
	}
	void Failures() {
		using enum SpatialTransferPhase;
		{
			Pair P;
			auto &Local = P.Authority;
			const CFrame Redirect(6,3,0);
			unsigned Published{};
			const auto PoseSignal = Local.Actor->GetPropertyChangedSignal("CFrame")->Connect([&](std::monostate) {
				if (Local.Actor->GetCFrame().Position == Redirect.Position) ++Published;
			});
			const auto DomainSignal = Local.Actor->GetPropertyChangedSignal("SpatialRegion")->Connect([&](std::monostate) {
				Local.Actor->SetCFrame(Redirect);
			});
			const auto Receipt = Local.Destination->PrepareSpatialDestination(Bounds);
			Check(Receipt && Local.Destination->CommitSpatialTransfer(Local.Actor, Local.Movement(), *Receipt).Succeeded() &&
				Local.AtDestination() && Local.Actor->GetCFrame().Position == Redirect.Position && Published == 1,
				"a domain callback superseded the pose but the old transfer still published it");
			DomainSignal->Disconnect();
			PoseSignal->Disconnect();
		}
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
				[&](const auto &Frame, const auto &Receipt, const auto &Lease) {
					P.Client.Reset();
					return P.Replica.Apply(Frame, Receipt, Lease);
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
	CheckAudienceFrames();
	CheckedHandoff();
	PreventBypasses();
	CharacterAdmission();
	Failures();
	std::cout << "[Network:SpatialTransfer] Independent native realms, portable proof, bilateral holds and revocation "
				 "passed.\n";
	return 0;
} catch (const std::exception &Error) {
	std::cerr << "[Network:SpatialTransfer] " << Error.what() << '\n';
	return 1;
}
