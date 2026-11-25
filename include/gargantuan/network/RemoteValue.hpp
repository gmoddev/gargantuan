#pragma once

#include "gargantuan/runtime/WireValue.hpp"

#include <utility>
#include <vector>

namespace gargantuan::network {
	struct RemoteValue;
	struct RemoteDictionaryEntry;

	struct RemoteArray {
		std::vector<RemoteValue> Values;
		bool operator==(const RemoteArray &) const;
	};

	struct RemoteDictionary {
		std::vector<RemoteDictionaryEntry> Entries;
		bool operator==(const RemoteDictionary &) const;
	};

	struct RemoteBuffer {
		std::vector<std::byte> Bytes;
		bool operator==(const RemoteBuffer &) const = default;
	};

	namespace detail {
		template <class> struct RemoteValueAlternatives;
		template <class... Scalars> struct RemoteValueAlternatives<std::variant<Scalars...>> {
			using Type = std::variant<Scalars..., RemoteArray, RemoteDictionary, RemoteBuffer>;
		};
	}

	using RemoteValueStorage = detail::RemoteValueAlternatives<WireValue>::Type;

	// Compound application values are deliberately outside the reflection,
	// journal, snapshot, and structural replication WireValue domain.
	struct RemoteValue : RemoteValueStorage {
		using RemoteValueStorage::RemoteValueStorage;
		RemoteValue();
		RemoteValue(WireValue Scalar);
		bool operator==(const RemoteValue &) const;
	};

	struct RemoteDictionaryEntry {
		std::string Key;
		RemoteValue Value;
		bool operator==(const RemoteDictionaryEntry &) const = default;
	};

	inline bool RemoteArray::operator==(const RemoteArray &) const = default;
	inline bool RemoteDictionary::operator==(const RemoteDictionary &) const = default;

	inline RemoteValue::RemoteValue() = default;
	inline RemoteValue::RemoteValue(WireValue Scalar)
		: RemoteValueStorage(std::visit(
			  [](auto &&Value) -> RemoteValueStorage { return std::move(Value); }, std::move(Scalar)
		  )) {}
	inline bool RemoteValue::operator==(const RemoteValue &) const = default;
}
