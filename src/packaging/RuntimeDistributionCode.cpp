#include "packaging/RuntimeDistributionCode.hpp"

#include "gargantuan/assets/AssetTypes.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/LuaSourceContainer.hpp"
#include "gargantuan/filesystem/ProjectIdentity.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"
#include "gargantuan/scripting/RuntimeScriptCode.hpp"
#include "packaging/ClientPackage.hpp"

#include <algorithm>
#include <set>

namespace gargantuan::runtime_distribution {
	namespace {
		using Json = JsonCodec::Json;
		using Representation = RuntimeDistributionRepresentation;
		using Diagnostics = RuntimeDistributionDiagnostics;
		constexpr std::size_t MaximumScriptCount = 4096;
		constexpr std::size_t MaximumDecodedBytes = 64 * 1024 * 1024;
		constexpr std::size_t MaximumSnapshotBytes = 16 * 1024 * 1024;

		[[noreturn]] void Reject(const char *Code, const char *Message) {
			throw RuntimeDistributionError(Code, Message);
		}

		std::string String(const Json &Object, const char *Field) {
			if (!Object.contains(Field) || !Object[Field].is_string())
				Reject("InvalidScriptCatalog", "Runtime script metadata is malformed.");
			return Object[Field].get<std::string>();
		}

		std::string ArtifactPath(std::string_view Id, Representation Kind) {
			return "content/scripts/" + std::string(Id) + (Kind == Representation::Protected ? ".gse" : ".luac");
		}

		std::string AssociatedData(
			const Metadata &Value, std::string_view Project, std::string_view SourceId, std::string_view Class,
			Diagnostics Diagnostic
		) {
			return Json{
				{"Domain", "GargantuanRuntimeScriptV1"}, {"ProjectId", Project}, {"ReleaseId", Value.ReleaseId},
				{"PublisherId", Value.PublisherId}, {"Target", GetRuntimeDistributionName(Value.Target)},
				{"ContentKeyId", Value.ContentKeyId}, {"KeyAccess", GetRuntimeDistributionName(Value.KeyAccess)},
				{"Compiler", Value.Compiler}, {"ClientCodeIdentity", Value.ClientCodeIdentity},
				{"ScriptId", SourceId}, {"Class", Class},
				{"Diagnostics", GetRuntimeDistributionName(Diagnostic)}, {"Representation", "Protected"},
			}.dump();
		}

		RuntimePackageSecret GetKey(const Metadata &Value, std::string_view Project,
			const RuntimePackageProvisionedSecurity &Security) {
			if (!IsRuntimePackageCryptoAvailable())
				Reject("UnsupportedRuntime", "This runtime distribution lacks cryptographic package support.");
			const auto ExpectedMode = Value.KeyAccess == RuntimeDistributionKeyAccess::HostProvisioned ?
				RuntimePackageProvisioningMode::Host : RuntimePackageProvisioningMode::Local;
			if (!Security.Keys || Security.Mode != ExpectedMode)
				Reject("KeyUnavailable", "The package content key provider is unavailable for this target.");
			RuntimePackageSecret Secret;
			RuntimePackageCryptoStatus Status;
			try {
				Status = Security.Keys->GetKey(
					{Project, Value.ReleaseId, Value.PublisherId, Value.ContentKeyId}, Secret);
			} catch (...) {
				Reject("KeyUnavailable", "The package content key provider failed.");
			}
			if (Status != RuntimePackageCryptoStatus::Ok || !Secret.IsAvailable())
				Reject("KeyUnavailable", "The package content key is unavailable.");
			return Secret;
		}

		Diagnostics ParseDiagnostics(std::string_view Text) {
			if (Text == "Minimal") return Diagnostics::Minimal;
			if (Text == "LineInfo") return Diagnostics::LineInfo;
			if (Text == "PrivateSymbols") return Diagnostics::PrivateSymbols;
			Reject("InvalidScriptCatalog", "Runtime script diagnostics are unsupported.");
		}

