#pragma once

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace gargantuan {
	std::filesystem::path GetSDLShaderPath(const std::filesystem::path &RelativePath);

	struct SDLShader {
		SDL_GPUShader *VertexShader = nullptr;
		SDL_GPUShader *FragmentShader = nullptr;
		void Destroy(SDL_GPUDevice *Gpu);
	};

	struct SDLFileShader final : public SDLShader {
		std::filesystem::path VertexFilepath;
		std::uint32_t VertexUniformBufferCount = 1;
		std::uint32_t VertexSamplerCount = 0;
		std::filesystem::path FragmentFilepath;
		std::uint32_t FragmentUniformBufferCount = 0;
		std::uint32_t FragmentSamplerCount = 0;

		void Init(SDL_GPUDevice *Gpu);

	  private:
		SDL_GPUShader *CompileFile(SDL_GPUDevice *Gpu, const std::filesystem::path &Path, SDL_GPUShaderCreateInfo Info);
	};
}
