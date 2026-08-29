#include "render/sdl/SDLPipelineBuilder.hpp"
#include "gargantuan/render/Mesh.hpp"
#include "gargantuan/render/SDLRenderer.hpp"
#include <SDL3/SDL_gpu.h>
#include <array>
#include <cstddef>

namespace gargantuan {
	SDLPipelineBuilder &SDLPipelineBuilder::SetVertexShader(SDL_GPUShader *shader) {
		VertexShader = shader;
		return *this;
	};

	SDLPipelineBuilder &SDLPipelineBuilder::SetFragmentShader(SDL_GPUShader *shader) {
		FragmentShader = shader;
		return *this;
	};

	SDLPipelineBuilder &SDLPipelineBuilder::SetColorFormat(SDL_GPUTextureFormat format) {
		ColorFormat = format;
		return *this;
	};

	SDLPipelineBuilder &SDLPipelineBuilder::SetColorEnabled(bool enabled) {
		ColorEnabled = enabled;
		return *this;
	};

	SDLPipelineBuilder &SDLPipelineBuilder::SetBlendingEnabled(bool enabled) {
		BlendingEnabled = enabled;
		return *this;
	};

	SDLPipelineBuilder &SDLPipelineBuilder::SetDepthFormat(SDL_GPUTextureFormat format) {
		DepthFormat = format;
		return *this;
	};

	SDLPipelineBuilder &SDLPipelineBuilder::SetDepthEnabled(bool enabled) {
		DepthEnabled = enabled;
		return *this;
	};

	SDL_GPUGraphicsPipelineCreateInfo SDLPipelineBuilder::BuildInfo() {
		static const std::array<SDL_GPUVertexBufferDescription, 1> BufferDescriptions{{{
			.slot = 0,
			.pitch = sizeof(Vertex),
			.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX,
		}}};
		static const std::array<SDL_GPUVertexAttribute, 6> Attributes{{
			{.location = 0, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, .offset = offsetof(Vertex, Position)},
			{.location = 1, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, .offset = offsetof(Vertex, Normal)},
			{.location = 2, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, .offset = offsetof(Vertex, Tangent)},
			{.location = 3, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, .offset = offsetof(Vertex, UV)},
			{.location = 4, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_USHORT4, .offset = offsetof(Vertex, Joints)},
			{.location = 5, .buffer_slot = 0, .format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, .offset = offsetof(Vertex, Weights)},
		}};
		SDL_GPUGraphicsPipelineCreateInfo info{};
		info.vertex_shader = VertexShader;
		info.fragment_shader = FragmentShader;

		info.vertex_input_state.vertex_attributes = Attributes.data();
		info.vertex_input_state.num_vertex_attributes = static_cast<Uint32>(Attributes.size());
		info.vertex_input_state.vertex_buffer_descriptions = BufferDescriptions.data();
		info.vertex_input_state.num_vertex_buffers = static_cast<Uint32>(BufferDescriptions.size());

		info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
		info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
		info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_BACK;
		info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;

		info.depth_stencil_state.enable_depth_test = DepthEnabled;
		info.depth_stencil_state.enable_depth_write = DepthEnabled;
		info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS;

		ColorTarget.format = ColorFormat;
		ColorTarget.blend_state.enable_blend = BlendingEnabled;

		if (BlendingEnabled) {
			ColorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
			ColorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
			ColorTarget.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
			ColorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
			ColorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
			ColorTarget.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
		} else {
			ColorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
			ColorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
			ColorTarget.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
			ColorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
			ColorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
			ColorTarget.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
		};

		info.target_info.color_target_descriptions = &ColorTarget;

		if (ColorEnabled) {
			info.target_info.num_color_targets = 1;
		} else {
			info.target_info.num_color_targets = 0;
		}

		info.target_info.depth_stencil_format = DepthFormat;
		info.target_info.has_depth_stencil_target = DepthEnabled;

		return info;
	}

	SDL_GPUGraphicsPipeline *SDLPipelineBuilder::Build(SDL_GPUDevice *gpu, SDLRendererMetrics *Metrics) {
		auto info = BuildInfo();
		auto *Pipeline = SDL_CreateGPUGraphicsPipeline(gpu, &info);
		if (Pipeline && Metrics) ++Metrics->PipelineCreations;
		return Pipeline;
	}
} // namespace gargantuan
