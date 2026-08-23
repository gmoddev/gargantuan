// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.

#include "gargantuan/gui/GuiResources.hpp"

#include "gargantuan/gui/GuiLimits.hpp"
#include "gargantuan/services/AssetService.hpp"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_textengine.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <list>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace gargantuan {
	namespace {
		using Clock = std::chrono::steady_clock;

		std::mutex TtfLifetimeMutex;
		std::size_t TtfLifetimeUsers = 0;

		void RetainTtf() {
			std::scoped_lock Lock(TtfLifetimeMutex);
			if (TtfLifetimeUsers++ == 0 && !TTF_Init()) {
				TtfLifetimeUsers = 0;
				throw std::runtime_error(std::string("SDL_ttf initialization failed: ") + SDL_GetError());
			}
		}

		void ReleaseTtf() {
			std::scoped_lock Lock(TtfLifetimeMutex);
			if (TtfLifetimeUsers > 0 && --TtfLifetimeUsers == 0) TTF_Quit();
		}

		std::uint64_t Nanoseconds(Clock::duration Duration) {
			return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Duration).count());
		}

		struct SanitizedText {
			std::string Bytes;
			std::vector<std::size_t> ByteBoundaries{0};
			bool ReplacedInvalid = false;
			bool Truncated = false;
			std::size_t Codepoints = 0;
		};

		void AppendReplacement(SanitizedText &Result) {
			if (Result.Bytes.size() + 3 <= GuiLimits::MaximumTextBytesPerObject) Result.Bytes.append("\xEF\xBF\xBD", 3);
			Result.ReplacedInvalid = true;
			++Result.Codepoints;
			Result.ByteBoundaries.push_back(Result.Bytes.size());
		}

		SanitizedText SanitizeUtf8(std::string_view Input) {
			SanitizedText Result;
			Result.Bytes.reserve(std::min(Input.size(), GuiLimits::MaximumTextBytesPerObject));
			std::size_t Index = 0;
			for (; Index < Input.size() &&
				Result.Bytes.size() < GuiLimits::MaximumTextBytesPerObject &&
				Result.Codepoints < GuiLimits::MaximumGlyphsPerTextObject;) {
				const auto First = static_cast<unsigned char>(Input[Index]);
				std::size_t Length = 0;
				std::uint32_t Codepoint = 0;
				if (First < 0x80) {
					Length = 1;
					Codepoint = First;
				} else if ((First & 0xE0) == 0xC0) {
					Length = 2;
					Codepoint = First & 0x1F;
				} else if ((First & 0xF0) == 0xE0) {
					Length = 3;
					Codepoint = First & 0x0F;
				} else if ((First & 0xF8) == 0xF0) {
					Length = 4;
					Codepoint = First & 0x07;
				}
				bool Valid = Length != 0 && Index + Length <= Input.size();
				for (std::size_t Continuation = 1; Valid && Continuation < Length; ++Continuation) {
					const auto Byte = static_cast<unsigned char>(Input[Index + Continuation]);
					Valid = (Byte & 0xC0) == 0x80;
					Codepoint = (Codepoint << 6) | (Byte & 0x3F);
				}
				if (Valid) {
					const bool Overlong = (Length == 2 && Codepoint < 0x80) || (Length == 3 && Codepoint < 0x800) ||
						(Length == 4 && Codepoint < 0x10000);
					Valid = !Overlong && Codepoint <= 0x10FFFF && !(Codepoint >= 0xD800 && Codepoint <= 0xDFFF);
				}
				if (!Valid) {
					AppendReplacement(Result);
					++Index;
					continue;
				}
				if (Result.Bytes.size() + Length > GuiLimits::MaximumTextBytesPerObject) break;
				Result.Bytes.append(Input.substr(Index, Length));
				Index += Length;
				++Result.Codepoints;
				Result.ByteBoundaries.push_back(Result.Bytes.size());
			}
			Result.Truncated = Index < Input.size();
			return Result;
		}

		struct FontKey {
			std::string Reference;
			std::uint64_t ContentRevision = 0;
			int Size64 = 0;
			int Dpi64 = 0;
			int Alignment = 0;
			auto operator<=>(const FontKey &) const = default;
		};

		struct FontKeyHash {
			std::size_t operator()(const FontKey &Key) const noexcept {
				return std::hash<std::string>{}(Key.Reference) ^ (std::hash<std::uint64_t>{}(Key.ContentRevision) << 1) ^
					(std::hash<int>{}(Key.Size64) << 2) ^ (std::hash<int>{}(Key.Dpi64) << 3) ^
					(std::hash<int>{}(Key.Alignment) << 4);
			}
		};

		struct GlyphKey {
			TTF_Font *Font = nullptr;
			std::uint32_t Index = 0;
			auto operator<=>(const GlyphKey &) const = default;
		};

		struct GlyphKeyHash {
			std::size_t operator()(const GlyphKey &Key) const noexcept {
				return std::hash<void *>{}(Key.Font) ^ (std::hash<std::uint32_t>{}(Key.Index) << 1);
			}
		};

		struct ShapeKey {
			std::string Text;
			std::string FontFace;
			std::uint64_t FontRevision = 0;
			int Size64 = 0;
			int Dpi64 = 0;
			int WrapWidth = 0;
			int Alignment = 0;
			bool Wrapped = false;
			bool EditableMetrics = false;
			bool operator==(const ShapeKey &) const = default;
		};

		struct ShapeKeyHash {
			std::size_t operator()(const ShapeKey &Key) const noexcept {
				std::size_t Result = std::hash<std::string>{}(Key.Text);
				Result ^= std::hash<std::string>{}(Key.FontFace) << 1;
				Result ^= std::hash<std::uint64_t>{}(Key.FontRevision) << 2;
				Result ^= std::hash<int>{}(Key.Size64) << 3;
				Result ^= std::hash<int>{}(Key.Dpi64) << 4;
				Result ^= std::hash<int>{}(Key.WrapWidth) << 5;
				Result ^= std::hash<int>{}(Key.Alignment) << 6;
				Result ^= std::hash<bool>{}(Key.Wrapped) << 7;
				Result ^= std::hash<bool>{}(Key.EditableMetrics) << 8;
				return Result;
			}
		};

		struct AtlasEntry {
			std::size_t Page = 0;
			std::uint32_t X = 0;
			std::uint32_t Y = 0;
			std::uint32_t Width = 0;
			std::uint32_t Height = 0;
		};

		struct AtlasPage {
			RenderTextureIdentity Texture;
			std::uint64_t Revision = 1;
			std::uint32_t ShelfX = 1;
			std::uint32_t ShelfY = 1;
			std::uint32_t ShelfHeight = 0;
			std::vector<std::uint8_t> Pixels;
			std::vector<GuiRect> DirtyRects;
			bool Published = false;
		};

		struct EngineGlyphOperation {
			AtlasEntry Atlas;
			SDL_Rect Source{};
			SDL_Rect Destination{};
		};

		struct EngineTextData {
			std::vector<EngineGlyphOperation> Glyphs;
			bool MissingGlyph = false;
			bool Truncated = false;
			bool ResourceLimit = false;
		};
	}

	struct GuiTextSystem::Impl {
		struct OpenFont {
			TTF_Font *Font = nullptr;
			std::shared_ptr<const std::vector<std::uint8_t>> Bytes;
		};
		std::shared_ptr<AssetService> Assets;
		TTF_TextEngine Engine{};
		std::unordered_map<FontKey, OpenFont, FontKeyHash> Fonts;
		std::unordered_map<GlyphKey, AtlasEntry, GlyphKeyHash> Glyphs;
		std::vector<AtlasPage> Pages;
		std::unordered_map<ShapeKey, std::pair<std::shared_ptr<const GuiShapedText>, std::uint64_t>, ShapeKeyHash> ShapeCache;
		std::uint64_t CacheTick = 0;
		std::size_t FrameTextureBudget = 0;
		GuiRuntimeProfile Profile;

		explicit Impl(std::shared_ptr<AssetService> AssetsValue) : Assets(std::move(AssetsValue)) {
			if (!Assets) throw std::invalid_argument("GUI text requires AssetService");
			RetainTtf();
			SDL_INIT_INTERFACE(&Engine);
			Engine.userdata = this;
			Engine.CreateText = [](void *Userdata, TTF_Text *Text) -> bool {
				return static_cast<Impl *>(Userdata)->CreateText(Text);
			};
			Engine.DestroyText = [](void *, TTF_Text *Text) {
				delete static_cast<EngineTextData *>(Text->internal->engine_text);
				Text->internal->engine_text = nullptr;
			};
		}

		~Impl() {
			ShapeCache.clear();
			for (const auto &[Key, Opened] : Fonts) {
				(void)Key;
				if (Opened.Font) TTF_CloseFont(Opened.Font);
			}
			ReleaseTtf();
		}

		TTF_Font *GetFont(const GuiTextRequest &Request, const AssetFontResource &Source) {
			const FontKey Key{
				Request.FontFace.empty() ? "builtin://font/default" : Request.FontFace,
				Source.ContentRevision,
				static_cast<int>(std::lround(Request.LogicalSize * 64.0f)),
				static_cast<int>(std::lround(Request.DpiScale * 64.0f)),
				std::clamp(Request.HorizontalAlignment, 0, 2),
			};
			if (auto Existing = Fonts.find(Key); Existing != Fonts.end()) return Existing->second.Font;
			if (Fonts.size() >= GuiLimits::MaximumFontInstances || !Source.Bytes || Source.Bytes->empty()) return nullptr;
			auto *Stream = SDL_IOFromConstMem(Source.Bytes->data(), Source.Bytes->size());
			auto *Font = Stream ? TTF_OpenFontIO(Stream, true, Request.LogicalSize) : nullptr;
			if (!Font) return nullptr;
			const int Dpi = std::max(1, static_cast<int>(std::lround(72.0f * Request.DpiScale)));
			if (!TTF_SetFontSizeDPI(Font, Request.LogicalSize, Dpi, Dpi)) {
				TTF_CloseFont(Font);
				return nullptr;
			}
			const TTF_HorizontalAlignment Alignment = Key.Alignment == 0 ? TTF_HORIZONTAL_ALIGN_LEFT :
				Key.Alignment == 2 ? TTF_HORIZONTAL_ALIGN_RIGHT : TTF_HORIZONTAL_ALIGN_CENTER;
			TTF_SetFontWrapAlignment(Font, Alignment);
			Fonts.emplace(Key, OpenFont{Font, Source.Bytes});
			return Font;
		}

		std::optional<AtlasEntry> EnsureGlyph(TTF_Font *Font, std::uint32_t GlyphIndex) {
			const auto LookupStart = Clock::now();
			const GlyphKey Key{Font, GlyphIndex};
			if (auto Existing = Glyphs.find(Key); Existing != Glyphs.end()) {
				Profile.GlyphLookupNanoseconds += Nanoseconds(Clock::now() - LookupStart);
				return Existing->second;
			}
			Profile.GlyphLookupNanoseconds += Nanoseconds(Clock::now() - LookupStart);

			const auto RasterStart = Clock::now();
			TTF_ImageType ImageType = TTF_IMAGE_INVALID;
			auto *Surface = TTF_GetGlyphImageForIndex(Font, GlyphIndex, &ImageType);
			if (!Surface || Surface->w <= 0 || Surface->h <= 0) return std::nullopt;
			auto *Converted = SDL_ConvertSurface(Surface, SDL_PIXELFORMAT_RGBA32);
			if (!Converted) return std::nullopt;
			Profile.GlyphRasterizationNanoseconds += Nanoseconds(Clock::now() - RasterStart);

			const auto Width = static_cast<std::uint32_t>(Converted->w);
			const auto Height = static_cast<std::uint32_t>(Converted->h);
			if (Width + 2 > GuiLimits::GlyphAtlasDimension || Height + 2 > GuiLimits::GlyphAtlasDimension) {
				SDL_DestroySurface(Converted);
				return std::nullopt;
			}

			std::size_t PageIndex = 0;
			for (; PageIndex < Pages.size(); ++PageIndex) {
				auto &Page = Pages[PageIndex];
				if (Page.ShelfX + Width + 1 > GuiLimits::GlyphAtlasDimension) {
					if (Page.ShelfY + Page.ShelfHeight + Height + 1 > GuiLimits::GlyphAtlasDimension) continue;
					Page.ShelfY += Page.ShelfHeight + 1;
					Page.ShelfX = 1;
					Page.ShelfHeight = 0;
				}
				if (Page.ShelfY + Height + 1 <= GuiLimits::GlyphAtlasDimension) break;
			}
			if (PageIndex == Pages.size()) {
				const std::size_t PageBytes = GuiLimits::GlyphAtlasDimension * GuiLimits::GlyphAtlasDimension * 4;
				if (Pages.size() >= GuiLimits::MaximumGlyphAtlasPages ||
					FrameTextureBudget + PageBytes > GuiLimits::MaximumTextureUploadBytesPerFrame) {
					SDL_DestroySurface(Converted);
					return std::nullopt;
				}
				FrameTextureBudget += PageBytes;
				AtlasPage Page;
				Page.Texture = {0x4755491000000000ULL + Pages.size() + 1, 1};
				Page.Pixels.resize(PageBytes, 0);
				Pages.push_back(std::move(Page));
			}

			auto &Page = Pages[PageIndex];
			const AtlasEntry Entry{PageIndex, Page.ShelfX, Page.ShelfY, Width, Height};
			if (Page.Published) {
				GuiRect Dirty{static_cast<float>(Entry.X), static_cast<float>(Entry.Y),
					static_cast<float>(Width), static_cast<float>(Height)};
				std::size_t PreviousBytes = 0;
				if (!Page.DirtyRects.empty()) {
					const auto &Previous = Page.DirtyRects.front();
					PreviousBytes = static_cast<std::size_t>(Previous.Width) * static_cast<std::size_t>(Previous.Height) * 4;
					const float MinimumX = std::min(Previous.X, Dirty.X);
					const float MinimumY = std::min(Previous.Y, Dirty.Y);
					const float MaximumX = std::max(Previous.X + Previous.Width, Dirty.X + Dirty.Width);
					const float MaximumY = std::max(Previous.Y + Previous.Height, Dirty.Y + Dirty.Height);
					Dirty = {MinimumX, MinimumY, MaximumX - MinimumX, MaximumY - MinimumY};
				}
				const std::size_t PatchBytes = static_cast<std::size_t>(Dirty.Width) * static_cast<std::size_t>(Dirty.Height) * 4;
				if (FrameTextureBudget - PreviousBytes + PatchBytes > GuiLimits::MaximumTextureUploadBytesPerFrame) {
					SDL_DestroySurface(Converted);
					return std::nullopt;
				}
				FrameTextureBudget = FrameTextureBudget - PreviousBytes + PatchBytes;
				Page.DirtyRects.assign(1, Dirty);
			}
			const auto *SourcePixels = static_cast<const std::uint8_t *>(Converted->pixels);
			for (std::uint32_t Row = 0; Row < Height; ++Row) {
				const auto *Source = SourcePixels + static_cast<std::size_t>(Row) * Converted->pitch;
				auto *Destination = Page.Pixels.data() +
					(static_cast<std::size_t>(Entry.Y + Row) * GuiLimits::GlyphAtlasDimension + Entry.X) * 4;
				std::memcpy(Destination, Source, static_cast<std::size_t>(Width) * 4);
			}
			SDL_DestroySurface(Converted);
			Page.ShelfX += Width + 1;
			Page.ShelfHeight = std::max(Page.ShelfHeight, Height);
			Glyphs.emplace(Key, Entry);
			return Entry;
		}

		bool CreateText(TTF_Text *Text) {
			auto Data = std::make_unique<EngineTextData>();
			Data->Glyphs.reserve(static_cast<std::size_t>(std::max(Text->internal->num_ops, 0)));
			for (int Index = 0; Index < Text->internal->num_ops; ++Index) {
				const auto &Operation = Text->internal->ops[Index];
				if (Operation.cmd != TTF_DRAW_COMMAND_COPY) continue;
				const auto &Copy = Operation.copy;
				auto Entry = EnsureGlyph(Copy.glyph_font, Copy.glyph_index);
				if (!Entry) {
					Data->ResourceLimit = true;
					continue;
				}
				Data->MissingGlyph = Data->MissingGlyph || Copy.glyph_index == 0;
				Data->Glyphs.push_back({*Entry, Copy.src, Copy.dst});
				if (Data->Glyphs.size() >= GuiLimits::MaximumGlyphsPerTextObject) {
					Data->Truncated = Index + 1 < Text->internal->num_ops;
					break;
				}
			}
			Text->internal->engine_text = Data.release();
			return true;
		}

		void EvictShapeCacheIfNeeded() {
			if (ShapeCache.size() < GuiLimits::MaximumShapedTextCacheEntries) return;
			auto Oldest = ShapeCache.end();
			for (auto It = ShapeCache.begin(); It != ShapeCache.end(); ++It)
				if (Oldest == ShapeCache.end() || It->second.second < Oldest->second.second) Oldest = It;
			if (Oldest != ShapeCache.end()) ShapeCache.erase(Oldest);
		}
	};

	GuiTextSystem::GuiTextSystem(std::shared_ptr<AssetService> Assets)
		: State(std::make_unique<Impl>(std::move(Assets))) {}
	GuiTextSystem::~GuiTextSystem() = default;

	std::shared_ptr<const GuiShapedText> GuiTextSystem::Shape(const GuiTextRequest &Request) {
		const auto Sanitized = SanitizeUtf8(Request.Text);
		const auto FontSource = State->Assets->ResolveFont(Request.FontFace);
		const ShapeKey Key{
			Sanitized.Bytes,
			Request.FontFace,
			FontSource ? FontSource->ContentRevision : 0,
			static_cast<int>(std::lround(Request.LogicalSize * 64.0f)),
			static_cast<int>(std::lround(Request.DpiScale * 64.0f)),
			Request.Wrapped ? std::max(0, static_cast<int>(std::lround(Request.LogicalWrapWidth * Request.DpiScale))) : 0,
			std::clamp(Request.HorizontalAlignment, 0, 2),
			Request.Wrapped,
			Request.EditableMetrics,
		};
		if (auto Existing = State->ShapeCache.find(Key); Existing != State->ShapeCache.end()) {
			Existing->second.second = ++State->CacheTick;
			return Existing->second.first;
		}

		const auto ShapeStart = Clock::now();
		auto Result = std::make_shared<GuiShapedText>();
		Result->InputBytes = Request.Text.size();
		Result->ReplacedInvalidUtf8 = Sanitized.ReplacedInvalid;
		Result->TruncatedInput = Sanitized.Truncated;
		auto *Font = FontSource ? State->GetFont(Request, *FontSource) : nullptr;
		if (!Font || Sanitized.Bytes.empty()) {
			if (Request.EditableMetrics) Result->CaretOffsets.push_back(0.0f);
			State->Profile.TextShapingNanoseconds += Nanoseconds(Clock::now() - ShapeStart);
			State->EvictShapeCacheIfNeeded();
			State->ShapeCache.emplace(Key, std::pair(std::shared_ptr<const GuiShapedText>(Result), ++State->CacheTick));
			return Result;
		}

		auto *Text = TTF_CreateText(&State->Engine, Font, Sanitized.Bytes.data(), Sanitized.Bytes.size());
		if (!Text) return Result;
		if (Request.Wrapped) TTF_SetTextWrapWidth(Text, Key.WrapWidth);
		int Width = 0;
		int Height = 0;
		if (TTF_GetTextSize(Text, &Width, &Height)) {
			Result->Width = static_cast<float>(Width) / Request.DpiScale;
			Result->Height = static_cast<float>(Height) / Request.DpiScale;
			if (Request.EditableMetrics) {
				Result->CaretOffsets.reserve(Sanitized.ByteBoundaries.size());
				for (const auto Boundary : Sanitized.ByteBoundaries) {
					TTF_SubString SubString{};
					if (TTF_GetTextSubString(Text, static_cast<int>(Boundary), &SubString)) {
						const float X = (SubString.flags & TTF_SUBSTRING_TEXT_END) != 0 ?
							static_cast<float>(SubString.rect.x + SubString.rect.w) : static_cast<float>(SubString.rect.x);
						Result->CaretOffsets.push_back(X / Request.DpiScale);
					} else Result->CaretOffsets.push_back(Result->CaretOffsets.empty() ? 0.0f : Result->CaretOffsets.back());
				}
			}
			if (const auto *Data = static_cast<const EngineTextData *>(Text->internal->engine_text)) {
				Result->UsedMissingGlyph = Data->MissingGlyph;
				Result->TruncatedInput = Result->TruncatedInput || Data->Truncated;
				Result->ResourceLimit = Data->ResourceLimit;
				Result->Glyphs.reserve(Data->Glyphs.size());
				for (const auto &Operation : Data->Glyphs) {
					const auto &Page = State->Pages[Operation.Atlas.Page];
					Result->Glyphs.push_back({
						Page.Texture,
						{
							static_cast<float>(Operation.Destination.x) / Request.DpiScale,
							static_cast<float>(Operation.Destination.y) / Request.DpiScale,
							static_cast<float>(Operation.Destination.w) / Request.DpiScale,
							static_cast<float>(Operation.Destination.h) / Request.DpiScale,
						},
						{
							static_cast<float>(Operation.Atlas.X + Operation.Source.x) / GuiLimits::GlyphAtlasDimension,
							static_cast<float>(Operation.Atlas.Y + Operation.Source.y) / GuiLimits::GlyphAtlasDimension,
							static_cast<float>(Operation.Source.w) / GuiLimits::GlyphAtlasDimension,
							static_cast<float>(Operation.Source.h) / GuiLimits::GlyphAtlasDimension,
						},
					});
				}
			}
		}
		TTF_DestroyText(Text);
		State->Profile.TextShapingNanoseconds += Nanoseconds(Clock::now() - ShapeStart);
		State->Profile.ShapedGlyphs += Result->Glyphs.size();
		State->EvictShapeCacheIfNeeded();
		State->ShapeCache.emplace(Key, std::pair(std::shared_ptr<const GuiShapedText>(Result), ++State->CacheTick));
		return Result;
	}

	GuiTextureChanges GuiTextSystem::DrainTextureChanges() {
		const auto Start = Clock::now();
		GuiTextureChanges Result;
		for (auto &Page : State->Pages) {
			if (!Page.Published) {
				auto Pixels = std::make_shared<const std::vector<std::uint8_t>>(Page.Pixels);
				Result.UploadBytes += Pixels->size();
				Result.Creates.push_back({Page.Texture, Page.Revision,
					static_cast<std::uint32_t>(GuiLimits::GlyphAtlasDimension),
					static_cast<std::uint32_t>(GuiLimits::GlyphAtlasDimension), RenderTextureFormat::Rgba8Unorm, Pixels});
				Page.Published = true;
				Page.DirtyRects.clear();
				continue;
			}
			for (const auto &Dirty : Page.DirtyRects) {
				const auto X = static_cast<std::uint32_t>(Dirty.X);
				const auto Y = static_cast<std::uint32_t>(Dirty.Y);
				const auto Width = static_cast<std::uint32_t>(Dirty.Width);
				const auto Height = static_cast<std::uint32_t>(Dirty.Height);
				auto Pixels = std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(Width) * Height * 4);
				for (std::uint32_t Row = 0; Row < Height; ++Row) {
					const auto *Source = Page.Pixels.data() +
						(static_cast<std::size_t>(Y + Row) * GuiLimits::GlyphAtlasDimension + X) * 4;
					std::memcpy(Pixels->data() + static_cast<std::size_t>(Row) * Width * 4, Source,
						static_cast<std::size_t>(Width) * 4);
				}
				Result.UploadBytes += Pixels->size();
				Result.Updates.push_back({Page.Texture, ++Page.Revision, X, Y, Width, Height, Pixels});
			}
			Page.DirtyRects.clear();
		}
		State->Profile.AtlasUpdateNanoseconds += Nanoseconds(Clock::now() - Start);
		State->Profile.TextureUpdates += Result.Creates.size() + Result.Updates.size();
		State->Profile.TextureUploadBytes += Result.UploadBytes;
		return Result;
	}

	GuiRuntimeProfile GuiTextSystem::ConsumeProfile() {
		auto Result = State->Profile;
		State->Profile = {};
		return Result;
	}

	void GuiTextSystem::ResetFrameBudget(std::size_t ReservedUploadBytes) {
		State->FrameTextureBudget = std::min(ReservedUploadBytes, GuiLimits::MaximumTextureUploadBytesPerFrame);
	}
	bool GuiTextSystem::HasFont() const { return State->Assets->ResolveFont("builtin://font/default").has_value(); }
}
