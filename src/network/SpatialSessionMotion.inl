// Controlled spatial movement is authoritative. The client acknowledges native collision in the same chart.
std::optional<SpatialAppliedBinding> ProjectMotionProvider(ConnectionId Connection, const Peer &Value,
	WorldRoot &World, const SpatialProviderReceipt &Native, const SpatialDestinationBounds &Bounds) const {
	if (Native.Provider == SpatialCollisionProvider::BlockGrid)
		return GetTransferGridBinding(Connection,Native,Bounds);
	const auto Source = TransferIdentity(std::dynamic_pointer_cast<Instance>(World.shared_from_this()));
	if (!Source.IsValid()) return {};
	if (Native.Provider == SpatialCollisionProvider::Terrain) {
		const auto Owner = World.GetOwnedTerrain();
		if (!Owner || Owner->GetObjectId()!=Native.Owner) return {};
		const TerrainSessionWindow *Window{};
		const TerrainWindowIdentity *Identity{};
		const auto Server = Configuration.Role==GameSessionRole::Server;
		if (&World==Runtime->Workspace.get() && (!Configuration.TerrainBootstrap || UsesBootstrapTerrain(Value))) {
			if (!Configuration.TerrainBootstrap) {
				if (!Native.AuthoritativeEmpty || !Native.Installed.empty()) return {};
				static constexpr std::array<std::uint8_t,8> Tag{'E','m','p','t','y','W','S',1};
				static const auto Empty = AssetContentId::Hash(Tag);
				return SpatialAppliedBinding{{Source,Native.Provider},Empty,0,1,true,true};
			}
			if (!Value.TerrainWindow || !Value.TerrainAdmitted || Value.TerrainPending ||
				!HasTerrainMotionRelease(Value) || (Server && !IsCurrentTerrainWindow(Owner->GetField(),Value))) return {};
			Window=&*Value.TerrainWindow;
			Identity=&Value.TerrainIdentity;
		} else {
			const auto &Lane=Value.NamedTerrain;
			if (!Lane || Lane->Region!=Source.ToObjectId() || Lane->Pending || !Lane->Window ||
				(Server ? (Lane->Acknowledged.FieldRevision!=Owner->GetField().GetRevision() &&
					Lane->Versions!=GetTerrainVersions(Owner->GetField(),Lane->Window->Center)) :
					(Lane->InstalledOwner.lock().get()!=&World || Lane->InstalledLifetime!=Native.Lifetime))) return {};
			Window=&*Lane->Window;
			Identity=&Lane->Acknowledged;
		}
		const auto Center=glm::vec3((glm::dvec3(Bounds.Minimum)+glm::dvec3(Bounds.Maximum))*.5);
		const auto Half=glm::vec3((glm::dvec3(Bounds.Maximum)-glm::dvec3(Bounds.Minimum))*.5);
		if (!Identity->Serial || Identity->SessionEpoch!=Value.SessionEpoch || Window->ManifestDigest!=Identity->ManifestDigest ||
			!ContainsTerrainBounds(Window->Grid,Window->Center,Center,Half)) return {};
		return SpatialAppliedBinding{{Source,Native.Provider},Identity->ManifestDigest,Identity->FieldRevision,
			Identity->Serial,true,Native.Installed.empty()};
	}
	const auto Part=std::dynamic_pointer_cast<BasePart>(ObjectRegistry::Get().Lookup(Native.Owner));
	const auto Wire=TransferIdentity(Part);
	if (!Part || !Wire.IsValid() || Native.Installed.size()!=1 || !Native.Resource.empty() ||
		Native.Shape>PhysicsShapeKind::CornerWedge || WorldRoot::GetEffectiveWorld(*Part).get()!=&World ||
		(Configuration.Role==GameSessionRole::Server && (!Replication || !Replication->CanDeliverSpatialContent(Connection,Native.Owner)))) return {};
	static constexpr std::array<std::uint8_t,8> Profile{'R','i','g','i','d','M','v',1};
	static const auto Pin=AssetContentId::Hash(Profile);
	return SpatialAppliedBinding{{Wire,Native.Provider},Pin,1,1,true,false};
}

std::optional<AssetContentId> MotionDigest(ConnectionId Connection,const Peer &Value,WorldRoot &World,
	const BasePart &Subject,const SpatialDestinationBounds &Bounds) const {
	if (!ValidSpatialArea(Bounds)) return {};
	auto Receipt=World.PrepareSpatialDestination(Bounds,{});
	if (!Receipt) return {};
	std::erase_if(Receipt->Providers,[&](const SpatialProviderReceipt &Provider) {
		return Provider.Owner==Subject.GetObjectId() && Provider.Provider==SpatialCollisionProvider::Rigid;
	});
	const SpatialTransferAdapters Adapters{
		.Project=[&](const SpatialProviderReceipt &Native,const SpatialDestinationBounds &Area) {
			return ProjectMotionProvider(Connection,Value,World,Native,Area);
		},
		.Resolve=[&](SpatialWireProvider Key) { return ResolveTransferOwner(Key); }
	};
	return GetSpatialCollisionDigest(*Receipt,Adapters);
}

