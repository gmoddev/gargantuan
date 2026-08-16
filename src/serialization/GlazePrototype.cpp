#include "serialization/GlazePrototype.hpp"

#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/runtime/AttributeValidation.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"
#include "gargantuan/runtime/TagIndex.hpp"
#include "gargantuan/runtime/WireCodec.hpp"
#include "runtime/SnapshotValidation.hpp"

#include <glaze/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <variant>

namespace gargantuan::GlazePrototype::Detail {
	using RawMap = std::map<std::string, glz::raw_json>;

	struct ObjectIdDto { std::uint32_t Slot{}; std::uint32_t Generation{}; };
	struct CursorDto { ObjectIdDto Scope; std::uint64_t NextSequence{}; };
	struct ExtensionDto { std::string ExtensionSchemaId; std::uint32_t DefinitionVersion{}; RawMap Properties; };
	struct CustomDto { std::string DeclaringClassSchemaId; std::uint32_t DefinitionVersion{}; RawMap Properties; };
	struct SnapshotObjectDto {
		ObjectIdDto Id;
		std::string ClassSchemaId;
		std::uint32_t ClassDefinitionVersion{};
		std::string ClassName;
		std::string Name;
		glz::raw_json Parent;
		RawMap Properties;
		RawMap Attributes;
		std::vector<ExtensionDto> Extensions;
		std::vector<CustomDto> CustomProperties;
		std::vector<std::string> Tags;
	};
	struct SnapshotDto { std::uint32_t Version{}; CursorDto Cursor; std::vector<SnapshotObjectDto> Objects; };
	struct JournalDto { std::uint32_t Version{}; std::vector<glz::raw_json> Records; };
	struct RecordHeaderDto {
		std::uint32_t Version{};
		std::uint64_t Sequence{};
		ObjectIdDto Scope;
		std::string Operation;
		ObjectIdDto ObjectId;
	};
	struct CreateDto : RecordHeaderDto {
		std::string ClassName;
		std::string ClassSchemaId;
		std::uint32_t DefinitionVersion{};
	};
	struct PropertyDto : RecordHeaderDto {
		std::string PropertyName;
		glz::raw_json Value;
		std::optional<std::string> DeclaringClassSchemaId;
		std::optional<std::uint32_t> DefinitionVersion;
	};
	struct AttributeDto : RecordHeaderDto { std::string AttributeName; glz::raw_json Value; };
	struct ExtensionPropertyDto : RecordHeaderDto {
		std::string ExtensionSchemaId;
		std::uint32_t DefinitionVersion{};
		std::string PropertyName;
		glz::raw_json Value;
	};
	struct TagDto : RecordHeaderDto { std::string TagName; };
	struct ReparentDto : RecordHeaderDto { glz::raw_json ParentId; };
	struct DestroyDto : RecordHeaderDto {};
}

