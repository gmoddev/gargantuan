// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#include "ApplicationTemplate.hpp"
#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/classes/Script.hpp"
#include "serialization/JsonCodec.hpp"

#include <algorithm>
#include <array>
#include <sstream>
#include <unordered_set>

namespace gargantuan::application_template {
	namespace {
		using Json = JsonCodec::Json;
		[[noreturn]] void Invalid(const char *Reason) { throw std::invalid_argument(std::string("[Package:Application] ") + Reason); }
		bool IsEntry(const Json &Node, std::string_view SourceId) {
			const auto &Props = Node.at("Properties");
			return Node.at("ClassName") == "Script" && Props.contains("SourceId") &&
				Props.at("SourceId").at("String") == SourceId;
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
			constexpr std::array Classes{"Folder", "Script", "ModuleScript", "ScreenGui", "Frame", "TextButton", "TextLabel",
				"TextBox", "ScrollingFrame", "UIListLayout", "ImageLabel", "ImageButton", "ViewportFrame", "Camera"};
			while (!Pending.empty()) {
				const auto &Node = *Pending.back();
				Pending.pop_back();
				const auto &Class = Node.at("ClassName").get_ref<const std::string &>();
				if (std::ranges::find(Classes, Class) == Classes.end() ||
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
		auto Packed = JsonCodec::Encode(Json{{"Version", 1}, {"Entry", SourceId}, {"Template", std::move(Template)}}, "application template");
		if (!Packed) Invalid("Application template encoding failed");
		Validate(*Packed);
		auto Gameplay = JsonCodec::Encode(*Scene, "gameplay projection");
		if (!Gameplay || !InstanceSerialization::InspectTerrainManifest(*Gameplay)) Invalid("Gameplay reference crosses the application boundary");
		Project = std::move(*Gameplay);
		return std::move(*Packed);
	}

	void Validate(std::string_view Template) { (void)Admit(Template); }

	Graph Load(std::string_view Template) {
		const auto Document = Admit(Template);
		std::istringstream Input(Document.at("Template").dump());
		auto Decoded = InstanceSerialization::DeserializeDetached(InstanceSerialization::InstanceFormat::Json, Input);
		if (!Decoded.Ok || !Decoded.Instance) Invalid("Unable to materialize the admitted template");
		Graph Result{std::make_shared<DataModel>(), {}};
		Decoded.Instance->SetParent(Result.World);
		for (const auto &Child : Decoded.Instance->Children) {
			auto Candidate = std::dynamic_pointer_cast<Script>(Child);
			if (Candidate && Candidate->GetSourceId() == Document.at("Entry")) Result.Entry = std::move(Candidate);
		}
		if (!Result.Entry) Invalid("Materialized application entry is missing");
		return Result;
	}
}
