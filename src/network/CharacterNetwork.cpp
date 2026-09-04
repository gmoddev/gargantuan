#include "gargantuan/network/CharacterNetwork.hpp"

#include "gargantuan/classes/KinematicCharacter.hpp"
#include "gargantuan/classes/WorldRoot.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <limits>
#include <set>
#include <tuple>
#include <utility>

namespace gargantuan::network {
	namespace {
		template <typename Value> void SaturatingIncrement(Value &Counter, Value Amount = 1) {
			Counter = Amount > std::numeric_limits<Value>::max() - Counter ? std::numeric_limits<Value>::max()
																		   : Counter + Amount;
		}

		void RecordPublicationDeadlineMiss(CharacterNetworkMetrics &Metrics, CharacterPublicationTier Tier) {
			SaturatingIncrement(Metrics.PublicationDeadlineMisses);
			switch (Tier) {
			case CharacterPublicationTier::FullRate:
				SaturatingIncrement(Metrics.PublicationFullRateDeadlineMisses);
				break;
			case CharacterPublicationTier::ReducedRate:
				SaturatingIncrement(Metrics.PublicationReducedRateDeadlineMisses);
				break;
			case CharacterPublicationTier::LowRate:
				SaturatingIncrement(Metrics.PublicationLowRateDeadlineMisses);
				break;
			}
		}

		bool Finite(const glm::vec3 &Value) {
			return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
		}

		bool ValidMotionRequest(const CharacterMotionRequest &Request) {
			return Finite(Request.Translation) && Finite(Request.Velocity) && std::isfinite(Request.YawRadians) &&
				   glm::length(Request.Translation) <= Character::MaximumMotionTranslation &&
				   std::abs(Request.YawRadians) <= Character::MaximumMotionYawRadians;
		}

		std::optional<CharacterControlEpoch> TakeSequence(CharacterControlEpoch &Next) {
			if (!Next.IsValid()) return std::nullopt;
			const auto Result = Next;
			Next = Next.TryNext().value_or(CharacterControlEpoch{});
			return Result;
		}

		bool IsExpectedInputOrder(
			const ReceivedMessageEvent &Event, StateChannelId Channel, CharacterInputSequence Sequence
		) {
			const auto *Order = std::get_if<RealtimeStateOrder>(&Event.Order);
			return Event.Delivery == DeliveryMode::UnreliableSequenced &&
				   Event.Traffic == TrafficClass::RealtimeState && Order && Order->Channel == Channel &&
				   Order->Sequence.Value() == Sequence.Value();
		}

		bool IsExpectedStateOrder(
			const ReceivedMessageEvent &Event, StateChannelId Channel, RealtimeStateSequence Sequence
		) {
			if (Event.Delivery == DeliveryMode::ReliableOrdered)
				return Event.Traffic == TrafficClass::Control && std::holds_alternative<std::monostate>(Event.Order);
			const auto *Order = std::get_if<RealtimeStateOrder>(&Event.Order);
			return Event.Delivery == DeliveryMode::UnreliableSequenced &&
				   Event.Traffic == TrafficClass::RealtimeState && Order && Order->Channel == Channel &&
				   Order->Sequence == Sequence;
		}

		bool ReliableControl(const ReceivedMessageEvent &Event) {
			return Event.Delivery == DeliveryMode::ReliableOrdered && Event.Traffic == TrafficClass::Control &&
				   std::holds_alternative<std::monostate>(Event.Order);
		}

		bool ReliableAction(const ReceivedMessageEvent &Event) {
			return Event.Delivery == DeliveryMode::ReliableOrdered &&
				   Event.Traffic == TrafficClass::ReliableApplication &&
				   std::holds_alternative<std::monostate>(Event.Order);
		}

		bool IsExpectedStateFrameOrder(const ReceivedMessageEvent &Event, CharacterStateFrameSequence Sequence) {
			const auto *Order = std::get_if<RealtimeStateOrder>(&Event.Order);
			return Event.Delivery == DeliveryMode::UnreliableSequenced &&
				   Event.Traffic == TrafficClass::RealtimeState && Order && Order->Channel.IsValid() &&
				   Order->Sequence.Value() == Sequence.Value();
		}

		std::optional<CharacterMaterializationEpoch> NextMaterializationEpoch(CharacterMaterializationEpoch Current) {
			if (!Current.IsValid()) return CharacterMaterializationEpoch(1);
			return Current.TryNext();
		}

		DisconnectInfo SchedulerFailure(const SchedulerSubmitResult &Result, std::string Diagnostic) {
			return Result.TerminalDisconnect.value_or(
				DisconnectInfo{
					DisconnectReason::ResourceExhaustion,
					std::move(Diagnostic),
				}
			);
		}

		void HashWord(std::uint64_t &Hash, std::uint64_t Value) {
			for (std::size_t Index = 0; Index < sizeof(Value); ++Index) {
				Hash ^= static_cast<std::uint8_t>(Value >> (Index * 8));
				Hash *= 1099511628211ull;
			}
		}

		void HashFloat(std::uint64_t &Hash, float Value) {
			HashWord(Hash, std::bit_cast<std::uint32_t>(Value));
		}

		std::uint64_t SemanticStateFingerprint(const CharacterAuthoritativeState &State) {
			std::uint64_t Hash = 1469598103934665603ull;
			HashWord(Hash, State.ControlEpoch.Value());
			HashWord(Hash, State.AcknowledgedInput.Value());
			HashWord(Hash, State.ResolvedAction.Value());
			for (std::size_t Axis = 0; Axis < 3; ++Axis)
				HashFloat(Hash, State.Transform.Position[Axis]);
			for (std::size_t Column = 0; Column < 3; ++Column)
				for (std::size_t Row = 0; Row < 3; ++Row)
					HashFloat(Hash, State.Transform.Rotation[Column][Row]);
			for (std::size_t Axis = 0; Axis < 3; ++Axis) {
				HashFloat(Hash, State.Velocity[Axis]);
				HashFloat(Hash, State.FloorNormal[Axis]);
			}
			HashWord(Hash, State.Flags);
			HashWord(Hash, State.ActiveAction ? 1 : 0);
			if (State.ActiveAction) {
				HashWord(Hash, State.ActiveAction->ActionSequence.Value());
				HashWord(Hash, State.ActiveAction->ActionToken);
				HashWord(Hash, State.ActiveAction->Animation.High);
				HashWord(Hash, State.ActiveAction->Animation.Low);
				for (const auto Byte : State.ActiveAction->ContentRevision.Bytes)
					HashWord(Hash, Byte);
				HashWord(Hash, State.ActiveAction->StartTick);
				HashWord(Hash, State.ActiveAction->DurationTicks);
			}
			return Hash;
		}

		std::uint64_t SemanticStateBytes(const CharacterAuthoritativeState &State) {
			return State.ActiveAction ? 184 : 112;
		}

		std::uint64_t PublicationPhase(ConnectionId Connection, ObjectId Character, std::uint64_t Interval) {
			if (Interval <= 1) return 0;
			std::uint64_t Hash = 1469598103934665603ull;
			HashWord(Hash, Connection.Slot);
			HashWord(Hash, Connection.Generation);
			HashWord(Hash, Character.Slot);
			HashWord(Hash, Character.Generation);
			return Hash % Interval;
		}

		std::uint64_t SaturatingTickAdd(std::uint64_t Tick, std::uint64_t Amount) {
			return Amount > std::numeric_limits<std::uint64_t>::max() - Tick ? std::numeric_limits<std::uint64_t>::max()
																			 : Tick + Amount;
		}

		std::uint64_t
		NextPublicationPhaseTick(std::uint64_t Tick, std::uint64_t Interval, std::uint64_t Phase, bool IncludeCurrent) {
			if (Interval == 0) return std::numeric_limits<std::uint64_t>::max();
			const auto Remainder = Tick % Interval;
			auto Delta = (Phase + Interval - Remainder) % Interval;
			if (Delta == 0 && !IncludeCurrent) Delta = Interval;
			return SaturatingTickAdd(Tick, Delta);
		}

		float ExpectedTravelDistance(
			glm::vec3 PreviousVelocity,
			glm::vec3 CurrentVelocity,
			std::uint64_t TickSpan,
			std::uint32_t SimulationTicksPerSecond
		) {
			if (SimulationTicksPerSecond == 0) return 0.0f;
			return std::max(glm::length(PreviousVelocity), glm::length(CurrentVelocity)) *
				   (static_cast<float>(TickSpan) / static_cast<float>(SimulationTicksPerSecond));
		}
	}

	bool CharacterNetworkConfiguration::IsValid() const {
		return SimulationTicksPerSecond != 0 && StateUpdatesPerSecond != 0 && ReducedStateUpdatesPerSecond != 0 &&
			   LowStateUpdatesPerSecond != 0 && StateUpdatesPerSecond <= SimulationTicksPerSecond &&
			   StateUpdatesPerSecond >= ReducedStateUpdatesPerSecond &&
			   ReducedStateUpdatesPerSecond >= LowStateUpdatesPerSecond &&
			   SimulationTicksPerSecond % StateUpdatesPerSecond == 0 &&
			   SimulationTicksPerSecond % ReducedStateUpdatesPerSecond == 0 &&
			   SimulationTicksPerSecond % LowStateUpdatesPerSecond == 0 && AbsoluteRefreshTicks != 0 &&
			   RemoteInterpolationDelayTicks != 0 && RemoteExtrapolationLimitTicks != 0 &&
			   RemoteExtrapolationLimitTicks <= MaximumRemoteCharacterSnapshotWindowTicks && InputFreshnessTicks != 0 &&
			   ImportanceUpdateTicks != 0 && PromotionTicks != 0 && std::isfinite(FullRateDistance) &&
			   std::isfinite(ReducedRateDistance) && std::isfinite(ImportanceHysteresis) && FullRateDistance > 0.0f &&
			   ReducedRateDistance > FullRateDistance && ImportanceHysteresis > 0.0f &&
			   ImportanceHysteresis < FullRateDistance &&
			   FullRateDistance + ImportanceHysteresis < ReducedRateDistance - ImportanceHysteresis &&
			   MaximumStateFrameBytes >=
				   CharacterStateFrameHeaderBytes + CompactCharacterStateBytes + CompactCharacterActionStateBytes &&
			   MaximumStateFrameBytes <= MaximumCharacterStateFrameBytes && MaximumPublicationStatesPerPeerTick != 0 &&
			   MaximumPublicationStatesPerPeerTick <= MaximumCharacterPublicationStatesPerPeerTick &&
			   MaximumPublicationStatesPerTick != 0 &&
			   MaximumPublicationStatesPerTick <= MaximumCharacterPublicationStatesPerTick &&
			   MaximumPublicationStatesPerPeerTick <= MaximumPublicationStatesPerTick &&
			   PublicationPeerQuantum != 0 && PublicationPeerQuantum <= MaximumPublicationStatesPerPeerTick;
	}

	std::uint64_t CharacterNetworkConfiguration::PublicationIntervalTicks() const {
		return IsValid() ? SimulationTicksPerSecond / StateUpdatesPerSecond : 0;
	}

	std::uint64_t CharacterNetworkConfiguration::PublicationIntervalTicks(CharacterPublicationTier Tier) const {
		if (!IsValid()) return 0;
		switch (Tier) {
		case CharacterPublicationTier::FullRate:
			return SimulationTicksPerSecond / StateUpdatesPerSecond;
		case CharacterPublicationTier::ReducedRate:
			return SimulationTicksPerSecond / ReducedStateUpdatesPerSecond;
		case CharacterPublicationTier::LowRate:
			return SimulationTicksPerSecond / LowStateUpdatesPerSecond;
		}
		return 0;
	}

	bool CharacterActionDefinition::IsValid() const {
		return Token != 0 && Animation.IsValid() && ContentRevision.IsValid() && DurationTicks != 0 &&
			   DurationTicks <= 60 * 60 * 10 && static_cast<bool>(EvaluateRootMotion);
	}

	struct AuthoritativeCharacterNetwork::PeerState {
		enum class PublicationQueueKind : std::uint8_t { None, Wheel, Forced, Owner, FullRate, ReducedRate, LowRate };

		struct PublicationState {
			std::uint64_t Fingerprint = 0;
			std::uint64_t LastAbsoluteTick = 0;
			std::uint64_t LastPublicationTick = 0;
			std::uint64_t TemporaryFullRateUntilTick = 0;
			std::uint64_t DesiredDueTick = 0;
			std::uint64_t HardDeadlineTick = 0;
			CharacterPublicationTier Tier = CharacterPublicationTier::FullRate;
			CharacterPublicationTier EffectiveTier = CharacterPublicationTier::FullRate;
			PublicationQueueKind QueueKind = PublicationQueueKind::None;
			std::uint8_t WheelSlot = 0;
			ObjectId PreviousScheduled;
			ObjectId NextScheduled;
			bool HasPublished = false;
			bool PublishImmediately = true;
			bool DeadlineEscalationRecorded = false;
			bool DeadlineMissRecorded = false;
		};

		struct PublicationList {
			ObjectId Head;
			ObjectId Tail;
			std::size_t Size = 0;
		};

		std::map<ObjectId, StateChannelId> Materialized;
		std::map<ObjectId, PublicationState> Published;
		std::array<PublicationList, CharacterPublicationWheelSlots> PublicationWheel;
		PublicationList ForcedDue;
		PublicationList OwnerDue;
		PublicationList FullRateDue;
		PublicationList ReducedRateDue;
		PublicationList LowRateDue;
		std::vector<glm::vec3> PublicationFocus;
		CharacterStateFrameSequence NextFrameSequence{1};
		CharacterMaterializationEpoch MaterializationEpoch;
		std::uint64_t LastImportanceUpdateTick = 0;
		std::uint64_t LastDueProcessingTick = 0;
		std::size_t SelectedThisTick = 0;
		std::size_t AcceptedBatchesThisTick = 0;
		bool SchedulerRejectedThisTick = false;
		bool ImportanceDirty = true;

		PublicationList *GetList(PublicationQueueKind Kind, std::uint8_t Slot = 0) {
			switch (Kind) {
			case PublicationQueueKind::Wheel:
				return Slot < PublicationWheel.size() ? &PublicationWheel[Slot] : nullptr;
			case PublicationQueueKind::Forced:
				return &ForcedDue;
			case PublicationQueueKind::Owner:
				return &OwnerDue;
			case PublicationQueueKind::FullRate:
				return &FullRateDue;
			case PublicationQueueKind::ReducedRate:
				return &ReducedRateDue;
			case PublicationQueueKind::LowRate:
				return &LowRateDue;
			case PublicationQueueKind::None:
				break;
			}
			return nullptr;
		}

		void Unlink(ObjectId Character) {
			auto Found = Published.find(Character);
			if (Found == Published.end() || Found->second.QueueKind == PublicationQueueKind::None) return;
			auto &State = Found->second;
			auto *List = GetList(State.QueueKind, State.WheelSlot);
			if (!List) return;
			if (State.PreviousScheduled.IsValid()) {
				auto Previous = Published.find(State.PreviousScheduled);
				if (Previous != Published.end()) Previous->second.NextScheduled = State.NextScheduled;
			} else {
				List->Head = State.NextScheduled;
			}
			if (State.NextScheduled.IsValid()) {
				auto Next = Published.find(State.NextScheduled);
				if (Next != Published.end()) Next->second.PreviousScheduled = State.PreviousScheduled;
			} else {
				List->Tail = State.PreviousScheduled;
			}
			if (List->Size != 0) --List->Size;
			State.QueueKind = PublicationQueueKind::None;
			State.PreviousScheduled = {};
			State.NextScheduled = {};
		}

		void LinkBack(ObjectId Character, PublicationQueueKind Kind, std::uint8_t Slot = 0) {
			auto Found = Published.find(Character);
			if (Found == Published.end() || Kind == PublicationQueueKind::None) return;
			Unlink(Character);
			auto *List = GetList(Kind, Slot);
			if (!List) return;
			auto &State = Found->second;
			State.QueueKind = Kind;
			State.WheelSlot = Slot;
			State.PreviousScheduled = List->Tail;
			State.NextScheduled = {};
			if (List->Tail.IsValid()) {
				auto Tail = Published.find(List->Tail);
				if (Tail != Published.end()) Tail->second.NextScheduled = Character;
			} else {
				List->Head = Character;
			}
			List->Tail = Character;
			++List->Size;
		}

		ObjectId PopFront(PublicationQueueKind Kind) {
			auto *List = GetList(Kind);
			if (!List || !List->Head.IsValid()) return {};
			const auto Result = List->Head;
			Unlink(Result);
			return Result;
		}
	};

