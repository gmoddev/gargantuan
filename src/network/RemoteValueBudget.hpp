#pragma once

#include "gargantuan/network/RemoteProtocol.hpp"

namespace gargantuan::network::detail {
	inline constexpr std::size_t RemoteHeaderBytes = 52;
	inline constexpr std::size_t RemotePayloadBytes = MaximumRemoteFrameBytes - RemoteHeaderBytes;

	struct RemoteValueBudget {
		std::size_t ValueCount = 0;
		std::size_t ReferenceCount = 0;
		std::size_t EncodedBytes = 0;
		std::size_t StorageBytes = 0;

		bool Value() { return ++ValueCount <= MaximumRemoteValueCount && Wire(1); }
		bool Reference() { return ++ReferenceCount <= MaximumRemoteReferenceCount; }

		bool Wire(std::size_t Count) {
			if (Count > RemotePayloadBytes - EncodedBytes) return false;
			EncodedBytes += Count;
			return true;
		}

		bool Storage(std::size_t Count) {
			if (Count > MaximumRemoteDecodedArgumentBytes - StorageBytes) return false;
			StorageBytes += Count;
			return true;
		}

		bool Children(std::size_t Count, std::size_t EntryBytes) {
			return Count <= MaximumRemoteValueCount - ValueCount && Storage(Count * EntryBytes);
		}
	};

	// Count the scalar payload after its one-byte tag. Both the Lua snapshot and
	// native codec use this accounting before copying caller-owned data.
	template <class Type> bool ChargeRemoteScalar(RemoteValueBudget &Budget, const Type &Value) {
		if constexpr (std::is_same_v<Type, std::monostate>)
			return true;
		else if constexpr (std::is_same_v<Type, bool>)
			return Budget.Wire(1);
		else if constexpr (std::is_same_v<Type, int> || std::is_same_v<Type, WireFloat>)
			return Budget.Wire(4);
		else if constexpr (std::is_same_v<Type, double> || std::is_same_v<Type, WireVector2> ||
			std::is_same_v<Type, WireUDim>)
			return Budget.Wire(8);
		else if constexpr (std::is_same_v<Type, WireObjectReference>)
			return Budget.Reference() && Budget.Wire(8);
		else if constexpr (std::is_same_v<Type, WireVector3> || std::is_same_v<Type, WireColor3>)
			return Budget.Wire(12);
		else if constexpr (std::is_same_v<Type, WireUDim2>)
			return Budget.Wire(16);
		else if constexpr (std::is_same_v<Type, WireCFrame>)
			return Budget.Wire(48);
		else if constexpr (std::is_same_v<Type, WireSchemaEnumValue>)
			return Budget.Wire(24);
		else if constexpr (std::is_same_v<Type, WireEnumItem>)
			return Budget.Wire(8 + Value.EnumType.size() + Value.Item.size()) &&
				Budget.Storage(Value.EnumType.size() + Value.Item.size());
		else
			return Budget.Wire(4 + Value.size()) && Budget.Storage(Value.size());
	}

	template <class Visitor> bool VisitRemoteValues(const RemoteValue &Value, const Visitor &Visit) {
		if (!Visit(Value)) return false;
		if (const auto *Array = std::get_if<RemoteArray>(&Value)) {
			for (const auto &Entry : Array->Values)
				if (!VisitRemoteValues(Entry, Visit)) return false;
		} else if (const auto *Dictionary = std::get_if<RemoteDictionary>(&Value)) {
			for (const auto &Entry : Dictionary->Entries)
				if (!VisitRemoteValues(Entry.Value, Visit)) return false;
		}
		return true;
	}
}
