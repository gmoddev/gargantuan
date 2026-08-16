#include "gargantuan/network/BinaryCodec.hpp"

#include "gargantuan/runtime/ProtocolInput.hpp"

#include <new>
#include <type_traits>

namespace gargantuan::network {
	GameBinaryWriter::GameBinaryWriter(std::size_t MaximumBytes) : MaximumBytes(MaximumBytes) {}

	void GameBinaryWriter::Float(float Value) {
		Integer(std::bit_cast<std::uint32_t>(Value));
	}
	void GameBinaryWriter::Double(double Value) {
		Integer(std::bit_cast<std::uint64_t>(Value));
	}

	void GameBinaryWriter::String(std::string_view Value) {
		if (Value.size() > std::numeric_limits<std::uint32_t>::max() ||
			!CanAppend(sizeof(std::uint32_t) + Value.size()))
			return;
		Integer(static_cast<std::uint32_t>(Value.size()));
		Bytes.insert(
			Bytes.end(),
			reinterpret_cast<const std::byte *>(Value.data()),
			reinterpret_cast<const std::byte *>(Value.data() + Value.size())
		);
	}

	void GameBinaryWriter::Append(std::span<const std::byte> Value) {
		if (!CanAppend(Value.size())) return;
		Bytes.insert(Bytes.end(), Value.begin(), Value.end());
	}

	bool GameBinaryWriter::Succeeded() const {
		return !Failed;
	}

	bool GameBinaryWriter::CanAppend(std::size_t Count) {
		if (Failed || Bytes.size() > MaximumBytes || Count > MaximumBytes - Bytes.size()) {
			Failed = true;
			return false;
		}
		return true;
	}

	GameBinaryReader::GameBinaryReader(std::span<const std::byte> Input, std::string_view Context)
		: Input(Input), Context(Context) {}

	bool GameBinaryReader::Float(float &Value) {
		std::uint32_t Bits;
		if (!Integer(Bits)) return false;
		Value = std::bit_cast<float>(Bits);
		return true;
	}

	bool GameBinaryReader::Double(double &Value) {
		std::uint64_t Bits;
		if (!Integer(Bits)) return false;
		Value = std::bit_cast<double>(Bits);
		return true;
	}

	bool GameBinaryReader::String(std::string &Value, std::size_t MaximumBytes) {
		std::uint32_t Length;
		if (!Integer(Length)) return false;
		if (Length > MaximumBytes) return Fail(std::string(Context) + " string exceeds its limit");
		if (Input.size() - Position < Length) return Fail(std::string(Context) + " string is truncated");
		Value.assign(reinterpret_cast<const char *>(Input.data() + Position), Length);
		Position += Length;
		if (!IsValidProtocolUtf8(Value)) return Fail(std::string(Context) + " string is not valid UTF-8");
		return true;
	}

	bool GameBinaryReader::Fail(std::string Message) {
		if (Error.empty()) Error = std::move(Message);
		return false;
	}

	bool GameBinaryReader::Complete() const {
		return Position == Input.size();
	}
	std::size_t GameBinaryReader::Remaining() const {
		return Input.size() - Position;
	}

	void WriteBinaryObjectId(GameBinaryWriter &Output, ObjectId Id) {
		Output.Integer(Id.Slot);
		Output.Integer(Id.Generation);
	}

	bool ReadBinaryObjectId(GameBinaryReader &Input, ObjectId &Id) {
		return Input.Integer(Id.Slot) && Input.Integer(Id.Generation) &&
			   (Id.IsValid() || Input.Fail("Binary ObjectId is invalid"));
	}

	void WriteBinarySchemaId(GameBinaryWriter &Output, SchemaId Id) {
		Output.Integer(Id.High);
		Output.Integer(Id.Low);
	}

	bool ReadBinarySchemaId(GameBinaryReader &Input, SchemaId &Id) {
		return Input.Integer(Id.High) && Input.Integer(Id.Low) &&
			   (Id.IsValid() || Input.Fail("Binary SchemaId is invalid"));
	}

