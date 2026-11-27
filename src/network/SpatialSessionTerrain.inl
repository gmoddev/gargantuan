// Main-thread GameSession ownership. Encoding and contour preparation only borrow immutable snapshots.
bool UsesBootstrapTerrain(const Peer &Value) const {
	return Configuration.TerrainBootstrap && (!Configuration.SpatialContent ||
		(!Value.AuthorizedSpatialRegion.IsValid() && Value.CurrentGrantGeneration==0));
}

ObjectId TerrainStreamOwner(ObjectId Region,std::uint64_t Grant) const {
	if (Region.IsValid() || !Grant || !Configuration.TerrainBootstrap || !Runtime) return Region;
	return TransferIdentity(Runtime->Workspace).ToObjectId();
}

void RetireBootstrapTerrain(Peer &Value) {
	if (!UsesBootstrapTerrain(Value)) return;
	Value.RetiredBootstrapSerial=Value.TerrainIdentity.Serial;
	if (Value.TerrainPending) Value.RetiredBootstrapSerial=std::max(Value.RetiredBootstrapSerial,Value.TerrainPending->Offer.Identity.Serial);
	if (Value.TerrainPending && Value.TerrainPending->Sending) Value.TerrainPending->Sending->Stop.request_stop();
	if (Configuration.Role==GameSessionRole::Client) {
		if (ClientTerrainWork) ClientTerrainWork->Stop.request_stop();
		ClientTerrainWork.reset();
		TerrainReceiveBuffer.clear();
		ReceivedTerrain.reset();
		const auto World=Runtime->Workspace;
		World->TerrainMotionAdmission.reset();
		NamedTerrainPublication=std::max(NamedTerrainPublication,Value.TerrainIdentity.Serial);
		if (Value.TerrainPending) NamedTerrainPublication=std::max(NamedTerrainPublication,Value.TerrainPending->Offer.Identity.Serial);
		auto Empty=std::make_unique<terrain::Field>(Configuration.TerrainBootstrap->Directory.Grid);
		if (!World->InstallTerrainReplica(std::move(Empty),{},NextDomainTerrainPublication()))
			throw std::runtime_error("[Terrain:Domain] Workspace collision retirement failed");
	}
	Value.TerrainWindow.reset();
	Value.TerrainPending.reset();
	Value.ReleasedTerrain.reset();
	Value.TerrainReleaseStartedTick.reset();
	Value.TerrainAdmitted=false;
}

std::shared_ptr<DomainTerrainDelivery> MakeDomainTerrain(ObjectId Region, std::uint64_t Grant) const {
	auto State = std::make_shared<DomainTerrainDelivery>();
	State->Region = Region;
	State->Grant = Grant;
	return State;
}

bool QueueDomainTerrain(ConnectionId Connection, const Peer &Value, const DomainTerrainDelivery &State,
	const std::vector<std::byte> &Payload) {
	return !Payload.empty() && QueueTerrainFrame(Connection, Value,
		EncodeDomainTerrain({State.Region, State.Grant, State.Grid, Payload}));
}

bool BeginDomainTerrain(DomainTerrainDelivery &State, const terrain::Field &Field,
	terrain::ChunkKey Center, std::uint64_t Epoch) {
	const auto Slot = FindTerrainSendSlot();
	if (Slot == TerrainSendSlots.end()) return false;
	if (State.Acknowledged.Serial == std::numeric_limits<std::uint64_t>::max())
		throw std::runtime_error("[Terrain:Domain] Window sequence exhausted");
	auto Snapshot = Field.Capture(GetTerrainSessionKeys(Center));
	if (!Snapshot) throw std::runtime_error("[Terrain:Domain] Unable to capture terrain window");
	TerrainTransfer Transfer{
		.Offer = {{Epoch, State.Acknowledged.Serial+1, Field.GetRevision()}, Center},
		.Sending = std::make_shared<TerrainSendWork>(), .Versions = GetTerrainVersions(Field,Center), .StartedTick = TerrainTick};
	const auto Project = DomainTerrainProject(State.Region,Epoch);
	const auto Work = Transfer.Sending;
	TerrainJobs.Submit([Work, Samples=std::move(*Snapshot), Project, Center] {
		Work->Window = CaptureTerrainSessionWindow(Samples, Project, Center, Work->Stop.get_token());
	}, Work->Completion);
	*Slot = Work;
	State.Grid = Field.GetGrid();
	State.Pending = std::move(Transfer);
	return true;
}

