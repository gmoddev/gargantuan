#include "gargantuan/network/CharacterNetwork.hpp"

#include "gargantuan/classes/KinematicCharacter.hpp"
#include "gargantuan/classes/WorldRoot.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <set>
#include <utility>

namespace gargantuan::network {
	namespace {
		template <typename Value> void SaturatingIncrement(Value &Counter, Value Amount = 1) {
			Counter = Amount > std::numeric_limits<Value>::max() - Counter ? std::numeric_limits<Value>::max()
																		   : Counter + Amount;
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
	}

	bool CharacterActionDefinition::IsValid() const {
		return Token != 0 && Animation.IsValid() && ContentRevision.IsValid() && DurationTicks != 0 &&
			   DurationTicks <= 60 * 60 * 10 && static_cast<bool>(EvaluateRootMotion);
	}

	struct AuthoritativeCharacterNetwork::PeerState {
		std::map<ObjectId, StateChannelId> Materialized;
	};

	struct AuthoritativeCharacterNetwork::CharacterState {
		std::weak_ptr<KinematicCharacter> Character;
		CharacterControlEpoch ControlEpoch;
		std::optional<ConnectionId> Controller;
		CharacterInputSequence LastReceivedInput;
		CharacterInputSequence AcknowledgedInput;
		std::optional<CharacterInputCommand> PendingInput;
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
	};

	AuthoritativeCharacterNetwork::~AuthoritativeCharacterNetwork() = default;

	AuthoritativeCharacterNetwork::AuthoritativeCharacterNetwork(
		INetworkScheduler &SchedulerValue,
		NetworkLimits LimitsValue,
		CharacterMovementPolicy MovementPolicyValue,
		CharacterActionPolicy ActionPolicyValue
	)
		: Scheduler(SchedulerValue), Limits(LimitsValue), MovementPolicy(std::move(MovementPolicyValue)),
		  ActionPolicy(std::move(ActionPolicyValue)) {
		if (!Limits.IsValid() || !MovementPolicy)
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
		(void)RevokeControl(Character, AuthoritativeTick);
		for (auto &[Connection, Peer] : Peers)
			Peer.Materialized.erase(Character);
		Characters.erase(Found);
		return true;
	}

	bool AuthoritativeCharacterNetwork::MarkMaterialized(
		ConnectionId Connection, ObjectId Character, StateChannelId Channel
	) {
		auto Peer = Peers.find(Connection);
		return Peer != Peers.end() && Characters.contains(Character) && Channel.IsValid() &&
			   Peer->second.Materialized.emplace(Character, Channel).second;
	}

	bool AuthoritativeCharacterNetwork::MarkUnmaterialized(ConnectionId Connection, ObjectId Character) {
		auto Peer = Peers.find(Connection);
		if (Peer == Peers.end() || !Peer->second.Materialized.erase(Character)) return false;
		auto Found = Characters.find(Character);
		if (Found != Characters.end() && Found->second.Controller == Connection)
			(void)RevokeControl(Character, std::max<std::uint64_t>(LastAuthoritativeTick, 1));
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
		State.LastReceivedAction = {};
		State.ResolvedAction = {};
		State.PendingActionCount = 0;
		State.ActiveAction.reset();
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
		Found->second.PendingActionCount = 0;
		Found->second.ActiveAction.reset();
		return true;
	}

	bool AuthoritativeCharacterNetwork::RegisterAction(CharacterActionDefinition Definition) {
		return Definition.IsValid() && Actions.size() < MaximumNetworkCharacters &&
			   Actions.emplace(Definition.Token, std::move(Definition)).second;
	}

