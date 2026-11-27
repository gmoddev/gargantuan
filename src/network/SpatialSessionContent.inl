// Private Main-thread GameSession methods. Only trusted server movement can open an additional domain.
std::shared_ptr<WorldRoot> TransferWorld(ObjectId SourceId) const {
	if (!Runtime || !SourceId.IsValid()) return {};
	const auto Object = Configuration.Role == GameSessionRole::Server ? ObjectRegistry::Get().Lookup(SourceId) : Replica.Resolve(SourceId);
	const auto World = std::dynamic_pointer_cast<WorldRoot>(Object);
	if (!World || World->GetDestroyed() || World->IsDestroying() ||
		SpatialRegion::FindAttachedWorld(*World) != Runtime->DataModel ||
		(World != Runtime->Workspace && !std::dynamic_pointer_cast<SpatialRegion>(World))) return {};
	return World;
}

bool HasTransferDomain(ConnectionId Connection, WireObjectId Destination, const SpatialDestinationBounds &Bounds, std::uint64_t Serial) const {
	const auto It=Peers.find(Connection);
	return It!=Peers.end() && It->second.Phase==PeerPhase::Ready && !PendingPeerFailures.contains(Connection) &&
		It->second.Staging && It->second.Staging->Frame.Destination==Destination.ToObjectId() &&
		It->second.Staging->Frame.Serial==Serial && It->second.Staging->Frame.Bounds==Bounds &&
		SpatialTransferClock::now()<It->second.Staging->Expires &&
		(Configuration.Role==GameSessionRole::Client || bool(TransferWorld(Destination.ToObjectId())));
}

std::optional<AssetContentId> GetTransferEmptyTerrainBinding(ConnectionId Connection, WireObjectId Destination,
	const SpatialDestinationBounds &Bounds, std::uint64_t Serial) const {
	if (!HasTransferDomain(Connection,Destination,Bounds,Serial)) return {};
	const auto &Stage=*Peers.at(Connection).Staging;
	const auto World=TransferWorld(Destination.ToObjectId());
	const auto Terrain=World?World->GetOwnedTerrain():nullptr;
	if (!Stage.Frame.EmptyTerrain || !Stage.EmptyTerrainPin.IsValid() || !Terrain ||
		Terrain->GetField().GetStats().MaterializedPages!=0) return {};
	return Stage.EmptyTerrainPin;
}

std::unique_ptr<Peer::TransferDomain> MakeTransferDomain(const SpatialAudienceFrame &Frame) const {
	const auto WorkspaceId = Configuration.Role==GameSessionRole::Server ? std::optional(Runtime->Workspace->GetObjectId()) :
		Replica.GetSourceObject(*Runtime->Workspace);
	if (!WorkspaceId) return {};
	auto Result=std::make_unique<Peer::TransferDomain>();
	Result->Frame=Frame;
	Result->Expires=SpatialTransferClock::now()+std::chrono::seconds(15);
	const auto Region=Frame.Destination==*WorkspaceId ? ObjectId{} : Frame.Destination;
	Result->Grant=std::make_shared<const SpatialStagingGrant>(SpatialStagingGrant{Region,Frame.Serial,Frame.Objects});
	Result->Grids=std::make_unique<BlockGridPeer>(Configuration.Role==GameSessionRole::Server,Frame.Epoch,Region,Configuration.SharedGridAssets,Frame.Serial);
	if (Frame.EmptyTerrain) {
		const auto Bytes=EncodeSpatialAudience(Frame);
		Result->EmptyTerrainPin=AssetContentId::Hash({reinterpret_cast<const std::uint8_t*>(Bytes.data()),Bytes.size()});
	}
	return Result;
}

bool SetStructuralAudience(ConnectionId Connection, ObjectId Region, const std::shared_ptr<const SpatialStagingGrant> &Grant) {
	if (!Relevance || !Replication) return false;
	const auto Previous=Relevance->GetSelectionSnapshot(Connection);
	if (!Previous || !Relevance->SetSpatialAudience(Connection,Region,Grant)) return false;
	if (Replication->ApplySpatialAudience(Connection,Relevance->GetSelectionSnapshot(Connection),CurrentTick)) return true;
	if (!Relevance->SetSpatialAudience(Connection,Previous->AuthorizedSpatialRegion,Previous->Staging))
		PendingPeerFailures.try_emplace(Connection,DisconnectInfo{DisconnectReason::ResourceExhaustion,"Spatial selection restoration failed"});
	return false;
}