		Representation ParseRepresentation(std::string_view Text) {
			if (Text == "Source") return Representation::Source;
			if (Text == "Compiled") return Representation::Compiled;
			if (Text == "Protected") return Representation::Protected;
			Reject("InvalidScriptCatalog", "Runtime script representation is unsupported.");
		}
	}

	Json ParseStrictJson(std::string_view Text, std::size_t MaximumBytes) {
		if (Text.size() > MaximumBytes)
			Reject("InvalidPolicy", "Runtime distribution JSON exceeds its byte bound.");
		std::vector<std::set<std::string, std::less<>>> Keys;
		try {
			auto Parsed = Json::parse(Text, [&](int Depth, Json::parse_event_t Event, Json &Value) {
				if (Depth > static_cast<int>(MaximumProtocolJsonDepth))
					Reject("InvalidPolicy", "Runtime distribution JSON exceeds its depth bound.");
				switch (Event) {
				case Json::parse_event_t::object_start: Keys.emplace_back(); break;
				case Json::parse_event_t::object_end: Keys.pop_back(); break;
				case Json::parse_event_t::key:
					if (!Keys.back().insert(Value.get_ref<const std::string &>()).second)
						Reject("InvalidPolicy", "Runtime distribution JSON contains a duplicate field.");
					break;
				default: break;
				}
				return true;
			});
			JsonCodec::ValidateTree(Parsed);
			return Parsed;
		} catch (const RuntimeDistributionError &) {
			throw;
		} catch (const std::exception &) {
			Reject("InvalidPolicy", "Runtime distribution JSON is malformed.");
		}
	}

	Json EncodeMetadata(const Metadata &Value) {
		Json Result = {
			{"Version", 1u}, {"Target", GetRuntimeDistributionName(Value.Target)},
			{"PublisherId", Value.PublisherId}, {"SigningKeyId", Value.SigningKeyId},
			{"ReleaseId", Value.ReleaseId}, {"KeyAccess", GetRuntimeDistributionName(Value.KeyAccess)},
			{"ContentKeyId", Value.ContentKeyId}, {"Compiler", Value.Compiler}, {"ScriptCatalog", CodeCatalogPath},
			{"ClientCodeIdentity", Value.ClientCodeIdentity},
		};
		if (Value.SpatialContent) {
			Result["Version"] = 2u;
			Result["SpatialContent"] = "BlockGrid1";
			if (!Value.SharedGridAssets.empty()) Result["SharedGridAssets"] = Value.SharedGridAssets;
			if (!Value.WorkspaceTerrainManifest.empty())
				Result["WorkspaceTerrain"] = {{"Manifest",Value.WorkspaceTerrainManifest},{"SpacingShift",Value.WorkspaceTerrainSpacingShift}};
		}
		return Result;
	}

