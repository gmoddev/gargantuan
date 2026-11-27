#pragma once
#include <memory>
#include <optional>

namespace gargantuan {
	class Instance;
	class SpatialRegion;
	class WorldRoot;

	// Only explicitly assigned roots allocate a binding. Descendants inherit
	// it without changing their parent, object identity or local transform.
	class SpatialDomainMember {
	  public:
		SpatialDomainMember();
		~SpatialDomainMember();
		[[nodiscard]] std::optional<std::shared_ptr<SpatialRegion>> GetAssignedRegion() const;
		void SetAssignedRegion(Instance &Root, std::optional<std::shared_ptr<SpatialRegion>> Value);
		[[nodiscard]] bool HasAssignedRegion() const noexcept {
			return Binding != nullptr;
		}
		[[nodiscard]] std::shared_ptr<WorldRoot> ResolveAssignedWorld(const Instance &Root) const;
		[[nodiscard]] static const Instance *GetAssignmentRoot(const Instance &Value);

	  private:
		friend class WorldRoot;
		void ChangeAssignedRegion(Instance &Root, std::optional<std::shared_ptr<SpatialRegion>> Value,
			const struct CFrame *DestinationPose);
		struct State;
		std::unique_ptr<State> Binding;
		void Refresh(const std::shared_ptr<Instance> &Root);
	};
}
