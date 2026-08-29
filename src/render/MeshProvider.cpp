#include "render/sdl/SDLMeshCache.hpp"
#include "gargantuan/render/PrimitiveMeshes.hpp"
#include "gargantuan/render/SDLRenderer.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <format>
#include <memory>
#include <stdexcept>
#include <utility>

namespace gargantuan {
	namespace {
		std::vector<std::pair<RenderGeometry, Mesh>> CreatePrimitiveMeshes() {
			return {
				{RenderGeometry::Ball, PrimitiveMeshes::Block()},
				{RenderGeometry::Block, PrimitiveMeshes::Block()},
				{RenderGeometry::Cylinder, PrimitiveMeshes::Block()},
				{RenderGeometry::Wedge, PrimitiveMeshes::Wedge()},
				{RenderGeometry::CornerWedge, PrimitiveMeshes::Block()},
			};
		}
	}

	void SDLMeshCache::Destroy() {
		if (!Gpu) return;
		DestroyDynamic();
		for (auto &[geometry, gpuMesh] : Meshes) {
			(void)geometry;
			gpuMesh->Destroy(Gpu);
		}
		Meshes.clear();
		Uploaded = false;
	}

	void SDLMeshCache::DestroyDynamic() {
		if (!Gpu) return;
		for (auto &[Mesh, GpuMesh] : DynamicMeshes) {
			(void)Mesh;
			GpuMesh->Destroy(Gpu);
		}
		DynamicMeshes.clear();
	}

	void SDLMeshCache::ApplyPublication(const RenderPublication &Publication) {
		if (!Gpu) throw std::logic_error("Cannot apply dynamic meshes without an SDL GPU device");
		if (Publication.FullResync) DestroyDynamic();
		for (const auto &Remove : Publication.MeshRemoves) {
			auto Found = DynamicMeshes.find(Remove.Mesh);
			if (Found == DynamicMeshes.end()) continue;
			Found->second->Destroy(Gpu);
			DynamicMeshes.erase(Found);
		}
		if (Publication.MeshCreates.empty() && Publication.MeshVertexUpdates.empty()) return;
		auto *Commands = SDL_AcquireGPUCommandBuffer(Gpu);
		if (!Commands) throw std::runtime_error(std::format("Failed to acquire dynamic mesh commands: {}", SDL_GetError()));
		auto *CopyPass = SDL_BeginGPUCopyPass(Commands);
		if (!CopyPass) {
			SDL_CancelGPUCommandBuffer(Commands);
			throw std::runtime_error(std::format("Failed to begin dynamic mesh upload: {}", SDL_GetError()));
		}
		bool CopyPassOpen = true;
		bool SubmitAttempted = false;
		try {
			for (const auto &Create : Publication.MeshCreates) {
				const bool CpuSkinned = std::ranges::any_of(Publication.AnimationPoseUpdates,
					[&](const auto &Pose) {
						return Pose.Mode == RenderAnimationSkinningMode::CpuFallback && Pose.PosedMesh == Create.Mesh;
					});
				auto Mesh = std::make_unique<SDLGpuMesh>(Create, Metrics);
				try {
					Mesh->Upload(Gpu, CopyPass);
					if (Metrics && CpuSkinned) ++Metrics->CpuSkinnedVertexUploads;
					const auto [Position, Inserted] = DynamicMeshes.emplace(Create.Mesh, std::move(Mesh));
					(void)Position;
					if (!Inserted) throw std::logic_error("Mesh publication attempted duplicate SDL residency");
				} catch (...) {
					if (Mesh) Mesh->Destroy(Gpu);
					throw;
				}
			}
			for (const auto &Update : Publication.MeshVertexUpdates) {
				DynamicMeshes.at(Update.Mesh)->UploadVertices(Gpu, CopyPass, Update.FirstVertex, *Update.Vertices);
				if (Metrics && std::ranges::any_of(Publication.AnimationPoseUpdates, [&](const auto &Pose) {
					return Pose.Mode == RenderAnimationSkinningMode::CpuFallback && Pose.PosedMesh == Update.Mesh;
				})) ++Metrics->CpuSkinnedVertexUploads;
			}
			SDL_EndGPUCopyPass(CopyPass);
			CopyPassOpen = false;
			SubmitAttempted = true;
			if (!SDL_SubmitGPUCommandBuffer(Commands))
				throw std::runtime_error(std::format("Failed to submit dynamic mesh upload: {}", SDL_GetError()));
		} catch (...) {
			if (CopyPassOpen) SDL_EndGPUCopyPass(CopyPass);
			if (!SubmitAttempted) SDL_CancelGPUCommandBuffer(Commands);
			DestroyDynamic();
			throw;
		}
	}

	void SDLMeshCache::UploadToGpu() {
		if (Uploaded) return;
		if (!Gpu) throw std::logic_error("Cannot upload primitive meshes without an SDL GPU device");
		auto cmd = SDL_AcquireGPUCommandBuffer(Gpu);
		if (!cmd) throw std::runtime_error(std::format("Failed to acquire mesh upload commands: {}", SDL_GetError()));
		auto copyPass = SDL_BeginGPUCopyPass(cmd);
		if (!copyPass) {
			SDL_CancelGPUCommandBuffer(cmd);
			throw std::runtime_error(std::format("Failed to begin mesh upload: {}", SDL_GetError()));
		}

		for (auto &[geometry, mesh] : CreatePrimitiveMeshes()) {
			auto gpuMesh = std::make_unique<SDLGpuMesh>(std::move(mesh), Metrics);
			gpuMesh->Upload(Gpu, copyPass);
			Meshes.emplace(geometry, std::move(gpuMesh));
		}

		SDL_EndGPUCopyPass(copyPass);
		if (!SDL_SubmitGPUCommandBuffer(cmd)) {
			Destroy();
			throw std::runtime_error(std::format("Failed to submit mesh upload: {}", SDL_GetError()));
		}
		Uploaded = true;
	}

	const SDLGpuMesh *SDLMeshCache::Find(RenderGeometry geometry) const {
		auto mesh = Meshes.find(geometry);
		return mesh == Meshes.end() ? nullptr : mesh->second.get();
	}

	const SDLGpuMesh *SDLMeshCache::Find(RenderMeshIdentity Mesh) const {
		const auto Found = DynamicMeshes.find(Mesh);
		return Found == DynamicMeshes.end() ? nullptr : Found->second.get();
	}
}
