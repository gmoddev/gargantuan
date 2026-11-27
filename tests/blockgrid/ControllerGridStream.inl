// Runs with the real two-sided peer protocol, installed native providers and changing secondary origins.
void CheckControllerGridStream() {
	using namespace blockgrid;
	auto World=std::make_shared<DataModel>();
	HeadlessRenderer AuthorView(Vector2(64,64)),ClientView(Vector2(64,64));
	Engine Authority(World,&AuthorView,nullptr,{.Mode=RuntimeMode::Offline,.AdmitSpatialContent=true});
	const auto Workspace=World->GetService("Workspace");
	const auto Tilt=CFrame::Angles(0,.2617994f,0)*CFrame::Angles(.78539816f,0,0);
	GridInterest Interest;
	Interest.ControlledMotion=SpatialDestinationBounds{{-3,-4,-3},{7,8,7}};
	Field Building;
	const auto Rectangle=SelectGridCoverage(Building,Tilt,.5f,Interest).Rectangle.value();
	const auto [Low,High]=Rectangle;
	unsigned Made{};
	for (int Z=Low.Z;Z<High.Z && Made<20;Z+=8) for (int Y=Low.Y;Y<High.Y && Made<20;Y+=8)
		for (int X=Low.X;X<High.X && Made<20;X+=8,++Made) {
			std::vector<blockgrid::Write> Writes;
			for (unsigned I=0;I<65;++I)
				Writes.push_back({SingleCell(OffsetCell({X,Y,Z},I)),{Stone,static_cast<std::uint8_t>(I%24)}});
			Require(Building.Apply(Writes),"controller fixture leaf");
		}
	const Cell FarA{1024,0,0},FarB{-1024,0,0},Never{99999,0,0};
	const std::array<blockgrid::Write,3> Remote{{{SingleCell(FarA),{Brick,0}},{SingleCell(FarB),{Brick,0}},{SingleCell(Never),{Stone,0}}}};
	Require(Building.Apply(Remote),"controller fixture remote cells");
	const auto A=std::make_shared<BlockGrid>();
	A->SetCellSize(.5f);A->SetCFrame(Tilt);A->RestoreField(std::move(Building));A->SetParent(Workspace);
	const auto B=Seed(Workspace,"SecondGrid",Brick);
	const auto IdA=A->GetObjectId(),IdB=B->GetObjectId();
	ReplicationCoordinator Coordinator(World);
	const auto Baseline=Coordinator.AddPeer({1,1},ReplicationEpoch(1));
	ReplicaApplier Replica;
	Require(Baseline.Succeeded() && Replica.ApplyFrame(*Baseline.Frame).Succeeded(),"controller grid shells");
	Engine Client(std::dynamic_pointer_cast<DataModel>(Replica.GetReplicaRoot()),&ClientView,nullptr,
		{.Mode=RuntimeMode::Offline,.AdmitSpatialContent=true});
	const auto RA=std::dynamic_pointer_cast<BlockGrid>(Replica.Resolve(IdA)),RB=std::dynamic_pointer_cast<BlockGrid>(Replica.Resolve(IdB));
	Require(RA && RB,"controller replica adoption");
	BlockGridPeer Out(true,54,{}),In(false,54,{});
	auto Sources=[&](ObjectId Id){return Id==IdA?A:Id==IdB?B:nullptr;};
	auto Shells=[&](ObjectId Id){return Id==IdA?RA:Id==IdB?RB:nullptr;};
	In.Track(IdA);In.Track(IdB);
	const auto Bounds=*Interest.ControlledMotion;
	const auto NeverBounds=Tilt.PointToWorldSpace(glm::vec3(Never.X+.5f,.5f,.5f)*.5f);
	Interest.Count=1;
	unsigned CoreFragments{},SecondaryOffers{},Continuous{},Cancellations{};
	bool BoundA{},BoundB{},ViewA{},ViewB{};
	std::optional<AssetContentId> OriginalA,OriginalB;
	for (std::uint64_t Tick=1;Tick<=80;++Tick) {
		const auto Focus=Tick<=45 ? (Tick%2?FarA:FarB) : (Tick<=62?FarA:FarB);
		Interest.Centers[0]=Tilt.PointToWorldSpace(glm::vec3(Focus.X+.5f,.5f,.5f)*.5f);
		In.Pump(Shells,[&](const GridStreamMessage& M) {
			const auto Encoded=EncodeGridStream(M);
			Out.Receive(DecodeGridStream(Encoded).value(),Sources,Tick); return true;
		},Tick);
		Out.Pump(Sources,[&](const GridStreamMessage& M) {
			if (M.Kind==GridStreamKind::Fragment && M.Scope.Grid==IdA && !BoundA) ++CoreFragments;
			if (M.Kind==GridStreamKind::Offer && BoundA && M.Scope.Grid==IdA) ++SecondaryOffers;
			if (M.Kind==GridStreamKind::Forget) ++Cancellations;
			const auto Encoded=EncodeGridStream(M);
			In.Receive(DecodeGridStream(Encoded).value(),Shells,Tick); return true;
		},Tick,Interest);
		const auto Inspect=[&](ObjectId Id,bool& Established,std::optional<AssetContentId>& Prior) {
			const auto S=Out.GetBinding(Id,*Sources(Id),false,Bounds),C=In.GetBinding(Id,*Shells(Id),false,Bounds);
			if (!Established && (!S || !C)) return;
			Require(S && C && S->Content==C->Content && S->LogicalRevision==C->LogicalRevision &&
				S->CoverageRevision==C->CoverageRevision,"secondary views interrupted or changed the installed actor receipt");
			Require(!Prior || *Prior==S->Content,"changing a camera changed the actor collision proof");
			Prior=S->Content; Established=true;
		};
		Inspect(IdA,BoundA,OriginalA);Inspect(IdB,BoundB,OriginalB);
		if (BoundA && BoundB) ++Continuous;
		if (RA->HasReplicaField()) {
			Require(RA->GetField().Get(Never).Definition==Air &&
				!In.GetBinding(IdA,*RA,true,SpatialDestinationBounds{NeverBounds,NeverBounds}),"unrequested distant occupied page was disclosed");
			ViewA|=RA->GetField().Get(FarA).Definition==Brick;
			ViewB|=RA->GetField().Get(FarB).Definition==Brick;
		}
	}
	Require(CoreFragments>=2 && SecondaryOffers>=2 && Continuous>=45 && Cancellations==0 && ViewA && ViewB,
		"actor priority or independently refreshed secondary views failed to progress");
	const auto Edit=std::make_shared<EditRequest>();
	Edit->Writes.push_back({SingleCell({0,0,0}),{Brick,17}});
	Require(MutationGateway{}.Apply(SubmitBlockGridEditCommand{IdA,World->GetObjectId(),Edit},
		ScriptSecurityContext::CoreTrusted()).Succeeded() && !Out.GetBinding(IdA,*A,false,Bounds),
		"changed controller geometry kept its previous receipt");
	bool Replacement{};
	for (std::uint64_t Tick=81;Tick<=100;++Tick) {
		In.Pump(Shells,[&](const GridStreamMessage& M) {
			const auto Encoded=EncodeGridStream(M);
			Out.Receive(DecodeGridStream(Encoded).value(),Sources,Tick); return true;
		},Tick);
		Out.Pump(Sources,[&](const GridStreamMessage& M) {
			const auto Encoded=EncodeGridStream(M);
			In.Receive(DecodeGridStream(Encoded).value(),Shells,Tick);
			if (M.Kind==GridStreamKind::Offer && M.Scope.Grid==IdA) {
				Require(!M.RetainsController && !In.GetBinding(IdA,*RA,false,Bounds),
					"a partial geometry replacement reused the old controller receipt");
				Replacement=true;
			}
			return true;
		},Tick,Interest);
	}
	const auto Old=Out.GetBinding(IdA,*A,false,Bounds);
	const auto Fresh=In.GetBinding(IdA,*RA,false,Bounds);
	Require(Replacement && Old && Fresh && Old->Content==Fresh->Content && Old->Content!=OriginalA,
		"fresh controller geometry failed to replace the previous footprint");
	const auto Populate=std::make_shared<EditRequest>();
	for (int Z=0;Z<4;++Z) for (int Y=0;Y<3;++Y) for (int X=0;X<10;++X)
		Populate->Writes.push_back({SingleCell({2048+8*X,8*Y,8*Z}),{Stone,0}});
	constexpr std::array<Cell,3> ViewCells{{{-1024,0,0},{-2048,0,0},{-3072,0,0}}};
	for (const auto P:ViewCells) Populate->Writes.push_back({SingleCell(P),{Brick,0}});
	Require(MutationGateway{}.Apply(SubmitBlockGridEditCommand{IdB,World->GetObjectId(),Populate},
		ScriptSecurityContext::CoreTrusted()).Succeeded(),"camera fallback source edits");
	const std::array<SpatialDestinationBounds,3> Refused{{
		{{4097,1,1},{4241,33,49}},     // 120 occupied pages in a legal rectangle.
		{{0,0,0},{6400,6400,6400}},   // Exceeds the rectangle address limit.
		{{3000000,0,0},{3000001,1,1}} // Outside the grid coordinate range.
	}};
	for (std::size_t I=0;I<Refused.size();++I) {
		Interest.ControlledMotion=Refused[I];
		const auto P=ViewCells[I];
		Interest.Centers[0]=glm::vec3(2*P.X+1,1,1);
		const auto Coverage=SelectGridCoverage(B->GetField(),B->GetCFrame(),B->GetCellSize(),Interest);
		Require(!Coverage.Whole && !Coverage.Rectangle && Coverage.Contains(SingleCell(P)),
			"refused controller footprint suppressed an authorized camera selection");
		auto Stage=Interest;Stage.Destination=Refused[I];
		const auto Staged=SelectGridCoverage(B->GetField(),B->GetCFrame(),B->GetCellSize(),Stage);
		Require(Staged.Origins.empty() && (!Staged.Rectangle || !B->GetField().CapturePages(Staged,{})),
			"refused staged landing substituted camera pages");
		for (std::uint64_t Tick=101+30*I;Tick<131+30*I;++Tick) {
			In.Pump(Shells,[&](const GridStreamMessage& M){Out.Receive(M,Sources,Tick);return true;},Tick);
			Out.Pump(Sources,[&](const GridStreamMessage& M){In.Receive(M,Shells,Tick);return true;},Tick,Interest);
		}
		Require(RB->HasReplicaField() && RB->GetField().Get(P).Definition==Brick &&
			!In.GetBinding(IdB,*RB,false,Refused[I]) && !Out.GetBinding(IdB,*B,false,Refused[I]),
			"camera content or controller denial failed at a rectangular limit");
	}
	A->SetParent(nullptr);A->SetParent(Workspace);
	Require(!Out.GetBinding(IdA,*A,false,Bounds),"stale native membership authorized movement after reattachment");
	std::cout<<"[BlockGrid:ControllerStream] initialFragments="<<CoreFragments<<" secondaryOffers="<<SecondaryOffers
		<<" continuousTicks="<<Continuous<<" canceledByCamera="<<Cancellations<<'\n';
	Client.Destroy();Authority.Destroy();
}
