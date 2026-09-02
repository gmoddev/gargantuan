#include "gargantuan/network/GameSession.hpp"

#include "gargantuan/Engine.hpp"
#include "gargantuan/Log.hpp"
#include "gargantuan/animation/AnimationTrack.hpp"
#include "gargantuan/assets/AssetTypes.hpp"
#include "gargantuan/classes/Animator.hpp"
#include "gargantuan/classes/Character.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/KinematicCharacter.hpp"
#include "gargantuan/classes/Player.hpp"
#include "gargantuan/classes/RemoteBase.hpp"
#include "gargantuan/classes/WorldRoot.hpp"
#include "gargantuan/network/CharacterNetwork.hpp"
#include "gargantuan/network/GameSessionProtocol.hpp"
#include "gargantuan/network/RemoteManager.hpp"
#include "gargantuan/network/ReplicaApplier.hpp"
#include "gargantuan/network/ReplicationCoordinator.hpp"
#include "gargantuan/network/ReplicationTransport.hpp"
#include "gargantuan/network/Scheduler.hpp"
#include "gargantuan/services/CharacterControlService.hpp"
#include "gargantuan/services/Players.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>
#include <variant>

namespace gargantuan::network {
	namespace {
		constexpr std::uint32_t CharacterMagic = 0x52484347;
		constexpr std::uint32_t RemoteMagic = 0x544d5247;

		enum class PeerPhase : std::uint8_t { TransportConnected, Accepted, Ready };

		std::uint32_t FrameMagic(std::span<const std::byte> Bytes) {
			if (Bytes.size() < 4) return 0;
			return std::to_integer<std::uint32_t>(Bytes[0]) | (std::to_integer<std::uint32_t>(Bytes[1]) << 8) |
				   (std::to_integer<std::uint32_t>(Bytes[2]) << 16) | (std::to_integer<std::uint32_t>(Bytes[3]) << 24);
		}

		StateChannelId CharacterStateChannel(ObjectId Object) {
			return StateChannelId((static_cast<std::uint64_t>(Object.Generation) << 32) | Object.Slot);
		}

		ObjectId ReplicationObject(const ReplicationIntent &Intent) {
			return std::visit([](const auto &Value) { return Value.Object; }, Intent);
		}

		bool SamePresentedAction(const CharacterActionState &Left, const CharacterActionState &Right) {
			return Left.ActionSequence == Right.ActionSequence && Left.ActionToken == Right.ActionToken &&
				   Left.Animation == Right.Animation && Left.ContentRevision == Right.ContentRevision &&
				   Left.StartTick == Right.StartTick && Left.DurationTicks == Right.DurationTicks;
		}
	}

	bool GameSessionConfiguration::IsValid() const {
		return Endpoint.IsValid() && Limits.IsValid() && HandshakeTimeoutTicks > 0 &&
			   HandshakeTimeoutTicks <= DefaultGameSessionHandshakeTimeoutTicks * 10;
	}

	NetworkLimits GameSessionConfiguration::DefaultLimits() {
		return {
			.MaximumReliableMessageBytes = 512 * 1024,
			.MaximumUnreliableMessageBytes = 1168,
			.MaximumQueuedReliableBytes = 16 * 1024 * 1024,
			.MaximumInFlightRemoteRequests = 1024,
			.MaximumDecodedMessageBytes = 512 * 1024,
			.MaximumSendBytesPerTick = 2 * 1024 * 1024,
			.MaximumReceiveBytesPerTick = 2 * 1024 * 1024,
			.MaximumMessagesPerTick = 1024,
		};
	}

	struct GameSession::Implementation {
		struct PresentedAction {
			CharacterActionState Action;
			std::shared_ptr<AnimationTrack> Track;
		};

		struct Peer {
			PeerPhase Phase = PeerPhase::TransportConnected;
			std::uint64_t ConnectedTick = 0;
			std::uint64_t Nonce = 0;
			std::uint64_t SessionEpoch = 0;
			ReplicationEpoch Replication;
			NetworkLimits Limits;
			ObjectId PlayerObject;
			std::uint32_t PlayerId = 0;
			std::shared_ptr<Player> PlayerValue;
			ObjectId ControlledCharacter;
		};

		std::shared_ptr<IGameTransport> Transport;
		GameSessionConfiguration Configuration;
		NetworkScheduler Scheduler;
		Engine *Runtime = nullptr;
		GameSessionStatus Status = GameSessionStatus::Stopped;
		std::string Failure;
		GameSessionMetrics Metrics;
		std::uint64_t CurrentTick = 0;
		std::uint64_t NextSessionEpoch = 1;
		std::uint64_t NextReplicationEpoch = 1;
		std::map<ConnectionId, Peer> Peers;
		std::optional<ConnectionId> PrimaryConnection;

		std::unique_ptr<ReplicationCoordinator> Replication;
		ReplicaApplier Replica;
		std::unique_ptr<RemoteManager> Remotes;
		std::unique_ptr<AuthoritativeCharacterNetwork> Authority;
		std::unique_ptr<PredictedCharacterNetwork> Prediction;
		std::set<ObjectId> ServerCharacters;
		std::set<ObjectId> ServerRemoteObjects;
		std::set<ObjectId> ClientCharacterObjects;
		std::set<ObjectId> ClientRemoteObjects;
		std::map<ObjectId, PresentedAction> PresentedActions;
		bool BaselineApplied = false;
		bool ClientRuntimeAttached = false;

