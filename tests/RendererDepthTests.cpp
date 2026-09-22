#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/editor/EditorViewport.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/RenderExtractor.hpp"
#include "gargantuan/render/SDLRenderer.hpp"
#include "render/sdl/SDLSceneDepth.hpp"
#include "render/sdl/SDLPipelineBuilder.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <glm/ext/matrix_clip_space.hpp>
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <tuple>

using namespace gargantuan;

namespace {

struct Box { glm::dvec3 Center; glm::dvec3 Size; };
const std::array<Box, 2> Scene{{{{-50.125, 0, 74.25}, {205.75, 1, 341}},
                               {{-10.5, 24, -28.75}, {30.5, 59, 135}}}};

double Intersect(const Box &Value, glm::dvec3 Eye, glm::dvec3 Ray) {
	double Near = 0, Far = std::numeric_limits<double>::infinity();
	for (int Axis = 0; Axis < 3; ++Axis) {
		const double Minimum = Value.Center[Axis] - Value.Size[Axis] * .5;
		const double Maximum = Value.Center[Axis] + Value.Size[Axis] * .5;
		if (std::abs(Ray[Axis]) < 1e-12) {
			if (Eye[Axis] < Minimum || Eye[Axis] > Maximum) return std::numeric_limits<double>::infinity();
			continue;
		}
		const double A = (Minimum - Eye[Axis]) / Ray[Axis], B = (Maximum - Eye[Axis]) / Ray[Axis];
		Near = std::max(Near, std::min(A, B)); Far = std::min(Far, std::max(A, B));
	}
	return Near <= Far ? Near : std::numeric_limits<double>::infinity();
}

struct Coverage { std::size_t Expected = 0, Missing = 0; };
Coverage Measure(const EditorViewportFrame &Frame, const RenderCameraSnapshot &Camera, double Scale) {
	const double Tangent = std::tan(glm::radians(static_cast<double>(Camera.VerticalFieldOfView)) * .5);
	std::vector<bool> Expected(Frame.Width * Frame.Height);
	for (unsigned Y = 0; Y < Frame.Height; ++Y) for (unsigned X = 0; X < Frame.Width; ++X) {
		const auto Ray = glm::dvec3(Camera.LookDirection) + glm::dvec3(Camera.RightDirection) *
			((2.0 * (X + .5) / Frame.Width - 1) * Frame.Width / Frame.Height * Tangent) +
			glm::dvec3(Camera.UpDirection) * ((1 - 2.0 * (Y + .5) / Frame.Height) * Tangent);
		const auto Floor = Intersect({Scene[0].Center * Scale, Scene[0].Size * Scale}, Camera.Position, Ray);
		const auto Block = Intersect({Scene[1].Center * Scale, Scene[1].Size * Scale}, Camera.Position, Ray);
		Expected[Y * Frame.Width + X] = Block < Floor && Block > Camera.NearPlane && Block < Camera.FarPlane;
	}
	Coverage Result;
	for (unsigned Y = 1; Y + 1 < Frame.Height; ++Y) for (unsigned X = 1; X + 1 < Frame.Width; ++X) {
		const auto Index = Y * Frame.Width + X;
		if (!Expected[Index] || !Expected[Index-1] || !Expected[Index+1] ||
			!Expected[Index-Frame.Width] || !Expected[Index+Frame.Width]) continue;
		++Result.Expected;
		const auto Pixel = Index * 3;
		if (Frame.RgbPixels[Pixel] != Frame.RgbPixels[Pixel+1] || Frame.RgbPixels[Pixel+1] != Frame.RgbPixels[Pixel+2])
			++Result.Missing;
	}
	return Result;
}

void WriteFrame(const std::filesystem::path &Path, const EditorViewportFrame &Frame) {
	std::ofstream File(Path, std::ios::binary);
	File << "P6\n" << Frame.Width << ' ' << Frame.Height << "\n255\n";
	File.write(reinterpret_cast<const char *>(Frame.RgbPixels.data()), Frame.RgbPixels.size());
	if (!File) throw std::runtime_error("Could not write raw frame");
}
}

