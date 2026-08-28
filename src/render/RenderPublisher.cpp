// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/render/RenderExtractor.hpp"

#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/DeformableBody.hpp"
#include "gargantuan/classes/MeshPart.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/Sky.hpp"
#include "gargantuan/classes/WorldRoot.hpp"
#include "gargantuan/environment/EnvironmentSemantics.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "gargantuan/services/Lighting.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_inverse.hpp>

namespace gargantuan {
	namespace {
		using ProfileClock = std::chrono::steady_clock;

		std::uint64_t ProfileNanoseconds(ProfileClock::duration Duration) {
			return static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(Duration).count()
			);
		}

		bool IsFinite(const glm::vec3 &Value) {
			return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
		}

		bool IsFinite(const glm::vec4 &Value) {
			return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z) && std::isfinite(Value.w);
		}

		bool IsFinite(const glm::mat4 &Value) {
			for (glm::length_t Column = 0; Column < 4; ++Column)
				if (!IsFinite(Value[Column])) return false;
			return true;
		}

		bool Equal(const glm::vec3 &Left, const glm::vec3 &Right) {
			return Left.x == Right.x && Left.y == Right.y && Left.z == Right.z;
		}

		bool Equal(const RenderEnvironmentState &Left, const RenderEnvironmentState &Right) {
			if (!Equal(Left.AmbientColor, Right.AmbientColor) || !Equal(Left.SunDirection, Right.SunDirection) ||
				!Equal(Left.SunColor, Right.SunColor) || Left.SunIntensity != Right.SunIntensity ||
				Left.ExposureMultiplier != Right.ExposureMultiplier ||
				!Equal(Left.EnvironmentColor, Right.EnvironmentColor) || Left.Fog.Enabled != Right.Fog.Enabled ||
				!Equal(Left.Fog.Color, Right.Fog.Color) || Left.Fog.Start != Right.Fog.Start ||
				Left.Fog.End != Right.Fog.End || Left.Sky.has_value() != Right.Sky.has_value())
				return false;
			if (!Left.Sky) return true;
			if (Left.Sky->FaceDimension != Right.Sky->FaceDimension) return false;
			for (std::size_t Index = 0; Index < Left.Sky->Faces.size(); ++Index)
				if (Left.Sky->Faces[Index].Texture != Right.Sky->Faces[Index].Texture ||
					Left.Sky->Faces[Index].ContentRevision != Right.Sky->Faces[Index].ContentRevision)
					return false;
			return true;
		}

		std::optional<RenderGeometry> GetGeometry(Enums::PartType Shape) {
			switch (Shape) {
				case Enums::PartType::Block: return RenderGeometry::Block;
				case Enums::PartType::Ball: return RenderGeometry::Ball;
				case Enums::PartType::Cylinder: return RenderGeometry::Cylinder;
				case Enums::PartType::Wedge: return RenderGeometry::Wedge;
				case Enums::PartType::CornerWedge: return RenderGeometry::CornerWedge;
			}
			return std::nullopt;
		}

		RenderCameraSnapshot BuildCamera(
			const RenderCameraInput &Input,
			std::uint32_t ViewportWidth,
			std::uint32_t ViewportHeight
		) {
			if (ViewportWidth == 0 || ViewportHeight == 0)
				throw std::invalid_argument("RenderPublication viewport dimensions must be nonzero");
			if (!IsFinite(Input.Position) || !IsFinite(Input.LookDirection) || !IsFinite(Input.UpDirection))
				throw std::invalid_argument("RenderPublication camera vectors must be finite");
			if (!std::isfinite(Input.VerticalFieldOfView) || Input.VerticalFieldOfView <= 0.0f ||
				Input.VerticalFieldOfView >= 180.0f)
				throw std::invalid_argument("RenderPublication camera field of view is invalid");
			if (!std::isfinite(Input.NearPlane) || !std::isfinite(Input.FarPlane) || Input.NearPlane <= 0.0f ||
				Input.FarPlane <= Input.NearPlane)
				throw std::invalid_argument("RenderPublication camera clipping planes are invalid");
			if (glm::length(Input.LookDirection) < 1e-6f || glm::length(Input.UpDirection) < 1e-6f)
				throw std::invalid_argument("RenderPublication camera directions must be nonzero");
			const auto Look = glm::normalize(Input.LookDirection);
			const auto CandidateRight = glm::cross(Look, glm::normalize(Input.UpDirection));
			if (glm::length(CandidateRight) < 1e-6f)
				throw std::invalid_argument("RenderPublication camera look and up directions are collinear");
			const auto Right = glm::normalize(CandidateRight);
			const auto Up = glm::normalize(glm::cross(Right, Look));
			const auto Aspect = static_cast<float>(ViewportWidth) / static_cast<float>(ViewportHeight);
			RenderCameraSnapshot Result;
			Result.Position = Input.Position;
			Result.RightDirection = Right;
			Result.UpDirection = Up;
			Result.LookDirection = Look;
			Result.VerticalFieldOfView = Input.VerticalFieldOfView;
			Result.NearPlane = Input.NearPlane;
			Result.FarPlane = Input.FarPlane;
			Result.ViewMatrix = glm::lookAt(Input.Position, Input.Position + Look, Up);
			Result.ProjectionMatrix = glm::perspective(glm::radians(Input.VerticalFieldOfView), Aspect, Input.NearPlane, Input.FarPlane);
			Result.ViewProjectionMatrix = Result.ProjectionMatrix * Result.ViewMatrix;
			if (!IsFinite(Result.ViewMatrix) || !IsFinite(Result.ProjectionMatrix) || !IsFinite(Result.ViewProjectionMatrix))
				throw std::invalid_argument("RenderPublication camera matrices are not finite");
			return Result;
		}

		std::optional<RenderItem> ExtractItem(const std::shared_ptr<Part> &PartValue, RenderExtractionDiagnostic &Diagnostic) {
			const auto Object = PartValue ? PartValue->GetObjectId() : ObjectId{};
			if (!PartValue || PartValue->GetDestroyed() || PartValue->IsDestroying()) {
				Diagnostic = {RenderExtractionIssue::DeadObject, Object, "Skipped a dead renderable object"};
				return std::nullopt;
			}
			if (!Object.IsValid() || ObjectRegistry::Get().Lookup(Object).get() != PartValue.get()) {
				Diagnostic = {RenderExtractionIssue::StaleObjectId, Object, "Skipped a renderable object with stale identity"};
				return std::nullopt;
			}
			const auto Geometry = GetGeometry(PartValue->GetShape());
			if (!Geometry) {
				Diagnostic = {RenderExtractionIssue::UnsupportedGeometry, Object, "Skipped a Part with unsupported geometry"};
				return std::nullopt;
			}
			const auto Frame = PartValue->GetCFrame();
			const auto Size = PartValue->GetSize();
			if (!IsFinite(Frame.Position) || !IsFinite(glm::vec4(Frame.Rotation[0], 0.0f)) ||
				!IsFinite(glm::vec4(Frame.Rotation[1], 0.0f)) || !IsFinite(glm::vec4(Frame.Rotation[2], 0.0f)) ||
				!IsFinite(Size) || std::abs(Size.x) < 1e-6f || std::abs(Size.y) < 1e-6f || std::abs(Size.z) < 1e-6f) {
				Diagnostic = {RenderExtractionIssue::InvalidTransform, Object, "Skipped a primitive with an invalid transform"};
				return std::nullopt;
			}
			const auto ColorValue = static_cast<glm::vec3>(PartValue->GetColor());
			const glm::vec4 Color(ColorValue, 1.0f - PartValue->GetTransparency());
			if (!IsFinite(Color)) {
				Diagnostic = {RenderExtractionIssue::InvalidVisualState, Object, "Skipped a primitive with invalid visual state"};
				return std::nullopt;
			}
			const auto Model = glm::translate(glm::mat4(1.0f), Frame.Position) * glm::mat4(Frame.Rotation) *
				glm::scale(glm::mat4(1.0f), Size);
			const auto Inverse = glm::inverse(Model);
			if (!IsFinite(Model) || !IsFinite(Inverse)) {
				Diagnostic = {RenderExtractionIssue::InvalidTransform, Object, "Skipped a primitive with a non-invertible transform"};
				return std::nullopt;
			}
			return RenderItem{Object, *Geometry, Model, Inverse, Color, PartValue->GetCastShadow()};
		}

		RenderMaterialState BuildMaterial(const RenderItem &Item) {
			RenderMaterialState Material;
			Material.BaseColorFactor = Item.Color;
			Material.OpacityMode = Item.Color.a < 1.0f ? RenderOpacityMode::Transparent : RenderOpacityMode::Opaque;
			return Material;
		}

		struct ExtractedMeshPart {
			RenderItem Item;
			RenderMeshIdentity Mesh;
			RenderMaterialState Material;
			std::shared_ptr<const std::vector<RenderPrimitiveMaterialState>> Primitives;
		};

		std::optional<ExtractedMeshPart> ExtractMeshPart(
			const WorldRoot &World,
			const std::shared_ptr<MeshPart> &PartValue,
			RenderExtractionDiagnostic &Diagnostic
		) {
			const auto Object = PartValue ? PartValue->GetObjectId() : ObjectId{};
			if (!PartValue || PartValue->GetDestroyed() || PartValue->IsDestroying()) {
				Diagnostic = {RenderExtractionIssue::DeadObject, Object, "Skipped a dead MeshPart"};
				return std::nullopt;
			}
			if (!Object.IsValid() || ObjectRegistry::Get().Lookup(Object).get() != PartValue.get()) {
				Diagnostic = {RenderExtractionIssue::StaleObjectId, Object, "Skipped a MeshPart with stale identity"};
				return std::nullopt;
			}
			const auto Frame = PartValue->GetCFrame();
			const auto Size = PartValue->GetSize();
			if (!IsFinite(Frame.Position) || !IsFinite(glm::vec4(Frame.Rotation[0], 0.0f)) ||
				!IsFinite(glm::vec4(Frame.Rotation[1], 0.0f)) || !IsFinite(glm::vec4(Frame.Rotation[2], 0.0f)) ||
				!IsFinite(Size) || std::abs(Size.x) < 1e-6f || std::abs(Size.y) < 1e-6f || std::abs(Size.z) < 1e-6f) {
				Diagnostic = {RenderExtractionIssue::InvalidTransform, Object, "Skipped a MeshPart with an invalid transform"};
				return std::nullopt;
			}
			const auto ColorValue = static_cast<glm::vec3>(PartValue->GetColor());
			const glm::vec4 Color(ColorValue, 1.0f - PartValue->GetTransparency());
			if (!IsFinite(Color)) {
				Diagnostic = {RenderExtractionIssue::InvalidVisualState, Object, "Skipped a MeshPart with invalid visual state"};
				return std::nullopt;
			}
			const auto Model = glm::translate(glm::mat4(1.0f), Frame.Position) * glm::mat4(Frame.Rotation) *
				glm::scale(glm::mat4(1.0f), Size);
			const auto Inverse = glm::inverse(Model);
			if (!IsFinite(Model) || !IsFinite(Inverse)) {
				Diagnostic = {RenderExtractionIssue::InvalidTransform, Object, "Skipped a MeshPart with a non-invertible transform"};
				return std::nullopt;
			}
			auto WorldValue = World.GetDataModel();
			auto Assets = WorldValue ? std::dynamic_pointer_cast<AssetService>(WorldValue->GetService("AssetService")) : nullptr;
			auto Mesh = Assets ? Assets->ResolveMeshResource(PartValue->GetMesh()) : std::nullopt;
			if (!Mesh || !Mesh->Value.Indices || Mesh->Value.Indices->empty()) {
				Diagnostic = {RenderExtractionIssue::UnsupportedGeometry, Object,
					"Skipped a MeshPart whose Mesh asset is missing or unavailable"};
				return std::nullopt;
			}

			const RenderItem Item{Object, RenderGeometry::Block, Model, Inverse, Color, PartValue->GetCastShadow()};
			auto ResolveMaterial = [&](std::string_view Reference) {
				auto Resolved = Assets->ResolveMaterial(Reference);
				auto Material = Resolved ? Resolved->RenderState : RenderMaterialState{};
				Material.BaseColorFactor *= Color;
				if (Material.OpacityMode == RenderOpacityMode::Opaque && Material.BaseColorFactor.a < 1.0f)
					Material.OpacityMode = RenderOpacityMode::Transparent;
				return Material;
			};
			auto PrimitiveStates = std::make_shared<std::vector<RenderPrimitiveMaterialState>>();
			if (!PartValue->GetMaterial().empty()) {
				PrimitiveStates->push_back({0, static_cast<std::uint32_t>(Mesh->Value.Indices->size()),
					ResolveMaterial(PartValue->GetMaterial())});
			} else if (Mesh->Value.Primitives && !Mesh->Value.Primitives->empty()) {
				PrimitiveStates->reserve(Mesh->Value.Primitives->size());
				for (const auto &Primitive : *Mesh->Value.Primitives) {
					const auto Reference = Primitive.Material ? AssetReference::FromAssetId(*Primitive.Material).Value : std::string{};
					PrimitiveStates->push_back({
						Primitive.FirstIndex, Primitive.IndexCount, ResolveMaterial(Reference),
					});
				}
			} else PrimitiveStates->push_back({0, static_cast<std::uint32_t>(Mesh->Value.Indices->size()), ResolveMaterial({})});
			return ExtractedMeshPart{Item, Mesh->Mesh, PrimitiveStates->front().Material, PrimitiveStates};
		}

		struct ExtractedDeformable {
			RenderItem Item;
			RenderMeshIdentity Mesh;
			std::uint64_t TopologyRevision = 0;
			std::uint64_t VertexRevision = 0;
			std::shared_ptr<const std::vector<RenderVertex>> Vertices;
			std::shared_ptr<const std::vector<std::uint32_t>> Indices;
			RenderBounds Bounds;
			bool Visible = true;
		};

		[[nodiscard]] RenderMeshIdentity GetDeformableMeshIdentity(ObjectId Object) {
			return {
				.Slot = (static_cast<std::uint64_t>(Object.Generation) << 32) | Object.Slot,
				.Generation = 1,
			};
		}

		[[nodiscard]] std::optional<ExtractedDeformable> ExtractDeformable(
			const WorldRoot &World,
			const std::shared_ptr<DeformableBody> &Body,
			RenderExtractionDiagnostic &Diagnostic
		) {
			const auto Object = Body ? Body->GetObjectId() : ObjectId{};
			if (!Body || Body->GetDestroyed() || Body->IsDestroying() ||
				Body->FindFirstAncestorWhichIsA("WorldRoot").get() != &World) {
				Diagnostic = {RenderExtractionIssue::DeadObject, Object, "Skipped a dead deformable object"};
				return std::nullopt;
			}
			if (!Object.IsValid() || ObjectRegistry::Get().Lookup(Object).get() != Body.get()) {
				Diagnostic = {RenderExtractionIssue::StaleObjectId, Object, "Skipped a deformable object with stale identity"};
				return std::nullopt;
			}
			const auto State = World.GetDeformableState(Object);
			if (!State || State->TopologyRevision == 0 || State->VertexRevision == 0 || !State->Positions ||
				!State->Indices || State->Positions->empty() || State->Indices->empty() || State->Indices->size() % 3 != 0) {
				Diagnostic = {RenderExtractionIssue::UnsupportedGeometry, Object, "Skipped an incomplete deformable mesh"};
				return std::nullopt;
			}

			auto Vertices = std::make_shared<std::vector<RenderVertex>>(State->Positions->size());
			RenderBounds Bounds{State->Positions->front(), State->Positions->front()};
			for (std::size_t Index = 0; Index < State->Positions->size(); ++Index) {
				const auto Position = (*State->Positions)[Index];
				if (!IsFinite(Position)) {
					Diagnostic = {RenderExtractionIssue::InvalidTransform, Object, "Skipped non-finite deformable vertices"};
					return std::nullopt;
				}
				(*Vertices)[Index].Position = Position;
				(*Vertices)[Index].Normal = {};
				Bounds.Minimum = glm::min(Bounds.Minimum, Position);
				Bounds.Maximum = glm::max(Bounds.Maximum, Position);
			}
			for (std::size_t Index = 0; Index < State->Indices->size(); Index += 3) {
				const auto A = (*State->Indices)[Index];
				const auto B = (*State->Indices)[Index + 1];
				const auto C = (*State->Indices)[Index + 2];
				if (A >= Vertices->size() || B >= Vertices->size() || C >= Vertices->size()) {
					Diagnostic = {RenderExtractionIssue::UnsupportedGeometry, Object, "Skipped out-of-range deformable topology"};
					return std::nullopt;
				}
				const auto Normal = glm::cross((*Vertices)[B].Position - (*Vertices)[A].Position, (*Vertices)[C].Position - (*Vertices)[A].Position);
				if (glm::dot(Normal, Normal) <= 1e-12f) continue;
				(*Vertices)[A].Normal += Normal;
				(*Vertices)[B].Normal += Normal;
				(*Vertices)[C].Normal += Normal;
			}
			const auto Extent = glm::max(Bounds.Maximum - Bounds.Minimum, glm::vec3(1e-6f));
			for (auto &Vertex : *Vertices) {
				Vertex.Normal = glm::dot(Vertex.Normal, Vertex.Normal) > 1e-12f ? glm::normalize(Vertex.Normal) : glm::vec3(0.0f, 1.0f, 0.0f);
				Vertex.TextureCoordinate = {
					(Vertex.Position.x - Bounds.Minimum.x) / Extent.x,
					(Vertex.Position.y - Bounds.Minimum.y) / Extent.y,
				};
			}

			const auto ColorValue = static_cast<glm::vec3>(Body->GetColor());
			const glm::vec4 Color(ColorValue, 1.0f - Body->GetTransparency());
			if (!IsFinite(Color)) {
				Diagnostic = {RenderExtractionIssue::InvalidVisualState, Object, "Skipped invalid deformable visual state"};
				return std::nullopt;
			}
			const glm::mat4 Identity(1.0f);
			return ExtractedDeformable{
				.Item = {Object, RenderGeometry::Block, Identity, Identity, Color, Body->GetCastShadow()},
				.Mesh = GetDeformableMeshIdentity(Object),
				.TopologyRevision = State->TopologyRevision,
				.VertexRevision = State->VertexRevision,
				.Vertices = std::move(Vertices),
				.Indices = State->Indices,
				.Bounds = Bounds,
				.Visible = Body->GetVisible(),
			};
		}

		std::size_t AddBounded(std::size_t Left, std::size_t Right) {
			if (Right > std::numeric_limits<std::size_t>::max() - Left)
				return std::numeric_limits<std::size_t>::max();
			return Left + Right;
		}

		std::size_t MultiplyBounded(std::size_t Left, std::size_t Right) {
			if (Left != 0 && Right > std::numeric_limits<std::size_t>::max() / Left)
				return std::numeric_limits<std::size_t>::max();
			return Left * Right;
		}

		std::size_t EstimatePublicationBytes(const RenderPublication &Publication) {
			std::size_t Result = sizeof(RenderPublication);
			Result = AddBounded(Result, MultiplyBounded(Publication.Creates.size(), sizeof(RenderObjectCreate)));
			Result = AddBounded(Result, MultiplyBounded(Publication.Updates.size(), sizeof(RenderObjectUpdate)));
			Result = AddBounded(Result, MultiplyBounded(Publication.Removes.size(), sizeof(RenderObjectRemove)));
			Result = AddBounded(Result, MultiplyBounded(Publication.MeshCreates.size(), sizeof(RenderMeshCreate)));
			Result = AddBounded(
				Result,
				MultiplyBounded(Publication.MeshVertexUpdates.size(), sizeof(RenderMeshVertexUpdate))
			);
			Result = AddBounded(Result, MultiplyBounded(Publication.MeshRemoves.size(), sizeof(RenderMeshRemove)));
			Result = AddBounded(Result, MultiplyBounded(Publication.TextureCreates.size(), sizeof(RenderTextureCreate)));
			Result = AddBounded(Result, MultiplyBounded(Publication.TextureUpdates.size(), sizeof(RenderTextureUpdate)));
			Result = AddBounded(Result, MultiplyBounded(Publication.TextureRemoves.size(), sizeof(RenderTextureRemove)));
			for (const auto &Create : Publication.MeshCreates)
				if (Create.Vertices && Create.Indices)
					Result = AddBounded(
						Result,
						AddBounded(
							MultiplyBounded(Create.Vertices->size(), sizeof(RenderVertex)),
							MultiplyBounded(Create.Indices->size(), sizeof(std::uint32_t))
						)
					);
			for (const auto &Update : Publication.MeshVertexUpdates)
				if (Update.Vertices)
					Result = AddBounded(Result, MultiplyBounded(Update.Vertices->size(), sizeof(RenderVertex)));
			Result = AddBounded(Result, MultiplyBounded(Publication.AnimationPoseUpdates.size(),
				sizeof(RenderAnimationPoseUpdate)));
			Result = AddBounded(Result, MultiplyBounded(Publication.AnimationPoseRemoves.size(),
				sizeof(RenderAnimationPoseRemove)));
			for (const auto &Update : Publication.AnimationPoseUpdates)
				if (Update.BonePalette) Result = AddBounded(Result,
					MultiplyBounded(Update.BonePalette->size(), sizeof(glm::mat4)));
			for (const auto &Create : Publication.TextureCreates)
				if (Create.Pixels) Result = AddBounded(Result, Create.Pixels->size());
			for (const auto &Update : Publication.TextureUpdates)
				if (Update.Pixels) Result = AddBounded(Result, Update.Pixels->size());
			for (const auto &Batch : Publication.GetUi().Batches)
				Result = AddBounded(
					Result,
					AddBounded(
						sizeof(RenderUiBatch),
						AddBounded(
							MultiplyBounded(Batch.Vertices.size(), sizeof(RenderUiVertex)),
							MultiplyBounded(Batch.Indices.size(), sizeof(std::uint32_t))
						)
					)
				);
			for (const auto &Diagnostic : Publication.Diagnostics)
				Result = AddBounded(Result, Diagnostic.Message.size());
			return Result;
		}
	}

	RenderPublisher::RenderPublisher(RenderDirtyAccumulator *DirtyAccumulator)
		: Dirty(DirtyAccumulator ? DirtyAccumulator : &RenderDirtyAccumulator::Get()),
		  DirtyConsumer(Dirty->CreateConsumer()) {
		PendingAnimationObjects.reserve(64);
	}

	RenderPublisher::~RenderPublisher() {
		if (Dirty) Dirty->ReleaseConsumer(DirtyConsumer);
	}

	RenderEnvironmentState RenderPublisher::BuildEnvironmentState(
		const WorldRoot &World, std::vector<RenderExtractionDiagnostic> &Diagnostics
	) {
		RenderEnvironmentState Result;
		auto DataModelValue = World.GetDataModel();
		if (!DataModelValue) return Result;

		auto LightingService = DataModelValue->FindService("Lighting");
		auto LightingValue = LightingService ? std::dynamic_pointer_cast<Lighting>(*LightingService) : nullptr;
		if (!LightingValue) return Result;

		const auto Sun = ComputeEnvironmentSunState(
			LightingValue->GetClockTime(), LightingValue->GetBrightness(), LightingValue->GetSunColor()
		);
		Result.AmbientColor = static_cast<glm::vec3>(LightingValue->GetAmbient());
		Result.SunDirection = Sun.Direction;
		Result.SunColor = Sun.Color;
		Result.SunIntensity = Sun.Intensity;
		Result.ExposureMultiplier = ComputeEnvironmentExposure(LightingValue->GetExposureCompensation());
		Result.EnvironmentColor = static_cast<glm::vec3>(LightingValue->GetEnvironmentColor());
		Result.Fog = {
			.Enabled = LightingValue->GetFogEnabled(),
			.Color = static_cast<glm::vec3>(LightingValue->GetFogColor()),
			.Start = LightingValue->GetFogStart(),
			.End = LightingValue->GetFogEnd(),
		};

		std::vector<std::shared_ptr<Sky>> EnabledSkies;
		for (const auto &Child : LightingValue->GetChildren()) {
			auto SkyValue = std::dynamic_pointer_cast<Sky>(Child);
			if (SkyValue && !SkyValue->GetDestroyed() && !SkyValue->IsDestroying() && SkyValue->GetEnabled())
				EnabledSkies.push_back(std::move(SkyValue));
		}
		std::ranges::sort(EnabledSkies, {}, [](const auto &Value) { return Value->GetObjectId(); });
		if (EnabledSkies.empty()) {
			LastKnownGoodSky.reset();
			return Result;
		}
		const auto &EffectiveSky = EnabledSkies.front();
		if (EnabledSkies.size() > 1)
			Diagnostics.push_back({
				.Issue = RenderExtractionIssue::InvalidSky,
				.Object = EffectiveSky->GetObjectId(),
				.Message =
					"[Environment:Sky] Multiple enabled direct Sky children found; the lowest ObjectId is effective",
			});

		auto AssetServiceValue = DataModelValue->FindService("AssetService");
		auto Assets = AssetServiceValue ? std::dynamic_pointer_cast<AssetService>(*AssetServiceValue) : nullptr;
		const std::array<std::string, 6> References{
			EffectiveSky->GetSkyboxPositiveX(),
			EffectiveSky->GetSkyboxNegativeX(),
			EffectiveSky->GetSkyboxPositiveY(),
			EffectiveSky->GetSkyboxNegativeY(),
			EffectiveSky->GetSkyboxPositiveZ(),
			EffectiveSky->GetSkyboxNegativeZ(),
		};
		RenderSkyState Candidate;
		bool Valid = Assets != nullptr;
		for (std::size_t Index = 0; Valid && Index < References.size(); ++Index) {
			const auto Record = Assets->GetAsset(References[Index]);
			const auto *Image = Record && Record->Asset ? std::get_if<ImportedImage>(Record->Asset.get()) : nullptr;
			const auto Available = Record && (Record->State == AssetState::Ready || Record->State == AssetState::Stale);
			const auto Resource = Record && Record->Kind == AssetKind::Image && Available && Image &&
										  Image->Width != 0 && Image->Width == Image->Height
									  ? Assets->ResolveImage(References[Index])
									  : std::nullopt;
			if (!Resource || !Resource->Texture.IsValid() || Resource->ContentRevision == 0 ||
				Resource->Width != Resource->Height || Resource->Width != Image->Width ||
				(Candidate.FaceDimension != 0 && Candidate.FaceDimension != Resource->Width)) {
				Valid = false;
				break;
			}
			Candidate.FaceDimension = Resource->Width;
			Candidate.Faces[Index] = {Resource->Texture, Resource->ContentRevision};
		}

		if (Valid) {
			Result.Sky = Candidate;
			LastKnownGoodSky = PublishedSky{EffectiveSky->GetObjectId(), Candidate};
			return Result;
		}

		Diagnostics.push_back({
			.Issue = RenderExtractionIssue::InvalidSky,
			.Object = EffectiveSky->GetObjectId(),
			.Message =
				"[Environment:Sky] Effective Sky is incomplete, unavailable, non-square, or dimension-mismatched",
		});
		const bool LastKnownGoodResident =
			LastKnownGoodSky && std::ranges::all_of(LastKnownGoodSky->State.Faces, [&](const RenderSkyFaceState &Face) {
				const auto Existing = PublishedTextures.find(Face.Texture);
				return Existing != PublishedTextures.end() && Existing->second.Revision == Face.ContentRevision &&
					   Existing->second.Width == LastKnownGoodSky->State.FaceDimension &&
					   Existing->second.Height == LastKnownGoodSky->State.FaceDimension;
			});
		if (LastKnownGoodResident && LastKnownGoodSky->Source == EffectiveSky->GetObjectId()) {
			Result.Sky = LastKnownGoodSky->State;
			Diagnostics.push_back({
				.Issue = RenderExtractionIssue::InvalidSky,
				.Object = EffectiveSky->GetObjectId(),
				.Message = "[Environment:Sky] Retaining the last-known-good coherent Sky",
			});
		} else
			LastKnownGoodSky.reset();
		return Result;
	}

	void RenderPublisher::SetUiFrame(RenderUiFrame UiFrame, ObjectId Source, std::uint64_t SourceGeneration) {
		SetUiFrame(std::make_shared<const RenderUiFrame>(std::move(UiFrame)), Source, SourceGeneration);
	}

	void RenderPublisher::SetUiFrame(
		std::shared_ptr<const RenderUiFrame> UiFrame,
		ObjectId Source,
		std::uint64_t SourceGeneration
	) {
		if (!UiFrame) throw std::invalid_argument("Render publisher requires an immutable UI frame");
		std::size_t Bytes = 0;
		for (const auto &Batch : UiFrame->Batches)
			Bytes = AddBounded(
				Bytes,
				AddBounded(
					sizeof(RenderUiBatch),
					AddBounded(
						MultiplyBounded(Batch.Vertices.size(), sizeof(RenderUiVertex)),
						MultiplyBounded(Batch.Indices.size(), sizeof(std::uint32_t))
					)
				)
			);
		CommittedUi = UiFrame;
		UiSource = Source;
		UiSourceGeneration = SourceGeneration;
		PendingUi = std::move(UiFrame);
		PendingUiGeometryBytes = Bytes;
		if (Scope.IsValid()) Dirty->MarkUi(Scope, Bytes);
	}

	void RenderPublisher::SetUiTextureChanges(
		std::vector<RenderTextureCreate> Creates,
		std::vector<RenderTextureUpdate> Updates,
		std::vector<RenderTextureRemove> Removes
	) {
		if (!PendingTextureCreates.empty() || !PendingTextureUpdates.empty() || !PendingTextureRemoves.empty())
			throw std::logic_error("Render publisher accepts one coherent UI texture change set per publication");
		const auto PreviousTextures = PublishedTextures;
		auto NextTextures = PublishedTextures;
		std::unordered_set<RenderTextureIdentity, RenderTextureIdentityHash> TouchedTextures;
		std::unordered_map<RenderTextureIdentity, std::size_t, RenderTextureIdentityHash> OperationCounts;
		std::unordered_map<RenderTextureIdentity, RenderTextureUpdate, RenderTextureIdentityHash> LastUpdates;
		TouchedTextures.reserve(Creates.size() + Updates.size() + Removes.size());
		for (const auto &Create : Creates) {
			const auto Expected = static_cast<std::size_t>(Create.Width) * Create.Height * 4;
			if (!Create.Texture.IsValid() || Create.Revision == 0 || Create.Width == 0 || Create.Height == 0 ||
				Create.Format != RenderTextureFormat::Rgba8Unorm || !Create.Pixels || Create.Pixels->size() != Expected ||
				NextTextures.contains(Create.Texture))
				throw std::invalid_argument("Render publisher rejects an invalid or duplicate UI texture creation");
			NextTextures.emplace(Create.Texture, PublishedTexture{
				Create.Revision, Create.Width, Create.Height, Create.Format, Create.Pixels});
			TouchedTextures.insert(Create.Texture);
			++OperationCounts[Create.Texture];
		}
		for (const auto &Update : Updates) {
			auto Existing = NextTextures.find(Update.Texture);
			const auto Expected = static_cast<std::size_t>(Update.Width) * Update.Height * 4;
			if (Existing == NextTextures.end() || Update.Revision <= Existing->second.Revision ||
				Update.X > Existing->second.Width || Update.Y > Existing->second.Height ||
				Update.Width > Existing->second.Width - Update.X || Update.Height > Existing->second.Height - Update.Y ||
				!Update.Pixels || Update.Pixels->size() != Expected)
				throw std::invalid_argument("Render publisher rejects an invalid UI texture update");
			auto Pixels = std::make_shared<std::vector<std::uint8_t>>(*Existing->second.Pixels);
			for (std::uint32_t Row = 0; Row < Update.Height; ++Row) {
				const auto SourceOffset = static_cast<std::size_t>(Row) * Update.Width * 4;
				const auto DestinationOffset =
					(static_cast<std::size_t>(Update.Y + Row) * Existing->second.Width + Update.X) * 4;
				std::copy_n(Update.Pixels->data() + SourceOffset, static_cast<std::size_t>(Update.Width) * 4,
					Pixels->data() + DestinationOffset);
			}
			Existing->second.Revision = Update.Revision;
			Existing->second.Pixels = std::shared_ptr<const std::vector<std::uint8_t>>(std::move(Pixels));
			TouchedTextures.insert(Update.Texture);
			++OperationCounts[Update.Texture];
			LastUpdates[Update.Texture] = Update;
		}
		for (const auto &Remove : Removes) {
			if (!NextTextures.erase(Remove.Texture))
				throw std::invalid_argument("Render publisher rejects a stale UI texture removal");
			TouchedTextures.insert(Remove.Texture);
			++OperationCounts[Remove.Texture];
		}

		std::vector<RenderTextureIdentity> OrderedTouched(TouchedTextures.begin(), TouchedTextures.end());
		std::ranges::sort(OrderedTouched);
		std::size_t Bytes = 0;
		for (const auto Texture : OrderedTouched) {
			const auto Previous = PreviousTextures.find(Texture);
			const auto Next = NextTextures.find(Texture);
			if (Previous == PreviousTextures.end() && Next != NextTextures.end()) {
				const auto &Value = Next->second;
				PendingTextureCreates.push_back({Texture, Value.Revision, Value.Width, Value.Height, Value.Format, Value.Pixels});
				Bytes = AddBounded(Bytes, Value.Pixels ? Value.Pixels->size() : 0);
			} else if (Previous != PreviousTextures.end() && Next == NextTextures.end()) {
				PendingTextureRemoves.push_back({Texture});
			} else if (Previous != PreviousTextures.end() && Next != NextTextures.end()) {
				if (OperationCounts.at(Texture) == 1 && LastUpdates.contains(Texture)) {
					PendingTextureUpdates.push_back(LastUpdates.at(Texture));
					Bytes = AddBounded(Bytes, LastUpdates.at(Texture).Pixels ? LastUpdates.at(Texture).Pixels->size() : 0);
				} else {
					const auto &Value = Next->second;
					PendingTextureUpdates.push_back({Texture, Value.Revision, 0, 0, Value.Width, Value.Height, Value.Pixels});
					Bytes = AddBounded(Bytes, Value.Pixels ? Value.Pixels->size() : 0);
				}
			}
		}
		PublishedTextures = std::move(NextTextures);
		PendingTextureBytes = Bytes;
		if (Scope.IsValid()) Dirty->MarkUi(Scope, PendingTextureBytes);
	}

	void RenderPublisher::SetAssetMeshChanges(
		std::vector<RenderMeshCreate> Creates,
		std::vector<RenderMeshRemove> Removes
	) {
		if (!PendingAssetMeshCreates.empty() || !PendingAssetMeshRemoves.empty())
			throw std::logic_error("Render publisher accepts one coherent asset mesh change set per publication");
		for (const auto &Remove : Removes) {
			if (!Remove.Mesh.IsValid() || !PublishedAssetMeshes.erase(Remove.Mesh))
				throw std::invalid_argument("Render publisher rejects a stale asset mesh removal");
			PendingAssetMeshRemoves.push_back(Remove);
		}
		for (const auto &Create : Creates) {
			if (!Create.Mesh.IsValid() || Create.TopologyRevision == 0 || Create.VertexRevision == 0 ||
				!Create.Vertices || Create.Vertices->empty() || !Create.Indices || Create.Indices->empty() ||
				PublishedAssetMeshes.contains(Create.Mesh))
				throw std::invalid_argument("Render publisher rejects an invalid or duplicate asset mesh creation");
			for (const auto Index : *Create.Indices) if (Index >= Create.Vertices->size())
				throw std::invalid_argument("Render publisher rejects an out-of-range asset mesh index");
			PublishedAssetMeshes.emplace(Create.Mesh, PublishedAssetMesh{
				Create.TopologyRevision, Create.VertexRevision, Create.Vertices, Create.Indices, Create.Bounds,
				Create.SkinInfluences
			});
			PendingAssetMeshCreates.push_back(Create);
		}
	}

	void RenderPublisher::SetAnimationPoseChanges(
		std::span<const RenderAnimationPoseState> Updates,
		std::span<const RenderAnimationPoseRemove> Removes
	) {
		auto HasPending = [&](ObjectId Object) {
			const auto Position = std::lower_bound(
				PendingAnimationObjects.begin(), PendingAnimationObjects.end(), Object);
			return Position != PendingAnimationObjects.end() && *Position == Object;
		};
		auto AddPending = [&](ObjectId Object) {
			PendingAnimationObjects.insert(std::lower_bound(
				PendingAnimationObjects.begin(), PendingAnimationObjects.end(), Object), Object);
		};
		for (const auto &Update : Updates) {
			const auto &Pose = Update.Pose;
			if (!Pose.Object.IsValid() || !Pose.SourceMesh.IsValid() || !Pose.PosedMesh.IsValid() ||
				Pose.SourceMesh == Pose.PosedMesh || Pose.PoseRevision == 0 || Update.TopologyRevision == 0 ||
				!Pose.BonePalette || Pose.BonePalette->empty() ||
				Pose.BonePalette->size() > AssetLimits::MaximumSkeletonBones || !Update.Vertices ||
				Update.Vertices->empty() || !Update.Indices || Update.Indices->empty() ||
				HasPending(Pose.Object))
				throw std::invalid_argument("Render publisher rejects an invalid or duplicate animation pose update");
			for (const auto &Matrix : *Pose.BonePalette) if (!IsFinite(Matrix))
				throw std::invalid_argument("Render publisher rejects a non-finite animation palette");
			for (const auto &Vertex : *Update.Vertices)
				if (!IsFinite(Vertex.Position) || !IsFinite(Vertex.Normal) || !IsFinite(Vertex.Tangent))
					throw std::invalid_argument("Render publisher rejects non-finite CPU-skinned vertices");
			for (const auto Index : *Update.Indices) if (Index >= Update.Vertices->size())
				throw std::invalid_argument("Render publisher rejects an out-of-range animated mesh index");
			if (const auto Existing = AnimationPoses.find(Pose.Object); Existing != AnimationPoses.end() &&
				Pose.PoseRevision <= Existing->second.Pose.PoseRevision)
				throw std::invalid_argument("Render publisher rejects a stale animation pose revision");
			AnimationPoses.insert_or_assign(Pose.Object, Update);
			AddPending(Pose.Object);
		}
		for (const auto &Remove : Removes) {
			if (!Remove.Object.IsValid() || HasPending(Remove.Object))
				throw std::invalid_argument("Render publisher rejects an invalid or duplicate animation pose removal");
			if (AnimationPoses.erase(Remove.Object) == 0 && !CommittedAnimationPoses.contains(Remove.Object))
				continue;
			AddPending(Remove.Object);
		}
	}

	RenderPublicationPtr RenderPublisher::Publish(
		const WorldRoot &World,
		const RenderCameraInput &CameraInput,
		std::uint32_t ViewportWidth,
		std::uint32_t ViewportHeight
	) {
		AssertAuthoritativeMutation("RenderPublication extraction");
		LastProfile = {};
		if (LastPublicationId == std::numeric_limits<RenderPublicationId>::max())
			throw std::overflow_error("RenderPublication identity exhausted and will not roll over");
		const auto CurrentScope = World.GetReplicationScopeId();
		if (!CurrentScope.IsValid()) throw std::invalid_argument("RenderPublication requires a live DataModel scope");
		const auto Camera = BuildCamera(CameraInput, ViewportWidth, ViewportHeight);

		if (PublishedWorld != &World || Scope != CurrentScope) {
			if (Scope.IsValid()) Dirty->ReleaseConsumer(DirtyConsumer, Scope);
			PublishedWorld = &World;
			Scope = CurrentScope;
			FullResyncRequested = true;
			PublishedItems.clear();
			PublishedDeformables.clear();
			CommittedAnimationPoses.clear();
			HasPublishedEnvironment = false;
			LastKnownGoodSky.reset();
		}
		const auto CaptureStart = ProfilingEnabled ? ProfileClock::now() : ProfileClock::time_point{};
		const auto DirtyBatch = Dirty->Capture(Scope, DirtyConsumer);
		if (ProfilingEnabled)
			LastProfile.DirtyCaptureNanoseconds = ProfileNanoseconds(ProfileClock::now() - CaptureStart);
		if (AddBounded(PendingUiGeometryBytes, PendingTextureBytes) > Dirty->GetLimits().MaximumUiBytes) {
			Dirty->RequestFullResync(Scope, "Pending render UI exceeds its byte limit");
			throw std::length_error("Pending render UI exceeds its byte limit");
		}

		auto FullResync = [&]() -> RenderPublicationPtr {
			auto Snapshot = FullExtractor.Extract(World, CameraInput, ViewportWidth, ViewportHeight);
			auto Result = std::make_shared<RenderPublication>();
			Result->Id = LastPublicationId + 1;
			Result->FullResync = true;
			Result->Diagnostics = Snapshot->Diagnostics;
			const auto Environment = BuildEnvironmentState(World, Result->Diagnostics);
			Result->Frame = {ViewportWidth, ViewportHeight, 1.0f, Camera, Environment};
			Result->EnvironmentChanged = true;
			for (const auto &Diagnostic : DirtyBatch.Diagnostics)
				Result->Diagnostics.push_back({RenderExtractionIssue::PublicationOverflow, {}, Diagnostic});
			Result->Creates.reserve(Snapshot->Items.size() + World.SoftBodies.size());
			std::unordered_map<ObjectId, RenderItem> Replacement;
			Replacement.reserve(Snapshot->Items.size() + World.SoftBodies.size());
			std::unordered_map<ObjectId, PublishedDeformable> DeformableReplacement;
			DeformableReplacement.reserve(World.SoftBodies.size());
			for (const auto &Item : Snapshot->Items) {
				Result->Creates.push_back({Item, std::nullopt, BuildMaterial(Item)});
				Replacement.emplace(Item.Object, Item);
			}
			for (const auto &BasePartValue : World.Parts) {
				auto MeshPartValue = std::dynamic_pointer_cast<MeshPart>(BasePartValue);
				if (!MeshPartValue) continue;
				RenderExtractionDiagnostic Diagnostic;
				auto Extracted = ExtractMeshPart(World, MeshPartValue, Diagnostic);
				if (!Extracted) {
					Result->Diagnostics.push_back(std::move(Diagnostic));
					continue;
				}
				auto DisplayMesh = Extracted->Mesh;
				if (const auto Animated = AnimationPoses.find(Extracted->Item.Object);
					Animated != AnimationPoses.end() && Animated->second.Pose.SourceMesh == Extracted->Mesh)
					DisplayMesh = Animated->second.Pose.PosedMesh;
				Result->Creates.push_back({Extracted->Item, DisplayMesh, Extracted->Material, true,
					Extracted->Primitives});
				Replacement.emplace(Extracted->Item.Object, Extracted->Item);
			}
			std::vector<std::shared_ptr<DeformableBody>> OrderedDeformables;
			OrderedDeformables.reserve(World.SoftBodies.size());
			for (const auto &Body : World.SoftBodies)
				if (Body) OrderedDeformables.push_back(Body);
			std::ranges::sort(OrderedDeformables, {}, [](const auto &Body) { return Body->GetObjectId(); });
			for (const auto &Body : OrderedDeformables) {
				RenderExtractionDiagnostic Diagnostic;
				auto Extracted = ExtractDeformable(World, Body, Diagnostic);
				if (!Extracted) {
					Result->Diagnostics.push_back(std::move(Diagnostic));
					continue;
				}
				Result->MeshCreates.push_back({
					.Mesh = Extracted->Mesh,
					.TopologyRevision = Extracted->TopologyRevision,
					.VertexRevision = Extracted->VertexRevision,
					.Vertices = Extracted->Vertices,
					.Indices = Extracted->Indices,
					.Bounds = Extracted->Bounds,
				});
				Result->Creates.push_back({
					Extracted->Item, Extracted->Mesh, BuildMaterial(Extracted->Item), Extracted->Visible
				});
				Replacement.emplace(Extracted->Item.Object, Extracted->Item);
				DeformableReplacement.emplace(
					Extracted->Item.Object,
					PublishedDeformable{Extracted->Mesh, Extracted->TopologyRevision, Extracted->VertexRevision}
				);
			}
			std::vector<std::pair<RenderMeshIdentity, PublishedAssetMesh>> OrderedAssetMeshes(
				PublishedAssetMeshes.begin(), PublishedAssetMeshes.end());
			std::ranges::sort(OrderedAssetMeshes, {}, &std::pair<RenderMeshIdentity, PublishedAssetMesh>::first);
			for (const auto &[Identity, Mesh] : OrderedAssetMeshes)
				Result->MeshCreates.push_back({Identity, Mesh.TopologyRevision, Mesh.VertexRevision,
					Mesh.Vertices, Mesh.Indices, Mesh.Bounds, Mesh.SkinInfluences});
			std::vector<std::pair<ObjectId, RenderAnimationPoseState>> OrderedAnimationPoses(
				AnimationPoses.begin(), AnimationPoses.end());
			std::ranges::sort(OrderedAnimationPoses, {},
				&std::pair<ObjectId, RenderAnimationPoseState>::first);
			for (const auto &[Object, State] : OrderedAnimationPoses) {
				(void)Object;
				Result->MeshCreates.push_back({State.Pose.PosedMesh, State.TopologyRevision,
					State.Pose.PoseRevision, State.Vertices, State.Indices, State.Bounds});
				Result->AnimationPoseUpdates.push_back(State.Pose);
			}
			Result->UiChanged = true;
			Result->SharedUi = CommittedUi;
			std::vector<std::pair<RenderTextureIdentity, PublishedTexture>> OrderedTextures(
				PublishedTextures.begin(), PublishedTextures.end());
			std::ranges::sort(OrderedTextures, {}, &std::pair<RenderTextureIdentity, PublishedTexture>::first);
			for (const auto &[Texture, State] : OrderedTextures)
				Result->TextureCreates.push_back({Texture, State.Revision, State.Width, State.Height, State.Format, State.Pixels});
			if (EstimatePublicationBytes(*Result) > Dirty->GetLimits().MaximumPublicationBytes) {
				Dirty->RequestFullResync(Scope, "Render full-resync publication exceeds its byte limit");
				throw std::length_error("Render full-resync publication exceeds its byte limit");
			}
			PublishedItems = std::move(Replacement);
			PublishedDeformables = std::move(DeformableReplacement);
			CommittedAnimationPoses = AnimationPoses;
			PublishedEnvironment = Environment;
			HasPublishedEnvironment = true;
			FullResyncRequested = false;
			LastPublicationId = Result->Id;
			++FullResyncCount;
			Dirty->Acknowledge(DirtyBatch);
			PendingUi.reset();
			PendingTextureCreates.clear();
			PendingTextureUpdates.clear();
			PendingTextureRemoves.clear();
			PendingAssetMeshCreates.clear();
			PendingAssetMeshRemoves.clear();
			PendingAnimationObjects.clear();
			PendingUiGeometryBytes = 0;
			PendingTextureBytes = 0;
			return std::shared_ptr<const RenderPublication>(std::move(Result));
		};

		if (FullResyncRequested || DirtyBatch.FullResyncRequired) return FullResync();

		const auto ExpansionStart = ProfilingEnabled ? ProfileClock::now() : ProfileClock::time_point{};
		std::vector<std::pair<ObjectId, RenderUpdateDomain>> DirtyObjects;
		DirtyObjects.reserve(DirtyBatch.Records.size() + PendingAnimationObjects.size());
		const bool HasHierarchyChanges = std::ranges::any_of(DirtyBatch.Records, [](const RenderDirtyRecord &Record) {
			return HasRenderUpdateDomain(Record.Domains, RenderUpdateDomain::Hierarchy);
		});
		if (!HasHierarchyChanges) {
			for (const auto &Record : DirtyBatch.Records) DirtyObjects.emplace_back(Record.Object, Record.Domains);
		} else {
			std::map<ObjectId, RenderUpdateDomain> Expanded;
			for (const auto &Record : DirtyBatch.Records) {
				Expanded[Record.Object] = Expanded[Record.Object] | Record.Domains;
				if (!HasRenderUpdateDomain(Record.Domains, RenderUpdateDomain::Hierarchy)) continue;
				auto Root = ObjectRegistry::Get().Lookup(Record.Object);
				if (!Root) continue;
				auto AddRenderable = [&](const std::shared_ptr<Instance> &Candidate) {
					if (!std::dynamic_pointer_cast<Part>(Candidate) &&
						!std::dynamic_pointer_cast<MeshPart>(Candidate) &&
						!std::dynamic_pointer_cast<DeformableBody>(Candidate)) return;
					const auto Object = Candidate->GetObjectId();
					Expanded[Object] = Expanded[Object] | RenderUpdateDomain::Transform | RenderUpdateDomain::Material |
						RenderUpdateDomain::Visibility | RenderUpdateDomain::Geometry | RenderUpdateDomain::Hierarchy;
				};
				AddRenderable(Root);
				for (const auto &Descendant : Root->GetDescendants()) AddRenderable(Descendant);
			}
			DirtyObjects.assign(Expanded.begin(), Expanded.end());
			DirtyObjects.reserve(DirtyObjects.size() + PendingAnimationObjects.size());
		}
		if (!PendingAnimationObjects.empty()) {
			std::ranges::sort(DirtyObjects, {}, &std::pair<ObjectId, RenderUpdateDomain>::first);
			for (const auto Object : PendingAnimationObjects) {
				const auto Position = std::lower_bound(DirtyObjects.begin(), DirtyObjects.end(), Object,
					[](const auto &Entry, ObjectId Value) { return Entry.first < Value; });
				if (Position != DirtyObjects.end() && Position->first == Object)
					Position->second = Position->second | RenderUpdateDomain::AnimationPose;
				else DirtyObjects.insert(Position, {Object, RenderUpdateDomain::AnimationPose});
			}
		}
		if (ProfilingEnabled)
			LastProfile.DirtyExpansionNanoseconds = ProfileNanoseconds(ProfileClock::now() - ExpansionStart);
		if (DirtyObjects.size() > Dirty->GetLimits().MaximumDistinctObjects) return FullResync();

		const auto ConstructionStart = ProfilingEnabled ? ProfileClock::now() : ProfileClock::time_point{};
		auto Result = std::make_shared<RenderPublication>();
		Result->Id = LastPublicationId + 1;
		Result->BaseId = LastPublicationId;
		const bool EnvironmentDirty = !HasPublishedEnvironment || !PendingTextureCreates.empty() ||
									  !PendingTextureUpdates.empty() || !PendingTextureRemoves.empty() ||
									  std::ranges::any_of(DirtyBatch.Records, [](const RenderDirtyRecord &Record) {
										  return HasRenderUpdateDomain(Record.Domains, RenderUpdateDomain::Environment);
									  });
		auto Environment = PublishedEnvironment;
		if (EnvironmentDirty) {
			auto Candidate = BuildEnvironmentState(World, Result->Diagnostics);
			if (!HasPublishedEnvironment || !Equal(Candidate, PublishedEnvironment)) {
				Environment = std::move(Candidate);
				Result->EnvironmentChanged = true;
			}
		}
		Result->Frame = {ViewportWidth, ViewportHeight, 1.0f, Camera, Environment};
		for (const auto &Diagnostic : DirtyBatch.Diagnostics)
			Result->Diagnostics.push_back({RenderExtractionIssue::PublicationOverflow, {}, Diagnostic});
		Result->Updates.reserve(DirtyObjects.size());
		if (ProfilingEnabled)
			LastProfile.PublicationConstructionNanoseconds +=
				ProfileNanoseconds(ProfileClock::now() - ConstructionStart);
		std::vector<std::pair<ObjectId, PublishedDeformable>> DeformableCacheUpdates;
		std::vector<ObjectId> DeformableCacheRemoves;
		for (const auto &[Object, Domains] : DirtyObjects) {
			const auto ExtractionStart = ProfilingEnabled ? ProfileClock::now() : ProfileClock::time_point{};
			auto Previous = PublishedItems.find(Object);
			auto InstanceValue = ObjectRegistry::Get().Lookup(Object);
			auto PartValue = std::dynamic_pointer_cast<Part>(InstanceValue);
			auto MeshPartValue = std::dynamic_pointer_cast<MeshPart>(InstanceValue);
			auto DeformableValue = std::dynamic_pointer_cast<DeformableBody>(InstanceValue);
			if (DeformableValue) {
				const bool ShouldRender = !DeformableValue->GetDestroyed() && !DeformableValue->IsDestroying() &&
					DeformableValue->FindFirstAncestorWhichIsA("WorldRoot").get() == &World;
				RenderExtractionDiagnostic Diagnostic;
				auto Extracted = ShouldRender ? ExtractDeformable(World, DeformableValue, Diagnostic) : std::nullopt;
				if (ProfilingEnabled)
					LastProfile.FinalStateExtractionNanoseconds +=
						ProfileNanoseconds(ProfileClock::now() - ExtractionStart);
				const auto OperationStart = ProfilingEnabled ? ProfileClock::now() : ProfileClock::time_point{};
				if (!Extracted) {
					if (ShouldRender) Result->Diagnostics.push_back(std::move(Diagnostic));
					if (Previous != PublishedItems.end()) Result->Removes.push_back({Object});
					if (auto Existing = PublishedDeformables.find(Object); Existing != PublishedDeformables.end()) {
						Result->MeshRemoves.push_back({Existing->second.Mesh});
						DeformableCacheRemoves.push_back(Object);
					}
					if (ProfilingEnabled)
						LastProfile.PublicationConstructionNanoseconds +=
							ProfileNanoseconds(ProfileClock::now() - OperationStart);
					continue;
				}

				auto Existing = PublishedDeformables.find(Object);
				if (Previous == PublishedItems.end()) {
					Result->MeshCreates.push_back({
						.Mesh = Extracted->Mesh,
						.TopologyRevision = Extracted->TopologyRevision,
						.VertexRevision = Extracted->VertexRevision,
						.Vertices = Extracted->Vertices,
						.Indices = Extracted->Indices,
						.Bounds = Extracted->Bounds,
					});
					Result->Creates.push_back({
						Extracted->Item, Extracted->Mesh, BuildMaterial(Extracted->Item), Extracted->Visible
					});
				} else {
					if (Existing == PublishedDeformables.end() || Existing->second.Mesh != Extracted->Mesh ||
						Existing->second.TopologyRevision != Extracted->TopologyRevision) {
						Dirty->RequestFullResync(Scope, "Deformable topology cache changed during incremental publication");
						return FullResync();
					}
					if (Existing->second.VertexRevision != Extracted->VertexRevision)
						Result->MeshVertexUpdates.push_back({
							.Mesh = Extracted->Mesh,
							.VertexRevision = Extracted->VertexRevision,
							.FirstVertex = 0,
							.Vertices = Extracted->Vertices,
							.Bounds = Extracted->Bounds,
						});
					Result->Updates.push_back({
						Object, Domains, Extracted->Item, Extracted->Mesh, BuildMaterial(Extracted->Item), Extracted->Visible
					});
				}
				DeformableCacheUpdates.emplace_back(
					Object, PublishedDeformable{Extracted->Mesh, Extracted->TopologyRevision, Extracted->VertexRevision}
				);
				if (ProfilingEnabled)
					LastProfile.PublicationConstructionNanoseconds +=
						ProfileNanoseconds(ProfileClock::now() - OperationStart);
				continue;
			}
			if (MeshPartValue) {
				const bool ShouldRender = !MeshPartValue->GetDestroyed() && !MeshPartValue->IsDestroying() &&
					MeshPartValue->FindFirstAncestorWhichIsA("WorldRoot").get() == &World;
				RenderExtractionDiagnostic Diagnostic;
				auto Extracted = ShouldRender ? ExtractMeshPart(World, MeshPartValue, Diagnostic) : std::nullopt;
				if (ProfilingEnabled)
					LastProfile.FinalStateExtractionNanoseconds +=
						ProfileNanoseconds(ProfileClock::now() - ExtractionStart);
				const auto OperationStart = ProfilingEnabled ? ProfileClock::now() : ProfileClock::time_point{};
				if (!Extracted) {
					if (ShouldRender) Result->Diagnostics.push_back(std::move(Diagnostic));
					if (Previous != PublishedItems.end()) Result->Removes.push_back({Object});
					if (ProfilingEnabled)
						LastProfile.PublicationConstructionNanoseconds +=
							ProfileNanoseconds(ProfileClock::now() - OperationStart);
					continue;
				}
				auto DisplayMesh = Extracted->Mesh;
				if (const auto Animated = AnimationPoses.find(Object);
					Animated != AnimationPoses.end() && Animated->second.Pose.SourceMesh == Extracted->Mesh)
					DisplayMesh = Animated->second.Pose.PosedMesh;
				if (Previous == PublishedItems.end()) Result->Creates.push_back({
					Extracted->Item, DisplayMesh, Extracted->Material, true, Extracted->Primitives
				});
				else Result->Updates.push_back({
					Object, Domains, Extracted->Item, DisplayMesh, Extracted->Material, true, Extracted->Primitives
				});
				if (ProfilingEnabled)
					LastProfile.PublicationConstructionNanoseconds +=
						ProfileNanoseconds(ProfileClock::now() - OperationStart);
				continue;
			}
			const bool ShouldRender = PartValue && !PartValue->GetDestroyed() && !PartValue->IsDestroying() &&
				PartValue->FindFirstAncestorWhichIsA("WorldRoot").get() == &World;
			if (!ShouldRender) {
				if (ProfilingEnabled)
					LastProfile.FinalStateExtractionNanoseconds +=
						ProfileNanoseconds(ProfileClock::now() - ExtractionStart);
				const auto OperationStart = ProfilingEnabled ? ProfileClock::now() : ProfileClock::time_point{};
				if (Previous != PublishedItems.end()) {
					Result->Removes.push_back({Object});
				}
				if (auto Existing = PublishedDeformables.find(Object); Existing != PublishedDeformables.end()) {
					Result->MeshRemoves.push_back({Existing->second.Mesh});
					DeformableCacheRemoves.push_back(Object);
				}
				if (ProfilingEnabled)
					LastProfile.PublicationConstructionNanoseconds +=
						ProfileNanoseconds(ProfileClock::now() - OperationStart);
				continue;
			}
			RenderExtractionDiagnostic Diagnostic;
			auto Item = ExtractItem(PartValue, Diagnostic);
			if (ProfilingEnabled)
				LastProfile.FinalStateExtractionNanoseconds +=
					ProfileNanoseconds(ProfileClock::now() - ExtractionStart);
			const auto OperationStart = ProfilingEnabled ? ProfileClock::now() : ProfileClock::time_point{};
			if (!Item) {
				Result->Diagnostics.push_back(std::move(Diagnostic));
				if (Previous != PublishedItems.end()) {
					Result->Removes.push_back({Object});
				}
				if (ProfilingEnabled)
					LastProfile.PublicationConstructionNanoseconds +=
						ProfileNanoseconds(ProfileClock::now() - OperationStart);
				continue;
			}
			if (Previous == PublishedItems.end()) {
				Result->Creates.push_back({*Item, std::nullopt, BuildMaterial(*Item)});
			} else {
				Result->Updates.push_back({Object, Domains, *Item, std::nullopt, BuildMaterial(*Item)});
			}
			if (ProfilingEnabled)
				LastProfile.PublicationConstructionNanoseconds +=
					ProfileNanoseconds(ProfileClock::now() - OperationStart);
		}
		const auto FinalConstructionStart = ProfilingEnabled ? ProfileClock::now() : ProfileClock::time_point{};
		if (PendingUi) {
			Result->UiChanged = true;
			Result->SharedUi = PendingUi;
		}
		for (const auto Object : PendingAnimationObjects) {
			const auto PreviousPose = CommittedAnimationPoses.find(Object);
			const auto CurrentPose = AnimationPoses.find(Object);
			if (PreviousPose == CommittedAnimationPoses.end() && CurrentPose != AnimationPoses.end()) {
				const auto &State = CurrentPose->second;
				Result->MeshCreates.push_back({State.Pose.PosedMesh, State.TopologyRevision,
					State.Pose.PoseRevision, State.Vertices, State.Indices, State.Bounds});
				Result->AnimationPoseUpdates.push_back(State.Pose);
			} else if (PreviousPose != CommittedAnimationPoses.end() && CurrentPose == AnimationPoses.end()) {
				Result->MeshRemoves.push_back({PreviousPose->second.Pose.PosedMesh});
				Result->AnimationPoseRemoves.push_back({Object});
			} else if (PreviousPose != CommittedAnimationPoses.end() && CurrentPose != AnimationPoses.end()) {
				const auto &PreviousState = PreviousPose->second;
				const auto &State = CurrentPose->second;
				if (PreviousState.Pose.PosedMesh != State.Pose.PosedMesh ||
					PreviousState.Pose.SourceMesh != State.Pose.SourceMesh ||
					PreviousState.TopologyRevision != State.TopologyRevision ||
					PreviousState.Vertices->size() != State.Vertices->size() ||
					PreviousState.Indices->size() != State.Indices->size()) {
					FullResyncRequested = true;
					return FullResync();
				}
				if (State.Pose.PoseRevision > PreviousState.Pose.PoseRevision) {
					Result->MeshVertexUpdates.push_back({State.Pose.PosedMesh, State.Pose.PoseRevision,
						0, State.Vertices, State.Bounds});
					Result->AnimationPoseUpdates.push_back(State.Pose);
				}
			}
		}
		Result->MeshCreates.insert(Result->MeshCreates.end(), PendingAssetMeshCreates.begin(), PendingAssetMeshCreates.end());
		Result->MeshRemoves.insert(Result->MeshRemoves.end(), PendingAssetMeshRemoves.begin(), PendingAssetMeshRemoves.end());
		Result->TextureCreates = PendingTextureCreates;
		Result->TextureUpdates = PendingTextureUpdates;
		Result->TextureRemoves = PendingTextureRemoves;
		if (EstimatePublicationBytes(*Result) > Dirty->GetLimits().MaximumPublicationBytes) {
			Dirty->RequestFullResync(Scope, "Incremental render publication exceeds its byte limit");
			throw std::length_error("Incremental render publication exceeds its byte limit");
		}
		if (ProfilingEnabled)
			LastProfile.PublicationConstructionNanoseconds +=
				ProfileNanoseconds(ProfileClock::now() - FinalConstructionStart);
		const auto ReconciliationStart = ProfilingEnabled ? ProfileClock::now() : ProfileClock::time_point{};
		try {
			PublishedItems.reserve(PublishedItems.size() + Result->Creates.size());
			for (const auto &Remove : Result->Removes) PublishedItems.erase(Remove.Object);
			for (const auto &Create : Result->Creates) PublishedItems.emplace(Create.Item.Object, Create.Item);
			for (const auto &Update : Result->Updates) PublishedItems.at(Update.Object) = Update.Item;
			for (const auto Object : DeformableCacheRemoves) PublishedDeformables.erase(Object);
			for (const auto &[Object, State] : DeformableCacheUpdates)
				PublishedDeformables.insert_or_assign(Object, State);
			for (const auto Object : PendingAnimationObjects) {
				if (const auto Current = AnimationPoses.find(Object); Current != AnimationPoses.end())
					CommittedAnimationPoses.insert_or_assign(Object, Current->second);
				else CommittedAnimationPoses.erase(Object);
			}
			if (Result->EnvironmentChanged) PublishedEnvironment = Result->Frame.Environment;
			HasPublishedEnvironment = true;
		} catch (...) {
			PublishedItems.clear();
			PublishedDeformables.clear();
			CommittedAnimationPoses.clear();
			FullResyncRequested = true;
			throw;
		}
		if (ProfilingEnabled)
			LastProfile.StateCacheReconciliationNanoseconds =
				ProfileNanoseconds(ProfileClock::now() - ReconciliationStart);
		LastPublicationId = Result->Id;
		Dirty->Acknowledge(DirtyBatch);
		PendingUi.reset();
		PendingTextureCreates.clear();
		PendingTextureUpdates.clear();
		PendingTextureRemoves.clear();
		PendingAssetMeshCreates.clear();
		PendingAssetMeshRemoves.clear();
		PendingAnimationObjects.clear();
		PendingUiGeometryBytes = 0;
		PendingTextureBytes = 0;
		return std::shared_ptr<const RenderPublication>(std::move(Result));
	}
}