template <> struct glz::meta<gargantuan::GlazePrototype::Detail::ObjectIdDto> {
	using T = gargantuan::GlazePrototype::Detail::ObjectIdDto;
	static constexpr auto value = object("Slot", &T::Slot, "Generation", &T::Generation);
};
template <> struct glz::meta<gargantuan::GlazePrototype::Detail::CursorDto> {
	using T = gargantuan::GlazePrototype::Detail::CursorDto;
	static constexpr auto value = object("Scope", &T::Scope, "NextSequence", &T::NextSequence);
};
template <> struct glz::meta<gargantuan::GlazePrototype::Detail::ExtensionDto> {
	using T = gargantuan::GlazePrototype::Detail::ExtensionDto;
	static constexpr auto value = object("ExtensionSchemaId", &T::ExtensionSchemaId, "DefinitionVersion", &T::DefinitionVersion, "Properties", &T::Properties);
};
template <> struct glz::meta<gargantuan::GlazePrototype::Detail::CustomDto> {
	using T = gargantuan::GlazePrototype::Detail::CustomDto;
	static constexpr auto value = object("DeclaringClassSchemaId", &T::DeclaringClassSchemaId, "DefinitionVersion", &T::DefinitionVersion, "Properties", &T::Properties);
};
template <> struct glz::meta<gargantuan::GlazePrototype::Detail::SnapshotObjectDto> {
	using T = gargantuan::GlazePrototype::Detail::SnapshotObjectDto;
	static constexpr auto value = object(
		"Id", &T::Id, "ClassSchemaId", &T::ClassSchemaId, "ClassDefinitionVersion", &T::ClassDefinitionVersion,
		"ClassName", &T::ClassName, "Name", &T::Name, "Parent", &T::Parent, "Properties", &T::Properties,
		"Attributes", &T::Attributes, "Extensions", &T::Extensions, "CustomProperties", &T::CustomProperties,
		"Tags", &T::Tags
	);
};
template <> struct glz::meta<gargantuan::GlazePrototype::Detail::SnapshotDto> {
	using T = gargantuan::GlazePrototype::Detail::SnapshotDto;
	static constexpr auto value = object("Version", &T::Version, "Cursor", &T::Cursor, "Objects", &T::Objects);
};
template <> struct glz::meta<gargantuan::GlazePrototype::Detail::JournalDto> {
	using T = gargantuan::GlazePrototype::Detail::JournalDto;
	static constexpr auto value = object("Version", &T::Version, "Records", &T::Records);
};
template <> struct glz::meta<gargantuan::GlazePrototype::Detail::RecordHeaderDto> {
	using T = gargantuan::GlazePrototype::Detail::RecordHeaderDto;
	static constexpr auto value = object("Version", &T::Version, "Sequence", &T::Sequence, "Scope", &T::Scope, "Operation", &T::Operation, "ObjectId", &T::ObjectId);
};
template <> struct glz::meta<gargantuan::GlazePrototype::Detail::CreateDto> {
	using T = gargantuan::GlazePrototype::Detail::CreateDto;
	static constexpr auto value = object(
		"Version", &T::Version, "Sequence", &T::Sequence, "Scope", &T::Scope, "Operation", &T::Operation,
		"ObjectId", &T::ObjectId, "ClassName", &T::ClassName, "ClassSchemaId", &T::ClassSchemaId,
		"DefinitionVersion", &T::DefinitionVersion
	);
};
template <> struct glz::meta<gargantuan::GlazePrototype::Detail::PropertyDto> {
	using T = gargantuan::GlazePrototype::Detail::PropertyDto;
	static constexpr auto value = object(
		"Version", &T::Version, "Sequence", &T::Sequence, "Scope", &T::Scope, "Operation", &T::Operation,
		"ObjectId", &T::ObjectId, "PropertyName", &T::PropertyName, "Value", &T::Value,
		"DeclaringClassSchemaId", &T::DeclaringClassSchemaId, "DefinitionVersion", &T::DefinitionVersion
	);
};
template <> struct glz::meta<gargantuan::GlazePrototype::Detail::AttributeDto> {
	using T = gargantuan::GlazePrototype::Detail::AttributeDto;
	static constexpr auto value = object(
		"Version", &T::Version, "Sequence", &T::Sequence, "Scope", &T::Scope, "Operation", &T::Operation,
		"ObjectId", &T::ObjectId, "AttributeName", &T::AttributeName, "Value", &T::Value
	);
};
template <> struct glz::meta<gargantuan::GlazePrototype::Detail::ExtensionPropertyDto> {
	using T = gargantuan::GlazePrototype::Detail::ExtensionPropertyDto;
	static constexpr auto value = object(
		"Version", &T::Version, "Sequence", &T::Sequence, "Scope", &T::Scope, "Operation", &T::Operation,
		"ObjectId", &T::ObjectId, "ExtensionSchemaId", &T::ExtensionSchemaId,
		"DefinitionVersion", &T::DefinitionVersion, "PropertyName", &T::PropertyName, "Value", &T::Value
	);
};
template <> struct glz::meta<gargantuan::GlazePrototype::Detail::TagDto> {
	using T = gargantuan::GlazePrototype::Detail::TagDto;
	static constexpr auto value = object("Version", &T::Version, "Sequence", &T::Sequence, "Scope", &T::Scope, "Operation", &T::Operation, "ObjectId", &T::ObjectId, "TagName", &T::TagName);
};
template <> struct glz::meta<gargantuan::GlazePrototype::Detail::ReparentDto> {
	using T = gargantuan::GlazePrototype::Detail::ReparentDto;
	static constexpr auto value = object("Version", &T::Version, "Sequence", &T::Sequence, "Scope", &T::Scope, "Operation", &T::Operation, "ObjectId", &T::ObjectId, "ParentId", &T::ParentId);
};
template <> struct glz::meta<gargantuan::GlazePrototype::Detail::DestroyDto> {
	using T = gargantuan::GlazePrototype::Detail::DestroyDto;
	static constexpr auto value = object("Version", &T::Version, "Sequence", &T::Sequence, "Scope", &T::Scope, "Operation", &T::Operation, "ObjectId", &T::ObjectId);
};

namespace gargantuan::GlazePrototype {
	namespace {
		using namespace Detail;
		using Generic = glz::generic_u64;
		constexpr glz::opts StrictReadOptions{.error_on_unknown_keys = true, .error_on_missing_keys = true};
		constexpr glz::opts LooseHeaderOptions{.error_on_unknown_keys = false, .error_on_missing_keys = true};
		constexpr glz::opts RequiredNullWriteOptions{.skip_null_members = false};

		SerializationErrorCode Classify(std::string_view Message) {
			if (Message.find("unexpected_end") != std::string_view::npos) return SerializationErrorCode::TruncatedInput;
			if (Message.find("unknown_key") != std::string_view::npos) return SerializationErrorCode::UnknownField;
			if (Message.find("missing_key") != std::string_view::npos) return SerializationErrorCode::MissingField;
			return SerializationErrorCode::InvalidSyntax;
		}

		template <class T, glz::opts Options = StrictReadOptions>
		SerializationResult<T> Read(std::string_view Encoded, std::string_view Name) {
			try {
				ValidateProtocolJsonDocument(Encoded, MaximumProtocolDocumentBytes);
				if (!IsValidProtocolUtf8(Encoded))
					return SerializationFailure(SerializationErrorCode::InvalidSyntax, std::string(Name) + " is not valid UTF-8", "$");
			} catch (const std::invalid_argument &Error) {
				return SerializationFailure(SerializationErrorCode::LimitExceeded, std::string(Name) + " violates protocol limits", "$", Error.what());
			}
			T Value{};
			auto Error = glz::read<Options>(Value, Encoded);
			if (Error) {
				auto Diagnostic = glz::format_error(Error, Encoded);
				return SerializationFailure(Classify(Diagnostic), std::string(Name) + " contains invalid JSON", "$", std::move(Diagnostic));
			}
			return Value;
		}