	struct AuthoritativeCharacterNetwork::CharacterState {
		std::weak_ptr<KinematicCharacter> Character;
		CharacterControlEpoch ControlEpoch;
		std::optional<ConnectionId> Controller;
		CharacterInputSequence LastReceivedInput;
		CharacterInputSequence AcknowledgedInput;
		std::optional<CharacterInputCommand> PendingInput;
		std::uint64_t LastInputArrivalTick = 0;
		bool InputTimedOut = false;
		std::uint64_t RateTick = 0;
		std::uint32_t CommandsAtRateTick = 0;
		std::array<CharacterActionRequest, MaximumPendingCharacterActions> PendingActions{};
		std::size_t PendingActionCount = 0;
		CharacterActionSequence LastReceivedAction;
		CharacterActionSequence ResolvedAction;
		CharacterActionSequence NextServerAction{1};
		std::optional<CharacterActionState> ActiveAction;
		std::uint64_t LastActionEvaluationTick = 0;
		RealtimeStateSequence NextStateSequence{1};
		bool ReliableStateRequired = false;
		std::optional<CharacterAuthoritativeState> PreparedState;
		std::uint64_t PreparedFingerprint = 0;
		std::uint64_t PreparedTick = 0;
		std::uint64_t CurrentFingerprint = 0;
		std::uint64_t CurrentFingerprintTick = 0;
		std::uint64_t SemanticPromotionUntilTick = 0;
		std::optional<CFrame> LastObservedTransform;
		glm::vec3 LastObservedVelocity{};
		std::uint64_t LastObservedTick = 0;
		bool WasMoving = false;
	};

	AuthoritativeCharacterNetwork::~AuthoritativeCharacterNetwork() = default;

	AuthoritativeCharacterNetwork::AuthoritativeCharacterNetwork(
		INetworkScheduler &SchedulerValue,
		NetworkLimits LimitsValue,
		CharacterMovementPolicy MovementPolicyValue,
		CharacterActionPolicy ActionPolicyValue,
		CharacterNetworkConfiguration ConfigurationValue
	)
		: Scheduler(SchedulerValue), Limits(LimitsValue), MovementPolicy(std::move(MovementPolicyValue)),
		  ActionPolicy(std::move(ActionPolicyValue)), Configuration(ConfigurationValue) {
		if (!Limits.IsValid() || !MovementPolicy || !Configuration.IsValid() ||
			Limits.MaximumUnreliableMessageBytes <
				CharacterStateFrameHeaderBytes + CompactCharacterStateBytes + CompactCharacterActionStateBytes)
			throw std::invalid_argument("[Character:Network] authoritative manager configuration is invalid");
	}

	bool AuthoritativeCharacterNetwork::AddPeer(ConnectionId Connection) {
		return Connection.IsValid() && Peers.size() < MaximumCharacterNetworkPeers &&
			   Peers.emplace(Connection, PeerState{}).second;
	}

	bool AuthoritativeCharacterNetwork::RemovePeer(ConnectionId Connection, std::uint64_t AuthoritativeTick) {
		if (!Peers.contains(Connection)) return false;
		std::vector<ObjectId> Controlled;
		for (const auto &[Id, Value] : Characters)
			if (Value.Controller == Connection) Controlled.push_back(Id);
		for (const auto Id : Controlled)
			(void)RevokeControl(Id, AuthoritativeTick);
		Peers.erase(Connection);
		if (Peers.empty()) PublicationPeerCursor.reset();
		return true;
	}

	bool AuthoritativeCharacterNetwork::RegisterCharacter(const std::shared_ptr<KinematicCharacter> &CharacterValue) {
		if (!CharacterValue || CharacterValue->GetDestroyed() || CharacterValue->IsDestroying() ||
			Characters.size() >= MaximumNetworkCharacters)
			return false;
		auto Epoch = TakeSequence(NextControlEpoch);
		return Epoch && Characters
							.emplace(
								CharacterValue->GetObjectId(),
								CharacterState{
									.Character = CharacterValue,
									.ControlEpoch = *Epoch,
								}
							)
							.second;
	}

	bool AuthoritativeCharacterNetwork::UnregisterCharacter(ObjectId Character, std::uint64_t AuthoritativeTick) {
		auto Found = Characters.find(Character);
		if (Found == Characters.end()) return false;
		for (const auto &[Connection, Peer] : Peers) {
			(void)Connection;
			if (Peer.Materialized.contains(Character) && !NextMaterializationEpoch(Peer.MaterializationEpoch))
				return false;
		}
		(void)RevokeControl(Character, AuthoritativeTick);
		for (auto &[Connection, Peer] : Peers) {
			(void)Connection;
			if (Peer.Materialized.erase(Character))
				Peer.MaterializationEpoch = *NextMaterializationEpoch(Peer.MaterializationEpoch);
			Peer.Unlink(Character);
			Peer.Published.erase(Character);
		}
		Characters.erase(Found);
		return true;
	}

	bool AuthoritativeCharacterNetwork::MarkMaterialized(
		ConnectionId Connection, ObjectId Character, StateChannelId Channel
	) {
		auto Peer = Peers.find(Connection);
		auto NextEpoch = Peer == Peers.end() ? std::nullopt
											 : NextMaterializationEpoch(Peer->second.MaterializationEpoch);
		if (Peer == Peers.end() || !Characters.contains(Character) || !Channel.IsValid() || !NextEpoch ||
			!Peer->second.Materialized.emplace(Character, Channel).second)
			return false;
		Peer->second.MaterializationEpoch = *NextEpoch;
		const auto PromotionStart = std::max<std::uint64_t>(LastAuthoritativeTick, 1);
		const auto PromotionUntil = PromotionStart >
											std::numeric_limits<std::uint64_t>::max() - Configuration.PromotionTicks
										? std::numeric_limits<std::uint64_t>::max()
										: PromotionStart + Configuration.PromotionTicks;
		Peer->second.Published[Character] = PeerState::PublicationState{
			.TemporaryFullRateUntilTick = PromotionUntil,
		};
		MakeRelationshipDue(Peer->second, Connection, Character, PromotionStart, true);
		Peer->second.ImportanceDirty = true;
		SaturatingIncrement(Metrics.TemporaryPromotions);
		return true;
	}

	bool AuthoritativeCharacterNetwork::MarkUnmaterialized(ConnectionId Connection, ObjectId Character) {
		auto Peer = Peers.find(Connection);
		auto NextEpoch = Peer == Peers.end() ? std::nullopt
											 : NextMaterializationEpoch(Peer->second.MaterializationEpoch);
		if (Peer == Peers.end() || !NextEpoch || !Peer->second.Materialized.erase(Character)) return false;
		Peer->second.MaterializationEpoch = *NextEpoch;
		Peer->second.Unlink(Character);
		Peer->second.Published.erase(Character);
		auto Found = Characters.find(Character);
		if (Found != Characters.end() && Found->second.Controller == Connection)
			(void)RevokeControl(Character, std::max<std::uint64_t>(LastAuthoritativeTick, 1));
		return true;
	}

	bool AuthoritativeCharacterNetwork::SetPeerPublicationFocus(
		ConnectionId Connection, std::span<const glm::vec3> FocusPoints
	) {
		auto Peer = Peers.find(Connection);
		if (Peer == Peers.end() || FocusPoints.size() > MaximumCharacterPublicationFocusPoints ||
			std::ranges::any_of(FocusPoints, [](glm::vec3 Point) { return !Finite(Point); }))
			return false;
		if (Peer->second.PublicationFocus.size() == FocusPoints.size() &&
			std::equal(
				Peer->second.PublicationFocus.begin(),
				Peer->second.PublicationFocus.end(),
				FocusPoints.begin(),
				[](const glm::vec3 &Left, const glm::vec3 &Right) {
					return Left.x == Right.x && Left.y == Right.y && Left.z == Right.z;
				}
			))
			return true;
		Peer->second.PublicationFocus.assign(FocusPoints.begin(), FocusPoints.end());
		Peer->second.ImportanceDirty = true;
		return true;
	}

	std::optional<CharacterControlEpoch> AuthoritativeCharacterNetwork::BindControl(
		ConnectionId Connection, ObjectId Character, std::uint64_t AuthoritativeTick
	) {
		auto Peer = Peers.find(Connection);
		auto Found = Characters.find(Character);
		if (AuthoritativeTick == 0 || Peer == Peers.end() || Found == Characters.end() ||
			!Peer->second.Materialized.contains(Character))
			return std::nullopt;
		if (Found->second.Controller) (void)RevokeControl(Character, AuthoritativeTick);
		auto Epoch = TakeSequence(NextControlEpoch);
		if (!Epoch) return std::nullopt;
		auto &State = Found->second;
		State.ControlEpoch = *Epoch;
		State.Controller = Connection;
		State.LastReceivedInput = {};
		State.AcknowledgedInput = {};
		State.PendingInput.reset();
		State.LastInputArrivalTick = 0;
		State.InputTimedOut = false;
		State.LastReceivedAction = {};
		State.ResolvedAction = {};
		State.PendingActionCount = 0;
		State.ActiveAction.reset();
		auto Publication = Peer->second.Published.find(Character);
		if (Publication != Peer->second.Published.end()) {
			Publication->second.Tier = CharacterPublicationTier::FullRate;
			const auto PromotionUntil = AuthoritativeTick >
												std::numeric_limits<std::uint64_t>::max() - Configuration.PromotionTicks
											? std::numeric_limits<std::uint64_t>::max()
											: AuthoritativeTick + Configuration.PromotionTicks;
			Publication->second.TemporaryFullRateUntilTick = std::max(
				Publication->second.TemporaryFullRateUntilTick, PromotionUntil
			);
			Publication->second.PublishImmediately = true;
			MakeRelationshipDue(Peer->second, Connection, Character, AuthoritativeTick, true);
		}
		const auto Channel = Peer->second.Materialized.at(Character);
		if (!SendControl(Connection, {Character, *Epoch, Channel, AuthoritativeTick, true})) {
			State.Controller.reset();
			return std::nullopt;
		}
		return Epoch;
	}

	bool AuthoritativeCharacterNetwork::RevokeControl(ObjectId Character, std::uint64_t AuthoritativeTick) {
		auto Found = Characters.find(Character);
		if (Found == Characters.end() || !Found->second.Controller || AuthoritativeTick == 0) return false;
		const auto Connection = *Found->second.Controller;
		const auto OldEpoch = Found->second.ControlEpoch;
		(void)SendControl(Connection, {Character, OldEpoch, {}, AuthoritativeTick, false});
		auto Epoch = TakeSequence(NextControlEpoch);
		Found->second.ControlEpoch = Epoch.value_or(CharacterControlEpoch{});
		Found->second.Controller.reset();
		Found->second.PendingInput.reset();
		Found->second.LastInputArrivalTick = 0;
		Found->second.InputTimedOut = false;
		Found->second.PendingActionCount = 0;
		Found->second.ActiveAction.reset();
		for (auto &[PeerConnection, Peer] : Peers)
			if (Peer.Published.contains(Character))
				RefreshRelationshipSchedule(Peer, PeerConnection, Character, AuthoritativeTick);
		return true;
	}

	bool AuthoritativeCharacterNetwork::RegisterAction(CharacterActionDefinition Definition) {
		return Definition.IsValid() && Actions.size() < MaximumNetworkCharacters &&
			   Actions.emplace(Definition.Token, std::move(Definition)).second;
	}

	void AuthoritativeCharacterNetwork::SetTerminalHandler(CharacterTerminalHandler Handler) {
		OnTerminal = std::move(Handler);
	}

	bool AuthoritativeCharacterNetwork::StartServerAction(
		ObjectId Character, std::uint32_t ActionToken, std::uint64_t AuthoritativeTick
	) {
		auto Found = Characters.find(Character);
		auto Definition = Actions.find(ActionToken);
		if (Found == Characters.end() || Definition == Actions.end() || AuthoritativeTick == 0 ||
			Definition->second.DurationTicks > std::numeric_limits<std::uint64_t>::max() - AuthoritativeTick ||
			!Found->second.NextServerAction.IsValid())
			return false;
		auto &State = Found->second;
		const auto Sequence = State.NextServerAction;
		State.NextServerAction = State.NextServerAction.TryNext().value_or(CharacterActionSequence{});
		State.ResolvedAction = Sequence;
		State.ActiveAction = CharacterActionState{
			Sequence,
			Definition->second.Token,
			Definition->second.Animation,
			Definition->second.ContentRevision,
			AuthoritativeTick,
			Definition->second.DurationTicks,
		};
		State.LastActionEvaluationTick = AuthoritativeTick;
		State.ReliableStateRequired = true;
		PromoteCharacter(Character, State, AuthoritativeTick);
		return true;
	}

