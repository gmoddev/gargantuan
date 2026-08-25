#include "gargantuan/packaging/PackageBuilder.hpp"

#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/filesystem/Project.hpp"
#include "gargantuan/reflection/PreRunRegistration.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "serialization/JsonCodec.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <chrono>
#include <format>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace gargantuan {
	namespace {
		using Json = nlohmann::ordered_json;

		constexpr std::string_view PackageFormat = "GargantuanGamePackage";
		constexpr std::string_view DistributionFormat = "GargantuanRuntimeDistribution";
		constexpr std::uint32_t DistributionVersion = 1;
		constexpr std::size_t MaximumManifestBytes = 4 * 1024 * 1024;
		constexpr std::size_t MaximumDistributionManifestBytes = 1024 * 1024;
		constexpr std::size_t MaximumContentEntries = 16'384;
		constexpr std::size_t MaximumRelativePathBytes = 512;
		constexpr std::uint64_t MaximumAggregateContentBytes = 8ull * 1024 * 1024 * 1024;
		constexpr std::uint64_t MaximumIndividualContentBytes = 512ull * 1024 * 1024;
		constexpr std::size_t MaximumProjectSnapshotBytes = 16 * 1024 * 1024;
		constexpr std::size_t MaximumDisplayNameBytes = 256;
		constexpr std::size_t MaximumRuntimeDependencies = 128;
		constexpr std::size_t MaximumRequiredShaders = 64;
		constexpr std::size_t MaximumDiagnostics = 128;
		constexpr std::array RequiredRuntimeFiles{
			"runtime/DefaultActionMap.luau",
			"runtime/DefaultPlayerController.luau",
			"runtime/DefaultCamera.luau",
			"runtime/DefaultPlayerRuntime.luau",
			"runtime/GargantuanSans.ttf",
			"shaders/gui.frag.spv",
			"shaders/gui.vert.spv",
			"shaders/opaque.frag.spv",
			"shaders/opaque.vert.spv",
			"shaders/shadow.frag.spv",
			"shaders/shadow.vert.spv",
			"notices/Gargantuan.txt",
			"notices/SDL3.txt",
			"notices/SDL3_image.txt",
			"notices/SDL3_ttf.txt",
		};

		struct ContentEntry {
			std::string Path;
			std::uint64_t Size = 0;
			std::string Sha256;
			std::string Category;
		};

		struct RuntimeDistributionEntry {
			std::string Path;
			std::string Category;
		};

		struct RuntimeDistributionDescription {
			std::string Platform;
			std::string Player;
			std::vector<RuntimeDistributionEntry> Files;
		};

		struct ParsedPackageManifest {
			PackageInspection Inspection;
			std::string ProjectPath;
			std::string AssetCatalogPath;
			std::optional<std::string> PreRunPath;
			std::string ContentTableSha256;
			std::vector<ContentEntry> Content;
		};

		class Sha256 final {
		  public:
			void Update(std::span<const std::uint8_t> Bytes) {
				if (Finalized) throw std::logic_error("SHA-256 digest is already finalized");
				if (Bytes.size() > ((std::numeric_limits<std::uint64_t>::max)() - TotalBytes))
					throw std::overflow_error("SHA-256 input length is exhausted");
				TotalBytes += Bytes.size();
				for (const auto Byte : Bytes) {
					Buffer[Buffered++] = Byte;
					if (Buffered == Buffer.size()) {
						Transform(Buffer);
						Buffered = 0;
					}
				}
			}

			std::string Final() {
				if (Finalized) throw std::logic_error("SHA-256 digest is already finalized");
				const auto BitLength = TotalBytes * 8;
				Buffer[Buffered++] = 0x80;
				if (Buffered > 56) {
					while (Buffered < Buffer.size())
						Buffer[Buffered++] = 0;
					Transform(Buffer);
					Buffered = 0;
				}
				while (Buffered < 56)
					Buffer[Buffered++] = 0;
				for (int Shift = 56; Shift >= 0; Shift -= 8)
					Buffer[Buffered++] = static_cast<std::uint8_t>(BitLength >> Shift);
				Transform(Buffer);
				Finalized = true;
				constexpr char Hex[] = "0123456789abcdef";
				std::string Result(64, '0');
				for (std::size_t Index = 0; Index < State.size(); ++Index) {
					for (std::size_t Byte = 0; Byte < 4; ++Byte) {
						const auto Value = static_cast<std::uint8_t>(State[Index] >> (24 - Byte * 8));
						Result[(Index * 4 + Byte) * 2] = Hex[Value >> 4];
						Result[(Index * 4 + Byte) * 2 + 1] = Hex[Value & 0xf];
					}
				}
				return Result;
			}

		  private:
			static constexpr std::array<std::uint32_t, 64> Constants{
				0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
				0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
				0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
				0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
				0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
				0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
				0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
				0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
			};
			std::array<std::uint32_t, 8> State{
				0x6a09e667,
				0xbb67ae85,
				0x3c6ef372,
				0xa54ff53a,
				0x510e527f,
				0x9b05688c,
				0x1f83d9ab,
				0x5be0cd19,
			};
			std::array<std::uint8_t, 64> Buffer{};
			std::size_t Buffered = 0;
			std::uint64_t TotalBytes = 0;
			bool Finalized = false;

			void Transform(const std::array<std::uint8_t, 64> &Block) {
				std::array<std::uint32_t, 64> Words{};
				for (std::size_t Index = 0; Index < 16; ++Index) {
					const auto Position = Index * 4;
					Words[Index] = (static_cast<std::uint32_t>(Block[Position]) << 24) |
								   (static_cast<std::uint32_t>(Block[Position + 1]) << 16) |
								   (static_cast<std::uint32_t>(Block[Position + 2]) << 8) | Block[Position + 3];
				}
				for (std::size_t Index = 16; Index < Words.size(); ++Index) {
					const auto S0 = std::rotr(Words[Index - 15], 7) ^ std::rotr(Words[Index - 15], 18) ^
									(Words[Index - 15] >> 3);
					const auto S1 = std::rotr(Words[Index - 2], 17) ^ std::rotr(Words[Index - 2], 19) ^
									(Words[Index - 2] >> 10);
					Words[Index] = Words[Index - 16] + S0 + Words[Index - 7] + S1;
				}
				auto [A, B, C, D, E, F, G, H] = State;
				for (std::size_t Index = 0; Index < Words.size(); ++Index) {
					const auto S1 = std::rotr(E, 6) ^ std::rotr(E, 11) ^ std::rotr(E, 25);
					const auto Choice = (E & F) ^ (~E & G);
					const auto T1 = H + S1 + Choice + Constants[Index] + Words[Index];
					const auto S0 = std::rotr(A, 2) ^ std::rotr(A, 13) ^ std::rotr(A, 22);
					const auto Majority = (A & B) ^ (A & C) ^ (B & C);
					const auto T2 = S0 + Majority;
					H = G;
					G = F;
					F = E;
					E = D + T1;
					D = C;
					C = B;
					B = A;
					A = T1 + T2;
				}
				State[0] += A;
				State[1] += B;
				State[2] += C;
				State[3] += D;
				State[4] += E;
				State[5] += F;
				State[6] += G;
				State[7] += H;
			}
		};

		void AddDiagnostic(
			std::vector<PackageDiagnostic> &Diagnostics,
			PackageDiagnosticSeverity Severity,
			std::string Category,
			std::string Code,
			std::string Message,
			std::string Item = {}
		) {
			if (Diagnostics.size() == MaximumDiagnostics) return;
			Diagnostics.push_back(
				{Severity, std::move(Category), std::move(Code), std::move(Message), std::move(Item)}
			);
		}

		bool HasErrors(const std::vector<PackageDiagnostic> &Diagnostics) {
			return std::ranges::any_of(Diagnostics, [](const auto &Diagnostic) {
				return Diagnostic.Severity == PackageDiagnosticSeverity::Error;
			});
		}

		std::string LowerPath(std::string_view Path) {
			std::string Result(Path);
			std::ranges::transform(Result, Result.begin(), [](unsigned char Value) {
				return static_cast<char>(std::tolower(Value));
			});
			return Result;
		}

		bool IsSafeRelativePath(std::string_view Value) {
			if (Value.empty() || Value.size() > MaximumRelativePathBytes || Value.front() == '/' ||
				Value.find('\\') != std::string_view::npos || Value.find(':') != std::string_view::npos)
				return false;
			const auto Path = std::filesystem::path(std::string(Value));
			if (Path.is_absolute() || Path.has_root_name() || Path.has_root_directory() ||
				Path.generic_string() != Value)
				return false;
			for (const auto &Component : Path)
				if (Component == "." || Component == ".." || Component.empty()) return false;
			return true;
		}

		bool IsReparseOrLink(const std::filesystem::path &Path) {
			std::error_code Error;
			const auto Status = std::filesystem::symlink_status(Path, Error);
			if (Error || std::filesystem::is_symlink(Status)) return true;
#if defined(_WIN32)
			const auto Attributes = GetFileAttributesW(Path.c_str());
			if (Attributes == INVALID_FILE_ATTRIBUTES || (Attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return true;
#endif
			return false;
		}

		std::optional<std::filesystem::path>
		ResolvePackageFile(const std::filesystem::path &Root, std::string_view Relative, bool MustExist) {
			if (!IsSafeRelativePath(Relative)) return std::nullopt;
			auto Current = Root;
			for (const auto &Component : std::filesystem::path(std::string(Relative))) {
				Current /= Component;
				if (std::filesystem::exists(Current) && IsReparseOrLink(Current)) return std::nullopt;
			}
			if (MustExist && (!std::filesystem::is_regular_file(Current) || IsReparseOrLink(Current)))
				return std::nullopt;
			return Current;
		}

		std::string HashBytes(std::span<const std::uint8_t> Bytes) {
			Sha256 Hash;
			Hash.Update(Bytes);
			return Hash.Final();
		}

		std::string HashText(std::string_view Text) {
			return HashBytes(std::span(reinterpret_cast<const std::uint8_t *>(Text.data()), Text.size()));
		}

		std::string HashFile(const std::filesystem::path &Path, std::uint64_t ExpectedSize) {
			std::ifstream Input(Path, std::ios::binary);
			if (!Input) throw std::runtime_error("Could not open a package content file");
			Sha256 Hash;
			std::array<std::uint8_t, 64 * 1024> Buffer{};
			std::uint64_t Total = 0;
			while (Input) {
				Input.read(reinterpret_cast<char *>(Buffer.data()), Buffer.size());
				const auto Count = Input.gcount();
				if (Count < 0 || Total > ExpectedSize - std::min<std::uint64_t>(Count, ExpectedSize))
					throw std::runtime_error("Package content size changed while hashing");
				if (Count != 0) Hash.Update(std::span(Buffer.data(), static_cast<std::size_t>(Count)));
				Total += static_cast<std::uint64_t>(Count);
			}
			if (!Input.eof() || Total != ExpectedSize)
				throw std::runtime_error("Could not read complete package content");
			return Hash.Final();
		}

		ContentEntry CopyAndHash(
			const std::filesystem::path &Source,
			const std::filesystem::path &Destination,
			std::string Relative,
			std::string Category,
			const PackageCancellationToken &Cancellation
		) {
			if (!std::filesystem::is_regular_file(Source) || IsReparseOrLink(Source))
				throw std::runtime_error("Runtime distribution contains a missing or redirected file");
			const auto Size = std::filesystem::file_size(Source);
			if (Size > MaximumIndividualContentBytes)
				throw std::runtime_error("Package content file exceeds its byte limit");
			std::filesystem::create_directories(Destination.parent_path());
			std::ifstream Input(Source, std::ios::binary);
			std::ofstream Output(Destination, std::ios::binary | std::ios::trunc);
			if (!Input || !Output) throw std::runtime_error("Could not open package staging files");
			Sha256 Hash;
			std::array<std::uint8_t, 64 * 1024> Buffer{};
			std::uint64_t Total = 0;
			while (Input) {
				if (Cancellation.IsCancelled()) throw std::runtime_error("Package build cancelled");
				Input.read(reinterpret_cast<char *>(Buffer.data()), Buffer.size());
				const auto Count = Input.gcount();
				if (Count < 0 || Total > Size - std::min<std::uint64_t>(Count, Size))
					throw std::runtime_error("Runtime content size changed while staging");
				if (Count != 0) {
					Output.write(reinterpret_cast<const char *>(Buffer.data()), Count);
					Hash.Update(std::span(Buffer.data(), static_cast<std::size_t>(Count)));
				}
				Total += static_cast<std::uint64_t>(Count);
			}
			Output.flush();
			if (!Input.eof() || !Output || Total != Size)
				throw std::runtime_error("Could not stage complete runtime content");
			Output.close();
			if (!Output) throw std::runtime_error("Could not close staged runtime content");
#if !defined(_WIN32)
			constexpr auto PortablePermissions = std::filesystem::perms::owner_all | std::filesystem::perms::group_all |
												 std::filesystem::perms::others_all;
			const auto SourcePermissions = std::filesystem::status(Source).permissions() & PortablePermissions;
			std::filesystem::permissions(Destination, SourcePermissions, std::filesystem::perm_options::replace);
#endif
			return {std::move(Relative), Size, Hash.Final(), std::move(Category)};
		}

		ContentEntry WriteAndHash(
			const std::filesystem::path &Destination,
			std::string Relative,
			std::string Category,
			std::span<const std::uint8_t> Bytes
		) {
			if (Bytes.size() > MaximumIndividualContentBytes)
				throw std::runtime_error("Package content exceeds its byte limit");
			std::filesystem::create_directories(Destination.parent_path());
			std::ofstream Output(Destination, std::ios::binary | std::ios::trunc);
			if (!Output) throw std::runtime_error("Could not open staged package content");
			Output.write(reinterpret_cast<const char *>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
			Output.flush();
			if (!Output) throw std::runtime_error("Could not write staged package content");
			return {std::move(Relative), Bytes.size(), HashBytes(Bytes), std::move(Category)};
		}

		std::string ReadBoundedText(const std::filesystem::path &Path, std::size_t MaximumBytes) {
			if (!std::filesystem::is_regular_file(Path) || IsReparseOrLink(Path))
				throw std::runtime_error("Required package file is missing or redirected");
			const auto Size = std::filesystem::file_size(Path);
			if (Size > MaximumBytes) throw std::runtime_error("Package document exceeds its byte limit");
			std::ifstream Input(Path, std::ios::binary);
			if (!Input) throw std::runtime_error("Could not open package document");
			std::string Result(static_cast<std::size_t>(Size), '\0');
			Input.read(Result.data(), static_cast<std::streamsize>(Result.size()));
			if (!Input && !Input.eof()) throw std::runtime_error("Could not read package document");
			return Result;
		}

		Json EncodeContent(const std::vector<ContentEntry> &Entries) {
			Json Result = Json::array();
			for (const auto &Entry : Entries)
				Result.push_back({
					{"Path", Entry.Path},
					{"Size", Entry.Size},
					{"Sha256", Entry.Sha256},
					{"Category", Entry.Category},
				});
			return Result;
		}

		std::optional<RuntimeDistributionDescription>
		ParseRuntimeDistribution(const std::filesystem::path &Root, std::vector<PackageDiagnostic> &Diagnostics) {
			try {
				const auto ManifestPath = Root / "runtime-distribution.json";
				auto Parsed = JsonCodec::Parse(
					ReadBoundedText(ManifestPath, MaximumDistributionManifestBytes),
					MaximumDistributionManifestBytes,
					"runtime distribution manifest"
				);
				if (!Parsed || !Parsed->is_object() || Parsed->size() != 6 || !Parsed->contains("Format") ||
					!(*Parsed)["Format"].is_string() ||
					(*Parsed)["Format"].get_ref<const std::string &>() != DistributionFormat ||
					!Parsed->contains("Version") || !(*Parsed)["Version"].is_number_unsigned() ||
					(*Parsed)["Version"].get<std::uint32_t>() != DistributionVersion ||
					!Parsed->contains("RuntimeCompatibility") ||
					!(*Parsed)["RuntimeCompatibility"].is_number_unsigned() ||
					(*Parsed)["RuntimeCompatibility"].get<std::uint32_t>() != RuntimeCompatibilityVersion ||
					!Parsed->contains("Platform") || !(*Parsed)["Platform"].is_string() ||
					!Parsed->contains("Player") || !(*Parsed)["Player"].is_string() || !Parsed->contains("Files") ||
					!(*Parsed)["Files"].is_array() || (*Parsed)["Files"].empty() ||
					(*Parsed)["Files"].size() > MaximumContentEntries)
					throw std::runtime_error("Runtime distribution manifest is invalid or incompatible");

				RuntimeDistributionDescription Result{
					.Platform = (*Parsed)["Platform"].get<std::string>(),
					.Player = (*Parsed)["Player"].get<std::string>(),
				};
				if (Result.Platform.empty() || Result.Platform.size() > 64 || !IsSafeRelativePath(Result.Player))
					throw std::runtime_error("Runtime distribution identity is invalid");
				std::unordered_set<std::string> Seen;
				std::string Prior;
				std::size_t RuntimeCount = 0;
				std::size_t ShaderCount = 0;
				for (const auto &File : (*Parsed)["Files"]) {
					if (!File.is_object() || File.size() != 2 || !File.contains("Path") || !File["Path"].is_string() ||
						!File.contains("Category") || !File["Category"].is_string())
						throw std::runtime_error("Runtime distribution file entry is malformed");
					auto Path = File["Path"].get<std::string>();
					auto Category = File["Category"].get<std::string>();
					if (!IsSafeRelativePath(Path) || (!Prior.empty() && Path <= Prior) ||
						!Seen.insert(LowerPath(Path)).second)
						throw std::runtime_error("Runtime distribution paths are unsafe, duplicated, or unsorted");
					if (Category != "Runtime" && Category != "Shader" && Category != "Notice")
						throw std::runtime_error("Runtime distribution category is unknown");
					if (Category == "Runtime" && ++RuntimeCount > MaximumRuntimeDependencies)
						throw std::runtime_error("Runtime distribution dependency count exceeds its bound");
					if (Category == "Shader" && ++ShaderCount > MaximumRequiredShaders)
						throw std::runtime_error("Runtime distribution shader count exceeds its bound");
					if (!ResolvePackageFile(Root, Path, true))
						throw std::runtime_error("Runtime distribution file is missing or escapes its root");
					Prior = Path;
					Result.Files.push_back({std::move(Path), std::move(Category)});
				}
				if (!Seen.contains(LowerPath(Result.Player)))
					throw std::runtime_error("Runtime distribution player is not listed");
				for (const auto Required : RequiredRuntimeFiles)
					if (!Seen.contains(LowerPath(Required)))
						throw std::runtime_error("Runtime distribution is incomplete");
				return Result;
			} catch (const std::exception &Error) {
				AddDiagnostic(
					Diagnostics,
					PackageDiagnosticSeverity::Error,
					"Runtime",
					"InvalidDistribution",
					"The selected Gargantuan runtime distribution is missing, incomplete, or incompatible.",
					Error.what()
				);
				return std::nullopt;
			}
		}

		std::optional<ParsedPackageManifest> ParseManifest(
			const std::filesystem::path &Root, std::vector<PackageDiagnostic> &Diagnostics, bool VerifyContent
		) {
			try {
				auto Parsed = JsonCodec::Parse(
					ReadBoundedText(Root / "game.package.json", MaximumManifestBytes),
					MaximumManifestBytes,
					"game package manifest"
				);
				if (!Parsed || !Parsed->is_object() || Parsed->size() != 12 || !Parsed->contains("Format") ||
					!(*Parsed)["Format"].is_string() ||
					(*Parsed)["Format"].get_ref<const std::string &>() != PackageFormat ||
					!Parsed->contains("PackageFormatVersion") ||
					!(*Parsed)["PackageFormatVersion"].is_number_unsigned() ||
					!Parsed->contains("RuntimeCompatibility") ||
					!(*Parsed)["RuntimeCompatibility"].is_number_unsigned() || !Parsed->contains("ProjectId") ||
					!(*Parsed)["ProjectId"].is_string() || !Parsed->contains("DisplayName") ||
					!(*Parsed)["DisplayName"].is_string() || !Parsed->contains("Configuration") ||
					!(*Parsed)["Configuration"].is_string() || !Parsed->contains("Revision") ||
					!(*Parsed)["Revision"].is_number_unsigned() || !Parsed->contains("UnsavedChanges") ||
					!(*Parsed)["UnsavedChanges"].is_boolean() || !Parsed->contains("Player") ||
					!(*Parsed)["Player"].is_string() || !Parsed->contains("Startup") ||
					!(*Parsed)["Startup"].is_object() || !Parsed->contains("ContentTableSha256") ||
					!(*Parsed)["ContentTableSha256"].is_string() || !Parsed->contains("Content") ||
					!(*Parsed)["Content"].is_array())
					throw std::runtime_error("Package manifest shape is invalid");
				const auto FormatVersion = (*Parsed)["PackageFormatVersion"].get<std::uint32_t>();
				if (FormatVersion != GamePackageFormatVersion)
					throw std::runtime_error("Package format version is unsupported");
				const auto Compatibility = (*Parsed)["RuntimeCompatibility"].get<std::uint32_t>();
				if (Compatibility != RuntimeCompatibilityVersion)
					throw std::runtime_error("Runtime compatibility version is unsupported");
				auto Identity = ProjectId::Parse((*Parsed)["ProjectId"].get_ref<const std::string &>());
				auto Configuration = ParsePackageConfiguration(
					(*Parsed)["Configuration"].get_ref<const std::string &>()
				);
				const auto DisplayName = (*Parsed)["DisplayName"].get<std::string>();
				const auto Player = (*Parsed)["Player"].get<std::string>();
				const auto &Startup = (*Parsed)["Startup"];
				if (!Identity || !Configuration || DisplayName.empty() ||
					DisplayName.size() > MaximumDisplayNameBytes || !IsSafeRelativePath(Player) ||
					Startup.size() != 3 || !Startup.contains("Project") || !Startup["Project"].is_string() ||
					!Startup.contains("AssetCatalog") || !Startup["AssetCatalog"].is_string() ||
					!Startup.contains("PreRun") || (!Startup["PreRun"].is_null() && !Startup["PreRun"].is_string()))
					throw std::runtime_error("Package startup metadata is invalid");

				ParsedPackageManifest Result;
				Result.Inspection = {
					.Identity = *Identity,
					.DisplayName = DisplayName,
					.Configuration = *Configuration,
					.FormatVersion = FormatVersion,
					.RuntimeCompatibility = Compatibility,
					.Revision = (*Parsed)["Revision"].get<std::uint64_t>(),
					.PlayerExecutable = Root / std::filesystem::path(Player),
				};
				if (Result.Inspection.Revision == 0) throw std::runtime_error("Package revision is invalid");
				Result.ProjectPath = Startup["Project"].get<std::string>();
				Result.AssetCatalogPath = Startup["AssetCatalog"].get<std::string>();
				if (Startup["PreRun"].is_string()) Result.PreRunPath = Startup["PreRun"].get<std::string>();
				if (!IsSafeRelativePath(Result.ProjectPath) || !IsSafeRelativePath(Result.AssetCatalogPath) ||
					(Result.PreRunPath && !IsSafeRelativePath(*Result.PreRunPath)))
					throw std::runtime_error("Package startup paths are unsafe");

				const auto &Content = (*Parsed)["Content"];
				if (Content.empty() || Content.size() > MaximumContentEntries)
					throw std::runtime_error("Package content count exceeds its bound");
				std::unordered_set<std::string> Seen;
				std::string Prior;
				std::uint64_t Aggregate = 0;
				for (const auto &Encoded : Content) {
					if (!Encoded.is_object() || Encoded.size() != 4 || !Encoded.contains("Path") ||
						!Encoded["Path"].is_string() || !Encoded.contains("Size") ||
						!Encoded["Size"].is_number_unsigned() || !Encoded.contains("Sha256") ||
						!Encoded["Sha256"].is_string() || !Encoded.contains("Category") ||
						!Encoded["Category"].is_string())
						throw std::runtime_error("Package content record is malformed");
					ContentEntry Entry{
						Encoded["Path"].get<std::string>(),
						Encoded["Size"].get<std::uint64_t>(),
						Encoded["Sha256"].get<std::string>(),
						Encoded["Category"].get<std::string>(),
					};
					if (!IsSafeRelativePath(Entry.Path) || (!Prior.empty() && Entry.Path <= Prior) ||
						!Seen.insert(LowerPath(Entry.Path)).second || !AssetContentId::Parse(Entry.Sha256) ||
						Entry.Size > MaximumIndividualContentBytes ||
						(Entry.Category != "Runtime" && Entry.Category != "Project" && Entry.Category != "Asset" &&
						 Entry.Category != "Shader" && Entry.Category != "Notice"))
						throw std::runtime_error("Package content record identity or bounds are invalid");
					if (Aggregate > MaximumAggregateContentBytes - Entry.Size)
						throw std::runtime_error("Package aggregate content size exceeds its bound");
					Aggregate += Entry.Size;
					Prior = Entry.Path;
					Result.Content.push_back(std::move(Entry));
				}
				Result.ContentTableSha256 = (*Parsed)["ContentTableSha256"].get<std::string>();
				if (!AssetContentId::Parse(Result.ContentTableSha256) ||
					HashText(EncodeContent(Result.Content).dump()) != Result.ContentTableSha256)
					throw std::runtime_error("Package content table integrity identity is invalid");
				for (const auto &Required : {Player, Result.ProjectPath, Result.AssetCatalogPath})
					if (!Seen.contains(LowerPath(Required)))
						throw std::runtime_error("Package required entry is absent");
				if (Result.PreRunPath && !Seen.contains(LowerPath(*Result.PreRunPath)))
					throw std::runtime_error("Package PreRun entry is absent");
				for (const auto Required : RequiredRuntimeFiles)
					if (!Seen.contains(LowerPath(Required)))
						throw std::runtime_error("Package runtime resource is absent");
				Result.Inspection.ContentCount = Result.Content.size();
				Result.Inspection.ContentBytes = Aggregate;

				if (VerifyContent)
					for (const auto &Entry : Result.Content) {
						auto Path = ResolvePackageFile(Root, Entry.Path, true);
						if (!Path || std::filesystem::file_size(*Path) != Entry.Size ||
							HashFile(*Path, Entry.Size) != Entry.Sha256)
							throw std::runtime_error("Package content is missing or has an integrity mismatch");
					}
				if (VerifyContent) {
					std::unordered_set<std::string> AllowedFiles{LowerPath("game.package.json")};
					std::unordered_set<std::string> AllowedDirectories;
					for (const auto &Entry : Result.Content) {
						AllowedFiles.insert(LowerPath(Entry.Path));
						auto Parent = std::filesystem::path(Entry.Path).parent_path();
						while (!Parent.empty()) {
							AllowedDirectories.insert(LowerPath(Parent.generic_string()));
							Parent = Parent.parent_path();
						}
					}
					std::size_t SeenEntries = 0;
					for (const auto &Entry : std::filesystem::recursive_directory_iterator(Root)) {
						if (++SeenEntries > MaximumContentEntries + AllowedDirectories.size() + 1 ||
							IsReparseOrLink(Entry.path()))
							throw std::runtime_error(
								"Package directory closure exceeds its bounds or contains redirection"
							);
						const auto Relative = LowerPath(Entry.path().lexically_relative(Root).generic_string());
						if ((Entry.is_regular_file() && !AllowedFiles.contains(Relative)) ||
							(Entry.is_directory() && !AllowedDirectories.contains(Relative)) ||
							(!Entry.is_regular_file() && !Entry.is_directory()))
							throw std::runtime_error("Package directory contains undeclared content");
					}
				}
				return Result;
			} catch (const std::exception &) {
				AddDiagnostic(
					Diagnostics,
					PackageDiagnosticSeverity::Error,
					"Integrity",
					"InvalidPackage",
					"The game package manifest or one of its required content files is invalid."
				);
				return std::nullopt;
			}
		}

		void ValidateRuntimeAssetCatalog(
			const std::filesystem::path &Root,
			const ParsedPackageManifest &Manifest,
			std::vector<PackageDiagnostic> &Diagnostics
		) {
			try {
				auto CatalogPath = ResolvePackageFile(Root, Manifest.AssetCatalogPath, true);
				if (!CatalogPath) throw std::runtime_error("Runtime asset catalog path is invalid");
				auto Parsed = JsonCodec::Parse(
					ReadBoundedText(*CatalogPath, 1024 * 1024), 1024 * 1024, "runtime asset catalog"
				);
				if (!Parsed || !Parsed->is_object() || Parsed->size() != 3 || !Parsed->contains("Format") ||
					(*Parsed)["Format"] != "GargantuanRuntimeAssets" || !Parsed->contains("Version") ||
					!(*Parsed)["Version"].is_number_unsigned() || (*Parsed)["Version"].get<std::uint32_t>() != 1 ||
					!Parsed->contains("Assets") || !(*Parsed)["Assets"].is_array() ||
					(*Parsed)["Assets"].size() > AssetLimits::MaximumCatalogRecords)
					throw std::runtime_error("Runtime asset catalog is invalid");
				std::unordered_map<std::string, std::string> ContentHashes;
				for (const auto &Entry : Manifest.Content)
					ContentHashes.emplace(LowerPath(Entry.Path), Entry.Sha256);
				std::unordered_set<std::string> AssetIdentities;
				std::vector<std::string> ReferencedDependencies;
				for (const auto &Asset : (*Parsed)["Assets"]) {
					if (!Asset.is_object() || Asset.size() != 8 || !Asset.contains("AssetId") ||
						!Asset["AssetId"].is_string() || !Asset.contains("Reference") ||
						!Asset["Reference"].is_string() || !Asset.contains("Kind") || !Asset["Kind"].is_string() ||
						!Asset.contains("Name") || !Asset["Name"].is_string() || !Asset.contains("ContentId") ||
						!Asset["ContentId"].is_string() || !Asset.contains("ContentRevision") ||
						!Asset["ContentRevision"].is_number_unsigned() || !Asset.contains("State") ||
						!Asset["State"].is_string() || !Asset.contains("Dependencies") ||
						!Asset["Dependencies"].is_array())
						throw std::runtime_error("Runtime asset catalog record is malformed");
					auto Identity = AssetId::Parse(Asset["AssetId"].get_ref<const std::string &>());
					auto Reference = AssetReference::Parse(Asset["Reference"].get_ref<const std::string &>());
					auto Kind = ParseAssetKind(Asset["Kind"].get_ref<const std::string &>());
					const auto &Name = Asset["Name"].get_ref<const std::string &>();
					const auto &State = Asset["State"].get_ref<const std::string &>();
					if (!Identity || !Reference || !Reference->ProjectAsset || *Reference->ProjectAsset != *Identity ||
						!Kind || Name.empty() || Name.size() > AssetLimits::MaximumNameBytes ||
						Asset["ContentRevision"].get<std::uint64_t>() == 0 || (State != "Ready" && State != "Stale") ||
						Asset["Dependencies"].size() > AssetLimits::MaximumDependencies ||
						!AssetIdentities.insert(Asset["AssetId"].get<std::string>()).second)
						throw std::runtime_error("Runtime asset catalog identity is invalid");
					std::unordered_set<std::string> Dependencies;
					for (const auto &Dependency : Asset["Dependencies"])
						if (!Dependency.is_string() || !AssetId::Parse(Dependency.get_ref<const std::string &>()) ||
							!Dependencies.insert(Dependency.get<std::string>()).second)
							throw std::runtime_error("Runtime asset dependency is invalid");
						else
							ReferencedDependencies.push_back(Dependency.get<std::string>());
					const auto ContentId = Asset["ContentId"].get<std::string>();
					const auto Content = ContentHashes.find(
						LowerPath("content/assets/artifacts/" + ContentId + ".gasset")
					);
					if (!AssetContentId::Parse(ContentId) || Content == ContentHashes.end() ||
						Content->second != ContentId)
						throw std::runtime_error("Runtime asset artifact is absent");
					if (State == "Stale")
						AddDiagnostic(
							Diagnostics,
							PackageDiagnosticSeverity::Warning,
							"Assets",
							"StaleAsset",
							"The package uses a last-known-good canonical asset artifact.",
							Asset.contains("Reference") && Asset["Reference"].is_string()
								? Asset["Reference"].get<std::string>()
								: ""
						);
				}
				for (const auto &Dependency : ReferencedDependencies)
					if (!AssetIdentities.contains(Dependency))
						throw std::runtime_error("Runtime asset dependency is absent");
			} catch (const std::exception &) {
				AddDiagnostic(
					Diagnostics,
					PackageDiagnosticSeverity::Error,
					"Assets",
					"InvalidRuntimeCatalog",
					"The packaged runtime asset catalog is malformed or references missing canonical content."
				);
			}
		}

		bool IsRecognizedPackageDirectory(const std::filesystem::path &Path) {
			if (!std::filesystem::is_directory(Path)) return false;
			return !HasErrors(PackageBuilder::Validate(Path));
		}

		std::filesystem::path UniqueSibling(const std::filesystem::path &Destination, std::string_view Kind) {
			static std::atomic_uint64_t Counter = 1;
			const auto Suffix = std::format(
				".gpk-{}-{}-{}",
				Kind,
				std::chrono::steady_clock::now().time_since_epoch().count(),
				Counter.fetch_add(1, std::memory_order_relaxed)
			);
			return Destination.parent_path() / Suffix;
		}

		void ValidateStagingPaths(
			const std::filesystem::path &Destination,
			const std::filesystem::path &Candidate,
			const RuntimeDistributionDescription &Distribution,
			const GamePayload &Payload
		) {
#if defined(_WIN32)
			constexpr std::size_t MaximumSupportedStagingPath = 240;
			auto Check = [&](std::string_view Relative) {
				if ((Destination / std::filesystem::path(Relative)).native().size() > MaximumSupportedStagingPath ||
					(Candidate / std::filesystem::path(Relative)).native().size() > MaximumSupportedStagingPath)
					throw std::runtime_error("Package output path is too long for this Windows runtime");
			};
			Check("game.package.json");
			Check("content/game.instance.json");
			Check("content/assets/catalog.json");
			if (Payload.PreRunSource) Check("content/prerun.luau");
			for (const auto &File : Distribution.Files)
				Check(File.Path);
			for (const auto &Artifact : Payload.Assets.Artifacts)
				Check("content/" + Artifact.RelativePath);
#else
			(void)Destination;
			(void)Candidate;
			(void)Distribution;
			(void)Payload;
#endif
		}

		void InjectFailure(PackageFailurePoint Requested, PackageFailurePoint Current) {
			if (Requested == Current) throw std::runtime_error("Injected package build failure");
		}

		void AddSize(PackageSizeBreakdown &Size, std::string_view Category, std::uint64_t Bytes) {
			if (Category == "Runtime")
				Size.RuntimeBytes += Bytes;
			else if (Category == "Project")
				Size.ProjectBytes += Bytes;
			else if (Category == "Asset")
				Size.AssetBytes += Bytes;
			else if (Category == "Shader")
				Size.ShaderBytes += Bytes;
			else
				Size.OtherBytes += Bytes;
		}
	}

	std::string_view GetPackageConfigurationName(PackageConfiguration Configuration) {
		switch (Configuration) {
		case PackageConfiguration::Development:
			return "Development";
		case PackageConfiguration::Release:
			return "Release";
		}
		return "Unknown";
	}

	std::optional<PackageConfiguration> ParsePackageConfiguration(std::string_view Value) {
		if (Value == "Development") return PackageConfiguration::Development;
		if (Value == "Release") return PackageConfiguration::Release;
		return std::nullopt;
	}

	std::string_view GetPackagePhaseName(PackagePhase Phase) {
		switch (Phase) {
		case PackagePhase::Snapshot:
			return "Snapshot";
		case PackagePhase::Validate:
			return "Validate";
		case PackagePhase::StageRuntime:
			return "Stage runtime";
		case PackagePhase::StageContent:
			return "Stage content";
		case PackagePhase::HashManifest:
			return "Hash/manifest";
		case PackagePhase::Finalize:
			return "Finalize";
		case PackagePhase::Complete:
			return "Complete";
		}
		return "Unknown";
	}

	std::filesystem::path GetPackageUserDataRoot(ProjectId Identity) {
		if (!Identity.IsValid()) throw std::invalid_argument("UserDataRoot requires a valid ProjectId");
		char *Raw = SDL_GetPrefPath("Gargantuan", Identity.ToString().c_str());
		if (!Raw) throw std::runtime_error("Could not resolve the Gargantuan user-data root");
		std::filesystem::path Result(Raw);
		SDL_free(Raw);
		return Result;
	}

	GamePayload PackageBuilder::Capture(
		const Project &ProjectValue,
		const std::shared_ptr<DataModel> &World,
		std::uint64_t AuthoritativeRevision,
		std::uint64_t PersistedRevision
	) {
		if (!World || !ProjectValue.Identity.IsValid() || AuthoritativeRevision == 0 || PersistedRevision == 0)
			throw std::invalid_argument("Package snapshot source is invalid");
		auto Root = std::static_pointer_cast<Instance>(World);
		auto ProjectJson = InstanceSerialization::Serialize(ProjectValue.InstanceFileFormat, Root);
		if (ProjectJson.size() > MaximumProjectSnapshotBytes)
			throw std::length_error("Project snapshot exceeds the package byte limit");
		auto Assets = std::dynamic_pointer_cast<AssetService>(World->FindFirstChildOfClass("AssetService", false));
		if (!Assets) throw std::runtime_error("Project has no canonical AssetService");
		GamePayload Result{
			.Identity = ProjectValue.Identity,
			.DisplayName = World->GetName(),
			.AuthoritativeRevision = AuthoritativeRevision,
			.UnsavedChanges = AuthoritativeRevision != PersistedRevision,
			.ProjectJson = std::move(ProjectJson),
			.PreRunSource = ReadProjectPreRunSource(ProjectValue.Root),
			.Assets = Assets->CaptureRuntimeAssets(),
		};
		if (Result.DisplayName.empty() || Result.DisplayName.size() > MaximumDisplayNameBytes)
			throw std::runtime_error("Project display name exceeds the package bound");
		return Result;
	}

	PackageBuildResult PackageBuilder::Build(const PackageBuildRequest &Request) {
		PackageBuildResult Result{
			.Identity = Request.Payload.Identity,
			.PackagedRevision = Request.Payload.AuthoritativeRevision,
			.PackagedUnsavedChanges = Request.Payload.UnsavedChanges,
		};
		std::filesystem::path Candidate;
		auto CurrentPhase = PackagePhase::Snapshot;
		try {
			auto RunPhase = [&](PackagePhase Phase, auto &&Work) {
				CurrentPhase = Phase;
				if (Request.Cancellation.IsCancelled()) throw std::runtime_error("Package build cancelled");
				if (Request.Progress) Request.Progress(Phase);
				const auto Started = std::chrono::steady_clock::now();
				Work();
				Result.Timings.push_back(
					{Phase,
					 std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - Started)}
				);
			};

			RunPhase(PackagePhase::Snapshot, [&] {
				InjectFailure(Request.FailurePoint, PackageFailurePoint::Snapshot);
				if (!Request.Payload.Identity.IsValid() || Request.Payload.AuthoritativeRevision == 0 ||
					Request.Payload.DisplayName.empty() ||
					Request.Payload.DisplayName.size() > MaximumDisplayNameBytes ||
					Request.Payload.ProjectJson.empty() ||
					Request.Payload.ProjectJson.size() > MaximumProjectSnapshotBytes ||
					Request.Payload.Assets.CatalogJson.size() > 1024 * 1024)
					throw std::runtime_error("Package payload snapshot is invalid or exceeds its bounds");
			});

			std::optional<RuntimeDistributionDescription> Distribution;
			std::filesystem::path Destination;
			RunPhase(PackagePhase::Validate, [&] {
				InjectFailure(Request.FailurePoint, PackageFailurePoint::Validate);
				if (!Request.RuntimeDistributionRoot.is_absolute() || !Request.OutputDirectory.is_absolute())
					throw std::runtime_error("Package roots must be absolute tooling inputs");
				Destination = Request.OutputDirectory.lexically_normal();
				if (Destination == Destination.root_path() || Destination.filename().empty())
					throw std::runtime_error("Package output destination is unsafe");
				const auto Parent = Destination.parent_path();
				if (!std::filesystem::is_directory(Parent) || IsReparseOrLink(Parent))
					throw std::runtime_error("Package output parent is unavailable or redirected");
				if (std::filesystem::exists(Destination)) {
					if (!std::filesystem::is_directory(Destination))
						throw std::runtime_error("Package output destination is not a directory");
					if (std::filesystem::directory_iterator(Destination) != std::filesystem::directory_iterator{} &&
						!IsRecognizedPackageDirectory(Destination))
						throw std::runtime_error("Package output contains unrelated files");
				}
				Distribution = ParseRuntimeDistribution(Request.RuntimeDistributionRoot, Result.Diagnostics);
				if (!Distribution) throw std::runtime_error("Runtime distribution validation failed");
				Candidate = UniqueSibling(Destination, "gargantuan-package-tmp");
				ValidateStagingPaths(Destination, Candidate, *Distribution, Request.Payload);
				std::filesystem::create_directory(Candidate);
			});

			std::vector<ContentEntry> Entries;
			RunPhase(PackagePhase::StageRuntime, [&] {
				InjectFailure(Request.FailurePoint, PackageFailurePoint::StageRuntime);
				for (const auto &File : Distribution->Files) {
					auto Source = ResolvePackageFile(Request.RuntimeDistributionRoot, File.Path, true);
					auto Target = ResolvePackageFile(Candidate, File.Path, false);
					if (!Source || !Target) throw std::runtime_error("Runtime distribution path is unsafe");
					Entries.push_back(CopyAndHash(*Source, *Target, File.Path, File.Category, Request.Cancellation));
				}
			});

			RunPhase(PackagePhase::StageContent, [&] {
				InjectFailure(Request.FailurePoint, PackageFailurePoint::StageContent);
				auto AddText = [&](std::string Relative, std::string Category, std::string_view Text) {
					auto Target = ResolvePackageFile(Candidate, Relative, false);
					if (!Target) throw std::runtime_error("Generated package path is unsafe");
					Entries.push_back(WriteAndHash(
						*Target,
						std::move(Relative),
						std::move(Category),
						std::span(reinterpret_cast<const std::uint8_t *>(Text.data()), Text.size())
					));
				};
				AddText("content/game.instance.json", "Project", Request.Payload.ProjectJson);
				AddText("content/assets/catalog.json", "Asset", Request.Payload.Assets.CatalogJson);
				if (Request.Payload.PreRunSource)
					AddText("content/prerun.luau", "Project", *Request.Payload.PreRunSource);
				for (const auto &Artifact : Request.Payload.Assets.Artifacts) {
					if (!Artifact.Bytes || !IsSafeRelativePath(Artifact.RelativePath) ||
						!std::string_view(Artifact.RelativePath).starts_with("assets/artifacts/"))
						throw std::runtime_error("Runtime asset snapshot contains an invalid artifact");
					const auto Relative = "content/" + Artifact.RelativePath;
					auto Target = ResolvePackageFile(Candidate, Relative, false);
					if (!Target) throw std::runtime_error("Runtime asset package path is unsafe");
					Entries.push_back(WriteAndHash(*Target, Relative, "Asset", *Artifact.Bytes));
				}
			});

			RunPhase(PackagePhase::HashManifest, [&] {
				InjectFailure(Request.FailurePoint, PackageFailurePoint::HashManifest);
				std::ranges::sort(Entries, {}, &ContentEntry::Path);
				for (std::size_t Index = 1; Index < Entries.size(); ++Index)
					if (LowerPath(Entries[Index - 1].Path) == LowerPath(Entries[Index].Path))
						throw std::runtime_error("Generated package contains a case-colliding path");
				std::uint64_t Aggregate = 0;
				for (const auto &Entry : Entries) {
					if (Aggregate > MaximumAggregateContentBytes - Entry.Size)
						throw std::runtime_error("Generated package exceeds its aggregate byte limit");
					Aggregate += Entry.Size;
					AddSize(Result.Size, Entry.Category, Entry.Size);
				}
				const auto Content = EncodeContent(Entries);
				const auto ContentHash = HashText(Content.dump());
				Json Startup{
					{"Project", "content/game.instance.json"},
					{"AssetCatalog", "content/assets/catalog.json"},
					{"PreRun", Request.Payload.PreRunSource ? Json("content/prerun.luau") : Json(nullptr)},
				};
				Json Manifest{
					{"Format", PackageFormat},
					{"PackageFormatVersion", GamePackageFormatVersion},
					{"RuntimeCompatibility", RuntimeCompatibilityVersion},
					{"ProjectId", Request.Payload.Identity.ToString()},
					{"DisplayName", Request.Payload.DisplayName},
					{"Configuration", GetPackageConfigurationName(Request.Configuration)},
					{"Revision", Request.Payload.AuthoritativeRevision},
					{"UnsavedChanges", Request.Payload.UnsavedChanges},
					{"Player", Distribution->Player},
					{"Startup", std::move(Startup)},
					{"ContentTableSha256", ContentHash},
					{"Content", Content},
				};
				auto Encoded = JsonCodec::Encode(Manifest, "game package manifest");
				if (!Encoded || Encoded->size() > MaximumManifestBytes)
					throw std::runtime_error("Generated package manifest exceeds its bound");
				InjectFailure(Request.FailurePoint, PackageFailurePoint::ManifestWrite);
				std::ofstream Output(Candidate / "game.package.json", std::ios::binary | std::ios::trunc);
				if (!Output) throw std::runtime_error("Could not create the package manifest");
				Output.write(Encoded->data(), static_cast<std::streamsize>(Encoded->size()));
				Output.flush();
				if (!Output) throw std::runtime_error("Could not finish the package manifest");
				Result.Size.OtherBytes += Encoded->size();
				Result.Size.TotalBytes = Aggregate + Encoded->size();
				auto CandidateDiagnostics = Validate(Candidate);
				Result.Diagnostics.insert(
					Result.Diagnostics.end(), CandidateDiagnostics.begin(), CandidateDiagnostics.end()
				);
				if (HasErrors(CandidateDiagnostics)) throw std::runtime_error("Generated package failed validation");
			});

			RunPhase(PackagePhase::Finalize, [&] {
				InjectFailure(Request.FailurePoint, PackageFailurePoint::Finalize);
				if (Request.Cancellation.IsCancelled()) throw std::runtime_error("Package build cancelled");
				std::filesystem::path Backup;
				if (std::filesystem::exists(Destination)) {
					Backup = UniqueSibling(Destination, "gargantuan-package-backup");
					std::filesystem::rename(Destination, Backup);
				}
				try {
					std::filesystem::rename(Candidate, Destination);
					Candidate.clear();
				} catch (...) {
					if (!Backup.empty() && !std::filesystem::exists(Destination))
						std::filesystem::rename(Backup, Destination);
					throw;
				}
				if (!Backup.empty()) std::filesystem::remove_all(Backup);
				Result.OutputDirectory = Destination;
				Result.PlayerExecutable = Destination / std::filesystem::path(Distribution->Player);
			});

			if (Request.Progress) Request.Progress(PackagePhase::Complete);
			Result.Ok = true;
			AddDiagnostic(
				Result.Diagnostics,
				PackageDiagnosticSeverity::Info,
				"Packaging",
				"Complete",
				"The standalone game package was built and validated."
			);
		} catch (const std::exception &Error) {
			Result.Cancelled = Request.Cancellation.IsCancelled() ||
							   std::string_view(Error.what()) == "Package build cancelled";
			std::string FailureDetail = std::string(GetPackagePhaseName(CurrentPhase)) + ": ";
			if (const auto *FilesystemError = dynamic_cast<const std::filesystem::filesystem_error *>(&Error))
				FailureDetail += FilesystemError->code().message();
			else
				FailureDetail += Error.what();
			AddDiagnostic(
				Result.Diagnostics,
				PackageDiagnosticSeverity::Error,
				"Packaging",
				Result.Cancelled ? "Cancelled" : "BuildFailed",
				Result.Cancelled ? "The package build was cancelled before finalization."
								 : "The package build failed; the previous valid destination was preserved.",
				std::move(FailureDetail)
			);
			if (!Candidate.empty()) {
				std::error_code Ignored;
				std::filesystem::remove_all(Candidate, Ignored);
			}
		}
		return Result;
	}

	std::vector<PackageDiagnostic> PackageBuilder::Validate(const std::filesystem::path &PackageRoot) {
		std::vector<PackageDiagnostic> Diagnostics;
		if (!PackageRoot.is_absolute() || !std::filesystem::is_directory(PackageRoot) || IsReparseOrLink(PackageRoot)) {
			AddDiagnostic(
				Diagnostics,
				PackageDiagnosticSeverity::Error,
				"Filesystem",
				"InvalidRoot",
				"The package root must be an absolute local directory without redirection."
			);
			return Diagnostics;
		}
		auto Manifest = ParseManifest(PackageRoot, Diagnostics, true);
		if (!Manifest) return Diagnostics;
		try {
			auto ProjectPath = ResolvePackageFile(PackageRoot, Manifest->ProjectPath, true);
			if (!ProjectPath) throw std::runtime_error("Project payload path is invalid");
			auto ProjectText = ReadBoundedText(*ProjectPath, MaximumProjectSnapshotBytes);
			auto ProjectJson = JsonCodec::Parse(ProjectText, MaximumProjectSnapshotBytes, "packaged project snapshot");
			if (!ProjectJson || !ProjectJson->is_object() || !ProjectJson->contains("Version") ||
				!(*ProjectJson)["Version"].is_number_unsigned() || !ProjectJson->contains("ClassName") ||
				!(*ProjectJson)["ClassName"].is_string() || (*ProjectJson)["ClassName"] != "DataModel")
				throw std::runtime_error("Packaged project snapshot is malformed");
		} catch (const std::exception &) {
			AddDiagnostic(
				Diagnostics,
				PackageDiagnosticSeverity::Error,
				"Validation",
				"InvalidProject",
				"The packaged project snapshot is malformed or exceeds its bounds."
			);
		}
		ValidateRuntimeAssetCatalog(PackageRoot, *Manifest, Diagnostics);
		if (!HasErrors(Diagnostics))
			AddDiagnostic(
				Diagnostics,
				PackageDiagnosticSeverity::Info,
				"Validation",
				"Valid",
				"The standalone package manifest, content hashes, project payload, assets, and runtime resources are "
				"valid."
			);
		return Diagnostics;
	}

	std::optional<PackageInspection>
	PackageBuilder::Inspect(const std::filesystem::path &PackageRoot, std::vector<PackageDiagnostic> &Diagnostics) {
		auto Manifest = ParseManifest(PackageRoot, Diagnostics, false);
		if (!Manifest) return std::nullopt;
		return Manifest->Inspection;
	}

	std::optional<RuntimePackagePayload>
	PackageBuilder::Load(const std::filesystem::path &PackageRoot, std::vector<PackageDiagnostic> &Diagnostics) {
		Diagnostics = Validate(PackageRoot);
		if (HasErrors(Diagnostics)) return std::nullopt;
		auto Manifest = ParseManifest(PackageRoot, Diagnostics, false);
		if (!Manifest) return std::nullopt;
		try {
			RuntimePackagePayload Result{.Inspection = Manifest->Inspection};
			Result.ProjectJson = ReadBoundedText(
				*ResolvePackageFile(PackageRoot, Manifest->ProjectPath, true), MaximumProjectSnapshotBytes
			);
			Result.Assets.CatalogJson = ReadBoundedText(
				*ResolvePackageFile(PackageRoot, Manifest->AssetCatalogPath, true), 1024 * 1024
			);
			if (Manifest->PreRunPath)
				Result.PreRunSource = ReadBoundedText(
					*ResolvePackageFile(PackageRoot, *Manifest->PreRunPath, true), MaximumPreRunSourceBytes
				);
			auto Catalog = JsonCodec::Parse(Result.Assets.CatalogJson, 1024 * 1024, "runtime asset catalog");
			if (!Catalog || !Catalog->contains("Assets") || !(*Catalog)["Assets"].is_array())
				throw std::runtime_error("Runtime asset catalog is malformed");
			std::set<std::string> ContentIds;
			for (const auto &Asset : (*Catalog)["Assets"])
				ContentIds.insert(Asset["ContentId"].get<std::string>());
			for (const auto &ContentId : ContentIds) {
				const auto Relative = "content/assets/artifacts/" + ContentId + ".gasset";
				auto Path = ResolvePackageFile(PackageRoot, Relative, true);
				if (!Path) throw std::runtime_error("Runtime asset artifact path is invalid");
				const auto Size = std::filesystem::file_size(*Path);
				if (Size > AssetLimits::MaximumArtifactBytes)
					throw std::runtime_error("Runtime asset artifact exceeds its bound");
				std::ifstream Input(*Path, std::ios::binary);
				auto Bytes = std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(Size));
				Input.read(reinterpret_cast<char *>(Bytes->data()), static_cast<std::streamsize>(Bytes->size()));
				if (!Input && !Input.eof()) throw std::runtime_error("Runtime asset artifact could not be read");
				Result.Assets.Artifacts.push_back({"assets/artifacts/" + ContentId + ".gasset", std::move(Bytes)});
			}
			return Result;
		} catch (const std::exception &) {
			AddDiagnostic(
				Diagnostics,
				PackageDiagnosticSeverity::Error,
				"Validation",
				"LoadFailed",
				"The validated package payload could not be loaded safely."
			);
			return std::nullopt;
		}
	}

	std::shared_ptr<DataModel>
	PackageBuilder::LoadWorld(const RuntimePackagePayload &Payload, const std::filesystem::path &PackageRoot) {
		if (!PackageRoot.is_absolute() || !std::filesystem::is_directory(PackageRoot) || IsReparseOrLink(PackageRoot))
			throw std::invalid_argument("Packaged runtime root is invalid");
		std::stringstream Input(Payload.ProjectJson);
		auto Parsed = InstanceSerialization::Deserialize(InstanceSerialization::InstanceFormat::Json, Input);
		if (!Parsed.Ok || !Parsed.Instance || !Parsed.Instance->IsA("DataModel"))
			throw std::runtime_error("The packaged project snapshot could not be loaded");
		auto World = std::dynamic_pointer_cast<DataModel>(Parsed.Instance);
		if (!World) throw std::runtime_error("The packaged project root is incompatible");
		World->MarkPersistenceSubtreeArchivable();
		World->Root = PackageRoot / "content";
		World->Filesystem = nullptr;
		auto Assets = std::dynamic_pointer_cast<AssetService>(World->GetService("AssetService"));
		if (!Assets) throw std::runtime_error("The packaged project has no canonical AssetService");
		Assets->LoadRuntimeAssetSnapshot(Payload.Assets);
		return World;
	}
}
