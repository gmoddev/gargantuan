#include "gargantuan/packaging/RuntimeDistribution.hpp"

#include "RuntimeDistributionCode.hpp"
#include "gargantuan/assets/AssetTypes.hpp"
#include "gargantuan/filesystem/ProjectIdentity.hpp"
#include "gargantuan/filesystem/SourceMount.hpp"
#include "serialization/JsonCodec.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <set>

namespace gargantuan {
	namespace {
		using Json = nlohmann::ordered_json;
		constexpr std::size_t MaximumPolicyBytes = 256 * 1024;
		constexpr std::size_t MaximumPolicyEntries = 4096;

		constexpr std::array Targets{
			std::pair{"OfflinePlayer", RuntimeDistributionTarget::OfflinePlayer},
			std::pair{"DistributedPlayer", RuntimeDistributionTarget::DistributedPlayer},
			std::pair{"DedicatedServer", RuntimeDistributionTarget::DedicatedServer},
		};
		constexpr std::array Presets{
			std::pair{"Open", RuntimeDistributionPreset::Open},
			std::pair{"Compiled", RuntimeDistributionPreset::Compiled},
			std::pair{"Hybrid", RuntimeDistributionPreset::Hybrid},
			std::pair{"Protected", RuntimeDistributionPreset::Protected},
		};
		constexpr std::array Representations{
			std::pair{"Source", RuntimeDistributionRepresentation::Source},
			std::pair{"Compiled", RuntimeDistributionRepresentation::Compiled},
			std::pair{"Protected", RuntimeDistributionRepresentation::Protected},
			std::pair{"Excluded", RuntimeDistributionRepresentation::Excluded},
		};
		constexpr std::array Diagnostics{
			std::pair{"Minimal", RuntimeDistributionDiagnostics::Minimal},
			std::pair{"LineInfo", RuntimeDistributionDiagnostics::LineInfo},
			std::pair{"PrivateSymbols", RuntimeDistributionDiagnostics::PrivateSymbols},
		};
		constexpr std::array KeyAccess{
			std::pair{"None", RuntimeDistributionKeyAccess::None},
			std::pair{"LocalProvisioned", RuntimeDistributionKeyAccess::LocalProvisioned},
			std::pair{"HostProvisioned", RuntimeDistributionKeyAccess::HostProvisioned},
		};

		[[noreturn]] void Invalid(const char *Message) {
			throw RuntimeDistributionError("InvalidPolicy", Message);
		}

		template <typename Choices>
		auto ReadChoice(const Choices &Values, const Json &Value, const char *Message) {
			if (Value.is_string())
				for (const auto &[Name, Choice] : Values)
					if (Value.get_ref<const std::string &>() == Name) return Choice;
			Invalid(Message);
		}

		template <typename Choices, typename T>
		std::string_view GetName(const Choices &Values, T Value) {
			for (const auto &[Name, Choice] : Values)
				if (Choice == Value) return Name;
			return {};
		}

		void OnlyFields(const Json &Value, std::initializer_list<std::string_view> Fields) {
			if (!Value.is_object() || std::ranges::any_of(Value.items(), [&](const auto &Item) {
				return std::ranges::find(Fields, Item.key()) == Fields.end();
			})) Invalid("Runtime distribution policy contains unknown fields.");
		}

		void CheckVersion(const Json &Value) {
			if (!Value.contains("Version") || !Value["Version"].is_number_unsigned() || Value["Version"] != 1)
				Invalid("Runtime distribution policy version is unsupported.");
		}

		std::string ReadId(const Json &Object, const char *Field) {
			if (!Object.contains(Field)) return {};
			const auto &Value = Object[Field];
			if (!Value.is_string() || !IsRuntimeDistributionIdentifier(Value.get_ref<const std::string &>()))
				Invalid("Runtime distribution selector is invalid.");
			return Value.get<std::string>();
		}

		struct Fields final {
			std::optional<RuntimeDistributionRepresentation> Representation;
			std::optional<RuntimeDistributionDiagnostics> Diagnostic;
			std::optional<bool> Encrypt;
		};