	bool AuthoritativeCharacterNetwork::Queue(
		ConnectionId Connection, const CharacterMessage &Message, StateChannelId Channel, bool Reliable
	) {
		const auto EncodeStarted = std::chrono::steady_clock::now();
		auto Encoded = EncodeCharacterMessage(Message);
		SaturatingIncrement(
			Metrics.StateEncodeCpuNanoseconds,
			static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - EncodeStarted)
					.count()
			)
		);
		if (!Encoded) {
			SaturatingIncrement(Metrics.ProtocolRejects);
			if (Reliable && OnTerminal)
				OnTerminal(
					Connection,
					{DisconnectReason::ResourceExhaustion, "Reliable Character message serialization failed"}
				);
			return false;
		}
		MessageOrder Order;
		DeliveryMode Delivery = DeliveryMode::ReliableOrdered;
		TrafficClass Traffic = TrafficClass::Control;
		if (!Reliable) {
			const auto *State = std::get_if<CharacterAuthoritativeState>(&Message);
			if (!State || !Channel.IsValid()) return false;
			Delivery = DeliveryMode::UnreliableSequenced;
			Traffic = TrafficClass::RealtimeState;
			Order = RealtimeStateOrder{Channel, State->StateSequence};
		} else if (std::holds_alternative<CharacterActionRequest>(Message) ||
				   std::holds_alternative<CharacterActionResult>(Message)) {
			Traffic = TrafficClass::ReliableApplication;
		}
		auto Intent = MakeNetworkMessageIntent(
			Connection, Delivery, Traffic, std::move(Order), std::move(*Encoded), Limits
		);
		if (!Intent) {
			if (Reliable && OnTerminal)
				OnTerminal(
					Connection, {DisconnectReason::ResourceExhaustion, "Reliable Character message intent was rejected"}
				);
			return false;
		}
		const auto Bytes = Intent->Payload().size();
		const auto SubmitStarted = std::chrono::steady_clock::now();
		auto Result = Scheduler.Submit(std::move(*Intent));
		SaturatingIncrement(
			Metrics.SchedulerSubmitCpuNanoseconds,
			static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - SubmitStarted)
					.count()
			)
		);
		if (!Result.Accepted()) {
			if (Reliable && OnTerminal)
				OnTerminal(
					Connection, SchedulerFailure(Result, "Reliable Character message was rejected by the scheduler")
				);
			return false;
		}
		SaturatingIncrement(Metrics.BytesOut, static_cast<std::uint64_t>(Bytes));
		SaturatingIncrement(Metrics.SchedulerSubmissions);
		return true;
	}

	bool AuthoritativeCharacterNetwork::QueueStateFrame(
		ConnectionId Connection, const CharacterStateFrame &Frame, StateChannelId Channel, bool Reliable
	) {
		if (!Reliable && !Channel.IsValid()) return false;
		const auto EncodeStarted = std::chrono::steady_clock::now();
		auto Encoded = EncodeCharacterMessage(CharacterMessage(Frame));
		SaturatingIncrement(
			Metrics.StateEncodeCpuNanoseconds,
			static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - EncodeStarted)
					.count()
			)
		);
		if (!Encoded || Encoded->size() > Configuration.MaximumStateFrameBytes ||
			Encoded->size() > Limits.MaximumUnreliableMessageBytes) {
			SaturatingIncrement(Metrics.ProtocolRejects);
			if (Reliable && OnTerminal)
				OnTerminal(
					Connection, {DisconnectReason::ResourceExhaustion, "Reliable Character state serialization failed"}
				);
			return false;
		}
		auto Intent = MakeNetworkMessageIntent(
			Connection,
			Reliable ? DeliveryMode::ReliableOrdered : DeliveryMode::UnreliableSequenced,
			Reliable ? TrafficClass::Control : TrafficClass::RealtimeState,
			Reliable ? MessageOrder{}
					 : MessageOrder(
						   RealtimeStateOrder{
							   Channel,
							   RealtimeStateSequence(Frame.FrameSequence.Value()),
						   }
					   ),
			std::move(*Encoded),
			Limits
		);
		if (!Intent) {
			if (Reliable && OnTerminal)
				OnTerminal(
					Connection, {DisconnectReason::ResourceExhaustion, "Reliable Character state intent was rejected"}
				);
			return false;
		}
		const auto Bytes = Intent->Payload().size();
		const auto SubmitStarted = std::chrono::steady_clock::now();
		auto Result = Scheduler.Submit(std::move(*Intent));
		SaturatingIncrement(
			Metrics.SchedulerSubmitCpuNanoseconds,
			static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - SubmitStarted)
					.count()
			)
		);
		if (!Result.Accepted()) {
			if (!Reliable) SaturatingIncrement(Metrics.PublicationSchedulerRejections);
			if ((Reliable || Result.IsTerminal()) && OnTerminal)
				OnTerminal(Connection, SchedulerFailure(Result, "Character state was rejected by the scheduler"));
			return false;
		}
		SaturatingIncrement(Metrics.BytesOut, static_cast<std::uint64_t>(Bytes));
		SaturatingIncrement(Metrics.CompactStateBytes, static_cast<std::uint64_t>(Bytes));
		SaturatingIncrement(Metrics.SchedulerSubmissions);
		SaturatingIncrement(Metrics.StateFramesEmitted);
		SaturatingIncrement(Metrics.StatesInFrames, static_cast<std::uint64_t>(Frame.StateCount));
		return true;
	}

	bool
	AuthoritativeCharacterNetwork::SendControl(ConnectionId Connection, const CharacterControlTransition &Transition) {
		return Queue(Connection, CharacterMessage(Transition), {}, true);
	}

	bool AuthoritativeCharacterNetwork::HandleTransportEvent(const TransportEvent &Event) {
		const auto *Received = std::get_if<ReceivedMessageEvent>(&Event);
		if (!Received || !Peers.contains(Received->Connection)) return false;
		SaturatingIncrement(Metrics.BytesIn, static_cast<std::uint64_t>(Received->Payload.size()));
		const auto DecodeStarted = std::chrono::steady_clock::now();
		auto Decoded = DecodeCharacterMessage(Received->Payload);
		SaturatingIncrement(
			Metrics.ProtocolDecodeCpuNanoseconds,
			static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - DecodeStarted)
					.count()
			)
		);
		if (!Decoded) {
			SaturatingIncrement(Metrics.ProtocolRejects);
			SaturatingIncrement(Metrics.MalformedFramesRejected);
			return false;
		}
		return HandleMessage(Received->Connection, *Received, std::move(*Decoded));
	}

	bool AuthoritativeCharacterNetwork::HandleMessage(
		ConnectionId Connection, const ReceivedMessageEvent &Event, CharacterMessage Message
	) {
		const auto AdmissionStarted = std::chrono::steady_clock::now();
		auto FinishAdmission = [&] {
			SaturatingIncrement(
				Metrics.CommandAdmissionCpuNanoseconds,
				static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
											   std::chrono::steady_clock::now() - AdmissionStarted
				)
											   .count())
			);
		};
		if (auto *Command = std::get_if<CharacterInputCommand>(&Message)) {
			SaturatingIncrement(Metrics.CommandsReceived);
			auto Found = Characters.find(Command->Character);
			if (Found == Characters.end() || Found->second.Controller != Connection ||
				Found->second.ControlEpoch != Command->ControlEpoch) {
				SaturatingIncrement(Metrics.UnauthorizedCommandsRejected);
				FinishAdmission();
				return false;
			}
			const auto Channel = Peers.at(Connection).Materialized.find(Command->Character);
			if (Channel == Peers.at(Connection).Materialized.end() ||
				!IsExpectedInputOrder(Event, Channel->second, Command->InputSequence)) {
				SaturatingIncrement(Metrics.ProtocolRejects);
				FinishAdmission();
				return false;
			}
			auto &State = Found->second;
			if (State.LastReceivedInput.IsValid() && !Command->InputSequence.IsNewerThan(State.LastReceivedInput)) {
				SaturatingIncrement(Metrics.StaleCommandsRejected);
				FinishAdmission();
				return false;
			}
			const auto AdmissionTick = std::max<std::uint64_t>(LastAuthoritativeTick, 1);
			if (State.RateTick != AdmissionTick) {
				State.RateTick = AdmissionTick;
				State.CommandsAtRateTick = 0;
			}
			if (++State.CommandsAtRateTick > MaximumCharacterCommandsPerTick) {
				SaturatingIncrement(Metrics.RateLimitedCommands);
				FinishAdmission();
				return false;
			}
			State.LastReceivedInput = Command->InputSequence;
			State.PendingInput = *Command;
			State.LastInputArrivalTick = AdmissionTick;
			State.InputTimedOut = false;
			FinishAdmission();
			return true;
		}
		if (auto *Request = std::get_if<CharacterActionRequest>(&Message)) {
			auto Found = Characters.find(Request->Character);
			if (!ReliableAction(Event) || Found == Characters.end() || Found->second.Controller != Connection ||
				Found->second.ControlEpoch != Request->ControlEpoch) {
				SaturatingIncrement(Metrics.UnauthorizedCommandsRejected);
				FinishAdmission();
				return false;
			}
			auto &State = Found->second;
			if (State.LastReceivedAction.IsValid() && !Request->ActionSequence.IsNewerThan(State.LastReceivedAction)) {
				SaturatingIncrement(Metrics.StaleCommandsRejected);
				FinishAdmission();
				return false;
			}
			if (State.PendingActionCount >= State.PendingActions.size()) {
				SaturatingIncrement(Metrics.RateLimitedCommands);
				FinishAdmission();
				return false;
			}
			State.LastReceivedAction = Request->ActionSequence;
			State.PendingActions[State.PendingActionCount++] = *Request;
			FinishAdmission();
			return true;
		}
		SaturatingIncrement(Metrics.ProtocolRejects);
		FinishAdmission();
		return false;
	}

	void AuthoritativeCharacterNetwork::Step(WorldRoot &World, std::uint64_t AuthoritativeTick) {
		if (AuthoritativeTick == 0) return;
		LastAuthoritativeTick = AuthoritativeTick;
		for (auto Iterator = Characters.begin(); Iterator != Characters.end();) {
			const auto Id = Iterator->first;
			auto &State = Iterator->second;
			auto CharacterValue = State.Character.lock();
			if (!CharacterValue || CharacterValue->GetDestroyed() || CharacterValue->IsDestroying() ||
				CharacterValue->GetObjectId() != Id) {
				++Iterator;
				(void)UnregisterCharacter(Id, AuthoritativeTick);
				continue;
			}

			for (std::size_t Index = 0; Index < State.PendingActionCount; ++Index) {
				const auto &Request = State.PendingActions[Index];
				const auto Selected = ActionPolicy ? ActionPolicy(*State.Controller, Request)
												   : std::optional<std::uint32_t>(Request.RequestedActionToken);
				const auto Definition = Selected ? Actions.find(*Selected) : Actions.end();
				State.ResolvedAction = Request.ActionSequence;
				std::optional<CharacterActionState> AcceptedAction;
				if (!Selected || Definition == Actions.end() ||
					Definition->second.DurationTicks > std::numeric_limits<std::uint64_t>::max() - AuthoritativeTick) {
					State.ReliableStateRequired = true;
					SaturatingIncrement(Metrics.ActionRequestsRejected);
				} else {
					AcceptedAction = CharacterActionState{
						Request.ActionSequence,
						Definition->second.Token,
						Definition->second.Animation,
						Definition->second.ContentRevision,
						AuthoritativeTick,
						Definition->second.DurationTicks,
					};
					State.ActiveAction = AcceptedAction;
					State.LastActionEvaluationTick = AuthoritativeTick;
					State.ReliableStateRequired = true;
					SaturatingIncrement(Metrics.ActionRequestsAccepted);
				}
				PromoteCharacter(Id, State, AuthoritativeTick);
				const bool ResultQueued = Queue(
					*State.Controller,
					CharacterMessage(
						CharacterActionResult{
							.Character = Request.Character,
							.ControlEpoch = Request.ControlEpoch,
							.ActionSequence = Request.ActionSequence,
							.RequestedActionToken = Request.RequestedActionToken,
							.Accepted = AcceptedAction.has_value(),
							.AuthoritativeAction = std::move(AcceptedAction),
						}
					),
					{},
					true
				);
				if (!ResultQueued) break;
			}
			State.PendingActionCount = 0;

			if (State.PendingInput) {
				auto Command = *State.PendingInput;
				const bool TickAdmissible = Command.SimulationTick <=
											AuthoritativeTick + MaximumCharacterCommandTickLead;
				const bool Fresh = AuthoritativeTick >= State.LastInputArrivalTick &&
								   AuthoritativeTick - State.LastInputArrivalTick <= Configuration.InputFreshnessTicks;
				const bool NewCommand = State.AcknowledgedInput != Command.InputSequence;
				if (!NewCommand) Command.Flags &= ~static_cast<std::uint8_t>(CharacterInputFlag::JumpRequested);
				if (!Fresh) {
					Command.MoveIntent = {};
					Command.Flags = 0;
					if (!State.InputTimedOut) {
						State.InputTimedOut = true;
						SaturatingIncrement(Metrics.InputFreshnessTimeouts);
					}
				}
				Command.SimulationTick = AuthoritativeTick;
				Command.DeltaSeconds = 1.0f / static_cast<float>(Configuration.SimulationTicksPerSecond);
				if (TickAdmissible) {
					const auto MovementStarted = std::chrono::steady_clock::now();
					try {
						auto Request = MovementPolicy(Command, *CharacterValue);
						if (ValidMotionRequest(Request)) {
							auto Result = CharacterValue->AdmitMotion(World, Request);
							if (Result.Succeeded()) {
								CharacterValue->ApplyRuntimeControllerFacts(
									Result.Velocity, Result.FloorNormal, Result.HasFloor
								);
								State.AcknowledgedInput = Command.InputSequence;
								if (NewCommand) SaturatingIncrement(Metrics.CommandsAccepted);
							}
						}
					} catch (...) {
						SaturatingIncrement(Metrics.MovementPolicyErrors);
					}
					SaturatingIncrement(
						Metrics.MovementCpuNanoseconds,
						static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
													   std::chrono::steady_clock::now() - MovementStarted
						)
													   .count())
					);
				} else {
					SaturatingIncrement(Metrics.StaleCommandsRejected);
				}
			}

			if (State.ActiveAction) {
				const auto Definition = Actions.find(State.ActiveAction->ActionToken);
				if (Definition == Actions.end()) {
					State.ActiveAction.reset();
					State.ReliableStateRequired = true;
					PromoteCharacter(Id, State, AuthoritativeTick);
				} else {
					const auto ActionEnd = State.ActiveAction->StartTick + State.ActiveAction->DurationTicks;
					const auto EvaluationTick = std::min(AuthoritativeTick, ActionEnd);
					if (EvaluationTick > State.LastActionEvaluationTick) {
						const auto RootStarted = std::chrono::steady_clock::now();
						try {
							auto Delta = Definition->second.EvaluateRootMotion(
								State.LastActionEvaluationTick, EvaluationTick
							);
							State.LastActionEvaluationTick = EvaluationTick;
							if (Delta) {
								auto Result = CharacterValue->AdmitMotion(
									World,
									{
										.Translation = Delta->Translation,
										.Velocity = CharacterValue->GetVelocity(),
										.YawRadians = Delta->YawRadians,
										.Source = CharacterMotionSource::Animation,
										.LocalSpace = true,
									}
								);
								if (Result.Succeeded())
									CharacterValue->ApplyRuntimeControllerFacts(
										Result.Velocity, Result.FloorNormal, Result.HasFloor
									);
							}
						} catch (...) {
							State.ActiveAction.reset();
							State.ReliableStateRequired = true;
							PromoteCharacter(Id, State, AuthoritativeTick);
							SaturatingIncrement(Metrics.ProtocolRejects);
						}
						SaturatingIncrement(
							Metrics.RootMotionCpuNanoseconds,
							static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
														   std::chrono::steady_clock::now() - RootStarted
							)
														   .count())
						);
					}
					if (State.ActiveAction && AuthoritativeTick >= ActionEnd) {
						State.ActiveAction.reset();
						State.ReliableStateRequired = true;
						PromoteCharacter(Id, State, AuthoritativeTick);
					}
				}
			}

			const auto CurrentTransform = CharacterValue->GetCFrame();
			const auto CurrentVelocity = CharacterValue->GetVelocity();
			bool Discontinuity = false;
			bool Moving = glm::length(CurrentVelocity) > 0.01f;
			if (State.LastObservedTransform && AuthoritativeTick >= State.LastObservedTick) {
				const auto TickSpan = AuthoritativeTick - State.LastObservedTick;
				const auto Travel = glm::distance(CurrentTransform.Position, State.LastObservedTransform->Position);
				const auto Expected = ExpectedTravelDistance(
					State.LastObservedVelocity, CurrentVelocity, TickSpan, Configuration.SimulationTicksPerSecond
				);
				Discontinuity = Travel >= MaximumHardCorrectionDistance + Expected;
				Moving = Moving || Travel > 0.001f;
			}
			if (Moving && !State.WasMoving) PromoteCharacter(Id, State, AuthoritativeTick);
			State.LastObservedTransform = CurrentTransform;
			State.LastObservedVelocity = CurrentVelocity;
			State.LastObservedTick = AuthoritativeTick;
			State.WasMoving = Moving;
			if (!Peers.empty() && State.NextStateSequence.IsValid()) {
				const CharacterAuthoritativeState FingerprintState{
					.Character = Id,
					.ControlEpoch = State.ControlEpoch,
					.StateSequence = State.NextStateSequence,
					.AcknowledgedInput = State.AcknowledgedInput,
					.ResolvedAction = State.ResolvedAction,
					.AuthoritativeTick = AuthoritativeTick,
					.Transform = CurrentTransform,
					.Velocity = CurrentVelocity,
					.FloorNormal = CharacterValue->GetFloorNormal(),
					.Flags = static_cast<std::uint8_t>(
						CharacterValue->GetGrounded() ? static_cast<std::uint8_t>(CharacterStateFlag::Grounded) : 0
					),
					.ActiveAction = State.ActiveAction,
				};
				State.CurrentFingerprint = SemanticStateFingerprint(FingerprintState);
				State.CurrentFingerprintTick = AuthoritativeTick;
			}

			if (Discontinuity || State.ReliableStateRequired) {
				const bool HasRecipient = std::ranges::any_of(Peers, [Id](const auto &Entry) {
					return Entry.second.Materialized.contains(Id);
				});
				if (Discontinuity) PromoteCharacter(Id, State, AuthoritativeTick);
				if (!HasRecipient || PublishState(Id, AuthoritativeTick, Discontinuity, true))
					State.ReliableStateRequired = false;
			}
			++Iterator;
		}
		UpdateImportance(AuthoritativeTick);
		PublishStateFrames(AuthoritativeTick);
	}

	std::optional<CharacterAuthoritativeState>
	AuthoritativeCharacterNetwork::BuildState(ObjectId Character, std::uint64_t AuthoritativeTick, bool Teleport) {
		auto Found = Characters.find(Character);
		if (Found == Characters.end() || AuthoritativeTick == 0 || !Found->second.NextStateSequence.IsValid())
			return std::nullopt;
		auto CharacterValue = Found->second.Character.lock();
		if (!CharacterValue || CharacterValue->GetDestroyed() || CharacterValue->IsDestroying()) return std::nullopt;
		auto &Runtime = Found->second;
		return CharacterAuthoritativeState{
			.Character = Character,
			.ControlEpoch = Runtime.ControlEpoch,
			.StateSequence = Runtime.NextStateSequence,
			.AcknowledgedInput = Runtime.AcknowledgedInput,
			.ResolvedAction = Runtime.ResolvedAction,
			.AuthoritativeTick = AuthoritativeTick,
			.Transform = CharacterValue->GetCFrame(),
			.Velocity = CharacterValue->GetVelocity(),
			.FloorNormal = CharacterValue->GetFloorNormal(),
			.Flags = static_cast<std::uint8_t>(
				(CharacterValue->GetGrounded() ? static_cast<std::uint8_t>(CharacterStateFlag::Grounded) : 0) |
				(Teleport ? static_cast<std::uint8_t>(CharacterStateFlag::Teleport) : 0)
			),
			.ActiveAction = Runtime.ActiveAction,
		};
	}

	void AuthoritativeCharacterNetwork::PromoteCharacter(
		ObjectId Character, CharacterState &State, std::uint64_t AuthoritativeTick
	) {
		const auto PromotionUntil = SaturatingTickAdd(AuthoritativeTick, Configuration.PromotionTicks);
		if (PromotionUntil > State.SemanticPromotionUntilTick) {
			State.SemanticPromotionUntilTick = PromotionUntil;
			SaturatingIncrement(Metrics.TemporaryPromotions);
		}
		for (auto &[Connection, Peer] : Peers) {
			if (!Peer.Materialized.contains(Character)) continue;
			auto Publication = Peer.Published.find(Character);
			if (Publication == Peer.Published.end()) continue;
			Publication->second.TemporaryFullRateUntilTick = std::max(
				Publication->second.TemporaryFullRateUntilTick, PromotionUntil
			);
			Peer.ImportanceDirty = true;
			RefreshRelationshipSchedule(Peer, Connection, Character, AuthoritativeTick);
		}
	}

	void AuthoritativeCharacterNetwork::MakeRelationshipDue(
		PeerState &Peer, ConnectionId Connection, ObjectId Character, std::uint64_t AuthoritativeTick, bool Forced
	) {
		auto Publication = Peer.Published.find(Character);
		auto Runtime = Characters.find(Character);
		if (Publication == Peer.Published.end() || Runtime == Characters.end() ||
			!Peer.Materialized.contains(Character))
			return;
		auto &State = Publication->second;
		const bool Owner = Runtime->second.Controller == Connection;
		const bool Promoted = Runtime->second.SemanticPromotionUntilTick > AuthoritativeTick ||
							  State.TemporaryFullRateUntilTick > AuthoritativeTick;
		State.EffectiveTier = Owner || Promoted ? CharacterPublicationTier::FullRate : State.Tier;
		if (State.DesiredDueTick == 0 || State.DesiredDueTick > AuthoritativeTick)
			State.DesiredDueTick = AuthoritativeTick;
		const auto Interval = Configuration.PublicationIntervalTicks(State.EffectiveTier);
		if (State.HardDeadlineTick == 0) State.HardDeadlineTick = SaturatingTickAdd(State.DesiredDueTick, Interval);
		if (!Forced && AuthoritativeTick >= State.HardDeadlineTick && !State.DeadlineEscalationRecorded) {
			State.DeadlineEscalationRecorded = true;
			SaturatingIncrement(Metrics.PublicationDeadlineEscalations);
		}
		if (!Forced && AuthoritativeTick > State.HardDeadlineTick && !State.DeadlineMissRecorded) {
			State.DeadlineMissRecorded = true;
			RecordPublicationDeadlineMiss(Metrics, State.EffectiveTier);
		}
		if (Forced || State.PublishImmediately) {
			State.PublishImmediately = true;
			Peer.LinkBack(Character, PeerState::PublicationQueueKind::Forced);
			return;
		}
		Peer.LinkBack(
			Character,
			Owner														? PeerState::PublicationQueueKind::Owner
			: State.EffectiveTier == CharacterPublicationTier::FullRate ? PeerState::PublicationQueueKind::FullRate
			: State.EffectiveTier == CharacterPublicationTier::ReducedRate
				? PeerState::PublicationQueueKind::ReducedRate
				: PeerState::PublicationQueueKind::LowRate
		);
	}

	void AuthoritativeCharacterNetwork::ScheduleRelationship(
		PeerState &Peer,
		ConnectionId Connection,
		ObjectId Character,
		std::uint64_t AuthoritativeTick,
		bool IncludeCurrentPhase
	) {
		auto Publication = Peer.Published.find(Character);
		auto Runtime = Characters.find(Character);
		if (Publication == Peer.Published.end() || Runtime == Characters.end() ||
			!Peer.Materialized.contains(Character))
			return;
		auto &State = Publication->second;
		if (State.PublishImmediately || !State.HasPublished) {
			MakeRelationshipDue(Peer, Connection, Character, AuthoritativeTick, true);
			return;
		}
		const bool Owner = Runtime->second.Controller == Connection;
		const bool Promoted = Runtime->second.SemanticPromotionUntilTick > AuthoritativeTick ||
							  State.TemporaryFullRateUntilTick > AuthoritativeTick;
		State.EffectiveTier = Owner || Promoted ? CharacterPublicationTier::FullRate : State.Tier;
		const auto Interval = Configuration.PublicationIntervalTicks(State.EffectiveTier);
		const auto Phase = State.EffectiveTier == CharacterPublicationTier::FullRate && Peer.PublicationFocus.empty()
							   ? 0
							   : PublicationPhase(Connection, Character, Interval);
		State.DesiredDueTick = NextPublicationPhaseTick(AuthoritativeTick, Interval, Phase, IncludeCurrentPhase);
		if (State.HasPublished)
			State.DesiredDueTick = std::min(
				State.DesiredDueTick, SaturatingTickAdd(State.LastAbsoluteTick, Configuration.AbsoluteRefreshTicks)
			);
		State.HardDeadlineTick = SaturatingTickAdd(State.DesiredDueTick, Interval);
		if (State.DesiredDueTick <= AuthoritativeTick) {
			MakeRelationshipDue(Peer, Connection, Character, AuthoritativeTick, false);
			return;
		}
		const auto Slot = static_cast<std::uint8_t>(State.DesiredDueTick % CharacterPublicationWheelSlots);
		Peer.LinkBack(Character, PeerState::PublicationQueueKind::Wheel, Slot);
	}

	void AuthoritativeCharacterNetwork::RefreshRelationshipSchedule(
		PeerState &Peer, ConnectionId Connection, ObjectId Character, std::uint64_t AuthoritativeTick
	) {
		auto Publication = Peer.Published.find(Character);
		auto Runtime = Characters.find(Character);
		if (Publication == Peer.Published.end() || Runtime == Characters.end()) return;
		auto &State = Publication->second;
		if (State.QueueKind == PeerState::PublicationQueueKind::Forced || State.PublishImmediately) {
			MakeRelationshipDue(Peer, Connection, Character, AuthoritativeTick, true);
			return;
		}
		const bool AlreadyDue = State.QueueKind != PeerState::PublicationQueueKind::Wheel &&
								State.QueueKind != PeerState::PublicationQueueKind::None;
		if (AlreadyDue) {
			const bool Owner = Runtime->second.Controller == Connection;
			const bool Promoted = Runtime->second.SemanticPromotionUntilTick > AuthoritativeTick ||
								  State.TemporaryFullRateUntilTick > AuthoritativeTick;
			const auto PreviousInterval = Configuration.PublicationIntervalTicks(State.EffectiveTier);
			State.EffectiveTier = Owner || Promoted ? CharacterPublicationTier::FullRate : State.Tier;
			const auto Interval = Configuration.PublicationIntervalTicks(State.EffectiveTier);
			const auto CandidateDeadline = SaturatingTickAdd(State.DesiredDueTick, Interval);
			State.HardDeadlineTick = Interval < PreviousInterval ? std::min(State.HardDeadlineTick, CandidateDeadline)
																 : std::max(State.HardDeadlineTick, CandidateDeadline);
			if (State.HardDeadlineTick > AuthoritativeTick) {
				State.DeadlineEscalationRecorded = false;
				State.DeadlineMissRecorded = false;
			}
			MakeRelationshipDue(Peer, Connection, Character, AuthoritativeTick, false);
			return;
		}
		ScheduleRelationship(Peer, Connection, Character, AuthoritativeTick, true);
	}

	void AuthoritativeCharacterNetwork::UpdateImportance(std::uint64_t AuthoritativeTick) {
		if (Peers.empty()) return;
		const auto Started = std::chrono::steady_clock::now();
		for (auto &[Connection, Peer] : Peers) {
			if (!Peer.ImportanceDirty && Peer.LastImportanceUpdateTick != 0 &&
				AuthoritativeTick >= Peer.LastImportanceUpdateTick &&
				AuthoritativeTick - Peer.LastImportanceUpdateTick < Configuration.ImportanceUpdateTicks)
				continue;
			for (const auto &[Character, Channel] : Peer.Materialized) {
				(void)Channel;
				auto Runtime = Characters.find(Character);
				auto Publication = Peer.Published.find(Character);
				if (Runtime == Characters.end() || Publication == Peer.Published.end()) continue;
				SaturatingIncrement(Metrics.ImportanceEvaluations);
				auto CharacterValue = Runtime->second.Character.lock();
				if (!CharacterValue || CharacterValue->GetDestroyed() || CharacterValue->IsDestroying()) continue;
				CharacterPublicationTier NextTier = Publication->second.Tier;
				const bool Owner = Runtime->second.Controller == Connection;
				const bool Promoted = Runtime->second.SemanticPromotionUntilTick > AuthoritativeTick ||
									  Publication->second.TemporaryFullRateUntilTick > AuthoritativeTick;
				if (Owner || Promoted || Peer.PublicationFocus.empty()) {
					NextTier = CharacterPublicationTier::FullRate;
				} else {
					float MinimumDistance = std::numeric_limits<float>::max();
					for (const auto Focus : Peer.PublicationFocus)
						MinimumDistance = std::min(
							MinimumDistance, glm::distance(CharacterValue->GetPosition(), Focus)
						);
					switch (Publication->second.Tier) {
					case CharacterPublicationTier::FullRate:
						NextTier = MinimumDistance <=
										   Configuration.FullRateDistance + Configuration.ImportanceHysteresis
									   ? CharacterPublicationTier::FullRate
								   : MinimumDistance >
										   Configuration.ReducedRateDistance + Configuration.ImportanceHysteresis
									   ? CharacterPublicationTier::LowRate
									   : CharacterPublicationTier::ReducedRate;
						break;
					case CharacterPublicationTier::ReducedRate:
						NextTier = MinimumDistance < Configuration.FullRateDistance - Configuration.ImportanceHysteresis
									   ? CharacterPublicationTier::FullRate
								   : MinimumDistance >
										   Configuration.ReducedRateDistance + Configuration.ImportanceHysteresis
									   ? CharacterPublicationTier::LowRate
									   : CharacterPublicationTier::ReducedRate;
						break;
					case CharacterPublicationTier::LowRate:
						NextTier = MinimumDistance >=
										   Configuration.ReducedRateDistance - Configuration.ImportanceHysteresis
									   ? CharacterPublicationTier::LowRate
								   : MinimumDistance <
										   Configuration.FullRateDistance - Configuration.ImportanceHysteresis
									   ? CharacterPublicationTier::FullRate
									   : CharacterPublicationTier::ReducedRate;
						break;
					}
				}
				if (NextTier != Publication->second.Tier) {
					Publication->second.Tier = NextTier;
					SaturatingIncrement(Metrics.ImportanceTierTransitions);
				}
				const auto ExpectedEffective = Owner || Promoted ? CharacterPublicationTier::FullRate : NextTier;
				if (Publication->second.EffectiveTier != ExpectedEffective)
					RefreshRelationshipSchedule(Peer, Connection, Character, AuthoritativeTick);
			}
			Peer.LastImportanceUpdateTick = AuthoritativeTick;
			Peer.ImportanceDirty = false;
		}
		SaturatingIncrement(
			Metrics.ImportanceEvaluationCpuNanoseconds,
			static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Started).count()
			)
		);
	}

	bool AuthoritativeCharacterNetwork::PublishState(
		ObjectId Character, std::uint64_t AuthoritativeTick, bool Teleport, bool Reliable
	) {
		auto Found = Characters.find(Character);
		auto State = BuildState(Character, AuthoritativeTick, Teleport);
		if (Found == Characters.end() || !State) return false;
		bool Sent = false;
		const auto Fingerprint = SemanticStateFingerprint(*State);
		for (auto &[Connection, Peer] : Peers) {
			auto Visible = Peer.Materialized.find(Character);
			if (Visible == Peer.Materialized.end()) continue;
			bool Queued = false;
			if (Peer.NextFrameSequence.IsValid() && GetCompactCharacterStateEncodedBytes(*State) != 0) {
				CharacterStateFrame Frame{
					.ServerTick = AuthoritativeTick,
					.FrameSequence = Peer.NextFrameSequence,
					.MaterializationEpoch = Peer.MaterializationEpoch,
					.StateCount = 1,
				};
				Frame.States[0] = *State;
				Queued = QueueStateFrame(Connection, Frame, Visible->second, Reliable);
				Peer.NextFrameSequence = Peer.NextFrameSequence.TryNext().value_or(CharacterStateFrameSequence{});
				if (Queued) {
					auto &Publication = Peer.Published[Character];
					const auto Age = Publication.HasPublished && AuthoritativeTick >= Publication.LastPublicationTick
										 ? AuthoritativeTick - Publication.LastPublicationTick
										 : 0;
					Publication.Fingerprint = Fingerprint;
					Publication.LastAbsoluteTick = AuthoritativeTick;
					Publication.LastPublicationTick = AuthoritativeTick;
					Publication.HasPublished = true;
					Publication.PublishImmediately = false;
					ScheduleRelationship(Peer, Connection, Character, AuthoritativeTick, false);
					SaturatingIncrement(Metrics.StateAgeSamples);
					SaturatingIncrement(Metrics.StateAgeTicks, Age);
					Metrics.MaximumStateAgeTicks = std::max(Metrics.MaximumStateAgeTicks, Age);
				}
			}
			if (Queued) {
				Sent = true;
				SaturatingIncrement(Metrics.AuthoritativeStatesSent);
				SaturatingIncrement(Metrics.AbsoluteStatesSent);
				SaturatingIncrement(Metrics.SemanticStateBytes, SemanticStateBytes(*State));
				if (Reliable) SaturatingIncrement(Metrics.ForcedSemanticPublications);
			}
		}
		Found->second.NextStateSequence = Found->second.NextStateSequence.TryNext().value_or(RealtimeStateSequence{});
		return Sent;
	}

	void AuthoritativeCharacterNetwork::PublishStateFrames(std::uint64_t AuthoritativeTick) {
		if (Peers.empty()) return;
		SaturatingIncrement(Metrics.PublicationBudgetTicks);
		SaturatingIncrement(
			Metrics.PublicationBudgetAvailable,
			static_cast<std::uint64_t>(Configuration.MaximumPublicationStatesPerTick)
		);
		for (auto &[Id, Runtime] : Characters) {
			(void)Id;
			Runtime.PreparedState.reset();
			Runtime.PreparedFingerprint = 0;
			Runtime.PreparedTick = 0;
		}
		const auto DueStarted = std::chrono::steady_clock::now();
		for (auto &[Connection, Peer] : Peers) {
			Peer.SelectedThisTick = 0;
			Peer.AcceptedBatchesThisTick = 0;
			Peer.SchedulerRejectedThisTick = false;
			auto ProcessSlot = [&](std::uint8_t Slot) {
				auto Current = Peer.PublicationWheel[Slot].Head;
				auto Remaining = Peer.PublicationWheel[Slot].Size;
				while (Current.IsValid() && Remaining-- != 0) {
					auto Publication = Peer.Published.find(Current);
					if (Publication == Peer.Published.end()) break;
					const auto Next = Publication->second.NextScheduled;
					if (Publication->second.DesiredDueTick <= AuthoritativeTick) {
						Peer.Unlink(Current);
						if (!Peer.Materialized.contains(Current) || !Characters.contains(Current)) {
							Current = Next;
							continue;
						}
						SaturatingIncrement(Metrics.DueStates);
						MakeRelationshipDue(Peer, Connection, Current, AuthoritativeTick, false);
					}
					Current = Next;
				}
			};
			if (Peer.LastDueProcessingTick == 0) {
				ProcessSlot(static_cast<std::uint8_t>(AuthoritativeTick % CharacterPublicationWheelSlots));
			} else if (AuthoritativeTick < Peer.LastDueProcessingTick ||
					   AuthoritativeTick - Peer.LastDueProcessingTick >= CharacterPublicationWheelSlots) {
				for (std::size_t Slot = 0; Slot < CharacterPublicationWheelSlots; ++Slot)
					ProcessSlot(static_cast<std::uint8_t>(Slot));
				SaturatingIncrement(Metrics.PublicationLargeTickRebuilds);
			} else {
				for (auto Tick = Peer.LastDueProcessingTick + 1; Tick <= AuthoritativeTick; ++Tick)
					ProcessSlot(static_cast<std::uint8_t>(Tick % CharacterPublicationWheelSlots));
			}
			Peer.LastDueProcessingTick = AuthoritativeTick;
			SaturatingIncrement(Metrics.DueStates, static_cast<std::uint64_t>(Peer.ForcedDue.Size));
			SaturatingIncrement(
				Metrics.PublicationOfferedStates,
				static_cast<std::uint64_t>(
					Peer.OwnerDue.Size + Peer.FullRateDue.Size + Peer.ReducedRateDue.Size + Peer.LowRateDue.Size
				)
			);
		}
		const auto DueElapsed = static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - DueStarted).count()
		);
		SaturatingIncrement(Metrics.DueSetCpuNanoseconds, DueElapsed);
		SaturatingIncrement(Metrics.PublicationDueDiscoveryCpuNanoseconds, DueElapsed);

		const auto ByteLimit = std::min(Configuration.MaximumStateFrameBytes, Limits.MaximumUnreliableMessageBytes);
		std::uint64_t ChangeDetectionNanoseconds = 0;
		std::uint64_t SnapshotNanoseconds = 0;
		std::uint64_t AssemblyNanoseconds = 0;
		std::uint64_t CommitNanoseconds = 0;
		std::uint64_t SelectionNanoseconds = 0;
		auto SelectNext = [&](PeerState &Peer, bool ForcedOnly) -> ObjectId {
			if (ForcedOnly) return Peer.PopFront(PeerState::PublicationQueueKind::Forced);
			if (Peer.OwnerDue.Head.IsValid()) return Peer.PopFront(PeerState::PublicationQueueKind::Owner);
			ObjectId DeadlineCandidate;
			PeerState::PublicationQueueKind DeadlineKind = PeerState::PublicationQueueKind::None;
			auto ConsiderDeadline = [&](PeerState::PublicationQueueKind Kind, const PeerState::PublicationList &List) {
				if (!List.Head.IsValid()) return;
				auto Candidate = Peer.Published.find(List.Head);
				if (Candidate == Peer.Published.end() || Candidate->second.HardDeadlineTick > AuthoritativeTick) return;
				if (!Candidate->second.DeadlineEscalationRecorded) {
					Candidate->second.DeadlineEscalationRecorded = true;
					SaturatingIncrement(Metrics.PublicationDeadlineEscalations);
				}
				if (!Candidate->second.DeadlineMissRecorded && AuthoritativeTick > Candidate->second.HardDeadlineTick) {
					Candidate->second.DeadlineMissRecorded = true;
					RecordPublicationDeadlineMiss(Metrics, Candidate->second.EffectiveTier);
				}
				if (!DeadlineCandidate.IsValid()) {
					DeadlineCandidate = List.Head;
					DeadlineKind = Kind;
					return;
				}
				const auto Existing = Peer.Published.find(DeadlineCandidate);
				if (Existing == Peer.Published.end() ||
					std::tie(Candidate->second.HardDeadlineTick, Candidate->second.DesiredDueTick, Candidate->first) <
						std::tie(Existing->second.HardDeadlineTick, Existing->second.DesiredDueTick, Existing->first)) {
					DeadlineCandidate = List.Head;
					DeadlineKind = Kind;
				}
			};
			ConsiderDeadline(PeerState::PublicationQueueKind::FullRate, Peer.FullRateDue);
			ConsiderDeadline(PeerState::PublicationQueueKind::ReducedRate, Peer.ReducedRateDue);
			ConsiderDeadline(PeerState::PublicationQueueKind::LowRate, Peer.LowRateDue);
			if (DeadlineCandidate.IsValid()) {
				Peer.Unlink(DeadlineCandidate);
				return DeadlineCandidate;
			}
			if (Peer.FullRateDue.Head.IsValid()) return Peer.PopFront(PeerState::PublicationQueueKind::FullRate);
			if (Peer.ReducedRateDue.Head.IsValid()) return Peer.PopFront(PeerState::PublicationQueueKind::ReducedRate);
			return Peer.PopFront(PeerState::PublicationQueueKind::LowRate);
		};

		auto PublishPeer =
			[&](ConnectionId Connection, PeerState &Peer, std::size_t MaximumStates, bool ForcedOnly) -> std::size_t {
			if (MaximumStates == 0 || Peer.SchedulerRejectedThisTick) return 0;
			CharacterStateFrame Frame{
				.ServerTick = AuthoritativeTick,
				.MaterializationEpoch = Peer.MaterializationEpoch,
			};
			std::size_t EncodedBytes = CharacterStateFrameHeaderBytes;
			StateChannelId FrameChannel;
			bool Stop = false;
			auto Flush = [&] {
				if (Frame.StateCount == 0 || !Peer.NextFrameSequence.IsValid()) return true;
				std::sort(
					Frame.States.begin(),
					Frame.States.begin() + Frame.StateCount,
					[](const CharacterAuthoritativeState &Left, const CharacterAuthoritativeState &Right) {
						return Left.Character < Right.Character;
					}
				);
				Frame.FrameSequence = Peer.NextFrameSequence;
				const bool Queued = QueueStateFrame(Connection, Frame, FrameChannel);
				Peer.NextFrameSequence = Peer.NextFrameSequence.TryNext().value_or(CharacterStateFrameSequence{});
				const auto CommitStarted = std::chrono::steady_clock::now();
				if (Queued) {
					if (Peer.AcceptedBatchesThisTick++ != 0) SaturatingIncrement(Metrics.BatchSplits);
					for (const auto &State : Frame.GetStates()) {
						auto Runtime = Characters.find(State.Character);
						auto Publication = Peer.Published.find(State.Character);
						if (Runtime == Characters.end() || Publication == Peer.Published.end()) continue;
						const bool Owner = Runtime->second.Controller == Connection;
						const auto Age = Publication->second.HasPublished &&
												 AuthoritativeTick >= Publication->second.LastPublicationTick
											 ? AuthoritativeTick - Publication->second.LastPublicationTick
											 : 0;
						if (!ForcedOnly) {
							const auto Latency = AuthoritativeTick >= Publication->second.DesiredDueTick
													 ? AuthoritativeTick - Publication->second.DesiredDueTick
													 : 0;
							SaturatingIncrement(Metrics.PublicationLatencySamples);
							SaturatingIncrement(Metrics.PublicationLatencyTicks, Latency);
							Metrics.MaximumPublicationLatencyTicks = std::max(
								Metrics.MaximumPublicationLatencyTicks, Latency
							);
							if (Latency >= 1 && Latency <= 3)
								SaturatingIncrement(Metrics.PublicationLatencyOneToThree);
							else if (Latency <= 6 && Latency != 0)
								SaturatingIncrement(Metrics.PublicationLatencyFourToSix);
							else if (Latency <= 12 && Latency != 0)
								SaturatingIncrement(Metrics.PublicationLatencySevenToTwelve);
							else if (Latency > 12)
								SaturatingIncrement(Metrics.PublicationLatencyOverTwelve);
							if (Owner) {
								SaturatingIncrement(Metrics.PublicationOwnerAgeSamples);
								SaturatingIncrement(Metrics.PublicationOwnerAgeTicks, Age);
								Metrics.MaximumPublicationOwnerAgeTicks = std::max(
									Metrics.MaximumPublicationOwnerAgeTicks, Age
								);
							}
						}
						Publication->second.Fingerprint = Runtime->second.PreparedFingerprint;
						Publication->second.LastAbsoluteTick = AuthoritativeTick;
						Publication->second.LastPublicationTick = AuthoritativeTick;
						Publication->second.HasPublished = true;
						Publication->second.PublishImmediately = false;
						Publication->second.DeadlineEscalationRecorded = false;
						Publication->second.DeadlineMissRecorded = false;
						ScheduleRelationship(Peer, Connection, State.Character, AuthoritativeTick, false);
						SaturatingIncrement(Metrics.StateAgeSamples);
						SaturatingIncrement(Metrics.StateAgeTicks, Age);
						Metrics.MaximumStateAgeTicks = std::max(Metrics.MaximumStateAgeTicks, Age);
						const auto StateBytes = static_cast<std::uint64_t>(GetCompactCharacterStateEncodedBytes(State));
						switch (Publication->second.EffectiveTier) {
						case CharacterPublicationTier::FullRate:
							SaturatingIncrement(Metrics.FullRateStatesSent);
							SaturatingIncrement(Metrics.FullRateStateBytes, StateBytes);
							SaturatingIncrement(Metrics.FullRateStateAgeTicks, Age);
							Metrics.MaximumFullRateStateAgeTicks = std::max(Metrics.MaximumFullRateStateAgeTicks, Age);
							break;
						case CharacterPublicationTier::ReducedRate:
							SaturatingIncrement(Metrics.ReducedRateStatesSent);
							SaturatingIncrement(Metrics.ReducedRateStateBytes, StateBytes);
							SaturatingIncrement(Metrics.ReducedRateStateAgeTicks, Age);
							Metrics.MaximumReducedRateStateAgeTicks = std::max(
								Metrics.MaximumReducedRateStateAgeTicks, Age
							);
							break;
						case CharacterPublicationTier::LowRate:
							SaturatingIncrement(Metrics.LowRateStatesSent);
							SaturatingIncrement(Metrics.LowRateStateBytes, StateBytes);
							SaturatingIncrement(Metrics.LowRateStateAgeTicks, Age);
							Metrics.MaximumLowRateStateAgeTicks = std::max(Metrics.MaximumLowRateStateAgeTicks, Age);
							break;
						}
						SaturatingIncrement(Metrics.AuthoritativeStatesSent);
						SaturatingIncrement(Metrics.AbsoluteStatesSent);
						SaturatingIncrement(Metrics.SemanticStateBytes, SemanticStateBytes(State));
						SaturatingIncrement(
							ForcedOnly ? Metrics.PublicationRequiredStatesAccepted : Metrics.PublicationStatesAccepted
						);
					}
				} else {
					for (const auto &State : Frame.GetStates()) {
						auto Publication = Peer.Published.find(State.Character);
						if (Publication == Peer.Published.end()) continue;
						MakeRelationshipDue(
							Peer, Connection, State.Character, AuthoritativeTick, Publication->second.PublishImmediately
						);
					}
					Peer.SchedulerRejectedThisTick = true;
					Stop = true;
				}
				SaturatingIncrement(
					CommitNanoseconds,
					static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
												   std::chrono::steady_clock::now() - CommitStarted
					)
												   .count())
				);
				Frame = CharacterStateFrame{
					.ServerTick = AuthoritativeTick,
					.MaterializationEpoch = Peer.MaterializationEpoch,
				};
				EncodedBytes = CharacterStateFrameHeaderBytes;
				FrameChannel = {};
				return Queued;
			};
			std::size_t Selected = 0;
			std::size_t Examined = 0;
			const auto MaximumExamined = Peer.Published.size();
			while (Selected < MaximumStates && Examined++ < MaximumExamined && !Stop) {
				const auto SelectionStarted = std::chrono::steady_clock::now();
				const auto Id = SelectNext(Peer, ForcedOnly);
				SaturatingIncrement(
					SelectionNanoseconds,
					static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
												   std::chrono::steady_clock::now() - SelectionStarted
					)
												   .count())
				);
				if (!Id.IsValid()) break;
				auto Runtime = Characters.find(Id);
				auto Publication = Peer.Published.find(Id);
				auto Channel = Peer.Materialized.find(Id);
				if (Runtime == Characters.end() || Publication == Peer.Published.end() ||
					Channel == Peer.Materialized.end())
					continue;
				const auto DetectionStarted = std::chrono::steady_clock::now();
				const bool Refresh = !Publication->second.HasPublished ||
									 AuthoritativeTick < Publication->second.LastAbsoluteTick ||
									 AuthoritativeTick - Publication->second.LastAbsoluteTick >=
										 Configuration.AbsoluteRefreshTicks;
				const bool Changed = Runtime->second.CurrentFingerprintTick != AuthoritativeTick ||
									 Publication->second.Fingerprint != Runtime->second.CurrentFingerprint;
				const bool Required = ForcedOnly || Publication->second.PublishImmediately || Refresh || Changed;
				SaturatingIncrement(
					ChangeDetectionNanoseconds,
					static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
												   std::chrono::steady_clock::now() - DetectionStarted
					)
												   .count())
				);
				SaturatingIncrement(Metrics.StatesConsidered);
				if (!Required) {
					Publication->second.PublishImmediately = false;
					SaturatingIncrement(Metrics.StatesSuppressedUnchanged);
					ScheduleRelationship(Peer, Connection, Id, AuthoritativeTick, false);
					continue;
				}
				if (Runtime->second.PreparedTick != AuthoritativeTick) {
					const auto SnapshotStarted = std::chrono::steady_clock::now();
					auto State = BuildState(Id, AuthoritativeTick);
					SaturatingIncrement(
						SnapshotNanoseconds,
						static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
													   std::chrono::steady_clock::now() - SnapshotStarted
						)
													   .count())
					);
					if (!State || GetCompactCharacterStateEncodedBytes(*State) == 0) {
						SaturatingIncrement(Metrics.ProtocolRejects);
						MakeRelationshipDue(
							Peer, Connection, Id, AuthoritativeTick, Publication->second.PublishImmediately
						);
						continue;
					}
					Runtime->second.PreparedState = *State;
					Runtime->second.PreparedFingerprint = SemanticStateFingerprint(*State);
					Runtime->second.PreparedTick = AuthoritativeTick;
					SaturatingIncrement(Metrics.StateSnapshotsBuilt);
				}
				if (!Runtime->second.PreparedState) {
					MakeRelationshipDue(
						Peer, Connection, Id, AuthoritativeTick, Publication->second.PublishImmediately
					);
					continue;
				}
				const auto StateBytes = GetCompactCharacterStateEncodedBytes(*Runtime->second.PreparedState);
				if (StateBytes == 0 || CharacterStateFrameHeaderBytes + StateBytes > ByteLimit) {
					SaturatingIncrement(Metrics.ProtocolRejects);
					MakeRelationshipDue(
						Peer, Connection, Id, AuthoritativeTick, Publication->second.PublishImmediately
					);
					continue;
				}
				if (Frame.StateCount == Frame.States.size() || EncodedBytes + StateBytes > ByteLimit) (void)Flush();
				if (Stop) break;
				const auto AssemblyStarted = std::chrono::steady_clock::now();
				if (Frame.StateCount == 0) FrameChannel = Channel->second;
				Frame.States[Frame.StateCount++] = *Runtime->second.PreparedState;
				EncodedBytes += StateBytes;
				SaturatingIncrement(
					AssemblyNanoseconds,
					static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
												   std::chrono::steady_clock::now() - AssemblyStarted
					)
												   .count())
				);
				++Selected;
				if (!ForcedOnly) ++Peer.SelectedThisTick;
				SaturatingIncrement(
					ForcedOnly ? Metrics.PublicationRequiredStatesSelected : Metrics.PublicationStatesSelected
				);
				SaturatingIncrement(Metrics.StateSnapshotRelationshipUses);
			}
			if (!Stop) (void)Flush();
			return Selected;
		};

		for (auto &[Connection, Peer] : Peers)
			while (Peer.ForcedDue.Size != 0 && !Peer.SchedulerRejectedThisTick)
				if (PublishPeer(Connection, Peer, Peer.ForcedDue.Size, true) == 0) break;

		std::size_t GlobalConsumed = 0;
		auto Start = PublicationPeerCursor ? Peers.lower_bound(*PublicationPeerCursor) : Peers.begin();
		if (Start == Peers.end()) Start = Peers.begin();
		auto Current = Start;
		auto Advance = [&] {
			if (++Current == Peers.end()) Current = Peers.begin();
		};
		if (!Peers.empty()) {
			for (std::size_t Visited = 0;
				 Visited < Peers.size() && GlobalConsumed < Configuration.MaximumPublicationStatesPerTick;
				 ++Visited) {
				auto &Peer = Current->second;
				const auto Capacity = Peer.SelectedThisTick >= Configuration.MaximumPublicationStatesPerPeerTick
										  ? 0
										  : Configuration.MaximumPublicationStatesPerPeerTick - Peer.SelectedThisTick;
				const auto RemainingGlobal = Configuration.MaximumPublicationStatesPerTick - GlobalConsumed;
				const auto RemainingPeers = Peers.size() - Visited - 1;
				const auto ReservedForPeers = std::min(RemainingPeers, RemainingGlobal);
				const auto Allowance = std::min({
					Configuration.PublicationPeerQuantum,
					Capacity,
					RemainingGlobal - ReservedForPeers,
				});
				const auto Selected = PublishPeer(Current->first, Peer, Allowance, false);
				GlobalConsumed += Selected;
				Advance();
			}
			bool MadeProgress = true;
			while (MadeProgress && GlobalConsumed < Configuration.MaximumPublicationStatesPerTick) {
				MadeProgress = false;
				for (std::size_t Visited = 0;
					 Visited < Peers.size() && GlobalConsumed < Configuration.MaximumPublicationStatesPerTick;
					 ++Visited) {
					auto &Peer = Current->second;
					const auto Capacity = Peer.SelectedThisTick >= Configuration.MaximumPublicationStatesPerPeerTick
											  ? 0
											  : Configuration.MaximumPublicationStatesPerPeerTick -
													Peer.SelectedThisTick;
					const auto RemainingGlobal = Configuration.MaximumPublicationStatesPerTick - GlobalConsumed;
					const auto Allowance = std::min({Configuration.PublicationPeerQuantum, Capacity, RemainingGlobal});
					const auto Selected = PublishPeer(Current->first, Peer, Allowance, false);
					GlobalConsumed += Selected;
					MadeProgress = MadeProgress || Selected != 0;
					Advance();
				}
			}
			if (Current == Start) Advance();
			PublicationPeerCursor = Current->first;
			SaturatingIncrement(Metrics.PublicationPeerFairnessRotations);
		}
		SaturatingIncrement(Metrics.PublicationBudgetConsumed, static_cast<std::uint64_t>(GlobalConsumed));
		std::uint64_t Deferred = 0;
		std::uint64_t OwnerDeferred = 0;
		std::uint64_t Overdue = 0;
		for (auto &[Connection, Peer] : Peers) {
			(void)Connection;
			const auto OrdinaryDue = Peer.OwnerDue.Size + Peer.FullRateDue.Size + Peer.ReducedRateDue.Size +
									 Peer.LowRateDue.Size;
			if (GlobalConsumed >= Configuration.MaximumPublicationStatesPerTick ||
				Peer.SelectedThisTick >= Configuration.MaximumPublicationStatesPerPeerTick) {
				SaturatingIncrement(Deferred, static_cast<std::uint64_t>(OrdinaryDue));
				SaturatingIncrement(OwnerDeferred, static_cast<std::uint64_t>(Peer.OwnerDue.Size));
			}
			for (const auto Kind :
				 {PeerState::PublicationQueueKind::Owner,
				  PeerState::PublicationQueueKind::FullRate,
				  PeerState::PublicationQueueKind::ReducedRate,
				  PeerState::PublicationQueueKind::LowRate}) {
				auto *List = Peer.GetList(Kind);
				if (!List || !List->Head.IsValid()) continue;
				auto Publication = Peer.Published.find(List->Head);
				if (Publication != Peer.Published.end() && Publication->second.DesiredDueTick < AuthoritativeTick)
					SaturatingIncrement(Overdue, static_cast<std::uint64_t>(List->Size));
			}
		}
		SaturatingIncrement(Metrics.PublicationStatesDeferred, Deferred);
		SaturatingIncrement(Metrics.PublicationOwnerDeferrals, OwnerDeferred);
		SaturatingIncrement(Metrics.PublicationOverdueRelationships, Overdue);
		if (Deferred != 0 && GlobalConsumed >= Configuration.MaximumPublicationStatesPerTick)
			SaturatingIncrement(Metrics.PublicationGlobalBudgetExhaustions);
		for (auto &[Id, Runtime] : Characters)
			if (Runtime.PreparedTick == AuthoritativeTick) {
				Runtime.NextStateSequence = Runtime.NextStateSequence.TryNext().value_or(RealtimeStateSequence{});
				Runtime.PreparedState.reset();
			}
		SaturatingIncrement(Metrics.StateChangeDetectionCpuNanoseconds, ChangeDetectionNanoseconds);
		SaturatingIncrement(Metrics.PublicationSnapshotCpuNanoseconds, SnapshotNanoseconds);
		SaturatingIncrement(Metrics.StateFrameAssemblyCpuNanoseconds, AssemblyNanoseconds);
		SaturatingIncrement(Metrics.PublicationCommitCpuNanoseconds, CommitNanoseconds);
		SaturatingIncrement(Metrics.PublicationSelectionCpuNanoseconds, SelectionNanoseconds);
	}

	CharacterNetworkMetrics AuthoritativeCharacterNetwork::GetMetrics() const {
		auto Result = Metrics;
		Result.FullRateRelationships = 0;
		Result.ReducedRateRelationships = 0;
		Result.LowRateRelationships = 0;
		Result.PublicationActiveRelationships = 0;
		Result.PublicationCurrentDueRelationships = 0;
		Result.PublicationCurrentOverdueRelationships = 0;
		Result.PublicationCurrentForcedRelationships = 0;
		Result.MaximumCurrentPublicationAgeTicks = 0;
		Result.MaximumCurrentOwnerPublicationAgeTicks = 0;
		for (const auto &[Connection, Peer] : Peers) {
			for (const auto &[Character, Publication] : Peer.Published) {
				auto Runtime = Characters.find(Character);
				if (Runtime == Characters.end() || !Peer.Materialized.contains(Character)) continue;
				const bool Owner = Runtime->second.Controller == Connection;
				SaturatingIncrement(Result.PublicationActiveRelationships);
				const bool Due = Publication.QueueKind != PeerState::PublicationQueueKind::None &&
								 Publication.QueueKind != PeerState::PublicationQueueKind::Wheel;
				if (Due) SaturatingIncrement(Result.PublicationCurrentDueRelationships);
				if (Publication.QueueKind == PeerState::PublicationQueueKind::Forced)
					SaturatingIncrement(Result.PublicationCurrentForcedRelationships);
				if (Due && Publication.DesiredDueTick < LastAuthoritativeTick)
					SaturatingIncrement(Result.PublicationCurrentOverdueRelationships);
				const auto Age = Publication.HasPublished && LastAuthoritativeTick >= Publication.LastPublicationTick
									 ? LastAuthoritativeTick - Publication.LastPublicationTick
									 : 0;
				Result.MaximumCurrentPublicationAgeTicks = std::max(Result.MaximumCurrentPublicationAgeTicks, Age);
				if (Owner)
					Result.MaximumCurrentOwnerPublicationAgeTicks = std::max(
						Result.MaximumCurrentOwnerPublicationAgeTicks, Age
					);
				const bool Promoted = Runtime->second.SemanticPromotionUntilTick > LastAuthoritativeTick ||
									  Publication.TemporaryFullRateUntilTick > LastAuthoritativeTick;
				const auto Tier = Owner || Promoted ? CharacterPublicationTier::FullRate : Publication.Tier;
				switch (Tier) {
				case CharacterPublicationTier::FullRate:
					SaturatingIncrement(Result.FullRateRelationships);
					break;
				case CharacterPublicationTier::ReducedRate:
					SaturatingIncrement(Result.ReducedRateRelationships);
					break;
				case CharacterPublicationTier::LowRate:
					SaturatingIncrement(Result.LowRateRelationships);
					break;
				}
			}
		}
		return Result;
	}

	std::optional<CharacterPublicationTier>
	AuthoritativeCharacterNetwork::GetPublicationTier(ConnectionId Connection, ObjectId Character) const {
		auto Peer = Peers.find(Connection);
		auto Runtime = Characters.find(Character);
		if (Peer == Peers.end() || Runtime == Characters.end() || !Peer->second.Materialized.contains(Character))
			return std::nullopt;
		auto Publication = Peer->second.Published.find(Character);
		if (Publication == Peer->second.Published.end()) return std::nullopt;
		const bool Owner = Runtime->second.Controller == Connection;
		const bool Promoted = Runtime->second.SemanticPromotionUntilTick > LastAuthoritativeTick ||
							  Publication->second.TemporaryFullRateUntilTick > LastAuthoritativeTick;
		return Owner || Promoted ? CharacterPublicationTier::FullRate : Publication->second.Tier;
	}

	std::optional<CharacterPublicationSchedulingState>
	AuthoritativeCharacterNetwork::GetPublicationSchedulingState(ConnectionId Connection, ObjectId Character) const {
		auto Peer = Peers.find(Connection);
		auto Runtime = Characters.find(Character);
		if (Peer == Peers.end() || Runtime == Characters.end() || !Peer->second.Materialized.contains(Character))
			return std::nullopt;
		auto Publication = Peer->second.Published.find(Character);
		if (Publication == Peer->second.Published.end()) return std::nullopt;
		const auto &State = Publication->second;
		const bool Owner = Runtime->second.Controller == Connection;
		const bool Promoted = Runtime->second.SemanticPromotionUntilTick > LastAuthoritativeTick ||
							  State.TemporaryFullRateUntilTick > LastAuthoritativeTick;
		return CharacterPublicationSchedulingState{
			.Tier = Owner || Promoted ? CharacterPublicationTier::FullRate : State.Tier,
			.LastAcceptedPublicationTick = State.LastPublicationTick,
			.DesiredDueTick = State.DesiredDueTick,
			.HardDeadlineTick = State.HardDeadlineTick,
			.HasPublished = State.HasPublished,
			.Due = State.QueueKind != PeerState::PublicationQueueKind::None &&
				   State.QueueKind != PeerState::PublicationQueueKind::Wheel,
			.Forced = State.QueueKind == PeerState::PublicationQueueKind::Forced,
		};
	}

	struct PredictedCharacterNetwork::ReplicaState {
		struct PresentationSnapshot {
			std::uint64_t Tick = 0;
			CFrame Transform;
			glm::vec3 Velocity{};
		};
		std::weak_ptr<KinematicCharacter> Character;
		CharacterControlEpoch Epoch;
		RealtimeStateSequence LastState;
		CharacterActionSequence LastResolvedAction;
		std::uint64_t LastAuthoritativeTick = 0;
		std::optional<CharacterAuthoritativeState> PendingState;
		std::optional<CharacterActionState> ActiveAction;
		std::array<PresentationSnapshot, MaximumRemoteCharacterSnapshots> PresentationSnapshots{};
		std::size_t PresentationSnapshotStart = 0;
		std::size_t PresentationSnapshotCount = 0;
		CharacterControlEpoch PresentationEpoch;
		std::optional<CFrame> LocalCorrectionStart;
		std::uint64_t LocalCorrectionDurationTicks = 0;
		std::uint64_t LocalCorrectionElapsedTicks = 0;
		std::uint64_t LastPresentationTick = 0;
		bool PresentationResetRequired = false;
	};

	struct PredictedCharacterNetwork::PeerState {
		struct PendingAction {
			CharacterActionSequence Sequence;
			std::uint32_t Token = 0;
		};
		ConnectionId Connection;
		std::optional<CharacterControlTransition> Control;
		std::optional<CharacterControlTransition> PendingControl;
		CharacterControlEpoch LastControlEpoch;
		CharacterInputSequence NextInput{1};
		CharacterActionSequence NextAction{1};
		CharacterActionSequence LastActionResult;
		std::array<CharacterInputCommand, MaximumCharacterPredictionHistory> History{};
		std::size_t HistoryStart = 0;
		std::size_t HistoryCount = 0;
		std::optional<CharacterActionState> PredictedAction;
		std::array<PendingAction, MaximumPendingCharacterActions> PendingActions{};
		std::size_t PendingActionCount = 0;
		std::uint64_t LastActionEvaluationTick = 0;
		bool PredictionSuspended = false;
		CharacterMaterializationEpoch MaterializationEpoch;
	};

	PredictedCharacterNetwork::~PredictedCharacterNetwork() = default;

	PredictedCharacterNetwork::PredictedCharacterNetwork(
		INetworkScheduler &SchedulerValue,
		NetworkLimits LimitsValue,
		CharacterMovementPolicy MovementPolicyValue,
		CharacterNetworkConfiguration ConfigurationValue
	)
		: Scheduler(SchedulerValue), Limits(LimitsValue), MovementPolicy(std::move(MovementPolicyValue)),
		  Configuration(ConfigurationValue) {
		if (!Limits.IsValid() || !MovementPolicy || !Configuration.IsValid())
			throw std::invalid_argument("[Character:Network] predicted manager configuration is invalid");
	}

	bool PredictedCharacterNetwork::AddPeer(ConnectionId Connection) {
		return Connection.IsValid() && Peers.size() < MaximumCharacterNetworkPeers &&
			   Peers.emplace(Connection, PeerState{.Connection = Connection}).second;
	}

	bool PredictedCharacterNetwork::RemovePeer(ConnectionId Connection) {
		if (!Peers.erase(Connection)) return false;
		for (auto &[Id, Replica] : Replicas) {
			if (auto CharacterValue = Replica.Character.lock();
				CharacterValue && !CharacterValue->GetDestroyed() && !CharacterValue->IsDestroying())
				CharacterValue->ApplyRuntimePresentationOffset(std::nullopt);
			Replica.PendingState.reset();
			Replica.PresentationSnapshotStart = 0;
			Replica.PresentationSnapshotCount = 0;
			Replica.PresentationEpoch = {};
			Replica.LocalCorrectionStart.reset();
			Replica.LastPresentationTick = 0;
		}
		return true;
	}

	bool PredictedCharacterNetwork::MarkMaterialized(
		ObjectId Character, const std::shared_ptr<KinematicCharacter> &Replica
	) {
		if (!Character.IsValid() || !Replica || Replica->GetDestroyed() || Replica->IsDestroying() ||
			Replicas.size() >= MaximumNetworkCharacters || Replicas.contains(Character))
			return false;
		for (const auto &[Connection, Peer] : Peers) {
			(void)Connection;
			if (!NextMaterializationEpoch(Peer.MaterializationEpoch)) return false;
		}
		Replicas.emplace(Character, ReplicaState{.Character = Replica});
		for (auto &[Connection, Peer] : Peers) {
			Peer.MaterializationEpoch = *NextMaterializationEpoch(Peer.MaterializationEpoch);
			if (Peer.PendingControl && Peer.PendingControl->Character == Character) {
				Peer.Control = Peer.PendingControl;
				Peer.PendingControl.reset();
				HardReset(Peer);
			}
		}
		return true;
	}

	bool PredictedCharacterNetwork::MarkUnmaterialized(ObjectId Character) {
		auto Replica = Replicas.find(Character);
		if (Replica == Replicas.end()) return false;
		for (const auto &[Connection, Peer] : Peers) {
			(void)Connection;
			if (!NextMaterializationEpoch(Peer.MaterializationEpoch)) return false;
		}
		if (auto CharacterValue = Replica->second.Character.lock();
			CharacterValue && !CharacterValue->GetDestroyed() && !CharacterValue->IsDestroying())
			CharacterValue->ApplyRuntimePresentationOffset(std::nullopt);
		Replicas.erase(Replica);
		for (auto &[Connection, Peer] : Peers) {
			Peer.MaterializationEpoch = *NextMaterializationEpoch(Peer.MaterializationEpoch);
			if (Peer.Control && Peer.Control->Character == Character) {
				Peer.Control.reset();
				HardReset(Peer);
			}
		}
		return true;
	}

	bool PredictedCharacterNetwork::RegisterAction(CharacterActionDefinition Definition) {
		return Definition.IsValid() && Actions.size() < MaximumNetworkCharacters &&
			   Actions.emplace(Definition.Token, std::move(Definition)).second;
	}

	void PredictedCharacterNetwork::SetTerminalHandler(CharacterTerminalHandler Handler) {
		OnTerminal = std::move(Handler);
	}

	bool PredictedCharacterNetwork::Queue(
		ConnectionId Connection, const CharacterMessage &Message, StateChannelId Channel, bool Reliable
	) {
		const auto EncodeStarted = std::chrono::steady_clock::now();
		auto Encoded = EncodeCharacterMessage(Message);
		SaturatingIncrement(
			Metrics.StateEncodeCpuNanoseconds,
			static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - EncodeStarted)
					.count()
			)
		);
		if (!Encoded) return false;
		DeliveryMode Delivery = DeliveryMode::ReliableOrdered;
		TrafficClass Traffic = TrafficClass::ReliableApplication;
		MessageOrder Order;
		if (!Reliable) {
			const auto *Input = std::get_if<CharacterInputCommand>(&Message);
			if (!Input || !Channel.IsValid()) return false;
			Delivery = DeliveryMode::UnreliableSequenced;
			Traffic = TrafficClass::RealtimeState;
			Order = RealtimeStateOrder{Channel, RealtimeStateSequence(Input->InputSequence.Value())};
		}
		auto Intent = MakeNetworkMessageIntent(
			Connection, Delivery, Traffic, std::move(Order), std::move(*Encoded), Limits
		);
		if (!Intent) return false;
		const auto Bytes = Intent->Payload().size();
		const auto SubmitStarted = std::chrono::steady_clock::now();
		auto Result = Scheduler.Submit(std::move(*Intent));
		SaturatingIncrement(
			Metrics.SchedulerSubmitCpuNanoseconds,
			static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - SubmitStarted)
					.count()
			)
		);
		if (!Result.Accepted()) {
			if (Reliable && OnTerminal)
				OnTerminal(
					Connection, SchedulerFailure(Result, "Reliable Character message was rejected by the scheduler")
				);
			return false;
		}
		SaturatingIncrement(Metrics.BytesOut, static_cast<std::uint64_t>(Bytes));
		SaturatingIncrement(Metrics.SchedulerSubmissions);
		return true;
	}

	bool PredictedCharacterNetwork::HandleTransportEvent(const TransportEvent &Event) {
		const auto *Received = std::get_if<ReceivedMessageEvent>(&Event);
		if (!Received || !Peers.contains(Received->Connection)) return false;
		SaturatingIncrement(Metrics.BytesIn, static_cast<std::uint64_t>(Received->Payload.size()));
		const auto DecodeStarted = std::chrono::steady_clock::now();
		auto Decoded = DecodeCharacterMessage(Received->Payload);
		SaturatingIncrement(
			Metrics.ProtocolDecodeCpuNanoseconds,
			static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - DecodeStarted)
					.count()
			)
		);
		if (!Decoded) {
			SaturatingIncrement(Metrics.ProtocolRejects);
			SaturatingIncrement(Metrics.MalformedFramesRejected);
			return false;
		}
		return HandleMessage(Received->Connection, *Received, std::move(*Decoded));
	}

	bool PredictedCharacterNetwork::HandleMessage(
		ConnectionId Connection, const ReceivedMessageEvent &Event, CharacterMessage Message
	) {
		auto &Peer = Peers.at(Connection);
		if (auto *Transition = std::get_if<CharacterControlTransition>(&Message)) {
			if (!ReliableControl(Event)) return false;
			if (!Transition->Bound) {
				const bool MatchesControl = Peer.Control && Peer.Control->Character == Transition->Character &&
											Peer.Control->ControlEpoch == Transition->ControlEpoch;
				const bool MatchesPending = Peer.PendingControl &&
											Peer.PendingControl->Character == Transition->Character &&
											Peer.PendingControl->ControlEpoch == Transition->ControlEpoch;
				if (!MatchesControl && !MatchesPending) return false;
				if (auto Replica = Replicas.find(Transition->Character); Replica != Replicas.end()) {
					Replica->second.LocalCorrectionStart.reset();
					Replica->second.PresentationSnapshotStart = 0;
					Replica->second.PresentationSnapshotCount = 0;
					Replica->second.PresentationResetRequired = true;
				}
				Peer.Control.reset();
				Peer.PendingControl.reset();
				Peer.PendingActionCount = 0;
				Peer.LastActionResult = {};
				HardReset(Peer);
				return true;
			}
			if (Peer.LastControlEpoch.IsValid() && !Transition->ControlEpoch.IsNewerThan(Peer.LastControlEpoch))
				return false;
			Peer.LastControlEpoch = Transition->ControlEpoch;
			Peer.PendingActionCount = 0;
			Peer.LastActionResult = {};
			if (Replicas.contains(Transition->Character)) {
				auto &Replica = Replicas.at(Transition->Character);
				Replica.LocalCorrectionStart.reset();
				Replica.PresentationSnapshotStart = 0;
				Replica.PresentationSnapshotCount = 0;
				Replica.PresentationResetRequired = true;
				Peer.Control = *Transition;
				Peer.PendingControl.reset();
				HardReset(Peer);
			} else {
				Peer.PendingControl = *Transition;
			}
			return true;
		}
		if (auto *Result = std::get_if<CharacterActionResult>(&Message)) {
			if (!ReliableAction(Event) || !Peer.Control || Peer.Control->Character != Result->Character ||
				Peer.Control->ControlEpoch != Result->ControlEpoch)
				return false;
			if (Peer.LastActionResult.IsValid() && !Result->ActionSequence.IsNewerThan(Peer.LastActionResult))
				return true;
			std::size_t Match = Peer.PendingActionCount;
			for (std::size_t Index = 0; Index < Peer.PendingActionCount; ++Index)
				if (Peer.PendingActions[Index].Sequence == Result->ActionSequence) {
					Match = Index;
					break;
				}
			if (Match == Peer.PendingActionCount || Peer.PendingActions[Match].Token != Result->RequestedActionToken)
				return false;
			for (std::size_t Index = Match + 1; Index < Peer.PendingActionCount; ++Index)
				Peer.PendingActions[Index - 1] = Peer.PendingActions[Index];
			--Peer.PendingActionCount;
			Peer.LastActionResult = Result->ActionSequence;
			if (Peer.PredictedAction && Peer.PredictedAction->ActionSequence == Result->ActionSequence) {
				const auto Definition = Result->AuthoritativeAction
											? Actions.find(Result->AuthoritativeAction->ActionToken)
											: Actions.end();
				if (Result->Accepted && Result->AuthoritativeAction && Definition != Actions.end() &&
					Definition->second.Animation == Result->AuthoritativeAction->Animation &&
					Definition->second.ContentRevision == Result->AuthoritativeAction->ContentRevision) {
					Peer.PredictedAction = Result->AuthoritativeAction;
					Peer.LastActionEvaluationTick = std::max(
						Peer.LastActionEvaluationTick, Result->AuthoritativeAction->StartTick
					);
				} else {
					Peer.PredictedAction.reset();
					Peer.LastActionEvaluationTick = 0;
				}
			}
			if (ActionResolutions.size() < MaximumCharacterPredictionHistory)
				ActionResolutions.push_back({
					Result->Character,
					Result->RequestedActionToken,
					Result->Accepted,
					Result->AuthoritativeAction,
				});
			return true;
		}
		if (auto *State = std::get_if<CharacterAuthoritativeState>(&Message)) {
			const bool ReliableState = Event.Delivery == DeliveryMode::ReliableOrdered &&
									   Event.Traffic == TrafficClass::Control &&
									   std::holds_alternative<std::monostate>(Event.Order);
			return HandleState(Peer, Event, *State, ReliableState);
		}
		if (auto *Frame = std::get_if<CharacterStateFrame>(&Message)) {
			const bool ReliableFrame = ReliableControl(Event);
			if (!ReliableFrame && !IsExpectedStateFrameOrder(Event, Frame->FrameSequence)) return false;
			if (Frame->MaterializationEpoch != Peer.MaterializationEpoch) {
				SaturatingIncrement(Metrics.StaleStatesDropped, static_cast<std::uint64_t>(Frame->StateCount));
				return true;
			}
			for (const auto &State : Frame->GetStates()) {
				if (!Replicas.contains(State.Character)) continue;
				(void)HandleState(Peer, Event, State, ReliableFrame, true);
			}
			return true;
		}
		SaturatingIncrement(Metrics.ProtocolRejects);
		return false;
	}

	bool PredictedCharacterNetwork::HandleState(
		PeerState &Peer,
		const ReceivedMessageEvent &Event,
		CharacterAuthoritativeState State,
		bool ReliableState,
		bool FramedState
	) {
		auto Replica = Replicas.find(State.Character);
		if (Replica == Replicas.end()) return false;
		if (!FramedState) {
			StateChannelId Channel;
			if (Peer.Control && Peer.Control->Character == State.Character) Channel = Peer.Control->Channel;
			if (!Channel.IsValid()) {
				const auto *Order = std::get_if<RealtimeStateOrder>(&Event.Order);
				if (Order) Channel = Order->Channel;
			}
			if ((!ReliableState && !Channel.IsValid()) || !IsExpectedStateOrder(Event, Channel, State.StateSequence))
				return false;
		}
		if (Replica->second.Epoch.IsValid() && State.ControlEpoch != Replica->second.Epoch &&
			!State.ControlEpoch.IsNewerThan(Replica->second.Epoch)) {
			SaturatingIncrement(Metrics.StaleStatesDropped);
			return false;
		}
		if (Replica->second.LastState.IsValid() && !State.StateSequence.IsNewerThan(Replica->second.LastState)) {
			if (ReliableState && State.ActiveAction && State.ResolvedAction == Replica->second.LastResolvedAction) {
				auto Definition = Actions.find(State.ActiveAction->ActionToken);
				if (Definition != Actions.end() && Definition->second.Animation == State.ActiveAction->Animation &&
					Definition->second.ContentRevision == State.ActiveAction->ContentRevision) {
					Replica->second.ActiveAction = State.ActiveAction;
					SaturatingIncrement(Metrics.StaleStatesDropped);
					return true;
				}
			}
			SaturatingIncrement(Metrics.StaleStatesDropped);
			return false;
		}
		if (Replica->second.PendingState &&
			!State.StateSequence.IsNewerThan(Replica->second.PendingState->StateSequence)) {
			if (ReliableState && State.ActiveAction &&
				State.ResolvedAction == Replica->second.PendingState->ResolvedAction) {
				Replica->second.PendingState->ActiveAction = State.ActiveAction;
				SaturatingIncrement(Metrics.StaleStatesDropped);
				return true;
			}
			SaturatingIncrement(Metrics.StaleStatesDropped);
			return false;
		}

		const bool LocallyControlled = Peer.Control && Peer.Control->Character == State.Character;
		if (!LocallyControlled) {
			auto &Presentation = Replica->second;
			bool Reset = State.Teleport() || (Presentation.PresentationEpoch.IsValid() &&
											  State.ControlEpoch != Presentation.PresentationEpoch);
			if (Presentation.PresentationSnapshotCount != 0) {
				const auto LastIndex = (Presentation.PresentationSnapshotStart +
										Presentation.PresentationSnapshotCount - 1) %
									   Presentation.PresentationSnapshots.size();
				const auto &Last = Presentation.PresentationSnapshots[LastIndex];
				const auto TickSpan = State.AuthoritativeTick >= Last.Tick ? State.AuthoritativeTick - Last.Tick : 0;
				const auto ExpectedTravel = ExpectedTravelDistance(
					Last.Velocity, State.Velocity, TickSpan, Configuration.SimulationTicksPerSecond
				);
				Reset = Reset || State.AuthoritativeTick < Last.Tick ||
						glm::distance(State.Transform.Position, Last.Transform.Position) >=
							MaximumHardCorrectionDistance + ExpectedTravel;
				if (!Reset && State.AuthoritativeTick == Last.Tick) {
					Presentation.PresentationSnapshots[LastIndex] = {
						State.AuthoritativeTick,
						State.Transform,
						State.Velocity,
					};
					Presentation.PresentationEpoch = State.ControlEpoch;
					Presentation.PendingState = State;
					return true;
				}
			}
			if (Reset) {
				Presentation.PresentationSnapshotStart = 0;
				Presentation.PresentationSnapshotCount = 0;
				Presentation.PresentationResetRequired = true;
				SaturatingIncrement(Metrics.InterpolationResets);
			}
			while (Presentation.PresentationSnapshotCount != 0) {
				const auto &Oldest = Presentation.PresentationSnapshots[Presentation.PresentationSnapshotStart];
				if (State.AuthoritativeTick <= Oldest.Tick ||
					State.AuthoritativeTick - Oldest.Tick <= MaximumRemoteCharacterSnapshotWindowTicks)
					break;
				Presentation.PresentationSnapshotStart = (Presentation.PresentationSnapshotStart + 1) %
														 Presentation.PresentationSnapshots.size();
				--Presentation.PresentationSnapshotCount;
			}
			if (Presentation.PresentationSnapshotCount == Presentation.PresentationSnapshots.size()) {
				Presentation.PresentationSnapshotStart = (Presentation.PresentationSnapshotStart + 1) %
														 Presentation.PresentationSnapshots.size();
				--Presentation.PresentationSnapshotCount;
			}
			const auto Insert = (Presentation.PresentationSnapshotStart + Presentation.PresentationSnapshotCount) %
								Presentation.PresentationSnapshots.size();
			Presentation.PresentationSnapshots[Insert] = {
				State.AuthoritativeTick,
				State.Transform,
				State.Velocity,
			};
			++Presentation.PresentationSnapshotCount;
			Presentation.PresentationEpoch = State.ControlEpoch;
		}
		Replica->second.PendingState = State;
		return true;
	}

	bool PredictedCharacterNetwork::Predict(
		PeerState &Peer, WorldRoot &World, const CharacterInputCommand &Command, bool Record
	) {
		if (Record && Peer.HistoryCount >= Peer.History.size()) {
			SaturatingIncrement(Metrics.HistoryOverflows);
			HardReset(Peer);
			Peer.PredictionSuspended = true;
			return false;
		}
		auto Replica = Replicas.find(Command.Character);
		auto CharacterValue = Replica == Replicas.end() ? nullptr : Replica->second.Character.lock();
		if (!CharacterValue || CharacterValue->GetDestroyed() || CharacterValue->IsDestroying()) return false;
		try {
			auto Request = MovementPolicy(Command, *CharacterValue);
			if (!ValidMotionRequest(Request)) return false;
			auto Result = CharacterValue->AdmitMotion(World, Request);
			if (!Result.Succeeded()) return false;
			CharacterValue->ApplyRuntimeControllerFacts(Result.Velocity, Result.FloorNormal, Result.HasFloor);
			if (Peer.PredictedAction && Command.SimulationTick > Peer.LastActionEvaluationTick) {
				auto Definition = Actions.find(Peer.PredictedAction->ActionToken);
				if (Definition != Actions.end()) {
					const auto ActionEnd = Peer.PredictedAction->StartTick + Peer.PredictedAction->DurationTicks;
					const auto EvaluationTick = std::min(Command.SimulationTick, ActionEnd);
					auto Delta = Definition->second.EvaluateRootMotion(Peer.LastActionEvaluationTick, EvaluationTick);
					Peer.LastActionEvaluationTick = EvaluationTick;
					if (Delta) {
						auto RootResult = CharacterValue->AdmitMotion(
							World,
							{
								.Translation = Delta->Translation,
								.Velocity = CharacterValue->GetVelocity(),
								.YawRadians = Delta->YawRadians,
								.Source = CharacterMotionSource::Animation,
								.LocalSpace = true,
							}
						);
						if (RootResult.Succeeded())
							CharacterValue->ApplyRuntimeControllerFacts(
								RootResult.Velocity, RootResult.FloorNormal, RootResult.HasFloor
							);
					}
					if (Command.SimulationTick >= ActionEnd) Peer.PredictedAction.reset();
				} else
					Peer.PredictedAction.reset();
			}
		} catch (...) {
			return false;
		}
		if (Record) {
			Peer.History[(Peer.HistoryStart + Peer.HistoryCount) % Peer.History.size()] = Command;
			++Peer.HistoryCount;
		}
		return true;
	}

	bool PredictedCharacterNetwork::SubmitInput(
		ConnectionId Connection,
		WorldRoot &World,
		std::uint64_t SimulationTick,
		float DeltaSeconds,
		glm::vec2 MoveIntent,
		float FacingYawRadians,
		bool JumpRequested
	) {
		auto Found = Peers.find(Connection);
		if (Found == Peers.end() || !Found->second.Control || !Found->second.NextInput.IsValid() ||
			(PredictionEnabled && Found->second.PredictionSuspended))
			return false;
		auto &Peer = Found->second;
		if (PredictionEnabled && Peer.HistoryCount >= Peer.History.size()) {
			SaturatingIncrement(Metrics.HistoryOverflows);
			HardReset(Peer);
			Peer.PredictionSuspended = true;
			return false;
		}
		CharacterInputCommand Command{
			.Character = Peer.Control->Character,
			.ControlEpoch = Peer.Control->ControlEpoch,
			.InputSequence = Peer.NextInput,
			.SimulationTick = SimulationTick,
			.DeltaSeconds = DeltaSeconds,
			.MoveIntent = MoveIntent,
			.FacingYawRadians = FacingYawRadians,
			.Flags = static_cast<std::uint8_t>(
				JumpRequested ? static_cast<std::uint8_t>(CharacterInputFlag::JumpRequested) : 0
			),
		};
		if (!Command.IsValid() || !Queue(Connection, CharacterMessage(Command), Peer.Control->Channel, false))
			return false;
		Peer.NextInput = Peer.NextInput.TryNext().value_or(CharacterInputSequence{});
		if (PredictionEnabled && !Predict(Peer, World, Command, true)) {
			HardReset(Peer);
			Peer.PredictionSuspended = true;
		}
		return true;
	}

	bool PredictedCharacterNetwork::RequestAction(
		ConnectionId Connection, std::uint32_t ActionToken, std::uint64_t SimulationTick
	) {
		auto Found = Peers.find(Connection);
		if (Found == Peers.end() || !Found->second.Control || !Found->second.NextAction.IsValid() ||
			Found->second.PredictionSuspended)
			return false;
		auto Definition = Actions.find(ActionToken);
		if (SimulationTick == 0 || Found->second.PendingActionCount >= MaximumPendingCharacterActions) return false;
		auto &Peer = Found->second;
		const auto BasedOn = Peer.NextInput.Value() > 1 ? CharacterInputSequence(Peer.NextInput.Value() - 1)
														: CharacterInputSequence{};
		CharacterActionRequest Request{
			Peer.Control->Character,
			Peer.Control->ControlEpoch,
			Peer.NextAction,
			BasedOn,
			ActionToken,
		};
		if (!Queue(Connection, CharacterMessage(Request), {}, true)) return false;
		Peer.PendingActions[Peer.PendingActionCount++] = {Request.ActionSequence, ActionToken};
		if (PredictionEnabled && Definition != Actions.end()) {
			Peer.PredictedAction = CharacterActionState{
				Request.ActionSequence,
				Definition->second.Token,
				Definition->second.Animation,
				Definition->second.ContentRevision,
				SimulationTick,
				Definition->second.DurationTicks,
			};
			Peer.LastActionEvaluationTick = SimulationTick;
		}
		Peer.NextAction = Peer.NextAction.TryNext().value_or(CharacterActionSequence{});
		return true;
	}

	void PredictedCharacterNetwork::SetPredictionEnabled(bool Enabled) {
		PredictionEnabled = Enabled;
		if (Enabled) return;
		for (auto &[Connection, Peer] : Peers) {
			(void)Connection;
			HardReset(Peer);
		}
	}

	void PredictedCharacterNetwork::Reconcile(WorldRoot &World) {
		const auto ReconcileStarted = std::chrono::steady_clock::now();
		for (auto &[Id, Replica] : Replicas) {
			if (!Replica.PendingState) continue;
			const auto State = *Replica.PendingState;
			const auto PreviousActiveAction = Replica.ActiveAction;
			auto RecordActionEnd = [&] {
				if (PreviousActiveAction &&
					(!Replica.ActiveAction ||
					 Replica.ActiveAction->ActionSequence != PreviousActiveAction->ActionSequence) &&
					ActionEndings.size() < MaximumCharacterPredictionHistory)
					ActionEndings.push_back({Id, PreviousActiveAction->ActionToken});
			};
			Replica.PendingState.reset();
			auto CharacterValue = Replica.Character.lock();
			if (!CharacterValue || CharacterValue->GetDestroyed() || CharacterValue->IsDestroying()) continue;
			PeerState *LocalPeer = nullptr;
			for (auto &[Connection, Peer] : Peers)
				if (Peer.Control && Peer.Control->Character == Id) {
					LocalPeer = &Peer;
					break;
				}
			if (!LocalPeer) {
				if (Replica.Epoch.IsValid() && State.ControlEpoch != Replica.Epoch &&
					!State.ControlEpoch.IsNewerThan(Replica.Epoch)) {
					SaturatingIncrement(Metrics.StaleStatesDropped);
					continue;
				}
				CharacterValue->ApplyRuntimeTransform(State.Transform);
				CharacterValue->ApplyRuntimeControllerFacts(State.Velocity, State.FloorNormal, State.Grounded());
				if (Replica.PresentationResetRequired) {
					CharacterValue->ApplyRuntimePresentationOffset(std::nullopt);
					Replica.PresentationResetRequired = false;
					SaturatingIncrement(Metrics.HardPresentationResets);
				}
				Replica.ActiveAction = State.ActiveAction;
				RecordActionEnd();
				Replica.Epoch = State.ControlEpoch;
				Replica.LastState = State.StateSequence;
				Replica.LastResolvedAction = State.ResolvedAction;
				Replica.LastAuthoritativeTick = State.AuthoritativeTick;
				continue;
			}
			auto &Peer = *LocalPeer;
			if (State.ControlEpoch != Peer.Control->ControlEpoch) {
				SaturatingIncrement(Metrics.StaleStatesDropped);
				continue;
			}
			const auto PreviousPresentation = CharacterValue->GetPresentationCFrame();
			const auto Correction = glm::length(CharacterValue->GetPosition() - State.Transform.Position);
			CharacterValue->ApplyRuntimeTransform(State.Transform);
			CharacterValue->ApplyRuntimeControllerFacts(State.Velocity, State.FloorNormal, State.Grounded());
			if (Correction > 1.0e-4f) SaturatingIncrement(Metrics.PredictionCorrections);
			bool Reset = State.Teleport() || Correction >= MaximumHardCorrectionDistance;
			while (Peer.HistoryCount != 0) {
				const auto &Command = Peer.History[Peer.HistoryStart];
				if (!State.AcknowledgedInput.IsValid() || Command.InputSequence.IsNewerThan(State.AcknowledgedInput))
					break;
				Peer.HistoryStart = (Peer.HistoryStart + 1) % Peer.History.size();
				--Peer.HistoryCount;
			}
			if (State.AcknowledgedInput.IsValid() && Peer.NextInput.IsValid() &&
				State.AcknowledgedInput.Value() >= Peer.NextInput.Value())
				Reset = true;
			const auto PreviousPredictedAction = Peer.PredictedAction;
			const auto PreviousActionTick = Peer.LastActionEvaluationTick;
			Peer.PredictedAction.reset();
			if (PreviousPredictedAction &&
				(!State.ResolvedAction.IsValid() ||
				 PreviousPredictedAction->ActionSequence.IsNewerThan(State.ResolvedAction))) {
				Peer.PredictedAction = PreviousPredictedAction;
				Peer.LastActionEvaluationTick = PreviousActionTick;
			} else if (State.ActiveAction && PredictionEnabled) {
				auto Definition = Actions.find(State.ActiveAction->ActionToken);
				if (Definition == Actions.end() || Definition->second.Animation != State.ActiveAction->Animation ||
					Definition->second.ContentRevision != State.ActiveAction->ContentRevision) {
					Reset = true;
				} else {
					Peer.PredictedAction = State.ActiveAction;
					Peer.LastActionEvaluationTick = State.AuthoritativeTick;
				}
			}
			if (Reset || !PredictionEnabled) {
				HardReset(Peer);
			} else {
				const auto Count = Peer.HistoryCount;
				const auto ReplayStarted = std::chrono::steady_clock::now();
				for (std::size_t Index = 0; Index < Count; ++Index) {
					const auto Command = Peer.History[(Peer.HistoryStart + Index) % Peer.History.size()];
					(void)Predict(Peer, World, Command, false);
					SaturatingIncrement(Metrics.PredictedCommandsReplayed);
				}
				SaturatingIncrement(
					Metrics.ReplayCpuNanoseconds,
					static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
												   std::chrono::steady_clock::now() - ReplayStarted
					)
												   .count())
				);
			}
			const auto PresentationError = glm::distance(
				PreviousPresentation.Position, CharacterValue->GetCFrame().Position
			);
			if (!Reset && PresentationError > 1.0e-4f &&
				PresentationError <= MaximumSmoothPresentationCorrectionDistance) {
				Replica.LocalCorrectionStart = CharacterValue->GetCFrame().ToObjectSpace(PreviousPresentation);
				Replica.LocalCorrectionDurationTicks = PresentationError <= TinyPresentationCorrectionDistance
														   ? TinyCorrectionSmoothingTicks
														   : SmallCorrectionSmoothingTicks;
				Replica.LocalCorrectionElapsedTicks = 0;
				Replica.LastPresentationTick = 0;
				CharacterValue->ApplyRuntimePresentationOffset(Replica.LocalCorrectionStart);
				SaturatingIncrement(Metrics.LocalSmoothCorrections);
			} else {
				Replica.LocalCorrectionStart.reset();
				Replica.LocalCorrectionDurationTicks = 0;
				Replica.LocalCorrectionElapsedTicks = 0;
				Replica.LastPresentationTick = 0;
				CharacterValue->ApplyRuntimePresentationOffset(std::nullopt);
				if (Reset || PresentationError > MaximumSmoothPresentationCorrectionDistance)
					SaturatingIncrement(Metrics.HardPresentationResets);
			}
			if (Reset)
				Replica.ActiveAction.reset();
			else
				Replica.ActiveAction = State.ActiveAction;
			RecordActionEnd();
			Peer.PredictionSuspended = false;
			Replica.Epoch = State.ControlEpoch;
			Replica.LastState = State.StateSequence;
			Replica.LastResolvedAction = State.ResolvedAction;
			Replica.LastAuthoritativeTick = State.AuthoritativeTick;
		}
		SaturatingIncrement(
			Metrics.ReconciliationCpuNanoseconds,
			static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
										   std::chrono::steady_clock::now() - ReconcileStarted
			)
										   .count())
		);
	}

	void PredictedCharacterNetwork::UpdatePresentation(std::uint64_t PresentationTick) {
		if (PresentationTick == 0) return;
		const auto Started = std::chrono::steady_clock::now();
		const CFrame Identity;
		for (auto &[Id, Replica] : Replicas) {
			auto CharacterValue = Replica.Character.lock();
			if (!CharacterValue || CharacterValue->GetDestroyed() || CharacterValue->IsDestroying()) continue;
			if (Replica.PresentationResetRequired) {
				CharacterValue->ApplyRuntimePresentationOffset(std::nullopt);
				Replica.LocalCorrectionStart.reset();
				Replica.LastPresentationTick = PresentationTick;
				Replica.PresentationResetRequired = false;
				SaturatingIncrement(Metrics.HardPresentationResets);
			}
			bool LocallyControlled = false;
			for (const auto &[Connection, Peer] : Peers)
				if (Peer.Control && Peer.Control->Character == Id) {
					LocallyControlled = true;
					break;
				}
			if (LocallyControlled) {
				if (!Replica.LocalCorrectionStart) continue;
				if (Replica.LastPresentationTick == 0) {
					Replica.LastPresentationTick = PresentationTick;
					continue;
				}
				if (PresentationTick > Replica.LastPresentationTick) {
					const auto Delta = PresentationTick - Replica.LastPresentationTick;
					const auto Remaining = Replica.LocalCorrectionDurationTicks -
										   std::min(
											   Replica.LocalCorrectionDurationTicks, Replica.LocalCorrectionElapsedTicks
										   );
					Replica.LocalCorrectionElapsedTicks += std::min(Remaining, Delta);
					Replica.LastPresentationTick = PresentationTick;
				}
				const auto Alpha = Replica.LocalCorrectionDurationTicks == 0
									   ? 1.0
									   : static_cast<double>(Replica.LocalCorrectionElapsedTicks) /
											 static_cast<double>(Replica.LocalCorrectionDurationTicks);
				if (Alpha >= 1.0) {
					CharacterValue->ApplyRuntimePresentationOffset(std::nullopt);
					Replica.LocalCorrectionStart.reset();
				} else {
					CharacterValue->ApplyRuntimePresentationOffset(Replica.LocalCorrectionStart->Lerp(Identity, Alpha));
				}
				continue;
			}

			if (Replica.PresentationSnapshotCount == 0) {
				CharacterValue->ApplyRuntimePresentationOffset(std::nullopt);
				continue;
			}
			const auto TargetTick = PresentationTick > Configuration.RemoteInterpolationDelayTicks
										? PresentationTick - Configuration.RemoteInterpolationDelayTicks
										: 0;
			const auto SnapshotAt = [&](std::size_t Index) -> const ReplicaState::PresentationSnapshot & {
				return Replica.PresentationSnapshots
					[(Replica.PresentationSnapshotStart + Index) % Replica.PresentationSnapshots.size()];
			};
			CFrame Presented = SnapshotAt(0).Transform;
			const auto &Newest = SnapshotAt(Replica.PresentationSnapshotCount - 1);
			if (TargetTick <= SnapshotAt(0).Tick) {
				Presented = SnapshotAt(0).Transform;
			} else if (TargetTick >= Newest.Tick) {
				Presented = Newest.Transform;
				const auto ExtrapolationTicks = TargetTick - Newest.Tick;
				if (ExtrapolationTicks != 0 && ExtrapolationTicks <= Configuration.RemoteExtrapolationLimitTicks) {
					Presented.Position += Newest.Velocity *
										  (static_cast<float>(ExtrapolationTicks) /
										   static_cast<float>(Configuration.SimulationTicksPerSecond));
					SaturatingIncrement(Metrics.RemoteExtrapolations);
				} else if (ExtrapolationTicks > Configuration.RemoteExtrapolationLimitTicks) {
					SaturatingIncrement(Metrics.RemoteExtrapolationHolds);
					SaturatingIncrement(Metrics.InterpolationBufferUnderruns);
				}
			} else {
				for (std::size_t Index = 1; Index < Replica.PresentationSnapshotCount; ++Index) {
					const auto &Right = SnapshotAt(Index);
					if (TargetTick > Right.Tick) continue;
					const auto &Left = SnapshotAt(Index - 1);
					const auto Span = Right.Tick - Left.Tick;
					const auto Alpha = Span == 0
										   ? 1.0
										   : static_cast<double>(TargetTick - Left.Tick) / static_cast<double>(Span);
					Presented = Left.Transform.Lerp(Right.Transform, Alpha);
					break;
				}
			}
			const auto Semantic = CharacterValue->GetCFrame();
			const auto ExpectedOffset = ExpectedTravelDistance(
				Newest.Velocity,
				Newest.Velocity,
				Configuration.RemoteInterpolationDelayTicks + Configuration.RemoteExtrapolationLimitTicks,
				Configuration.SimulationTicksPerSecond
			);
			if (glm::distance(Presented.Position, Semantic.Position) >=
				MaximumHardCorrectionDistance + ExpectedOffset) {
				CharacterValue->ApplyRuntimePresentationOffset(std::nullopt);
				Replica.PresentationSnapshotStart = 0;
				Replica.PresentationSnapshotCount = 0;
				SaturatingIncrement(Metrics.InterpolationResets);
				SaturatingIncrement(Metrics.HardPresentationResets);
				continue;
			}
			const auto Offset = Semantic.ToObjectSpace(Presented);
			CharacterValue->ApplyRuntimePresentationOffset(
				Offset.FuzzyEq(Identity) ? std::nullopt : std::optional<CFrame>(Offset)
			);
			Replica.LastPresentationTick = PresentationTick;
		}
		SaturatingIncrement(
			Metrics.InterpolationCpuNanoseconds,
			static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Started).count()
			)
		);
	}

	void PredictedCharacterNetwork::HardReset(PeerState &Peer) {
		Peer.HistoryStart = 0;
		Peer.HistoryCount = 0;
		Peer.PredictedAction.reset();
		Peer.LastActionEvaluationTick = 0;
		Peer.PredictionSuspended = false;
		SaturatingIncrement(Metrics.HardResets);
	}

	std::optional<CharacterControlTransition> PredictedCharacterNetwork::GetControl(ConnectionId Connection) const {
		auto Found = Peers.find(Connection);
		return Found == Peers.end() ? std::nullopt : Found->second.Control;
	}

	std::optional<CharacterActionState> PredictedCharacterNetwork::GetAuthoritativeAction(ObjectId Character) const {
		auto Found = Replicas.find(Character);
		return Found == Replicas.end() ? std::nullopt : Found->second.ActiveAction;
	}

	std::optional<CharacterActionPresentation>
	PredictedCharacterNetwork::GetPresentationAction(ObjectId Character) const {
		for (const auto &[Connection, Peer] : Peers) {
			(void)Connection;
			if (Peer.Control && Peer.Control->Character == Character && Peer.PredictedAction)
				return CharacterActionPresentation{*Peer.PredictedAction, Peer.LastActionEvaluationTick, true};
		}
		auto Found = Replicas.find(Character);
		if (Found == Replicas.end() || !Found->second.ActiveAction) return std::nullopt;
		return CharacterActionPresentation{
			*Found->second.ActiveAction,
			std::max(Found->second.LastAuthoritativeTick, Found->second.ActiveAction->StartTick),
			false,
		};
	}

	std::size_t PredictedCharacterNetwork::GetPredictionHistorySize(ConnectionId Connection) const {
		auto Found = Peers.find(Connection);
		return Found == Peers.end() ? 0 : Found->second.HistoryCount;
	}

	std::size_t PredictedCharacterNetwork::GetPresentationSnapshotCount(ObjectId Character) const {
		auto Found = Replicas.find(Character);
		return Found == Replicas.end() ? 0 : Found->second.PresentationSnapshotCount;
	}

	std::vector<CharacterActionResolution> PredictedCharacterNetwork::DrainActionResolutions() {
		auto Result = std::move(ActionResolutions);
		ActionResolutions.clear();
		return Result;
	}

	std::vector<CharacterActionEnded> PredictedCharacterNetwork::DrainActionEndings() {
		auto Result = std::move(ActionEndings);
		ActionEndings.clear();
		return Result;
	}
}
