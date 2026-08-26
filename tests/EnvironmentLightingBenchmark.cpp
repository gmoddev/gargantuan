// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/Sky.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/RenderExtractor.hpp"
#include "gargantuan/render/RenderProjection.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "gargantuan/services/Lighting.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <string_view>
#include <vector>

namespace {
	using Clock = std::chrono::steady_clock;
	using Mutation = std::function<void(std::size_t)>;

	struct Fixture {
		std::shared_ptr<gargantuan::DataModel> Game;
		std::shared_ptr<gargantuan::Workspace> World;
		std::shared_ptr<gargantuan::Lighting> LightingValue;
		std::shared_ptr<gargantuan::AssetService> Assets;
		std::vector<std::shared_ptr<gargantuan::Part>> Parts;
		gargantuan::RenderPublisher Publisher;
		gargantuan::RenderProjection Projection;
		gargantuan::RenderCameraInput Camera;
	};

	std::unique_ptr<Fixture> BuildFixture(std::size_t PartCount) {
		using namespace gargantuan;
		ChangeJournal::Get().Clear();
		RenderDirtyAccumulator::Get().Clear();
		auto Result = std::make_unique<Fixture>();
		Result->Game = std::make_shared<DataModel>();
		Result->World = std::dynamic_pointer_cast<Workspace>(Result->Game->GetService("Workspace"));
		Result->LightingValue = std::dynamic_pointer_cast<Lighting>(Result->Game->GetService("Lighting"));
		Result->Assets = std::dynamic_pointer_cast<AssetService>(Result->Game->GetService("AssetService"));
		Result->Parts.reserve(PartCount);
		for (std::size_t Index = 0; Index < PartCount; ++Index) {
			auto PartValue = std::make_shared<Part>();
			PartValue->SetParent(Result->World);
			Result->Parts.push_back(std::move(PartValue));
		}
		Result->Camera = MakeLookAtRenderCameraInput(
			glm::vec3(0.0f, 0.0f, 10.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f)
		);
		auto TextureChanges = Result->Assets->DrainTextureChanges();
		Result->Publisher.SetUiTextureChanges(
			std::move(TextureChanges.Creates), std::move(TextureChanges.Updates), std::move(TextureChanges.Removes)
		);
		auto Initial = Result->Publisher.Publish(*Result->World, Result->Camera, 1280, 720);
		(void)Result->Projection.Apply(*Initial);
		return Result;
	}

	void Measure(
		Fixture &BenchmarkFixture,
		std::size_t PartCount,
		std::string_view Scenario,
		std::size_t Frames,
		const Mutation &Mutate,
		std::size_t EstimatedSkyResidentBytes = 0
	) {
		std::uint64_t DirtyNanoseconds = 0;
		std::uint64_t PublicationNanoseconds = 0;
		std::uint64_t ApplicationNanoseconds = 0;
		std::uint64_t EnvironmentChanges = 0;
		std::uint64_t ObjectOperations = 0;
		std::uint64_t TextureCreates = 0;
		std::uint64_t TextureUpdates = 0;
		for (std::size_t FrameIndex = 0; FrameIndex < Frames; ++FrameIndex) {
			const auto DirtyStart = Clock::now();
			Mutate(FrameIndex);
			DirtyNanoseconds += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - DirtyStart).count();

			const auto PublicationStart = Clock::now();
			auto TextureChanges = BenchmarkFixture.Assets->DrainTextureChanges();
			BenchmarkFixture.Publisher.SetUiTextureChanges(
				std::move(TextureChanges.Creates), std::move(TextureChanges.Updates), std::move(TextureChanges.Removes)
			);
			auto Publication = BenchmarkFixture.Publisher.Publish(
				*BenchmarkFixture.World, BenchmarkFixture.Camera, 1280, 720
			);
			PublicationNanoseconds +=
				std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - PublicationStart).count();
			EnvironmentChanges += Publication->EnvironmentChanged ? 1 : 0;
			ObjectOperations += Publication->Creates.size() + Publication->Updates.size() + Publication->Removes.size();
			TextureCreates += Publication->TextureCreates.size();
			TextureUpdates += Publication->TextureUpdates.size();

