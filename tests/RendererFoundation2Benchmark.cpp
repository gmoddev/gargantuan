// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/RenderExtractor.hpp"
#include "gargantuan/render/RenderProjection.hpp"
#include "gargantuan/services/Workspace.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

namespace {
	using Clock = std::chrono::steady_clock;

	double Milliseconds(Clock::duration Duration) {
		return std::chrono::duration<double, std::milli>(Duration).count();
	}

	double Mean(const std::vector<double> &Samples) {
		return Samples.empty() ? 0.0 : std::accumulate(Samples.begin(), Samples.end(), 0.0) / Samples.size();
	}

	struct PipelineBreakdown {
		double PropertySetterOtherMeanMs = 0.0;
		double JournalMeanMs = 0.0;
		double ClassificationMeanMs = 0.0;
		double AccumulatorMeanMs = 0.0;
		double DirtyCaptureMeanMs = 0.0;
		double DirtyExpansionMeanMs = 0.0;
		double FinalStateExtractionMeanMs = 0.0;
		double PublicationConstructionMeanMs = 0.0;
		double PublisherReconciliationMeanMs = 0.0;
	};

	void Print(
		std::string_view Scenario,
		std::size_t Elements,
		std::size_t Frames,
		const std::vector<double> &Accumulation,
		const std::vector<double> &Generation,
		const std::vector<double> &Application,
		std::size_t Creates,
		std::size_t Updates,
		std::size_t Removes,
		std::size_t UploadBytes,
		std::size_t Draws,
		std::size_t FullResyncs,
		const PipelineBreakdown &Breakdown = {}
	) {
		std::cout << Scenario << ',' << Elements << ',' << Frames << ',' << std::fixed << std::setprecision(4)
			<< Mean(Accumulation) << ',' << Mean(Generation) << ',' << Mean(Application) << ',' << Creates << ',' << Updates << ',' << Removes << ','
			<< UploadBytes << ',' << Draws << ',' << FullResyncs << ','
			<< Breakdown.PropertySetterOtherMeanMs << ',' << Breakdown.JournalMeanMs << ','
			<< Breakdown.ClassificationMeanMs << ',' << Breakdown.AccumulatorMeanMs << ','
			<< Breakdown.DirtyCaptureMeanMs << ',' << Breakdown.DirtyExpansionMeanMs << ','
			<< Breakdown.FinalStateExtractionMeanMs << ',' << Breakdown.PublicationConstructionMeanMs << ','
			<< Breakdown.PublisherReconciliationMeanMs << '\n';
	}