	bool AuthoritativeCharacterNetwork::StartServerAction(
		ObjectId Character, std::uint32_t ActionToken, std::uint64_t AuthoritativeTick
	) {
		auto Found = Characters.find(Character);
		auto Definition = Actions.find(ActionToken);
		if (Found == Characters.end() || Definition == Actions.end() || AuthoritativeTick == 0 ||
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
		} else if (std::holds_alternative<CharacterActionRequest>(Message)) {
			Traffic = TrafficClass::ReliableApplication;
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
		if (!Result.Accepted()) return false;
		SaturatingIncrement(Metrics.BytesOut, static_cast<std::uint64_t>(Bytes));
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
				if (!Selected || Definition == Actions.end()) {
					State.ActiveAction.reset();
					State.ReliableStateRequired = true;
					SaturatingIncrement(Metrics.ActionRequestsRejected);
					continue;
				}
				State.ActiveAction = CharacterActionState{
					Request.ActionSequence,
					Definition->second.Token,
					Definition->second.Animation,
					Definition->second.ContentRevision,
					AuthoritativeTick,
					Definition->second.DurationTicks,
				};
				State.LastActionEvaluationTick = AuthoritativeTick;
				State.ReliableStateRequired = true;
				SaturatingIncrement(Metrics.ActionRequestsAccepted);
			}
			State.PendingActionCount = 0;

			if (State.PendingInput) {
				const auto Command = *State.PendingInput;
				State.PendingInput.reset();
				if (Command.SimulationTick <= AuthoritativeTick + MaximumCharacterCommandTickLead) {
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
								SaturatingIncrement(Metrics.CommandsAccepted);
							}
						}
					} catch (...) {
						SaturatingIncrement(Metrics.ProtocolRejects);
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
				if (Definition == Actions.end() ||
					AuthoritativeTick >= State.ActiveAction->StartTick + State.ActiveAction->DurationTicks) {
					State.ActiveAction.reset();
				} else if (AuthoritativeTick > State.LastActionEvaluationTick) {
					const auto RootStarted = std::chrono::steady_clock::now();
					try {
						auto Delta = Definition->second.EvaluateRootMotion(
							State.LastActionEvaluationTick, AuthoritativeTick
						);
						State.LastActionEvaluationTick = AuthoritativeTick;
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
			}
			const bool Reliable = State.ReliableStateRequired;
			State.ReliableStateRequired = false;
			(void)PublishState(Id, AuthoritativeTick, false, Reliable);
			++Iterator;
		}
	}

