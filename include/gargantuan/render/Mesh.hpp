#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace gargantuan {
	struct Vertex {
	  public:
		glm::vec3 Position;
		glm::vec3 Normal;
		glm::vec2 UV;
		glm::vec4 Tangent{1.0f, 0.0f, 0.0f, 1.0f};
		std::array<std::uint16_t, 4> Joints{};
		glm::vec4 Weights{1.0f, 0.0f, 0.0f, 0.0f};
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
