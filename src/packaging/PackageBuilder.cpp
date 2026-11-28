#include "gargantuan/packaging/PackageBuilder.hpp"

#include "ClientPackage.hpp"
#include "RuntimeDistributionCode.hpp"
#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/ModuleScript.hpp"
#include "gargantuan/classes/PlayerGui.hpp"
#include "gargantuan/classes/ScreenGui.hpp"
#include "gargantuan/classes/Script.hpp"
#include "gargantuan/content/ResidencySpatialMetadata.hpp"
#include "gargantuan/filesystem/Project.hpp"
#include "gargantuan/filesystem/Paths.hpp"
#include "gargantuan/filesystem/DiskFilesystem.hpp"
#include "gargantuan/gui/PlayerGuiScope.hpp"
#include "gargantuan/reflection/PreRunRegistration.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"
#include "gargantuan/runtime/ServicePolicy.hpp"
#include "gargantuan/services/AssetService.hpp"
#include "gargantuan/scripting/RuntimeScriptCode.hpp"
#include "assets/AssetImporter.hpp"
#include "assets/PreparedInstanceSerialization.hpp"
#include "serialization/JsonCodec.hpp"
#include "ApplicationTemplate.hpp"
#include "PrivateServerSnapshot.hpp"
#include "terrain/ScenePersistence.hpp"
#include "terrain/TerrainProfile.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <format>
#include <functional>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#include <Windows.h>
#endif

#if defined(GetClassName)
#undef GetClassName
#endif
#if defined(min)
#undef min
#endif
#if defined(max)
#undef max
#endif