		template <class T, glz::opts Options = glz::opts{}>
		SerializationResult<std::string> Write(const T &Value, std::string_view Name) {
			std::string Encoded;
			auto Error = glz::write<Options>(Value, Encoded);
			if (Error) return SerializationFailure(
				SerializationErrorCode::InternalFailure, std::string(Name) + " Glaze encoding failed", "$",
				glz::format_error(Error, Encoded)
			);
			return Encoded;
		}

		ObjectIdDto ToDto(WireObjectId Value) { return {Value.Slot, Value.Generation}; }
		WireObjectId FromDto(const ObjectIdDto &Value) {
			WireObjectId Result{Value.Slot, Value.Generation};
			if (!Result.IsValid()) throw std::invalid_argument("ObjectId is invalid");
			return Result;
		}

		SerializationResult<glz::raw_json> EncodeNullableId(const std::optional<WireObjectId> &Value) {
			if (!Value) return glz::raw_json{"null"};
			auto Encoded = Write(ToDto(*Value), "ObjectId");
			if (!Encoded) return std::unexpected(Encoded.error());
			return glz::raw_json{std::move(*Encoded)};
		}

		SerializationResult<std::optional<WireObjectId>> DecodeNullableId(std::string_view Encoded) {
			auto Value = Read<std::optional<ObjectIdDto>>(Encoded, "nullable ObjectId");
			if (!Value) return std::unexpected(Value.error());
			if (!*Value) return std::optional<WireObjectId>{};
			try { return std::optional<WireObjectId>{FromDto(**Value)}; }
			catch (const std::exception &Error) { return SerializationFailure(SerializationErrorCode::InvalidValue, "ObjectId is invalid", "$", Error.what()); }
		}

		bool HasOnly(const Generic &Value, std::initializer_list<std::string_view> Fields) {
			if (!Value.is_object() || Value.size() != Fields.size()) return false;
			for (const auto &Field : Fields) if (!Value.contains(Field)) return false;
			return true;
		}

		std::optional<std::int32_t> Signed32(const Generic &Value) {
			if (auto Raw = Value.get_if<std::uint64_t>(); Raw && *Raw <= std::uint64_t{INT32_MAX}) return static_cast<std::int32_t>(*Raw);
			if (auto Raw = Value.get_if<std::int64_t>(); Raw && *Raw >= INT32_MIN && *Raw <= INT32_MAX) return static_cast<std::int32_t>(*Raw);
			return std::nullopt;
		}

		std::optional<double> FiniteDouble(const Generic &Value) {
			double Result{};
			if (auto Raw = Value.get_if<double>()) Result = *Raw;
			else if (auto Raw = Value.get_if<std::uint64_t>()) Result = static_cast<double>(*Raw);
			else if (auto Raw = Value.get_if<std::int64_t>()) Result = static_cast<double>(*Raw);
			else return std::nullopt;
			return std::isfinite(Result) ? std::optional(Result) : std::nullopt;
		}

		std::optional<float> FiniteFloat(const Generic &Value) {
			auto Decoded = FiniteDouble(Value);
			if (!Decoded || *Decoded < -std::numeric_limits<float>::max() || *Decoded > std::numeric_limits<float>::max()) return std::nullopt;
			const auto Result = static_cast<float>(*Decoded);
			return std::isfinite(Result) ? std::optional(Result) : std::nullopt;
		}

		Generic::array_t FloatArray(std::initializer_list<float> Values) {
			Generic::array_t Result;
			Result.reserve(Values.size());
			for (const auto Value : Values) Result.emplace_back(static_cast<double>(Value));
			return Result;
		}

		std::size_t CountNodes(const Generic &Value) {
			std::size_t Count = 1;
			if (Value.is_array()) for (const auto &Child : Value.get_array()) Count += CountNodes(Child);
			else if (Value.is_object()) for (const auto &[Name, Child] : Value.get_object()) { (void)Name; Count += CountNodes(Child); }
			return Count;
		}

		std::string Quoted(std::string_view Value) {
			auto Encoded = Write(std::string(Value), "JSON string");
			if (!Encoded) throw std::runtime_error(Encoded.error().Format());
			return std::move(*Encoded);
		}

		std::string Floating(double Value) {
			auto Encoded = Write(Value, "JSON number");
			if (!Encoded) throw std::runtime_error(Encoded.error().Format());
			if (Encoded->find_first_of(".eE") == std::string::npos) Encoded->append(".0");
			return std::move(*Encoded);
		}

		template <class Range> std::string FloatingArray(const Range &Values) {
			std::string Result{"["};
			bool First = true;
			for (const auto Value : Values) { if (!First) Result.push_back(','); First = false; Result += Floating(static_cast<double>(Value)); }
			Result.push_back(']');
			return Result;
		}

