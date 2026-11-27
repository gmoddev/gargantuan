// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/blockgrid/Field.hpp"
#include "gargantuan/blockgrid/Assets.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace gargantuan::blockgrid {
	namespace {
		const std::array<Definition,3> Builtins{{
			{"",{},{},false,false,0},
			{"Stone",Shape::Cube,{139/255.f,144/255.f,151/255.f}},
			{"Brick",Shape::Cube,{150/255.f,57/255.f,37/255.f}}
		}};
		bool CanonicalHex(std::string_view Text,std::size_t Size) {
			return Text.size()==Size && std::ranges::all_of(Text,[](char C){return (C>='0' && C<='9') || (C>='a' && C<='f');}) &&
				std::ranges::any_of(Text,[](char C){return C!='0';});
		}
		void ValidatePins(const AssetBinding& A) {
			if (A.Pins.empty() || A.Pins.size()>MaximumAssetPins || (A.Material.empty() && A.Mesh.empty()))
				throw std::invalid_argument("[BlockGrid:Asset] Invalid resource closure");
			std::string_view Last;
			for (const auto& Pin:A.Pins) {
				const auto Kind=static_cast<AssetKind>(Pin.Kind);
				if (!CanonicalHex(Pin.Id,32) || !CanonicalHex(Pin.Digest,64) || Pin.Id<=Last ||
					(Kind!=AssetKind::Mesh && Kind!=AssetKind::Material && Kind!=AssetKind::Image && Kind!=AssetKind::Collision))
					throw std::invalid_argument("[BlockGrid:Asset] Invalid or unordered dependency");
				Last=Pin.Id;
			}
			const auto Role=[&](const std::string& Id,AssetKind Type) {
				if (Id.empty()) return;
				const auto It=std::ranges::lower_bound(A.Pins,Id,{},&ResourcePin::Id);
				if (It==A.Pins.end() || It->Id!=Id || It->Kind!=static_cast<std::uint8_t>(Type))
					throw std::invalid_argument("[BlockGrid:Asset] Asset role does not match a pinned resource");
			};
			Role(A.Material,AssetKind::Material); Role(A.Mesh,AssetKind::Mesh); Role(A.Collision,AssetKind::Collision);
		}
	}
	std::size_t AssetBindingBytes(const AssetBinding& A) noexcept {
		return 2+A.Material.size()+A.Mesh.size()+A.Collision.size()+97*A.Pins.size();
	}
	std::size_t AssetPinCharge(const Definition& D) noexcept {
		if (!D.Assets) return 0;
		return sizeof(AssetBinding)+2*AssetBindingBytes(*D.Assets)+D.Assets->Pins.size()*sizeof(ResourcePin)+
			(D.Assets->Resolved?D.Assets->Resolved->RetainedBytes:0);
	}
	void ValidateDefinition(const Definition& D) {
		constexpr std::string_view Prefix="project:";
		const auto Lower=[](char C){return C>='a' && C<='z';};
		const bool Alias=D.Key.starts_with("material:");
		if (Alias) {
			if (!D.Assets || D.Assets->Material!=std::string_view(D.Key).substr(9) || !D.Assets->Mesh.empty() ||
				D.Geometry!=Shape::Cube || D.Color!=Point{1,1,1} || !D.CanCollide || !D.Queryable || D.Ports!=63)
				throw std::invalid_argument("[BlockGrid:Asset] Material shorthand requires a plain material cube");
		} else if (!D.Key.starts_with(Prefix) || D.Key.size()<=Prefix.size() || D.Key.size()>Prefix.size()+48 ||
			!Lower(D.Key[Prefix.size()]) || !std::ranges::all_of(std::string_view(D.Key).substr(Prefix.size()),[&](char C){
				return Lower(C) || (C>='0' && C<='9') || C=='_' || C=='-';
			})) throw std::invalid_argument("[BlockGrid:Definition] Expected project: followed by a lowercase identifier");
		if (D.Geometry>Shape::Mesh || D.Ports>63 || !std::ranges::all_of(D.Color,[](float C) {
				return std::isfinite(C) && C>=0 && C<=1 && !std::signbit(C);
			})) throw std::invalid_argument("[BlockGrid:Definition] Unsupported shape, color or port mask");
		if (D.Assets) ValidatePins(*D.Assets);
		if ((D.Geometry==Shape::Mesh)!=bool(D.Assets && !D.Assets->Mesh.empty()) ||
			(D.Assets && !D.Assets->Collision.empty() && (!D.CanCollide || D.Geometry!=Shape::Mesh)) ||
			(D.Geometry==Shape::Mesh && D.CanCollide && D.Assets->Collision.empty()))
			throw std::invalid_argument("[BlockGrid:Asset] Mesh and collision representations do not match their roles");
	}
	std::string DefinitionDigest(const Definition& D) {
		std::vector<std::uint8_t> Bytes{'B','D','E','F',1,static_cast<std::uint8_t>(D.Geometry)};
		for (const auto Channel:D.Color) {
			const auto Bits=std::bit_cast<std::uint32_t>(Channel);
			for (unsigned I=0;I<4;++I) Bytes.push_back(static_cast<std::uint8_t>(Bits>>(8*I)));
		}
		Bytes.push_back(static_cast<std::uint8_t>(unsigned(D.CanCollide)|(unsigned(D.Queryable)<<1)));
		Bytes.push_back(D.Ports);
		if (D.Assets) {
			Bytes[4]=2;
			for (const auto* S:{&D.Assets->Material,&D.Assets->Mesh,&D.Assets->Collision}) {
				Bytes.push_back(static_cast<std::uint8_t>(S->size())); Bytes.insert(Bytes.end(),S->begin(),S->end());
			}
			for (const auto& Pin:D.Assets->Pins) {
				Bytes.push_back(Pin.Kind); Bytes.insert(Bytes.end(),Pin.Id.begin(),Pin.Id.end());
				Bytes.insert(Bytes.end(),Pin.Digest.begin(),Pin.Digest.end());
			}
		}
		return AssetContentId::Hash(Bytes).ToString();
	}
	const Definition& Field::Lookup(const Catalog* Bindings,std::uint16_t Id) {
		if (Id<=Brick) return Builtins[Id];
		if (!Bindings || Id-Brick-1>=Bindings->Entries.size())
			throw std::invalid_argument("[BlockGrid:Definition] Unknown block definition");
		return Bindings->Entries[Id-Brick-1];
	}
	const Definition& Field::GetDefinition(std::uint16_t Id) const { return Lookup(Definitions.get(),Id); }
	const Definition& Field::Candidate::GetDefinition(std::uint16_t Id) const { return Lookup(Definitions,Id); }
	std::optional<std::uint16_t> Field::FindDefinitionId(std::string_view Key) const {
		if (Key=="Stone") return Stone;
		if (Key=="Brick") return Brick;
		if (Definitions) {
			const auto Found=Definitions->Names.find(Key);
			if (Found!=Definitions->Names.end()) return Found->second;
		}
		return {};
	}
	std::uint16_t Field::GetDefinitionId(std::string_view Key) const {
		if (const auto Id=FindDefinitionId(Key)) return *Id;
		throw std::invalid_argument("[BlockGrid:Definition] Unknown block definition");
	}
	std::size_t Field::GetDefinitionCount() const noexcept { return Definitions?Definitions->Entries.size():0; }
	void Field::HydrateAssets(const std::function<std::shared_ptr<const AssetBinding>(const AssetBinding&)>& Resolve) {
		if (!HasAssetBindings()) return;
		std::shared_ptr<Catalog> Next;
		for (std::size_t I=0;I<Definitions->Entries.size();++I) {
			const auto& A=Definitions->Entries[I].Assets;
			if (!A) continue;
			const auto Resolved=Resolve(*A);
			if (!Resolved) {
				if (A->Resolved) continue;
				throw std::invalid_argument("[BlockGrid:Asset] Unresolved project dependency");
			}
			if (!Resolved->Resolved || *Resolved!=*A)
				throw std::invalid_argument("[BlockGrid:Asset] Resource closure differs from the pinned dataset");
			if (!Next) Next=std::make_shared<Catalog>(*Definitions);
			auto& Spec=Next->Entries[I];
			Next->AssetBytes-=AssetPinCharge(Spec);
			Spec.Assets=Resolved;
			Next->AssetBytes+=AssetPinCharge(Spec);
			if (Next->AssetBytes>MaximumCatalogAssetBytes) throw std::length_error("[BlockGrid:Limit] Asset storage capacity exhausted");
		}
		if (Next) {
			if (GeometryRevision==std::numeric_limits<std::uint64_t>::max())
				throw std::overflow_error("[BlockGrid:Revision] Geometry revision exhausted");
			Definitions=std::move(Next);
			++GeometryRevision;
		}
	}
	bool Field::IsValid(Block B) const noexcept {
		return ValidBlock(B) && (B.Definition<=Brick || B.Definition<=Brick+GetDefinitionCount());
	}
	bool Field::Define(Definition Spec,bool Replace,const Admission& BeforeCommit) {
		for (auto& C:Spec.Color) if (C==0) C=0;
		ValidateDefinition(Spec);
		std::uint16_t Id{};
		if (Definitions) {
			const auto It=Definitions->Names.find(Spec.Key);
			if (It!=Definitions->Names.end()) Id=It->second;
		}
		if (Id) {
			if (Spec==GetDefinition(Id)) return false;
			if (!Replace) throw std::domain_error("[BlockGrid:Conflict] Key already binds a different definition");
		} else if (Replace) throw std::invalid_argument("[BlockGrid:Definition] Replacement requires an existing binding");
		else if (GetDefinitionCount()>=MaximumDefinitions)
			throw std::length_error("[BlockGrid:Limit] Custom definition capacity exhausted");
		const auto Prior=Id?GetDefinition(Id).Assets:nullptr;
		const auto Pins=(Definitions?Definitions->PinCount:0)-(Prior?Prior->Pins.size():0)+(Spec.Assets?Spec.Assets->Pins.size():0);
		if (Pins>MaximumCatalogPins) throw std::length_error("[BlockGrid:Limit] Catalog asset dependency capacity exhausted");
		const auto Bytes=EncodedBytes+(Id?0:Spec.Key.size()+16+(Definitions?0:2))-
			(Prior?AssetBindingBytes(*Prior):0)+(Spec.Assets?AssetBindingBytes(*Spec.Assets):0);
		if (Bytes+(State?State->MaxEncodedBytes():0)>MaximumEncodedBytes || Revision==std::numeric_limits<std::uint64_t>::max())
			throw std::length_error("[BlockGrid:Limit] Grid capacity exhausted");
		auto Updated=Definitions?std::make_shared<Catalog>(*Definitions):std::make_shared<Catalog>();
		Updated->AssetCount=Updated->AssetCount-unsigned(bool(Prior))+unsigned(bool(Spec.Assets));
		Updated->PinCount=Pins;
		Updated->AssetBytes=Updated->AssetBytes-(Id?AssetPinCharge(GetDefinition(Id)):0)+AssetPinCharge(Spec);
		if (Updated->AssetBytes>MaximumCatalogAssetBytes) throw std::length_error("[BlockGrid:Limit] Asset storage capacity exhausted");
		Directory Affected;
		if (Id) {
			Updated->Entries[Id-Brick-1]=std::move(Spec);
			if (Dependencies) {
				const auto First=Dependencies->lower_bound({Id,{-CoordinateLimit,-CoordinateLimit,-CoordinateLimit}});
				for (auto It=First;It!=Dependencies->end() && It->Id==Id;++It) {
					if (Affected.size()>=MaximumCommands) throw std::length_error("[BlockGrid:Limit] Definition affects too many leaves");
					Affected.emplace(It->Origin,Leaves.at(It->Origin));
				}
			}
		} else {
			Updated->Names.emplace(Spec.Key,static_cast<std::uint16_t>(Brick+1+Updated->Entries.size()));
			Updated->Entries.push_back(std::move(Spec));
		}
		const Candidate Next(*this,Updated.get(),Affected,{},Leaves.size(),Occupied);
		if (BeforeCommit && !BeforeCommit(Next,{})) throw std::length_error("[BlockGrid:Admission] Provider capacity denied");
		Definitions=std::move(Updated);
		EncodedBytes=Bytes;
		++Revision;
		++GeometryRevision;
		++PublicRevision;
		return true;
	}
	void Field::UndoDefinitionInsertion(const Definition& Expected,const Admission& BeforeCommit) {
		const auto Last=static_cast<std::uint16_t>(Brick+GetDefinitionCount());
		if (!Definitions || Definitions->Entries.back()!=Expected ||
			(Dependencies && Dependencies->lower_bound({Last,{-CoordinateLimit,-CoordinateLimit,-CoordinateLimit}})!=Dependencies->end()))
			throw std::domain_error("[BlockGrid:Conflict] Definition still has references or catalog history has diverged");
		if (Revision==std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("[BlockGrid:Revision] Revision exhausted");
		std::shared_ptr<Catalog> Next;
		if (Definitions->Entries.size()>1) {
			Next=std::make_shared<Catalog>(*Definitions);
			Next->Names.erase(Expected.Key);
			Next->Entries.pop_back();
			if (Expected.Assets) {
				--Next->AssetCount; Next->PinCount-=Expected.Assets->Pins.size();
				Next->AssetBytes-=AssetPinCharge(Definitions->Entries.back());
			}
		}
		const Directory Empty;
		const Candidate View(*this,Next.get(),Empty,{},Leaves.size(),Occupied);
		if (BeforeCommit && !BeforeCommit(View,{})) throw std::length_error("[BlockGrid:Admission] Provider capacity denied");
		EncodedBytes-=Expected.Key.size()+16+(Next?0:2)+(Expected.Assets?AssetBindingBytes(*Expected.Assets):0);
		Definitions=std::move(Next);
		++Revision;
		++GeometryRevision;
		++PublicRevision;
	}
}