	bool AuthoritativeCharacterNetwork::PublishState(
		ObjectId Character, std::uint64_t AuthoritativeTick, bool Teleport, bool Reliable
	) {
		auto Found = Characters.find(Character);
		if (Found == Characters.end() || AuthoritativeTick == 0 || !Found->second.NextStateSequence.IsValid())
			return false;
		auto CharacterValue = Found->second.Character.lock();
		if (!CharacterValue || CharacterValue->GetDestroyed() || CharacterValue->IsDestroying()) return false;
		auto &Runtime = Found->second;
		CharacterAuthoritativeState State{
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
		bool Sent = false;
		for (const auto &[Connection, Peer] : Peers) {
			auto Visible = Peer.Materialized.find(Character);
			if (Visible == Peer.Materialized.end()) continue;
			if (Queue(Connection, CharacterMessage(State), Visible->second, Reliable)) {
				Sent = true;
				SaturatingIncrement(Metrics.AuthoritativeStatesSent);
			}
		}
		Runtime.NextStateSequence = Runtime.NextStateSequence.TryNext().value_or(RealtimeStateSequence{});
		return Sent;
	}

	struct PredictedCharacterNetwork::ReplicaState {
		std::weak_ptr<KinematicCharacter> Character;
		CharacterControlEpoch Epoch;
		RealtimeStateSequence LastState;
		CharacterActionSequence LastResolvedAction;
		std::optional<CharacterAuthoritativeState> PendingState;
		std::optional<CharacterActionState> ActiveAction;
	};

	struct PredictedCharacterNetwork::PeerState {
		ConnectionId Connection;
		std::optional<CharacterControlTransition> Control;
		std::optional<CharacterControlTransition> PendingControl;
		CharacterControlEpoch LastControlEpoch;
		CharacterInputSequence NextInput{1};
		CharacterActionSequence NextAction{1};
		std::array<CharacterInputCommand, MaximumCharacterPredictionHistory> History{};
		std::size_t HistoryStart = 0;
		std::size_t HistoryCount = 0;
		std::optional<CharacterActionState> PredictedAction;
		std::uint64_t LastActionEvaluationTick = 0;
		bool PredictionSuspended = false;
	};

	PredictedCharacterNetwork::~PredictedCharacterNetwork() = default;

	PredictedCharacterNetwork::PredictedCharacterNetwork(
		INetworkScheduler &SchedulerValue, NetworkLimits LimitsValue, CharacterMovementPolicy MovementPolicyValue
	)
		: Scheduler(SchedulerValue), Limits(LimitsValue), MovementPolicy(std::move(MovementPolicyValue)) {
		if (!Limits.IsValid() || !MovementPolicy)
			throw std::invalid_argument("[Character:Network] predicted manager configuration is invalid");
	}

	bool PredictedCharacterNetwork::AddPeer(ConnectionId Connection) {
		return Connection.IsValid() && Peers.size() < MaximumCharacterNetworkPeers &&
			   Peers.emplace(Connection, PeerState{.Connection = Connection}).second;
	}

	bool PredictedCharacterNetwork::RemovePeer(ConnectionId Connection) {
		return Peers.erase(Connection) != 0;
	}

	bool PredictedCharacterNetwork::MarkMaterialized(
		ObjectId Character, const std::shared_ptr<KinematicCharacter> &Replica
	) {
		if (!Character.IsValid() || !Replica || Replica->GetDestroyed() || Replica->IsDestroying() ||
			Replicas.size() >= MaximumNetworkCharacters || Replicas.contains(Character))
			return false;
		Replicas.emplace(Character, ReplicaState{.Character = Replica});
		for (auto &[Connection, Peer] : Peers)
			if (Peer.PendingControl && Peer.PendingControl->Character == Character) {
				Peer.Control = Peer.PendingControl;
				Peer.PendingControl.reset();
				HardReset(Peer);
			}
		return true;
	}

	bool PredictedCharacterNetwork::MarkUnmaterialized(ObjectId Character) {
		if (!Replicas.erase(Character)) return false;
		for (auto &[Connection, Peer] : Peers)
			if (Peer.Control && Peer.Control->Character == Character) {
				Peer.Control.reset();
				HardReset(Peer);
			}
		return true;
	}

	bool PredictedCharacterNetwork::RegisterAction(CharacterActionDefinition Definition) {
		return Definition.IsValid() && Actions.size() < MaximumNetworkCharacters &&
			   Actions.emplace(Definition.Token, std::move(Definition)).second;
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
		if (!Result.Accepted()) return false;
		SaturatingIncrement(Metrics.BytesOut, static_cast<std::uint64_t>(Bytes));
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
				if (!MatchesControl && !MatchesPending)
					return false;
				Peer.Control.reset();
				Peer.PendingControl.reset();
				HardReset(Peer);
				return true;
			}
			if (Peer.LastControlEpoch.IsValid() &&
				!Transition->ControlEpoch.IsNewerThan(Peer.LastControlEpoch))
				return false;
			Peer.LastControlEpoch = Transition->ControlEpoch;
			if (Replicas.contains(Transition->Character)) {
				Peer.Control = *Transition;
				Peer.PendingControl.reset();
				HardReset(Peer);
			} else {
				Peer.PendingControl = *Transition;
			}
			return true;
		}
		if (auto *State = std::get_if<CharacterAuthoritativeState>(&Message)) {
			auto Replica = Replicas.find(State->Character);
			if (Replica == Replicas.end()) return false;
			StateChannelId Channel;
			if (Peer.Control && Peer.Control->Character == State->Character) Channel = Peer.Control->Channel;
			if (!Channel.IsValid()) {
				const auto *Order = std::get_if<RealtimeStateOrder>(&Event.Order);
				if (Order) Channel = Order->Channel;
			}
			const bool ReliableState = Event.Delivery == DeliveryMode::ReliableOrdered &&
									   Event.Traffic == TrafficClass::Control &&
									   std::holds_alternative<std::monostate>(Event.Order);
			if ((!ReliableState && !Channel.IsValid()) || !IsExpectedStateOrder(Event, Channel, State->StateSequence))
				return false;
			if (Replica->second.LastState.IsValid() && !State->StateSequence.IsNewerThan(Replica->second.LastState)) {
				if (ReliableState && State->ActiveAction &&
					State->ResolvedAction == Replica->second.LastResolvedAction) {
					auto Definition = Actions.find(State->ActiveAction->ActionToken);
					if (Definition != Actions.end() && Definition->second.Animation == State->ActiveAction->Animation &&
						Definition->second.ContentRevision == State->ActiveAction->ContentRevision) {
						Replica->second.ActiveAction = State->ActiveAction;
						SaturatingIncrement(Metrics.StaleStatesDropped);
						return true;
					}
				}
				SaturatingIncrement(Metrics.StaleStatesDropped);
				return false;
			}
			if (Replica->second.PendingState &&
				!State->StateSequence.IsNewerThan(Replica->second.PendingState->StateSequence)) {
				if (ReliableState && State->ActiveAction &&
					State->ResolvedAction == Replica->second.PendingState->ResolvedAction) {
					Replica->second.PendingState->ActiveAction = State->ActiveAction;
					SaturatingIncrement(Metrics.StaleStatesDropped);
					return true;
				}
				SaturatingIncrement(Metrics.StaleStatesDropped);
				return false;
			}
			Replica->second.PendingState = *State;
			return true;
		}
		SaturatingIncrement(Metrics.ProtocolRejects);
		return false;
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
				if (Definition != Actions.end() &&
					Command.SimulationTick < Peer.PredictedAction->StartTick + Peer.PredictedAction->DurationTicks) {
					auto Delta = Definition->second.EvaluateRootMotion(
						Peer.LastActionEvaluationTick, Command.SimulationTick
					);
					Peer.LastActionEvaluationTick = Command.SimulationTick;
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
			Found->second.PredictionSuspended)
			return false;
		auto &Peer = Found->second;
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
		if (!Command.IsValid() || !Predict(Peer, World, Command, true)) return false;
		if (!Queue(Connection, CharacterMessage(Command), Peer.Control->Channel, false)) return false;
		Peer.NextInput = Peer.NextInput.TryNext().value_or(CharacterInputSequence{});
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
		if (Definition == Actions.end() || SimulationTick == 0) return false;
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
		Peer.PredictedAction = CharacterActionState{
			Request.ActionSequence,
			Definition->second.Token,
			Definition->second.Animation,
			Definition->second.ContentRevision,
			SimulationTick,
			Definition->second.DurationTicks,
		};
		Peer.LastActionEvaluationTick = SimulationTick;
		Peer.NextAction = Peer.NextAction.TryNext().value_or(CharacterActionSequence{});
		return true;
	}

