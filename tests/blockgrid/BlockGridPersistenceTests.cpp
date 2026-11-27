// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "RegionalGridFixture.hpp"
#include "gargantuan/assets/GasInstanceSerialization.hpp"
#include "gargantuan/classes/BlockGrid.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/editor/PlaySession.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/Project.hpp"
#include "gargantuan/packaging/PackageBuilder.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <fstream>
#include <iostream>

namespace {
	using namespace gargantuan;
	namespace Fs=std::filesystem;

	void Require(bool Condition,std::string_view Message) {
		if (!Condition) throw std::runtime_error(std::string(Message));
	}
	template<class T> void RequireRejected(T&& Run,std::string_view Message) {
		try { Run(); } catch (const std::exception&) { return; }
		throw std::runtime_error(std::string(Message));
	}
	struct Scratch {
		Fs::path Root=Fs::temp_directory_path()/("gargantuan-blockgrid-persistence-"+ProjectId::New().ToString());
		Scratch() { Require(Fs::create_directory(Root),"Could not create a fresh project"); }
		~Scratch() { std::error_code Ignored; Fs::remove_all(Root,Ignored); }
	};
	void CheckWorld(const std::shared_ptr<DataModel>& World,const blockgrid::SegmentData& Expected,ObjectId Original) {
		auto Grid=std::dynamic_pointer_cast<BlockGrid>(World->FindFirstDescendant("SavedGrid"));
		Require(Grid && Grid->GetObjectId()!=Original && blockgrid::Encode(Grid->GetField())==Expected,
			"Restored world lost canonical grid data or reused an authoring lifetime");
		Require(Grid->GetField().GetValue({12,12,12},0)==231 && Grid->GetField().GetValue({12,12,12},1)==17,
			"Trusted restore lost public or private cell state");
	}
	void ReplaceByte(const Fs::path& Path) {
		std::fstream Writer(Path,std::ios::binary|std::ios::in|std::ios::out);
		char Original{};
		Writer.read(&Original,1);
		Original^=0x5a;
		Writer.seekp(0);
		Writer.write(&Original,1);
		Writer.close();
		Require(Writer.good(),"Failed to write the fixture's altered byte");
	}
	void CheckPackage(const Fs::path& Runtime,const Fs::path& Destination,const GamePayload& Payload,
		const blockgrid::SegmentData& Expected,ObjectId Id) {
		auto Built=PackageBuilder::Build({.Payload=Payload,.RuntimeDistributionRoot=Runtime,.OutputDirectory=Destination});
		if (!Built.Ok) {
			for (const auto& D:Built.Diagnostics) if (D.Severity==PackageDiagnosticSeverity::Error)
				throw std::runtime_error(D.Code+" "+D.Message+" "+D.Item);
			throw std::runtime_error("Trusted package build did not finish");
		}
		std::vector<PackageDiagnostic> Diagnostics;
		auto Loaded=PackageBuilder::Load(Destination,Diagnostics);
		Require(Loaded && Loaded->GridSegments==Payload.GridSegments,"Verified package lost its native segments");
		auto World=PackageBuilder::LoadWorld(*Loaded,Destination);
		CheckWorld(World,Expected,Id);
		World->Destroy();
		const auto PartPath=Destination/"content"/".gargantuan"/"blockgrid"/blockgrid::SegmentFileName(Payload.GridSegments.begin()->first);
		ReplaceByte(PartPath);
		Diagnostics.clear();
		Require(!PackageBuilder::Load(Destination,Diagnostics),"Runtime admitted a corrupt grid segment");
		// A prior verified immutable capture must still restore its own bytes.
		World=PackageBuilder::LoadWorld(*Loaded,Destination);
		CheckWorld(World,Expected,Id);
		World->Destroy();
		Require(Fs::remove(PartPath),"Could not remove the fixture's package segment");
		Diagnostics.clear();
		Require(!PackageBuilder::Load(Destination,Diagnostics),"Runtime admitted a missing grid segment");
	}
	void Run(const Fs::path& Runtime) {
		Scratch Temporary;
		const auto Origin=Temporary.Root/"Origin";
		Fs::create_directory(Origin);
		DiskFilesystem Files(Origin);
		auto ProjectValue=Project::fromInit(&Files,"LargeGrid");
		auto Game=ProjectValue.DeserializeGame();
		Game->InitializeLoadedProjectRevision();
		const auto Grid=std::make_shared<BlockGrid>();
		Grid->SetName("SavedGrid");
		Grid->SetCanCollide(false);
		Grid->SetCanQuery(false);
		Grid->SetVisible(false);
		Grid->RestoreField(blockgrid::test::LargePublicField());
		Grid->SetParent(Game->GetService("Workspace"));
		Game->MarkPersistenceSubtreeArchivable();
		const auto Expected=blockgrid::Encode(Grid->GetField());
		const auto Id=Grid->GetObjectId();
		Require(Expected.size()>Gas::MaximumBinaryBytes,"The fixture must exceed the generic binary limit");
		const auto Cloned=std::dynamic_pointer_cast<BlockGrid>(Grid->Clone());
		Require(Cloned && !Cloned->GetParent() && Cloned->GetObjectId()!=Id &&
			blockgrid::Encode(Cloned->GetField())==blockgrid::Encode(Grid->GetField(),blockgrid::DataProfile::Clone),
			"Instance.Clone lost the copy-profile field or reused a native lifetime");
		Cloned->Destroy();

		const auto Revision=Game->GetAuthoritativeRevision();
		const auto Snapshot=ProjectValue.CaptureGame(Game,Revision);
		Require(Snapshot.GridSegments.size()==5 && Snapshot.Contents.size()<64*1024 &&
			Snapshot.Contents.find(blockgrid::SegmentKey)!=std::string::npos,
			"Project capture did not create five native artifacts and a small root");
		ProjectValue.PersistGameAtomically(Snapshot);
		auto Reopened=Project::fromExisting(&Files).DeserializeGame();
		CheckWorld(Reopened,Expected,Id);
		Reopened->Destroy();
		{
			PlaySession Session({1},Snapshot.Contents,ProjectValue.InstanceFileFormat,Origin,128,96,Revision,
				Snapshot.Assets,false,{},{},{},{},&Snapshot.GridSegments);
			CheckWorld(Session.GetWorld(),Expected,Id);
			Session.Stop();
		}
		Game->SetName("StagedReplacement");
		const auto Changed=ProjectValue.CaptureGame(Game,Game->GetAuthoritativeRevision());
		RequireRejected([&]{ ProjectValue.PersistGameAtomically(Changed,[]{ throw std::runtime_error("injected publication failure"); }); },
			"Atomic replacement fault did not stop publication");
		Require(Files.ReadFileToString(ProjectValue.InstanceFilePath)==Snapshot.Contents,"A failed replacement changed the prior project root");
		auto Missing=Changed;
		Missing.GridSegments.erase(Missing.GridSegments.begin());
		RequireRejected([&]{ ProjectValue.PersistGameAtomically(Missing); },"Incomplete capture replaced the durable project");
		const auto Path=ProjectValue.RootConfiguration/"blockgrid"/blockgrid::SegmentFileName(Snapshot.GridSegments.begin()->first);
		auto Prepared=ProjectValue.PrepareGameLoad();
		ReplaceByte(Path);
		RequireRejected([&]{ (void)ProjectValue.PrepareGameLoad(); },"Project preflight accepted a corrupt native artifact");
		RequireRejected([&]{ ProjectValue.PersistGameAtomically(Changed); },"A save accepted an invalid artifact at a content-addressed path");
		Require(Files.ReadFileToString(ProjectValue.InstanceFilePath)==Snapshot.Contents,"A corrupt closure changed the durable root");
		Reopened=ProjectValue.DeserializeGame(std::move(Prepared));
		CheckWorld(Reopened,Expected,Id);
		Reopened->Destroy();
		Require(Fs::remove(Path),"Failed to remove the test artifact");
		RequireRejected([&]{ (void)ProjectValue.PrepareGameLoad(); },"Project preflight accepted a missing native artifact");

		// Both GAS kinds carry all grid bytes and can be consumed while project CAS is incomplete.
		const auto Export=Temporary.Root/"Portable.gasp";
		ProjectValue.ExportGasPlace(Game,Export,{});
		const auto File=Files.ReadFileToString(Export);
		const auto Portable=Gas::Decode(std::span(reinterpret_cast<const std::uint8_t*>(File.data()),File.size())).value();
		Require(Gas::GetFormatVersion(Portable)==4 && Portable.GridSegments==Snapshot.GridSegments,"GASP lacks the independent grid closure");
		Reopened=ProjectValue.LoadGasPlace(Portable);
		CheckWorld(Reopened,Expected,Id);
		Reopened->Destroy();
		const std::array<std::shared_ptr<Instance>,1> Roots{Grid};
		const auto Model=InstanceSerialization::CaptureGasDocument(Roots,Gas::DocumentKind::Model).value();
		const auto Materialized=InstanceSerialization::MaterializeGasDocument(Gas::Decode(Gas::Encode(Model).value()).value(),false);
		Require(Materialized.Succeeded() && blockgrid::Encode(std::dynamic_pointer_cast<BlockGrid>(Materialized.Roots.front())->GetField())==Expected,
			"Portable GASM lost the authored native field");
		Materialized.Roots.front()->Destroy();
		ProjectValue.PersistGameAtomically(Changed);
		Reopened=ProjectValue.DeserializeGame();
		Require(Reopened->GetName()=="StagedReplacement","Retry did not repair missing artifacts and publish the new root");
		CheckWorld(Reopened,Expected,Id);
		Reopened->Destroy();

		const auto Current=Game->GetAuthoritativeRevision();
		const auto Trusted=PackageBuilder::Capture(ProjectValue,Game,Current,Current);
		Require(Trusted.GridSegments==Snapshot.GridSegments,"Trusted package capture lost field bytes");
		Files.WriteStringToFile(Origin/".gargantuan"/"package-audience.json",R"({"Version":1,"SharedAssets":[]})");
		const auto Client=PackageBuilder::Capture(ProjectValue,Game,Current,Current,PackageAudience::DistributedPlayer,
			RuntimeDistributionRequest{.Target=RuntimeDistributionTarget::DistributedPlayer});
		Require(Client.GridSegments.empty() && Client.ProjectJson.empty() && Client.DistributedPlayer,"Distributed Player received private grid data");
		Game->Destroy();
		CheckPackage(Runtime,Temporary.Root/"TrustedPackage",Trusted,Expected,Id);
		std::cout<<"[BlockGrid:Persistence] "<<Expected.size()<<" bytes, "<<Snapshot.GridSegments.size()
			<<" native artifacts, "<<Snapshot.Contents.size()<<" project JSON bytes. Clone, Play, disk, GASM, GASP, package and failure checks passed.\n";
	}
}
int main(int Count,char** Arguments) {
	try {
		Require(Count==2,"Expected one trusted runtime distribution directory");
		Require(SDL_Init(0),SDL_GetError());
		gargantuan::BootstrapNativeRuntimeSchema();
		Run(std::filesystem::absolute(Arguments[1]));
		SDL_Quit();
		return 0;
	} catch(const std::exception& Error) {
		std::cerr<<"[BlockGrid:Persistence] "<<Error.what()<<'\n';
		SDL_Quit();
		return 1;
	}
}