	void WriteBinaryWireValue(GameBinaryWriter &Output, const WireValue &Value) {
		Output.Integer(static_cast<std::uint8_t>(Value.index()));
		std::visit(
			[&](const auto &Typed) {
				using Type = std::decay_t<decltype(Typed)>;
				if constexpr (std::is_same_v<Type, std::monostate>) {
				} else if constexpr (std::is_same_v<Type, bool>)
					Output.Integer<std::uint8_t>(Typed ? 1 : 0);
				else if constexpr (std::is_same_v<Type, int>)
					Output.Integer<std::int32_t>(Typed);
				else if constexpr (std::is_same_v<Type, double>)
					Output.Double(Typed);
				else if constexpr (std::is_same_v<Type, WireFloat>)
					Output.Float(Typed.Value);
				else if constexpr (std::is_same_v<Type, std::string>)
					Output.String(Typed);
				else if constexpr (std::is_same_v<Type, WireVector2>) {
					Output.Float(Typed.X);
					Output.Float(Typed.Y);
				} else if constexpr (std::is_same_v<Type, WireVector3>) {
					Output.Float(Typed.X);
					Output.Float(Typed.Y);
					Output.Float(Typed.Z);
				} else if constexpr (std::is_same_v<Type, WireColor3>) {
					Output.Float(Typed.R);
					Output.Float(Typed.G);
					Output.Float(Typed.B);
				} else if constexpr (std::is_same_v<Type, WireUDim>) {
					Output.Float(Typed.Scale);
					Output.Integer<std::int32_t>(Typed.Offset);
				} else if constexpr (std::is_same_v<Type, WireUDim2>) {
					Output.Float(Typed.X.Scale);
					Output.Integer<std::int32_t>(Typed.X.Offset);
					Output.Float(Typed.Y.Scale);
					Output.Integer<std::int32_t>(Typed.Y.Offset);
				} else if constexpr (std::is_same_v<Type, WireCFrame>) {
					for (const auto Component : Typed.Components)
						Output.Float(Component);
				} else if constexpr (std::is_same_v<Type, WireEnumItem>) {
					Output.String(Typed.EnumType);
					Output.String(Typed.Item);
				} else if constexpr (std::is_same_v<Type, WireSchemaEnumValue>) {
					WriteBinarySchemaId(Output, Typed.EnumSchemaId);
					Output.Integer(Typed.DefinitionVersion);
					Output.Integer(Typed.ItemValue);
				} else if constexpr (std::is_same_v<Type, WireObjectReference>) {
					WriteBinaryObjectId(Output, Typed.Object.ToObjectId());
				}
			},
			Value
		);
	}

	bool ReadBinaryWireValue(GameBinaryReader &Input, WireValue &Value, std::size_t MaximumStringBytes) {
		std::uint8_t Tag;
		if (!Input.Integer(Tag) || Tag >= std::variant_size_v<WireValue>)
			return Input.Fail("Binary WireValue tag is invalid");
		switch (Tag) {
		case 0:
			Value = std::monostate{};
			break;
		case 1: {
			std::uint8_t V;
			if (!Input.Integer(V) || V > 1) return Input.Fail("Binary Boolean is invalid");
			Value = V != 0;
			break;
		}
		case 2: {
			std::int32_t V;
			if (!Input.Integer(V)) return false;
			Value = static_cast<int>(V);
			break;
		}
		case 3: {
			double V;
			if (!Input.Double(V)) return false;
			Value = V;
			break;
		}
		case 4: {
			float V;
			if (!Input.Float(V)) return false;
			Value = WireFloat{V};
			break;
		}
		case 5: {
			std::string V;
			if (!Input.String(V, MaximumStringBytes)) return false;
			Value = std::move(V);
			break;
		}
		case 6: {
			WireVector2 V;
			if (!Input.Float(V.X) || !Input.Float(V.Y)) return false;
			Value = V;
			break;
		}
		case 7: {
			WireVector3 V;
			if (!Input.Float(V.X) || !Input.Float(V.Y) || !Input.Float(V.Z)) return false;
			Value = V;
			break;
		}
		case 8: {
			WireColor3 V;
			if (!Input.Float(V.R) || !Input.Float(V.G) || !Input.Float(V.B)) return false;
			Value = V;
			break;
		}
		case 9: {
			WireUDim V;
			std::int32_t Offset;
			if (!Input.Float(V.Scale) || !Input.Integer(Offset)) return false;
			V.Offset = Offset;
			Value = V;
			break;
		}
		case 10: {
			WireUDim2 V;
			std::int32_t XOffset, YOffset;
			if (!Input.Float(V.X.Scale) || !Input.Integer(XOffset) || !Input.Float(V.Y.Scale) ||
				!Input.Integer(YOffset))
				return false;
			V.X.Offset = XOffset;
			V.Y.Offset = YOffset;
			Value = V;
			break;
		}
		case 11: {
			WireCFrame V;
			for (auto &Component : V.Components)
				if (!Input.Float(Component)) return false;
			Value = V;
			break;
		}
		case 12: {
			WireEnumItem V;
			if (!Input.String(V.EnumType, MaximumProtocolIdentifierBytes) ||
				!Input.String(V.Item, MaximumProtocolIdentifierBytes))
				return false;
			Value = std::move(V);
			break;
		}
		case 13: {
			WireSchemaEnumValue V;
			if (!ReadBinarySchemaId(Input, V.EnumSchemaId) || !Input.Integer(V.DefinitionVersion) ||
				!Input.Integer(V.ItemValue) || V.DefinitionVersion == 0)
				return Input.Fail("Binary schema enum is invalid");
			Value = V;
			break;
		}
		case 14: {
			ObjectId V;
			if (!ReadBinaryObjectId(Input, V)) return false;
			Value = WireObjectReference{WireObjectId::FromObjectId(V)};
			break;
		}
		default:
			return Input.Fail("Binary WireValue tag is unsupported");
		}
		try {
			ValidateProtocolWireValue(Value);
		} catch (const std::exception &Error) {
			return Input.Fail(Error.what());
		}
		return true;
	}
}
