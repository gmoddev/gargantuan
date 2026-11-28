// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
// Included by the native moving-construction benchmark inside its anonymous namespace.
struct ScaleCell {
	blockgrid::Cell Position;
	std::uint16_t Definition;
	std::uint8_t Orientation{};
	double Volume{1.};
	blockgrid::Shape Shape=blockgrid::Shape::Cube;
};

std::vector<ScaleCell> MakeLayout(std::string_view Pattern, unsigned Count,
	const std::array<std::uint16_t,3>& Special) {
	std::vector<ScaleCell> Layout;
	Layout.reserve(Count);
	for (unsigned I = 0; I < Count; ++I) {
		int X = I % 16, Y = I / 256, Z = (I / 16) % 16;
		if (Pattern == "Fragmented") { X *= 2; Y *= 2; Z *= 2; }
		if (Pattern == "Hollow") {
			const int Edge = I % 64;
			Y = I / 64;
			if (Edge < 16) { X = Edge; Z = 0; }
			else if (Edge < 32) { X = 16; Z = Edge - 16; }
			else if (Edge < 48) { X = 48 - Edge; Z = 16; }
			else { X = 0; Z = 64 - Edge; }
		}
		if (Pattern == "Irregular") X += 3 * ((Z / 3 + Y / 4) % 2);
		ScaleCell Cell{{X,Y,Z}, I%2 ? blockgrid::Stone : blockgrid::Brick};
		if (Pattern == "Mixed" && I % 64 == 7) {
			const auto Kind = (I / 64) % 3;
			Cell.Definition = Special[Kind];
			Cell.Orientation = static_cast<std::uint8_t>((I / 64) % 24);
			Cell.Volume = Kind == 2 ? .75 : .5;
			constexpr std::array Shapes{blockgrid::Shape::Slab,blockgrid::Shape::Wedge,blockgrid::Shape::Stairs};
			Cell.Shape=Shapes[Kind];
		}
		Layout.push_back(Cell);
	}
	return Layout;
}

// Integrate the authored unit solids, independently of the physics hull enumeration.
// The wedge occupies 0 <= Y <= Z <= 1, and the stair is a half slab plus a quarter block.
struct ReferenceMass {
	double Mass{};
	glm::dvec3 Center{};
	glm::dmat3 Inertia{0.};
};
ReferenceMass IntegrateCells(std::span<const ScaleCell> Cells, double Unit) {
	ReferenceMass Result;
	glm::dmat3 Second(0.);
	for (const auto& Cell : Cells) {
		glm::dvec3 Center(.5);
		glm::dmat3 Covariance(1./12);
		switch (Cell.Shape) {
			case blockgrid::Shape::Cube: break;
			case blockgrid::Shape::Slab: Center.y=.25; Covariance[1][1]=1./48; break;
			case blockgrid::Shape::Wedge:
				Center.y=1./3; Center.z=2./3;
				Covariance[1][1]=Covariance[2][2]=1./18;
				Covariance[1][2]=Covariance[2][1]=1./36;
				break;
			case blockgrid::Shape::Stairs:
				Center.y=5./12; Center.z=7./12;
				Covariance[1][1]=Covariance[2][2]=11./144;
				Covariance[1][2]=Covariance[2][1]=1./36;
				break;
			default: throw std::runtime_error("unexpected oracle geometry");
		}
		glm::dmat3 R(0.);
		const auto& Basis=blockgrid::Basis(Cell.Orientation);
		for (int I=0;I<3;++I) for (int J=0;J<3;++J) R[I][J]=Basis[I][J];
		const auto C=(glm::dvec3(Cell.Position.X,Cell.Position.Y,Cell.Position.Z)+.5+R*(Center-.5))*Unit;
		const auto Mass=Cell.Volume*Unit*Unit*Unit;
		Result.Mass+=Mass;
		Result.Center+=C*Mass;
		Second+=Mass*(Unit*Unit*R*Covariance*glm::transpose(R)+glm::outerProduct(C,C));
	}
	Result.Center/=Result.Mass;
	Second-=Result.Mass*glm::outerProduct(Result.Center,Result.Center);
	Result.Inertia=glm::dmat3(Second[0][0]+Second[1][1]+Second[2][2])-Second;
	return Result;
}

