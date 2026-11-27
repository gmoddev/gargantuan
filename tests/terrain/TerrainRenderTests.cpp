// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/render/RenderProjection.hpp"
#include "gargantuan/render/Mesh.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/runtime/AttributeValidation.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "gargantuan/services/Workspace.hpp"
#include "assets/AssetImporter.hpp"
#include "terrain/RenderStage.hpp"
#include "terrain/TerrainMaterialPalette.hpp"
#include "TerrainMaterialProbe.hpp"

#include <array>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>

using namespace gargantuan;
using namespace gargantuan::terrain;

namespace {
    constexpr AssetId SlateAsset{22, 1};
    constexpr AssetId CopperAsset{22, 2};
    constexpr AssetId TranslucentAsset{22, 3};
    constexpr MaterialId Slate = 0xffff'fffe;
    constexpr MaterialId Copper = 32;

    void Require(bool Condition, const char *Message) {
        if (!Condition) throw std::runtime_error(Message);
    }

    RenderPublication NewPublication(RenderPublicationId Id, RenderPublicationId Base) {
        RenderPublication Result{};
        Result.Id = Id;
        Result.BaseId = Base;
        Result.FullResync = Base == 0;
        Result.Frame.ViewportWidth = 640;
        Result.Frame.ViewportHeight = 480;
        return Result;
    }

    std::shared_ptr<const SurfaceMesh> MakeTriangle(std::array<MaterialId, 3> Materials) {
        auto Value = std::make_shared<SurfaceMesh>();
        Value->Vertices = {
            {{0.f, 1.f, 0.f}, {0.f, 1.f, 0.f}, Materials[0]},
            {{0.f, 1.f, 1.f}, {0.f, 1.f, 0.f}, Materials[1]},
            {{1.f, 1.f, 0.f}, {0.f, 1.f, 0.f}, Materials[2]},
        };
        Value->Indices = {0, 1, 2};
        return Value;
    }

