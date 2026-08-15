#pragma once

#include <expected>
#include <string>
#include <utility>

namespace gargantuan {
	enum class SerializationErrorCode {
		InvalidSyntax,
		UnsupportedVersion,
		InvalidType,
		MissingField,
		UnknownField,
		LimitExceeded,
		InvalidValue,
		TruncatedInput,
		InternalFailure,
	};

	struct SerializationError {
		SerializationErrorCode Code = SerializationErrorCode::InternalFailure;
		std::string Path;
		std::string Message;
		std::string ImplementationDiagnostic;

		[[nodiscard]] std::string Format() const;
	};

	template <class Value>
	using SerializationResult = std::expected<Value, SerializationError>;

	inline std::unexpected<SerializationError> SerializationFailure(
		SerializationErrorCode Code,
		std::string Message,
		std::string Path = {},
		std::string ImplementationDiagnostic = {}
	) {
		return std::unexpected(SerializationError{
			Code,
			std::move(Path),
			std::move(Message),
			std::move(ImplementationDiagnostic),
		});
	}
}
