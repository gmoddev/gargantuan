#include "gargantuan/network/ReplicationProtocol.hpp"

#include "gargantuan/network/BinaryCodec.hpp"
#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"
#include "gargantuan/runtime/AttributeValidation.hpp"
#include "gargantuan/runtime/ProtocolInput.hpp"
#include "gargantuan/runtime/TagIndex.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <new>
#include <set>
#include <type_traits>

namespace gargantuan::network {
	namespace {
		constexpr std::uint32_t ReplicationMagic = 0x4c505247; // "GRPL" in little endian.
		constexpr std::size_t ReplicationHeaderBytes = 36;
		constexpr std::size_t ReplicationSchemaEntryBytes = 21;
		constexpr std::size_t MinimumReplicationOperationBytes = 9;

		using Writer = GameBinaryWriter;
		class Reader final : public GameBinaryReader {
		  public:
			explicit Reader(std::span<const std::byte> Input) : GameBinaryReader(Input, "Replication frame") {}
		};

		void WriteObjectId(Writer &Output, ObjectId Id) {
			WriteBinaryObjectId(Output, Id);
		}
		bool ReadObjectId(Reader &Input, ObjectId &Id) {
			return ReadBinaryObjectId(Input, Id);
		}
		void WriteSchemaId(Writer &Output, SchemaId Id) {
			WriteBinarySchemaId(Output, Id);
		}
		bool ReadSchemaId(Reader &Input, SchemaId &Id) {
			return ReadBinarySchemaId(Input, Id);
		}
		void WriteOptionalObjectId(Writer &Output, const std::optional<ObjectId> &Id) {
			Output.Integer<std::uint8_t>(Id ? 1 : 0);
			if (Id) WriteObjectId(Output, *Id);
		}
		bool ReadOptionalObjectId(Reader &Input, std::optional<ObjectId> &Id) {
			std::uint8_t Present;
			if (!Input.Integer(Present) || Present > 1) return Input.Fail("Replication optional marker is invalid");
			if (!Present) {
				Id.reset();
				return true;
			}
			ObjectId Value;
			if (!ReadObjectId(Input, Value)) return false;
			Id = Value;
			return true;
		}

		void WriteWireValue(Writer &Output, const WireValue &Value) {
			WriteBinaryWireValue(Output, Value);
		}

		bool ReadWireValue(Reader &Input, WireValue &Value) {
			return ReadBinaryWireValue(Input, Value, MaximumProtocolStringBytes);
		}

		void WriteValueMap(
			Writer &Output,
			const std::map<std::string, WireValue> &Values,
			const StructuralPropertyPatchList *NilProperties = nullptr
		) {
			Output.Integer(static_cast<std::uint32_t>(Values.size()));
			for (const auto &[Name, Value] : Values) {
				Output.String(Name);
				if (NilProperties && NilProperties->Contains(Name))
					WriteWireValue(Output, std::monostate{});
				else
					WriteWireValue(Output, Value);
			}
		}
		bool ReadValueMap(Reader &Input, std::map<std::string, WireValue> &Values, std::size_t MaximumCount) {
			std::uint32_t Count;
			if (!Input.Integer(Count) || Count > MaximumCount)
				return Input.Fail("Replication value map exceeds its limit");
			for (std::uint32_t Index = 0; Index < Count; ++Index) {
				std::string Name;
				WireValue Value;
				if (!Input.String(Name, MaximumProtocolIdentifierBytes) || !ReadWireValue(Input, Value)) return false;
				if (!Values.emplace(std::move(Name), std::move(Value)).second)
					return Input.Fail("Replication value map contains duplicate keys");
			}
			return true;
		}

