#include "gargantuan/runtime/WireJournal.hpp"

#include "gargantuan/runtime/WireCodec.hpp"
#include "gargantuan/runtime/AttributeValidation.hpp"
#include "gargantuan/runtime/TagIndex.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"

#include <stdexcept>
#include <type_traits>

namespace gargantuan {
	namespace {
		const char *OperationName(WireJournalOperation operation) {
			switch (operation) {
				case WireJournalOperation::Create: return "Create";
				case WireJournalOperation::PropertyUpdate: return "PropertyUpdate";
				case WireJournalOperation::AttributeUpdate: return "AttributeUpdate";
				case WireJournalOperation::ExtensionPropertyUpdate: return "ExtensionPropertyUpdate";
				case WireJournalOperation::TagAdded: return "TagAdded";
				case WireJournalOperation::TagRemoved: return "TagRemoved";
				case WireJournalOperation::Reparent: return "Reparent";
				case WireJournalOperation::Destroy: return "Destroy";
			}
			throw std::runtime_error("Unknown wire journal operation");
		}

		std::optional<WireJournalOperation> ParseOperation(std::string_view operation) {
			if (operation == "Create") return WireJournalOperation::Create;
			if (operation == "PropertyUpdate") return WireJournalOperation::PropertyUpdate;
			if (operation == "AttributeUpdate") return WireJournalOperation::AttributeUpdate;
			if (operation == "ExtensionPropertyUpdate") return WireJournalOperation::ExtensionPropertyUpdate;
			if (operation == "TagAdded") return WireJournalOperation::TagAdded;
			if (operation == "TagRemoved") return WireJournalOperation::TagRemoved;
			if (operation == "Reparent") return WireJournalOperation::Reparent;
			if (operation == "Destroy") return WireJournalOperation::Destroy;
			return std::nullopt;
		}

		bool HasOnlyFields(const WireJson &encoded, std::initializer_list<std::string_view> allowed) {
			for (const auto &[name, value] : encoded.items()) {
				(void)value;
				bool found = false;
				for (auto candidate : allowed) found = found || name == candidate;
				if (!found) return false;
			}
			return true;
		}
	}

	WireJournalRecord EncodeChangeRecord(const ChangeRecord &record) {
		WireJournalRecord encoded{
			.Sequence = record.Sequence,
			.Scope = WireObjectId::FromObjectId(record.Scope),
			.Object = WireObjectId::FromObjectId(record.Object),
		};
		std::visit(
			[&](const auto &payload) {
				using Payload = std::decay_t<decltype(payload)>;
				if constexpr (std::is_same_v<Payload, ObjectCreatedChange>) {
					encoded.Operation = WireJournalOperation::Create;
					encoded.ClassName = payload.ClassName;
				} else if constexpr (std::is_same_v<Payload, PropertyUpdatedChange>) {
					if (!payload.Replicated)
						throw std::runtime_error("Non-replicated property change cannot cross the wire boundary");
					encoded.Operation = WireJournalOperation::PropertyUpdate;
					encoded.PropertyName = payload.PropertyName;
					encoded.Value = payload.Value;
				} else if constexpr (std::is_same_v<Payload, AttributeUpdatedChange>) {
					encoded.Operation = WireJournalOperation::AttributeUpdate;
					encoded.AttributeName = payload.AttributeName;
					encoded.Value = payload.Value.value_or(WireValue(std::monostate{}));
				} else if constexpr (std::is_same_v<Payload, ExtensionPropertyUpdatedChange>) {
					encoded.Operation = WireJournalOperation::ExtensionPropertyUpdate;
					encoded.ExtensionSchemaId = payload.ExtensionSchemaId;
					encoded.DefinitionVersion = payload.DefinitionVersion;
					encoded.ExtensionPropertyName = payload.PropertyName;
					encoded.Value = payload.Value;
				} else if constexpr (std::is_same_v<Payload, TagAddedChange>) {
					encoded.Operation = WireJournalOperation::TagAdded;
					encoded.TagName = payload.TagName;
				} else if constexpr (std::is_same_v<Payload, TagRemovedChange>) {
					encoded.Operation = WireJournalOperation::TagRemoved;
					encoded.TagName = payload.TagName;
				} else if constexpr (std::is_same_v<Payload, ObjectReparentedChange>) {
					encoded.Operation = WireJournalOperation::Reparent;
					if (payload.Parent) encoded.Parent = WireObjectId::FromObjectId(*payload.Parent);
				} else {
					encoded.Operation = WireJournalOperation::Destroy;
				}
			},
			record.Payload
		);
		return encoded;
	}

