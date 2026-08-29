#pragma once

#include "gargantuan/datatypes/CFrame.hpp"
#include "gargantuan/runtime/ObjectId.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace gargantuan {
	class AnimationRuntime;
	class AssetService;
	class Attachment;
	class Instance;

	struct SemanticSpatialTransform {
		glm::mat4 Matrix{1.0f};
		CFrame WorldCFrame;
		std::uint64_t Revision = 0;
		bool Animated = false;
	};

	struct SemanticSpatialMetrics {
		std::size_t RegisteredAttachments = 0;
		std::size_t IndexedSemanticAnchors = 0;
		std::size_t IndexedRigs = 0;
		std::uint64_t RigsVisited = 0;
		std::uint64_t AnchorResolutions = 0;
		std::uint64_t AnimatedAnchorResolutions = 0;
		std::uint64_t StaticFallbackResolutions = 0;
		std::uint64_t ChangedAnchors = 0;
		std::uint64_t DirtyMarks = 0;
		std::uint64_t CacheHits = 0;
		std::uint64_t ResolutionCpuNanoseconds = 0;
		std::uint64_t BindingBookkeepingCpuNanoseconds = 0;
		std::uint64_t TransformResolutionCpuNanoseconds = 0;
		std::uint64_t DirtyPropagationCpuNanoseconds = 0;
		std::uint64_t SteadyStateAllocations = 0;
	};

	class SemanticSpatialResolver final : public std::enable_shared_from_this<SemanticSpatialResolver> {
	  public:
		using DiagnosticCallback = std::function<void(std::string Code, std::string Message)>;

		static constexpr std::size_t MaximumRegisteredAttachments = 65'536;
		static constexpr std::size_t MaximumSemanticAnchors = 65'536;
		static constexpr std::size_t MaximumSemanticAnchorsPerRig = 1'024;
		static constexpr std::size_t MaximumAttachmentDepth = 64;

		SemanticSpatialResolver(
			std::shared_ptr<AssetService> Assets,
			AnimationRuntime *Animation = nullptr,
			DiagnosticCallback Diagnostic = {}
		);
		~SemanticSpatialResolver();
		SemanticSpatialResolver(const SemanticSpatialResolver &) = delete;
		SemanticSpatialResolver &operator=(const SemanticSpatialResolver &) = delete;

		void RegisterAttachment(const std::shared_ptr<Attachment> &AttachmentValue);
		void Step();
		void Shutdown();

		[[nodiscard]] std::optional<SemanticSpatialTransform>
		ResolveAttachment(const std::shared_ptr<Attachment> &AttachmentValue);
		[[nodiscard]] std::optional<SemanticSpatialTransform> ResolveWorldTransform(
			const std::shared_ptr<Instance> &Value,
			std::vector<std::shared_ptr<Instance>> *Observed = nullptr
		);
		[[nodiscard]] SemanticSpatialMetrics GetMetrics() const;

		[[nodiscard]] static std::optional<SemanticSpatialTransform>
		ResolveStaticAttachment(const std::shared_ptr<Attachment> &AttachmentValue);
		[[nodiscard]] static std::optional<SemanticSpatialTransform>
		ResolveStaticWorldTransform(const std::shared_ptr<Instance> &Value);

	  private:
		struct Impl;
		std::unique_ptr<Impl> State;
	};
}
