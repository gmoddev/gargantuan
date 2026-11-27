// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/blockgrid/Assets.hpp"
#include "gargantuan/classes/BlockGrid.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"
#include "gargantuan/physics/PhysicsTypes.hpp"
#include "gargantuan/services/AssetService.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>

namespace gargantuan::blockgrid {
	namespace {
		[[noreturn]] void Invalid(const char* Reason) { throw std::invalid_argument(std::string("[BlockGrid:Asset] ")+Reason); }
		using Records=std::map<AssetId,AssetRecord>;
		const AssetRecord& Collect(AssetService& Service,Records& Graph,std::set<AssetId>& Visiting,AssetId Id) {
			if (Visiting.contains(Id)) Invalid("Cyclic asset dependency");
			if (const auto It=Graph.find(Id);It!=Graph.end()) return It->second;
			if (Graph.size()>=MaximumAssetPins) Invalid("Asset dependency budget exhausted");
			auto Record=Service.GetAsset(AssetReference::FromAssetId(Id).Value);
			if (!Record || !Record->Asset || Record->State!=AssetState::Ready || !Record->ContentId.IsValid() || Record->BuiltIn ||
				(Record->Kind!=AssetKind::Mesh && Record->Kind!=AssetKind::Collision && Record->Kind!=AssetKind::Image && Record->Kind!=AssetKind::Material))
				Invalid("Expected a ready project mesh, collision, material or image");
			const auto& Stored=Graph.emplace(Id,std::move(*Record)).first->second;
			if (Stored.Dependencies.size()>MaximumAssetPins) Invalid("Too many direct dependencies");
			Visiting.insert(Id);
			for (auto Child:Stored.Dependencies) Collect(Service,Graph,Visiting,Child);
			Visiting.erase(Id);
			return Stored;
		}
		template<class V> bool Finite(const V& X) {
			for (int I=0;I<X.length();++I) if (!std::isfinite(X[I])) return false;
			return true;
		}
		std::shared_ptr<const std::vector<glm::vec3>> VisualPositions(const ImportedMesh& Mesh) {
			// The physics backend keys immutable resources by ID/revision and also verifies buffer identity.
			// Query-only uses across grids must therefore retain the same conversion of a live visual resource.
			using Source=std::weak_ptr<const std::vector<RenderVertex>>;
			static std::mutex Lock;
			static std::map<Source,std::weak_ptr<const std::vector<glm::vec3>>,std::owner_less<>> Cache;
			std::scoped_lock Guard(Lock);
			const auto It=Cache.find(Mesh.Vertices);
			if (It!=Cache.end()) if (const auto Live=It->second.lock()) return Live;
			std::erase_if(Cache,[](const auto& Pair){return Pair.second.expired();});
			if (Cache.size()>=MaximumPhysicsCollisionResources) Invalid("Visual query resource budget exhausted");
			auto Positions=std::make_shared<std::vector<glm::vec3>>();
			Positions->reserve(Mesh.Vertices->size());
			for (const auto& P:*Mesh.Vertices) Positions->push_back(P.Position);
			Cache[Mesh.Vertices]=Positions;
			return Positions;
		}
		void CheckMesh(const std::shared_ptr<const std::vector<glm::vec3>>& Vertices,
			const std::shared_ptr<const std::vector<std::uint32_t>>& Indices) {
			if (!Vertices || !Indices || Vertices->size()<3 || Vertices->size()>MaximumMeshVertices || Indices->empty() ||
				Indices->size()%3 || Indices->size()>3*MaximumMeshTriangles) Invalid("Mesh exceeds block geometry bounds");
			for (const auto& P:*Vertices) if (!Finite(P) || glm::any(glm::greaterThan(glm::abs(P),glm::vec3(.5f))))
				Invalid("Mesh vertices must lie within the centered unit cell");
			for (std::size_t I=0;I<Indices->size();I+=3) {
				const auto A=(*Indices)[I], B=(*Indices)[I+1], C=(*Indices)[I+2];
				if (A>=Vertices->size() || B>=Vertices->size() || C>=Vertices->size()) Invalid("Invalid mesh index");
				const auto Cross=glm::cross((*Vertices)[B]-(*Vertices)[A],(*Vertices)[C]-(*Vertices)[A]);
				// All supported cell sizes, including 0.5, must meet the backend's scaled area contract.
				if (glm::dot(Cross,Cross)<1.6e-9f) Invalid("Degenerate or undersized block triangle");
			}
		}
		RenderMaterialState MaterialView(AssetService& Service,const Records& Graph,AssetId Id) {
			const auto It=Graph.find(Id);
			if (It==Graph.end() || It->second.Kind!=AssetKind::Material) Invalid("Undeclared mesh material");
			const auto Material=Service.ResolveMaterial(It->second.Reference.Value);
			if (!Material || Material->ContentRevision!=It->second.ContentRevision) Invalid("Material unavailable");
			const auto Image=[&](const std::optional<AssetId>& Source,const std::optional<RenderTextureIdentity>& Target) {
				if (!Source) return;
				const auto Child=Graph.find(*Source);
				if (Child==Graph.end() || Child->second.Kind!=AssetKind::Image || !Target) Invalid("Material texture unavailable");
			};
			const auto& M=Material->Value;
			const auto& R=Material->RenderState;
			Image(M.BaseColorTexture,R.BaseColorTexture); Image(M.NormalTexture,R.NormalTexture);
			Image(M.MetallicRoughnessTexture,R.MetallicRoughnessTexture); Image(M.EmissiveTexture,R.EmissiveTexture);
			Image(M.OcclusionTexture,R.OcclusionTexture);
			return R;
		}
	}
	std::shared_ptr<const AssetBinding> ResolveBlockAssets(AssetService& Service,const AssetBinding& Requested) {
		const auto Sequence=Service.ReadChanges(0).NextSequence;
		Records Graph;
		std::set<AssetId> Visiting;
		const auto Role=[&](const std::string& Text,AssetKind Kind)->const AssetRecord* {
			if (Text.empty()) return nullptr;
			const auto Id=AssetId::Parse(Text);
			if (!Id) Invalid("Expected a canonical project asset ID");
			const auto& R=Collect(Service,Graph,Visiting,*Id);
			if (R.Kind!=Kind) Invalid("Project asset has the wrong role");
			return &R;
		};
		const auto* Mesh=Role(Requested.Mesh,AssetKind::Mesh);
		const auto* Material=Role(Requested.Material,AssetKind::Material);
		const auto* Collision=Role(Requested.Collision,AssetKind::Collision);
		if ((!Mesh && !Material) || (Collision && !Mesh)) Invalid("Missing primary resource");
		auto Result=std::make_shared<AssetBinding>();
		Result->Mesh=Requested.Mesh; Result->Material=Requested.Material; Result->Collision=Requested.Collision;
		for (const auto& [Id,R]:Graph) Result->Pins.push_back({Id.ToString(),R.ContentId.ToString(),static_cast<std::uint8_t>(R.Kind)});
		if (!Requested.Pins.empty() && Requested.Pins!=Result->Pins) Invalid("Project content differs from the pinned dependency closure");
		auto View=std::make_shared<ResolvedBlockAssets>();
		const auto Opaque=[](const RenderMaterialState& M){return M.OpacityMode==RenderOpacityMode::Opaque && M.BaseColorFactor.a>=1.f;};
		if (Material) { View->Material=MaterialView(Service,Graph,Material->Id); View->FullyOpaque=Opaque(*View->Material); }
		if (Mesh) {
			const auto* V=std::get_if<ImportedMesh>(Mesh->Asset.get());
			if (!V || V->Skeleton || V->SkinInfluences || !V->Vertices || V->Vertices->size()>MaximumMeshVertices ||
				V->SubmeshCount>MaximumMeshPrimitives) Invalid("Expected a bounded static mesh");
			auto Positions=VisualPositions(*V);
			for (const auto& P:*V->Vertices) {
				if (!Finite(P.Normal) || !Finite(P.Tangent) || !Finite(P.TextureCoordinate)) Invalid("Nonfinite vertex attribute");
				if (P.Tint[3]!=255) View->FullyOpaque=false;
			}
			CheckMesh(Positions,V->Indices);
			auto Slots=std::make_shared<std::vector<RenderPrimitiveMaterialState>>();
			if (V->Primitives && !V->Primitives->empty()) {
				if (V->Primitives->size()!=V->SubmeshCount || V->Primitives->size()>MaximumMeshPrimitives) Invalid("Invalid submesh count");
				std::size_t End=0;
				for (const auto& P:*V->Primitives) {
					if (P.FirstIndex!=End || !P.IndexCount || P.IndexCount%3 || P.IndexCount>V->Indices->size()-End)
						Invalid("Mesh slots must partition complete triangles");
					auto Slot=View->Material?*View->Material:P.Material?MaterialView(Service,Graph,*P.Material):RenderMaterialState{};
					View->FullyOpaque=View->FullyOpaque && Opaque(Slot);
					Slots->push_back({P.FirstIndex,P.IndexCount,std::move(Slot)}); End+=P.IndexCount;
				}
				if (End!=V->Indices->size()) Invalid("Mesh slots do not cover every index");
			} else {
				if (V->SubmeshCount!=1) Invalid("Missing material slot ranges");
				Slots->push_back({0,static_cast<std::uint32_t>(V->Indices->size()),View->Material.value_or(RenderMaterialState{})});
			}
			View->VisualMesh=*V;
			View->Primitives=std::move(Slots);
			PhysicsTriangleMesh Query;
			if (Collision) {
				if (V->CollisionAsset!=Collision->Id) Invalid("Collision must be the selected mesh's explicit companion");
				const auto* C=std::get_if<ImportedCollision>(Collision->Asset.get());
				if (!C) Invalid("Invalid collision resource");
				CheckMesh(C->Positions,C->Indices);
				Query={Collision->Reference.Value,Collision->ContentRevision,C->Positions,C->Indices};
			} else {
				Query={Mesh->Reference.Value,Mesh->ContentRevision,std::move(Positions),V->Indices};
			}
			View->QueryMesh=std::make_shared<const PhysicsTriangleMesh>(std::move(Query));
			View->RetainedBytes=sizeof(PhysicsTriangleMesh)+V->Vertices->size()*sizeof(RenderVertex)+V->Indices->size()*sizeof(std::uint32_t)+
				View->Primitives->size()*(sizeof(RenderPrimitiveMaterialState)+sizeof(ImportedMeshPrimitive))+
				View->QueryMesh->Positions->size()*sizeof(glm::vec3)+View->QueryMesh->Indices->size()*sizeof(std::uint32_t);
		}
		View->RetainedBytes+=sizeof(ResolvedBlockAssets);
		if (Service.ReadChanges(0).NextSequence!=Sequence) Invalid("Project asset catalog changed during validation");
		Result->Resolved=std::move(View);
		return Result;
	}
}

