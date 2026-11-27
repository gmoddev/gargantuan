// Runs inside the session fixture with real authoritative edits, replica shells, providers and both directions of GBGS.
void CheckPairedInstallations() {
	auto World=std::make_shared<DataModel>();
	HeadlessRenderer ServerView(Vector2(64,64)),ClientView(Vector2(64,64));
	Engine Authority(World,&ServerView,nullptr,{.Mode=RuntimeMode::Offline,.AdmitSpatialContent=true});
	const auto Parent=World->GetService("Workspace");
	const auto Source=Seed(Parent,"PairSource",blockgrid::Stone),Target=Seed(Parent,"PairTarget",blockgrid::Brick);
	Target->SetCFrame(CFrame(2,0,0));
	const auto S=Source->GetObjectId(),D=Target->GetObjectId();
	ReplicationCoordinator Coordinator(World);
	const auto Structural=Coordinator.AddPeer({1,1},ReplicationEpoch(1));
	ReplicaApplier Replica;
	Require(Structural.Succeeded() && Replica.ApplyFrame(*Structural.Frame).Succeeded(),"paired structural bootstrap");
	Engine Client(std::dynamic_pointer_cast<DataModel>(Replica.GetReplicaRoot()),&ClientView,nullptr,
		{.Mode=RuntimeMode::Offline,.AdmitSpatialContent=true});
	const auto RS=std::dynamic_pointer_cast<BlockGrid>(Replica.Resolve(S)),RD=std::dynamic_pointer_cast<BlockGrid>(Replica.Resolve(D));
	Require(RS && RD,"missing paired shells");
	const SpatialDestinationBounds Bounds{{-6.f,.1f,2.1f},{-3.9f,1.9f,3.9f}};
	const SpatialDestinationBounds Far{glm::vec3(999),glm::vec3(1000)};
	const SpatialDestinationBounds OnlySourceWindow{{19.1f,.1f,.1f},{19.3f,.3f,.3f}};
	bool BothVisible=true;
	auto ServerFind=[&](ObjectId Id){return Id==S?Source:Id==D && BothVisible?Target:nullptr;};
	auto ClientFind=[&](ObjectId Id){return Id==S?RS:Id==D && BothVisible?RD:nullptr;};
	BlockGridPeer Out(true,89,{}),In(false,89,{});
	In.Track(S);In.Track(D);
	GridInterest Interest;Interest.Count=1;
	std::uint64_t Tick{},Generation{};
	unsigned PairedAcks{};
	bool Redacted{};
	auto Proof=[&](ObjectId Id) {
		const auto ServerGrid=ServerFind(Id),ClientGrid=ClientFind(Id);
		return ServerGrid && ClientGrid && Out.GetBinding(Id,*ServerGrid,false,Bounds) && In.GetBinding(Id,*ClientGrid,false,Bounds);
	};
	auto Exchange=[&] {
		++Tick;
		std::vector<std::vector<std::byte>> Frames;
		auto Send=[&](const GridStreamMessage& M){Frames.push_back(EncodeGridStream(M));return true;};
		In.Pump(ClientFind,Send,Tick);
		for (const auto& Bytes:Frames) {
			const auto M=DecodeGridStream(Bytes);Require(M.has_value(),"paired client wire");
			const bool Ack=BothVisible && Generation && M->Kind==GridStreamKind::Installed && M->Fence.Generation==Generation;
			if (Ack && !PairedAcks) {
				auto Changed=*M;++Changed.Fence.Generation;bool Rejected{};
				try {Out.Receive(Changed,ServerFind,Tick);} catch(const std::invalid_argument&){Rejected=true;}
				Require(Rejected,"forged paired generation accepted");
			}
			Out.Receive(*M,ServerFind,Tick);
			if (Ack && ++PairedAcks==1) {
				Require(!Out.GetBinding(S,*Source,false,Bounds) && !Out.GetBinding(D,*Target,false,Bounds) &&
					!In.GetBinding(S,*RS,false,Bounds) && !In.GetBinding(D,*RD,false,Bounds),"half-installed pair certified movement");
			}
		}
		Frames.clear();
		Out.Pump(ServerFind,Send,Tick,Interest);
		for (const auto& Bytes:Frames) {
			const auto M=DecodeGridStream(Bytes);Require(M.has_value(),"paired server wire");
			if (!BothVisible && M->Kind==GridStreamKind::Offer) {
				Require(M->Fence.Generation==Generation && M->Fence.Other==ObjectId{},"withheld participant identity disclosed");
				Redacted=true;
			}
			In.Receive(*M,ClientFind,Tick);
		}
	};
	for (unsigned I=0;I<40 && !(Proof(S) && Proof(D));++I) Exchange();
	Require(Proof(S) && Proof(D) && !Out.GetBinding(S,*Source,false,Far) && !In.GetBinding(D,*RD,false,Far),
		"scoped paired baseline did not distinguish covered and uncovered landings");
	Require(Out.GetBinding(S,*Source,false,OnlySourceWindow).has_value() && !Out.GetBinding(D,*Target,false,OnlySourceWindow),
		"fixture did not produce physically different covered leaf boundaries");
	auto Edit=std::make_shared<blockgrid::EditRequest>();
	Edit->ExpectedRevision=Target->GetBlockRevision();
	Edit->Bilateral=blockgrid::GridTransferEdit{{},S,Source->GetBlockRevision()};
	Require(MutationGateway{}.Apply(SubmitBlockGridEditCommand{D,World->GetObjectId(),Edit},
		ScriptSecurityContext::CoreTrusted()).Succeeded(),"authoritative native paired merge failed");
	const auto Stamp=Source->GetPublicationTransfer();
	Require(Stamp && Stamp==Target->GetPublicationTransfer(),"publication stamp did not reach both participants");
	Generation=Stamp->WorldRevision;
	Require(!Proof(S) && !Proof(D),"stale paired snapshots admitted after source commit");
	for (unsigned I=0;I<50 && !(Proof(S) && Proof(D));++I) Exchange();
	Require(PairedAcks==2 && Proof(S) && Proof(D) && RS->GetField().GetOccupiedCount()==0 &&
		RD->GetField().GetOccupiedCount()==2,"paired current installations never converged");
	Require(!Out.GetBinding(S,*Source,false,OnlySourceWindow) && !In.GetBinding(S,*RS,false,OnlySourceWindow),
		"one participant's regional absence was certified by its paired neighbor");
	BothVisible=false; In.Forget(D);Out.Forget(D);
	for (unsigned I=0;I<8;++I) Exchange();
	Require(Redacted && !Proof(S) && !RD->HasReplicaField(),"retired counterpart admitted the surviving movement receipt");
	std::cout<<"[BlockGrid:PairedStream] acknowledged="<<PairedAcks<<" withheld=1 halfInstalledRejected=1\n";
	Client.Destroy(); Authority.Destroy();
}
