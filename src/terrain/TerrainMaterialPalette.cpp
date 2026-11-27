// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "terrain/TerrainMaterialPalette.hpp"
#include "gargantuan/runtime/AttributeValidation.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "gargantuan/services/Workspace.hpp"
#include "serialization/JsonCodec.hpp"

#include <cmath>

namespace gargantuan::terrain {
    std::expected<std::shared_ptr<const TerrainMaterialPalette>, EditStatus> TerrainMaterialPalette::Resolve(
        const AssetService &Assets, std::span<const TerrainMaterialBinding> Bindings) {
        if (Bindings.size() > MaximumBindings) return std::unexpected(EditStatus::LimitExceeded);
        if (Bindings.empty()) return std::shared_ptr<const TerrainMaterialPalette>{};
        try {
            std::vector<TerrainMaterialBinding> Ordered(Bindings.begin(), Bindings.end());
            std::ranges::sort(Ordered, {}, &TerrainMaterialBinding::Id);
            MaterialId Previous = 0;
            for (const auto &Binding : Ordered) {
                if (Binding.Id == Previous || !Binding.Asset.IsValid()) return std::unexpected(EditStatus::InvalidInput);
                Previous = Binding.Id;
            }

            auto Palette = std::make_shared<TerrainMaterialPalette>();
            Palette->Entries.reserve(Ordered.size());
            for (const auto &Binding : Ordered) {
                const auto Record = Assets.GetAsset(AssetReference::FromAssetId(Binding.Asset).Value);
                if (!Record || Record->BuiltIn || !Record->Asset || Record->ContentRevision == 0 ||
                    !Record->ContentId.IsValid() ||
                    (Record->State != AssetState::Ready && Record->State != AssetState::Stale &&
                        Record->State != AssetState::Importing))
                    return std::unexpected(EditStatus::Unavailable);
                const auto *Source = std::get_if<ImportedMaterial>(Record->Asset.get());
                if (!Source || Record->Kind != AssetKind::Material || Source->AlphaMode != AssetMaterialAlphaMode::Opaque)
                    return std::unexpected(EditStatus::InvalidInput);

                // Terrain currently has no meaningful shared UV/tangent frame
                // for project textures. The profile shares the authored factors.
                RenderMaterialState Value;
                Value.Revision = Record->ContentRevision;
                Value.ShadingModel = RenderShadingModel::MetallicRoughness;
                Value.BaseColorFactor = glm::vec4(glm::vec3(Source->BaseColorFactor), 1.f);
                Value.Metallic = Source->MetallicFactor;
                Value.Roughness = Source->RoughnessFactor;
                Value.EmissiveFactor = Source->EmissiveFactor;
                const auto Channel = [](float Factor) {
                    return static_cast<std::uint8_t>(std::lround(std::clamp(Factor, 0.f, 1.f) * 255.f));
                };
                Palette->Entries.push_back({Binding.Id, Value, {
                    Channel(Value.BaseColorFactor.r), Channel(Value.BaseColorFactor.g), Channel(Value.BaseColorFactor.b), 255}});
            }
            return Palette;
        } catch (const std::bad_alloc &) {
            return std::unexpected(EditStatus::LimitExceeded);
        }
    }

    std::expected<std::shared_ptr<const TerrainMaterialPalette>, EditStatus>
    ResolveTerrainMaterialBindings(const Workspace &World, const AssetService &Assets) {
        try {
            const auto Attribute = World.GetAttributeValue(TerrainMaterialBindingsAttribute);
            if (!Attribute) return std::shared_ptr<const TerrainMaterialPalette>{};
            const auto *Encoded = std::get_if<std::string>(&*Attribute);
            if (!Encoded) return std::unexpected(EditStatus::InvalidInput);
            if (Encoded->empty()) return std::shared_ptr<const TerrainMaterialPalette>{};

            const auto Rows = JsonCodec::Parse(*Encoded, MaximumAttributeValueBytes, TerrainMaterialBindingsAttribute);
            if (!Rows) {
                switch (Rows.error().Code) {
                    case SerializationErrorCode::LimitExceeded: return std::unexpected(EditStatus::LimitExceeded);
                    case SerializationErrorCode::InternalFailure: return std::unexpected(EditStatus::PreparationFailed);
                    default: return std::unexpected(EditStatus::InvalidInput);
                }
            }
            if (!Rows->is_array()) return std::unexpected(EditStatus::InvalidInput);
            if (Rows->size() > TerrainMaterialPalette::MaximumBindings)
                return std::unexpected(EditStatus::LimitExceeded);

            std::vector<TerrainMaterialBinding> Bindings;
            Bindings.reserve(Rows->size());
            for (const auto &Row : *Rows) {
                if (!Row.is_object() || Row.size() != 2 || !Row.contains("Id") ||
                    !Row.contains("Asset") || !Row["Asset"].is_string())
                    return std::unexpected(EditStatus::InvalidInput);
                const auto Id = JsonCodec::DecodeUnsigned32(Row["Id"]);
                const auto Asset = AssetId::Parse(Row["Asset"].get_ref<const std::string &>());
                if (!Id || !*Id || !Asset) return std::unexpected(EditStatus::InvalidInput);
                Bindings.push_back({*Id, *Asset});
            }
            return TerrainMaterialPalette::Resolve(Assets, Bindings);
        } catch (const std::bad_alloc &) {
            return std::unexpected(EditStatus::LimitExceeded);
        }
    }
}