	void RunRigid(
		std::string_view Name,
		std::size_t Count,
		std::size_t ChangedPerFrame,
		std::size_t WritesPerObject,
		std::size_t Frames,
		bool ProfileEnabled
	) {
		using namespace gargantuan;
		ChangeJournal::Get().Clear();
		RenderDirtyAccumulator::Get().Clear();
		auto Game = std::make_shared<DataModel>();
		auto World = std::dynamic_pointer_cast<Workspace>(Game->GetService("Workspace"));
		std::vector<std::shared_ptr<Part>> Parts;
		Parts.reserve(Count);
		const auto Side = static_cast<std::size_t>(std::ceil(std::sqrt(static_cast<double>(Count))));
		for (std::size_t Index = 0; Index < Count; ++Index) {
			auto Value = std::make_shared<Part>();
			Value->SetCFrame(CFrame(glm::vec3(
				static_cast<float>(Index % Side) * 2.0f, static_cast<float>(Index / Side) * 2.0f, 0.0f
			)));
			Value->SetParent(World);
			Parts.push_back(std::move(Value));
		}
		RenderPublisher Publisher;
		RenderProjection Projection;
		const auto Camera = MakeLookAtRenderCameraInput(
			glm::vec3(0.0f, 0.0f, 1000.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f)
		);
		auto Initial = Publisher.Publish(*World, Camera, 1280, 720);
		(void)Projection.Apply(*Initial);
		auto &Dirty = RenderDirtyAccumulator::Get();
		auto &Journal = ChangeJournal::Get();
		Dirty.ResetProfile();
		Journal.ResetProfile();
		Dirty.SetProfilingEnabled(ProfileEnabled);
		Journal.SetProfilingEnabled(ProfileEnabled);
		Publisher.SetProfilingEnabled(ProfileEnabled);
		std::vector<double> Accumulation;
		std::vector<double> Generation;
		std::vector<double> Application;
		std::vector<double> DirtyCapture;
		std::vector<double> DirtyExpansion;
		std::vector<double> FinalStateExtraction;
		std::vector<double> PublicationConstruction;
		std::vector<double> PublisherReconciliation;
		std::size_t Creates = 0, Updates = 0, Removes = 0, UploadBytes = 0, FullResyncs = 0;
		for (std::size_t Frame = 0; Frame < Frames; ++Frame) {
			const auto AccumulationStart = Clock::now();
			for (std::size_t Index = 0; Index < ChangedPerFrame; ++Index) {
				const auto ObjectIndex = (Index + Frame * ChangedPerFrame) % Count;
				for (std::size_t Write = 0; Write < WritesPerObject; ++Write) {
					auto Current = Parts[ObjectIndex]->GetCFrame();
					Current.Position.z = static_cast<float>(Frame + 1) * 0.01f + static_cast<float>(Write) * 0.000001f;
					Parts[ObjectIndex]->SetCFrame(Current);
				}
			}
			Accumulation.push_back(Milliseconds(Clock::now() - AccumulationStart));
			const auto GenerationStart = Clock::now();
			auto Publication = Publisher.Publish(*World, Camera, 1280, 720);
			Generation.push_back(Milliseconds(Clock::now() - GenerationStart));
			if (ProfileEnabled) {
				const auto PublisherProfile = Publisher.GetLastProfile();
				DirtyCapture.push_back(static_cast<double>(PublisherProfile.DirtyCaptureNanoseconds) / 1'000'000.0);
				DirtyExpansion.push_back(static_cast<double>(PublisherProfile.DirtyExpansionNanoseconds) / 1'000'000.0);
				FinalStateExtraction.push_back(static_cast<double>(PublisherProfile.FinalStateExtractionNanoseconds) / 1'000'000.0);
				PublicationConstruction.push_back(static_cast<double>(PublisherProfile.PublicationConstructionNanoseconds) / 1'000'000.0);
				PublisherReconciliation.push_back(static_cast<double>(PublisherProfile.StateCacheReconciliationNanoseconds) / 1'000'000.0);
			}
			const auto ApplyStart = Clock::now();
			const auto Changes = Projection.Apply(*Publication);
			Application.push_back(Milliseconds(Clock::now() - ApplyStart));
			Creates += Changes.Created;
			Updates += Changes.Updated;
			Removes += Changes.Removed;
			UploadBytes += Changes.VertexUploadBytes;
			FullResyncs += Publication->FullResync ? 1 : 0;
		}
		Dirty.SetProfilingEnabled(false);
		Journal.SetProfilingEnabled(false);
		PipelineBreakdown Breakdown;
		if (ProfileEnabled) {
			const auto DirtyProfile = Dirty.GetProfile();
			const auto JournalProfile = Journal.GetProfile();
			Breakdown = {
				.JournalMeanMs = static_cast<double>(JournalProfile.JournalNanoseconds) / 1'000'000.0 / Frames,
				.ClassificationMeanMs = static_cast<double>(DirtyProfile.ClassificationNanoseconds) / 1'000'000.0 / Frames,
				.AccumulatorMeanMs = static_cast<double>(DirtyProfile.AccumulationNanoseconds) / 1'000'000.0 / Frames,
				.DirtyCaptureMeanMs = Mean(DirtyCapture),
				.DirtyExpansionMeanMs = Mean(DirtyExpansion),
				.FinalStateExtractionMeanMs = Mean(FinalStateExtraction),
				.PublicationConstructionMeanMs = Mean(PublicationConstruction),
				.PublisherReconciliationMeanMs = Mean(PublisherReconciliation),
			};
			Breakdown.PropertySetterOtherMeanMs = std::max(
				0.0,
				Mean(Accumulation) - Breakdown.JournalMeanMs - Breakdown.ClassificationMeanMs - Breakdown.AccumulatorMeanMs
			);
		}
		Print(Name, Count, Frames, Accumulation, Generation, Application, Creates, Updates, Removes, UploadBytes, Count,
			FullResyncs, Breakdown);
		Game->Destroy();
	}