void SendDomainTerrain(ConnectionId Connection, Peer &Value, DomainTerrainDelivery &State,
	const WorldRoot &Domain, const SpatialDestinationBounds *Bounds) {
	const auto Owner = Domain.GetOwnedTerrain();
	if (!Owner || !Replication || !Replication->CanDeliverSpatialContent(Connection,State.Region)) return;
	const auto &Field = Owner->GetField();
	const auto Actor = TerrainFocusActor(Value.PlayerValue);
	const auto Focus = Relevance->GetResolvedFocus(Connection);
	const auto Position = Bounds ? glm::vec3((glm::dvec3(Bounds->Minimum)+glm::dvec3(Bounds->Maximum))*.5) :
		(!Actor && !Focus.empty() ? Focus.front() : GetTerrainFocusBounds(Actor).Position);
	const auto Center = GetTerrainSessionCenter(Field.GetGrid(), {Position.x,Position.y,Position.z});
	if (!Center) return;
	if (!State.Pending) {
		if (State.Window && State.Window->Center==*Center && State.Grid==Field.GetGrid() &&
			(State.Acknowledged.FieldRevision==Field.GetRevision() || State.Versions==GetTerrainVersions(Field,*Center))) return;
		(void)BeginDomainTerrain(State,Field,*Center,Value.SessionEpoch);
		return;
	}
	auto &Pending = *State.Pending;
	if (Pending.Sending) {
		auto &Work = Pending.Sending;
		if (!Work->Completion->IsComplete()) return;
		if (Work->Completion->GetFirstException() || !Work->Window)
			throw std::runtime_error("[Terrain:Domain] Encoding the selected terrain window failed");
		Pending.Window = std::move(Work->Window);
		Work.reset();
		auto &Window = *Pending.Window;
		if (State.Window) ReuseTerrainSessionLeaves(Window,*State.Window);
		Pending.Offer.ManifestByteCount = static_cast<std::uint32_t>(Window.ManifestBytes.size());
		Pending.Offer.LeafByteCount = static_cast<std::uint32_t>(Window.Size);
		Pending.Offer.Identity.ManifestDigest = Window.ManifestDigest;
	}
	if (Scheduler.HasQueuedControl(Connection)) return;
	if (!Pending.Offered) {
		if (Pending.Offer.Center!=*Center || Pending.Versions!=GetTerrainVersions(Field,*Center)) {
			State.Pending.reset();
			return;
		}
		Pending.Offered = QueueDomainTerrain(Connection,Value,State,EncodeTerrainWindowOffer(Pending.Offer));
		return;
	}
	if (Pending.Cursor == Pending.Offer.GetTransferBytes()) return;
	const auto ManifestSize = Pending.Offer.ManifestByteCount;
	const auto Slice = Pending.Cursor < ManifestSize ?
		std::as_bytes(std::span(Pending.Window->ManifestBytes)).subspan(Pending.Cursor) :
		GetTerrainSessionFragment(*Pending.Window,Pending.Cursor-ManifestSize);
	const auto Overhead = static_cast<std::size_t>(ReliableServiceEnvelopeBytes) + DomainTerrainHeaderBytes + TerrainFragmentHeaderBytes;
	if (Slice.empty() || Value.Limits.MaximumQueuedReliableBytes/2 <= Overhead)
		throw std::runtime_error("[Terrain:Domain] Invalid terrain fragment capacity");
	const auto Count = std::min({Slice.size(), MaximumTerrainFragmentBytes, Value.Limits.MaximumQueuedReliableBytes/2-Overhead});
	auto Frame = EncodeTerrainFragment({Value.SessionEpoch,Pending.Offer.Identity.Serial,static_cast<std::uint32_t>(Pending.Cursor),Slice.first(Count)});
	if (QueueDomainTerrain(Connection,Value,State,Frame)) Pending.Cursor += Count;
}

