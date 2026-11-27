#include "gargantuan/network/SpatialTransfer.hpp"

#include "gargantuan/classes/BasePart.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/WorldRoot.hpp"
#include "gargantuan/network/BinaryCodec.hpp"
#include "gargantuan/network/SpatialTransferProtection.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>

namespace gargantuan::network {
	namespace {
		constexpr std::uint32_t Magic = 0x32545347;
		constexpr std::uint8_t Version = 1;
		constexpr auto Lifetime = std::chrono::seconds(3);

		void Put(GameBinaryWriter &Out, WireObjectId Id) {
			Out.Integer(Id.Slot);
			Out.Integer(Id.Generation);
		}
		bool Get(GameBinaryReader &In, WireObjectId &Id) {
			return In.Integer(Id.Slot) && In.Integer(Id.Generation) && Id.IsValid();
		}
		void Put(GameBinaryWriter &Out, const AssetContentId &Id) {
			Out.Append(std::as_bytes(std::span(Id.Bytes)));
		}
		bool Get(GameBinaryReader &In, AssetContentId &Id) {
			std::span<const std::byte> Bytes;
			if (!In.Take(Id.Bytes.size(), Bytes)) return false;
			for (std::size_t I = 0; I < Bytes.size(); ++I)
				Id.Bytes[I] = std::to_integer<std::uint8_t>(Bytes[I]);
			return Id.IsValid();
		}
		void Put(GameBinaryWriter &Out, const glm::vec3 &Value) {
			for (unsigned Index = 0; Index < 3; ++Index)
				Out.Float(Value[Index] == 0.f ? 0.f : Value[Index]);
		}
		bool Get(GameBinaryReader &In, glm::vec3 &Value) {
			return In.Float(Value.x) && In.Float(Value.y) && In.Float(Value.z);
		}
		void Put(GameBinaryWriter &Out, const SpatialWireProvider &Key) {
			Put(Out, Key.Owner);
			Out.Integer(std::uint8_t(Key.Kind));
		}

		WireCFrame WirePose(const CFrame &Pose) {
			WireCFrame Out{{Pose.Position.x, Pose.Position.y, Pose.Position.z}};
			for (unsigned I = 0; I < 3; ++I)
				for (unsigned J = 0; J < 3; ++J)
					Out.Components[3 + I * 3 + J] = Pose.Rotation[I][J];
			return Out;
		}
		bool ValidPose(const WireCFrame &Pose) {
			if (!std::ranges::all_of(Pose.Components, [](float V) { return std::isfinite(V); })) return false;
			glm::mat3 R;
			for (unsigned I = 0; I < 3; ++I)
				for (unsigned J = 0; J < 3; ++J)
					R[I][J] = Pose.Components[3 + I * 3 + J];
			for (unsigned I = 0; I < 3; ++I)
				for (unsigned J = 0; J < 3; ++J)
					if (std::abs(glm::dot(R[I], R[J]) - (I == J ? 1.f : 0.f)) > 1.e-4f) return false;
			return glm::determinant(R) > 0.f;
		}
		bool Valid(const SpatialTransferScope &Scope) {
			return Scope.SessionEpoch && Scope.WorldEpoch && Scope.FocusEpoch && Scope.PlayerId &&
				   Scope.Subject.IsValid() && Scope.Source.IsValid() && Scope.Destination.IsValid();
		}
		bool Valid(const SpatialTransferFrame &Frame) {
			if (Frame.Phase < SpatialTransferPhase::Offer || Frame.Phase > SpatialTransferPhase::Cancel ||
				!Valid(Frame.Scope) || !Frame.Serial || !Frame.Collision.IsValid() || !ValidPose(Frame.SourcePose) ||
				!ValidPose(Frame.DestinationPose) || Frame.Owners.empty() ||
				Frame.Owners.size() > MaximumSpatialTransferProviders || !std::ranges::is_sorted(Frame.Owners) ||
				std::adjacent_find(Frame.Owners.begin(), Frame.Owners.end()) != Frame.Owners.end())
				return false;
			for (int I = 0; I < 3; ++I)
				if (!std::isfinite(Frame.Bounds.Minimum[I]) || !std::isfinite(Frame.Bounds.Maximum[I]) ||
					Frame.Bounds.Minimum[I] > Frame.Bounds.Maximum[I] || !std::isfinite(Frame.LinearVelocity[I]) ||
					!std::isfinite(Frame.AngularVelocity[I]))
					return false;
			return std::ranges::all_of(Frame.Owners, [](const auto &Key) {
				return Key.Owner.IsValid() && Key.Kind <= SpatialCollisionProvider::BlockGrid;
			});
		}
		bool Same(const SpatialTransferFrame &A, const SpatialTransferFrame &B) {
			return A.Scope == B.Scope && A.Serial == B.Serial && A.Bounds == B.Bounds && A.Collision == B.Collision &&
				   A.SourcePose == B.SourcePose && A.DestinationPose == B.DestinationPose &&
				   A.LinearVelocity == B.LinearVelocity && A.AngularVelocity == B.AngularVelocity &&
				   A.Owners == B.Owners;
		}