		Implementation(
			std::shared_ptr<IGameTransport> TransportValue,
			GameSessionConfiguration ConfigurationValue,
			Engine *ServerRuntime
		)
			: Transport(std::move(TransportValue)), Configuration(std::move(ConfigurationValue)), Scheduler(*Transport),
			  Runtime(ServerRuntime) {
			if (!Transport || !Configuration.IsValid())
				throw std::invalid_argument("[Network:Session] Game session configuration is invalid");
			if (Configuration.Role == GameSessionRole::Server && !Runtime)
				throw std::invalid_argument("[Network:Session] Server game session requires an Engine runtime");
			if (Configuration.Role == GameSessionRole::Client && Runtime)
				throw std::invalid_argument(
					"[Network:Session] Client Engine attaches after trusted replication bootstrap"
				);
			if (Configuration.Role == GameSessionRole::Server) InitializeServerManagers();
		}

		~Implementation() {
			Stop();
		}

		void InitializeServerManagers() {
			Replication = std::make_unique<ReplicationCoordinator>(Runtime->DataModel, [this](ObjectId Object) {
				return !Runtime->Players->IsRuntimeModule(Object);
			});
			Remotes = std::make_unique<RemoteManager>(
				RemoteManagerRole::Server,
				Scheduler,
				[this](ConnectionId Connection, ObjectId Object) {
					const auto *View = Replication->GetView(Connection);
					return View && View->Knows(Object);
				},
				[](ObjectId Object) { return ObjectRegistry::Get().Lookup(Object); }
			);
			Authority = std::make_unique<AuthoritativeCharacterNetwork>(
				Scheduler,
				Configuration.Limits,
				[this](const CharacterInputCommand &Command, const KinematicCharacter &CharacterValue) {
					return Runtime->CharacterControl->EvaluateMovement(Command, CharacterValue);
				},
				[this](ConnectionId Connection, const CharacterActionRequest &Request) -> std::optional<std::uint32_t> {
					auto PeerIterator = Peers.find(Connection);
					if (PeerIterator == Peers.end() || PeerIterator->second.Phase != PeerPhase::Ready ||
						!PeerIterator->second.PlayerValue)
						return std::nullopt;
					auto CharacterValue = std::dynamic_pointer_cast<KinematicCharacter>(
						ObjectRegistry::Get().Lookup(Request.Character)
					);
					return CharacterValue &&
								   Runtime->CharacterControl->EvaluateAction(
									   PeerIterator->second.PlayerValue, CharacterValue, Request.RequestedActionToken
								   )
							   ? std::optional<std::uint32_t>(Request.RequestedActionToken)
							   : std::nullopt;
				}
			);
			Runtime->CharacterControl->AttachActionRegistration([this](
																	const CharacterActionDefinition &Definition, bool
																) { return Authority->RegisterAction(Definition); });
		}

		void InitializeClientManagers() {
			const auto Connection = *PrimaryConnection;
			auto &PeerValue = Peers.at(Connection);
			Remotes = std::make_unique<RemoteManager>(
				RemoteManagerRole::Client,
				Scheduler,
				[this](ConnectionId, ObjectId Object) { return Replica.Resolve(Object) != nullptr; },
				[this](ObjectId Object) { return Replica.Resolve(Object); }
			);
			Prediction = std::make_unique<PredictedCharacterNetwork>(
				Scheduler,
				PeerValue.Limits,
				[this](const CharacterInputCommand &Command, const KinematicCharacter &CharacterValue) {
					return Runtime->CharacterControl->EvaluateMovement(Command, CharacterValue);
				}
			);
			if (!Remotes->AddPeer(Connection, PeerValue.Replication, PeerValue.Limits) ||
				!Prediction->AddPeer(Connection))
				throw std::runtime_error("[Network:Session] Failed to attach client gameplay managers");
			Runtime->CharacterControl->AttachClientBridge(
				[this, Connection](std::uint64_t Tick, float DeltaSeconds, glm::vec2 Move, float Facing, bool Jump) {
					return Prediction->SubmitInput(
						Connection, *Runtime->WorldRoot, Tick, DeltaSeconds, Move, Facing, Jump
					);
				},
				[this, Connection](std::uint32_t Action, std::uint64_t Tick) {
					return Prediction->RequestAction(Connection, Action, Tick);
				}
			);
			Runtime->CharacterControl->AttachActionRegistration(
				[this](const CharacterActionDefinition &Definition, bool Predictable) {
					return !Predictable || Prediction->RegisterAction(Definition);
				}
			);
			Runtime->CharacterControl->AttachPredictionMode([this](bool Enabled) {
				Prediction->SetPredictionEnabled(Enabled);
			});
			SynchronizeClientGraph();
		}