	void RunDeformable(std::size_t VertexCount, std::size_t Frames) {
		using namespace gargantuan;
		RenderProjection Projection;
		auto Vertices = std::make_shared<const std::vector<RenderVertex>>(std::vector<RenderVertex>(VertexCount));
		auto Indices = std::make_shared<const std::vector<std::uint32_t>>(std::vector<std::uint32_t>{0, 1, 2});
		RenderPublication Initial{.Id = 1, .FullResync = true, .Frame = {.ViewportWidth = 1280, .ViewportHeight = 720}};
		Initial.MeshCreates.push_back({{1, 1}, 1, 1, Vertices, Indices, {glm::vec3(-1.0f), glm::vec3(1.0f)}});
		(void)Projection.Apply(Initial);
		std::vector<double> Generation;
		std::vector<double> Application;
		std::size_t UploadBytes = 0;
		for (std::size_t Frame = 0; Frame < Frames; ++Frame) {
			const auto GenerationStart = Clock::now();
			auto Changed = std::make_shared<std::vector<RenderVertex>>(VertexCount);
			for (std::size_t Index = 0; Index < VertexCount; ++Index) {
				const auto Phase = static_cast<float>(Index + Frame) * 0.01f;
				(*Changed)[Index].Position = {static_cast<float>(Index), std::sin(Phase), 0.0f};
				(*Changed)[Index].Normal = glm::normalize(glm::vec3(-std::cos(Phase) * 0.01f, 1.0f, 0.0f));
			}
			RenderPublication Publication{.Id = Frame + 2, .BaseId = Frame + 1, .Frame = Initial.Frame};
			Publication.MeshVertexUpdates.push_back({
				{1, 1}, Frame + 2, 0, std::shared_ptr<const std::vector<RenderVertex>>(Changed),
				{glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(static_cast<float>(VertexCount), 1.0f, 0.0f)},
			});
			Generation.push_back(Milliseconds(Clock::now() - GenerationStart));
			const auto ApplyStart = Clock::now();
			const auto Changes = Projection.Apply(Publication);
			Application.push_back(Milliseconds(Clock::now() - ApplyStart));
			UploadBytes += Changes.VertexUploadBytes;
		}
		Print("deformable", VertexCount, Frames, {}, Generation, Application, 0, Frames, 0, UploadBytes, 1, 0);
	}

