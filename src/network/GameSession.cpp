#include "gargantuan/network/GameSession.hpp"

#include "GameSessionTestAccess.hpp"

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
#include <chrono>
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
		constexpr std::size_t MaximumGameSessionPollEventsPerCall = 128;

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

		std::optional<DisconnectInfo> SchedulerFlushFailure(const SchedulerFlushResult &Result) {
			if (Result.TerminalDisconnect) return Result.TerminalDisconnect;
			if (Result.Status == SchedulerFlushStatus::InvalidConnection ||
				Result.Status == SchedulerFlushStatus::InvalidBudget)
				return DisconnectInfo{
					DisconnectReason::ResourceExhaustion,
					"Scheduler flush ownership became invalid",
				};
			return std::nullopt;
		}
	}

	bool GameSessionConfiguration::IsValid() const {
		return Endpoint.IsValid() && Limits.IsValid() && HandshakeTimeoutTicks > 0 &&
			   HandshakeTimeoutTicks <= DefaultGameSessionHandshakeTimeoutTicks * 10 && Relevance.IsValid() &&
			   (AllowInsecureDevelopmentNetwork || IsLoopbackTransportEndpoint(Endpoint));
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
			std::set<ObjectId> MaterializedCharacters;
			std::set<ObjectId> MaterializedRemotes;
			std::set<ObjectId> RemoteMaterializedObjects;
			bool SchedulerFlushedThisStep = false;
		};

		std::shared_ptr<IGameTransport> Transport;
		GameSessionConfiguration Configuration;
		NetworkScheduler Scheduler;
		Engine *Runtime = nullptr;
		GameSessionStatus Status = GameSessionStatus::Created;
		std::string Failure;
		GameSessionMetrics Metrics;
		std::uint64_t CurrentTick = 0;
		std::uint64_t NextSessionEpoch = 1;
		std::uint64_t NextReplicationEpoch = 1;
		std::map<ConnectionId, Peer> Peers;
		std::optional<ConnectionId> PrimaryConnection;

		std::unique_ptr<ReplicationCoordinator> Replication;
		std::unique_ptr<ReplicationRelevance> Relevance;
		ReplicaApplier Replica;
		std::unique_ptr<RemoteManager> Remotes;
		std::unique_ptr<AuthoritativeCharacterNetwork> Authority;
		std::unique_ptr<PredictedCharacterNetwork> Prediction;
		std::set<ObjectId> ServerCharacters;
		std::set<ObjectId> ServerRemoteObjects;
		std::set<ObjectId> ClientCharacterObjects;
		std::set<ObjectId> ClientRemoteObjects;
		std::set<ObjectId> ClientMaterializedCharacters;
		std::set<ObjectId> ClientKnownObjects;
		SignalConnection::Pointer ServerDescendantAdded;
		SignalConnection::Pointer ServerDescendantRemoved;
		std::map<ObjectId, PresentedAction> PresentedActions;
		std::optional<CharacterControlService::RuntimeAttachment> ControlAttachment;
		std::map<ConnectionId, DisconnectInfo> PendingPeerFailures;
		std::optional<DisconnectInfo> PendingSessionFailure;
		bool BaselineApplied = false;
		bool ClientRuntimeAttached = false;
		detail::GameSessionFailurePoint FailurePoint = detail::GameSessionFailurePoint::None;

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
		}

		~Implementation() {
			Stop();
		}

		bool InjectFailure(detail::GameSessionFailurePoint Point) {
			if (FailurePoint != Point) return false;
			FailurePoint = detail::GameSessionFailurePoint::None;
			return true;
		}

		bool RegisterServerObject(const std::shared_ptr<Instance> &Object) {
			if (!Object || Object->GetDestroyed() || Object->IsDestroying()) return true;
			if (auto CharacterValue = std::dynamic_pointer_cast<KinematicCharacter>(Object)) {
				const auto Id = CharacterValue->GetObjectId();
				if (!ServerCharacters.contains(Id)) {
					if (!Authority->RegisterCharacter(CharacterValue)) return false;
					ServerCharacters.insert(Id);
				}
			}
			if (auto RemoteValue = std::dynamic_pointer_cast<RemoteBase>(Object)) {
				const auto Id = RemoteValue->GetObjectId();
				if (!ServerRemoteObjects.contains(Id)) {
					if (!RemoteValue->BindRemoteManager(Remotes.get())) return false;
					ServerRemoteObjects.insert(Id);
				}
			}
			return true;
		}

		void UnregisterServerObject(const std::shared_ptr<Instance> &Object) {
			if (!Object) return;
			const auto Id = Object->GetObjectId();
			if (ServerCharacters.contains(Id)) {
				if (!Authority->UnregisterCharacter(Id, std::max(CurrentTick, std::uint64_t{1}))) {
					if (!PendingSessionFailure)
						PendingSessionFailure = DisconnectInfo{
							DisconnectReason::ResourceExhaustion,
							"[Character:Relevance] failed to retire the authoritative Character lifetime",
						};
					return;
				}
				ServerCharacters.erase(Id);
				for (auto &[Connection, PeerValue] : Peers) {
					(void)Connection;
					PeerValue.MaterializedCharacters.erase(Id);
					if (PeerValue.ControlledCharacter == Id) {
						PeerValue.ControlledCharacter = {};
						++Metrics.CharacterControlRevocations;
					}
				}
			}
			if (ServerRemoteObjects.erase(Id)) {
				if (!Remotes->UnregisterRemote(Id)) {
					if (!PendingSessionFailure)
						PendingSessionFailure = DisconnectInfo{
							DisconnectReason::ResourceExhaustion,
							"[Remote:Registry] failed to retire an authoritative Remote lifetime",
						};
					return;
				}
				for (auto &[Connection, PeerValue] : Peers) {
					(void)Connection;
					PeerValue.MaterializedRemotes.erase(Id);
				}
			}
		}

		void InitializeServerManagers() {
			Replication = std::make_unique<ReplicationCoordinator>(Runtime->DataModel, [this](ObjectId Object) {
				return !Runtime->Players->IsRuntimeModule(Object);
			});
			Relevance = std::make_unique<ReplicationRelevance>(
				Runtime->DataModel,
				[this](ObjectId Object) { return Runtime->Players->IsRuntimeModule(Object); },
				Configuration.Relevance
			);
			Remotes = std::make_unique<RemoteManager>(
				RemoteManagerRole::Server,
				Scheduler,
				[this](ConnectionId Connection, ObjectId Object) {
					const auto *View = Replication->GetView(Connection);
					return View && View->Knows(Object);
				},
				[](ObjectId Object) { return ObjectRegistry::Get().Lookup(Object); }
			);
			Remotes->SetTerminalHandler([this](ConnectionId Connection, const DisconnectInfo &Information) {
				PendingPeerFailures.try_emplace(Connection, Information);
			});
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
			Authority->SetTerminalHandler([this](ConnectionId Connection, const DisconnectInfo &Information) {
				PendingPeerFailures.try_emplace(Connection, Information);
			});
			for (const auto &Object : Runtime->DataModel->GetDescendants())
				if (!RegisterServerObject(Object))
					throw std::runtime_error("[Network:Session] Server gameplay object registration failed");
			ControlAttachment = Runtime->CharacterControl->AttachRuntime(
				{},
				{},
				[this](const CharacterActionDefinition &Definition, bool) {
					return Authority->RegisterAction(Definition);
				},
				{}
			);
			if (!ControlAttachment)
				throw std::runtime_error("[Network:Session] Server Character-control attachment failed");
			ServerDescendantAdded = Runtime->DataModel->DescendantAdded->Connect(
				[this](std::shared_ptr<Instance> Object) {
					if (!RegisterServerObject(Object))
						if (!PendingSessionFailure)
							PendingSessionFailure = DisconnectInfo{
								DisconnectReason::ResourceExhaustion,
								"Server gameplay object registration failed",
							};
				}
			);
			ServerDescendantRemoved = Runtime->DataModel->DescendantRemoved->Connect(
				[this](std::shared_ptr<Instance> Object) { UnregisterServerObject(Object); }
			);
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
			Remotes->SetTerminalHandler([this](ConnectionId FailedConnection, const DisconnectInfo &Information) {
				PendingPeerFailures.try_emplace(FailedConnection, Information);
			});
			Prediction = std::make_unique<PredictedCharacterNetwork>(
				Scheduler,
				PeerValue.Limits,
				[this](const CharacterInputCommand &Command, const KinematicCharacter &CharacterValue) {
					return Runtime->CharacterControl->EvaluateMovement(Command, CharacterValue);
				}
			);
			Prediction->SetTerminalHandler([this](ConnectionId FailedConnection, const DisconnectInfo &Information) {
				PendingPeerFailures.try_emplace(FailedConnection, Information);
			});
			if (!Remotes->AddPeer(Connection, PeerValue.Replication, PeerValue.Limits) ||
				InjectFailure(detail::GameSessionFailurePoint::RemoteManagerPeerCreation))
				throw std::runtime_error("[Network:Session] Failed to attach client gameplay managers");
			if (!Prediction->AddPeer(Connection) ||
				InjectFailure(detail::GameSessionFailurePoint::PredictedCharacterPeerCreation))
				throw std::runtime_error("[Network:Session] Failed to attach client Character manager");
			for (const auto Object : ClientKnownObjects)
				if (!Remotes->MarkMaterialized(Connection, Object))
					throw std::runtime_error("[Network:Session] Client Remote materialization failed");
			ControlAttachment = Runtime->CharacterControl->AttachRuntime(
				[this, Connection](std::uint64_t Tick, float DeltaSeconds, glm::vec2 Move, float Facing, bool Jump) {
					return Prediction->SubmitInput(
						Connection, *Runtime->WorldRoot, Tick, DeltaSeconds, Move, Facing, Jump
					);
				},
				[this, Connection](std::uint32_t Action, std::uint64_t Tick) {
					return Prediction->RequestAction(Connection, Action, Tick);
				},
				[this](const CharacterActionDefinition &Definition, bool Predictable) {
					return !Predictable || Prediction->RegisterAction(Definition);
				},
				[this](bool Enabled) { Prediction->SetPredictionEnabled(Enabled); }
			);
			if (!ControlAttachment)
				throw std::runtime_error("[Network:Session] Client Character-control attachment failed");
			if (InjectFailure(detail::GameSessionFailurePoint::RuntimeCallbackAttachment))
				throw std::runtime_error("[Network:Session] Injected Character-control attachment failure");
			if (!SynchronizeClientGraph() ||
				InjectFailure(detail::GameSessionFailurePoint::ClientGraphSynchronization))
				throw std::runtime_error("[Network:Session] Client gameplay graph synchronization failed");
		}

		bool ApplyServerRemoteMaterialization(ConnectionId Connection, const ReplicationFrame &Frame) {
			auto Peer = Peers.find(Connection);
			if (Peer == Peers.end() || Peer->second.Phase != PeerPhase::Ready || !Remotes) return true;
			for (const auto &Operation : Frame.Operations)
				if (const auto *Publish = std::get_if<PublishReplication>(&Operation.Intent)) {
					if (!Peer->second.RemoteMaterializedObjects.contains(Publish->Object)) {
						if (!Remotes->MarkMaterialized(Connection, Publish->Object)) return false;
						Peer->second.RemoteMaterializedObjects.insert(Publish->Object);
					}
				}
			const auto *View = Replication ? Replication->GetView(Connection) : nullptr;
			if (!View) return false;
			for (auto Iterator = Peer->second.RemoteMaterializedObjects.begin();
				 Iterator != Peer->second.RemoteMaterializedObjects.end();) {
				if (View->Knows(*Iterator)) {
					++Iterator;
					continue;
				}
				if (!Remotes->MarkUnmaterialized(Connection, *Iterator)) return false;
				Iterator = Peer->second.RemoteMaterializedObjects.erase(Iterator);
			}
			return true;
		}

		TransportOperationResult Start() {
			if (Status != GameSessionStatus::Created) return {.Status = TransportOperationStatus::InvalidState};
			Status = GameSessionStatus::Starting;
			if (Configuration.Role == GameSessionRole::Server) {
				try {
					InitializeServerManagers();
				} catch (...) {
					FailSession({DisconnectReason::ResourceExhaustion, "Server session initialization failed"});
					return {
						.Status = TransportOperationStatus::ResourceExhausted,
						.TerminalDisconnect = DisconnectInfo{
							DisconnectReason::ResourceExhaustion,
							"Server session initialization failed",
						},
					};
				}
			}
			auto Result = Transport->Start({
				.Role = Configuration.Role == GameSessionRole::Server ? TransportRole::Server : TransportRole::Client,
				.Endpoint = Configuration.Endpoint,
				.AdvertisedLimits = Configuration.Limits,
			});
			if (Result.Succeeded() && InjectFailure(detail::GameSessionFailurePoint::TransportStart)) {
				FailSession({DisconnectReason::ResourceExhaustion, "Injected transport-start acquisition failure"});
				return {
					.Status = TransportOperationStatus::ResourceExhausted,
					.TerminalDisconnect = DisconnectInfo{
						DisconnectReason::ResourceExhaustion,
						"Injected transport-start acquisition failure",
					},
				};
			}
			if (Result.Succeeded())
				Status = Configuration.Role == GameSessionRole::Server ? GameSessionStatus::Listening
																	   : GameSessionStatus::Connecting;
			else {
				FailSession(Result.TerminalDisconnect.value_or(DisconnectInfo{
					DisconnectReason::TransportFailure,
					"Transport endpoint failed to start",
				}));
			}
			return Result;
		}

		void ReleaseSessionResources() {
			ControlAttachment.reset();
			if (ServerDescendantAdded) ServerDescendantAdded->Disconnect();
			if (ServerDescendantRemoved) ServerDescendantRemoved->Disconnect();
			ServerDescendantAdded.reset();
			ServerDescendantRemoved.reset();
			for (auto &[Character, Presented] : PresentedActions) {
				(void)Character;
				if (Presented.Track) Presented.Track->Stop();
			}
			PresentedActions.clear();
			std::vector<ConnectionId> Connections;
			Connections.reserve(Peers.size());
			for (const auto &[Connection, PeerValue] : Peers) {
				(void)PeerValue;
				Connections.push_back(Connection);
			}
			for (const auto Connection : Connections)
				TearDownPeer(Connection);
			Remotes.reset();
			Authority.reset();
			Prediction.reset();
			Replication.reset();
			Relevance.reset();
			Replica.Reset();
			ServerCharacters.clear();
			ServerRemoteObjects.clear();
			ClientCharacterObjects.clear();
			ClientRemoteObjects.clear();
			ClientMaterializedCharacters.clear();
			ClientKnownObjects.clear();
			PrimaryConnection.reset();
			PendingPeerFailures.clear();
			PendingSessionFailure.reset();
			BaselineApplied = false;
			ClientRuntimeAttached = false;
			Runtime = nullptr;
		}

		void FailSession(DisconnectInfo Information) {
			if (Status == GameSessionStatus::Failed || Status == GameSessionStatus::Closed) return;
			const auto Diagnostic = Information.Diagnostic.size() <= 512
									? Information.Diagnostic
									: Information.Diagnostic.substr(0, 512);
			Status = GameSessionStatus::Closing;
			ReleaseSessionResources();
			(void)Transport->Stop(std::move(Information));
			Failure = Diagnostic.empty() ? "Game session failed" : Diagnostic;
			Status = GameSessionStatus::Failed;
		}

		void Stop() {
			if (Status == GameSessionStatus::Closed || Status == GameSessionStatus::Failed) return;
			const bool TransportStarted = Status != GameSessionStatus::Created;
			Status = GameSessionStatus::Closing;
			ReleaseSessionResources();
			if (TransportStarted)
				(void)Transport->Stop({DisconnectReason::LocalShutdown, "Game session stopped"});
			Status = GameSessionStatus::Closed;
		}

		bool QueueSession(ConnectionId Connection, const GameSessionMessage &Message, const NetworkLimits &Limits) {
			auto Encoded = EncodeGameSessionMessage(Message);
			if (!Encoded) return false;
			auto Intent = MakeNetworkMessageIntent(
				Connection, DeliveryMode::ReliableOrdered, TrafficClass::Control, {}, std::move(*Encoded), Limits
			);
			return Intent && Scheduler.Submit(std::move(*Intent)).Accepted();
		}

		SerializationResult<SchedulerSubmitResult> QueueStructuralFrame(
			const ReplicationFrame &Frame, ConnectionId Connection, const NetworkLimits &Limits
		) {
			if (InjectFailure(detail::GameSessionFailurePoint::StructuralSchedulerAdmission))
				return SchedulerSubmitResult{
					SchedulerSubmitStatus::ReliableBacklogExhausted,
					DisconnectInfo{
						DisconnectReason::ResourceExhaustion,
						"Injected reliable structural scheduler exhaustion",
					},
				};
			return QueueReplicationFrame(Frame, Connection, Limits, Scheduler);
		}

		void FailPeer(ConnectionId Connection, DisconnectReason Reason, std::string Diagnostic) {
			if (Reason == DisconnectReason::ProtocolViolation) ++Metrics.ProtocolRejects;
			LOG_WARN(
				App,
				"[Network:Session] Rejecting peer %u:%u: %s",
				Connection.Slot,
				Connection.Generation,
				Diagnostic.c_str()
			);
			if (Configuration.Role == GameSessionRole::Client)
				FailSession({Reason, std::move(Diagnostic)});
			else {
				TearDownPeer(Connection);
				(void)Transport->Disconnect(Connection, {Reason, std::move(Diagnostic)});
			}
		}

		void Reject(ConnectionId Connection, DisconnectReason Reason, std::string Diagnostic) {
			FailPeer(Connection, Reason, std::move(Diagnostic));
		}

		bool DrainFailures() {
			if (PendingSessionFailure) {
				auto Information = std::move(*PendingSessionFailure);
				PendingSessionFailure.reset();
				FailSession(std::move(Information));
				return false;
			}
			auto Failures = std::move(PendingPeerFailures);
			PendingPeerFailures.clear();
			for (auto &[Connection, Information] : Failures) {
				if (!Peers.contains(Connection)) continue;
				FailPeer(Connection, Information.Reason, std::move(Information.Diagnostic));
				if (Configuration.Role == GameSessionRole::Client) return false;
			}
			return Status != GameSessionStatus::Failed && Status != GameSessionStatus::Closed;
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
			if (InjectFailure(detail::GameSessionFailurePoint::SchedulerRegistration)) {
				Reject(Connection, DisconnectReason::ResourceExhaustion, "Injected scheduler registration failure");
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
			const auto AcceptanceStarted = std::chrono::steady_clock::now();
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
			const auto PlayerCreationStarted = std::chrono::steady_clock::now();
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
			Metrics.PlayerCreationCpuNanoseconds += static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - PlayerCreationStarted
				)
					.count()
			);
			PeerValue.PlayerObject = PeerValue.PlayerValue->GetObjectId();
			PeerValue.PlayerId = static_cast<std::uint32_t>(PeerValue.PlayerValue->GetPlayerId());
			++Metrics.PlayersCreated;
			auto CharacterValue = PeerValue.PlayerValue->GetCharacter() ? std::dynamic_pointer_cast<KinematicCharacter>(
																							  *PeerValue.PlayerValue->GetCharacter()
																						  )
																				: nullptr;
			const auto OwnerCharacter = CharacterValue ? CharacterValue->GetObjectId() : ObjectId{};
			const auto RelevanceStarted = std::chrono::steady_clock::now();
			if (!Relevance->AddPeer(Connection, PeerValue.PlayerObject, OwnerCharacter)) {
				Reject(Connection, DisconnectReason::ResourceExhaustion, "Server relevance registration failed");
				return;
			}
			Metrics.RelevanceInitializationCpuNanoseconds += static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - RelevanceStarted
				)
					.count()
			);
			const auto *Selection = Relevance->GetSelection(Connection);
			if (!Selection) {
				Reject(Connection, DisconnectReason::ResourceExhaustion, "Server relevance selection is unavailable");
				return;
			}
			const auto ReplicationMetricsBefore = Replication->GetMetrics();
			auto Baseline = Replication->AddPeer(Connection, PeerValue.Replication, *Selection);
			const auto ReplicationMetricsAfter = Replication->GetMetrics();
			Metrics.BaselineSnapshotCpuNanoseconds += ReplicationMetricsAfter.SnapshotCaptureCpuNanoseconds -
																					  ReplicationMetricsBefore.SnapshotCaptureCpuNanoseconds;
			Metrics.BaselineDiscoveryCpuNanoseconds += ReplicationMetricsAfter.BaselineDiscoveryCpuNanoseconds -
																					   ReplicationMetricsBefore.BaselineDiscoveryCpuNanoseconds;
			Metrics.BaselineEncodeCpuNanoseconds += ReplicationMetricsAfter.BaselineEncodeCpuNanoseconds -
																			ReplicationMetricsBefore.BaselineEncodeCpuNanoseconds;
			if (!Baseline.Succeeded()) {
				Reject(Connection, DisconnectReason::ResourceExhaustion, "Server replication registration failed");
				return;
			}
			auto BaselineQueued = QueueStructuralFrame(*Baseline.Frame, Connection, PeerValue.Limits);
			if (
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
			Metrics.SessionAcceptanceCpuNanoseconds += static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - AcceptanceStarted
				)
					.count()
			);
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
			if (!Scheduler.CancelConnection(Connection) ||
				!Scheduler.RegisterConnection(Connection, Accepted.NegotiatedLimits)) {
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
				Reject(
					Message.Connection,
					DisconnectReason::ProtocolViolation,
					"Structural replication frame was rejected: " + Result.Message
				);
				return;
			}
			if (Frame->Kind == ReplicationMessageKind::Baseline &&
				InjectFailure(detail::GameSessionFailurePoint::ReplicationPeerCreation)) {
				FailSession({DisconnectReason::ResourceExhaustion, "Injected replication-peer acquisition failure"});
				return;
			}
			for (const auto &Operation : Frame->Operations) {
				const auto Object = ReplicationObject(Operation.Intent);
				if (const auto *Publish = std::get_if<PublishReplication>(&Operation.Intent)) {
					ClientKnownObjects.insert(Object);
					if (Remotes && !Remotes->MarkMaterialized(Message.Connection, Object)) {
						FailSession({
							DisconnectReason::ResourceExhaustion,
							"Client Remote materialization registration failed",
						});
						return;
					}
					auto ReplicaObject = Replica.Resolve(Object);
					if (std::dynamic_pointer_cast<KinematicCharacter>(ReplicaObject))
						ClientCharacterObjects.insert(Object);
					if (std::dynamic_pointer_cast<RemoteBase>(ReplicaObject)) ClientRemoteObjects.insert(Object);
				} else if (std::holds_alternative<UnpublishReplication>(Operation.Intent) ||
						   std::holds_alternative<DestroyReplication>(Operation.Intent)) {
					ClientKnownObjects.erase(Object);
					ClientCharacterObjects.erase(Object);
					ClientRemoteObjects.erase(Object);
					if (Prediction && ClientMaterializedCharacters.erase(Object) &&
						!Prediction->MarkUnmaterialized(Object)) {
						FailSession({
							DisconnectReason::ResourceExhaustion,
							"Client Character unmaterialization registration failed",
						});
						return;
					}
					if (Remotes && !Remotes->MarkUnmaterialized(Message.Connection, Object)) {
						FailSession({
							DisconnectReason::ResourceExhaustion,
							"Client Remote unmaterialization registration failed",
						});
						return;
					}
				}
			}
			for (auto Iterator = ClientKnownObjects.begin(); Iterator != ClientKnownObjects.end();) {
				if (Replica.Resolve(*Iterator)) {
					++Iterator;
					continue;
				}
				if (Remotes && !Remotes->MarkUnmaterialized(Message.Connection, *Iterator)) {
					FailSession({
						DisconnectReason::ResourceExhaustion,
						"Client materialization registry diverged from its replica",
					});
					return;
				}
				if (Prediction && ClientMaterializedCharacters.erase(*Iterator) &&
					!Prediction->MarkUnmaterialized(*Iterator)) {
					FailSession({
						DisconnectReason::ResourceExhaustion,
						"Client Character registry diverged from its replica",
					});
					return;
				}
				ClientCharacterObjects.erase(*Iterator);
				ClientRemoteObjects.erase(*Iterator);
				Iterator = ClientKnownObjects.erase(Iterator);
			}
			BaselineApplied = BaselineApplied || Frame->Kind == ReplicationMessageKind::Baseline;
			if (ClientRuntimeAttached && !SynchronizeClientGraph()) {
				FailSession({
					DisconnectReason::ResourceExhaustion,
					"Client gameplay graph synchronization failed",
				});
				return;
			}
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
			if (!PlayerValue) return;
			if (!Root || !PlayersValue || PeerIterator->second.PlayerId == 0 ||
				static_cast<std::uint32_t>(PlayerValue->GetPlayerId()) != PeerIterator->second.PlayerId ||
				!PlayersValue->SetTrustedLocalPlayer(PlayerValue)) {
				Reject(
					*PrimaryConnection,
					DisconnectReason::ProtocolViolation,
					"Trusted LocalPlayer association was not materialized"
				);
				return;
			}
			if (InjectFailure(detail::GameSessionFailurePoint::LocalPlayerResolution)) {
				FailSession({DisconnectReason::ResourceExhaustion, "Injected LocalPlayer resolution failure"});
				return;
			}
			Status = GameSessionStatus::Accepted;
		}

		void ReadyServerPeer(ConnectionId Connection, const GameSessionClientReady &Ready) {
			const auto RegistrationStarted = std::chrono::steady_clock::now();
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
			if (const auto *View = Replication->GetView(Connection)) {
				PeerValue.RemoteMaterializedObjects = std::set<ObjectId>(
					View->KnownObjects.begin(), View->KnownObjects.end()
				);
				for (const auto Object : PeerValue.RemoteMaterializedObjects)
					if (!Remotes->MarkMaterialized(Connection, Object)) {
						FailPeer(
							Connection,
							DisconnectReason::ResourceExhaustion,
							"Server gameplay materialization registration failed"
						);
						return;
					}
			}
			++Metrics.ReadyPeers;
			Metrics.GameplayRegistrationCpuNanoseconds += static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - RegistrationStarted
				)
					.count()
			);
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
			std::array<TransportEvent, MaximumGameSessionPollEventsPerCall> Buffer;
			const auto Count = Transport->PollEvents(Buffer);
			for (std::size_t Index = 0; Index < Count; ++Index) {
				auto Event = std::move(Buffer[Index]);
				if (const auto *Changed = std::get_if<ConnectionStateEvent>(&Event)) {
					if (Changed->Current == ConnectionState::Connected)
						OnConnected(Changed->Connection);
					else if (Changed->Current == ConnectionState::Closed) {
						if (Configuration.Role == GameSessionRole::Client)
							FailSession({DisconnectReason::RemoteShutdown, "Game server connection closed"});
						else
							TearDownPeer(Changed->Connection);
					}
				} else if (const auto *Received = std::get_if<ReceivedMessageEvent>(&Event)) {
					HandleReceived(*Received);
				} else if (const auto *Disconnected = std::get_if<DisconnectedEvent>(&Event)) {
					if (Configuration.Role == GameSessionRole::Client)
						FailSession(Disconnected->Information);
					else
						TearDownPeer(Disconnected->Connection);
				} else if (const auto *Failed = std::get_if<TransportFailureEvent>(&Event)) {
					FailSession(Failed->Information);
				}
				if (Status == GameSessionStatus::Failed || Status == GameSessionStatus::Closed) return Count;
			}
			(void)DrainFailures();
			return Count;
		}

		void SynchronizeServerGraph() {
			if (!Runtime || !Authority || !Remotes || !Relevance) return;
			const auto Started = std::chrono::steady_clock::now();
			for (auto &[Connection, PeerValue] : Peers) {
				if (PeerValue.Phase != PeerPhase::Ready) continue;
				const auto *View = Replication->GetView(Connection);
				if (!View) continue;
				std::set<ObjectId> DesiredRemotes;
				for (const auto Remote : ServerRemoteObjects)
					if (View->Knows(Remote)) DesiredRemotes.insert(Remote);
				bool PeerHealthy = true;
				for (const auto Remote : PeerValue.MaterializedRemotes)
					if (!DesiredRemotes.contains(Remote) && !Remotes->MarkUnmaterialized(Connection, Remote))
						PeerHealthy = false;
				for (const auto Remote : DesiredRemotes)
					if (!PeerValue.MaterializedRemotes.contains(Remote)) {
						if (!Remotes->MarkMaterialized(Connection, Remote) ||
							!Remotes->PublishRemote(Connection, Remote, true))
							PeerHealthy = false;
					}
				if (!PeerHealthy) {
					PendingPeerFailures.try_emplace(
						Connection,
						DisconnectInfo{
							DisconnectReason::ResourceExhaustion,
							"Remote relevance registration failed",
						}
					);
					continue;
				}
				PeerValue.MaterializedRemotes = std::move(DesiredRemotes);

				std::set<ObjectId> DesiredCharacters;
				for (const auto Character : ServerCharacters) {
					auto CharacterValue = std::dynamic_pointer_cast<KinematicCharacter>(
						ObjectRegistry::Get().Lookup(Character)
					);
					auto RootPart = CharacterValue ? CharacterValue->GetRootPart() : std::nullopt;
					if (View->Knows(Character) && Relevance->IsRuntimeRelevant(Connection, Character) && RootPart &&
						View->Knows((*RootPart)->GetObjectId()))
						DesiredCharacters.insert(Character);
				}
				for (const auto Character : PeerValue.MaterializedCharacters)
					if (!DesiredCharacters.contains(Character) &&
						!Authority->MarkUnmaterialized(Connection, Character)) {
						PeerHealthy = false;
					}
				for (const auto Character : DesiredCharacters)
					if (!PeerValue.MaterializedCharacters.contains(Character) &&
						!Authority->MarkMaterialized(Connection, Character, CharacterStateChannel(Character))) {
						PeerHealthy = false;
					}
				if (!PeerHealthy) {
					PendingPeerFailures.try_emplace(
						Connection,
						DisconnectInfo{
							DisconnectReason::ResourceExhaustion,
							"[Character:Relevance] materialization generation could not advance",
						}
					);
					continue;
				}
				PeerValue.MaterializedCharacters = std::move(DesiredCharacters);

				auto CharacterValue = PeerValue.PlayerValue && PeerValue.PlayerValue->GetCharacter()
										  ? std::dynamic_pointer_cast<KinematicCharacter>(
												*PeerValue.PlayerValue->GetCharacter()
											)
										  : nullptr;
				const auto Desired = CharacterValue ? CharacterValue->GetObjectId() : ObjectId{};
				if (Desired == PeerValue.ControlledCharacter) continue;
				if (PeerValue.ControlledCharacter.IsValid()) {
					if (!Authority->RevokeControl(
							PeerValue.ControlledCharacter, std::max(CurrentTick, std::uint64_t{1})
						)) {
						PendingPeerFailures.try_emplace(
							Connection,
							DisconnectInfo{
								DisconnectReason::ResourceExhaustion,
								"Owner Character control revocation failed",
							}
						);
						continue;
					}
					++Metrics.CharacterControlRevocations;
				}
				PeerValue.ControlledCharacter = {};
				if (Desired.IsValid() && View->Knows(Desired) && PeerValue.MaterializedCharacters.contains(Desired)) {
					if (Authority->BindControl(Connection, Desired, std::max(CurrentTick, std::uint64_t{1}))) {
						PeerValue.ControlledCharacter = Desired;
						++Metrics.CharacterControlBindings;
					} else {
						PendingPeerFailures.try_emplace(
							Connection,
							DisconnectInfo{
								DisconnectReason::ResourceExhaustion,
								"Owner Character control binding failed",
							}
						);
					}
				}
			}
			Metrics.ServerGraphSynchronizationCpuNanoseconds += static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Started).count()
			);
		}

		bool SynchronizeClientGraph() {
			if (!Prediction || !Remotes || !PrimaryConnection) return false;
			std::set<ObjectId> DesiredCharacters;
			for (const auto Object : ClientCharacterObjects) {
				auto CharacterValue = std::dynamic_pointer_cast<KinematicCharacter>(Replica.Resolve(Object));
				if (CharacterValue && CharacterValue->GetRootPart()) DesiredCharacters.insert(Object);
			}
			for (const auto Object : ClientMaterializedCharacters)
				if (!DesiredCharacters.contains(Object) && !Prediction->MarkUnmaterialized(Object)) return false;
			for (const auto Object : DesiredCharacters)
				if (!ClientMaterializedCharacters.contains(Object)) {
					auto CharacterValue = std::dynamic_pointer_cast<KinematicCharacter>(Replica.Resolve(Object));
					if (!CharacterValue || !Prediction->MarkMaterialized(Object, CharacterValue)) return false;
				}
			ClientMaterializedCharacters = std::move(DesiredCharacters);
			for (const auto Object : ClientRemoteObjects) {
				auto RemoteValue = std::dynamic_pointer_cast<RemoteBase>(Replica.Resolve(Object));
				if (!RemoteValue) continue;
				if (!RemoteValue->BindRemoteManager(Remotes.get(), *PrimaryConnection, Object) ||
					!Remotes->PublishRemote(*PrimaryConnection, Object, true))
					return false;
			}
			return true;
		}

		void SynchronizeActionPresentation() {
			if (!ClientRuntimeAttached || !Prediction || !Runtime) return;
			for (auto Iterator = PresentedActions.begin(); Iterator != PresentedActions.end();) {
				if (ClientMaterializedCharacters.contains(Iterator->first)) {
					++Iterator;
					continue;
				}
				if (Iterator->second.Track) Iterator->second.Track->Stop();
				Iterator = PresentedActions.erase(Iterator);
			}
			for (const auto Object : ClientMaterializedCharacters) {
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
			if (Authority) (void)Authority->RemovePeer(Connection, std::max(CurrentTick, std::uint64_t{1}));
			if (Prediction) (void)Prediction->RemovePeer(Connection);
			if (Remotes) (void)Remotes->RemovePeer(Connection);
			if (Relevance) (void)Relevance->RemovePeer(Connection);
			if (Replication) (void)Replication->RemovePeer(Connection);
			(void)Scheduler.CancelConnection(Connection);
			if (Configuration.Role == GameSessionRole::Server && PeerValue.PlayerValue) {
				if (Runtime->Players->RemoveSessionPlayer(PeerValue.PlayerValue)) ++Metrics.PlayersRemoved;
			} else if (Configuration.Role == GameSessionRole::Client) {
				if (Runtime) Runtime->Players->ClearTrustedLocalPlayer();
				Replica.Reset();
				BaselineApplied = false;
				ClientCharacterObjects.clear();
				ClientMaterializedCharacters.clear();
				ClientRemoteObjects.clear();
				ClientKnownObjects.clear();
				PrimaryConnection.reset();
			}
		}

		void Step(std::uint64_t SimulationTick) {
			if (Status == GameSessionStatus::Created || Status == GameSessionStatus::Starting ||
				Status == GameSessionStatus::Closing || Status == GameSessionStatus::Closed ||
				Status == GameSessionStatus::Failed)
				return;
			if (!DrainFailures()) return;
			CurrentTick = SimulationTick;
			for (auto &[Connection, PeerValue] : Peers) {
				(void)Connection;
				PeerValue.SchedulerFlushedThisStep = false;
			}
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
				for (auto &[Connection, PeerValue] : Peers) {
					if (PeerValue.Phase == PeerPhase::TransportConnected) continue;
					auto CharacterValue = PeerValue.PlayerValue && PeerValue.PlayerValue->GetCharacter()
											  ? std::dynamic_pointer_cast<KinematicCharacter>(
													*PeerValue.PlayerValue->GetCharacter()
												)
											  : nullptr;
					if (!Relevance->SetOwnerCharacter(
							Connection, CharacterValue ? CharacterValue->GetObjectId() : ObjectId{}
						))
						PendingPeerFailures.try_emplace(
							Connection,
							DisconnectInfo{
								DisconnectReason::ResourceExhaustion,
								"Owner Character relevance update failed",
							}
						);
				}
				if (!DrainFailures()) return;
				if (!Relevance->Update(std::max(SimulationTick, std::uint64_t{1}))) {
					FailSession({
						DisconnectReason::ResourceExhaustion,
						"[Replication:Relevance] " + Relevance->GetFailure(),
					});
					return;
				}
				for (const auto &[Connection, PeerValue] : Peers) {
					if (PeerValue.Phase != PeerPhase::Ready) continue;
					if (!Authority->SetPeerPublicationFocus(Connection, Relevance->GetResolvedFocus(Connection)))
						PendingPeerFailures.try_emplace(
							Connection,
							DisconnectInfo{
								DisconnectReason::ResourceExhaustion,
								"[Character:Importance] trusted publication focus update failed",
							}
						);
				}
				if (!DrainFailures()) return;
				for (auto &[Connection, PeerValue] : Peers) {
					if (PeerValue.Phase == PeerPhase::TransportConnected) continue;
					auto Statistics = Scheduler.GetStatistics(Connection);
					if (!Statistics) {
						PendingPeerFailures.try_emplace(
							Connection,
							DisconnectInfo{
								DisconnectReason::ResourceExhaustion,
								"Scheduler ownership disappeared before structural publication",
							}
						);
						continue;
					}
					if (Statistics->QueuedMessages != 0) {
						auto Flushed = Scheduler.Flush(
							Connection, SchedulerTickBudget::FromNetworkLimits(PeerValue.Limits)
						);
						PeerValue.SchedulerFlushedThisStep = true;
						if (auto Failure = SchedulerFlushFailure(Flushed)) {
							PendingPeerFailures.try_emplace(Connection, std::move(*Failure));
							continue;
						}
					}
					bool StructuralSubmission = false;
					const auto *Selection = Relevance->GetSelection(Connection);
					if (!Selection) {
						PendingPeerFailures.try_emplace(
							Connection,
							DisconnectInfo{
								DisconnectReason::ResourceExhaustion,
								"Server relevance selection disappeared before structural publication",
							}
						);
						continue;
					}
					if (Relevance->WasSelectionEvaluated(Connection) || Replication->HasPendingRelevance(Connection)) {
						auto Transition = Replication->UpdateRelevance(Connection, *Selection);
						if (Transition.Succeeded() && Transition.Frame) {
							auto Queued = QueueStructuralFrame(*Transition.Frame, Connection, PeerValue.Limits);
							if (!Queued || !Queued->Accepted()) {
								PendingPeerFailures.try_emplace(
									Connection,
									Queued && Queued->TerminalDisconnect
										? *Queued->TerminalDisconnect
										: DisconnectInfo{
											  DisconnectReason::ResourceExhaustion,
											  "Structural relevance transition was rejected by the scheduler",
										  }
								);
								continue;
							}
							StructuralSubmission = true;
							if (!ApplyServerRemoteMaterialization(Connection, *Transition.Frame))
								PendingPeerFailures.try_emplace(
									Connection,
									DisconnectInfo{
										DisconnectReason::ResourceExhaustion,
										"Remote materialization could not follow structural commit",
									}
								);
							if (PendingPeerFailures.contains(Connection)) continue;
						} else if (Transition.Error != "No replication relevance changes are available") {
							PendingPeerFailures.try_emplace(
								Connection,
								DisconnectInfo{
									DisconnectReason::ResourceExhaustion,
									"[Replication:Relevance] " + Transition.Error,
								}
							);
							continue;
						}
					}
					auto Frame = Replication->ProduceIncremental(Connection);
					if (Frame.Succeeded() && Frame.Frame) {
						auto Queued = QueueStructuralFrame(*Frame.Frame, Connection, PeerValue.Limits);
						if (!Queued || !Queued->Accepted()) {
							PendingPeerFailures.try_emplace(
								Connection,
								Queued && Queued->TerminalDisconnect
									? *Queued->TerminalDisconnect
									: DisconnectInfo{
										  DisconnectReason::ResourceExhaustion,
										  "Structural delta was rejected by the scheduler",
									  }
							);
							continue;
						}
						StructuralSubmission = true;
						if (!ApplyServerRemoteMaterialization(Connection, *Frame.Frame))
							PendingPeerFailures.try_emplace(
								Connection,
								DisconnectInfo{
									DisconnectReason::ResourceExhaustion,
									"Remote materialization could not follow structural commit",
								}
							);
						if (PendingPeerFailures.contains(Connection)) continue;
					} else if (Frame.Error != "No replication changes are available" &&
							   Frame.Error != "No relevant replication changes are available") {
						PendingPeerFailures.try_emplace(
							Connection,
							DisconnectInfo{
								DisconnectReason::ResourceExhaustion,
								"[Replication:Relevance] " + Frame.Error,
							}
						);
					}
					if (StructuralSubmission) {
						auto Flushed = Scheduler.Flush(
							Connection, SchedulerTickBudget::FromNetworkLimits(PeerValue.Limits)
						);
						PeerValue.SchedulerFlushedThisStep = true;
						if (auto Failure = SchedulerFlushFailure(Flushed))
							PendingPeerFailures.try_emplace(Connection, std::move(*Failure));
					}
				}
				if (!DrainFailures()) return;
				SynchronizeServerGraph();
				if (!DrainFailures()) return;
				Authority->Step(*Runtime->WorldRoot, std::max(SimulationTick, std::uint64_t{1}));
				if (!DrainFailures()) return;
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
			if (!DrainFailures()) return;
			std::vector<std::pair<ConnectionId, DisconnectInfo>> TerminalConnections;
			for (const auto &[Connection, PeerValue] : Peers) {
				if (Configuration.Role == GameSessionRole::Server && PeerValue.Phase == PeerPhase::TransportConnected)
					continue;
				if (PeerValue.SchedulerFlushedThisStep) continue;
				auto Flushed = Scheduler.Flush(
					Connection,
					SchedulerTickBudget::FromNetworkLimits(
						PeerValue.Limits.IsValid() ? PeerValue.Limits : Configuration.Limits
					)
				);
				if (auto Failure = SchedulerFlushFailure(Flushed))
					TerminalConnections.emplace_back(Connection, std::move(*Failure));
			}
			for (auto &[Connection, Information] : TerminalConnections)
				FailPeer(Connection, Information.Reason, std::move(Information.Diagnostic));
		}

		bool AttachClientRuntime(Engine &RuntimeValue) {
			if (Configuration.Role != GameSessionRole::Client || ClientRuntimeAttached ||
				Status != GameSessionStatus::Accepted || !PrimaryConnection ||
				RuntimeValue.DataModel != GetClientDataModel())
				return false;
			Runtime = &RuntimeValue;
			try {
				InitializeClientManagers();
			} catch (...) {
				FailSession({
					DisconnectReason::ResourceExhaustion,
					"Client runtime bootstrap acquisition failed",
				});
				return false;
			}
			auto &PeerValue = Peers.at(*PrimaryConnection);
			auto Ready = InjectFailure(detail::GameSessionFailurePoint::ClientReadySerialization)
							 ? SerializationResult<std::vector<std::byte>>(
								   SerializationFailure(
									   SerializationErrorCode::InternalFailure,
									   "Injected ClientReady serialization failure"
								   )
							   )
							 : EncodeGameSessionMessage(GameSessionClientReady{
								   PeerValue.SessionEpoch,
								   PeerValue.Replication,
								   PeerValue.PlayerObject,
							   });
			auto ReadyIntent = Ready ? MakeNetworkMessageIntent(
																				*PrimaryConnection,
																				DeliveryMode::ReliableOrdered,
																				TrafficClass::Control,
																				{},
																				std::move(*Ready),
																				PeerValue.Limits
																			)
																		  : std::nullopt;
			const bool ReadyAccepted = ReadyIntent &&
				!InjectFailure(detail::GameSessionFailurePoint::ClientReadySchedulerAdmission) &&
				Scheduler.Submit(std::move(*ReadyIntent)).Accepted();
			if (!ReadyAccepted) {
				FailSession({
					DisconnectReason::ResourceExhaustion,
					"Client readiness was rejected by the scheduler",
				});
				return false;
			}
			ClientRuntimeAttached = true;
			PeerValue.Phase = PeerPhase::Ready;
			Status = GameSessionStatus::Ready;
			++Metrics.ReadyPeers;
			return true;
		}

		std::shared_ptr<DataModel> GetClientDataModel() const {
			return Configuration.Role == GameSessionRole::Client &&
						   (Status == GameSessionStatus::Accepted || Status == GameSessionStatus::Ready)
					   ? std::dynamic_pointer_cast<DataModel>(Replica.GetReplicaRoot())
					   : nullptr;
		}
	};

	GameSession::GameSession(
		std::shared_ptr<IGameTransport> Transport, GameSessionConfiguration Configuration, Engine *ServerRuntime
	)
		: State(std::make_unique<Implementation>(std::move(Transport), std::move(Configuration), ServerRuntime)) {}

	GameSession::~GameSession() = default;

	void detail::GameSessionTestAccess::SetFailurePoint(
		GameSession &Session, detail::GameSessionFailurePoint Point
	) {
		Session.State->FailurePoint = Point;
	}

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
		auto Result = State->Metrics;
		if (State->Relevance) {
			const auto RelevanceMetrics = State->Relevance->GetMetrics();
			Result.RelevantObjects = RelevanceMetrics.DesiredObjects;
			Result.RelevanceEnters = RelevanceMetrics.RelevanceEnters;
			Result.RelevanceLeaves = RelevanceMetrics.RelevanceLeaves;
			Result.RelevanceQueries = RelevanceMetrics.SpatialQueries;
			Result.RelevanceRegionsVisited = RelevanceMetrics.QueryRegions;
			Result.RelevanceCandidates = RelevanceMetrics.CandidateObjects;
			Result.RelevanceCandidateMembershipVisits = RelevanceMetrics.CandidateMembershipVisits;
			Result.RelevanceCandidateDedupHits = RelevanceMetrics.CandidateDedupHits;
			Result.SpatialRegionCount = RelevanceMetrics.SpatialRegions;
			Result.SpatialMembershipCount = RelevanceMetrics.SpatialMemberships;
			Result.LargeSpatialObjectCount = RelevanceMetrics.LargeSpatialObjects;
			Result.SpatialMembershipMoves = RelevanceMetrics.SpatialMoves;
			Result.SpatialSameRegionUpdates = RelevanceMetrics.SameRegionUpdates;
			Result.SpatialRegionBucketsRemoved = RelevanceMetrics.RegionBucketsRemoved;
			Result.SpatialPeakRegionOccupancy = RelevanceMetrics.PeakRegionOccupancy;
			Result.SpatialQueryLimitFailures = RelevanceMetrics.SpatialQueryLimitFailures;
			Result.SpatialCandidateLimitFailures = RelevanceMetrics.SpatialCandidateLimitFailures;
			Result.RelevanceCpuNanoseconds = RelevanceMetrics.UpdateCpuNanoseconds;
		}
		if (State->Replication) {
			const auto &ReplicationMetrics = State->Replication->GetMetrics();
			Result.MaterializationBacklog = ReplicationMetrics.MaterializationBacklog;
			Result.MaterializationTransitions = ReplicationMetrics.RelevanceTransitions;
			Result.MaterializationCpuNanoseconds = ReplicationMetrics.RelevanceTransitionCpuNanoseconds;
		}
		if (State->Authority) {
			const auto CharacterMetrics = State->Authority->GetMetrics();
			Result.CharacterImportanceEvaluations = CharacterMetrics.ImportanceEvaluations;
			Result.CharacterImportanceTierTransitions = CharacterMetrics.ImportanceTierTransitions;
			Result.CharacterTemporaryPromotions = CharacterMetrics.TemporaryPromotions;
			Result.CharacterForcedSemanticPublications = CharacterMetrics.ForcedSemanticPublications;
			Result.CharacterFullRateStates = CharacterMetrics.FullRateStatesSent;
			Result.CharacterReducedRateStates = CharacterMetrics.ReducedRateStatesSent;
			Result.CharacterLowRateStates = CharacterMetrics.LowRateStatesSent;
			Result.CharacterStateBytes = CharacterMetrics.FullRateStateBytes + CharacterMetrics.ReducedRateStateBytes +
										 CharacterMetrics.LowRateStateBytes;
			Result.CharacterMaximumStateAgeTicks = CharacterMetrics.MaximumStateAgeTicks;
			Result.CharacterImportanceCpuNanoseconds = CharacterMetrics.ImportanceEvaluationCpuNanoseconds;
			Result.CharacterDueSetCpuNanoseconds = CharacterMetrics.DueSetCpuNanoseconds;
			Result.CharacterPublicationBudgetConsumed = CharacterMetrics.PublicationBudgetConsumed;
			Result.CharacterPublicationStatesSelected = CharacterMetrics.PublicationStatesSelected;
			Result.CharacterPublicationStatesAccepted = CharacterMetrics.PublicationStatesAccepted;
			Result.CharacterPublicationStatesOffered = CharacterMetrics.PublicationOfferedStates;
			Result.CharacterPublicationStatesDeferred = CharacterMetrics.PublicationStatesDeferred;
			Result.CharacterPublicationOverdueRelationships = CharacterMetrics.PublicationOverdueRelationships;
			Result.CharacterPublicationDeadlineMisses = CharacterMetrics.PublicationDeadlineMisses;
			Result.CharacterPublicationLatencySamples = CharacterMetrics.PublicationLatencySamples;
			Result.CharacterPublicationLatencyTicks = CharacterMetrics.PublicationLatencyTicks;
			Result.CharacterMaximumPublicationLatencyTicks = CharacterMetrics.MaximumPublicationLatencyTicks;
			Result.CharacterMaximumOwnerStateAgeTicks = CharacterMetrics.MaximumPublicationOwnerAgeTicks;
			Result.CharacterPublicationOwnerDeferrals = CharacterMetrics.PublicationOwnerDeferrals;
			Result.CharacterPublicationBudgetExhaustions = CharacterMetrics.PublicationGlobalBudgetExhaustions;
			Result.CharacterPublicationSchedulerRejections = CharacterMetrics.PublicationSchedulerRejections;
			Result.CharacterPublicationSelectionCpuNanoseconds = CharacterMetrics.PublicationSelectionCpuNanoseconds;
			Result.CharacterPublicationActiveRelationships = CharacterMetrics.PublicationActiveRelationships;
			Result.CharacterPublicationCurrentDueRelationships = CharacterMetrics.PublicationCurrentDueRelationships;
			Result.CharacterPublicationCurrentOverdueRelationships =
				CharacterMetrics.PublicationCurrentOverdueRelationships;
			Result.CharacterMaximumCurrentPublicationAgeTicks = CharacterMetrics.MaximumCurrentPublicationAgeTicks;
		}
		for (const auto &[Connection, PeerValue] : State->Peers) {
			if (const auto *View = State->Replication ? State->Replication->GetView(Connection) : nullptr)
				Result.MaterializedObjects += View->KnownObjects.size();
			Result.MaterializedCharacters += PeerValue.MaterializedCharacters.size();
		}
		return Result;
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
	bool GameSession::SetTrustedReplicationFocus(ConnectionId Connection, std::span<const glm::vec3> FocusPoints) {
		return State->Configuration.Role == GameSessionRole::Server && State->Relevance &&
			   State->Relevance->SetTrustedFocus(Connection, FocusPoints);
	}
}
