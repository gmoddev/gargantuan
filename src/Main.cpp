#include "gargantuan/Engine.hpp"
#include "gargantuan/Log.hpp"
#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Script.hpp"
#include "gargantuan/datatypes/Vector2.hpp"
#include "gargantuan/editor/EditorHost.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/Project.hpp"
#include "gargantuan/render/Renderer.hpp"
#include "gargantuan/render/SDLRenderer.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/runtime/DataModelRoot.hpp"
#include "platform/sdl/SDLHost.hpp"
#include "telemetry/OptionalTelemetry.hpp"

#include <SDL3/SDL.h>
#include <argparse/argparse.hpp>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

using namespace gargantuan;

namespace {
	constexpr std::uint64_t RuntimeSchemaFatalCode = 0x1001;
	constexpr std::uint64_t EditorHostFatalCode = 0x1002;
	constexpr std::uint64_t RuntimeConstructionFatalCode = 0x1003;
	constexpr std::uint64_t EngineLoopFatalCode = 0x1004;

	[[nodiscard]] bool IsStudioEditorToken(std::string_view Token) {
		return Token.size() == 64 && std::ranges::all_of(Token, [](unsigned char Character) {
			return (Character >= '0' && Character <= '9') || (Character >= 'a' && Character <= 'f');
		});
	}

	[[nodiscard]] bool EnabledEnvironmentPolicy(const char *Name) {
		const auto *Value = std::getenv(Name);
		return Value && std::string_view(Value) == "1";
	}
}

Engine *ConstructProject(std::string path, BaseRenderer *renderer, bool AudioEnabled) {
	auto root = std::filesystem::path(path);
	try {
		auto fs = new DiskFilesystem(root);
		auto project = Project::fromExisting(fs);
		auto game = project.DeserializeGame();
		return new Engine(game, renderer, {}, {.AudioEnabled = AudioEnabled});
	} catch (std::exception &e) {
		LOG_CRITICAL(App, "%s", e.what());
		throw;
	}
}

Engine *ConstructScript(std::string path, BaseRenderer *renderer, bool AudioEnabled) {
	try {
		auto game = std::make_shared<DataModel>();
		auto engine = new Engine(game, renderer, {}, {.AudioEnabled = AudioEnabled});

		auto script = ScriptFromFile<Script>(path.c_str());
		script->SetParent(engine->Workspace);

		return engine;
	} catch (std::exception &e) {
		LOG_CRITICAL(App, "%s", e.what());
		throw;
	}
}

Engine *ConstructInstance(std::string path, BaseRenderer *renderer, bool AudioEnabled) {
	SDL_PathInfo pathInfo;
	if (!SDL_GetPathInfo(path.c_str(), &pathInfo)) {
		throw std::runtime_error("Failed to inspect the requested Instance file");
	} else if (pathInfo.type != SDL_PATHTYPE_FILE) {
		throw std::runtime_error("The requested Instance target is not a file");
	}

	InstanceSerialization::InstanceFormat format;
	if (path.ends_with(".instance.json")) {
		format = InstanceSerialization::InstanceFormat::Json;
	} else if (path.ends_with(".instance.bin")) {
		format = InstanceSerialization::InstanceFormat::Binary;
	} else {
		throw std::runtime_error("Unable to infer the requested Instance file format");
	}

	std::ifstream fileStream(path);
	if (!fileStream.is_open()) {
		throw std::runtime_error("Failed to open the requested Instance file");
	}

	auto deserialized = InstanceSerialization::Deserialize(format, fileStream);
	if (!deserialized.Ok) {
		LOG_CRITICAL(App, "Failed to deserialize instance file %s:", path.c_str());
		for (auto &reason : deserialized.Errors) {
			LOG_CRITICAL(App, "* %s", reason.c_str());
		}
		throw std::runtime_error("Failed to deserialize the requested Instance file");
	}

	auto instance = deserialized.Instance;
	auto game = PrepareDataModelRoot(instance);

	return new Engine(game, renderer, {}, {.AudioEnabled = AudioEnabled});
}