		SerializationResult<std::string> EncodeWire(const WireValue &Value) {
			try {
				ValidateProtocolWireValue(Value);
				return std::visit([&](const auto &Typed) -> SerializationResult<std::string> {
					using T = std::decay_t<decltype(Typed)>;
					if constexpr (std::is_same_v<T, std::monostate>) return std::string(R"({"Type":"Null"})");
					else if constexpr (std::is_same_v<T, bool>) return std::string(Typed ? R"({"Type":"Bool","Value":true})" : R"({"Type":"Bool","Value":false})");
					else if constexpr (std::is_same_v<T, int>) return std::string(R"({"Type":"Int","Value":)") + std::to_string(Typed) + "}";
					else if constexpr (std::is_same_v<T, double>) return std::string(R"({"Type":"Double","Value":)") + Floating(Typed) + "}";
					else if constexpr (std::is_same_v<T, WireFloat>) return std::string(R"({"Type":"Float","Value":)") + Floating(Typed.Value) + "}";
					else if constexpr (std::is_same_v<T, std::string>) return std::string(R"({"Type":"String","Value":)") + Quoted(Typed) + "}";
					else if constexpr (std::is_same_v<T, WireVector2>) return std::string(R"({"Type":"Vector2","Value":)") + FloatingArray(std::array{Typed.X, Typed.Y}) + "}";
					else if constexpr (std::is_same_v<T, WireVector3>) return std::string(R"({"Type":"Vector3","Value":)") + FloatingArray(std::array{Typed.X, Typed.Y, Typed.Z}) + "}";
					else if constexpr (std::is_same_v<T, WireColor3>) return std::string(R"({"Type":"Color3","Value":)") + FloatingArray(std::array{Typed.R, Typed.G, Typed.B}) + "}";
					else if constexpr (std::is_same_v<T, WireUDim>) return std::string(R"({"Type":"UDim","Value":[)") + Floating(Typed.Scale) + "," + std::to_string(Typed.Offset) + "]}";
					else if constexpr (std::is_same_v<T, WireUDim2>) return std::string(R"({"Type":"UDim2","Value":[)") + Floating(Typed.X.Scale) + "," + std::to_string(Typed.X.Offset) + "," + Floating(Typed.Y.Scale) + "," + std::to_string(Typed.Y.Offset) + "]}";
					else if constexpr (std::is_same_v<T, WireCFrame>) return std::string(R"({"Type":"CFrame","Value":)") + FloatingArray(Typed.Components) + "}";
					else if constexpr (std::is_same_v<T, WireEnumItem>) return std::string(R"({"Type":"EnumItem","Enum":)") + Quoted(Typed.EnumType) + R"(,"Value":)" + Quoted(Typed.Item) + "}";
					else if constexpr (std::is_same_v<T, WireSchemaEnumValue>) return std::string(R"({"Type":"SchemaEnum","SchemaId":)") + Quoted(Typed.EnumSchemaId.ToString()) + R"(,"DefinitionVersion":)" + std::to_string(Typed.DefinitionVersion) + R"(,"Value":)" + std::to_string(Typed.ItemValue) + "}";
					else return std::string(R"({"Type":"ObjectReference","Value":{"Slot":)") + std::to_string(Typed.Object.Slot) + R"(,"Generation":)" + std::to_string(Typed.Object.Generation) + "}}";
				}, Value);
			} catch (const std::exception &Error) {
				return SerializationFailure(SerializationErrorCode::InvalidValue, "WireValue is invalid", "$", Error.what());
			}
		}