		void WritePublish(
			Writer &Output, const PublishReplication &Value, const StructuralPropertyPatchList *NilProperties = nullptr
		) {
			WriteObjectId(Output, Value.Object);
			WriteSchemaId(Output, Value.ClassSchemaId);
			Output.Integer(Value.DefinitionVersion);
			WriteOptionalObjectId(Output, Value.Parent);
			Output.String(Value.ClassName);
			Output.String(Value.Name);
			WriteValueMap(Output, Value.Properties, NilProperties);
			WriteValueMap(Output, Value.Attributes);
			Output.Integer(static_cast<std::uint32_t>(Value.Extensions.size()));
			for (const auto &State : Value.Extensions) {
				WriteSchemaId(Output, State.ExtensionSchemaId);
				Output.Integer(State.DefinitionVersion);
				WriteValueMap(Output, State.Properties);
			}
			Output.Integer(static_cast<std::uint32_t>(Value.CustomProperties.size()));
			for (const auto &State : Value.CustomProperties) {
				WriteSchemaId(Output, State.DeclaringClassSchemaId);
				Output.Integer(State.DefinitionVersion);
				WriteValueMap(Output, State.Properties);
			}
			Output.Integer(static_cast<std::uint32_t>(Value.Tags.size()));
			for (const auto &Tag : Value.Tags)
				Output.String(Tag);
		}
		bool ReadPublish(Reader &Input, PublishReplication &Value) {
			if (!ReadObjectId(Input, Value.Object) || !ReadSchemaId(Input, Value.ClassSchemaId) ||
				!Input.Integer(Value.DefinitionVersion) || Value.DefinitionVersion == 0 ||
				!ReadOptionalObjectId(Input, Value.Parent) ||
				!Input.String(Value.ClassName, MaximumProtocolIdentifierBytes) ||
				!Input.String(Value.Name, MaximumProtocolStringBytes) ||
				!ReadValueMap(Input, Value.Properties, MaximumSnapshotPropertiesPerObject) ||
				!ReadValueMap(Input, Value.Attributes, MaximumAttributesPerInstance))
				return false;
			std::uint32_t Count;
			if (!Input.Integer(Count) || Count > MaximumCustomExtensionDefinitions)
				return Input.Fail("Replication extension state exceeds its limit");
			Value.Extensions.reserve(Count);
			for (std::uint32_t Index = 0; Index < Count; ++Index) {
				SnapshotExtensionState State;
				if (!ReadSchemaId(Input, State.ExtensionSchemaId) || !Input.Integer(State.DefinitionVersion) ||
					State.DefinitionVersion == 0 ||
					!ReadValueMap(Input, State.Properties, MaximumExtensionOverridesPerInstance))
					return false;
				Value.Extensions.push_back(std::move(State));
			}
			if (!Input.Integer(Count) || Count > MaximumCustomClassDefinitions)
				return Input.Fail("Replication custom state exceeds its limit");
			Value.CustomProperties.reserve(Count);
			for (std::uint32_t Index = 0; Index < Count; ++Index) {
				SnapshotCustomClassState State;
				if (!ReadSchemaId(Input, State.DeclaringClassSchemaId) || !Input.Integer(State.DefinitionVersion) ||
					State.DefinitionVersion == 0 ||
					!ReadValueMap(Input, State.Properties, MaximumCustomPropertyOverridesPerInstance))
					return false;
				Value.CustomProperties.push_back(std::move(State));
			}
			if (!Input.Integer(Count) || Count > MaximumTagsPerInstance)
				return Input.Fail("Replication tags exceed their limit");
			Value.Tags.reserve(Count);
			std::set<std::string> UniqueTags;
			for (std::uint32_t Index = 0; Index < Count; ++Index) {
				std::string Tag;
				if (!Input.String(Tag, MaximumTagNameBytes) || !UniqueTags.insert(Tag).second)
					return Input.Fail("Replication tags are invalid or duplicated");
				Value.Tags.push_back(std::move(Tag));
			}
			return true;
		}

		enum class OperationCode : std::uint8_t {
			Publish,
			Property,
			Extension,
			Reparent,
			Attribute,
			TagAdded,
			TagRemoved,
			Unpublish,
			Destroy
		};

