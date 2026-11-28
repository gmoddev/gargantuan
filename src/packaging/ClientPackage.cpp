#include "ClientPackage.hpp"

#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/FileLink.hpp"
#include "gargantuan/classes/ModuleScript.hpp"
#include "gargantuan/classes/Script.hpp"
#include "gargantuan/classes/ScreenGui.hpp"
#include "gargantuan/gui/PlayerGuiScope.hpp"
#include "gargantuan/services/StarterGui.hpp"
#include "gargantuan/filesystem/SourceMount.hpp"
#include "gargantuan/reflection/InstanceClassRegistry.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"
#include "serialization/JsonCodec.hpp"

#include <memory>
#include <set>
#include <stdexcept>

namespace gargantuan::client_package {
	namespace {
		using Json = nlohmann::ordered_json;
		constexpr std::size_t MaximumPolicyBytes = 256 * 1024;
		static_assert(MaximumCodeSourceBytes == MaximumScriptSourceBytes);

		template <typename Visit>
		void VisitClientCode(const DataModel &World, bool Authored, bool IncludeTemplates, Visit &&Read) {
			std::vector<std::string> Path;
			std::size_t Nodes = 0;
			const auto Walk = [&](const auto &Self, const std::shared_ptr<Instance> &Object, bool Shared, bool Template) -> void {
				if (++Nodes > MaximumPersistenceObjects || Path.size() == MaximumProtocolJsonDepth)
					throw std::length_error("Distributed Player code hierarchy exceeds its bound");
				if ((Authored && !Object->GetArchivable()) || dynamic_cast<FileLink *>(Object.get())) return;
				const auto *Definition = InstanceClassRegistry::GetDefinition(Object.get());
				if (!Definition) throw std::runtime_error("Distributed Player code has no frozen class definition");
				if (Definition->ServicePolicy == SchemaServicePolicy::ServerOnly) return;
				Shared = Shared || Definition->ServicePolicy == SchemaServicePolicy::Replicated;
				const auto ScriptValue = std::dynamic_pointer_cast<Script>(Object);
				const auto Module = std::dynamic_pointer_cast<ModuleScript>(Object);
				const bool ClientScript = ScriptValue && ScriptValue->GetRunContext() == Enums::RunContext::Client;
				if (ScriptValue && !ClientScript && !Template) return;
				if (Module && !Shared && !Template) return;
				Path.push_back(Object->GetName());
				if (ClientScript && (!Shared || Template)) Read(Path, "Script", ScriptValue);
				if (Module) Read(Path, "ModuleScript", Module);
				for (const auto &Child : Object->Children) Self(Self, Child, Shared, Template);
				Path.pop_back();
			};
			for (const auto &Child : World.Children) {
				if (Authored && IncludeTemplates && dynamic_cast<StarterGui *>(Child.get())) {
					if (!Child->GetArchivable()) continue;
					Path.emplace_back("StarterGui");
					for (const auto &Gui : GetStarterGuiTemplates(Child, false))
						Walk(Walk, Gui.Root, false, true);
					Path.pop_back();
				} else Walk(Walk, Child, false, false);
			}
		}

	}

	std::vector<AssetId> ReadSharedAssets(BaseFilesystem &Filesystem) {
		SourceMount Mount(Filesystem);
		SourceMountBudget Budget;
		const auto Source = Mount.ReadFile(".gargantuan/package-audience.json", MaximumPolicyBytes, Budget);
		if (!Source) throw std::runtime_error("Distributed Player requires a readable .gargantuan/package-audience.json");
		const auto Policy = JsonCodec::Parse(*Source, MaximumPolicyBytes, "package audience policy");
		if (!Policy || !Policy->is_object() || Policy->size() != 2 || !Policy->contains("Version") ||
			!(*Policy)["Version"].is_number_unsigned() || (*Policy)["Version"] != 1 ||
			!Policy->contains("SharedAssets") || !(*Policy)["SharedAssets"].is_array() ||
			(*Policy)["SharedAssets"].size() > AssetLimits::MaximumCatalogRecords)
			throw std::runtime_error("Distributed Player asset audience policy is invalid");
		std::set<AssetId> Shared;
		for (const auto &Entry : (*Policy)["SharedAssets"]) {
			const auto Id = Entry.is_string() ? AssetId::Parse(Entry.get_ref<const std::string &>()) : std::nullopt;
			if (!Id || Id->ToString() != Entry.get_ref<const std::string &>() || !Shared.insert(*Id).second)
				throw std::runtime_error("SharedAssets must contain distinct canonical project AssetIds");
		}
		return {Shared.begin(), Shared.end()};
	}

	std::string CaptureCode(const DataModel &World, bool IncludePlayerGuiTemplates) {
		CodeMap Code;
		std::size_t RawBytes = 0;
		VisitClientCode(World, true, IncludePlayerGuiTemplates, [&](const auto &Path, std::string_view Class, const auto &ScriptValue) {
			for (const auto &Segment : Path) {
				if (!ValidCodePathSegment(Segment)) throw std::runtime_error("Distributed Player code path is invalid");
				RawBytes += Segment.size();
			}
			const auto Source = ScriptValue->GetSource();
			RawBytes += Source.size();
			if (RawBytes > MaximumCodeBytes || Source.size() > MaximumScriptSourceBytes)
				throw std::length_error("Distributed Player code exceeds its bound");
			if (!Code.emplace(CodeKey{Path, std::string(Class)}, Source).second)
				throw std::runtime_error("Distributed Player code path is ambiguous");
		});
		Json Entries = Json::array();
		for (const auto &[Key, Source] : Code)
			Entries.push_back({{"Path", Key.first}, {"Class", Key.second}, {"Source", Source}});
		auto Bytes = Json({{"Version", 1}, {"Code", std::move(Entries)}}).dump();
		if (Bytes.size() > MaximumCodeBytes) throw std::length_error("Distributed Player code exceeds its byte limit");
		return Bytes;
	}

	std::vector<RuntimeDistributionScript> CaptureEligibleScripts(const DataModel &World) {
		std::vector<RuntimeDistributionScript> Scripts;
		VisitClientCode(World, true, true, [&](const auto &Path, std::string_view Class, const auto &Script) {
			Scripts.push_back({.SourceId = Script->GetSourceId(), .Path = Path, .Class = std::string(Class),
				.Eligible = true});
		});
		return Scripts;
	}

	std::size_t HydrateCode(std::string_view Bytes, const DataModel &World) {
		const auto Code = DecodeCode(Bytes);
		std::map<CodeKey, std::shared_ptr<LuaSourceContainer>> Targets;
		VisitClientCode(World, false, false, [&](const auto &Path, std::string_view Class, const auto &ScriptValue) {
			CodeKey Key{Path, std::string(Class)};
			if (!Code.contains(Key)) return;
			if (!Targets.emplace(std::move(Key), ScriptValue).second)
				throw std::runtime_error("Distributed Player code target is ambiguous");
		});
		for (const auto &[Key, Target] : Targets)
			if (!Target->GetSource().empty() && Target->GetSource() != Code.at(Key))
				throw std::runtime_error("Replicated client code target already contains different source");
		for (const auto &[Key, Target] : Targets) Target->SetSource(Code.at(Key));
		return Targets.size();
	}
}