		SerializationResult<WireValue> DecodeWire(std::string_view Encoded, std::size_t *NodeCount = nullptr) {
			auto Parsed = Read<Generic>(Encoded, "WireValue");
			if (!Parsed) return std::unexpected(Parsed.error());
			if (NodeCount) *NodeCount += CountNodes(*Parsed);
			const auto &Document = *Parsed;
			if (!Document.is_object() || !Document.contains("Type") || !Document["Type"].is_string())
				return SerializationFailure(SerializationErrorCode::InvalidType, "WireValue Type is missing or invalid", "$.Type");
			const auto &Type = Document["Type"].get<std::string>();
			try { ValidateProtocolString(Type, 32, "WireValue type"); }
			catch (const std::exception &Error) { return SerializationFailure(SerializationErrorCode::LimitExceeded, Error.what(), "$.Type"); }
			if (Type == "Null" && HasOnly(Document, {"Type"})) return WireValue(std::monostate{});
			if (!Document.contains("Value")) return SerializationFailure(SerializationErrorCode::MissingField, "WireValue Value is missing", "$.Value");
			const auto &Raw = Document["Value"];
			try {
				if (Type == "Bool" && HasOnly(Document, {"Type", "Value"}) && Raw.is_boolean()) return WireValue(Raw.get<bool>());
				if (Type == "Int" && HasOnly(Document, {"Type", "Value"})) if (auto V = Signed32(Raw)) return WireValue(static_cast<int>(*V));
				if (Type == "Double" && HasOnly(Document, {"Type", "Value"})) if (auto V = FiniteDouble(Raw)) return WireValue(*V);
				if (Type == "Float" && HasOnly(Document, {"Type", "Value"})) if (auto V = FiniteFloat(Raw)) return WireValue(WireFloat{*V});
				if (Type == "String" && HasOnly(Document, {"Type", "Value"}) && Raw.is_string()) {
					ValidateProtocolString(Raw.get<std::string>(), MaximumProtocolStringBytes, "Wire string"); return WireValue(Raw.get<std::string>());
				}
				auto Floats = [&](std::size_t Count) -> std::optional<std::vector<float>> {
					if (!Raw.is_array() || Raw.size() != Count) return std::nullopt;
					std::vector<float> Result; Result.reserve(Count);
					for (const auto &Item : Raw.get_array()) { auto V = FiniteFloat(Item); if (!V) return std::nullopt; Result.push_back(*V); }
					return Result;
				};
				if (HasOnly(Document, {"Type", "Value"})) {
					if (Type == "Vector2") if (auto V = Floats(2)) return WireValue(WireVector2{(*V)[0], (*V)[1]});
					if (Type == "Vector3") if (auto V = Floats(3)) return WireValue(WireVector3{(*V)[0], (*V)[1], (*V)[2]});
					if (Type == "Color3") if (auto V = Floats(3)) return WireValue(WireColor3{(*V)[0], (*V)[1], (*V)[2]});
					if (Type == "UDim" && Raw.is_array() && Raw.size() == 2) { auto S = FiniteFloat(Raw[0]); auto O = Signed32(Raw[1]); if (S && O) return WireValue(WireUDim{*S, *O}); }
					if (Type == "UDim2" && Raw.is_array() && Raw.size() == 4) { auto XS = FiniteFloat(Raw[0]); auto XO = Signed32(Raw[1]); auto YS = FiniteFloat(Raw[2]); auto YO = Signed32(Raw[3]); if (XS && XO && YS && YO) return WireValue(WireUDim2{{*XS, *XO}, {*YS, *YO}}); }
					if (Type == "CFrame") if (auto V = Floats(12)) { WireCFrame Result{}; std::copy(V->begin(), V->end(), Result.Components.begin()); return WireValue(Result); }
					if (Type == "ObjectReference" && Raw.is_object() && HasOnly(Raw, {"Slot", "Generation"})) {
						auto Slot = Raw["Slot"].get_if<std::uint64_t>(); auto Generation = Raw["Generation"].get_if<std::uint64_t>();
						if (Slot && Generation && *Slot <= UINT32_MAX && *Generation <= UINT32_MAX) { WireObjectId Id{static_cast<std::uint32_t>(*Slot), static_cast<std::uint32_t>(*Generation)}; if (Id.IsValid()) return WireValue(WireObjectReference{Id}); }
					}
				}
				if (Type == "EnumItem" && HasOnly(Document, {"Type", "Enum", "Value"}) && Document["Enum"].is_string() && Raw.is_string()) {
					auto Enum = Document["Enum"].get<std::string>(); auto Item = Raw.get<std::string>();
					ValidateProtocolString(Enum, MaximumProtocolIdentifierBytes, "Wire enum"); ValidateProtocolString(Item, MaximumProtocolIdentifierBytes, "Wire enum item");
					return WireValue(WireEnumItem{std::move(Enum), std::move(Item)});
				}
				if (Type == "SchemaEnum" && HasOnly(Document, {"Type", "SchemaId", "DefinitionVersion", "Value"}) && Document["SchemaId"].is_string()) {
					auto Id = SchemaId::Parse(Document["SchemaId"].get<std::string>()); auto Version = Document["DefinitionVersion"].get_if<std::uint64_t>(); auto Item = Signed32(Raw);
					if (Id && Version && *Version > 0 && *Version <= UINT32_MAX && Item) return WireValue(WireSchemaEnumValue{*Id, static_cast<std::uint32_t>(*Version), *Item});
				}
			} catch (const std::exception &Error) {
				return SerializationFailure(SerializationErrorCode::InvalidValue, "WireValue validation failed", "$", Error.what());
			}
			return SerializationFailure(SerializationErrorCode::InvalidValue, "WireValue has invalid fields or values", "$");
		}

		SerializationResult<RawMap> EncodeMap(const std::map<std::string, WireValue> &Values) {
			RawMap Result;
			for (const auto &[Name, Value] : Values) {
				auto Encoded = EncodeWire(Value); if (!Encoded) return std::unexpected(Encoded.error());
				Result.emplace(Name, glz::raw_json{std::move(*Encoded)});
			}
			return Result;
		}

		SerializationResult<std::map<std::string, WireValue>> DecodeMap(const RawMap &Values, std::size_t *NodeCount = nullptr) {
			std::map<std::string, WireValue> Result;
			for (const auto &[Name, Value] : Values) {
				auto Decoded = DecodeWire(Value.str, NodeCount); if (!Decoded) return std::unexpected(Decoded.error());
				Result.emplace(Name, std::move(*Decoded));
			}
			return Result;
		}

		const char *OperationName(WireJournalOperation Value) {
			switch (Value) {
				case WireJournalOperation::Create: return "Create"; case WireJournalOperation::PropertyUpdate: return "PropertyUpdate";
				case WireJournalOperation::AttributeUpdate: return "AttributeUpdate"; case WireJournalOperation::ExtensionPropertyUpdate: return "ExtensionPropertyUpdate";
				case WireJournalOperation::TagAdded: return "TagAdded"; case WireJournalOperation::TagRemoved: return "TagRemoved";
				case WireJournalOperation::Reparent: return "Reparent"; case WireJournalOperation::Destroy: return "Destroy";
			}
			throw std::invalid_argument("Unknown journal operation");
		}

		template <class T> T Header(const WireJournalRecord &Record) {
			T Value; Value.Version = Record.Version; Value.Sequence = Record.Sequence; Value.Scope = ToDto(Record.Scope);
			Value.Operation = OperationName(Record.Operation); Value.ObjectId = ToDto(Record.Object); return Value;
		}