		Fields ParseFields(const Json &Value, bool Group = false) {
			if (Group) OnlyFields(Value, {"Representation", "Diagnostics", "Encryption", "Members"});
			else OnlyFields(Value, {"Representation", "Diagnostics", "Encryption"});
			Fields Result;
			if (Value.contains("Representation")) {
				Result.Representation = ReadChoice(Representations, Value["Representation"], "Script representation is invalid.");
				if (Result.Representation == RuntimeDistributionRepresentation::Excluded)
					Invalid("Audience eligibility determines which scripts can ship.");
				if (Result.Representation == RuntimeDistributionRepresentation::Protected) {
					Result.Representation = RuntimeDistributionRepresentation::Compiled;
					Result.Encrypt = true;
				}
			}
			if (Value.contains("Diagnostics"))
				Result.Diagnostic = ReadChoice(Diagnostics, Value["Diagnostics"], "Script diagnostics are invalid.");
			if (Value.contains("Encryption")) {
				const auto &Encryption = Value["Encryption"];
				if (Encryption != "None" && Encryption != "Authenticated") Invalid("Script encryption is invalid.");
				if (Result.Encrypt && Encryption == "None") Invalid("Protected conflicts with Encryption None.");
				Result.Encrypt = Encryption == "Authenticated";
			}
			return Result;
		}

		Fields PresetFields(const RuntimeDistributionRequest &Request) {
			auto Representation = RuntimeDistributionRepresentation::Compiled;
			if (Request.Preset == RuntimeDistributionPreset::Open) Representation = RuntimeDistributionRepresentation::Source;
			return {Representation, RuntimeDistributionDiagnostics::Minimal,
				Request.Preset == RuntimeDistributionPreset::Protected};
		}

		template <typename T>
		void MergeGroup(std::optional<T> &Prior, const std::optional<T> &Value, bool &Conflict) {
			if (!Value) return;
			if (Prior && Prior != Value) Conflict = true;
			else Prior = Value;
		}

		struct GroupFields final {
			Fields Values;
			bool RepresentationConflict = false;
			bool DiagnosticConflict = false;
			bool EncryptionConflict = false;
		};
	}

	RuntimeDistributionError::RuntimeDistributionError(std::string_view Code, const char *Message, std::string_view Item)
		: std::runtime_error(Message), Code(Code), Item(Item) {}

	bool IsRuntimeDistributionIdentifier(std::string_view Value) {
		return !Value.empty() && Value.size() <= 96 && std::ranges::all_of(Value, [](char Character) {
			return (Character >= 'a' && Character <= 'z') || (Character >= 'A' && Character <= 'Z') ||
				(Character >= '0' && Character <= '9') || Character == '.' || Character == '_' || Character == '-';
		});
	}

	std::string_view GetRuntimeDistributionName(RuntimeDistributionTarget Value) { return GetName(Targets, Value); }
	std::string_view GetRuntimeDistributionName(RuntimeDistributionRepresentation Value) {
		return GetName(Representations, Value);
	}
	std::string_view GetRuntimeDistributionName(RuntimeDistributionDiagnostics Value) { return GetName(Diagnostics, Value); }
	std::string_view GetRuntimeDistributionName(RuntimeDistributionKeyAccess Value) { return GetName(KeyAccess, Value); }

	std::string GetRuntimeDistributionGuiDigest(
		std::string_view ClientCodeIdentity, std::string_view SourceId, int SourceVersion
	) {
		const auto Tuple = Json::array({"GargantuanRuntimeGuiV1", ClientCodeIdentity, SourceId, SourceVersion}).dump();
		return AssetContentId::Hash(std::span(
			reinterpret_cast<const std::uint8_t *>(Tuple.data()), Tuple.size())).ToString();
	}