	void PredictedCharacterNetwork::Reconcile(WorldRoot &World) {
		const auto ReconcileStarted = std::chrono::steady_clock::now();
		for (auto &[Id, Replica] : Replicas) {
			if (!Replica.PendingState) continue;
			const auto State = *Replica.PendingState;
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
				Replica.ActiveAction.reset();
				if (State.ActiveAction) {
					auto Definition = Actions.find(State.ActiveAction->ActionToken);
					if (Definition != Actions.end() && Definition->second.Animation == State.ActiveAction->Animation &&
						Definition->second.ContentRevision == State.ActiveAction->ContentRevision)
						Replica.ActiveAction = State.ActiveAction;
					else
						SaturatingIncrement(Metrics.ProtocolRejects);
				}
				Replica.Epoch = State.ControlEpoch;
				Replica.LastState = State.StateSequence;
				Replica.LastResolvedAction = State.ResolvedAction;
				continue;
			}
			auto &Peer = *LocalPeer;
			if (State.ControlEpoch != Peer.Control->ControlEpoch) {
				SaturatingIncrement(Metrics.StaleStatesDropped);
				continue;
			}
			const auto Correction = glm::length(CharacterValue->GetPosition() - State.Transform.Position);
			CharacterValue->ApplyRuntimeTransform(State.Transform);
			CharacterValue->ApplyRuntimeControllerFacts(State.Velocity, State.FloorNormal, State.Grounded());
			if (Correction > 1.0e-4f) SaturatingIncrement(Metrics.PredictionCorrections);
			bool Reset = State.Teleport() || Correction > MaximumHardCorrectionDistance;
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
			} else if (State.ActiveAction) {
				auto Definition = Actions.find(State.ActiveAction->ActionToken);
				if (Definition == Actions.end() || Definition->second.Animation != State.ActiveAction->Animation ||
					Definition->second.ContentRevision != State.ActiveAction->ContentRevision) {
					Reset = true;
				} else {
					Peer.PredictedAction = State.ActiveAction;
					Peer.LastActionEvaluationTick = State.AuthoritativeTick;
				}
			}
			if (Reset) {
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
			if (Reset)
				Replica.ActiveAction.reset();
			else
				Replica.ActiveAction = State.ActiveAction;
			Peer.PredictionSuspended = false;
			Replica.Epoch = State.ControlEpoch;
			Replica.LastState = State.StateSequence;
			Replica.LastResolvedAction = State.ResolvedAction;
		}
		SaturatingIncrement(
			Metrics.ReconciliationCpuNanoseconds,
			static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
										   std::chrono::steady_clock::now() - ReconcileStarted
			)
										   .count())
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

	std::size_t PredictedCharacterNetwork::GetPredictionHistorySize(ConnectionId Connection) const {
		auto Found = Peers.find(Connection);
		return Found == Peers.end() ? 0 : Found->second.HistoryCount;
	}
}
