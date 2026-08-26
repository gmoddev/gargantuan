#include "gargantuan/Engine.hpp"
#include "gargantuan/Log.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/filesystem/Paths.hpp"
#include "gargantuan/packaging/PackageBuilder.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/render/Renderer.hpp"
#include "gargantuan/render/SDLRenderer.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "platform/sdl/SDLHost.hpp"

#include <SDL3/SDL.h>
#include <argparse/argparse.hpp>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>

#if defined(_WIN32)
#include <Windows.h>
#endif

using namespace gargantuan;

namespace {
#if defined(_WIN32)
	void ValidateNativeRuntimeClosure(const std::filesystem::path &PackageRoot) {
		const auto CanonicalRoot = std::filesystem::weakly_canonical(PackageRoot);
		for (const auto *ModuleName : {
				 L"SDL3.dll",
				 L"MSVCP140.dll",
				 L"VCRUNTIME140.dll",
				 L"VCRUNTIME140_1.dll",
			 }) {
			const auto Module = GetModuleHandleW(ModuleName);
			std::wstring Buffer(32'768, L'\0');
			const auto Length = Module ? GetModuleFileNameW(Module, Buffer.data(), static_cast<DWORD>(Buffer.size()))
									   : 0;
			if (Length == 0 || Length == Buffer.size())
				throw std::runtime_error("A required native runtime module is unavailable");
			Buffer.resize(Length);
			if (std::filesystem::weakly_canonical(Buffer).parent_path() != CanonicalRoot)
				throw std::runtime_error("A required native runtime module was not loaded from the package root");
		}
	}
#else
	void ValidateNativeRuntimeClosure(const std::filesystem::path &) {}
#endif
}

int main(int argc, char *argv[]) {
	argparse::ArgumentParser Program("GargantuanPlayer");
	Program.add_description("Gargantuan standalone game runtime");
	Program.add_argument("--headless").flag().help("disable the graphical renderer");
	Program.add_argument("--startup-smoke").flag().help("exit after a bounded runtime startup smoke");
	Program.add_argument("--max-frames").scan<'i', int>().default_value(0).help("bounded test-only frame count");
	try {
		Program.parse_args(argc, argv);
	} catch (const std::exception &) {
		std::cerr << "GargantuanPlayer arguments are invalid.\n";
		return 2;
	}

	const auto PackageRoot = Paths::GetExecutableDirectory().lexically_normal();
	std::vector<PackageDiagnostic> Diagnostics;
	auto Payload = PackageBuilder::Load(PackageRoot, Diagnostics);
	if (!Payload) {
		std::cerr << "GargantuanPlayer could not validate this game package.\n";
		for (const auto &Diagnostic : Diagnostics)
			if (Diagnostic.Severity == PackageDiagnosticSeverity::Error)
				std::cerr << "[Package:" << Diagnostic.Category << "] " << Diagnostic.Message << '\n';
		return 3;
	}
	try {
		ValidateNativeRuntimeClosure(PackageRoot);
	} catch (const std::exception &) {
		std::cerr << "GargantuanPlayer could not establish its packaged native runtime closure.\n";
		return 4;
	}

	try {
		BootstrapPackagedRuntimeSchema(Payload->PreRunSource);
	} catch (const std::exception &) {
		std::cerr << "GargantuanPlayer could not initialize the packaged runtime schema.\n";
		return 5;
	}

	const bool Headless = Program.is_used("--headless");
	if (!SDL_Init(Headless ? SDL_INIT_EVENTS : SDL_INIT_VIDEO)) {
		std::cerr << "GargantuanPlayer could not initialize the platform runtime.\n";
		return 6;
	}
	struct SdlLifetime final {
		~SdlLifetime() {
			SDL_Quit();
		}
	} Sdl;

	std::unique_ptr<BaseRenderer> Renderer;
	std::unique_ptr<Engine> Runtime;
	try {
		const Vector2 ViewportSize(720, 540);
		if (Headless)
			Renderer = std::make_unique<HeadlessRenderer>(ViewportSize);
		else
			Renderer = std::make_unique<SDLRenderer>(ViewportSize);
		auto World = PackageBuilder::LoadWorld(*Payload, PackageRoot);
		Runtime = std::make_unique<Engine>(
			World, Renderer.get(), nullptr, EngineProviderConfiguration{.AudioEnabled = !Headless}
		);
		Runtime->ProcessService->Alive = true;
		const auto UserDataRoot = GetPackageUserDataRoot(Payload->Inspection.Identity);
		LOG_INFO(
			App,
			"[Package:Runtime] Started %s (%s); user data is isolated by ProjectId",
			Payload->Inspection.DisplayName.c_str(),
			Payload->Inspection.Identity.ToString().c_str()
		);
		(void)UserDataRoot;

		SDLHost Host;
		const auto RequestedFrames = Program.get<int>("--max-frames");
		const auto MaximumFrames = RequestedFrames > 0 ? RequestedFrames
													   : (Program.is_used("--startup-smoke") ? 12 : 0);
		int Frames = 0;
		while (Runtime->ProcessService->Alive) {
			HostEvent Event;
			while (Host.PollEvent(Event)) {
				auto Result = Runtime->ProcessEvent(Event);
				if (Result.Command) Host.Apply(*Result.Command);
				if (!Runtime->ProcessService->Alive) break;
			}
			if (!Runtime->ProcessService->Alive) break;
			Runtime->Step();
			if (MaximumFrames > 0 && ++Frames >= MaximumFrames) Runtime->ProcessService->MarkExit(0);
		}
		const auto ExitCode = Runtime->ProcessService->ExitCode;
		Runtime->Destroy();
		Runtime.reset();
		Renderer.reset();
		return ExitCode;
	} catch (const std::exception &Error) {
		std::cerr << "GargantuanPlayer stopped because packaged runtime startup failed: " << Error.what() << "\n";
		if (Runtime) Runtime->Destroy();
		Runtime.reset();
		Renderer.reset();
		return 7;
	}
}