void PrepareDomainTerrain(DomainTerrainDelivery &State) {
	for (auto &Slot : NamedTerrainDecodeSlots) if (Slot && Slot->Completion->IsComplete()) Slot.reset();
	const auto Slot = std::ranges::find(NamedTerrainDecodeSlots,nullptr);
	if (Slot == NamedTerrainDecodeSlots.end()) return;
	const auto &Offer = State.Pending->Offer;
	const auto Profile = MakeTerrainReceiver(DomainTerrainProject(State.Region,Offer.Identity.SessionEpoch),1,
		Offer.Identity.ManifestDigest,State.Grid);
	auto Work = std::make_shared<TerrainReceiveWork>();
	Work->Window = ReadTerrainSessionWindow(*Profile,Offer.Center,Offer.Identity.ManifestDigest,
		std::span(State.Bytes).first(Offer.ManifestByteCount));
	if (State.Window) ReuseTerrainSessionLeaves(Work->Window,*State.Window);
	if (Work->Window.Size != Offer.LeafByteCount)
		throw std::invalid_argument("[Terrain:Domain] Leaf closure differs from offered byte count");
	TerrainJobs.Submit([Work, Bytes=std::exchange(State.Bytes,{}), ManifestSize=Offer.ManifestByteCount] {
		Work->Field = LoadTerrainSessionField(Work->Window,std::span(Bytes).subspan(ManifestSize));
		auto Prepared = terrain::PrepareTerrainReplica(*Work->Field,Work->Stop.get_token());
		if (!Prepared) throw std::runtime_error("[Terrain:Domain] Native terrain preparation failed");
		Work->Chunks = std::move(*Prepared);
	}, Work->Completion);
	*Slot = Work;
	State.Receiving = std::move(Work);
}

void BindDomainCoverage(const std::shared_ptr<DomainTerrainDelivery> &State, const std::shared_ptr<WorldRoot> &Domain) {
	if (State->InstalledOwner.lock()==Domain && State->Coverage) return;
	State->Coverage = std::make_shared<const WorldRoot::TerrainMotionGuard>(
		[Weak=std::weak_ptr(State)](ObjectId Object, glm::vec3 Position, glm::vec3 Extents) {
			const auto Current = Weak.lock();
			if (!Current || Object.IsValid()) return false;
			const auto Owner = Current->InstalledOwner.lock();
			const auto Terrain = Owner ? Owner->GetOwnedTerrain() : nullptr;
			if (!Terrain || Terrain->GetField().GetLifetime()!=Current->InstalledLifetime) return false;
			const auto *Window = Current->Receiving && Current->Receiving->Installed ? &Current->Receiving->Window :
				(Current->Window ? &*Current->Window : nullptr);
			return Window && ContainsTerrainBounds(Window->Grid,Window->Center,Position,Extents);
		});
	State->InstalledOwner = Domain;
	Domain->TerrainMotionAdmission = State->Coverage;
}

std::uint64_t NextDomainTerrainPublication() {
	if (NamedTerrainPublication==std::numeric_limits<std::uint64_t>::max())
		throw std::overflow_error("[Terrain:Domain] Native publication exhausted");
	return ++NamedTerrainPublication;
}

void PumpDomainTerrain(ConnectionId Connection, Peer &Value, std::shared_ptr<DomainTerrainDelivery> &State,
	ObjectId Region, std::uint64_t Grant, const SpatialDestinationBounds *Bounds) {
	Region=TerrainStreamOwner(Region,Grant);
	if (!Region.IsValid()) return;
	if (!State) State = MakeDomainTerrain(Region,Grant);
	if (State->Pending && TerrainTick-State->Pending->StartedTick >= Configuration.HandshakeTimeoutTicks)
		throw std::runtime_error("[Terrain:Domain] Terrain receipt timed out");
	const auto Domain = TransferWorld(Region);
	if (!Domain) return;
	if (Configuration.Role==GameSessionRole::Server) {
		SendDomainTerrain(Connection,Value,*State,*Domain,Bounds);
		return;
	}
	BindDomainCoverage(State,Domain);
	if (!State->Pending) return;
	auto &Pending = *State->Pending;
	if (!State->Receiving && State->Bytes.size()==Pending.Offer.GetTransferBytes()) PrepareDomainTerrain(*State);
	auto &Work = State->Receiving;
	if (!Work || !Work->Completion->IsComplete()) return;
	if (Work->Completion->GetFirstException()) throw std::runtime_error("[Terrain:Domain] Invalid terrain closure");
	if (!Work->Installed) {
		if (!Domain->InstallTerrainReplica(std::move(Work->Field),std::move(Work->Chunks),NextDomainTerrainPublication(),true))
			throw std::runtime_error("[Terrain:Domain] Native collision cutover failed");
		State->InstalledLifetime = Domain->GetOwnedTerrain()->GetField().GetLifetime();
		Work->Installed = true;
	}
	if (QueueDomainTerrain(Connection,Value,*State,EncodeTerrainWindowReady(Pending.Offer.Identity))) {
		State->Acknowledged = Pending.Offer.Identity;
		State->Window = std::move(Work->Window);
		State->Pending.reset();
		Work.reset();
	}
}