		TransportOperationResult Start() {
			if (Status != GameSessionStatus::Stopped) return {.Status = TransportOperationStatus::InvalidState};
			auto Result = Transport->Start({
				.Role = Configuration.Role == GameSessionRole::Server ? TransportRole::Server : TransportRole::Client,
				.Endpoint = Configuration.Endpoint,
				.AdvertisedLimits = Configuration.Limits,
			});
			if (Result.Succeeded())
				Status = Configuration.Role == GameSessionRole::Server ? GameSessionStatus::Listening
																	   : GameSessionStatus::Connecting;
			else {
				Status = GameSessionStatus::Failed;
				Failure = "Transport endpoint failed to start";
			}
			return Result;
		}

		void Stop() {
			if (Status == GameSessionStatus::Stopped) return;
			for (auto &[Character, Presented] : PresentedActions) {
				(void)Character;
				if (Presented.Track) Presented.Track->Stop();
			}
			PresentedActions.clear();
			if (Runtime) Runtime->CharacterControl->DetachRuntime();
			for (auto Iterator = Peers.begin(); Iterator != Peers.end();) {
				auto Connection = Iterator->first;
				++Iterator;
				TearDownPeer(Connection);
			}
			(void)Transport->Stop({DisconnectReason::LocalShutdown, "Game session stopped"});
			Status = GameSessionStatus::Stopped;
		}

		bool QueueSession(ConnectionId Connection, const GameSessionMessage &Message, const NetworkLimits &Limits) {
			auto Encoded = EncodeGameSessionMessage(Message);
			if (!Encoded) return false;
			auto Intent = MakeNetworkMessageIntent(
				Connection, DeliveryMode::ReliableOrdered, TrafficClass::Control, {}, std::move(*Encoded), Limits
			);
			return Intent && Scheduler.Submit(std::move(*Intent)).Accepted();
		}

		void Reject(ConnectionId Connection, DisconnectReason Reason, std::string Diagnostic) {
			if (Reason == DisconnectReason::ProtocolViolation) ++Metrics.ProtocolRejects;
			LOG_WARN(
				App,
				"[Network:Session] Rejecting peer %u:%u: %s",
				Connection.Slot,
				Connection.Generation,
				Diagnostic.c_str()
			);
			if (Configuration.Role == GameSessionRole::Client) Failure = Diagnostic;
			(void)Transport->Disconnect(Connection, {Reason, Diagnostic});
			TearDownPeer(Connection);
		}

		void OnConnected(ConnectionId Connection) {
			if (!Connection.IsValid() || Peers.size() >= MaximumGameSessionPeers) {
				Reject(Connection, DisconnectReason::ResourceExhaustion, "Game session peer capacity reached");
				return;
			}
			Peer Value;
			Value.ConnectedTick = CurrentTick;
			if (!Peers.emplace(Connection, Value).second) return;
			++Metrics.TransportConnections;
			if (Configuration.Role == GameSessionRole::Server) return;
			if (PrimaryConnection) {
				Reject(
					Connection, DisconnectReason::ProtocolViolation, "Client game session accepts one server connection"
				);
				return;
			}
			PrimaryConnection = Connection;
			if (!Scheduler.RegisterConnection(Connection, Configuration.Limits)) {
				Reject(Connection, DisconnectReason::ResourceExhaustion, "Client scheduler registration failed");
				return;
			}
			auto &PeerValue = Peers.at(Connection);
			PeerValue.Nonce = Configuration.ClientNonce != 0
								  ? Configuration.ClientNonce
								  : (static_cast<std::uint64_t>(Connection.Generation) << 32) | Connection.Slot;
			if (!QueueSession(
					Connection, GameSessionClientHello{PeerValue.Nonce, Configuration.Limits}, Configuration.Limits
				))
				Reject(Connection, DisconnectReason::ResourceExhaustion, "Client hello could not be queued");
		}

		void AcceptServerPeer(ConnectionId Connection, const GameSessionClientHello &Hello) {
			auto Iterator = Peers.find(Connection);
			if (Iterator == Peers.end() || Iterator->second.Phase != PeerPhase::TransportConnected) {
				++Metrics.RejectedHandshakes;
				Reject(Connection, DisconnectReason::ProtocolViolation, "Duplicate or out-of-order client hello");
				return;
			}
			auto Limits = NegotiateNetworkLimits(Configuration.Limits, Hello.AdvertisedLimits);
			if (!Limits || !Scheduler.RegisterConnection(Connection, *Limits) || NextSessionEpoch == 0 ||
				NextReplicationEpoch == 0) {
				++Metrics.RejectedHandshakes;
				Reject(Connection, DisconnectReason::ResourceExhaustion, "Game session negotiation failed");
				return;
			}
			auto &PeerValue = Iterator->second;
			PeerValue.Nonce = Hello.Nonce;
			PeerValue.SessionEpoch = NextSessionEpoch++;
			PeerValue.Replication = ReplicationEpoch(NextReplicationEpoch++);
			PeerValue.Limits = *Limits;
			try {
				PeerValue.PlayerValue = Runtime->Players->CreateSessionPlayer({
					"session-development",
					"peer-" + std::to_string(PeerValue.SessionEpoch),
				});
			} catch (const std::exception &) {
				++Metrics.RejectedHandshakes;
				Reject(Connection, DisconnectReason::ResourceExhaustion, "Server Player creation failed");
				return;
			}
			PeerValue.PlayerObject = PeerValue.PlayerValue->GetObjectId();
			PeerValue.PlayerId = static_cast<std::uint32_t>(PeerValue.PlayerValue->GetPlayerId());
			++Metrics.PlayersCreated;
			SynchronizeServerGraph();
			auto Baseline = Replication->AddPeer(Connection, PeerValue.Replication);
			auto BaselineQueued = Baseline.Succeeded()
									  ? QueueReplicationFrame(*Baseline.Frame, Connection, PeerValue.Limits, Scheduler)
									  : SerializationResult<SchedulerSubmitResult>(SerializationFailure(
											SerializationErrorCode::InternalFailure,
											"Replication baseline was not produced"
										));
			if (!Baseline.Succeeded() ||
				!QueueSession(
					Connection,
					GameSessionServerAccepted{
						.Nonce = PeerValue.Nonce,
						.SessionEpoch = PeerValue.SessionEpoch,
						.Replication = PeerValue.Replication,
						.Player = PeerValue.PlayerObject,
						.PlayerId = PeerValue.PlayerId,
						.Identity = SessionIdentityKind::DevelopmentLocal,
						.NegotiatedLimits = PeerValue.Limits,
					},
					PeerValue.Limits
				) ||
				!BaselineQueued || !BaselineQueued->Accepted()) {
				Reject(Connection, DisconnectReason::ResourceExhaustion, "Server bootstrap could not be queued");
				return;
			}
			PeerValue.Phase = PeerPhase::Accepted;
			++Metrics.AcceptedPeers;
		}