	Metadata DecodeMetadata(const Json &Value) {
		const bool Spatial = Value.is_object() && Value.value("Version", 0u) == 2;
		const bool Terrain = Spatial && Value.contains("WorkspaceTerrain");
		const bool Shared = Spatial && Value.contains("SharedGridAssets");
		if (!Value.is_object() || Value.size() != (Spatial ? (11u + unsigned(Terrain) + unsigned(Shared)) : 10u) ||
			(!Spatial && Value.value("Version", 0u) != 1) || (Spatial && String(Value,"SpatialContent") != "BlockGrid1") ||
			String(Value, "ScriptCatalog") != CodeCatalogPath)
			Reject("InvalidPolicy", "Runtime distribution manifest metadata is unsupported.");
		Metadata Result;
		Result.SpatialContent = Spatial;
		if (Shared) {
			const auto &Pins = Value.at("SharedGridAssets");
			if (!Pins.is_object() || Pins.empty() || Pins.size() > AssetLimits::MaximumCatalogRecords)
				Reject("InvalidPolicy","Shared grid asset pins exceed the package bound.");
			for (const auto &[Key, Digest] : Pins.items()) {
				const auto Id = AssetId::Parse(Key);
				const auto Hash = Digest.is_string() ? AssetContentId::Parse(Digest.get_ref<const std::string &>()) : std::nullopt;
				if (!Id || !Id->IsValid() || Id->ToString()!=Key || !Hash || !Hash->IsValid() || Hash->ToString()!=Digest.get<std::string>())
					Reject("InvalidPolicy","Shared grid asset pins are invalid.");
				Result.SharedGridAssets.emplace(Key, Hash->ToString());
			}
		}
		if (Terrain) {
			const auto &Pin = Value.at("WorkspaceTerrain");
			if (!Pin.is_object() || Pin.size()!=2 || !Pin.contains("SpacingShift") || !Pin["SpacingShift"].is_number_integer())
				Reject("InvalidPolicy","The spatial terrain binding is malformed.");
			Result.WorkspaceTerrainManifest = String(Pin,"Manifest");
			const auto Spacing=Pin["SpacingShift"].get<double>();
			if (!AssetContentId::Parse(Result.WorkspaceTerrainManifest) || Spacing < -1 || Spacing > 1)
				Reject("InvalidPolicy","The spatial terrain binding is outside the supported grid profile.");
			Result.WorkspaceTerrainSpacingShift=static_cast<int>(Spacing);
		}
		const auto Target = String(Value, "Target");
		if (Target == "DistributedPlayer") Result.Target = RuntimeDistributionTarget::DistributedPlayer;
		else if (Target == "DedicatedServer") Result.Target = RuntimeDistributionTarget::DedicatedServer;
		else if (Target != "OfflinePlayer") Reject("InvalidPolicy", "Runtime distribution target is unsupported.");
		if (Spatial && Result.Target == RuntimeDistributionTarget::OfflinePlayer)
			Reject("InvalidPolicy", "Spatial delivery requires a multiplayer target.");
		const auto KeyAccess = String(Value, "KeyAccess");
		if (KeyAccess == "LocalProvisioned") Result.KeyAccess = RuntimeDistributionKeyAccess::LocalProvisioned;
		else if (KeyAccess == "HostProvisioned") Result.KeyAccess = RuntimeDistributionKeyAccess::HostProvisioned;
		else if (KeyAccess != "None") Reject("InvalidPolicy", "Runtime distribution key access is unsupported.");
		Result.PublisherId = String(Value, "PublisherId");
		Result.SigningKeyId = String(Value, "SigningKeyId");
		Result.ReleaseId = String(Value, "ReleaseId");
		Result.ContentKeyId = String(Value, "ContentKeyId");
		Result.Compiler = String(Value, "Compiler");
		Result.ClientCodeIdentity = String(Value, "ClientCodeIdentity");
		if (!IsRuntimeDistributionIdentifier(Result.PublisherId) || !AssetContentId::Parse(Result.ReleaseId) ||
			(!Result.SigningKeyId.empty() && !IsRuntimeDistributionIdentifier(Result.SigningKeyId)) ||
			(!Result.ContentKeyId.empty() && !IsRuntimeDistributionIdentifier(Result.ContentKeyId)) ||
			(Result.KeyAccess == RuntimeDistributionKeyAccess::None) != Result.ContentKeyId.empty() ||
			(Result.Target == RuntimeDistributionTarget::OfflinePlayer ? !Result.ClientCodeIdentity.empty() :
				!AssetContentId::Parse(Result.ClientCodeIdentity)))
			Reject("InvalidPolicy", "Runtime distribution release identity or key metadata is invalid.");
		if (Result.Compiler != GetRuntimeScriptCompilerIdentity())
			Reject("UnsupportedCompiler", "The runtime distribution compiler identity is unsupported.");
		return Result;
	}

