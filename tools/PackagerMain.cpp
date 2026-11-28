#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/filesystem/Paths.hpp"
#include "gargantuan/filesystem/Project.hpp"
#include "gargantuan/packaging/PackageBuilder.hpp"
#include "gargantuan/reflection/PreRunRegistration.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/serialization/GasDocument.hpp"
#include "serialization/JsonCodec.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

using namespace gargantuan;

namespace {
	std::filesystem::path AbsolutePath(std::string_view Utf8) {
		return std::filesystem::absolute(std::filesystem::u8path(Utf8)).lexically_normal();
	}

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
		if (Size > Gas::MaximumFileBytes)
			return SerializationFailure(SerializationErrorCode::LimitExceeded, "GASP input exceeds the document limit.");
		std::ifstream Input(Path, std::ios::binary);
		if (!Input)
			return SerializationFailure(SerializationErrorCode::InvalidValue, "GASP input could not be opened.");
		if (Size>Gas::MaximumDocumentBytes) {
			std::array<std::uint8_t,Gas::FilePrefixBytes> Prefix{};
			if (!Input.read(reinterpret_cast<char*>(Prefix.data()),Prefix.size()) || !Gas::AdmitsFileLength(Prefix,Size))
				return SerializationFailure(SerializationErrorCode::LimitExceeded,"GASP input exceeds its document profile.");
			Input.seekg(0);
		}
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
		constexpr std::array<std::string_view, 7> BuildOptions{
			"--project", "--output", "--gasp", "--runtime", "--configuration", "--audience", "--distribution",
		};
		std::set<std::string_view> Seen;
		bool ValidOptions = argc % 2 == 0;
		for (int Index = 2; ValidOptions && Index + 1 < argc; Index += 2) {
			const std::string_view Name(argv[Index]), Value(argv[Index + 1]);
			ValidOptions = std::ranges::contains(BuildOptions, Name) && Seen.insert(Name).second &&
				!Value.empty() && !Value.starts_with("--");
		}
		auto ProjectRoot = ValueAfter(argc, argv, "--project");
		auto Output = ValueAfter(argc, argv, "--output");
		auto GasInput = ValueAfter(argc, argv, "--gasp");
		auto RuntimeRoot = ValueAfter(argc, argv, "--runtime");
		auto AudienceName = ValueAfter(argc, argv, "--audience");
		auto Audience = ParsePackageAudience(AudienceName.value_or("Trusted"));
		auto PolicyPath = ValueAfter(argc, argv, "--distribution");
		auto ConfigurationName = ValueAfter(argc, argv, "--configuration").value_or("Release");
		auto Configuration = ParsePackageConfiguration(ConfigurationName);
		if (!ValidOptions || !ProjectRoot || !Output || !Configuration || !Audience) {
			std::cerr << "Usage: gargantuan-packager build --project <root> --output <directory> "
						 "[--gasp <file>] [--runtime <distribution>] [--configuration Development|Release] "
						 "[--audience Trusted|DistributedPlayer] [--distribution <policy-request.json>]\n";
			return 2;
		}
		try {
			std::optional<RuntimeDistributionRequest> Policy;
			if (PolicyPath) {
				std::ifstream PolicyFile(std::filesystem::u8path(*PolicyPath), std::ios::binary);
				std::array<char, 8193> Buffer{};
				PolicyFile.read(Buffer.data(), static_cast<std::streamsize>(Buffer.size()));
				if (PolicyFile.gcount() == 0 || PolicyFile.gcount() == Buffer.size() || PolicyFile.bad())
					throw RuntimeDistributionError("InvalidPolicy", "The runtime distribution request could not be read safely.");
				Policy = ParseRuntimeDistributionRequest({Buffer.data(), static_cast<std::size_t>(PolicyFile.gcount())});
				const auto Requested = Policy->Target == RuntimeDistributionTarget::DistributedPlayer ?
					PackageAudience::DistributedPlayer : PackageAudience::Trusted;
				if (AudienceName && *Audience != Requested)
					throw RuntimeDistributionError("AudienceViolation", "The requested target and audience disagree.");
				Audience = Requested;
			}
			const auto ProjectPath = AbsolutePath(*ProjectRoot);
			const auto OutputPath = AbsolutePath(*Output);
			const auto DistributionPath = RuntimeRoot ? AbsolutePath(*RuntimeRoot)
				: Paths::GetExecutableDirectory() / (Policy && Policy->Target == RuntimeDistributionTarget::DedicatedServer ?
					"ServerRuntimeDistribution" : "RuntimeDistribution");
			std::optional<Gas::Document> GasPlace;
			if (GasInput) {
				auto Parsed = ReadGasPlace(AbsolutePath(*GasInput));
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
				ProjectValue, World, World->GetAuthoritativeRevision(), World->GetAuthoritativeRevision(), *Audience, Policy
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
			if (Policy) {
				JsonCodec::Json Codes = JsonCodec::Json::array();
				JsonCodec::Json Timings = JsonCodec::Json::object();
				for (const auto &Value : Result.Diagnostics) Codes.push_back(Value.Code);
				for (const auto &Timing : Result.Timings) Timings[GetPackagePhaseName(Timing.Phase)] = Timing.Duration.count();
				auto Receipt = JsonCodec::Json{
					{"Ok", Result.Ok}, {"Target", GetRuntimeDistributionName(Policy->Target)},
					{"ManifestSha256", Result.ManifestSha256}, {"ExecutableSha256", Result.PlayerExecutableSha256},
					{"Diagnostics", std::move(Codes)}, {"PhaseMicroseconds", std::move(Timings)},
					{"TotalBytes", Result.Size.TotalBytes}, {"ProjectBytes", Result.Size.ProjectBytes},
				};
				if (!Result.PrivateDiagnosticsPath.empty())
					Receipt["PrivateDiagnosticsPath"] = Paths::ToGenericUtf8(Result.PrivateDiagnosticsPath);
				std::cout << Receipt.dump() << '\n';
			}
			if (!Result.Ok) return 3;
			std::cout << "[Package:Result] revision=" << Result.PackagedRevision
					  << " projectId=" << Result.Identity.ToString() << " totalBytes=" << Result.Size.TotalBytes
					  << " runtimeBytes=" << Result.Size.RuntimeBytes << " projectBytes=" << Result.Size.ProjectBytes
					  << " assetBytes=" << Result.Size.AssetBytes << " shaderBytes=" << Result.Size.ShaderBytes
					  << " otherBytes=" << Result.Size.OtherBytes << '\n';
			return 0;
		} catch (const RuntimeDistributionError &Error) {
			PrintDiagnostics({{PackageDiagnosticSeverity::Error, "RuntimeDistribution", Error.Code, Error.what(), {}}});
			std::cout << JsonCodec::Json{{"Ok", false}, {"Diagnostics", JsonCodec::Json::array({Error.Code})}}.dump() << '\n';
			return 3;
		} catch (const std::exception &) {
			if (*Audience == PackageAudience::DistributedPlayer) {
				std::cerr << "[Package:Audience] Could not capture a distributed Player package. "
					"Check the native schema, client code paths, and .gargantuan/package-audience.json.\n";
				return 3;
			}
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
			const auto ProjectPath = AbsolutePath(*ProjectRoot);
			const auto OutputPath = AbsolutePath(*Output);
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
			auto Parsed = ReadGasPlace(AbsolutePath(*GasInput));
			if (!Parsed) return GasFailure(Parsed.error().Message);
			const auto ProjectPath = AbsolutePath(*ProjectRoot);
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
		std::optional<PackageAudience> Audience;
		RuntimeDistributionAdmission Admission;
		bool Valid = argc >= 3;
		for (int Index = 3; Valid && Index < argc; ++Index) {
			const std::string_view Option(argv[Index]);
			if (Option == "--require-package-publisher" && !Admission.RequirePublisher) {
				Admission.RequirePublisher = true;
			} else if (Option == "--audience" && !Audience && Index + 1 < argc) {
				Audience = ParsePackageAudience(argv[++Index]);
				Valid = static_cast<bool>(Audience);
			} else if (Option == "--expected-manifest-sha256" && !Admission.ExpectedManifestSha256 && Index + 1 < argc) {
				Admission.ExpectedManifestSha256 = argv[++Index];
				Valid = AssetContentId::Parse(*Admission.ExpectedManifestSha256).has_value();
			} else Valid = false;
		}
		if (!Valid) {
			std::cerr << "Usage: gargantuan-packager validate <package> [--audience Trusted|DistributedPlayer] "
				"[--expected-manifest-sha256 <digest>] [--require-package-publisher]\n";
			return 2;
		}
		auto Diagnostics = PackageBuilder::Validate(AbsolutePath(argv[2]), Audience, Admission);
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
		auto Inspection = PackageBuilder::Inspect(AbsolutePath(argv[2]), Diagnostics);
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
				  << ",\"ContentBytes\":" << Inspection->ContentBytes;
		if (Inspection->Audience == PackageAudience::DistributedPlayer)
			std::cout << ",\"Audience\":\"" << GetPackageAudienceName(Inspection->Audience) << '"';
		if (Inspection->Target)
			std::cout << ",\"Target\":\"" << GetRuntimeDistributionName(*Inspection->Target)
				<< "\",\"ManifestSha256\":\"" << Inspection->ManifestSha256 << '"';
		std::cout << "}\n";
		return 0;
	}
}

static int RunPackager(int argc, char *argv[]) {
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

#if defined(_WIN32)
int wmain(int Count, wchar_t *WideArgs[]) {
	std::vector<std::string> Utf8;
	std::vector<char *> Arguments;
	Utf8.reserve(Count);
	for (int Index = 0; Index < Count; ++Index)
		Utf8.push_back(Paths::ToUtf8(WideArgs[Index]));
	for (auto &Value : Utf8) Arguments.push_back(Value.data());
	Arguments.push_back(nullptr);
	return RunPackager(Count, Arguments.data());
}
#else
int main(int Count, char *Args[]) {
	return RunPackager(Count, Args);
}
#endif