		void AcceptClientSession(ConnectionId Connection, const GameSessionServerAccepted &Accepted) {
			auto Iterator = Peers.find(Connection);
			if (Iterator == Peers.end() || Iterator->second.Phase != PeerPhase::TransportConnected ||
				Accepted.Nonce != Iterator->second.Nonce) {
				++Metrics.RejectedHandshakes;
				Reject(
					Connection, DisconnectReason::ProtocolViolation, "Server acceptance does not match the client hello"
				);
				return;
			}
			auto Expected = NegotiateNetworkLimits(Configuration.Limits, Accepted.NegotiatedLimits);
			if (!Expected || *Expected != Accepted.NegotiatedLimits) {
				++Metrics.RejectedHandshakes;
				Reject(Connection, DisconnectReason::ProtocolViolation, "Server returned invalid negotiated limits");
				return;
			}
			(void)Scheduler.CancelConnection(Connection);
			if (!Scheduler.RegisterConnection(Connection, Accepted.NegotiatedLimits)) {
				Reject(
					Connection, DisconnectReason::ResourceExhaustion, "Client negotiated scheduler registration failed"
				);
				return;
			}
			auto &PeerValue = Iterator->second;
			PeerValue.Phase = PeerPhase::Accepted;
			PeerValue.SessionEpoch = Accepted.SessionEpoch;
			PeerValue.Replication = Accepted.Replication;
			PeerValue.Limits = Accepted.NegotiatedLimits;
			PeerValue.PlayerObject = Accepted.Player;
			PeerValue.PlayerId = Accepted.PlayerId;
			++Metrics.AcceptedPeers;
			TryFinalizeClientBootstrap();
		}

		void ApplyReplication(const ReceivedMessageEvent &Message) {
			auto Frame = DecodeReplicationFrame(Message.Payload);
			if (!Frame) {
				Reject(
					Message.Connection, DisconnectReason::ProtocolViolation, "Malformed structural replication frame"
				);
				return;
			}
			auto Result = Replica.ApplyFrame(*Frame);
			if (!Result.Succeeded()) {
				Failure = "Structural replication frame was rejected: " + Result.Message;
				Reject(
					Message.Connection, DisconnectReason::ProtocolViolation, "Structural replication frame was rejected"
				);
				return;
			}
			for (const auto &Operation : Frame->Operations) {
				const auto Object = ReplicationObject(Operation.Intent);
				if (const auto *Publish = std::get_if<PublishReplication>(&Operation.Intent)) {
					auto ReplicaObject = Replica.Resolve(Object);
					if (std::dynamic_pointer_cast<KinematicCharacter>(ReplicaObject))
						ClientCharacterObjects.insert(Object);
					if (std::dynamic_pointer_cast<RemoteBase>(ReplicaObject)) ClientRemoteObjects.insert(Object);
				} else if (std::holds_alternative<UnpublishReplication>(Operation.Intent) ||
						   std::holds_alternative<DestroyReplication>(Operation.Intent)) {
					ClientCharacterObjects.erase(Object);
					ClientRemoteObjects.erase(Object);
					if (Prediction) (void)Prediction->MarkUnmaterialized(Object);
					if (Remotes) (void)Remotes->MarkUnmaterialized(Message.Connection, Object);
				}
			}
			BaselineApplied = BaselineApplied || Frame->Kind == ReplicationMessageKind::Baseline;
			if (ClientRuntimeAttached) SynchronizeClientGraph();
			TryFinalizeClientBootstrap();
		}