	void RunGui(std::size_t QuadCount, std::size_t Frames) {
		using namespace gargantuan;
		RenderProjection Projection;
		RenderPublication Initial{.Id = 1, .FullResync = true, .Frame = {.ViewportWidth = 1280, .ViewportHeight = 720}};
		auto AtlasPixels = std::make_shared<const std::vector<std::uint8_t>>(std::vector<std::uint8_t>(4, 255));
		for (std::uint64_t Texture = 1; Texture <= 8; ++Texture)
			Initial.TextureCreates.push_back({{Texture, 1}, 1, 1, 1, RenderTextureFormat::Rgba8Unorm, AtlasPixels});
		(void)Projection.Apply(Initial);
		std::vector<double> Generation;
		std::vector<double> Application;
		std::size_t UploadBytes = 0;
		constexpr std::size_t QuadsPerBatch = 256;
		for (std::size_t Frame = 0; Frame < Frames; ++Frame) {
			const auto GenerationStart = Clock::now();
			RenderPublication Publication{.Id = Frame + 2, .BaseId = Frame + 1, .Frame = Initial.Frame};
			Publication.Ui = {1280, 720, 1.0f, {}};
			for (std::size_t First = 0; First < QuadCount; First += QuadsPerBatch) {
				const auto BatchQuads = std::min(QuadsPerBatch, QuadCount - First);
				RenderUiBatch Batch;
				Batch.Texture = RenderTextureIdentity{1 + (First / QuadsPerBatch) % 8, 1};
				Batch.Layer = static_cast<std::int32_t>(First / QuadsPerBatch);
				Batch.Clip = RenderUiClipRect{0.0f, 0.0f, 1280.0f, 720.0f};
				Batch.Vertices.resize(BatchQuads * 4);
				Batch.Indices.reserve(BatchQuads * 6);
				for (std::size_t Quad = 0; Quad < BatchQuads; ++Quad) {
					const auto Base = static_cast<std::uint32_t>(Quad * 4);
					Batch.Indices.insert(Batch.Indices.end(), {Base, Base + 1, Base + 2, Base + 2, Base + 3, Base});
				}
				Publication.Ui.Batches.push_back(std::move(Batch));
			}
			Generation.push_back(Milliseconds(Clock::now() - GenerationStart));
			const auto ApplyStart = Clock::now();
			const auto Changes = Projection.Apply(Publication);
			Application.push_back(Milliseconds(Clock::now() - ApplyStart));
			UploadBytes += Changes.UiVertices * sizeof(RenderUiVertex) + Changes.UiIndices * sizeof(std::uint32_t);
		}
		Print("gui", QuadCount, Frames, {}, Generation, Application, 0, 0, 0, UploadBytes,
			(QuadCount + QuadsPerBatch - 1) / QuadsPerBatch, 0);
	}

	void RunMixed(std::size_t Frames) {
		using namespace gargantuan;
		constexpr std::size_t RigidCount = 50000;
		constexpr std::size_t ChangedRigid = 500;
		constexpr std::size_t VertexCount = 16384;
		constexpr std::size_t QuadCount = 5000;
		constexpr std::size_t QuadsPerBatch = 256;
		RenderProjection Projection;
		RenderPublication Initial{.Id = 1, .FullResync = true, .Frame = {.ViewportWidth = 1280, .ViewportHeight = 720}};
		Initial.Creates.reserve(RigidCount);
		for (std::size_t Index = 0; Index < RigidCount; ++Index)
			Initial.Creates.push_back({RenderItem{.Object = {static_cast<std::uint32_t>(Index + 1), 1}}});
		auto Vertices = std::make_shared<const std::vector<RenderVertex>>(std::vector<RenderVertex>(VertexCount));
		auto Indices = std::make_shared<const std::vector<std::uint32_t>>(std::vector<std::uint32_t>{0, 1, 2});
		Initial.MeshCreates.push_back({{1, 1}, 1, 1, Vertices, Indices, {glm::vec3(-1.0f), glm::vec3(1.0f)}});
		auto AtlasPixels = std::make_shared<const std::vector<std::uint8_t>>(std::vector<std::uint8_t>(4, 255));
		for (std::uint64_t Texture = 1; Texture <= 8; ++Texture)
			Initial.TextureCreates.push_back({{Texture, 1}, 1, 1, 1, RenderTextureFormat::Rgba8Unorm, AtlasPixels});
		(void)Projection.Apply(Initial);
		std::vector<double> Generation;
		std::vector<double> Application;
		std::size_t Updates = 0;
		std::size_t UploadBytes = 0;
		for (std::size_t Frame = 0; Frame < Frames; ++Frame) {
			const auto GenerationStart = Clock::now();
			RenderPublication Publication{.Id = Frame + 2, .BaseId = Frame + 1, .Frame = Initial.Frame};
			Publication.Updates.reserve(ChangedRigid);
			for (std::size_t Index = 0; Index < ChangedRigid; ++Index) {
				const auto Slot = static_cast<std::uint32_t>((Index + Frame * ChangedRigid) % RigidCount + 1);
				RenderItem Item{.Object = {Slot, 1}};
				Item.ModelMatrix[3].z = static_cast<float>(Frame + 1) * 0.01f;
				Item.InverseModelMatrix[3].z = -Item.ModelMatrix[3].z;
				Publication.Updates.push_back({Item.Object, RenderUpdateDomain::Transform, Item});
			}
			auto ChangedVertices = std::make_shared<std::vector<RenderVertex>>(VertexCount);
			for (std::size_t Index = 0; Index < VertexCount; ++Index) {
				const auto Phase = static_cast<float>(Index + Frame) * 0.01f;
				(*ChangedVertices)[Index].Position = {static_cast<float>(Index), std::sin(Phase), 0.0f};
				(*ChangedVertices)[Index].Normal = glm::normalize(glm::vec3(-std::cos(Phase) * 0.01f, 1.0f, 0.0f));
			}
			Publication.MeshVertexUpdates.push_back({
				{1, 1}, Frame + 2, 0, std::shared_ptr<const std::vector<RenderVertex>>(ChangedVertices),
				{glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(static_cast<float>(VertexCount), 1.0f, 0.0f)},
			});
			Publication.Ui = {1280, 720, 1.0f, {}};
			for (std::size_t First = 0; First < QuadCount; First += QuadsPerBatch) {
				const auto BatchQuads = std::min(QuadsPerBatch, QuadCount - First);
				RenderUiBatch Batch;
				Batch.Texture = RenderTextureIdentity{1 + (First / QuadsPerBatch) % 8, 1};
				Batch.Clip = RenderUiClipRect{0.0f, 0.0f, 1280.0f, 720.0f};
				Batch.Vertices.resize(BatchQuads * 4);
				for (std::size_t Quad = 0; Quad < BatchQuads; ++Quad) {
					const auto Base = static_cast<std::uint32_t>(Quad * 4);
					Batch.Indices.insert(Batch.Indices.end(), {Base, Base + 1, Base + 2, Base + 2, Base + 3, Base});
				}
				Publication.Ui.Batches.push_back(std::move(Batch));
			}
			Generation.push_back(Milliseconds(Clock::now() - GenerationStart));
			const auto ApplyStart = Clock::now();
			const auto Changes = Projection.Apply(Publication);
			Application.push_back(Milliseconds(Clock::now() - ApplyStart));
			Updates += Changes.Updated + Changes.MeshesUpdated;
			UploadBytes += Changes.VertexUploadBytes + Changes.UiVertices * sizeof(RenderUiVertex) +
				Changes.UiIndices * sizeof(std::uint32_t);
		}
		Print("mixed", RigidCount, Frames, {}, Generation, Application, 0, Updates, 0, UploadBytes,
			RigidCount + 1 + (QuadCount + QuadsPerBatch - 1) / QuadsPerBatch, 0);
	}
}