		struct Evidence {
			std::vector<SpatialWireProvider> Owners;
			AssetContentId Digest;
		};
		std::optional<Evidence>
		Project(const SpatialDestinationReceipt &Receipt, const SpatialTransferAdapters &Adapters) {
			if (!Adapters.Project || !Adapters.Resolve || Receipt.Providers.empty() ||
				Receipt.Providers.size() > MaximumSpatialTransferProviders)
				return {};
			struct Entry {
				SpatialAppliedBinding Binding;
				const SpatialProviderReceipt *Native;
			};
			std::vector<Entry> Entries;
			for (const auto &Native : Receipt.Providers) {
				const auto Binding = Adapters.Project(Native, Receipt.Bounds);
				if (!Binding || !Binding->Applied || !Binding->Content.IsValid() || !Binding->CoverageRevision ||
					!Binding->Owner.Owner.IsValid() || Binding->Owner.Kind != Native.Provider ||
					Adapters.Resolve(Binding->Owner) != Native.Owner ||
					(Native.AuthoritativeEmpty && !Binding->Empty) ||
					(Binding->Empty && !Native.AuthoritativeEmpty && !Native.Installed.empty()))
					return {};
				Entries.push_back({*Binding, &Native});
			}
			std::ranges::sort(Entries, {}, [](const Entry &Value) { return Value.Binding.Owner; });
			// Chunk and piece coordinates are shared geometry facts. Native body IDs and local derivative
			// counters are checked by ValidateSpatialDestination and never enter a wire digest.
			constexpr std::size_t MaximumFootprintPieces = 1024;
			std::size_t Pieces{};
			GameBinaryWriter Out(48 * 1024);
			Put(Out, Receipt.Bounds.Minimum);
			Put(Out, Receipt.Bounds.Maximum);
			Evidence Result;
			for (const auto &[B, N] : Entries) {
				if (!Result.Owners.empty() && Result.Owners.back() == B.Owner) return {};
				Result.Owners.push_back(B.Owner);
				Put(Out, B.Owner);
				Put(Out, B.Content);
				Out.Integer(B.LogicalRevision);
				Out.Integer(B.CoverageRevision);
				Out.Integer(std::uint8_t(B.Empty));
				for (const float V : WirePose(N->Pose).Components)
					Out.Float(V == 0.f ? 0.f : V);
				Put(Out, N->Size);
				Out.Integer(std::uint8_t(N->Shape));
				Out.Integer(N->CollisionGroup);
				Out.Integer(N->CollisionMask);
				if (N->Installed.size() > MaximumFootprintPieces - Pieces) return {};
				Pieces += N->Installed.size();
				using Piece = std::tuple<std::array<std::int64_t, 3>, std::uint8_t, std::uint8_t, std::uint8_t>;
				std::vector<Piece> Footprint;
				for (const auto &Native : N->Installed)
					Footprint.emplace_back(Native.Chunk, Native.CatalogGeometry, Native.Orientation, Native.Piece);
				std::ranges::sort(Footprint);
				Out.Integer(std::uint16_t(Footprint.size()));
				for (const auto &[Coordinates, Geometry, Orientation, Subpiece] : Footprint) {
					for (const auto Axis : Coordinates)
						Out.Integer(Axis);
					Out.Integer(Geometry);
					Out.Integer(Orientation);
					Out.Integer(Subpiece);
				}
			}
			if (!Out.Succeeded()) return {};
			const auto *Data = reinterpret_cast<const std::uint8_t *>(Out.Bytes.data());
			Result.Digest = AssetContentId::Hash({Data, Out.Bytes.size()});
			return Result;
		}

