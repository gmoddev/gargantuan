// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "gargantuan/editor/EditorViewport.hpp"
#include "gargantuan/render/SDLRenderer.hpp"
#include "terrain/RenderStage.hpp"
#include "terrain/TerrainMaterialPalette.hpp"

#include <SDL3/SDL.h>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace gargantuan::terrain::test {
    namespace materialprobe {
        constexpr auto Width = 704;
        constexpr auto Height = 396;
        using Clock = std::chrono::steady_clock;

        inline void Require(bool Condition, const char *Message) {
            if (!Condition) throw std::runtime_error(Message);
        }

        struct Video {
            Video() {
                SDL_SetHintWithPriority(SDL_HINT_GPU_DRIVER, "vulkan", SDL_HINT_OVERRIDE);
                SDL_SetLogPriority(SDL_LOG_CATEGORY_GPU, SDL_LOG_PRIORITY_INFO);
                if (!SDL_Init(SDL_INIT_VIDEO))
                    throw std::runtime_error(std::string("SDL video initialization failed: ") + SDL_GetError());
            }
            ~Video() { SDL_Quit(); }
        };

        inline std::shared_ptr<const SurfaceMesh> MakeTerrain(ChunkKey Key, bool Cave) {
            std::vector<Sample> Samples;
            constexpr auto Extent = ChunkSide + 3;
            Samples.reserve(Extent * Extent * Extent);
            for (int Z = -1; Z <= ChunkSide + 1; ++Z)
                for (int Y = -1; Y <= ChunkSide + 1; ++Y)
                    for (int X = -1; X <= ChunkSide + 1; ++X) {
                        const auto WorldX = static_cast<double>(Key.X * ChunkSide + X);
                        double Density = Y - 17.2 - 2.1 * std::sin(WorldX * .16) - 1.2 * std::cos(Z * .24);
                        if (Cave)
                            Density = std::max(Density, 5.7 - std::hypot(Y - 8.1 - .19 * std::sin(WorldX * .26),
                                Z - 15.8 - .46 * std::cos(WorldX * .13)));
                        const auto Magnitude = std::clamp(std::llround(std::abs(Density) * 880), 1LL, 32767LL);
                        const auto Signed = static_cast<std::int16_t>(Density < 0 ? -Magnitude : Magnitude);
                        const auto Material = std::sin(WorldX * .18 + Z * .22 + Y * .43) > .14 ? 1u : 2u;
                        Samples.push_back({Signed, Signed < 0 ? Material : 0});
                    }
            auto Surface = std::make_shared<SurfaceMesh>();
            Require(ExtractSurface({Samples, ChunkSide, ChunkSide, ChunkSide, 1.0}, {}, *Surface) ==
                SurfaceStatus::Success && !Surface->Indices.empty(), "material probe MC33 fixture extraction");
            return Surface;
        }

        inline RenderFrameState Frame(bool Cave) {
            RenderFrameState Result;
            Result.ViewportWidth = Width;
            Result.ViewportHeight = Height;
            auto &Camera = Result.Camera;
            Camera.Position = Cave ? glm::vec3(-25, 8.0f, 15.6f) : glm::vec3(27, 47, 59);
            const auto Target = Cave ? glm::vec3(6, 7.3f, 17.6f) : glm::vec3(-3, 16, 16);
            Camera.LookDirection = glm::normalize(Target - Camera.Position);
            Camera.RightDirection = glm::normalize(glm::cross(Camera.LookDirection, glm::vec3(0, 1, 0)));
            Camera.UpDirection = glm::normalize(glm::cross(Camera.RightDirection, Camera.LookDirection));
            Camera.VerticalFieldOfView = Cave ? 67.f : 51.f;
            Camera.FarPlane = 170.f;
            Camera.ViewMatrix = glm::lookAtRH(Camera.Position, Target, Camera.UpDirection);
            Camera.ProjectionMatrix = glm::perspectiveRH_ZO(glm::radians(Camera.VerticalFieldOfView),
                static_cast<float>(Width) / Height, Camera.NearPlane, Camera.FarPlane);
            Camera.ViewProjectionMatrix = Camera.ProjectionMatrix * Camera.ViewMatrix;
            auto &Environment = Result.Environment;
            Environment.AmbientColor = Cave ? glm::vec3(.018f) : glm::vec3(.29f, .34f, .41f);
            Environment.SunDirection = glm::normalize(glm::vec3(-.55f, .82f, -.42f));
            Environment.SunIntensity = Cave ? .24f : 2.1f;
            Environment.DirectionalShadows.CascadeCount = 2;
            Environment.DirectionalShadows.Resolution = 512;
            Environment.DirectionalShadows.Distance = 110.f;
            if (Cave) {
                auto Lights = std::make_shared<std::vector<RenderLocalLightState>>();
                Lights->push_back({.Object = {900, 1}, .Kind = RenderLocalLightKind::Point,
                    .Position = {-19, 8.8f, 15.1f}, .Color = {1.f, .76f, .48f}, .Intensity = 40.f, .Range = 23.f});
                Lights->push_back({.Object = {901, 1}, .Kind = RenderLocalLightKind::Point,
                    .Position = {-4, 8.2f, 16.1f}, .Color = {.43f, .63f, 1.f}, .Intensity = 29.f, .Range = 21.f});
                Environment.LocalLights = std::move(Lights);
            }
            return Result;
        }

        inline std::shared_ptr<RenderPublication> Publish(const RenderFrameState &Frame, RenderPublicationId Id) {
            auto Publication = std::make_shared<RenderPublication>();
            Publication->Id = Id;
            Publication->BaseId = Id - 1;
            Publication->FullResync = Id == 1;
            Publication->Frame = Frame;
            Publication->EnvironmentChanged = Id == 1;
            return Publication;
        }

        inline EditorViewportFrame Measure(SDLRenderer &Runtime, const std::filesystem::path &Output,
            bool Cave, const std::array<ChunkPublicationCandidate, 2> &Chunks,
            const std::shared_ptr<const TerrainMaterialPalette> &Palette, TerrainMaterialRenderMode Mode) {
            const auto Configuration = Frame(Cave);
            constexpr std::size_t Repetitions = 15;
            std::array<std::int64_t, Repetitions> CpuTimes{};
            std::shared_ptr<RenderPublication> First;
            std::size_t UploadBytes = 0, Ranges = 0, StateBytes = 0;
            for (auto &Time : CpuTimes) {
                RenderStage Stage({29, 4}, Palette, Mode);
                RenderProjection Projection;
                auto Publication = Publish(Configuration, 1);
                const auto Begin = Clock::now();
                Stage.Stage({}, Chunks).value()->Install(1);
                const auto State = Stage.Capture();
                AppendTerrainRenderState(*Publication, {}, State);
                UploadBytes = Projection.Apply(*Publication).VertexUploadBytes;
                Time = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - Begin).count();
                Ranges = 0;
                StateBytes = State->Bytes;
                for (const auto &Draw : Projection.GetTerrainDraws()) Ranges += Draw.Primitives ? Draw.Primitives->size() : 1;
                First = std::move(Publication);
            }
            std::ranges::sort(CpuTimes);
            const auto Before = Runtime.GetMetrics();
            Runtime.Draw(First);
            const auto Cold = Runtime.GetMetrics();
            std::array<std::int64_t, Repetitions> FrameTimes{};
            std::array<std::uint64_t, Repetitions> FenceWaits{};
            RenderPublicationId Id = 2;
            for (std::size_t Index = 0; Index < Repetitions; ++Index) {
                const auto Begin = Clock::now();
                Runtime.Draw(Publish(Configuration, Id++));
                FrameTimes[Index] = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - Begin).count();
                FenceWaits[Index] = Runtime.GetMetrics().LastGpuCompletionWaitNanoseconds;
            }
            std::ranges::sort(FrameTimes);
            std::ranges::sort(FenceWaits);
            const auto After = Runtime.GetMetrics();
            Require(Cold.FramesSubmitted == Before.FramesSubmitted + 1 &&
                After.FramesSubmitted == Cold.FramesSubmitted + Repetitions &&
                After.UploadedBytes == Cold.UploadedBytes && Cold.UploadedBytes > Before.UploadedBytes,
                "material probe SDL draw and resident upload metrics");
            EditorViewportRenderer View(Width, Height);
            auto Frame = View.Capture(First);
            std::ofstream Bytes(Output, std::ios::binary);
            Bytes << "P6\n" << Frame.Width << ' ' << Frame.Height << "\n255\n";
            Bytes.write(reinterpret_cast<const char *>(Frame.RgbPixels.data()),
                static_cast<std::streamsize>(Frame.RgbPixels.size()));
            Require(Bytes.good() && Frame.RgbPixels.size() == Width * Height * 3, "material probe native readback");
            const auto Name = !Palette ? "Tint" : (Mode == TerrainMaterialRenderMode::Tint ? "ProjectTint" : "PrimitivePbr");
            std::cout << (Cave ? "Cave" : "Rolling") << ',' << Name << ','
                << Ranges << ',' << StateBytes << ',' << UploadBytes << ',' << Cold.UploadedBytes - Before.UploadedBytes
                << ',' << Cold.DrawCalls - Before.DrawCalls << ',' << CpuTimes[Repetitions / 2] << ',' << CpuTimes.back()
                << ',' << FrameTimes[Repetitions / 2] << ',' << FrameTimes.back() << ',' << FenceWaits[Repetitions / 2] << '\n';
            return Frame;
        }
    }

    inline void RunTerrainMaterialProbe(const std::filesystem::path &Output,
        const std::shared_ptr<const TerrainMaterialPalette> &Palette) {
        using namespace materialprobe;
        Require(std::filesystem::is_directory(Output), "material probe requires an existing output directory");
        Video Context;
        SDLRenderer Runtime(Vector2(Width, Height), {.Offscreen = true, .WaitForGpuCompletion = true, .DebugDevice = false});
        Runtime.Draw(Publish(Frame(false), 1));
        std::cout << "[Terrain:MaterialProbe] SDL driver " << Runtime.GetDriverName() << '\n';
        std::cout << "Scene,Path,PrimitiveRanges,RenderStateBytes,ProjectionUploadBytes,NativeUploadBytes,NativeDrawCalls,"
            "CpuStagePublishProjectionP50Ns,CpuStagePublishProjectionMaxNs,FrameWallP50Ns,FrameWallMaxNs,FenceWaitP50Ns\n";
        for (const bool Cave : {false, true}) {
            constexpr ChunkKey West{-1, 0, 0}, East{0, 0, 0};
            const std::array Chunks{ChunkPublicationCandidate{West, MakeTerrain(West, Cave), {}, true},
                ChunkPublicationCandidate{East, MakeTerrain(East, Cave), {}, true}};
            const std::string Name = Cave ? "Cave" : "Rolling";
            const auto Tint = Measure(Runtime, Output / (Name + "-Tint.ppm"), Cave, Chunks, {}, TerrainMaterialRenderMode::Tint);
            const auto ProjectTint = Measure(Runtime, Output / (Name + "-ProjectTint.ppm"), Cave, Chunks, Palette,
                TerrainMaterialRenderMode::Tint);
            const auto Pbr = Measure(Runtime, Output / (Name + "-PrimitivePbr.ppm"), Cave, Chunks, Palette,
                TerrainMaterialRenderMode::PrimitivePbr);
            Require(Tint.RgbPixels != ProjectTint.RgbPixels && ProjectTint.RgbPixels != Pbr.RgbPixels,
                "material probe rendered tints and PBR factors visibly change output");
        }
    }
}
