// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "gargantuan/render/RenderPublication.hpp"
#include <limits>
#include <stdexcept>

namespace gargantuan {
	class ApplicationDisplay final {
	  public:
		RenderPublicationPtr Compose(const RenderPublicationPtr &Gameplay, const RenderPublication &App, bool HasWorld) {
			auto Frame = Gameplay ? std::make_shared<RenderPublication>(*Gameplay) : std::make_shared<RenderPublication>();
			Frame->FullResync = Gameplay ? Gameplay->FullResync : !HasWorld && App.FullResync;
			if (LastId == std::numeric_limits<RenderPublicationId>::max())
				throw std::overflow_error("[Lifecycle:Display] Publication identity exhausted");
			Frame->BaseId = Frame->FullResync ? InvalidRenderPublicationId : LastId;
			Frame->Id = ++LastId;
			if (Gameplay) LastWorldFrame = Gameplay->Frame;
			if (!Gameplay) {
				Frame->Frame = HasWorld ? LastWorldFrame : App.Frame;
				Frame->Frame.ViewportWidth = App.Frame.ViewportWidth;
				Frame->Frame.ViewportHeight = App.Frame.ViewportHeight;
				Frame->EnvironmentChanged = Frame->FullResync;
			}
			if (!HasWorld) { WorldUi.reset(); WorldViews.reset(); }
			if (Gameplay && (Gameplay->UiChanged || Gameplay->FullResync)) WorldUi = Gameplay->SharedUi;
			if (Gameplay && (Gameplay->Viewports || Gameplay->FullResync)) WorldViews = Gameplay->Viewports;
			if (App.UiChanged || App.FullResync) AppUi = App.SharedUi;
			if (App.Viewports || App.FullResync) AppViews = App.Viewports;
			bool AdmissionChanged = false;
			if (Frame->FullResync || Frame->Viewports || App.Viewports) {
				auto Views = std::make_shared<RenderViewportScenes>();
				if (AppViews) *Views = *AppViews;
				if (WorldViews) {
					const auto Available = MaximumRenderViewports - Views->size();
					Views->insert(Views->end(), WorldViews->begin(), WorldViews->begin() + std::min(Available, WorldViews->size()));
				}
				std::vector<ObjectId> Next;
				for (const auto &Scene : *Views) Next.push_back(Scene->Viewport);
				AdmissionChanged = Next != AdmittedViews;
				AdmittedViews = std::move(Next);
				Frame->Viewports = std::move(Views);
			}
			if (Frame->FullResync || Frame->UiChanged || App.UiChanged || AdmissionChanged) {
				auto Ui = std::make_shared<RenderUiFrame>();
				if (WorldUi) *Ui = *WorldUi;
				Ui->ViewportWidth = App.Frame.ViewportWidth;
				Ui->ViewportHeight = App.Frame.ViewportHeight;
				if (AppUi) {
					for (const auto &Batch : AppUi->Batches) {
						auto &Copy = Ui->Batches.emplace_back(Batch);
						if (Copy.Texture) Copy.Texture->Generation |= ApplicationTextureGeneration;
					}
				}
				std::erase_if(Ui->Batches, [&](const auto &Batch) {
					return Batch.Viewport.IsValid() && std::ranges::find(AdmittedViews, Batch.Viewport) == AdmittedViews.end();
				});
				Frame->SharedUi = std::move(Ui);
				Frame->UiChanged = true;
			}
			const auto AppendTextures = [](auto &World, const auto &Application) {
				for (const auto &Item : World)
					if (Item.Texture.Generation & ApplicationTextureGeneration)
						throw std::overflow_error("[Lifecycle:Display] Texture generation exhausted");
				for (const auto &Item : Application)
					World.emplace_back(Item).Texture.Generation |= ApplicationTextureGeneration;
			};
			AppendTextures(Frame->TextureCreates, App.TextureCreates);
			AppendTextures(Frame->TextureUpdates, App.TextureUpdates);
			AppendTextures(Frame->TextureRemoves, App.TextureRemoves);
			return Frame;
		}
	  private:
		static constexpr std::uint32_t ApplicationTextureGeneration = 1u << 31;
		RenderPublicationId LastId{};
		RenderFrameState LastWorldFrame;
		std::vector<ObjectId> AdmittedViews;
		std::shared_ptr<const RenderUiFrame> WorldUi, AppUi;
		std::shared_ptr<const RenderViewportScenes> WorldViews, AppViews;
	};
}
