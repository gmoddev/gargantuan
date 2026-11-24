#include "gargantuan/GuiRuntimeConfig.hpp"
#include "gargantuan/assets/GasInstanceSerialization.hpp"
#include "gargantuan/classes/Camera.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/Frame.hpp"
#include "gargantuan/classes/Part.hpp"
#include "gargantuan/classes/ScreenGui.hpp"
#include "gargantuan/classes/ViewportFrame.hpp"
#include "gargantuan/gui/GuiRuntime.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/RenderExtractor.hpp"
#include "gargantuan/render/RenderProjection.hpp"
#include "gargantuan/runtime/ChangeJournal.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"
#include "gargantuan/services/Workspace.hpp"
#include "../src/runtime/RuntimeWorkDiagnostics.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace gargantuan;

namespace {
	void Check(bool Passed, const char *Message) {
		if (!Passed) throw std::runtime_error(Message);
	}

	void TestViewportFrame() {
		auto World = std::make_shared<DataModel>();
		const auto WorkspaceValue = std::dynamic_pointer_cast<Workspace>(World->GetService("Workspace"));
		auto Screen = std::make_shared<ScreenGui>();
		// Exercise the legacy root placement where the scene still must not join the world.
		Screen->SetParent(WorkspaceValue);
		auto Viewport = std::make_shared<ViewportFrame>();
		Viewport->SetName("Preview");
		Viewport->SetSize(UDim2::fromOffset(220, 110));
		Viewport->SetParent(Screen);
		auto Group = std::make_shared<Folder>();
		Group->SetParent(Viewport);
		auto CameraValue = std::make_shared<Camera>();
		CameraValue->SetName("InspectionCamera");
		CameraValue->SetCFrame(CFrame(0.0f, 0.0f, 5.0f));
		CameraValue->SetFieldOfView(50.0f);
		CameraValue->SetParent(Group);
		Viewport->SetCurrentCamera(CameraValue);
		auto PartValue = std::make_shared<Part>();
		PartValue->SetParent(Group);

		bool Rejected = false;
		try { Viewport->SetCurrentCamera(WorkspaceValue->GetCurrentCamera()); }
		catch (const std::invalid_argument &) { Rejected = true; }
		Check(Rejected && Viewport->GetCurrentCamera() == CameraValue, "The viewport accepted a world camera");
		Check(std::ranges::find(WorkspaceValue->Parts, PartValue) == WorkspaceValue->Parts.end(),
			"The viewport part joined gameplay physics");

		GuiRuntime Gui(World, DefaultGuiFontPath);
		Gui.SetViewport({320, 240, 1.0f});
		RenderPublisher Publisher;
		RenderProjection Projection;
		const auto Publish = [&] {
			Gui.Publish(Publisher);
			return Publisher.Publish(*WorkspaceValue, RenderCameraInput{}, 320, 240);
		};
		(void)Gui.Reconcile();
		const auto First = Publish();
		(void)Projection.Apply(*First);
		Check(Projection.GetSize() == 0 && Projection.GetViewports().size() == 1,
			"ViewportFrame geometry leaked into the ordinary projection");
		const auto Scene = Projection.GetViewports().front();
		Check(Scene->Objects.size() == 1 && Scene->Objects.front().Visual.Item.Object == PartValue->GetObjectId(),
			"ViewportFrame did not extract its bounded scene");
		const float Aspect = Scene->Camera.ProjectionMatrix[1][1] / Scene->Camera.ProjectionMatrix[0][0];
		Check(Scene->Width == 220 && Scene->Height == 110 && std::abs(Aspect - 2.0f) < 0.01f,
			"Viewport camera used the surface aspect instead of the GUI bounds");
		Check(std::ranges::count(Projection.GetUi().Batches, Viewport->GetObjectId(), &RenderUiBatch::Viewport) == 1,
			"ViewportFrame has no normal GUI composition batch");
		(void)Gui.Reconcile();
		(void)Projection.Apply(*Publish());
		Check(Projection.GetViewports().front() == Scene, "An unchanged viewport replaced its committed scene");

		Viewport->SetVisible(false);
		(void)Gui.Reconcile();
		(void)Projection.Apply(*Publish());
		Check(Projection.GetViewports().empty() &&
			std::ranges::none_of(Projection.GetUi().Batches, [](const auto &Batch) { return Batch.Viewport.IsValid(); }),
			"Hidden viewport retained requested target or draw batch");
		Viewport->SetVisible(true);
		Viewport->SetRenderScale(0.5f);
		(void)Gui.Reconcile();
		(void)Projection.Apply(*Publish());
		Check(Projection.GetViewports().size() == 1 && Projection.GetViewports().front()->Width == 110 &&
			Projection.GetViewports().front()->Height == 55, "A revealed viewport did not apply RenderScale");
		Viewport->SetPosition(UDim2::fromOffset(640, 0));
		(void)Gui.Reconcile();
		(void)Projection.Apply(*Publish());
		Check(Projection.GetViewports().empty(), "A fully clipped viewport requested an offscreen target");
		const auto OuterClip = std::make_shared<Frame>();
		OuterClip->SetSize(UDim2::fromOffset(96, 96));
		OuterClip->SetClipsDescendants(true);
		OuterClip->SetParent(Screen);
		const auto InnerClip = std::make_shared<Frame>();
		InnerClip->SetSize(UDim2::fromOffset(96, 96));
		InnerClip->SetPosition(UDim2::fromOffset(128, 0));
		InnerClip->SetClipsDescendants(true);
		InnerClip->SetParent(OuterClip);
		Viewport->SetPosition(UDim2::fromOffset(-128, 0));
		Viewport->SetParent(InnerClip);
		(void)Gui.Reconcile();
		(void)Projection.Apply(*Publish());
		Check(Projection.GetViewports().empty(), "Disjoint ancestor clips must not expose an in-frame viewport");
		InnerClip->SetPosition(UDim2::fromOffset(0, 0));
		Viewport->SetPosition(UDim2::fromOffset(0, 0));
		(void)Gui.Reconcile();
		(void)Projection.Apply(*Publish());
		Check(Projection.GetViewports().size() == 1, "A nested viewport returns when its ancestor clips overlap");
		Viewport->SetParent(Screen);
		OuterClip->Destroy();

		Screen->MarkPersistenceSubtreeArchivable();
		const auto Cloned = Screen->Clone();
		const auto CloneViewport = std::dynamic_pointer_cast<ViewportFrame>(Cloned->FindFirstChild("Preview", true));
		const auto CloneCamera = Cloned->FindFirstChild("InspectionCamera", true);
		Check(CloneViewport && CloneCamera != CameraValue && CloneViewport->GetCurrentCamera().value_or(nullptr) == CloneCamera,
			"GUI template cloning did not remap its local Camera reference");
		Cloned->Destroy();
		World->MarkPersistenceSubtreeArchivable();
		for (const auto Kind : {Gas::DocumentKind::Model, Gas::DocumentKind::Place}) {
			const std::array Roots{Kind == Gas::DocumentKind::Place
				? std::static_pointer_cast<Instance>(World) : std::static_pointer_cast<Instance>(Screen)};
			const auto Place = Kind == Gas::DocumentKind::Place
				? std::optional(Gas::PlaceMetadata{.Identity = {{1}}}) : std::nullopt;
			const auto Doc = InstanceSerialization::CaptureGasDocument(Roots, Kind, Place);
			Check(Doc.has_value(), "GAS capture lost the viewport camera reference");
			const auto Bytes = Gas::Encode(*Doc);
			Check(Bytes.has_value(), "GAS could not encode a ViewportFrame");
			const auto Decoded = Gas::Decode(*Bytes);
			Check(Decoded.has_value(), "GAS could not decode its ViewportFrame");
			const auto Restored = InstanceSerialization::MaterializeGasDocument(*Decoded, false);
			Check(Restored.Succeeded(), "GAS failed to materialize a ViewportFrame");
			const auto RestoredRoot = Restored.Roots.front();
			const auto Frame = std::dynamic_pointer_cast<ViewportFrame>(RestoredRoot->FindFirstChild("Preview", true));
			const auto RestoredCamera = RestoredRoot->FindFirstChild("InspectionCamera", true);
			Check(Frame && RestoredCamera != CameraValue && Frame->GetCurrentCamera().value_or(nullptr) == RestoredCamera &&
				Frame->GetRenderScale() == 0.5f, "GAS did not preserve camera scope and authored quality");
			RestoredRoot->Destroy();
		}

		CameraValue->SetParent(Screen);
		Check(!Viewport->GetCurrentCamera(), "A Camera outside the viewport remained readable");
		CameraValue->SetParent(Group);
		Check(!Viewport->GetCurrentCamera(), "A detached Camera reference resurrected after reparent");
		Viewport->SetCurrentCamera(CameraValue);
		const auto LaterSibling = std::make_shared<Folder>();
		LaterSibling->SetParent(Group);
		std::size_t StaleNotifications = 0;
		const auto SiblingObserver = LaterSibling->AncestryChanged->Connect([&](const auto &Event) {
			if (std::get<0>(Event) == Group) ++StaleNotifications;
		});
		const auto CameraObserver = Viewport->GetPropertyChangedSignal("CurrentCamera")->Connect([&](auto) {
			LaterSibling->SetParent(Viewport);
		});
		Group->SetParent(Screen);
		Check(!Viewport->GetCurrentCamera() && LaterSibling->GetParent() == Viewport && StaleNotifications == 0,
			"Camera reference observers can reparent a later sibling without receiving stale ancestry notifications");
		CameraObserver->Disconnect();
		SiblingObserver->Disconnect();
		PartValue->SetParent(WorkspaceValue);
		Check(std::ranges::find(WorkspaceValue->Parts, PartValue) != WorkspaceValue->Parts.end(),
			"A part reparented out of the viewport did not join gameplay physics");
		World->Destroy();
	}

