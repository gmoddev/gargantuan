#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace gargantuan {
	struct Vertex {
	  public:
		glm::vec3 Position;
		glm::vec3 Normal;
		glm::vec2 UV;
	};

	static constexpr int UI_SOLID_COLOR_INDEX = -1;
	struct UIVertex {
	  public:
		glm::vec2 AbsolutePosition;
		glm::vec2 AbsoluteSize;
		glm::vec2 UV;
		glm::vec4 Color;
		int TextureIndex = UI_SOLID_COLOR_INDEX;
	};

	struct Mesh {
	  public:
		std::vector<Vertex> Vertices;
		std::vector<uint32_t> Indices;
	};
}; // namespace gargantuan
