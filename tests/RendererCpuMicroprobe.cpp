// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

// Standalone bounded source-kernel probe. This never calls SDL and cannot
// estimate driver command recording or GPU execution time.
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include "gargantuan/render/RenderPublication.hpp"

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <numeric>
#include <vector>

namespace {
	using namespace gargantuan;
	using Clock = std::chrono::steady_clock;
	using ClipPlanes = std::array<glm::vec4, 6>;
	constexpr RenderBounds PrimitiveBounds{glm::vec3(-0.5f), glm::vec3(0.5f)};
	std::atomic_uint64_t Sink{};

	// Verbatim Foundation 3/4 OpaquePass camera-plane and conservative box test.
	ClipPlanes GetClipPlanes(const RenderCameraSnapshot &Camera) {
		const auto Rows = glm::transpose(Camera.ProjectionMatrix * Camera.ViewMatrix);
		return {Rows[3] + Rows[0], Rows[3] - Rows[0], Rows[3] + Rows[1], Rows[3] - Rows[1],
			Rows[2], Rows[3] - Rows[2]};
	}

	bool OutsideView(const ClipPlanes &Planes, const glm::mat4 &Model, const RenderBounds &Bounds) {
		const auto Center = Bounds.Minimum * 0.5f + Bounds.Maximum * 0.5f;
		const auto Extent = Bounds.Maximum * 0.5f - Bounds.Minimum * 0.5f;
		const auto ToLocal = glm::transpose(Model);
		for (const auto &Plane : Planes) {
			const auto LocalPlane = ToLocal * Plane;
			const auto Normal = glm::vec3(LocalPlane);
			const auto AbsoluteNormal = glm::abs(Normal);
			const auto Radius = glm::dot(AbsoluteNormal, Extent);
			const auto Distance = glm::dot(Normal, Center) + LocalPlane.w;
			const auto Margin = 1e-5f * (1.0f + glm::dot(AbsoluteNormal, glm::abs(Center)) +
				std::abs(LocalPlane.w) + Radius);
			if (std::isfinite(Distance) && std::isfinite(Radius) && Distance + Radius < -Margin)
				return true;
		}
		return false;
	}

	// Exact Foundation 3 and Foundation 4 PartUniforms layouts and value packing
	// for the benchmark's untextured metallic-roughness material.
	struct alignas(16) Foundation3PartUniforms {
		glm::mat4 ModelMatrix;
		glm::mat4 NormalMatrix;
		glm::vec4 Color;
		glm::vec4 MaterialValues;
		glm::vec4 MaterialFlags;
	};
	struct alignas(16) Foundation4PartUniforms {
		glm::mat4 ModelMatrix;
		glm::mat4 NormalMatrix;
		glm::vec4 Color;
		glm::vec4 MaterialValues;
		glm::vec4 MaterialFlags;
		glm::vec4 EmissiveOcclusion;
		glm::vec4 MaterialControls;
		glm::vec4 ColorSamplerModes;
	};
	static_assert(sizeof(Foundation3PartUniforms) == 176);
	static_assert(sizeof(Foundation4PartUniforms) == 224);

	struct Case {
		std::vector<RenderItem> Items;
		ClipPlanes Planes;
		std::size_t ExpectedCulled = 0;
	};

	Case MakeCase(unsigned CulledPercent) {
		constexpr std::size_t Count = 50'000;
		const auto Side = static_cast<std::size_t>(std::ceil(std::sqrt(static_cast<double>(Count))));
		const auto Span = 2.0f * static_cast<float>(Side);
		const auto Distance = std::max(4.0f, Span * 1.5f);
		RenderCameraSnapshot Camera;
		Camera.Position = {0.0f, 0.0f, Distance};
		Camera.ViewMatrix = glm::lookAt(Camera.Position, glm::vec3(0.0f), Camera.UpDirection);
		Camera.ProjectionMatrix = glm::perspectiveRH_ZO(glm::radians(45.0f), 1280.0f / 720.0f,
			Camera.NearPlane, Camera.FarPlane);
		Case Result;
		Result.Planes = GetClipPlanes(Camera);
		Result.Items.reserve(Count);
		for (std::size_t Index = 0; Index < Count; ++Index) {
			const bool Behind = CulledPercent == 100 || (CulledPercent == 50 && Index % 2 != 0);
			const glm::vec3 Position{2.0f * static_cast<float>(Index % Side) - (Span - 2.0f) * 0.5f,
				2.0f * static_cast<float>(Index / Side) - (Span - 2.0f) * 0.5f,
				Behind ? Distance + 5.0f : 0.0f};
			RenderItem Item;
			Item.Object = {static_cast<std::uint32_t>(Index + 1), 1};
			Item.ModelMatrix = glm::translate(glm::mat4(1.0f), Position);
			Item.InverseModelMatrix = glm::translate(glm::mat4(1.0f), -Position);
			Result.Items.push_back(Item);
			Result.ExpectedCulled += Behind;
		}
		return Result;
	}