	std::string SerializeWireJournalRecords(const std::vector<WireJournalRecord> &records) {
		WireJson document{{"Version", WireJournalFormatVersion}, {"Records", WireJson::array()}};
		for (const auto &record : records) {
			WireJson encoded{
				{"Version", record.Version},
				{"Sequence", record.Sequence},
				{"Scope", EncodeWireObjectId(record.Scope)},
				{"Operation", OperationName(record.Operation)},
				{"ObjectId", EncodeWireObjectId(record.Object)},
			};
			switch (record.Operation) {
				case WireJournalOperation::Create:
					encoded["ClassName"] = record.ClassName.value_or("");
					break;
				case WireJournalOperation::PropertyUpdate:
					encoded["PropertyName"] = record.PropertyName.value_or("");
					encoded["Value"] = record.Value ? EncodeWireValue(*record.Value) : WireJson(nullptr);
					break;
				case WireJournalOperation::AttributeUpdate:
					encoded["AttributeName"] = record.AttributeName.value_or("");
					encoded["Value"] = record.Value ? EncodeWireValue(*record.Value) : WireJson(nullptr);
					break;
				case WireJournalOperation::ExtensionPropertyUpdate:
					encoded["ExtensionSchemaId"] = record.ExtensionSchemaId
						? record.ExtensionSchemaId->ToString() : "";
					encoded["DefinitionVersion"] = record.DefinitionVersion.value_or(0);
					encoded["PropertyName"] = record.ExtensionPropertyName.value_or("");
					encoded["Value"] = record.Value ? EncodeWireValue(*record.Value) : WireJson(nullptr);
					break;
				case WireJournalOperation::TagAdded:
				case WireJournalOperation::TagRemoved:
					encoded["TagName"] = record.TagName.value_or("");
					break;
				case WireJournalOperation::Reparent:
					encoded["ParentId"] = record.Parent ? EncodeWireObjectId(*record.Parent) : WireJson(nullptr);
					break;
				case WireJournalOperation::Destroy: break;
			}
			document["Records"].push_back(std::move(encoded));
		}
		return document.dump();
	}

