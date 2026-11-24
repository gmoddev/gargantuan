#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/Paths.hpp"
#include "gargantuan/filesystem/Project.hpp"
#include "gargantuan/packaging/PackageBuilder.hpp"
#include "gargantuan/reflection/PreRunRegistration.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/serialization/GasDocument.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
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

	bool HasSingleValue(int Argc, char *Argv[], std::string_view Name,
		const std::optional<std::string> &Value, bool Required = true) {
		const auto Count = std::count_if(Argv + 2, Argv + Argc, [Name](const char *Argument) {
			const std::string_view Option(Argument);
			return Option == Name || (Option.starts_with(Name) && Option.substr(Name.size()).starts_with('='));
		});
		if (Count == 0) return !Required;
		return Count == 1 && Value && !Value->empty() && !Value->starts_with("--");
	}

	int GasFailure(std::string_view Message) {
		std::cerr << "[error][Package:GAS] " << Message << '\n';
		return 3;
	}

	SerializationResult<Gas::Document> ReadGasPlace(const std::filesystem::path &Path) {
		if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(Path)))
			return SerializationFailure(SerializationErrorCode::InvalidValue, "GASP input must be a regular file.");
		const auto Size = std::filesystem::file_size(Path);
		if (Size > Gas::MaximumDocumentBytes)
			return SerializationFailure(SerializationErrorCode::LimitExceeded, "GASP input exceeds the document limit.");
		std::ifstream Input(Path, std::ios::binary);
		if (!Input)
			return SerializationFailure(SerializationErrorCode::InvalidValue, "GASP input could not be opened.");
		Gas::Binary Bytes(static_cast<std::size_t>(Size));
		if (!Bytes.empty() && !Input.read(reinterpret_cast<char *>(Bytes.data()), static_cast<std::streamsize>(Size)))
			return SerializationFailure(SerializationErrorCode::TruncatedInput, "GASP input could not be read in full.");
		if (Input.peek() != std::char_traits<char>::eof() || !Input.eof() || Input.bad())
			return SerializationFailure(SerializationErrorCode::InvalidValue, "GASP input changed or could not be read in full.");
		auto Document = Gas::Decode(Bytes);
		if (!Document) return Document;
		if (Document->Kind != Gas::DocumentKind::Place || !Document->Place)
			return SerializationFailure(SerializationErrorCode::InvalidType, "Package input must be a GAS place.");
		return Document;
	}

	struct LoadedGasPlace {
		std::shared_ptr<DataModel> World;
		std::optional<std::string> PreRunSource;
	};

	struct ScopedProjectWorld {
		std::shared_ptr<DataModel> World;

		~ScopedProjectWorld() noexcept {
			if (!World) return;
			try {
				// Explicitly destroy the private graph to break authored reference cycles.
				World->Destroy();
			} catch (...) {
				// Preserve the command result if teardown fails.
			}
		}
	};

	SerializationResult<LoadedGasPlace> LoadGasPlace(const Project &ProjectValue, const Gas::Document &Document) {
		if (Document.Place->Identity != ProjectValue.Identity)
			return SerializationFailure(SerializationErrorCode::InvalidValue,
				"Place identity does not match the project origin.");
		auto TrustedPreRun = ReadProjectPreRunSource(ProjectValue.Root);
		if (Document.Place->PreRunSource != TrustedPreRun)
			return SerializationFailure(SerializationErrorCode::InvalidValue,
				"Place PreRun source does not match the project origin.");
		// Execute the exact source already read from the trusted origin.
		BootstrapPackagedRuntimeSchema(TrustedPreRun);
		return LoadedGasPlace{ProjectValue.LoadGasPlace(Document), std::move(TrustedPreRun)};
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
		auto GasInput = ValueAfter(argc, argv, "--gasp");
		auto RuntimeRoot = ValueAfter(argc, argv, "--runtime");
		auto ConfigurationName = ValueAfter(argc, argv, "--configuration").value_or("Release");
		auto Configuration = ParsePackageConfiguration(ConfigurationName);
		if (!ProjectRoot || !Output || !Configuration || !HasSingleValue(argc, argv, "--gasp", GasInput, false)) {
			std::cerr << "Usage: gargantuan-packager build --project <root> --output <directory> "
						 "[--gasp <file>] [--runtime <distribution>] [--configuration Development|Release]\n";
			return 2;
		}
		try {
			const auto ProjectPath = std::filesystem::absolute(*ProjectRoot).lexically_normal();
			const auto OutputPath = std::filesystem::absolute(*Output).lexically_normal();
			const auto DistributionPath = RuntimeRoot ? std::filesystem::absolute(*RuntimeRoot).lexically_normal()
													  : Paths::GetExecutableDirectory() / "RuntimeDistribution";
			std::optional<Gas::Document> GasPlace;
			if (GasInput) {
				auto Parsed = ReadGasPlace(std::filesystem::absolute(*GasInput).lexically_normal());
				if (!Parsed) return GasFailure(Parsed.error().Message);
				GasPlace = std::move(*Parsed);
			} else {
				BootstrapProjectRuntimeSchema(ProjectPath);
			}
			DiskFilesystem Filesystem(ProjectPath);
			auto ProjectValue = GasPlace ? Project::FromGasOrigin(&Filesystem) : Project::fromExisting(&Filesystem);
			ScopedProjectWorld ProjectWorld;
			auto &World = ProjectWorld.World;
			std::optional<std::string> TrustedPreRun;
			if (GasPlace) {
				auto Loaded = LoadGasPlace(ProjectValue, *GasPlace);
				if (!Loaded) return GasFailure(Loaded.error().Message);
				World = std::move(Loaded->World);
				TrustedPreRun = std::move(Loaded->PreRunSource);
				GasPlace.reset();
			} else {
				World = ProjectValue.DeserializeGame();
			}
			World->InitializeLoadedProjectRevision();
			auto Payload = PackageBuilder::Capture(
				ProjectValue, World, World->GetAuthoritativeRevision(), World->GetAuthoritativeRevision()
			);
			if (GasInput && Payload.PreRunSource != TrustedPreRun)
				return GasFailure("Project PreRun source changed during package capture.");
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
			return 0;
		} catch (const std::exception &) {
			if (GasInput) return GasFailure("The GAS place could not be captured or packaged safely.");
			std::cerr << "[error][Package:Packaging] The authoring project could not be captured or packaged safely.\n";
			return 3;
		}
	}

	int ExportGasPlace(int Argc, char *Argv[]) {
		auto ProjectRoot = ValueAfter(Argc, Argv, "--project");
		auto Output = ValueAfter(Argc, Argv, "--output");
		if (Argc != 6 || !HasSingleValue(Argc, Argv, "--project", ProjectRoot) ||
			!HasSingleValue(Argc, Argv, "--output", Output)) {
			std::cerr << "Usage: gargantuan-packager export-gasp --project <root> --output <file.gasp>\n";
			return 2;
		}
		try {
			const auto ProjectPath = std::filesystem::absolute(*ProjectRoot).lexically_normal();
			const auto OutputPath = std::filesystem::absolute(*Output).lexically_normal();
			const auto TrustedPreRun = BootstrapProjectRuntimeSchema(ProjectPath);
			DiskFilesystem Filesystem(ProjectPath);
			auto ProjectValue = Project::fromExisting(&Filesystem);
			const ScopedProjectWorld ProjectWorld{ProjectValue.DeserializeGame()};
			ProjectValue.ExportGasPlace(ProjectWorld.World, OutputPath, TrustedPreRun);
			std::cout << "[Package:GAS] Exported a GAS place.\n";
			return 0;
		} catch (const std::exception &) {
			return GasFailure("The authoring project could not be exported safely.");
		}
	}

	int RestoreGasPlace(int Argc, char *Argv[]) {
		auto ProjectRoot = ValueAfter(Argc, Argv, "--project");
		auto GasInput = ValueAfter(Argc, Argv, "--gasp");
		if (Argc != 6 || !HasSingleValue(Argc, Argv, "--project", ProjectRoot) ||
			!HasSingleValue(Argc, Argv, "--gasp", GasInput)) {
			std::cerr << "Usage: gargantuan-packager restore-gasp --project <root> --gasp <file>\n";
			return 2;
		}
		try {
			auto Parsed = ReadGasPlace(std::filesystem::absolute(*GasInput).lexically_normal());
			if (!Parsed) return GasFailure(Parsed.error().Message);
			const auto ProjectPath = std::filesystem::absolute(*ProjectRoot).lexically_normal();
			DiskFilesystem Filesystem(ProjectPath);
			auto ProjectValue = Project::FromGasOrigin(&Filesystem);
			auto Loaded = LoadGasPlace(ProjectValue, *Parsed);
			if (!Loaded) return GasFailure(Loaded.error().Message);
			const ScopedProjectWorld ProjectWorld{std::move(Loaded->World)};
			ProjectValue.RestoreGasPlace(ProjectWorld.World, Loaded->PreRunSource);
			std::cout << "[Package:GAS] Restored project.instance.json.\n";
			return 0;
		} catch (const std::exception &) {
			return GasFailure("The GAS place could not be restored to a project instance file safely.");
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
		std::cerr << "Usage: gargantuan-packager <build|export-gasp|restore-gasp|validate|inspect> ...\n";
		return 2;
	}
	const std::string_view Command(argv[1]);
	if (Command == "build") return BuildPackage(argc, argv);
	if (Command == "export-gasp") return ExportGasPlace(argc, argv);
	if (Command == "restore-gasp") return RestoreGasPlace(argc, argv);
	if (Command == "validate") return ValidatePackage(argc, argv);
	if (Command == "inspect") return InspectPackage(argc, argv);
	std::cerr << "Unknown gargantuan-packager command.\n";
	return 2;
}
