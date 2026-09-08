#include "host/common/PackagedHost.hpp"

#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace gargantuan::host {
	std::optional<network::TransportEndpoint> ParseEndpoint(std::string_view Value) {
		if (Value.empty() || Value.size() > network::MaximumTransportEndpointBytes) return std::nullopt;
		std::string Host;
		std::string PortText;
		if (Value.front() == '[') {
			const auto End = Value.find(']');
			if (End == std::string_view::npos || End + 2 > Value.size() || Value[End + 1] != ':') return std::nullopt;
			Host = Value.substr(1, End - 1);
			PortText = Value.substr(End + 2);
		} else {
			const auto Separator = Value.rfind(':');
			if (Separator == std::string_view::npos) return std::nullopt;
			Host = Value.substr(0, Separator);
			PortText = Value.substr(Separator + 1);
		}
		if (Host.empty() || PortText.empty()) return std::nullopt;
		std::uint32_t Port = 0;
		for (const auto Character : PortText) {
			if (Character < '0' || Character > '9' || Port > 6553) return std::nullopt;
			Port = Port * 10 + static_cast<std::uint32_t>(Character - '0');
		}
		network::TransportEndpoint Result{std::move(Host), static_cast<std::uint16_t>(Port)};
		return Result.IsValid() ? std::optional(std::move(Result)) : std::nullopt;
	}

	namespace {
		void ValidateNativeRuntimeClosure(const std::filesystem::path &RuntimeRoot) {
#if defined(_WIN32)
			const auto CanonicalRoot = std::filesystem::weakly_canonical(RuntimeRoot);
			for (const auto *ModuleName : {
					 L"SDL3.dll",
					 L"MSVCP140.dll",
					 L"VCRUNTIME140.dll",
					 L"VCRUNTIME140_1.dll",
				 }) {
				const auto Module = GetModuleHandleW(ModuleName);
				std::wstring Buffer(32'768, L'\0');
				const auto Length = Module ? GetModuleFileNameW(Module, Buffer.data(), static_cast<DWORD>(Buffer.size())) : 0;
				if (Length == 0 || Length == Buffer.size())
					throw std::runtime_error("a required native runtime module is unavailable");
				Buffer.resize(Length);
				if (std::filesystem::weakly_canonical(Buffer).parent_path() != CanonicalRoot)
					throw std::runtime_error("a required native runtime module was not loaded from the runtime root");
			}
#else
			(void)RuntimeRoot;
#endif
		}
	}

	std::optional<RuntimePackagePayload> BootstrapPackagedRuntime(
		const std::filesystem::path &PackageRoot,
		const std::filesystem::path &RuntimeRoot,
		std::string_view HostName,
		int &ExitCode
	) {
		std::vector<PackageDiagnostic> Diagnostics;
		auto Payload = PackageBuilder::Load(PackageRoot, Diagnostics);
		if (!Payload) {
			std::cerr << HostName << " could not validate this game package.\n";
			for (const auto &Diagnostic : Diagnostics)
				if (Diagnostic.Severity == PackageDiagnosticSeverity::Error)
					std::cerr << "[Package:" << Diagnostic.Category << "] " << Diagnostic.Message << '\n';
			ExitCode = 3;
			return std::nullopt;
		}
		try {
			ValidateNativeRuntimeClosure(RuntimeRoot);
		} catch (const std::exception &) {
			std::cerr << HostName << " could not establish its packaged native runtime closure.\n";
			ExitCode = 4;
			return std::nullopt;
		}
		try {
			BootstrapPackagedRuntimeSchema(Payload->PreRunSource);
		} catch (const std::exception &) {
			std::cerr << HostName << " could not initialize the packaged runtime schema.\n";
			ExitCode = 5;
			return std::nullopt;
		}
		return Payload;
	}
}
