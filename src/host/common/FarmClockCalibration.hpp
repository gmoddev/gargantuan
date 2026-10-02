#pragma once

#include "../../network/GnsServiceDiagnostics.hpp"
#include "gargantuan/network/RemoteProtocol.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace gargantuan::host {
	// Farm-only, borrowed Main-thread GNS observation. It changes no transport
	// policy and retains no payload. Only the existing ScaleFunction echo's
	// clock:<epoch>:<sequence> arguments are captured outside measured phases.
	class FarmClockCalibration final {
	  public:
		struct Record {
			network::ConnectionId Connection;
			std::uint64_t Request = 0;
			std::uint64_t Nanoseconds = 0;
			std::uint32_t Bytes = 0;
			int Epoch = 0, Sequence = 0, Kind = 0, Result = -1;
			const char *Stage = "";
		};
		static constexpr std::size_t MaximumRecords = 4096;

		FarmClockCalibration(std::string RunIdValue, const char *RoleValue, int SlotValue)
			: RunId(std::move(RunIdValue)), Role(RoleValue), Slot(SlotValue) {
			Sink.Context = this;
			Sink.Record = Observe;
		}
		~FarmClockCalibration() {
			SetActive(false);
			Dump();
		}
		FarmClockCalibration(const FarmClockCalibration &) = delete;
		FarmClockCalibration &operator=(const FarmClockCalibration &) = delete;

		void SetTarget(ObjectId Value) noexcept { Target = Value; }
		void SetEpoch(int Value) noexcept { CurrentEpoch = Value; }
		void SetActive(bool Value) noexcept {
			if (Value == Active) return;
			if (Value && std::strcmp(Role, "client") == 0 && CurrentEpoch <= CompletedEpoch) return;
			if (Value && Target.IsValid()) {
				Previous = network::detail::ActiveGnsService;
				network::detail::ActiveGnsService = &Sink;
				Active = true;
			} else if (!Value && Active) {
				network::detail::ActiveGnsService = Previous;
				Previous = nullptr;
				Active = false;
			}
		}
		[[nodiscard]] bool IsActive() const noexcept { return Active; }
		[[nodiscard]] std::size_t Count() const noexcept { return Used; }
		[[nodiscard]] std::uint64_t Dropped() const noexcept { return Overflow; }
		[[nodiscard]] const Record &At(std::size_t Index) const noexcept { return Records[Index]; }

		void Capture(network::detail::GnsServiceRecord Value, std::span<const std::byte> Payload) noexcept {
			if (!Target.IsValid() ||
				(std::strcmp(Value.Stage, "GnsBefore") != 0 &&
				 std::strcmp(Value.Stage, "GnsQueued") != 0 &&
				 std::strcmp(Value.Stage, "GnsReceive") != 0) ||
				Payload.size() < 52 || std::memcmp(Payload.data(), "GRMT", 4) != 0)
				return;
			auto Message = network::DecodeRemoteMessage(Payload);
			if (!Message || Message->Remote != Target ||
				(Message->Kind != network::RemoteMessageKind::Request &&
				 Message->Kind != network::RemoteMessageKind::Response) ||
				Message->Arguments.size() != 1)
				return;
			const auto *Marker = std::get_if<std::string>(&Message->Arguments.front());
			if (!Marker) return;
			int Epoch = 0, Sequence = 0;
			if (!ParseMarker(*Marker, Epoch, Sequence)) return;
			if (Used == MaximumRecords) {
				if (Overflow != std::numeric_limits<std::uint64_t>::max()) ++Overflow;
				return;
			}
			Records[Used++] = {.Connection = Value.Connection,
				.Request = Message->Request.Value(), .Nanoseconds = Value.Nanoseconds,
				.Bytes = Value.Bytes, .Epoch = Epoch, .Sequence = Sequence,
				.Kind = static_cast<int>(Message->Kind), .Result = Value.Result,
				.Stage = Value.Stage};
			// The fourth reply's native receive closes this client's calibration
			// trace before a phase packet could arrive, even if the replicated
			// active=false marker has not reached the client yet.
			if (std::strcmp(Role, "client") == 0 && Epoch == CurrentEpoch &&
				Sequence == 4 && Message->Kind == network::RemoteMessageKind::Response &&
				std::strcmp(Value.Stage, "GnsReceive") == 0) {
				CompletedEpoch = Epoch;
				SetActive(false);
			}
		}

	  private:
		static bool ParseMarker(std::string_view Marker, int &Epoch, int &Sequence) noexcept {
			if (!Marker.starts_with("clock:")) return false;
			Marker.remove_prefix(6);
			if (Marker.size() != 3 || Marker[1] != ':' ||
				Marker[0] < '1' || Marker[0] > '5' ||
				Marker[2] < '1' || Marker[2] > '4') return false;
			Epoch = Marker[0] - '0';
			Sequence = Marker[2] - '0';
			return true;
		}
		static void Observe(void *Context, network::detail::GnsServiceRecord Value,
			std::span<const std::byte> Payload) noexcept {
			static_cast<FarmClockCalibration *>(Context)->Capture(Value, Payload);
		}
		void Dump() const noexcept {
			for (std::size_t Index = 0; Index < Used; ++Index) {
				const auto &Value = Records[Index];
				std::printf("[Qualification:FarmClock] event=native run=%s role=%s slot=%d epoch=%d sequence=%d request=%llu connection_slot=%u connection_generation=%u stage=%s monotonic_ns=%llu kind=%d bytes=%u result=%d\n",
					RunId.c_str(), Role, Slot, Value.Epoch, Value.Sequence,
					static_cast<unsigned long long>(Value.Request),
					Value.Connection.Slot, Value.Connection.Generation, Value.Stage,
					static_cast<unsigned long long>(Value.Nanoseconds), Value.Kind,
					Value.Bytes, Value.Result);
			}
			std::printf("[Qualification:FarmClock] event=terminal run=%s role=%s slot=%d records=%zu overflow=%llu\n",
				RunId.c_str(), Role, Slot, Used, static_cast<unsigned long long>(Overflow));
		}

		std::string RunId;
		const char *Role;
		int Slot;
		ObjectId Target;
		std::array<Record, MaximumRecords> Records{};
		std::size_t Used = 0;
		std::uint64_t Overflow = 0;
		bool Active = false;
		int CurrentEpoch = 0, CompletedEpoch = 0;
		network::detail::GnsServiceSink Sink{};
		network::detail::GnsServiceSink *Previous = nullptr;
	};
}