	std::string RemoveCompiledSource(std::string_view Snapshot, const RuntimeDistributionPlan &Plan, bool Distributed) {
		const auto Parsed = JsonCodec::Parse(Snapshot, MaximumSnapshotBytes, "runtime script projection");
		if (!Parsed) Reject("InvalidScriptCatalog", "Runtime script projection could not be read safely.");
		auto Value = std::move(*Parsed);
		std::map<client_package::CodeKey, std::string> Paths;
		std::set<std::string, std::less<>> Remaining;
		if (Distributed) {
			const auto Authorized = client_package::DecodeCode(Snapshot);
			std::size_t Matching = 0;
			for (const auto &Script : Plan.Scripts) {
				if (!Script.Eligible) continue;
				const auto Found = Authorized.find({Script.Path, Script.Class});
				if (Found == Authorized.end() || Found->second != Script.Source)
					Reject("AudienceViolation", "The selected script differs from the authorized client projection.");
				++Matching;
			}
			if (Matching != Authorized.size())
				Reject("AudienceViolation", "The selected scripts do not cover the authorized client projection.");
		}
		for (const auto &Script : Plan.Scripts) {
			if (!Script.Eligible || Script.Representation == Representation::Source) continue;
			Remaining.insert(Script.SourceId);
			if (Distributed && !Paths.emplace(client_package::CodeKey{Script.Path, Script.Class}, Script.SourceId).second)
				Reject("AudienceViolation", "Two distributed script entries have the same binding.");
		}
		if (Distributed) {
			for (auto &Entry : Value.at("Code")) {
				const auto Key = client_package::CodeKey{
					Entry.at("Path").get<std::vector<std::string>>(), String(Entry, "Class")};
				if (auto Match = Paths.find(Key); Match != Paths.end()) {
					Entry["Source"] = "";
					Remaining.erase(Match->second);
				}
			}
		} else {
			const auto Strip = [&](auto &Self, Json &Node) -> void {
				if (Node.contains("Properties")) {
					auto &Properties = Node["Properties"];
					if (Properties.contains("SourceId") && Properties["SourceId"].contains("String") &&
						Properties["SourceId"]["String"].is_string() &&
						Remaining.erase(Properties["SourceId"]["String"].get_ref<const std::string &>()) != 0)
						Properties.erase("Source");
				}
				if (Node.contains("Children"))
					for (auto &Child : Node["Children"]) Self(Self, Child);
			};
			Strip(Strip, Value);
		}
		if (!Remaining.empty())
			Reject("InvalidScriptCatalog", "A selected compiled script is absent from the runtime snapshot.");
		return Value.dump();
	}

	std::string BuildCatalog(const RuntimeDistributionPlan &Plan, const Metadata &Value, std::string_view Project,
		const RuntimePackageProvisionedSecurity &Security, const ArtifactWriter &Write) {
		std::vector<const RuntimeDistributionScript *> Entries;
		for (const auto &Script : Plan.Scripts)
			if (Script.Eligible) Entries.push_back(&Script);
		std::ranges::sort(Entries, {}, [](const auto *Entry) { return Entry->SourceId; });
		Json Scripts = Json::array();
		RuntimePackageSecret Secret;
		std::size_t DecodedBytes = 0;
		for (const auto *Entry : Entries) {
			const auto Kind = Entry->Representation;
			if (Kind != Representation::Source) {
				const auto CompileDiagnostics = Entry->Diagnostics == Diagnostics::Minimal ?
					RuntimeScriptDiagnostics::Stripped : RuntimeScriptDiagnostics::LineInfo;
				auto Compiled = CompileRuntimeScript(Entry->Source, CompileDiagnostics);
				if (!Compiled.Error.empty() || Compiled.Bytecode.empty())
					throw RuntimeDistributionError("CompilationFailed",
						"A selected script could not be compiled. Locate its SourceId in the package policy preview.",
						Entry->SourceId);
				if (Compiled.Bytecode.size() > MaximumDecodedBytes - DecodedBytes)
					Reject("InvalidScriptCatalog", "Runtime bytecode exceeds the package byte bound.");
				DecodedBytes += Compiled.Bytecode.size();
				if (Kind == Representation::Protected) {
					if (!Secret.IsAvailable()) Secret = GetKey(Value, Project, Security);
					std::string Sealed;
					if (SealRuntimePackageScript(Compiled.Bytecode,
						AssociatedData(Value, Project, Entry->SourceId, Entry->Class, Entry->Diagnostics), Secret, Sealed) !=
						RuntimePackageCryptoStatus::Ok)
						Reject("EncryptionFailed", "A selected runtime script could not be encrypted.");
					Write(ArtifactPath(Entry->SourceId, Kind), Sealed);
				} else Write(ArtifactPath(Entry->SourceId, Kind), Compiled.Bytecode);
			}
			Scripts.push_back({
				{"Id", Entry->SourceId}, {"Path", Entry->Path}, {"Class", Entry->Class},
				{"Representation", GetRuntimeDistributionName(Kind)},
				{"Diagnostics", GetRuntimeDistributionName(Entry->Diagnostics)},
			});
		}
		const auto Catalog = Json{{"Version", 1u}, {"Scripts", std::move(Scripts)}}.dump();
		if (Catalog.size() > MaximumCatalogBytes) Reject("InvalidScriptCatalog", "The runtime script catalog is too large.");
		return Catalog;
	}

