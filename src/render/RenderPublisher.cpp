// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/render/RenderExtractor.hpp"

#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/DeformableBody.hpp"
#include "gargantuan/classes/WorldRoot.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"

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
		  DirtyConsumer(Dirty->CreateConsumer()) {}

	RenderPublisher::~RenderPublisher() {
		if (Dirty) Dirty->ReleaseConsumer(DirtyConsumer);
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

	RenderPublicationPtr RenderPublisher::Publish(
		const WorldRoot &World,
		const RenderCameraInput &CameraInput,
		std::uint32_t ViewportWidth,
		std::uint32_t ViewportHeight,
		glm::vec3 LightDirection
	) {
		AssertAuthoritativeMutation("RenderPublication extraction");
		LastProfile = {};
		if (LastPublicationId == std::numeric_limits<RenderPublicationId>::max())
			throw std::overflow_error("RenderPublication identity exhausted and will not roll over");
		const auto CurrentScope = World.GetReplicationScopeId();
		if (!CurrentScope.IsValid()) throw std::invalid_argument("RenderPublication requires a live DataModel scope");
		if (!IsFinite(LightDirection) || glm::length(LightDirection) < 1e-6f)
			throw std::invalid_argument("RenderPublication light direction must be finite and nonzero");
		const auto Camera = BuildCamera(CameraInput, ViewportWidth, ViewportHeight);

		if (PublishedWorld != &World || Scope != CurrentScope) {
			if (Scope.IsValid()) Dirty->ReleaseConsumer(DirtyConsumer, Scope);
			PublishedWorld = &World;
			Scope = CurrentScope;
			FullResyncRequested = true;
			PublishedItems.clear();
			PublishedDeformables.clear();
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
			auto Snapshot = FullExtractor.Extract(World, CameraInput, ViewportWidth, ViewportHeight, LightDirection);
			auto Result = std::make_shared<RenderPublication>();
			Result->Id = LastPublicationId + 1;
			Result->FullResync = true;
			Result->Frame = {ViewportWidth, ViewportHeight, 1.0f, Camera, glm::normalize(LightDirection)};
			Result->Diagnostics = Snapshot->Diagnostics;
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
			FullResyncRequested = false;
			LastPublicationId = Result->Id;
			++FullResyncCount;
			Dirty->Acknowledge(DirtyBatch);
			PendingUi.reset();
			PendingTextureCreates.clear();
			PendingTextureUpdates.clear();
			PendingTextureRemoves.clear();
			PendingUiGeometryBytes = 0;
			PendingTextureBytes = 0;
			return std::shared_ptr<const RenderPublication>(std::move(Result));
		};

		if (FullResyncRequested || DirtyBatch.FullResyncRequired) return FullResync();

		const auto ExpansionStart = ProfilingEnabled ? ProfileClock::now() : ProfileClock::time_point{};
		std::vector<std::pair<ObjectId, RenderUpdateDomain>> DirtyObjects;
		const bool HasHierarchyChanges = std::ranges::any_of(DirtyBatch.Records, [](const RenderDirtyRecord &Record) {
			return HasRenderUpdateDomain(Record.Domains, RenderUpdateDomain::Hierarchy);
		});
		if (!HasHierarchyChanges) {
			DirtyObjects.reserve(DirtyBatch.Records.size());
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
						!std::dynamic_pointer_cast<DeformableBody>(Candidate)) return;
					const auto Object = Candidate->GetObjectId();
					Expanded[Object] = Expanded[Object] | RenderUpdateDomain::Transform | RenderUpdateDomain::Material |
						RenderUpdateDomain::Visibility | RenderUpdateDomain::Geometry | RenderUpdateDomain::Hierarchy;
				};
				AddRenderable(Root);
				for (const auto &Descendant : Root->GetDescendants()) AddRenderable(Descendant);
			}
			DirtyObjects.assign(Expanded.begin(), Expanded.end());
		}
		if (ProfilingEnabled)
			LastProfile.DirtyExpansionNanoseconds = ProfileNanoseconds(ProfileClock::now() - ExpansionStart);
		if (DirtyObjects.size() > Dirty->GetLimits().MaximumDistinctObjects) return FullResync();

		const auto ConstructionStart = ProfilingEnabled ? ProfileClock::now() : ProfileClock::time_point{};
		auto Result = std::make_shared<RenderPublication>();
		Result->Id = LastPublicationId + 1;
		Result->BaseId = LastPublicationId;
		Result->Frame = {ViewportWidth, ViewportHeight, 1.0f, Camera, glm::normalize(LightDirection)};
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
		} catch (...) {
			PublishedItems.clear();
			PublishedDeformables.clear();
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
		PendingUiGeometryBytes = 0;
		PendingTextureBytes = 0;
		return std::shared_ptr<const RenderPublication>(std::move(Result));
	}
}
