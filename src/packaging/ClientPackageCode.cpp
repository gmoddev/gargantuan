#include "ClientPackage.hpp"
#include "gargantuan/filesystem/ProjectIdentity.hpp"

#include "gargantuan/runtime/ProtocolInput.hpp"
#include "serialization/JsonCodec.hpp"

#include <stdexcept>

namespace gargantuan::client_package {
	bool ValidCodePathSegment(std::string_view Value) {
		constexpr std::size_t MaximumPathSegmentBytes = 256;
		return !Value.empty() && Value.size() <= MaximumPathSegmentBytes &&
			Value.find('\0') == std::string_view::npos && IsValidProtocolUtf8(Value);
	}

	CodeMap DecodeCode(std::string_view Bytes, std::map<std::string, CodeKey> *GuiBindings) {
		const auto Document = JsonCodec::Parse(Bytes, MaximumCodeBytes, "distributed Player code");
		if (!Document || !Document->is_object() || Document->size() != 2 || !Document->contains("Version") ||
			!(*Document)["Version"].is_number_unsigned() || ((*Document)["Version"] != 1 && (*Document)["Version"] != 2) ||
			!Document->contains("Code") || !(*Document)["Code"].is_array() ||
			(*Document)["Code"].size() > MaximumPersistenceObjects)
			throw std::runtime_error("Distributed Player code document is invalid");
		CodeMap Result;
		std::map<std::string, CodeKey> Gui;
		for (const auto &Entry : (*Document)["Code"]) {
			const bool Bound = (*Document)["Version"] == 2 && Entry.contains("GuiSourceId");
			if (!Entry.is_object() || Entry.size() != (Bound ? 4 : 3) || !Entry.contains("Path") || !Entry["Path"].is_array() ||
				Entry["Path"].empty() || Entry["Path"].size() > MaximumProtocolJsonDepth ||
				!Entry.contains("Class") || (Entry["Class"] != "Script" && Entry["Class"] != "ModuleScript") ||
				!Entry.contains("Source") || !Entry["Source"].is_string() ||
				Entry["Source"].get_ref<const std::string &>().size() > MaximumCodeSourceBytes)
				throw std::runtime_error("Distributed Player code entry is invalid");
			CodeKey Key{{}, Entry["Class"].get<std::string>()};
			for (const auto &Segment : Entry["Path"]) {
				if (!Segment.is_string() || !ValidCodePathSegment(Segment.get_ref<const std::string &>()))
					throw std::runtime_error("Distributed Player code path is invalid");
				Key.first.push_back(Segment.get<std::string>());
			}
			if (!Result.empty() && Key <= Result.rbegin()->first)
				throw std::runtime_error("Distributed Player code paths are duplicated or unsorted");
			if (Bound) {
				const auto &Id = Entry["GuiSourceId"];
				if (Key.first.size() < 3 || Key.first.front() != "StarterGui" || !Id.is_string() ||
					!ProjectId::Parse(Id.get_ref<const std::string &>()) ||
					!Gui.emplace(Id.get<std::string>(), Key).second)
					throw std::runtime_error("Distributed Player GUI binding is invalid");
			}
			Result.emplace(std::move(Key), Entry["Source"].get<std::string>());
		}
		if (GuiBindings) *GuiBindings = std::move(Gui);
		return Result;
	}

	void ValidateCode(std::string_view Bytes) {
		(void)DecodeCode(Bytes);
	}
}
