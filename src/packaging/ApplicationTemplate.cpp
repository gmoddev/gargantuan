// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "ApplicationTemplate.hpp"
#include "gargantuan/packaging/PackageBuilder.hpp"
#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/assets/AssetTypes.hpp"
#include "gargantuan/classes/Script.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/runtime/ApplicationPresentation.hpp"
#include "serialization/JsonCodec.hpp"

#include <algorithm>
#include <array>
#include <sstream>
#include <unordered_set>

namespace gargantuan::application_template {
	std::string WorldBaseline(const RuntimePackagePayload &Package) {
		const auto Digest = [](const std::string &Value) {
			return AssetContentId::Hash(std::span(reinterpret_cast<const std::uint8_t *>(Value.data()), Value.size())).ToString();
		};
		return Digest(Package.ProjectJson) + "/" + Digest(Package.Assets.CatalogJson) + "/" +
			Package.ContentManifestDigest.ToString() + "/" + Digest(Package.PreRunSource.value_or(""));
	}

	namespace {
		using Json = JsonCodec::Json;
		[[noreturn]] void Invalid(const char *Reason) { throw std::invalid_argument(std::string("[Package:Application] ") + Reason); }
		bool IsEntry(const Json &Node, std::string_view SourceId) {
			const auto &Props = Node.at("Properties");
			return Node.at("ClassName") == "Script" && Props.contains("SourceId") &&
				Props.at("SourceId").at("String").get_ref<const std::string &>() == SourceId;
		}

		Json Admit(std::string_view Text) {
			auto Parsed = JsonCodec::Parse(Text, MaximumBytes, "application template");
			if (!Parsed || !Parsed->is_object() || Parsed->size() != 3 || Parsed->at("Version") != 1)
				Invalid("Unsupported application template");
			const auto &Entry = Parsed->at("Entry").get_ref<const std::string &>();
			const auto &Folder = Parsed->at("Template");
			if (!ProjectId::Parse(Entry) || Folder.at("ClassName") != "Folder" || Folder.at("Version") != 5)
				Invalid("Invalid application entry binding");
			if (std::ranges::count_if(Folder.at("Children"), [&](const Json &Child) { return IsEntry(Child, Entry); }) != 1)
				Invalid("Entry must identify one direct client Script in the application Folder");
			std::vector<const Json *> Pending{&Folder};
			std::unordered_set<std::uint32_t> Ids;
			std::unordered_set<std::string> SourceIds;
			std::vector<std::uint32_t> Targets;
			std::size_t Viewports = 0;
			while (!Pending.empty()) {
				const auto &Node = *Pending.back();
				Pending.pop_back();
				const auto &Class = Node.at("ClassName").get_ref<const std::string &>();
				if (!ApplicationPresentation::SupportsClass(Class) ||
					Node.at("ClassSchemaId") != SchemaId::FromNativeName("Engine", Class).ToString())
					Invalid("Template contains a class outside the admitted presentation surface");
				const auto Id = JsonCodec::DecodeUnsigned32(Node.at("NodeId"));
				if (!Id || !*Id || !Ids.insert(*Id).second) Invalid("Invalid template node identity");
				const auto &Props = Node.at("Properties");
				if (Class == "ViewportFrame" && ++Viewports > 8) Invalid("Too many viewports");
				if (Class == "Script" || Class == "ModuleScript") {
					const auto &Source = Props.at("SourceId").at("String").get_ref<const std::string &>();
					if (!ProjectId::Parse(Source) || !SourceIds.insert(Source).second) Invalid("Invalid template source identity");
					if (Class == "Script" && Props.at("RunContext").at("EnumItem") != Json::array({"RunContext", "Client"}))
						Invalid("Template scripts require client context");
					if (Source == Entry && Props.at("Enabled").at("Bool") != true) Invalid("Entry is disabled");
				}
				for (const auto &[Name, Value] : Props.items()) {
					if (!Value.contains("InstanceReference") || Value.at("InstanceReference").is_null()) continue;
					const auto Target = JsonCodec::DecodeUnsigned32(Value.at("InstanceReference"));
					if (!Target || !*Target || Targets.size() >= 16384) Invalid("Invalid application reference");
					Targets.push_back(*Target);
				}
				const auto &Children = Node.at("Children");
				if (!Children.is_array() || Children.size() + Pending.size() > 1024 - Ids.size())
					Invalid("Template exceeds the application object limit");
				for (const auto &Child : Children) Pending.push_back(&Child);
			}
			for (const auto Target : Targets) if (!Ids.contains(Target)) Invalid("Application reference leaves the template");
			return std::move(*Parsed);
		}
	}