		SerializationResult<glz::raw_json> EncodeRecord(const WireJournalRecord &Record) {
			SerializationResult<std::string> Encoded = SerializationFailure(SerializationErrorCode::InternalFailure, "Unencoded journal record");
			switch (Record.Operation) {
				case WireJournalOperation::Create: { auto V = Header<CreateDto>(Record); V.ClassName = Record.ClassName.value_or(""); V.ClassSchemaId = Record.ClassSchemaId ? Record.ClassSchemaId->ToString() : ""; V.DefinitionVersion = Record.DefinitionVersion.value_or(0); Encoded = Write(V, "Create record"); break; }
				case WireJournalOperation::PropertyUpdate: { auto V = Header<PropertyDto>(Record); V.PropertyName = Record.PropertyName.value_or(""); auto W = Record.Value ? EncodeWire(*Record.Value) : SerializationResult<std::string>(std::string("null")); if (!W) return std::unexpected(W.error()); V.Value = glz::raw_json{std::move(*W)}; if (Record.DeclaringClassSchemaId) { V.DeclaringClassSchemaId = Record.DeclaringClassSchemaId->ToString(); V.DefinitionVersion = Record.DefinitionVersion.value_or(0); } Encoded = Write(V, "PropertyUpdate record"); break; }
				case WireJournalOperation::AttributeUpdate: { auto V = Header<AttributeDto>(Record); V.AttributeName = Record.AttributeName.value_or(""); auto W = Record.Value ? EncodeWire(*Record.Value) : SerializationResult<std::string>(std::string("null")); if (!W) return std::unexpected(W.error()); V.Value = glz::raw_json{std::move(*W)}; Encoded = Write(V, "AttributeUpdate record"); break; }
				case WireJournalOperation::ExtensionPropertyUpdate: { auto V = Header<ExtensionPropertyDto>(Record); V.ExtensionSchemaId = Record.ExtensionSchemaId ? Record.ExtensionSchemaId->ToString() : ""; V.DefinitionVersion = Record.DefinitionVersion.value_or(0); V.PropertyName = Record.ExtensionPropertyName.value_or(""); auto W = Record.Value ? EncodeWire(*Record.Value) : SerializationResult<std::string>(std::string("null")); if (!W) return std::unexpected(W.error()); V.Value = glz::raw_json{std::move(*W)}; Encoded = Write(V, "ExtensionPropertyUpdate record"); break; }
				case WireJournalOperation::TagAdded: case WireJournalOperation::TagRemoved: { auto V = Header<TagDto>(Record); V.TagName = Record.TagName.value_or(""); Encoded = Write(V, "Tag record"); break; }
				case WireJournalOperation::Reparent: { auto V = Header<ReparentDto>(Record); auto Parent = EncodeNullableId(Record.Parent); if (!Parent) return std::unexpected(Parent.error()); V.ParentId = std::move(*Parent); Encoded = Write(V, "Reparent record"); break; }
				case WireJournalOperation::Destroy: Encoded = Write(Header<DestroyDto>(Record), "Destroy record"); break;
			}
			if (!Encoded) return std::unexpected(Encoded.error()); return glz::raw_json{std::move(*Encoded)};
		}
	}

	SerializationResult<std::string> EncodeSnapshot(const Snapshot &Value) {
		try {
			SnapshotDto Dto{.Version = Value.Version, .Cursor = {ToDto(WireObjectId::FromObjectId(Value.Cursor.Scope)), Value.Cursor.NextSequence}};
			Dto.Objects.reserve(Value.Objects.size());
			for (const auto &Object : Value.Objects) {
				auto Properties = EncodeMap(Object.Properties); if (!Properties) return std::unexpected(Properties.error());
				auto Attributes = EncodeMap(Object.Attributes); if (!Attributes) return std::unexpected(Attributes.error());
				auto Parent = EncodeNullableId(Object.Parent); if (!Parent) return std::unexpected(Parent.error());
				SnapshotObjectDto Encoded{ToDto(Object.Id), Object.ClassSchemaId.ToString(), Object.ClassDefinitionVersion, Object.ClassName, Object.Name, std::move(*Parent), std::move(*Properties), std::move(*Attributes)};
				for (const auto &State : Object.Extensions) { auto P = EncodeMap(State.Properties); if (!P) return std::unexpected(P.error()); Encoded.Extensions.push_back({State.ExtensionSchemaId.ToString(), State.DefinitionVersion, std::move(*P)}); }
				for (const auto &State : Object.CustomProperties) { auto P = EncodeMap(State.Properties); if (!P) return std::unexpected(P.error()); Encoded.CustomProperties.push_back({State.DeclaringClassSchemaId.ToString(), State.DefinitionVersion, std::move(*P)}); }
				Encoded.Tags = Object.Tags; Dto.Objects.push_back(std::move(Encoded));
			}
			return Write<SnapshotDto, RequiredNullWriteOptions>(Dto, "Snapshot");
		} catch (const std::exception &Error) { return SerializationFailure(SerializationErrorCode::InvalidValue, "Snapshot semantic validation failed", "$", Error.what()); }
	}

