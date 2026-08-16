#pragma once

#include "gargantuan/reflection/SchemaId.hpp"
#include "gargantuan/runtime/ObjectId.hpp"
#include "gargantuan/runtime/WireValue.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace gargantuan::network {
	class GameBinaryWriter {
	  public:
		explicit GameBinaryWriter(std::size_t MaximumBytes = std::numeric_limits<std::size_t>::max());

		template <typename Value> void Integer(Value Number) {
			static_assert(std::is_integral_v<Value>);
			if (!CanAppend(sizeof(Value))) return;
			using Unsigned = std::make_unsigned_t<Value>;
			auto Bits = static_cast<Unsigned>(Number);
			for (std::size_t Index = 0; Index < sizeof(Value); ++Index)
				Bytes.push_back(static_cast<std::byte>((Bits >> (Index * 8)) & 0xff));
		}

		void Float(float Value);
		void Double(double Value);
		void String(std::string_view Value);
		void Append(std::span<const std::byte> Value);
		[[nodiscard]] bool Succeeded() const;

		std::vector<std::byte> Bytes;

	  private:
		bool CanAppend(std::size_t Count);
		std::size_t MaximumBytes;
		bool Failed = false;
	};

	class GameBinaryReader {
	  public:
		explicit GameBinaryReader(std::span<const std::byte> Input, std::string_view Context = "Binary protocol");

		template <typename Value> bool Integer(Value &Number) {
			static_assert(std::is_integral_v<Value>);
			if (Input.size() - Position < sizeof(Value)) return Fail(std::string(Context) + " is truncated");
			using Unsigned = std::make_unsigned_t<Value>;
			Unsigned Bits = 0;
			for (std::size_t Index = 0; Index < sizeof(Value); ++Index)
				Bits |= static_cast<Unsigned>(std::to_integer<unsigned char>(Input[Position++])) << (Index * 8);
			Number = static_cast<Value>(Bits);
			return true;
		}

		bool Float(float &Value);
		bool Double(double &Value);
		bool String(std::string &Value, std::size_t MaximumBytes);
		bool Fail(std::string Message);
		[[nodiscard]] bool Complete() const;
		[[nodiscard]] std::size_t Remaining() const;

		std::string Error;

	  private:
		std::span<const std::byte> Input;
		std::size_t Position = 0;
		std::string Context;
	};

	void WriteBinaryObjectId(GameBinaryWriter &Output, ObjectId Id);
	bool ReadBinaryObjectId(GameBinaryReader &Input, ObjectId &Id);
	void WriteBinarySchemaId(GameBinaryWriter &Output, SchemaId Id);
	bool ReadBinarySchemaId(GameBinaryReader &Input, SchemaId &Id);
	void WriteBinaryWireValue(GameBinaryWriter &Output, const WireValue &Value);
	bool ReadBinaryWireValue(GameBinaryReader &Input, WireValue &Value, std::size_t MaximumStringBytes);
}
