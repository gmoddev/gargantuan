#include "serialization/JsonCodec.hpp"

#include "gargantuan/runtime/ProtocolInput.hpp"

#include <exception>
#include <stdexcept>

namespace gargantuan::JsonCodec {
	namespace {
		SerializationErrorCode ClassifyValidationError(std::string_view Message) {
			if (Message.find("limit") != std::string_view::npos ||
				Message.find("length") != std::string_view::npos)
				return SerializationErrorCode::LimitExceeded;
			return SerializationErrorCode::InvalidValue;
		}
	}

	SerializationResult<Json> Parse(
		std::string_view Encoded,
		std::size_t MaximumBytes,
		std::string_view DocumentName
	) {
		try {
			ValidateProtocolJsonDocument(Encoded, MaximumBytes);
		} catch (const std::invalid_argument &Error) {
			return SerializationFailure(
				ClassifyValidationError(Error.what()),
				std::string(DocumentName) + " JSON was rejected before parsing",
				"$",
				Error.what()
			);
		}

		try {
			auto Document = Json::parse(Encoded);
			ValidateTree(Document);
			return Document;
		} catch (const nlohmann::json::parse_error &Error) {
			const std::string_view Diagnostic = Error.what();
			const auto Code = Diagnostic.find("unexpected end") != std::string_view::npos
				? SerializationErrorCode::TruncatedInput
				: SerializationErrorCode::InvalidSyntax;
			return SerializationFailure(
				Code,
				std::string(DocumentName) + " contains invalid JSON syntax",
				"$",
				Error.what()
			);
		} catch (const std::invalid_argument &Error) {
			return SerializationFailure(
				ClassifyValidationError(Error.what()),
				std::string(DocumentName) + " JSON violates protocol validation",
				"$",
				Error.what()
			);
		} catch (const nlohmann::json::exception &Error) {
			return SerializationFailure(
				SerializationErrorCode::InvalidType,
				std::string(DocumentName) + " JSON has an invalid structural type",
				"$",
				Error.what()
			);
		} catch (const std::exception &Error) {
			return SerializationFailure(
				SerializationErrorCode::InternalFailure,
				std::string(DocumentName) + " JSON parsing failed",
				"$",
				Error.what()
			);
		}
	}

	SerializationResult<std::string> Encode(const Json &Value, std::string_view DocumentName) {
		try {
			return Value.dump();
		} catch (const nlohmann::json::exception &Error) {
			return SerializationFailure(
				SerializationErrorCode::InternalFailure,
				std::string(DocumentName) + " JSON encoding failed",
				"$",
				Error.what()
			);
		} catch (const std::exception &Error) {
			return SerializationFailure(
				SerializationErrorCode::InternalFailure,
				std::string(DocumentName) + " JSON encoding failed",
				"$",
				Error.what()
			);
		}
	}
}
