// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/blockgrid/Persistence.hpp"
#include "gargantuan/blockgrid/Field.hpp"
#include <algorithm>
#include <stdexcept>

namespace gargantuan::blockgrid {
	namespace {
		constexpr std::array<std::uint8_t,5> Magic{'B','G','S','M',1};
		[[noreturn]] void Invalid() { throw std::invalid_argument("[BlockGrid:Persistence] Invalid native segment closure"); }
		std::size_t Count(std::size_t Size) {
			if (Size<=SegmentBytes || Size>MaximumEncodedBytes) Invalid();
			return (Size+SegmentBytes-1)/SegmentBytes;
		}
		void Charge(std::size_t& Total,std::size_t Size) {
			if (Size>MaximumDocumentCellBytes-Total) throw std::length_error("[BlockGrid:Persistence] Native document byte budget exhausted");
			Total+=Size;
		}
		std::size_t PartSize(const SegmentManifest& Manifest,std::size_t I) {
			return std::min<std::size_t>(Manifest.Size-I*SegmentBytes,SegmentBytes);
		}
		void CheckPart(const AssetContentId& Digest,std::size_t Size,const Segment& Part) {
			if (!Part.Bytes || Part.Bytes->size()!=Size || AssetContentId::Hash(*Part.Bytes)!=Digest) Invalid();
		}
		SegmentData Assemble(const SegmentManifest& Manifest,const SegmentMap& Parts,bool CheckIdentities) {
			if (Count(Manifest.Size)!=Manifest.Parts.size()) Invalid();
			SegmentData Bytes;
			Bytes.reserve(Manifest.Size);
			for (std::size_t I=0;I<Manifest.Parts.size();++I) {
				const auto It=Parts.find(Manifest.Parts[I]);
				if (It==Parts.end() || !It->second.Bytes || It->second.Bytes->size()!=PartSize(Manifest,I)) Invalid();
				if (CheckIdentities && AssetContentId::Hash(*It->second.Bytes)!=It->first) Invalid();
				Bytes.insert(Bytes.end(),It->second.Bytes->begin(),It->second.Bytes->end());
			}
			if (AssetContentId::Hash(Bytes)!=Manifest.Digest) Invalid();
			return Bytes;
		}
	}
	SegmentManifest DecodeSegmentManifest(std::span<const std::uint8_t> Bytes) {
		constexpr auto Head=Magic.size()+4+32;
		if (Bytes.size()<Head || !std::ranges::equal(Bytes.first(Magic.size()),Magic)) Invalid();
		SegmentManifest M;
		for (unsigned I=0;I<4;++I) M.Size|=std::uint32_t(Bytes[Magic.size()+I])<<(8*I);
		const auto N=Count(M.Size);
		if (Bytes.size()!=Head+32*N) Invalid();
		std::ranges::copy(Bytes.subspan(Magic.size()+4,32),M.Digest.Bytes.begin());
		if (!M.Digest.IsValid()) Invalid();
		for (std::size_t I=0;I<N;++I) {
			auto& Id=M.Parts.emplace_back();std::ranges::copy(Bytes.subspan(Head+32*I,32),Id.Bytes.begin());
			if (!Id.IsValid()) Invalid();
		}
		return M;
	}
	SegmentData SegmentCapture::Store(std::span<const std::uint8_t> Bytes) {
		(void)Count(Bytes.size());Charge(LogicalBytes,Bytes.size());
		SegmentData Out(Magic.begin(),Magic.end());
		for (unsigned I=0;I<4;++I) Out.push_back(static_cast<std::uint8_t>(Bytes.size()>>(8*I)));
		const auto Digest=AssetContentId::Hash(Bytes);
		Out.insert(Out.end(),Digest.Bytes.begin(),Digest.Bytes.end());
		for (std::size_t At=0;At<Bytes.size();At+=SegmentBytes) {
			const auto Slice=Bytes.subspan(At,std::min(SegmentBytes,Bytes.size()-At));
			const auto Id=AssetContentId::Hash(Slice);
			auto It=Parts.find(Id);
			if (It==Parts.end()) {
				if (Parts.size()>=MaximumDocumentSegments) Invalid();
				Parts.emplace(Id,Segment{std::make_shared<const SegmentData>(Slice.begin(),Slice.end())});
			} else if (!It->second.Bytes || !std::ranges::equal(*It->second.Bytes,Slice)) Invalid();
			Out.insert(Out.end(),Id.Bytes.begin(),Id.Bytes.end());
		}
		return Out;
	}
	void SegmentRequirements::Add(const SegmentManifest& M) {
		if (Count(M.Size)!=M.Parts.size() || !M.Digest.IsValid()) Invalid();
		Charge(LogicalBytes,M.Size);
		for (std::size_t I=0;I<M.Parts.size();++I) {
			if (!M.Parts[I].IsValid()) Invalid();
			const auto Size=PartSize(M,I);
			const auto [It,Added]=Parts.try_emplace(M.Parts[I],Size);
			if ((!Added && It->second!=Size) || Parts.size()>MaximumDocumentSegments) Invalid();
		}
		Datasets.push_back(M);
	}
	SegmentMap ReadSegments(const SegmentRequirements& Required,const SegmentReader& Read) {
		SegmentMap Result;
		for (const auto& [Id,Size]:Required.Parts) {
			if (!Read) Invalid();
			auto Part=Read(Id,Size);CheckPart(Id,Size,Part);Result.emplace(Id,std::move(Part));
		}
		for (const auto& Manifest:Required.Datasets) (void)Assemble(Manifest,Result,false);
		return Result;
	}
	void VerifySegments(const SegmentRequirements& Required,const SegmentMap& Actual) {
		if (Required.Parts.size()!=Actual.size()) Invalid();
		for (const auto& [Id,Size]:Required.Parts) {
			const auto Found=Actual.find(Id);if (Found==Actual.end()) Invalid();
			CheckPart(Id,Size,Found->second);
		}
		for (const auto& Manifest:Required.Datasets) (void)Assemble(Manifest,Actual,false);
	}
	SegmentData JoinSegments(const SegmentManifest& M,const SegmentMap& Parts) {
		return Assemble(M,Parts,true);
	}
	std::string SegmentFileName(const AssetContentId& Digest) {
		if (!Digest.IsValid()) Invalid();
		return Digest.ToString()+".bgseg";
	}
}