	enum class Kernel { Cull, Foundation3, Foundation4 };
	struct Observation { double Milliseconds; std::size_t Culled, Prepared; };

	Observation Run(const Case &Input, Kernel Selected, std::vector<std::byte> &Scratch) {
		const RenderMaterialState Material{.BaseColorFactor = glm::vec4(0.5f, 0.5f, 0.5f, 1.0f),
			.ShadingModel = RenderShadingModel::MetallicRoughness};
		std::size_t Culled = 0, Prepared = 0, Offset = 0;
		const auto Start = Clock::now();
		for (const auto &Item : Input.Items) {
			if (OutsideView(Input.Planes, Item.ModelMatrix, PrimitiveBounds)) { ++Culled; continue; }
			if (Selected == Kernel::Cull) continue;
			const float Shading = Material.MetallicRoughnessTexture ? 2.0f :
				Material.ShadingModel == RenderShadingModel::MetallicRoughness ? 1.0f : 0.0f;
			if (Selected == Kernel::Foundation3) {
				const Foundation3PartUniforms Part{
					.ModelMatrix = Item.ModelMatrix,
					.NormalMatrix = glm::transpose(Item.InverseModelMatrix),
					.Color = Material.BaseColorFactor,
					.MaterialValues = {Material.AlphaCutoff, static_cast<float>(Material.OpacityMode),
						Material.Metallic, Material.Roughness},
					.MaterialFlags = {Shading, Material.BaseColorTexture ? 1.0f : 0.0f,
						Material.NormalTexture ? 1.0f : 0.0f, 1.0f},
				};
				std::memcpy(Scratch.data() + Offset, &Part, sizeof(Part));
				Offset += sizeof(Part);
			} else {
				const Foundation4PartUniforms Part{
					.ModelMatrix = Item.ModelMatrix,
					.NormalMatrix = glm::transpose(Item.InverseModelMatrix),
					.Color = Material.BaseColorFactor,
					.MaterialValues = {Material.AlphaCutoff, static_cast<float>(Material.OpacityMode),
						Material.Metallic, Material.Roughness},
					.MaterialFlags = {Shading, Material.BaseColorTexture ? 1.0f : 0.0f,
						Material.NormalTexture ? 1.0f : 0.0f, 2.0f},
					.EmissiveOcclusion = glm::vec4(Material.EmissiveFactor,
						Material.OcclusionTexture ? Material.OcclusionStrength : 0.0f),
					.MaterialControls = {Material.NormalScale, Material.EmissiveTexture ? 1.0f : 0.0f, 0.0f, 0.0f},
					.ColorSamplerModes = {0.0f, 0.0f, 0.0f, 0.0f},
				};
				std::memcpy(Scratch.data() + Offset, &Part, sizeof(Part));
				Offset += sizeof(Part);
			}
			++Prepared;
		}
		const auto Elapsed = std::chrono::duration<double, std::milli>(Clock::now() - Start).count();
		if (Offset) Sink.fetch_add(static_cast<std::uint64_t>(Scratch[Offset - 1]), std::memory_order_relaxed);
		return {Elapsed, Culled, Prepared};
	}
}

int main() {
	std::vector<std::byte> Scratch(50'000 * sizeof(Foundation4PartUniforms));
	std::cout << "[Renderer:CpuMicroprobe] F3bytes=" << sizeof(Foundation3PartUniforms)
		<< " F4bytes=" << sizeof(Foundation4PartUniforms) << '\n';
	std::cout << "CulledPercent,Round,Kernel,Milliseconds,Culled,Prepared\n";
	for (const unsigned Percent : {0u, 50u, 100u}) {
		const auto Input = MakeCase(Percent);
		if (Run(Input, Kernel::Cull, Scratch).Culled != Input.ExpectedCulled) return 2;
		for (unsigned Round = 0; Round < 16; ++Round) {
			const std::array Order = Round % 2 == 0
				? std::array{Kernel::Cull, Kernel::Foundation3, Kernel::Foundation4}
				: std::array{Kernel::Foundation4, Kernel::Foundation3, Kernel::Cull};
			for (const auto Selected : Order) {
				const auto Result = Run(Input, Selected, Scratch);
				std::cout << Percent << ',' << Round << ',' <<
					(Selected == Kernel::Cull ? "Cull" : Selected == Kernel::Foundation3 ? "F3" : "F4")
					<< ',' << Result.Milliseconds << ',' << Result.Culled << ',' << Result.Prepared << '\n';
			}
		}
	}
	std::cout << "[Renderer:CpuMicroprobe] Sink=" << Sink.load() << '\n';
}