		void WriteOperation(Writer &Output, const ReplicationOperation &Operation) {
			std::visit(
				[&](const auto &Value) {
					using Type = std::decay_t<decltype(Value)>;
					if constexpr (std::is_same_v<Type, PublishReplication>) {
						Output.Integer(static_cast<std::uint8_t>(OperationCode::Publish));
						WritePublish(Output, Value);
					} else if constexpr (std::is_same_v<Type, PreparedPublishReplication>) {
						Output.Integer(static_cast<std::uint8_t>(OperationCode::Publish));
						WritePublish(Output, Value.Template->Publication, &Value.NilProperties);
					} else if constexpr (std::is_same_v<Type, PropertyReplicationUpdate>) {
						Output.Integer(static_cast<std::uint8_t>(OperationCode::Property));
						WriteObjectId(Output, Value.Object);
						Output.String(Value.PropertyName);
						WriteWireValue(Output, Value.Value);
						Output.Integer<std::uint8_t>(Value.DeclaringClassSchemaId ? 1 : 0);
						if (Value.DeclaringClassSchemaId) {
							WriteSchemaId(Output, *Value.DeclaringClassSchemaId);
							Output.Integer(*Value.DefinitionVersion);
						}
					} else if constexpr (std::is_same_v<Type, ExtensionPropertyReplicationUpdate>) {
						Output.Integer(static_cast<std::uint8_t>(OperationCode::Extension));
						WriteObjectId(Output, Value.Object);
						WriteSchemaId(Output, Value.ExtensionSchemaId);
						Output.Integer(Value.DefinitionVersion);
						Output.String(Value.PropertyName);
						WriteWireValue(Output, Value.Value);
					} else if constexpr (std::is_same_v<Type, ReparentReplication>) {
						Output.Integer(static_cast<std::uint8_t>(OperationCode::Reparent));
						WriteObjectId(Output, Value.Object);
						WriteOptionalObjectId(Output, Value.Parent);
					} else if constexpr (std::is_same_v<Type, AttributeReplicationUpdate>) {
						Output.Integer(static_cast<std::uint8_t>(OperationCode::Attribute));
						WriteObjectId(Output, Value.Object);
						Output.String(Value.AttributeName);
						Output.Integer<std::uint8_t>(Value.Value ? 1 : 0);
						if (Value.Value) WriteWireValue(Output, *Value.Value);
					} else if constexpr (std::is_same_v<Type, TagAddedReplication>) {
						Output.Integer(static_cast<std::uint8_t>(OperationCode::TagAdded));
						WriteObjectId(Output, Value.Object);
						Output.String(Value.TagName);
					} else if constexpr (std::is_same_v<Type, TagRemovedReplication>) {
						Output.Integer(static_cast<std::uint8_t>(OperationCode::TagRemoved));
						WriteObjectId(Output, Value.Object);
						Output.String(Value.TagName);
					} else if constexpr (std::is_same_v<Type, UnpublishReplication>) {
						Output.Integer(static_cast<std::uint8_t>(OperationCode::Unpublish));
						WriteObjectId(Output, Value.Object);
					} else {
						Output.Integer(static_cast<std::uint8_t>(OperationCode::Destroy));
						WriteObjectId(Output, Value.Object);
					}
				},
				Operation.Intent
			);
		}

		bool ReadOperation(Reader &Input, ReplicationEpoch Epoch, ReplicationOperation &Operation) {
			std::uint8_t Code;
			if (!Input.Integer(Code) || Code > static_cast<std::uint8_t>(OperationCode::Destroy))
				return Input.Fail("Replication opcode is invalid");
			Operation.Epoch = Epoch;
			switch (static_cast<OperationCode>(Code)) {
			case OperationCode::Publish: {
				PublishReplication V;
				if (!ReadPublish(Input, V)) return false;
				Operation.Intent = std::move(V);
				break;
			}
			case OperationCode::Property: {
				PropertyReplicationUpdate V;
				std::uint8_t HasSchema;
				if (!ReadObjectId(Input, V.Object) || !Input.String(V.PropertyName, MaximumProtocolIdentifierBytes) ||
					!ReadWireValue(Input, V.Value) || !Input.Integer(HasSchema) || HasSchema > 1)
					return Input.Fail("Replication property operation is invalid");
				if (HasSchema) {
					SchemaId Id;
					std::uint32_t Version;
					if (!ReadSchemaId(Input, Id) || !Input.Integer(Version) || Version == 0)
						return Input.Fail("Replication property schema is invalid");
					V.DeclaringClassSchemaId = Id;
					V.DefinitionVersion = Version;
				}
				Operation.Intent = std::move(V);
				break;
			}
			case OperationCode::Extension: {
				ExtensionPropertyReplicationUpdate V;
				if (!ReadObjectId(Input, V.Object) || !ReadSchemaId(Input, V.ExtensionSchemaId) ||
					!Input.Integer(V.DefinitionVersion) || V.DefinitionVersion == 0 ||
					!Input.String(V.PropertyName, MaximumProtocolIdentifierBytes) || !ReadWireValue(Input, V.Value))
					return false;
				Operation.Intent = std::move(V);
				break;
			}
			case OperationCode::Reparent: {
				ReparentReplication V;
				if (!ReadObjectId(Input, V.Object) || !ReadOptionalObjectId(Input, V.Parent)) return false;
				Operation.Intent = V;
				break;
			}
			case OperationCode::Attribute: {
				AttributeReplicationUpdate V;
				std::uint8_t HasValue;
				if (!ReadObjectId(Input, V.Object) || !Input.String(V.AttributeName, MaximumAttributeNameBytes) ||
					!Input.Integer(HasValue) || HasValue > 1)
					return Input.Fail("Replication attribute operation is invalid");
				if (HasValue) {
					WireValue W;
					if (!ReadWireValue(Input, W)) return false;
					V.Value = std::move(W);
				}
				Operation.Intent = std::move(V);
				break;
			}
			case OperationCode::TagAdded: {
				TagAddedReplication V;
				if (!ReadObjectId(Input, V.Object) || !Input.String(V.TagName, MaximumTagNameBytes)) return false;
				Operation.Intent = std::move(V);
				break;
			}
			case OperationCode::TagRemoved: {
				TagRemovedReplication V;
				if (!ReadObjectId(Input, V.Object) || !Input.String(V.TagName, MaximumTagNameBytes)) return false;
				Operation.Intent = std::move(V);
				break;
			}
			case OperationCode::Unpublish: {
				UnpublishReplication V;
				if (!ReadObjectId(Input, V.Object)) return false;
				Operation.Intent = V;
				break;
			}
			case OperationCode::Destroy: {
				DestroyReplication V;
				if (!ReadObjectId(Input, V.Object)) return false;
				Operation.Intent = V;
				break;
			}
			}
			return Operation.IsValid() || Input.Fail("Replication operation failed semantic validation");
		}
	}