	RuntimeDistributionCodeCatalog DecodeCatalog(std::string_view Catalog) {
		const auto Value = ParseStrictJson(Catalog, MaximumCatalogBytes);
		if (!Value.is_object() || Value.size() != 2 || Value.value("Version", 0u) != 1 ||
			!Value.contains("Scripts") || !Value["Scripts"].is_array() ||
			Value["Scripts"].size() > MaximumScriptCount)
			Reject("InvalidScriptCatalog", "Runtime script catalog is invalid.");
		RuntimeDistributionCodeCatalog Result;
		for (const auto &Record : Value["Scripts"]) {
			if (!Record.is_object() || Record.size() != 5 || !Record.contains("Path") ||
				!Record["Path"].is_array() || Record["Path"].empty() || Record["Path"].size() > MaximumProtocolJsonDepth)
				Reject("InvalidScriptCatalog", "Runtime script catalog entry is invalid.");
			RuntimeDistributionArtifact Entry;
			Entry.SourceId = String(Record, "Id");
			Entry.Class = String(Record, "Class");
			if (!ProjectId::Parse(Entry.SourceId) || (Entry.Class != "Script" && Entry.Class != "ModuleScript") ||
				(!Result.Scripts.empty() && Entry.SourceId <= Result.Scripts.rbegin()->first))
				Reject("InvalidScriptCatalog", "Runtime script identity or class is invalid.");
			for (const auto &Segment : Record["Path"]) {
				if (!Segment.is_string() || !client_package::ValidCodePathSegment(Segment.get_ref<const std::string &>()))
					Reject("InvalidScriptCatalog", "Runtime script binding path is invalid.");
				Entry.Path.push_back(Segment.get<std::string>());
			}
			Entry.Representation = ParseRepresentation(String(Record, "Representation"));
			Entry.Diagnostics = ParseDiagnostics(String(Record, "Diagnostics"));
			const auto Id = Entry.SourceId;
			Result.Scripts.emplace(Id, std::move(Entry));
		}
		return Result;
	}