void CheckMass(const blockgrid::AssemblyProjection& State,const ReferenceMass& Expected,Json& Row,
	const std::string& Prefix) {
	const auto CenterError=glm::length(glm::dvec3(State.LocalCenterOfMass)-Expected.Center);
	double Relative{};
	for (int I=0;I<3;++I) for (int J=0;J<3;++J)
		Relative=std::max(Relative,std::abs(State.LocalInertia[I][J]-Expected.Inertia[I][J])/
			std::max({1.,std::abs(Expected.Inertia[I][J]),.001*Expected.Mass}));
	Row[Prefix+"COMError"]=CenterError;
	Row[Prefix+"InertiaRelativeError"]=Relative;
	Require(blockgrid::ValidAssemblyProjection(State) && std::abs(State.Mass-Expected.Mass)<.06 &&
		CenterError<.006 && Relative<.025,
		("authored solid integrals disagree at "+Prefix+" "+Row.dump()).c_str());
}

Json StepDistribution(std::vector<double> Times) {
	std::sort(Times.begin(), Times.end());
	const auto P = [&](double Fraction) { return Times[static_cast<std::size_t>(std::ceil(Times.size()*Fraction))-1]; };
	return {{"N",Times.size()},{"P50",P(.50)},{"P95",P(.95)},{"P99",P(.99)},{"Worst",Times.back()}};
}

PhysicsBodyId CheckChildren(WorldRoot& World, const BlockGrid& Grid, unsigned& HullCount) {
	const std::array Required{Grid.GetObjectId()};
	const auto Receipt = World.PrepareSpatialDestination({{-150,-150,-150},{150,150,150}}, Required);
	Require(Receipt.has_value(), "could not inspect authoritative collision");
	const auto Found = std::ranges::find(Receipt->Providers, Grid.GetObjectId(), &SpatialProviderReceipt::Owner);
	Require(Found != Receipt->Providers.end() && Found->Shape == PhysicsShapeKind::ConvexAssembly &&
		!Found->Installed.empty(), "missing native convex children");
	HullCount = static_cast<unsigned>(Found->Installed.size());
	const auto Body = Found->Installed.front().Body;
	for (unsigned I=0; I<HullCount; ++I)
		Require(Found->Installed[I].Body == Body && Found->Installed[I].Piece == I, "unexpected compound child ownership");
	return Body;
}