			const auto ApplicationStart = Clock::now();
			(void)BenchmarkFixture.Projection.Apply(*Publication);
			ApplicationNanoseconds +=
				std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - ApplicationStart).count();
		}

		const auto EstimatedCacheBytes = PartCount * sizeof(gargantuan::RenderItem) +
										 sizeof(gargantuan::RenderEnvironmentState) + EstimatedSkyResidentBytes;
		std::cout << PartCount << ',' << Scenario << ',' << Frames << ',' << DirtyNanoseconds / Frames << ','
				  << PublicationNanoseconds / Frames << ',' << ApplicationNanoseconds / Frames << ','
				  << EnvironmentChanges << ',' << ObjectOperations << ',' << TextureCreates << ',' << TextureUpdates
				  << ',' << EstimatedCacheBytes << ',' << EstimatedSkyResidentBytes << '\n';
	}

	void RunScenario(std::size_t PartCount, std::size_t Frames) {
		using namespace gargantuan;
		auto FixtureValue = BuildFixture(PartCount);
		auto &BenchmarkFixture = *FixtureValue;
		Measure(BenchmarkFixture, PartCount, "Static", Frames, [](std::size_t) {});
		Measure(BenchmarkFixture, PartCount, "ClockTime", Frames, [&](std::size_t Frame) {
			BenchmarkFixture.LightingValue->SetClockTime(static_cast<float>((Frame + 1) % 24));
		});
		Measure(BenchmarkFixture, PartCount, "Fog", Frames, [&](std::size_t Frame) {
			BenchmarkFixture.LightingValue->SetFogEnabled(Frame % 2 == 0);
		});
		Measure(BenchmarkFixture, PartCount, "Exposure", Frames, [&](std::size_t Frame) {
			BenchmarkFixture.LightingValue->SetExposureCompensation(Frame % 2 == 0 ? -1.0f : 1.0f);
		});

		std::vector<std::uint8_t> StablePixels(4 * 4 * 4, 96);
		std::vector<std::uint8_t> RevisionPixels(4 * 4 * 4, 32);
		const auto StableFace = BenchmarkFixture.Assets->RegisterMemoryImage("BenchmarkSkyStable", 4, 4, StablePixels);
		const auto RevisionFace = BenchmarkFixture.Assets->RegisterMemoryImage(
			"BenchmarkSkyPositiveX", 4, 4, RevisionPixels
		);
		auto SkyValue = std::make_shared<Sky>();
		SkyValue->SetSkyboxPositiveX(RevisionFace);
		SkyValue->SetSkyboxNegativeX(StableFace);
		SkyValue->SetSkyboxPositiveY(StableFace);
		SkyValue->SetSkyboxNegativeY(StableFace);
		SkyValue->SetSkyboxPositiveZ(StableFace);
		SkyValue->SetSkyboxNegativeZ(StableFace);
		SkyValue->SetParent(BenchmarkFixture.LightingValue);
		auto TextureChanges = BenchmarkFixture.Assets->DrainTextureChanges();
		BenchmarkFixture.Publisher.SetUiTextureChanges(
			std::move(TextureChanges.Creates), std::move(TextureChanges.Updates), std::move(TextureChanges.Removes)
		);
		auto SkyInitial = BenchmarkFixture.Publisher.Publish(
			*BenchmarkFixture.World, BenchmarkFixture.Camera, 1280, 720
		);
		(void)BenchmarkFixture.Projection.Apply(*SkyInitial);

		constexpr std::size_t EstimatedSkyResidentBytes = 2 * 4 * 4 * 4;
		Measure(
			BenchmarkFixture,
			PartCount,
			"SkyReimport",
			Frames,
			[&](std::size_t Frame) {
				std::fill(RevisionPixels.begin(), RevisionPixels.end(), static_cast<std::uint8_t>(Frame + 33));
				(void)BenchmarkFixture.Assets->RegisterMemoryImage("BenchmarkSkyPositiveX", 4, 4, RevisionPixels);
			},
			EstimatedSkyResidentBytes
		);
		BenchmarkFixture.Game->Destroy();
	}
}

int main(int ArgumentCount, char **Arguments) {
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
		const bool Quick = ArgumentCount > 1 && std::string_view(Arguments[1]) == "--quick";
		std::cout << "PartCount,Scenario,Frames,DirtyNanosecondsPerFrame,PublicationNanosecondsPerFrame,"
					 "ApplicationNanosecondsPerFrame,EnvironmentChanges,ObjectOperations,TextureCreates,TextureUpdates,"
					 "EstimatedCacheBytes,EstimatedSkyResidentBytes\n";
		RunScenario(1'000, Quick ? 4 : 20);
		if (!Quick) {
			RunScenario(10'000, 20);
			RunScenario(50'000, 20);
		}
	} catch (const std::exception &Error) {
		std::cerr << "[Environment:Benchmark] " << Error.what() << '\n';
		return 1;
	}
	return 0;
}