	RuntimeDistributionRequest ParseRuntimeDistributionRequest(std::string_view Bytes) {
		const auto Document = runtime_distribution::ParseStrictJson(Bytes, 8192);
		const auto *Value = &Document;
		OnlyFields(*Value, {"Version", "Target", "Preset", "Diagnostics", "PublisherTrust", "SigningKeyId", "KeyAccess",
			"ContentKeyId", "PolicyTarget", "ModLoading"});
		CheckVersion(*Value);
		if (!Value->contains("Target") || !Value->contains("Preset")) Invalid("Runtime distribution target and preset are required.");
		RuntimeDistributionRequest Result;
		Result.Target = ReadChoice(Targets, (*Value)["Target"], "Runtime distribution target is unsupported.");
		Result.Preset = ReadChoice(Presets, (*Value)["Preset"], "Runtime distribution preset is unsupported.");
		if (Value->contains("Diagnostics"))
			Result.Diagnostics = ReadChoice(Diagnostics, (*Value)["Diagnostics"], "Script diagnostics are invalid.");
		if (Value->contains("KeyAccess"))
			Result.KeyAccess = ReadChoice(KeyAccess, (*Value)["KeyAccess"], "Key access is unsupported.");
		Result.SigningKeyId = ReadId(*Value, "SigningKeyId");
		Result.ContentKeyId = ReadId(*Value, "ContentKeyId");
		Result.PolicyTarget = ReadId(*Value, "PolicyTarget");
		if (Value->contains("PublisherTrust")) {
			const auto &Trust = (*Value)["PublisherTrust"];
			if (Trust != "Required" && Trust != "DigestOnly") Invalid("Publisher trust policy is unsupported.");
			Result.RequirePublisher = Trust == "Required";
		}
		if (Result.RequirePublisher != !Result.SigningKeyId.empty()) Invalid("Publisher signing requires a signing key ID.");
		if ((Result.KeyAccess == RuntimeDistributionKeyAccess::None) != Result.ContentKeyId.empty())
			Invalid("Selected key access requires a content key ID.");
		if (Value->contains("ModLoading") && (*Value)["ModLoading"] != "Disabled")
			Invalid("External mod loading is reserved for a separately qualified extension policy.");
		if (Result.Target == RuntimeDistributionTarget::DedicatedServer &&
			Result.KeyAccess == RuntimeDistributionKeyAccess::LocalProvisioned)
			Invalid("Dedicated Server requires host-provisioned content keys.");
		if (Result.Target != RuntimeDistributionTarget::DedicatedServer &&
			Result.KeyAccess == RuntimeDistributionKeyAccess::HostProvisioned)
			Invalid("Player packages require locally provisioned content keys.");
		return Result;
	}