	SerializationResult<Snapshot> DecodeSnapshot(std::string_view Encoded) {
		auto Parsed = Read<SnapshotDto>(Encoded, "Snapshot"); if (!Parsed) return std::unexpected(Parsed.error());
		try {
			if (Parsed->Version != SnapshotFormatVersion) return SerializationFailure(SerializationErrorCode::UnsupportedVersion, "Unsupported Snapshot version", "$.Version");
			Snapshot Value; Value.Version = Parsed->Version; Value.Cursor = {FromDto(Parsed->Cursor.Scope).ToObjectId(), Parsed->Cursor.NextSequence};
			if (Parsed->Objects.empty() || Parsed->Objects.size() > MaximumSnapshotObjects) throw std::invalid_argument("Snapshot object count is outside its supported range");
			std::size_t NodeCount = 8;
			for (const auto &Object : Parsed->Objects) {
				auto Parent = DecodeNullableId(Object.Parent.str); if (!Parent) return std::unexpected(Parent.error());
				NodeCount += *Parent ? 15 : 13;
				auto ClassId = SchemaId::Parse(Object.ClassSchemaId); if (!ClassId) throw std::invalid_argument("Snapshot ClassSchemaId is invalid");
				auto Properties = DecodeMap(Object.Properties, &NodeCount); if (!Properties) return std::unexpected(Properties.error());
				auto Attributes = DecodeMap(Object.Attributes, &NodeCount); if (!Attributes) return std::unexpected(Attributes.error());
				SnapshotObject Result{FromDto(Object.Id), *ClassId, Object.ClassDefinitionVersion, Object.ClassName, Object.Name, std::nullopt, std::move(*Properties), std::move(*Attributes)};
				Result.Parent = std::move(*Parent);
				for (const auto &State : Object.Extensions) { NodeCount += 4; auto Id = SchemaId::Parse(State.ExtensionSchemaId); auto P = DecodeMap(State.Properties, &NodeCount); if (!Id || !P) throw std::invalid_argument("Snapshot extension state is invalid"); Result.Extensions.push_back({*Id, State.DefinitionVersion, std::move(*P)}); }
				for (const auto &State : Object.CustomProperties) { NodeCount += 4; auto Id = SchemaId::Parse(State.DeclaringClassSchemaId); auto P = DecodeMap(State.Properties, &NodeCount); if (!Id || !P) throw std::invalid_argument("Snapshot custom state is invalid"); Result.CustomProperties.push_back({*Id, State.DefinitionVersion, std::move(*P)}); }
				NodeCount += Object.Tags.size();
				if (NodeCount > MaximumProtocolJsonNodes) throw std::invalid_argument("JSON node count exceeds its limit");
				Result.Tags = Object.Tags; Value.Objects.push_back(std::move(Result));
			}
			ValidateSnapshotSemantic(Value); return Value;
		} catch (const std::exception &Error) { return SerializationFailure(SerializationErrorCode::InvalidValue, "Snapshot semantic validation failed", "$", Error.what()); }
	}

	SerializationResult<std::string> EncodeJournal(const std::vector<WireJournalRecord> &Value) {
		if (Value.size() > MaximumWireJournalRecords) return SerializationFailure(SerializationErrorCode::LimitExceeded, "Journal record count exceeds its limit", "$.Records");
		JournalDto Dto{.Version = WireJournalFormatVersion}; Dto.Records.reserve(Value.size());
		for (const auto &Record : Value) { auto Encoded = EncodeRecord(Record); if (!Encoded) return std::unexpected(Encoded.error()); Dto.Records.push_back(std::move(*Encoded)); }
		return Write(Dto, "WireJournal");
	}

