// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.

#include "gargantuan/GuiRuntimeConfig.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Frame.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/ScreenGui.hpp"
#include "gargantuan/classes/ScrollingFrame.hpp"
#include "gargantuan/classes/TextButton.hpp"
#include "gargantuan/classes/TextBox.hpp"
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
		Distribution ObservationMicroseconds;
		Distribution LayoutMicroseconds;
		Distribution TextMicroseconds;
		Distribution PresentationMicroseconds;
		Distribution AccessibilityMicroseconds;
		Distribution SnapshotCommitMicroseconds;
		Distribution DisplayMicroseconds;
		Distribution FrameConstructionMicroseconds;
		Distribution FrameCopyMicroseconds;
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
		std::vector<double> Observation;
		std::vector<double> Layout;
		std::vector<double> Text;
		std::vector<double> Presentation;
		std::vector<double> Accessibility;
		std::vector<double> SnapshotCommit;
		std::vector<double> Display;
		std::vector<double> FrameConstruction;
		std::vector<double> FrameCopy;
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
			Observation.push_back(static_cast<double>(GuiProfile.ObservationNanoseconds + GuiProfile.DirtyMarkingNanoseconds) / 1000.0);
			Layout.push_back(static_cast<double>(GuiProfile.MeasureNanoseconds + GuiProfile.ArrangeNanoseconds) / 1000.0);
			Text.push_back(static_cast<double>(GuiProfile.TextShapingNanoseconds + GuiProfile.GlyphLookupNanoseconds +
				GuiProfile.GlyphRasterizationNanoseconds + GuiProfile.AtlasUpdateNanoseconds) / 1000.0);
			Presentation.push_back(static_cast<double>(GuiProfile.PresentationResolutionNanoseconds) / 1000.0);
			Accessibility.push_back(static_cast<double>(GuiProfile.AccessibilityNanoseconds) / 1000.0);
			SnapshotCommit.push_back(static_cast<double>(GuiProfile.SnapshotCommitNanoseconds) / 1000.0);
			Display.push_back(static_cast<double>(GuiProfile.DisplayListNanoseconds + GuiProfile.BatchingNanoseconds) / 1000.0);
			FrameConstruction.push_back(static_cast<double>(GuiProfile.FrameConstructionNanoseconds) / 1000.0);
			FrameCopy.push_back(static_cast<double>(GuiProfile.FrameCopyNanoseconds) / 1000.0);
			Publication.push_back(static_cast<double>(GuiProfile.PublicationNanoseconds +
				PublisherProfile.PublicationConstructionNanoseconds) / 1000.0);
			Projection.push_back(std::chrono::duration<double, std::micro>(End - ProjectionStart).count());
		}
		const auto Profile = Value.Runtime.GetLastProfile();
		const auto PresentationValue = Value.Runtime.GetCommittedPresentation();
		std::size_t PrimitiveCount = 0;
		for (const auto &Batch : PresentationValue->Frame.Batches) PrimitiveCount += Batch.Indices.size() / 6;
		return {std::move(Name), Value.Nodes.size(), Summarize(std::move(Total)), Summarize(std::move(Semantic)),
			Summarize(std::move(Observation)), Summarize(std::move(Layout)), Summarize(std::move(Text)),
			Summarize(std::move(Presentation)), Summarize(std::move(Accessibility)), Summarize(std::move(SnapshotCommit)),
			Summarize(std::move(Display)), Summarize(std::move(FrameConstruction)), Summarize(std::move(FrameCopy)), Summarize(std::move(Publication)),
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

	ScenarioResult StaticButtonScenario(std::string Name, std::size_t Count, std::size_t Frames) {
		Scene Value;
		AddButtons(Value, Count);
		Value.CommitInitial();
		return Run(std::move(Name), Value, Frames, [](std::size_t) {});
	}

	ScenarioResult PressedButtonScenario(std::size_t Count, std::size_t Frames) {
		Scene Value;
		AddButtons(Value, Count);
		Value.CommitInitial();
		return Run("one_hovered_pressed_button", Value, Frames, [&](std::size_t Frame) {
			const auto Node = std::dynamic_pointer_cast<gargantuan::TextButton>(Value.Nodes[Frame % Count]);
			const auto Position = Node->GetAbsolutePosition() + Node->GetAbsoluteSize() / 2.0f;
			(void)Value.Runtime.ProcessPointer({6, gargantuan::Enums::GuiPointerType::Mouse,
				gargantuan::Enums::GuiPointerButton::Primary, gargantuan::GuiPointerAction::Down, Position});
			(void)Value.Runtime.ProcessPointer({6, gargantuan::Enums::GuiPointerType::Mouse,
				gargantuan::Enums::GuiPointerButton::Primary, gargantuan::GuiPointerAction::Up, Position});
		});
	}

	ScenarioResult ScrollingScenario(std::string Name, std::size_t Count, std::size_t Frames, bool Nested) {
		using namespace gargantuan;
		Scene Value;
		auto Scroll = std::make_shared<ScrollingFrame>();
		Scroll->SetSize(UDim2::fromOffset(1800, 900));
		Scroll->SetCanvasSize(UDim2::fromOffset(1800, static_cast<int>(Count / 20 * 18 + 900)));
		Scroll->SetAutomaticCanvasSize(Enums::AutomaticSize::None);
		Scroll->SetParent(Value.Root);
		Value.Nodes.push_back(Scroll);
		std::shared_ptr<ScrollingFrame> Inner;
		std::shared_ptr<Instance> Parent = Scroll;
		if (Nested) {
			Inner = std::make_shared<ScrollingFrame>();
			Inner->SetPosition(UDim2::fromOffset(40, 40));
			Inner->SetSize(UDim2::fromOffset(1600, 700));
			Inner->SetCanvasSize(UDim2::fromOffset(1600, static_cast<int>(Count / 20 * 18 + 700)));
			Inner->SetAutomaticCanvasSize(Enums::AutomaticSize::None);
			Inner->SetParent(Scroll);
			Parent = Inner;
			Value.Nodes.push_back(Inner);
		}
		for (std::size_t Index = 0; Index < Count; ++Index) {
			auto FrameValue = std::make_shared<Frame>();
			FrameValue->SetSize(UDim2::fromOffset(70, 14));
			FrameValue->SetPosition(UDim2::fromOffset(static_cast<int>(Index % 20) * 76,
				static_cast<int>(Index / 20) * 18));
			FrameValue->SetParent(Parent);
			Value.Nodes.push_back(std::move(FrameValue));
		}
		Value.CommitInitial();
		return Run(std::move(Name), Value, Frames, [&](std::size_t Frame) {
			auto Target = Inner ? Inner : Scroll;
			Target->SetCanvasPosition({0.0f, static_cast<float>((Frame * 37) % 500)});
		});
	}

	ScenarioResult TextEditScenario(std::size_t Frames) {
		using namespace gargantuan;
		Scene Value;
		auto Input = std::make_shared<TextBox>();
		Input->SetSize(UDim2::fromOffset(400, 36));
		Input->SetText("Editable UTF-8");
		Input->SetParent(Value.Root);
		Value.Nodes.push_back(Input);
		Value.CommitInitial();
		Input->CaptureFocus();
		(void)Value.Runtime.ProcessEvent(KeyEvent{{1}, PhysicalKey::Right, LogicalKey::End,
			KeyModifier::None, ButtonState::Pressed, false});
		return Run("text_edit_insert_delete", Value, Frames, [&](std::size_t Frame) {
			if (Frame & 1) {
				(void)Value.Runtime.ProcessEvent(KeyEvent{{1}, PhysicalKey::Backspace, LogicalKey::Backspace,
					KeyModifier::None, ButtonState::Pressed, false});
			} else if (auto Text = BoundedUtf8::From("\xCE\xA9")) {
				(void)Value.Runtime.ProcessEvent(TextInputEvent{{1}, *Text});
			}
		});
	}

	ScenarioResult SiblingScenario(std::size_t Count, std::size_t Frames) {
		using namespace gargantuan;
		Scene Value;
		Value.Root->SetZIndexBehavior(Enums::ZIndexBehavior::Sibling);
		const std::size_t Contexts = 100;
		for (std::size_t Context = 0; Context < Contexts; ++Context) {
			auto Parent = std::make_shared<Frame>();
			Parent->SetZIndex(static_cast<int>(Context % 8));
			Parent->SetParent(Value.Root);
			Value.Nodes.push_back(Parent);
			for (std::size_t Child = 0; Child < Count / Contexts; ++Child) {
				auto ValueNode = std::make_shared<Frame>();
				ValueNode->SetSize(UDim2::fromOffset(8, 8));
				ValueNode->SetPosition(UDim2::fromOffset(static_cast<int>(Child % 50) * 9,
					static_cast<int>(Child / 50) * 9));
				ValueNode->SetZIndex(static_cast<int>(Child % 32));
				ValueNode->SetParent(Parent);
				Value.Nodes.push_back(std::move(ValueNode));
			}
		}
		Value.CommitInitial();
		return Run("sibling_stacking_stress", Value, Frames, [](std::size_t) {});
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
		Part(Result.ObservationMicroseconds); std::cout << ',';
		Part(Result.LayoutMicroseconds); std::cout << ',';
		Part(Result.TextMicroseconds); std::cout << ',';
		Part(Result.PresentationMicroseconds); std::cout << ',';
		Part(Result.AccessibilityMicroseconds); std::cout << ',';
		Part(Result.SnapshotCommitMicroseconds); std::cout << ',';
		Part(Result.DisplayMicroseconds); std::cout << ',';
		Part(Result.FrameConstructionMicroseconds); std::cout << ',';
		Part(Result.FrameCopyMicroseconds); std::cout << ',';
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
		Results.push_back(FrameScenario("frames_10000_100_isolated_presentation_writes", Large, Frames, 1));
		Results.push_back(FrameScenario("frames_10000_1pct_layout", Large, Frames, 2));
		Results.push_back(ClipScenario(Large, Frames));
		Results.push_back(TextScenario("text_repeated", Quick ? 40 : 1'000, Frames, false));
		Results.push_back(TextScenario("text_unique", Quick ? 40 : 1'000, Frames, true));
		Results.push_back(StaticButtonScenario("buttons_5000_static", Quick ? 100 : 5'000, Frames));
		Results.push_back(StaticButtonScenario("buttons_10000_static", Quick ? 300 : 10'000, Frames));
		Results.push_back(ButtonScenario("buttons_10000_moving_hover", Quick ? 300 : 10'000, Frames));
		Results.push_back(PressedButtonScenario(Quick ? 300 : 10'000, Frames));
		Results.push_back(ScrollingScenario("nested_scrolling", Quick ? 100 : 1'000, Frames, true));
		Results.push_back(ScrollingScenario("scrolling_10000_children", Quick ? 300 : 10'000, Frames, false));
		Results.push_back(TextEditScenario(Frames));
		Results.push_back(SiblingScenario(Large, Frames));
		Results.push_back(HitScenario("hit_test_1000", Small, Quick ? 30 : 1'000));
		Results.push_back(HitScenario("hit_test_10000", Large, Quick ? 30 : 1'000));
		Results.push_back(AutomaticListScenario(Quick ? 100 : 5'000, Frames));
		Results.push_back(ViewportScenario(Small, Frames));
		Results.push_back(MixedScenario(Small, Frames));
		std::cout << "scenario,nodes,total_mean_us,total_p50_us,total_p95_us,total_p99_us,"
			"semantic_mean_us,semantic_p50_us,semantic_p95_us,semantic_p99_us,"
			"observation_mean_us,observation_p50_us,observation_p95_us,observation_p99_us,"
			"layout_mean_us,layout_p50_us,layout_p95_us,layout_p99_us,"
			"text_mean_us,text_p50_us,text_p95_us,text_p99_us,"
			"presentation_mean_us,presentation_p50_us,presentation_p95_us,presentation_p99_us,"
			"accessibility_mean_us,accessibility_p50_us,accessibility_p95_us,accessibility_p99_us,"
			"snapshot_commit_mean_us,snapshot_commit_p50_us,snapshot_commit_p95_us,snapshot_commit_p99_us,"
			"display_mean_us,display_p50_us,display_p95_us,display_p99_us,"
			"frame_construction_mean_us,frame_construction_p50_us,frame_construction_p95_us,frame_construction_p99_us,"
			"frame_copy_mean_us,frame_copy_p50_us,frame_copy_p95_us,frame_copy_p99_us,"
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
