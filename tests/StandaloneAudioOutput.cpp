// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "gargantuan/audio/AudioBackend.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
	struct Capture {
		std::atomic<unsigned> Both{}, MenuOnly{};
		static void SDLCALL Receive(void *Context, const SDL_AudioSpec *Spec, float *Samples, int Bytes) {
			if (Spec->channels < 2 || Bytes <= 0) return;
			auto &Read = *static_cast<Capture *>(Context);
			float Left = 0, Right = 0;
			for (int I = 0; I + 1 < Bytes / static_cast<int>(sizeof(float)); I += Spec->channels) {
				Left = std::max(Left, std::abs(Samples[I]));
				Right = std::max(Right, std::abs(Samples[I + 1]));
			}
			if (Left > 0.02f && Right > 0.02f) ++Read.Both;
			if (Left > 0.02f && Right < 0.0001f) ++Read.MenuOnly;
		}
	};
	void Require(bool Condition, const char *Message) {
		if (!Condition) throw std::runtime_error(Message);
	}
	bool Await(const std::atomic<unsigned> &Counter, unsigned Start) {
		const auto Deadline = SDL_GetTicks() + 1500;
		while (Counter.load() - Start < 3 && SDL_GetTicks() < Deadline) SDL_Delay(3);
		return Counter.load() - Start >= 3;
	}
}

int main(int Count, char **Args) {
	using namespace gargantuan;
	try {
		Require(SDL_InitSubSystem(SDL_INIT_AUDIO), SDL_GetError());
		struct AudioLifetime { ~AudioLifetime() { SDL_QuitSubSystem(SDL_INIT_AUDIO); } } Lifetime;
		const std::string_view Driver = SDL_GetCurrentAudioDriver();
		if (Count == 2 && std::string_view(Args[1]) == "--require-device")
			Require(Driver != "dummy" && Driver != "disk", "A physical or OS playback endpoint is required");
		SDL_AudioSpec Spec{SDL_AUDIO_F32, 2, 48000};
		Capture Read;
		const auto Device = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &Spec);
		Require(Device != 0, SDL_GetError());
		struct DeviceLifetime {
			SDL_AudioDeviceID Id;
			~DeviceLifetime() { SDL_CloseAudioDevice(Id); }
		} Open{Device};
		Require(SDL_SetAudioPostmixCallback(Device, Capture::Receive, &Read), SDL_GetError());
		auto Output = CreateSdlAudioOutput(Device);
		auto App = Output->OpenStream(), Game = Output->OpenStream();
		Require(App->IsAvailable() && Game->IsAvailable(), "Unable to bind both streams");
		std::vector<float> Menu(48000 * 4 * 2), World(Menu.size());
		for (std::size_t F = 0; F < Menu.size() / 2; ++F) {
			Menu[F * 2] = 0.08f * std::sin(static_cast<float>(F % 240) * 0.02618f);
			World[F * 2 + 1] = 0.08f * std::sin(static_cast<float>(F % 120) * 0.05236f);
		}
		Require(App->Submit(Menu) && Game->Submit(World), "Native streams rejected samples");
		Require(Await(Read.Both, 0), "Both authored channels must reach the output");
		Game->SetPlaybackHeld(true);
		const auto Held = Game->GetQueuedFrames();
		const auto AppStart = App->GetQueuedFrames();
		Require(Await(Read.MenuOnly, Read.MenuOnly.load()), "Menu-only waveform did not reach the held output");
		Require(Held > 0 && Held == Game->GetQueuedFrames() && AppStart > App->GetQueuedFrames(), "Incorrect held sample cursors");
		Game->SetPlaybackHeld(false);
		Require(Await(Read.Both, Read.Both.load()), "Gameplay waveform did not resume");
		Require(Game->GetQueuedFrames() < Held, "Gameplay cursor stayed parked");
		Game.reset();
		Require(Await(Read.MenuOnly, Read.MenuOnly.load()), "Retired gameplay remained in the output");
		App.reset();
		Output.reset();
		Require(SDL_SetAudioPostmixCallback(Device, nullptr, nullptr), SDL_GetError());
		std::cout << "[Lifecycle:Audio] Driver=" << Driver << " MixedBlocks=" << Read.Both
			<< " MenuOnlyBlocks=" << Read.MenuOnly << " HeldFrames=" << Held << '\n';
		return 0;
	} catch (const std::exception &Error) {
		std::cerr << "[Lifecycle:Audio] " << Error.what() << '\n';
		return 1;
	}
}