	WireJournalParseResult DeserializeWireJournalRecords(std::string_view serialized) {
		WireJournalParseResult result;
		try {
			auto document = WireJson::parse(serialized);
			if (!document.is_object() || !HasOnlyFields(document, {"Version", "Records"}) ||
				document.value("Version", 0u) != WireJournalFormatVersion ||
				!document.contains("Records") || !document["Records"].is_array()) {
				result.Errors.push_back("Invalid or unsupported wire journal envelope");
				return result;
			}

			std::vector<WireJournalRecord> records;
			for (const auto &encoded : document["Records"]) {
				if (!encoded.is_object() || !encoded.contains("Version") ||
					encoded.value("Version", 0u) != WireJournalFormatVersion ||
					!encoded.contains("Sequence") || !encoded["Sequence"].is_number_unsigned() ||
					!encoded.contains("Scope") ||
					!encoded.contains("Operation") || !encoded["Operation"].is_string() ||
					!encoded.contains("ObjectId")) {
					result.Errors.push_back("Invalid or unsupported wire journal record");
					return result;
				}
				auto operation = ParseOperation(encoded["Operation"].get<std::string>());
				auto scope = DecodeWireObjectId(encoded["Scope"]);
				auto object = DecodeWireObjectId(encoded["ObjectId"]);
				const auto sequence = encoded["Sequence"].get<std::uint64_t>();
				if (!operation || !scope || !object || sequence == 0) {
					result.Errors.push_back("Invalid wire journal operation, sequence, scope, or ObjectId");
					return result;
				}

				WireJournalRecord record{.Sequence = sequence, .Scope = *scope, .Operation = *operation, .Object = *object};
				switch (*operation) {
					case WireJournalOperation::Create:
						if (!HasOnlyFields(encoded, {"Version", "Sequence", "Scope", "Operation", "ObjectId", "ClassName"}) ||
							!encoded.contains("ClassName") || !encoded["ClassName"].is_string() ||
							encoded["ClassName"].get<std::string>().empty())
							throw std::invalid_argument("Invalid Create journal record");
						record.ClassName = encoded["ClassName"].get<std::string>();
						break;
					case WireJournalOperation::PropertyUpdate: {
						if (!HasOnlyFields(encoded, {"Version", "Sequence", "Scope", "Operation", "ObjectId", "PropertyName", "Value"}) ||
							!encoded.contains("PropertyName") || !encoded["PropertyName"].is_string() ||
							encoded["PropertyName"].get<std::string>().empty() || !encoded.contains("Value"))
							throw std::invalid_argument("Invalid PropertyUpdate journal record");
						auto value = DecodeWireValue(encoded["Value"]);
						if (!value) throw std::invalid_argument("Invalid PropertyUpdate WireValue");
						record.PropertyName = encoded["PropertyName"].get<std::string>();
						record.Value = std::move(*value);
						break;
					}
					case WireJournalOperation::AttributeUpdate: {
						if (!HasOnlyFields(encoded, {"Version", "Sequence", "Scope", "Operation", "ObjectId", "AttributeName", "Value"}) ||
							!encoded.contains("AttributeName") || !encoded["AttributeName"].is_string() ||
							encoded["AttributeName"].get_ref<const std::string &>().empty() || !encoded.contains("Value"))
							throw std::invalid_argument("Invalid AttributeUpdate journal record");
						auto value = DecodeWireValue(encoded["Value"]);
						if (!value) throw std::invalid_argument("Invalid AttributeUpdate WireValue");
						record.AttributeName = encoded["AttributeName"].get<std::string>();
						ValidateAttributeName(*record.AttributeName);
						if (!std::holds_alternative<std::monostate>(*value)) (void)ValidateAttributeValue(*value);
						record.Value = std::move(*value);
						break;
					}
					case WireJournalOperation::ExtensionPropertyUpdate: {
						if (!HasOnlyFields(encoded, {
								"Version", "Sequence", "Scope", "Operation", "ObjectId", "ExtensionSchemaId",
								"DefinitionVersion", "PropertyName", "Value"
							}) || !encoded.contains("ExtensionSchemaId") || !encoded["ExtensionSchemaId"].is_string() ||
							!encoded.contains("DefinitionVersion") || !encoded["DefinitionVersion"].is_number_unsigned() ||
							!encoded.contains("PropertyName") || !encoded["PropertyName"].is_string() ||
							!encoded.contains("Value"))
							throw std::invalid_argument("Invalid ExtensionPropertyUpdate journal record");
						const auto encodedId = encoded["ExtensionSchemaId"].get<std::string>();
						auto extensionId = SchemaId::Parse(encodedId);
						if (!extensionId || extensionId->ToString() != encodedId)
							throw std::invalid_argument("Invalid extension SchemaId");
						const auto version = encoded["DefinitionVersion"].get<std::uint32_t>();
						if (version == 0) throw std::invalid_argument("Invalid extension definition version");
						auto name = encoded["PropertyName"].get<std::string>();
						auto *extension = GetActiveRuntimeSchemaRegistry().FindExtensionById(*extensionId);
						auto *property = GetActiveRuntimeSchemaRegistry().FindExtensionProperty(*extensionId, name);
						auto value = DecodeWireValue(encoded["Value"]);
						if (!extension || extension->DefinitionVersion != version || !property || !value)
							throw std::invalid_argument("Unknown or incompatible extension property identity");
						(void)ValidateSchemaExtensionPropertyValue(property->Type, *value);
						record.ExtensionSchemaId = *extensionId;
						record.DefinitionVersion = version;
						record.ExtensionPropertyName = std::move(name);
						record.Value = std::move(*value);
						break;
					}
					case WireJournalOperation::TagAdded:
					case WireJournalOperation::TagRemoved:
						if (!HasOnlyFields(encoded, {"Version", "Sequence", "Scope", "Operation", "ObjectId", "TagName"}) ||
							!encoded.contains("TagName") || !encoded["TagName"].is_string())
							throw std::invalid_argument("Invalid tag journal record");
						record.TagName = encoded["TagName"].get<std::string>();
						ValidateTagName(*record.TagName);
						break;
					case WireJournalOperation::Reparent:
						if (!HasOnlyFields(encoded, {"Version", "Sequence", "Scope", "Operation", "ObjectId", "ParentId"}) ||
							!encoded.contains("ParentId"))
							throw std::invalid_argument("Invalid Reparent journal record");
						if (!encoded["ParentId"].is_null()) {
							auto parent = DecodeWireObjectId(encoded["ParentId"]);
							if (!parent) throw std::invalid_argument("Invalid Reparent parent ObjectId");
							record.Parent = *parent;
						}
						break;
					case WireJournalOperation::Destroy:
						if (!HasOnlyFields(encoded, {"Version", "Sequence", "Scope", "Operation", "ObjectId"}))
							throw std::invalid_argument("Invalid Destroy journal record");
						break;
				}
				record.Version = WireJournalFormatVersion;
				records.push_back(std::move(record));
			}
			result.Value = std::move(records);
		} catch (const std::exception &error) {
			result.Errors.push_back(std::string("Invalid wire journal: ") + error.what());
		}
		return result;
	}
}
