#include "gargantuan/editor/EditorViewport.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

using namespace gargantuan;

namespace {
	using Clock = std::chrono::steady_clock;

	RenderPublicationPtr MakePublication(std::uint32_t Width, std::uint32_t Height, RenderPublicationId Id) {
		auto Publication = std::make_shared<RenderPublication>();
		Publication->Id = Id;
		Publication->FullResync = true;
		Publication->Frame.ViewportWidth = Width;
		Publication->Frame.ViewportHeight = Height;
		return Publication;
	}

	double Percentile(std::vector<double> Values, double Fraction) {
		if (Values.empty()) return 0.0;
		std::sort(Values.begin(), Values.end());
		const auto Index = static_cast<std::size_t>(
			std::clamp(Fraction, 0.0, 1.0) * static_cast<double>(Values.size() - 1)
		);
		return Values[Index];
	}

	void Run(std::uint32_t Width, std::uint32_t Height, std::uint32_t TargetFps, double Seconds) {
		EditorViewportRenderer Renderer(Width, Height);
		RenderPublicationId PublicationId = 1;
		(void)Renderer.CaptureBgra(MakePublication(Width, Height, PublicationId++));
		const auto Start = Clock::now();
		const auto Finish = Start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(Seconds));
		const auto Cadence = TargetFps == 0 ? Clock::duration::zero() :
			std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / TargetFps));
		auto Next = Start;
		std::vector<double> CaptureMilliseconds;
		std::uint64_t PayloadBytes = 0;
		while (Clock::now() < Finish) {
			const auto CaptureStart = Clock::now();
			auto Frame = Renderer.CaptureBgra(MakePublication(Width, Height, PublicationId++));
			const auto CaptureEnd = Clock::now();
			CaptureMilliseconds.push_back(std::chrono::duration<double, std::milli>(CaptureEnd - CaptureStart).count());
			PayloadBytes += Frame.BgraPixels.size();
			if (TargetFps != 0) {
				Next += Cadence;
				std::this_thread::sleep_until(Next);
			}
		}
		const auto Elapsed = std::chrono::duration<double>(Clock::now() - Start).count();
		const auto Frames = CaptureMilliseconds.size();
		const auto Mean = Frames == 0 ? 0.0 :
			std::accumulate(CaptureMilliseconds.begin(), CaptureMilliseconds.end(), 0.0) / Frames;
		std::cout
			<< "{\"Width\":" << Width
			<< ",\"Height\":" << Height
			<< ",\"TargetFps\":" << TargetFps
			<< ",\"Frames\":" << Frames
			<< ",\"AchievedFps\":" << (Frames / Elapsed)
			<< ",\"CaptureMeanMs\":" << Mean
			<< ",\"CaptureP50Ms\":" << Percentile(CaptureMilliseconds, 0.50)
			<< ",\"CaptureP95Ms\":" << Percentile(CaptureMilliseconds, 0.95)
			<< ",\"CaptureP99Ms\":" << Percentile(CaptureMilliseconds, 0.99)
			<< ",\"CpuPayloadMiBPerSecond\":" << (PayloadBytes / Elapsed / 1024.0 / 1024.0)
			<< "}\n";
	}
}

int main(int ArgumentCount, char **Arguments) {
	const double Seconds = ArgumentCount > 1 ? std::stod(Arguments[1]) : 2.0;
	for (const auto [Width, Height] : std::vector<std::pair<std::uint32_t, std::uint32_t>>{
		{844, 565}, {1280, 720}, {1920, 1080},
	})
		for (const auto TargetFps : {20u, 30u, 60u, 120u, 144u, 0u}) Run(Width, Height, TargetFps, Seconds);
	return 0;
}