bool StageTransferDomain(ConnectionId Connection, WireObjectId Destination, const SpatialDestinationBounds &Bounds, std::uint64_t Serial) {
	const auto It=Peers.find(Connection);
	if (!Configuration.SpatialContent || Configuration.Role!=GameSessionRole::Server || It==Peers.end() ||
		It->second.Phase!=PeerPhase::Ready || !It->second.Grids || PendingPeerFailures.contains(Connection) ||
		!Serial || !ValidSpatialArea(Bounds)) return false;
	auto &Value=It->second;
	if (Value.Staging) return HasTransferDomain(Connection,Destination,Bounds,Serial);
	if (Serial<=Value.LastStagingSerial) return false;
	const auto World=TransferWorld(Destination.ToObjectId());
	if (!World || (World==Runtime->Workspace?ObjectId{}:World->GetObjectId())==Value.AuthorizedSpatialRegion) return false;
	const auto Proof=World->PrepareSpatialDestination(Bounds,{});
	if (!Proof || Proof->Providers.size()>MaximumSpatialTransferProviders) return false;
	std::set<ObjectId> Members;
	auto AddAncestors=[&](std::shared_ptr<Instance> Object) {
		while (Object && Object!=Runtime->DataModel) {
			if (Object->GetDataModel()!=Runtime->DataModel || Object->GetDestroyed() || Object->IsDestroying()) return false;
			if (!Members.insert(Object->GetObjectId()).second) return true;
			if (Members.size()>MaximumSpatialStagingObjects) return false;
			Object=Object->GetParent().value_or(nullptr);
		}
		return bool(Object);
	};
	if (!AddAncestors(World)) return false;
	const auto Actor=TerrainFocusActor(Value.PlayerValue);
	if (!Actor || !AddAncestors(Actor)) return false;
	if (const auto CharacterValue=std::dynamic_pointer_cast<Character>(Actor))
		if (const auto Root=CharacterValue->GetRootPart(); Root && !AddAncestors(*Root)) return false;
	bool EmptyTerrain=false;
	for (const auto &Provider:Proof->Providers) {
		if (Provider.Provider==SpatialCollisionProvider::Terrain) {
			EmptyTerrain=Provider.AuthoritativeEmpty && Provider.Installed.empty();
			continue;
		}
		if (!AddAncestors(ObjectRegistry::Get().Lookup(Provider.Owner))) return false;
	}
	SpatialAudienceFrame Frame{SpatialAudienceAction::Stage,Value.SessionEpoch,Serial,World->GetObjectId(),Bounds,EmptyTerrain,
		{Members.begin(),Members.end()}};
	auto Stage=MakeTransferDomain(Frame);
	if (!Stage || !SetStructuralAudience(Connection,Value.AuthorizedSpatialRegion,Stage->Grant)) return false;
	if (!QueueTerrainFrame(Connection,Value,EncodeSpatialAudience(Frame))) {
		if (!SetStructuralAudience(Connection,Value.AuthorizedSpatialRegion,{}))
			PendingPeerFailures.try_emplace(Connection,DisconnectInfo{DisconnectReason::ResourceExhaustion,"Staging rollback failed"});
		return false;
	}
	Value.LastStagingSerial=Serial;
	Value.Staging=std::move(Stage);
	return true;
}

void FinishTransferDomain(Peer &Value,bool Promote) {
	if (Promote) {
		RetireDomainTerrain(Value.NamedTerrain);
		Value.NamedTerrain=std::move(Value.Staging->Terrain);
		Value.Grids->Retire();
		Value.Grids=std::move(Value.Staging->Grids);
		Value.AuthorizedSpatialRegion=Value.Staging->Grant->Region;
		Value.CurrentGrantGeneration=Value.Staging->Frame.Serial;
	} else {
		RetireDomainTerrain(Value.Staging->Terrain);
		Value.Staging->Grids->Retire();
	}
	Value.Staging.reset();
}
bool ReleaseTransferDomain(ConnectionId Connection,std::uint64_t Serial,bool Promote=false) {
	const auto It=Peers.find(Connection);
	if (Configuration.Role!=GameSessionRole::Server || It==Peers.end() || !It->second.Staging ||
		It->second.Staging->Frame.Serial!=Serial) return false;
	auto &Value=It->second;
	const auto &Stage=*Value.Staging;
	if (Promote && (SpatialTransferClock::now()>=Stage.Expires || !TransferWorld(Stage.Frame.Destination))) return false;
	const auto Next=Promote?Stage.Grant->Region:Value.AuthorizedSpatialRegion;
	if (!SetStructuralAudience(Connection,Next,{})) return false;
	const SpatialAudienceFrame End{Promote?SpatialAudienceAction::Promote:SpatialAudienceAction::Release,
		Value.SessionEpoch,Serial,Stage.Frame.Destination};
	const bool Sent=QueueTerrainFrame(Connection,Value,EncodeSpatialAudience(End));
	FinishTransferDomain(Value,Promote);
	if (!Sent) PendingPeerFailures.try_emplace(Connection,DisconnectInfo{DisconnectReason::ResourceExhaustion,"Audience transition could not be delivered"});
	return Sent;
}