	const Instance *FindFolder(const DataModel &World) {
		const auto &Id = World.GetApplicationEntry();
		if (Id.empty()) return nullptr;
		const Instance *Found = nullptr;
		for (const auto &Child : World.Children) {
			if (Child->GetClassName() != "Folder") continue;
			for (const auto &Node : Child->Children) {
				const auto Entry = std::dynamic_pointer_cast<Script>(Node);
				if (!Entry || Entry->GetSourceId() != Id) continue;
				if (Found) Invalid("Application entry is ambiguous");
				Found = Child.get();
			}
		}
		if (!Found) Invalid("Application entry has no direct Folder");
		return Found;
	}

	Bindings GetBindings(std::string_view Template) {
		const auto Document = Admit(Template);
		Bindings Result;
		std::vector<std::string> Path;
		const auto Visit = [&](auto &Self, const Json &Node) -> void {
			Path.push_back(Node.at("Name").get<std::string>());
			const auto Class = Node.at("ClassName").get<std::string>();
			if (Class == "Script" || Class == "ModuleScript") {
				const auto &Properties = Node.at("Properties");
				const auto Id = Properties.at("SourceId").at("String").get<std::string>();
				const auto Source = Properties.contains("Source") ? Properties.at("Source").at("String").get<std::string>() : std::string{};
				Result.emplace(Id, SourceBinding{Path, Class, Source});
			}
			for (const auto &Child : Node.at("Children")) Self(Self, Child);
			Path.pop_back();
		};
		Visit(Visit, Document.at("Template"));
		return Result;
	}

	std::string Extract(std::string &Project, std::string_view SourceId) {
		auto Scene = JsonCodec::Parse(Project, 16 * 1024 * 1024, "application capture");
		if (!Scene || !ProjectId::Parse(SourceId)) Invalid("Invalid application project or entry identity");
		auto &Children = Scene->at("Children");
		const auto Folder = std::ranges::find_if(Children, [&](const Json &Child) {
			return Child.at("ClassName") == "Folder" && std::ranges::any_of(Child.at("Children"),
				[&](const Json &Node) { return IsEntry(Node, SourceId); });
		});
		if (Folder == Children.end()) Invalid("Designated entry is missing from its application Folder");
		auto Template = std::move(*Folder);
		Children.erase(Folder);
		Scene->at("Properties").erase("ApplicationEntry");
		Template["Version"] = 5;
		std::unordered_set<std::uint32_t> ApplicationIds;
		std::vector<const Json *> Pending{&Template};
		while (!Pending.empty()) {
			const auto &Node = *Pending.back();
			Pending.pop_back();
			ApplicationIds.insert(Node.at("NodeId").get<std::uint32_t>());
			for (const auto &Child : Node.at("Children")) Pending.push_back(&Child);
		}
		Pending.push_back(&*Scene);
		while (!Pending.empty()) {
			const auto &Node = *Pending.back();
			Pending.pop_back();
			for (const auto &[Name, Property] : Node.at("Properties").items()) {
				if (Property.contains("InstanceReference") && Property.at("InstanceReference").is_number_unsigned() &&
					ApplicationIds.contains(Property.at("InstanceReference").get<std::uint32_t>()))
					Invalid("Gameplay reference crosses the application boundary");
			}
			for (const auto &Child : Node.at("Children")) Pending.push_back(&Child);
		}
		auto Packed = JsonCodec::Encode(Json{{"Version", 1}, {"Entry", std::string(SourceId)}, {"Template", std::move(Template)}}, "application template");
		if (!Packed) Invalid("Application template encoding failed");
		Validate(*Packed);
		auto Gameplay = JsonCodec::Encode(*Scene, "gameplay projection");
		if (!Gameplay || !InstanceSerialization::InspectTerrainManifest(*Gameplay)) Invalid("Gameplay reference crosses the application boundary");
		Project = std::move(*Gameplay);
		return std::move(*Packed);
	}

