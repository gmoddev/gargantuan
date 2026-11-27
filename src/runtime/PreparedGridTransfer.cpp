// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "PreparedGridTransfer.hpp"
#include "gargantuan/blockgrid/Assets.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "gargantuan/classes/BlockGrid.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/SpatialRegion.hpp"
#include "gargantuan/classes/WorldRoot.hpp"
#include "gargantuan/network/SpatialTransferProtection.hpp"
#include "gargantuan/render/RenderDirtyAccumulator.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <stdexcept>

namespace gargantuan {
	namespace {
		using namespace blockgrid;
		std::atomic_size_t ParkedShells{};
		GridTransferConfiguration Configuration(const BlockGrid& G) {
			const auto R=G.GetSpatialRegion().value_or(nullptr);
			return {G.GetCFrame(),G.GetCellSize(),G.GetVisible(),G.GetCanCollide(),G.GetCanQuery(),R?R->GetObjectId():ObjectId{}};
		}
		std::shared_ptr<BlockGrid> FindGrid(ObjectId Id) {
			auto Value=std::dynamic_pointer_cast<BlockGrid>(ObjectRegistry::Get().Lookup(Id));
			if (!Value || Value->GetDestroyed() || Value->IsDestroying())
				throw std::domain_error("[BlockGrid:Conflict] Transfer grid identity is stale");
			return Value;
		}
		GridLattice Lattice(const BlockGrid& Source,const BlockGrid& Destination) {
			if (Source.GetCellSize()!=Destination.GetCellSize())
				throw std::invalid_argument("[BlockGrid:Alignment] Cell sizes differ");
			if (Source.GetField().GetOccupiedCount()>MaximumTransferCells)
				throw std::length_error("[BlockGrid:Limit] Merge exceeds its cell budget");
			const auto S=Source.GetCFrame(),D=Destination.GetCFrame();
			const auto Inverse=glm::inverse(glm::dmat3(D.Rotation));
			const auto Relative=Inverse*glm::dmat3(S.Rotation);
			const auto Origin=Inverse*(glm::dvec3(S.Position)-glm::dvec3(D.Position))/double(Source.GetCellSize());
			const auto Aligned=glm::round(Origin);
			if (!glm::all(glm::lessThanEqual(glm::abs(Origin-Aligned),glm::dvec3(1e-4))) ||
				!glm::all(glm::lessThanEqual(glm::abs(Aligned),glm::dvec3(2*CoordinateLimit))))
				throw std::invalid_argument("[BlockGrid:Alignment] Origins must differ by integer grid coordinates");
			for (std::uint8_t R=0;R<24;++R) {
				const auto& B=Basis(R);
				glm::dmat3 Delta=Relative;
				bool Cardinal=true;
				for (unsigned I=0;I<3;++I) for (unsigned J=0;J<3;++J) {
					Delta[I][J]-=B[I][J];
					Cardinal &= std::abs(Delta[I][J])<=1e-5;
				}
				if (!Cardinal) continue;
				Source.GetField().ForEachOccupied([&](Cell P,Block) {
					const auto Residual=(Origin-Aligned)+Delta*(glm::dvec3(P.X,P.Y,P.Z)+.5);
					if (!glm::all(glm::lessThanEqual(glm::abs(Residual),glm::dvec3(1e-4))))
						throw std::invalid_argument("[BlockGrid:Alignment] Occupied cells do not coincide on this lattice");
				});
				return {{static_cast<int>(Aligned.x),static_cast<int>(Aligned.y),static_cast<int>(Aligned.z)},R};
			}
			throw std::invalid_argument("[BlockGrid:Alignment] Grid rotation must preserve the cell lattice");
		}
		void CheckOwner(const std::shared_ptr<BlockGrid>& Grid,DataModel& World) {
			if (Grid->GetDestroyed() || Grid->IsDestroying() || Grid->GetDataModel().get()!=&World ||
				Grid->IsReplicaMaterialization()) throw std::domain_error("[BlockGrid:Access] Grids must be authoritative in one DataModel");
			if (Grid->IsDynamicAssembly()) throw std::domain_error("[BlockGrid:Motion] Freeze the assembly before transferring cells");
			if (!Grid->EnsureAssetBindings()) throw std::domain_error("[BlockGrid:Unavailable] Project asset pins are unavailable");
		}
		void CheckLandingReservation(DataModel& World,const BlockGrid& Grid) {
			const auto Hold=World.GetSpatialTransferProtection();
			if (Hold && !Hold->AllowsResource(Grid))
				throw std::domain_error("[BlockGrid:Admission] Collision is reserved for an active spatial transfer");
		}
	}
	std::size_t GridTransferHistory::Charge() const noexcept {
		return sizeof(*this)+Before.Source.GetSnapshotBytes()+Before.Destination.GetSnapshotBytes()+
			After.Source.GetSnapshotBytes()+After.Destination.GetSnapshotBytes();
	}
	GridTransferHistory::~GridTransferHistory() { if (Parked) --ParkedShells; }
	bool BlockGrid::HasParkedTransfers() noexcept { return ParkedShells.load(std::memory_order_relaxed)!=0; }
	std::shared_ptr<const BlockGridTransferStamp> BlockGrid::GetLastTransfer() const {
		if (!LastTransfer) return {};
		const auto Index=LastTransfer->Grids[0]==GetObjectId()?0u:1u;
		return LastTransfer->PublicRevisions[Index]==GetPublicBlockRevision()?LastTransfer:nullptr;
	}
	MutationResult PreparedGridTransfer::Apply(const std::shared_ptr<BlockGrid>& Grid,const blockgrid::EditRequest& Request,
		const MutationAuthorityContext& Authority) {
		const auto World=Grid->GetDataModel();
		const auto& Op=*Request.Bilateral;
		if (Grid->GetBlockRevision()!=*Request.ExpectedRevision)
			throw std::domain_error("[BlockGrid:Conflict] Grid revision changed");
		if (World->Transactions.GetOpenCount() || World->HasDeferredRevision() || Grid->IsTransferParked())
			throw std::domain_error("[BlockGrid:Admission] Transfer requires an available standalone transaction");
		World->EnsureAuthoritativeRevisionAvailable();
		const auto Parent=Grid->GetParent().value_or(nullptr);
		const auto Domain=WorldRoot::GetEffectiveWorld(*Grid);
		if (!Parent || !Domain || Parent->GetDestroyed() || Parent->IsDestroying())
			throw std::domain_error("[BlockGrid:Admission] A live sibling parent and collision domain are required");
		CheckLandingReservation(*World,*Grid);
		CheckOwner(Grid,*World);
		std::shared_ptr<BlockGrid> Source,Destination;
		blockgrid::GridLattice Mapping;
		if (Op.Extract) {
			if (Op.Source.IsValid() || !Op.SourceRevision.empty()) throw std::invalid_argument("[BlockGrid:Transfer] Mixed extraction arguments");
			(void)blockgrid::Volume(*Op.Extract,blockgrid::MaximumTransferCells);
			const auto Low=Op.Extract->Min;
			Mapping={{-Low.X,-Low.Y,-Low.Z},0};
			Source=Grid;
			ScopedChangeJournalSuppression Suppress;
			Destination=std::make_shared<BlockGrid>();
			Destination->SetName("ExtractedGrid");
			Destination->SetCellSize(Source->GetCellSize());
			Destination->SetCFrame(Source->GetCFrame()*CFrame(glm::vec3(Low.X,Low.Y,Low.Z)*Source->GetCellSize()));
			Destination->SetVisible(Source->GetVisible()); Destination->SetCanCollide(Source->GetCanCollide());
			Destination->SetCanQuery(Source->GetCanQuery()); Destination->SetSpatialRegion(Source->GetSpatialRegion());
		} else {
			Destination=Grid; Source=FindGrid(Op.Source);
			if (Source==Destination || Source->GetParent().value_or(nullptr)!=Parent ||
				WorldRoot::GetEffectiveWorld(*Source)!=Domain || Source->IsTransferParked())
				throw std::domain_error("[BlockGrid:Admission] Merge requires distinct siblings in the same collision domain");
			CheckLandingReservation(*World,*Source);
			CheckOwner(Source,*World);
			if (Source->GetBlockRevision()!=Op.SourceRevision) throw std::domain_error("[BlockGrid:Conflict] Source revision changed");
			Mapping=Lattice(*Source,*Destination);
		}
		if (Source->Editing || Destination->Editing) throw std::domain_error("[BlockGrid:Admission] A transfer participant is busy");
		CheckLandingReservation(*World,*Source); CheckLandingReservation(*World,*Destination);
		auto Next=blockgrid::PlanGridTransfer(Source->GetField(),Destination->GetField(),Op.Extract,Mapping);
		std::shared_ptr<GridTransferHistory> Receipt;
		std::optional<AuthoritativeTransactionHistory> History;
		if (Authority.GetOrigin()==MutationCommandOrigin::Studio) {
			const auto Charge=Source->Cells.GetSnapshotBytes()+Destination->Cells.GetSnapshotBytes()+
				Next.Source.GetSnapshotBytes()+Next.Destination.GetSnapshotBytes()+sizeof(GridTransferHistory)+sizeof(BlockGridTransactionChange);
			if (Charge>MaximumTransactionSemanticBytes || Charge>World->Transactions.RetainedByteLimit)
				throw std::length_error("[BlockGrid:Limit] Transfer exceeds the Undo history budget");
			Receipt=std::make_shared<GridTransferHistory>();
			Receipt->Source=Source->GetObjectId(); Receipt->Destination=Destination->GetObjectId();
			Receipt->Parent=Parent->GetObjectId(); Receipt->Domain=Domain->GetObjectId(); Receipt->Extract=Op.Extract.has_value();
			Receipt->SourceConfig=Configuration(*Source); Receipt->DestinationConfig=Configuration(*Destination);
			Receipt->Before={Source->Cells,Destination->Cells}; Receipt->After=Next;
			CommittedTransaction Action{.Id=AuthoritativeTransactionHistory::AllocatePreparedIdentity(),
				.Label=Op.Extract?"Extract BlockGrid":"Merge BlockGrids",.Origin=TransactionOrigin::Studio,
				.StartingRevision=World->GetAuthoritativeRevision(),.ResultingRevision=World->GetAuthoritativeRevision()+1};
			if (!Action.Id.IsValid()) throw std::overflow_error("[BlockGrid:Limit] Authoring identity exhausted");
			Action.Changes.emplace_back(BlockGridTransactionChange{Grid->GetObjectId(),{},Receipt});
			Action.SemanticBytes=Charge; Action.ReplayPolicy=TransactionReplayPolicy::PreparedGridTransfer;
			Action.Author=Authority.GetAuthor(); History.emplace(World->Transactions); History->Retain(std::move(Action));
		}
		return Install(*World,Domain,Parent,Source,Destination,std::move(Next),Op.Extract?Hierarchy::Attach:Hierarchy::Keep,
			Receipt,std::move(History));
	}
	MutationResult PreparedGridTransfer::Install(DataModel& World,const std::shared_ptr<WorldRoot>& Domain,
		const std::shared_ptr<Instance>& Parent,const std::shared_ptr<BlockGrid>& Source,const std::shared_ptr<BlockGrid>& Destination,
		blockgrid::TransferFields Next,Hierarchy Change,const std::shared_ptr<const GridTransferHistory>& Receipt,
		std::optional<AuthoritativeTransactionHistory> History) {
		World.EnsureAuthoritativeRevisionAvailable();
		CheckLandingReservation(World,*Source); CheckLandingReservation(World,*Destination);
		const auto Starting=World.GetAuthoritativeRevision();
		const auto OldParent=Destination->GetParent().value_or(nullptr);
		const bool FirstAdoption=!Destination->GetDataModel();
		const auto OldIndex=OldParent?std::ranges::find(OldParent->Children,Destination)-OldParent->Children.begin():0;
		const auto OldParking=Destination->ParkedBy;
		std::vector<std::shared_ptr<Instance>> Deferred;
		std::vector<BufferedChangeRecord> Records;
		MutationResult Result;
		bool Changed=false,Registered=false;
		{
			ScopedAuthoritativeRevisionDeferral Defer(World,Changed);
			try {
				if (Change!=Hierarchy::Keep) {
					ScopedChangeJournalCapture Capture;
					Destination->ParkedBy.reset();
					Destination->SetParentInternal(Change==Hierarchy::Attach?Parent:nullptr,&Deferred);
					Records=Capture.Take();
					if (!Changed || Deferred.size()!=1) throw std::domain_error("[BlockGrid:Conflict] Extracted hierarchy changed");
				}
				if (Change==Hierarchy::Attach) {
					if (WorldRoot::GetEffectiveWorld(*Destination)!=Domain)
						throw std::domain_error("[BlockGrid:Admission] Extracted grid cannot join the source domain");
					Registered=true; Domain->AddBlockGrid(Destination);
				}
				auto Coverage=Domain->PrepareBlockGridTransfer(*Source,Next.Source,*Destination,Next.Destination);
				auto Stamp=std::make_shared<const BlockGridTransferStamp>(BlockGridTransferStamp{
					World.GetObjectId(),Starting+1,{Source->GetObjectId(),Destination->GetObjectId()},
					{Source->RevisionToken(Next.Source.GetPublicRevision())+"/peers",Destination->RevisionToken(Next.Destination.GetPublicRevision())+"/peers"}});
				auto S=Source->RevisionToken(Next.Source.GetRevision()),D=Destination->RevisionToken(Next.Destination.GetRevision());
				Records.push_back({{},Source->GetObjectId(),PropertyUpdatedChange{"BlockRevision",std::move(S),false}});
				Records.push_back({{},Destination->GetObjectId(),PropertyUpdatedChange{"BlockRevision",D,false}});
				Result={.Status=MutationStatus::Success,.Object=Destination->GetObjectId(),.BlockRevision=std::move(D)};
				if (World.GetAuthoritativeRevision()!=Starting || World.Transactions.GetOpenCount())
					throw std::domain_error("[BlockGrid:Conflict] World changed during transfer preparation");
				CheckLandingReservation(World,*Source); CheckLandingReservation(World,*Destination);
				ChangeJournal::PreparedJournalBatch Journal(ChangeJournal::Get(),Records);
				static_assert(std::is_nothrow_move_assignable_v<blockgrid::Field>);
				Source->Cells=std::move(Next.Source); Destination->Cells=std::move(Next.Destination);
				Source->AssetSequence=0; Destination->AssetSequence=0;
				Source->LastTransfer=Stamp; Destination->LastTransfer=std::move(Stamp);
				Coverage();
				if (Receipt && Change==Hierarchy::Detach) { Destination->ParkedBy=Receipt; Receipt->Parked=Destination; ++ParkedShells; }
				if (Receipt && Change==Hierarchy::Attach && Receipt->Parked) { Receipt->Parked.reset(); --ParkedShells; }
				if (History) World.Transactions.InstallPrepared(*History);
				Journal.Install(); ++World.AuthoritativeRevision; Journal.Release();
			} catch (...) {
				// This Main-only hierarchy stage has never fired observers. Restore the same sibling position and ownership.
				if (Registered) Domain->RemoveBlockGrid(Destination);
				if (Destination->ParentReference.lock()!=OldParent) {
					if (const auto NewParent=Destination->ParentReference.lock()) std::erase(NewParent->Children,Destination);
					if (OldParent) OldParent->Children.insert(OldParent->Children.begin()+OldIndex,Destination);
					Destination->ParentReference=OldParent;
				}
				if (FirstAdoption && Destination->OwningDataModel.lock()) { Destination->OwningDataModel.reset(); World.ReleaseInstance(); }
				Destination->ParkedBy=OldParking;
				throw;
			}
		}
		if (Change==Hierarchy::Detach) Domain->RemoveBlockGrid(Destination);
		for (const auto& Record:Records) {
			try { RenderDirtyAccumulator::Get().RecordChange(Record.Scope,Record.Object,Record.Payload); } catch (...) {}
		}
		if (Change!=Hierarchy::Keep) {
			try { Destination->NotifyParentChanged(Destination,OldParent,Change==Hierarchy::Attach?Parent:nullptr,Deferred); } catch (...) {}
		}
		for (const auto& Grid:{Source,Destination}) {
			const auto Signal=Grid->PropertyChangedSignals.find("BlockRevision");
			if (Signal!=Grid->PropertyChangedSignals.end() && Signal->second) {
				auto Value=Signal->second; try { Value->Fire({}); } catch (...) {}
			}
		}
		return Result;
	}
	TransactionResult PreparedGridTransfer::Replay(DataModel& World,const ScriptSecurityContext& Security,bool Redo) {
		if ((Security.Domain!=ScriptExecutionDomain::Studio && Security.Domain!=ScriptExecutionDomain::Core) ||
			!Security.HasCapability(ScriptCapability::EditorCommands) || !Security.HasCapability(ScriptCapability::MutateDataModel))
			return {.Status=TransactionStatus::InvalidState,.Message="[BlockGrid:Access] Authoring history permission required"};
		try {
			auto History=World.Transactions;
			const auto* Action=Redo?History.GetRedoTransaction():History.GetUndoTransaction();
			if (!Action || Action->ReplayPolicy!=TransactionReplayPolicy::PreparedGridTransfer || Action->Changes.size()!=1 ||
				History.GetOpenCount() || World.HasDeferredRevision()) throw std::domain_error("[BlockGrid:History] Transfer history is unavailable");
			const auto R=std::get<BlockGridTransactionChange>(Action->Changes.front()).Transfer;
			if (!R) throw std::logic_error("[BlockGrid:History] Transfer inverse is unavailable");
			const auto Source=FindGrid(History.ResolveIdentity(R->Source)),Destination=FindGrid(History.ResolveIdentity(R->Destination));
			const auto Parent=ObjectRegistry::Get().Lookup(History.ResolveIdentity(R->Parent));
			const auto Domain=std::dynamic_pointer_cast<WorldRoot>(ObjectRegistry::Get().Lookup(History.ResolveIdentity(R->Domain)));
			const auto Change=!R->Extract?Hierarchy::Keep:Redo?Hierarchy::Attach:Hierarchy::Detach;
			CheckLandingReservation(World,*Source); CheckLandingReservation(World,*Destination);
			CheckOwner(Source,World); CheckOwner(Destination,World);
			if (!Parent || !Domain || Source->GetParent().value_or(nullptr)!=Parent || WorldRoot::GetEffectiveWorld(*Source)!=Domain ||
				Source==Destination || Source->Editing || Destination->Editing || Source->IsTransferParked() ||
				Configuration(*Source)!=R->SourceConfig || Configuration(*Destination)!=R->DestinationConfig ||
				(Change==Hierarchy::Attach ? Destination->GetParent().has_value() || R->Parked!=Destination || Destination->ParkedBy.lock()!=R :
					Destination->GetParent().value_or(nullptr)!=Parent || WorldRoot::GetEffectiveWorld(*Destination)!=Domain || Destination->IsTransferParked()) ||
				(R->Extract && !Destination->Children.empty()))
				throw std::domain_error("[BlockGrid:Conflict] Transfer hierarchy, configuration or dependents have diverged");
			const auto& Expected=Redo?R->Before:R->After;
			if (!Source->Cells.HasSameContents(Expected.Source) || !Destination->Cells.HasSameContents(Expected.Destination))
				throw std::domain_error("[BlockGrid:Conflict] Transfer data has diverged");
			auto Next=Redo?R->After:R->Before;
			if (Next.Source.HasAssetBindings() || Next.Destination.HasAssetBindings()) {
				auto Assets=std::dynamic_pointer_cast<AssetService>(World.GetService("AssetService"));
				if (!Assets) throw std::domain_error("[BlockGrid:Unavailable] Project asset catalog is unavailable");
				const auto Resolve=[&](const blockgrid::AssetBinding& A){return blockgrid::ResolveBlockAssets(*Assets,A);};
				Next.Source.HydrateAssets(Resolve); Next.Destination.HydrateAssets(Resolve);
			}
			Next.Source.AdvanceTransferRevision(Source->Cells); Next.Destination.AdvanceTransferRevision(Destination->Cells);
			if (Redo) ++History.Cursor; else --History.Cursor;
			const auto Id=Action->Id; const auto Start=World.GetAuthoritativeRevision();
			(void)Install(World,Domain,Parent,Source,Destination,std::move(Next),Change,R,std::move(History));
			return {.Status=TransactionStatus::Success,.Id=Id,.StartingRevision=Start,.ResultingRevision=Start+1,.ChangeCount=1};
		} catch (const std::exception& E) { return {.Status=TransactionStatus::ExecutionFailed,.Message=E.what()}; }
	}
}