	bool ReplicationFrame::IsValid() const {
		if (Version != ReplicationProtocolVersion || !Epoch.IsValid() || !Sequence.IsValid() ||
			Kind > ReplicationMessageKind::Incremental || Operations.size() > MaximumReplicationOperationsPerFrame ||
			Schema.size() > MaximumReplicationSchemaDefinitions)
			return false;
		if (Kind == ReplicationMessageKind::Baseline && Schema.empty()) return false;
		if (Kind == ReplicationMessageKind::Incremental && !Schema.empty()) return false;
		std::set<SchemaId> UniqueSchemaIds;
		for (const auto &Entry : Schema)
			if (!Entry.Id.IsValid() || Entry.DefinitionVersion == 0 || Entry.Kind > SchemaDefinitionKind::Extension ||
				!UniqueSchemaIds.insert(Entry.Id).second)
				return false;
		for (const auto &Operation : Operations)
			if (Operation.Epoch != Epoch || !Operation.IsValid()) return false;
		return true;
	}

	std::vector<SchemaCompatibilityEntry> CaptureReplicationSchemaCompatibility() {
		std::vector<SchemaCompatibilityEntry> Result;
		for (const auto *Definition : GetActiveRuntimeSchemaRegistry().EnumerateDefinitions())
			Result.push_back(
				{GetSchemaDefinitionId(*Definition),
				 GetSchemaDefinitionVersion(*Definition),
				 GetSchemaDefinitionKind(*Definition)}
			);
		std::sort(Result.begin(), Result.end(), [](const auto &Left, const auto &Right) { return Left.Id < Right.Id; });
		return Result;
	}

	bool IsReplicationSchemaCompatible(std::span<const SchemaCompatibilityEntry> Remote) {
		const auto Local = CaptureReplicationSchemaCompatibility();
		return std::ranges::equal(Local, Remote);
	}

	SerializationResult<std::vector<std::byte>> EncodeReplicationFrame(const ReplicationFrame &Frame) {
		try {
			if (!Frame.IsValid())
				return SerializationFailure(SerializationErrorCode::InvalidValue, "Replication frame is invalid");
			Writer Payload(MaximumReplicationFrameBytes - ReplicationHeaderBytes);
			for (const auto &Entry : Frame.Schema) {
				WriteSchemaId(Payload, Entry.Id);
				Payload.Integer(Entry.DefinitionVersion);
				Payload.Integer(static_cast<std::uint8_t>(Entry.Kind));
			}
			for (const auto &Operation : Frame.Operations)
				WriteOperation(Payload, Operation);
			if (!Payload.Succeeded())
				return SerializationFailure(
					SerializationErrorCode::LimitExceeded, "Replication frame exceeds its byte limit"
				);
			Writer Output(MaximumReplicationFrameBytes);
			Output.Integer(ReplicationMagic);
			Output.Integer(Frame.Version);
			Output.Integer(static_cast<std::uint8_t>(Frame.Kind));
			Output.Integer<std::uint8_t>(0);
			Output.Integer(Frame.Epoch.Value());
			Output.Integer(Frame.Sequence.Value());
			Output.Integer(static_cast<std::uint32_t>(Frame.Schema.size()));
			Output.Integer(static_cast<std::uint32_t>(Frame.Operations.size()));
			Output.Integer(static_cast<std::uint32_t>(Payload.Bytes.size()));
			Output.Bytes.insert(Output.Bytes.end(), Payload.Bytes.begin(), Payload.Bytes.end());
			return Output.Bytes;
		} catch (const std::bad_alloc &) {
			return SerializationFailure(
				SerializationErrorCode::LimitExceeded, "Replication frame allocation exceeded available resources"
			);
		}
	}

