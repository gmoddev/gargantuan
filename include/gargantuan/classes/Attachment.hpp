#pragma once

#include "gargantuan/classes/generated/Attachment.hpp"

#include <memory>
#include <string>

namespace gargantuan {
	class SemanticSpatialResolver;

	class Attachment : public Instance {
		I_Attachment;

	  private:
		friend class SemanticSpatialResolver;

		std::string JointPath;
		std::weak_ptr<SemanticSpatialResolver> SpatialRuntime;

		void AttachSpatialRuntime(const std::shared_ptr<SemanticSpatialResolver> &Runtime);
		void DetachSpatialRuntime(const SemanticSpatialResolver *Runtime);
		void FireWorldCFrameChanged();
	};
}