		struct HeldState {
			SpatialTransferFrame Frame;
			std::shared_ptr<WorldRoot> Destination;
			SpatialDestinationReceipt Receipt;
			SpatialTransferLease Lease;
			bool Committing{};
		};
		bool LocalSource(
			const SpatialTransferFrame &Frame, const WorldRoot &Destination, const SpatialTransferAdapters &Adapters
		) {
			if (!Adapters.ResolveObject || Adapters.ResolveObject(Frame.Scope.Destination) != Destination.GetObjectId())
				return false;
			const auto Id = Adapters.ResolveObject(Frame.Scope.Subject);
			const auto Native = Id ? std::dynamic_pointer_cast<BasePart>(ObjectRegistry::Get().Lookup(*Id)) : nullptr;
			const auto Source = Native ? WorldRoot::GetEffectiveWorld(*Native) : nullptr;
			return Source && Source->GetDataModel() == Destination.GetDataModel() &&
				   Adapters.ResolveObject(Frame.Scope.Source) == Source->GetObjectId() &&
				   WirePose(Native->GetCFrame()) == Frame.SourcePose &&
				   Native->GetLinearVelocity() == Frame.LinearVelocity &&
				   Native->GetAngularVelocity() == Frame.AngularVelocity;
		}
		bool EvidenceIsCurrent(const HeldState &State, const SpatialTransferAdapters &Adapters) {
			if (!State.Destination || !LocalSource(State.Frame, *State.Destination, Adapters) ||
				!State.Destination->ValidateSpatialDestination(State.Receipt))
				return false;
			const auto Proof = Project(State.Receipt, Adapters);
			return Proof && Proof->Digest == State.Frame.Collision && Proof->Owners == State.Frame.Owners &&
				   (!State.Lease || (*State.Lease)());
		}
		SpatialTransferStep Send(
			HeldState &State,
			SpatialTransferPhase Phase,
			SpatialTransferStatus Outcome = SpatialTransferStatus::Progress
		) {
			State.Frame.Phase = Phase;
			return {Outcome, State.Frame};
		}
	}

	bool IsSpatialTransfer(std::span<const std::byte> Bytes) {
		GameBinaryReader In(Bytes);
		std::uint32_t Tag{};
		return In.Integer(Tag) && Tag == Magic;
	}
	std::vector<std::byte> EncodeSpatialTransfer(const SpatialTransferFrame &Frame) {
		if (!Valid(Frame)) return {};
		GameBinaryWriter Out(MaximumSpatialTransferBytes);
		Out.Integer(Magic);
		Out.Integer(Version);
		Out.Integer(std::uint8_t(Frame.Phase));
		const auto &Scope = Frame.Scope;
		Out.Integer(Scope.SessionEpoch);
		Out.Integer(Scope.WorldEpoch);
		Out.Integer(Scope.FocusEpoch);
		Out.Integer(Scope.PlayerId);
		Put(Out, Scope.Subject);
		Put(Out, Scope.Source);
		Put(Out, Scope.Destination);
		Out.Integer(Frame.Serial);
		Put(Out, Frame.Bounds.Minimum);
		Put(Out, Frame.Bounds.Maximum);
		for (const auto V : Frame.SourcePose.Components)
			Out.Float(V);
		for (const auto V : Frame.DestinationPose.Components)
			Out.Float(V);
		Put(Out, Frame.LinearVelocity);
		Put(Out, Frame.AngularVelocity);
		Put(Out, Frame.Collision);
		Out.Integer(std::uint8_t(Frame.Owners.size()));
		for (const auto &Key : Frame.Owners)
			Put(Out, Key);
		return Out.Succeeded() ? std::move(Out.Bytes) : std::vector<std::byte>{};
	}
	std::optional<SpatialTransferFrame> DecodeSpatialTransfer(std::span<const std::byte> Bytes) {
		if (Bytes.size() > MaximumSpatialTransferBytes) return {};
		GameBinaryReader In(Bytes);
		SpatialTransferFrame Frame;
		std::uint32_t Tag{};
		std::uint8_t V{}, Phase{}, Count{};
		auto &Scope = Frame.Scope;
		if (!In.Integer(Tag) || Tag != Magic || !In.Integer(V) || V != Version || !In.Integer(Phase) ||
			!In.Integer(Scope.SessionEpoch) || !In.Integer(Scope.WorldEpoch) || !In.Integer(Scope.FocusEpoch) ||
			!In.Integer(Scope.PlayerId) || !Get(In, Scope.Subject) || !Get(In, Scope.Source) ||
			!Get(In, Scope.Destination) || !In.Integer(Frame.Serial) || !Get(In, Frame.Bounds.Minimum) ||
			!Get(In, Frame.Bounds.Maximum))
			return {};
		Frame.Phase = static_cast<SpatialTransferPhase>(Phase);
		for (auto &F : Frame.SourcePose.Components)
			if (!In.Float(F)) return {};
		for (auto &F : Frame.DestinationPose.Components)
			if (!In.Float(F)) return {};
		if (!Get(In, Frame.LinearVelocity) || !Get(In, Frame.AngularVelocity) || !Get(In, Frame.Collision) ||
			!In.Integer(Count) || !Count || Count > MaximumSpatialTransferProviders ||
			In.Remaining() != std::size_t(Count) * 9)
			return {};
		Frame.Owners.resize(Count);
		for (auto &Owner : Frame.Owners) {
			std::uint8_t Kind{};
			if (!Get(In, Owner.Owner) || !In.Integer(Kind)) return {};
			Owner.Kind = static_cast<SpatialCollisionProvider>(Kind);
		}
		return In.Complete() && Valid(Frame) ? std::optional(std::move(Frame)) : std::nullopt;
	}

