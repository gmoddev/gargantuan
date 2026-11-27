// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/blockgrid/Field.hpp"
#include "gargantuan/assets/AssetTypes.hpp"
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
	}
	void ValidateDefinition(const Definition& D) {
		constexpr std::string_view Prefix="project:";
		const auto Lower=[](char C){return C>='a' && C<='z';};
		if (!D.Key.starts_with(Prefix) || D.Key.size()<=Prefix.size() || D.Key.size()>Prefix.size()+48 ||
			!Lower(D.Key[Prefix.size()]) || !std::ranges::all_of(std::string_view(D.Key).substr(Prefix.size()),[&](char C){
				return Lower(C) || (C>='0' && C<='9') || C=='_' || C=='-';
			})) throw std::invalid_argument("[BlockGrid:Definition] Expected project: followed by a lowercase identifier");
		if (D.Geometry>Shape::Stairs || D.Ports>63 || !std::ranges::all_of(D.Color,[](float C) {
				return std::isfinite(C) && C>=0 && C<=1 && !std::signbit(C);
			})) throw std::invalid_argument("[BlockGrid:Definition] Unsupported shape, color or port mask");
	}
	std::string DefinitionDigest(const Definition& D) {
		std::vector<std::uint8_t> Bytes{'B','D','E','F',1,static_cast<std::uint8_t>(D.Geometry)};
		for (const auto Channel:D.Color) {
			const auto Bits=std::bit_cast<std::uint32_t>(Channel);
			for (unsigned I=0;I<4;++I) Bytes.push_back(static_cast<std::uint8_t>(Bits>>(8*I)));
		}
		Bytes.push_back(static_cast<std::uint8_t>(unsigned(D.CanCollide)|(unsigned(D.Queryable)<<1)));
		Bytes.push_back(D.Ports);
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
		const auto Bytes=EncodedBytes+(Id?0:Spec.Key.size()+16+(Definitions?0:2));
		if (Bytes>MaximumEncodedBytes || Revision==std::numeric_limits<std::uint64_t>::max())
			throw std::length_error("[BlockGrid:Limit] Grid capacity exhausted");
		auto Updated=Definitions?std::make_shared<Catalog>(*Definitions):std::make_shared<Catalog>();
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
		}
		const Directory Empty;
		const Candidate View(*this,Next.get(),Empty,{},Leaves.size(),Occupied);
		if (BeforeCommit && !BeforeCommit(View,{})) throw std::length_error("[BlockGrid:Admission] Provider capacity denied");
		EncodedBytes-=Expected.Key.size()+16+(Next?0:2);
		Definitions=std::move(Next);
		++Revision;
	}
}