int main(int ArgumentCount, char **Arguments) {
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
		const std::size_t Frames = ArgumentCount > 1 ? std::stoull(Arguments[1]) : 30;
		const bool ProfileEnabled = ArgumentCount > 2 && std::string_view(Arguments[2]) == "--profile";
		if (Frames == 0) throw std::invalid_argument("frame count must be positive");
		std::cout << "Scenario,Elements,Frames,DirtyAccumulationMeanMs,PublicationGenerationMeanMs,ProjectionApplyMeanMs,Creates,Updates,Removes,UploadBytes,DrawOrBatchCount,FullResyncs,PropertySetterOtherMeanMs,JournalMeanMs,ClassificationMeanMs,AccumulatorMeanMs,DirtyCaptureMeanMs,DirtyExpansionMeanMs,FinalStateExtractionMeanMs,PublicationConstructionMeanMs,PublisherReconciliationMeanMs\n";
		for (const auto Count : {1000u, 10000u, 50000u}) RunRigid("static", Count, 0, 1, Frames, ProfileEnabled);
		RunRigid("mostly-static", 50000, 500, 1, Frames, ProfileEnabled);
		RunRigid("dynamic", 50000, 50000, 1, Frames, ProfileEnabled);
		RunRigid("dynamic-redundant", 50000, 50000, 4, Frames, ProfileEnabled);
		for (const auto Count : {4096u, 16384u, 65536u}) RunDeformable(Count, Frames);
		RunGui(10000, Frames);
		RunMixed(Frames);
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Render:Foundation2Benchmark] " << Error.what() << '\n';
		return 1;
	}
}