	struct SpatialTransferReservations::Table {
		struct Record {
			ObjectId Subject, Source, Domain;
			SpatialTransferFrame Frame;
			SpatialDestinationReceipt Receipt;
			std::weak_ptr<const std::function<bool()>> Grant;
			SpatialDestinationBounds Bounds;
			std::vector<ObjectId> Owners;
			std::vector<std::shared_ptr<const void>> Resources;
			std::function<bool()> Authorized;
			bool Active{true}, Armed{}, Consumed{};
		};
		std::vector<std::weak_ptr<Record>> Live;
		template <class T> bool Contains(T Predicate) const {
			return std::ranges::any_of(Live, [&](const auto &Reference) {
				const auto Hold = Reference.lock();
				return Hold && Hold->Active && Predicate(*Hold);
			});
		}
	};
	SpatialTransferReservations::SpatialTransferReservations() : Records(std::make_unique<Table>()) {}
	SpatialTransferReservations::~SpatialTransferReservations() {
		Reset();
	}
	void SpatialTransferReservations::Reset() {
		for (const auto &Entry : Records->Live)
			if (const auto Hold = Entry.lock()) Hold->Active = false;
		Records->Live.clear();
	}
	bool SpatialTransferReservations::HasHolds() const {
		return Records->Contains([](const auto &) { return true; });
	}
	bool SpatialTransferReservations::HoldsMember(ObjectId Member) const {
		return Records->Contains([&](const auto &Hold) { return Hold.Subject == Member; });
	}
	bool SpatialTransferReservations::HoldsDestination(ObjectId Domain) const {
		return Records->Contains([&](const auto &Hold) { return Hold.Domain == Domain; });
	}
	bool SpatialTransferReservations::HoldsDomain(ObjectId Domain) const {
		return Records->Contains([&](const auto &Hold) { return Hold.Source == Domain || Hold.Domain == Domain; });
	}
	bool SpatialTransferReservations::ApplyBilateral(
		const SpatialTransferLease &Lease, const SpatialTransferFrame &Frame, const std::function<bool()> &Apply
	) {
		for (const auto &Weak : Records->Live) {
			const auto Hold = Weak.lock();
			if (!Hold || !Hold->Active || Hold->Armed || Hold->Consumed || Hold->Grant.lock() != Lease ||
				!Same(Hold->Frame, Frame))
				continue;
			Hold->Armed = true;
			struct Guard {
				Table::Record &Hold;
				~Guard() {
					Hold.Armed = false;
				}
			} Clear{*Hold};
			return Apply() && Hold->Consumed;
		}
		return false;
	}
	bool SpatialTransferReservations::Admits(
		const SpatialTransferLease &Lease,
		const SpatialMovementRequest &Movement,
		const SpatialDestinationReceipt &Receipt
	) const {
		return Lease && Records->Contains([&](auto &Hold) {
			if (!Hold.Armed || Hold.Consumed || Hold.Grant.lock() != Lease || Hold.Subject != Movement.Member ||
				Hold.Source != Movement.SourceDomain || Hold.Receipt != Receipt ||
				Hold.Frame.SourcePose != WirePose(Movement.SourcePose) ||
				Hold.Frame.DestinationPose != WirePose(Movement.DestinationPose) || !(*Lease)())
				return false;
			Hold.Consumed = true;
			return true;
		});
	}
	bool SpatialTransferReservations::HoldsProvider(ObjectId Owner) const {
		return Records->Contains([&](const auto &Hold) {
			return std::ranges::find(Hold.Owners, Owner) != Hold.Owners.end();
		});
	}
	bool SpatialTransferReservations::HoldsBounds(ObjectId Domain, const SpatialDestinationBounds &Bounds) const {
		return Records->Contains([&](const auto &Hold) {
			if (Hold.Domain != Domain) return false;
			for (unsigned I = 0; I < 3; ++I)
				if (Bounds.Minimum[I] > Hold.Bounds.Maximum[I] || Bounds.Maximum[I] < Hold.Bounds.Minimum[I])
					return false;
			return true;
		});
	}
	SpatialTransferLease SpatialTransferReservations::Reserve(
		ObjectId Subject,
		const SpatialTransferFrame &Frame,
		const SpatialDestinationReceipt &Receipt,
		std::function<bool()> Authorized,
		std::vector<std::shared_ptr<const void>> Resources
	) {
		std::erase_if(Records->Live, [](const auto &Entry) { return Entry.expired(); });
		if (!Subject.IsValid() || !Receipt.Realm.IsValid() || !Receipt.Domain.IsValid() || !Receipt.Activation ||
			!Valid(Frame) || Frame.Bounds != Receipt.Bounds || Receipt.Providers.empty() ||
			Receipt.Providers.size() != Frame.Owners.size() || Records->Live.size() >= 32 || !Authorized ||
			!Authorized() || HoldsMember(Subject) || HoldsBounds(Receipt.Domain, Receipt.Bounds))
			return {};
		auto Hold = std::make_shared<Table::Record>();
		const auto Instance = ObjectRegistry::Get().Lookup(Subject);
		const auto Source = Instance ? WorldRoot::GetEffectiveWorld(*Instance) : nullptr;
		if (!Source || Source->GetReplicationScopeId() != Receipt.Realm) return {};
		if (const auto Realm = Source->GetDataModel())
			if (const auto Protection = Realm->GetSpatialTransferProtection()) {
				const auto Destination = std::dynamic_pointer_cast<WorldRoot>(
					ObjectRegistry::Get().Lookup(Receipt.Domain)
				);
				const auto Part = std::dynamic_pointer_cast<BasePart>(Instance);
				if (&Protection->Reservations != this || !Destination || !Part ||
					!Source->CanHoldSpatialSubject(Subject) || !Destination->CanHoldSpatialCollision(Subject) ||
					(!Part->GetAnchored() &&
					 (!Part->GetKinematic() || glm::dot(Part->GetLinearVelocity(), Part->GetLinearVelocity()) != 0.f ||
					  glm::dot(Part->GetAngularVelocity(), Part->GetAngularVelocity()) != 0.f)))
					return {};
			}
		Hold->Source = Source->GetObjectId();
		Hold->Frame = Frame;
		Hold->Receipt = Receipt;
		Hold->Subject = Subject;
		Hold->Domain = Receipt.Domain;
		Hold->Bounds = Receipt.Bounds;
		for (const auto &Provider : Receipt.Providers)
			Hold->Owners.push_back(Provider.Owner);
		Hold->Resources = std::move(Resources);
		Hold->Authorized = std::move(Authorized);
		Records->Live.push_back(Hold);
		auto Grant = std::make_shared<const std::function<bool()>>([Hold] {
			return Hold->Active && Hold->Authorized();
		});
		Hold->Grant = Grant;
		return Grant;
	}