	SerializationResult<ReplicationFrame> DecodeReplicationFrame(std::span<const std::byte> Bytes) {
		try {
			if (Bytes.size() > MaximumReplicationFrameBytes)
				return SerializationFailure(
					SerializationErrorCode::LimitExceeded, "Replication frame exceeds its byte limit"
				);
			Reader Input(Bytes);
			std::uint32_t Magic, SchemaCount, OperationCount, PayloadBytes;
			std::uint16_t Version;
			std::uint8_t Kind, Reserved;
			std::uint64_t Epoch, Sequence;
			if (!Input.Integer(Magic) || !Input.Integer(Version) || !Input.Integer(Kind) || !Input.Integer(Reserved) ||
				!Input.Integer(Epoch) || !Input.Integer(Sequence) || !Input.Integer(SchemaCount) ||
				!Input.Integer(OperationCount) || !Input.Integer(PayloadBytes))
				return SerializationFailure(SerializationErrorCode::TruncatedInput, Input.Error);
			if (Magic != ReplicationMagic)
				return SerializationFailure(
					SerializationErrorCode::InvalidSyntax, "Replication frame magic is invalid"
				);
			if (Version != ReplicationProtocolVersion)
				return SerializationFailure(
					SerializationErrorCode::UnsupportedVersion, "Replication protocol version is unsupported"
				);
			if (Kind > static_cast<std::uint8_t>(ReplicationMessageKind::Incremental) || Reserved != 0)
				return SerializationFailure(
					SerializationErrorCode::InvalidValue, "Replication frame header is invalid"
				);
			if (SchemaCount > MaximumReplicationSchemaDefinitions ||
				OperationCount > MaximumReplicationOperationsPerFrame)
				return SerializationFailure(
					SerializationErrorCode::LimitExceeded, "Replication frame count exceeds its limit"
				);
			if (PayloadBytes != Input.Remaining())
				return SerializationFailure(
					SerializationErrorCode::TruncatedInput, "Replication payload length does not match the frame"
				);
			if (SchemaCount > Input.Remaining() / ReplicationSchemaEntryBytes)
				return SerializationFailure(
					SerializationErrorCode::TruncatedInput, "Replication schema count cannot fit in the payload"
				);
			const auto BytesAfterSchema = Input.Remaining() - SchemaCount * ReplicationSchemaEntryBytes;
			if (OperationCount > BytesAfterSchema / MinimumReplicationOperationBytes)
				return SerializationFailure(
					SerializationErrorCode::TruncatedInput, "Replication operation count cannot fit in the payload"
				);
			ReplicationFrame Frame{
				Version,
				static_cast<ReplicationMessageKind>(Kind),
				ReplicationEpoch(Epoch),
				ReliableReplicationSequence(Sequence)
			};
			Frame.Schema.reserve(SchemaCount);
			for (std::uint32_t Index = 0; Index < SchemaCount; ++Index) {
				SchemaCompatibilityEntry Entry;
				std::uint8_t DefinitionKind;
				if (!ReadSchemaId(Input, Entry.Id) || !Input.Integer(Entry.DefinitionVersion) ||
					Entry.DefinitionVersion == 0 || !Input.Integer(DefinitionKind) ||
					DefinitionKind > static_cast<std::uint8_t>(SchemaDefinitionKind::Extension))
					return SerializationFailure(
						SerializationErrorCode::InvalidValue,
						Input.Error.empty() ? "Replication schema manifest is invalid" : Input.Error
					);
				Entry.Kind = static_cast<SchemaDefinitionKind>(DefinitionKind);
				Frame.Schema.push_back(Entry);
			}
			Frame.Operations.reserve(OperationCount);
			for (std::uint32_t Index = 0; Index < OperationCount; ++Index) {
				ReplicationOperation Operation;
				if (!ReadOperation(Input, Frame.Epoch, Operation))
					return SerializationFailure(SerializationErrorCode::InvalidValue, Input.Error);
				Frame.Operations.push_back(std::move(Operation));
			}
			if (!Input.Complete() || !Frame.IsValid())
				return SerializationFailure(
					SerializationErrorCode::InvalidValue, "Replication frame contains trailing or invalid data"
				);
			return Frame;
		} catch (const std::bad_alloc &) {
			return SerializationFailure(
				SerializationErrorCode::LimitExceeded, "Replication frame allocation exceeded available resources"
			);
		}
	}
}
