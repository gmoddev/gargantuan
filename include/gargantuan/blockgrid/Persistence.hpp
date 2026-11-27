// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "gargantuan/assets/AssetTypes.hpp"
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <vector>

namespace gargantuan::blockgrid {
	inline constexpr char SegmentKey[]="NativeBlockCellSegmentsV2";
	inline constexpr std::size_t SegmentBytes=256*1024;
	inline constexpr std::size_t MaximumDocumentCellBytes=64*1024*1024;
	inline constexpr std::size_t MaximumDocumentSegments=2*MaximumDocumentCellBytes/SegmentBytes;
	using SegmentData=std::vector<std::uint8_t>;
	struct Segment {
		std::shared_ptr<const SegmentData> Bytes;
		bool operator==(const Segment& Other) const {
			return Bytes==Other.Bytes || (Bytes && Other.Bytes && *Bytes==*Other.Bytes);
		}
	};
	using SegmentMap=std::map<AssetContentId,Segment>;
	using SegmentReader=std::function<Segment(const AssetContentId&,std::size_t)>;
	struct SegmentManifest {
		std::uint32_t Size{};
		AssetContentId Digest;
		std::vector<AssetContentId> Parts;
	};
	struct SegmentRequirements {
		std::map<AssetContentId,std::size_t> Parts;
		std::vector<SegmentManifest> Datasets;
		std::size_t LogicalBytes{};
		void Add(const SegmentManifest& Manifest);
	};
	struct SegmentCapture {
		SegmentMap Parts;
		std::size_t LogicalBytes{};
		[[nodiscard]] SegmentData Store(std::span<const std::uint8_t> Bytes);
	};
	[[nodiscard]] SegmentManifest DecodeSegmentManifest(std::span<const std::uint8_t> Bytes);
	[[nodiscard]] SegmentMap ReadSegments(const SegmentRequirements& Required,const SegmentReader& Read);
	void VerifySegments(const SegmentRequirements& Required,const SegmentMap& Actual);
	[[nodiscard]] SegmentData JoinSegments(const SegmentManifest& Manifest,const SegmentMap& Parts);
	[[nodiscard]] std::string SegmentFileName(const AssetContentId& Digest);
}