	struct SpatialTransferServer::State : HeldState {
		ConnectionId Connection;
		std::shared_ptr<BasePart> Subject;
		SpatialMovementRequest Movement;
		SpatialTransferClock::time_point Deadline;
	};
	SpatialTransferServer::SpatialTransferServer() = default;
	SpatialTransferServer::~SpatialTransferServer() = default;
	void SpatialTransferServer::Reset() {
		++Mutation;
		Current.reset();
	}

	SpatialTransferStep SpatialTransferServer::Cancel() {
		if (!Current) return {};
		if (Current->Committing) return {SpatialTransferStatus::Busy, {}};
		if (Current->Frame.Phase == SpatialTransferPhase::Commit ||
			Current->Frame.Phase == SpatialTransferPhase::Applied) {
			const auto Result = Current->Frame.Phase == SpatialTransferPhase::Applied
									? SpatialTransferStatus::Applied
									: SpatialTransferStatus::CommittedUnconfirmed;
			Reset();
			return {Result, {}};
		}
		auto Rejection = Send(*Current, SpatialTransferPhase::Cancel, SpatialTransferStatus::Canceled);
		Reset();
		return Rejection;
	}
	SpatialTransferStep SpatialTransferServer::Begin(
		ConnectionId Connection,
		const SpatialTransferScope &Scope,
		const std::shared_ptr<BasePart> &Subject,
		const std::shared_ptr<WorldRoot> &Destination,
		const SpatialMovementRequest &Movement,
		const SpatialDestinationBounds &Bounds,
		std::span<const ObjectId> RequiredOwners,
		const SpatialTransferAdapters &Adapters,
		SpatialTransferClock::time_point Now
	) {
		if (Current && Current->Frame.Phase != SpatialTransferPhase::Applied) return {SpatialTransferStatus::Busy, {}};
		const auto Started = ++Mutation;
		if (!Connection.IsValid() || !Valid(Scope) || !Subject || !Destination || !Adapters.Reserve ||
			!Adapters.ResolveObject || LastSerial == std::numeric_limits<std::uint64_t>::max() ||
			Subject->GetObjectId() != Movement.Member || Adapters.ResolveObject(Scope.Subject) != Movement.Member ||
			Adapters.ResolveObject(Scope.Source) != Movement.SourceDomain ||
			Adapters.ResolveObject(Scope.Destination) != Destination->GetObjectId())
			return {SpatialTransferStatus::Denied, {}};
		const auto Source = WorldRoot::GetEffectiveWorld(*Subject);
		if (!Source || Source->GetObjectId() != Movement.SourceDomain ||
			Source->GetSpatialActivation() != Movement.SourceActivation)
			return {SpatialTransferStatus::Stale, {}};
		auto Native = Destination->PrepareSpatialDestination(Bounds, RequiredOwners);
		if (!Native) return {SpatialTransferStatus::Denied, {}};
		auto Proof = Project(*Native, Adapters);
		if (!Proof) return {SpatialTransferStatus::Denied, {}};
		auto Next = std::make_shared<State>();
		Next->Connection = Connection;
		Next->Subject = Subject;
		Next->Destination = Destination;
		Next->Movement = Movement;
		Next->Receipt = std::move(*Native);
		Next->Deadline = Now + Lifetime;
		Next->Frame = {
			SpatialTransferPhase::Offer,
			Scope,
			LastSerial + 1,
			Bounds,
			WirePose(Movement.SourcePose),
			WirePose(Movement.DestinationPose),
			Subject->GetLinearVelocity(),
			Subject->GetAngularVelocity(),
			std::move(Proof->Owners),
			Proof->Digest
		};
		if (!Valid(Next->Frame) || !LocalSource(Next->Frame, *Destination, Adapters))
			return {SpatialTransferStatus::Denied, {}};
		Next->Lease = Adapters.Reserve(Next->Frame, Next->Receipt);
		if (!Next->Lease || !EvidenceIsCurrent(*Next, Adapters) || Started != Mutation)
			return {SpatialTransferStatus::Denied, {}};
		++LastSerial;
		Current = std::move(Next);
		return {SpatialTransferStatus::Progress, Current->Frame};
	}
	SpatialTransferStep SpatialTransferServer::Poll(
		ConnectionId Connection,
		const SpatialTransferScope &Scope,
		const SpatialTransferAdapters &Adapters,
		SpatialTransferClock::time_point Now
	) {
		if (!Current || Connection != Current->Connection) return {};
		if (Current->Committing) return {SpatialTransferStatus::Busy, {}};
		if (Scope != Current->Frame.Scope) {
			auto Result = Cancel();
			Result.Reply.reset();
			return Result;
		}
		if (Current->Frame.Phase == SpatialTransferPhase::Applied) return {SpatialTransferStatus::Applied, {}};
		if (Now >= Current->Deadline) {
			auto Result = Cancel();
			if (Result.Status == SpatialTransferStatus::Canceled) Result.Status = SpatialTransferStatus::Expired;
			return Result;
		}
		const auto Before = Current;
		if (Before->Frame.Phase != SpatialTransferPhase::Commit) {
			const bool Valid = EvidenceIsCurrent(*Before, Adapters);
			if (Before != Current) return {};
			if (!Valid) return Cancel();
		}
		return {SpatialTransferStatus::Progress, {}};
	}
	SpatialTransferStep SpatialTransferServer::Receive(
		ConnectionId Connection,
		const SpatialTransferScope &Scope,
		const SpatialTransferFrame &Frame,
		const SpatialTransferAdapters &Adapters,
		SpatialTransferClock::time_point Now
	) {
		if (!Current || Connection != Current->Connection || !Valid(Frame) || !Same(Frame, Current->Frame)) return {};
		auto Status = Poll(Connection, Scope, Adapters, Now);
		if (Status.Status != SpatialTransferStatus::Progress) return Status;
		using enum SpatialTransferPhase;
		if (Frame.Phase == SpatialTransferPhase::Cancel) return this->Cancel();
		const auto Keep = Current;
		auto &State = *Keep;
		if (Frame.Phase == Ready && (State.Frame.Phase == Offer || State.Frame.Phase == Reserve))
			return Send(State, Reserve);
		if (Frame.Phase == Held && State.Frame.Phase == Commit) return {SpatialTransferStatus::Progress, State.Frame};
		if (Frame.Phase == Held && State.Frame.Phase == Reserve) {
			State.Committing = true;
			const auto Apply = [&] {
				return State.Destination
					->CommitSpatialTransfer(State.Subject, State.Movement, State.Receipt, State.Lease)
					.Succeeded();
			};
			const auto Realm = State.Destination->GetDataModel();
			const auto Protection = Realm ? Realm->GetSpatialTransferProtection() : nullptr;
			const bool Moved = Protection ? Protection->Reservations.ApplyBilateral(State.Lease, Frame, Apply)
										  : Apply();
			State.Committing = false;
			if (Keep != Current)
				return {Moved ? SpatialTransferStatus::CommittedUnconfirmed : SpatialTransferStatus::Stale, {}};
			if (!Moved) return this->Cancel();
			return Send(State, Commit);
		}
		if (Frame.Phase == Applied && State.Frame.Phase == Commit) {
			State.Lease.reset();
			State.Subject.reset();
			State.Destination.reset();
			State.Receipt = {};
			State.Frame.Phase = Applied;
			return {SpatialTransferStatus::Applied, {}};
		}
		return {};
	}