int main(int argc, char *argv[]) {
	argparse::ArgumentParser program("gargantuan");
	program.add_description("An independent game engine for Roblox developers");
	program.add_group("Targets");
	program.add_argument("--project").help("path of a project directory to be loaded").default_value("-");
	program.add_argument("--script").help("path of a Luau script to be loaded").default_value("-");
	program.add_argument("--instance").help("path ofg an instance file to be loaded").default_value("-");
	program.add_group("Engine");
	program.add_argument("--headless").flag().help("whether to disable the renderer");
	program.add_argument("--enable_roblox_compat").flag().help("use roblox api compatibility (overrides projects)");
	program.add_argument("--editor-host").flag().help("run the versioned local EditorHost protocol over standard I/O");
	program.add_argument("--editor-token").help("per-launch EditorHost session token").default_value("");
	program.add_argument("--telemetry-crashes").flag().help("enable optional sanitized crash reporting for this process");
	program.add_argument("--telemetry-performance").flag().help("enable optional coarse performance snapshots for this process");
	program.add_group("Logging");
	program.add_argument("--no_ansi").flag().help("disable ansi logs");
	program.add_argument("--no_pretty").flag().help("whether to print json structured logs");
	program.add_argument("--app_log_level").help("log level for gargantuan itself").default_value("trace");
	program.add_argument("--lua_log_level").help("log level for lua runtime").default_value("trace");
	program.add_argument("--sdl_log_level").help("log level for sdl library").default_value("trace");

	try {
		program.parse_args(argc, argv);
	} catch (std::exception &e) {
		std::cerr << e.what() << std::endl;
		std::exit(1);
	}

	LogContext logContext{
		.EnableAnsi = !program.is_used("--no_ansi"),
		.EnablePretty = !program.is_used("--no_pretty"),
	};
	SDL_SetLogPriorities(SDL_LOG_PRIORITY_TRACE);
	SDL_SetLogPriority(LogCategory::App, SDL_LOG_PRIORITY_TRACE);
	SDL_SetLogPriority(LogCategory::Lua, SDL_LOG_PRIORITY_TRACE);
	SDL_SetLogOutputFunction(GetLogOutputFunction(&logContext), &logContext);

	LOG_INFO(App, "Gargantuan start");

	const auto EditorHostMode = program.is_used("--editor-host");
	const auto EditorToken = program.get<std::string>("--editor-token");
	telemetry::Consent TelemetryConsent{
		.CrashReportsEnabled = program.is_used("--telemetry-crashes"),
		.PerformanceSnapshotsEnabled = program.is_used("--telemetry-performance"),
	};
	std::optional<std::array<std::uint8_t, 16>> ParentLaunchId;
	if (EditorHostMode && IsStudioEditorToken(EditorToken)) {
		TelemetryConsent.CrashReportsEnabled = TelemetryConsent.CrashReportsEnabled ||
			EnabledEnvironmentPolicy("GARGANTUAN_TELEMETRY_CRASHES");
		TelemetryConsent.PerformanceSnapshotsEnabled = TelemetryConsent.PerformanceSnapshotsEnabled ||
			EnabledEnvironmentPolicy("GARGANTUAN_TELEMETRY_PERFORMANCE");
		if (const auto *LaunchId = std::getenv("GARGANTUAN_TELEMETRY_LAUNCH_ID"))
			ParentLaunchId = telemetry::OptionalTelemetry::ParseLaunchId(LaunchId);
	}
#if defined(NDEBUG)
	const std::string TelemetryBuildConfiguration = "release";
#else
	const std::string TelemetryBuildConfiguration = "debug";
#endif
	auto Telemetry = telemetry::OptionalTelemetry::Load({
		.HostComponent = EditorHostMode ? telemetry::Component::EditorHost : telemetry::Component::Engine,
		.CategoryConsent = TelemetryConsent,
		.LibraryPath = telemetry::OptionalTelemetry::DefaultLibraryPath(),
		.StorageDirectory = telemetry::OptionalTelemetry::DefaultStorageDirectory(),
		.ApplicationVersion = "0.0.0",
		.BuildId = "gargantuan-main",
		.BuildConfiguration = TelemetryBuildConfiguration,
		.ParentLaunchId = ParentLaunchId,
	});
	Telemetry.SetPhase(telemetry::Phase::Startup);

	int hasProject = program.is_used("--project");
	int hasScript = program.is_used("--script");
	int hasInstance = program.is_used("--instance");
	Telemetry.SetPhase(telemetry::Phase::RuntimeSchemaBootstrap);
	try {
		if (hasProject) BootstrapProjectRuntimeSchema(program.get<std::string>("--project"));
		else BootstrapNativeRuntimeSchema();
	} catch (const std::exception &exception) {
		Telemetry.ReportControlledFatal(RuntimeSchemaFatalCode);
		std::cerr << "Runtime schema bootstrap failed: " << exception.what() << std::endl;
		return 1;
	}
	if (EditorHostMode) {
		if (hasProject + hasScript + hasInstance != 0) {
			LOG_CRITICAL(App, "EditorHost opens projects through its protocol and cannot accept a target argument");
			return 1;
		}
		try {
			Telemetry.SetPhase(telemetry::Phase::EditorHostLoop);
			EditorHost host(EditorToken, true);
			return host.Run(std::cin, std::cout, [&Telemetry] { Telemetry.PollPerformance(); });
		} catch (const std::exception &error) {
			Telemetry.ReportControlledFatal(EditorHostFatalCode);
			LOG_CRITICAL(App, "%s", error.what());
			return 1;
		}
	}
	if (hasProject + hasScript + hasInstance == 0) {
		LOG_CRITICAL(App, "No target provided, specify one of: --project, --script, or --instance");
		std::exit(1);
	} else if (hasProject + hasScript + hasInstance > 1) {
		LOG_CRITICAL(App, "Too many targets provided, specify one of: --project, --script, or --instance");
		std::exit(1);
	}

	Vector2 viewportSize(720, 540);
	BaseRenderer *renderer = nullptr;

	std::atexit(SDL_Quit);

	if (program.is_used("--headless")) {
		SDL_Init(SDL_INIT_EVENTS);

		renderer = new HeadlessRenderer(viewportSize);
	} else {
		SDL_Init(SDL_INIT_VIDEO);

		try {
			renderer = new SDLRenderer(viewportSize);
		} catch (std::exception &e) {
			Telemetry.ReportControlledFatal(RuntimeConstructionFatalCode);
			LOG_CRITICAL(App, "Failed to construct SDL3 renderer: %s", e.what());
			return 1;
		}
	}

	Engine *engine = nullptr;
	Telemetry.SetPhase(telemetry::Phase::ProjectOpen);
	try {
		const bool AudioEnabled = !program.is_used("--headless");
		engine = hasProject  ? ConstructProject(program.get<std::string>("--project"), renderer, AudioEnabled)
				 : hasScript ? ConstructScript(program.get<std::string>("--script"), renderer, AudioEnabled)
							 : ConstructInstance(program.get<std::string>("--instance"), renderer, AudioEnabled);
	} catch (const std::exception &Error) {
		Telemetry.ReportControlledFatal(RuntimeConstructionFatalCode);
		LOG_CRITICAL(App, "%s", Error.what());
		delete renderer;
		return 1;
	}

	LOG_INFO(App, "Starting engine loop");
	Telemetry.SetPhase(telemetry::Phase::EngineLoop);
	engine->ProcessService->Alive = true;
	SDLHost Host;
	auto PreviousFrame = std::chrono::steady_clock::now();
	try {
		while (engine->ProcessService->Alive) {
			HostEvent Event;
			while (Host.PollEvent(Event)) {
				auto Result = engine->ProcessEvent(Event);
				if (Result.Command) Host.Apply(*Result.Command);
				if (!engine->ProcessService->Alive) break;
			}
			if (!engine->ProcessService->Alive) break;
			const auto CurrentFrame = std::chrono::steady_clock::now();
			Telemetry.ObserveFrame(CurrentFrame - PreviousFrame);
			PreviousFrame = CurrentFrame;
			engine->Step();
		}
	} catch (std::exception &e) {
		Telemetry.ReportControlledFatal(EngineLoopFatalCode);
		std::cerr << e.what() << std::endl;
		engine->Destroy();
		delete engine;
		delete renderer;
		return 1;
	}

	auto exitCode = engine->ProcessService->ExitCode;
	engine->Destroy();
	delete engine;
	delete renderer;
	return exitCode;
}