void TrackGrid(ConnectionId Connection,Peer &Value,ObjectId Id) {
	if (Value.Grids && ResolveGrid(Connection,Value,Id,false)) Value.Grids->Track(Id);
	if (Value.Staging && ResolveGrid(Connection,Value,Id,true)) Value.Staging->Grids->Track(Id);
}
void ReceiveSpatialAudience(const ReceivedMessageEvent &Message,Peer &Value) {
	if (!Configuration.SpatialContent || Configuration.Role!=GameSessionRole::Client || !ClientRuntimeAttached ||
		Value.Phase!=PeerPhase::Ready || Message.Delivery!=DeliveryMode::ReliableOrdered || Message.Traffic!=TrafficClass::Control ||
		!std::holds_alternative<std::monostate>(Message.Order)) {
		Reject(Message.Connection,DisconnectReason::ProtocolViolation,"Unexpected spatial audience control");
		return;
	}
	const auto Frame=DecodeSpatialAudience(Message.Payload);
	if (!Frame || Frame->Epoch!=Value.SessionEpoch) {
		Reject(Message.Connection,DisconnectReason::ProtocolViolation,"Malformed spatial audience control");
		return;
	}
	if (Frame->Action==SpatialAudienceAction::Stage) {
		if (Value.Staging || Frame->Serial<=Value.LastStagingSerial) {
			Reject(Message.Connection,DisconnectReason::ProtocolViolation,"Overlapping spatial audience control");
			return;
		}
		auto Stage=MakeTransferDomain(*Frame);
		if (!Stage || Stage->Grant->Region==Value.AuthorizedSpatialRegion) {
			Reject(Message.Connection,DisconnectReason::ProtocolViolation,"Invalid destination audience");
			return;
		}
		Value.LastStagingSerial=Frame->Serial;
		Value.Staging=std::move(Stage);
		for (const auto Id:Frame->Objects) if (ClientKnownObjects.contains(Id)) TrackGrid(Message.Connection,Value,Id);
		return;
	}
	if (!Value.Staging || Value.Staging->Frame.Destination!=Frame->Destination || Value.Staging->Frame.Serial!=Frame->Serial) {
		Reject(Message.Connection,DisconnectReason::ProtocolViolation,"Stale spatial audience disposition");
		return;
	}
	const bool Promote=Frame->Action==SpatialAudienceAction::Promote;
	FinishTransferDomain(Value,Promote);
}

std::optional<SpatialAppliedBinding> GetTransferGridBinding(ConnectionId Connection,
	const SpatialProviderReceipt &Provider,const SpatialDestinationBounds &Bounds) const {
	if (Provider.Provider!=SpatialCollisionProvider::BlockGrid || !ValidSpatialArea(Bounds)) return {};
	const auto It=Peers.find(Connection);
	const auto Grid=std::dynamic_pointer_cast<BlockGrid>(ObjectRegistry::Get().Lookup(Provider.Owner));
	if (!Grid || It==Peers.end() || It->second.Phase!=PeerPhase::Ready || PendingPeerFailures.contains(Connection)) return {};
	const bool Server=Configuration.Role==GameSessionRole::Server;
	const auto Id=Server?std::optional(Provider.Owner):Replica.GetSourceObject(*Grid);
	if (!Id) return {};
	const auto &Value=It->second;
	if (Value.Staging && HasTransferDomain(Connection,WireObjectId::FromObjectId(Value.Staging->Frame.Destination),Bounds,Value.Staging->Frame.Serial) &&
		ResolveGrid(Connection,Value,*Id,true)==Grid)
		return Value.Staging->Grids->GetBinding(*Id,*Grid,Provider.AuthoritativeEmpty && Provider.Installed.empty());
	if (!Value.Grids || ResolveGrid(Connection,Value,*Id,false)!=Grid) return {};
	return Value.Grids->GetBinding(*Id,*Grid,Provider.AuthoritativeEmpty && Provider.Installed.empty());
}