	void TestOwnedHierarchyDepth() {
		const auto World = std::make_shared<DataModel>();
		const auto WorkspaceValue = World->GetService("Workspace");
		std::shared_ptr<Instance> Target = WorkspaceValue;
		for (std::size_t Depth = 2; Depth < MaximumProtocolJsonDepth - 1; ++Depth) {
			const auto FolderValue = std::make_shared<Folder>();
			FolderValue->SetParent(Target);
			Target = FolderValue;
		}
		const auto Root = std::make_shared<Folder>();
		Root->SetParent(WorkspaceValue);
		std::make_shared<Folder>()->SetParent(Root);
		const auto BeforeRevision = World->GetAuthoritativeRevision();
		const auto Scope = World->GetObjectId();
		const auto BeforeSequence = ChangeJournal::Get().CreateCursor(Scope).NextSequence;
		std::size_t Notifications = 0;
		const auto Observer = Root->AncestryChanged->Connect([&](const auto &) { ++Notifications; });
		bool Rejected = false;
		try { Root->SetParent(Target); }
		catch (const std::length_error &) { Rejected = true; }
		Check(Rejected && Root->GetParent() == WorkspaceValue && Notifications == 0 &&
			World->GetAuthoritativeRevision() == BeforeRevision &&
			ChangeJournal::Get().CreateCursor(Scope).NextSequence == BeforeSequence,
			"A same-world move beyond the hierarchy depth cap must fail before publication");
		Observer->Disconnect();
		World->Destroy();
	}

