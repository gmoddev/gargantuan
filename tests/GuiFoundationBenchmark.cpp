// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.

#include "gargantuan/GuiRuntimeConfig.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Frame.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/ScreenGui.hpp"
#include "gargantuan/classes/TextButton.hpp"
#include "gargantuan/classes/TextLabel.hpp"
#include "gargantuan/classes/UIListLayout.hpp"
#include "gargantuan/gui/GuiRuntime.hpp"
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

	struct Distribution {
		double Mean = 0.0;
		double P50 = 0.0;
		double P95 = 0.0;
		double P99 = 0.0;
	};

	Distribution Summarize(std::vector<double> Values) {
		if (Values.empty()) return {};
		const double Mean = std::accumulate(Values.begin(), Values.end(), 0.0) / Values.size();
		std::ranges::sort(Values);
		auto Percentile = [&](double P) {
			return Values[std::min(Values.size() - 1,
				static_cast<std::size_t>(std::ceil(P * static_cast<double>(Values.size()))) - 1)];
		};
		return {Mean, Percentile(0.50), Percentile(0.95), Percentile(0.99)};
	}

	struct ScenarioResult {
		std::string Name;
		std::size_t Nodes = 0;
		Distribution TotalMicroseconds;
		Distribution SemanticMicroseconds;
		Distribution LayoutMicroseconds;
		Distribution TextMicroseconds;
		Distribution DisplayMicroseconds;
		Distribution PublicationMicroseconds;
		Distribution ProjectionMicroseconds;
		std::size_t Batches = 0;
		std::size_t Primitives = 0;
		std::size_t TextureUpdates = 0;
		std::size_t UploadedBytes = 0;
	};

	struct Scene {
		std::shared_ptr<gargantuan::DataModel> Game = std::make_shared<gargantuan::DataModel>();
		std::shared_ptr<gargantuan::Workspace> Workspace =
			std::dynamic_pointer_cast<gargantuan::Workspace>(Game->GetService("Workspace"));
		gargantuan::GuiRuntime Runtime{Game, std::filesystem::path(gargantuan::DefaultGuiFontPath)};
		gargantuan::RenderPublisher Publisher;
		gargantuan::RenderProjection Projection;
		std::shared_ptr<gargantuan::ScreenGui> Root = std::make_shared<gargantuan::ScreenGui>();
		std::vector<std::shared_ptr<gargantuan::GuiObject>> Nodes;

		Scene() {
			Runtime.SetViewport({1920, 1080, 1.0f, {}});
			Root->SetParent(Game);
			Publisher.SetProfilingEnabled(true);
		}

		void CommitInitial() {
			(void)Runtime.Reconcile();
			Runtime.Publish(Publisher);
			(void)Projection.Apply(*Publisher.Publish(*Workspace, gargantuan::RenderCameraInput{}, 1920, 1080));
		}
	};

	void AddFrames(Scene &Value, std::size_t Count, bool Interactable = false) {
		using namespace gargantuan;
		Value.Nodes.reserve(Value.Nodes.size() + Count);
		for (std::size_t Index = 0; Index < Count; ++Index) {
			auto Node = std::make_shared<Frame>();
			Node->SetSize(UDim2::fromOffset(12, 8));
			Node->SetPosition(UDim2::fromOffset(static_cast<int>((Index % 120) * 14), static_cast<int>((Index / 120) * 10)));
			Node->SetInteractable(Interactable);
			Node->SetParent(Value.Root);
			Value.Nodes.push_back(std::move(Node));
		}
	}

	void AddButtons(Scene &Value, std::size_t Count) {
		using namespace gargantuan;
		Value.Nodes.reserve(Value.Nodes.size() + Count);
		for (std::size_t Index = 0; Index < Count; ++Index) {
			auto Node = std::make_shared<TextButton>();
			Node->SetText("Button");
			Node->SetTextSize(12.0f);
			Node->SetSize(UDim2::fromOffset(56, 18));
			Node->SetPosition(UDim2::fromOffset(static_cast<int>((Index % 32) * 59), static_cast<int>((Index / 32) * 20)));
			Node->SetParent(Value.Root);
			Value.Nodes.push_back(std::move(Node));
		}
	}

	template <typename Mutate>
	ScenarioResult Run(std::string Name, Scene &Value, std::size_t Frames, Mutate Update) {
		using namespace gargantuan;
		std::vector<double> Total;
		std::vector<double> Semantic;
		std::vector<double> Layout;
		std::vector<double> Text;
		std::vector<double> Display;
		std::vector<double> Publication;
		std::vector<double> Projection;
		Total.reserve(Frames);
		for (std::size_t Frame = 0; Frame < Frames; ++Frame) {
			const auto Start = Clock::now();
			Update(Frame);
			(void)Value.Runtime.Reconcile();
			Value.Runtime.Publish(Value.Publisher);
			const auto Viewport = Value.Runtime.GetCommittedLayout()->Viewport;
			const auto PublicationValue = Value.Publisher.Publish(
				*Value.Workspace, RenderCameraInput{}, Viewport.PhysicalWidth, Viewport.PhysicalHeight);
			const auto ProjectionStart = Clock::now();
			(void)Value.Projection.Apply(*PublicationValue);
			const auto End = Clock::now();
			const auto GuiProfile = Value.Runtime.GetLastProfile();
			const auto PublisherProfile = Value.Publisher.GetLastProfile();
			Total.push_back(std::chrono::duration<double, std::micro>(End - Start).count());
			Semantic.push_back(static_cast<double>(GuiProfile.SemanticDirtyNanoseconds) / 1000.0);
			Layout.push_back(static_cast<double>(GuiProfile.MeasureNanoseconds + GuiProfile.ArrangeNanoseconds) / 1000.0);
			Text.push_back(static_cast<double>(GuiProfile.TextShapingNanoseconds + GuiProfile.GlyphLookupNanoseconds +
				GuiProfile.GlyphRasterizationNanoseconds + GuiProfile.AtlasUpdateNanoseconds) / 1000.0);
			Display.push_back(static_cast<double>(GuiProfile.DisplayListNanoseconds + GuiProfile.BatchingNanoseconds) / 1000.0);
			Publication.push_back(static_cast<double>(GuiProfile.PublicationNanoseconds +
				PublisherProfile.PublicationConstructionNanoseconds) / 1000.0);
			Projection.push_back(std::chrono::duration<double, std::micro>(End - ProjectionStart).count());
		}
		const auto Profile = Value.Runtime.GetLastProfile();
		const auto PresentationValue = Value.Runtime.GetCommittedPresentation();
		std::size_t PrimitiveCount = 0;
		for (const auto &Batch : PresentationValue->Frame.Batches) PrimitiveCount += Batch.Indices.size() / 6;
		return {std::move(Name), Value.Nodes.size(), Summarize(std::move(Total)), Summarize(std::move(Semantic)), Summarize(std::move(Layout)),
			Summarize(std::move(Text)), Summarize(std::move(Display)), Summarize(std::move(Publication)),
			Summarize(std::move(Projection)), PresentationValue->Frame.Batches.size(), PrimitiveCount,
			Profile.TextureUpdates, Profile.TextureUploadBytes};
	}

	ScenarioResult FrameScenario(std::string Name, std::size_t Count, std::size_t Frames, int Mutation) {
		using namespace gargantuan;
		Scene Value;
		AddFrames(Value, Count);
		Value.CommitInitial();
		return Run(std::move(Name), Value, Frames, [&](std::size_t Frame) {
			if (Mutation == 0) return;
			const std::size_t Changed = std::max<std::size_t>(1, Count / 100);
			for (std::size_t Index = 0; Index < Changed; ++Index) {
				auto Node = std::dynamic_pointer_cast<gargantuan::Frame>(Value.Nodes[(Frame * Changed + Index) % Count]);
				if (Mutation == 1) Node->SetBackgroundColor3(Color3((Frame & 1) ? 0.25f : 0.75f, 0.4f, 0.6f));
				else Node->SetPosition(UDim2::fromOffset(static_cast<int>(((Index + Frame) % 120) * 14),
					static_cast<int>(((Index + Frame) / 120) * 10)));
			}
		});
	}

	ScenarioResult TextScenario(std::string Name, std::size_t Count, std::size_t Frames, bool Unique) {
		using namespace gargantuan;
		Scene Value;
		for (std::size_t Index = 0; Index < Count; ++Index) {
			auto Label = std::make_shared<TextLabel>();
			Label->SetText(Unique ? "Label " + std::to_string(Index) + " â" : "Repeated Gargantuan label");
			Label->SetTextSize(14.0f);
			Label->SetSize(UDim2::fromOffset(180, 22));
			Label->SetPosition(UDim2::fromOffset(static_cast<int>((Index % 10) * 185), static_cast<int>((Index / 10) * 24)));
			Label->SetParent(Value.Root);
			Value.Nodes.push_back(std::move(Label));
		}
		Value.CommitInitial();
		return Run(std::move(Name), Value, Frames, [&](std::size_t Frame) {
			if (!Unique) return;
			const std::size_t Changed = std::max<std::size_t>(1, Count / 100);
			for (std::size_t Index = 0; Index < Changed; ++Index) {
				auto Label = std::dynamic_pointer_cast<TextLabel>(Value.Nodes[(Frame * Changed + Index) % Count]);
				Label->SetText("Dynamic " + std::to_string(Frame) + "/" + std::to_string(Index) + " UTF-8");
			}
		});
	}

	ScenarioResult ButtonScenario(std::string Name, std::size_t Count, std::size_t Frames) {
		Scene Value;
		AddButtons(Value, Count);
		Value.CommitInitial();
		return Run(std::move(Name), Value, Frames, [&](std::size_t Frame) {
			const float X = static_cast<float>((Frame * 59) % 1888 + 4);
			const float Y = static_cast<float>((Frame * 20) % 1060 + 4);
			(void)Value.Runtime.ProcessPointer({3, gargantuan::Enums::GuiPointerType::Mouse,
				gargantuan::Enums::GuiPointerButton::None, gargantuan::GuiPointerAction::Move, {X, Y}});
		});
	}

	ScenarioResult ClipScenario(std::size_t Count, std::size_t Frames) {
		using namespace gargantuan;
		Scene Value;
		std::shared_ptr<Instance> Parent = Value.Root;
		for (int Depth = 0; Depth < 24; ++Depth) {
			auto FrameValue = std::make_shared<Frame>();
			FrameValue->SetPosition(UDim2::fromOffset(1, 1));
			FrameValue->SetSize(UDim2(1.0f, -2, 1.0f, -2));
			FrameValue->SetParent(Parent);
			Parent = FrameValue;
			Value.Nodes.push_back(std::move(FrameValue));
		}
		for (std::size_t Index = Value.Nodes.size(); Index < Count; ++Index) {
			auto FrameValue = std::make_shared<Frame>();
			FrameValue->SetSize(UDim2::fromOffset(10, 10));
			FrameValue->SetPosition(UDim2::fromOffset(static_cast<int>(Index % 100) * 11, static_cast<int>(Index / 100) * 11));
			FrameValue->SetParent(Parent);
			Value.Nodes.push_back(std::move(FrameValue));
		}
		Value.CommitInitial();
		return Run("deep_nested_clipping", Value, Frames, [](std::size_t) {});
	}

	ScenarioResult AutomaticListScenario(std::size_t Count, std::size_t Frames) {
		using namespace gargantuan;
		Scene Value;
		for (std::size_t Group = 0; Value.Nodes.size() < Count; ++Group) {
			auto Container = std::make_shared<Frame>();
			Container->SetPosition(UDim2::fromOffset(static_cast<int>(Group % 40) * 45, static_cast<int>(Group / 40) * 70));
			Container->SetAutomaticSize(Enums::AutomaticSize::XY);
			Container->SetParent(Value.Root);
			Value.Nodes.push_back(Container);
			auto List = std::make_shared<UIListLayout>();
			List->SetPadding({0.0f, 2});
			List->SetParent(Container);
			for (int Child = 0; Child < 4 && Value.Nodes.size() < Count; ++Child) {
				auto FrameValue = std::make_shared<Frame>();
				FrameValue->SetSize(UDim2::fromOffset(40, 12));
				FrameValue->SetParent(Container);
				Value.Nodes.push_back(std::move(FrameValue));
			}
		}
		Value.CommitInitial();
		return Run("automatic_size_list_layout", Value, Frames, [&](std::size_t Frame) {
			auto Leaf = std::dynamic_pointer_cast<gargantuan::Frame>(Value.Nodes[(Frame * 5 + 1) % Value.Nodes.size()]);
			if (Leaf) Leaf->SetSize(UDim2::fromOffset(40 + static_cast<int>(Frame & 1), 12));
		});
	}

	ScenarioResult ViewportScenario(std::size_t Count, std::size_t Frames) {
		Scene Value;
		AddFrames(Value, Count);
		Value.CommitInitial();
		return Run("viewport_resize_dpi_safe_area", Value, Frames, [&](std::size_t Frame) {
			if (Frame & 1) Value.Runtime.SetViewport({1170, 2532, 3.0f, {0, 16, 0, 12}});
			else Value.Runtime.SetViewport({2560, 1440, 1.0f, {}});
		});
	}

	ScenarioResult MixedScenario(std::size_t Count, std::size_t Frames) {
		using namespace gargantuan;
		Scene Value;
		AddFrames(Value, Count);
		for (int Index = 0; Index < 100; ++Index) {
			auto WorldPart = std::make_shared<Part>();
			WorldPart->SetPosition({static_cast<float>(Index), 0.0f, 0.0f});
			WorldPart->SetParent(Value.Workspace);
		}
		Value.CommitInitial();
		return Run("mixed_world_gui_publication", Value, Frames, [](std::size_t) {});
	}

	ScenarioResult HitScenario(std::string Name, std::size_t Count, std::size_t Frames) {
		using namespace gargantuan;
		Scene Value;
		AddFrames(Value, Count, true);
		Value.CommitInitial();
		std::vector<double> Samples;
		Samples.reserve(Frames);
		for (std::size_t Frame = 0; Frame < Frames; ++Frame) {
			const Vector2 Point(static_cast<float>((Frame % 120) * 14 + 3),
				static_cast<float>(((Frame / 120) % 90) * 10 + 3));
			const auto Start = Clock::now();
			(void)Value.Runtime.ProcessPointer({5, Enums::GuiPointerType::Mouse, Enums::GuiPointerButton::None,
				GuiPointerAction::Move, Point});
			Samples.push_back(std::chrono::duration<double, std::micro>(Clock::now() - Start).count());
		}
		ScenarioResult Result;
		Result.Name = std::move(Name);
		Result.Nodes = Count;
		Result.TotalMicroseconds = Summarize(std::move(Samples));
		Result.Batches = Value.Runtime.GetCommittedPresentation()->Frame.Batches.size();
		return Result;
	}

	void Print(const ScenarioResult &Result) {
		auto Part = [](const Distribution &Value) {
			std::cout << std::fixed << std::setprecision(2) << Value.Mean << ',' << Value.P50 << ',' << Value.P95 << ',' << Value.P99;
		};
		std::cout << Result.Name << ',' << Result.Nodes << ',';
		Part(Result.TotalMicroseconds); std::cout << ',';
		Part(Result.SemanticMicroseconds); std::cout << ',';
		Part(Result.LayoutMicroseconds); std::cout << ',';
		Part(Result.TextMicroseconds); std::cout << ',';
		Part(Result.DisplayMicroseconds); std::cout << ',';
		Part(Result.PublicationMicroseconds); std::cout << ',';
		Part(Result.ProjectionMicroseconds);
		std::cout << ',' << Result.Batches << ',' << Result.Primitives << ',' << Result.TextureUpdates << ',' << Result.UploadedBytes << '\n';
	}
}