void RetireDomainTerrain(std::shared_ptr<DomainTerrainDelivery> &State) {
	if (!State) return;
	const auto Owner = State->InstalledOwner.lock();
	if (Owner && !Owner->GetDestroyed() && !Owner->IsDestroying() && Owner->TerrainMotionAdmission.lock()==State->Coverage) {
		Owner->TerrainMotionAdmission.reset();
		if (State->InstalledLifetime && Owner->GetOwnedTerrain() && Owner->IsTerrainCollisionReady()) {
			auto Empty = std::make_unique<terrain::Field>(State->Grid);
			(void)Owner->InstallTerrainReplica(std::move(Empty),{},NextDomainTerrainPublication());
		}
	}
	State.reset();
}

void ReceiveDomainTerrain(const ReceivedMessageEvent &Message, Peer &Value) {
	try {
		if (!Value.Grids || Value.Phase!=PeerPhase::Ready || Message.Delivery!=DeliveryMode::ReliableOrdered ||
			Message.Traffic!=TrafficClass::Control || !std::holds_alternative<std::monostate>(Message.Order))
			throw std::invalid_argument("[Terrain:Domain] Unexpected terrain control");
		const auto Frame = DecodeDomainTerrain(Message.Payload);
		if (!Frame) throw std::invalid_argument("[Terrain:Domain] Malformed terrain envelope");
		const bool Staged = Value.Staging && Value.Staging->Frame.Serial==Frame->Grant;
		if (!Staged && Value.CurrentGrantGeneration!=Frame->Grant) {
			if (Frame->Grant<=Value.LastStagingSerial) return;
			throw std::invalid_argument("[Terrain:Domain] Unknown grant");
		}
		const auto GrantRegion = Staged ? Value.Staging->Grant->Region : Value.AuthorizedSpatialRegion;
		const auto Region = TerrainStreamOwner(GrantRegion,Frame->Grant);
		if (!Region.IsValid() || Region!=Frame->Region || (!GrantRegion.IsValid() &&
			(!Configuration.TerrainBootstrap || Frame->Grid!=Configuration.TerrainBootstrap->Directory.Grid)))
			throw std::invalid_argument("[Terrain:Domain] Wrong terrain owner");
		auto &Lane = Staged ? Value.Staging->Terrain : Value.NamedTerrain;
		if (!Lane) Lane = MakeDomainTerrain(Region,Frame->Grant);
		const auto Payload = Frame->Payload;
		if (Configuration.Role==GameSessionRole::Server) {
			const auto Ready = DecodeTerrainWindowReady(Payload);
			if (!Ready || !Lane->Pending || !Lane->Pending->Offered || *Ready!=Lane->Pending->Offer.Identity ||
				Lane->Grid!=Frame->Grid || Lane->Pending->Cursor!=Lane->Pending->Offer.GetTransferBytes())
				throw std::invalid_argument("[Terrain:Domain] Invalid terrain acknowledgment");
			Lane->Acknowledged = *Ready;
			Lane->Window = std::move(Lane->Pending->Window);
			Lane->Versions = Lane->Pending->Versions;
			for (auto &Leaf:Lane->Window->Artifacts) Leaf.Source.reset();
			Lane->Window->ManifestBytes.clear();
			Lane->Pending.reset();
			return;
		}
		if (const auto Offer = DecodeTerrainWindowOffer(Payload)) {
			if (Lane->Pending || Offer->Identity.SessionEpoch!=Value.SessionEpoch ||
				Lane->Acknowledged.Serial==std::numeric_limits<std::uint64_t>::max() || Offer->Identity.Serial!=Lane->Acknowledged.Serial+1 ||
				Offer->Identity.FieldRevision<Lane->Acknowledged.FieldRevision || !Offer->ManifestByteCount ||
				(Lane->Window && Lane->Grid!=Frame->Grid))
				throw std::invalid_argument("[Terrain:Domain] Invalid terrain offer");
			Lane->Grid = Frame->Grid;
			Lane->Pending.emplace(TerrainTransfer{.Offer=*Offer,.StartedTick=TerrainTick});
			Lane->Bytes.clear();
			return;
		}
		const auto Fragment = DecodeTerrainFragment(Payload);
		if (!Fragment || !Lane->Pending || Lane->Receiving || Frame->Grid!=Lane->Grid ||
			Fragment->SessionEpoch!=Value.SessionEpoch || Fragment->Serial!=Lane->Pending->Offer.Identity.Serial ||
			Fragment->Offset!=Lane->Bytes.size() || Lane->Bytes.size()>Lane->Pending->Offer.GetTransferBytes() ||
			Fragment->Bytes.size()>Lane->Pending->Offer.GetTransferBytes()-Lane->Bytes.size())
			throw std::invalid_argument("[Terrain:Domain] Unexpected terrain fragment");
		if (Lane->Bytes.empty()) Lane->Bytes.reserve(Lane->Pending->Offer.GetTransferBytes());
		const auto *Start = reinterpret_cast<const std::uint8_t*>(Fragment->Bytes.data());
		Lane->Bytes.insert(Lane->Bytes.end(),Start,Start+Fragment->Bytes.size());
	} catch (const std::exception &Error) {
		Reject(Message.Connection,DisconnectReason::ProtocolViolation,Error.what());
	}
}