    void AddAsset(AssetRuntimeSnapshot &Snapshot, std::ostream &Records, AssetId Id, AssetKind Kind,
        const ImportedAsset &Asset, std::initializer_list<AssetId> Dependencies = {}) {
        auto Bytes = EncodeAssetArtifact(Asset, Kind).value();
        const auto Content = AssetContentId::Hash(*Bytes).ToString();
        if (!Snapshot.Artifacts.empty()) Records << ',';
        Records << R"({"AssetId":")" << Id.ToString() << R"(","Reference":")" << AssetReference::FromAssetId(Id).Value
            << R"(","Kind":")" << GetAssetKindName(Kind) << R"(","Name":"Terrain Probe","ContentId":")" << Content
            << R"(","ContentRevision":9,"State":"Ready","Dependencies":[)";
        const char *Separator = "";
        for (const auto Dependency : Dependencies) {
            Records << Separator << '"' << Dependency.ToString() << '"';
            Separator = ",";
        }
        Records << "]}";
        Snapshot.Artifacts.push_back({"assets/artifacts/" + Content + ".gasset", std::move(Bytes)});
    }

    void LoadProbeAssets(AssetService &Assets) {
        constexpr AssetId ImageAsset{23, 1};
        AssetRuntimeSnapshot Snapshot;
        std::ostringstream Records;
        Records << R"({"Format":"GargantuanRuntimeAssets","Version":1,"Assets":[)";
        auto Pixels = std::make_shared<const std::vector<std::uint8_t>>(std::initializer_list<std::uint8_t>{255, 160, 58, 255});
        AddAsset(Snapshot, Records, ImageAsset, AssetKind::Image, ImportedImage{1, 1, Pixels});
        ImportedMaterial Rock;
        Rock.BaseColorFactor = {.10f, .21f, .36f, 1.f};
        Rock.RoughnessFactor = .83f;
        Rock.MetallicFactor = .06f;
        Rock.BaseColorTexture = ImageAsset;
        AddAsset(Snapshot, Records, SlateAsset, AssetKind::Material, Rock, {ImageAsset});
        ImportedMaterial Metal;
        Metal.BaseColorFactor = {.75f, .25f, .07f, 1.f};
        Metal.RoughnessFactor = .38f;
        Metal.MetallicFactor = .65f;
        Metal.EmissiveFactor = {.015f, .004f, .002f};
        AddAsset(Snapshot, Records, CopperAsset, AssetKind::Material, Metal);
        Metal.AlphaMode = AssetMaterialAlphaMode::Blend;
        AddAsset(Snapshot, Records, TranslucentAsset, AssetKind::Material, Metal);
        Records << "]}";
        Snapshot.CatalogJson = std::move(Records).str();
        Assets.LoadRuntimeAssetSnapshot(Snapshot);
    }

    std::shared_ptr<const TerrainMaterialPalette> MakeProbePalette(AssetService &Assets) {
        const std::array Bindings{TerrainMaterialBinding{Slate, SlateAsset}, TerrainMaterialBinding{Copper, CopperAsset}};
        return TerrainMaterialPalette::Resolve(Assets, Bindings).value();
    }

    void TestTerrainMaterialBindings(const AssetService &Assets) {
        auto World = std::make_shared<DataModel>();
        const auto Space = std::dynamic_pointer_cast<Workspace>(World->GetService("Workspace"));
        Require(static_cast<bool>(Space), "workspace material fixture");
        const auto Set = [&](WireValue Value) {
            Require(Space->ApplyAttributeMutation(TerrainMaterialBindingsAttribute, std::move(Value)) ==
                MutationStatus::Success, "terrain binding attribute mutation");
            return ResolveTerrainMaterialBindings(*Space, Assets);
        };
        Require(!ResolveTerrainMaterialBindings(*Space, Assets).value() && !Set(std::string{}).value() &&
            !Set(std::string(" [] \n")).value(), "absent or empty terrain bindings select built-in tint");

        const auto Record = [](MaterialId Id, AssetId Asset) {
            return R"({"Id":)" + std::to_string(Id) + R"(,"Asset":")" + Asset.ToString() + R"("})";
        };
        constexpr auto MaximumId = std::numeric_limits<MaterialId>::max();
        const auto Valid = Set("[" + Record(MaximumId, SlateAsset) + "," + Record(1, CopperAsset) + "]");
        Require(Valid && *Valid && (*Valid)->Find(MaximumId) && (*Valid)->Find(1), "parse full-range stable field ID");
        const auto SlateTint = (*Valid)->GetTint((*Valid)->Find(MaximumId));
        Require(!Set(std::string("[]")).value() && (*Valid)->GetTint((*Valid)->Find(MaximumId)) == SlateTint,
            "resolved startup palette is immutable after attribute mutation");

        const auto WithAsset = [&](std::string_view Prefix, std::string_view Suffix = "\"}]") {
            return std::string(Prefix) + SlateAsset.ToString() + std::string(Suffix);
        };
        const std::array Malformed{
            std::string("null"), std::string("{}"), std::string("["),
            std::string(R"([{"Id":1,"Asset":null}])"),
            WithAsset(R"([{"Id":0,"Asset":")"), WithAsset(R"([{"Id":-1,"Asset":")"),
            WithAsset(R"([{"Id":4294967296,"Asset":")"), WithAsset(R"([{"Id":1.0,"Asset":")"),
            WithAsset(R"([{"Id":1,"Asset":"asset://)"),
            WithAsset(R"([{"Id":1,"Asset":")", R"(","Color":"red"}])"),
            "[" + Record(1, SlateAsset) + "," + Record(1, CopperAsset) + "]",
        };
        Require(Set(false).error() == EditStatus::InvalidInput, "non-string terrain attribute is rejected");
        for (const auto &Value : Malformed)
            Require(Set(Value).error() == EditStatus::InvalidInput, "malformed or duplicate binding is rejected");
        Require(Set("[" + Record(1, AssetId{999, 1}) + "]").error() == EditStatus::Unavailable &&
            Set("[" + Record(1, AssetId{23, 1}) + "]").error() == EditStatus::InvalidInput &&
            Set("[" + Record(1, TranslucentAsset) + "]").error() == EditStatus::InvalidInput,
            "unavailable asset, wrong kind, and translucent project material are rejected");

        std::string ShortIds = "[";
        for (MaterialId Id = 1; Id <= TerrainMaterialPalette::MaximumBindings; ++Id) {
            if (Id != 1) ShortIds += ',';
            ShortIds += Record(Id, SlateAsset);
        }
        Require(Set(ShortIds + "]").value()->Find(64) &&
            Set(ShortIds + "," + Record(65, SlateAsset) + "]").error() == EditStatus::LimitExceeded,
            "64 compact small IDs fit the attribute and a 65th exceeds the palette count");

        std::string LongIds = "[";
        for (MaterialId Offset = 0; Offset < 60; ++Offset) {
            if (Offset != 0) LongIds += ',';
            LongIds += Record(MaximumId - Offset, SlateAsset);
        }
        Require(ValidateAttributeValue(WireValue{LongIds + "]"}) == 4049 && Set(LongIds + "]").value()->Find(MaximumId),
            "60 full-width IDs fit the encoded 4096 byte attribute");
        bool WireRejected = false;
        try {
            static_cast<void>(ValidateAttributeValue(WireValue{LongIds + "," + Record(MaximumId - 60, SlateAsset) + "]"}));
        } catch (const std::invalid_argument &) {
            WireRejected = true;
        }
        Require(WireRejected, "61 full-width IDs exceed the attribute wire limit");
    }

    void TestOwnedProjection() {
        ObjectId Owner{29, 4};
        RenderStage Stage(Owner);
        constexpr std::array<std::uint8_t, 4> White{255, 255, 255, 255};
        Require(RenderVertex{}.Tint == White && Vertex{}.Tint == White, "ordinary mesh vertices default to neutral tint");
        const std::array Initial{
            ChunkPublicationCandidate{{-1, 0, 0}, MakeTriangle({1, 2, 900}), {}, true},
            ChunkPublicationCandidate{{0, 0, 0}, MakeTriangle({2, 2, 2}), {}, true},
        };
        auto Prepared = Stage.Stage({}, Initial).value();
        Require(!Stage.Capture(), "staging does not publish draws");
        Require(Stage.Stage({}, Initial).error() == EditStatus::Unavailable, "only one render stage can be outstanding");
        Prepared->Install(1);
        const auto First = Stage.Capture();
        Prepared->Install(1);
        Require(Stage.Capture() == First, "install is idempotent");
        Require(First && First->Draws->size() == 2, "materialized chunk draw list");
        const auto &Mixed = First->Chunks.at({-1, 0, 0});
        const auto &Vertices = *Mixed.Resource.Vertices;
        Require(!Mixed.Draw.Primitives && Mixed.Draw.Material.BaseColorFactor == glm::vec4(1.f) &&
            *Mixed.Resource.Indices == Initial.front().Visual->Indices &&
            Vertices[0].Tint == std::array<std::uint8_t, 4>{61, 94, 41, 255} &&
            Vertices[1].Tint == std::array<std::uint8_t, 4>{97, 94, 84, 255} &&
            Vertices[2].Tint == std::array<std::uint8_t, 4>{107, 99, 82, 255},
            "one opaque draw interpolates three vertex swatches including unmapped material fallback");

        auto Full = NewPublication(1, 0);
        AppendTerrainRenderState(Full, {}, First);
        auto Scene = Full.MeshCreates.front();
        Scene.Mesh.Domain = RenderMeshDomain::Scene;
        Full.MeshCreates.push_back(Scene);
        RenderProjection Projection;
        Require(Projection.Apply(Full).MeshesCreated == 3 && Projection.GetObjects().empty() &&
            Projection.GetTerrainDraws().size() == 2, "terrain meshes are domain-separated and share one semantic owner");
        Require(Projection.GetTerrainDraws().front().Item.Object == Owner &&
            Projection.GetTerrainDraws().front().Item.ModelMatrix[3].x == -32.0f, "negative chunk transform");

        auto Invalid = NewPublication(2, 1);
        Invalid.MeshRemoves.push_back({Full.MeshCreates.front().Mesh});
        try {
            static_cast<void>(Projection.Apply(Invalid));
            throw std::logic_error("bound terrain removal unexpectedly accepted");
        } catch (const std::invalid_argument &) {
            Require(Projection.GetLastPublicationId() == 1 && Projection.GetTerrainDraws().size() == 2,
                "bad terrain list preserves projection");
        }

        const std::array Replacements{
            ChunkPublicationCandidate{{-1, 0, 0}, MakeTriangle({5, 5, 5}), {}, false},
            ChunkPublicationCandidate{{0, 0, 0}, nullptr, {}, true},
        };
        Stage.Stage({}, Replacements).value()->Install(2);
        auto Incremental = NewPublication(2, 1);
        AppendTerrainRenderState(Incremental, First, Stage.Capture());
        const auto Changes = Projection.Apply(Incremental);
        Require(Changes.MeshesCreated == 1 && Changes.MeshesRemoved == 2 && Projection.GetTerrainDraws().size() == 1,
            "incremental terrain replace and remove");
        Require(Projection.GetMeshBounds(Scene.Mesh) && First->Draws->size() == 2, "scene identities and prior state survive");
        auto NoChange = NewPublication(3, 2);
        AppendTerrainRenderState(NoChange, Stage.Capture(), Stage.Capture());
        Require(!NoChange.TerrainDraws && NoChange.MeshCreates.empty(), "unchanged cursor emits no terrain work");

        std::unique_ptr<StagedPublication> Late;
        {
            RenderStage Closing(Owner);
            Late = Closing.Stage({}, Initial).value();
        }
        Late->Install(3);
        Require(!Late->IsInstalled(), "late token cannot publish after its render owner closes");
    }

    void TestTerrainMaterials(AssetService &Assets, const std::shared_ptr<const TerrainMaterialPalette> &Palette) {
        const auto SlateSlot = Palette->Find(Slate);
        Require(SlateSlot && Palette->Find(Copper) && !Palette->Find(7), "palette maps sparse stable IDs");
        const auto Existing = Assets.ResolveMaterial(AssetReference::FromAssetId(SlateAsset).Value);
        Require(Existing && Existing->RenderState.BaseColorTexture &&
            Palette->GetMaterial(SlateSlot).Revision == Existing->RenderState.Revision &&
            Palette->GetMaterial(SlateSlot).BaseColorFactor == Existing->RenderState.BaseColorFactor &&
            Palette->GetMaterial(SlateSlot).Metallic == Existing->RenderState.Metallic &&
            Palette->GetMaterial(SlateSlot).Roughness == Existing->RenderState.Roughness &&
            !Palette->GetMaterial(SlateSlot).BaseColorTexture, "opaque project factors are bound independently of texture residency");

        const std::array Invalid{TerrainMaterialBinding{0, SlateAsset}};
        const std::array Missing{TerrainMaterialBinding{Slate, AssetId{999, 1}}};
        const std::array Duplicate{TerrainMaterialBinding{Slate, SlateAsset}, TerrainMaterialBinding{Slate, CopperAsset}};
        const std::array Translucent{TerrainMaterialBinding{Slate, TranslucentAsset}};
        const std::array<TerrainMaterialBinding, TerrainMaterialPalette::MaximumBindings + 1> Overflow{};
        Require(!TerrainMaterialPalette::Resolve(Assets, {}).value() &&
            TerrainMaterialPalette::Resolve(Assets, Invalid).error() == EditStatus::InvalidInput &&
            TerrainMaterialPalette::Resolve(Assets, Missing).error() == EditStatus::Unavailable &&
            TerrainMaterialPalette::Resolve(Assets, Duplicate).error() == EditStatus::InvalidInput &&
            TerrainMaterialPalette::Resolve(Assets, Translucent).error() == EditStatus::InvalidInput &&
            TerrainMaterialPalette::Resolve(Assets, Overflow).error() == EditStatus::LimitExceeded,
            "bounded palette admission rejects unsupported and unresolved factors");

        auto Mixed = std::make_shared<SurfaceMesh>(*MakeTriangle({Slate, Slate, Slate}));
        auto Other = Mixed->Vertices[1];
        Other.Material = Copper;
        Other.Position.X += 1.f;
        Mixed->Vertices.push_back(Other);
        Mixed->Indices.insert(Mixed->Indices.end(), {2, 1, 3});
        const std::array Candidates{
            ChunkPublicationCandidate{{-1, 0, 0}, Mixed, {}, true},
            ChunkPublicationCandidate{{0, 0, 0}, MakeTriangle({Copper, Copper, Copper}), {}, true},
        };
        RenderStage TintStage({29, 4}, Palette);
        TintStage.Stage({}, Candidates).value()->Install(1);
        const auto &Tint = TintStage.Capture()->Chunks.at({-1, 0, 0});
        Require(!Tint.Draw.Primitives && *Tint.Resource.Indices == Mixed->Indices &&
            Tint.Resource.Vertices->size() == Mixed->Vertices.size() &&
            Tint.Resource.Vertices->at(1).Tint == Palette->GetTint(SlateSlot) &&
            Tint.Resource.Vertices->at(3).Tint == Palette->GetTint(Palette->Find(Copper)),
            "project tint mode blends original vertices with the default draw shape");

        RenderStage Stage({29, 4}, Palette, TerrainMaterialRenderMode::PrimitivePbr);
        Stage.Stage({}, Candidates).value()->Install(1);
        const auto Ready = Stage.Capture();
        const auto &First = Ready->Chunks.at({-1, 0, 0});
        const auto &Indices = *First.Resource.Indices;
        const auto &Vertices = *First.Resource.Vertices;
        const auto &Materials = *First.Draw.Primitives;
        const auto &Second = Ready->Chunks.at({0, 0, 0});
        constexpr std::array<std::uint8_t, 4> White{255, 255, 255, 255};
        Require(Indices == std::vector<std::uint32_t>{2, 1, 3, 0, 4, 5} && Vertices.size() == 6 &&
            Vertices[4].Position == Vertices[1].Position && Vertices[4].Normal == Vertices[1].Normal &&
            Vertices[5].Position == Vertices[2].Position && Vertices[5].Normal == Vertices[2].Normal &&
            Vertices[1].Tint == Palette->GetTint(SlateSlot) && Vertices[4].Tint == White &&
            Materials.size() == 2 && Materials[0].FirstIndex == 0 && Materials[0].IndexCount == 3 &&
            Materials[1].FirstIndex == 3 && Materials[1].IndexCount == 3 &&
            Materials[1].Material == Palette->GetMaterial(SlateSlot) &&
            !Second.Draw.Primitives && Second.Draw.Material == Palette->GetMaterial(Palette->Find(Copper)) &&
            Second.Resource.Vertices->front().Tint == White,
            "material ranges preserve exact geometry and single-range mapped chunks");
        auto Publication = NewPublication(1, 0);
        AppendTerrainRenderState(Publication, {}, Ready);
        RenderProjection Projection;
        Require(Projection.Apply(Publication).VertexUploadBytes == 9 * sizeof(RenderVertex) + 9 * sizeof(std::uint32_t) &&
            Projection.GetTerrainDraws().size() == 2 &&
            Projection.GetTerrainDraws().front().Primitives->size() == 2 && Ready->AdmissionBytes == Ready->Bytes,
            "public projection receives bounded geometry and PBR ranges");

        constexpr std::size_t Floor = 31u * 1024 * 1024;
        auto Reserve = Candidates;
        Reserve[0].RenderBytesFloor = Floor;
        Reserve[1].RenderBytesFloor = Floor;
        Require(Stage.Stage({}, Reserve).error() == EditStatus::LimitExceeded && Stage.Capture() == Ready,
            "admission floor failure preserves the installed state");
        Reserve[1].RenderBytesFloor = 0;
        Stage.Stage({}, Reserve).value()->Install(2);
        Require(Stage.Capture()->Bytes == Ready->Bytes &&
            Stage.Capture()->AdmissionBytes == Floor + Second.Bytes, "reserved bytes are separate from actual bytes");
        Stage.Stage({}, Candidates).value()->Install(3);
        Require(Stage.Capture()->AdmissionBytes == Ready->Bytes, "replacement retires the reserved budget");

        const auto BeforePaint = Stage.Capture();
        auto PaintBase = NewPublication(2, 0);
        AppendTerrainRenderState(PaintBase, {}, BeforePaint);
        static_cast<void>(Projection.Apply(PaintBase));
        const std::array Paint{ChunkPublicationCandidate{{0, 0, 0}, MakeTriangle({Slate, Slate, Slate}), {}, false}};
        Stage.Stage({}, Paint).value()->Install(4);
        auto PaintPublication = NewPublication(3, 2);
        AppendTerrainRenderState(PaintPublication, BeforePaint, Stage.Capture());
        const auto PaintChanges = Projection.Apply(PaintPublication);
        Require(PaintChanges.MeshesCreated == 1 && PaintChanges.MeshesRemoved == 1 &&
            Stage.Capture()->Chunks.at({-1, 0, 0}).Resource.Mesh == BeforePaint->Chunks.at({-1, 0, 0}).Resource.Mesh &&
            Stage.Capture()->Chunks.at({0, 0, 0}).Draw.Material == Palette->GetMaterial(SlateSlot),
            "paint publishes one chunk with its current factor assignment");
    }

    void TestChunkMaterialLimit(AssetService &Assets) {
        const std::array Bindings{
            TerrainMaterialBinding{5, SlateAsset}, TerrainMaterialBinding{1, SlateAsset},
            TerrainMaterialBinding{4, SlateAsset}, TerrainMaterialBinding{2, SlateAsset},
            TerrainMaterialBinding{3, SlateAsset}};
        const auto Palette = TerrainMaterialPalette::Resolve(Assets, Bindings).value();
        auto Surface = std::make_shared<SurfaceMesh>();
        for (MaterialId Id = 5; Id > 0; --Id) {
            const auto Triangle = MakeTriangle({Id, Id, Id});
            const auto Base = static_cast<std::uint32_t>(Surface->Vertices.size());
            Surface->Vertices.insert(Surface->Vertices.end(), Triangle->Vertices.begin(), Triangle->Vertices.end());
            for (const auto Index : Triangle->Indices) Surface->Indices.push_back(Base + Index);
        }
        const std::array Candidates{ChunkPublicationCandidate{{0, 0, 0}, Surface, {}, true}};
        RenderStage Stage({29, 4}, Palette, TerrainMaterialRenderMode::PrimitivePbr);
        Stage.Stage({}, Candidates).value()->Install(1);
        const auto &Chunk = Stage.Capture()->Chunks.begin()->second;
        Require(Chunk.Draw.Primitives->size() == TerrainMaterialPalette::MaximumChunkMaterials + 1 &&
            Chunk.Draw.Primitives->front().IndexCount == 3 &&
            Chunk.Resource.Vertices->at(Chunk.Resource.Indices->front()).Tint == Palette->GetTint(Palette->Find(5)) &&
            Chunk.Resource.Indices->at(3) == 12 && Chunk.Resource.Indices->at(12) == 3 &&
            Chunk.Resource.Vertices->size() == Surface->Vertices.size(), "higher stable IDs fall back deterministically");
    }
}

int main(int Argc, char **Argv) try {
    TestOwnedProjection();
    BootstrapNativeRuntimeSchema();
    AssetService Assets;
    LoadProbeAssets(Assets);
    TestTerrainMaterialBindings(Assets);
    TestTerrainMaterials(Assets, MakeProbePalette(Assets));
    TestChunkMaterialLimit(Assets);
    if (Argc == 3 && std::string_view(Argv[1]) == "--material-probe") {
        const std::array Bindings{TerrainMaterialBinding{1, CopperAsset}, TerrainMaterialBinding{2, SlateAsset}};
        gargantuan::terrain::test::RunTerrainMaterialProbe(Argv[2], TerrainMaterialPalette::Resolve(Assets, Bindings).value());
    }
    std::cout << "[Terrain:RenderTests] Passed\n";
    return 0;
} catch (const std::exception &Error) {
    std::cerr << "[Terrain:RenderTests] " << Error.what() << '\n';
    return 1;
}