	SerializationResult<std::vector<WireJournalRecord>> DecodeJournal(std::string_view Encoded) {
		auto Parsed = Read<JournalDto>(Encoded, "WireJournal"); if (!Parsed) return std::unexpected(Parsed.error());
		if (Parsed->Version != WireJournalFormatVersion) return SerializationFailure(SerializationErrorCode::UnsupportedVersion, "Unsupported WireJournal version", "$.Version");
		if (Parsed->Records.size() > MaximumWireJournalRecords) return SerializationFailure(SerializationErrorCode::LimitExceeded, "Journal record count exceeds its limit", "$.Records");
		std::vector<WireJournalRecord> Result; Result.reserve(Parsed->Records.size());
		try {
			for (const auto &Raw : Parsed->Records) {
				auto HeaderValue = Read<RecordHeaderDto, LooseHeaderOptions>(Raw.str, "Journal record"); if (!HeaderValue) return std::unexpected(HeaderValue.error());
				if (HeaderValue->Version != WireJournalFormatVersion || HeaderValue->Sequence == 0) throw std::invalid_argument("Journal record version or sequence is invalid");
				WireJournalRecord Record{.Sequence = HeaderValue->Sequence, .Scope = FromDto(HeaderValue->Scope), .Object = FromDto(HeaderValue->ObjectId)};
				if (HeaderValue->Operation == "Create") {
					auto V = Read<CreateDto>(Raw.str, "Create record"); if (!V) return std::unexpected(V.error()); Record.Operation = WireJournalOperation::Create;
					auto Id = SchemaId::Parse(V->ClassSchemaId); auto *Definition = Id ? GetActiveRuntimeSchemaRegistry().FindClassById(*Id) : nullptr;
					ValidateProtocolString(V->ClassName, MaximumProtocolIdentifierBytes, "Create class name");
					const auto ExpectedName = Definition && Definition->ConstructionKind == SchemaClassConstructionKind::CustomData ? Definition->CanonicalName : Definition ? Definition->ClassName : std::string{};
					if (!Definition || V->DefinitionVersion == 0 || Definition->DefinitionVersion != V->DefinitionVersion || V->ClassName != ExpectedName || !GetActiveRuntimeSchemaRegistry().IsClassConstructible(*Definition)) throw std::invalid_argument("Create class identity is invalid");
					Record.ClassName = V->ClassName; Record.ClassSchemaId = *Id; Record.DefinitionVersion = V->DefinitionVersion;
				} else if (HeaderValue->Operation == "PropertyUpdate") {
					auto V = Read<PropertyDto>(Raw.str, "PropertyUpdate record"); if (!V) return std::unexpected(V.error()); Record.Operation = WireJournalOperation::PropertyUpdate; if (V->PropertyName.empty() || V->DeclaringClassSchemaId.has_value() != V->DefinitionVersion.has_value()) throw std::invalid_argument("PropertyUpdate fields are invalid");
					auto W = DecodeWire(V->Value.str); if (!W) return std::unexpected(W.error()); ValidateProtocolString(V->PropertyName, MaximumProtocolIdentifierBytes, "Property name"); Record.PropertyName = V->PropertyName; Record.Value = std::move(*W);
					if (V->DeclaringClassSchemaId) { auto Id = SchemaId::Parse(*V->DeclaringClassSchemaId); auto *Property = Id ? GetActiveRuntimeSchemaRegistry().FindCustomClassProperty(*Id, V->PropertyName) : nullptr; auto *Definition = Id ? GetActiveRuntimeSchemaRegistry().FindClassById(*Id) : nullptr; if (!Id || !Property || !Definition || Definition->ConstructionKind != SchemaClassConstructionKind::CustomData || Definition->DefinitionVersion != *V->DefinitionVersion) throw std::invalid_argument("Custom PropertyUpdate identity is invalid"); (void)ValidateSchemaExtensionPropertyValue(Property->Type, *Record.Value); Record.DeclaringClassSchemaId = *Id; Record.DefinitionVersion = *V->DefinitionVersion; }
				} else if (HeaderValue->Operation == "AttributeUpdate") {
					auto V = Read<AttributeDto>(Raw.str, "AttributeUpdate record"); if (!V) return std::unexpected(V.error()); auto W = DecodeWire(V->Value.str); if (!W) return std::unexpected(W.error()); ValidateAttributeName(V->AttributeName); if (!std::holds_alternative<std::monostate>(*W)) (void)ValidateAttributeValue(*W); Record.Operation = WireJournalOperation::AttributeUpdate; Record.AttributeName = V->AttributeName; Record.Value = std::move(*W);
				} else if (HeaderValue->Operation == "ExtensionPropertyUpdate") {
					auto V = Read<ExtensionPropertyDto>(Raw.str, "ExtensionPropertyUpdate record"); if (!V) return std::unexpected(V.error()); auto Id = SchemaId::Parse(V->ExtensionSchemaId); auto *Extension = Id ? GetActiveRuntimeSchemaRegistry().FindExtensionById(*Id) : nullptr; auto *Property = Id ? GetActiveRuntimeSchemaRegistry().FindExtensionProperty(*Id, V->PropertyName) : nullptr; auto W = DecodeWire(V->Value.str); ValidateProtocolString(V->PropertyName, MaximumProtocolIdentifierBytes, "Extension property name"); if (!Id || Id->ToString() != V->ExtensionSchemaId || !Extension || !Property || !W || Extension->DefinitionVersion != V->DefinitionVersion) throw std::invalid_argument("ExtensionPropertyUpdate identity is invalid"); (void)ValidateSchemaExtensionPropertyValue(Property->Type, *W); Record.Operation = WireJournalOperation::ExtensionPropertyUpdate; Record.ExtensionSchemaId = *Id; Record.DefinitionVersion = V->DefinitionVersion; Record.ExtensionPropertyName = V->PropertyName; Record.Value = std::move(*W);
				} else if (HeaderValue->Operation == "TagAdded" || HeaderValue->Operation == "TagRemoved") {
					auto V = Read<TagDto>(Raw.str, "Tag record"); if (!V) return std::unexpected(V.error()); ValidateTagName(V->TagName); Record.Operation = HeaderValue->Operation == "TagAdded" ? WireJournalOperation::TagAdded : WireJournalOperation::TagRemoved; Record.TagName = V->TagName;
				} else if (HeaderValue->Operation == "Reparent") {
					auto V = Read<ReparentDto>(Raw.str, "Reparent record"); if (!V) return std::unexpected(V.error()); auto Parent = DecodeNullableId(V->ParentId.str); if (!Parent) return std::unexpected(Parent.error()); Record.Operation = WireJournalOperation::Reparent; Record.Parent = std::move(*Parent);
				} else if (HeaderValue->Operation == "Destroy") {
					auto V = Read<DestroyDto>(Raw.str, "Destroy record"); if (!V) return std::unexpected(V.error()); Record.Operation = WireJournalOperation::Destroy;
				} else throw std::invalid_argument("Unknown journal operation");
				Result.push_back(std::move(Record));
			}
			return Result;
		} catch (const std::exception &Error) { return SerializationFailure(SerializationErrorCode::InvalidValue, "WireJournal semantic validation failed", "$", Error.what()); }
	}
}
