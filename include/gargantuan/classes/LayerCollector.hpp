#pragma once

#include "gargantuan/classes/generated/LayerCollector.hpp"
#include "gargantuan/reflection/Enums.hpp"

namespace gargantuan {
	G_ENUM(ZIndexBehavior, Sibling, Global);

	class LayerCollector : public GuiBase2d {
		I_LayerCollector;
	};
}