	void ResolveRuntimeDistributionPolicy(BaseFilesystem &Filesystem, RuntimeDistributionPlan &Plan) {
		Json Policy = Json::object();
		SourceMount Mount(Filesystem);
		SourceMountBudget Budget;
		const auto Read = Mount.ReadFile(".gargantuan/runtime-distribution.json", MaximumPolicyBytes, Budget);
		if (Read) {
			Policy = runtime_distribution::ParseStrictJson(*Read, MaximumPolicyBytes);
			OnlyFields(Policy, {"Version", "Defaults", "Targets", "Groups", "Overrides", "SessionIdentity"});
			CheckVersion(Policy);
		} else if (Filesystem.Exists(".gargantuan/runtime-distribution.json")) {
			Invalid("Runtime distribution project policy could not be read safely.");
		}
		const auto CanonicalPolicy = Policy.dump();
		Plan.PolicySha256 = AssetContentId::Hash(std::span(
			reinterpret_cast<const std::uint8_t *>(CanonicalPolicy.data()), CanonicalPolicy.size())).ToString();
		if (Plan.Request.Target != RuntimeDistributionTarget::OfflinePlayer) {
			Plan.SessionIdentity = ReadId(Policy, "SessionIdentity");
			if (!ProjectId::Parse(Plan.SessionIdentity))
				Invalid("Network distribution requires a shared project SessionIdentity.");
		}

		Fields Base = PresetFields(Plan.Request);
		const auto Apply = [](Fields &Output, const Fields &Input) {
			if (Input.Representation) Output.Representation = Input.Representation;
			if (Input.Diagnostic) Output.Diagnostic = Input.Diagnostic;
			if (Input.Encrypt) Output.Encrypt = Input.Encrypt;
		};
		std::string BaseOrigin = "Preset";
		if (Policy.contains("Defaults")) {
			Apply(Base, ParseFields(Policy["Defaults"]));
			BaseOrigin = "Project";
		}
		const auto Target = Plan.Request.PolicyTarget.empty() ?
			std::string(GetRuntimeDistributionName(Plan.Request.Target)) : Plan.Request.PolicyTarget;
		if (Policy.contains("Targets")) {
			const auto &TargetValues = Policy["Targets"];
			if (!TargetValues.is_object() || TargetValues.size() > 32) Invalid("Package targets exceed their bound.");
			for (const auto &[Name, Value] : TargetValues.items()) {
				if (!IsRuntimeDistributionIdentifier(Name)) Invalid("Package target name is invalid.");
				const auto Options = ParseFields(Value);
				if (Name == Target) {
					Apply(Base, Options);
					BaseOrigin = "Target";
				}
			}
		}
		if (!Plan.Request.PolicyTarget.empty() && (!Policy.contains("Targets") || !Policy["Targets"].contains(Target)))
			Invalid("Selected package target is missing.");
		if (Plan.Request.Diagnostics) Base.Diagnostic = Plan.Request.Diagnostics;

		std::set<std::string, std::less<>> Known;
		for (const auto &Script : Plan.Scripts)
			if (!ProjectId::Parse(Script.SourceId) || !Known.insert(Script.SourceId).second)
				Invalid("Script identities are missing or duplicated. Save the project to persist unique identities.");
		std::map<std::string, GroupFields, std::less<>> Groups;
		if (Policy.contains("Groups")) {
			const auto &GroupValues = Policy["Groups"];
			if (!GroupValues.is_object() || GroupValues.size() > 128) Invalid("Script groups exceed their bound.");
			std::size_t Members = 0;
			for (const auto &[Name, Value] : GroupValues.items()) {
				if (!IsRuntimeDistributionIdentifier(Name)) Invalid("Script group name is invalid.");
				const auto Options = ParseFields(Value, true);
				if (!Value.contains("Members") || !Value["Members"].is_array()) Invalid("Script group membership is required.");
				std::set<std::string, std::less<>> Unique;
				for (const auto &Member : Value["Members"]) {
					if (++Members > MaximumPolicyEntries || !Member.is_string()) Invalid("Script group membership exceeds its bound.");
					const auto &Id = Member.get_ref<const std::string &>();
					if (!Known.contains(Id) || !Unique.insert(Id).second) Invalid("Script group contains an invalid or duplicate member.");
					auto &Group = Groups[Id];
					MergeGroup(Group.Values.Representation, Options.Representation, Group.RepresentationConflict);
					MergeGroup(Group.Values.Diagnostic, Options.Diagnostic, Group.DiagnosticConflict);
					MergeGroup(Group.Values.Encrypt, Options.Encrypt, Group.EncryptionConflict);
				}
			}
		}
		std::map<std::string, Fields, std::less<>> Overrides;
		if (Policy.contains("Overrides")) {
			const auto &Values = Policy["Overrides"];
			if (!Values.is_object() || Values.size() > MaximumPolicyEntries) Invalid("Script overrides exceed their bound.");
			for (const auto &[Id, Value] : Values.items()) {
				if (!Known.contains(Id)) Invalid("Script override references an unknown identity.");
				Overrides.emplace(Id, ParseFields(Value));
			}
		}
		for (auto &Script : Plan.Scripts) {
			auto Resolved = Base;
			Script.Origin = BaseOrigin;
			const auto Override = Overrides.find(Script.SourceId);
			if (const auto Group = Groups.find(Script.SourceId); Group != Groups.end()) {
				if ((Group->second.RepresentationConflict &&
					(Override == Overrides.end() || !Override->second.Representation)) ||
					(Group->second.DiagnosticConflict && (Override == Overrides.end() || !Override->second.Diagnostic)) ||
					(Group->second.EncryptionConflict && (Override == Overrides.end() || !Override->second.Encrypt)))
					Invalid("Script groups disagree on policy. Resolve the conflicting fields in a script override.");
				Apply(Resolved, Group->second.Values);
				Script.Origin = "Group";
			}
			if (Override != Overrides.end()) {
				Apply(Resolved, Override->second);
				Script.Origin = "Script";
			}
			Script.Diagnostics = *Resolved.Diagnostic;
			if (Script.Eligible) {
				if (*Resolved.Encrypt && *Resolved.Representation == RuntimeDistributionRepresentation::Source)
					Invalid("Readable source and authenticated encryption conflict for an eligible script.");
				Script.Representation = *Resolved.Encrypt ? RuntimeDistributionRepresentation::Protected : *Resolved.Representation;
			} else Script.Origin = "Audience";
			if (Script.Representation == RuntimeDistributionRepresentation::Protected &&
				Plan.Request.KeyAccess == RuntimeDistributionKeyAccess::None)
				Invalid("An eligible protected script requires provisioned key access.");
		}
	}
}