	struct SpatialTransferReplica::State : HeldState {};
	SpatialTransferReplica::SpatialTransferReplica() = default;
	SpatialTransferReplica::~SpatialTransferReplica() = default;
	void SpatialTransferReplica::Reset() {
		++Mutation;
		Active.reset();
	}
	SpatialTransferStep SpatialTransferReplica::Cancel() {
		if (!Active) return {};
		if (Active->Committing) return {SpatialTransferStatus::Busy, {}};
		if (Active->Frame.Phase == SpatialTransferPhase::Applied) {
			Reset();
			return {SpatialTransferStatus::Applied, {}};
		}
		auto Rejection = Send(*Active, SpatialTransferPhase::Cancel, SpatialTransferStatus::Canceled);
		Reset();
		return Rejection;
	}
	SpatialTransferStep
	SpatialTransferReplica::Poll(const SpatialTransferScope &Scope, const SpatialTransferAdapters &Adapters) {
		if (!Active) return {};
		if (Active->Committing) return {SpatialTransferStatus::Busy, {}};
		if (Active->Frame.Scope != Scope) {
			auto Result = Cancel();
			Result.Reply.reset();
			return Result;
		}
		if (Active->Frame.Phase == SpatialTransferPhase::Applied) return {SpatialTransferStatus::Applied, {}};
		const auto Before = Active;
		const bool Valid = EvidenceIsCurrent(*Before, Adapters);
		if (Before != Active) return {};
		if (!Valid) return Cancel();
		return {SpatialTransferStatus::Progress, {}};
	}
	SpatialTransferStep SpatialTransferReplica::Receive(
		const SpatialTransferScope &Scope,
		const SpatialTransferFrame &Frame,
		const std::shared_ptr<WorldRoot> &Destination,
		const SpatialTransferAdapters &Adapters,
		const ApplyMovement &Apply
	) {
		using enum SpatialTransferPhase;
		if (!Valid(Frame) || Frame.Scope != Scope || !Destination || !Adapters.ResolveObject ||
			Adapters.ResolveObject(Scope.Destination) != Destination->GetObjectId())
			return {};
		if (Active && Active->Frame.Scope != Scope) Reset();
		if (Frame.Phase == Offer && (!Active || !Same(Frame, Active->Frame))) {
			if ((Active && Active->Frame.Phase != Applied) ||
				(LastEpoch == Scope.SessionEpoch && Frame.Serial <= LastSerial) || !Adapters.Resolve ||
				!Adapters.Reserve || !LocalSource(Frame, *Destination, Adapters))
				return {};
			const auto Started = ++Mutation;
			LastEpoch = Scope.SessionEpoch;
			LastSerial = Frame.Serial;
			const auto Reject = [&] {
				auto Response = Frame;
				Response.Phase = SpatialTransferPhase::Cancel;
				return SpatialTransferStep{SpatialTransferStatus::Denied, std::move(Response)};
			};
			std::vector<ObjectId> Owners;
			for (const auto Key : Frame.Owners) {
				const auto Local = Adapters.Resolve(Key);
				if (!Local || !Local->IsValid()) return Reject();
				Owners.push_back(*Local);
			}
			auto Native = Destination->PrepareSpatialDestination(Frame.Bounds, Owners);
			if (!Native) return Reject();
			const auto Projected = Project(*Native, Adapters);
			if (!Projected || Projected->Owners != Frame.Owners || Projected->Digest != Frame.Collision)
				return Reject();
			if (Started != Mutation) return {};
			auto Next = std::make_shared<State>();
			Next->Frame = Frame;
			Next->Receipt = std::move(*Native);
			Next->Destination = Destination;
			Active = std::move(Next);
			return Send(*Active, Ready);
		}
		if (!Active || !Same(Frame, Active->Frame)) return {};
		if (Frame.Phase == SpatialTransferPhase::Cancel) return this->Cancel();
		if (Active->Frame.Phase == Applied && Frame.Phase == Commit)
			return {SpatialTransferStatus::Applied, Active->Frame};
		auto Status = Poll(Scope, Adapters);
		if (Status.Status != SpatialTransferStatus::Progress) return Status;
		if (Frame.Phase == Offer && Active->Frame.Phase == Ready)
			return {SpatialTransferStatus::Progress, Active->Frame};
		if (Frame.Phase == Reserve && Active->Frame.Phase == Held)
			return {SpatialTransferStatus::Progress, Active->Frame};
		if (Frame.Phase == Reserve && Active->Frame.Phase == Ready) {
			const auto Before = Active;
			auto Lease = Adapters.Reserve(Frame, Before->Receipt);
			const bool Valid = Lease && (*Lease)() && EvidenceIsCurrent(*Before, Adapters);
			if (Active != Before) return {};
			if (!Valid) return this->Cancel();
			Active->Lease = std::move(Lease);
			return Send(*Active, Held);
		}
		if (Frame.Phase == Commit && Active->Frame.Phase == Held) {
			const auto Before = Active;
			Before->Committing = true;
			const auto Move = [&] { return Apply && Apply(Frame, Before->Receipt, Before->Lease); };
			const auto Realm = Before->Destination->GetDataModel();
			const auto Protection = Realm ? Realm->GetSpatialTransferProtection() : nullptr;
			const bool Moved = Protection ? Protection->Reservations.ApplyBilateral(Before->Lease, Frame, Move)
										  : Move();
			Before->Committing = false;
			if (Before != Active) return {Moved ? SpatialTransferStatus::Applied : SpatialTransferStatus::Stale, {}};
			if (!Moved) return this->Cancel();
			Active->Lease.reset();
			Active->Destination.reset();
			Active->Receipt = {};
			return Send(*Active, Applied, SpatialTransferStatus::Applied);
		}
		return {};
	}
}