int main(int ArgumentCount, char **Arguments) {
	const bool Quick = ArgumentCount > 1 && std::string_view(Arguments[1]) == "--quick";
	try {
		gargantuan::BootstrapNativeRuntimeSchema();
		const std::size_t Small = Quick ? 100 : 1'000;
		const std::size_t Large = Quick ? 300 : 10'000;
		const std::size_t Frames = Quick ? 3 : 120;
		std::vector<ScenarioResult> Results;
		Results.push_back(FrameScenario("frames_1000_static", Small, Frames, 0));
		Results.push_back(FrameScenario("frames_10000_static", Large, Frames, 0));
		Results.push_back(FrameScenario("frames_10000_1pct_presentation", Large, Frames, 1));
		Results.push_back(FrameScenario("frames_10000_1pct_layout", Large, Frames, 2));
		Results.push_back(ClipScenario(Large, Frames));
		Results.push_back(TextScenario("text_repeated", Quick ? 40 : 1'000, Frames, false));
		Results.push_back(TextScenario("text_unique", Quick ? 40 : 1'000, Frames, true));
		Results.push_back(ButtonScenario("buttons_5000_and_hit_test", Quick ? 100 : 5'000, Frames));
		Results.push_back(ButtonScenario("buttons_10000_and_hit_test", Quick ? 300 : 10'000, Frames));
		Results.push_back(HitScenario("hit_test_1000", Small, Quick ? 30 : 1'000));
		Results.push_back(HitScenario("hit_test_10000", Large, Quick ? 30 : 1'000));
		Results.push_back(AutomaticListScenario(Quick ? 100 : 5'000, Frames));
		Results.push_back(ViewportScenario(Small, Frames));
		Results.push_back(MixedScenario(Small, Frames));
		std::cout << "scenario,nodes,total_mean_us,total_p50_us,total_p95_us,total_p99_us,"
			"semantic_mean_us,semantic_p50_us,semantic_p95_us,semantic_p99_us,"
			"layout_mean_us,layout_p50_us,layout_p95_us,layout_p99_us,"
			"text_mean_us,text_p50_us,text_p95_us,text_p99_us,"
			"display_mean_us,display_p50_us,display_p95_us,display_p99_us,"
			"publication_mean_us,publication_p50_us,publication_p95_us,publication_p99_us,"
			"projection_mean_us,projection_p50_us,projection_p95_us,projection_p99_us,"
			"batches,primitives,texture_updates,uploaded_bytes\n";
		for (const auto &Result : Results) Print(Result);
		std::cout << "[Gui:Benchmark] GPU preparation is intentionally unavailable in this headless run; use the Renderer 2C GPU benchmark for backend-only cost.\n";
	} catch (const std::exception &Exception) {
		std::cerr << "[Gui:Benchmark] failed: " << Exception.what() << '\n';
		return 1;
	}
	return 0;
}
