// Included in GameSession::Implementation. All transfer work runs on Main.
using SessionTransferRequest = SessionSpatialTransfer::Request;

static bool HasTransfer(const Peer &Value) {
	return Value.SpatialTransfer && Value.SpatialTransfer->Pending.has_value();
}
static bool HasCommittedTransfer(const Peer &Value) {
	return HasTransfer(Value) && Value.SpatialTransfer->Pending->Committed;
}
bool IsTransferSubject(ObjectId Object) const {
	return !TransferPoseSubjects.empty() && TransferPoseSubjects.contains(Object);
}
std::shared_ptr<Player> TransferPlayer(ConnectionId Connection, const Peer &Value) const {
	if (!Configuration.SpatialContent || !SpatialProtection || !Runtime || Value.Phase != PeerPhase::Ready ||
		PendingPeerFailures.contains(Connection)) return {};
	if (Configuration.Role == GameSessionRole::Server) return ResolveRemotePlayer({Connection, Value.Replication});
	if (PrimaryConnection != Connection || !ClientRuntimeAttached || Value.Replication != Replica.GetEpoch()) return {};
	const auto PlayerValue = Runtime->Players->GetLocalPlayer().value_or(nullptr);
	return PlayerValue && PlayerValue == Replica.Resolve(Value.PlayerObject) ? PlayerValue : nullptr;
}
static std::shared_ptr<BasePart> TransferSubject(const Player &PlayerValue) {
	if (const auto Entity = GenericPossession(PlayerValue)) return std::dynamic_pointer_cast<BasePart>(Entity);
	const auto CharacterValue = std::dynamic_pointer_cast<KinematicCharacter>(PlayerValue.GetCharacter().value_or(nullptr));
	return CharacterValue ? CharacterValue->GetRootPart().value_or(nullptr) : nullptr;
}
std::shared_ptr<Instance> ResolveTransferObject(WireObjectId Id) const {
	if (!Runtime || !Id.IsValid()) return {};
	auto Value = Configuration.Role == GameSessionRole::Server ? ObjectRegistry::Get().Lookup(Id.ToObjectId()) : Replica.Resolve(Id.ToObjectId());
	return Value && !Value->GetDestroyed() && !Value->IsDestroying() && Value->GetDataModel() == Runtime->DataModel ? Value : nullptr;
}
WireObjectId TransferIdentity(const std::shared_ptr<Instance> &Object) const {
	if (!Object || !Runtime || Object->GetDestroyed() || Object->IsDestroying() || Object->GetDataModel() != Runtime->DataModel) return {};
	const auto Id = Configuration.Role == GameSessionRole::Server ? std::optional(Object->GetObjectId()) : Replica.GetSourceObject(*Object);
	return Id ? WireObjectId::FromObjectId(*Id) : WireObjectId{};
}
bool TransferScopeCurrent(ConnectionId Connection, const Peer &Value, const SessionTransferRequest &Request) const {
	const auto PlayerValue = TransferPlayer(Connection, Value);
	const auto &Scope = Request.Scope;
	if (!PlayerValue || Value.SessionEpoch != Scope.SessionEpoch || Value.Replication.Value() != Scope.WorldEpoch ||
		Value.PlayerId != Scope.PlayerId || Scope.FocusEpoch != Request.Serial ||
		PlayerValue->GetPossessionRevision() != Request.PossessionRevision) return false;
	const auto Subject = TransferSubject(*PlayerValue);
	if (!Subject || TransferIdentity(Subject) != Scope.Subject) return false;
	const auto Controller = LegacyCharacter(PlayerValue);
	if (TransferIdentity(Controller) != Scope.Controller) return false;
	const auto Origin = std::dynamic_pointer_cast<WorldRoot>(ResolveTransferObject(Scope.Source));
	if (!Origin || !Origin->GetSpatialActivation()) return false;
	const auto AuthorizedSource = Origin == Runtime->Workspace ? ObjectId{} : Scope.Source.ToObjectId();
	if (Value.AuthorizedSpatialRegion != AuthorizedSource) return false;
	const auto CurrentDomain = TransferIdentity(WorldRoot::GetEffectiveWorld(*Subject));
	return CurrentDomain == Scope.Source || (Request.Committed && CurrentDomain == Scope.Destination);
}
std::optional<ObjectId> ResolveTransferOwner(const SpatialWireProvider &Key) const {
	const auto Object = ResolveTransferObject(Key.Owner);
	if (!Object) return {};
	if (Key.Kind == SpatialCollisionProvider::Terrain) {
		const auto Domain = std::dynamic_pointer_cast<WorldRoot>(Object);
		const auto Owner = Domain ? Domain->GetOwnedTerrain() : nullptr;
		return Owner ? std::optional(Owner->GetObjectId()) : std::nullopt;
	}
	if ((Key.Kind == SpatialCollisionProvider::BlockGrid && !std::dynamic_pointer_cast<BlockGrid>(Object)) ||
		(Key.Kind == SpatialCollisionProvider::Rigid && !std::dynamic_pointer_cast<BasePart>(Object))) return {};
	return Object->GetObjectId();
}
std::optional<SpatialAppliedBinding> ProjectTransferProvider(ConnectionId Connection, const Peer &Value,
	const SpatialProviderReceipt &Native, const SpatialDestinationBounds &Bounds) {
	if (!HasTransfer(Value)) return {};
	const auto &Request = *Value.SpatialTransfer->Pending;
	const auto Destination = std::dynamic_pointer_cast<WorldRoot>(ResolveTransferObject(Request.Scope.Destination));
	if (!Destination || Request.Bounds != Bounds ||
		!HasTransferDomain(Connection, Request.Scope.Destination, Bounds, Request.Serial)) return {};
	if (Native.Provider == SpatialCollisionProvider::BlockGrid) return GetTransferGridBinding(Connection, Native, Bounds);
	if (Native.Provider == SpatialCollisionProvider::Terrain) {
		const auto Owner = Destination->GetOwnedTerrain();
		if (!Owner || Owner->GetObjectId() != Native.Owner) return {};
		const SpatialWireProvider Key{Request.Scope.Destination, Native.Provider};
		if (Destination == Runtime->Workspace && Configuration.TerrainBootstrap) {
			const auto &Identity = Value.TerrainIdentity;
			if (!Value.TerrainWindow || Value.TerrainPending || !Value.TerrainAdmitted ||
				Identity.SessionEpoch != Value.SessionEpoch || !Identity.Serial ||
				!Identity.ManifestDigest.IsValid() || Identity.ManifestDigest != Value.TerrainWindow->ManifestDigest ||
				!Destination->IsTerrainCollisionReady() ||
				(Configuration.Role == GameSessionRole::Server && !IsCurrentTerrainWindow(Owner->GetField(), Value))) return {};
			const auto Center = glm::vec3((glm::dvec3(Bounds.Minimum) + glm::dvec3(Bounds.Maximum)) * .5);
			const auto Extents = glm::vec3((glm::dvec3(Bounds.Maximum) - glm::dvec3(Bounds.Minimum)) * .5);
			if (!ContainsTerrainBounds(Value.TerrainWindow->Grid, Value.TerrainWindow->Center, Center, Extents)) return {};
			return SpatialAppliedBinding{Key, Identity.ManifestDigest, Identity.FieldRevision, Identity.Serial, true,
				Native.Installed.empty()};
		}
		const auto Empty = GetTransferEmptyTerrainBinding(Connection, Request.Scope.Destination, Bounds, Request.Serial);
		if (!Empty || !Empty->IsValid() || !Native.AuthoritativeEmpty || !Native.Installed.empty()) return {};
		return SpatialAppliedBinding{Key, *Empty, 0, Request.Serial, true, true};
	}
	const auto Part = std::dynamic_pointer_cast<BasePart>(ObjectRegistry::Get().Lookup(Native.Owner));
	const auto Key = TransferIdentity(Part);
	if (!Part || !Key.IsValid() || WorldRoot::GetEffectiveWorld(*Part) != Destination || Native.Installed.size() != 1 ||
		!Native.Resource.empty() || Native.Shape > PhysicsShapeKind::CornerWedge) return {};
	// For native primitives the backend shape, dimensions, pose and collision filters are encoded
	// independently by SR2's canonical receipt. Resource-backed triangle meshes require a verified leaf.
	static const auto Primitive = [] {
		constexpr std::string_view Profile{"Gargantuan.RigidPrimitive.1"};
		return AssetContentId::Hash({reinterpret_cast<const std::uint8_t *>(Profile.data()), Profile.size()});
	}();
	return SpatialAppliedBinding{{Key, Native.Provider}, Primitive, 1, Request.Serial, true, Native.AuthoritativeEmpty};
}
SpatialTransferAdapters SessionTransferAdapters(ConnectionId Connection) {
	return {
		.Project = [this, Connection](const SpatialProviderReceipt &Provider, const SpatialDestinationBounds &Bounds) {
			const auto Found = Peers.find(Connection);
			return Found == Peers.end() ? std::optional<SpatialAppliedBinding>{} : ProjectTransferProvider(Connection, Found->second, Provider, Bounds);
		},
		.Resolve = [this](SpatialWireProvider Key) { return ResolveTransferOwner(Key); },
		.ResolveObject = [this](WireObjectId Id) -> std::optional<ObjectId> {
			const auto Object = ResolveTransferObject(Id);
			return Object ? std::optional(Object->GetObjectId()) : std::nullopt;
		},
		.Reserve = [this, Connection](const SpatialTransferFrame &Frame, const SpatialDestinationReceipt &Receipt) -> SpatialTransferLease {
			const auto Subject = ResolveTransferObject(Frame.Scope.Subject);
			if (!Subject || !SpatialProtection) return {};
			return SpatialProtection->Reservations.Reserve(Subject->GetObjectId(), Frame, Receipt,
				[this, Connection, Scope = Frame.Scope, Serial = Frame.Serial, Bounds = Frame.Bounds] {
					const auto Found = Peers.find(Connection);
					return Found != Peers.end() && HasTransfer(Found->second) && Found->second.SpatialTransfer->Pending->Serial == Serial &&
						Found->second.SpatialTransfer->Pending->Scope == Scope &&
						TransferScopeCurrent(Connection, Found->second, *Found->second.SpatialTransfer->Pending) &&
						HasTransferDomain(Connection, Scope.Destination, Bounds, Serial);
				});
		}
	};
}
bool SendTransferFrame(ConnectionId Connection, Peer &Value, const SpatialTransferFrame &Frame) {
	auto Bytes = EncodeSpatialTransfer(Frame);
	if (!Bytes.empty() && QueueTerrainFrame(Connection, Value, std::move(Bytes))) return true;
	PendingPeerFailures.try_emplace(Connection, DisconnectInfo{DisconnectReason::ResourceExhaustion,
		"[Spatial:Transfer] Reliable movement disposition could not be queued"});
	return false;
}
void FenceTransferMotion(ConnectionId Connection, Peer &Value, const SpatialTransferScope &Scope, bool Fenced) {
	if (Fenced) TransferPoseSubjects.insert(Scope.Subject.ToObjectId());
	else TransferPoseSubjects.erase(Scope.Subject.ToObjectId());
	if (Configuration.Role == GameSessionRole::Server) {
		if (Scope.Controller.IsValid() && Authority) {
			if (Fenced && Value.ControlledCharacter == Scope.Controller.ToObjectId()) {
				(void)Authority->RevokeControl(Value.ControlledCharacter, std::max(CurrentTick, std::uint64_t{1}));
				Value.ControlledCharacter = {};
			}
			Authority->SetSpatialTransferFence(Scope.Controller.ToObjectId(), Fenced);
		}
		if (Fenced && Value.ControlledPossession.IsValid()) {
			if (!ServerPossessionInput.SetPossession(Connection, {}, std::max(CurrentTick, std::uint64_t{1}), Value.Limits))
				PendingPeerFailures.try_emplace(Connection, DisconnectInfo{DisconnectReason::ResourceExhaustion, "[Spatial:Transfer] Input revocation failed"});
			Value.ControlledPossession = {};
			Value.ControlledPossessionRevision = 0;
		}
		if (Fenced) {
			const auto Subject = Scope.Subject.ToObjectId();
			for (auto &[OtherConnection, Other] : Peers) if (Other.PossessionPoses.contains(Subject))
				RetirePossessionPoseBinding(OtherConnection, Other, Subject);
		}
	} else {
		if (Prediction && Scope.Controller.IsValid()) Prediction->SetSpatialTransferFence(Scope.Controller.ToObjectId(), Fenced);
		if (Fenced) {
			ClientPosePresentation.Forget(Scope.Subject.ToObjectId());
			ClientPossessionPoses.erase(Scope.Subject.ToObjectId());
			if (const auto Subject = std::dynamic_pointer_cast<BasePart>(ResolveTransferObject(Scope.Subject)))
				Subject->SetCharacterPresentationOffset(std::nullopt);
		}
	}
}
void RetireSessionTransfer(ConnectionId Connection, Peer &Value) {
	if (!Value.SpatialTransfer) return;
	if (HasTransfer(Value)) {
		const auto Request = *Value.SpatialTransfer->Pending;
		Value.SpatialTransfer->Server.Reset();
		Value.SpatialTransfer->Replica.Reset();
		FenceTransferMotion(Connection, Value, Request.Scope, false);
		if (Configuration.Role == GameSessionRole::Server) (void)ReleaseTransferDomain(Connection, Request.Serial);
		Value.SpatialTransfer->Pending.reset();
	}
}
void FinishTransfer(ConnectionId Connection, Peer &Value, SpatialTransferStatus Status) {
	if (!HasTransfer(Value)) return;
	const auto Request = *Value.SpatialTransfer->Pending;
	if (Configuration.Role == GameSessionRole::Server) {
		const bool Promote = Status == SpatialTransferStatus::Applied;
		if (!ReleaseTransferDomain(Connection, Request.Serial, Promote) && Promote)
			Status = SpatialTransferStatus::CommittedUnconfirmed;
	}
	Value.SpatialTransfer->Server.Reset();
	Value.SpatialTransfer->Replica.Reset();
	Value.SpatialTransfer->Pending.reset();
	Value.SpatialTransfer->Result = Status;
	FenceTransferMotion(Connection, Value, Request.Scope, false);
	if (Status == SpatialTransferStatus::CommittedUnconfirmed)
		PendingPeerFailures.try_emplace(Connection, DisconnectInfo{DisconnectReason::ProtocolViolation,
			"[Spatial:Transfer] Committed actor requires controlling replica resynchronization"});
}
SpatialTransferStatus CancelTransfer(ConnectionId Connection, Peer &Value) {
	if (!HasTransfer(Value)) return SpatialTransferStatus::Stale;
	auto &Lane = *Value.SpatialTransfer;
	auto Step = Configuration.Role == GameSessionRole::Server ? Lane.Server.Cancel() : Lane.Replica.Cancel();
	if (Step.Status == SpatialTransferStatus::Busy) return Step.Status;
	if (Step.Reply) (void)SendTransferFrame(Connection, Value, *Step.Reply);
	const auto Status = Lane.Pending->Committed || Step.Status == SpatialTransferStatus::CommittedUnconfirmed ?
		SpatialTransferStatus::CommittedUnconfirmed : SpatialTransferStatus::Canceled;
	FinishTransfer(Connection, Value, Status);
	return Status;
}
void AdvanceTransfer(ConnectionId Connection, Peer &Value, const SpatialTransferStep &Step) {
	if (!HasTransfer(Value) || Step.Status == SpatialTransferStatus::Busy || Step.Status == SpatialTransferStatus::Stale) return;
	auto &Request = *Value.SpatialTransfer->Pending;
	if (Step.Reply) {
		if (Step.Reply->Phase == SpatialTransferPhase::Commit || Step.Reply->Phase == SpatialTransferPhase::Applied)
			Request.Committed = true;
		if (!SendTransferFrame(Connection, Value, *Step.Reply)) {
			FinishTransfer(Connection, Value, Request.Committed ? SpatialTransferStatus::CommittedUnconfirmed : SpatialTransferStatus::Canceled);
			return;
		}
	}
	if (Step.Status != SpatialTransferStatus::Progress) {
		if (Configuration.Role == GameSessionRole::Client && Step.Status == SpatialTransferStatus::Applied) return;
		FinishTransfer(Connection, Value, Step.Status);
	}
}
SpatialTransferStatus StartSpatialTransfer(ConnectionId Connection, ObjectId DestinationId, const CFrame &Pose,
	const SpatialDestinationBounds &Bounds) {
	if (GetCurrentExecutionDomain() != ExecutionDomain::Main || Configuration.Role != GameSessionRole::Server ||
		!Configuration.SpatialContent || !SpatialProtection) return SpatialTransferStatus::Denied;
	const auto Found = Peers.find(Connection);
	if (Found == Peers.end()) return SpatialTransferStatus::Denied;
	auto &Value = Found->second;
	if (HasTransfer(Value)) return SpatialTransferStatus::Busy;
	const auto PlayerValue = TransferPlayer(Connection, Value);
	const auto Subject = PlayerValue ? TransferSubject(*PlayerValue) : nullptr;
	const auto Source = Subject ? WorldRoot::GetEffectiveWorld(*Subject) : nullptr;
	const auto Destination = std::dynamic_pointer_cast<WorldRoot>(ResolveTransferObject(WireObjectId::FromObjectId(DestinationId)));
	const auto Profile = WorldRoot::DescribeSpatialMovement(Subject);
	if (!PlayerValue || !Source || !Destination || !Profile || !Source->CanHoldSpatialSubject(Subject->GetObjectId()) ||
		!Destination->CanHoldSpatialCollision(Subject->GetObjectId())) return SpatialTransferStatus::Denied;
	const bool Controls = Profile->Controller.IsValid() ? Value.ControlledCharacter == Profile->Controller :
		Value.ControlledPossession == Subject->GetObjectId();
	if (!Controls) return SpatialTransferStatus::Denied;
	const SpatialMovementRequest Movement{Subject->GetObjectId(), Source->GetObjectId(), Source->GetSpatialActivation(),
		Subject->GetCFrame(), Pose};
	const auto Native = Destination->PrepareSpatialDestination(Bounds);
	if (!Native || !Destination->CheckSpatialLanding(Subject, Movement, *Native).Succeeded()) return SpatialTransferStatus::Denied;
	if (!Value.SpatialTransfer) Value.SpatialTransfer = std::make_unique<SessionSpatialTransfer>();
	auto &Lane = *Value.SpatialTransfer;
	if (Lane.LastSerial == std::numeric_limits<std::uint64_t>::max()) return SpatialTransferStatus::Denied;
	const auto Serial = ++Lane.LastSerial;
	Lane.Pending.emplace(SessionTransferRequest{
		.Scope = {Value.SessionEpoch, Value.Replication.Value(), Serial, Value.PlayerId,
			TransferIdentity(Subject), TransferIdentity(Source), TransferIdentity(Destination), WireObjectId::FromObjectId(Profile->Controller)},
		.Bounds = Bounds, .Serial = Serial, .PossessionRevision = PlayerValue->GetPossessionRevision(),
		.SourcePose = Movement.SourcePose, .DestinationPose = Pose, .Deadline = SpatialTransferClock::now() + std::chrono::seconds(3)});
	const auto Scope = Lane.Pending->Scope;
	if (!TransferScopeCurrent(Connection, Value, *Lane.Pending) || !StageTransferDomain(Connection, Scope.Destination, Bounds, Serial)) {
		Lane.Pending.reset();
		Lane.Result = SpatialTransferStatus::Denied;
		return *Lane.Result;
	}
	Lane.Result = SpatialTransferStatus::Progress;
	FenceTransferMotion(Connection, Value, Scope, true);
	PumpSessionTransfer(Connection, Value);
	return Lane.Result.value_or(SpatialTransferStatus::Denied);
}
bool ApplyReplicaTransfer(const SpatialTransferFrame &Frame, const SpatialDestinationReceipt &Receipt,
	const SpatialTransferLease &Lease) {
	const auto Subject = std::dynamic_pointer_cast<BasePart>(ResolveTransferObject(Frame.Scope.Subject));
	const auto Source = std::dynamic_pointer_cast<WorldRoot>(ResolveTransferObject(Frame.Scope.Source));
	const auto Destination = std::dynamic_pointer_cast<WorldRoot>(ResolveTransferObject(Frame.Scope.Destination));
	if (!Subject || !Source || !Destination) return false;
	return Destination->CommitSpatialTransfer(Subject, {Subject->GetObjectId(), Source->GetObjectId(), Source->GetSpatialActivation(),
		GetSpatialTransferPose(Frame.SourcePose), GetSpatialTransferPose(Frame.DestinationPose)}, Receipt, Lease).Succeeded();
}
void ReceiveSpatialTransfer(const ReceivedMessageEvent &Message, Peer &Value) {
	if (!Configuration.SpatialContent || Value.Phase != PeerPhase::Ready || Message.Delivery != DeliveryMode::ReliableOrdered ||
		Message.Traffic != TrafficClass::Control || !std::holds_alternative<std::monostate>(Message.Order)) {
		Reject(Message.Connection, DisconnectReason::ProtocolViolation, "Spatial transfer outside its negotiated control channel");
		return;
	}
	const auto Frame = DecodeSpatialTransfer(Message.Payload);
	if (!Frame) {
		Reject(Message.Connection, DisconnectReason::ProtocolViolation, "Malformed spatial movement frame");
		return;
	}
	const auto Connection = Message.Connection;
	const bool Server = Configuration.Role == GameSessionRole::Server;
	if ((Server && (Frame->Phase == SpatialTransferPhase::Offer || Frame->Phase == SpatialTransferPhase::Reserve || Frame->Phase == SpatialTransferPhase::Commit)) ||
		(!Server && (Frame->Phase == SpatialTransferPhase::Ready || Frame->Phase == SpatialTransferPhase::Held || Frame->Phase == SpatialTransferPhase::Applied))) {
		Reject(Connection, DisconnectReason::ProtocolViolation, "Spatial movement phase does not belong to the sender");
		return;
	}
	const auto &Scope = Frame->Scope;
	if (Scope.SessionEpoch != Value.SessionEpoch || Scope.WorldEpoch != Value.Replication.Value() ||
		Scope.PlayerId != Value.PlayerId || Scope.FocusEpoch != Frame->Serial) return;
	if (!Server && Frame->Phase == SpatialTransferPhase::Offer && !HasTransfer(Value)) {
		const auto PlayerValue = TransferPlayer(Connection, Value);
		if (!PlayerValue || TransferIdentity(TransferSubject(*PlayerValue)) != Scope.Subject ||
			TransferIdentity(LegacyCharacter(PlayerValue)) != Scope.Controller ||
			!HasTransferDomain(Connection, Scope.Destination, Frame->Bounds, Frame->Serial)) return;
		if (!Value.SpatialTransfer) Value.SpatialTransfer = std::make_unique<SessionSpatialTransfer>();
		auto &Lane = *Value.SpatialTransfer;
		if (Frame->Serial <= Lane.LastSerial) return;
		Lane.Pending.emplace(SessionTransferRequest{
			.Scope = Scope, .Bounds = Frame->Bounds, .Serial = Frame->Serial, .PossessionRevision = PlayerValue->GetPossessionRevision(),
			.SourcePose = GetSpatialTransferPose(Frame->SourcePose), .DestinationPose = GetSpatialTransferPose(Frame->DestinationPose),
			.Deadline = SpatialTransferClock::now() + std::chrono::seconds(3), .WaitingOffer = *Frame});
		if (!TransferScopeCurrent(Connection, Value, *Lane.Pending)) { Lane.Pending.reset(); return; }
		Lane.LastSerial = Frame->Serial;
		Lane.Result = SpatialTransferStatus::Progress;
		FenceTransferMotion(Connection, Value, Scope, true);
		PumpSessionTransfer(Connection, Value);
		return;
	}
	if (!HasTransfer(Value)) return;
	auto &Lane = *Value.SpatialTransfer;
	auto &Request = *Lane.Pending;
	if (Request.Scope != Scope || Request.Serial != Frame->Serial || Request.Bounds != Frame->Bounds) return;
	if (!TransferScopeCurrent(Connection, Value, Request)) { (void)CancelTransfer(Connection, Value); return; }
	if (!Request.Started) {
		if (Frame->Phase == SpatialTransferPhase::Cancel) (void)CancelTransfer(Connection, Value);
		return;
	}
	const auto Adapters = SessionTransferAdapters(Connection);
	const auto Destination = std::dynamic_pointer_cast<WorldRoot>(ResolveTransferObject(Scope.Destination));
	const auto Step = Server ? Lane.Server.Receive(Connection, Scope, *Frame, Adapters, SpatialTransferClock::now()) :
		Lane.Replica.Receive(Scope, *Frame, Destination, Adapters,
			[this](const auto &F, const auto &Receipt, const auto &Lease) { return ApplyReplicaTransfer(F, Receipt, Lease); });
	AdvanceTransfer(Connection, Value, Step);
}
void PumpSessionTransfer(ConnectionId Connection, Peer &Value) {
	if (!HasTransfer(Value)) return;
	auto &Lane = *Value.SpatialTransfer;
	auto &Request = *Lane.Pending;
	const auto Now = SpatialTransferClock::now();
	const bool Server = Configuration.Role == GameSessionRole::Server;
	if (!Server && Request.Committed && !HasTransferDomain(Connection, Request.Scope.Destination, Request.Bounds, Request.Serial)) {
		const auto Domain = ResolveTransferObject(Request.Scope.Destination);
		const auto Expected = Domain == Runtime->Workspace ? ObjectId{} : Request.Scope.Destination.ToObjectId();
		const auto Owner = TransferPlayer(Connection, Value);
		const auto Subject = Owner ? TransferSubject(*Owner) : nullptr;
		const bool Moved = Subject && TransferIdentity(Subject) == Request.Scope.Subject && Domain &&
			WorldRoot::GetEffectiveWorld(*Subject) == Domain && Value.AuthorizedSpatialRegion == Expected;
		FinishTransfer(Connection, Value, Moved ? SpatialTransferStatus::Applied : SpatialTransferStatus::CommittedUnconfirmed);
		return;
	}
	if (!TransferScopeCurrent(Connection, Value, Request) ||
		!HasTransferDomain(Connection, Request.Scope.Destination, Request.Bounds, Request.Serial)) {
		(void)CancelTransfer(Connection, Value);
		return;
	}
	if (Now >= Request.Deadline) {
		if (CancelTransfer(Connection, Value) == SpatialTransferStatus::Canceled) Lane.Result = SpatialTransferStatus::Expired;
		return;
	}
	const auto Adapters = SessionTransferAdapters(Connection);
	if (Request.Started) {
		AdvanceTransfer(Connection, Value, Server ? Lane.Server.Poll(Connection, Request.Scope, Adapters, Now) : Lane.Replica.Poll(Request.Scope, Adapters));
		return;
	}
	const auto Subject = std::dynamic_pointer_cast<BasePart>(ResolveTransferObject(Request.Scope.Subject));
	const auto Source = std::dynamic_pointer_cast<WorldRoot>(ResolveTransferObject(Request.Scope.Source));
	const auto Destination = std::dynamic_pointer_cast<WorldRoot>(ResolveTransferObject(Request.Scope.Destination));
	if (!Destination || !Source || !Subject) return;
	if (!Server) {
		if (!Request.WaitingOffer || !IsSpatialOfferInstalled(*Request.WaitingOffer, *Destination, Adapters)) return;
		auto Frame = std::move(*Request.WaitingOffer);
		Request.WaitingOffer.reset();
		Request.Started = true;
		AdvanceTransfer(Connection, Value, Lane.Replica.Receive(Request.Scope, Frame, Destination, Adapters,
			[this](const auto &F, const auto &Receipt, const auto &Lease) { return ApplyReplicaTransfer(F, Receipt, Lease); }));
		return;
	}
	const SpatialMovementRequest Movement{Subject->GetObjectId(), Source->GetObjectId(), Source->GetSpatialActivation(),
		Request.SourcePose, Request.DestinationPose};
	const auto Step = Lane.Server.Begin(Connection, Request.Scope, Subject, Destination, Movement, Request.Bounds, {}, Adapters, Now, Request.Serial);
	if (Step.Status == SpatialTransferStatus::Denied) return;
	Request.Started = Step.Status == SpatialTransferStatus::Progress;
	AdvanceTransfer(Connection, Value, Step);
}
void PumpSpatialTransfers() {
	for (auto &[Connection, Value] : Peers) if (HasTransfer(Value) && !PendingPeerFailures.contains(Connection))
		PumpSessionTransfer(Connection, Value);
}

