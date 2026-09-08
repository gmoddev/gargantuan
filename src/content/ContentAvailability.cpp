#include "gargantuan/content/ContentAvailability.hpp"

#include "assets/PreparedInstanceSerialization.hpp"
#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Instance.hpp"
#include "gargantuan/runtime/JobSystem.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"
#include "gargantuan/services/Workspace.hpp"
#include "serialization/JsonCodec.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <queue>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace gargantuan {
	using Json = JsonCodec::Json;

	namespace {
		constexpr std::string_view ManifestFormat = "GargantuanPackageContent";
		using AvailabilityClock = std::chrono::steady_clock;
		using AvailabilityTimePoint = AvailabilityClock::time_point;

		void RecordDuration(
			ContentAvailabilityDurationMetric &Metric,
			AvailabilityTimePoint StartedAt,
			AvailabilityTimePoint FinishedAt
		) {
			if (FinishedAt < StartedAt) return;
			const auto Microseconds = static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::microseconds>(FinishedAt - StartedAt).count()
			);
			++Metric.Samples;
			Metric.TotalMicroseconds += Microseconds;
			Metric.MaximumMicroseconds = std::max(Metric.MaximumMicroseconds, Microseconds);
		}

		struct DurationScope final {
			explicit DurationScope(ContentAvailabilityDurationMetric &MetricValue)
				: Metric(MetricValue), StartedAt(AvailabilityClock::now()) {}
			~DurationScope() { RecordDuration(Metric, StartedAt, AvailabilityClock::now()); }
			ContentAvailabilityDurationMetric &Metric;
			AvailabilityTimePoint StartedAt;
		};

		bool IsSafeBlobReference(std::string_view Value) {
			if (Value.empty() || Value.size() > MaximumPackageBlobReferenceBytes || Value.front() == '/' ||
				Value.front() == '\\' || Value.find('\\') != std::string_view::npos || Value.find(':') != std::string_view::npos)
				return false;
			std::filesystem::path Path(Value);
			if (Path.is_absolute() || Path.has_root_name() || Path.has_root_directory()) return false;
			for (const auto &Part : Path)
				if (Part == "." || Part == ".." || Part.empty()) return false;
			return Path.generic_string() == Value;
		}

		std::expected<std::string, ContentProviderError> ReadBoundedFile(
			const std::filesystem::path &Path,
			std::size_t MaximumBytes
		) {
			std::error_code Error;
			if (!std::filesystem::is_regular_file(Path, Error) || Error || std::filesystem::is_symlink(Path, Error))
				return std::unexpected(ContentProviderError{ContentProviderErrorCode::NotFound, "content file is absent"});
			const auto Size = std::filesystem::file_size(Path, Error);
			if (Error || Size > MaximumBytes)
				return std::unexpected(ContentProviderError{
					ContentProviderErrorCode::ResourceExhausted, "content file exceeds its byte limit"
				});
			std::ifstream Input(Path, std::ios::binary);
			if (!Input)
				return std::unexpected(ContentProviderError{ContentProviderErrorCode::Unavailable, "content file could not be opened"});
			std::string Result(static_cast<std::size_t>(Size), '\0');
			Input.read(Result.data(), static_cast<std::streamsize>(Result.size()));
			if (!Input && !Input.eof())
				return std::unexpected(ContentProviderError{ContentProviderErrorCode::Unavailable, "content file could not be read"});
			return Result;
		}

		std::expected<std::filesystem::path, ContentProviderError> ResolveLocalContentPath(
			const std::filesystem::path &CanonicalRoot,
			std::string_view Reference
		) {
			if (!IsSafeBlobReference(Reference))
				return std::unexpected(ContentProviderError{
					ContentProviderErrorCode::InvalidResponse, "content file reference is unsafe"
				});
			std::error_code Error;
			auto Candidate = CanonicalRoot;
			for (const auto &Part : std::filesystem::path(Reference)) {
				Candidate /= Part;
				const auto Status = std::filesystem::symlink_status(Candidate, Error);
				if (Error || std::filesystem::is_symlink(Status))
					return std::unexpected(ContentProviderError{
						ContentProviderErrorCode::NotFound, "content file path is unavailable"
					});
			}
			auto Canonical = std::filesystem::weakly_canonical(Candidate, Error);
			if (Error)
				return std::unexpected(ContentProviderError{
					ContentProviderErrorCode::NotFound, "content file path is unavailable"
				});
			const auto Relative = Canonical.lexically_relative(CanonicalRoot);
			if (Relative.empty() || Relative.is_absolute() || *Relative.begin() == "..")
				return std::unexpected(ContentProviderError{
					ContentProviderErrorCode::InvalidResponse, "content file escapes its package root"
				});
			return Canonical;
		}

		bool CheckedAdd(std::size_t &Value, std::size_t Addition, std::size_t Maximum) {
			if (Addition > Maximum - Value) return false;
			Value += Addition;
			return true;
		}

		bool BoundsValid(const PackageCoarseBounds &Bounds) {
			for (std::size_t Axis = 0; Axis < 3; ++Axis)
				if (!std::isfinite(Bounds.Minimum[Axis]) || !std::isfinite(Bounds.Maximum[Axis]) ||
					Bounds.Minimum[Axis] > Bounds.Maximum[Axis]) return false;
			return true;
		}

		ContentProviderError CancelledError(const ContentRequestContext &Context) {
			return Context.IsExpired()
				? ContentProviderError{ContentProviderErrorCode::DeadlineExceeded, "content request deadline elapsed"}
				: ContentProviderError{ContentProviderErrorCode::Cancelled, "content request was cancelled"};
		}

		struct JsonStringToken final {
			std::size_t End = 0;
			bool Equals = false;
		};

		JsonStringToken ScanJsonStringToken(
			std::string_view Encoded,
			std::size_t Start,
			std::string_view Expected
		) {
			std::size_t ExpectedPosition = 0;
			bool Equals = true;
			for (auto Position = Start + 1; Position < Encoded.size(); ++Position) {
				const auto Character = static_cast<unsigned char>(Encoded[Position]);
				if (Character == '"')
					return {Position + 1, Equals && ExpectedPosition == Expected.size()};
				std::uint32_t Decoded = Character;
				if (Character == '\\') {
					if (++Position == Encoded.size()) return {Encoded.size(), false};
					const char Escape = Encoded[Position];
					if (Escape == 'u') {
						if (Encoded.size() - Position <= 4) return {Encoded.size(), false};
						Decoded = 0;
						for (std::size_t Digit = 0; Digit < 4; ++Digit) {
							const auto Hex = static_cast<unsigned char>(Encoded[++Position]);
							Decoded <<= 4;
							if (Hex >= '0' && Hex <= '9') Decoded += Hex - '0';
							else if (Hex >= 'a' && Hex <= 'f') Decoded += Hex - 'a' + 10;
							else if (Hex >= 'A' && Hex <= 'F') Decoded += Hex - 'A' + 10;
							else Equals = false;
						}
					} else {
						switch (Escape) {
						case '"': Decoded = '"'; break;
						case '\\': Decoded = '\\'; break;
						case '/': Decoded = '/'; break;
						case 'b': Decoded = '\b'; break;
						case 'f': Decoded = '\f'; break;
						case 'n': Decoded = '\n'; break;
						case 'r': Decoded = '\r'; break;
						case 't': Decoded = '\t'; break;
						default: Equals = false; break;
						}
					}
				}
				if (ExpectedPosition >= Expected.size() ||
					Decoded != static_cast<unsigned char>(Expected[ExpectedPosition])) Equals = false;
				++ExpectedPosition;
			}
			return {Encoded.size(), false};
		}

		std::size_t ValidateManifestEntryArrayCount(std::string_view Encoded, std::size_t Position) {
			std::size_t Depth = 1;
			std::size_t Entries = 0;
			bool InString = false;
			bool Escaped = false;
			bool InElement = false;
			for (++Position; Position < Encoded.size() && Depth != 0; ++Position) {
				const char Character = Encoded[Position];
				if (InString) {
					if (Escaped) Escaped = false;
					else if (Character == '\\') Escaped = true;
					else if (Character == '"') InString = false;
					continue;
				}
				if (Character == '"') {
					if (Depth == 1 && !InElement) {
						InElement = true;
						if (++Entries > MaximumPackageContentUnits)
							throw std::length_error("manifest entry count exceeds its bound");
					}
					InString = true;
					continue;
				}
				if (Character == '{' || Character == '[') {
					if (Depth == 1 && !InElement) {
						InElement = true;
						if (++Entries > MaximumPackageContentUnits)
							throw std::length_error("manifest entry count exceeds its bound");
					}
					++Depth;
					continue;
				}
				if (Character == '}' || Character == ']') {
					--Depth;
					continue;
				}
				if (Depth != 1) continue;
				if (Character == ',') {
					InElement = false;
					continue;
				}
				if (!std::isspace(static_cast<unsigned char>(Character)) && !InElement) {
					InElement = true;
					if (++Entries > MaximumPackageContentUnits)
						throw std::length_error("manifest entry count exceeds its bound");
				}
			}
			return Position;
		}

		void ValidateManifestEntryCountBeforeParse(std::string_view Encoded) {
			std::size_t ObjectDepth = 0;
			std::size_t ArrayDepth = 0;
			for (std::size_t Position = 0; Position < Encoded.size(); ++Position) {
				const char Character = Encoded[Position];
				if (Character == '{') {
					++ObjectDepth;
					continue;
				}
				if (Character == '}') {
					if (ObjectDepth != 0) --ObjectDepth;
					continue;
				}
				if (Character == '[') {
					++ArrayDepth;
					continue;
				}
				if (Character == ']') {
					if (ArrayDepth != 0) --ArrayDepth;
					continue;
				}
				if (Character != '"') continue;
				const auto Token = ScanJsonStringToken(Encoded, Position, "Entries");
				Position = Token.End == 0 ? Position : Token.End - 1;
				if (!Token.Equals || ObjectDepth != 1 || ArrayDepth != 0) continue;
				auto Value = Token.End;
				while (Value < Encoded.size() && std::isspace(static_cast<unsigned char>(Encoded[Value]))) ++Value;
				if (Value == Encoded.size() || Encoded[Value++] != ':') continue;
				while (Value < Encoded.size() && std::isspace(static_cast<unsigned char>(Encoded[Value]))) ++Value;
				if (Value == Encoded.size() || Encoded[Value] != '[') continue;
				const auto End = ValidateManifestEntryArrayCount(Encoded, Value);
				if (End != 0) Position = End - 1;
			}
		}
	}

	bool IsValidPackageContentKey(std::string_view Value) {
		if (Value.empty() || Value.size() > MaximumPackageContentKeyBytes || Value.front() == '/' ||
			Value.back() == '/' || Value.find("//") != std::string_view::npos) return false;
		for (const unsigned char Character : Value)
			if (!((Character >= 'a' && Character <= 'z') || (Character >= '0' && Character <= '9') ||
				  Character == '-' || Character == '_' || Character == '.' || Character == '/')) return false;
		for (std::size_t Start = 0; Start < Value.size();) {
			const auto End = Value.find('/', Start);
			const auto Segment = Value.substr(Start, End == std::string_view::npos ? Value.size() - Start : End - Start);
			if (Segment == "." || Segment == "..") return false;
			if (End == std::string_view::npos) break;
			Start = End + 1;
		}
		return true;
	}

	std::string EncodePackageContentManifest(const PackageContentManifest &Manifest) {
		Json Entries = Json::array();
		for (const auto &Entry : Manifest.Entries) {
			Json Bounds = nullptr;
			if (Entry.CoarseBounds)
				Bounds = Json{{"Minimum", Entry.CoarseBounds->Minimum}, {"Maximum", Entry.CoarseBounds->Maximum}};
			Entries.push_back({
				{"Key", Entry.Key},
				{"Blob", Entry.BlobReference},
				{"Digest", Entry.Digest.ToString()},
				{"CompressedBytes", Entry.CompressedBytes},
				{"UncompressedBytes", Entry.UncompressedBytes},
				{"ObjectCount", Entry.ObjectCount},
				{"Dependencies", Entry.Dependencies},
				{"PackageSpace", Entry.PackageSpaceKey},
				{"CoarseBounds", std::move(Bounds)},
				{"Flags", static_cast<std::uint8_t>(Entry.Flags)},
			});
		}
		Json Encoded{
			{"Format", ManifestFormat},
			{"Version", PackageContentManifestVersion},
			{"ProjectId", Manifest.Package.Project.ToString()},
			{"PackageVersion", Manifest.Package.PackageVersion},
			{"InstanceSchemaVersion", Manifest.InstanceSchemaVersion},
			{"Entries", std::move(Entries)},
		};
		auto Result = JsonCodec::Encode(Encoded, "package content manifest");
		if (!Result || Result->size() > MaximumPackageContentManifestBytes)
			throw std::runtime_error("Package content manifest exceeds its bound");
		return std::move(*Result);
	}

	PackageContentManifestResult ParsePackageContentManifest(std::string_view Encoded) {
		try {
			if (Encoded.empty() || Encoded.size() > MaximumPackageContentManifestBytes)
				throw std::runtime_error("manifest byte length is invalid");
			ValidateProtocolJsonDocument(Encoded, MaximumPackageContentManifestBytes);
			if (!IsValidProtocolUtf8(Encoded)) throw std::runtime_error("manifest is not valid UTF-8");
			ValidateManifestEntryCountBeforeParse(Encoded);
			auto Parsed = Json::parse(Encoded);
			JsonCodec::ValidateTree(Parsed, MaximumPackageContentManifestJsonNodes);
			if (!Parsed.is_object() || Parsed.size() != 6 || !Parsed["Format"].is_string() ||
				Parsed["Format"].get_ref<const std::string &>() != ManifestFormat ||
				!Parsed["Version"].is_number_unsigned() ||
				Parsed["Version"].get<std::uint32_t>() != PackageContentManifestVersion ||
				!Parsed["ProjectId"].is_string() || !Parsed["PackageVersion"].is_number_unsigned() ||
				!Parsed["InstanceSchemaVersion"].is_number_unsigned() || !Parsed["Entries"].is_array())
				throw std::runtime_error("manifest shape or version is invalid");
			auto Project = ProjectId::Parse(Parsed["ProjectId"].get_ref<const std::string &>());
			const auto PackageVersion = Parsed["PackageVersion"].get<std::uint64_t>();
			const auto SchemaVersion = Parsed["InstanceSchemaVersion"].get<std::uint32_t>();
			const auto &WireEntries = Parsed["Entries"];
			if (!Project || PackageVersion == 0 || SchemaVersion != PackageContentInstanceSchemaVersion ||
				WireEntries.size() > MaximumPackageContentUnits)
				throw std::runtime_error("manifest identity or entry count is invalid");
			PackageContentManifest Result{{*Project, PackageVersion}, SchemaVersion, {}};
			Result.Entries.reserve(WireEntries.size());
			std::unordered_set<std::string> Seen;
			std::size_t EdgeCount = 0;
			std::string Previous;
			for (const auto &Wire : WireEntries) {
				if (!Wire.is_object() || Wire.size() != 10 || !Wire["Key"].is_string() || !Wire["Blob"].is_string() ||
					!Wire["Digest"].is_string() || !Wire["CompressedBytes"].is_number_unsigned() ||
					!Wire["UncompressedBytes"].is_number_unsigned() || !Wire["ObjectCount"].is_number_unsigned() ||
					!Wire["Dependencies"].is_array() || !Wire["PackageSpace"].is_string() ||
					(!Wire["CoarseBounds"].is_null() && !Wire["CoarseBounds"].is_object()) ||
					!Wire["Flags"].is_number_unsigned()) throw std::runtime_error("content entry shape is invalid");
				PackageContentEntry Entry;
				Entry.Key = Wire["Key"].get<std::string>();
				Entry.BlobReference = Wire["Blob"].get<std::string>();
				auto Digest = AssetContentId::Parse(Wire["Digest"].get_ref<const std::string &>());
				Entry.CompressedBytes = Wire["CompressedBytes"].get<std::uint64_t>();
				Entry.UncompressedBytes = Wire["UncompressedBytes"].get<std::uint64_t>();
				Entry.ObjectCount = Wire["ObjectCount"].get<std::uint32_t>();
				Entry.PackageSpaceKey = Wire["PackageSpace"].get<std::string>();
				const auto Flags = Wire["Flags"].get<std::uint64_t>();
				if (!IsValidPackageContentKey(Entry.Key) || (!Previous.empty() && Entry.Key <= Previous) ||
					!Seen.insert(Entry.Key).second || !IsSafeBlobReference(Entry.BlobReference) || !Digest ||
					Entry.CompressedBytes == 0 || Entry.CompressedBytes > MaximumPackageContentPayloadBytes ||
					Entry.UncompressedBytes != Entry.CompressedBytes || Entry.ObjectCount == 0 ||
					Entry.ObjectCount > MaximumPackageContentObjectsPerUnit || Entry.PackageSpaceKey.empty() ||
					Entry.PackageSpaceKey.size() > MaximumPackageSpaceKeyBytes || Flags > 3)
					throw std::runtime_error("content entry identity or bounds are invalid");
				Entry.Digest = *Digest;
				Entry.Flags = static_cast<PackageContentFlags>(Flags);
				if (Wire["Dependencies"].size() > MaximumPackageContentDependenciesPerUnit ||
					!CheckedAdd(EdgeCount, Wire["Dependencies"].size(), MaximumPackageContentDependencies))
					throw std::runtime_error("dependency count exceeds its bound");
				std::string PriorDependency;
				for (const auto &WireDependency : Wire["Dependencies"]) {
					if (!WireDependency.is_string()) throw std::runtime_error("dependency key is not a string");
					auto Dependency = WireDependency.get<std::string>();
					if (!IsValidPackageContentKey(Dependency) || Dependency == Entry.Key ||
						(!PriorDependency.empty() && Dependency <= PriorDependency))
						throw std::runtime_error("dependency identity is invalid, duplicated, or unsorted");
					PriorDependency = Dependency;
					Entry.Dependencies.push_back(std::move(Dependency));
				}
				if (!Wire["CoarseBounds"].is_null()) {
					const auto &WireBounds = Wire["CoarseBounds"];
					if (WireBounds.size() != 2 || !WireBounds["Minimum"].is_array() ||
						WireBounds["Minimum"].size() != 3 || !WireBounds["Maximum"].is_array() ||
						WireBounds["Maximum"].size() != 3)
						throw std::runtime_error("coarse bounds shape is invalid");
					PackageCoarseBounds Bounds;
					for (std::size_t Axis = 0; Axis < 3; ++Axis) {
						if (!WireBounds["Minimum"][Axis].is_number() || !WireBounds["Maximum"][Axis].is_number())
							throw std::runtime_error("coarse bounds component is invalid");
						Bounds.Minimum[Axis] = WireBounds["Minimum"][Axis].get<float>();
						Bounds.Maximum[Axis] = WireBounds["Maximum"][Axis].get<float>();
					}
					if (!BoundsValid(Bounds)) throw std::runtime_error("coarse bounds are not finite or ordered");
					Entry.CoarseBounds = Bounds;
				}
				Previous = Entry.Key;
				Result.Entries.push_back(std::move(Entry));
			}
			std::unordered_map<std::string, std::size_t> Indices;
			for (std::size_t Index = 0; Index < Result.Entries.size(); ++Index)
				Indices.emplace(Result.Entries[Index].Key, Index);
			std::vector<std::size_t> InDegree(Result.Entries.size());
			std::vector<std::vector<std::size_t>> Dependents(Result.Entries.size());
			for (std::size_t Index = 0; Index < Result.Entries.size(); ++Index)
				for (const auto &Dependency : Result.Entries[Index].Dependencies) {
					auto Found = Indices.find(Dependency);
					if (Found == Indices.end()) throw std::runtime_error("dependency target is absent");
					++InDegree[Index];
					Dependents[Found->second].push_back(Index);
				}
			std::queue<std::size_t> Ready;
			for (std::size_t Index = 0; Index < InDegree.size(); ++Index) if (InDegree[Index] == 0) Ready.push(Index);
			std::size_t Visited = 0;
			while (!Ready.empty()) {
				const auto Index = Ready.front();
				Ready.pop();
				++Visited;
				for (const auto Dependent : Dependents[Index]) if (--InDegree[Dependent] == 0) Ready.push(Dependent);
			}
			if (Visited != Result.Entries.size()) throw std::runtime_error("dependency graph contains a cycle");
			return Result;
		} catch (const std::exception &Error) {
			return std::unexpected(Error.what());
		}
	}

	struct PackageContentCoarseIndex::Impl {
		struct Cell {
			std::string Space;
			std::array<std::int32_t, 3> Coordinate{};
			auto operator<=>(const Cell &) const = default;
		};
		float CellSize = 512.0f;
		std::map<Cell, std::vector<std::size_t>> Cells;
		std::size_t Memberships = 0;
	};

	PackageContentCoarseIndex::PackageContentCoarseIndex(const PackageContentManifest &Manifest, float CellSize) {
		if (!std::isfinite(CellSize) || CellSize <= 0.0f) throw std::invalid_argument("Package coarse cell size is invalid");
		if (Manifest.Entries.size() > MaximumPackageContentUnits)
			throw std::length_error("Package coarse index exceeds the content-unit limit");
		auto Mutable = std::make_shared<Impl>();
		Mutable->CellSize = CellSize;
		for (std::size_t Index = 0; Index < Manifest.Entries.size(); ++Index) {
			const auto &Entry = Manifest.Entries[Index];
			if (!Entry.CoarseBounds) continue;
			if (!BoundsValid(*Entry.CoarseBounds)) throw std::invalid_argument("Package coarse bounds are invalid");
			std::array<std::int32_t, 3> Minimum{}, Maximum{};
			std::uint64_t MembershipCount = 1;
			for (std::size_t Axis = 0; Axis < 3; ++Axis) {
				const auto MinimumCell = std::floor(Entry.CoarseBounds->Minimum[Axis] / CellSize);
				const auto MaximumCell = std::floor(Entry.CoarseBounds->Maximum[Axis] / CellSize);
				if (MinimumCell < std::numeric_limits<std::int32_t>::min() ||
					MaximumCell > std::numeric_limits<std::int32_t>::max())
					throw std::length_error("Package coarse bounds exceed the addressable range");
				Minimum[Axis] = static_cast<std::int32_t>(MinimumCell);
				Maximum[Axis] = static_cast<std::int32_t>(MaximumCell);
				const auto Extent = static_cast<std::uint64_t>(
					static_cast<std::int64_t>(Maximum[Axis]) - Minimum[Axis] + 1
				);
				if (Extent > 4096 / MembershipCount)
					throw std::length_error("Package content overlaps too many coarse cells");
				MembershipCount *= Extent;
			}
			for (std::int64_t X = Minimum[0]; X <= Maximum[0]; ++X)
				for (std::int64_t Y = Minimum[1]; Y <= Maximum[1]; ++Y)
					for (std::int64_t Z = Minimum[2]; Z <= Maximum[2]; ++Z) {
						Mutable->Cells[{
							Entry.PackageSpaceKey,
							{static_cast<std::int32_t>(X), static_cast<std::int32_t>(Y), static_cast<std::int32_t>(Z)},
						}].push_back(Index);
						++Mutable->Memberships;
					}
		}
		State = std::move(Mutable);
	}

	std::vector<std::size_t> PackageContentCoarseIndex::Query(
		std::string_view PackageSpaceKey,
		const PackageCoarseBounds &Bounds,
		std::size_t MaximumResults
	) const {
		std::vector<std::size_t> Result;
		if (!State || MaximumResults == 0 || !BoundsValid(Bounds) || PackageSpaceKey.empty()) return Result;
		std::array<std::int32_t, 3> Minimum{}, Maximum{};
		std::uint64_t CellCount = 1;
		for (std::size_t Axis = 0; Axis < 3; ++Axis) {
			const auto A = std::floor(Bounds.Minimum[Axis] / State->CellSize);
			const auto B = std::floor(Bounds.Maximum[Axis] / State->CellSize);
			if (A < std::numeric_limits<std::int32_t>::min() || B > std::numeric_limits<std::int32_t>::max()) return Result;
			Minimum[Axis] = static_cast<std::int32_t>(A);
			Maximum[Axis] = static_cast<std::int32_t>(B);
			const auto Extent = static_cast<std::uint64_t>(
				static_cast<std::int64_t>(Maximum[Axis]) - Minimum[Axis] + 1
			);
			if (Extent > 4096 / CellCount) return Result;
			CellCount *= Extent;
		}
		std::unordered_set<std::size_t> Seen;
		for (std::int64_t X = Minimum[0]; X <= Maximum[0] && Result.size() < MaximumResults; ++X)
			for (std::int64_t Y = Minimum[1]; Y <= Maximum[1] && Result.size() < MaximumResults; ++Y)
				for (std::int64_t Z = Minimum[2]; Z <= Maximum[2] && Result.size() < MaximumResults; ++Z) {
					auto Found = State->Cells.find({
						std::string(PackageSpaceKey),
						{static_cast<std::int32_t>(X), static_cast<std::int32_t>(Y), static_cast<std::int32_t>(Z)},
					});
					if (Found == State->Cells.end()) continue;
					for (const auto Index : Found->second)
						if (Seen.insert(Index).second) {
							Result.push_back(Index);
							if (Result.size() == MaximumResults) break;
						}
				}
		std::ranges::sort(Result);
		return Result;
	}

	std::size_t PackageContentCoarseIndex::GetMembershipCount() const { return State ? State->Memberships : 0; }

	bool ContentRequestContext::IsCancelled() const {
		return Cancelled && Cancelled->load(std::memory_order_acquire);
	}
	bool ContentRequestContext::IsExpired() const { return std::chrono::steady_clock::now() >= Deadline; }

	struct LocalPackageContentProvider::Impl {
		std::filesystem::path Root;
		std::string ManifestReference;
		AssetContentId ExpectedManifestDigest;
		std::mutex Mutex;
		bool Started = false;
		std::optional<PackageContentManifest> Manifest;
		std::unordered_map<std::string, PackageContentEntry> Entries;
	};

	LocalPackageContentProvider::LocalPackageContentProvider(
		std::filesystem::path PackageRoot,
		std::string ManifestReference,
		AssetContentId ExpectedManifestDigest
	) : State(std::make_shared<Impl>()) {
		State->Root = std::move(PackageRoot);
		State->ManifestReference = std::move(ManifestReference);
		State->ExpectedManifestDigest = ExpectedManifestDigest;
	}

	ContentProviderLifecycleResult LocalPackageContentProvider::Start(const ContentRequestContext &Context) {
		if (Context.IsCancelled() || Context.IsExpired()) return std::unexpected(CancelledError(Context));
		if (!State->Root.is_absolute() || !IsSafeBlobReference(State->ManifestReference) ||
			!State->ExpectedManifestDigest.IsValid())
			return std::unexpected(ContentProviderError{ContentProviderErrorCode::InvalidResponse, "local package configuration is invalid"});
		std::error_code Error;
		auto Canonical = std::filesystem::weakly_canonical(State->Root, Error);
		if (Error || !std::filesystem::is_directory(Canonical, Error) || std::filesystem::is_symlink(Canonical, Error))
			return std::unexpected(ContentProviderError{ContentProviderErrorCode::Unavailable, "local package root is unavailable"});
		std::scoped_lock Lock(State->Mutex);
		State->Root = std::move(Canonical);
		State->Started = true;
		return {};
	}

	void LocalPackageContentProvider::Stop() {
		std::scoped_lock Lock(State->Mutex);
		State->Started = false;
		State->Manifest.reset();
		State->Entries.clear();
	}

	ContentManifestProviderResult LocalPackageContentProvider::GetManifest(
		const ContentRequestContext &Context,
		const PackageContentNamespace &Package
	) {
		if (Context.IsCancelled() || Context.IsExpired()) return std::unexpected(CancelledError(Context));
		std::filesystem::path Root;
		std::string Reference;
		{
			std::scoped_lock Lock(State->Mutex);
			if (!State->Started)
				return std::unexpected(ContentProviderError{ContentProviderErrorCode::Unavailable, "local provider is stopped"});
			Root = State->Root;
			Reference = State->ManifestReference;
		}
		auto ManifestPath = ResolveLocalContentPath(Root, Reference);
		if (!ManifestPath) return std::unexpected(ManifestPath.error());
		auto Encoded = ReadBoundedFile(*ManifestPath, MaximumPackageContentManifestBytes);
		if (!Encoded) return std::unexpected(Encoded.error());
		const auto Bytes = std::span(reinterpret_cast<const std::uint8_t *>(Encoded->data()), Encoded->size());
		if (AssetContentId::Hash(Bytes) != State->ExpectedManifestDigest)
			return std::unexpected(ContentProviderError{ContentProviderErrorCode::InvalidResponse, "manifest digest mismatch"});
		auto Parsed = ParsePackageContentManifest(*Encoded);
		if (!Parsed || Parsed->Package != Package)
			return std::unexpected(ContentProviderError{ContentProviderErrorCode::InvalidResponse, "manifest namespace mismatch"});
		{
			std::scoped_lock Lock(State->Mutex);
			if (!State->Started)
				return std::unexpected(ContentProviderError{ContentProviderErrorCode::Cancelled, "local provider stopped during request"});
			State->Entries.clear();
			for (const auto &Entry : Parsed->Entries) State->Entries.emplace(Entry.Key, Entry);
			State->Manifest = *Parsed;
		}
		return *Encoded;
	}

	ContentPayloadProviderResult LocalPackageContentProvider::GetContent(
		const ContentRequestContext &Context,
		const PackageContentIdentity &Identity
	) {
		if (Context.IsCancelled() || Context.IsExpired()) return std::unexpected(CancelledError(Context));
		std::filesystem::path Root;
		PackageContentEntry Entry;
		{
			std::scoped_lock Lock(State->Mutex);
			if (!State->Started || !State->Manifest)
				return std::unexpected(ContentProviderError{ContentProviderErrorCode::Unavailable, "manifest is not available"});
			if (State->Manifest->Package != Identity.Package)
				return std::unexpected(ContentProviderError{ContentProviderErrorCode::NotFound, "package namespace is absent"});
			auto Found = State->Entries.find(Identity.Key);
			if (Found == State->Entries.end())
				return std::unexpected(ContentProviderError{ContentProviderErrorCode::NotFound, "content key is absent"});
			Entry = Found->second;
			Root = State->Root;
		}
		auto BlobPath = ResolveLocalContentPath(Root, Entry.BlobReference);
		if (!BlobPath) return std::unexpected(BlobPath.error());
		auto Encoded = ReadBoundedFile(*BlobPath, static_cast<std::size_t>(Entry.CompressedBytes));
		if (!Encoded) return std::unexpected(Encoded.error());
		if (Encoded->size() != Entry.CompressedBytes)
			return std::unexpected(ContentProviderError{ContentProviderErrorCode::InvalidResponse, "content byte length mismatch"});
		auto Bytes = std::make_shared<std::vector<std::uint8_t>>(Encoded->begin(), Encoded->end());
		return ContentPayload{Identity, Entry.Digest, std::move(Bytes)};
	}

	struct ContentAvailabilityService::Impl {
		struct ContentTimes final {
			AvailabilityTimePoint ProviderStarted{};
			AvailabilityTimePoint BytesReceived{};
			AvailabilityTimePoint VerificationFinished{};
			AvailabilityTimePoint PreparationStarted{};
			AvailabilityTimePoint WorkerPreparationFinished{};
		};
		struct PreparedContent final {
			std::string Key;
			std::shared_ptr<const std::vector<std::uint8_t>> Bytes;
			InstanceSerialization::Internal::PreparedInstanceDocumentPtr Document;
			std::size_t EncodedBytes = 0;
			bool CacheHit = false;
			ContentTimes Times;
		};
		struct Record {
			ContentResidencyState Residency = ContentResidencyState::Unavailable;
			std::uint32_t DirectDemand = 0;
			std::uint32_t DependencyDemand = 0;
			std::uint32_t Pins = 0;
			std::shared_ptr<std::atomic_bool> RequestCancelled;
			std::shared_ptr<const std::vector<std::uint8_t>> CachedPayload;
			InstanceSerialization::Internal::PreparedInstanceDocumentPtr Prepared;
			std::size_t EncodedBytes = 0;
			std::optional<AvailabilityTimePoint> AcceptedAt;
			std::optional<AvailabilityTimePoint> QueuedAt;
			ContentTimes Times;
			std::shared_ptr<Instance> Root;
			std::unordered_set<const Instance *> PackageObjects;
		};
		struct PreparedManifest final {
			PackageContentManifest Manifest;
			std::unordered_map<std::string, std::size_t> Indices;
			std::vector<std::vector<std::size_t>> Dependents;
			std::vector<Record> Records;
			std::size_t EncodedBytes = 0;
		};
		enum class CompletionKind : std::uint8_t { Manifest, Content };
		struct Completion {
			CompletionKind Kind = CompletionKind::Manifest;
			std::uint64_t Generation = 0;
			std::string Key;
			std::size_t ReservedBytes = 0;
			std::expected<PreparedManifest, ContentProviderError> Manifest = std::unexpected(ContentProviderError{});
			std::expected<PreparedContent, ContentProviderError> Content = std::unexpected(ContentProviderError{});
		};

		std::shared_ptr<DataModel> World;
		std::shared_ptr<Workspace> WorkspaceValue;
		ContentAvailabilityConfiguration Configuration;
		std::function<void(std::string, std::string)> Diagnostic;
		JobSystem Jobs;
		std::shared_ptr<std::atomic_bool> Cancelled = std::make_shared<std::atomic_bool>(false);
		std::uint64_t Generation = 1;
		mutable std::mutex CompletionMutex;
		std::vector<Completion> Completions;
		std::size_t CompletionBytes = 0;
		std::size_t CompletionBytesHighWater = 0;
		std::optional<PackageContentManifest> Manifest;
		std::unordered_map<std::string, std::size_t> Indices;
		std::vector<std::vector<std::size_t>> Dependents;
		std::vector<Record> Records;
		ContentAvailabilityMetrics Metrics;
		std::size_t InFlight = 0;
		std::size_t ReservedCompletionBytes = 0;
		std::size_t Pending = 0;
		std::size_t CachedBytes = 0;
		bool ManifestInFlight = false;
		bool ManifestFailed = false;
		bool Stopped = false;

		Impl(
			std::shared_ptr<DataModel> WorldValue,
			std::shared_ptr<Workspace> WorkspaceInput,
			ContentAvailabilityConfiguration Input,
			std::function<void(std::string, std::string)> DiagnosticInput
		) : World(std::move(WorldValue)), WorkspaceValue(std::move(WorkspaceInput)),
			Configuration(std::move(Input)), Diagnostic(std::move(DiagnosticInput)), Jobs(Configuration.Limits.WorkerCount) {}

		void Report(std::string Code, std::string Message) {
			if (Diagnostic) Diagnostic(std::move(Code), std::move(Message));
		}

		ContentRequestContext Context() const {
			return {Cancelled, std::chrono::steady_clock::now() + Configuration.Limits.RequestTimeout, Generation};
		}

		std::size_t Desire(const Record &Value) const {
			return static_cast<std::size_t>(Value.DirectDemand) + Value.DependencyDemand + Value.Pins;
		}

		void SubmitManifest() {
			ManifestInFlight = true;
			++InFlight;
			Metrics.InFlightHighWater = std::max<std::uint64_t>(Metrics.InFlightHighWater, InFlight);
			auto Provider = Configuration.Provider;
			auto Package = Configuration.Package;
			auto RequestContext = Context();
			Jobs.Submit([this, Provider = std::move(Provider), Package, RequestContext] {
				Completion Result;
				Result.Kind = CompletionKind::Manifest;
				Result.Generation = RequestContext.SessionGeneration;
				auto Encoded = Provider->GetManifest(RequestContext, Package);
				if (!Encoded) {
					Result.Manifest = std::unexpected(Encoded.error());
				} else {
					const auto Bytes = std::span(
						reinterpret_cast<const std::uint8_t *>(Encoded->data()), Encoded->size()
					);
					if (AssetContentId::Hash(Bytes) != Configuration.ManifestDigest) {
						Result.Manifest = std::unexpected(ContentProviderError{
							ContentProviderErrorCode::InvalidResponse, "content provider returned a manifest with the wrong digest"
						});
					} else if (auto Parsed = ParsePackageContentManifest(*Encoded);
						!Parsed || Parsed->Package != Package) {
						Result.Manifest = std::unexpected(ContentProviderError{
							ContentProviderErrorCode::InvalidResponse,
							Parsed ? "manifest namespace differs from the active package" : Parsed.error()
						});
					} else {
						PreparedManifest Prepared;
						Prepared.EncodedBytes = Encoded->size();
						Prepared.Manifest = std::move(*Parsed);
						Prepared.Records.resize(Prepared.Manifest.Entries.size());
						Prepared.Dependents.resize(Prepared.Manifest.Entries.size());
						Prepared.Indices.reserve(Prepared.Manifest.Entries.size());
						for (std::size_t Index = 0; Index < Prepared.Manifest.Entries.size(); ++Index)
							Prepared.Indices.emplace(Prepared.Manifest.Entries[Index].Key, Index);
						for (std::size_t Index = 0; Index < Prepared.Manifest.Entries.size(); ++Index)
							for (const auto &Dependency : Prepared.Manifest.Entries[Index].Dependencies)
								Prepared.Dependents[Prepared.Indices.at(Dependency)].push_back(Index);
						Result.Manifest = std::move(Prepared);
					}
				}
				std::scoped_lock Lock(CompletionMutex);
				Completions.push_back(std::move(Result));
			});
		}

		void SubmitContent(std::size_t Index) {
			auto &RecordValue = Records[Index];
			RecordValue.Residency = ContentResidencyState::Acquiring;
			RecordValue.RequestCancelled = std::make_shared<std::atomic_bool>(false);
			--Pending;
			++InFlight;
			++Metrics.Acquisitions;
			Metrics.InFlightHighWater = std::max<std::uint64_t>(Metrics.InFlightHighWater, InFlight);
			auto Provider = Configuration.Provider;
			auto Identity = PackageContentIdentity{Configuration.Package, Manifest->Entries[Index].Key};
			const auto ExpectedDigest = Manifest->Entries[Index].Digest;
			const auto ExpectedBytes = static_cast<std::size_t>(Manifest->Entries[Index].CompressedBytes);
			ReservedCompletionBytes += ExpectedBytes;
			auto RequestContext = Context();
			RequestContext.Cancelled = RecordValue.RequestCancelled;
			Jobs.Submit([this, Provider = std::move(Provider), Identity = std::move(Identity), ExpectedDigest, ExpectedBytes, RequestContext] {
				Completion Result;
				Result.Kind = CompletionKind::Content;
				Result.Generation = RequestContext.SessionGeneration;
				Result.Key = Identity.Key;
				Result.ReservedBytes = ExpectedBytes;
				PreparedContent Prepared;
				Prepared.Key = Identity.Key;
				Prepared.Times.ProviderStarted = AvailabilityClock::now();
				auto Payload = Provider->GetContent(RequestContext, Identity);
				Prepared.Times.BytesReceived = AvailabilityClock::now();
				if (!Payload) {
					Result.Content = std::unexpected(Payload.error());
				} else if (RequestContext.IsCancelled() || RequestContext.IsExpired()) {
					Result.Content = std::unexpected(CancelledError(RequestContext));
				} else if (Payload->Identity != Identity || Payload->Digest != ExpectedDigest || !Payload->Bytes ||
					Payload->Bytes->size() != ExpectedBytes || AssetContentId::Hash(*Payload->Bytes) != ExpectedDigest) {
					Result.Content = std::unexpected(ContentProviderError{
						ContentProviderErrorCode::InvalidResponse, "content payload identity, digest, or size is invalid"
					});
				} else {
					Prepared.EncodedBytes = Payload->Bytes->size();
					Prepared.Bytes = Payload->Bytes;
					Prepared.Times.VerificationFinished = AvailabilityClock::now();
					Prepared.Times.PreparationStarted = Prepared.Times.VerificationFinished;
					auto Document = InstanceSerialization::Internal::PrepareDetachedJson(*Payload->Bytes);
					Prepared.Times.WorkerPreparationFinished = AvailabilityClock::now();
					if (RequestContext.IsCancelled() || RequestContext.IsExpired()) {
						Result.Content = std::unexpected(CancelledError(RequestContext));
					} else if (!Document) {
						Result.Content = std::unexpected(ContentProviderError{
							ContentProviderErrorCode::InvalidResponse, std::move(Document.error())
						});
					} else {
						Prepared.Document = std::move(*Document);
						Result.Content = std::move(Prepared);
					}
				}
				std::scoped_lock Lock(CompletionMutex);
				if (Result.Content &&
					Result.Content->EncodedBytes <= Configuration.Limits.MaximumCompletedPayloadBytes -
						std::min(CompletionBytes, Configuration.Limits.MaximumCompletedPayloadBytes)) {
					CompletionBytes += Result.Content->EncodedBytes;
					CompletionBytesHighWater = std::max(CompletionBytesHighWater, CompletionBytes);
					Completions.push_back(std::move(Result));
				} else if (!Result.Content) Completions.push_back(std::move(Result));
				else {
					Result.Content = std::unexpected(ContentProviderError{
						ContentProviderErrorCode::ResourceExhausted, "completed-payload queue byte limit reached"
					});
					Completions.push_back(std::move(Result));
				}
			});
		}

		void SubmitCachedContent(std::size_t Index) {
			auto &RecordValue = Records[Index];
			RecordValue.Residency = ContentResidencyState::Acquiring;
			RecordValue.RequestCancelled = std::make_shared<std::atomic_bool>(false);
			--Pending;
			++InFlight;
			++Metrics.CacheHits;
			Metrics.InFlightHighWater = std::max<std::uint64_t>(Metrics.InFlightHighWater, InFlight);
			const auto ExpectedBytes = RecordValue.EncodedBytes;
			ReservedCompletionBytes += ExpectedBytes;
			auto Bytes = RecordValue.CachedPayload;
			auto Key = Manifest->Entries[Index].Key;
			auto RequestContext = Context();
			RequestContext.Cancelled = RecordValue.RequestCancelled;
			Jobs.Submit([this, Bytes = std::move(Bytes), Key = std::move(Key), ExpectedBytes, RequestContext] {
				Completion Result;
				Result.Kind = CompletionKind::Content;
				Result.Generation = RequestContext.SessionGeneration;
				Result.Key = Key;
				Result.ReservedBytes = ExpectedBytes;
				PreparedContent Prepared;
				Prepared.Key = Key;
				Prepared.Bytes = Bytes;
				Prepared.EncodedBytes = ExpectedBytes;
				Prepared.CacheHit = true;
				Prepared.Times.ProviderStarted = AvailabilityClock::now();
				Prepared.Times.BytesReceived = Prepared.Times.ProviderStarted;
				Prepared.Times.VerificationFinished = Prepared.Times.BytesReceived;
				Prepared.Times.PreparationStarted = Prepared.Times.VerificationFinished;
				auto Document = InstanceSerialization::Internal::PrepareDetachedJson(*Bytes);
				Prepared.Times.WorkerPreparationFinished = AvailabilityClock::now();
				if (RequestContext.IsCancelled() || RequestContext.IsExpired()) {
					Result.Content = std::unexpected(CancelledError(RequestContext));
				} else if (!Document) {
					Result.Content = std::unexpected(ContentProviderError{
						ContentProviderErrorCode::InvalidResponse, std::move(Document.error())
					});
				} else {
					Prepared.Document = std::move(*Document);
					Result.Content = std::move(Prepared);
				}
				std::scoped_lock Lock(CompletionMutex);
				if (Result.Content) {
					CompletionBytes += Result.Content->EncodedBytes;
					CompletionBytesHighWater = std::max(CompletionBytesHighWater, CompletionBytes);
				}
				Completions.push_back(std::move(Result));
			});
		}

		void AdjustDependencies(std::size_t Index, bool Add) {
			std::vector<std::size_t> PendingDependencies;
			for (const auto &Key : Manifest->Entries[Index].Dependencies) PendingDependencies.push_back(Indices.at(Key));
			while (!PendingDependencies.empty()) {
				const auto Dependency = PendingDependencies.back();
				PendingDependencies.pop_back();
				auto &RecordValue = Records[Dependency];
				const auto PriorDesire = Desire(RecordValue);
				if (Add) {
					if (RecordValue.DependencyDemand == std::numeric_limits<std::uint32_t>::max()) continue;
					++RecordValue.DependencyDemand;
					if (PriorDesire != 0) continue;
					RecordValue.AcceptedAt = AvailabilityClock::now();
				} else {
					if (RecordValue.DependencyDemand == 0) continue;
					--RecordValue.DependencyDemand;
					if (Desire(RecordValue) != 0) continue;
				}
				for (const auto &Key : Manifest->Entries[Dependency].Dependencies)
					PendingDependencies.push_back(Indices.at(Key));
			}
		}

		bool Request(std::size_t Index) {
			auto &RecordValue = Records[Index];
			++Metrics.Requests;
			if (RecordValue.DirectDemand == std::numeric_limits<std::uint32_t>::max()) return false;
			const auto PriorDesire = Desire(RecordValue);
			if (RecordValue.DirectDemand++ != 0) {
				++Metrics.AcquisitionDeduplications;
				return true;
			}
			if (PriorDesire == 0) {
				RecordValue.AcceptedAt = AvailabilityClock::now();
				AdjustDependencies(Index, true);
			}
			return true;
		}

		bool Release(std::size_t Index) {
			auto &RecordValue = Records[Index];
			if (RecordValue.DirectDemand == 0) return false;
			--RecordValue.DirectDemand;
			if (Desire(RecordValue) == 0) AdjustDependencies(Index, false);
			return true;
		}

		void StartRequests() {
			if (!Manifest) return;
			for (std::size_t Index = 0; Index < Records.size(); ++Index) {
				auto &RecordValue = Records[Index];
				if (Desire(RecordValue) == 0) {
					if (RecordValue.Residency == ContentResidencyState::Available) {
						RecordValue.Prepared.reset();
						RecordValue.Residency = ContentResidencyState::Unavailable;
					}
					if (RecordValue.Residency == ContentResidencyState::Requested) {
						RecordValue.Residency = ContentResidencyState::Unavailable;
						if (Pending != 0) --Pending;
						++Metrics.Cancellations;
					}
					if (RecordValue.Residency == ContentResidencyState::Acquiring && RecordValue.RequestCancelled &&
						!RecordValue.RequestCancelled->exchange(true, std::memory_order_acq_rel))
						++Metrics.Cancellations;
					continue;
				}
				if (RecordValue.Residency == ContentResidencyState::Unavailable) {
					if (Pending >= Configuration.Limits.MaximumPendingRequests) continue;
					RecordValue.Residency = ContentResidencyState::Requested;
					RecordValue.QueuedAt = AvailabilityClock::now();
					if (RecordValue.AcceptedAt)
						RecordDuration(Metrics.Timing.AcceptedToQueued, *RecordValue.AcceptedAt, *RecordValue.QueuedAt);
					++Pending;
					Metrics.PendingHighWater = std::max<std::uint64_t>(Metrics.PendingHighWater, Pending);
				}
				const auto ExpectedBytes = static_cast<std::size_t>(Manifest->Entries[Index].CompressedBytes);
				if (RecordValue.Residency != ContentResidencyState::Requested) continue;
				if (ExpectedBytes > Configuration.Limits.MaximumCompletedPayloadBytes -
					std::min(ReservedCompletionBytes, Configuration.Limits.MaximumCompletedPayloadBytes)) {
					++Metrics.CompletionCapacityDeferrals;
					continue;
				}
				if (InFlight >= Configuration.Limits.MaximumInFlight) continue;
				if (RecordValue.CachedPayload) SubmitCachedContent(Index);
				else SubmitContent(Index);
			}
		}

		void MakeCacheSpace(std::size_t RequiredBytes, std::size_t PreservedIndex) {
			if (RequiredBytes <= Configuration.Limits.MaximumCachedPayloadBytes -
				std::min(CachedBytes, Configuration.Limits.MaximumCachedPayloadBytes)) return;
			for (std::size_t Index = Records.size(); Index-- > 0;) {
				if (Index == PreservedIndex) continue;
				auto &RecordValue = Records[Index];
				if (!RecordValue.CachedPayload || RecordValue.Residency == ContentResidencyState::Acquiring ||
					(!RecordValue.Root && Desire(RecordValue) != 0)) continue;
				CachedBytes -= RecordValue.EncodedBytes;
				RecordValue.CachedPayload.reset();
				if (!RecordValue.Root) RecordValue.Prepared.reset();
				RecordValue.EncodedBytes = 0;
				if (!RecordValue.Root) RecordValue.Residency = ContentResidencyState::Unavailable;
				++Metrics.CacheEvictions;
				if (RequiredBytes <= Configuration.Limits.MaximumCachedPayloadBytes -
					std::min(CachedBytes, Configuration.Limits.MaximumCachedPayloadBytes)) return;
			}
		}

		bool DependenciesResident(std::size_t Index) const {
			for (const auto &Key : Manifest->Entries[Index].Dependencies)
				if (Records[Indices.at(Key)].Residency != ContentResidencyState::Resident) return false;
			return true;
		}

		bool HasResidentDependent(std::size_t Index) const {
			return std::ranges::any_of(Dependents[Index], [&](std::size_t Dependent) {
				return Records[Dependent].Residency == ContentResidencyState::Resident ||
					Records[Dependent].Residency == ContentResidencyState::Evicting;
			});
		}
	};

	ContentAvailabilityService::ContentAvailabilityService(
		std::shared_ptr<DataModel> World,
		std::shared_ptr<Workspace> WorkspaceValue,
		ContentAvailabilityConfiguration Configuration,
		std::function<void(std::string, std::string)> Diagnostic
	) {
		if (!World || !WorkspaceValue || WorkspaceValue->GetDataModel() != World || !Configuration.Provider ||
			!Configuration.Package.Project.IsValid() || Configuration.Package.PackageVersion == 0 ||
			!Configuration.ManifestDigest.IsValid() || Configuration.Limits.WorkerCount == 0 ||
			Configuration.Limits.WorkerCount > MaximumContentAvailabilityWorkers ||
			Configuration.Limits.MaximumInFlight == 0 ||
			Configuration.Limits.MaximumInFlight > MaximumContentAvailabilityInFlight ||
			Configuration.Limits.MaximumPendingRequests == 0 ||
			Configuration.Limits.MaximumPendingRequests > MaximumContentAvailabilityPendingRequests ||
			Configuration.Limits.MaximumCompletedPayloadBytes < MaximumPackageContentPayloadBytes ||
			Configuration.Limits.MaximumCompletedPayloadBytes > MaximumContentAvailabilityCompletedPayloadBytes ||
			Configuration.Limits.MaximumCachedPayloadBytes < MaximumPackageContentPayloadBytes ||
			Configuration.Limits.MaximumCachedPayloadBytes > MaximumContentAvailabilityCachedPayloadBytes ||
			Configuration.Limits.MaximumCompletionsPerTick == 0 ||
			Configuration.Limits.MaximumCompletionsPerTick > MaximumContentAvailabilityCompletionsPerTick ||
			Configuration.Limits.MaximumAdmissionUnitsPerTick == 0 ||
			Configuration.Limits.MaximumAdmissionUnitsPerTick > MaximumContentAvailabilityAdmissionUnitsPerTick ||
			Configuration.Limits.MaximumAdmissionObjectsPerTick < MaximumPackageContentObjectsPerUnit ||
			Configuration.Limits.MaximumAdmissionObjectsPerTick > MaximumContentAvailabilityAdmissionObjectsPerTick ||
			Configuration.Limits.MaximumAdmissionBytesPerTick < MaximumPackageContentPayloadBytes ||
			Configuration.Limits.MaximumAdmissionBytesPerTick > MaximumContentAvailabilityAdmissionBytesPerTick ||
			Configuration.Limits.MaximumEvictionUnitsPerTick == 0 ||
			Configuration.Limits.MaximumEvictionUnitsPerTick > MaximumContentAvailabilityEvictionUnitsPerTick ||
			Configuration.Limits.MaximumEvictionObjectsPerTick < MaximumPackageContentObjectsPerUnit ||
			Configuration.Limits.MaximumEvictionObjectsPerTick > MaximumContentAvailabilityEvictionObjectsPerTick ||
			Configuration.Limits.RequestTimeout < std::chrono::milliseconds(10) ||
			Configuration.Limits.RequestTimeout > std::chrono::seconds(10))
			throw std::invalid_argument("Content availability configuration is invalid");
		State = std::make_unique<Impl>(
			std::move(World), std::move(WorkspaceValue), std::move(Configuration), std::move(Diagnostic)
		);
		auto Started = State->Configuration.Provider->Start(State->Context());
		if (!Started) throw std::runtime_error("Content provider could not start: " + Started.error().Message);
		State->SubmitManifest();
	}

	ContentAvailabilityService::~ContentAvailabilityService() { Stop(); }

	void ContentAvailabilityService::Step() {
		if (!State || State->Stopped) return;
		DurationScope StepDuration(State->Metrics.Timing.Step);
		std::vector<Impl::Completion> Completed;
		{
			std::scoped_lock Lock(State->CompletionMutex);
			const auto Count = std::min(State->Configuration.Limits.MaximumCompletionsPerTick, State->Completions.size());
			Completed.reserve(Count);
			for (std::size_t Index = 0; Index < Count; ++Index) {
				auto Item = std::move(State->Completions[Index]);
				if (Item.Content) State->CompletionBytes -= Item.Content->EncodedBytes;
				Completed.push_back(std::move(Item));
			}
			State->Completions.erase(State->Completions.begin(), State->Completions.begin() + static_cast<std::ptrdiff_t>(Count));
			State->Metrics.CompletedPayloadBytesHighWater = std::max<std::uint64_t>(
				State->Metrics.CompletedPayloadBytesHighWater, State->CompletionBytesHighWater
			);
		}
		for (auto &Completion : Completed) {
			if (State->InFlight != 0) --State->InFlight;
			if (Completion.ReservedBytes <= State->ReservedCompletionBytes)
				State->ReservedCompletionBytes -= Completion.ReservedBytes;
			else State->ReservedCompletionBytes = 0;
			if (Completion.Generation != State->Generation) {
				++State->Metrics.StaleCompletionsRejected;
				++State->Metrics.Cancellations;
				continue;
			}
			if (Completion.Kind == Impl::CompletionKind::Manifest) {
				State->ManifestInFlight = false;
				if (!Completion.Manifest) {
					State->ManifestFailed = true;
					++State->Metrics.Failures;
					State->Report("ManifestUnavailable", Completion.Manifest.error().Message);
					continue;
				}
				auto Prepared = std::move(*Completion.Manifest);
				State->Metrics.ManifestBytes = Prepared.EncodedBytes;
				State->Manifest = std::move(Prepared.Manifest);
				State->Indices = std::move(Prepared.Indices);
				State->Dependents = std::move(Prepared.Dependents);
				State->Records = std::move(Prepared.Records);
				for (std::size_t Index = 0; Index < State->Manifest->Entries.size(); ++Index)
					if (State->Configuration.Mode == ContentResidencyMode::FullyResident || HasPackageContentFlag(
							State->Manifest->Entries[Index].Flags, PackageContentFlags::RequiredAtBootstrap
						)) State->Request(Index);
				continue;
			}
			auto Found = State->Indices.find(Completion.Key);
			if (Found == State->Indices.end()) continue;
			auto &RecordValue = State->Records[Found->second];
			if (RecordValue.Residency != ContentResidencyState::Acquiring) continue;
			if (State->Desire(RecordValue) == 0 ||
				(RecordValue.RequestCancelled && RecordValue.RequestCancelled->load(std::memory_order_acquire))) {
				if (RecordValue.RequestCancelled &&
					!RecordValue.RequestCancelled->exchange(true, std::memory_order_acq_rel))
					++State->Metrics.Cancellations;
				RecordValue.RequestCancelled.reset();
				RecordValue.Residency = ContentResidencyState::Unavailable;
				continue;
			}
			RecordValue.RequestCancelled.reset();
			if (!Completion.Content) {
				RecordValue.Residency = ContentResidencyState::Failed;
				++State->Metrics.Failures;
				State->Report("AcquisitionFailed", Completion.Content.error().Message);
				continue;
			}
			const auto &Entry = State->Manifest->Entries[Found->second];
			const auto &Prepared = *Completion.Content;
			if (Prepared.Key != Entry.Key || !Prepared.Document || Prepared.EncodedBytes != Entry.CompressedBytes) {
				RecordValue.Residency = ContentResidencyState::Failed;
				++State->Metrics.Failures;
				State->Report("PayloadRejected", "Content payload identity or size is invalid");
				continue;
			}
			if (!RecordValue.CachedPayload) {
				State->MakeCacheSpace(Prepared.EncodedBytes, Found->second);
				if (Prepared.EncodedBytes > State->Configuration.Limits.MaximumCachedPayloadBytes -
					std::min(State->CachedBytes, State->Configuration.Limits.MaximumCachedPayloadBytes)) {
					RecordValue.Residency = ContentResidencyState::Failed;
					++State->Metrics.Failures;
					State->Report("PayloadRejected", "Content immutable cache byte limit is exhausted");
					continue;
				}
				RecordValue.CachedPayload = Prepared.Bytes;
				RecordValue.EncodedBytes = Prepared.EncodedBytes;
				State->CachedBytes += Prepared.EncodedBytes;
				State->Metrics.CachedPayloadBytesHighWater = std::max<std::uint64_t>(
					State->Metrics.CachedPayloadBytesHighWater, State->CachedBytes
				);
			}
			RecordValue.Prepared = Prepared.Document;
			RecordValue.Times = Prepared.Times;
			if (!Prepared.CacheHit) {
				State->Metrics.BytesAcquired += Prepared.EncodedBytes;
				State->Metrics.BytesVerified += Prepared.EncodedBytes;
			}
			RecordValue.Residency = ContentResidencyState::Available;
		}

		State->StartRequests();
		if (!State->Manifest) return;
		State->Metrics.AdmissionPendingHighWater = std::max<std::uint64_t>(
			State->Metrics.AdmissionPendingHighWater,
			std::ranges::count_if(State->Records, [](const Impl::Record &RecordValue) {
				return RecordValue.Residency == ContentResidencyState::Available ||
					RecordValue.Residency == ContentResidencyState::Admitting;
			})
		);
		std::size_t Units = 0;
		std::size_t Objects = 0;
		std::size_t Bytes = 0;
		for (std::size_t Index = 0; Index < State->Records.size() && Units < State->Configuration.Limits.MaximumAdmissionUnitsPerTick; ++Index) {
			auto &RecordValue = State->Records[Index];
			const auto &Entry = State->Manifest->Entries[Index];
			if (RecordValue.Residency != ContentResidencyState::Available || State->Desire(RecordValue) == 0) continue;
			if (!State->DependenciesResident(Index) ||
				Objects + Entry.ObjectCount > State->Configuration.Limits.MaximumAdmissionObjectsPerTick ||
				Bytes + Entry.UncompressedBytes > State->Configuration.Limits.MaximumAdmissionBytesPerTick) {
				++State->Metrics.AdmissionDeferrals;
				continue;
			}
			RecordValue.Residency = ContentResidencyState::Admitting;
			auto Prepared = InstanceSerialization::Internal::MaterializeDetachedJson(RecordValue.Prepared);
			const auto PreparationFinished = AvailabilityClock::now();
			if (!Prepared.Ok || !Prepared.Instance || Prepared.Instance->IsA("DataModel") ||
				Prepared.ObjectsDecoded != Entry.ObjectCount) {
				State->CachedBytes -= RecordValue.EncodedBytes;
				RecordValue.CachedPayload.reset();
				RecordValue.Prepared.reset();
				RecordValue.EncodedBytes = 0;
				RecordValue.Residency = ContentResidencyState::Failed;
				++State->Metrics.Failures;
				const auto Detail = !Prepared.Errors.empty()
					? Prepared.Errors.front()
					: "decoded " + std::to_string(Prepared.ObjectsDecoded) + " objects; expected " +
						std::to_string(Entry.ObjectCount);
				State->Report("AdmissionDecodeRejected", "Content payload did not decode to its declared detached subtree: " + Detail);
				continue;
			}
			auto Descendants = Prepared.Instance->GetDescendants();
			RecordValue.PackageObjects.clear();
			RecordValue.PackageObjects.reserve(Descendants.size() + 1);
			RecordValue.PackageObjects.insert(Prepared.Instance.get());
			for (const auto &Object : Descendants) RecordValue.PackageObjects.insert(Object.get());
			const auto CommitStarted = AvailabilityClock::now();
			try {
				Prepared.Instance->SetParent(State->WorkspaceValue);
			} catch (const std::exception &Error) {
				State->CachedBytes -= RecordValue.EncodedBytes;
				RecordValue.CachedPayload.reset();
				RecordValue.Prepared.reset();
				RecordValue.EncodedBytes = 0;
				RecordValue.PackageObjects.clear();
				RecordValue.Residency = ContentResidencyState::Failed;
				++State->Metrics.Failures;
				State->Report("AdmissionCommitRejected", Error.what());
				continue;
			}
			RecordValue.Root = std::move(Prepared.Instance);
			RecordValue.Residency = ContentResidencyState::Resident;
			const auto ResidentAt = AvailabilityClock::now();
			RecordValue.Prepared.reset();
			if (RecordValue.QueuedAt) {
				RecordDuration(State->Metrics.Timing.QueuedToProviderStart, *RecordValue.QueuedAt, RecordValue.Times.ProviderStarted);
				RecordDuration(State->Metrics.Timing.Provider, RecordValue.Times.ProviderStarted, RecordValue.Times.BytesReceived);
				RecordDuration(State->Metrics.Timing.Verification, RecordValue.Times.BytesReceived, RecordValue.Times.VerificationFinished);
				RecordDuration(State->Metrics.Timing.VerificationToPreparation, RecordValue.Times.VerificationFinished, RecordValue.Times.PreparationStarted);
				RecordDuration(State->Metrics.Timing.WorkerPreparation, RecordValue.Times.PreparationStarted, RecordValue.Times.WorkerPreparationFinished);
				RecordDuration(State->Metrics.Timing.Preparation, RecordValue.Times.PreparationStarted, PreparationFinished);
				RecordDuration(State->Metrics.Timing.PreparationToCommit, PreparationFinished, CommitStarted);
				RecordDuration(State->Metrics.Timing.Commit, CommitStarted, ResidentAt);
			}
			if (RecordValue.AcceptedAt)
				RecordDuration(State->Metrics.Timing.EndToEnd, *RecordValue.AcceptedAt, ResidentAt);
			++Units;
			Objects += Entry.ObjectCount;
			Bytes += static_cast<std::size_t>(Entry.UncompressedBytes);
			++State->Metrics.Admissions;
			State->Metrics.ObjectsAdmitted += Entry.ObjectCount;
		}

		Units = 0;
		Objects = 0;
		for (std::size_t Index = State->Records.size(); Index-- > 0 && Units < State->Configuration.Limits.MaximumEvictionUnitsPerTick;) {
			auto &RecordValue = State->Records[Index];
			const auto &Entry = State->Manifest->Entries[Index];
			if (RecordValue.Residency != ContentResidencyState::Resident || State->Desire(RecordValue) != 0 ||
				State->HasResidentDependent(Index) ||
				Objects + Entry.ObjectCount > State->Configuration.Limits.MaximumEvictionObjectsPerTick) continue;
			if (!RecordValue.Root || RecordValue.Root->GetDestroyed()) {
				RecordValue.Root.reset();
				RecordValue.PackageObjects.clear();
				RecordValue.Residency = RecordValue.Prepared ? ContentResidencyState::Available : ContentResidencyState::Unavailable;
				continue;
			}
			auto Live = RecordValue.Root->GetDescendants();
			bool RuntimeChild = !RecordValue.PackageObjects.contains(RecordValue.Root.get());
			for (const auto &Object : Live)
				if (!RecordValue.PackageObjects.contains(Object.get())) {
					RuntimeChild = true;
					break;
				}
			if (RuntimeChild) continue;
			RecordValue.Residency = ContentResidencyState::Evicting;
			RecordValue.Root->Destroy();
			RecordValue.Root.reset();
			RecordValue.PackageObjects.clear();
			RecordValue.Residency = RecordValue.Prepared ? ContentResidencyState::Available : ContentResidencyState::Unavailable;
			++Units;
			Objects += Entry.ObjectCount;
			++State->Metrics.Evictions;
			State->Metrics.ObjectsEvicted += Entry.ObjectCount;
		}
		State->StartRequests();
	}

	void ContentAvailabilityService::Stop() {
		if (!State || State->Stopped) return;
		State->Stopped = true;
		State->Cancelled->store(true, std::memory_order_release);
		for (auto &RecordValue : State->Records)
			if (RecordValue.RequestCancelled) RecordValue.RequestCancelled->store(true, std::memory_order_release);
		++State->Generation;
		State->Metrics.Cancellations += State->InFlight + State->Pending;
		State->Configuration.Provider->Stop();
		State->Jobs.Shutdown(false);
		for (auto &RecordValue : State->Records) {
			if (RecordValue.Root && !RecordValue.Root->GetDestroyed()) RecordValue.Root->Destroy();
			RecordValue.Root.reset();
			RecordValue.PackageObjects.clear();
			RecordValue.Prepared.reset();
			RecordValue.CachedPayload.reset();
			RecordValue.EncodedBytes = 0;
			RecordValue.Residency = ContentResidencyState::Unavailable;
		}
		State->InFlight = 0;
		State->ReservedCompletionBytes = 0;
		State->Pending = 0;
		State->CachedBytes = 0;
		std::scoped_lock Lock(State->CompletionMutex);
		State->Completions.clear();
		State->CompletionBytes = 0;
	}

	bool ContentAvailabilityService::RequestContent(std::string_view Key) {
		if (!State || State->Stopped || !State->Manifest) return false;
		auto Found = State->Indices.find(std::string(Key));
		return Found != State->Indices.end() && State->Request(Found->second);
	}

	bool ContentAvailabilityService::ReleaseContent(std::string_view Key) {
		if (!State || State->Stopped || !State->Manifest) return false;
		auto Found = State->Indices.find(std::string(Key));
		return Found != State->Indices.end() && State->Release(Found->second);
	}

	bool ContentAvailabilityService::PinContent(std::string_view Key) {
		if (!State || State->Stopped || !State->Manifest) return false;
		auto Found = State->Indices.find(std::string(Key));
		if (Found == State->Indices.end() || State->Records[Found->second].Pins == std::numeric_limits<std::uint32_t>::max()) return false;
		auto &RecordValue = State->Records[Found->second];
		const auto PriorDesire = State->Desire(RecordValue);
		++RecordValue.Pins;
		if (PriorDesire == 0) {
			RecordValue.AcceptedAt = AvailabilityClock::now();
			State->AdjustDependencies(Found->second, true);
		}
		return true;
	}

	bool ContentAvailabilityService::UnpinContent(std::string_view Key) {
		if (!State || State->Stopped || !State->Manifest) return false;
		auto Found = State->Indices.find(std::string(Key));
		if (Found == State->Indices.end() || State->Records[Found->second].Pins == 0) return false;
		auto &RecordValue = State->Records[Found->second];
		--RecordValue.Pins;
		if (State->Desire(RecordValue) == 0) State->AdjustDependencies(Found->second, false);
		return true;
	}

	bool ContentAvailabilityService::RetryContent(std::string_view Key) {
		if (!State || State->Stopped || !State->Manifest) return false;
		auto Found = State->Indices.find(std::string(Key));
		if (Found == State->Indices.end() || State->Records[Found->second].Residency != ContentResidencyState::Failed) return false;
		auto &RecordValue = State->Records[Found->second];
		RecordValue.Residency = ContentResidencyState::Unavailable;
		RecordValue.AcceptedAt = AvailabilityClock::now();
		RecordValue.QueuedAt.reset();
		State->StartRequests();
		return true;
	}

	std::optional<ContentResidencyState> ContentAvailabilityService::GetState(std::string_view Key) const {
		if (!State || !State->Manifest) return std::nullopt;
		auto Found = State->Indices.find(std::string(Key));
		return Found == State->Indices.end() ? std::nullopt : std::optional(State->Records[Found->second].Residency);
	}

	bool ContentAvailabilityService::IsManifestAvailable() const { return State && State->Manifest.has_value(); }
	bool ContentAvailabilityService::IsBootstrapComplete() const {
		if (!State || !State->Manifest) return false;
		for (std::size_t Index = 0; Index < State->Manifest->Entries.size(); ++Index)
			if (HasPackageContentFlag(State->Manifest->Entries[Index].Flags, PackageContentFlags::RequiredAtBootstrap) &&
				State->Records[Index].Residency != ContentResidencyState::Resident) return false;
		return true;
	}
	bool ContentAvailabilityService::IsFullyResident() const {
		if (!State || !State->Manifest) return false;
		return std::ranges::all_of(State->Records, [](const Impl::Record &RecordValue) {
			return RecordValue.Residency == ContentResidencyState::Resident;
		});
	}
	std::size_t ContentAvailabilityService::GetActiveRequestCount() const {
		return State ? State->InFlight + State->Pending : 0;
	}
	ContentAvailabilityMetrics ContentAvailabilityService::GetMetrics() const {
		if (!State) return {};
		auto Result = State->Metrics;
		const auto Now = AvailabilityClock::now();
		for (const auto &RecordValue : State->Records) {
			switch (RecordValue.Residency) {
			case ContentResidencyState::Requested:
				++Result.RequestedUnits;
				break;
			case ContentResidencyState::Acquiring:
				++Result.AcquiringUnits;
				break;
			case ContentResidencyState::Available:
			case ContentResidencyState::Admitting:
				++Result.PreparedUnits;
				break;
			case ContentResidencyState::Resident:
				++Result.ResidentUnits;
				break;
			case ContentResidencyState::Evicting:
				++Result.EvictingUnits;
				break;
			case ContentResidencyState::Failed:
				++Result.FailedUnits;
				break;
			case ContentResidencyState::Unavailable:
				break;
			}
			if (State->Desire(RecordValue) != 0 && RecordValue.AcceptedAt &&
				RecordValue.Residency != ContentResidencyState::Resident) {
				const auto Age = static_cast<std::uint64_t>(
					std::chrono::duration_cast<std::chrono::microseconds>(Now - *RecordValue.AcceptedAt).count()
				);
				Result.OldestRequestAgeMicroseconds = std::max(Result.OldestRequestAgeMicroseconds, Age);
			}
		}
		Result.ReservedCompletionPayloadBytes = State->ReservedCompletionBytes;
		Result.CachedPayloadBytes = State->CachedBytes;
		{
			std::scoped_lock Lock(State->CompletionMutex);
			Result.CompletedPayloadBytes = State->CompletionBytes;
		}
		return Result;
	}
	const PackageContentManifest *ContentAvailabilityService::GetManifest() const {
		return State && State->Manifest ? &*State->Manifest : nullptr;
	}
}
