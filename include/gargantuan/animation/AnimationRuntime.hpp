#pragma once

#include "gargantuan/assets/AssetTypes.hpp"
#include "gargantuan/render/RenderPublication.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gargantuan {
	class Animator;
	class AssetService;

	struct AnimationPoseSnapshot {
		AssetContentId SkeletonCompatibilityId;
		std::uint64_t PoseRevision = 0;
		std::shared_ptr<const std::vector<glm::mat4>> JointModelTransforms;
		std::shared_ptr<const std::vector<glm::mat4>> BonePalette;
	};

	struct AnimationRuntimeMetrics {
		std::size_t TrackedAnimators = 0;
		std::size_t ActiveRigs = 0;
		std::size_t ActiveTracks = 0;
		std::uint64_t EvaluatedBones = 0;
		std::uint64_t SkinnedVertices = 0;
		std::uint64_t PoseUpdates = 0;
		std::uint64_t EvaluationCpuNanoseconds = 0;
		std::uint64_t SamplingAndBlendingCpuNanoseconds = 0;
		std::uint64_t HierarchyCpuNanoseconds = 0;
		std::uint64_t SkinMatrixCpuNanoseconds = 0;
		std::uint64_t SkinningCpuNanoseconds = 0;
		std::uint64_t PosePublicationCpuNanoseconds = 0;
		std::uint64_t BufferAllocations = 0;
	};

	class AnimationRuntime final {
	  public:
		using DiagnosticCallback = std::function<void(std::string Code, std::string Message)>;

		static constexpr std::size_t MaximumAnimators = 4096;
		static constexpr std::size_t MaximumBonesPerRig = AssetLimits::MaximumSkeletonBones;

		AnimationRuntime(std::shared_ptr<AssetService> Assets, DiagnosticCallback Diagnostic = {});
		~AnimationRuntime();
		AnimationRuntime(const AnimationRuntime &) = delete;
		AnimationRuntime &operator=(const AnimationRuntime &) = delete;

		void RegisterAnimator(const std::shared_ptr<Animator> &AnimatorValue);
		void Step(float DeltaTime);
		void Shutdown();

		[[nodiscard]] const std::vector<RenderAnimationPoseState> &GetPoseUpdates() const;
		[[nodiscard]] const std::vector<RenderAnimationPoseRemove> &GetPoseRemoves() const;
		void ClearChanges();
		[[nodiscard]] std::optional<AnimationPoseSnapshot> GetPose(ObjectId Object) const;
		[[nodiscard]] AnimationRuntimeMetrics GetMetrics() const;

		[[nodiscard]] static bool SkinMeshCpu(
			const ImportedMesh &Mesh,
			std::span<const glm::mat4> BonePalette,
			std::vector<RenderVertex> &Output,
			RenderBounds &Bounds
		);

	  private:
		struct Impl;
		std::unique_ptr<Impl> State;
	};
}