	void Validate(std::string_view Template) { (void)Admit(Template); }

	AssetRuntimeSnapshot SelectAssets(std::string_view Template, const AssetRuntimeSnapshot &PackageAssets) {
		const auto Document = Admit(Template);
		auto Catalog = JsonCodec::Parse(PackageAssets.CatalogJson, 1024 * 1024, "application asset catalog");
		if (!Catalog) Invalid("Invalid runtime asset catalog");
		auto &Records = Catalog->at("Assets");
		std::unordered_map<std::string, const Json *> ById;
		for (const auto &Record : Records) ById.emplace(Record.at("AssetId").get<std::string>(), &Record);
		std::vector<const Json *> Nodes{&Document.at("Template")};
		std::vector<std::string> Pending;
		while (!Nodes.empty()) {
			const auto &Node = *Nodes.back();
			Nodes.pop_back();
			for (const auto *Field : {"Properties", "Attributes"}) {
				for (const auto &[Name, Value] : Node.at(Field).items()) {
					if (Name == "Source" || !Value.contains("String")) continue;
					const auto &Reference = Value.at("String").get_ref<const std::string &>();
					if (Reference.starts_with("asset://")) Pending.push_back(Reference.substr(8));
				}
			}
			for (const auto &Child : Node.at("Children")) Nodes.push_back(&Child);
		}
		std::unordered_set<std::string> Selected, Files;
		while (!Pending.empty()) {
			auto Id = std::move(Pending.back());
			Pending.pop_back();
			if (!Selected.insert(Id).second) continue;
			const auto Found = ById.find(Id);
			if (Found == ById.end()) Invalid("An application asset is missing from the package");
			const auto &Record = *Found->second;
			Files.insert("assets/artifacts/" + Record.at("ContentId").get<std::string>() + ".gasset");
			for (const auto &Dependency : Record.at("Dependencies")) Pending.push_back(Dependency.get<std::string>());
		}
		Json Kept = Json::array();
		for (const auto &Record : Records)
			if (Selected.contains(Record.at("AssetId").get_ref<const std::string &>())) Kept.push_back(Record);
		Records = std::move(Kept);
		AssetRuntimeSnapshot Result{Catalog->dump(), {}};
		for (const auto &File : PackageAssets.Artifacts)
			if (Files.contains(File.RelativePath)) Result.Artifacts.push_back(File);
		if (Result.Artifacts.size() != Files.size()) Invalid("An application artifact is missing from the package");
		return Result;
	}

	Graph Load(std::string_view Template) {
		const auto Document = Admit(Template);
		std::istringstream Input(Document.at("Template").dump());
		auto Decoded = InstanceSerialization::DeserializeDetached(InstanceSerialization::InstanceFormat::Json, Input);
		if (!Decoded.Ok || !Decoded.Instance) Invalid("Unable to materialize the admitted template");
		Graph Result{ApplicationPresentation::CreateGraph(), {}};
		Decoded.Instance->SetParent(Result.World);
		for (const auto &Child : Decoded.Instance->Children) {
			auto Candidate = std::dynamic_pointer_cast<Script>(Child);
			if (Candidate && Candidate->GetSourceId() == Document.at("Entry")) Result.Entry = std::move(Candidate);
		}
		if (!Result.Entry) Invalid("Materialized application entry is missing");
		return Result;
	}
}
