#pragma once

#include "gargantuan/classes/generated/Sky.hpp"

namespace gargantuan {
	class Sky final : public Instance {
		I_Sky;

	  private:
		bool Enabled = true;
		std::string SkyboxPositiveX;
		std::string SkyboxNegativeX;
		std::string SkyboxPositiveY;
		std::string SkyboxNegativeY;
		std::string SkyboxPositiveZ;
		std::string SkyboxNegativeZ;
		void SetFace(std::string &Destination, std::string Value, const char *PropertyName);
	};
}
