#include "gargantuan/filesystem/Paths.hpp"
#include "gargantuan/render/SDLRenderer.hpp"
#include "render/sdl/SDLShader.hpp"

#include <SDL3/SDL.h>

#include <format>
#include <stdexcept>

namespace gargantuan {
	std::filesystem::path GetSDLShaderPath(const std::filesystem::path &relativePath) {
		return Paths::GetExecutableDirectory() / "shaders" / relativePath;
	}

	void
	GetShaderFormat(SDL_GPUDevice *gpu, SDL_GPUShaderFormat &format, std::string &extension, std::string &entrypoint) {
		SDL_GPUShaderFormat supportedFormats = SDL_GetGPUShaderFormats(gpu);
		if (supportedFormats & SDL_GPU_SHADERFORMAT_METALLIB) {
			format = SDL_GPU_SHADERFORMAT_METALLIB;
			extension = ".metallib";
			entrypoint = "main0";
		} else if (supportedFormats & SDL_GPU_SHADERFORMAT_MSL) {
			format = SDL_GPU_SHADERFORMAT_MSL;
			extension = ".metal";
			entrypoint = "main0";
		} else if (supportedFormats & SDL_GPU_SHADERFORMAT_SPIRV) {
			format = SDL_GPU_SHADERFORMAT_SPIRV;
			extension = ".spv";
			entrypoint = "main";
		}
	}

	void SDLShader::Destroy(SDL_GPUDevice *gpu) {
		if (VertexShader) {
			SDL_ReleaseGPUShader(gpu, VertexShader);
			VertexShader = nullptr;
		}

		if (FragmentShader) {
			SDL_ReleaseGPUShader(gpu, FragmentShader);
			FragmentShader = nullptr;
		}
	}

	SDL_GPUShader *SDLFileShader::CompileFile(
		SDL_GPUDevice *gpu, const std::filesystem::path &filepath, SDL_GPUShaderCreateInfo info
	) {
		size_t codeSize;
		const auto DisplayPath = Paths::ToUtf8(filepath);
		void *code = SDL_LoadFile(DisplayPath.c_str(), &codeSize);
		if (code == nullptr) {
			throw std::runtime_error(
				std::format("Failed to open required shader file '{}': {}", DisplayPath, SDL_GetError())
			);
		}

		info.code_size = codeSize;
		info.code = static_cast<const Uint8 *>(code);

		SDL_GPUShader *shader = SDL_CreateGPUShader(gpu, &info);
		SDL_free(code);

		if (shader == nullptr) {
			throw std::runtime_error(std::format("Failed to create shader from '{}': {}", DisplayPath, SDL_GetError()));
		};

		return shader;
	}

	void SDLFileShader::Init(SDL_GPUDevice *gpu, SDLRendererMetrics *Metrics) {
		SDL_GPUShaderFormat format = SDL_GPU_SHADERFORMAT_INVALID;
		std::string extension, entrypoint;
		GetShaderFormat(gpu, format, extension, entrypoint);

		if (!VertexShader) {
			std::string filepath = VertexFilepath.string() + extension;
			VertexShader = CompileFile(
				gpu,
				filepath.c_str(),
				{
					.entrypoint = entrypoint.c_str(),
					.format = format,
					.stage = SDL_GPU_SHADERSTAGE_VERTEX,
					.num_samplers = VertexSamplerCount,
					.num_storage_textures = 0,
					.num_storage_buffers = VertexStorageBufferCount,
					.num_uniform_buffers = VertexUniformBufferCount,
				}
			);
			if (Metrics) ++Metrics->ShaderCreations;
		}

		if (!FragmentShader) {
			std::string filepath = FragmentFilepath.string() + extension;
			FragmentShader = CompileFile(
				gpu,
				filepath,
				{
					.entrypoint = entrypoint.c_str(),
					.format = format,
					.stage = SDL_GPU_SHADERSTAGE_FRAGMENT,
					.num_samplers = FragmentSamplerCount,
					.num_storage_textures = 0,
					.num_storage_buffers = FragmentStorageBufferCount,
					.num_uniform_buffers = FragmentUniformBufferCount,
				}
			);
			if (Metrics) ++Metrics->ShaderCreations;
		}
	}
} // namespace gargantuan