	void AdmitCatalog(RuntimeDistributionCodeCatalog &Catalog, const Metadata &Value, std::string_view Project,
		const RuntimePackageProvisionedSecurity &Security, bool TrustedCompiler, const ArtifactReader &Read) {
		RuntimePackageSecret Secret;
		std::size_t DecodedBytes = 0;
		for (auto &[Id, Entry] : Catalog.Scripts) {
			if (Entry.Representation == Representation::Source) continue;
			if (!TrustedCompiler)
				Reject("UntrustedBytecode", "Compiled artifacts require an enrolled publisher or expected manifest digest.");
			const auto Maximum = Entry.Representation == Representation::Protected ?
				MaximumRuntimePackageScriptContainerBytes : MaximumRuntimeScriptBytecodeBytes;
			auto Bytes = Read(ArtifactPath(Id, Entry.Representation), Maximum);
			RuntimePackagePlaintext Plaintext;
			if (Entry.Representation == Representation::Protected) {
				if (!Secret.IsAvailable()) Secret = GetKey(Value, Project, Security);
				if (OpenRuntimePackageScript(Bytes, AssociatedData(Value, Project, Id, Entry.Class, Entry.Diagnostics),
					Secret, Plaintext) != RuntimePackageCryptoStatus::Ok)
					Reject("ScriptAuthenticationFailed", "A protected runtime script failed authentication.");
				if (Plaintext.View().size() > MaximumDecodedBytes - DecodedBytes)
					Reject("InvalidScriptCatalog", "Runtime bytecode exceeds the package byte bound.");
				Bytes.assign(Plaintext.View());
				Plaintext.Reset();
			}
			if (Bytes.size() > MaximumDecodedBytes - DecodedBytes)
				Reject("InvalidScriptCatalog", "Runtime bytecode exceeds the package byte bound.");
			DecodedBytes += Bytes.size();
			try {
				Entry.Code = AdmitTrustedRuntimeScriptCode(std::move(Bytes), Value.Compiler, Id);
			} catch (const std::invalid_argument &) {
				Reject("UnsupportedCompiler", "Runtime script bytecode has incompatible framing.");
			}
		}
	}

	std::size_t BindCatalog(const RuntimeDistributionCodeCatalog &Catalog,
		const std::shared_ptr<DataModel> &World, bool Distributed) {
		std::map<client_package::CodeKey, const RuntimeDistributionArtifact *> Paths;
		if (Distributed)
			for (const auto &[Id, Entry] : Catalog.Scripts)
				if (!Paths.emplace(client_package::CodeKey{Entry.Path, Entry.Class}, &Entry).second)
					Reject("InvalidScriptCatalog", "Distributed code bindings are ambiguous.");
		std::set<std::string, std::less<>> Bound;
		std::vector<std::string> Path;
		std::size_t Nodes = 0;
		const auto Bind = [&](auto &Self, const std::shared_ptr<Instance> &Object) -> void {
			if (++Nodes > MaximumPersistenceObjects || Path.size() == MaximumProtocolJsonDepth)
				Reject("InvalidScriptCatalog", "Runtime script binding exceeded its traversal bound.");
			Path.push_back(Object->GetName());
			if (auto Script = std::dynamic_pointer_cast<LuaSourceContainer>(Object)) {
				const RuntimeDistributionArtifact *Entry = nullptr;
				if (Distributed) {
					const auto Found = Paths.find({Path, std::string((Object->GetClassName)())});
					if (Found != Paths.end()) Entry = Found->second;
				} else {
					const auto Found = Catalog.Scripts.find(Script->GetSourceId());
					if (Found != Catalog.Scripts.end()) Entry = &Found->second;
				}
				if (Entry && !Bound.insert(Entry->SourceId).second)
					Reject("InvalidScriptCatalog", "Multiple objects claimed the same runtime script.");
				if (Entry && Entry->Code) {
					if ((Object->GetClassName)() != Entry->Class || !Script->GetSource().empty())
						Reject("AudienceViolation", "A compiled script binding conflicts with clear source or class.");
					Script->BindRuntimeScriptCode(Entry->Code);
				} else if (Entry) {
					Script->SetRuntimeSourceDiagnostics(Entry->Diagnostics == Diagnostics::Minimal ?
						RuntimeScriptDiagnostics::Stripped : RuntimeScriptDiagnostics::LineInfo);
				}
			}
			for (const auto &Child : Object->Children) Self(Self, Child);
			Path.pop_back();
		};
		for (const auto &Child : World->Children) Bind(Bind, Child);
		if (!Distributed && Bound.size() != Catalog.Scripts.size())
			Reject("InvalidScriptCatalog", "The runtime snapshot is missing a script binding.");
		return Bound.size();
	}
}
