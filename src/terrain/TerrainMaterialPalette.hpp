// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "gargantuan/assets/AssetTypes.hpp"
#include "terrain/EditCoordinator.hpp"

#include <algorithm>

namespace gargantuan {
    class AssetService;
    class Workspace;

    namespace terrain {
        inline constexpr std::string_view TerrainMaterialBindingsAttribute = "TerrainMaterialBindings";

        struct TerrainMaterialBinding {
            MaterialId Id{};
            AssetId Asset;
        };

        // Private renderer snapshot. IDs are terrain sample values; assets
        // supply the appearance of selected IDs without changing the field.
        class TerrainMaterialPalette final {
        public:
            static constexpr std::size_t MaximumBindings = 64;
            static constexpr std::size_t MaximumChunkMaterials = 4;

            // Resolve a complete opt-in snapshot from the current project
            // catalog. This profile uses factors from opaque PBR materials.
            [[nodiscard]] static std::expected<std::shared_ptr<const TerrainMaterialPalette>, EditStatus>
            Resolve(const AssetService &Assets, std::span<const TerrainMaterialBinding> Bindings);

            [[nodiscard]] std::uint8_t Find(MaterialId Id) const noexcept {
                const auto Found = std::ranges::lower_bound(Entries, Id, {}, &Entry::Id);
                return Found != Entries.end() && Found->Id == Id ? static_cast<std::uint8_t>(Found - Entries.begin() + 1) : 0;
            }

            // Pass a nonzero slot from Find on this snapshot.
            [[nodiscard]] const RenderMaterialState &GetMaterial(std::uint8_t Slot) const noexcept {
                return Entries[Slot - 1].Material;
            }
            [[nodiscard]] const std::array<std::uint8_t, 4> &GetTint(std::uint8_t Slot) const noexcept {
                return Entries[Slot - 1].Tint;
            }

        private:
            struct Entry {
                MaterialId Id;
                RenderMaterialState Material;
                std::array<std::uint8_t, 4> Tint;
            };

            std::vector<Entry> Entries;
        };

        // Read the ordinary serialized Workspace attribute once during startup.
        // A null palette selects the built-in tint.
        [[nodiscard]] std::expected<std::shared_ptr<const TerrainMaterialPalette>, EditStatus>
        ResolveTerrainMaterialBindings(const Workspace &World, const AssetService &Assets);
    }
}
