#pragma once

#include "gargantuan/assets/AssetTypes.hpp"
#include "gargantuan/blockgrid/Persistence.hpp"
#include "gargantuan/classes/Instance.hpp"
#include "gargantuan/terrain/DomainTerrainBindings.hpp"

#include <expected>
#include <format>
#include <functional>
#include <cstdint>
#include <istream>
#include <optional>
#include <string_view>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gargantuan { class SpatialRegion; }
namespace gargantuan::InstanceSerialization {
	enum class InstanceFormat : int { Json, Binary };
	inline constexpr int TerrainSceneVersion = 6;
	inline constexpr int DomainTerrainSceneVersion = 7;
	inline constexpr int SegmentedGridSceneVersion = 8;
	using TerrainSceneAdmission = std::function<void(DataModel &, const AssetContentId &)>;
	using DomainTerrainAdmission = std::function<void(SpatialRegion &, std::uint32_t, const AssetContentId &)>;
	void VisitDocumentRegions(const std::shared_ptr<DataModel> &Root,
		const std::function<void(SpatialRegion &, std::uint32_t)> &Visit);
	struct DeserializationState {
		bool Ok = false;
		std::shared_ptr<Instance> Instance;
		std::vector<std::string> Errors;

		std::vector<std::string> CurrentPath{"(TOP)"};
		std::size_t ObjectsDecoded = 0;
		std::uint32_t FormatVersion = 0;
		std::unordered_map<std::uint32_t, std::shared_ptr<gargantuan::Instance>> DocumentNodes;
		struct PendingReference {
			std::shared_ptr<gargantuan::Instance> Owner;
			std::string Property;
			std::optional<std::uint32_t> TargetId;
		};
		std::vector<PendingReference> PendingReferences;
		bool PreserveSourceIds = true;
		const blockgrid::SegmentMap* GridSegments{};
		std::unordered_set<std::string> SourceIds;
		std::unordered_map<std::shared_ptr<gargantuan::Instance>, std::vector<std::string>> PendingTags;
		std::string FormatCurrentPath();

		template <class... Args> void PushError(std::format_string<Args...> fmt, Args &&...args) {
			auto formatted = std::format(fmt, std::forward<Args>(args)...);
			Errors.push_back(formatted);
		}

		template <class... Args> std::nullopt_t ReturnError(std::format_string<Args...> fmt, Args &&...args) {
			auto formatted = std::format(fmt, std::forward<Args>(args)...);
			Errors.push_back(formatted);
			return std::nullopt;
		}
	};

	std::string Serialize(InstanceFormat format, std::shared_ptr<Instance> &instance,
		std::optional<AssetContentId> TerrainManifest = {}, const terrain::DomainTerrainBindings &Domains = {},
		blockgrid::SegmentCapture* GridSegments=nullptr);
	std::string SerializeEmptyProject(InstanceFormat Format, std::string_view ProjectName);
	DeserializationState Deserialize(InstanceFormat format, std::istream &input,
		const TerrainSceneAdmission &AdmitTerrain = {}, const DomainTerrainAdmission &AdmitDomains = {},
		const blockgrid::SegmentMap* GridSegments=nullptr);
	[[nodiscard]] blockgrid::SegmentRequirements InspectGridSegments(std::string_view Json);
	[[nodiscard]] std::expected<std::optional<AssetContentId>, std::string> InspectTerrainManifest(std::string_view Json,
		terrain::DomainTerrainBindings *Domains = nullptr,blockgrid::SegmentRequirements* Grids=nullptr);
	DeserializationState DeserializeDetached(InstanceFormat format, std::istream &input);
	// Clone/duplicate create new logical resources. Undo/Redo uses the ordinary
	// identity-preserving detached reconstruction above.
	DeserializationState DeserializeClone(InstanceFormat Format, std::istream &Input,const blockgrid::SegmentMap* GridSegments=nullptr);
	[[nodiscard]] std::string SerializeClone(std::shared_ptr<Instance> Root,blockgrid::SegmentCapture* GridSegments=nullptr);
}
