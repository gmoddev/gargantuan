// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "BenchmarkAllocationProbe.hpp"
#include "gargantuan/render/RenderProjection.hpp"

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string_view>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {
	using namespace gargantuan;
	using Clock = std::chrono::steady_clock;

	void Require(bool Value) {
		if (!Value) throw std::runtime_error("[Render:ProjectionBenchmark] committed projection disagrees with input");
	}

	void Measure(std::string_view Scene) {
		constexpr unsigned Warmup = 200, Samples = 400;
		RenderFrameState Frame{.ViewportWidth = 640, .ViewportHeight = 360};
		RenderProjection Projection;
		RenderPublication Initial{.Id = 1, .FullResync = true, .Frame = Frame};
		const unsigned Count = Scene == "Camera" ? 24 : 96;
		for (unsigned Slot = 1; Slot <= Count; ++Slot)
			Initial.Creates.push_back({RenderItem{.Object = {Slot, 1}}});
		Require(Projection.Apply(Initial).Created == Count);

		std::array<RenderPublication, 3> Packets{};
		for (auto &Packet : Packets) {
			Packet.Frame = Frame;
			if (Scene != "Camera") {
				for (unsigned Slot = 1; Slot <= 12; ++Slot) {
					const ObjectId Id{Slot, 1};
					Packet.Updates.push_back({Id, RenderUpdateDomain::Transform, RenderItem{.Object = Id}});
				}
			}
		}
		if (Scene == "Resources") {
			const auto Vertices = std::make_shared<const std::vector<RenderVertex>>(3);
			const auto Indices = std::make_shared<const std::vector<std::uint32_t>>(
				std::initializer_list<std::uint32_t>{0, 1, 2});
			const auto Pixels = std::make_shared<const std::vector<std::uint8_t>>(16, 255);
			const RenderBounds Bounds{glm::vec3(-1), glm::vec3(1)};
			Packets[0].MeshCreates.push_back({{1, 1}, 1, 1, Vertices, Indices, Bounds});
			Packets[0].TextureCreates.push_back({{2, 1}, 1, 2, 2, RenderTextureFormat::Rgba8Unorm, Pixels});
			Packets[1].MeshVertexUpdates.push_back({{1, 1}, 2, 0, Vertices, Bounds});
			Packets[1].TextureUpdates.push_back({{2, 1}, 2, 0, 0, 2, 2, Pixels});
			Packets[2].MeshRemoves.push_back({{1, 1}});
			Packets[2].TextureRemoves.push_back({{2, 1}});
		}

		BenchmarkAllocationProbe::Counts Allocations;
		std::uint64_t Elapsed = 0, Validation = 0, Mutation = 0;
		for (unsigned Index = 0; Index < Warmup + Samples; ++Index) {
			const auto Phase = Index % Packets.size();
			auto &Packet = Packets[Phase];
			Packet.BaseId = Projection.GetLastPublicationId();
			Packet.Id = Packet.BaseId + 1;
			Packet.Frame.Camera.Position.x = static_cast<float>(Index % 7);
			for (auto &Update : Packet.Updates) {
				Update.Item.ModelMatrix[3].x = static_cast<float>(Index);
				Update.Item.InverseModelMatrix[3].x = -static_cast<float>(Index);
			}
			RenderProjectionChanges Changes;
			if (Index < Warmup) {
				Changes = Projection.Apply(Packet);
			} else {
				const auto Start = Clock::now();
				{
					const BenchmarkAllocationProbe::Scope Observe(Allocations);
					Changes = Projection.Apply(Packet);
				}
				Elapsed += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - Start).count();
				Validation += Changes.ValidationNanoseconds;
				Mutation += Changes.MutationNanoseconds;
			}
			Require(Changes.Updated == Packet.Updates.size() && Projection.GetSize() == Count &&
				Projection.GetLastPublicationId() == Packet.Id &&
				Projection.GetFrame().Camera.Position.x == Packet.Frame.Camera.Position.x);
			if (!Packet.Updates.empty())
				Require(Projection.GetItem({1, 1})->ModelMatrix[3].x == static_cast<float>(Index));
			const auto Resident = static_cast<std::size_t>(Scene == "Resources" && Phase != 2);
			Require(Projection.GetMeshCount() == Resident && Projection.GetTextureCount() == Resident);
		}
		const auto Last = Projection.GetLastPublicationId();
		bool Rejected = false;
		try { (void)Projection.Apply(Packets[2]); } catch (const std::invalid_argument &) { Rejected = true; }
		Require(Rejected && Projection.GetLastPublicationId() == Last);
		std::cout << Scene << ',' << Samples << ',' << Elapsed << ',' << Validation << ',' << Mutation
			<< ',' << Allocations.Calls << ',' << Allocations.Bytes << '\n';
	}
}

int main(int ArgCount, char **Args) {
	try {
#ifdef _WIN32
		if (ArgCount == 2) {
			const auto Cpu = std::stoul(Args[1]);
			if (Cpu >= 64 || !SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << Cpu))
				throw std::runtime_error("[Render:ProjectionBenchmark] requested CPU unavailable");
		}
#endif
		Measure("Camera");
		Measure("Transforms");
		Measure("Resources");
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << Error.what() << '\n';
		return 1;
	}
}