static bool EnclosesMotion(const SpatialDestinationBounds &Bounds,glm::vec3 From,glm::vec3 To,glm::vec3 Half) {
	return glm::all(glm::greaterThanEqual(Half,glm::vec3(0))) &&
		glm::all(glm::greaterThanEqual(glm::min(From,To)-Half,Bounds.Minimum)) &&
		glm::all(glm::lessThanEqual(glm::max(From,To)+Half,Bounds.Maximum));
}

bool CheckMotionWindow(ConnectionId Connection,const Peer &Value,WorldRoot &World,const BasePart &Subject,
	const SpatialMotionWindow &Frame) const {
	const auto PlayerValue=TransferPlayer(Connection,Value);
	const auto Controlled=PlayerValue?GetSpatialTransferSubject(*PlayerValue):nullptr;
	if (!PlayerValue || HasTransfer(Value) || Frame.Epoch!=Value.SessionEpoch || Frame.Grant!=Value.CurrentGrantGeneration ||
		Controlled.get()!=&Subject || TransferIdentity(Controlled).ToObjectId()!=Frame.Subject ||
		TransferIdentity(WorldRoot::GetEffectiveWorld(Subject)).ToObjectId()!=Frame.Domain ||
		(&World!=Runtime->Workspace.get() ? Frame.Domain : ObjectId{})!=Value.AuthorizedSpatialRegion ||
		(Configuration.Role==GameSessionRole::Server && Frame.Possession!=PlayerValue->GetPossessionRevision())) return false;
	const auto Current=MotionDigest(Connection,Value,World,Subject,Frame.Bounds);
	return Current && *Current==Frame.Digest;
}

bool AdmitSpatialMotion(WorldRoot &World,ObjectId Actor,glm::vec3 From,glm::vec3 To,glm::vec3 Extents) {
	if (Configuration.Role!=GameSessionRole::Server || !Runtime || World.GetDataModel()!=Runtime->DataModel) return false;
	for (auto &[Connection,Value]:Peers) {
		const auto PlayerValue=Value.PlayerValue;
		const auto Subject=PlayerValue?GetSpatialTransferSubject(*PlayerValue):nullptr;
		const auto Controller=PlayerValue?LegacyCharacter(PlayerValue):nullptr;
		if ((!Subject || Subject->GetObjectId()!=Actor) && (!Controller || Controller->GetObjectId()!=Actor)) continue;
		if (!Subject || WorldRoot::GetEffectiveWorld(*Subject).get()!=&World || !Value.MotionWindow || !Value.MotionWindow->Installed ||
			!EnclosesMotion(Value.MotionWindow->Bounds,From,To,Extents)) return false;
		if (CheckMotionWindow(Connection,Value,World,*Subject,*Value.MotionWindow)) return true;
		Value.MotionWindow.reset();
		Value.NextMotionCheckTick=0;
		return false;
	}
	return true; // A server-simulated NPC has no controlling peer.
}

void AttachSpatialMotion() {
	SpatialMotionAdmission=std::make_shared<const WorldRoot::SpatialMotionGuard>(
		[this](WorldRoot &World,ObjectId Actor,glm::vec3 From,glm::vec3 To,glm::vec3 Extents) {
			return AdmitSpatialMotion(World,Actor,From,To,Extents);
		});
	Runtime->Workspace->SpatialMotionAdmission=SpatialMotionAdmission;
	for (const auto &Region:Runtime->DataModel->GetSpatialRegions()) Region->SpatialMotionAdmission=SpatialMotionAdmission;
}

