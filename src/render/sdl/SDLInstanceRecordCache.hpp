#pragma once

#include "gargantuan/render/RenderPublication.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace gargantuan {
	// Matches the untextured opaque vertex inputs. Native texture-view flags and
	// sampler modes are deliberately absent from eligibility for this first path.
	struct alignas(16) SDLInstanceRecord {
		glm::mat4 ModelMatrix{1.0f};
		glm::mat4 NormalMatrix{1.0f};
		glm::vec4 Color{1.0f};
		glm::vec4 MaterialValues{};
		glm::vec4 MaterialFlags{};
		glm::vec4 EmissiveOcclusion{};
		glm::vec4 MaterialControls{};
		glm::vec4 ColorSamplerModes{};
	};
	static_assert(sizeof(glm::mat4) == 64 && sizeof(glm::vec4) == 16);
	static_assert(sizeof(SDLInstanceRecord) == 224);
	static_assert(std::is_trivially_copyable_v<SDLInstanceRecord>);
	static_assert(offsetof(SDLInstanceRecord, ModelMatrix) == 0);
	static_assert(offsetof(SDLInstanceRecord, NormalMatrix) == 64);
	static_assert(offsetof(SDLInstanceRecord, Color) == 128);
	static_assert(offsetof(SDLInstanceRecord, MaterialValues) == 144);
	static_assert(offsetof(SDLInstanceRecord, MaterialFlags) == 160);
	static_assert(offsetof(SDLInstanceRecord, EmissiveOcclusion) == 176);
	static_assert(offsetof(SDLInstanceRecord, MaterialControls) == 192);
	static_assert(offsetof(SDLInstanceRecord, ColorSamplerModes) == 208);

	struct SDLInstanceDirtyRange {
		std::uint32_t First = 0;
		std::uint32_t Count = 0;
	};

	struct SDLInstanceUploadPlan {
		bool Full = false;
		std::vector<SDLInstanceDirtyRange> Ranges;
		std::size_t Bytes = 0;
	};

	// Disposable renderer-owned value cache. Its slots have no semantic authority;
	// publication ObjectIds remain the only identity and picking source.
	class SDLInstanceRecordCache final {
	  public:
		static constexpr std::size_t MaximumRecords = 65'536;
		static constexpr std::size_t MaximumSparseRanges = 256;

		explicit SDLInstanceRecordCache(float OutputModeValue) : OutputMode(OutputModeValue) {}

		void Clear() {
			Slots.clear();
			FreeSlots.clear();
			ObjectSlots.clear();
			DirtySlots.clear();
			FullUpload = true;
			Available = true;
		}

		// Called only after neutral projection validation. Failure disables this
		// optional path until a full resync; ordinary drawing remains available.
		[[nodiscard]] bool Apply(const RenderPublication &Publication) {
			if (Publication.FullResync) Clear();
			if (!Available) return false;
			for (const auto &Remove : Publication.Removes) {
				const auto Existing = ObjectSlots.find(Remove.Object);
				if (Existing == ObjectSlots.end()) continue;
				Slots[Existing->second].Active = false;
				FreeSlots.push_back(Existing->second);
				ObjectSlots.erase(Existing);
			}
			for (const auto &Create : Publication.Creates) {
				if (ObjectSlots.contains(Create.Item.Object)) return Disable();
				if (FreeSlots.empty() && Slots.size() == MaximumRecords) return Disable();
				std::uint32_t Slot;
				if (!FreeSlots.empty()) {
					Slot = FreeSlots.back();
					FreeSlots.pop_back();
				} else {
					Slot = static_cast<std::uint32_t>(Slots.size());
					Slots.emplace_back();
				}
				Slots[Slot] = {Pack(Create.Item, SelectMaterial(Create.Material, Create.Primitives)), true};
				ObjectSlots.emplace(Create.Item.Object, Slot);
				DirtySlots.push_back(Slot);
			}
			for (const auto &Update : Publication.Updates) {
				const auto Existing = ObjectSlots.find(Update.Object);
				if (Existing == ObjectSlots.end()) return Disable();
				const auto Packed = Pack(Update.Item, SelectMaterial(Update.Material, Update.Primitives));
				if (SameRecord(Slots[Existing->second].Record, Packed)) continue;
				Slots[Existing->second].Record = Packed;
				DirtySlots.push_back(Existing->second);
			}
			return true;
		}

		[[nodiscard]] std::optional<std::uint32_t> Find(ObjectId Object) const {
			if (!Available) return std::nullopt;
			const auto Existing = ObjectSlots.find(Object);
			return Existing == ObjectSlots.end() ? std::nullopt : std::optional(Existing->second);
		}

		[[nodiscard]] const SDLInstanceRecord *Get(std::uint32_t Slot) const {
			return Slot < Slots.size() && Slots[Slot].Active ? &Slots[Slot].Record : nullptr;
		}

		[[nodiscard]] std::size_t SlotCount() const { return Slots.size(); }
		[[nodiscard]] std::size_t ActiveCount() const { return ObjectSlots.size(); }
		[[nodiscard]] bool IsAvailable() const { return Available; }

		[[nodiscard]] SDLInstanceUploadPlan PlanUpload() const {
			SDLInstanceUploadPlan Plan;
			if (!Available || Slots.empty()) return Plan;
			if (FullUpload) {
				Plan.Full = true;
				Plan.Ranges.push_back({0, static_cast<std::uint32_t>(Slots.size())});
				Plan.Bytes = Slots.size() * sizeof(SDLInstanceRecord);
				return Plan;
			}
			auto Dirty = DirtySlots;
			std::ranges::sort(Dirty);
			Dirty.erase(std::unique(Dirty.begin(), Dirty.end()), Dirty.end());
			for (const auto Slot : Dirty) {
				if (!Plan.Ranges.empty() && Plan.Ranges.back().First + Plan.Ranges.back().Count == Slot)
					++Plan.Ranges.back().Count;
				else Plan.Ranges.push_back({Slot, 1});
			}
			if (Plan.Ranges.size() > MaximumSparseRanges || Dirty.size() > Slots.size() / 4) {
				Plan.Full = true;
				Plan.Ranges = {{0, static_cast<std::uint32_t>(Slots.size())}};
				Plan.Bytes = Slots.size() * sizeof(SDLInstanceRecord);
			} else Plan.Bytes = Dirty.size() * sizeof(SDLInstanceRecord);
			return Plan;
		}

		void MarkUploaded() { DirtySlots.clear(); FullUpload = false; }
		void MarkFullUpload() { FullUpload = true; }

	  private:
		struct SlotState { SDLInstanceRecord Record; bool Active = false; };
		float OutputMode = 1.0f;
		std::vector<SlotState> Slots;
		std::vector<std::uint32_t> FreeSlots;
		std::unordered_map<ObjectId, std::uint32_t> ObjectSlots;
		std::vector<std::uint32_t> DirtySlots;
		bool FullUpload = true;
		bool Available = true;

		[[nodiscard]] bool Disable() {
			Slots.clear(); FreeSlots.clear(); ObjectSlots.clear(); DirtySlots.clear();
			FullUpload = true; Available = false;
			return false;
		}

		[[nodiscard]] static const RenderMaterialState &SelectMaterial(
			const RenderMaterialState &Fallback,
			const std::shared_ptr<const std::vector<RenderPrimitiveMaterialState>> &Primitives) {
			return Primitives && Primitives->size() == 1 ? Primitives->front().Material : Fallback;
		}

		[[nodiscard]] SDLInstanceRecord Pack(const RenderItem &Item, const RenderMaterialState &Material) const {
			const float Shading = Material.MetallicRoughnessTexture ? 2.0f :
				Material.ShadingModel == RenderShadingModel::MetallicRoughness ? 1.0f : 0.0f;
			return {
				.ModelMatrix = Item.ModelMatrix,
				.NormalMatrix = glm::transpose(Item.InverseModelMatrix),
				.Color = Material.BaseColorFactor,
				.MaterialValues = {Material.AlphaCutoff, static_cast<float>(Material.OpacityMode),
					Material.Metallic, Material.Roughness},
				.MaterialFlags = {Shading, Material.BaseColorTexture ? 1.0f : 0.0f,
					Material.NormalTexture ? 1.0f : 0.0f, OutputMode},
				.EmissiveOcclusion = glm::vec4(Material.EmissiveFactor,
					Material.OcclusionTexture ? Material.OcclusionStrength : 0.0f),
				.MaterialControls = {Material.NormalScale, Material.EmissiveTexture ? 1.0f : 0.0f, 0.0f, 0.0f},
				.ColorSamplerModes = {},
			};
		}

		[[nodiscard]] static bool SameRecord(const SDLInstanceRecord &Left, const SDLInstanceRecord &Right) {
			// Pack initializes every byte of this contiguous GPU payload. Equal bytes
			// imply equal render values; different signed-zero encodings may cause an
			// extra upload, which still preserves the exact new payload.
			return std::memcmp(&Left, &Right, sizeof(SDLInstanceRecord)) == 0;
		}
	};
} // namespace gargantuan
