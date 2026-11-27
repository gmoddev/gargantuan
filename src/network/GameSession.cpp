#include "gargantuan/network/GameSession.hpp"
#include "../runtime/RuntimeWorkDiagnostics.hpp"
#include "../runtime/PublicationLatencyDiagnostics.hpp"
#include "../terrain/EditCoordinator.hpp"
#include "SessionSendAllowance.hpp"
#include "ReliableByteAdmission.hpp"
#include "PooledReliableServiceFeedback.hpp"
#include "PooledServiceDiagnostics.hpp"
#include "OrdinaryServicePolicy.hpp"

#include "GameSessionTestAccess.hpp"
#include "BlockGridPeer.hpp"
#include "gargantuan/classes/BlockGrid.hpp"
#include "gargantuan/classes/SpatialRegion.hpp"

#include "gargantuan/Engine.hpp"
#include "gargantuan/Log.hpp"
#include "gargantuan/animation/AnimationTrack.hpp"
#include "gargantuan/assets/AssetTypes.hpp"
#include "gargantuan/classes/Animator.hpp"
#include "gargantuan/classes/Character.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/KinematicCharacter.hpp"
#include "gargantuan/classes/LuaSourceContainer.hpp"
#include "gargantuan/classes/Player.hpp"
#include "gargantuan/classes/PlayerGui.hpp"
#include "gargantuan/classes/RemoteBase.hpp"
#include "gargantuan/classes/ScreenGui.hpp"
#include "gargantuan/classes/Terrain.hpp"
#include "gargantuan/classes/WorldRoot.hpp"
#include "gargantuan/gui/PlayerGuiScope.hpp"
#include "gargantuan/network/CharacterNetwork.hpp"
#include "gargantuan/network/GameSessionProtocol.hpp"
#include "gargantuan/network/GameplayPose.hpp"
#include "gargantuan/network/RemoteManager.hpp"
#include "gargantuan/network/ReplicaApplier.hpp"
#include "gargantuan/network/ReplicationCoordinator.hpp"
#include "gargantuan/network/ReplicationTransport.hpp"
#include "gargantuan/network/Scheduler.hpp"
#include "gargantuan/network/TerrainSession.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"
#include "gargantuan/runtime/ServicePolicy.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"
#include "gargantuan/runtime/JobSystem.hpp"
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
		constexpr std::size_t MaximumPossessionPosesPerPeer = 16;
		constexpr std::uint64_t PossessionPoseIntervalTicks = 3;

		enum class PeerPhase : std::uint8_t { TransportConnected, BootstrapPending, Accepted, Ready };

		void SaturatingAdd(std::uint64_t &Value, std::uint64_t Amount) {
			Value += std::min(Amount, std::numeric_limits<std::uint64_t>::max() - Value);
		}

		std::uint64_t ServiceTime() {
			return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count());
		}

		std::uint32_t FrameMagic(std::span<const std::byte> Bytes) {
			if (Bytes.size() < 4) return 0;
			return std::to_integer<std::uint32_t>(Bytes[0]) | (std::to_integer<std::uint32_t>(Bytes[1]) << 8) |
				   (std::to_integer<std::uint32_t>(Bytes[2]) << 16) | (std::to_integer<std::uint32_t>(Bytes[3]) << 24);
		}

		StateChannelId CharacterStateChannel(ObjectId Object) {
			return StateChannelId((static_cast<std::uint64_t>(Object.Generation) << 32) | Object.Slot);
		}

		std::shared_ptr<Instance> GenericPossession(const Player &PlayerValue) {
			auto Entity = PlayerValue.GetPossessedEntity().value_or(nullptr);
			return Entity && Entity != PlayerValue.GetCharacter().value_or(nullptr) ? Entity : nullptr;
		}

		std::shared_ptr<KinematicCharacter> LegacyCharacter(const std::shared_ptr<Player> &PlayerValue) {
			if (!PlayerValue || GenericPossession(*PlayerValue)) return nullptr;
			return std::dynamic_pointer_cast<KinematicCharacter>(PlayerValue->GetCharacter().value_or(nullptr));
		}

		bool IsIndependentPosePart(const std::shared_ptr<BasePart> &Part) {
			return Part && !Part->GetDestroyed() && !Part->IsDestroying() && Part->GetAnchored() &&
				!Part->FindFirstAncestorWhichIsA("Character");
		}

		std::shared_ptr<Instance> TerrainFocusActor(const std::shared_ptr<Player> &PlayerValue) {
			if (!PlayerValue) return {};
			if (auto Entity = GenericPossession(*PlayerValue)) return Entity;
			return PlayerValue->GetCharacter().value_or(nullptr);
		}

		struct TerrainFocusBounds {
			glm::vec3 Position{0.0f};
			glm::vec3 Extents{0.0f};
		};

		TerrainFocusBounds GetTerrainFocusBounds(const std::shared_ptr<Instance> &Actor) {
			if (const auto Controller = std::dynamic_pointer_cast<KinematicCharacter>(Actor))
				return {Controller->GetPosition(), {Controller->GetCapsuleRadius(), Controller->GetCapsuleHeight() * 0.5f,
					Controller->GetCapsuleRadius()}};
			if (const auto Part = std::dynamic_pointer_cast<BasePart>(Actor))
				return {Part->GetCFrame().Position, glm::vec3(glm::length(Part->GetSize()) * 0.5f)};
			return {};
		}

		bool ContainsTerrainBounds(terrain::GridSpec Grid, terrain::ChunkKey Center, glm::vec3 Position, glm::vec3 Extents) {
			return ContainsTerrainMotion({.Grid = Grid, .Center = Center},
				{Position.x, Position.y, Position.z}, {Extents.x, Extents.y, Extents.z});
		}

		ObjectId OwnerSpatialRoot(const std::shared_ptr<Player> &PlayerValue, const ReplicationRelevance &Relevance) {
			if (!PlayerValue) return {};
			if (auto Entity = GenericPossession(*PlayerValue)) return Relevance.GetSpatialRoot(Entity->GetObjectId());
			if (auto CharacterValue = LegacyCharacter(PlayerValue)) return CharacterValue->GetObjectId();
			return {};
		}

		ObjectId ReplicationObject(const ReplicationIntent &Intent) {
			return GetReplicationObject(Intent);
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
			   StructuralReplication.IsValid() &&
			   (!ResolveInitialSpatialAudience || (SpatialContent && Role == GameSessionRole::Server)) &&
			   (!SpatialContent || (IsLoopbackTransportEndpoint(Endpoint) && !ReliableService &&
				Limits.MaximumReliableMessageBytes >= MaximumGridStreamFragment + 128)) &&
			   (!ReliableService || (Role == GameSessionRole::Server && ReliableService->IsValid() &&
				ReliableService->MaximumConnections <= MaximumGameSessionPeers)) &&
			   (!DeclareNormalOrdinaryService || Role == GameSessionRole::Client) &&
			   (!TerrainBootstrap || (IsLoopbackTransportEndpoint(Endpoint) && !ReliableService && RuntimeDistributionIdentity.empty() &&
				TerrainBootstrap->Binding.IsValid() && TerrainBootstrap->Directory.Project.IsValid() &&
				TerrainBootstrap->Directory.Grid.IsValid() &&
				Limits.MaximumReliableMessageBytes >= TerrainFragmentHeaderBytes + MaximumTerrainFragmentBytes &&
				(Role == GameSessionRole::Server ? TerrainBootstrap->Source.size() == TerrainBootstrap->Directory.Chunks.size()
												 : TerrainBootstrap->Source.empty()))) &&
			   (RuntimeDistributionIdentity.empty() || IsGameSessionDistributionIdentity(RuntimeDistributionIdentity)) &&
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

		struct TerrainSendWork {
			std::shared_ptr<JobGroup> Completion = std::make_shared<JobGroup>();
			std::stop_source Stop;
			std::optional<TerrainSessionWindow> Window;
		};

		struct TerrainReceiveWork {
			std::shared_ptr<JobGroup> Completion = std::make_shared<JobGroup>();
			std::stop_source Stop;
			TerrainSessionWindow Window;
			std::unique_ptr<terrain::Field> Field;
			std::vector<terrain::ChunkPublicationCandidate> Chunks;
			bool Installed{};
		};

		using TerrainVersions = std::array<terrain::ChunkVersion, TerrainWindowChunkCount>;

		struct TerrainMotionRegion {
			terrain::ChunkKey Center;
			terrain::Revision Revision;
		};

		struct TerrainTransfer {
			TerrainWindowOffer Offer;
			std::optional<TerrainSessionWindow> Window;
			std::shared_ptr<TerrainSendWork> Sending;
			TerrainVersions Versions;
			std::size_t Cursor{};
			std::uint64_t StartedTick{};
			bool Offered{};
		};

		struct Peer {
			struct PoseBinding {
				std::uint64_t Epoch = 0;
				std::uint64_t LastSentTick = 0;
				std::uint64_t FirstDueTick = 0;
			};
			PeerPhase Phase = PeerPhase::TransportConnected;
			std::uint64_t ConnectedTick = 0;
			std::uint64_t Nonce = 0;
			std::uint64_t SessionEpoch = 0;
			ReplicationEpoch Replication;
			NetworkLimits Limits;
			ObjectId PlayerObject;
			ObjectId AuthorizedSpatialRegion;
			std::unique_ptr<BlockGridPeer> Grids;
			std::uint32_t PlayerId = 0;
			std::optional<TerrainSessionWindow> TerrainWindow;
			TerrainWindowIdentity TerrainIdentity;
			TerrainVersions KnownTerrainVersions;
			terrain::Revision CheckedTerrainRevision{};
			std::optional<TerrainTransfer> TerrainPending;
			bool TerrainAdmitted = false;
			std::optional<TerrainMotionRegion> ReleasedTerrain;
			std::optional<std::uint64_t> TerrainReleaseStartedTick;
			std::shared_ptr<Player> PlayerValue;
			ObjectId ControlledCharacter;
			ObjectId ControlledPossession;
			std::uint64_t ControlledPossessionRevision = 0;
			std::uint64_t AcceptedStructuralSequence = 0;
			std::map<ObjectId, PoseBinding> PossessionPoses;
			std::set<ObjectId> MaterializedCharacters;
			std::set<ObjectId> MaterializedRemotes;
			std::set<ObjectId> RemoteMaterializedObjects;
			detail::SessionSendAllowance SendAllowance;
			detail::PooledReliableServiceFeedback ReliableFeedback;
			std::shared_ptr<detail::OrdinaryServicePolicy> OrdinaryPolicy;
			// Bounded evidence for the one ACK-gated accepted grant. Populated only
			// with internal evidence enabled; never used for admission.
			std::uint64_t CausalGrantToken = 0, CausalGrantSequence = 0, CausalGrantBytes = 0, CausalAcceptedBefore = 0;
			std::array<std::uint64_t, 2> CausalGrantFingerprint{};
			std::uint64_t CausalLastFirstSent = 0, CausalLastAcked = 0;
			bool CausalDeliveryObserved = false;
			bool PreferPendingRelevance = true;
			bool StructuralSubmittedThisStep = false;
			bool ByteDeferredThisStep = false;
			std::size_t StructuralTransitionsConsumedThisStep = 0;
			std::size_t JournalRecordsConsumedThisStep = 0;
			std::optional<std::uint64_t> JournalPendingSinceTick;
		};

		std::shared_ptr<IGameTransport> Transport;
		GameSessionConfiguration Configuration;
		JobSystem TerrainJobs{1, JobSystem::StartPolicy::OnFirstSubmission};
		std::array<std::shared_ptr<TerrainSendWork>, MaximumTerrainBootstrapPeers> TerrainSendSlots;
		std::optional<ConnectionId> TerrainFairnessCursor;
		std::vector<std::uint8_t> TerrainReceiveBuffer;
		std::unique_ptr<terrain::Field> ReceivedTerrain;
		std::shared_ptr<TerrainReceiveWork> ClientTerrainWork;
		std::shared_ptr<const WorldRoot::TerrainMotionGuard> TerrainMotionAdmission;
		std::shared_ptr<const DataModel::DefaultNetworkSpatialProfile> NetworkSpatialProfile;
		NetworkScheduler Scheduler;
		GameplayInputServer ServerPossessionInput{Scheduler};
		GameplayInputClient ClientPossessionInput{Scheduler};
		GameplayPosePresentation ClientPosePresentation;
		struct ActivePose {
			ObjectId Player;
			std::uint64_t Revision;
			std::uint64_t MovementTick;
		};
		struct ClientPoseBinding {
			std::uint64_t RequiredStructuralSequence;
			std::weak_ptr<BasePart> ReplicaPart;
			std::uint64_t AppliedTick = 0;
			std::uint64_t ResolvedAtSequence = 0;
			ReplicationEpoch ResolvedAtEpoch;
		};
		std::map<ObjectId, ActivePose> ActivePossessionPoses;
		std::map<ObjectId, ClientPoseBinding> ClientPossessionPoses;
		std::uint64_t NextGameplayPoseEpoch = 1;
		std::optional<detail::ReliableByteAdmission> ByteAdmission;
		Engine *Runtime = nullptr;
		GameSessionStatus Status = GameSessionStatus::Created;
		std::string Failure;
		GameSessionMetrics Metrics;
		std::uint64_t CurrentTick = 0;
		std::uint64_t TerrainTick = 0;
		std::uint64_t NextSessionEpoch = 1;
		std::uint64_t NextReplicationEpoch = 1;
		std::map<ConnectionId, Peer> Peers;
		std::map<ObjectId, ConnectionId> ConnectionsByPlayer;
		std::optional<ConnectionId> PrimaryConnection;
		std::optional<ConnectionId> StructuralFairnessCursor;
		std::optional<ConnectionId> StructuralJournalFairnessCursor;
		std::vector<ConnectionId> StructuralConnectionsScratch;

		std::unique_ptr<ReplicationCoordinator> Replication;
		std::unique_ptr<ReplicationRelevance> Relevance;
		ReplicaApplier Replica;
		std::unique_ptr<RemoteManager> Remotes;
		RemoteMetrics FinalRemoteMetrics;
		std::unique_ptr<AuthoritativeCharacterNetwork> Authority;
		std::unique_ptr<PredictedCharacterNetwork> Prediction;
		std::set<ObjectId> ServerCharacters;
		std::set<ObjectId> ServerRemoteObjects;
		std::set<ObjectId> ClientCharacterObjects;
		std::set<ObjectId> ClientRemoteObjects;
		std::set<ObjectId> ClientMaterializedCharacters;
		std::set<ObjectId> ClientKnownObjects;
		std::set<ObjectId> PendingClientGuiCode;
		ObjectId ClientGuiCharacter;
		SignalConnection::Pointer ServerDescendantAdded;
		SignalConnection::Pointer ServerDescendantRemoved;
		std::map<ObjectId, PresentedAction> PresentedActions;
		std::optional<CharacterControlService::RuntimeAttachment> ControlAttachment;
		std::map<ConnectionId, DisconnectInfo> PendingPeerFailures;
		std::optional<DisconnectInfo> PendingSessionFailure;
		bool BaselineApplied = false;
		bool ClientRuntimeAttached = false;
		std::shared_ptr<detail::OrdinaryServiceBudget> OrdinarySource;
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
			Configuration.StructuralReplication.ScopeSpatialDomains = Configuration.SpatialContent;
			Configuration.Relevance.ScopeSpatialDomains = Configuration.SpatialContent;
			if (Configuration.Role == GameSessionRole::Server && !Runtime)
				throw std::invalid_argument("[Network:Session] Server game session requires an Engine runtime");
			if (Configuration.Role == GameSessionRole::Client && Runtime)
				throw std::invalid_argument(
					"[Network:Session] Client Engine attaches after trusted replication bootstrap"
				);
			if (Configuration.ReliableService) {
				if (Configuration.ReliableService->IsPooled() && !detail::ReliableServiceFeedbackAccess::Enable(*Transport))
					throw std::invalid_argument("[Network:Session] POOLED_SERVICE requires native attributed retirement and terminal feedback");
				ByteAdmission.emplace(*Configuration.ReliableService);
				Configuration.Limits.MaximumReliableMessageBytes = std::min<std::size_t>(
					Configuration.Limits.MaximumReliableMessageBytes,
					MaximumReliableServiceGroupBytes - ReliableServiceEnvelopeBytes);
			}
			if ((Configuration.ReliableService && Configuration.ReliableService->IsPooled()) ||
				Configuration.DeclareNormalOrdinaryService)
				OrdinarySource = std::make_shared<detail::OrdinaryServiceBudget>();
		}
		bool ArmOrdinaryPolicy(ConnectionId Connection, Peer &PeerValue) {
			if (!OrdinarySource || PeerValue.OrdinaryPolicy) return true;
			auto Policy = std::make_shared<detail::OrdinaryServicePolicy>(OrdinarySource);
			if (!detail::ReliableServiceFeedbackAccess::DeclareOrdinaryPolicy(Scheduler, Connection, Policy) ||
				!detail::ReliableServiceFeedbackAccess::DeclareOrdinaryPolicy(*Transport, Connection, Policy) ||
				!Policy->SetReady(true)) return false;
			PeerValue.OrdinaryPolicy = std::move(Policy);
			return true;
		}

		~Implementation() {
			Stop();
		}

		bool SpatialValidationRequested = false;
		bool SpatialValidationPassed = false;
		std::optional<std::chrono::steady_clock::time_point> LastClientCharacterService;
		std::optional<std::chrono::steady_clock::time_point> LastClientRemoteService;

		void RecordClientService(std::uint64_t &Count, std::uint64_t &MaximumGap,
			std::optional<std::chrono::steady_clock::time_point> &Previous) {
			const auto Now = std::chrono::steady_clock::now();
			if (Previous) MaximumGap = std::max(MaximumGap, static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(Now - *Previous).count()));
			if (Count != std::numeric_limits<std::uint64_t>::max()) ++Count;
			Previous = Now;
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

		std::shared_ptr<Player> ResolveRemotePlayer(const RemotePeerContext &Identity) const {
			if (Configuration.Role != GameSessionRole::Server || Status != GameSessionStatus::Listening ||
				!Runtime || PendingSessionFailure || PendingPeerFailures.contains(Identity.Connection))
				return nullptr;
			const auto Found = Peers.find(Identity.Connection);
			if (Found == Peers.end() || Found->second.Phase != PeerPhase::Ready ||
				Found->second.Replication != Identity.Epoch)
				return nullptr;
			const auto &PlayerValue = Found->second.PlayerValue;
			if (!PlayerValue || PlayerValue->GetDestroyed() || PlayerValue->IsDestroying() ||
				PlayerValue->GetObjectId() != Found->second.PlayerObject ||
				PlayerValue->GetParent().value_or(nullptr) != Runtime->Players)
				return nullptr;
			const auto Reverse = ConnectionsByPlayer.find(Found->second.PlayerObject);
			return Reverse != ConnectionsByPlayer.end() && Reverse->second == Identity.Connection ? PlayerValue : nullptr;
		}

		std::optional<RemotePeerContext> ResolveRemotePeer(const Player &PlayerValue) const {
			if (!Runtime || PlayerValue.GetDestroyed() || PlayerValue.IsDestroying() ||
				PlayerValue.GetParent().value_or(nullptr) != Runtime->Players) return std::nullopt;
			const auto Binding = ConnectionsByPlayer.find(PlayerValue.GetObjectId());
			if (Binding == ConnectionsByPlayer.end()) return std::nullopt;
			const auto Found = Peers.find(Binding->second);
			if (Found == Peers.end() || Found->second.PlayerValue.get() != &PlayerValue) return std::nullopt;
			const RemotePeerContext Identity{Binding->second, Found->second.Replication};
			return ResolveRemotePlayer(Identity) ? std::optional{Identity} : std::nullopt;
		}

		void InitializeServerManagers() {
			Replication = std::make_unique<ReplicationCoordinator>(
				Runtime->DataModel,
				[this](ObjectId Object) { return !Runtime->Players->IsRuntimeModule(Object); },
				true,
				Configuration.StructuralReplication
			);
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
					return Configuration.SpatialContent ? Replication->CanDeliverSpatialContent(Connection,Object) : View && View->Knows(Object);
				},
				[](ObjectId Object) { return ObjectRegistry::Get().Lookup(Object); }
			);
			Remotes->SetPlayerResolvers(
				[this](const RemotePeerContext &Identity) { return ResolveRemotePlayer(Identity); },
				[this](const Player &PlayerValue) { return ResolveRemotePeer(PlayerValue); }
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
				{},
				{},
				[this](const Player &PlayerValue, std::uint64_t Tick) {
					return ReadPossessionInput(PlayerValue, Tick);
				},
				[this](const Player &PlayerValue, std::uint64_t Tick, const CFrame &Transform) {
					return MovePossessedPart(PlayerValue, Tick, Transform);
				}
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
			Remotes->SetOutgoingReferenceMapper([this](const Instance &Value) { return Replica.GetSourceObject(Value); });
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
					if (!IsTerrainMotionReleased(Connection)) return false;
					const auto PlayerValue = Runtime->Players->GetLocalPlayer().value_or(nullptr);
					if (PlayerValue && GenericPossession(*PlayerValue)) return false;
					return Prediction->SubmitInput(
						Connection, *Runtime->WorldRoot, Tick, DeltaSeconds, Move, Facing, Jump
					);
				},
				[this, Connection](std::uint32_t Action, std::uint64_t Tick) {
					if (!IsTerrainMotionReleased(Connection)) return false;
					const auto PlayerValue = Runtime->Players->GetLocalPlayer().value_or(nullptr);
					if (PlayerValue && GenericPossession(*PlayerValue)) return false;
					return Prediction->RequestAction(Connection, Action, Tick);
				},
				[this](const CharacterActionDefinition &Definition, bool Predictable) {
					return Prediction->RegisterAction(Definition, Predictable);
				},
				[this](bool Enabled) { Prediction->SetPredictionEnabled(Enabled); },
				[this, Connection](std::uint64_t Tick, glm::vec2 Move, float Facing, bool Jump) {
					const auto PeerIterator = Peers.find(Connection);
					const auto PlayerValue = Runtime->Players->GetLocalPlayer().value_or(nullptr);
					const auto Entity = PlayerValue ? GenericPossession(*PlayerValue) : nullptr;
					const auto Source = ClientPossessionInput.GetPossessedEntity();
					return PeerIterator != Peers.end() && PeerIterator->second.Phase == PeerPhase::Ready &&
						IsTerrainMotionReleased(Connection) &&
						Entity && Source.IsValid() && Replica.Resolve(Source) == Entity &&
						ClientPossessionInput.Submit(Connection, Source, Tick, Move, Facing, Jump,
							PeerIterator->second.Limits);
				}
			);
			if (!ControlAttachment)
				throw std::runtime_error("[Network:Session] Client Character-control attachment failed");
			if (InjectFailure(detail::GameSessionFailurePoint::RuntimeCallbackAttachment))
				throw std::runtime_error("[Network:Session] Injected Character-control attachment failure");
			if (!SynchronizeClientGraph() || InjectFailure(detail::GameSessionFailurePoint::ClientGraphSynchronization))
				throw std::runtime_error("[Network:Session] Client gameplay graph synchronization failed");
		}

		bool ApplyServerRemoteMaterialization(ConnectionId Connection, const ReplicationFrame &Frame) {
			runtime_detail::WorkScope Work(runtime_detail::WorkPhase::RemoteMaterialization);
			auto Peer = Peers.find(Connection);
			if (Peer == Peers.end() || Peer->second.Phase != PeerPhase::Ready || !Remotes) return true;
			for (const auto &Operation : Frame.Operations)
				if (IsPublishReplication(Operation.Intent)) {
					const auto Object = ReplicationObject(Operation.Intent);
					if (!Peer->second.RemoteMaterializedObjects.contains(Object)) {
						if (!Remotes->MarkMaterialized(Connection, Object)) return false;
						Peer->second.RemoteMaterializedObjects.insert(Object);
					}
				}
			const auto *View = Replication ? Replication->GetView(Connection) : nullptr;
			if (!View) return false;
			for (auto Iterator = Peer->second.RemoteMaterializedObjects.begin();
				 Iterator != Peer->second.RemoteMaterializedObjects.end();) {
				runtime_detail::CountWork(runtime_detail::WorkCounter::RemoteRegistryExamined);
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
					const auto Owner = Runtime && Runtime->Workspace ? Runtime->Workspace->GetTerrain() : nullptr;
					if (Runtime && Runtime->DataModel)
					NetworkSpatialProfile = Runtime->DataModel->AcquireDefaultNetworkSpatialProfile(Configuration.SpatialContent);
					if ((Configuration.TerrainBootstrap && (!Owner || !Runtime->Workspace->IsTerrainCollisionReady())) ||
						(!Configuration.TerrainBootstrap && Owner && Owner->GetField().GetStats().MaterializedPages))
						throw std::runtime_error("[Terrain:Network] Server terrain lacks collision or a delivery binding");
					if (Configuration.TerrainBootstrap) AttachTerrainMotionAdmission(*Runtime->WorldRoot);
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
				FailSession(Result.TerminalDisconnect.value_or(
					DisconnectInfo{
						DisconnectReason::TransportFailure,
						"Transport endpoint failed to start",
					}
				));
			}
			return Result;
		}

		void ReleaseSessionResources() {
			NetworkSpatialProfile.reset();
			TerrainMotionAdmission.reset();
			if (ClientTerrainWork) ClientTerrainWork->Stop.request_stop();
			ClientTerrainWork.reset();
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
			for (auto &Slot : TerrainSendSlots)
				if (Slot) Slot->Stop.request_stop();
			if (Remotes) FinalRemoteMetrics = Remotes->GetMetrics();
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
			PendingClientGuiCode.clear();
			ClientGuiCharacter = {};
			PrimaryConnection.reset();
			ConnectionsByPlayer.clear();
			ClientPossessionInput.Reset();
			ClientPosePresentation.Clear();
			ActivePossessionPoses.clear();
			ClientPossessionPoses.clear();
			PendingPeerFailures.clear();
			PendingSessionFailure.reset();
			BaselineApplied = false;
			TerrainReceiveBuffer = std::vector<std::uint8_t>{};
			ReceivedTerrain.reset();
			ClientRuntimeAttached = false;
			Runtime = nullptr;
		}

		void RecordPooledLifecycle(detail::PooledLifecyclePoint Point, ConnectionId Connection, DisconnectReason Reason) {
			if (const auto *Sink = detail::ActivePooledService; Sink && Sink->Lifecycle)
				Sink->Lifecycle(Sink->Context, {Point, Connection, ServiceTime(),
					static_cast<std::uint8_t>(Configuration.Role), static_cast<std::uint8_t>(Status),
					static_cast<std::uint32_t>(Reason)});
		}
		void FailSession(DisconnectInfo Information) {
			if (Status == GameSessionStatus::Failed || Status == GameSessionStatus::Closed) return;
			RecordPooledLifecycle(detail::PooledLifecyclePoint::FailSession, PrimaryConnection.value_or(ConnectionId{}), Information.Reason);
			const auto Diagnostic = Information.Diagnostic.size() <= 512 ? Information.Diagnostic
																		 : Information.Diagnostic.substr(0, 512);
			Status = GameSessionStatus::Closing;
			ReleaseSessionResources();
			(void)Transport->Stop(std::move(Information));
			Failure = Diagnostic.empty() ? "Game session failed" : Diagnostic;
			Status = GameSessionStatus::Failed;
		}

		void Stop() {
			if (Status == GameSessionStatus::Closed || Status == GameSessionStatus::Failed) return;
			RecordPooledLifecycle(detail::PooledLifecyclePoint::LocalStop, PrimaryConnection.value_or(ConnectionId{}), DisconnectReason::LocalShutdown);
			const bool TransportStarted = Status != GameSessionStatus::Created;
			Status = GameSessionStatus::Closing;
			ReleaseSessionResources();
			if (TransportStarted) (void)Transport->Stop({DisconnectReason::LocalShutdown, "Game session stopped"});
			Status = GameSessionStatus::Closed;
		}

		bool QueueSession(ConnectionId Connection, const GameSessionMessage &Message, const NetworkLimits &Limits) {
			runtime_detail::WorkProducerScope Producer(runtime_detail::WorkProducer::SessionControl);
			auto Encoded = EncodeGameSessionMessage(Message);
			if (!Encoded) return false;
			auto Intent = MakeNetworkMessageIntent(
				Connection, DeliveryMode::ReliableOrdered, TrafficClass::Control, {}, std::move(*Encoded), Limits
			);
			return Intent && Scheduler.Submit(std::move(*Intent)).Accepted();
		}

		AssetContentId GetTerrainBinding() const {
			return Configuration.TerrainBootstrap ? Configuration.TerrainBootstrap->Binding : AssetContentId{};
		}

		static bool HasTerrainMotionRelease(const Peer &Value) {
			return Value.ReleasedTerrain && Value.ReleasedTerrain->Revision == Value.TerrainIdentity.FieldRevision &&
				(!Value.TerrainPending ||
					Value.TerrainPending->Offer.Identity.FieldRevision == Value.TerrainIdentity.FieldRevision);
		}

		bool IsTerrainMotionReleased(ConnectionId Connection) const {
			if (!Configuration.TerrainBootstrap) return true;
			const auto Found = Peers.find(Connection);
			return Found != Peers.end() && HasTerrainMotionRelease(Found->second);
		}

		static TerrainVersions GetTerrainVersions(const terrain::Field &Field, terrain::ChunkKey Center) {
			TerrainVersions Versions;
			std::ranges::transform(GetTerrainSessionKeys(Center), Versions.begin(),
				[&](terrain::ChunkKey Key) { return Field.GetVersion(Key).value(); });
			return Versions;
		}

		static bool IsCurrentTerrainWindow(const terrain::Field &Field, const Peer &Value) {
			return Value.TerrainWindow && (Value.CheckedTerrainRevision == Field.GetRevision() ||
				Value.KnownTerrainVersions == GetTerrainVersions(Field, Value.TerrainWindow->Center));
		}

		bool TerrainMotionAllowed(const Peer &Value, glm::vec3 Position, glm::vec3 Extents) const {
			if (!Configuration.TerrainBootstrap) return true;
			if (Value.Phase != PeerPhase::Ready || !Value.TerrainWindow ||
				!HasTerrainMotionRelease(Value) || !Runtime->Workspace->IsTerrainCollisionReady())
				return false;
			if (Configuration.Role == GameSessionRole::Server &&
				!IsCurrentTerrainWindow(Runtime->Workspace->GetTerrain()->GetField(), Value))
				return false;
			const auto Grid = Value.TerrainWindow->Grid;
			return ContainsTerrainBounds(Grid, Value.ReleasedTerrain->Center, Position, Extents) &&
				ContainsTerrainBounds(Grid, Value.TerrainWindow->Center, Position, Extents) &&
				(!Value.TerrainPending || ContainsTerrainBounds(Grid, Value.TerrainPending->Offer.Center, Position, Extents));
		}

		void AttachTerrainMotionAdmission(WorldRoot &World) {
			auto &Slot = World.TerrainMotionAdmission;
			if (!Slot.expired()) throw std::runtime_error("[Terrain:Network] Terrain motion admission already attached");
			TerrainMotionAdmission = std::make_shared<const WorldRoot::TerrainMotionGuard>(
				[this](ObjectId Object, glm::vec3 Position, glm::vec3 Extents) {
					if (Configuration.Role == GameSessionRole::Client) {
						const auto Found = PrimaryConnection ? Peers.find(*PrimaryConnection) : Peers.end();
						if (Found == Peers.end()) return false;
						const auto &Value = Found->second;
						if (Object.IsValid()) return TerrainMotionAllowed(Value, Position, Extents);
						const auto Grid = Configuration.TerrainBootstrap->Directory.Grid;
						if (Value.TerrainPending &&
							((Value.Phase == PeerPhase::Accepted && Value.TerrainAdmitted) ||
								(ClientTerrainWork && ClientTerrainWork->Installed)))
							return ContainsTerrainBounds(Grid, Value.TerrainPending->Offer.Center, Position, Extents);
						return Value.TerrainWindow && ContainsTerrainBounds(Grid, Value.TerrainWindow->Center, Position, Extents);
					}
					if (!Object.IsValid()) return true;
					for (const auto &[Connection, Value] : Peers) {
						const auto Controller = TerrainFocusActor(Value.PlayerValue);
						if (Controller && Controller->GetObjectId() == Object)
							return !PendingPeerFailures.contains(Connection) && TerrainMotionAllowed(Value, Position, Extents);
					}
					return true;
				});
			Slot = TerrainMotionAdmission;
		}

		bool QueueTerrainFrame(ConnectionId Connection, const Peer &Value, std::vector<std::byte> Frame) {
			if (Frame.empty() || !Scheduler.HasReliableCapacity(Connection, Frame.size())) return false;
			const auto FrameBytes = Frame.size() + static_cast<std::size_t>(ReliableServiceEnvelopeBytes);
			const auto NativeBudget = std::min(Value.Limits.MaximumQueuedReliableBytes,
				std::max(FrameBytes, Value.Limits.MaximumQueuedReliableBytes / 2));
			const auto Native = Transport->GetStatistics(Connection);
			if (FrameBytes > NativeBudget || (Native && Native->QueuedReliableBytes &&
				*Native->QueuedReliableBytes > NativeBudget - FrameBytes)) return false;
			auto Intent = MakeNetworkMessageIntent(Connection, DeliveryMode::ReliableOrdered, TrafficClass::Control,
				{}, std::move(Frame), Value.Limits);
			if (!Intent) return false;
			auto Submitted = Scheduler.Submit(std::move(*Intent));
			if (Submitted.TerminalDisconnect) PendingPeerFailures.try_emplace(Connection, std::move(*Submitted.TerminalDisconnect));
			return Submitted.Accepted();
		}

		std::shared_ptr<BlockGrid> ResolveGrid(ConnectionId Connection, const Peer &Value, ObjectId Id) const {
			const bool Server = Configuration.Role == GameSessionRole::Server;
			if (Server && (!Replication || !Replication->CanDeliverSpatialContent(Connection,Id))) return {};
			auto Grid = std::dynamic_pointer_cast<BlockGrid>(Server ? ObjectRegistry::Get().Lookup(Id) : Replica.Resolve(Id));
			if (!Grid || !Runtime || Grid->GetDataModel() != Runtime->DataModel || Grid->IsDestroying() || Grid->GetDestroyed()) return {};
			auto Owner = WorldRoot::GetEffectiveWorld(*Grid);
			if (Owner == Runtime->Workspace) return Value.AuthorizedSpatialRegion == ObjectId{} ? Grid : nullptr;
			if (!Owner || !std::dynamic_pointer_cast<SpatialRegion>(Owner)) return {};
			const auto Source = Server ? std::optional(Owner->GetObjectId()) : Replica.GetSourceObject(*Owner);
			return Source && *Source == Value.AuthorizedSpatialRegion ? Grid : nullptr;
		}

		void PumpGrids(ConnectionId Connection, Peer &Value) {
			if (!Value.Grids || Value.Phase != PeerPhase::Ready || PendingPeerFailures.contains(Connection)) return;
			try {
				Value.Grids->Pump([&](ObjectId Id) { return ResolveGrid(Connection,Value,Id); },
					[&](const GridStreamMessage &M) { return QueueTerrainFrame(Connection,Value,EncodeGridStream(M)); },CurrentTick);
			} catch (const std::exception &Error) {
				PendingPeerFailures.try_emplace(Connection,DisconnectInfo{DisconnectReason::ResourceExhaustion,Error.what()});
			}
		}

		void ReceiveGrid(const ReceivedMessageEvent &Message, Peer &Value) {
			if (!Value.Grids || Value.Phase != PeerPhase::Ready || Message.Delivery != DeliveryMode::ReliableOrdered ||
				Message.Traffic != TrafficClass::Control || !std::holds_alternative<std::monostate>(Message.Order)) {
				Reject(Message.Connection,DisconnectReason::ProtocolViolation,"Grid stream outside the negotiated session");
				return;
			}
			try {
				const auto Parsed = DecodeGridStream(Message.Payload);
				if (!Parsed) throw std::invalid_argument("[BlockGrid:Network] Malformed grid frame");
				Value.Grids->Receive(*Parsed,[&](ObjectId Id) { return ResolveGrid(Message.Connection,Value,Id); },CurrentTick);
			} catch (const std::exception &Error) {
				Reject(Message.Connection,DisconnectReason::ProtocolViolation,Error.what());
			}
		}

		bool CanReceiveTerrain(const ReceivedMessageEvent &Message, const Peer &Value) const {
			return Configuration.TerrainBootstrap &&
				(Value.Phase == PeerPhase::Accepted || Value.Phase == PeerPhase::Ready) &&
				Message.Delivery == DeliveryMode::ReliableOrdered && Message.Traffic == TrafficClass::Control &&
				std::holds_alternative<std::monostate>(Message.Order);
		}

		auto FindTerrainSendSlot() {
			for (auto &Slot : TerrainSendSlots)
				if (Slot && Slot->Completion->IsComplete()) Slot.reset();
			return std::ranges::find_if(TerrainSendSlots, [](const auto &Slot) { return !Slot; });
		}

		bool BeginTerrainWindow(Peer &Value, const terrain::Field &Field, terrain::ChunkKey Center) {
			const auto Slot = FindTerrainSendSlot();
			if (Slot == TerrainSendSlots.end()) return false;
			if (Value.TerrainIdentity.Serial == std::numeric_limits<std::uint64_t>::max())
				throw std::runtime_error("[Terrain:Network] Region sequence exhausted");
			const auto &Data = *Configuration.TerrainBootstrap;
			TerrainTransfer Transfer{
				.Offer = {{Value.SessionEpoch, Value.TerrainIdentity.Serial + 1, Field.GetRevision()}, Center},
				.Versions = GetTerrainVersions(Field, Center),
				.StartedTick = TerrainTick,
			};
			auto Snapshot = Field.Capture(GetTerrainSessionKeys(Center));
			if (!Snapshot) throw std::runtime_error("[Terrain:Network] Terrain window capture failed");
			Transfer.Sending = std::make_shared<TerrainSendWork>();
			const auto &Work = Transfer.Sending;
			TerrainJobs.Submit([Work, Samples = std::move(*Snapshot), Project = Data.Directory.Project, Center] {
				Work->Window = CaptureTerrainSessionWindow(Samples, Project, Center, Work->Stop.get_token());
			}, Work->Completion);
			*Slot = Work;
			Value.TerrainPending = std::move(Transfer);
			if (Value.Phase == PeerPhase::Ready && !Value.TerrainReleaseStartedTick)
				Value.TerrainReleaseStartedTick = TerrainTick;
			return true;
		}

		void SendTerrainWindow(ConnectionId Connection, Peer &Value, const terrain::Field &Field) {
			auto &Transfer = *Value.TerrainPending;
			if (const auto &Work = Transfer.Sending) {
				if (!Work->Completion->IsComplete()) return;
				if (!Work->Window) throw std::runtime_error("[Terrain:Network] Terrain window encoding failed");
				Transfer.Window = std::move(Work->Window);
				Transfer.Offer.ManifestByteCount = static_cast<std::uint32_t>(Transfer.Window->ManifestBytes.size());
				try {
					const auto Pinned = SelectTerrainSessionWindow(*Configuration.TerrainBootstrap, Transfer.Offer.Center);
					if (Pinned.ManifestDigest == Transfer.Window->ManifestDigest) Transfer.Offer.ManifestByteCount = 0;
				} catch (const std::length_error &) {
					// The edit may make the live window fit when the original package window cannot fit.
				}
				if (Value.TerrainWindow) ReuseTerrainSessionLeaves(*Transfer.Window, *Value.TerrainWindow);
				Transfer.Sending.reset();
			}
			const auto &Window = *Transfer.Window;
			if (Scheduler.HasQueuedControl(Connection)) return;
			if (!Transfer.Offered) {
				if (Transfer.Versions != GetTerrainVersions(Field, Transfer.Offer.Center)) {
					// No frame named this cohort yet. Keep the original admission deadline when refreshing it.
					const auto StartedTick = Transfer.StartedTick;
					if (BeginTerrainWindow(Value, Field, Transfer.Offer.Center)) Value.TerrainPending->StartedTick = StartedTick;
					return;
				}
				Transfer.Offer.LeafByteCount = static_cast<std::uint32_t>(Window.Size);
				Transfer.Offer.Identity.ManifestDigest = Window.ManifestDigest;
				Transfer.Offered = QueueTerrainFrame(Connection, Value, EncodeTerrainWindowOffer(Transfer.Offer));
				return;
			}
			if (Transfer.Cursor == Transfer.Offer.GetTransferBytes()) return;
			const auto ManifestBytes = Transfer.Offer.ManifestByteCount;
			const auto Slice = Transfer.Cursor < ManifestBytes
				? std::as_bytes(std::span(Window.ManifestBytes)).subspan(Transfer.Cursor)
				: GetTerrainSessionFragment(Window, Transfer.Cursor - ManifestBytes);
			if (Slice.empty()) throw std::runtime_error("[Terrain:Network] Terrain transfer lost an immutable leaf");
			const auto Capacity = Value.Limits.MaximumQueuedReliableBytes / 2 -
				static_cast<std::size_t>(ReliableServiceEnvelopeBytes) - TerrainFragmentHeaderBytes;
			const auto Fragment = Slice.first(std::min(Slice.size(), Capacity));
			auto Frame = EncodeTerrainFragment({Value.SessionEpoch, Transfer.Offer.Identity.Serial,
				static_cast<std::uint32_t>(Transfer.Cursor), Fragment});
			if (QueueTerrainFrame(Connection, Value, std::move(Frame))) Transfer.Cursor += Fragment.size();
		}

		bool TerrainReleaseTimedOut(const Peer &Value) const {
			return Value.TerrainReleaseStartedTick && TerrainTick - *Value.TerrainReleaseStartedTick >= Configuration.HandshakeTimeoutTicks;
		}

		static bool CoversTerrainFocus(const Peer &Value) {
			const auto Bounds = GetTerrainFocusBounds(TerrainFocusActor(Value.PlayerValue));
			return ContainsTerrainBounds(Value.TerrainWindow->Grid, Value.TerrainWindow->Center, Bounds.Position, Bounds.Extents);
		}

		static void ReleaseTerrainMotion(Peer &Value) {
			Value.ReleasedTerrain = TerrainMotionRegion{Value.TerrainWindow->Center, Value.TerrainIdentity.FieldRevision};
			Value.TerrainReleaseStartedTick.reset();
		}

		void PumpServerTerrain() {
			const auto Owner = Runtime->Workspace->GetTerrain();
			if (!Owner || !Runtime->Workspace->IsTerrainCollisionReady()) {
				FailSession({DisconnectReason::ResourceExhaustion, "Server terrain collision became unavailable"});
				return;
			}
			const auto &Field = Owner->GetField();
			for (auto &[Connection, Value] : Peers) {
				if (Value.Phase != PeerPhase::Ready || !Value.TerrainReleaseStartedTick) continue;
				if (TerrainReleaseTimedOut(Value)) {
					PendingPeerFailures.try_emplace(Connection,
						DisconnectInfo{DisconnectReason::Timeout, "Terrain motion release timed out"});
					continue;
				}
				if (!Value.TerrainPending && IsCurrentTerrainWindow(Field, Value) && CoversTerrainFocus(Value) &&
					!Scheduler.HasQueuedControl(Connection) &&
					QueueTerrainFrame(Connection, Value, EncodeTerrainWindowReady(Value.TerrainIdentity)))
					ReleaseTerrainMotion(Value);
			}
			auto InFlight = std::ranges::count_if(Peers, [](const auto &Entry) { return Entry.second.TerrainPending.has_value(); });
			const auto Limit = std::ssize(TerrainSendSlots);
			auto Next = TerrainFairnessCursor ? Peers.upper_bound(*TerrainFairnessCursor) : Peers.begin();
			for (std::size_t Visited = 0; Visited < Peers.size() && InFlight < Limit; ++Visited) {
				if (Next == Peers.end()) Next = Peers.begin();
				auto &[Connection, Value] = *Next++;
				if (Value.Phase != PeerPhase::Ready || Value.TerrainPending || !Value.TerrainWindow ||
					PendingPeerFailures.contains(Connection)) continue;
				const bool Current = IsCurrentTerrainWindow(Field, Value);
				if (Current) Value.CheckedTerrainRevision = Field.GetRevision();
				if (Current && Value.TerrainReleaseStartedTick && CoversTerrainFocus(Value)) continue;
				const auto Bounds = GetTerrainFocusBounds(TerrainFocusActor(Value.PlayerValue));
				const auto Center = GetTerrainSessionCenter(Field.GetGrid(),
					{Bounds.Position.x, Bounds.Position.y, Bounds.Position.z});
				if (!Center || (*Center == Value.TerrainWindow->Center && Current)) continue;
				try {
					if (!BeginTerrainWindow(Value, Field, *Center)) break;
					TerrainFairnessCursor = Connection;
					++InFlight;
				} catch (const std::exception &) {
					PendingPeerFailures.try_emplace(Connection,
						DisconnectInfo{DisconnectReason::ResourceExhaustion, "Server terrain window capture failed"});
				}
			}
			for (auto &[Connection, Value] : Peers) {
				if (Value.Phase != PeerPhase::Accepted && Value.Phase != PeerPhase::Ready) continue;
				try {
					if (!Value.TerrainPending || PendingPeerFailures.contains(Connection)) continue;
					if (TerrainTick - Value.TerrainPending->StartedTick >= Configuration.HandshakeTimeoutTicks) {
						PendingPeerFailures.try_emplace(Connection,
							DisconnectInfo{DisconnectReason::Timeout, "Terrain window readiness timed out"});
						continue;
					}
					SendTerrainWindow(Connection, Value, Field);
				} catch (const std::exception &Error) {
					LOG_WARN(App, "[Terrain:Network] Server delivery failed: %.160s", Error.what());
					PendingPeerFailures.try_emplace(Connection,
						DisconnectInfo{DisconnectReason::ResourceExhaustion, "Server terrain window delivery failed"});
				}
			}
		}

		void ReceiveTerrainWindowReady(const ReceivedMessageEvent &Message, Peer &Value) {
			const auto Ready = DecodeTerrainWindowReady(Message.Payload);
			if (Configuration.Role == GameSessionRole::Client) {
				if (!CanReceiveTerrain(Message, Value) || !ClientRuntimeAttached || !Value.TerrainReleaseStartedTick ||
					Value.TerrainPending || !Value.TerrainWindow || !Ready || *Ready != Value.TerrainIdentity) {
					Reject(Message.Connection, DisconnectReason::ProtocolViolation, "Invalid terrain motion release");
					return;
				}
				if (!Value.ReleasedTerrain)
					LOG_INFO(App, "[Terrain:Network] Client released movement after the server confirmed admitted collision");
				ReleaseTerrainMotion(Value);
				return;
			}
			const auto &Transfer = Value.TerrainPending;
			if (!CanReceiveTerrain(Message, Value) ||
				!Transfer || !Transfer->Offered || Transfer->Cursor != Transfer->Offer.GetTransferBytes() ||
				!Ready || *Ready != Transfer->Offer.Identity) {
				Reject(Message.Connection, DisconnectReason::ProtocolViolation, "Invalid terrain window acknowledgment");
				return;
			}
			Value.TerrainIdentity = *Ready;
			Value.CheckedTerrainRevision = Ready->FieldRevision;
			Value.KnownTerrainVersions = Transfer->Versions;
			Value.TerrainWindow = std::move(Value.TerrainPending->Window);
			for (auto &Range : Value.TerrainWindow->Artifacts) Range.Source.reset();
			Value.TerrainWindow->ManifestBytes.clear();
			Value.TerrainPending.reset();
		}

		bool QueueTerrainReady(ConnectionId Connection, Peer &Value, TerrainSessionWindow &&Window) {
			const auto Identity = Value.TerrainPending->Offer.Identity;
			if (!QueueTerrainFrame(Connection, Value, EncodeTerrainWindowReady(Identity))) return false;
			Value.TerrainIdentity = Identity;
			Value.TerrainWindow = std::move(Window);
			Value.TerrainPending.reset();
			return true;
		}

		void FinishTerrainDelivery(Peer &Value) {
			auto &Transfer = *Value.TerrainPending;
			const auto ManifestSize = Transfer.Offer.ManifestByteCount;
			if (ManifestSize) {
				Transfer.Window = ReadTerrainSessionWindow(*Configuration.TerrainBootstrap, Transfer.Offer.Center,
					Transfer.Offer.Identity.ManifestDigest, std::span(TerrainReceiveBuffer).first(ManifestSize));
				if (Value.TerrainWindow) ReuseTerrainSessionLeaves(*Transfer.Window, *Value.TerrainWindow);
				if (Transfer.Window->Size != Transfer.Offer.LeafByteCount)
					throw std::invalid_argument("[Terrain:Network] Terrain leaf bytes do not match the offer");
			}
			auto Bytes = std::exchange(TerrainReceiveBuffer, {});
			if (Value.Phase == PeerPhase::Accepted) {
				ReceivedTerrain = LoadTerrainSessionField(*Transfer.Window, std::span(Bytes).subspan(ManifestSize));
				SynchronizeClientGuiAndBootstrap();
				return;
			}
			auto Work = std::make_shared<TerrainReceiveWork>();
			Work->Window = std::move(*Transfer.Window);
			TerrainJobs.Submit([Work, Bytes = std::move(Bytes), ManifestSize] {
				Work->Field = LoadTerrainSessionField(Work->Window, std::span(Bytes).subspan(ManifestSize));
				auto Prepared = terrain::PrepareTerrainReplica(*Work->Field, Work->Stop.get_token());
				if (!Prepared) throw std::runtime_error("[Terrain:Network] Terrain window preparation failed");
				Work->Chunks = std::move(*Prepared);
			}, Work->Completion);
			ClientTerrainWork = std::move(Work);
		}

		bool PumpClientTerrain() {
			if (!PrimaryConnection) return true;
			const auto Connection = *PrimaryConnection;
			auto &Value = Peers.at(Connection);
			if (TerrainReleaseTimedOut(Value)) {
				FailSession({DisconnectReason::Timeout, "Terrain motion release timed out"});
				return false;
			}
			if (!ClientTerrainWork || !ClientTerrainWork->Completion->IsComplete()) return true;
			const auto &Work = ClientTerrainWork;
			if (!Value.TerrainPending || Work->Completion->GetFirstException() ||
				(!Work->Installed && !Runtime->WorldRoot->InstallTerrainReplica(std::move(Work->Field), std::move(Work->Chunks),
					Value.TerrainPending->Offer.Identity.Serial))) {
				FailSession({DisconnectReason::ResourceExhaustion, "Client terrain collision cutover failed"});
				return false;
			}
			Work->Installed = true;
			if (QueueTerrainReady(Connection, Value, std::move(Work->Window))) ClientTerrainWork.reset();
			return true;
		}

		void ReceiveTerrainWindowOffer(const ReceivedMessageEvent &Message, Peer &Value) {
			const auto Offer = DecodeTerrainWindowOffer(Message.Payload);
			if (Configuration.Role != GameSessionRole::Client || !CanReceiveTerrain(Message, Value) ||
				Value.TerrainPending || !Offer || Offer->Identity.SessionEpoch != Value.SessionEpoch ||
				Offer->Identity.Serial != Value.TerrainIdentity.Serial + 1 ||
				Offer->Identity.FieldRevision < Value.TerrainIdentity.FieldRevision) {
				Reject(Message.Connection, DisconnectReason::ProtocolViolation, "Invalid terrain window offer");
				return;
			}
			try {
				auto &Transfer = Value.TerrainPending.emplace(TerrainTransfer{.Offer = *Offer, .StartedTick = TerrainTick});
				if (Value.Phase == PeerPhase::Ready && !Value.TerrainReleaseStartedTick)
					Value.TerrainReleaseStartedTick = TerrainTick;
				if (!Offer->ManifestByteCount) {
					Transfer.Window = SelectTerrainSessionWindow(*Configuration.TerrainBootstrap, Offer->Center);
					if (Value.TerrainWindow) ReuseTerrainSessionLeaves(*Transfer.Window, *Value.TerrainWindow);
					if (Transfer.Window->ManifestDigest != Offer->Identity.ManifestDigest || Transfer.Window->Size != Offer->LeafByteCount)
						throw std::invalid_argument("[Terrain:Network] Terrain window conflicts with pinned directory");
				}
				if (!Offer->GetTransferBytes()) FinishTerrainDelivery(Value);
			} catch (const std::exception &Error) {
				LOG_WARN(App, "[Terrain:Network] Window offer failed: %.160s", Error.what());
				Reject(Message.Connection, DisconnectReason::ProtocolViolation, "Terrain window verification failed");
			}
		}

		void ReceiveTerrainFragment(const ReceivedMessageEvent &Message, Peer &Value) {
			const auto &Transfer = Value.TerrainPending;
			const auto Fragment = DecodeTerrainFragment(Message.Payload);
			if (Configuration.Role != GameSessionRole::Client || !CanReceiveTerrain(Message, Value) ||
				!Transfer || ReceivedTerrain || (Value.TerrainAdmitted && Value.Phase == PeerPhase::Accepted) ||
				ClientTerrainWork || !Fragment || Fragment->Serial != Transfer->Offer.Identity.Serial ||
				Fragment->SessionEpoch != Value.SessionEpoch ||
				Fragment->Offset != TerrainReceiveBuffer.size() ||
				TerrainReceiveBuffer.size() > Transfer->Offer.GetTransferBytes() ||
				Fragment->Bytes.size() > Transfer->Offer.GetTransferBytes() - TerrainReceiveBuffer.size()) {
				Reject(Message.Connection, DisconnectReason::ProtocolViolation, "Invalid terrain window fragment");
				return;
			}
			try {
				if (TerrainReceiveBuffer.empty()) TerrainReceiveBuffer.reserve(Transfer->Offer.GetTransferBytes());
				const auto *Bytes = reinterpret_cast<const std::uint8_t *>(Fragment->Bytes.data());
				TerrainReceiveBuffer.insert(TerrainReceiveBuffer.end(), Bytes, Bytes + Fragment->Bytes.size());
				if (TerrainReceiveBuffer.size() == Transfer->Offer.GetTransferBytes()) FinishTerrainDelivery(Value);
			} catch (const std::exception &Error) {
				LOG_WARN(App, "[Terrain:Network] Window verification failed: %.160s", Error.what());
				Reject(Message.Connection, DisconnectReason::ProtocolViolation, "Terrain window verification failed");
			}
		}

		SerializationResult<SchedulerSubmitResult>
		QueueStructuralFrame(const ReplicationFrame &Frame, std::vector<std::byte> Encoded,
			ConnectionId Connection, const NetworkLimits &Limits, std::uint64_t RetirementToken = 0) {
			runtime_detail::WorkProducerScope Producer(Frame.Kind == ReplicationMessageKind::Baseline ? runtime_detail::WorkProducer::Bootstrap :
				runtime_detail::WorkProducer::Structural);
			if (InjectFailure(detail::GameSessionFailurePoint::StructuralSchedulerAdmission))
				return SchedulerSubmitResult{
					SchedulerSubmitStatus::ReliableBacklogExhausted,
					DisconnectInfo{
						DisconnectReason::ResourceExhaustion,
						"Injected reliable structural scheduler exhaustion",
					},
				};
			if (!RetirementToken) return QueueEncodedReplicationFrame(Frame.Sequence, std::move(Encoded), Connection, Limits, Scheduler);
			auto Intent = runtime_detail::MeasureWork(runtime_detail::WorkPhase::StructuralIntent, [&] {
				return MakeNetworkMessageIntent(Connection, DeliveryMode::ReliableOrdered,
					TrafficClass::StructuralReplication, ReliableReplicationOrder{Frame.Sequence}, std::move(Encoded), Limits);
			});
			if (!Intent || !detail::ReliableServiceFeedbackAccess::Attribute(*Intent, RetirementToken))
				return SchedulerSubmitResult{SchedulerSubmitStatus::IntentRejected};
			return runtime_detail::MeasureWork(runtime_detail::WorkPhase::StructuralSubmit, [&] { return Scheduler.Submit(std::move(*Intent)); });
		}

		void FailPeer(ConnectionId Connection, DisconnectReason Reason, std::string Diagnostic) {
			RecordPooledLifecycle(detail::PooledLifecyclePoint::FailPeer, Connection, Reason);
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
				if (IsPooled()) (void)Transport->Disconnect(Connection, {Reason, Diagnostic});
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
			if (!Connection.IsValid() || Peers.size() >= MaximumGameSessionPeers ||
				(Configuration.ReliableService && Peers.size() >= Configuration.ReliableService->MaximumConnections)) {
				Reject(Connection, DisconnectReason::ResourceExhaustion, "Game session peer capacity reached");
				return;
			}
			Peer Value;
			Value.ReliableFeedback.SeparateNativeCapacity = IsPooled() &&
				Configuration.ReliableService->Pooled.SeparateNativeCapacity;
			Value.ReliableFeedback.DiagnosticPerformance = IsPooled() &&
				Configuration.ReliableService->Pooled.DiagnosticPerformance;
			Value.ConnectedTick = CurrentTick;
			if (!Peers.emplace(Connection, std::move(Value)).second) return;
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
					Connection, GameSessionClientHello{PeerValue.Nonce, Configuration.Limits,
						GetTerrainBinding(), Configuration.RuntimeDistributionIdentity, Configuration.SpatialContent}, Configuration.Limits
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
			if (Hello.SpatialContent != Configuration.SpatialContent || Hello.TerrainBinding != GetTerrainBinding()) {
				++Metrics.RejectedHandshakes;
				Reject(Connection, DisconnectReason::IncompatibleVersion, "Client native content profile differs from the server");
				return;
			}
			if (Hello.RuntimeDistributionIdentity != Configuration.RuntimeDistributionIdentity) {
				++Metrics.RejectedHandshakes;
				Reject(Connection, DisconnectReason::ProtocolViolation, "Client runtime distribution identity is incompatible");
				return;
			}
			auto Limits = NegotiateNetworkLimits(Configuration.Limits, Hello.AdvertisedLimits);
			if (Configuration.SpatialContent && (!Limits || Limits->MaximumReliableMessageBytes < MaximumGridStreamFragment + 128 ||
				std::ranges::count_if(Peers, [](const auto &Entry) { return bool(Entry.second.Grids); }) >= 8)) {
				Reject(Connection, DisconnectReason::ResourceExhaustion, "Spatial delivery capacity is unavailable");
				return;
			}
			if (Configuration.TerrainBootstrap && (!Limits ||
				Limits->MaximumReliableMessageBytes < TerrainFragmentHeaderBytes + MaximumTerrainFragmentBytes ||
				std::ranges::count_if(Peers, [](const auto &Entry) { return Entry.second.TerrainPending.has_value(); })
					>= MaximumTerrainBootstrapPeers)) {
				++Metrics.RejectedHandshakes;
				Reject(Connection, DisconnectReason::ResourceExhaustion, "Terrain bootstrap capacity is unavailable");
				return;
			}
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
				PeerValue.PlayerObject = PeerValue.PlayerValue->GetObjectId();
				ConnectionsByPlayer.emplace(PeerValue.PlayerObject, Connection);
				if (Configuration.SpatialContent) {
					if (Configuration.ResolveInitialSpatialAudience)
						PeerValue.AuthorizedSpatialRegion = Configuration.ResolveInitialSpatialAudience(*PeerValue.PlayerValue);
					PeerValue.Grids = std::make_unique<BlockGridPeer>(true,PeerValue.SessionEpoch,PeerValue.AuthorizedSpatialRegion);
				}
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
			PeerValue.PlayerId = static_cast<std::uint32_t>(PeerValue.PlayerValue->GetPlayerId());
			++Metrics.PlayersCreated;
			if (const auto &Terrain = Configuration.TerrainBootstrap) {
				try {
					const auto Position = GetTerrainFocusBounds(TerrainFocusActor(PeerValue.PlayerValue)).Position;
					const auto Center = GetTerrainSessionCenter(Terrain->Directory.Grid, {Position.x, Position.y, Position.z});
					if (!Center) throw std::invalid_argument("[Terrain:Network] Spawn is outside the terrain profile");
					if (!BeginTerrainWindow(PeerValue, Runtime->Workspace->GetTerrain()->GetField(), *Center))
						throw std::runtime_error("[Terrain:Network] Terrain worker capacity is unavailable");
				} catch (const std::exception &) {
					Reject(Connection, DisconnectReason::ResourceExhaustion, "Server terrain spawn window is unavailable");
					return;
				}
			}
			const auto RelevanceStarted = std::chrono::steady_clock::now();
			if (!Relevance->AddPeer(Connection, PeerValue.PlayerObject, OwnerSpatialRoot(PeerValue.PlayerValue, *Relevance), PeerValue.AuthorizedSpatialRegion)) {
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
			auto Registered = Replication->RegisterPeerPlanned(Connection, PeerValue.Replication, Relevance->GetSelectionSnapshot(Connection));
			if (!Registered.Succeeded()) {
				Reject(Connection, DisconnectReason::ResourceExhaustion, "Server replication registration failed");
				return;
			}
			PeerValue.Phase = PeerPhase::BootstrapPending;
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
				Accepted.Nonce != Iterator->second.Nonce || Accepted.TerrainBinding != GetTerrainBinding() ||
				Accepted.SpatialContent != Configuration.SpatialContent) {
				++Metrics.RejectedHandshakes;
				Reject(
					Connection, DisconnectReason::ProtocolViolation, "Server acceptance does not match the client hello"
				);
				return;
			}
			auto Expected = NegotiateNetworkLimits(Configuration.Limits, Accepted.NegotiatedLimits);
			if (Accepted.RuntimeDistributionIdentity != Configuration.RuntimeDistributionIdentity) {
				++Metrics.RejectedHandshakes;
				Reject(Connection, DisconnectReason::ProtocolViolation, "Server runtime distribution identity is incompatible");
				return;
			}
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
			PeerValue.AuthorizedSpatialRegion = Accepted.AuthorizedSpatialRegion;
			if (Configuration.SpatialContent)
				PeerValue.Grids = std::make_unique<BlockGridPeer>(false,PeerValue.SessionEpoch,PeerValue.AuthorizedSpatialRegion);
			PeerValue.Replication = Accepted.Replication;
			PeerValue.Limits = Accepted.NegotiatedLimits;
			PeerValue.PlayerObject = Accepted.Player;
			PeerValue.PlayerId = Accepted.PlayerId;
			++Metrics.AcceptedPeers;
			SynchronizeClientGuiAndBootstrap();
		}

		void ApplyReplication(const ReceivedMessageEvent &Message) {
			const auto DecodeStarted = std::chrono::steady_clock::now();
			auto Frame = DecodeReplicationFrame(Message.Payload);
			const auto ApplyStarted = std::chrono::steady_clock::now();
			Metrics.ClientStructuralBytesReceived += Message.Payload.size();
			Metrics.ClientStructuralDecodeNanoseconds += static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(ApplyStarted - DecodeStarted).count());
			if (!Frame) {
				Reject(
					Message.Connection, DisconnectReason::ProtocolViolation, "Malformed structural replication frame"
				);
				return;
			}
			if (!Configuration.RuntimeDistributionIdentity.empty()) {
				const auto IsSource = [](std::string_view Name, const WireValue &Value) {
					if (Name != "Source" && Name != "ReplicatedSource") return false;
					const auto *Text = std::get_if<std::string>(&Value);
					return !Text || !Text->empty();
				};
				for (const auto &Operation : Frame->Operations) {
					bool Invalid = false;
					if (const auto *Publish = std::get_if<PublishReplication>(&Operation.Intent)) {
						for (const auto &[Name, Value] : Publish->Properties)
							Invalid = Invalid || IsSource(Name, Value);
					} else if (const auto *Property = std::get_if<PropertyReplicationUpdate>(&Operation.Intent)) {
						Invalid = IsSource(Property->PropertyName, Property->Value);
					}
					if (Invalid) {
						Reject(Message.Connection, DisconnectReason::ProtocolViolation,
							"Managed runtime distribution received unauthorized script source");
						return;
					}
				}
			}
			ReplicaApplier::BeforePublish Prepare;
			if (Configuration.HydratePackagedCode) {
				Prepare = [&](ObjectId Identity, const std::shared_ptr<Instance> &Object) {
					const auto Source = std::dynamic_pointer_cast<LuaSourceContainer>(Object);
					if (!Source) return;
					Configuration.HydratePackagedCode(Identity, Source, !ClientRuntimeAttached);
					if (!ClientRuntimeAttached || !Configuration.HydratePlayerGuiCode ||
						Source->GetGuiTemplateSourceId().empty() || !PrimaryConnection) return;
					const auto Peer = Peers.find(*PrimaryConnection);
					const auto Gui = FindPlayerGuiScope(*Source);
					const auto Owner = Gui ? std::dynamic_pointer_cast<Player>(Gui->GetParent().value_or(nullptr)) : nullptr;
					if (Peer != Peers.end() && Owner &&
						static_cast<std::uint32_t>(Owner->GetPlayerId()) == Peer->second.PlayerId &&
						!Configuration.HydratePlayerGuiCode(Source))
						throw std::runtime_error("Managed PlayerGui script binding is incompatible");
				};
			}
			auto Result = Replica.ApplyFrame(*Frame, Prepare);
			Metrics.ClientStructuralApplyNanoseconds += static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - ApplyStarted).count());
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
			const auto LocalPeer = PrimaryConnection ? Peers.find(*PrimaryConnection) : Peers.end();
			const auto TrustedOwner = LocalPeer != Peers.end() ? LocalPeer->second.PlayerObject : ObjectId{};
			bool CharacterProjected = false;
			for (const auto &Operation : Frame->Operations) {
				const auto Object = ReplicationObject(Operation.Intent);
				if (Object == TrustedOwner) {
					const auto *Property = std::get_if<PropertyReplicationUpdate>(&Operation.Intent);
					CharacterProjected = CharacterProjected || IsPublishReplication(Operation.Intent) ||
						(Property && Property->PropertyName == "Character");
				}
				if (IsPublishReplication(Operation.Intent)) {
					ClientKnownObjects.insert(Object);
					if (Remotes && !Remotes->MarkMaterialized(Message.Connection, Object)) {
						FailSession({
							DisconnectReason::ResourceExhaustion,
							"Client Remote materialization registration failed",
						});
						return;
					}
					auto ReplicaObject = Replica.Resolve(Object);
					if (ClientRuntimeAttached && LocalPeer != Peers.end() && LocalPeer->second.Grids &&
						std::dynamic_pointer_cast<BlockGrid>(ReplicaObject)) {
						try { LocalPeer->second.Grids->Track(Object); }
						catch (const std::exception &Error) { FailSession({DisconnectReason::ResourceExhaustion,Error.what()}); return; }
					}
					if (std::dynamic_pointer_cast<KinematicCharacter>(ReplicaObject))
						ClientCharacterObjects.insert(Object);
					if (std::dynamic_pointer_cast<RemoteBase>(ReplicaObject)) ClientRemoteObjects.insert(Object);
					if (Configuration.HydratePlayerGuiCode) {
						if (const auto Source = std::dynamic_pointer_cast<LuaSourceContainer>(ReplicaObject);
							Source && !Source->GetGuiTemplateSourceId().empty()) PendingClientGuiCode.insert(Object);
					}
				} else if (std::holds_alternative<UnpublishReplication>(Operation.Intent) ||
						   std::holds_alternative<DestroyReplication>(Operation.Intent)) {
					if (auto Found = ClientPossessionPoses.find(Object); Found != ClientPossessionPoses.end() &&
						Found->second.RequiredStructuralSequence <= Frame->Sequence.Value()) {
						ClientPosePresentation.Forget(Object);
						ClientPossessionPoses.erase(Found);
					}
					// A validated property write (for example Player.Character = nil)
					// can destroy a subtree before its later bounded Leave frames arrive.
					// The post-apply sweep already retires those local registrations.
					const bool WasKnown = ClientKnownObjects.erase(Object) != 0;
					if (LocalPeer != Peers.end() && LocalPeer->second.Grids) LocalPeer->second.Grids->Forget(Object);
					ClientCharacterObjects.erase(Object);
					ClientRemoteObjects.erase(Object);
					PendingClientGuiCode.erase(Object);
					if (Prediction && ClientMaterializedCharacters.erase(Object) &&
						!Prediction->MarkUnmaterialized(Object)) {
						FailSession({
							DisconnectReason::ResourceExhaustion,
							"Client Character unmaterialization registration failed",
						});
						return;
					}
					if (Remotes && WasKnown && !Remotes->MarkUnmaterialized(Message.Connection, Object)) {
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
				PendingClientGuiCode.erase(*Iterator);
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
			SynchronizeClientGuiAndBootstrap(CharacterProjected);
		}

		void ReconcileClientGui(const std::shared_ptr<Player> &Owner, const std::shared_ptr<PlayerGui> &Gui,
			bool CharacterProjected) {
			if (Owner->GetDestroyed() || Owner->IsDestroying() || Gui->GetDestroyed() || Gui->IsDestroying()) return;
			for (const auto Object : PendingClientGuiCode) {
				const auto Source = std::dynamic_pointer_cast<LuaSourceContainer>(Replica.Resolve(Object));
				if (!Source || FindPlayerGuiScope(*Source) != Gui) continue;
				try {
					if (!Configuration.HydratePlayerGuiCode(Source) && !Configuration.RuntimeDistributionIdentity.empty()) {
						FailSession({DisconnectReason::ProtocolViolation, "Managed PlayerGui script binding is incompatible"});
						return;
					}
				} catch (const std::exception &Error) {
					if (!Configuration.RuntimeDistributionIdentity.empty()) {
						FailSession({DisconnectReason::ProtocolViolation, "Managed PlayerGui script binding failed"});
						return;
					}
					LOG_WARN(App, "[Gui:Provision] Client GUI code hydration failed: %.160s", Error.what());
				}
			}
			PendingClientGuiCode.clear();

			if (!CharacterProjected) return;
			const auto Character = Owner->GetCharacter().value_or(nullptr);
			if (!Character || Character->GetDestroyed() || Character->IsDestroying()) return;
			const auto CharacterId = Character->GetObjectId();
			if (ClientGuiCharacter == CharacterId) return;
			const bool Respawn = ClientGuiCharacter.IsValid();
			ClientGuiCharacter = CharacterId;
			if (!Respawn) return;
			// Server-owned roots retire through structural replication. Locally
			// created roots can opt into the same reset policy independently.
			for (const auto &Child : Gui->GetChildren()) {
				if (Gui->GetDestroyed() || Gui->IsDestroying()) break;
				const auto Root = std::dynamic_pointer_cast<ScreenGui>(Child);
				if (!Root || Root->GetParent().value_or(nullptr) != Gui || !Root->GetResetOnSpawn() ||
					Root->IsReplicaMaterialization()) continue;
				try {
					Root->Destroy();
				} catch (const std::exception &Error) {
					LOG_WARN(App, "[Gui:Provision] Client-local GUI reset failed: %.160s", Error.what());
				}
			}
		}

		void SynchronizeClientGuiAndBootstrap(bool CharacterProjected = false) {
			if (!PrimaryConnection || !BaselineApplied) return;
			auto PeerIterator = Peers.find(*PrimaryConnection);
			if (PeerIterator == Peers.end() ||
				(PeerIterator->second.Phase != PeerPhase::Accepted && PeerIterator->second.Phase != PeerPhase::Ready) ||
				Replica.GetEpoch() != PeerIterator->second.Replication)
				return;
			const bool AlreadyReady = PeerIterator->second.Phase == PeerPhase::Ready;
			auto Root = std::dynamic_pointer_cast<DataModel>(Replica.GetReplicaRoot());
			auto PlayerValue = std::dynamic_pointer_cast<Player>(Replica.Resolve(PeerIterator->second.PlayerObject));
			auto PlayersValue = Root ? std::dynamic_pointer_cast<Players>(Root->FindFirstChildOfClass("Players", false))
									 : nullptr;
			if (!PlayerValue) return;
			const auto Gui = PlayerValue->GetPlayerGui().value_or(nullptr);
			if (!Gui) return;
			if (Configuration.TerrainBootstrap && !PeerIterator->second.TerrainAdmitted) {
				if (!ReceivedTerrain || !Root) return;
				try {
					const auto Space = std::dynamic_pointer_cast<Workspace>(Root->GetService("Workspace"));
					if (!Space) throw std::runtime_error("Replica Workspace is unavailable");
					Space->RestoreTerrainField(std::move(ReceivedTerrain));
					AttachTerrainMotionAdmission(*Space);
					PeerIterator->second.TerrainAdmitted = true;
				} catch (const std::exception &) {
					Reject(*PrimaryConnection, DisconnectReason::ProtocolViolation, "Replica terrain admission failed");
					return;
				}
			}
			const bool InitialLocalBinding = !AlreadyReady && PlayersValue && !PlayersValue->GetLocalPlayer();
			if (!Root || !PlayersValue || PeerIterator->second.PlayerId == 0 ||
				static_cast<std::uint32_t>(PlayerValue->GetPlayerId()) != PeerIterator->second.PlayerId ||
				!(AlreadyReady ? PlayersValue->GetLocalPlayer().value_or(nullptr) == PlayerValue
							   : PlayersValue->SetTrustedLocalPlayer(PlayerValue))) {
				Reject(
					*PrimaryConnection,
					DisconnectReason::ProtocolViolation,
					"Trusted LocalPlayer association was not materialized"
				);
				return;
			}
			ReconcileClientGui(PlayerValue, Gui, InitialLocalBinding || CharacterProjected);
			if (Status == GameSessionStatus::Failed) return;
			if (AlreadyReady) return;
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
				Ready.Replication != Iterator->second.Replication || Ready.Player != Iterator->second.PlayerObject ||
				Ready.TerrainBinding != GetTerrainBinding() || Ready.SpatialContent != Configuration.SpatialContent ||
				(Configuration.TerrainBootstrap && (!Iterator->second.TerrainWindow || Iterator->second.TerrainPending ||
					!Runtime->Workspace->IsTerrainCollisionReady()))) {
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
			if (!ArmOrdinaryPolicy(Connection, PeerValue)) {
				Reject(Connection, DisconnectReason::ResourceExhaustion, "Ordinary source policy registration failed");
				return;
			}
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
			if (Configuration.TerrainBootstrap) {
				PeerValue.TerrainReleaseStartedTick = TerrainTick;
				LOG_INFO(App, "[Terrain:Network] Server admitted gameplay after verified terrain readiness");
			}
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
			runtime_detail::PublicationPacketScope Latency(
				Configuration.Role == GameSessionRole::Client ? "ClientCallback" : "ServerCallback",
				Configuration.Role == GameSessionRole::Client ? "ClientHandled" : "ServerHandled",
				Received.Connection, Received.Payload);
			auto PeerIterator = Peers.find(Received.Connection);
			if (PeerIterator == Peers.end()) return;
			const auto Magic = FrameMagic(Received.Payload);
			if (Magic == 0x53455347) {
				HandleSessionMessage(Received.Connection, Received);
				return;
			}
			if (IsGridStream(Received.Payload)) {
				ReceiveGrid(Received,PeerIterator->second);
				return;
			}
			if (IsTerrainWindowOffer(Received.Payload)) {
				ReceiveTerrainWindowOffer(Received, PeerIterator->second);
				return;
			}
			if (IsTerrainWindowReady(Received.Payload)) {
				ReceiveTerrainWindowReady(Received, PeerIterator->second);
				return;
			}
			if (IsTerrainFragment(Received.Payload)) {
				ReceiveTerrainFragment(Received, PeerIterator->second);
				return;
			}
			if (PeerIterator->second.Phase == PeerPhase::TransportConnected ||
				PeerIterator->second.Phase == PeerPhase::BootstrapPending) {
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
			if (IsGameplayInputFrame(Received.Payload)) {
				if (Configuration.Role == GameSessionRole::Server)
					(void)ServerPossessionInput.HandleReceived(Received, std::max(CurrentTick, std::uint64_t{1}));
				else if (!ClientPossessionInput.HandleReceived(Received))
					Reject(Received.Connection, DisconnectReason::ProtocolViolation, "Invalid possession control frame");
				return;
			}
			if (Configuration.Role == GameSessionRole::Client && IsGameplayPoseFrame(Received.Payload)) {
				HandlePossessionPose(Received);
				return;
			}
			TransportEvent Event = Received;
			if (Magic == CharacterMagic) {
				if (Configuration.Role == GameSessionRole::Server)
					(void)Authority->HandleTransportEvent(Event);
				else if (Prediction->HandleTransportEvent(Event))
					RecordClientService(Metrics.ClientCharacterMessagesHandled,
						Metrics.ClientCharacterMaximumServiceGapNanoseconds, LastClientCharacterService);
			} else if (Magic == RemoteMagic) {
				if (Remotes->HandleTransportEvent(Event) && Configuration.Role == GameSessionRole::Client)
					RecordClientService(Metrics.ClientRemoteMessagesHandled,
						Metrics.ClientRemoteMaximumServiceGapNanoseconds, LastClientRemoteService);
			} else {
				Reject(Received.Connection, DisconnectReason::ProtocolViolation, "Unknown game-session protocol frame");
			}
		}

		std::size_t Poll() {
			runtime_detail::WorkScope Work(runtime_detail::WorkPhase::SessionPoll);
			std::array<TransportEvent, MaximumGameSessionPollEventsPerCall> Buffer;
			const auto Count = runtime_detail::MeasureWork(runtime_detail::WorkPhase::TransportPoll,
				[&] { return Transport->PollEvents(Buffer); });
			runtime_detail::RecordWorkUnits(runtime_detail::WorkPhase::TransportPoll, Count);
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

		bool CanUseReplica(ConnectionId Connection, const ReplicationView &View, ObjectId Object) const {
			return Configuration.SpatialContent ? Replication->CanDeliverSpatialContent(Connection,Object) : View.Knows(Object);
		}

		void SynchronizeServerGraph() {
			runtime_detail::WorkScope Work(runtime_detail::WorkPhase::GraphSynchronization);
			if (!Runtime || !Authority || !Remotes || !Relevance) return;
			const auto Started = std::chrono::steady_clock::now();
			for (auto &[Connection, PeerValue] : Peers) {
				if (PeerValue.Phase != PeerPhase::Ready) continue;
				const auto *View = Replication->GetView(Connection);
				if (!View) continue;
				runtime_detail::CountWork(runtime_detail::WorkCounter::GraphPeers);
				std::set<ObjectId> DesiredRemotes;
				for (const auto Remote : ServerRemoteObjects) {
					runtime_detail::CountWork(runtime_detail::WorkCounter::GraphRemotes);
					if (CanUseReplica(Connection,*View,Remote)) DesiredRemotes.insert(Remote);
				}
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
				// Selection is speculative until its structural transaction is accepted.
				// Retain the previous materialized candidates until committed knowledge
				// removes their RootPart, otherwise cancelled Leaves advance only the
				// server's GCHR epoch and permanently stale-drop subsequent client state.
				auto CharacterCandidates = PeerValue.MaterializedCharacters;
				const auto SelectedCharacters = Relevance->GetRuntimeCharacterCandidates(Connection);
				CharacterCandidates.insert(SelectedCharacters.begin(), SelectedCharacters.end());
				for (const auto Character : CharacterCandidates) {
					runtime_detail::CountWork(runtime_detail::WorkCounter::GraphCharacters);
					runtime_detail::RecordWorkUnits(runtime_detail::WorkPhase::GraphSynchronization);
					// The typed list is only an inspection hint. Removal before this
					// peer's next 3E evaluation cannot restore an old registration.
					if (!ServerCharacters.contains(Character)) continue;
					auto CharacterValue = std::dynamic_pointer_cast<KinematicCharacter>(
						ObjectRegistry::Get().Lookup(Character)
					);
					auto RootPart = CharacterValue ? CharacterValue->GetRootPart() : std::nullopt;
					const bool Materialized = CanUseReplica(Connection,*View,Character) && RootPart &&
						CanUseReplica(Connection,*View,(*RootPart)->GetObjectId());
					runtime_detail::CountWork(runtime_detail::WorkCounter::GraphKnownChecks);
					if (!Materialized) runtime_detail::CountWork(runtime_detail::WorkCounter::GraphIrrelevant);
					runtime_detail::RecordWorkDisposition(runtime_detail::WorkPhase::GraphSynchronization, Materialized);
					if (Materialized)
						DesiredCharacters.insert(Character);
				}
				for (const auto Character : PeerValue.MaterializedCharacters)
					if (!DesiredCharacters.contains(Character)) {
						runtime_detail::CountWork(runtime_detail::WorkCounter::GraphChanges);
						if (!Authority->MarkUnmaterialized(Connection, Character, CurrentTick)) PeerHealthy = false;
						else if (PeerValue.ControlledCharacter == Character) {
							// MarkUnmaterialized revokes the native control lease. Clear the
							// session's mirror so the same Player.Character can rebind when
							// its RootPart becomes materialized again.
							PeerValue.ControlledCharacter = {};
							++Metrics.CharacterControlRevocations;
						}
					}
				for (const auto Character : DesiredCharacters)
					if (!PeerValue.MaterializedCharacters.contains(Character)) {
						runtime_detail::CountWork(runtime_detail::WorkCounter::GraphChanges);
						if (!Authority->MarkMaterialized(Connection, Character, CharacterStateChannel(Character))) PeerHealthy = false;
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

				auto CharacterValue = LegacyCharacter(PeerValue.PlayerValue);
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
				if (Desired.IsValid() && CanUseReplica(Connection,*View,Desired) && PeerValue.MaterializedCharacters.contains(Desired)) {
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
			SynchronizePossessionInput();
			Metrics.ServerGraphSynchronizationCpuNanoseconds += static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Started).count()
			);
		}

		std::optional<GameplayInputSample> ReadPossessionInput(const Player &PlayerValue, std::uint64_t Tick) {
			if (PlayerValue.GetDestroyed() || PlayerValue.IsDestroying()) return std::nullopt;
			const auto Connection = ConnectionsByPlayer.find(PlayerValue.GetObjectId());
			if (Connection == ConnectionsByPlayer.end() || PendingPeerFailures.contains(Connection->second))
				return std::nullopt;
			const auto PeerIterator = Peers.find(Connection->second);
			const auto Entity = GenericPossession(PlayerValue);
			if (PeerIterator == Peers.end() || PeerIterator->second.Phase != PeerPhase::Ready ||
				PeerIterator->second.PlayerValue.get() != &PlayerValue || !Entity)
				return std::nullopt;
			const auto Sample = ServerPossessionInput.Consume(Connection->second, Entity->GetObjectId(),
				Tick, PlayerValue.GetPossessionRevision());
			if (Sample && Sample->Fresh && runtime_detail::ActivePublicationLatency &&
				runtime_detail::ActivePublicationLatency->CapturePossession)
				runtime_detail::RecordPublicationLatency({.Stage = "PossessionConsumed", .Connection = Connection->second,
					.Object = Entity->GetObjectId(), .Tick = Tick, .Sequence = Sample->AcknowledgedInput.Value()});
			return Sample;
		}

		void SynchronizePossessionInput() {
			const auto Tick = std::max(CurrentTick, std::uint64_t{1});
			const auto Fail = [this](ConnectionId Connection) {
				PendingPeerFailures.try_emplace(Connection, DisconnectInfo{
					DisconnectReason::ResourceExhaustion, "[Gameplay:Possession] Control lease admission failed"});
			};
			const auto Target = [this](ConnectionId Connection, const Peer &PeerValue) -> ObjectId {
				if (PeerValue.Phase != PeerPhase::Ready || !PeerValue.PlayerValue ||
					PendingPeerFailures.contains(Connection)) return {};
				const auto Entity = GenericPossession(*PeerValue.PlayerValue);
				const auto *View = Replication->GetView(Connection);
				const auto Object = Entity ? Entity->GetObjectId() : ObjectId{};
				return Object.IsValid() && View && CanUseReplica(Connection,*View,Object) ? Object : ObjectId{};
			};
			// Retire old leases first so a transfer is independent of connection order.
			for (auto &[Connection, PeerValue] : Peers) {
				if (!PeerValue.ControlledPossession.IsValid()) continue;
				if (PeerValue.ControlledPossession == Target(Connection, PeerValue) &&
					PeerValue.ControlledPossessionRevision == PeerValue.PlayerValue->GetPossessionRevision())
					continue;
				if (!ServerPossessionInput.SetPossession(Connection, {}, Tick, PeerValue.Limits))
					Fail(Connection);
				PeerValue.ControlledPossession = {};
				PeerValue.ControlledPossessionRevision = 0;
			}
			for (auto &[Connection, PeerValue] : Peers) {
				if (PeerValue.ControlledPossession.IsValid()) continue;
				const auto Object = Target(Connection, PeerValue);
				if (!Object.IsValid()) continue;
				const auto Revision = PeerValue.PlayerValue->GetPossessionRevision();
				if (!ServerPossessionInput.SetPossession(Connection, Object, Tick, PeerValue.Limits, Revision)) {
					Fail(Connection);
					continue;
				}
				PeerValue.ControlledPossession = Object;
				PeerValue.ControlledPossessionRevision = Revision;
			}
		}

		bool MovePossessedPart(const Player &PlayerValue, std::uint64_t Tick, const CFrame &Transform) {
			runtime_detail::WorkScope Work(runtime_detail::WorkPhase::GameplayMotion);
			const auto Started = std::chrono::steady_clock::now();
			if (PlayerValue.GetDestroyed() || PlayerValue.IsDestroying() || !IsValidGameplayPoseTransform(Transform))
				return false;
			const auto Owner = PlayerValue.GetObjectId();
			const auto Connection = ConnectionsByPlayer.find(Owner);
			if (Connection == ConnectionsByPlayer.end() || PendingPeerFailures.contains(Connection->second))
				return false;
			const auto PeerIterator = Peers.find(Connection->second);
			if (PeerIterator == Peers.end() || PeerIterator->second.Phase != PeerPhase::Ready ||
				PeerIterator->second.PlayerValue.get() != &PlayerValue)
				return false;
			const auto Part = std::dynamic_pointer_cast<BasePart>(GenericPossession(PlayerValue));
			if (!IsIndependentPosePart(Part)) return false;
			const auto Object = Part->GetObjectId();
			const auto Revision = PlayerValue.GetPossessionRevision();
			const auto &PeerValue = PeerIterator->second;
			if (PeerValue.ControlledPossession != Object || PeerValue.ControlledPossessionRevision != Revision ||
				glm::distance(Part->GetCFrame().Position, Transform.Position) > Character::MaximumMotionTranslation)
				return false;
			if (Configuration.SpatialContent) {
				const auto Domain = WorldRoot::GetEffectiveWorld(*Part);
				const auto Proof = Domain ? Domain->SpatialMotionAdmission.lock() : nullptr;
				const glm::vec3 Extents(glm::length(Part->GetSize()) * 0.5f);
				if (!Proof || !(*Proof)(*Domain,Object,Part->GetCFrame().Position,Transform.Position,Extents)) return false;
			}
			if (Configuration.TerrainBootstrap) {
				const glm::vec3 Extents(glm::length(Part->GetSize()) * 0.5f);
				if (!TerrainMotionAllowed(PeerValue, Part->GetCFrame().Position, Extents) ||
					!TerrainMotionAllowed(PeerValue, Transform.Position, Extents)) return false;
			}
			const auto Existing = ActivePossessionPoses.find(Object);
			if (Existing != ActivePossessionPoses.end() && Existing->second.MovementTick >= Tick) return false;
			if (Existing == ActivePossessionPoses.end() && ActivePossessionPoses.size() >= MaximumGameSessionPeers)
				return false;
			ActivePossessionPoses.insert_or_assign(Object, ActivePose{Owner, Revision, Tick});
			runtime_detail::MeasureWork(runtime_detail::WorkPhase::GameplayPoseCommit,
				[&] { Part->SetSimulationCFrame(Transform); });
			SaturatingAdd(Metrics.PossessionMotionAccepted, 1);
			SaturatingAdd(Metrics.PossessionMotionNanoseconds, static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - Started).count()));
			return true;
		}

		bool QueuePossessionPose(ConnectionId Connection, const Peer &PeerValue, const GameplayPoseMessage &Message) {
			auto Intent = runtime_detail::MeasureWork(runtime_detail::WorkPhase::GameplayPoseEncode,
				[&] { return MakeGameplayPoseIntent(Connection, Message, PeerValue.Limits); });
			if (!Intent) return false;
			const auto Bytes = Intent->Payload().size();
			const auto Accepted = runtime_detail::MeasureWork(runtime_detail::WorkPhase::GameplayPoseEnqueue,
				[&] { return Scheduler.Submit(std::move(*Intent)).Accepted(); });
			runtime_detail::RecordWorkDisposition(runtime_detail::WorkPhase::GameplayPoseEnqueue, Accepted);
			if (!Accepted) return false;
			SaturatingAdd(Metrics.PossessionPoseBytesAccepted, Bytes);
			if (std::holds_alternative<GameplayPoseSnapshot>(Message))
				SaturatingAdd(Metrics.PossessionPoseSnapshotsAccepted, 1);
			return true;
		}

		void RetirePossessionPoseBinding(ConnectionId Connection, Peer &PeerValue, ObjectId Object) {
			auto Found = PeerValue.PossessionPoses.find(Object);
			if (Found == PeerValue.PossessionPoses.end()) return;
			if (!QueuePossessionPose(Connection, PeerValue, GameplayPoseControl{
				.Part = Object, .MaterializationEpoch = Found->second.Epoch, .Published = false})) {
				PendingPeerFailures.try_emplace(Connection, DisconnectInfo{
					DisconnectReason::ResourceExhaustion, "[Gameplay:Pose] Control retirement failed"});
			}
			PeerValue.PossessionPoses.erase(Found);
		}

		void PublishPossessionPoses(std::uint64_t Tick) {
			runtime_detail::WorkScope Work(runtime_detail::WorkPhase::GameplayPosePublish);
			if (ActivePossessionPoses.empty()) {
				const auto HasBindings = std::ranges::any_of(Peers, [](const auto &Entry) {
					return !Entry.second.PossessionPoses.empty();
				});
				if (!HasBindings) return;
			}
			for (auto Iterator = ActivePossessionPoses.begin(); Iterator != ActivePossessionPoses.end();) {
				const auto Part = std::dynamic_pointer_cast<BasePart>(ObjectRegistry::Get().Lookup(Iterator->first));
				const auto Owner = std::dynamic_pointer_cast<Player>(ObjectRegistry::Get().Lookup(Iterator->second.Player));
				if (IsIndependentPosePart(Part) && Owner && Owner->GetPossessionRevision() == Iterator->second.Revision &&
					GenericPossession(*Owner) == Part) {
					++Iterator;
					continue;
				}
				Iterator = ActivePossessionPoses.erase(Iterator);
				if (Part && !Part->GetDestroyed() && Part->GetDataModel() == Runtime->DataModel)
					Part->NotifyPropertyCommitted("CFrame");
			}
			for (auto &[Connection, PeerValue] : Peers) {
				if (PeerValue.Phase != PeerPhase::Ready || PendingPeerFailures.contains(Connection)) continue;
				const auto *View = Replication->GetView(Connection);
				if (!View) continue;
				const auto Owned = PeerValue.ControlledPossession;
				std::array<ObjectId, MaximumPossessionPosesPerPeer> Desired{};
				std::size_t Count = 0;
				{
					runtime_detail::WorkScope Selection(runtime_detail::WorkPhase::GameplayPoseRelevance);
					std::size_t Visited = 0;
					if (Owned.IsValid() && ActivePossessionPoses.contains(Owned) && CanUseReplica(Connection,*View,Owned))
						Desired[Count++] = Owned;
					for (const auto &[Object, Pose] : ActivePossessionPoses) {
						(void)Pose;
						if (Count == Desired.size()) break;
						++Visited;
						if (Object != Owned && CanUseReplica(Connection,*View,Object)) Desired[Count++] = Object;
					}
					runtime_detail::RecordWorkUnits(runtime_detail::WorkPhase::GameplayPoseRelevance, Visited);
				}
				const auto Selected = std::span(Desired).first(Count);
				for (auto Iterator = PeerValue.PossessionPoses.begin(); Iterator != PeerValue.PossessionPoses.end();) {
					const auto Object = (Iterator++)->first;
					if (std::ranges::find(Selected, Object) == Selected.end())
						RetirePossessionPoseBinding(Connection, PeerValue, Object);
				}
				if (PendingPeerFailures.contains(Connection)) continue;
				for (const auto Object : Selected) {
					auto Found = PeerValue.PossessionPoses.find(Object);
					if (Found == PeerValue.PossessionPoses.end()) {
						if (NextGameplayPoseEpoch == 0 || PeerValue.AcceptedStructuralSequence == 0 ||
							!QueuePossessionPose(Connection, PeerValue, GameplayPoseControl{
								.Part = Object, .MaterializationEpoch = NextGameplayPoseEpoch,
								.Published = true, .RequiredStructuralSequence = PeerValue.AcceptedStructuralSequence})) {
							PendingPeerFailures.try_emplace(Connection, DisconnectInfo{
								DisconnectReason::ResourceExhaustion, "[Gameplay:Pose] Control publication failed"});
							break;
						}
						// Publish control immediately. Align only the initial snapshot
						// so a peer has a stable phase without extra snapshots.
						const auto Phase = Connection.Slot % PossessionPoseIntervalTicks;
						const auto Delay = (Phase + PossessionPoseIntervalTicks - Tick % PossessionPoseIntervalTicks) %
							PossessionPoseIntervalTicks;
						const auto FirstDueTick = Tick + std::min(Delay, std::numeric_limits<std::uint64_t>::max() - Tick);
						Found = PeerValue.PossessionPoses.emplace(Object,
							Peer::PoseBinding{.Epoch = NextGameplayPoseEpoch++, .FirstDueTick = FirstDueTick}).first;
					}
					auto &Binding = Found->second;
					if (Binding.LastSentTick == 0) {
						if (Tick < Binding.FirstDueTick) continue;
					} else if (Tick <= Binding.LastSentTick || Tick - Binding.LastSentTick < PossessionPoseIntervalTicks)
						continue;
					const auto Part = std::dynamic_pointer_cast<BasePart>(ObjectRegistry::Get().Lookup(Object));
					if (Part && QueuePossessionPose(Connection, PeerValue, GameplayPoseSnapshot{
						.Part = Object, .MaterializationEpoch = Binding.Epoch, .ServerTick = Tick,
						.Transform = Part->GetCFrame(), .Teleport = Binding.LastSentTick == 0}))
						Binding.LastSentTick = Tick;
				}
			}
		}

		void HandlePossessionPose(const ReceivedMessageEvent &Received) {
			runtime_detail::WorkScope Work(runtime_detail::WorkPhase::GameplayPoseReceive);
			auto Message = runtime_detail::MeasureWork(runtime_detail::WorkPhase::GameplayPoseDecode,
				[&] { return DecodeGameplayPose(Received); });
			if (!Message) return;
			if (const auto *Control = std::get_if<GameplayPoseControl>(&*Message)) {
				if (!ClientPosePresentation.ApplyControl(*Control)) return;
				if (auto Found = ClientPossessionPoses.find(Control->Part); Found != ClientPossessionPoses.end()) {
					if (const auto Part = Found->second.ReplicaPart.lock(); Part && !Part->GetDestroyed())
						Part->SetCharacterPresentationOffset(std::nullopt);
					ClientPossessionPoses.erase(Found);
				}
				if (Control->Published)
					ClientPossessionPoses.emplace(Control->Part, ClientPoseBinding{Control->RequiredStructuralSequence});
			} else {
				const auto Accepted = ClientPosePresentation.Accept(std::get<GameplayPoseSnapshot>(*Message),
					std::max(CurrentTick, std::uint64_t{1}));
				runtime_detail::RecordWorkDisposition(runtime_detail::WorkPhase::GameplayPoseReceive, Accepted);
			}
		}

		void UpdatePossessionPresentation(std::uint64_t Tick) {
			runtime_detail::WorkScope Work(runtime_detail::WorkPhase::GameplayPosePresentation);
			runtime_detail::RecordWorkUnits(runtime_detail::WorkPhase::GameplayPosePresentation, ClientPossessionPoses.size());
			for (auto &[Object, Binding] : ClientPossessionPoses) {
				const auto StructuralSequence = Replica.GetNextSequence().Value();
				if (StructuralSequence <= Binding.RequiredStructuralSequence) continue;
				const auto StructuralEpoch = Replica.GetEpoch();
				auto Part = Binding.ResolvedAtSequence == StructuralSequence && Binding.ResolvedAtEpoch == StructuralEpoch
					? Binding.ReplicaPart.lock() : nullptr;
				if (!Part) Part = std::dynamic_pointer_cast<BasePart>(Replica.Resolve(Object));
				if (!IsIndependentPosePart(Part)) {
					if (const auto Previous = Binding.ReplicaPart.lock(); Previous && !Previous->GetDestroyed())
						Previous->SetCharacterPresentationOffset(std::nullopt);
					Binding.ReplicaPart.reset();
					continue;
				}
				Binding.ReplicaPart = Part;
				Binding.ResolvedAtSequence = StructuralSequence;
				Binding.ResolvedAtEpoch = StructuralEpoch;
				if (const auto Latest = ClientPosePresentation.GetLatest(Object); Latest && Latest->ServerTick > Binding.AppliedTick) {
					Part->SetSimulationCFrame(Latest->Transform);
					Binding.AppliedTick = Latest->ServerTick;
					SaturatingAdd(Metrics.ClientPossessionPoseSnapshotsApplied, 1);
					if (PrimaryConnection && runtime_detail::ActivePublicationLatency &&
						runtime_detail::ActivePublicationLatency->CapturePossession)
						runtime_detail::RecordPublicationLatency({.Stage = "PoseApplied", .Connection = *PrimaryConnection,
							.Object = Object, .Tick = Latest->ServerTick, .Sequence = Latest->ServerTick,
							.Epoch = Latest->MaterializationEpoch});
				}
				if (const auto RenderPose = ClientPosePresentation.Sample(Object, Tick))
					Part->SetCharacterPresentationOffset(Part->GetCFrame().ToObjectSpace(*RenderPose));
			}
		}

		bool SynchronizeClientGraph() {
			if (!Prediction || !Remotes || !PrimaryConnection) return false;
			std::set<ObjectId> DesiredCharacters;
			for (const auto Object : ClientCharacterObjects) {
				auto CharacterValue = std::dynamic_pointer_cast<KinematicCharacter>(Replica.Resolve(Object));
				// Replicated-service Characters are shared templates, not runtime
				// spatial candidates in the authoritative relevance graph. Keep
				// their structural replica without advancing the peer GCHR epoch.
				if (CharacterValue && CharacterValue->GetRootPart() &&
					GetInstanceServicePolicy(*CharacterValue) != SchemaServicePolicy::Replicated)
					DesiredCharacters.insert(Object);
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

		bool IsPooled() const { return Configuration.ReliableService && Configuration.ReliableService->IsPooled(); }

		void RecordCausalDelivery(ConnectionId Connection, Peer &PeerValue,
			const std::optional<detail::ReliableServiceFeedback> &Sample, bool Valid) const {
			if (!detail::ActiveStructuralCausalEvidence || !ByteAdmission || !ByteAdmission->Debt(Connection)) return;
			// A pending native result leaves this observation incomplete.  Keep the
			// last causal delivery until a complete snapshot can advance it.
			if (Valid && !Sample) return;
			const auto Before = PeerValue.CausalAcceptedBefore;
			Valid = Valid && Sample && Sample->CountersValid && Sample->Connection == Connection &&
				PeerValue.CausalGrantToken == ByteAdmission->DebtToken(Connection) &&
				PeerValue.CausalGrantBytes == ByteAdmission->Debt(Connection) &&
				Sample->StructuralPayloadBytesAcked >= Before &&
				Sample->StructuralPayloadBytesAcked <= Sample->StructuralPayloadBytesFirstSent &&
				Sample->StructuralPayloadBytesFirstSent - Before <= PeerValue.CausalGrantBytes;
			const auto FirstSent = Valid ? Sample->StructuralPayloadBytesFirstSent - Before : 0;
			const auto Acked = Valid ? Sample->StructuralPayloadBytesAcked - Before : 0;
			if (Valid && PeerValue.CausalDeliveryObserved && PeerValue.CausalLastFirstSent == FirstSent &&
				PeerValue.CausalLastAcked == Acked) return;
			PeerValue.CausalDeliveryObserved = Valid;
			PeerValue.CausalLastFirstSent = FirstSent;
			PeerValue.CausalLastAcked = Acked;
			detail::RecordStructuralCausal({.Kind = detail::StructuralCausalKind::Delivery,
				.Connection = Connection, .SourceScope = Runtime ? Runtime->DataModel->GetObjectId() : ObjectId{},
				.Sequence = PeerValue.CausalGrantSequence, .GrantToken = ByteAdmission->DebtToken(Connection),
				.CompleteBytes = ByteAdmission->Debt(Connection),
				.FirstSent = FirstSent, .Acked = Acked,
				.AcceptedBefore = Before, .Valid = Valid, .Fingerprint = PeerValue.CausalGrantFingerprint});
		}
		bool RetireStructural(ConnectionId Connection, std::uint64_t Token, std::uint64_t Bytes) {
			if (!ByteAdmission->Retire(Connection, Token, Bytes)) return false;
			detail::RecordStructuralCausal({.Kind = detail::StructuralCausalKind::Retired,
				.Connection = Connection, .SourceScope = Runtime ? Runtime->DataModel->GetObjectId() : ObjectId{},
				.GrantToken = Token, .CompleteBytes = Bytes});
			return true;
		}
		bool ReturnStructuralNativeCapacity(ConnectionId Connection,
			const detail::PooledReliableServiceFeedback::Result &Result) {
			const auto Now = ServiceTime();
			return !Result.NativeCompletedToken || (ByteAdmission->CompleteNativeSend(Connection,
				Result.NativeCompletedToken, Result.NativeCompletedBytes, Result.ObservedAtMicroseconds, Now,
				Result.NativeCompletedAtMicroseconds) &&
				Peers.at(Connection).ReliableFeedback.CompleteNativeCapacity(Result, Now));
		}

		bool UpdateOrdinaryFunding() {
			std::uint64_t Debt = 0, Lower = 0, Upper = 0;
			for (auto &[Id, Value] : Peers) {
				auto Accepted = detail::ReliableServiceFeedbackAccess::Accepted(Scheduler, Id);
				if (!Accepted && Value.Phase == PeerPhase::TransportConnected) Accepted = detail::ReliableServiceAcceptedBytes{};
				if (!Accepted) return false;
				const auto Updated = Value.ReliableFeedback.Observe(Id, *Accepted, {}, ServiceTime(),
					ByteAdmission->DebtToken(Id), ByteAdmission->Debt(Id));
				if (!Updated.Valid || Value.ReliableFeedback.OrdinaryDebt > std::numeric_limits<std::uint64_t>::max() - Debt)
					return false;
				if (!ByteAdmission->SetOrdinaryUnsent(Id, Value.ReliableFeedback.OrdinaryUnsent)) return false;
				Debt += Value.ReliableFeedback.OrdinaryDebt;
				Lower += Value.ReliableFeedback.GameplayLower;
				Upper += Value.ReliableFeedback.GameplayUpper;
			}
			const auto Funded = detail::PooledReliableServiceFeedback::FundedOrdinary(Debt, Lower, Upper);
			if (!Funded) return false;
			ByteAdmission->SetOrdinaryFunding(*Funded);
			return true;
		}
		void RecordPooledExecutionWithoutAcceptance(ConnectionId Connection, const detail::PooledReliableServiceFeedback &Ledger,
			detail::PooledObservationPoint Point, detail::PooledExecutionState Before, std::uint64_t Now, bool WorkReady) {
			const auto *Sink = detail::ActivePooledService;
			if (!Sink || !Sink->CaptureExecution || !Sink->Record) return;
			// No synthetic accounting observation is forwarded to qualification sinks.
			detail::PooledServiceRecord Record;
			Record.Connection = Connection; Record.SimulationTick = CurrentTick; Record.NowMicroseconds = Now;
			Record.Point = Point; Record.ExecutionBefore = Before; Record.ExecutionRejected = true;
			Record.InvalidAfter = Ledger.Invalid; Record.WorkReady = WorkReady;
			Record.AcceptanceAvailable = false; Record.DiagnosticOnly = true;
			Record.DebtToken = ByteAdmission->DebtToken(Connection); Record.DebtBytes = ByteAdmission->Debt(Connection);
			Record.Result.ExecutionFailed = Ledger.ExecutionFailed;
			Record.Result.MainOpportunityFailed = Ledger.MainOpportunityFailed; Record.Result.TurnoverFailed = Ledger.TurnoverFailed;
			Record.Result.MaximumMainOpportunityMicroseconds = Ledger.MaximumMainOpportunityMicroseconds;
			Record.Result.MaximumTurnoverMicroseconds = Ledger.MaximumTurnoverMicroseconds;
			Sink->Record(Sink->Context, Record);
		}
		void RecordPooledObservation(ConnectionId Connection, const detail::ReliableServiceAcceptedBytes &Accepted,
			const std::optional<detail::ReliableServiceFeedback> &Sample,
			const detail::PooledReliableServiceFeedback::Result &Result, std::uint64_t Now,
			detail::PooledObservationPoint Point = detail::PooledObservationPoint::Other,
			detail::PooledExecutionState Before = {}, bool ExecutionRejected = false, bool WorkReady = false) {
			if (const auto *Sink = detail::ActivePooledService; Sink && Sink->Record) {
				if (Sink->CaptureExecution && !Before.Captured && Peers.contains(Connection))
					Before = detail::CapturePooledExecution(Peers.at(Connection).ReliableFeedback);
				Sink->Record(Sink->Context, detail::PooledServiceRecord{
					.Connection = Connection, .SimulationTick = CurrentTick, .NowMicroseconds = Now,
					.DebtToken = ByteAdmission->DebtToken(Connection), .DebtBytes = ByteAdmission->Debt(Connection),
					.StructuralJournalLag = Replication ? Replication->GetJournalLag(Connection) : 0,
					.Accepted = Accepted, .Feedback = Sample, .Result = Result,
					.Admission = ByteAdmission->GetMetrics(), .Point = Point,
					.ExecutionBefore = Before, .ExecutionRejected = ExecutionRejected,
					.InvalidAfter = Sink->CaptureExecution && Peers.contains(Connection) && Peers.at(Connection).ReliableFeedback.Invalid,
					.WorkReady = WorkReady});
			}
		}

		void TearDownPeer(ConnectionId Connection) {
			RecordPooledLifecycle(detail::PooledLifecyclePoint::PeerTeardown, Connection, DisconnectReason::LocalShutdown);
			auto Iterator = Peers.find(Connection);
			if (IsPooled()) {
				// Closing is not a release. Purge first, reconcile the immutable
				// old-generation receipt, cancel unsent work, then release the slot.
				(void)Transport->Disconnect(Connection, {DisconnectReason::LocalShutdown, "Session peer teardown"});
				const auto Final = detail::ReliableServiceFeedbackAccess::Observe(*Transport, Connection);
				if (Iterator != Peers.end()) {
					auto Accepted = detail::ReliableServiceFeedbackAccess::Accepted(Scheduler, Connection);
					if (!Accepted && Iterator->second.Phase == PeerPhase::TransportConnected) Accepted = detail::ReliableServiceAcceptedBytes{};
					if (!Final || Final->State != ConnectionState::Closed || !Accepted) {
						PendingSessionFailure = DisconnectInfo{DisconnectReason::TransportFailure,
							"[Network:PooledService] Terminal purge evidence unavailable; debt retained"};
						return;
					}
					const auto Last = Iterator->second.ReliableFeedback.Observe(Connection, *Accepted, Final, ServiceTime(),
						ByteAdmission->DebtToken(Connection), ByteAdmission->Debt(Connection));
					RecordCausalDelivery(Connection, Iterator->second, Final, Last.Valid);
					RecordPooledObservation(Connection, *Accepted, Final, Last, ServiceTime(), detail::PooledObservationPoint::PeerTeardown);
					if (Last.Valid && Last.RetiredBytes)
						(void)RetireStructural(Connection, Last.RetiredToken, Last.RetiredBytes);
					(void)Scheduler.CancelConnection(Connection);
					(void)ByteAdmission->TerminalRelease(Connection);
				}
				(void)detail::ReliableServiceFeedbackAccess::Release(*Transport, Connection);
			}
			if (Iterator == Peers.end()) return;
			auto PeerValue = std::move(Iterator->second);
			Peers.erase(Iterator);
			if (PeerValue.TerrainPending && PeerValue.TerrainPending->Sending)
				PeerValue.TerrainPending->Sending->Stop.request_stop();
			ConnectionsByPlayer.erase(PeerValue.PlayerObject);
			ServerPossessionInput.RemovePeer(Connection);
			if (ByteAdmission) ByteAdmission->Remove(Connection);
			if (Peers.empty()) {
				StructuralFairnessCursor.reset();
				StructuralJournalFairnessCursor.reset();
				StructuralConnectionsScratch.clear();
			}
			if (Authority) (void)Authority->RemovePeer(Connection, std::max(CurrentTick, std::uint64_t{1}));
			if (Prediction) (void)Prediction->RemovePeer(Connection);
			if (Remotes) (void)Remotes->RemovePeer(Connection);
			if (Relevance) (void)Relevance->RemovePeer(Connection);
			if (Replication) (void)Replication->RemovePeer(Connection);
			(void)Scheduler.CancelConnection(Connection);
			if (Configuration.Role == GameSessionRole::Server && PeerValue.PlayerValue) {
				if (auto *Policy = Runtime->GetResidencyDemandPolicy()) {
					const auto PlayerObject = PeerValue.PlayerValue->GetObjectId();
					(void)Policy->RemoveFocus((static_cast<std::uint64_t>(PlayerObject.Generation) << 32) | PlayerObject.Slot);
				}
				if (Runtime->Players->RemoveSessionPlayer(PeerValue.PlayerValue)) ++Metrics.PlayersRemoved;
			} else if (Configuration.Role == GameSessionRole::Client) {
				ClientPossessionInput.Reset();
				ClientPosePresentation.Clear();
				for (const auto &[Object, Binding] : ClientPossessionPoses) {
					(void)Object;
					if (const auto Part = Binding.ReplicaPart.lock(); Part && !Part->GetDestroyed())
						Part->SetCharacterPresentationOffset(std::nullopt);
				}
				ClientPossessionPoses.clear();
				if (Runtime) Runtime->Players->ClearTrustedLocalPlayer();
				if (const auto Owner = std::dynamic_pointer_cast<Player>(Replica.Resolve(PeerValue.PlayerObject))) {
					if (const auto Gui = Owner->GetPlayerGui().value_or(nullptr)) {
						try {
							Gui->Destroy();
						} catch (const std::exception &Error) {
							LOG_WARN(App, "[Gui:Provision] Disconnect GUI teardown failed: %.160s", Error.what());
						} catch (...) {
							LOG_WARN(App, "[Gui:Provision] Disconnect GUI teardown failed");
						}
					}
				}
				Replica.Reset();
				BaselineApplied = false;
				ClientCharacterObjects.clear();
				ClientMaterializedCharacters.clear();
				ClientRemoteObjects.clear();
				ClientKnownObjects.clear();
				PendingClientGuiCode.clear();
				ClientGuiCharacter = {};
				PrimaryConnection.reset();
			}
		}

		void ObserveOrdinaryQueue(ConnectionId Connection, Peer &PeerValue, std::uint64_t Now) {
			const auto Oldest = detail::ReliableServiceFeedbackAccess::OldestProtectedOrdinary(Scheduler, Connection);
			const bool Failed = !Oldest || (*Oldest && (Now < *Oldest ||
				Now - *Oldest > PooledReliableServiceProfile::QueueWindowMicroseconds));
			const auto EvidenceOldest = detail::ReliableServiceFeedbackAccess::OldestQualifiedOrdinary(Scheduler, Connection);
			const bool EvidenceFailed = !EvidenceOldest || (*EvidenceOldest && (Now < *EvidenceOldest ||
				Now - *EvidenceOldest > PooledReliableServiceProfile::QueueWindowMicroseconds));
			auto &Ledger = PeerValue.ReliableFeedback;
			// Missing/retrograde queue evidence is correctness, independently of
			// whether the ordinary latency target is enforced by this profile.
			if (!Oldest || (*Oldest && Now < *Oldest)) Ledger.Invalid = true;
			Ledger.QueuedOrdinaryFailed = Failed;
			if (Failed) Ledger.QueuedOrdinaryEverFailed = true;
			if (EvidenceFailed && !Ledger.QueuedQualifiedOrdinaryEverFailed) {
				Ledger.QueuedQualifiedOrdinaryEverFailed = true;
				SaturatingAdd(Metrics.QualifiedOrdinaryQueueFailures, 1);
			}
			if (IsPooled()) {
				if (const auto Accepted = detail::ReliableServiceFeedbackAccess::Accepted(Scheduler, Connection)) {
					const auto Result = Ledger.Observe(Connection, *Accepted, {}, Now,
						ByteAdmission->DebtToken(Connection), ByteAdmission->Debt(Connection));
					RecordPooledObservation(Connection, *Accepted, {}, Result, Now);
				}
			}
		}
		void Step(std::uint64_t SimulationTick) {
			runtime_detail::WorkScope Work(runtime_detail::WorkPhase::SessionStep);
			if (Status == GameSessionStatus::Created || Status == GameSessionStatus::Starting ||
				Status == GameSessionStatus::Closing || Status == GameSessionStatus::Closed ||
				Status == GameSessionStatus::Failed)
				return;
			if (!DrainFailures()) return;
			// PlayerHost switches from its bootstrap counter to Engine ticks at runtime attachment.
			if (Configuration.TerrainBootstrap)
				SaturatingAdd(TerrainTick, SimulationTick > CurrentTick ? SimulationTick - CurrentTick : std::uint64_t{1});
			CurrentTick = SimulationTick;
			// Residency consumes committed server Character facts, independently of
			// 3E relevance and 3J accepted materialization. Full Player ObjectId
			// generations scope focus lifetime across disconnect/session replacement.
			if (Configuration.Role == GameSessionRole::Server && Runtime) {
				if (auto *Policy = Runtime->GetResidencyDemandPolicy()) {
					for (const auto &[Connection, PeerValue] : Peers) {
						if (!PeerValue.PlayerValue) continue;
						const auto PlayerObject = PeerValue.PlayerValue->GetObjectId();
						const auto Focus = (static_cast<std::uint64_t>(PlayerObject.Generation) << 32) | PlayerObject.Slot;
						const auto CharacterValue = PeerValue.PlayerValue->GetCharacter().value_or(nullptr);
						if (PeerValue.Phase != PeerPhase::Ready || !CharacterValue || CharacterValue->GetDestroyed() ||
							CharacterValue->IsDestroying() || CharacterValue->GetDataModel() != Runtime->DataModel) {
							(void)Policy->RemoveFocus(Focus);
							continue;
						}
						const auto Kinematic = std::dynamic_pointer_cast<KinematicCharacter>(CharacterValue);
						if (!Policy->UpdateFocus(Focus, CharacterValue->GetCFrame().Position,
							Kinematic ? Kinematic->GetVelocity() : glm::vec3(0.0f)))
							(void)Policy->RemoveFocus(Focus);
					}
				}
			}
			for (auto &[Connection, PeerValue] : Peers)
				if (PeerValue.Phase == PeerPhase::Ready) ObserveOrdinaryQueue(Connection, PeerValue, ServiceTime());
			if (IsPooled() && Replication) {
				// Arm before reconciliation, relevance, planning and encoding. The
				// endpoint below certifies a completed pass, never its entry alone.
				for (auto &[Connection, PeerValue] : Peers) {
					if (PeerValue.Phase != PeerPhase::Ready) continue;
					const bool WorkReady = Replication->HasPendingRelevance(Connection) ||
						Replication->IsPlanningReady(Connection) || Replication->GetJournalLag(Connection) != 0;
					const auto OpportunityAt = ServiceTime();
					const auto ExecutionBefore = detail::ActivePooledService && detail::ActivePooledService->CaptureExecution
						? detail::CapturePooledExecution(PeerValue.ReliableFeedback) : detail::PooledExecutionState{};
					if (!PeerValue.ReliableFeedback.BeginAdmissionOpportunity(OpportunityAt, WorkReady)) {
						if (const auto Accepted = detail::ReliableServiceFeedbackAccess::Accepted(Scheduler, Connection)) {
							const auto Result = PeerValue.ReliableFeedback.Observe(Connection, *Accepted, {}, OpportunityAt,
								ByteAdmission->DebtToken(Connection), ByteAdmission->Debt(Connection));
							RecordPooledObservation(Connection, *Accepted, {}, Result, OpportunityAt,
								detail::PooledObservationPoint::AdmissionBegin, ExecutionBefore, true, WorkReady);
						} else RecordPooledExecutionWithoutAcceptance(Connection, PeerValue.ReliableFeedback,
							detail::PooledObservationPoint::AdmissionBegin, ExecutionBefore, OpportunityAt, WorkReady);
						PendingPeerFailures.try_emplace(Connection, DisconnectInfo{DisconnectReason::TransportFailure,
							"[Network:PooledService] Structural execution opportunity exceeded its product budget"});
					}
				}
				if (!DrainFailures()) return;
			}
			if (Replication) Replication->ProcessCatalogRetirement(SimulationTick);
			for (auto &[Connection, PeerValue] : Peers) {
				(void)Connection;
				PeerValue.SendAllowance.Reset();
				PeerValue.StructuralSubmittedThisStep = false;
				PeerValue.ByteDeferredThisStep = false;
				PeerValue.StructuralTransitionsConsumedThisStep = 0;
				PeerValue.JournalRecordsConsumedThisStep = 0;
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
					if (!Relevance->SetOwnerCharacter(Connection, OwnerSpatialRoot(PeerValue.PlayerValue, *Relevance)))
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
				if (SpatialValidationRequested) {
					SpatialValidationRequested = false;
					SpatialValidationPassed = Relevance->VerifySpatialIndex();
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
						auto Flushed = PeerValue.SendAllowance.Flush(Scheduler, Connection, PeerValue.Limits);
						if (auto Failure = SchedulerFlushFailure(Flushed)) {
							PendingPeerFailures.try_emplace(Connection, std::move(*Failure));
							continue;
						}
					}
					if (!Relevance->WasSelectionEvaluated(Connection)) continue;
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
					auto Recorded = Replication->RequestPlanning(Connection, Relevance->GetSelectionSnapshot(Connection), SimulationTick);
					if (!Recorded.Succeeded())
						PendingPeerFailures.try_emplace(
							Connection,
							DisconnectInfo{
								DisconnectReason::ResourceExhaustion,
								"[Replication:StructuralScheduler] " + Recorded.Error,
							}
						);
				}
				if (!DrainFailures()) return;

				if (ByteAdmission) {
					if (!ByteAdmission->BeginStep(ServiceTime())) {
						FailSession({DisconnectReason::ResourceExhaustion, "Reliable byte admission clock/ownership failed"});
						return;
					}
					// Snapshot the whole admitted connection envelope, including bootstrap.
					// Scheduler bytes have not reached the provider: count both, not either.
					for (const auto &[Connection, PeerValue] : Peers) {
						(void)PeerValue;
						if (IsPooled()) {
							auto Accepted = detail::ReliableServiceFeedbackAccess::Accepted(Scheduler, Connection);
							if (!Accepted && PeerValue.Phase == PeerPhase::TransportConnected) Accepted = detail::ReliableServiceAcceptedBytes{};
							const auto Sample = detail::ReliableServiceFeedbackAccess::Observe(*Transport, Connection);
							if (!Accepted) {
								FailSession({DisconnectReason::ResourceExhaustion, "Pooled acceptance accounting unavailable"}); return;
							}
							const auto ObservedNow = ServiceTime();
							const auto Result = Peers.at(Connection).ReliableFeedback.Observe(Connection, *Accepted, Sample,
								ObservedNow, ByteAdmission->DebtToken(Connection), ByteAdmission->Debt(Connection));
							RecordCausalDelivery(Connection, Peers.at(Connection), Sample, Result.Valid);
							RecordPooledObservation(Connection, *Accepted, Sample, Result, ObservedNow);
							if (!Result.Valid || !ReturnStructuralNativeCapacity(Connection, Result) || (Result.RetiredBytes &&
								!RetireStructural(Connection, Result.RetiredToken, Result.RetiredBytes)) || Result.Terminal) {
								PendingPeerFailures.try_emplace(Connection, DisconnectInfo{DisconnectReason::TransportFailure,
									Result.Terminal ? "[Network:PooledService] Native connection terminated before event delivery" :
									"[Network:PooledService] Contradictory native service feedback"});
							}
							if (!ByteAdmission->ObserveService(Connection, detail::ReliableByteAdmission::ServiceObservation{
								.ObservedAtMicroseconds = Result.ObservedAtMicroseconds, .Qualified = Result.Qualified,
								.Available = Result.Valid && Result.Available,
								.OrdinaryDebt = Peers.at(Connection).ReliableFeedback.OrdinaryDebt})) {
								FailSession({DisconnectReason::ResourceExhaustion, "Pooled generation envelope failed"}); return;
							}
							continue;
						}
						const auto Backend = Transport->GetStatistics(Connection);
						const auto Queued = Scheduler.GetStatistics(Connection);
						std::optional<std::uint64_t> Exposure;
						if (Backend && Backend->QueuedReliableBytes && Queued) {
							Exposure = *Backend->QueuedReliableBytes;
							SaturatingAdd(*Exposure, Queued->QueuedReliableBytes);
							SaturatingAdd(*Exposure, Queued->QueuedReliableMessages * ReliableServiceEnvelopeBytes);
						}
						if (!ByteAdmission->Observe(Connection, Exposure)) {
							FailSession({DisconnectReason::ResourceExhaustion, "Reliable byte admission connection envelope failed"});
							return;
						}
					}
					if (IsPooled() && (!DrainFailures() || !UpdateOrdinaryFunding())) {
						FailSession({DisconnectReason::TransportFailure, "Pooled aggregate funding accounting failed"}); return;
					}
				}
				Replication->ProcessPlanning(SimulationTick);
				auto &StructuralConnections = StructuralConnectionsScratch;
				StructuralConnections.clear();
				if (StructuralConnections.capacity() < Peers.size()) StructuralConnections.reserve(Peers.size());
				bool HasPendingBootstrap = false;
				for (const auto &[Connection, PeerValue] : Peers) {
					if (PeerValue.Phase != PeerPhase::TransportConnected) StructuralConnections.push_back(Connection);
					if (PeerValue.Phase != PeerPhase::TransportConnected)
						runtime_detail::MaximumWork(runtime_detail::WorkCounter::JournalLagBeforeServiceMaximum,
							Replication->GetJournalLag(Connection));
					HasPendingBootstrap = HasPendingBootstrap || PeerValue.Phase == PeerPhase::BootstrapPending;
				}
				std::optional<ConnectionId> JournalStartConnection;
				if (IsPooled() && !StructuralConnections.empty() && StructuralJournalFairnessCursor) {
					const auto Found = std::lower_bound(StructuralConnections.begin(), StructuralConnections.end(),
						*StructuralJournalFairnessCursor);
					JournalStartConnection = Found == StructuralConnections.end() ? StructuralConnections.front() : *Found;
				}
				if (!StructuralConnections.empty()) {
					std::size_t Start = 0;
					if (StructuralFairnessCursor) {
						auto Found = std::lower_bound(
							StructuralConnections.begin(), StructuralConnections.end(), *StructuralFairnessCursor
						);
						Start = Found == StructuralConnections.end()
									? 0
									: static_cast<std::size_t>(Found - StructuralConnections.begin());
					}
					std::rotate(
						StructuralConnections.begin(),
						StructuralConnections.begin() + static_cast<std::ptrdiff_t>(Start),
						StructuralConnections.end()
					);
					if (Start != 0) Replication->RecordPeerFairnessRotation();
				}

				std::optional<ConnectionId> LastServicedConnection;
				std::optional<ConnectionId> LastGrantedConnection;
				std::optional<ConnectionId> LastJournalServicedConnection;
				std::size_t GlobalConsumed = 0;
				std::size_t GlobalJournalConsumed = 0;
				bool MadeProgress = false;
				auto RefreshStructuralService = [&](ConnectionId Connection, Peer &PeerValue, bool BeforeEligibility) {
					if (!IsPooled()) return true;
					// The old loop did not reconcile a just-submitted peer again until
					// the next step. Preserve that behavior and its lifecycle timestamps;
					// this refresh must not introduce another grant for that peer here.
					if (BeforeEligibility && PeerValue.StructuralSubmittedThisStep) return true;
					const auto Now = ServiceTime();
					const auto &Previous = PeerValue.ReliableFeedback.Previous;
					if (Previous && Previous->ObservedAtMicroseconds <= Now &&
						Now - Previous->ObservedAtMicroseconds <= Configuration.ReliableService->Pooled.FeedbackFreshnessMicroseconds)
						return true;
					// Query native evidence, never retimestamp the earlier envelope.
					// Relevance/planning or a preceding peer's encoding may age it before
					// eligibility; this same reconciliation also protects acceptance.
					const auto Accepted = detail::ReliableServiceFeedbackAccess::Accepted(Scheduler, Connection);
					if (!Accepted) {
						PendingPeerFailures.try_emplace(Connection, DisconnectInfo{DisconnectReason::TransportFailure,
							"[Network:PooledService] Acceptance accounting disappeared during preparation"}); return false;
					}
					const auto Sample = detail::ReliableServiceFeedbackAccess::Observe(*Transport, Connection);
					const auto ObservedNow = ServiceTime();
					const auto Result = PeerValue.ReliableFeedback.Observe(Connection, *Accepted, Sample,
						ObservedNow, ByteAdmission->DebtToken(Connection), ByteAdmission->Debt(Connection));
					RecordCausalDelivery(Connection, PeerValue, Sample, Result.Valid);
					RecordPooledObservation(Connection, *Accepted, Sample, Result, ObservedNow);
					if (!Result.Valid || !ReturnStructuralNativeCapacity(Connection, Result) || (Result.RetiredBytes &&
						!RetireStructural(Connection, Result.RetiredToken, Result.RetiredBytes)) || Result.Terminal) {
						PendingPeerFailures.try_emplace(Connection, DisconnectInfo{DisconnectReason::TransportFailure,
							"[Network:PooledService] Native evidence failed during preparation"}); return false;
					}
					if (!ByteAdmission->RefreshService(Connection, detail::ReliableByteAdmission::ServiceObservation{
						Result.ObservedAtMicroseconds, Result.Qualified, Result.Available, PeerValue.ReliableFeedback.OrdinaryDebt})) {
						PendingPeerFailures.try_emplace(Connection, DisconnectInfo{DisconnectReason::TransportFailure,
							"[Network:PooledService] Structural service evidence could not refresh"}); return false;
					}
					if (!UpdateOrdinaryFunding()) {
						PendingSessionFailure = DisconnectInfo{DisconnectReason::TransportFailure,
							"Pooled ordinary funding became invalid"}; return false;
					}
					return true;
				};
				auto SubmitStructural =
					[&](ConnectionId Connection, Peer &PeerValue, ReplicationProduceResult &Produced) {
						std::optional<detail::ReliableByteAdmission::Reservation> Receipt;
						if (ByteAdmission) {
							if (!RefreshStructuralService(Connection, PeerValue, false)) return false;
							if (IsPooled()) {
								// The encoded demand and its immediate eligibility check share
								// one timestamp. Sampling twice can put eligibility before the
								// exact demand by one microsecond in the evidence stream.
								const auto AdmissionAt = ServiceTime();
								(void)ByteAdmission->Allowance(Connection, AdmissionAt);
								ByteAdmission->ObserveExactDemand(Connection,
									Produced.EncodedFrame.size() + ReliableServiceEnvelopeBytes, AdmissionAt,
									Produced.DiagnosticFingerprint);
							}
							Receipt = ByteAdmission->Reserve(Connection, Produced.EncodedFrame.size() + ReliableServiceEnvelopeBytes);
							if (!Receipt) {
								if (IsPooled() && Replication->DiscardSchedulerPreparation(Connection, Produced.Frame->Sequence).Succeeded()) {
									ByteAdmission->DeferSize(Connection, Produced.EncodedFrame.size() + ReliableServiceEnvelopeBytes,
										Produced.DiagnosticFingerprint);
									PeerValue.ByteDeferredThisStep = true;
									PeerValue.StructuralTransitionsConsumedThisStep += Produced.SelectedTransitions;
									GlobalConsumed += Produced.SelectedTransitions;
									LastServicedConnection = Connection;
									return false;
								}
								// No yield or second producer exists between quote and reserve.
								// A failed invariant is terminal, never acceptance without credit.
								PendingPeerFailures.try_emplace(Connection, DisconnectInfo{DisconnectReason::ResourceExhaustion,
									"Reliable structural byte reservation changed during preparation"});
								return false;
								}
						}
						const auto CausalBytes = Produced.EncodedFrame.size() + ReliableServiceEnvelopeBytes;
						const bool CaptureCausalIdentity = detail::ActiveAdmissionEvidence || detail::ActiveStructuralCausalEvidence;
						const auto CausalFingerprint = CaptureCausalIdentity
							? Produced.DiagnosticFingerprint : std::array<std::uint64_t, 2>{};
						const auto CausalAcceptedBefore = CaptureCausalIdentity
							? detail::ReliableServiceFeedbackAccess::Accepted(Scheduler, Connection) : std::nullopt;
						auto Queued = QueueStructuralFrame(*Produced.Frame, std::move(Produced.EncodedFrame), Connection,
							PeerValue.Limits, IsPooled() && Receipt ? Receipt->Token : 0);
						if (!Queued || !Queued->Accepted()) {
							detail::RecordStructuralCausal({.Kind = detail::StructuralCausalKind::Rejected,
								.Connection = Connection, .SourceScope = Runtime->DataModel->GetObjectId(),
								.Sequence = Produced.Frame->Sequence.Value()});
							if (Receipt) (void)ByteAdmission->Rollback(*Receipt);
							PendingPeerFailures.try_emplace(
								Connection,
								Queued && Queued->TerminalDisconnect
									? *Queued->TerminalDisconnect
									: DisconnectInfo{
										  DisconnectReason::ResourceExhaustion,
										  "Structural transition was rejected by the reliable scheduler",
									  }
							);
							return false;
						}
						// The scheduler has accepted the exact grant, but has not sent it.
						// Preparation and a rolled-back reservation are not F1 drain time.
						const auto GrantActivatedAt = IsPooled() && Receipt && PeerValue.Phase == PeerPhase::Ready
							? ServiceTime() : std::numeric_limits<std::uint64_t>::max();
						// Once queued, bytes are charged even if a later semantic invariant
						// terminates the peer. Do not refund already accepted traffic.
						// Bootstrap has no performance activation clock, but separated
						// capacity still needs its exact token/byte offer before ACK.
						const bool OfferPublished = !Receipt || !IsPooled() ||
							(PeerValue.Phase != PeerPhase::Ready && !Configuration.ReliableService->Pooled.SeparateNativeCapacity) ||
							PeerValue.ReliableFeedback.PublishOffer(Receipt->Token, Receipt->Bytes, GrantActivatedAt);
						if (Receipt && (!ByteAdmission->Commit(*Receipt, GrantActivatedAt) ||
							(IsPooled() && !Scheduler.ActivateReliableGrant(Connection, Receipt->Token, GrantActivatedAt)) ||
							!OfferPublished)) {
							PendingPeerFailures.try_emplace(Connection, DisconnectInfo{DisconnectReason::ResourceExhaustion,
								"Reliable byte reservation commit failed"});
							return false;
						}
						if (IsPooled() && Receipt && detail::ActivePooledService && detail::ActivePooledService->AcceptedGrant)
							detail::ActivePooledService->AcceptedGrant(detail::ActivePooledService->Context,
								Connection, Receipt->Token, Receipt->Bytes, GrantActivatedAt);
						auto Committed = Replication->CommitSchedulerAcceptance(Connection, Produced.Frame->Sequence);
						if (!Committed.Succeeded()) {
							PendingPeerFailures.try_emplace(
								Connection,
								DisconnectInfo{
									DisconnectReason::ResourceExhaustion,
									"[Replication:StructuralScheduler] " + Committed.Error,
								}
							);
							return false;
						}
						PeerValue.AcceptedStructuralSequence = Produced.Frame->Sequence.Value();
						if (!PeerValue.PossessionPoses.empty())
							for (const auto &Operation : Produced.Frame->Operations)
								if (std::holds_alternative<UnpublishReplication>(Operation.Intent) ||
									std::holds_alternative<DestroyReplication>(Operation.Intent))
									RetirePossessionPoseBinding(Connection, PeerValue, ReplicationObject(Operation.Intent));
						if (CaptureCausalIdentity) {
							PeerValue.CausalGrantToken = Receipt ? Receipt->Token : 0;
							PeerValue.CausalGrantSequence = Produced.Frame->Sequence.Value();
							PeerValue.CausalGrantBytes = CausalBytes;
							PeerValue.CausalGrantFingerprint = CausalFingerprint;
							PeerValue.CausalAcceptedBefore = CausalAcceptedBefore ? CausalAcceptedBefore->Structural : 0;
							PeerValue.CausalDeliveryObserved = false;
							detail::RecordStructuralCausal({.Kind = detail::StructuralCausalKind::Accepted,
								.Connection = Connection, .SourceScope = Runtime->DataModel->GetObjectId(),
								.Sequence = Produced.Frame->Sequence.Value(),
								.GrantToken = PeerValue.CausalGrantToken, .CompleteBytes = CausalBytes,
								.Fingerprint = CausalFingerprint});
						}
						// Only an accepted GRPL Leave retires a recipient/object relationship.
						// Preparation, credit deferral and rejected scheduler work do not.
						if (runtime_detail::ActivePublicationLatency)
							for (const auto &Operation : Produced.Frame->Operations)
								if (std::holds_alternative<UnpublishReplication>(Operation.Intent) ||
									std::holds_alternative<DestroyReplication>(Operation.Intent))
									runtime_detail::RecordPublicationLatency({
										.Stage = "RecipientRetired", .Connection = Connection,
										.Object = ReplicationObject(Operation.Intent), .Tick = SimulationTick,
									});
						if (!ApplyServerRemoteMaterialization(Connection, *Produced.Frame)) {
							PendingPeerFailures.try_emplace(
								Connection,
								DisconnectInfo{
									DisconnectReason::ResourceExhaustion,
									"Remote materialization could not follow structural commit",
								}
							);
							return false;
						}
						PeerValue.StructuralSubmittedThisStep = true;
						LastServicedConnection = Connection;
						LastGrantedConnection = Connection;
						// An accepted F1 grant is already on its finite first-send clock.
						// Submit it before planning the next peer's large frame; the
						// shared step allowance keeps normal scheduler precedence/bounds.
						if (IsPooled() && Receipt && PeerValue.Phase == PeerPhase::Ready) {
							auto Flushed = PeerValue.SendAllowance.Flush(Scheduler, Connection, PeerValue.Limits);
							if (auto Failure = SchedulerFlushFailure(Flushed)) {
								PendingPeerFailures.try_emplace(Connection, std::move(*Failure));
								return false;
							}
						}
						return true;
					};

				// A deferred bootstrap must not repeatedly miss its turn behind an
				// already-ready peer's bulk journal. Reserve up to a quarter of the
				// existing global cap, then run the ordinary rotating pass unchanged.
				for (const bool BootstrapOnly : {true, false}) {
					if (BootstrapOnly && !HasPendingBootstrap) continue;
					const auto GlobalLimit = Configuration.StructuralReplication.MaximumTransitionsPerTick;
					const auto BootstrapLimit = GlobalLimit - GlobalLimit / 4 >= Configuration.StructuralReplication.PeerQuantum
						? GlobalLimit / 4 : 0;
					const auto PassLimit = BootstrapOnly ? BootstrapLimit : GlobalLimit;
					MadeProgress = true;
					while (MadeProgress && GlobalConsumed < PassLimit) {
						MadeProgress = false;
						for (const auto Connection : StructuralConnections) {
							if (PendingSessionFailure) { (void)DrainFailures(); return; }
							auto Peer = Peers.find(Connection);
							if (Peer == Peers.end() || PendingPeerFailures.contains(Connection)) continue;
							if (Peer->second.ByteDeferredThisStep) continue;
							if (BootstrapOnly && Peer->second.Phase != PeerPhase::BootstrapPending) continue;
							auto &Consumed = Peer->second.StructuralTransitionsConsumedThisStep;
							if (Consumed >= Configuration.StructuralReplication.MaximumTransitionsPerPeerTick) continue;
							const auto Allowance = std::min({
								Configuration.StructuralReplication.PeerQuantum,
								Configuration.StructuralReplication.MaximumTransitionsPerPeerTick - Consumed,
								PassLimit - GlobalConsumed,
							});
							if (Allowance == 0) break;
							std::size_t AvailableFrameBytes = MaximumReplicationFrameBytes;
							if (ByteAdmission) {
								if (!Replication->HasPendingStructuralWork(Connection)) {
									if (!ByteAdmission->NoWork(Connection, true, ServiceTime())) {
										FailSession({DisconnectReason::ResourceExhaustion, "Reliable source-empty clock/ownership failed"}); return;
									}
									continue;
								}
								if (!RefreshStructuralService(Connection, Peer->second, true)) {
									if (PendingSessionFailure) { (void)DrainFailures(); return; }
									continue;
								}
								if (IsPooled() && !UpdateOrdinaryFunding()) {
									FailSession({DisconnectReason::TransportFailure, "Pooled ordinary funding became invalid"}); return;
								}
								const auto Bytes = ByteAdmission->Allowance(Connection, ServiceTime());
								if (Bytes < 36 + ReliableServiceEnvelopeBytes) continue;
								AvailableFrameBytes = static_cast<std::size_t>(Bytes - ReliableServiceEnvelopeBytes);
							}
							auto DeferBytes = [&](const ReplicationProduceResult &Produced) {
								if (!Produced.DeferredForBytes) return false;
								ByteAdmission->DeferSize(Connection, Produced.RequiredFrameBytes + ReliableServiceEnvelopeBytes,
									Produced.DiagnosticFingerprint);
								Peer->second.ByteDeferredThisStep = true;
								Consumed += Produced.SelectedTransitions;
								GlobalConsumed += Produced.SelectedTransitions;
								LastServicedConnection = Connection;
								return true;
							};
							if (Peer->second.Phase == PeerPhase::BootstrapPending) {
								if (!Replication->IsPlanningReady(Connection)) {
									if (ByteAdmission) ByteAdmission->DeferSource(Connection);
									continue;
								}
								const auto CriticalTransitions = Replication->GetPendingCriticalTransitionCount(Connection);
								if (CriticalTransitions == 0) {
									PendingPeerFailures.try_emplace(
										Connection,
										DisconnectInfo{
											DisconnectReason::ResourceExhaustion,
											"Structural bootstrap has no required transition",
										}
									);
									continue;
								}
								if (CriticalTransitions > Allowance) {
									if (ByteAdmission) ByteAdmission->DeferSource(Connection);
									continue;
								}
								const auto ReplicationMetricsBefore = Replication->GetCumulativeMetrics();
								auto Produced = Replication->ProducePendingBaseline(Connection, Allowance, SimulationTick,
									Peer->second.Limits.MaximumReliableMessageBytes, AvailableFrameBytes);
								if (DeferBytes(Produced)) continue;
								const auto ReplicationMetricsAfter = Replication->GetCumulativeMetrics();
								Metrics.BaselineSnapshotCpuNanoseconds +=
									ReplicationMetricsAfter.SnapshotCaptureCpuNanoseconds -
									ReplicationMetricsBefore.SnapshotCaptureCpuNanoseconds;
								Metrics.BaselineDiscoveryCpuNanoseconds +=
									ReplicationMetricsAfter.BaselineDiscoveryCpuNanoseconds -
									ReplicationMetricsBefore.BaselineDiscoveryCpuNanoseconds;
								Metrics.BaselineEncodeCpuNanoseconds +=
									ReplicationMetricsAfter.BaselineEncodeCpuNanoseconds -
									ReplicationMetricsBefore.BaselineEncodeCpuNanoseconds;
								// A world revision can obsolete a queued bootstrap while its
								// peer awaits bounded 3E service. Defer, never accept stale work.
								if (Produced.Error == "No replication relevance changes are available") {
									if (ByteAdmission) ByteAdmission->DeferSource(Connection);
									continue;
								}
								if (!Produced.Succeeded() || !Produced.Frame) {
									PendingPeerFailures.try_emplace(
										Connection,
										DisconnectInfo{
											DisconnectReason::ResourceExhaustion,
											"[Replication:StructuralScheduler] " + Produced.Error,
										}
									);
									continue;
								}
								if (!SubmitStructural(Connection, Peer->second, Produced)) continue;
								if (!QueueSession(
										Connection,
										GameSessionServerAccepted{
											.Nonce = Peer->second.Nonce,
											.SessionEpoch = Peer->second.SessionEpoch,
											.Replication = Peer->second.Replication,
											.Player = Peer->second.PlayerObject,
											.PlayerId = Peer->second.PlayerId,
											.Identity = SessionIdentityKind::DevelopmentLocal,
											.NegotiatedLimits = Peer->second.Limits,
											.TerrainBinding = GetTerrainBinding(),
											.RuntimeDistributionIdentity = Configuration.RuntimeDistributionIdentity,
											.SpatialContent = Configuration.SpatialContent,
											.AuthorizedSpatialRegion = Peer->second.AuthorizedSpatialRegion,
										},
										Peer->second.Limits
									)) {
									PendingPeerFailures.try_emplace(
										Connection,
										DisconnectInfo{
											DisconnectReason::ResourceExhaustion,
											"Server bootstrap acceptance could not be queued",
										}
									);
									continue;
								}
								Peer->second.Phase = PeerPhase::Accepted;
								++Metrics.AcceptedPeers;
								Consumed += Produced.SelectedTransitions;
								GlobalConsumed += Produced.SelectedTransitions;
								MadeProgress = Produced.SelectedTransitions != 0;
								continue;
							}
							auto ProduceRelevance = [&]() {
								if (!Replication->HasPendingRelevance(Connection)) return false;
								auto Produced = Replication->ProducePendingRelevance(Connection, Allowance, SimulationTick,
									Peer->second.Limits.MaximumReliableMessageBytes, AvailableFrameBytes);
								if (DeferBytes(Produced)) return true;
								if (Produced.Succeeded() && Produced.Frame) {
									if (SubmitStructural(Connection, Peer->second, Produced)) {
										Consumed += Produced.SelectedTransitions;
										GlobalConsumed += Produced.SelectedTransitions;
										MadeProgress = Produced.SelectedTransitions != 0;
										Peer->second.PreferPendingRelevance = false;
									}
									return true;
								}
								if (Produced.Error == "No replication relevance changes are available") return false;
								PendingPeerFailures.try_emplace(
									Connection,
									DisconnectInfo{
										DisconnectReason::ResourceExhaustion,
										"[Replication:StructuralScheduler] " + Produced.Error,
									}
								);
								return true;
							};
							auto ProduceJournal = [&]() {
								auto &JournalConsumed = Peer->second.JournalRecordsConsumedThisStep;
								const auto JournalAllowance = std::min(
									Configuration.StructuralReplication.MaximumJournalRecordsPerPeerTick - JournalConsumed,
									Configuration.StructuralReplication.MaximumJournalRecordsPerTick - GlobalJournalConsumed);
								if (JournalAllowance == 0) {
									if (Replication->GetJournalLag(Connection) != 0) SaturatingAdd(Metrics.JournalBudgetDeferrals, 1);
									return false;
								}
								auto Produced = Replication->ProduceIncremental(Connection, Allowance,
									Peer->second.Limits.MaximumReliableMessageBytes, JournalAllowance, AvailableFrameBytes);
								JournalConsumed += Produced.JournalRecordsExamined;
								GlobalJournalConsumed += Produced.JournalRecordsExamined;
								if (DeferBytes(Produced)) return true;
								if (Produced.JournalRecordsExamined != 0) {
									LastServicedConnection = Connection;
									LastJournalServicedConnection = Connection;
								}
								if (Produced.Succeeded() && Produced.Frame) {
									if (SubmitStructural(Connection, Peer->second, Produced)) {
										Consumed += Produced.SelectedTransitions;
										GlobalConsumed += Produced.SelectedTransitions;
										MadeProgress = Produced.SelectedTransitions != 0;
										Peer->second.PreferPendingRelevance = true;
									}
									return true;
								}
								if (Produced.Error == "No replication changes are available" ||
									Produced.Error == "No relevant replication changes are available")
									return false;
								PendingPeerFailures.try_emplace(
									Connection,
									DisconnectInfo{
										DisconnectReason::ResourceExhaustion,
										"[Replication:StructuralScheduler] " + Produced.Error,
									}
								);
								return true;
							};
							if (Peer->second.PreferPendingRelevance) {
								if (ProduceRelevance()) continue;
								if (ProduceJournal()) continue;
							} else {
								if (ProduceJournal()) continue;
								if (ProduceRelevance()) continue;
							}
							// No frame can mean exhausted journal allowance or unfinished
							// planning. Neither may erase the exact candidate or fair wait.
							if (ByteAdmission) {
								if (!Replication->HasPendingStructuralWork(Connection)) {
									if (!ByteAdmission->NoWork(Connection, true, ServiceTime())) {
										FailSession({DisconnectReason::ResourceExhaustion, "Reliable source-empty clock/ownership failed"}); return;
									}
								} else ByteAdmission->DeferSource(Connection);
							}
						}
					}
				}
				// Complete READY batches can keep the operation budget saturated for
				// many ticks. Preserve independent progress for journal prefixes that
				// provably cannot emit structural work, under the unchanged read caps.
				if (ByteAdmission || GlobalConsumed >= Configuration.StructuralReplication.MaximumTransitionsPerTick) {
					const auto JournalStart = JournalStartConnection
						? static_cast<std::size_t>(std::ranges::find(StructuralConnections, *JournalStartConnection) - StructuralConnections.begin())
						: std::size_t{0};
					for (std::size_t Offset = 0; Offset < StructuralConnections.size(); ++Offset) {
						const auto Connection = StructuralConnections[(JournalStart + Offset) % StructuralConnections.size()];
						if (GlobalJournalConsumed >= Configuration.StructuralReplication.MaximumJournalRecordsPerTick) break;
						auto Peer = Peers.find(Connection);
						if (Peer == Peers.end() || PendingPeerFailures.contains(Connection) ||
							Peer->second.Phase == PeerPhase::TransportConnected || Peer->second.Phase == PeerPhase::BootstrapPending) continue;
						auto &Consumed = Peer->second.JournalRecordsConsumedThisStep;
						const auto Allowance = std::min(Configuration.StructuralReplication.MaximumJournalRecordsPerPeerTick - Consumed,
							Configuration.StructuralReplication.MaximumJournalRecordsPerTick - GlobalJournalConsumed);
						if (!Allowance) continue;
						auto Produced = Replication->ProduceIncremental(Connection, 0, Peer->second.Limits.MaximumReliableMessageBytes, Allowance);
						Consumed += Produced.JournalRecordsExamined;
						GlobalJournalConsumed += Produced.JournalRecordsExamined;
						if (Produced.JournalRecordsExamined) LastJournalServicedConnection = Connection;
						if (Produced.Error != "No replication changes are available" && Produced.Error != "No relevant replication changes are available")
							PendingPeerFailures.try_emplace(Connection, DisconnectInfo{DisconnectReason::ResourceExhaustion,
								"[Replication:StructuralScheduler] " + Produced.Error});
					}
				}
				if (ByteAdmission) ByteAdmission->EndStep();
				if (!StructuralConnections.empty()) {
					if (IsPooled()) {
						// Slot opportunity rotates only after accepted work. A shadow
						// journal reader must not redirect the next free structural slot.
						if (LastGrantedConnection) {
							auto Last = std::ranges::find(StructuralConnections, *LastGrantedConnection);
							StructuralFairnessCursor = ++Last == StructuralConnections.end() ? StructuralConnections.front() : *Last;
						}
						if (LastJournalServicedConnection) {
							auto Last = std::ranges::find(StructuralConnections, *LastJournalServicedConnection);
							StructuralJournalFairnessCursor = ++Last == StructuralConnections.end() ? StructuralConnections.front() : *Last;
						}
					} else {
					auto Last = LastServicedConnection
						? std::ranges::find(StructuralConnections, *LastServicedConnection) : StructuralConnections.begin();
					// Coalesced/skipped records consume service even with no frame.
					// Continue after the last reader when the journal budget binds.
					if (GlobalJournalConsumed >= Configuration.StructuralReplication.MaximumJournalRecordsPerTick &&
						LastJournalServicedConnection)
						Last = std::ranges::find(StructuralConnections, *LastJournalServicedConnection);
					if (Last == StructuralConnections.end() || ++Last == StructuralConnections.end())
						StructuralFairnessCursor = StructuralConnections.front();
					else
						StructuralFairnessCursor = *Last;
					}
				}
				if (GlobalConsumed >= Configuration.StructuralReplication.MaximumTransitionsPerTick &&
					Replication->HasPendingStructuralWork())
					Replication->RecordGlobalBudgetExhaustion();
				Metrics.StructuralMaximumTransitionsSelectedPerTick = std::max<std::uint64_t>(
					Metrics.StructuralMaximumTransitionsSelectedPerTick, GlobalConsumed
				);
				SaturatingAdd(Metrics.JournalRecordsExamined, GlobalJournalConsumed);
				Metrics.JournalRecordsPerTickHighWater = std::max<std::uint64_t>(Metrics.JournalRecordsPerTickHighWater, GlobalJournalConsumed);
				Metrics.JournalBacklogRecords = 0;
				Metrics.JournalOldestBacklogEpisodeAgeTicks = 0;
				Metrics.JournalStagingBytes = Peers.size() * (sizeof(std::size_t) + sizeof(std::optional<std::uint64_t>));
				for (const auto Connection : StructuralConnections) {
					auto Peer = Peers.find(Connection);
					if (Peer != Peers.end()) {
						Metrics.JournalRecordsPerPeerTickHighWater = std::max<std::uint64_t>(
							Metrics.JournalRecordsPerPeerTickHighWater, Peer->second.JournalRecordsConsumedThisStep);
						const auto Lag = Replication->GetJournalLag(Connection);
						runtime_detail::MaximumWork(runtime_detail::WorkCounter::JournalLagAfterServiceMaximum, Lag);
						SaturatingAdd(Metrics.JournalBacklogRecords, Lag);
						auto &PendingSince = Peer->second.JournalPendingSinceTick;
						if (Lag == 0) PendingSince.reset();
						else {
							if (!PendingSince) PendingSince = SimulationTick;
							Metrics.JournalOldestBacklogEpisodeAgeTicks = std::max(Metrics.JournalOldestBacklogEpisodeAgeTicks,
								SimulationTick >= *PendingSince ? SimulationTick - *PendingSince : 0);
						}
					}
					if (Peer == Peers.end() || PendingPeerFailures.contains(Connection) ||
						!Peer->second.StructuralSubmittedThisStep)
						continue;
					auto Flushed = Peer->second.SendAllowance.Flush(Scheduler, Connection, Peer->second.Limits);
					if (auto Failure = SchedulerFlushFailure(Flushed))
						PendingPeerFailures.try_emplace(Connection, std::move(*Failure));
				}
				Metrics.JournalBacklogRecordsHighWater = std::max(Metrics.JournalBacklogRecordsHighWater, Metrics.JournalBacklogRecords);
				Metrics.JournalMaximumBacklogEpisodeAgeTicks = std::max(Metrics.JournalMaximumBacklogEpisodeAgeTicks, Metrics.JournalOldestBacklogEpisodeAgeTicks);
				if (IsPooled()) {
					for (auto &[Connection, PeerValue] : Peers) {
						if (PeerValue.Phase != PeerPhase::Ready || PendingPeerFailures.contains(Connection)) continue;
						const bool WorkReady = Replication->HasPendingRelevance(Connection) ||
							Replication->IsPlanningReady(Connection) || Replication->GetJournalLag(Connection) != 0;
						const auto CompletedAt = ServiceTime();
						const auto ExecutionBefore = detail::ActivePooledService && detail::ActivePooledService->CaptureExecution
							? detail::CapturePooledExecution(PeerValue.ReliableFeedback) : detail::PooledExecutionState{};
						const bool Fits = PeerValue.ReliableFeedback.CompleteAdmissionOpportunity(CompletedAt, WorkReady);
						const auto Accepted = detail::ReliableServiceFeedbackAccess::Accepted(Scheduler, Connection);
						if (!Accepted) {
							RecordPooledExecutionWithoutAcceptance(Connection, PeerValue.ReliableFeedback,
								detail::PooledObservationPoint::AdmissionComplete, ExecutionBefore, CompletedAt, WorkReady);
							FailSession({DisconnectReason::TransportFailure, "[Network:PooledService] Completed opportunity lost acceptance accounting"});
							return;
						}
						const auto Result = PeerValue.ReliableFeedback.Observe(Connection, *Accepted, {}, CompletedAt,
							ByteAdmission->DebtToken(Connection), ByteAdmission->Debt(Connection));
						RecordPooledObservation(Connection, *Accepted, {}, Result, CompletedAt,
							detail::PooledObservationPoint::AdmissionComplete, ExecutionBefore, !Fits, WorkReady);
						if (!Fits)
							PendingPeerFailures.try_emplace(Connection, DisconnectInfo{DisconnectReason::TransportFailure,
								"[Network:PooledService] Structural retirement or completed admission opportunity exceeded its product budget"});
					}
				}
				if (!DrainFailures()) return;
				SynchronizeServerGraph();
				if (!DrainFailures()) return;
				Authority->Step(*Runtime->WorldRoot, std::max(SimulationTick, std::uint64_t{1}));
				PublishPossessionPoses(std::max(SimulationTick, std::uint64_t{1}));
				if (!DrainFailures()) return;
			} else if (ClientRuntimeAttached && Prediction) {
				if (!PumpClientTerrain()) return;
				UpdatePossessionPresentation(SimulationTick);
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
			if (Configuration.Role == GameSessionRole::Server && Configuration.TerrainBootstrap) PumpServerTerrain();
			if (Configuration.SpatialContent) for (auto &[Connection,Value] : Peers) PumpGrids(Connection,Value);
			if (Status == GameSessionStatus::Failed) return;
			if (!DrainFailures()) return;
			if (Remotes) (void)Remotes->Pump(Configuration.Limits.MaximumMessagesPerTick);
			if (!DrainFailures()) return;
			std::vector<std::pair<ConnectionId, DisconnectInfo>> TerminalConnections;
			for (auto &[Connection, PeerValue] : Peers) {
				if (Configuration.Role == GameSessionRole::Server && PeerValue.Phase == PeerPhase::TransportConnected)
					continue;
				auto Flushed = PeerValue.SendAllowance.Flush(Scheduler, Connection,
					PeerValue.Limits.IsValid() ? PeerValue.Limits : Configuration.Limits);
				if (auto Failure = SchedulerFlushFailure(Flushed))
					TerminalConnections.emplace_back(Connection, std::move(*Failure));
			}
			for (auto &[Connection, Information] : TerminalConnections)
				FailPeer(Connection, Information.Reason, std::move(Information.Diagnostic));
			for (auto &[Connection, PeerValue] : Peers)
				if (PeerValue.Phase == PeerPhase::Ready) ObserveOrdinaryQueue(Connection, PeerValue, ServiceTime());
		}

		bool AttachClientRuntime(Engine &RuntimeValue) {
			if (Configuration.Role != GameSessionRole::Client || ClientRuntimeAttached ||
				Status != GameSessionStatus::Accepted || !PrimaryConnection ||
				RuntimeValue.DataModel != GetClientDataModel())
				return false;
			auto &PeerValue = Peers.at(*PrimaryConnection);
			if (Configuration.TerrainBootstrap &&
				(!TerrainMotionAdmission || !PeerValue.TerrainAdmitted || !PeerValue.TerrainPending || !PeerValue.TerrainPending->Window ||
					!RuntimeValue.Workspace->IsTerrainCollisionReady())) {
				FailSession({DisconnectReason::ResourceExhaustion, "Terrain collision is unavailable on the client"});
				return false;
			}
			Runtime = &RuntimeValue;
			try {
				if (Configuration.SpatialContent) {
					NetworkSpatialProfile = Runtime->DataModel->AcquireDefaultNetworkSpatialProfile(true);
					for (const auto Id : ClientKnownObjects)
						if (std::dynamic_pointer_cast<BlockGrid>(Replica.Resolve(Id))) PeerValue.Grids->Track(Id);
				}
				InitializeClientManagers();
				if (Configuration.TerrainBootstrap &&
					!QueueTerrainReady(*PrimaryConnection, PeerValue, std::move(*PeerValue.TerrainPending->Window)))
					throw std::runtime_error("[Terrain:Network] Initial collision acknowledgment could not be queued");
			} catch (...) {
				FailSession({
					DisconnectReason::ResourceExhaustion,
					"Client runtime bootstrap acquisition failed",
				});
				return false;
			}
			auto Ready = InjectFailure(detail::GameSessionFailurePoint::ClientReadySerialization)
							 ? SerializationResult<std::vector<std::byte>>(SerializationFailure(
								   SerializationErrorCode::InternalFailure, "Injected ClientReady serialization failure"
							   ))
							 : EncodeGameSessionMessage(
								   GameSessionClientReady{
									   PeerValue.SessionEpoch,
									   PeerValue.Replication,
									   PeerValue.PlayerObject,
									   GetTerrainBinding(),
									   Configuration.SpatialContent,
								   }
							   );
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
			if (!ArmOrdinaryPolicy(*PrimaryConnection, PeerValue)) {
				FailSession({DisconnectReason::ResourceExhaustion, "Client ordinary source policy registration failed"});
				return false;
			}
			Status = GameSessionStatus::Ready;
			if (Configuration.TerrainBootstrap) {
				PeerValue.TerrainReleaseStartedTick = TerrainTick;
				LOG_INFO(App, "[Terrain:Network] Client installed verified terrain collision before gameplay readiness");
			}
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

	void detail::GameSessionTestAccess::SetFailurePoint(GameSession &Session, detail::GameSessionFailurePoint Point) {
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
		// Bounded aggregates only; no peer labels or retained per-message history.
		auto Result = State->Metrics;
		Result.PossessionInput = State->ServerPossessionInput.GetMetrics();
		if (State->ByteAdmission) {
			Result.ReliableAdmission = State->ByteAdmission->GetMetrics();
			Result.ReliableAdmissionPeerStates = State->ByteAdmission->PeerCount();
			Result.ReliableAdmissionLogicalBytes = State->ByteAdmission->LogicalBytes();
		}
		Result.ClientReplica = State->Replica.GetMetrics();
		if (State->Prediction) {
			const auto CharacterMetrics = State->Prediction->GetMetrics();
			Result.ClientActionSubmissionAttempts = CharacterMetrics.ActionSubmissionAttempts;
			Result.ClientActionSubmissionNoControl = CharacterMetrics.ActionSubmissionNoControl;
			Result.ClientActionSubmissionSuspended = CharacterMetrics.ActionSubmissionSuspended;
			Result.ClientActionSubmissionInvalid = CharacterMetrics.ActionSubmissionInvalid;
			Result.ClientActionSubmissionPendingFull = CharacterMetrics.ActionSubmissionPendingFull;
			Result.ClientActionSubmissionSchedulerRejected = CharacterMetrics.ActionSubmissionSchedulerRejected;
			Result.ClientCharacterStaleStatesDropped = CharacterMetrics.StaleStatesDropped;
			Result.ClientMaterializationEpochMismatches = CharacterMetrics.MaterializationEpochMismatches;
			Result.ClientLastExpectedMaterializationEpoch = CharacterMetrics.LastExpectedMaterializationEpoch;
			Result.ClientLastReceivedMaterializationEpoch = CharacterMetrics.LastReceivedMaterializationEpoch;
			Result.ClientCharacterHistoryOverflows = CharacterMetrics.HistoryOverflows;
		}
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
			Result.RelevancePeerEvaluations = RelevanceMetrics.PeerEvaluations;
			Result.RelevanceSelectionCacheHits = RelevanceMetrics.SelectionCacheHits;
			Result.RelevancePeerEvaluationsHighWater = RelevanceMetrics.PeerEvaluationsHighWater;
			Result.RelevanceDeferredPeers = RelevanceMetrics.DeferredPeers;
			Result.RelevanceDeferredPeersHighWater = RelevanceMetrics.DeferredPeersHighWater;
			Result.RelevanceOldestPendingAgeTicks = RelevanceMetrics.OldestPendingAgeTicks;
			Result.RelevanceMaximumPendingAgeTicks = RelevanceMetrics.MaximumPendingAgeTicks;
			Result.RelevanceStagingBytes = RelevanceMetrics.StagingBytes;
			Result.RelevanceCharacterCandidateBytes = RelevanceMetrics.CharacterCandidateBytes;
		}
		if (State->Replication) {
			const auto &ReplicationMetrics = State->Replication->GetMetrics();
			Result.DependencyPlanRebuilds = ReplicationMetrics.DependencyPlanRebuilds;
			Result.DependencyPlanCacheHits = ReplicationMetrics.DependencyPlanCacheHits;
			Result.MaterializationBacklog = ReplicationMetrics.MaterializationBacklog;
			Result.MaterializationTransitions = ReplicationMetrics.RelevanceTransitions;
			Result.MaterializationCpuNanoseconds = ReplicationMetrics.RelevanceTransitionCpuNanoseconds;
			Result.StructuralTemplateBuilds = ReplicationMetrics.StructuralTemplateBuilds;
			Result.StructuralTemplateHits = ReplicationMetrics.StructuralTemplateHits;
			Result.StructuralTemplateMisses = ReplicationMetrics.StructuralTemplateMisses;
			Result.StructuralTemplateInvalidations = ReplicationMetrics.StructuralTemplateInvalidations;
			Result.StructuralTemplateBytes = ReplicationMetrics.StructuralTemplateBytes;
			Result.PeerMaterializationPlans = ReplicationMetrics.PeerMaterializationPlans;
			Result.PeerPatchOperations = ReplicationMetrics.PeerPatchOperations;
			Result.ReferencePatchOperations = ReplicationMetrics.ReferencePatchOperations;
			Result.StructuralBytesEncoded = ReplicationMetrics.StructuralBytesEncoded;
			Result.StructuralBytesReused = ReplicationMetrics.StructuralBytesReused;
			Result.ScratchHighWaterBytes = ReplicationMetrics.ScratchHighWaterBytes;
			Result.StructuralSchedulingTicks = ReplicationMetrics.StructuralSchedulingTicks;
			Result.StructuralTransitionsOffered = ReplicationMetrics.StructuralTransitionsOffered;
			Result.StructuralTransitionsSelected = ReplicationMetrics.StructuralTransitionsSelected;
			Result.StructuralTransitionsPrepared = ReplicationMetrics.StructuralTransitionsPrepared;
			Result.StructuralTransitionsEncoded = ReplicationMetrics.StructuralTransitionsEncoded;
			Result.StructuralTransitionsAccepted = ReplicationMetrics.StructuralTransitionsAccepted;
			Result.StructuralTransitionsCommitted = ReplicationMetrics.StructuralTransitionsCommitted;
			Result.StructuralTransitionsDeferredByBudget = ReplicationMetrics.StructuralTransitionsDeferredByBudget;
			Result.StructuralTransitionsCancelled = ReplicationMetrics.StructuralTransitionsCancelled;
			Result.StructuralTransitionsReplanned = ReplicationMetrics.StructuralTransitionsReplanned;
			Result.StructuralDeadlineMisses = ReplicationMetrics.StructuralDeadlineMisses;
			Result.StructuralPeerFairnessRotations = ReplicationMetrics.StructuralPeerFairnessRotations;
			Result.StructuralGlobalBudgetExhaustions = ReplicationMetrics.StructuralGlobalBudgetExhaustions;
			Result.StructuralDependencyPlanOperations = ReplicationMetrics.StructuralDependencyPlanOperations;
			Result.StructuralBacklogLimitFailures = ReplicationMetrics.StructuralBacklogLimitFailures;
			Result.StructuralJournalLagFailures = ReplicationMetrics.StructuralJournalLagFailures;
			Result.StructuralMaximumJournalLagRecords = ReplicationMetrics.StructuralMaximumJournalLagRecords;
			Result.StructuralPendingEnters = ReplicationMetrics.StructuralPendingEnters;
			Result.StructuralPendingLeaves = ReplicationMetrics.StructuralPendingLeaves;
			Result.StructuralPendingCritical = ReplicationMetrics.StructuralPendingCritical;
			Result.StructuralActivePeers = ReplicationMetrics.StructuralActivePeers;
			Result.StructuralOldestPendingAgeTicks = ReplicationMetrics.StructuralOldestPendingAgeTicks;
			Result.StructuralCriticalOldestAgeTicks = ReplicationMetrics.StructuralCriticalOldestAgeTicks;
			Result.StructuralSelectionCpuNanoseconds = ReplicationMetrics.StructuralSelectionCpuNanoseconds;
			Result.StructuralMaximumTransitionsSelectedPerTick =
				State->Metrics.StructuralMaximumTransitionsSelectedPerTick;
			Result.AcceptedAncestryObjects = ReplicationMetrics.AcceptedAncestryObjects;
			Result.AcceptedAncestryLogicalBytes = ReplicationMetrics.AcceptedAncestryLogicalBytes;
			Result.CatalogRetiredObjects = ReplicationMetrics.CatalogRetiredObjects;
			Result.CatalogRetiredHighWater = ReplicationMetrics.CatalogRetiredHighWater;
			Result.CatalogRetirementExaminations = ReplicationMetrics.CatalogRetirementExaminations;
			Result.CatalogRetirementMaximumTickExaminations = ReplicationMetrics.CatalogRetirementMaximumTickExaminations;
			Result.CatalogRetirementReleases = ReplicationMetrics.CatalogRetirementReleases;
			Result.CatalogRetentionLogicalBytes = ReplicationMetrics.CatalogRetentionLogicalBytes;
			Result.CatalogReferenceIndexBytes = ReplicationMetrics.CatalogReferenceIndexBytes;
			Result.PlanningWork = ReplicationMetrics.PlanningWork;
			Result.PlanningMaximumTickWork = ReplicationMetrics.PlanningMaximumTickWork;
			Result.PlanningServiceOpportunities = ReplicationMetrics.PlanningServiceOpportunities;
			Result.PlanningMaximumServiceGapTicks = ReplicationMetrics.PlanningMaximumServiceGapTicks;
			Result.PlanningMaximumPeerSlice = ReplicationMetrics.PlanningMaximumPeerSlice;
			Result.PlanningResumes = ReplicationMetrics.PlanningResumes;
			Result.PlanningInvalidations = ReplicationMetrics.PlanningInvalidations;
			Result.PlanningReadyBatches = ReplicationMetrics.PlanningReadyBatches;
			Result.PlanningRecords = ReplicationMetrics.PlanningRecords;
			Result.PlanningRecordsHighWater = ReplicationMetrics.PlanningRecordsHighWater;
			Result.PlanningPeerRecordsHighWater = ReplicationMetrics.PlanningPeerRecordsHighWater;
			Result.PlanningCpuNanoseconds = ReplicationMetrics.PlanningCpuNanoseconds;
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
			Result.StructuralAcceptedFeedbackBytes += PeerValue.ReliableFeedback.Accepted.Structural;
			if (PeerValue.ReliableFeedback.Previous) {
				Result.StructuralFirstSentFeedbackBytes +=
					PeerValue.ReliableFeedback.Previous->StructuralPayloadBytesFirstSent;
				Result.StructuralAckedFeedbackBytes +=
					PeerValue.ReliableFeedback.Previous->StructuralPayloadBytesAcked;
				++Result.StructuralFeedbackPeersObserved;
			}
			if (const auto Scheduler = State->Scheduler.GetStatistics(Connection))
				Result.SchedulerQueuedReliableBytes += Scheduler->QueuedReliableBytes;
			if (const auto Native = State->Transport->GetStatistics(Connection); Native && Native->QueuedReliableBytes) {
				Result.NativeQueuedReliableBytes += *Native->QueuedReliableBytes;
				++Result.NativeQueuedReliablePeersObserved;
			}
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
		return Iterator != State->Peers.end() &&
					   (Iterator->second.Phase == PeerPhase::Accepted || Iterator->second.Phase == PeerPhase::Ready)
				   ? Iterator->second.PlayerValue
				   : nullptr;
	}
	std::shared_ptr<Player> GameSession::ResolveRemotePlayer(const RemotePeerContext &Peer) const {
		return State->ResolveRemotePlayer(Peer);
	}
	std::optional<RemotePeerContext> GameSession::ResolveRemotePeer(const Player &PlayerValue) const {
		return State->ResolveRemotePeer(PlayerValue);
	}
	std::vector<GameSessionPeerIdentity> GameSession::GetPeerIdentities() const {
		std::vector<GameSessionPeerIdentity> Result;
		Result.reserve(State->Peers.size());
		for (const auto &[Connection, PeerValue] : State->Peers)
			if (PeerValue.Phase == PeerPhase::Accepted || PeerValue.Phase == PeerPhase::Ready)
				Result.push_back(GameSessionPeerIdentity{
					.Connection = Connection,
					.Nonce = PeerValue.Nonce,
					.SessionEpoch = PeerValue.SessionEpoch,
					.PlayerId = PeerValue.PlayerId,
					.Ready = PeerValue.Phase == PeerPhase::Ready,
				});
		return Result;
	}
	bool GameSession::SetTrustedReplicationFocus(ConnectionId Connection, std::span<const glm::vec3> FocusPoints) {
		return State->Configuration.Role == GameSessionRole::Server && State->Relevance &&
			   State->Relevance->SetTrustedFocus(Connection, FocusPoints);
	}

	bool detail::GameSessionTestAccess::VerifySpatialIndex(const GameSession &Session) {
		return !Session.State->SpatialValidationRequested && Session.State->SpatialValidationPassed;
	}
	std::optional<std::vector<detail::ClientReplicaIdentity>> detail::GameSessionTestAccess::ReadClientReplicaIdentities(
		const GameSession &Session, std::span<const std::shared_ptr<Instance>> Observed) {
		if (GetCurrentExecutionDomain() != ExecutionDomain::Main) return {};
		if (!Session.State || Session.State->Configuration.Role != GameSessionRole::Client ||
			Session.State->Status != GameSessionStatus::Ready) return {};
		return ReadClientReplicaIdentities(Session.State->Replica, Observed);
	}
	std::optional<std::vector<detail::ClientReplicaIdentity>> detail::GameSessionTestAccess::ReadClientReplicaIdentities(
		const ReplicaApplier &Replica, std::span<const std::shared_ptr<Instance>> Observed) {
		if (GetCurrentExecutionDomain() != ExecutionDomain::Main) return {};
		// The existing replica object cap bounds the one map traversal; the farm
		// supplies at most 32 live final observations, once per recovery case.
		const auto Epoch = Replica.GetEpoch();
		const auto Root = Replica.GetReplicaRoot();
		if (Observed.empty() || Observed.size() > 32 || !Epoch.IsValid() || !Root ||
			Root->GetDestroyed() || Root->IsDestroying() || Replica.Receiver.Objects.size() > MaximumSnapshotObjects) return {};
		std::vector<ClientReplicaIdentity> Result(Observed.size());
		for (std::size_t Index = 0; Index < Observed.size(); ++Index) {
			const auto &Object = Observed[Index];
			if (!Object || Object->GetDestroyed() || Object->IsDestroying() || !Object->GetObjectId().IsValid() ||
				Object->GetReplicationScopeId() != Root->GetObjectId() ||
				ObjectRegistry::Get().Lookup(Object->GetObjectId()) != Object) return {};
			for (std::size_t Prior = 0; Prior < Index; ++Prior)
				if (Observed[Prior] == Object || Observed[Prior]->GetObjectId() == Object->GetObjectId()) return {};
			Result[Index].Local = Object->GetObjectId(); Result[Index].Epoch = Epoch;
		}
		for (const auto &[WireId, InstanceValue] : Replica.Receiver.Objects) {
			for (std::size_t Index = 0; Index < Observed.size(); ++Index) {
				if (InstanceValue != Observed[Index]) continue;
				if (!WireId.IsValid() || Result[Index].Source.IsValid() ||
					InstanceValue->GetObjectId() != Result[Index].Local || InstanceValue->GetDestroyed() ||
					InstanceValue->IsDestroying()) return {};
				Result[Index].Source = WireId.ToObjectId();
			}
		}
		for (std::size_t Index = 0; Index < Result.size(); ++Index)
			if (!Result[Index].Source.IsValid() || Replica.Resolve(Result[Index].Source) != Observed[Index]) return {};
		if (Replica.GetEpoch() != Epoch || Replica.GetReplicaRoot() != Root) return {};
		return Result;
	}
	bool detail::GameSessionTestAccess::VerifyClientReplicaIdentities(const GameSession &Session,
		std::span<const std::shared_ptr<Instance>> Observed, std::span<const ClientReplicaIdentity> Identities) {
		if (GetCurrentExecutionDomain() != ExecutionDomain::Main) return false;
		if (!Session.State || Session.State->Configuration.Role != GameSessionRole::Client ||
			Session.State->Status != GameSessionStatus::Ready) return false;
		return VerifyClientReplicaIdentities(Session.State->Replica, Observed, Identities);
	}
	bool detail::GameSessionTestAccess::VerifyClientReplicaIdentities(const ReplicaApplier &Replica,
		std::span<const std::shared_ptr<Instance>> Observed, std::span<const ClientReplicaIdentity> Identities) {
		if (GetCurrentExecutionDomain() != ExecutionDomain::Main) return false;
		if (Observed.empty() || Observed.size() > 32 || Observed.size() != Identities.size()) return false;
		const auto Epoch = Replica.GetEpoch();
		const auto Root = Replica.GetReplicaRoot();
		if (!Epoch.IsValid() || !Root || Root->GetDestroyed() || Root->IsDestroying()) return false;
		for (std::size_t Index = 0; Index < Observed.size(); ++Index) {
			const auto &Object = Observed[Index]; const auto &Identity = Identities[Index];
			if (!Object || Object->GetDestroyed() || Object->IsDestroying() || !Identity.Source.IsValid() ||
				!Identity.Local.IsValid() || Identity.Epoch != Epoch || Object->GetObjectId() != Identity.Local ||
				Object->GetReplicationScopeId() != Root->GetObjectId() ||
				ObjectRegistry::Get().Lookup(Identity.Local) != Object || Replica.Resolve(Identity.Source) != Object) return false;
			for (std::size_t Prior = 0; Prior < Index; ++Prior)
				if (Observed[Prior] == Object || Identities[Prior].Source == Identity.Source ||
					Identities[Prior].Local == Identity.Local) return false;
		}
		return Replica.GetEpoch() == Epoch && Replica.GetReplicaRoot() == Root;
	}
	std::optional<detail::PooledPostpassSnapshot> detail::GameSessionTestAccess::ReadPooledPostpass(const GameSession &Session) {
		if (GetCurrentExecutionDomain() != ExecutionDomain::Main) return {};
		const auto *State = Session.State.get();
		if (!State || State->Configuration.Role != GameSessionRole::Server || !State->IsPooled() ||
			!State->ByteAdmission || State->Peers.size() > MaximumGameSessionPeers) return {};
		PooledPostpassSnapshot Result;
		Result.Admission = State->ByteAdmission->GetMetrics();
		Result.Peers.reserve(State->Peers.size());
		for (const auto &[Connection, Peer] : State->Peers) {
			const auto Accepted = ReliableServiceFeedbackAccess::Accepted(State->Scheduler, Connection);
			if (!Accepted) return {};
			const auto &Ledger = Peer.ReliableFeedback;
			PooledPostpassPeer Value;
			Value.Connection = Connection; Value.Ready = Peer.Phase == PeerPhase::Ready;
			Value.Execution = CapturePooledExecution(Ledger);
			Value.Healthy = !Ledger.Invalid && (Ledger.DiagnosticPerformance ||
				(!Ledger.ExecutionFailed && !Ledger.QueuedOfferFailed && !Ledger.QueuedOrdinaryFailed && !Ledger.QueuedOrdinaryEverFailed));
			Value.Accepted = *Accepted; Value.ReconciledAccepted = Ledger.Accepted;
			Value.Retired = Ledger.StructuralRetired; Value.OrdinaryDebt = Ledger.OrdinaryDebt;
			Value.OrdinaryUnsent = Ledger.OrdinaryUnsent;
			Value.Debt = State->ByteAdmission->Debt(Connection); Value.DebtToken = State->ByteAdmission->DebtToken(Connection);
			if (Ledger.Previous) {
				const auto &Native = *Ledger.Previous;
				Value.FeedbackComplete = Native.Connection == Connection && Native.CountersValid &&
					Native.State == ConnectionState::Connected;
				Value.Healthy &= !Native.StructuralCompletionEvidenceInvalid && !Native.ProtectedOrdinaryEvidenceInvalid &&
					(Ledger.DiagnosticPerformance || (!Native.StructuralCompletionServiceFailed && !Native.StructuralLastCompletedCompletionFailed &&
					 !Native.ProtectedOrdinaryEverFailed && !Native.ProtectedOrdinaryActiveDeadlineFailed));
				Value.FirstSent = Native.StructuralPayloadBytesFirstSent; Value.Acked = Native.StructuralPayloadBytesAcked;
				Value.ReliableAcked = Native.ReliablePayloadBytesAcked;
				Value.NativePending = Native.PendingReliableStreamBytes; Value.NativeUnacked = Native.SentUnackedReliableStreamBytes;
				Value.NativeGrantToken = Native.ActiveAttributedRetirementToken;
			}
			Result.Peers.push_back(Value);
		}
		return Result;
	}
	void detail::GameSessionTestAccess::RequestSpatialValidation(GameSession &Session) {
		Session.State->SpatialValidationRequested = true;
		Session.State->SpatialValidationPassed = false;
	}
	std::optional<SpatialCellAddress> detail::GameSessionTestAccess::GetSpatialCellAddress(
		const GameSession &Session, ObjectId Object) {
		return Session.State->Relevance ? Session.State->Relevance->GetSpatialCellAddress(Object) : std::nullopt;
	}
	bool detail::GameSessionTestAccess::RequestClientCharacterAction(GameSession &Session, std::uint32_t Token, std::uint64_t Tick) {
		return Session.State->Prediction && Session.State->PrimaryConnection &&
			Session.State->Prediction->RequestAction(*Session.State->PrimaryConnection, Token, Tick);
	}
	CharacterNetworkMetrics detail::GameSessionTestAccess::GetCharacterMetrics(const GameSession &Session) {
		return Session.State->Authority ? Session.State->Authority->GetMetrics() :
			Session.State->Prediction ? Session.State->Prediction->GetMetrics() : CharacterNetworkMetrics{};
	}
	std::uint64_t detail::GameSessionTestAccess::GetReliableEventsAccepted(const GameSession &Session) {
		return Session.State->Remotes ? Session.State->Remotes->GetMetrics().ReliableEventsAccepted : 0;
	}
	RemoteMetrics detail::GameSessionTestAccess::GetRemoteMetrics(const GameSession &Session) {
		return Session.State->Remotes ? Session.State->Remotes->GetMetrics() : Session.State->FinalRemoteMetrics;
	}
	std::vector<ConnectionId> detail::GameSessionTestAccess::GetConnections(const GameSession &Session) {
		std::vector<ConnectionId> Result;
		for (const auto &[Connection, PeerValue] : Session.State->Peers) Result.push_back(Connection);
		return Result;
	}
	std::vector<detail::JournalRequirement> detail::GameSessionTestAccess::GetJournalRequirements(const GameSession &Session) {
		std::vector<JournalRequirement> Result;
		const auto *Replication = Session.State->Replication.get();
		if (!Replication) return Result;
		Result.push_back({Replication->CatalogCursor, {}, true, false, Replication->NameCoalescingBegin});
		for (const auto &[Connection, Peer] : Replication->Peers)
			Result.push_back({Peer.JournalCursor, Connection, false, Peer.PreparedCommit.has_value(),
				Replication->NameCoalescingBegin, Replication->HasPendingRelevance(Connection)});
		return Result;
	}
	ReplicationMetrics detail::GameSessionTestAccess::GetReplicationMetrics(const GameSession &Session) {
		return Session.State->Replication ? Session.State->Replication->GetMetrics() : ReplicationMetrics{};
	}
	std::optional<SchedulerStatistics> detail::GameSessionTestAccess::GetSchedulerStatistics(
		const GameSession &Session, ConnectionId Connection) {
		return Session.State->Scheduler.GetStatistics(Connection);
	}
	detail::GameSessionTestAccess::GameplayClientCounters detail::GameSessionTestAccess::GetGameplayClientCounters(
		const GameSession &Session) {
		const auto &State = *Session.State;
		return {State.ClientPossessionInput.GetInputsProduced(), State.Metrics.ClientPossessionPoseSnapshotsApplied,
			State.Metrics.ClientStructuralApplyNanoseconds};
	}
	std::optional<std::vector<detail::StructuralCausalSnapshot>>
	detail::GameSessionTestAccess::CaptureStructuralCausalSnapshot(const GameSession &Session, std::string &Error,
		std::uint64_t NativeDeadlineMicroseconds) {
		const auto *State = Session.State.get();
		if (!State || !State->Replication || !State->ByteAdmission || !State->IsPooled()) {
			Error = "Causal snapshot requires a pooled server"; return {};
		}
		const auto &Replication = *State->Replication;
		const auto Tail = ChangeJournal::Get().CreateCursor(Replication.SourceRootId).NextSequence;
		std::vector<StructuralCausalSnapshot> Result;
		Result.reserve(State->Peers.size());
		for (const auto &[Connection, Peer] : State->Peers) {
			const auto Found = Replication.Peers.find(Connection);
			if (Peer.Phase != PeerPhase::Ready || Found == Replication.Peers.end() || Found->second.PreparedCommit) {
				Error = "Causal snapshot requires ready peers without outstanding preparation"; return {};
			}
			const auto &Value = Found->second;
			if (Value.JournalCursor.Scope != Replication.SourceRootId || Value.JournalCursor.NextSequence > Tail) {
				Error = "Causal snapshot journal cursor is invalid"; return {};
			}
			StructuralCausalSnapshot Snapshot{.Connection = Connection, .SourceScope = Replication.SourceRootId,
				.JournalCursor = Value.JournalCursor.NextSequence, .JournalTail = Tail,
				.NextSequence = Value.NextSequence.Value(), .AcceptedRevision = Value.AcceptedRevision,
				.PendingTokenWatermark = Value.NextPendingToken};
			Snapshot.HasUnresolvedPlanning = Value.Planned &&
				(Value.PlanningSelection != Value.ResolvedSelection ||
				 Value.DesiredDependencyCursor.Scope != Replication.DependencyCursor.Scope ||
				 Value.DesiredDependencyCursor.NextSequence != Replication.DependencyCursor.NextSequence);
			Snapshot.Pending.reserve(Value.PendingTransitions.size());
			for (const auto &[Object, Pending] : Value.PendingTransitions)
				Snapshot.Pending.push_back({Pending.Token, Object, Pending.Kind == ReplicationCoordinator::PendingTransitionKind::Enter});
			Snapshot.GrantBytes = State->ByteAdmission->Debt(Connection);
			Snapshot.GrantToken = Snapshot.GrantBytes ? State->ByteAdmission->DebtToken(Connection) : 0;
			const auto Accepted = detail::ReliableServiceFeedbackAccess::Accepted(State->Scheduler, Connection);
			if (!Accepted) { Error = "Causal snapshot requires accepted structural conservation"; return {}; }
			Snapshot.CumulativeAccepted = Accepted->Structural;
			if (Snapshot.GrantBytes) {
				if (Peer.CausalGrantToken != Snapshot.GrantToken || Peer.CausalGrantBytes != Snapshot.GrantBytes ||
					Peer.CausalGrantFingerprint == std::array<std::uint64_t, 2>{} ||
					!Peer.CausalGrantSequence || Peer.CausalAcceptedBefore > Snapshot.CumulativeAccepted ||
					Snapshot.CumulativeAccepted - Peer.CausalAcceptedBefore != Snapshot.GrantBytes) {
					Error = "Causal snapshot accepted grant lacks exact captured identity"; return {};
				}
				Snapshot.GrantSequence = Peer.CausalGrantSequence;
				Snapshot.GrantFingerprint = Peer.CausalGrantFingerprint;
				Snapshot.GrantAcceptedBefore = Peer.CausalAcceptedBefore;
			}
			Result.push_back(std::move(Snapshot));
		}
		// Freeze all Main-owned source/accepted identities before resolving native
		// feedback. No Session step, admission or retirement occurs during this wait.
		for (auto &Snapshot : Result) {
			const auto Native = NativeDeadlineMicroseconds
				? detail::ReliableServiceFeedbackAccess::ObserveResolved(*State->Transport, Snapshot.Connection,
					NativeDeadlineMicroseconds)
				: detail::ReliableServiceFeedbackAccess::Observe(*State->Transport, Snapshot.Connection);
			if (!Native || !Native->CountersValid || Native->Connection != Snapshot.Connection ||
				Native->StructuralPayloadBytesAcked > Native->StructuralPayloadBytesFirstSent ||
				Native->StructuralPayloadBytesFirstSent > Snapshot.CumulativeAccepted ||
				(NativeDeadlineMicroseconds && ServiceTime() >= NativeDeadlineMicroseconds) ||
				(Snapshot.GrantBytes && Snapshot.GrantAcceptedBefore > Native->StructuralPayloadBytesAcked)) {
				Error = "Causal snapshot requires valid resolved structural native conservation"; return {};
			}
			Snapshot.CumulativeFirstSent = Native->StructuralPayloadBytesFirstSent;
			Snapshot.CumulativeAcked = Native->StructuralPayloadBytesAcked;
			if (Snapshot.GrantBytes) {
				Snapshot.GrantFirstSent = Snapshot.CumulativeFirstSent - Snapshot.GrantAcceptedBefore;
				Snapshot.GrantAcked = Snapshot.CumulativeAcked - Snapshot.GrantAcceptedBefore;
			}
		}
		for (const auto &Snapshot : Result) {
			const auto Found = Replication.Peers.find(Snapshot.Connection);
			const auto Peer = State->Peers.find(Snapshot.Connection);
			const auto Accepted = detail::ReliableServiceFeedbackAccess::Accepted(State->Scheduler, Snapshot.Connection);
			if (Found == Replication.Peers.end() || Peer == State->Peers.end() || Peer->second.Phase != PeerPhase::Ready ||
				!Accepted || Accepted->Structural != Snapshot.CumulativeAccepted || Found->second.PreparedCommit ||
				Found->second.JournalCursor.Scope != Snapshot.SourceScope ||
				Found->second.JournalCursor.NextSequence != Snapshot.JournalCursor ||
				Found->second.NextSequence.Value() != Snapshot.NextSequence ||
				Found->second.AcceptedRevision != Snapshot.AcceptedRevision ||
				Found->second.NextPendingToken != Snapshot.PendingTokenWatermark ||
				Found->second.PendingTransitions.size() != Snapshot.Pending.size() ||
				State->ByteAdmission->Debt(Snapshot.Connection) != Snapshot.GrantBytes ||
				(Snapshot.GrantBytes && State->ByteAdmission->DebtToken(Snapshot.Connection) != Snapshot.GrantToken)) {
				Error = "Causal snapshot source or accepted debt changed during native resolution"; return {};
			}
			const auto &Value = Found->second;
			const bool PlanningUnresolved = Value.Planned &&
				(Value.PlanningSelection != Value.ResolvedSelection ||
				 Value.DesiredDependencyCursor.Scope != Replication.DependencyCursor.Scope ||
				 Value.DesiredDependencyCursor.NextSequence != Replication.DependencyCursor.NextSequence);
			if (PlanningUnresolved != Snapshot.HasUnresolvedPlanning ||
				(Snapshot.GrantBytes && (Peer->second.CausalGrantToken != Snapshot.GrantToken ||
				 Peer->second.CausalGrantBytes != Snapshot.GrantBytes ||
				 Peer->second.CausalGrantSequence != Snapshot.GrantSequence ||
				 Peer->second.CausalGrantFingerprint != Snapshot.GrantFingerprint ||
				 Peer->second.CausalAcceptedBefore != Snapshot.GrantAcceptedBefore))) {
				Error = "Causal snapshot planning or grant identity changed during native resolution"; return {};
			}
			for (const auto &Pending : Snapshot.Pending) {
				const auto Value = Found->second.PendingTransitions.find(Pending.Object);
				if (Value == Found->second.PendingTransitions.end() || Value->second.Token != Pending.Token ||
					(Value->second.Kind == ReplicationCoordinator::PendingTransitionKind::Enter) != Pending.Enter) {
					Error = "Causal snapshot pending identity changed during native resolution"; return {};
				}
			}
		}
		if (State->Peers.size() != Result.size() || Replication.Peers.size() != Result.size() ||
			ChangeJournal::Get().CreateCursor(Replication.SourceRootId).NextSequence != Tail) {
			Error = "Causal snapshot journal changed during native resolution"; return {};
		}
		if (NativeDeadlineMicroseconds && ServiceTime() >= NativeDeadlineMicroseconds) {
			Error = "Causal snapshot native resolution exceeded its shared deadline"; return {};
		}
		Error.clear(); return Result;
	}
	bool detail::GameSessionTestAccess::SetOrdinarySourceNormal(GameSession &Session, bool Normal) {
		return !Session.State->OrdinarySource || Session.State->OrdinarySource->SetNormal(Normal);
	}
	std::optional<std::vector<detail::StructuralCausalSnapshot>>
	detail::GameSessionTestAccess::CaptureStructuralSourceSnapshot(const GameSession &Session, std::string &Error) {
		const auto *State = Session.State.get();
		if (!State || !State->Replication || !State->ByteAdmission || !State->IsPooled()) {
			Error = "Causal source snapshot requires a pooled server"; return {};
		}
		const auto &Replication = *State->Replication;
		const auto Tail = ChangeJournal::Get().CreateCursor(Replication.SourceRootId).NextSequence;
		std::vector<StructuralCausalSnapshot> Result;
		Result.reserve(State->Peers.size());
		for (const auto &[Connection, Peer] : State->Peers) {
			const auto Found = Replication.Peers.find(Connection);
			if (Peer.Phase != PeerPhase::Ready || Found == Replication.Peers.end() || Found->second.PreparedCommit) {
				Error = "Causal source snapshot requires ready peers without outstanding preparation"; return {};
			}
			const auto &Value = Found->second;
			if (Value.JournalCursor.Scope != Replication.SourceRootId || Value.JournalCursor.NextSequence > Tail) {
				Error = "Causal source snapshot journal cursor is invalid"; return {};
			}
			StructuralCausalSnapshot Snapshot{.Connection = Connection, .SourceScope = Replication.SourceRootId,
				.JournalCursor = Value.JournalCursor.NextSequence, .JournalTail = Tail,
				.NextSequence = Value.NextSequence.Value(), .AcceptedRevision = Value.AcceptedRevision,
				.PendingTokenWatermark = Value.NextPendingToken};
			Snapshot.Pending.reserve(Value.PendingTransitions.size());
			for (const auto &[Object, Pending] : Value.PendingTransitions)
				Snapshot.Pending.push_back({Pending.Token, Object,
					Pending.Kind == ReplicationCoordinator::PendingTransitionKind::Enter});
			Result.push_back(std::move(Snapshot));
		}
		Error.clear(); return Result;
	}
	std::optional<std::vector<detail::StructuralDeliverySnapshot>>
	detail::GameSessionTestAccess::GetStructuralDeliverySnapshots(const GameSession &Session, std::string &Error) {
		const auto *State = Session.State.get();
		if (!State || !State->ByteAdmission || !State->IsPooled()) {
			Error = "Structural delivery requires a pooled server"; return {};
		}
		std::vector<StructuralDeliverySnapshot> Result;
		Result.reserve(State->Peers.size());
		for (const auto &[Connection, Peer] : State->Peers) {
			if (Peer.Phase != PeerPhase::Ready) { Error = "Structural delivery peer is not ready"; return {}; }
			const auto Accepted = detail::ReliableServiceFeedbackAccess::Accepted(State->Scheduler, Connection);
			const auto Native = detail::ReliableServiceFeedbackAccess::Observe(*State->Transport, Connection);
			if (!Accepted || !Native || !Native->CountersValid || Native->Connection != Connection ||
				Native->StructuralPayloadBytesAcked > Native->StructuralPayloadBytesFirstSent ||
				Native->StructuralPayloadBytesFirstSent > Accepted->Structural) {
				Error = "Structural delivery conservation is invalid"; return {};
			}
			const auto Debt = State->ByteAdmission->Debt(Connection);
			Result.push_back({Connection, Accepted->Structural, Native->StructuralPayloadBytesFirstSent,
				Native->StructuralPayloadBytesAcked, Debt ? State->ByteAdmission->DebtToken(Connection) : 0, Debt});
		}
		Error.clear(); return Result;
	}
	std::optional<detail::FrozenCessationQuote> detail::GameSessionTestAccess::CaptureFrozenCessationQuote(
		const GameSession &Session, std::string &Error) {
		const auto *State = Session.State.get();
		if (!State || !State->Replication || !State->ByteAdmission || !State->IsPooled() ||
			State->Peers.empty() || State->Peers.size() != State->Replication->Peers.size()) {
			Error = "Frozen cessation quote requires a complete pooled server peer set";
			return {};
		}
		FrozenCessationQuote Result;
		std::uint64_t AcceptedSum = 0;
		for (const auto &[Connection, Peer] : State->Peers) {
			if (Peer.Phase != PeerPhase::Ready) {
				Error = "Frozen cessation quote requires every peer to be ready";
				return {};
			}
			const auto MaximumFrame = std::min<std::uint64_t>(Peer.Limits.MaximumReliableMessageBytes,
				MaximumReliableServiceGroupBytes - ReliableServiceEnvelopeBytes);
			if (MaximumFrame < 36) {
				Error = "Frozen cessation quote peer frame limit is invalid";
				return {};
			}
			Result.MaximumFrameBytes.emplace(Connection, static_cast<std::size_t>(MaximumFrame));
			const auto Debt = State->ByteAdmission->Debt(Connection);
			if (Debt > MaximumReliableServiceGroupBytes ||
				Debt > std::numeric_limits<std::uint64_t>::max() - AcceptedSum) {
				Error = "Frozen cessation quote accepted debt is invalid";
				return {};
			}
			AcceptedSum += Debt;
			Result.AcceptedUnretiredCompleteBytes.emplace(Connection, Debt);
		}
		if (AcceptedSum != State->ByteAdmission->GetMetrics().OutstandingBytes) {
			Error = "Frozen cessation quote accepted debt does not conserve";
			return {};
		}
		Result.Replication = State->Replication->CaptureFrozenQuote(Error);
		if (!Result.Replication) return {};
		Error.clear();
		return Result;
	}
}
