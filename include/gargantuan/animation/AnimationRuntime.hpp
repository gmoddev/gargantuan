#pragma once

#include "gargantuan/animation/AnimationUpdatePolicy.hpp"
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
	struct AnimationRuntimeTestAccess;
	class Animator;
	class AssetService;

	struct AnimationPoseSnapshot {
		AssetContentId SkeletonCompatibilityId;
		std::uint64_t PoseRevision = 0;
		std::shared_ptr<const std::vector<glm::mat4>> JointModelTransforms;
		std::shared_ptr<const std::vector<RenderSkinPaletteEntry>> SkinPalette;
	};

	struct AnimationRuntimeOptions {
		bool CpuSkinningFallback = false;
		bool DetailedProfiling = false;
		bool ParallelPoseEvaluation = true;
		std::size_t PoseWorkerCount = 0;
	};

	struct AnimationRuntimeMetrics {
		std::size_t TrackedAnimators = 0;
		std::size_t ActiveRigs = 0;
		std::size_t ActiveTracks = 0;
		std::uint64_t EvaluatedBones = 0;
		std::uint64_t SkinnedVertices = 0;
		std::uint64_t PoseUpdates = 0;
		std::uint64_t EvaluationCpuNanoseconds = 0;
		std::uint64_t TrackAdvanceCpuNanoseconds = 0;
		std::uint64_t KeyframeLookupCpuNanoseconds = 0;
		std::uint64_t InterpolationCpuNanoseconds = 0;
		std::uint64_t TrackBlendingCpuNanoseconds = 0;
		std::uint64_t BindPoseFallbackCpuNanoseconds = 0;
		std::uint64_t SamplingAndBlendingCpuNanoseconds = 0;
		std::uint64_t HierarchyCpuNanoseconds = 0;
		std::uint64_t SkinMatrixCpuNanoseconds = 0;
		std::uint64_t SkinningCpuNanoseconds = 0;
		std::uint64_t SemanticJointCacheCpuNanoseconds = 0;
		std::uint64_t PosePublicationCpuNanoseconds = 0;
		std::uint64_t PolicyCpuNanoseconds = 0;
		std::uint64_t BufferAllocations = 0;
		std::size_t FullRateAnimators = 0;
		std::size_t ReducedRateAnimators = 0;
		std::size_t FrozenVisualAnimators = 0;
		std::size_t SemanticRequiredAnimators = 0;
		std::uint64_t PoseEvaluations = 0;
		std::uint64_t SkippedPoseEvaluations = 0;
		std::uint64_t ImmediatePoseRefreshes = 0;
		std::uint64_t PolicyTransitions = 0;
		std::uint64_t VisibilityFeedbackDrops = 0;
		std::uint64_t HeadlessVisualPoseSkips = 0;
		std::uint64_t PoseJobsScheduled = 0;
		std::uint64_t PoseJobBatches = 0;
		std::uint64_t StalePoseJobDrops = 0;
		std::uint64_t PoseWorkerCpuNanoseconds = 0;
		std::uint64_t PoseJobSchedulingCpuNanoseconds = 0;
		std::uint64_t PoseJobWaitNanoseconds = 0;
		std::uint64_t PoseMergeCpuNanoseconds = 0;
		std::size_t ActivePoseJobs = 0;
		std::size_t PoseWorkerCapacity = 0;
	};

	class AnimationRuntime final {
	  public:
		using DiagnosticCallback = std::function<void(std::string Code, std::string Message)>;

		static constexpr std::size_t MaximumAnimators = 4096;
		static constexpr std::size_t MaximumBonesPerRig = AssetLimits::MaximumSkeletonBones;

		AnimationRuntime(
			std::shared_ptr<AssetService> Assets,
			DiagnosticCallback Diagnostic = {},
			AnimationRuntimeOptions Options = {}
		);
		~AnimationRuntime();
		AnimationRuntime(const AnimationRuntime &) = delete;
		AnimationRuntime &operator=(const AnimationRuntime &) = delete;

		void RegisterAnimator(const std::shared_ptr<Animator> &AnimatorValue);
		void Step(float DeltaTime);
		void Step(float DeltaTime, const AnimationUpdateContext &Context);
		void RequestPoseRefresh(ObjectId Object);
		void Shutdown();

		[[nodiscard]] const std::vector<RenderAnimationPoseState> &GetPoseUpdates() const;
		[[nodiscard]] const std::vector<RenderAnimationPoseRemove> &GetPoseRemoves() const;
		void ClearChanges();
		[[nodiscard]] std::optional<AnimationPoseSnapshot> GetPose(ObjectId Object) const;
		[[nodiscard]] AnimationRuntimeMetrics GetMetrics() const;

		[[nodiscard]] static bool SkinMeshCpu(
			const ImportedMesh &Mesh,
			std::span<const RenderSkinPaletteEntry> SkinPalette,
			std::vector<RenderVertex> &Output,
			RenderBounds &Bounds
		);

	  private:
		friend struct AnimationRuntimeTestAccess;
		void SetBeforePoseMergeForTesting(std::function<void()> Callback);
		struct Impl;
		std::unique_ptr<Impl> State;
	};
}