#include "WindowsPackagePublication.hpp"

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
			"runtime/DefaultInteractionRuntime.luau",
			"runtime/DefaultCharacterRuntime.luau",
			"runtime/DefaultLocomotion.luau",
			"runtime/DefaultCamera.luau",
			"runtime/DefaultPlayerRuntime.luau",
			"runtime/GargantuanSans.ttf",
			"shaders/gui.frag.spv",
			"shaders/gui.vert.spv",
			"shaders/opaque.frag.spv",
			"shaders/opaque.vert.spv",
			"shaders/shadow.frag.spv",
			"shaders/shadow.vert.spv",
			"shaders/sky.frag.spv",
			"shaders/sky.vert.spv",
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

		bool IsPlayerExecutable(std::string_view Path) {
			return Path == "GargantuanPlayer" || Path == "GargantuanPlayer.exe";
		}

		bool IsServerExecutable(std::string_view Path) {
			return Path == "GargantuanServer" || Path == "GargantuanServer.exe";
		}

		bool IsDistributedEngineFile(std::string_view Path, std::string_view Category) {
			constexpr std::array RuntimeResources{
				"runtime/DefaultActionMap.luau", "runtime/DefaultInteractionRuntime.luau",
				"runtime/DefaultCamera.luau", "runtime/DefaultCharacterRuntime.luau",
				"runtime/DefaultCharacterAssembly.luau", "runtime/DefaultCharacterAnimation.luau",
				"runtime/DefaultLocomotion.luau", "runtime/DefaultNetworkLocomotion.luau",
				"runtime/DefaultNetworkCharacterRuntime.luau", "runtime/DefaultPlayerRuntime.luau",
				"runtime/DefaultNetworkClientRuntime.luau", "runtime/DefaultNetworkServerRuntime.luau",
				"runtime/GargantuanSans.ttf",
			};
			constexpr std::array Shaders{
				"shaders/gui.frag.spv", "shaders/gui.vert.spv", "shaders/ibl_brdf.frag.spv",
				"shaders/ibl_convolve.frag.spv", "shaders/ibl_fullscreen.vert.spv", "shaders/ibl_source.frag.spv",
				"shaders/opaque-instanced.vert.spv", "shaders/opaque-portable.frag.spv", "shaders/opaque.frag.spv",
				"shaders/opaque.vert.spv", "shaders/scene-resolve.frag.spv", "shaders/scene-resolve.vert.spv",
				"shaders/shadow-instanced.vert.spv", "shaders/shadow-opaque.frag.spv", "shaders/shadow.frag.spv",
				"shaders/shadow.vert.spv", "shaders/sky.frag.spv", "shaders/sky.vert.spv",
			};
			constexpr std::array Notices{
				"notices/Gargantuan.txt", "notices/argparse.txt", "notices/Box3D.txt", "notices/Manifold.txt",
				"notices/Manifold-Authors.txt", "notices/GLM.txt", "notices/nlohmann-json.txt", "notices/Luau.txt",
				"notices/magic-enum.txt", "notices/SDL3.txt", "notices/SDL3_image.txt", "notices/SDL3_ttf.txt",
				"notices/Tracy.txt", "notices/GameNetworkingSockets.txt", "notices/ProtocolBuffers.txt",
				"notices/MSVC-Runtime.txt", "notices/openssl.txt", "notices/MC33.txt",
			};
			constexpr std::array Libraries{
				"SDL3.dll", "libprotobuf.dll", "libprotobufd.dll", "concrt140.dll", "msvcp140.dll",
				"msvcp140_1.dll", "msvcp140_2.dll", "msvcp140_atomic_wait.dll", "msvcp140_codecvt_ids.dll",
				"vccorlib140.dll", "vcruntime140.dll", "vcruntime140_1.dll", "vcruntime140_threads.dll",
				"libSDL3.so", "libSDL3.dylib", "libprotobuf.so", "libprotobuf.dylib", "libprotobuf.a",
				"libcrypto-3-x64.dll", "libcrypto-3.dll", "libcrypto-3-arm64.dll", "libcrypto.so.3", "libcrypto.3.dylib",
			};
			auto Contains = [Path](const auto &Names) { return std::ranges::find(Names, Path) != Names.end(); };
			if (Category == "Shader") return Contains(Shaders);
			if (Category == "Notice") return Contains(Notices);
			if (Category != "Runtime") return false;
			if (IsPlayerExecutable(Path) || Contains(RuntimeResources) || Contains(Libraries)) return true;
			const auto Versioned = [Path](std::string_view Prefix, std::string_view Suffix) {
				if (!Path.starts_with(Prefix) || !Path.ends_with(Suffix) ||
					Path.size() <= Prefix.size() + Suffix.size()) return false;
				const auto Version = Path.substr(Prefix.size(), Path.size() - Prefix.size() - Suffix.size());
				return Version.front() >= '0' && Version.front() <= '9' &&
					std::ranges::all_of(Version, [](char Digit) { return (Digit >= '0' && Digit <= '9') || Digit == '.'; });
			};
			return Versioned("libSDL3.so.", "") || Versioned("libSDL3.", ".dylib") ||
				Versioned("libprotobuf.so.", "") || Versioned("libprotobuf.", ".dylib");
		}

		struct ParsedPackageManifest {
			PackageInspection Inspection;
			std::string ProjectPath;
			bool HasApplication = false;
			std::string AssetCatalogPath;
			std::optional<std::string> PreRunPath;
			std::optional<std::string> ContentManifestPath;
			AssetContentId ContentManifestDigest;
			std::string ContentTableSha256;
			std::vector<ContentEntry> Content;
			std::optional<runtime_distribution::Metadata> RuntimeDistribution;
			bool TrustedCompiler = false;
			RuntimePackageProvisionedSecurity DistributionSecurity;
			std::optional<RuntimeDistributionCodeCatalog> ScriptCatalog;
		};

		struct SerializedSubtreeInspection final {
			std::size_t Objects = 0;
			bool BootstrapOnly = false;
			std::optional<PackageCoarseBounds> Bounds;
		};

		class PackageSpatialMeshResolver;
		SerializedSubtreeInspection InspectSerializedSubtree(const Json &Root, PackageSpatialMeshResolver &Meshes);

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

		std::string GetPackageTerrainPath(const terrain::TerrainArtifactId &Id) {
			const auto Source = terrain::GetTerrainArtifactPath(Id);
			if (!Source) throw terrain::TerrainPersistenceFailure(terrain::TerrainPersistenceError::InvalidBinding);
			return "content/" + *Source;
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
			if (Input.gcount() != static_cast<std::streamsize>(Result.size()) ||
				Input.peek() != std::char_traits<char>::eof() || Input.bad())
				throw std::runtime_error("Package document changed or could not be read completely");
			return Result;
		}

		const ContentEntry *FindContentEntry(const ParsedPackageManifest &Manifest, std::string_view Relative) {
			const auto Entry = std::ranges::lower_bound(Manifest.Content, Relative, {}, &ContentEntry::Path);
			return Entry != Manifest.Content.end() && Entry->Path == Relative ? &*Entry : nullptr;
		}

		void VerifyContentFiles(const std::filesystem::path &Root, const ParsedPackageManifest &Manifest,
			const std::set<std::string> &DeferredTerrainChunks) {
			for (const auto &Entry : Manifest.Content) {
				if (DeferredTerrainChunks.contains(Entry.Path)) continue;
				const auto Path = ResolvePackageFile(Root, Entry.Path, true);
				if (!Path || std::filesystem::file_size(*Path) != Entry.Size || HashFile(*Path, Entry.Size) != Entry.Sha256)
					throw std::runtime_error("Package content is missing or has an integrity mismatch");
			}
		}

		std::string ReadPinnedText(const std::filesystem::path &PackageRoot, const ParsedPackageManifest &Manifest,
			std::string_view Relative, std::size_t MaximumBytes) {
			const auto *Entry = FindContentEntry(Manifest, Relative);
			const auto Path = ResolvePackageFile(PackageRoot, Relative, true);
			if (!Entry || !Path) throw std::runtime_error("Package payload has no pinned content entry");
			auto Bytes = ReadBoundedText(*Path, MaximumBytes);
			if (Bytes.size() != Entry->Size || HashText(Bytes) != Entry->Sha256)
				throw std::runtime_error("Package payload changed after content validation");
			return Bytes;
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

		std::string ReadDistributedPlayerDigest(const std::filesystem::path &Root) {
			const auto Path = ResolvePackageFile(Root, "distributed-player-capability.json", true);
			if (!Path) throw std::runtime_error("Distributed Player requires a current GNS-enabled Player distribution");
			const auto Capability = JsonCodec::Parse(ReadBoundedText(*Path, 1024), 1024, "distributed Player runtime");
			if (!Capability || !Capability->is_object() || Capability->size() != 2 ||
				!Capability->contains("PackageFormatVersion") || !(*Capability)["PackageFormatVersion"].is_number_unsigned() ||
				(*Capability)["PackageFormatVersion"] != DistributedPlayerPackageFormatVersion ||
				!Capability->contains("PlayerSha256") || !(*Capability)["PlayerSha256"].is_string())
				throw std::runtime_error("Distributed Player runtime capability is invalid");
			const auto Digest = (*Capability)["PlayerSha256"].get<std::string>();
			if (!AssetContentId::Parse(Digest)) throw std::runtime_error("Distributed Player runtime digest is invalid");
			return Digest;
		}

		std::string ReadRuntimeDistributionDigest(const std::filesystem::path &Root) {
			const auto File = ResolvePackageFile(Root, "runtime-distribution-capability.json", true);
			if (!File) throw RuntimeDistributionError("UnsupportedRuntime", "The selected runtime distribution has no format-4 capability.");
			const auto Value = JsonCodec::Parse(ReadBoundedText(*File, 4096), 4096, "runtime distribution capability");
			if (!Value || !Value->is_object() || Value->size() != 3 || Value->value("Version", 0u) != 1 ||
				Value->value("PackageFormatVersion", 0u) != RuntimeDistributionPackageFormatVersion ||
				!Value->contains("ExecutableSha256") || !(*Value)["ExecutableSha256"].is_string() ||
				!AssetContentId::Parse((*Value)["ExecutableSha256"].get_ref<const std::string &>()))
				throw RuntimeDistributionError("UnsupportedRuntime", "The selected runtime capability is invalid.");
			return (*Value)["ExecutableSha256"].get<std::string>();
		}

		bool CanDeferTerrain(const ParsedPackageManifest &Manifest, PackageRuntimeLoadMode Mode) {
			return !Manifest.HasApplication && Mode == PackageRuntimeLoadMode::LoopbackDevelopmentClient &&
				Manifest.Inspection.Configuration == PackageConfiguration::Development &&
				Manifest.Inspection.Audience == PackageAudience::Trusted && !Manifest.Inspection.Target;
		}

		std::optional<ParsedPackageManifest> ParseManifest(
			const std::filesystem::path &Root, std::vector<PackageDiagnostic> &Diagnostics, bool VerifyContent,
			const RuntimeDistributionAdmission &Admission = {},
			PackageRuntimeLoadMode Mode = PackageRuntimeLoadMode::Complete
		) {
			try {
				const auto ManifestPath = ResolvePackageFile(Root, "game.package.json", true);
				if (!ManifestPath) throw std::runtime_error("Package manifest file is unsafe");
				const auto Raw = ReadBoundedText(*ManifestPath, MaximumManifestBytes);
				auto Parsed = JsonCodec::Parse(
					Raw,
					MaximumManifestBytes,
					"game package manifest"
				);
				if (!Parsed || !Parsed->is_object() || !Parsed->contains("Format") ||
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
				const auto FormatVersion = (*Parsed)["PackageFormatVersion"].get<std::uint64_t>();
				const bool Managed = FormatVersion == RuntimeDistributionPackageFormatVersion || FormatVersion == ManagedApplicationPackageFormatVersion;
				const bool Standalone = FormatVersion == StandalonePackageFormatVersion || FormatVersion == ManagedApplicationPackageFormatVersion ||
					FormatVersion == DistributedApplicationPackageFormatVersion;
				if (Managed) (void)runtime_distribution::ParseStrictJson(Raw, MaximumManifestBytes);
				const bool Distributed = FormatVersion == DistributedPlayerPackageFormatVersion || FormatVersion == DistributedApplicationPackageFormatVersion ||
					(Managed && Parsed->contains("Audience"));
				if (FormatVersion != 1 && FormatVersion != GamePackageFormatVersion && !Distributed && !Managed && !Standalone)
					throw std::runtime_error("Package format version is unsupported");
				if (Parsed->size() != (Distributed ? 13u : 12u) + (Managed ? 1u : 0u) ||
					(Distributed && (!Parsed->contains("Audience") ||
						!(*Parsed)["Audience"].is_string() ||
						(*Parsed)["Audience"].get_ref<const std::string &>() !=
							GetPackageAudienceName(PackageAudience::DistributedPlayer))) ||
					(Managed && !Parsed->contains("RuntimeDistribution")))
					throw std::runtime_error("Package audience metadata is invalid");
				const auto Compatibility = (*Parsed)["RuntimeCompatibility"].get<std::uint64_t>();
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
					!Startup.contains("AssetCatalog") || !Startup["AssetCatalog"].is_string())
					throw std::runtime_error("Package startup metadata is invalid");
				if (Standalone && (!IsPlayerExecutable(Player) || !Startup.contains("Application") ||
					Startup["Application"] != std::string(application_template::Path)))
					throw std::runtime_error("Application startup binding is invalid");
				if (Distributed) {
					if (!IsPlayerExecutable(Player) || Startup.size() != (Standalone ? 3 : 2) || !Startup.contains("ClientCode") ||
						!Startup["ClientCode"].is_string() ||
						Startup["ClientCode"].get_ref<const std::string &>() != client_package::CodePath ||
						Startup["AssetCatalog"].get_ref<const std::string &>() != client_package::CatalogPath)
						throw std::runtime_error("Distributed Player startup metadata is invalid");
				} else if (Startup.size() != (FormatVersion == 1 ? 3 : Standalone ? 5 : 4) ||
					(Standalone && (!Startup.contains("Application") || Startup["Application"] != std::string(application_template::Path))) ||
					!Startup.contains("Project") || !Startup["Project"].is_string() ||
					!Startup.contains("PreRun") || (!Startup["PreRun"].is_null() && !Startup["PreRun"].is_string()) ||
					(FormatVersion != 1 && (!Startup.contains("ContentManifest") || !Startup["ContentManifest"].is_string()))) {
					throw std::runtime_error("Package startup metadata is invalid");
				}

				ParsedPackageManifest Result;
				Result.HasApplication = Standalone;
				Result.Inspection = {
					.Identity = *Identity,
					.DisplayName = DisplayName,
					.Configuration = *Configuration,
					.FormatVersion = static_cast<std::uint32_t>(FormatVersion),
					.RuntimeCompatibility = static_cast<std::uint32_t>(Compatibility),
					.Revision = (*Parsed)["Revision"].get<std::uint64_t>(),
					.PlayerExecutable = Root / std::filesystem::path(Player),
					.Audience = Distributed ? PackageAudience::DistributedPlayer : PackageAudience::Trusted,
					.ManifestSha256 = HashText(Raw),
				};
				if (Managed) {
					Result.RuntimeDistribution = runtime_distribution::DecodeMetadata((*Parsed)["RuntimeDistribution"]);
					const auto &Metadata = *Result.RuntimeDistribution;
					Result.Inspection.Target = Metadata.Target;
					if ((Metadata.Target == RuntimeDistributionTarget::DistributedPlayer) != Distributed ||
						(Metadata.Target == RuntimeDistributionTarget::DedicatedServer ? !IsServerExecutable(Player) :
							!IsPlayerExecutable(Player)))
						throw RuntimeDistributionError("AudienceViolation", "Runtime distribution target does not match its manifest and executable.");
				}
				if (VerifyContent) {
					if (Admission.ExpectedManifestSha256) {
						if (*Admission.ExpectedManifestSha256 != Result.Inspection.ManifestSha256)
							throw RuntimeDistributionError("ManifestDigestMismatch", "The package does not match its provisioned manifest digest.");
						Result.TrustedCompiler = true;
					}
					const bool Signed = Result.RuntimeDistribution && !Result.RuntimeDistribution->SigningKeyId.empty();
					if (Admission.RequirePublisher && !Signed)
						throw RuntimeDistributionError("PublisherRequired", "Deployment requires a publisher-signed package.");
					if (Result.RuntimeDistribution && (Signed || !Result.RuntimeDistribution->ContentKeyId.empty())) {
						Result.DistributionSecurity = Admission.Security ? *Admission.Security :
							LoadProvisionedRuntimePackageSecurity(RuntimePackageProvisioningUse::Runtime);
					}
					if (Signed) {
						const auto &Metadata = *Result.RuntimeDistribution;
						const auto Project = Identity->ToString();
						RuntimePackagePublicKey Key;
						if (!IsRuntimePackageCryptoAvailable())
							throw RuntimeDistributionError("UnsupportedRuntime", "Publisher verification is unavailable in this runtime.");
						if (!Result.DistributionSecurity.Trust ||
							Result.DistributionSecurity.Trust->GetPublicKey(
								{Project, Metadata.ReleaseId, Metadata.PublisherId, Metadata.SigningKeyId}, Key
							) != RuntimePackageCryptoStatus::Ok)
							throw RuntimeDistributionError("PublisherKeyUnavailable", "The package publisher is not enrolled for this release.");
						const auto SignaturePath = ResolvePackageFile(Root, runtime_distribution::SignaturePath, true);
						if (!SignaturePath)
							throw RuntimeDistributionError("InvalidSignature", "The publisher signature is missing or unsafe.");
						const auto Signature = ReadBoundedText(*SignaturePath, RuntimePackageSignature{}.size());
						if (VerifyRuntimePackageManifest(Raw, Key,
							{reinterpret_cast<const unsigned char *>(Signature.data()), Signature.size()}) !=
							RuntimePackageCryptoStatus::Ok)
							throw RuntimeDistributionError("InvalidSignature", "The publisher signature did not verify.");
						Result.TrustedCompiler = true;
					}
				}
				if (Result.Inspection.Revision == 0) throw std::runtime_error("Package revision is invalid");
				Result.ProjectPath = Startup[Distributed ? "ClientCode" : "Project"].get<std::string>();
				Result.AssetCatalogPath = Startup["AssetCatalog"].get<std::string>();
				if (!Distributed && Startup["PreRun"].is_string())
					Result.PreRunPath = Startup["PreRun"].get<std::string>();
				if (!Distributed && FormatVersion != 1) Result.ContentManifestPath = Startup["ContentManifest"].get<std::string>();
				if (!IsSafeRelativePath(Result.ProjectPath) || !IsSafeRelativePath(Result.AssetCatalogPath) ||
					(Result.PreRunPath && !IsSafeRelativePath(*Result.PreRunPath)) ||
					(Result.ContentManifestPath && !IsSafeRelativePath(*Result.ContentManifestPath)))
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
					if (Distributed) {
						const bool ProjectCode = Entry.Category == "Project" && (Entry.Path == client_package::CodePath ||
							(Standalone && Entry.Path == application_template::Path));
						const bool Catalog = Entry.Path == client_package::CatalogPath && Entry.Category == "Asset";
						const bool Artifact = std::string_view(Entry.Path).starts_with("content/assets/artifacts/") &&
							Entry.Category == "Asset";
						const bool EngineFile = IsDistributedEngineFile(Entry.Path, Entry.Category);
						const bool RuntimeCode = Managed && Entry.Category == "Project" &&
							(Entry.Path == runtime_distribution::CodeCatalogPath ||
								std::string_view(Entry.Path).starts_with("content/scripts/"));
						if (!ProjectCode && !Catalog && !Artifact && !EngineFile && !RuntimeCode)
							throw std::runtime_error("Distributed Player package contains an unexpected entry");
					}
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
				if (Result.ContentManifestPath) {
					if (!Seen.contains(LowerPath(*Result.ContentManifestPath)))
						throw std::runtime_error("Package content manifest entry is absent");
					auto Found = std::ranges::find(Result.Content, *Result.ContentManifestPath, &ContentEntry::Path);
					if (Found == Result.Content.end()) throw std::runtime_error("Package content manifest metadata is absent");
					Result.ContentManifestDigest = *AssetContentId::Parse(Found->Sha256);
					const auto ContentManifestFile = ResolvePackageFile(Root, *Result.ContentManifestPath, true);
					if (!ContentManifestFile) throw std::runtime_error("Package content manifest file is absent");
					auto ContentManifestText = ReadBoundedText(*ContentManifestFile, MaximumPackageContentManifestBytes);
					auto ContentManifest = ParsePackageContentManifest(ContentManifestText);
					if (!ContentManifest || ContentManifest->Package.Project != Result.Inspection.Identity ||
						ContentManifest->Package.PackageVersion != Result.Inspection.Revision)
						throw std::runtime_error("Package content manifest is invalid or has the wrong namespace");
					for (const auto &Unit : ContentManifest->Entries) {
						auto UnitFound = std::ranges::find(Result.Content, Unit.BlobReference, &ContentEntry::Path);
						if (UnitFound == Result.Content.end() || UnitFound->Size != Unit.CompressedBytes ||
							UnitFound->Sha256 != Unit.Digest.ToString())
							throw std::runtime_error("Package content unit is absent from the verified content table");
						if (VerifyContent) {
							const auto UnitFile = ResolvePackageFile(Root, Unit.BlobReference, true);
							if (!UnitFile) throw std::runtime_error("Package content unit file is absent");
							auto UnitText = ReadBoundedText(*UnitFile, MaximumPackageContentPayloadBytes);
							auto UnitDocument = JsonCodec::Parse(
								UnitText, MaximumPackageContentPayloadBytes, "package content unit"
							);
							if (!UnitDocument || !UnitDocument->is_object() ||
								UnitDocument->value("Version", 0u) != ContentManifest->InstanceSchemaVersion ||
								UnitDocument->value("ClassName", "") == "DataModel")
								throw std::runtime_error("Package content unit payload is malformed");
						}
					}
				}
				for (const auto Required : RequiredRuntimeFiles)
					if (!Seen.contains(LowerPath(Required)))
						throw std::runtime_error("Package runtime resource is absent");
				Result.Inspection.ContentCount = Result.Content.size();
				Result.Inspection.ContentBytes = Aggregate;

				if (VerifyContent && !CanDeferTerrain(Result, Mode))
					VerifyContentFiles(Root, Result, {});
				if (VerifyContent) {
					std::unordered_set<std::string> AllowedFiles{LowerPath("game.package.json")};
					if (Result.RuntimeDistribution && !Result.RuntimeDistribution->SigningKeyId.empty())
						AllowedFiles.insert(std::string(runtime_distribution::SignaturePath));
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
						if (++SeenEntries > MaximumContentEntries + AllowedDirectories.size() + 2 ||
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
			} catch (const RuntimeDistributionError &Error) {
				AddDiagnostic(Diagnostics, PackageDiagnosticSeverity::Error, "Integrity", Error.Code, Error.what());
				return std::nullopt;
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
				std::unordered_set<std::string> ArtifactPaths;
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
					ArtifactPaths.insert("content/assets/artifacts/" + ContentId + ".gasset");
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
				if (Manifest.Inspection.Audience == PackageAudience::DistributedPlayer)
					for (const auto &Entry : Manifest.Content)
						if (Entry.Category == "Asset" && Entry.Path != Manifest.AssetCatalogPath &&
							!ArtifactPaths.contains(Entry.Path))
							throw std::runtime_error("Distributed Player package contains an unreferenced asset artifact");
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

#if defined(_WIN32)
		using PackagePublication = windows_package_publication::Writer;
#else
		class PackagePublication final {
		  public:
			explicit PackagePublication(const std::filesystem::path &Destination) : Lease(Destination) {
				Lease += ".gpk-publish";
				if (!std::filesystem::create_directory(Lease))
					throw std::runtime_error("Another package publication holds this destination");
			}

			PackagePublication(const PackagePublication &) = delete;
			PackagePublication &operator=(const PackagePublication &) = delete;

			~PackagePublication() {
				std::error_code Ignored;
				std::filesystem::remove(Lease, Ignored);
			}

		  private:
			std::filesystem::path Lease;
		};
#endif

		std::string PackageGridPath(const AssetContentId& Id) { return "content/.gargantuan/blockgrid/"+blockgrid::SegmentFileName(Id); }
		void ValidateStagingPaths(
			const std::filesystem::path &Destination,
			const std::filesystem::path &Candidate,
			const RuntimeDistributionDescription &Distribution,
			const GamePayload &Payload,
			const AssetRuntimeSnapshot &Assets
		) {
#if defined(_WIN32)
			constexpr std::size_t MaximumSupportedStagingPath = 240;
			auto Check = [&](std::string_view Relative) {
				if ((Destination / std::filesystem::path(Relative)).native().size() > MaximumSupportedStagingPath ||
					(Candidate / std::filesystem::path(Relative)).native().size() > MaximumSupportedStagingPath)
					throw std::runtime_error("Package output path is too long for this Windows runtime");
			};
			Check("game.package.json");
			if (Payload.DistributedPlayer) Check(client_package::CodePath);
			else {
				Check("content/game.instance.json");
				Check("content/content.manifest.json");
			}
			Check("content/assets/catalog.json");
			if (Payload.PreRunSource) Check("content/prerun.luau");
			for (const auto &File : Distribution.Files)
				Check(File.Path);
			for (const auto &Artifact : Assets.Artifacts)
				Check("content/" + Artifact.RelativePath);
			for (const auto &Unit : Payload.ContentUnits) Check(Unit.Entry.BlobReference);
			for (const auto &Artifact : Payload.TerrainArtifacts) Check(GetPackageTerrainPath(Artifact.Id));
			for (const auto& [Id,Part]:Payload.GridSegments) Check(PackageGridPath(Id));
#else
			(void)Destination;
			(void)Candidate;
			(void)Distribution;
			(void)Payload;
			(void)Assets;
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

		class PackageSpatialMeshResolver final {
			struct MeshRecord final {
				AssetContentId Content;
				std::set<std::string> Dependencies;
				bool Inspected = false;
				std::optional<RenderBounds> Bounds;
				bool HasCollision = false;
			};
			std::map<std::string, MeshRecord> Records;
			std::map<std::string, AssetContentId> Collisions;
			std::function<std::shared_ptr<const std::vector<std::uint8_t>>(std::string_view)> LoadArtifact;

		  public:
			PackageSpatialMeshResolver(std::string_view Catalog,
				std::function<std::shared_ptr<const std::vector<std::uint8_t>>(std::string_view)> Loader)
				: LoadArtifact(std::move(Loader)) {
				if (Catalog.size() > 1024 * 1024) throw std::runtime_error("Spatial asset catalog exceeds its bound");
				auto Parsed = JsonCodec::Parse(Catalog, 1024 * 1024, "spatial asset catalog");
				if (!Parsed || !Parsed->is_object() || !Parsed->contains("Format") ||
					(*Parsed)["Format"] != "GargantuanRuntimeAssets" || !Parsed->contains("Version") ||
					(*Parsed)["Version"] != 1 || !Parsed->contains("Assets") ||
					!(*Parsed)["Assets"].is_array() || (*Parsed)["Assets"].size() > AssetLimits::MaximumCatalogRecords)
					throw std::runtime_error("Spatial asset catalog is invalid");
				for (const auto &Entry : (*Parsed)["Assets"]) {
					if (!Entry.is_object() || !Entry.contains("Kind") || !Entry["Kind"].is_string())
						throw std::runtime_error("Spatial asset record is malformed");
					if (Entry["Kind"] != "Mesh" && Entry["Kind"] != "Collision") continue;
					if (!Entry.contains("Reference") || !Entry["Reference"].is_string() ||
						!Entry.contains("ContentId") || !Entry["ContentId"].is_string() ||
						!Entry.contains("AssetId") || !Entry["AssetId"].is_string())
						throw std::runtime_error("Spatial mesh identity is malformed");
					const auto &Reference = Entry["Reference"].get_ref<const std::string &>();
					const auto Content = AssetContentId::Parse(Entry["ContentId"].get_ref<const std::string &>());
					const auto Identity = AssetId::Parse(Entry["AssetId"].get_ref<const std::string &>());
					if (!Identity || !Content || AssetReference::FromAssetId(*Identity).Value != Reference)
						throw std::runtime_error("Spatial mesh identity is invalid or duplicated");
					if (Entry["Kind"] == "Mesh") {
						if (!Entry.contains("Dependencies") || !Entry["Dependencies"].is_array() ||
							Entry["Dependencies"].size() > AssetLimits::MaximumDependencies)
							throw std::runtime_error("Spatial mesh dependencies are malformed");
						MeshRecord Record{};
						Record.Content = *Content;
						for (const auto &Dependency : Entry["Dependencies"])
							if (!Dependency.is_string() ||
								!AssetId::Parse(Dependency.get_ref<const std::string &>()) ||
								!Record.Dependencies.insert(Dependency.get<std::string>()).second)
								throw std::runtime_error("Spatial mesh dependency identity is invalid");
						if (!Records.emplace(Reference, std::move(Record)).second)
							throw std::runtime_error("Spatial mesh identity is duplicated");
					} else if (!Collisions.emplace(Reference, *Content).second)
						throw std::runtime_error("Spatial collision identity is duplicated");
				}
			}

			[[nodiscard]] std::optional<RenderBounds> Resolve(std::string_view Reference, bool RequireCollision) {
				auto Found = Records.find(std::string(Reference));
				if (Found == Records.end()) return std::nullopt;
				auto &Record = Found->second;
				if (Record.Inspected) return RequireCollision && !Record.HasCollision ? std::nullopt : Record.Bounds;
				Record.Inspected = true;
				const auto Bytes = LoadArtifact(Record.Content.ToString());
				if (!Bytes || Bytes->empty() || Bytes->size() > AssetLimits::MaximumArtifactBytes) return std::nullopt;
				const auto Decoded = DecodeAssetArtifact(*Bytes, AssetKind::Mesh, Record.Content);
				if (!Decoded) return std::nullopt;
				const auto *Mesh = std::get_if<ImportedMesh>(&Decoded->Asset);
				if (!Mesh || !Mesh->Vertices || Mesh->Vertices->empty() || Mesh->Skeleton) return std::nullopt;
				const auto &Bounds = Mesh->Bounds;
				for (std::size_t Axis = 0; Axis < 3; ++Axis)
					if (!std::isfinite(Bounds.Minimum[Axis]) || !std::isfinite(Bounds.Maximum[Axis]) ||
						Bounds.Minimum[Axis] > Bounds.Maximum[Axis]) return std::nullopt;
				for (const auto &Vertex : *Mesh->Vertices)
					for (std::size_t Axis = 0; Axis < 3; ++Axis)
						if (!std::isfinite(Vertex.Position[Axis]) || Vertex.Position[Axis] < Bounds.Minimum[Axis] ||
							Vertex.Position[Axis] > Bounds.Maximum[Axis]) return std::nullopt;
				RenderBounds Aggregate = Bounds;
				if (Mesh->CollisionAsset) {
					if (!Record.Dependencies.contains(Mesh->CollisionAsset->ToString())) return std::nullopt;
					const auto CollisionReference = AssetReference::FromAssetId(*Mesh->CollisionAsset).Value;
					const auto Companion = Collisions.find(CollisionReference);
					if (Companion == Collisions.end()) return std::nullopt;
					const auto CollisionBytes = LoadArtifact(Companion->second.ToString());
					if (!CollisionBytes || CollisionBytes->empty() ||
						CollisionBytes->size() > AssetLimits::MaximumArtifactBytes) return std::nullopt;
					const auto CollisionDecoded = DecodeAssetArtifact(*CollisionBytes, AssetKind::Collision, Companion->second);
					if (!CollisionDecoded) return std::nullopt;
					const auto *Collision = std::get_if<ImportedCollision>(&CollisionDecoded->Asset);
					if (!Collision || !Collision->Positions || Collision->Positions->empty()) return std::nullopt;
					for (std::size_t Axis = 0; Axis < 3; ++Axis)
						if (!std::isfinite(Collision->Bounds.Minimum[Axis]) ||
							!std::isfinite(Collision->Bounds.Maximum[Axis]) ||
							Collision->Bounds.Minimum[Axis] > Collision->Bounds.Maximum[Axis]) return std::nullopt;
					for (const auto &Position : *Collision->Positions)
						for (std::size_t Axis = 0; Axis < 3; ++Axis)
							if (!std::isfinite(Position[Axis]) || Position[Axis] < Collision->Bounds.Minimum[Axis] ||
								Position[Axis] > Collision->Bounds.Maximum[Axis]) return std::nullopt;
					for (std::size_t Axis = 0; Axis < 3; ++Axis) {
						Aggregate.Minimum[Axis] = std::min(Aggregate.Minimum[Axis], Collision->Bounds.Minimum[Axis]);
						Aggregate.Maximum[Axis] = std::max(Aggregate.Maximum[Axis], Collision->Bounds.Maximum[Axis]);
					}
					Record.HasCollision = true;
				}
				Record.Bounds = Aggregate;
				if (RequireCollision && !Record.HasCollision) return std::nullopt;
				return Record.Bounds;
			}
		};

		PackageSpatialMeshResolver SnapshotSpatialMeshes(const AssetRuntimeSnapshot &Assets) {
			std::map<std::string, std::shared_ptr<const std::vector<std::uint8_t>>> Artifacts;
			for (const auto &Artifact : Assets.Artifacts)
				if (!Artifacts.emplace(Artifact.RelativePath, Artifact.Bytes).second)
					throw std::runtime_error("Spatial asset artifact path is duplicated");
			return PackageSpatialMeshResolver(Assets.CatalogJson, [Artifacts = std::move(Artifacts)](std::string_view Content) {
				const std::string Path = "assets/artifacts/" + std::string(Content) + ".gasset";
				const auto Found = Artifacts.find(Path);
				return Found == Artifacts.end() ? std::shared_ptr<const std::vector<std::uint8_t>>{} : Found->second;
			});
		}

		PackageSpatialMeshResolver PackageSpatialMeshes(const std::filesystem::path &Root,
			std::string_view Catalog) {
			return PackageSpatialMeshResolver(Catalog, [Root](std::string_view Content) {
				const auto Path = ResolvePackageFile(Root,
					"content/assets/artifacts/" + std::string(Content) + ".gasset", true);
				if (!Path) return std::shared_ptr<const std::vector<std::uint8_t>>{};
				const auto Bytes = ReadBoundedText(*Path, AssetLimits::MaximumArtifactBytes);
				return std::make_shared<const std::vector<std::uint8_t>>(Bytes.begin(), Bytes.end());
			});
		}

		SerializedSubtreeInspection InspectSerializedSubtree(const Json &Root, PackageSpatialMeshResolver &Meshes) {
			SerializedSubtreeInspection Result;
			std::vector<const Json *> Pending{&Root};
			std::array<double, 3> Minimum{
				std::numeric_limits<double>::infinity(),
				std::numeric_limits<double>::infinity(),
				std::numeric_limits<double>::infinity(),
			};
			std::array<double, 3> Maximum{
				-std::numeric_limits<double>::infinity(),
				-std::numeric_limits<double>::infinity(),
				-std::numeric_limits<double>::infinity(),
			};
			bool HasBounds = false;
			while (!Pending.empty()) {
				const auto *Node = Pending.back();
				Pending.pop_back();
				if (!Node->is_object() || ++Result.Objects > MaximumPackageContentObjectsPerUnit) return Result;
				const auto ClassName = Node->value("ClassName", "");
				if (Node->contains(blockgrid::SegmentKey) || ClassName == "Script" || ClassName == "ModuleScript" || ClassName == "RemoteEvent" ||
					ClassName == "RemoteFunction" || ClassName == "Character" || ClassName == "KinematicCharacter" ||
					ClassName == "Player" || ClassName == "Camera" || ClassName == "Terrain" || ClassName == "FileLink")
					Result.BootstrapOnly = true;
				const bool Primitive = ClassName == "Part";
				const bool MeshPartValue = ClassName == "MeshPart";
				if (Primitive || MeshPartValue || (Node->contains("Properties") &&
					(*Node)["Properties"].is_object() &&
					((*Node)["Properties"].contains("CFrame") || (*Node)["Properties"].contains("Size")))) {
					if (!Primitive && !MeshPartValue) { Result.BootstrapOnly = true; continue; }
					if (!Node->contains("Properties") || !(*Node)["Properties"].is_object()) {
						Result.BootstrapOnly = true; continue;
					}
					const auto &Properties = (*Node)["Properties"];
					if (!Properties.contains("Anchored") || !Properties["Anchored"].is_object() ||
						!Properties["Anchored"].contains("Bool") || Properties["Anchored"]["Bool"] != true) {
						Result.BootstrapOnly = true; continue;
					}
					if (!Properties.contains("CFrame") || !Properties["CFrame"].is_object() ||
						!Properties["CFrame"].contains("CFrame") || !Properties["CFrame"]["CFrame"].is_array() ||
						Properties["CFrame"]["CFrame"].size() != 12 || !Properties.contains("Size") ||
						!Properties["Size"].is_object() || !Properties["Size"].contains("Vector3") ||
						!Properties["Size"]["Vector3"].is_array() || Properties["Size"]["Vector3"].size() != 3) {
						Result.BootstrapOnly = true; continue;
					}
					const auto &Frame = Properties["CFrame"]["CFrame"];
					const auto &Size = Properties["Size"]["Vector3"];
					std::array<double, 12> Transform{};
					std::array<double, 3> Scale{};
					bool Finite = true;
					for (std::size_t Index = 0; Index < Transform.size(); ++Index) {
						if (!Frame[Index].is_number()) { Finite = false; break; }
						Transform[Index] = Frame[Index].get<double>();
						Finite = Finite && std::isfinite(Transform[Index]);
					}
					for (std::size_t Axis = 0; Axis < 3; ++Axis) {
						if (!Size[Axis].is_number()) { Finite = false; break; }
						Scale[Axis] = Size[Axis].get<double>();
						Finite = Finite && std::isfinite(Scale[Axis]);
					}
					if (!Finite) { Result.BootstrapOnly = true; continue; }
					// Box3D converts CFrame rotation through a quaternion. Only rigid, proper
					// bases have the same spatial envelope as the renderer's matrix path.
					constexpr double RigidTolerance = 0.000001;
					for (std::size_t Column = 0; Column < 3; ++Column) {
						double LengthSquared = 0.0;
						for (std::size_t Row = 0; Row < 3; ++Row)
							LengthSquared += Transform[3 + Column * 3 + Row] * Transform[3 + Column * 3 + Row];
						if (std::abs(LengthSquared - 1.0) > RigidTolerance) Finite = false;
						for (std::size_t Other = 0; Other < Column; ++Other) {
							double Dot = 0.0;
							for (std::size_t Row = 0; Row < 3; ++Row)
								Dot += Transform[3 + Column * 3 + Row] * Transform[3 + Other * 3 + Row];
							if (std::abs(Dot) > RigidTolerance) Finite = false;
						}
					}
					const double Determinant =
						Transform[3] * (Transform[7] * Transform[11] - Transform[10] * Transform[8]) -
						Transform[6] * (Transform[4] * Transform[11] - Transform[10] * Transform[5]) +
						Transform[9] * (Transform[4] * Transform[8] - Transform[7] * Transform[5]);
					if (!std::isfinite(Determinant) || std::abs(Determinant - 1.0) > RigidTolerance || !Finite) {
						Result.BootstrapOnly = true; continue;
					}
					std::array<double, 3> LocalMinimum{-0.5, -0.5, -0.5};
					std::array<double, 3> LocalMaximum{0.5, 0.5, 0.5};
					if (MeshPartValue) {
						if (!Properties.contains("CollisionMode")) { Result.BootstrapOnly = true; continue; }
						const auto &Mode = Properties["CollisionMode"];
						if (!Mode.is_object() || !Mode.contains("EnumItem") || !Mode["EnumItem"].is_array() ||
							Mode["EnumItem"].size() != 2 || Mode["EnumItem"][0] != "MeshCollisionMode" ||
							!Mode["EnumItem"][1].is_string()) {
							Result.BootstrapOnly = true; continue;
						}
						const auto &Name = Mode["EnumItem"][1];
						if (Name != "Box" && Name != "StaticMesh") { Result.BootstrapOnly = true; continue; }
						const bool RequireCollision = Name == "StaticMesh";
						if (!Properties.contains("Mesh") || !Properties["Mesh"].is_object() ||
							!Properties["Mesh"].contains("String") || !Properties["Mesh"]["String"].is_string()) {
							Result.BootstrapOnly = true; continue;
						}
						const auto Bounds = Meshes.Resolve(Properties["Mesh"]["String"].get_ref<const std::string &>(),
							RequireCollision);
						if (!Bounds) { Result.BootstrapOnly = true; continue; }
						for (std::size_t Axis = 0; Axis < 3; ++Axis) {
							LocalMinimum[Axis] = std::min(LocalMinimum[Axis], static_cast<double>(Bounds->Minimum[Axis]));
							LocalMaximum[Axis] = std::max(LocalMaximum[Axis], static_cast<double>(Bounds->Maximum[Axis]));
						}
					}
					for (std::size_t Corner = 0; Corner < 8; ++Corner) {
						std::array<double, 3> Local{};
						for (std::size_t Axis = 0; Axis < 3; ++Axis)
							Local[Axis] = (Corner & (1u << Axis)) ? LocalMaximum[Axis] : LocalMinimum[Axis];
						for (std::size_t Row = 0; Row < 3; ++Row) {
							double Position = Transform[Row];
							double Magnitude = std::abs(Position);
							for (std::size_t Column = 0; Column < 3; ++Column) {
								const double Term = Transform[3 + Column * 3 + Row] * Scale[Column] * Local[Column];
								Position += Term;
								Magnitude += std::abs(Term);
							}
							// Leave room for float matrix arithmetic and Box3D's quat_cast and
							// normalization of an accepted near-rigid basis.
							const double Margin =
								(32.0 * std::numeric_limits<float>::epsilon() + 64.0 * RigidTolerance) *
								(Magnitude + 1.0);
							if (!std::isfinite(Position) || !std::isfinite(Margin)) { Result.BootstrapOnly = true; break; }
							Minimum[Row] = std::min(Minimum[Row], Position - Margin);
							Maximum[Row] = std::max(Maximum[Row], Position + Margin);
						}
						if (Result.BootstrapOnly) break;
					}
					HasBounds = true;
				}
				if (Node->contains("Children")) {
					if (!(*Node)["Children"].is_array()) Result.BootstrapOnly = true;
					else {
						if ((*Node)["Children"].size() > MaximumPackageContentObjectsPerUnit -
							std::min<std::size_t>(MaximumPackageContentObjectsPerUnit, Pending.size())) {
							Result.BootstrapOnly = true; return Result;
						}
						for (const auto &Child : (*Node)["Children"]) Pending.push_back(&Child);
					}
				}
			}
			if (HasBounds && !Result.BootstrapOnly) {
				PackageCoarseBounds Encoded;
				for (std::size_t Axis = 0; Axis < 3; ++Axis) {
					if (!std::isfinite(Minimum[Axis]) || !std::isfinite(Maximum[Axis]) ||
						Minimum[Axis] <= -std::numeric_limits<float>::max() ||
						Maximum[Axis] >= std::numeric_limits<float>::max()) return Result;
					Encoded.Minimum[Axis] = std::nextafter(static_cast<float>(Minimum[Axis]),
						-std::numeric_limits<float>::infinity());
					Encoded.Maximum[Axis] = std::nextafter(static_cast<float>(Maximum[Axis]),
						std::numeric_limits<float>::infinity());
					if (!std::isfinite(Encoded.Minimum[Axis]) || !std::isfinite(Encoded.Maximum[Axis])) return Result;
				}
				Result.Bounds = Encoded;
			}
			return Result;
		}

		std::optional<std::size_t> CoarseMembershipCount(const PackageCoarseBounds &Bounds) {
			std::size_t Count = 1;
			for (std::size_t Axis = 0; Axis < 3; ++Axis) {
				const double MinimumCell = std::floor(static_cast<double>(Bounds.Minimum[Axis]) / 512.0);
				const double MaximumCell = std::floor(static_cast<double>(Bounds.Maximum[Axis]) / 512.0);
				if (!std::isfinite(MinimumCell) || !std::isfinite(MaximumCell) ||
					MinimumCell < std::numeric_limits<std::int32_t>::min() ||
					MaximumCell > std::numeric_limits<std::int32_t>::max()) return std::nullopt;
				const auto Span = static_cast<std::uint64_t>(MaximumCell - MinimumCell + 1.0);
				if (Span == 0 || Span > MaximumPackageCoarseQueryCells / Count) return std::nullopt;
				Count *= static_cast<std::size_t>(Span);
			}
			return Count;
		}

		std::optional<ResidencyUnitBoundsMetadata> InspectSpatialContentUnit(std::string_view Payload,
			const PackageContentEntry &Entry, PackageSpatialMeshResolver &Meshes,
			std::shared_ptr<const RuntimeSchemaRegistry> SchemaSnapshot = {},
			std::uint32_t ExpectedSchemaVersion = PackageContentInstanceSchemaVersion) {
			if (Entry.PackageSpaceKey != "default" || Payload.empty() ||
				Payload.size() > MaximumPackageContentPayloadBytes)
				throw std::runtime_error("Package content unit has no qualified default-space bounds");
			const auto Bytes = std::span(reinterpret_cast<const std::uint8_t *>(Payload.data()), Payload.size());
			const auto Prepared = InstanceSerialization::Internal::PrepareDetachedJson(
				Bytes, Entry.ObjectCount, std::move(SchemaSnapshot));
			if (!Prepared) throw std::runtime_error("Package content unit has invalid Instance identity or references: " +
				Prepared.error());
			auto Parsed = JsonCodec::Parse(Payload, MaximumPackageContentPayloadBytes, "package spatial content unit");
			if (!Parsed || !Parsed->is_object() ||
				(ExpectedSchemaVersion != 4 && ExpectedSchemaVersion != PackageContentInstanceSchemaVersion) ||
				Parsed->value("Version", 0u) != ExpectedSchemaVersion ||
				Parsed->value("ClassName", "") == "DataModel")
				throw std::runtime_error("Package spatial content unit is malformed");
			const auto Inspection = InspectSerializedSubtree(*Parsed, Meshes);
			if (Inspection.BootstrapOnly || Inspection.Objects != Entry.ObjectCount ||
				Inspection.Bounds.has_value() != Entry.CoarseBounds.has_value() ||
				(Inspection.Bounds && !CoarseMembershipCount(*Inspection.Bounds)))
				throw std::runtime_error("Package spatial content unit lacks a conservative aggregate");
			if (!Inspection.Bounds) return std::nullopt;
			return ResidencyUnitBoundsMetadata{Entry.Key, Entry.Digest, Entry.PackageSpaceKey, *Inspection.Bounds};
		}

		void CheckSpatialProof(const PackageContentManifest &Manifest,
			ResidencySpatialMetadata &Metadata) {
			const auto Report = InspectResidencySpatialMetadata(Manifest, Metadata);
			if (Report.Status != ResidencyMetadataStatus::Compatible ||
				Report.CompatibleUnits != Report.SpatialUnits)
				throw std::runtime_error("Package manifest does not contain qualified content bounds");
			(void)PackageContentCoarseIndex(Manifest);
		}

		void PartitionPackageContent(GamePayload &Payload, PackageSpatialMeshResolver &Meshes) {
			auto Project = JsonCodec::Parse(Payload.ProjectJson, MaximumProjectSnapshotBytes, "package project snapshot");
			if (!Project || !Project->is_object() || !Project->contains("Children") || !(*Project)["Children"].is_array())
				throw std::runtime_error("Package project snapshot cannot be partitioned");
			Json *Workspace = nullptr;
			for (auto &Child : (*Project)["Children"])
				if (Child.is_object() && Child.value("ClassName", "") == "Workspace") {
					Workspace = &Child;
					break;
				}
			if (!Workspace || !Workspace->contains("Children") || !(*Workspace)["Children"].is_array())
				throw std::runtime_error("Package project has no serializable Workspace");
			// A package unit is a separate v5 Instance document. Keep both ends of
			// every authored reference in bootstrap until cross-unit fixups exist.
			std::unordered_set<std::uint32_t> ReferencedIds;
			auto VisitNodes = [](const Json &Root, auto &&Visit) {
				std::vector<const Json *> Pending{&Root};
				std::size_t Count = 0;
				while (!Pending.empty()) {
					const auto *Node = Pending.back();
					Pending.pop_back();
					if (!Node->is_object() || ++Count > MaximumPersistenceObjects)
						throw std::runtime_error("Malformed or oversized package Instance hierarchy");
					Visit(*Node);
					if (!Node->contains("Children") || !(*Node)["Children"].is_array())
						throw std::runtime_error("Malformed package Instance children");
					if ((*Node)["Children"].size() > MaximumPersistenceObjects - Count -
						std::min(Pending.size(), MaximumPersistenceObjects - Count))
						throw std::runtime_error("Package Instance hierarchy exceeds object bound");
					for (const auto &Child : (*Node)["Children"]) Pending.push_back(&Child);
				}
			};
			VisitNodes(*Project, [&](const Json &Node) {
				if (!Node.contains("Properties") || !Node["Properties"].is_object())
					throw std::runtime_error("Malformed package Instance properties");
				for (const auto &[_, Property] : Node["Properties"].items()) {
					if (!Property.is_object() || !Property.contains("InstanceReference") ||
						Property["InstanceReference"].is_null()) continue;
					if (!Property["InstanceReference"].is_number_unsigned())
						throw std::runtime_error("Malformed package Instance reference");
					const auto Id = Property["InstanceReference"].get<std::uint64_t>();
					if (Id == 0 || Id > MaximumPersistenceObjects)
						throw std::runtime_error("Package Instance reference exceeds document bounds");
					ReferencedIds.insert(static_cast<std::uint32_t>(Id));
				}
			});
			Json Retained = Json::array();
			std::size_t UnitNumber = 0;
			std::size_t Memberships = 0;
			for (auto &Child : (*Workspace)["Children"]) {
				bool HasReferenceBoundary = false;
				VisitNodes(Child, [&](const Json &Node) {
					if (!Node.contains("NodeId") || !Node["NodeId"].is_number_unsigned() ||
						Node["NodeId"].get<std::uint64_t>() == 0 ||
						Node["NodeId"].get<std::uint64_t>() > MaximumPersistenceObjects)
						throw std::runtime_error("Package Instance node lacks bounded v5 identity");
					if (ReferencedIds.contains(Node["NodeId"].get<std::uint32_t>())) HasReferenceBoundary = true;
					if (!Node.contains("Properties") || !Node["Properties"].is_object()) return;
					for (const auto &[_, Property] : Node["Properties"].items())
						if (Property.is_object() && Property.contains("InstanceReference") &&
							!Property["InstanceReference"].is_null()) HasReferenceBoundary = true;
				});
				if (HasReferenceBoundary) {
					Retained.push_back(std::move(Child));
					continue;
				}
				const auto SpatialStarted = std::chrono::steady_clock::now();
				auto Inspection = InspectSerializedSubtree(Child, Meshes);
				Payload.SpatialInspectionDuration += std::chrono::duration_cast<std::chrono::microseconds>(
					std::chrono::steady_clock::now() - SpatialStarted);
				if (Inspection.BootstrapOnly || !Inspection.Bounds || Inspection.Objects == 0 ||
					Inspection.Objects > MaximumPackageContentObjectsPerUnit) {
					Retained.push_back(std::move(Child));
					continue;
				}
				const auto Cells = CoarseMembershipCount(*Inspection.Bounds);
				if (!Cells || *Cells > MaximumPackageCoarseIndexCells - Memberships ||
					Payload.ContentUnits.size() >= MaximumPackageContentUnits) {
					Retained.push_back(std::move(Child));
					continue;
				}
				auto Unit = Child;
				Unit["Version"] = PackageContentInstanceSchemaVersion;
				auto Encoded = JsonCodec::Encode(Unit, "package content unit");
				if (!Encoded || Encoded->empty() || Encoded->size() > MaximumPackageContentPayloadBytes) {
					Retained.push_back(std::move(Child));
					continue;
				}
				const auto Key = std::format("workspace/{:08x}", UnitNumber++);
				const auto Blob = std::format("content/regions/{:08x}.instance.json", UnitNumber - 1);
				const auto Bytes = std::span(reinterpret_cast<const std::uint8_t *>(Encoded->data()), Encoded->size());
				Payload.ContentUnits.push_back({
					.Entry = {
						.Key = Key,
						.BlobReference = Blob,
						.Digest = AssetContentId::Hash(Bytes),
						.CompressedBytes = Encoded->size(),
						.UncompressedBytes = Encoded->size(),
						.ObjectCount = static_cast<std::uint32_t>(Inspection.Objects),
						.Dependencies = {},
						.PackageSpaceKey = "default",
						.CoarseBounds = Inspection.Bounds,
						.Flags = PackageContentFlags::ImmutableBaseline,
					},
					.Payload = std::move(*Encoded),
				});
				Memberships += *Cells;
			}
			(*Workspace)["Children"] = std::move(Retained);
			auto Bootstrap = JsonCodec::Encode(*Project, "package bootstrap project");
			if (!Bootstrap || Bootstrap->size() > MaximumProjectSnapshotBytes)
				throw std::runtime_error("Package bootstrap project exceeds its bound");
			Payload.ProjectJson = std::move(*Bootstrap);
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

	std::string_view GetPackageAudienceName(PackageAudience Audience) {
		switch (Audience) {
		case PackageAudience::Trusted: return "Trusted";
		case PackageAudience::DistributedPlayer: return "DistributedPlayer";
		}
		return "Unknown";
	}

	std::optional<PackageAudience> ParsePackageAudience(std::string_view Value) {
		if (Value == "Trusted") return PackageAudience::Trusted;
		if (Value == "DistributedPlayer") return PackageAudience::DistributedPlayer;
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
		if (Raw) {
			std::filesystem::path Result(Raw);
			SDL_free(Raw);
			return Result;
		}
		const std::string SdlError = SDL_GetError();
		std::filesystem::path Result;
#if defined(_WIN32)
		if (const auto *ApplicationData = _wgetenv(L"APPDATA"))
			Result = ApplicationData;
		else if (const auto *LocalApplicationData = _wgetenv(L"LOCALAPPDATA"))
			Result = LocalApplicationData;
#else
		if (const auto *DataHome = std::getenv("XDG_DATA_HOME"))
			Result = DataHome;
		else if (const auto *UserHome = std::getenv("HOME"))
			Result = std::filesystem::path(UserHome) / ".local/share";
#endif
		if (Result.empty())
			throw std::runtime_error(std::format("Could not resolve the Gargantuan user-data root: {}", SdlError));
		Result /= "Gargantuan";
		Result /= Identity.ToString();
		return Result;
	}

	RuntimeDistributionPlan PackageBuilder::CaptureRuntimeDistribution(
		const Project &ProjectValue, const std::shared_ptr<DataModel> &World,
		const RuntimeDistributionRequest &Request, bool RetainSource
	) {
		AssertAuthoritativeMutation("PackageBuilder::CaptureRuntimeDistribution");
		if (!World) throw RuntimeDistributionError("InvalidPolicy", "Runtime distribution requires a loaded project.");
		const bool Distributed = Request.Target == RuntimeDistributionTarget::DistributedPlayer;
		std::map<std::string, std::vector<std::string>, std::less<>> PublicPaths;
		if (Distributed)
			for (auto &Script : client_package::CaptureEligibleScripts(*World))
				PublicPaths.emplace(std::move(Script.SourceId), std::move(Script.Path));
		RuntimeDistributionPlan Plan{.Request = Request};
		std::vector<std::shared_ptr<LuaSourceContainer>> Objects;
		std::vector<std::string> Path;
		std::size_t Nodes = 0;
		const auto *Application = application_template::FindFolder(*World);
		const auto Walk = [&](auto &Self, const std::shared_ptr<Instance> &Object, bool Authored, bool InApplication) -> void {
			InApplication |= Object.get() == Application;
			if (++Nodes > MaximumPersistenceObjects || Path.size() == MaximumProtocolJsonDepth)
				throw RuntimeDistributionError("InvalidPolicy", "The script capture exceeds its object or depth bound.");
			Authored = Authored && Object->GetArchivable();
			Path.push_back(Object->GetName());
			if (auto Script = std::dynamic_pointer_cast<LuaSourceContainer>(Object)) {
				if (Plan.Scripts.size() == 4096)
					throw RuntimeDistributionError("InvalidPolicy", "The script catalog exceeds its entry bound.");
				auto Id = Script->GetSourceId();
				const auto Public = PublicPaths.find(Id);
				const bool Eligible = Authored && (InApplication ? Request.Target != RuntimeDistributionTarget::DedicatedServer :
					!Distributed || Public != PublicPaths.end());
				Plan.Scripts.push_back({.SourceId = std::move(Id), .Path = Public == PublicPaths.end() ? Path : Public->second,
					.Class = std::string(Object->GetClassName()), .Eligible = Eligible});
				Objects.push_back(std::move(Script));
			}
			for (const auto &Child : Object->Children) Self(Self, Child, Authored, InApplication);
			Path.pop_back();
		};
		for (const auto &Child : World->Children) Walk(Walk, Child, true, false);
		DiskFilesystem Filesystem(ProjectValue.Root);
		ResolveRuntimeDistributionPolicy(Filesystem, Plan);
		std::size_t SourceBytes = 0;
		for (std::size_t Index = 0; RetainSource && Index < Plan.Scripts.size(); ++Index) {
			auto &Script = Plan.Scripts[Index];
			if (!Script.Eligible) continue;
			Script.Source = Objects[Index]->GetSource();
			if (Script.Source.size() > MaximumScriptSourceBytes ||
				Script.Source.size() > MaximumProjectSnapshotBytes - SourceBytes)
				throw RuntimeDistributionError("InvalidPolicy", "The script capture exceeds its source byte bound.");
			SourceBytes += Script.Source.size();
		}
		return Plan;
	}

	GamePayload PackageBuilder::Capture(
		const Project &ProjectValue,
		const std::shared_ptr<DataModel> &World,
		std::uint64_t AuthoritativeRevision,
		std::uint64_t PersistedRevision,
		PackageAudience Audience,
		std::optional<RuntimeDistributionRequest> RuntimeDistribution
	) {
		AssertAuthoritativeMutation("PackageBuilder::Capture schema snapshot");
		if (!World || !ProjectValue.Identity.IsValid() || AuthoritativeRevision == 0 || PersistedRevision == 0)
			throw std::invalid_argument("Package snapshot source is invalid");
		const bool ManagedMultiplayer = RuntimeDistribution &&
			RuntimeDistribution->Target != RuntimeDistributionTarget::OfflinePlayer;
		const auto ApplicationEntry = World->GetApplicationEntry();

		bool SpatialContent = false;
		if (ManagedMultiplayer) {
			for (const auto &Node : World->GetDescendants())
				if (Node->IsA("BlockGrid") || Node->IsA("SpatialRegion")) {
					SpatialContent = true;
					break;
				}
		}
		auto Terrain = terrain::CaptureSceneTerrain(*World, ProjectValue.Identity);

		const auto TerrainManifest = Terrain ? std::optional(Terrain->ManifestDigest) : std::nullopt;
		auto Domains = terrain::CaptureDocumentTerrain(World, ProjectValue.Identity, Terrain ? Terrain->ClosureBytes : 0);
		const auto DomainBindings = terrain::GetDomainTerrainBindings(Domains);
		if ((Terrain || !Domains.empty()) && (Audience != PackageAudience::Trusted || ManagedMultiplayer) &&
			!(ManagedMultiplayer && SpatialContent))
			throw RuntimeDistributionError("UnsupportedTerrainTarget",
				"Terrain requires a trusted package or a managed Offline Player.");
		if (Audience != PackageAudience::Trusted || (RuntimeDistribution &&
			RuntimeDistribution->Target != RuntimeDistributionTarget::OfflinePlayer)) {
			try {
				for (const auto &Node : World->GetDescendants())
					DataModel::CheckDefaultNetworkSpatialParticipant(*Node,SpatialContent);
			} catch (const std::invalid_argument &Error) {
				throw RuntimeDistributionError("UnsupportedSpatialTarget", Error.what());
			}
		}
		bool Dirty = AuthoritativeRevision != PersistedRevision;
		if (!Dirty && (Terrain || !Domains.empty())) {
			terrain::DomainTerrainBindings Prior;
			Dirty = ProjectValue.GetPersistedTerrainManifest(&Prior) != TerrainManifest || Prior != DomainBindings;
		}
		auto Assets = std::dynamic_pointer_cast<AssetService>(World->FindFirstChildOfClass("AssetService", false));
		if (!Assets) throw std::runtime_error("Project has no canonical AssetService");
		std::optional<AssetRuntimeSnapshot> SharedSnapshot;
		if (SpatialContent && World->Filesystem && World->Filesystem->Exists(".gargantuan/package-audience.json")) {
			const auto SharedIds = client_package::ReadSharedAssets(*World->Filesystem);
			SharedSnapshot = Assets->CaptureRuntimeAssets(std::span<const AssetId>(SharedIds));
		}
		GamePayload Result{
			.Identity = ProjectValue.Identity,
			.DisplayName = World->GetName(),
			.AuthoritativeRevision = AuthoritativeRevision,
			.UnsavedChanges = Dirty,
			.PreRunSource = ReadProjectPreRunSource(ProjectValue.Root),
			.SchemaSnapshot = GetRuntimeSchemaLifecycle().GetActiveRegistry(),
			.TerrainArtifacts = Terrain && Audience == PackageAudience::Trusted ? std::move(Terrain->Artifacts) : std::vector<terrain::TerrainArtifact>{},
		};
		if (Audience == PackageAudience::Trusted && !Domains.empty()) {
			for (auto &[Node, Capture] : Domains)
				for (auto &Artifact : Capture.Artifacts) Result.TerrainArtifacts.push_back(std::move(Artifact));
			std::ranges::sort(Result.TerrainArtifacts, {}, &terrain::TerrainArtifact::Id);
			const auto Extra = std::ranges::unique(Result.TerrainArtifacts, {}, &terrain::TerrainArtifact::Id);
			Result.TerrainArtifacts.erase(Extra.begin(), Extra.end());
		}
		if (Result.DisplayName.empty() || Result.DisplayName.size() > MaximumDisplayNameBytes)
			throw std::runtime_error("Project display name exceeds the package bound");
		if (RuntimeDistribution) {
			const bool Distributed = RuntimeDistribution->Target == RuntimeDistributionTarget::DistributedPlayer;
			if (Distributed != (Audience == PackageAudience::DistributedPlayer))
				throw RuntimeDistributionError("AudienceViolation", "The runtime distribution target and package audience disagree.");
			Result.DistributionPlan = CaptureRuntimeDistribution(ProjectValue, World, *RuntimeDistribution, true);
			Result.DistributionPlan->SpatialContent = SpatialContent;
			if (SharedSnapshot) {
				const auto Catalog = Json::parse(SharedSnapshot->CatalogJson);
				for (const auto &Asset : Catalog.at("Assets"))
					Result.DistributionPlan->SharedGridAssets.emplace(Asset.at("AssetId").get<std::string>(), Asset.at("ContentId").get<std::string>());
			}
			if (SpatialContent && Terrain) {
				Result.DistributionPlan->WorkspaceTerrainManifest = TerrainManifest->ToString();
				Result.DistributionPlan->WorkspaceTerrainSpacingShift = Terrain->Index.Grid.SpacingShift;
			}
			if (RuntimeDistribution->Target != RuntimeDistributionTarget::OfflinePlayer) {
				Json Bindings = Json::array();
				auto ClientScripts = client_package::CaptureEligibleScripts(*World);
				std::ranges::sort(ClientScripts, {}, &RuntimeDistributionScript::SourceId);
				for (const auto &Script : ClientScripts)
					Bindings.push_back({Script.SourceId, Script.Path, Script.Class});
				Json Identity{
					{"Domain", SpatialContent ? "GargantuanSpatialClientCodeV1" : "GargantuanClientCodeV1"}, {"Project", Result.Identity.ToString()},
					{"Session", Result.DistributionPlan->SessionIdentity}, {"Scripts", std::move(Bindings)},
					{"Projection", HashText(client_package::CaptureCode(*World))},
				};
				if (!Result.DistributionPlan->SharedGridAssets.empty())
					Identity["SharedGridAssets"] = Result.DistributionPlan->SharedGridAssets;
				Result.DistributionPlan->ClientCodeIdentity = HashText(Identity.dump());
			}
			if (Result.PreRunSource && (RuntimeDistribution->Preset != RuntimeDistributionPreset::Open ||
				std::ranges::any_of(Result.DistributionPlan->Scripts, [](const auto &Script) {
					return Script.Eligible && Script.Representation != RuntimeDistributionRepresentation::Source;
				})))
				throw RuntimeDistributionError("UnsupportedRuntime", "Compiled and protected packages currently require native schema without PreRun.");
		}
		blockgrid::SegmentCapture Grids;
		if (Audience == PackageAudience::Trusted || !ApplicationEntry.empty()) {
			std::shared_ptr<Instance> Root = World;
			Result.ProjectJson = InstanceSerialization::Serialize(ProjectValue.InstanceFileFormat, Root, TerrainManifest, DomainBindings, &Grids);
			if (!ApplicationEntry.empty()) {
				auto Template = application_template::Extract(Result.ProjectJson, ApplicationEntry);
				if (!RuntimeDistribution || RuntimeDistribution->Target != RuntimeDistributionTarget::DedicatedServer)
					Result.ApplicationTemplate = std::move(Template);
			}
		}
		if (Audience == PackageAudience::DistributedPlayer) {
			if (!World->Filesystem || World->Root != ProjectValue.Root || !Result.SchemaSnapshot)
				throw std::invalid_argument("Distributed Player capture requires the loaded project origin");
			if (Result.PreRunSource || std::ranges::any_of(Result.SchemaSnapshot->EnumerateDefinitions(),
				[](const SchemaDefinition *Definition) {
					return GetSchemaDefinitionProvenance(*Definition) != SchemaProvenance::NativeEngine;
				}))
				throw std::runtime_error("Distributed Player packages currently require the native schema without PreRun");
			if (!SharedSnapshot) {
				const auto SharedAssets = client_package::ReadSharedAssets(*World->Filesystem);
				SharedSnapshot = Assets->CaptureRuntimeAssets(std::span<const AssetId>(SharedAssets));
			}
			if (Result.ApplicationTemplate)
				(void)application_template::SelectAssets(*Result.ApplicationTemplate, *SharedSnapshot);
			Result.ProjectJson.clear();
			Result.DistributedPlayer = std::shared_ptr<const DistributedPlayerCapture>(new DistributedPlayerCapture(
				Result.Identity, Result.AuthoritativeRevision, client_package::CaptureCode(*World,
					RuntimeDistribution ? client_package::GuiProjection::ManagedCatalog : Result.ApplicationTemplate ?
					client_package::GuiProjection::SourceBindings : client_package::GuiProjection::Omit),
				std::move(*SharedSnapshot)
			));
			return Result;
		}
		if (Audience != PackageAudience::Trusted) throw std::invalid_argument("Unknown package audience");
		Result.GridSegments=std::move(Grids.Parts);
		if (Result.ProjectJson.size() > MaximumProjectSnapshotBytes)
			throw std::length_error("Project snapshot exceeds the package byte limit");
		Result.Assets = Assets->CaptureRuntimeAssets();
		auto Meshes = SnapshotSpatialMeshes(Result.Assets);
		PartitionPackageContent(Result, Meshes);
		PackageContentManifest Manifest{{Result.Identity, Result.AuthoritativeRevision},
			PackageContentInstanceSchemaVersion, {}};
		ResidencySpatialMetadata Metadata{ResidencySpatialMetadataVersion, Manifest.Package, {}};
		for (const auto &Unit : Result.ContentUnits) {
			Manifest.Entries.push_back(Unit.Entry);
			if (auto Spatial = InspectSpatialContentUnit(Unit.Payload, Unit.Entry, Meshes, Result.SchemaSnapshot))
				Metadata.Units.push_back(std::move(*Spatial));
		}
		CheckSpatialProof(Manifest, Metadata);
		return Result;
	}

	PackageBuildResult PackageBuilder::Build(const PackageBuildRequest &Request) {
		const auto &Client = Request.Payload.DistributedPlayer;
		const auto &Plan = Request.Payload.DistributionPlan;
		const bool Distributed = static_cast<bool>(Client);
		const auto Audience = Distributed ? PackageAudience::DistributedPlayer : PackageAudience::Trusted;
		const auto &PackageAssets = Distributed ? Client->Assets : Request.Payload.Assets;
		PackageBuildResult Result{
			.Identity = Request.Payload.Identity,
			.PackagedRevision = Request.Payload.AuthoritativeRevision,
			.PackagedUnsavedChanges = Request.Payload.UnsavedChanges,
		};
		std::filesystem::path Candidate;
#if defined(_WIN32)
		windows_package_publication::OutputParents OutputParents;
#endif
		std::filesystem::path PrivateDiagnosticsCandidate;
		std::filesystem::path PrivateDiagnosticsTempDirectory;
		std::filesystem::path NewPrivateDiagnosticsPath;
		std::optional<runtime_distribution::Metadata> DistributionMetadata;
		RuntimePackageProvisionedSecurity DistributionSecurity;
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
				if (Request.Payload.ApplicationTemplate) {
					application_template::Validate(*Request.Payload.ApplicationTemplate);
				}
				if (!Request.Payload.SchemaSnapshot || !Request.Payload.Identity.IsValid() ||
					Request.Payload.AuthoritativeRevision == 0 ||
					Request.Payload.DisplayName.empty() ||
					Request.Payload.DisplayName.size() > MaximumDisplayNameBytes ||
					PackageAssets.CatalogJson.size() > 1024 * 1024)
					throw std::runtime_error("Package payload snapshot is invalid or exceeds its bounds");
				bool SpatialServerTerrain = false;
				if (!Request.Payload.TerrainArtifacts.empty() && Plan && Plan->SpatialContent && !Distributed &&
					Plan->Request.Target == RuntimeDistributionTarget::DedicatedServer &&
					Request.Configuration == PackageConfiguration::Development) {
					terrain::DomainTerrainBindings Domains;
					const auto Workspace = InstanceSerialization::InspectTerrainManifest(Request.Payload.ProjectJson, &Domains);
					SpatialServerTerrain = Workspace && (Workspace->has_value() || !Domains.empty()) &&
						(*Workspace ? (*Workspace)->ToString() : std::string{}) == Plan->WorkspaceTerrainManifest;
				}
				if (!Request.Payload.TerrainArtifacts.empty() && (Distributed ||
					(Plan && Plan->Request.Target != RuntimeDistributionTarget::OfflinePlayer && !SpatialServerTerrain)))
					throw RuntimeDistributionError("UnsupportedTerrainTarget",
						"Terrain requires a trusted package or a managed Offline Player.");
				if (Distributed) {
					if (!Request.Payload.ProjectJson.empty() || Request.Payload.PreRunSource ||
						!Request.Payload.GridSegments.empty() || !Request.Payload.ContentUnits.empty() || !Request.Payload.Assets.CatalogJson.empty() ||
						!Request.Payload.Assets.Artifacts.empty() || Client->Project != Request.Payload.Identity ||
						Client->Revision != Request.Payload.AuthoritativeRevision)
						throw std::runtime_error("Distributed Player package contains trusted project payload");
					client_package::ValidateCode(Client->Code);
					return;
				}
				if (Request.Payload.ProjectJson.empty() ||
					Request.Payload.ProjectJson.size() > MaximumProjectSnapshotBytes ||
					Request.Payload.ContentUnits.size() > MaximumPackageContentUnits ||
					(Request.Payload.ContentInstanceSchemaVersion != 4 &&
						Request.Payload.ContentInstanceSchemaVersion != PackageContentInstanceSchemaVersion) ||
					(Request.Payload.ContentInstanceSchemaVersion == 4 && Request.Payload.ContentUnits.empty()))
					throw std::runtime_error("Package payload snapshot is invalid or exceeds its bounds");
				PackageContentManifest ContentManifest{
					.Package = {Request.Payload.Identity, Request.Payload.AuthoritativeRevision},
					.InstanceSchemaVersion = Request.Payload.ContentInstanceSchemaVersion,
				};
				blockgrid::VerifySegments(InstanceSerialization::InspectGridSegments(Request.Payload.ProjectJson),Request.Payload.GridSegments);
				auto Meshes = SnapshotSpatialMeshes(Request.Payload.Assets);
				ResidencySpatialMetadata Metadata{ResidencySpatialMetadataVersion, ContentManifest.Package, {}};
				for (const auto &Unit : Request.Payload.ContentUnits) {
					if (Unit.Payload.size() != Unit.Entry.CompressedBytes ||
						Unit.Entry.UncompressedBytes != Unit.Entry.CompressedBytes ||
						AssetContentId::Hash(std::span(
							reinterpret_cast<const std::uint8_t *>(Unit.Payload.data()), Unit.Payload.size()
						)) != Unit.Entry.Digest)
						throw std::runtime_error("Package content unit does not match its immutable metadata");
					ContentManifest.Entries.push_back(Unit.Entry);
					if (auto Spatial = InspectSpatialContentUnit(
						Unit.Payload, Unit.Entry, Meshes, Request.Payload.SchemaSnapshot,
						Request.Payload.ContentInstanceSchemaVersion))
						Metadata.Units.push_back(std::move(*Spatial));
				}
				(void)ParsePackageContentManifest(EncodePackageContentManifest(ContentManifest)).value();
				CheckSpatialProof(ContentManifest, Metadata);
			});

			std::optional<RuntimeDistributionDescription> Distribution;
			std::optional<std::string> DistributedRuntimeDigest;
			std::optional<std::string> ManagedRuntimeDigest;
			std::filesystem::path Destination;
			auto CheckDestination = [&](const std::filesystem::path &Directory, bool VerifyExisting) {
				const auto Parent = Directory.parent_path();
				if (!std::filesystem::is_directory(Parent) || IsReparseOrLink(Parent))
					throw std::runtime_error("Package output parent is unavailable or redirected");
				if (std::filesystem::symlink_status(Directory).type() == std::filesystem::file_type::not_found) return;
				if (!std::filesystem::is_directory(Directory) || IsReparseOrLink(Directory))
					throw std::runtime_error("Package output destination is unavailable or redirected");
				if (std::filesystem::is_empty(Directory)) return;
				if (VerifyExisting) {
					if (!HasErrors(Validate(Directory, Audience, {
						.Security = Plan ? std::optional(DistributionSecurity) : std::nullopt,
						.VerifyExecutableArtifacts = false,
					}))) return;
				} else {
					std::vector<PackageDiagnostic> Diagnostics;
					const auto Existing = Inspect(Directory, Diagnostics);
					if (Existing && Existing->Audience == Audience) return;
				}
				throw std::runtime_error("Package output contains unrelated files or has a different audience");
			};
#if defined(_WIN32)
			auto Fingerprint = [&](const std::filesystem::path &Directory) {
				if (std::filesystem::is_empty(Directory)) return std::string("empty");
				return HashText(ReadBoundedText(Directory / L"game.package.json", MaximumManifestBytes));
			};
			auto Recover = [&] {
				windows_package_publication::Recover(Destination, [&](const std::filesystem::path &Directory) {
					CheckDestination(Directory, true);
					return Fingerprint(Directory);
				}, IsReparseOrLink, UniqueSibling);
			};
#endif
			RunPhase(PackagePhase::Validate, [&] {
				InjectFailure(Request.FailurePoint, PackageFailurePoint::Validate);
				if (!Request.RuntimeDistributionRoot.is_absolute() || !Request.OutputDirectory.is_absolute())
					throw std::runtime_error("Package roots must be absolute tooling inputs");
				Destination = Request.OutputDirectory.lexically_normal();
				if (Destination == Destination.root_path() || Destination.filename().empty())
					throw std::runtime_error("Package output destination is unsafe");
#if defined(_WIN32)
				OutputParents.Acquire(Destination.parent_path());
#endif
				CheckDestination(Destination, false);
				Distribution = ParseRuntimeDistribution(Request.RuntimeDistributionRoot, Result.Diagnostics);
				if (!Distribution) throw std::runtime_error("Runtime distribution validation failed");
				if (Plan) {
					const auto &Policy = Plan->Request;
					if (Distributed != (Policy.Target == RuntimeDistributionTarget::DistributedPlayer) ||
						(Policy.Target == RuntimeDistributionTarget::DedicatedServer ? !IsServerExecutable(Distribution->Player) :
							!IsPlayerExecutable(Distribution->Player)))
						throw RuntimeDistributionError("AudienceViolation", "Package target does not match the selected runtime closure.");
					ManagedRuntimeDigest = ReadRuntimeDistributionDigest(Request.RuntimeDistributionRoot);
					if (Policy.RequirePublisher || Policy.KeyAccess != RuntimeDistributionKeyAccess::None)
						DistributionSecurity = Request.DistributionSecurity ? *Request.DistributionSecurity :
							LoadProvisionedRuntimePackageSecurity(RuntimePackageProvisioningUse::Build);
					if (Policy.RequirePublisher && (!IsRuntimePackageCryptoAvailable() || !DistributionSecurity.Signer))
						throw RuntimeDistributionError("SigningFailed", "A native publisher signer is unavailable.");
					Result.Target = Policy.Target;
				}
				if (Distributed) {
					if (!IsPlayerExecutable(Distribution->Player) ||
						std::ranges::any_of(Distribution->Files, [](const auto &File) {
							return !IsDistributedEngineFile(File.Path, File.Category);
						}))
						throw std::runtime_error("Distributed Player requires an Engine Player-only runtime distribution");
					DistributedRuntimeDigest = ReadDistributedPlayerDigest(Request.RuntimeDistributionRoot);
				}
#if defined(_WIN32)
				if (windows_package_publication::HasEntry(windows_package_publication::Journal(Destination))) {
					PackagePublication Publication(Destination);
					Recover();
				}
#endif
				auto NewCandidate = UniqueSibling(Destination, "gargantuan-package-tmp");
				ValidateStagingPaths(Destination, NewCandidate, *Distribution, Request.Payload, PackageAssets);
				if (!std::filesystem::create_directory(NewCandidate))
					throw std::runtime_error("Could not reserve a private package staging directory");
				Candidate = std::move(NewCandidate);
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
				if (DistributedRuntimeDigest) {
					const auto PlayerFile = std::ranges::find(Entries, Distribution->Player, &ContentEntry::Path);
					if (PlayerFile == Entries.end() || PlayerFile->Sha256 != *DistributedRuntimeDigest)
						throw std::runtime_error("Distributed Player capability does not match the staged executable");
				}
				if (ManagedRuntimeDigest) {
					const auto Executable = std::ranges::find(Entries, Distribution->Player, &ContentEntry::Path);
					if (Executable == Entries.end() || Executable->Sha256 != *ManagedRuntimeDigest)
						throw RuntimeDistributionError("UnsupportedRuntime", "The runtime capability does not match the staged executable.");
					const auto &Policy = Plan->Request;
					const auto Project = Request.Payload.Identity.ToString();
					Json Identity{
						{"ProjectId", Project}, {"Revision", Request.Payload.AuthoritativeRevision},
						{"Target", GetRuntimeDistributionName(Policy.Target)}, {"Executable", Executable->Sha256},
						{"Project", HashText(Distributed ? Client->Code : Request.Payload.ProjectJson)},
						{"AssetCatalog", HashText(PackageAssets.CatalogJson)}, {"SigningKey", Policy.SigningKeyId},
						{"KeyAccess", GetRuntimeDistributionName(Policy.KeyAccess)}, {"ContentKey", Policy.ContentKeyId},
						{"Compiler", GetRuntimeScriptCompilerIdentity()},
						{"ClientCodeIdentity", Plan->ClientCodeIdentity},
					};
					std::string ReleaseId;
					if (std::ranges::any_of(Plan->Scripts, [](const auto &Script) {
						return Script.Eligible && Script.Representation == RuntimeDistributionRepresentation::Protected;
					})) {
						std::array<unsigned char, 32> Random{};
						if (GenerateRuntimePackageReleaseSalt(Random) != RuntimePackageCryptoStatus::Ok)
							throw RuntimeDistributionError("EncryptionFailed", "A secure release identity could not be generated.");
						ReleaseId = HashText({reinterpret_cast<const char *>(Random.data()), Random.size()});
					} else {
						Json Scripts = Json::array();
						for (const auto &Script : Plan->Scripts)
							if (Script.Eligible)
								Scripts.push_back({Script.SourceId, GetRuntimeDistributionName(Script.Representation),
									GetRuntimeDistributionName(Script.Diagnostics), HashText(Script.Source)});
						Identity["Scripts"] = std::move(Scripts);
						ReleaseId = HashText(Identity.dump());
					}
					DistributionMetadata = runtime_distribution::Metadata{
						.Target = Policy.Target, .KeyAccess = Policy.KeyAccess, .ReleaseId = std::move(ReleaseId),
						.PublisherId = Project, .SigningKeyId = Policy.SigningKeyId,
						.ContentKeyId = Policy.ContentKeyId, .Compiler = std::string(GetRuntimeScriptCompilerIdentity()),
						.ClientCodeIdentity = Plan->ClientCodeIdentity,
						.SpatialContent = Plan->SpatialContent,
						.SharedGridAssets = Plan->SharedGridAssets,
						.WorkspaceTerrainManifest = Plan->WorkspaceTerrainManifest,
						.WorkspaceTerrainSpacingShift = Plan->WorkspaceTerrainSpacingShift,
					};
					Result.PlayerExecutableSha256 = Executable->Sha256;
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
				if (Plan) {
					auto Index = runtime_distribution::BuildCatalog(*Plan, *DistributionMetadata,
						Request.Payload.Identity.ToString(), DistributionSecurity, [&](std::string Relative, std::string_view Bytes) {
							if (Request.Cancellation.IsCancelled()) throw std::runtime_error("Package build cancelled");
							AddText(std::move(Relative), "Project", Bytes);
						});
					AddText(std::string(runtime_distribution::CodeCatalogPath), "Project", Index);
				}
				std::optional<RuntimeDistributionPlan> Gameplay;
				const auto *WorldPlan = Plan ? &*Plan : nullptr;
				if (Request.Payload.ApplicationTemplate && IsPlayerExecutable(Distribution->Player)) {
					auto Template = *Request.Payload.ApplicationTemplate;
					if (Plan) {
						const auto Bindings = application_template::GetBindings(Template);
						RuntimeDistributionPlan Application;
						Gameplay.emplace();
						for (const auto &Script : Plan->Scripts) {
							const auto Match = Bindings.find(Script.SourceId);
							if (Match == Bindings.end()) Gameplay->Scripts.push_back(Script);
							else {
								if (!Script.Eligible || Match->second.Path != Script.Path || Match->second.Class != Script.Class ||
									Match->second.Source != Script.Source)
									throw RuntimeDistributionError("AudienceViolation", "Application script projection disagrees with the policy.");
								Application.Scripts.push_back(Script);
							}
						}
						if (Application.Scripts.size() != Bindings.size())
							throw RuntimeDistributionError("AudienceViolation", "Application script policy is incomplete.");
						auto Document = Json::parse(Template);
						Document["Template"] = Json::parse(runtime_distribution::RemoveCompiledSource(
							Document.at("Template").dump(), Application, false));
						Template = Document.dump();
						WorldPlan = &*Gameplay;
					}
					AddText(std::string(application_template::Path), "Project", Template);
				}
				if (Distributed) {
					AddText(std::string(client_package::CodePath), "Project",
						WorldPlan ? runtime_distribution::RemoveCompiledSource(Client->Code, *WorldPlan, true) : Client->Code);
				} else {
					AddText("content/game.instance.json", "Project", WorldPlan ?
						runtime_distribution::RemoveCompiledSource(Request.Payload.ProjectJson, *WorldPlan, false) :
						Request.Payload.ProjectJson);
					PackageContentManifest ContentManifest{
						.Package = {Request.Payload.Identity, Request.Payload.AuthoritativeRevision},
						.InstanceSchemaVersion = Request.Payload.ContentInstanceSchemaVersion,
					};
					for (const auto &Unit : Request.Payload.ContentUnits) {
						ContentManifest.Entries.push_back(Unit.Entry);
						AddText(Unit.Entry.BlobReference, "Project", Unit.Payload);
					}
					AddText("content/content.manifest.json", "Project", EncodePackageContentManifest(ContentManifest));
					if (Request.Payload.PreRunSource)
						AddText("content/prerun.luau", "Project", *Request.Payload.PreRunSource);
				}
				AddText(std::string(client_package::CatalogPath), "Asset", PackageAssets.CatalogJson);
				for (const auto &Artifact : PackageAssets.Artifacts) {
					if (!Artifact.Bytes || !IsSafeRelativePath(Artifact.RelativePath) ||
						!std::string_view(Artifact.RelativePath).starts_with("assets/artifacts/"))
						throw std::runtime_error("Runtime asset snapshot contains an invalid artifact");
					const auto Relative = "content/" + Artifact.RelativePath;
					auto Target = ResolvePackageFile(Candidate, Relative, false);
					if (!Target) throw std::runtime_error("Runtime asset package path is unsafe");
					Entries.push_back(WriteAndHash(*Target, Relative, "Asset", *Artifact.Bytes));
				}
				std::size_t TerrainBytes = 0;
				for (const auto& [Digest,Part]:Request.Payload.GridSegments) {
					if (Request.Cancellation.IsCancelled()) throw std::runtime_error("Package build cancelled");
					const auto Relative=PackageGridPath(Digest);
					const auto Target=ResolvePackageFile(Candidate,Relative,false);
					if (!Target || !Part.Bytes || AssetContentId::Hash(*Part.Bytes)!=Digest)
						throw std::runtime_error("[BlockGrid:Package] Invalid immutable grid segment");
					Entries.push_back(WriteAndHash(*Target,Relative,"Project",*Part.Bytes));
				}
				for (const auto &Artifact : Request.Payload.TerrainArtifacts) {
					if (Request.Cancellation.IsCancelled()) throw std::runtime_error("Package build cancelled");
					if (!Artifact.Bytes || Artifact.Bytes->size() > terrain::MaximumTerrainClosureBytes - TerrainBytes ||
						AssetContentId::Hash(*Artifact.Bytes) != Artifact.Id.Digest)
						throw terrain::TerrainPersistenceFailure(terrain::TerrainPersistenceError::CorruptArtifact);
					TerrainBytes += Artifact.Bytes->size();
					const auto Relative = GetPackageTerrainPath(Artifact.Id);
					auto Target = ResolvePackageFile(Candidate, Relative, false);
					if (!Target) throw std::runtime_error("Terrain package path is unsafe");
					Entries.push_back(WriteAndHash(*Target, Relative, "Project", *Artifact.Bytes));
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
				Json Startup = Distributed ? Json{
					{"ClientCode", client_package::CodePath},
					{"AssetCatalog", client_package::CatalogPath},
				} : Json{
					{"Project", "content/game.instance.json"},
					{"ContentManifest", "content/content.manifest.json"},
					{"AssetCatalog", "content/assets/catalog.json"},
					{"PreRun", Request.Payload.PreRunSource ? Json("content/prerun.luau") : Json(nullptr)},
				};
				const bool WithApplication = Request.Payload.ApplicationTemplate && IsPlayerExecutable(Distribution->Player);
				if (WithApplication) Startup["Application"] = application_template::Path;
				Json Manifest{
					{"Format", PackageFormat},
					{"PackageFormatVersion", WithApplication ?
						(Plan ? ManagedApplicationPackageFormatVersion : Distributed ? DistributedApplicationPackageFormatVersion : StandalonePackageFormatVersion) :
						(Plan ? RuntimeDistributionPackageFormatVersion : Distributed ? DistributedPlayerPackageFormatVersion : GamePackageFormatVersion)},
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
				if (Distributed) Manifest["Audience"] = GetPackageAudienceName(Audience);
				if (DistributionMetadata) Manifest["RuntimeDistribution"] = runtime_distribution::EncodeMetadata(*DistributionMetadata);
				auto Encoded = JsonCodec::Encode(Manifest, "game package manifest");
				if (!Encoded || Encoded->size() > MaximumManifestBytes)
					throw std::runtime_error("Generated package manifest exceeds its bound");
				InjectFailure(Request.FailurePoint, PackageFailurePoint::ManifestWrite);
				std::ofstream Output(Candidate / "game.package.json", std::ios::binary | std::ios::trunc);
				if (!Output) throw std::runtime_error("Could not create the package manifest");
				Output.write(Encoded->data(), static_cast<std::streamsize>(Encoded->size()));
				Output.close();
				if (!Output) throw std::runtime_error("Could not finish the package manifest");
				Result.ManifestSha256 = HashText(*Encoded);
				Result.Size.OtherBytes += Encoded->size();
				Result.Size.TotalBytes = Aggregate + Encoded->size();
				if (DistributionMetadata && !DistributionMetadata->SigningKeyId.empty()) {
					const auto &Metadata = *DistributionMetadata;
					const auto Project = Request.Payload.Identity.ToString();
					RuntimePackageSignature Signature;
					RuntimePackageCryptoStatus SignedStatus;
					try {
						SignedStatus = DistributionSecurity.Signer->Sign({Project, Metadata.ReleaseId, Metadata.PublisherId,
							Metadata.SigningKeyId}, *Encoded, Signature);
					} catch (...) {
						throw RuntimeDistributionError("SigningFailed", "The package publisher signing provider failed.");
					}
					if (SignedStatus != RuntimePackageCryptoStatus::Ok)
						throw RuntimeDistributionError("SigningFailed", "The package publisher could not sign this release.");
					std::ofstream Signed(Candidate / runtime_distribution::SignaturePath, std::ios::binary | std::ios::trunc);
					Signed.write(reinterpret_cast<const char *>(Signature.data()), static_cast<std::streamsize>(Signature.size()));
					Signed.close();
					if (!Signed) throw RuntimeDistributionError("SigningFailed", "The package signature could not be staged.");
					Result.Size.OtherBytes += Signature.size();
					Result.Size.TotalBytes += Signature.size();
				}
				auto CandidateDiagnostics = Validate(Candidate, Audience, {
					.ExpectedManifestSha256 = Plan ? std::optional(Result.ManifestSha256) : std::nullopt,
					.Security = Plan ? std::optional(DistributionSecurity) : std::nullopt,
				});
				Result.Diagnostics.insert(
					Result.Diagnostics.end(), CandidateDiagnostics.begin(), CandidateDiagnostics.end()
				);
				if (HasErrors(CandidateDiagnostics)) throw std::runtime_error("Generated package failed validation");
			});

			RunPhase(PackagePhase::Finalize, [&] {
				InjectFailure(Request.FailurePoint, PackageFailurePoint::Finalize);
				if (Request.Cancellation.IsCancelled()) throw std::runtime_error("Package build cancelled");
				PackagePublication Publication(Destination);
#if defined(_WIN32)
				Recover();
#endif
				CheckDestination(Destination, true);
				std::filesystem::path PrivateDiagnosticsPath;
				if (Plan && DistributionMetadata) {
					Json Scripts = Json::array();
					for (const auto &Script : Plan->Scripts) {
						if (!Script.Eligible || Script.Diagnostics != RuntimeDistributionDiagnostics::PrivateSymbols) continue;
						Scripts.push_back({
							{"Id", Script.SourceId}, {"Path", Script.Path}, {"Class", Script.Class},
							{"SourceSha256", HashText(Script.Source)},
						});
					}
					if (!Scripts.empty()) {
						const auto Symbols = Json{
							{"Format", "GargantuanPrivateScriptSymbols"}, {"Version", 1u},
							{"ProjectId", Request.Payload.Identity.ToString()}, {"Revision", Request.Payload.AuthoritativeRevision},
							{"ReleaseId", DistributionMetadata->ReleaseId}, {"ManifestSha256", Result.ManifestSha256},
							{"Compiler", DistributionMetadata->Compiler}, {"Scripts", std::move(Scripts)},
						}.dump();
						if (Symbols.size() > runtime_distribution::MaximumCatalogBytes)
							throw RuntimeDistributionError("PrivateDiagnosticsFailed", "The private diagnostic index exceeds its bound.");
						PrivateDiagnosticsPath = Destination.parent_path() /
							("gargantuan-private-symbols-" + HashText(Paths::ToUtf8(Destination.filename())).substr(0, 16) +
								"-" + Result.ManifestSha256 + ".json");
						if (std::filesystem::exists(PrivateDiagnosticsPath)) {
							if (IsReparseOrLink(PrivateDiagnosticsPath) ||
								ReadBoundedText(PrivateDiagnosticsPath, runtime_distribution::MaximumCatalogBytes) != Symbols)
								throw RuntimeDistributionError("PrivateDiagnosticsFailed", "A conflicting private diagnostic artifact already exists.");
						} else {
							PrivateDiagnosticsTempDirectory = UniqueSibling(PrivateDiagnosticsPath, "gargantuan-symbols-tmp");
							if (!std::filesystem::create_directory(PrivateDiagnosticsTempDirectory)) {
								PrivateDiagnosticsTempDirectory.clear();
								throw RuntimeDistributionError("PrivateDiagnosticsFailed", "The private diagnostics staging directory is unavailable.");
							}
#if !defined(_WIN32)
							std::filesystem::permissions(PrivateDiagnosticsTempDirectory, std::filesystem::perms::owner_all);
#endif
							PrivateDiagnosticsCandidate = PrivateDiagnosticsTempDirectory / "symbols.json";
							std::ofstream Index(PrivateDiagnosticsCandidate, std::ios::binary | std::ios::trunc);
							if (!Index)
								throw RuntimeDistributionError("PrivateDiagnosticsFailed", "The private diagnostic artifact could not be opened.");
#if !defined(_WIN32)
							std::filesystem::permissions(PrivateDiagnosticsCandidate,
								std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
#endif
							Index.write(Symbols.data(), static_cast<std::streamsize>(Symbols.size()));
							Index.close();
							if (!Index)
								throw RuntimeDistributionError("PrivateDiagnosticsFailed", "The private diagnostic artifact could not be written.");
							std::filesystem::create_hard_link(PrivateDiagnosticsCandidate, PrivateDiagnosticsPath);
							NewPrivateDiagnosticsPath = PrivateDiagnosticsPath;
							std::filesystem::remove(PrivateDiagnosticsCandidate);
							PrivateDiagnosticsCandidate.clear();
							std::filesystem::remove(PrivateDiagnosticsTempDirectory);
							PrivateDiagnosticsTempDirectory.clear();
						}
					}
				}
#if defined(_WIN32)
				const auto Previous = windows_package_publication::HasEntry(Destination) ? Fingerprint(Destination) : std::string{};
				if (!windows_package_publication::Install(Destination, Candidate, Previous, Result.ManifestSha256,
					Request.Cancellation, UniqueSibling))
					AddDiagnostic(Result.Diagnostics, PackageDiagnosticSeverity::Warning, "Filesystem",
						"BackupCleanup", "The validated package was installed, but prior backup cleanup did not complete.");
#else
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
				if (!Backup.empty()) {
					std::error_code Cleanup;
					std::filesystem::remove_all(Backup, Cleanup);
					if (Cleanup)
						AddDiagnostic(Result.Diagnostics, PackageDiagnosticSeverity::Warning, "Filesystem",
							"BackupCleanup", "The validated package was installed, but prior backup cleanup did not complete.");
				}
#endif
				Result.OutputDirectory = Destination;
				Result.PlayerExecutable = Destination / std::filesystem::path(Distribution->Player);
				Result.PrivateDiagnosticsPath = std::move(PrivateDiagnosticsPath);
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
			const auto *DistributionError = dynamic_cast<const RuntimeDistributionError *>(&Error);
			Result.Cancelled = Request.Cancellation.IsCancelled() ||
							   std::string_view(Error.what()) == "Package build cancelled";
			std::string FailureDetail = std::string(GetPackagePhaseName(CurrentPhase)) + ": ";
			if (const auto *FilesystemError = dynamic_cast<const std::filesystem::filesystem_error *>(&Error))
				FailureDetail += FilesystemError->code().message();
			else
				FailureDetail += Error.what();
			if (DistributionError && !DistributionError->Item.empty())
				FailureDetail += " [" + DistributionError->Item + "]";
			AddDiagnostic(
				Result.Diagnostics,
				PackageDiagnosticSeverity::Error,
				"Packaging",
				Result.Cancelled ? "Cancelled" : (DistributionError ? DistributionError->Code : "BuildFailed"),
				DistributionError && DistributionError->Code == "PublicationRecoveryRequired"
					? "The package was not installed. The previous validated package is in the recovery directory."
					: Result.Cancelled ? "The package build was cancelled before finalization."
								 : "The package build failed; the previous valid destination was preserved.",
				std::move(FailureDetail)
			);
			if (!Candidate.empty()) {
				std::error_code Ignored;
				std::filesystem::remove_all(Candidate, Ignored);
			}
			std::error_code Ignored;
			if (!PrivateDiagnosticsCandidate.empty()) std::filesystem::remove(PrivateDiagnosticsCandidate, Ignored);
			if (!PrivateDiagnosticsTempDirectory.empty()) std::filesystem::remove(PrivateDiagnosticsTempDirectory, Ignored);
			if (!NewPrivateDiagnosticsPath.empty() && !Candidate.empty())
				std::filesystem::remove(NewPrivateDiagnosticsPath, Ignored);
		}
		return Result;
	}

	static terrain::TerrainArtifact PinClientTerrainManifest(const std::filesystem::path &Root,
		const ParsedPackageManifest &Package, AssetContentId Digest, std::set<std::string> &DeferredChunks) {
		using enum terrain::TerrainPersistenceError;
		const terrain::TerrainArtifactId Id{terrain::TerrainArtifactPurpose::Manifest, Digest};
		const auto ManifestPath = GetPackageTerrainPath(Id);
		const auto *ManifestPin = FindContentEntry(Package, ManifestPath);
		if (!ManifestPin || ManifestPin->Category != "Project" || ManifestPin->Sha256 != Digest.ToString())
			throw terrain::TerrainPersistenceFailure(InvalidBinding);
		constexpr auto MaximumPinnedManifestBytes = terrain::TerrainManifestHeaderBytes +
			terrain::MaximumStartupSourceChunks * terrain::TerrainManifestEntryBytes;
		const auto Text = ReadPinnedText(Root, Package, ManifestPath, MaximumPinnedManifestBytes);
		auto Bytes = std::make_shared<const std::vector<std::uint8_t>>(Text.begin(), Text.end());
		const auto Project = Package.Inspection.Identity;
		const auto Directory = terrain::DecodeTerrainManifest(*Bytes, Project, terrain::GetTerrainDatasetId(Project));
		if (!Directory) throw terrain::TerrainPersistenceFailure(CorruptArtifact);
		std::size_t ClosureBytes = Bytes->size();
		for (const auto &Chunk : Directory->Chunks) {
			const auto ChunkDigest = AssetContentId{Chunk.Digest.Bytes};
			const auto Path = GetPackageTerrainPath({terrain::TerrainArtifactPurpose::Chunk, ChunkDigest});
			const auto *Pin = FindContentEntry(Package, Path);
			if (!Pin || Pin->Category != "Project" || Pin->Size != Chunk.ByteCount || Pin->Sha256 != ChunkDigest.ToString() ||
				Path == Package.ProjectPath || Path == Package.AssetCatalogPath || Path == Package.PreRunPath ||
				Path == Package.ContentManifestPath || Root / Path == Package.Inspection.PlayerExecutable ||
				!DeferredChunks.insert(Path).second)
				throw terrain::TerrainPersistenceFailure(InvalidBinding);
			if (Chunk.ByteCount > terrain::MaximumTerrainClosureBytes - ClosureBytes)
				throw terrain::TerrainPersistenceFailure(LimitExceeded);
			ClosureBytes += Chunk.ByteCount;
		}
		return {Id, std::move(Bytes)};
	}


	static std::string ReadPinnedPackageFile(const std::filesystem::path &PackageRoot, const ParsedPackageManifest &Manifest,
		std::string_view RelativePath, std::size_t MaximumBytes) {
		const auto Entry = std::ranges::find(Manifest.Content, RelativePath, &ContentEntry::Path);
		const auto Path = ResolvePackageFile(PackageRoot, RelativePath, true);
		if (Entry == Manifest.Content.end() || !Path)
			throw RuntimeDistributionError("InvalidPackage", "The package payload has no pinned content entry.");
		auto Bytes = ReadBoundedText(*Path, MaximumBytes);
		if (Bytes.size() != Entry->Size || HashText(Bytes) != Entry->Sha256)
			throw RuntimeDistributionError("InvalidPackage", "The package payload changed after validation.");
		return Bytes;
	}

	static RuntimeDistributionCodeCatalog ReadDistributionCatalog(const std::filesystem::path &PackageRoot,
		const ParsedPackageManifest &Manifest) {
		auto Catalog = runtime_distribution::DecodeCatalog(ReadPinnedPackageFile(PackageRoot, Manifest,
			runtime_distribution::CodeCatalogPath, runtime_distribution::MaximumCatalogBytes));
		std::set<std::string, std::less<>> Links;
		for (const auto &[Id, Script] : Catalog.Scripts) {
			if (Script.Representation == RuntimeDistributionRepresentation::Source) continue;
			auto Path = "content/scripts/" + Id +
				(Script.Representation == RuntimeDistributionRepresentation::Protected ? ".gse" : ".luac");
			const auto Match = std::ranges::find(Manifest.Content, Path, &ContentEntry::Path);
			if (Match == Manifest.Content.end() || Match->Category != "Project" || Match->Size == 0 ||
				Match->Size > MaximumRuntimePackageScriptContainerBytes)
				throw RuntimeDistributionError("InvalidScriptCatalog", "A runtime script artifact is missing or exceeds its bound.");
			Links.insert(std::move(Path));
		}
		for (const auto &Entry : Manifest.Content)
			if (std::string_view(Entry.Path).starts_with("content/scripts/") && !Links.contains(Entry.Path))
				throw RuntimeDistributionError("InvalidScriptCatalog", "The package contains an unexpected runtime script artifact.");
		const auto Application = Manifest.HasApplication ? application_template::GetBindings(ReadPinnedPackageFile(
			PackageRoot, Manifest, application_template::Path, application_template::MaximumBytes)) : application_template::Bindings{};
		for (const auto &[Id, Binding] : Application) {
			const auto Found = Catalog.Scripts.find(Id);
			if (Found == Catalog.Scripts.end() || Found->second.Class != Binding.Class || Found->second.Path != Binding.Path ||
				(Found->second.Representation != RuntimeDistributionRepresentation::Source && !Binding.Source.empty()))
				throw RuntimeDistributionError("AudienceViolation", "Application code differs from its authorized catalog.");
		}
		if (Manifest.Inspection.Audience == PackageAudience::DistributedPlayer) {
			const auto Projection = client_package::DecodeCode(ReadPinnedPackageFile(PackageRoot, Manifest,
				Manifest.ProjectPath, client_package::MaximumCodeBytes));
			std::set<client_package::CodeKey> Bound;
			if (Projection.size() + Application.size() != Catalog.Scripts.size())
				throw RuntimeDistributionError("AudienceViolation", "The script catalog and audience projection disagree.");
			for (const auto &[Id, Script] : Catalog.Scripts) {
				if (Application.contains(Id)) continue;
				auto Key = client_package::CodeKey{Script.Path, Script.Class};
				const auto Found = Projection.find(Key);
				if (Found == Projection.end() || !Bound.insert(std::move(Key)).second ||
					(Script.Representation != RuntimeDistributionRepresentation::Source && !Found->second.empty()))
					throw RuntimeDistributionError("AudienceViolation", "The audience projection has an incompatible script entry.");
			}
		}
		return Catalog;
	}

	static std::vector<PackageDiagnostic> ValidatePackageRoot(const std::filesystem::path &PackageRoot,
		std::optional<ParsedPackageManifest> *VerifiedManifest,
		std::optional<PackageAudience> ExpectedAudience = std::nullopt,
		const RuntimeDistributionAdmission &Admission = {},
		std::vector<terrain::TerrainArtifact> *TerrainArtifacts = nullptr,
		PackageRuntimeLoadMode Mode = PackageRuntimeLoadMode::Complete,blockgrid::SegmentMap* GridSegments=nullptr) {
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
		auto Manifest = ParseManifest(PackageRoot, Diagnostics, true, Admission, Mode);
		if (!Manifest) return Diagnostics;
		if (Manifest->RuntimeDistribution) {
			try {
				auto Catalog = ReadDistributionCatalog(PackageRoot, *Manifest);
				if (!VerifiedManifest && Admission.VerifyExecutableArtifacts)
					runtime_distribution::AdmitCatalog(Catalog, *Manifest->RuntimeDistribution,
						Manifest->Inspection.Identity.ToString(), Manifest->DistributionSecurity, Manifest->TrustedCompiler,
						[&](std::string_view Path, std::size_t Bound) {
							return ReadPinnedPackageFile(PackageRoot, *Manifest, Path, Bound);
						});
				if (VerifiedManifest) Manifest->ScriptCatalog = std::move(Catalog);
			} catch (const RuntimeDistributionError &Error) {
				AddDiagnostic(Diagnostics, PackageDiagnosticSeverity::Error, "RuntimeDistribution", Error.Code, Error.what());
				return Diagnostics;
			} catch (const std::exception &) {
				AddDiagnostic(Diagnostics, PackageDiagnosticSeverity::Error, "RuntimeDistribution", "InvalidScriptCatalog",
					"The runtime script catalog could not be validated safely.");
				return Diagnostics;
			}
		}
		if (ExpectedAudience && Manifest->Inspection.Audience != *ExpectedAudience) {
			AddDiagnostic(Diagnostics, PackageDiagnosticSeverity::Error, "Validation", "AudienceMismatch",
				"The package has a different audience from the one requested.");
			return Diagnostics;
		}
		const bool Distributed = Manifest->Inspection.Audience == PackageAudience::DistributedPlayer;
		const bool IsLoopbackClient = CanDeferTerrain(*Manifest, Mode);
		std::optional<AssetContentId> TerrainDigest;
		terrain::DomainTerrainBindings DomainBindings;
		blockgrid::SegmentRequirements GridRequirements;
		try {
			if (Manifest->HasApplication)
				application_template::Validate(ReadPinnedPackageFile(PackageRoot, *Manifest, application_template::Path, application_template::MaximumBytes));
			if (Distributed) {
				client_package::ValidateCode(ReadPinnedPackageFile(
					PackageRoot, *Manifest, Manifest->ProjectPath, client_package::MaximumCodeBytes));
			} else {

				const auto ProjectText = ReadPinnedText(PackageRoot, *Manifest, Manifest->ProjectPath, MaximumProjectSnapshotBytes);
				auto ProjectJson = JsonCodec::Parse(ProjectText, MaximumProjectSnapshotBytes, "packaged project snapshot");
				if (!ProjectJson || !ProjectJson->is_object() || !ProjectJson->contains("Version") ||
					!(*ProjectJson)["Version"].is_number_unsigned() || !ProjectJson->contains("ClassName") ||
					!(*ProjectJson)["ClassName"].is_string() || (*ProjectJson)["ClassName"] != "DataModel")
					throw std::runtime_error("Packaged project snapshot is malformed");
				const auto Digest = InstanceSerialization::InspectTerrainManifest(ProjectText, &DomainBindings,&GridRequirements);
				if (!Digest) throw std::runtime_error(Digest.error());
				TerrainDigest = *Digest;
			}
		} catch (const std::exception &) {
			AddDiagnostic(
				Diagnostics,
				PackageDiagnosticSeverity::Error,
				"Validation",
				Distributed ? "InvalidClientCode" : "InvalidProject",
				"The packaged project payload is malformed or exceeds its bounds."
			);
		}
		const bool SpatialServerTerrain = Manifest->RuntimeDistribution && Manifest->RuntimeDistribution->SpatialContent &&
			Manifest->Inspection.Configuration == PackageConfiguration::Development &&
			Manifest->Inspection.Target == RuntimeDistributionTarget::DedicatedServer;
		if (SpatialServerTerrain &&
			(TerrainDigest ? TerrainDigest->ToString() : std::string{}) != Manifest->RuntimeDistribution->WorkspaceTerrainManifest) {
			AddDiagnostic(Diagnostics,PackageDiagnosticSeverity::Error,"Content","InvalidTerrainClosure","Spatial server terrain does not match its package binding.");
			return Diagnostics;
		}
		if ((!DomainBindings.empty() && IsLoopbackClient) || ((TerrainDigest || !DomainBindings.empty()) &&
			Manifest->Inspection.Target && Manifest->Inspection.Target != RuntimeDistributionTarget::OfflinePlayer && !SpatialServerTerrain)) {
			AddDiagnostic(Diagnostics, PackageDiagnosticSeverity::Error, "Content", "UnsupportedTerrainTarget",
				"Terrain requires a trusted package or a managed Offline Player.");
			return Diagnostics;
		}
		std::set<std::string> DeferredChunks;
		if (!HasErrors(Diagnostics)) try {
			const auto Count=std::ranges::count_if(Manifest->Content,[](const ContentEntry& E) {
				return E.Path.starts_with("content/.gargantuan/blockgrid/");
			});
			if (std::size_t(Count)!=GridRequirements.Parts.size()) throw std::invalid_argument("Unexpected grid segment count");
			const auto Parts=blockgrid::ReadSegments(GridRequirements,[&](const AssetContentId& Id,std::size_t Size) {
				const auto Path=PackageGridPath(Id);
				const auto Entry=std::ranges::find(Manifest->Content,Path,&ContentEntry::Path);
				if (Entry==Manifest->Content.end() || Entry->Category!="Project" || Entry->Size!=Size || Entry->Sha256!=Id.ToString())
					throw std::invalid_argument("Grid segment differs from its mandatory package pin");
				const auto Bytes=ReadPinnedPackageFile(PackageRoot,*Manifest,Path,Size);
				return blockgrid::Segment{std::make_shared<const blockgrid::SegmentData>(Bytes.begin(),Bytes.end())};
			});
			if (GridSegments) *GridSegments=Parts;
		} catch (const std::exception& Error) {
			AddDiagnostic(Diagnostics,PackageDiagnosticSeverity::Error,"Content","InvalidGridClosure","A packaged grid segment failed verification.",Error.what());
		}
		if (!HasErrors(Diagnostics)) {
			try {
				std::size_t Admitted = 0;
				if (TerrainDigest && IsLoopbackClient) {
					auto Pin = PinClientTerrainManifest(PackageRoot, *Manifest, *TerrainDigest, DeferredChunks);
					Admitted = DeferredChunks.size() + 1;
					if (TerrainArtifacts) TerrainArtifacts->push_back(std::move(Pin));
				} else if (TerrainDigest || !DomainBindings.empty()) {
					std::set<terrain::TerrainArtifactId> AdmittedIds;
					const auto Reader = terrain::BudgetDocumentTerrainReads([&](const terrain::TerrainArtifactId &Id, std::size_t Maximum) ->
						std::expected<std::vector<std::uint8_t>, terrain::TerrainPersistenceError> {
						using enum terrain::TerrainPersistenceError;
						const auto Relative = GetPackageTerrainPath(Id);
						const auto *Entry = FindContentEntry(*Manifest, Relative);
						if (!Entry) return std::unexpected(MissingArtifact);
						if (Entry->Size > Maximum) return std::unexpected(LimitExceeded);
						if (Entry->Category != "Project" || Entry->Sha256 != Id.Digest.ToString())
							return std::unexpected(CorruptArtifact);
						const auto File = ResolvePackageFile(PackageRoot, Relative, true);
						if (!File) return std::unexpected(MissingArtifact);
						const auto Text = ReadBoundedText(*File, Maximum);
						if (Text.size() != Entry->Size) return std::unexpected(CorruptArtifact);
						std::vector<std::uint8_t> Bytes(Text.begin(), Text.end());
						if (AdmittedIds.insert(Id).second && TerrainArtifacts)
							TerrainArtifacts->push_back({Id, std::make_shared<const std::vector<std::uint8_t>>(Bytes)});
						return Bytes;
					});
					if (TerrainDigest) (void)terrain::LoadSceneTerrain(Manifest->Inspection.Identity, *TerrainDigest, Reader);
					for (const auto &[Node, Digest] : DomainBindings)
						(void)terrain::LoadDomainTerrain(Manifest->Inspection.Identity, Node, Digest, Reader);
					Admitted = AdmittedIds.size();
				}
				const auto Pinned = std::ranges::count_if(Manifest->Content, [](const ContentEntry &Entry) {
					return Entry.Path.starts_with("content/.gargantuan/terrain/");
				});
				if (static_cast<std::size_t>(Pinned) != Admitted)
					throw terrain::TerrainPersistenceFailure(terrain::TerrainPersistenceError::InvalidBinding);
				if (TerrainArtifacts) std::ranges::sort(*TerrainArtifacts, {}, &terrain::TerrainArtifact::Id);
			} catch (const std::exception &Error) {
				AddDiagnostic(Diagnostics, PackageDiagnosticSeverity::Error, "Content", "InvalidTerrainClosure",
					"The packaged terrain closure could not be verified.", Error.what());
			}
		}
		if (IsLoopbackClient && !HasErrors(Diagnostics)) {
			try {
				VerifyContentFiles(PackageRoot, *Manifest, DeferredChunks);
			} catch (const std::exception &Error) {
				AddDiagnostic(Diagnostics, PackageDiagnosticSeverity::Error, "Content", "ContentIntegrityMismatch",
					"The package contains missing or unverified startup content.", Error.what());
			}
		}
		ValidateRuntimeAssetCatalog(PackageRoot, *Manifest, Diagnostics);
		if (!HasErrors(Diagnostics) && Manifest->ContentManifestPath) {
			try {
				const auto ContentManifestPath = ResolvePackageFile(PackageRoot, *Manifest->ContentManifestPath, true);
				const auto CatalogPath = ResolvePackageFile(PackageRoot, Manifest->AssetCatalogPath, true);
				if (!ContentManifestPath || !CatalogPath)
					throw std::runtime_error("Package spatial inputs are unavailable");
				const auto ManifestText = ReadBoundedText(*ContentManifestPath, MaximumPackageContentManifestBytes);
				if (HashText(ManifestText) != Manifest->ContentManifestDigest.ToString())
					throw std::runtime_error("Package spatial manifest changed after content verification");
				const auto Parsed = ParsePackageContentManifest(ManifestText);
				if (!Parsed) throw std::runtime_error("Package spatial manifest is invalid");
				const auto CatalogText = ReadBoundedText(*CatalogPath, 1024 * 1024);
				const auto CatalogEntry = std::ranges::find(Manifest->Content, Manifest->AssetCatalogPath,
					&ContentEntry::Path);
				if (CatalogEntry == Manifest->Content.end() || HashText(CatalogText) != CatalogEntry->Sha256)
					throw std::runtime_error("Package spatial catalog changed after content verification");
				auto Meshes = PackageSpatialMeshes(PackageRoot, CatalogText);
				ResidencySpatialMetadata Metadata{ResidencySpatialMetadataVersion, Parsed->Package, {}};
				for (const auto &Unit : Parsed->Entries) {
					const auto UnitPath = ResolvePackageFile(PackageRoot, Unit.BlobReference, true);
					if (!UnitPath) throw std::runtime_error("Package spatial unit path is invalid");
					const auto UnitText = ReadBoundedText(*UnitPath, MaximumPackageContentPayloadBytes);
					if (HashText(UnitText) != Unit.Digest.ToString())
						throw std::runtime_error("Package spatial unit changed after content verification");
					if (auto Spatial = InspectSpatialContentUnit(UnitText, Unit, Meshes, {},
						Parsed->InstanceSchemaVersion))
						Metadata.Units.push_back(std::move(*Spatial));
				}
				CheckSpatialProof(*Parsed, Metadata);
			} catch (const std::exception &) {
				AddDiagnostic(Diagnostics, PackageDiagnosticSeverity::Error, "Content",
					"InvalidSpatialMetadata", "The package contains unverified or underbounded spatial content.");
			}
		}
		if (!HasErrors(Diagnostics)) {
			if (VerifiedManifest) *VerifiedManifest = std::move(Manifest);
			AddDiagnostic(
				Diagnostics,
				PackageDiagnosticSeverity::Info,
				"Validation",
				IsLoopbackClient ? "NetworkBootstrapValid" : "Valid",
				IsLoopbackClient ? "The Development client startup content and terrain manifest pins are valid."
					: "The standalone package manifest, content hashes, project payload, assets, and runtime resources are valid."
			);
		}
		return Diagnostics;
	}

	PrivateServerSnapshot::~PrivateServerSnapshot() {
		std::error_code Error;
		std::filesystem::remove_all(Root, Error);
	}

	static ParsedPackageManifest ReadPairedManifest(const std::filesystem::path &Source, const std::string &ExpectedManifest,
		const PackageInspection &Client) {
		std::vector<PackageDiagnostic> Diagnostics;
		auto Manifest = ParseManifest(Source, Diagnostics, false);
		if (!Manifest || Manifest->Inspection.ManifestSha256 != ExpectedManifest ||
			Manifest->Inspection.Identity != Client.Identity || Manifest->Inspection.Revision != Client.Revision ||
			!IsServerExecutable(Manifest->Inspection.PlayerExecutable.filename().string()) ||
			Manifest->Inspection.Audience != PackageAudience::Trusted ||
			(Manifest->Inspection.Target && *Manifest->Inspection.Target != RuntimeDistributionTarget::DedicatedServer))
			throw std::invalid_argument("Invalid paired server package");
		return std::move(*Manifest);
	}

	std::string PrivateServerSnapshot::ReadWorldBaseline(const std::filesystem::path &Source, const std::string &ExpectedManifest,
		const PackageInspection &Client) {
		const auto Manifest = ReadPairedManifest(Source, ExpectedManifest, Client);
		const auto Digest = [&](const std::string &Path) {
			const auto *Entry = FindContentEntry(Manifest, Path);
			if (!Entry) throw std::invalid_argument("Paired world metadata is missing");
			return Entry->Sha256;
		};
		return Digest(Manifest.ProjectPath) + "/" + Digest(Manifest.AssetCatalogPath) + "/" +
			Manifest.ContentManifestDigest.ToString() + "/" + (Manifest.PreRunPath ? Digest(*Manifest.PreRunPath) : HashText(""));
	}

	std::shared_ptr<PrivateServerSnapshot> PrivateServerSnapshot::Stage(const std::filesystem::path &Source,
		const std::string &ExpectedManifest, const PackageInspection &Client, const std::filesystem::path &Profile,
		PackageCancellationToken Token, const std::filesystem::path &RuntimeEnrollment) {
		auto Manifest = ReadPairedManifest(Source, ExpectedManifest, Client);
		RuntimeDistributionAdmission Admission{.ExpectedManifestSha256 = ExpectedManifest};
		if (!RuntimeEnrollment.empty()) {
			Admission.Security = LoadProvisionedRuntimePackageSecurity(RuntimeEnrollment, RuntimePackageProvisioningUse::Runtime);
			if (Admission.Security->Status != RuntimePackageCryptoStatus::Ok)
				throw std::invalid_argument("Paired server runtime enrollment is unavailable");
		}
		const auto Directory = Profile / (".server-" + ProjectId::New().ToString());
		if (IsReparseOrLink(Profile) || !std::filesystem::create_directory(Directory))
			throw std::runtime_error("Cannot create private paired server snapshot");
		auto Snapshot = std::shared_ptr<PrivateServerSnapshot>(new PrivateServerSnapshot(Directory));
		std::filesystem::permissions(Directory, std::filesystem::perms::owner_all);
		const auto Copy = [&](const std::string &Relative) {
			const auto From = ResolvePackageFile(Source, Relative, true);
			if (!From) throw std::runtime_error("Paired server file is unavailable");
			return CopyAndHash(*From, Directory / Relative, Relative, {}, Token);
		};
		if (Copy("game.package.json").Sha256 != ExpectedManifest) throw std::runtime_error("Server manifest changed");
		if (Manifest.RuntimeDistribution && !Manifest.RuntimeDistribution->SigningKeyId.empty())
			Copy(std::string(runtime_distribution::SignaturePath));
		for (const auto &Item : Manifest.Content) {
			const auto Staged = Copy(Item.Path);
			if (Staged.Size != Item.Size || Staged.Sha256 != Item.Sha256)
				throw std::runtime_error("Server package changed during admission");
		}
		if (HasErrors(PackageBuilder::Validate(Directory, PackageAudience::Trusted, Admission)))
			throw std::runtime_error("Staged server failed admission");
		return Snapshot;
	}

	std::vector<PackageDiagnostic> PackageBuilder::Validate(const std::filesystem::path &PackageRoot,
		std::optional<PackageAudience> ExpectedAudience, const RuntimeDistributionAdmission &Admission) {
		return ValidatePackageRoot(PackageRoot, nullptr, ExpectedAudience, Admission);
	}

	std::optional<PackageInspection>
	PackageBuilder::Inspect(const std::filesystem::path &PackageRoot, std::vector<PackageDiagnostic> &Diagnostics) {
		auto Manifest = ParseManifest(PackageRoot, Diagnostics, false);
		if (!Manifest) return std::nullopt;
		return Manifest->Inspection;
	}

	std::optional<RuntimePackagePayload>
	PackageBuilder::Load(const std::filesystem::path &PackageRoot, std::vector<PackageDiagnostic> &Diagnostics,
		const RuntimeDistributionAdmission &Admission, PackageRuntimeLoadMode Mode) {
		std::optional<ParsedPackageManifest> Manifest;
		std::vector<terrain::TerrainArtifact> TerrainArtifacts;
		blockgrid::SegmentMap GridSegments;
		Diagnostics = ValidatePackageRoot(PackageRoot, &Manifest, std::nullopt, Admission, &TerrainArtifacts, Mode,&GridSegments);
		if (HasErrors(Diagnostics)) return std::nullopt;
		if (!Manifest) return std::nullopt;
		try {
			auto ReadPinned = [&](std::string_view RelativePath, std::size_t MaximumBytes) {
				return ReadPinnedPackageFile(PackageRoot, *Manifest, RelativePath, MaximumBytes);
			};
			RuntimePackagePayload Result{.Inspection = Manifest->Inspection};
			if (Manifest->HasApplication)
				Result.ApplicationTemplate = ReadPinned(application_template::Path, application_template::MaximumBytes);
			if (CanDeferTerrain(*Manifest, Mode) && !TerrainArtifacts.empty())
				Result.LoadMode = PackageRuntimeLoadMode::LoopbackDevelopmentClient;
			Result.TerrainArtifacts = std::move(TerrainArtifacts);
			Result.GridSegments=std::move(GridSegments);
			if (Manifest->RuntimeDistribution) {
				Result.ClientCodeIdentity = Manifest->RuntimeDistribution->ClientCodeIdentity;
				Result.SpatialContent = Manifest->RuntimeDistribution->SpatialContent;
				Result.SharedGridAssets = Manifest->RuntimeDistribution->SharedGridAssets;
				Result.WorkspaceTerrainManifest = Manifest->RuntimeDistribution->WorkspaceTerrainManifest;
				Result.WorkspaceTerrainSpacingShift = Manifest->RuntimeDistribution->WorkspaceTerrainSpacingShift;
				auto Catalog = std::move(*Manifest->ScriptCatalog);
				runtime_distribution::AdmitCatalog(Catalog, *Manifest->RuntimeDistribution,
					Result.Inspection.Identity.ToString(), Manifest->DistributionSecurity, Manifest->TrustedCompiler, ReadPinned);
				if (Result.ApplicationTemplate) {
					auto AppCatalog = std::make_shared<RuntimeDistributionCodeCatalog>();
					for (const auto &[Id, Binding] : application_template::GetBindings(*Result.ApplicationTemplate)) {
						auto Node = Catalog.Scripts.extract(Id);
						if (Node.empty()) throw RuntimeDistributionError("InvalidScriptCatalog", "Application binding is missing.");
						AppCatalog->Scripts.insert(std::move(Node));
					}
					Result.ApplicationCode = std::move(AppCatalog);
				}
				Result.RuntimeCode = std::make_shared<RuntimeDistributionCodeCatalog>(std::move(Catalog));
			}
			Result.ContentManifestReference = Manifest->ContentManifestPath;
			Result.ContentManifestDigest = Manifest->ContentManifestDigest;
			if (Result.Inspection.Audience == PackageAudience::DistributedPlayer)
				Result.ClientCodeJson = ReadPinned(Manifest->ProjectPath, client_package::MaximumCodeBytes);
			else
				Result.ProjectJson = ReadPinned(Manifest->ProjectPath, MaximumProjectSnapshotBytes);
			Result.Assets.CatalogJson = ReadPinned(Manifest->AssetCatalogPath, 1024 * 1024);
			if (Manifest->PreRunPath)
				Result.PreRunSource = ReadPinned(*Manifest->PreRunPath, MaximumPreRunSourceBytes);
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
				if (Input.gcount() != static_cast<std::streamsize>(Bytes->size()) ||
					AssetContentId::Hash(*Bytes).ToString() != ContentId)
					throw std::runtime_error("Runtime asset artifact changed after spatial validation");
				Result.Assets.Artifacts.push_back({"assets/artifacts/" + ContentId + ".gasset", std::move(Bytes)});
			}
			return Result;
		} catch (const RuntimeDistributionError &Error) {
			AddDiagnostic(Diagnostics, PackageDiagnosticSeverity::Error, "RuntimeDistribution", Error.Code, Error.what());
			return std::nullopt;
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

	static std::shared_ptr<DataModel> LoadPackagedScene(const RuntimePackagePayload &Payload,
		const std::filesystem::path &PackageRoot, const InstanceSerialization::TerrainSceneAdmission &AdmitTerrain,
		const InstanceSerialization::DomainTerrainAdmission &AdmitDomains = {}) {
		if (!PackageRoot.is_absolute() || !std::filesystem::is_directory(PackageRoot) || IsReparseOrLink(PackageRoot))
			throw std::invalid_argument("Packaged runtime root is invalid");
		std::stringstream Input(Payload.ProjectJson);
		auto Parsed = InstanceSerialization::Deserialize(InstanceSerialization::InstanceFormat::Json, Input, AdmitTerrain, AdmitDomains,&Payload.GridSegments);
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

	std::shared_ptr<DataModel>
	PackageBuilder::LoadWorld(const RuntimePackagePayload &Payload, const std::filesystem::path &PackageRoot) {
		if (Payload.Inspection.Audience == PackageAudience::DistributedPlayer)
			throw std::invalid_argument("A distributed Player package requires a replicated world");
		if (Payload.LoadMode != PackageRuntimeLoadMode::Complete)
			throw std::invalid_argument("[Package:Terrain] A complete payload is required to load a gameplay world");
		const auto Reader = terrain::BudgetDocumentTerrainReads(
			[&Payload](const terrain::TerrainArtifactId &Id, std::size_t Maximum) {
				return terrain::ReadCapturedTerrainArtifact(Payload.TerrainArtifacts, Id, Maximum);
			});
		auto World = LoadPackagedScene(Payload, PackageRoot, terrain::GetSceneTerrainAdmission(Payload.Inspection.Identity, Reader),
			terrain::GetDomainTerrainAdmission(Payload.Inspection.Identity, Reader));
		if (Payload.Inspection.Target == RuntimeDistributionTarget::DedicatedServer)
			World->PackagedClientCodeIdentity = Payload.ClientCodeIdentity;
		if (Payload.RuntimeCode)
			(void)runtime_distribution::BindCatalog(*Payload.RuntimeCode, World, false);
		return World;
	}

	std::shared_ptr<DataModel> PackageBuilder::LoadClientCodeWorld(const RuntimePackagePayload &Payload,
		const std::filesystem::path &PackageRoot) {
		if (Payload.LoadMode != PackageRuntimeLoadMode::LoopbackDevelopmentClient ||
			Payload.Inspection.Configuration != PackageConfiguration::Development ||
			Payload.Inspection.Audience != PackageAudience::Trusted || Payload.Inspection.Target)
			throw std::invalid_argument("[Package:Terrain] Client code hydration requires a loopback Development payload");
		return LoadPackagedScene(Payload, PackageRoot, [&Payload](DataModel &, const AssetContentId &Digest) {
			const terrain::TerrainArtifactId Id{terrain::TerrainArtifactPurpose::Manifest, Digest};
			const auto *Manifest = terrain::FindTerrainArtifact(Payload.TerrainArtifacts, Id);
			if (Payload.TerrainArtifacts.size() != 1 || !Manifest || !Manifest->Bytes ||
				AssetContentId::Hash(*Manifest->Bytes) != Digest)
				throw std::runtime_error("[Package:Terrain] Client code world did not match its terrain manifest pin");
		});
	}

	std::optional<ContentAvailabilityConfiguration> PackageBuilder::GetLocalContentConfiguration(
		const RuntimePackagePayload &Payload,
		const std::filesystem::path &PackageRoot,
		ContentResidencyMode Mode
	) {
		if (Payload.LoadMode != PackageRuntimeLoadMode::Complete)
			throw std::invalid_argument("[Package:Terrain] Local gameplay content requires a complete package payload");
		if (!Payload.ContentManifestReference) return std::nullopt;
		if (!PackageRoot.is_absolute() || !Payload.Inspection.Identity.IsValid() || Payload.Inspection.Revision == 0 ||
			!Payload.ContentManifestDigest.IsValid())
			throw std::invalid_argument("Packaged content configuration is invalid");
		return ContentAvailabilityConfiguration{
			.Provider = std::make_shared<LocalPackageContentProvider>(
				PackageRoot, *Payload.ContentManifestReference, Payload.ContentManifestDigest
			),
			.Package = {Payload.Inspection.Identity, Payload.Inspection.Revision},
			.ManifestDigest = Payload.ContentManifestDigest,
			.Mode = Mode,
		};
	}

	std::size_t PackageBuilder::HydrateClientCode(
		const std::shared_ptr<DataModel> &TrustedPackageWorld, const std::shared_ptr<DataModel> &ReplicatedWorld
	) {
		if (!TrustedPackageWorld || !ReplicatedWorld || TrustedPackageWorld == ReplicatedWorld)
			throw std::invalid_argument(
				"Packaged client code hydration requires distinct trusted and replicated worlds"
			);
		std::map<std::pair<std::string, std::string>, std::vector<std::shared_ptr<Instance>>> Targets;
		for (const auto &Target : ReplicatedWorld->GetDescendants())
			if (!FindPlayerGuiScope(*Target))
				Targets[std::make_pair(Target->GetFullName(), (Target->GetClassName)())].push_back(Target);
		std::size_t Hydrated = 0;
		for (const auto &SourceObject : TrustedPackageWorld->GetDescendants()) {
			auto Source = std::dynamic_pointer_cast<LuaSourceContainer>(SourceObject);
			if (!Source || IsStarterGuiTemplate(*Source)) continue;
			if (auto ScriptValue = std::dynamic_pointer_cast<Script>(Source);
				ScriptValue && ScriptValue->GetRunContext() != Enums::RunContext::Client)
				continue;
			if (!std::dynamic_pointer_cast<Script>(Source) && !std::dynamic_pointer_cast<ModuleScript>(Source))
				continue;
			auto Found = Targets.find(std::make_pair(SourceObject->GetFullName(), (SourceObject->GetClassName)()));
			if (Found == Targets.end()) continue;
			if (Found->second.size() != 1) throw std::runtime_error("Packaged client code target is ambiguous");
			auto Target = std::dynamic_pointer_cast<LuaSourceContainer>(Found->second.front());
			if (!Target) throw std::runtime_error("Packaged client code target changed class");
			if (auto TargetScript = std::dynamic_pointer_cast<Script>(Target);
				TargetScript && TargetScript->GetRunContext() != Enums::RunContext::Client)
				throw std::runtime_error("Packaged client Script changed runtime role");
			if (!Target->GetSource().empty() && Target->GetSource() != Source->GetSource())
				throw std::runtime_error("Replicated client code target already contains different source");
			Target->SetSource(Source->GetSource());
			++Hydrated;
		}
		return Hydrated;
	}

	std::function<bool(const std::shared_ptr<LuaSourceContainer> &)>
	PackageBuilder::MakePlayerGuiCodeHydrator(const std::shared_ptr<DataModel> &TrustedPackageWorld) {
		struct TrustedSource {
			std::string ClassName;
			std::string Digest;
			std::string Source;
		};
		std::map<std::string, TrustedSource> Sources;
		const auto Starter = TrustedPackageWorld ? TrustedPackageWorld->FindFirstChildOfClass("StarterGui", false) : nullptr;
		if (!Starter) return {};
		for (const auto &Root : Starter->GetChildren()) {
			if (!std::dynamic_pointer_cast<ScreenGui>(Root)) continue;
			for (const auto &Object : Root->GetDescendants()) {
				const auto Source = std::dynamic_pointer_cast<LuaSourceContainer>(Object);
				if (!Source || !IsClientPlayerGuiSource(*Source)) continue;
				const auto Inserted = Sources.emplace(Source->GetSourceId(),
					TrustedSource{Source->GetClassName(), Source->GetSourceDigest(), Source->GetSource()});
				if (!Inserted.second) throw std::runtime_error("Packaged StarterGui SourceId is ambiguous");
			}
		}
		return [Sources = std::move(Sources)](const std::shared_ptr<LuaSourceContainer> &Target) {
			if (!Target || Target->GetDestroyed() || Target->IsDestroying() || !IsClientPlayerGuiSource(*Target))
				return false;
			auto Gui = FindPlayerGuiScope(*Target);
			if (!Gui || !FindPlayerGuiOwner(*Gui)) return false;
			auto Root = Target->GetParent().value_or(nullptr);
			while (Root && Root->GetParent().value_or(nullptr) != Gui)
				Root = Root->GetParent().value_or(nullptr);
			if (!std::dynamic_pointer_cast<ScreenGui>(Root)) return false;
			const auto Found = Sources.find(Target->GetGuiTemplateSourceId());
			return Found != Sources.end() && Found->second.ClassName == Target->GetClassName() &&
				Found->second.Digest == Target->GetGuiTemplateSourceDigest() &&
				Target->HydrateGuiTemplateSource(Found->second.Source);
		};
	}

	std::function<bool(const std::shared_ptr<LuaSourceContainer> &)>
	PackageBuilder::MakeDistributedPlayerGuiCodeHydrator(const RuntimePackagePayload &Payload) {
		if (!Payload.ClientCodeJson) return {};
		std::map<std::string, client_package::CodeKey> Gui;
		auto Projection = client_package::DecodeCode(*Payload.ClientCodeJson, &Gui);
		if (!Payload.RuntimeCode) {
			if (Gui.empty()) return {};
			struct Binding { std::string Class, Source, Digest; };
			std::map<std::string, Binding> Sources;
			for (const auto &[Id, Key] : Gui) {
				auto &Source = Projection.at(Key);
				const auto Digest = AssetContentId::Hash(std::span(
					reinterpret_cast<const std::uint8_t *>(Source.data()), Source.size())).ToString();
				Sources.emplace(Id, Binding{Key.second, std::move(Source), Digest});
			}
			return [Sources = std::move(Sources)](const std::shared_ptr<LuaSourceContainer> &Target) {
				if (!Target || !IsClientPlayerGuiSource(*Target)) return false;
				const auto Found = Sources.find(Target->GetGuiTemplateSourceId());
				return Found != Sources.end() && Found->second.Class == (Target->GetClassName)() &&
					Found->second.Digest == Target->GetGuiTemplateSourceDigest() &&
					Target->HydrateGuiTemplateSource(Found->second.Source);
			};
		}
		auto Catalog = Payload.RuntimeCode;
		return [Catalog = std::move(Catalog), Projection = std::move(Projection), Identity = Payload.ClientCodeIdentity](
			const std::shared_ptr<LuaSourceContainer> &Target
		) {
			if (!Target) return false;
			const auto Found = Catalog->Scripts.find(Target->GetGuiTemplateSourceId());
			if (Found == Catalog->Scripts.end()) return true;
			if (!IsClientPlayerGuiSource(*Target) || Target->GetGuiTemplateSourceVersion() != Target->GetSourceVersion() ||
				Found->second.Path.empty() ||
				Found->second.Path.front() != "StarterGui" || Found->second.Class != (Target->GetClassName)())
				return false;
			if (Target->GetGuiTemplateSourceDigest() != GetRuntimeDistributionGuiDigest(
				Identity, Found->second.SourceId, Target->GetGuiTemplateSourceVersion())) return false;
			if (Found->second.Code) {
				if (Target->HasRuntimeScriptCode()) return true;
				if (!Target->GetSource().empty()) return false;
				Target->BindRuntimeScriptCode(Found->second.Code);
				return true;
			}
			const auto Source = Projection.find({Found->second.Path, Found->second.Class});
			if (Source != Projection.end() && !Target->GetSource().empty())
				return Target->GetSource() == Source->second;
			if (Source == Projection.end() || !Target->HydrateGuiTemplateSource(Source->second)) return false;
			Target->SetRuntimeSourceDiagnostics(Found->second.Diagnostics == RuntimeDistributionDiagnostics::Minimal ?
				RuntimeScriptDiagnostics::Stripped : RuntimeScriptDiagnostics::LineInfo);
			return true;
		};
	}

	std::function<void(ObjectId, const std::shared_ptr<LuaSourceContainer> &, bool)>
	PackageBuilder::MakeDistributedCodeHydrator(const RuntimePackagePayload &Payload) {
		if (!Payload.RuntimeCode || !Payload.ClientCodeJson) return {};
		auto Projection = client_package::DecodeCode(*Payload.ClientCodeJson);
		std::map<client_package::CodeKey, RuntimeDistributionArtifact> Bindings;
		for (const auto &[Id, Entry] : Payload.RuntimeCode->Scripts) {
			(void)Id;
			Bindings.emplace(client_package::CodeKey{Entry.Path, Entry.Class}, Entry);
		}
		return [Bindings = std::move(Bindings), Projection = std::move(Projection),
			ProvisionedObjects = std::map<client_package::CodeKey, ObjectId>{}, ProvisionedWorld = ObjectId{}](
			ObjectId ReplicaSource, const std::shared_ptr<LuaSourceContainer> &Target, bool BeforeRuntimeAttach
		) mutable {
			if (!Target || !Target->GetGuiTemplateSourceId().empty() || Target->HasRuntimeScriptCode()) return;
			if (const auto Script = std::dynamic_pointer_cast<gargantuan::Script>(Target);
				Script && Script->GetRunContext() != Enums::RunContext::Client) return;
			if (std::dynamic_pointer_cast<ModuleScript>(Target) &&
				GetInstanceServicePolicy(*Target) != SchemaServicePolicy::Replicated) return;
			std::vector<std::string> Path;
			auto Current = std::static_pointer_cast<Instance>(Target);
			while (Current && !std::dynamic_pointer_cast<DataModel>(Current)) {
				if (Path.size() == MaximumProtocolJsonDepth)
					throw RuntimeDistributionError("AudienceViolation", "A runtime script path exceeds its depth bound.");
				Path.push_back(Current->GetName());
				Current = Current->GetParent().value_or(nullptr);
			}
			if (!Current) return;
			const auto WorldIdentity = Current->GetObjectId();
			if (WorldIdentity != ProvisionedWorld) {
				if (!BeforeRuntimeAttach)
					throw RuntimeDistributionError("AudienceViolation", "Managed script bindings belong to a different replica world.");
				ProvisionedObjects.clear();
				ProvisionedWorld = WorldIdentity;
			}
			std::ranges::reverse(Path);
			const client_package::CodeKey Key{std::move(Path), (Target->GetClassName)()};
			const auto Bound = Bindings.find(Key);
			if (Bound == Bindings.end()) return;
			if (BeforeRuntimeAttach) {
				const auto [Position, Inserted] = ProvisionedObjects.emplace(Key, ReplicaSource);
				if (!Inserted && Position->second != ReplicaSource)
					throw RuntimeDistributionError("AudienceViolation", "Two replica objects claimed the same package binding.");
				return;
			}
			const auto Provisioned = ProvisionedObjects.find(Key);
			if (Provisioned == ProvisionedObjects.end() || Provisioned->second != ReplicaSource)
				throw RuntimeDistributionError("AudienceViolation", "A new replica generation cannot assume an existing package binding.");
			if (!Target->GetSource().empty())
				throw RuntimeDistributionError("AudienceViolation", "A materialized script already contains clear source.");
			if (Bound->second.Code) Target->BindRuntimeScriptCode(Bound->second.Code);
			else {
				Target->SetSource(Projection.at(Key));
				Target->SetRuntimeSourceDiagnostics(Bound->second.Diagnostics == RuntimeDistributionDiagnostics::Minimal ?
					RuntimeScriptDiagnostics::Stripped : RuntimeScriptDiagnostics::LineInfo);
			}
		};
	}

	std::size_t PackageBuilder::HydrateDistributedClientCode(
		const RuntimePackagePayload &Payload, const std::shared_ptr<DataModel> &ReplicatedWorld
	) {
		if (!ReplicatedWorld || Payload.Inspection.Audience != PackageAudience::DistributedPlayer ||
			!Payload.ClientCodeJson)
			throw std::invalid_argument("Distributed Player code requires a client package and replicated world");
		const auto Hydrated = client_package::HydrateCode(*Payload.ClientCodeJson, *ReplicatedWorld);
		if (Payload.RuntimeCode)
			(void)runtime_distribution::BindCatalog(*Payload.RuntimeCode, ReplicatedWorld, true);
		return Hydrated;
	}
}
