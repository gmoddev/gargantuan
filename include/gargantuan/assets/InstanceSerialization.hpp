#pragma once

#include "gargantuan/classes/Instance.hpp"

#include <format>
#include <istream>
#include <optional>
#include <string_view>
#include <utility>
#include <unordered_map>
#include <vector>

namespace gargantuan::InstanceSerialization {
	enum class InstanceFormat : int { Json, Binary };
	struct DeserializationState {
		bool Ok = false;
		std::shared_ptr<Instance> Instance;
		std::vector<std::string> Errors;

		std::vector<std::string> CurrentPath{"(TOP)"};
		std::size_t ObjectsDecoded = 0;
		std::unordered_map<std::shared_ptr<gargantuan::Instance>, std::vector<std::string>> PendingTags;
		std::string FormatCurrentPath();

		template <class... Args> void PushError(std::format_string<Args...> fmt, Args &&...args) {
			auto formatted = std::format(fmt, std::forward<Args>(args)...);
			Errors.push_back(formatted);
		}

		template <class... Args> std::nullopt_t ReturnError(std::format_string<Args...> fmt, Args &&...args) {
			auto formatted = std::format(fmt, std::forward<Args>(args)...);
			Errors.push_back(formatted);
			return std::nullopt;
		}
	};

	std::string Serialize(InstanceFormat format, std::shared_ptr<Instance> &instance);
	std::string SerializeEmptyProject(InstanceFormat Format, std::string_view ProjectName);
	DeserializationState Deserialize(InstanceFormat format, std::istream &input);
}