		void TryFinalizeClientBootstrap() {
			if (!PrimaryConnection || !BaselineApplied) return;
			auto PeerIterator = Peers.find(*PrimaryConnection);
			if (PeerIterator == Peers.end() || PeerIterator->second.Phase != PeerPhase::Accepted ||
				Replica.GetEpoch() != PeerIterator->second.Replication)
				return;
			auto Root = std::dynamic_pointer_cast<DataModel>(Replica.GetReplicaRoot());
			auto PlayerValue = std::dynamic_pointer_cast<Player>(Replica.Resolve(PeerIterator->second.PlayerObject));
			auto PlayersValue = Root ? std::dynamic_pointer_cast<Players>(Root->FindFirstChildOfClass("Players", false))
									 : nullptr;
			if (!Root || !PlayerValue || !PlayersValue || PeerIterator->second.PlayerId == 0 ||
				static_cast<std::uint32_t>(PlayerValue->GetPlayerId()) != PeerIterator->second.PlayerId ||
				!PlayersValue->SetTrustedLocalPlayer(PlayerValue)) {
				Reject(
					*PrimaryConnection,
					DisconnectReason::ProtocolViolation,
					"Trusted LocalPlayer association was not materialized"
				);
				return;
			}
			Status = GameSessionStatus::Accepted;
		}

		void ReadyServerPeer(ConnectionId Connection, const GameSessionClientReady &Ready) {
			auto Iterator = Peers.find(Connection);
			if (Iterator == Peers.end() || Iterator->second.Phase != PeerPhase::Accepted ||
				Ready.SessionEpoch != Iterator->second.SessionEpoch ||
				Ready.Replication != Iterator->second.Replication || Ready.Player != Iterator->second.PlayerObject) {
				Reject(
					Connection,
					DisconnectReason::ProtocolViolation,
					"Client readiness does not match its accepted session"
				);
				return;
			}
			auto &PeerValue = Iterator->second;
			if (!Remotes->AddPeer(Connection, PeerValue.Replication, PeerValue.Limits) ||
				!Authority->AddPeer(Connection)) {
				Reject(Connection, DisconnectReason::ResourceExhaustion, "Server gameplay manager registration failed");
				return;
			}
			PeerValue.Phase = PeerPhase::Ready;
			++Metrics.ReadyPeers;
			SynchronizeServerGraph();
		}

		void HandleSessionMessage(ConnectionId Connection, const ReceivedMessageEvent &Received) {
			if (Received.Delivery != DeliveryMode::ReliableOrdered || Received.Traffic != TrafficClass::Control ||
				!std::holds_alternative<std::monostate>(Received.Order)) {
				Reject(
					Connection,
					DisconnectReason::ProtocolViolation,
					"Game session frame used invalid delivery semantics"
				);
				return;
			}
			auto Decoded = DecodeGameSessionMessage(Received.Payload);
			if (!Decoded) {
				++Metrics.RejectedHandshakes;
				Reject(
					Connection,
					Decoded.error().Code == SerializationErrorCode::UnsupportedVersion
						? DisconnectReason::IncompatibleVersion
						: DisconnectReason::ProtocolViolation,
					"Game session handshake was rejected"
				);
				return;
			}
			if (Configuration.Role == GameSessionRole::Server) {
				if (const auto *Hello = std::get_if<GameSessionClientHello>(&*Decoded))
					AcceptServerPeer(Connection, *Hello);
				else if (const auto *Ready = std::get_if<GameSessionClientReady>(&*Decoded))
					ReadyServerPeer(Connection, *Ready);
				else
					Reject(
						Connection, DisconnectReason::ProtocolViolation, "Client sent a server-only session message"
					);
			} else {
				if (const auto *Accepted = std::get_if<GameSessionServerAccepted>(&*Decoded))
					AcceptClientSession(Connection, *Accepted);
				else
					Reject(
						Connection, DisconnectReason::ProtocolViolation, "Server sent a client-only session message"
					);
			}
		}

		void HandleReceived(const ReceivedMessageEvent &Received) {
			auto PeerIterator = Peers.find(Received.Connection);
			if (PeerIterator == Peers.end()) return;
			const auto Magic = FrameMagic(Received.Payload);
			if (Magic == 0x53455347) {
				HandleSessionMessage(Received.Connection, Received);
				return;
			}
			if (PeerIterator->second.Phase == PeerPhase::TransportConnected) {
				++Metrics.RejectedPreAcceptanceMessages;
				Reject(
					Received.Connection,
					DisconnectReason::ProtocolViolation,
					"Gameplay data arrived before session acceptance"
				);
				return;
			}
			if (Configuration.Role == GameSessionRole::Client &&
				Received.Traffic == TrafficClass::StructuralReplication) {
				ApplyReplication(Received);
				return;
			}
			if (PeerIterator->second.Phase != PeerPhase::Ready) {
				++Metrics.RejectedPreAcceptanceMessages;
				Reject(
					Received.Connection,
					DisconnectReason::ProtocolViolation,
					"Gameplay data arrived before client readiness"
				);
				return;
			}
			TransportEvent Event = Received;
			if (Magic == CharacterMagic) {
				if (Configuration.Role == GameSessionRole::Server)
					(void)Authority->HandleTransportEvent(Event);
				else
					(void)Prediction->HandleTransportEvent(Event);
			} else if (Magic == RemoteMagic) {
				(void)Remotes->HandleTransportEvent(Event);
			} else {
				Reject(Received.Connection, DisconnectReason::ProtocolViolation, "Unknown game-session protocol frame");
			}
		}

