// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, you can obtain one at http://mozilla.org/MPL/2.0/.

#pragma once

#include "gargantuan/render/RenderProjection.hpp"
#include "render/sdl/SDLShadowCasterCoverage.hpp"
#include "render/sdl/SDLShadowPoseLedger.hpp"
#include "render/sdl/SDLShadowResources.hpp"

#include <SDL3/SDL_gpu.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace gargantuan {
	struct SDLSpotShadowAssignment {
		ObjectId Light;
		std::uint32_t Layer = 0;
		glm::mat4 Matrix{1.0f};
		glm::vec3 Origin{0.0f};
		bool Dirty = true;
		bool HasCasters = false;
		bool Sampleable = false;
	};

	class SDLSpotShadowAtlas final {
	  public:
		void ApplyPublication(const RenderProjection &Before, const RenderPublication &Publication) {
			if (Publication.FullResync) {
				Invalidate();
				return;
			}
			if (std::ranges::none_of(Slots, [](const auto &Slot) {
				return Slot.Light && Slot.Committed;
			})) return;
			const auto &Objects = Before.GetObjects();
			const auto Find = [&](ObjectId Id) -> const RenderProjectedObject * {
				const auto Found = Objects.find(Id);
				return Found == Objects.end() ? nullptr : &Found->second;
			};
			const auto IncomingCaster = [&](ObjectId Id) {
				return std::ranges::any_of(Publication.Creates,
					[&](const auto &Create) {
						return Create.Item.Object == Id && Create.Visible && Create.Item.CastShadow;
					}) || std::ranges::any_of(Publication.Updates,
						[&](const auto &Update) {
							return Update.Object == Id && Update.Visible && Update.Item.CastShadow;
						});
			};
			const bool PoseCasterChanged = std::ranges::any_of(Publication.AnimationPoseUpdates,
				[&](const auto &Pose) {
					const auto *Object = Find(Pose.Object);
					return (Object && Object->Visible && Object->Item.CastShadow) || IncomingCaster(Pose.Object);
				}) || std::ranges::any_of(Publication.AnimationPoseRemoves,
				[&](const auto &Pose) {
					const auto *Object = Find(Pose.Object);
					return Object && Object->Visible && Object->Item.CastShadow;
				});
			bool DeformableCasterChanged = false;
			if (!Publication.MeshVertexUpdates.empty()) {
				std::unordered_set<RenderMeshIdentity, RenderMeshIdentityHash> EditedMeshes;
				for (const auto &Update : Publication.MeshVertexUpdates) EditedMeshes.insert(Update.Mesh);
				DeformableCasterChanged = std::ranges::any_of(Objects, [&](const auto &Entry) {
					return Entry.second.Visible && Entry.second.Item.CastShadow && Entry.second.Mesh &&
						EditedMeshes.contains(*Entry.second.Mesh);
				}) || std::ranges::any_of(Publication.Creates, [&](const auto &Create) {
					return Create.Visible && Create.Item.CastShadow && Create.Mesh && EditedMeshes.contains(*Create.Mesh);
				}) || std::ranges::any_of(Publication.Updates, [&](const auto &Update) {
					return Update.Visible && Update.Item.CastShadow && Update.Mesh && EditedMeshes.contains(*Update.Mesh);
				});
			}
			const bool UnboundedCasterChange = PoseCasterChanged || DeformableCasterChanged;
			for (auto &Slot : Slots) {
				if (!Slot.Light || !Slot.Committed) continue;
				const auto Relevant = [&](const auto &Object) {
					return Object.Visible && Object.Item.CastShadow &&
						SDLShadowCasterMayAffect(Before, Object.Item, Object.Mesh, Slot.Matrix,
						SpotlightShadowResolution, Slot.LastLight->Position);
				};
				const auto WasRelevant = [&](ObjectId Id) {
					const auto *Object = Find(Id);
					return Object && Relevant(*Object);
				};
				if (UnboundedCasterChange || std::ranges::any_of(Publication.Creates, Relevant) ||
					std::ranges::any_of(Publication.Removes,
						[&](const auto &Remove) { return WasRelevant(Remove.Object); }) ||
					std::ranges::any_of(Publication.Updates, [&](const auto &Update) {
						return WasRelevant(Update.Object) || Relevant(Update);
					}) ||
					std::ranges::any_of(Publication.MeshRemoves,
						[&](const auto &Remove) { return Slot.Meshes.contains(Remove.Mesh); }) ||
					std::ranges::any_of(Publication.TextureUpdates,
						[&](const auto &Update) { return Slot.Masks.contains(Update.Texture); }) ||
					std::ranges::any_of(Publication.TextureRemoves,
						[&](const auto &Remove) { return Slot.Masks.contains(Remove.Texture); })) {
					Slot.Committed = false;
					Slot.CommittedPoses.clear();
				}
			}
		}

		void Prepare(const RenderFrameState &Frame, const SDL_GPUTexture *Target) {
			struct Candidate {
				const RenderLocalLightState *Light;
				double Score;
			};
			std::vector<Candidate> Candidates;
			if (Frame.Environment.LocalLights) {
				for (const auto &Light : *Frame.Environment.LocalLights) {
					if (Light.Kind != RenderLocalLightKind::Spot || !Light.ShadowEnabled ||
						Light.Intensity <= 0.0f || Light.Range <= 0.1f || !IsVisible(Frame, Light)) continue;
					const double Distance = glm::length(glm::dvec3(Light.Position) -
						glm::dvec3(Frame.Camera.Position));
					const double Nearest = std::max(1.0, Distance - Light.Range);
					const double Score = static_cast<double>(Light.Intensity) *
						std::max({Light.Color.r, Light.Color.g, Light.Color.b}) / (Nearest * Nearest);
					Candidates.push_back({&Light, Score});
				}
			}
			std::ranges::sort(Candidates, [](const Candidate &Left, const Candidate &Right) {
				if (Left.Light->ShadowPriority != Right.Light->ShadowPriority)
					return Left.Light->ShadowPriority > Right.Light->ShadowPriority;
				if (Left.Score != Right.Score) return Left.Score > Right.Score;
				return Left.Light->Object < Right.Light->Object;
			});
			Dropped = Candidates.size() > MaximumShadowedSpotlights
				? static_cast<std::uint32_t>(Candidates.size() - MaximumShadowedSpotlights) : 0;
			if (Candidates.size() > MaximumShadowedSpotlights) Candidates.resize(MaximumShadowedSpotlights);
			std::array<bool, MaximumShadowedSpotlights> Reserved{};
			Assignments.clear();
			for (const auto &Candidate : Candidates) {
				for (std::uint32_t Index = 0; Index < MaximumShadowedSpotlights; ++Index) {
					if (Slots[Index].Light && *Slots[Index].Light == Candidate.Light->Object) {
						Reserved[Index] = true;
						break;
					}
				}
			}
			for (const auto &Candidate : Candidates) {
				std::uint32_t Layer = MaximumShadowedSpotlights;
				for (std::uint32_t Index = 0; Index < MaximumShadowedSpotlights; ++Index)
					if (Slots[Index].Light && *Slots[Index].Light == Candidate.Light->Object) {
						Layer = Index;
						break;
					}
				if (Layer == MaximumShadowedSpotlights)
					for (std::uint32_t Index = 0; Index < MaximumShadowedSpotlights; ++Index)
						if (!Reserved[Index]) {
							Layer = Index;
							Reserved[Index] = true;
							break;
						}
				if (Layer == MaximumShadowedSpotlights) continue;
				auto &Slot = Slots[Layer];
				const auto Matrix = MakeMatrix(*Candidate.Light);
				const bool GeometryChanged = !Slot.Light || *Slot.Light != Candidate.Light->Object ||
					!Slot.LastLight || Slot.LastLight->Position != Candidate.Light->Position ||
					Slot.LastLight->Direction != Candidate.Light->Direction ||
					Slot.LastLight->Range != Candidate.Light->Range ||
					Slot.LastLight->OuterConeCosine != Candidate.Light->OuterConeCosine;
				if (GeometryChanged || Slot.Target != Target || Slot.Matrix != Matrix) {
					Slot.Committed = false;
					Slot.CommittedPoses.clear();
				}
				Slot.Light = Candidate.Light->Object;
				Slot.LastLight = *Candidate.Light;
				Slot.Target = Target;
				Slot.Matrix = Matrix;
				Assignments.push_back({Candidate.Light->Object, Layer, Matrix, Candidate.Light->Position,
					!Slot.Committed, Slot.HasCasters, Slot.Committed});
			}
			for (std::uint32_t Index = 0; Index < MaximumShadowedSpotlights; ++Index)
				if (!Reserved[Index]) Slots[Index] = {};
		}

		[[nodiscard]] const std::vector<SDLSpotShadowAssignment> &GetAssignments() const { return Assignments; }
		[[nodiscard]] std::uint32_t GetDroppedCount() const { return Dropped; }
		[[nodiscard]] bool MayAffect(const RenderProjection &Projection, const RenderProjectedObject &Object,
			std::uint32_t Layer) const {
			return Layer < MaximumShadowedSpotlights && Slots[Layer].LastLight &&
				Object.Visible && Object.Item.CastShadow &&
				SDLShadowCasterMayAffect(Projection, Object.Item, Object.Mesh, Slots[Layer].Matrix,
					SpotlightShadowResolution, Slots[Layer].LastLight->Position);
		}
		void BeginRecording(std::uint32_t Layer) {
			if (Layer >= MaximumShadowedSpotlights) return;
			auto &Slot = Slots[Layer];
			Slot.Recorded = false;
			Slot.RecordingPoses.clear();
			Slot.Meshes.clear();
			Slot.Masks.clear();
			Slot.Overflow = false;
		}
		void DependOnMesh(std::uint32_t Layer, RenderMeshIdentity Mesh) {
			if (Layer < MaximumShadowedSpotlights) Track(Slots[Layer].Meshes, Mesh, Slots[Layer].Overflow);
		}
		void DependOnMask(std::uint32_t Layer, RenderTextureIdentity Texture) {
			if (Layer < MaximumShadowedSpotlights) Track(Slots[Layer].Masks, Texture, Slots[Layer].Overflow);
		}
		void RecordPose(std::uint32_t Layer, ObjectId Object, std::uint64_t Revision) {
			if (Layer >= MaximumShadowedSpotlights || Revision == 0) return;
			auto &Slot = Slots[Layer];
			if (Slot.RecordingPoses.size() >= 32'768 && !Slot.RecordingPoses.contains(Object)) {
				Slot.Overflow = true;
				return;
			}
			MergeSDLShadowPoseRevision(Slot.RecordingPoses, Object, Revision);
		}
		void MergeCommittedPoses(std::uint32_t Layer, SDLShadowPoseRevisions &Destination) const {
			if (Layer < MaximumShadowedSpotlights && Slots[Layer].Committed)
				MergeSDLShadowPoseRevisions(Destination, Slots[Layer].CommittedPoses);
		}
		void FinishRecording(std::uint32_t Layer, bool AllResident, bool HasCasters) {
			if (Layer >= MaximumShadowedSpotlights) return;
			auto &Slot = Slots[Layer];
			Slot.HasCasters = HasCasters;
			Slot.Recorded = AllResident && !Slot.Overflow;
			for (auto &Assignment : Assignments) if (Assignment.Layer == Layer) {
				Assignment.HasCasters = HasCasters;
				Assignment.Sampleable = Slot.Recorded;
				break;
			}
		}
		[[nodiscard]] std::uint32_t OnFrameSubmitted() {
			std::uint32_t Built = 0;
			for (auto &Slot : Slots) if (Slot.Recorded) {
				Slot.Committed = true;
				Slot.Recorded = false;
				Slot.CommittedPoses = std::move(Slot.RecordingPoses);
				++Built;
			}
			for (auto &Slot : Slots) Slot.RecordingPoses.clear();
			return Built;
		}
		void Invalidate() {
			for (auto &Slot : Slots) {
				Slot.Committed = false;
				Slot.Recorded = false;
				Slot.CommittedPoses.clear();
				Slot.RecordingPoses.clear();
			}
		}

	  private:
		struct Layer {
			std::optional<ObjectId> Light;
			std::optional<RenderLocalLightState> LastLight;
			const SDL_GPUTexture *Target = nullptr;
			glm::mat4 Matrix{1.0f};
			std::unordered_set<RenderMeshIdentity, RenderMeshIdentityHash> Meshes;
			std::unordered_set<RenderTextureIdentity, RenderTextureIdentityHash> Masks;
			SDLShadowPoseRevisions CommittedPoses;
			SDLShadowPoseRevisions RecordingPoses;
			bool Committed = false;
			bool Recorded = false;
			bool HasCasters = false;
			bool Overflow = false;
		};
		template <typename Set, typename Identity>
		static void Track(Set &Dependencies, Identity Value, bool &Overflow) {
			if (Dependencies.size() < 32'768) Dependencies.insert(Value);
			else if (!Dependencies.contains(Value)) Overflow = true;
		}
		[[nodiscard]] static glm::mat4 MakeMatrix(const RenderLocalLightState &Light) {
			const auto Direction = glm::normalize(Light.Direction);
			const auto Up = std::abs(Direction.y) > 0.99f ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
			const auto Outer = std::clamp(Light.OuterConeCosine, 0.01f, 0.9999f);
			const auto Fov = 2.0f * std::acos(Outer);
			const auto Near = std::min(0.1f, Light.Range * 0.01f);
			return glm::perspectiveRH_ZO(Fov, 1.0f, Near, Light.Range) *
				glm::lookAt(glm::vec3(0), Direction, Up);
		}
		[[nodiscard]] static bool IsVisible(const RenderFrameState &Frame, const RenderLocalLightState &Light) {
			const auto Rows = glm::transpose(glm::dmat4(Frame.Camera.ViewProjectionMatrix));
			const std::array Planes{Rows[3] + Rows[0], Rows[3] - Rows[0], Rows[3] + Rows[1],
				Rows[3] - Rows[1], Rows[2], Rows[3] - Rows[2]};
			const glm::dvec3 Center(Light.Position);
			for (const auto &Plane : Planes) {
				const auto Normal = glm::dvec3(Plane);
				const auto Distance = glm::dot(Normal, Center) + Plane.w;
				const auto Radius = glm::length(Normal) * Light.Range;
				if (std::isfinite(Distance) && std::isfinite(Radius) && Distance < -Radius - 1e-5)
					return false;
			}
			return true;
		}
		std::array<Layer, MaximumShadowedSpotlights> Slots{};
		std::vector<SDLSpotShadowAssignment> Assignments;
		std::uint32_t Dropped = 0;
	};
}