std::optional<SpatialAppliedBinding> GetTransferTerrainBinding(ConnectionId Connection,
	const SpatialProviderReceipt &Native, const SpatialDestinationBounds &Bounds) const {
	const auto It = Peers.find(Connection);
	if (Native.Provider!=SpatialCollisionProvider::Terrain || It==Peers.end() || !It->second.Staging) return {};
	const auto &Stage = *It->second.Staging;
	const auto &Lane = Stage.Terrain;
	const auto Destination = WireObjectId::FromObjectId(Stage.Frame.Destination);
	if (!Lane || Lane->Pending || !Lane->Window || !Lane->Acknowledged.ManifestDigest.IsValid() ||
		!HasTransferDomain(Connection,Destination,Bounds,Stage.Frame.Serial)) return {};
	const auto Domain = TransferWorld(Lane->Region);
	const auto Owner = Domain ? Domain->GetOwnedTerrain() : nullptr;
	if (!Owner || Owner->GetObjectId()!=Native.Owner || !Domain->IsTerrainCollisionReady()) return {};
	const auto Center = glm::vec3((glm::dvec3(Bounds.Maximum)+glm::dvec3(Bounds.Minimum))*.5);
	const auto Extents = glm::vec3((glm::dvec3(Bounds.Maximum)-glm::dvec3(Bounds.Minimum))*.5);
	if (!ContainsTerrainBounds(Lane->Grid,Lane->Window->Center,Center,Extents)) return {};
	const auto &Field = Owner->GetField();
	if (Configuration.Role==GameSessionRole::Server) {
		if (Field.GetRevision()!=Lane->Acknowledged.FieldRevision && GetTerrainVersions(Field,Lane->Window->Center)!=Lane->Versions) return {};
	} else if (Lane->InstalledOwner.lock()!=Domain || Lane->InstalledLifetime!=Field.GetLifetime()) return {};
	return SpatialAppliedBinding{{Destination,SpatialCollisionProvider::Terrain},Lane->Acknowledged.ManifestDigest,
		Lane->Acknowledged.FieldRevision,Lane->Acknowledged.Serial,true,Native.Installed.empty()};
}