		std::size_t Poll() {
			std::array<TransportEvent, 256> Buffer;
			std::size_t Total = 0;
			for (;;) {
				const auto Count = Transport->PollEvents(Buffer);
				Total += Count;
				for (std::size_t Index = 0; Index < Count; ++Index) {
					auto Event = std::move(Buffer[Index]);
					if (const auto *Changed = std::get_if<ConnectionStateEvent>(&Event)) {
						if (Changed->Current == ConnectionState::Connected)
							OnConnected(Changed->Connection);
						else if (Changed->Current == ConnectionState::Closed)
							TearDownPeer(Changed->Connection);
					} else if (const auto *Received = std::get_if<ReceivedMessageEvent>(&Event)) {
						HandleReceived(*Received);
					} else if (const auto *Disconnected = std::get_if<DisconnectedEvent>(&Event)) {
						TearDownPeer(Disconnected->Connection);
					} else if (const auto *Failed = std::get_if<TransportFailureEvent>(&Event)) {
						Status = GameSessionStatus::Failed;
						Failure = Failed->Information.Diagnostic;
					}
				}
				if (Count < Buffer.size()) break;
			}
			return Total;
		}

		void SynchronizeServerGraph() {
			if (!Runtime || !Authority || !Remotes) return;
			std::set<ObjectId> CurrentCharacters;
			std::set<ObjectId> CurrentRemotes;
			for (const auto &Object : Runtime->DataModel->GetDescendants()) {
				if (auto CharacterValue = std::dynamic_pointer_cast<KinematicCharacter>(Object)) {
					const auto Id = CharacterValue->GetObjectId();
					CurrentCharacters.insert(Id);
					if (!ServerCharacters.contains(Id)) (void)Authority->RegisterCharacter(CharacterValue);
				}
				if (auto RemoteValue = std::dynamic_pointer_cast<RemoteBase>(Object)) {
					const auto Id = RemoteValue->GetObjectId();
					CurrentRemotes.insert(Id);
					if (!ServerRemoteObjects.contains(Id)) {
						(void)RemoteValue->BindRemoteManager(Remotes.get());
					}
				}
			}
			for (const auto Id : ServerCharacters)
				if (!CurrentCharacters.contains(Id))
					(void)Authority->UnregisterCharacter(Id, std::max(CurrentTick, std::uint64_t{1}));
			for (const auto Id : ServerRemoteObjects)
				if (!CurrentRemotes.contains(Id)) (void)Remotes->UnregisterRemote(Id);
			ServerCharacters = std::move(CurrentCharacters);
			ServerRemoteObjects = std::move(CurrentRemotes);

			for (auto &[Connection, PeerValue] : Peers) {
				if (PeerValue.Phase != PeerPhase::Ready) continue;
				const auto *View = Replication->GetView(Connection);
				if (!View) continue;
				for (const auto Object : View->KnownObjects)
					(void)Remotes->MarkMaterialized(Connection, Object);
				for (const auto Remote : ServerRemoteObjects)
					if (View->Knows(Remote)) (void)Remotes->PublishRemote(Connection, Remote, true);
				for (const auto Character : ServerCharacters)
					if (View->Knows(Character))
						(void)Authority->MarkMaterialized(Connection, Character, CharacterStateChannel(Character));

				auto CharacterValue = PeerValue.PlayerValue && PeerValue.PlayerValue->GetCharacter()
										  ? std::dynamic_pointer_cast<KinematicCharacter>(
												*PeerValue.PlayerValue->GetCharacter()
											)
										  : nullptr;
				const auto Desired = CharacterValue ? CharacterValue->GetObjectId() : ObjectId{};
				if (Desired == PeerValue.ControlledCharacter) continue;
				if (PeerValue.ControlledCharacter.IsValid()) {
					(void)Authority->RevokeControl(
						PeerValue.ControlledCharacter, std::max(CurrentTick, std::uint64_t{1})
					);
					++Metrics.CharacterControlRevocations;
				}
				PeerValue.ControlledCharacter = {};
				if (Desired.IsValid() && View->Knows(Desired) &&
					Authority->BindControl(Connection, Desired, std::max(CurrentTick, std::uint64_t{1}))) {
					PeerValue.ControlledCharacter = Desired;
					++Metrics.CharacterControlBindings;
				}
			}
		}

		void SynchronizeClientGraph() {
			if (!ClientRuntimeAttached || !Prediction || !Remotes || !PrimaryConnection) return;
			for (const auto Object : ClientCharacterObjects) {
				auto CharacterValue = std::dynamic_pointer_cast<KinematicCharacter>(Replica.Resolve(Object));
				if (CharacterValue) (void)Prediction->MarkMaterialized(Object, CharacterValue);
			}
			for (const auto Object : ClientRemoteObjects) {
				auto RemoteValue = std::dynamic_pointer_cast<RemoteBase>(Replica.Resolve(Object));
				if (!RemoteValue) continue;
				if (RemoteValue->BindRemoteManager(Remotes.get(), *PrimaryConnection, Object)) {
					(void)Remotes->PublishRemote(*PrimaryConnection, Object, true);
					(void)Remotes->MarkMaterialized(*PrimaryConnection, Object);
				}
			}
		}