int main(int Argc, char **Argv) {
	try {
		bool Observe = false, Extended = false;
		float Near = .1f, Far = 100000;
		std::string Projection = "production";
		std::filesystem::path Output;
		for (int Index = 1; Index < Argc; ++Index) {
			const std::string Argument = Argv[Index];
			if (Argument == "--observe") Observe = true;
			else if (Argument == "--extended") Extended = true;
			else if (Argument.starts_with("--near=")) Near = std::stof(Argument.substr(7));
			else if (Argument.starts_with("--far=")) Far = std::stof(Argument.substr(6));
			else if (Argument.starts_with("--projection=")) Projection = Argument.substr(13);
			else if (Argument.starts_with("--output=")) Output = Argument.substr(9);
			else throw std::invalid_argument("Unknown depth test argument");
		}
		if (!Output.empty()) std::filesystem::create_directories(Output);
		BootstrapNativeRuntimeSchema();
		auto Game = std::make_shared<DataModel>();
		auto World = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
		std::array<std::shared_ptr<Part>, 2> Parts;
		for (std::size_t Index = 0; Index < Parts.size(); ++Index) {
			Parts[Index] = std::make_shared<Part>();
			Parts[Index]->SetColor(Index == 0 ? Color3(.5f, .5f, 1) : Color3(1, 1, 1));
			Parts[Index]->SetParent(World);
		}
		RenderPublisher Publisher;
		EditorViewportRenderer Renderer(800, 600);
		auto *Device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_METALLIB, false, nullptr);
		if (!Device) throw std::runtime_error(SDL_GetError());
		std::cout << "[Render:Depth] Driver=" << SDL_GetGPUDeviceDriver(Device)
			<< " D16=" << SDL_GPUTextureSupportsFormat(Device, SDL_GPU_TEXTUREFORMAT_D16_UNORM, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)
			<< " D24=" << SDL_GPUTextureSupportsFormat(Device, SDL_GPU_TEXTUREFORMAT_D24_UNORM, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)
			<< " D32=" << SDL_GPUTextureSupportsFormat(Device, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET) << '\n';
		SDL_DestroyGPUDevice(Device);
		auto Builder = SDLPipelineBuilder().SetDepthEnabled(true).SetDepthFormat(SDLSceneDepthFormat);
		const auto PipelineInfo = Builder.BuildInfo();
		if (SDLSceneDepthFormat != SDL_GPU_TEXTUREFORMAT_D32_FLOAT ||
			PipelineInfo.depth_stencil_state.compare_op != SDL_GPU_COMPAREOP_LESS ||
			!PipelineInfo.depth_stencil_state.enable_depth_test || !PipelineInfo.depth_stencil_state.enable_depth_write)
			throw std::runtime_error("Unexpected scene depth state");
		std::size_t MissingTotal = 0, Cases = 0;
		std::cout << "[Render:Depth] Near=" << Near << " Far=" << Far << " Projection=" << Projection << '\n';
		for (unsigned Variant = 0; Variant < 3; ++Variant) {
			const double Scale = Variant == 2 ? .1 : 1;
			for (std::size_t Index = 0; Index < Parts.size(); ++Index) {
				Parts[Index]->SetSize(glm::vec3(Scene[Index].Size * Scale));
				Parts[Index]->SetCFrame(CFrame(glm::vec3(Scene[Index].Center * Scale)));
				Parts[Index]->SetCastShadow(Variant == 0);
			}
			for (int Distance : {200, 500, 1000, 2000}) for (int Angle : {0, 45, 90}) {
				const double Radians = glm::radians(static_cast<double>(Angle));
				const auto Target = glm::vec3(glm::dvec3(-10.5, 15, -28.75) * Scale);
				const auto Position = Target + glm::vec3(glm::dvec3(std::cos(Radians), .55, std::sin(Radians)) * (Distance * Scale));
				auto Input = MakeLookAtRenderCameraInput(Position, Target, {0, 1, 0}, 60);
				Input.NearPlane = Near; Input.FarPlane = Far;
				auto Value = std::make_shared<RenderPublication>(*Publisher.Publish(*World, Input, 800, 600));
				auto &Camera = Value->Frame.Camera;
				if (Projection == "zo") Camera.ProjectionMatrix = glm::perspectiveRH_ZO(glm::radians(60.0f), 800.0f/600, Near, Far);
				else if (Projection == "no") Camera.ProjectionMatrix = glm::perspectiveRH_NO(glm::radians(60.0f), 800.0f/600, Near, Far);
				else if (Projection != "production") throw std::invalid_argument("Unknown projection");
				Camera.ViewProjectionMatrix = Camera.ProjectionMatrix * Camera.ViewMatrix;
				if (Cases == 0) std::cout << "[Render:Depth] Matrix22=" << Camera.ProjectionMatrix[2][2]
					<< " Matrix32=" << Camera.ProjectionMatrix[3][2] << '\n';
				const auto Frame = Renderer.Capture(Value);
				const auto Result = Measure(Frame, Camera, Scale);
				const auto Name = std::to_string(Variant) + "-" + std::to_string(Distance) + "-" + std::to_string(Angle);
				std::cout << "[Render:Depth] " << Name << " missing=" << Result.Missing << " expected=" << Result.Expected << '\n';
				if (!Output.empty()) WriteFrame(Output / (Name + ".ppm"), Frame);
				if (!Result.Expected) throw std::runtime_error("Empty coverage oracle");
				MissingTotal += Result.Missing; ++Cases;
			}
		}
		if (Extended) {
			// Independent occluders with known, nonzero surface separation.
			// Near/far probes also distinguish API clipping from depth rejection.
			for (const auto &[Depth, Gap, Visible] : std::array<std::tuple<float, float, bool>, 7>{{
				{.15f, .001f, true}, {.05f, .001f, false}, {1, .001f, true},
				{500, .5f, true}, {2000, 8, true}, {10000, 256, true}, {110000, 256, false}}}) {
				Parts[0]->SetSize({Depth, Depth, .001f});
				Parts[0]->SetCFrame(CFrame(glm::vec3(0, 0, -Depth - Gap)));
				Parts[1]->SetSize({Depth * .5f, Depth * .5f, .001f});
				Parts[1]->SetCFrame(CFrame(glm::vec3(0, 0, -Depth)));
				for (auto &PartValue : Parts) PartValue->SetCastShadow(false);
				const auto Input = MakeLookAtRenderCameraInput({0, 0, 0}, {0, 0, -1}, {0, 1, 0}, 60);
				const auto Frame = Renderer.Capture(Publisher.Publish(*World, Input, 800, 600));
				std::size_t Wrong = 0;
				for (unsigned Y = 294; Y <= 306; ++Y) for (unsigned X = 394; X <= 406; ++X) {
					const auto Pixel = (Y * Frame.Width + X) * 3;
					const bool White = Frame.RgbPixels[Pixel] > 20 && Frame.RgbPixels[Pixel] == Frame.RgbPixels[Pixel+1] && Frame.RgbPixels[Pixel+1] == Frame.RgbPixels[Pixel+2];
					Wrong += White != Visible;
				}
				std::cout << "[Render:Depth] separation depth=" << Depth << " gap=" << Gap << " visible=" << Visible << " wrong=" << Wrong << '\n';
				MissingTotal += Wrong;
			}
			for (std::size_t Index = 0; Index < Parts.size(); ++Index) {
				Parts[Index]->SetSize(glm::vec3(Scene[Index].Size));
				Parts[Index]->SetCFrame(CFrame(glm::vec3(Scene[Index].Center)));
				Parts[Index]->SetCastShadow(true);
			}
			SDLRenderer Runtime({800, 600}, {.Offscreen = true, .WaitForGpuCompletion = true});
			RenderPublisher RuntimePublisher;
			for (const auto &[Width, Height] : std::array<std::pair<unsigned, unsigned>, 5>{{{1920,1080},{2560,1440},{3840,2160},{320,240},{800,600}}}) {
				const auto Begin = std::chrono::steady_clock::now();
				Renderer.Resize(Width, Height);
				Publisher.RequestFullResync();
				const auto ResizeUs = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - Begin).count();
				const auto Input = MakeLookAtRenderCameraInput({343.0534f,290,-28.75f + 353.5534f}, {-10.5f,15,-28.75f}, {0,1,0}, 60);
				std::vector<long long> Times;
				for (int Index = 0; Index < 24; ++Index) {
					const auto View = Renderer.CaptureBgra(Publisher.Publish(*World, Input, Width, Height));
					if (Index >= 4) Times.push_back(View.Total.count());
				}
				std::sort(Times.begin(), Times.end());
				const auto Value = Publisher.Publish(*World, Input, Width, Height);
				const auto Frame = Renderer.Capture(Value);
				const auto Result = Measure(Frame, Value->Frame.Camera, 1);
				MissingTotal += Result.Missing;
				Runtime.Resize(Width, Height);
				Runtime.Draw(RuntimePublisher.Publish(*World, Input, Width, Height));
				Runtime.WaitForIdle();
				std::cout << "[Render:Depth] resize=" << Width << 'x' << Height << " resizeUs=" << ResizeUs
					<< " captureMedianUs=" << Times[Times.size()/2] << " captureMaxUs=" << Times.back() << " missing=" << Result.Missing << '\n';
			}
			if (Runtime.GetMetrics().FramesSubmitted != 5) throw std::runtime_error("Runtime resize path did not submit all frames");
		}
		Game->Destroy();
		std::cout << "[Render:Depth] Cases=" << Cases << " MissingTotal=" << MissingTotal << '\n';
		return Observe || MissingTotal == 0 ? 0 : 1;
	} catch (const std::exception &Error) {
		std::cerr << "[Render:Depth] " << Error.what() << '\n';
		return 1;
	}
}
