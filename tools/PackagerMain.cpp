#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/Paths.hpp"
#include "gargantuan/filesystem/Project.hpp"
#include "gargantuan/packaging/PackageBuilder.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

using namespace gargantuan;

namespace {
	std::optional<std::string> ValueAfter(int argc, char *argv[], std::string_view Name) {
		for (int Index = 2; Index + 1 < argc; ++Index)
			if (std::string_view(argv[Index]) == Name) return argv[Index + 1];
		return std::nullopt;
	}

	void PrintDiagnostics(const std::vector<PackageDiagnostic> &Diagnostics) {
		for (const auto &Diagnostic : Diagnostics) {
			const auto Severity = Diagnostic.Severity == PackageDiagnosticSeverity::Error	  ? "error"
								  : Diagnostic.Severity == PackageDiagnosticSeverity::Warning ? "warning"
																							  : "info";
			std::cout << '[' << Severity << "][Package:" << Diagnostic.Category << "] " << Diagnostic.Code << ": "
					  << Diagnostic.Message;
			if (!Diagnostic.Item.empty()) std::cout << " (" << Diagnostic.Item << ')';
			std::cout << '\n';
		}
	}

	int BuildPackage(int argc, char *argv[]) {
		auto ProjectRoot = ValueAfter(argc, argv, "--project");
		auto Output = ValueAfter(argc, argv, "--output");
		auto RuntimeRoot = ValueAfter(argc, argv, "--runtime");
		auto ConfigurationName = ValueAfter(argc, argv, "--configuration").value_or("Release");
		auto Configuration = ParsePackageConfiguration(ConfigurationName);
		if (!ProjectRoot || !Output || !Configuration) {
			std::cerr << "Usage: gargantuan-packager build --project <root> --output <directory> "
						 "[--runtime <distribution>] [--configuration Development|Release]\n";
			return 2;
		}
		const auto ProjectPath = std::filesystem::absolute(*ProjectRoot).lexically_normal();
		const auto OutputPath = std::filesystem::absolute(*Output).lexically_normal();
		const auto DistributionPath = RuntimeRoot ? std::filesystem::absolute(*RuntimeRoot).lexically_normal()
												  : Paths::GetExecutableDirectory() / "RuntimeDistribution";
		try {
			BootstrapProjectRuntimeSchema(ProjectPath);
			DiskFilesystem Filesystem(ProjectPath);
			auto ProjectValue = Project::fromExisting(&Filesystem);
			auto World = ProjectValue.DeserializeGame();
			World->InitializeLoadedProjectRevision();
			auto Payload = PackageBuilder::Capture(
				ProjectValue, World, World->GetAuthoritativeRevision(), World->GetAuthoritativeRevision()
			);
			auto Result = PackageBuilder::Build({
				.Payload = std::move(Payload),
				.RuntimeDistributionRoot = DistributionPath,
				.OutputDirectory = OutputPath,
				.Configuration = *Configuration,
				.Progress = [](PackagePhase Phase) {
					std::cout << "[Package:Progress] " << GetPackagePhaseName(Phase) << '\n';
				},
			});
			PrintDiagnostics(Result.Diagnostics);
			if (!Result.Ok) return 3;
			std::cout << "[Package:Result] revision=" << Result.PackagedRevision
					  << " projectId=" << Result.Identity.ToString() << " totalBytes=" << Result.Size.TotalBytes
					  << " runtimeBytes=" << Result.Size.RuntimeBytes << " projectBytes=" << Result.Size.ProjectBytes
					  << " assetBytes=" << Result.Size.AssetBytes << " shaderBytes=" << Result.Size.ShaderBytes
					  << " otherBytes=" << Result.Size.OtherBytes << '\n';
			World->Destroy();
			return 0;
		} catch (const std::exception &) {
			std::cerr << "[error][Package:Packaging] The authoring project could not be captured or packaged safely.\n";
			return 3;
		}
	}

	int ValidatePackage(int argc, char *argv[]) {
		if (argc != 3) {
			std::cerr << "Usage: gargantuan-packager validate <package>\n";
			return 2;
		}
		auto Diagnostics = PackageBuilder::Validate(std::filesystem::absolute(argv[2]).lexically_normal());
		PrintDiagnostics(Diagnostics);
		return std::ranges::any_of(
				   Diagnostics,
				   [](const auto &Diagnostic) { return Diagnostic.Severity == PackageDiagnosticSeverity::Error; }
			   )
				   ? 3
				   : 0;
	}

	int InspectPackage(int argc, char *argv[]) {
		if (argc != 3) {
			std::cerr << "Usage: gargantuan-packager inspect <package>\n";
			return 2;
		}
		std::vector<PackageDiagnostic> Diagnostics;
		auto Inspection = PackageBuilder::Inspect(std::filesystem::absolute(argv[2]).lexically_normal(), Diagnostics);
		if (!Inspection) {
			PrintDiagnostics(Diagnostics);
			return 3;
		}
		std::cout << "{\"ProjectId\":\"" << Inspection->Identity.ToString() << "\",\"DisplayName\":\"";
		for (const auto Character : Inspection->DisplayName)
			std::cout
				<< (Character == '"' || Character == '\\' || static_cast<unsigned char>(Character) < 0x20 ? '_'
																										  : Character);
		std::cout << "\",\"PackageFormatVersion\":" << Inspection->FormatVersion
				  << ",\"RuntimeCompatibility\":" << Inspection->RuntimeCompatibility
				  << ",\"Revision\":" << Inspection->Revision << ",\"ContentCount\":" << Inspection->ContentCount
				  << ",\"ContentBytes\":" << Inspection->ContentBytes << "}\n";
		return 0;
	}
}

int main(int argc, char *argv[]) {
	if (!SDL_Init(0)) {
		std::cerr << "[error][Package:Runtime] SDL platform support could not initialize.\n";
		return 1;
	}
	struct SdlLifetime final {
		~SdlLifetime() {
			SDL_Quit();
		}
	} Sdl;
	if (argc < 2) {
		std::cerr << "Usage: gargantuan-packager <build|validate|inspect> ...\n";
		return 2;
	}
	const std::string_view Command(argv[1]);
	if (Command == "build") return BuildPackage(argc, argv);
	if (Command == "validate") return ValidatePackage(argc, argv);
	if (Command == "inspect") return InspectPackage(argc, argv);
	std::cerr << "Unknown gargantuan-packager command.\n";
	return 2;
}