		void SynchronizeActionPresentation() {
			if (!ClientRuntimeAttached || !Prediction || !Runtime) return;
			for (auto Iterator = PresentedActions.begin(); Iterator != PresentedActions.end();) {
				if (ClientCharacterObjects.contains(Iterator->first)) {
					++Iterator;
					continue;
				}
				if (Iterator->second.Track) Iterator->second.Track->Stop();
				Iterator = PresentedActions.erase(Iterator);
			}
			for (const auto Object : ClientCharacterObjects) {
				auto Presentation = Prediction->GetPresentationAction(Object);
				if (!Presentation || Presentation->PhaseTick < Presentation->Action.StartTick ||
					Presentation->PhaseTick - Presentation->Action.StartTick >= Presentation->Action.DurationTicks) {
					auto Existing = PresentedActions.find(Object);
					if (Existing != PresentedActions.end()) {
						if (Existing->second.Track) Existing->second.Track->Stop();
						PresentedActions.erase(Existing);
						++Metrics.ActionPresentationStops;
					}
					continue;
				}
				auto Existing = PresentedActions.find(Object);
				if (Existing != PresentedActions.end() &&
					SamePresentedAction(Existing->second.Action, Presentation->Action))
					continue;
				if (Existing != PresentedActions.end()) {
					if (Existing->second.Track) Existing->second.Track->Stop();
					PresentedActions.erase(Existing);
					++Metrics.ActionPresentationStops;
				}

				auto CharacterValue = std::dynamic_pointer_cast<Character>(Replica.Resolve(Object));
				auto AnimatorValue = CharacterValue ? std::dynamic_pointer_cast<Animator>(
														  CharacterValue->FindFirstChildOfClass("Animator", true)
													  )
													: nullptr;
				const auto Reference = AssetReference::FromAssetId(Presentation->Action.Animation).Value;
				const auto Record = Runtime->Assets->GetAsset(Reference);
				if (!AnimatorValue || !Record || Record->Kind != AssetKind::Animation ||
					Record->State != AssetState::Ready || Record->Id != Presentation->Action.Animation ||
					Record->ContentId != Presentation->Action.ContentRevision) {
					++Metrics.ActionPresentationDeferrals;
					continue;
				}
				try {
					auto Track = AnimatorValue->CreateTrack(Reference);
					const auto PhaseTicks = Presentation->PhaseTick - Presentation->Action.StartTick;
					const auto PhaseSeconds = static_cast<float>(PhaseTicks) /
											  static_cast<float>(DefaultCharacterSimulationTicksPerSecond);
					Track->Play();
					Track->SetTimePosition(std::min(PhaseSeconds, Track->GetDuration()));
					PresentedActions.emplace(Object, PresentedAction{Presentation->Action, std::move(Track)});
					++Metrics.ActionsPresented;
				} catch (const std::exception &) {
					// Asset or rig readiness is transient. A still-active action is retried on the next safe session
					// step.
					++Metrics.ActionPresentationDeferrals;
				}
			}
		}

		void TearDownPeer(ConnectionId Connection) {
			auto Iterator = Peers.find(Connection);
			if (Iterator == Peers.end()) return;
			auto PeerValue = std::move(Iterator->second);
			Peers.erase(Iterator);
			if (Remotes) (void)Remotes->RemovePeer(Connection);
			if (Authority) (void)Authority->RemovePeer(Connection, std::max(CurrentTick, std::uint64_t{1}));
			if (Prediction) (void)Prediction->RemovePeer(Connection);
			if (Replication) (void)Replication->RemovePeer(Connection);
			(void)Scheduler.CancelConnection(Connection);
			if (Configuration.Role == GameSessionRole::Server && PeerValue.PlayerValue) {
				if (Runtime->Players->RemoveSessionPlayer(PeerValue.PlayerValue)) ++Metrics.PlayersRemoved;
			} else if (Configuration.Role == GameSessionRole::Client) {
				if (Runtime) Runtime->Players->ClearTrustedLocalPlayer();
				Replica.Reset();
				BaselineApplied = false;
				ClientCharacterObjects.clear();
				ClientRemoteObjects.clear();
				PrimaryConnection.reset();
				if (Status != GameSessionStatus::Stopped) Status = GameSessionStatus::Failed;
				if (Failure.empty()) Failure = "Game server connection closed";
			}
		}