namespace gargantuan {
	blockgrid::Definition BlockGrid::ResolveMaterialAlias(std::string_view Key) const {
		if (!Key.starts_with("material:") || !AssetId::Parse(Key.substr(9)))
			throw std::invalid_argument("[BlockGrid:Definition] Unknown definition or invalid material ID");
		blockgrid::Definition D{std::string(Key)};
		auto Binding=std::make_shared<blockgrid::AssetBinding>();
		Binding->Material=Key.substr(9);
		D.Assets=std::move(Binding);
		return ResolveAssetDefinition(std::move(D));
	}
	std::shared_ptr<const blockgrid::Patch> BlockGrid::PlanMaterialEdit(const blockgrid::EditRequest& Edit) const {
		using namespace blockgrid;
		if (Edit.Materials.empty() || Edit.Materials.size()>MaximumNewMaterials || (Edit.Writes.empty()==Edit.Commands.empty()))
			throw std::invalid_argument("[BlockGrid:Material] Invalid number of bindings or placement commands");
		Field Staged=Cells;
		std::vector<blockgrid::Definition> Resolved;
		for (const auto& Candidate:Edit.Materials) {
			if (!Candidate.Key.starts_with("material:") || Staged.FindDefinitionId(Candidate.Key))
				throw std::domain_error("[BlockGrid:Conflict] Material binding already exists");
			auto D=ResolveAssetDefinition(Candidate);
			(void)Staged.Define(D); Resolved.push_back(std::move(D));
		}
		const auto First=static_cast<std::uint16_t>(Brick+1+Cells.GetDefinitionCount());
		auto Out=std::make_shared<Patch>();
		if (!Edit.Commands.empty()) Out->StateCells=Staged.PlanOperations(Edit.Commands);
		else Out->StateCells=Staged.PlanOperations(std::vector<Operation>(Edit.Writes.begin(),Edit.Writes.end()));
		// An edit may place an unseen material then clear or replace it. Keep only aliases used in final cells.
		std::vector<std::uint16_t> Remap(Resolved.size());
		for (const auto& C:Out->StateCells) if (C.After.Value.Definition>=First) Remap.at(C.After.Value.Definition-First)=1;
		for (std::size_t I=0;I<Remap.size();++I) if (Remap[I]) {
			Remap[I]=static_cast<std::uint16_t>(First+Out->Materials.size());
			Out->Materials.push_back(std::move(Resolved[I]));
		}
		for (auto& C:Out->StateCells) if (C.After.Value.Definition>=First) C.After.Value.Definition=Remap[C.After.Value.Definition-First];
		return Out;
	}
	blockgrid::Definition BlockGrid::ResolveAssetDefinition(blockgrid::Definition Spec) const {
		AssertAuthoritativeMutation("BlockGrid definition validation");
		if (Spec.Assets) {
			const auto World=GetDataModel();
			const auto Service=World?std::dynamic_pointer_cast<AssetService>(World->GetService("AssetService")):nullptr;
			if (!Service) throw std::invalid_argument("[BlockGrid:Unavailable] No project asset catalog");
			Spec.Assets=blockgrid::ResolveBlockAssets(*Service,*Spec.Assets);
		}
		blockgrid::ValidateDefinition(Spec);
		return Spec;
	}
	bool BlockGrid::EnsureAssetBindings() const {
		AssertAuthoritativeMutation("BlockGrid asset validation");
		if (!Cells.HasAssetBindings()) return true;
		const auto World=GetDataModel();
		const auto Service=World?std::dynamic_pointer_cast<AssetService>(World->GetService("AssetService")):nullptr;
		if (!Service) return false;
		const bool Same=LastAssetService.lock()==Service;
		const auto Seq=Service->ReadChanges(0).NextSequence;
		if (Same && AssetSequence==Seq) return AssetsReady;
		const auto Changes=Service->ReadChanges(Same?AssetSequence:0);
		const bool All=!Same || !AssetsReady || Changes.RescanRequired;
		std::set<std::string> Changed;
		if (!All) for (const auto& Change:Changes.Changes) Changed.insert(Change.Reference);
		try {
			const auto Previous=Cells.GetGeometryRevision();
			Cells.HydrateAssets([&](const blockgrid::AssetBinding& Binding)->std::shared_ptr<const blockgrid::AssetBinding> {
				if (!All && Binding.Resolved && !std::ranges::any_of(Binding.Pins,[&](const auto& P){return Changed.contains("asset://"+P.Id);}))
					return {};
				return blockgrid::ResolveBlockAssets(*Service,Binding);
			});
			if (Previous!=Cells.GetGeometryRevision()) ++AssetPublicationRevision;
			AssetsReady=true;
		} catch (const std::invalid_argument&) { AssetsReady=false; }
		catch (const std::length_error&) { AssetsReady=false; }
		LastAssetService=Service;
		AssetSequence=Changes.NextSequence;
		return AssetsReady;
	}
}