	void RunHiddenViewportWorkload(std::size_t HiddenCount) {
		using namespace runtime_detail;
		constexpr std::size_t SteadyFrames = 240;
		auto World = std::make_shared<DataModel>();
		const auto WorkspaceValue = std::dynamic_pointer_cast<Workspace>(World->GetService("Workspace"));
		auto Screen = std::make_shared<ScreenGui>();
		Screen->SetParent(World);
		const auto AddView = [&](bool Visible) {
			auto View = std::make_shared<ViewportFrame>();
			View->SetSize(UDim2::fromOffset(128, 96));
			View->SetVisible(Visible);
			View->SetParent(Screen);
			auto CameraValue = std::make_shared<Camera>();
			CameraValue->SetCFrame(CFrame(0.0f, 0.0f, 6.0f));
			CameraValue->SetParent(View);
			View->SetCurrentCamera(CameraValue);
			std::make_shared<Part>()->SetParent(View);
			return View;
		};
		const auto Visible = AddView(true);
		for (std::size_t Index = 0; Index < HiddenCount; ++Index) (void)AddView(false);
		GuiRuntime Gui(World, DefaultGuiFontPath);
		Gui.SetViewport({320, 240, 1.0f});
		RenderPublisher Publisher;
		RenderProjection Projection;
		const auto Step = [&] {
			(void)Gui.Reconcile();
			Gui.Publish(Publisher);
			(void)Projection.Apply(*Publisher.Publish(*WorkspaceValue, RenderCameraInput{}, 320, 240));
		};
		const auto Extract = static_cast<std::size_t>(WorkPhase::ViewportExtract);
		WorkSample Cold{};
		{
			WorkCapture Capture(&Cold);
			Step();
		}
		Check(Cold[Extract].Units == 1 && Projection.GetViewports().size() == 1 &&
			Projection.GetViewports().front()->Viewport == Visible->GetObjectId(),
			"Hidden inventory previews performed first-frame scene extraction");
		WorkSample Steady{};
		const auto Started = std::chrono::steady_clock::now();
		{
			WorkCapture Capture(&Steady);
			for (std::size_t Frame = 0; Frame < SteadyFrames; ++Frame) Step();
		}
		const auto Elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now() - Started).count();
		Check(Projection.GetViewports().size() == 1 && Steady[Extract].Units <= SteadyFrames,
			"Hidden inventory previews emitted work beyond the visible view");
		Visible->SetVisible(false);
		WorkSample Hidden{};
		{
			WorkCapture Capture(&Hidden);
			Step();
			Step();
		}
		Check(Hidden[Extract].Units == 0 && Projection.GetViewports().empty(),
			"A fully hidden inventory retained or extracted presentation scenes");
		std::cout << "[Gui:ViewportWorkload] hidden_authored=" << HiddenCount
			<< " frames=" << SteadyFrames << " steady_total_ns=" << Elapsed
			<< " cold_extract_ns=" << Cold[Extract].Nanoseconds << " cold_extract_units=" << Cold[Extract].Units
			<< " steady_extract_ns=" << Steady[Extract].Nanoseconds << " steady_extract_units=" << Steady[Extract].Units
			<< " all_hidden_extract_units=" << Hidden[Extract].Units << '\n';
		World->Destroy();
	}
}

int main() {
	try {
		BootstrapNativeRuntimeSchema();
		TestViewportFrame();
		TestOwnedHierarchyDepth();
		RunHiddenViewportWorkload(0);
		RunHiddenViewportWorkload(128);
		SDL_Quit();
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Gui:ViewportTest] " << Error.what() << '\n';
		SDL_Quit();
		return 1;
	}
}