void PumpSpatialMotion(ConnectionId Connection,Peer &Value) {
	if (Value.Phase!=PeerPhase::Ready || PendingPeerFailures.contains(Connection)) return;
	const bool Server=Configuration.Role==GameSessionRole::Server;
	if (HasTransfer(Value) || (Server && Value.Staging)) { Value.MotionWindow.reset(); return; }
	const auto PlayerValue=TransferPlayer(Connection,Value);
	const auto Subject=PlayerValue?GetSpatialTransferSubject(*PlayerValue):nullptr;
	const auto Domain=Subject?WorldRoot::GetEffectiveWorld(*Subject):nullptr;
	if (!Subject || !Domain) return;
	const auto Wire=TransferIdentity(Subject).ToObjectId(), WorldId=TransferIdentity(Domain).ToObjectId();
	if (!Wire.IsValid() || !WorldId.IsValid() ||
		(Domain==Runtime->Workspace ? ObjectId{} : WorldId)!=Value.AuthorizedSpatialRegion) return;
	if (!Server) {
		if (!Value.MotionWindow || Value.MotionWindow->Installed) return;
		auto &Frame=*Value.MotionWindow;
		if (!CheckMotionWindow(Connection,Value,*Domain,*Subject,Frame)) return;
		auto Reply=Frame;
		Reply.Installed=true;
		if (QueueTerrainFrame(Connection,Value,EncodeMotionWindow(Reply))) Frame.Installed=true;
		return;
	}
	if (CurrentTick < Value.NextMotionCheckTick) return;
	Value.NextMotionCheckTick=CurrentTick+6;
	const auto Actor=TerrainFocusActor(PlayerValue);
	const auto Geometry=GetTerrainFocusBounds(Actor);
	const auto Cell=glm::floor(Geometry.Position/8.f)*8.f;
	const auto Pad=Geometry.Extents+glm::vec3(4);
	const SpatialDestinationBounds Area{Cell-Pad,Cell+glm::vec3(8)+Pad};
	if (!ValidSpatialArea(Area)) { Value.MotionWindow.reset(); return; }
	const auto Possession=PlayerValue->GetPossessionRevision();
	if (Value.MotionWindow && Value.MotionWindow->Installed && Value.MotionWindow->Bounds==Area &&
		Value.MotionWindow->Subject==Wire && Value.MotionWindow->Domain==WorldId &&
		Value.MotionWindow->Possession==Possession && Value.MotionWindow->Grant==Value.CurrentGrantGeneration) return;
	const auto Digest=MotionDigest(Connection,Value,*Domain,*Subject,Area);
	if (!Digest) { Value.MotionWindow.reset(); return; }
	if (Value.MotionWindow && Value.MotionWindow->Digest==*Digest && Value.MotionWindow->Bounds==Area &&
		Value.MotionWindow->Subject==Wire && Value.MotionWindow->Domain==WorldId &&
		Value.MotionWindow->Possession==Possession && Value.MotionWindow->Grant==Value.CurrentGrantGeneration) return;
	if (Value.LastMotionSerial==std::numeric_limits<std::uint64_t>::max())
		throw std::runtime_error("[Spatial:Motion] Sequence exhausted");
	const SpatialMotionWindow Frame{false,Value.SessionEpoch,Value.CurrentGrantGeneration,Value.LastMotionSerial+1,
		Possession,WorldId,Wire,Area,*Digest};
	Value.MotionWindow.reset();
	if (QueueTerrainFrame(Connection,Value,EncodeMotionWindow(Frame))) {
		Value.LastMotionSerial=Frame.Serial;
		Value.MotionWindow=Frame;
	}
}

void ReceiveMotionWindow(const ReceivedMessageEvent &Message,Peer &Value) {
	const auto Frame=DecodeMotionWindow(Message.Payload);
	const bool Server=Configuration.Role==GameSessionRole::Server;
	if (!Value.Grids || Value.Phase!=PeerPhase::Ready || Message.Delivery!=DeliveryMode::ReliableOrdered ||
		Message.Traffic!=TrafficClass::Control || !std::holds_alternative<std::monostate>(Message.Order) ||
		!Frame || Frame->Installed!=Server || Frame->Epoch!=Value.SessionEpoch) {
		Reject(Message.Connection,DisconnectReason::ProtocolViolation,"Invalid motion coverage control");
		return;
	}
	if (Server) {
		if (Frame->Serial>Value.LastMotionSerial) {
			Reject(Message.Connection,DisconnectReason::ProtocolViolation,"Unknown motion coverage receipt");
			return;
		}
		if (!Value.MotionWindow || Frame->Serial!=Value.MotionWindow->Serial || HasTransfer(Value) || Value.Staging) return;
		auto Expected=*Value.MotionWindow;
		Expected.Installed=true;
		if (*Frame!=Expected) { Reject(Message.Connection,DisconnectReason::ProtocolViolation,"Conflicting motion coverage"); return; }
		Value.MotionWindow=Expected;
	} else {
		if (Frame->Serial<=Value.LastMotionSerial || Frame->Grant!=Value.CurrentGrantGeneration) {
			Reject(Message.Connection,DisconnectReason::ProtocolViolation,"Stale motion coverage offer");
			return;
		}
		Value.LastMotionSerial=Frame->Serial;
		Value.MotionWindow=Frame;
	}
}