Json MeasureScale(std::string_view Pattern, unsigned Count, unsigned Trial, bool Original) {
	using namespace gargantuan::runtime_detail;
	const auto BeforeWorld = b3GetByteCount();
	Json Row{{"Layout",Pattern},{"Occupied",Count},{"Trial",Trial}};
	const float Unit = Count == 4096 ? .5f : 1.f;
	Row["CellSize"] = Unit;
	{
		auto Game = std::make_shared<DataModel>();
		auto World = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
		World->SetGravity(0.f);
		auto Grid = std::make_shared<BlockGrid>();
		Grid->SetCellSize(Unit);
		Grid->SetCFrame(CFrame(0,8,0));
		Grid->SetParent(World);
		MutationGateway Gateway;
		const auto Apply = [&](const blockgrid::EditRequest& E) {
			return Gateway.Apply(SubmitBlockGridEditCommand{Grid->GetObjectId(),Game->GetObjectId(),
				std::make_shared<blockgrid::EditRequest>(E)},ScriptSecurityContext::CoreTrusted());
		};
		const auto Submit = [&](const blockgrid::EditRequest& E) {
			const auto Reply=Apply(E);
			if (!Reply.Succeeded()) throw std::runtime_error(Reply.Message);
		};
		std::array<std::uint16_t,3> Special{};
		if (Pattern == "Mixed") {
			const std::array Shapes{blockgrid::Shape::Slab,blockgrid::Shape::Wedge,blockgrid::Shape::Stairs};
			for (unsigned I=0; I<Shapes.size(); ++I) {
				blockgrid::Definition Spec; Spec.Key="project:shape"+std::to_string(I); Spec.Geometry=Shapes[I];
				blockgrid::EditRequest Edit; Edit.Binding=blockgrid::BindingEdit{Spec};
				Submit(Edit); Special[I]=Grid->GetField().GetDefinitionId(Spec.Key);
			}
		}
		const auto Layout = MakeLayout(Pattern,Count,Special);
		std::map<std::pair<int,int>, blockgrid::Cell> Tops;
		const auto FullMass=IntegrateCells(Layout,Unit);
		const auto Center=[&](blockgrid::Cell C){return (glm::dvec3(C.X,C.Y,C.Z)+.5)*double(Unit);};
		for (const auto& C : Layout) {
			const auto Key=std::pair(C.Position.X,C.Position.Z);
			const auto It=Tops.find(Key);
			if (It==Tops.end() || It->second.Y<C.Position.Y) Tops[Key]=C.Position;
		}
		const auto BeforeField = b3GetByteCount();
		Row["EditFieldUs"] = TimeUs([&] {
			for (std::size_t Begin=0; Begin<Layout.size(); Begin+=blockgrid::MaximumCommands) {
				blockgrid::EditRequest Edit;
				const auto End=std::min(Layout.size(),Begin+blockgrid::MaximumCommands);
				for (std::size_t I=Begin; I<End; ++I)
					Edit.Writes.push_back({blockgrid::SingleCell(Layout[I].Position),{Layout[I].Definition,Layout[I].Orientation}});
				Submit(Edit);
			}
		});
		Require(Grid->GetField().GetOccupiedCount()==Count,"generator has duplicate occupied positions");
		Row["FieldSnapshotBudgetChargeBytes"] = Grid->GetField().GetSnapshotBytes();
		Row["CanonicalFieldBytes"] = blockgrid::Encode(Grid->GetField()).size();
		Row["StaticInstallUs"] = TimeUs([&]{ World->StepPhysics(0.,std::nullopt); });
		const auto StaticBytes = b3GetByteCount();
		Row["Box3DStaticBytesOverEmpty"] = StaticBytes-BeforeField;
		blockgrid::EditRequest Enable;
		Enable.ExpectedRevision=Grid->GetBlockRevision(); Enable.Motion=blockgrid::MotionEdit{true};
		const bool Expected = Original ? Count<=128 : Pattern!="Fragmented" || Count<=128;
		bool Accepted{};
		WorkSample Activation{};
		{
			WorkCapture Capture(&Activation);
			Row["ActivateUs"]=TimeUs([&]{const auto Reply=Apply(Enable); Accepted=Reply.Succeeded(); Row["Admission"]=Reply.Message;});
		}
		const auto& Created=Activation[static_cast<std::size_t>(WorkPhase::PhysicsBodyCreate)];
		Row["NativeBodyAndShapeCreationUs"]=Created.Nanoseconds/1000.;
		Row["NativeBodiesCreatedAtActivation"]=Created.Units;
		Row["Accepted"]=Accepted;
		Require(Accepted==Expected,
			("unexpected admission "+std::string(Pattern)+" "+std::to_string(Count)+" "+Row.dump()).c_str());
		if (Accepted) {
			unsigned Hulls{};
			const auto Body = CheckChildren(*World,*Grid,Hulls);
			Row["InstalledHulls"]=Hulls;
			Row["Box3DDynamicBytesOverEmpty"]=b3GetByteCount()-BeforeField;
			const auto State=*Grid->GetAssemblyProjection();
			Row["Mass"]=State.Mass;
			CheckMass(State,FullMass,Row,"");
			auto Velocity=[&](blockgrid::Point V,blockgrid::Point A) {
				blockgrid::EditRequest E; E.ExpectedRevision=Grid->GetBlockRevision();
				E.Motion=blockgrid::MotionEdit{true,std::pair{V,A}}; Submit(E);
			};
			std::vector<double> VelocityTimes;
			for (unsigned I=0;I<16;++I) VelocityTimes.push_back(TimeUs([&]{
				Velocity({I%2?2.f:2.1f,0,0},{0,.35f,0});
			}));
			Row["VelocityCommandAndPublishUs"]=StepDistribution(VelocityTimes);
			Row["VelocitySamplesUs"]=VelocityTimes;
			const auto Topology=Grid->GetField().GetGeometryRevision();
			const auto InitialProjection=Grid->GetAssemblyRevision();
			std::uint64_t Published=InitialProjection;
			unsigned MotionEvents{};
			const auto Observer=Grid->GetPropertyChangedSignal("CFrame")->Connect([&](auto) {
				const auto Sample=Grid->GetAssemblyProjection();
				Require(Sample && Sample->Revision>Published && Sample->Pose.Position==Grid->GetCFrame().Position &&
					Sample->Pose.Rotation==Grid->GetCFrame().Rotation,"motion observer read a stale published projection");
				Published=Sample->Revision;
				++MotionEvents;
			});
			std::vector<double> FreeMotion;
			for (unsigned I=0;I<30;++I) FreeMotion.push_back(TimeUs([&]{World->StepPhysics(1./60,std::nullopt);}));
			Observer->Disconnect();
			Require(MotionEvents>10 && Published==Grid->GetAssemblyRevision(),"missing observed motion publications");
			Row["MotionEvents30Ticks"]=MotionEvents;
			Row["FreeMotionWithObserverUs"]=StepDistribution(FreeMotion);
			Row["FreeMotionSamplesUs"]=FreeMotion;
			const auto Moved=*Grid->GetAssemblyProjection();
			const auto BeforeCOM=State.Pose.PointToWorldSpace(State.LocalCenterOfMass);
			const auto AfterCOM=Moved.Pose.PointToWorldSpace(Moved.LocalCenterOfMass);
			Require(AfterCOM.x>BeforeCOM.x+.3f && Moved.Pose.Rotation!=State.Pose.Rotation &&
				Grid->GetAssemblyRevision()>InitialProjection && Grid->GetField().GetGeometryRevision()==Topology,
				"translation or rotation did not publish independently of geometry");
			RaycastParams Params; Params.IncludeBlockGrids=true;
			std::vector<double> Queries;
			unsigned Index{};
			for(const auto& [Column,Cell]:Tops) {
				if (Index++ % std::max<std::size_t>(1,Tops.size()/16)) continue;
				const auto B=Grid->GetField().Get(Cell);
				if (B.Definition!=blockgrid::Stone && B.Definition!=blockgrid::Brick) continue;
				const auto Local=glm::vec3(Center(Cell));
				const auto Origin=Moved.Pose.PointToWorldSpace(Local+glm::vec3(0,1.5f*Unit,0));
				const auto Dir=Moved.Pose.VectorToWorldSpace({0,-3*Unit,0});
				Queries.push_back(TimeUs([&]{
					const auto Hit=World->ResolveRaycast(Origin,Dir,Params);
					Require(Hit.GridInstance==Grid && Hit.Cell==std::optional(glm::ivec3(Cell.X,Cell.Y,Cell.Z)),
						"moving world ray lost its authored cell");
				}));
			}
			Require(!Queries.empty(),"missing moving query sample");
			Row["WorldRayUs"]=StepDistribution(Queries);
			Row["RaySamplesUs"]=Queries;
			blockgrid::EditRequest Remove,Insert;
			const auto Removable=Layout.front();
			Remove.Writes.push_back({blockgrid::SingleCell(Removable.Position),{}});
			Insert.Writes.push_back({blockgrid::SingleCell(Removable.Position),{Removable.Definition,Removable.Orientation}});
			const auto Revision=Grid->GetField().GetGeometryRevision();
			Row["MovingRemoveUs"]=TimeUs([&]{Submit(Remove);});
			CheckMass(*Grid->GetAssemblyProjection(),IntegrateCells(std::span(Layout).subspan(1),Unit),Row,"Removed");
			Row["MovingInsertUs"]=TimeUs([&]{Submit(Insert);});
			CheckMass(*Grid->GetAssemblyProjection(),FullMass,Row,"Restored");
			Require(Grid->GetField().GetGeometryRevision()==Revision+2 && CheckChildren(*World,*Grid,Hulls)==Body &&
				glm::length(Grid->GetAssemblyProjection()->LinearVelocity)>.5f,"live edits lost motion or physical identity");
			auto Ground=std::make_shared<Part>();
			Ground->SetSize({100,1,100}); Ground->SetCFrame(CFrame(16,-.5,16)); Ground->SetAnchored(true); Ground->SetParent(World);
			Velocity({0,0,0},{0,0,0}); Grid->SetCFrame(CFrame(0,2,0)); World->SetGravity(20.f);
			std::vector<double> Steps;
			WorkSample Work{};
			{
				WorkCapture Capture(&Work);
				for (unsigned I=0;I<120;++I) Steps.push_back(TimeUs([&]{World->StepPhysics(1./60,std::nullopt);}));
			}
			Row["StepUs"]=StepDistribution(Steps);
			Row["ContactStepSamplesUs"]=Steps;
			for(const auto Phase : {WorkPhase::PhysicsPairsProfile,WorkPhase::PhysicsCollideProfile,WorkPhase::PhysicsSolveProfile})
				Row[std::string(WorkPhaseNames[static_cast<std::size_t>(Phase)])+"120Us"]=Work[static_cast<std::size_t>(Phase)].Nanoseconds/1000.;
			const auto Rest=*Grid->GetAssemblyProjection();
			Row["SettledGridY"]=Rest.Pose.Position.y;
			Require(std::abs(Rest.Pose.Position.y)<.3f,"compound failed actual floor contact or tipped");
			Row["RetireGridUs"]=TimeUs([&]{Grid->Destroy();});
			Require(World->GetRegisteredBlockGridCount()==0,"grid remained registered after destruction");
		} else {
			Require(Grid->GetBlockRevision()==*Enable.ExpectedRevision && Grid->GetMotionMode()=="Static" &&
				Grid->GetField().GetOccupiedCount()==Count,"denied motion changed static construction");
		}
		Game->Destroy();
	}
	Row["Box3DBytesAfterWorldRetirement"]=b3GetByteCount()-BeforeWorld;
	Require(Row["Box3DBytesAfterWorldRetirement"].get<std::int64_t>()==0,"Box3D retained native bytes after world release");
	return Row;
}

int RunCompoundScale(int Argc, char** Argv) {
	if (std::string_view(Argv[1])!="--compound-scale" || Argc>3) throw std::invalid_argument("expected --compound-scale [--original|--smoke]");
	const auto Option=Argc==3?std::string_view(Argv[2]):std::string_view{};
	if (!Option.empty() && Option!="--smoke" && Option!="--original") throw std::invalid_argument("unrecognized scale option");
	const bool Smoke=Option=="--smoke";
	for(unsigned Trial=1;Trial<=(Smoke?1u:3u);++Trial)
		for(const auto Count:{128u,256u,512u,1024u,2048u,4096u})
			for(const auto Pattern:{"Solid","Fragmented","Hollow","Irregular","Mixed"}) {
				if (Smoke && Count!=128 && Count!=2048) continue;
				std::cout<<MeasureScale(Pattern,Count,Trial,Option=="--original").dump()<<std::endl;
			}
	return 0;
}