		void Step(std::uint64_t SimulationTick) {
			if (Status == GameSessionStatus::Stopped || Status == GameSessionStatus::Failed) return;
			CurrentTick = SimulationTick;
			std::vector<ConnectionId> TimedOut;
			for (const auto &[Connection, PeerValue] : Peers)
				if (PeerValue.Phase != PeerPhase::Ready && SimulationTick >= PeerValue.ConnectedTick &&
					SimulationTick - PeerValue.ConnectedTick >= Configuration.HandshakeTimeoutTicks)
					TimedOut.push_back(Connection);
			for (const auto Connection : TimedOut) {
				++Metrics.HandshakeTimeouts;
				Reject(Connection, DisconnectReason::Timeout, "Game session handshake timed out");
			}

			if (Configuration.Role == GameSessionRole::Server) {
				SynchronizeServerGraph();
				for (auto &[Connection, PeerValue] : Peers) {
					if (PeerValue.Phase == PeerPhase::TransportConnected) continue;
					auto Frame = Replication->ProduceIncremental(Connection);
					if (Frame.Succeeded() && Frame.Frame)
						(void)QueueReplicationFrame(*Frame.Frame, Connection, PeerValue.Limits, Scheduler);
				}
				Authority->Step(*Runtime->WorldRoot, std::max(SimulationTick, std::uint64_t{1}));
			} else if (ClientRuntimeAttached && Prediction) {
				Prediction->Reconcile(*Runtime->WorldRoot);
				Prediction->UpdatePresentation(SimulationTick);
				SynchronizeActionPresentation();
				for (const auto &Resolution : Prediction->DrainActionResolutions()) {
					auto CharacterValue = std::dynamic_pointer_cast<Character>(Replica.Resolve(Resolution.Character));
					if (CharacterValue)
						Runtime->CharacterControl->PublishActionResolution(
							CharacterValue, Resolution.RequestedActionToken, Resolution.Accepted
						);
				}
				for (const auto &Ended : Prediction->DrainActionEndings()) {
					auto CharacterValue = std::dynamic_pointer_cast<Character>(Replica.Resolve(Ended.Character));
					if (CharacterValue)
						Runtime->CharacterControl->PublishActionEnded(CharacterValue, Ended.ActionToken);
				}
			}
			if (Remotes) (void)Remotes->Pump(Configuration.Limits.MaximumMessagesPerTick);
			std::vector<std::pair<ConnectionId, DisconnectInfo>> TerminalConnections;
			for (const auto &[Connection, PeerValue] : Peers) {
				if (Configuration.Role == GameSessionRole::Server && PeerValue.Phase == PeerPhase::TransportConnected)
					continue;
				auto Flushed = Scheduler.Flush(
					Connection,
					SchedulerTickBudget::FromNetworkLimits(
						PeerValue.Limits.IsValid() ? PeerValue.Limits : Configuration.Limits
					)
				);
				if (Flushed.IsTerminal()) TerminalConnections.emplace_back(Connection, *Flushed.TerminalDisconnect);
			}
			for (auto &[Connection, Information] : TerminalConnections)
				Reject(Connection, Information.Reason, std::move(Information.Diagnostic));
		}

		bool AttachClientRuntime(Engine &RuntimeValue) {
			if (Configuration.Role != GameSessionRole::Client || ClientRuntimeAttached ||
				Status != GameSessionStatus::Accepted || !PrimaryConnection ||
				RuntimeValue.DataModel != GetClientDataModel())
				return false;
			Runtime = &RuntimeValue;
			ClientRuntimeAttached = true;
			try {
				InitializeClientManagers();
			} catch (const std::exception &Error) {
				ClientRuntimeAttached = false;
				Failure = Error.what();
				Status = GameSessionStatus::Failed;
				return false;
			}
			auto &PeerValue = Peers.at(*PrimaryConnection);
			if (!QueueSession(
					*PrimaryConnection,
					GameSessionClientReady{PeerValue.SessionEpoch, PeerValue.Replication, PeerValue.PlayerObject},
					PeerValue.Limits
				)) {
				Status = GameSessionStatus::Failed;
				Failure = "Client readiness could not be queued";
				return false;
			}
			PeerValue.Phase = PeerPhase::Ready;
			Status = GameSessionStatus::Ready;
			++Metrics.ReadyPeers;
			return true;
		}

		std::shared_ptr<DataModel> GetClientDataModel() const {
			return Configuration.Role == GameSessionRole::Client && Status >= GameSessionStatus::Accepted
					   ? std::dynamic_pointer_cast<DataModel>(Replica.GetReplicaRoot())
					   : nullptr;
		}
	};

	GameSession::GameSession(
		std::shared_ptr<IGameTransport> Transport, GameSessionConfiguration Configuration, Engine *ServerRuntime
	)
		: State(std::make_unique<Implementation>(std::move(Transport), std::move(Configuration), ServerRuntime)) {}

	GameSession::~GameSession() = default;

	TransportOperationResult GameSession::Start() {
		return State->Start();
	}
	void GameSession::Stop() {
		State->Stop();
	}
	std::size_t GameSession::Poll() {
		return State->Poll();
	}
	void GameSession::Step(std::uint64_t SimulationTick) {
		State->Step(SimulationTick);
	}
	bool GameSession::AttachClientRuntime(Engine &Runtime) {
		return State->AttachClientRuntime(Runtime);
	}
	GameSessionRole GameSession::GetRole() const {
		return State->Configuration.Role;
	}
	GameSessionStatus GameSession::GetStatus() const {
		return State->Status;
	}
	const std::string &GameSession::GetFailure() const {
		return State->Failure;
	}
	GameSessionMetrics GameSession::GetMetrics() const {
		return State->Metrics;
	}
	std::shared_ptr<DataModel> GameSession::GetClientDataModel() const {
		return State->GetClientDataModel();
	}
	std::optional<ConnectionId> GameSession::GetPrimaryConnection() const {
		return State->PrimaryConnection;
	}
	std::shared_ptr<Player> GameSession::GetAcceptedPlayer(ConnectionId Connection) const {
		auto Iterator = State->Peers.find(Connection);
		return Iterator != State->Peers.end() && Iterator->second.Phase != PeerPhase::TransportConnected
				   ? Iterator->second.PlayerValue
				   : nullptr;
	}
}
