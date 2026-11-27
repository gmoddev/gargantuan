// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0

#include "gargantuan/classes/Terrain.hpp"

#include "../terrain/TerrainMutation.hpp"
#include "gargantuan/classes/WorldRoot.hpp"
#include "gargantuan/datatypes/Vector3.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"
#include "gargantuan/runtime/MutationGateway.hpp"
#include "gargantuan/scripting/ScriptEngine.hpp"
#include "gargantuan/scripting/Userdata.hpp"
#include "gargantuan/terrain/Field.hpp"

#include <lua.h>

#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <format>
#include <stdexcept>
#include <string>
#include <vector>

namespace gargantuan {
	namespace {
		constexpr std::uint64_t MaximumRegionSamples = 4096;
		constexpr std::size_t EncodedSampleBytes = 6;

		const ScriptEngine &RequireReadAccess(lua_State *L, const Instance &Object) {
			const auto &Security = GetCurrentScriptSecurityContext();
			const auto *Script = ScriptEngine::Get(L);
			if (GetCurrentExecutionDomain() != ExecutionDomain::Main ||
				!Security.HasCapability(ScriptCapability::ReadDataModel) || !Script || Script->IsStopping() ||
				!Script->DataModel || Object.GetDestroyed() || Object.IsDestroying() ||
				Object.GetDataModel() != Script->DataModel)
				throw std::runtime_error("[Terrain:Access] A live terrain in the current world is required");
			return *Script;
		}

		std::shared_ptr<WorldRoot> GetWorld(const Instance &Object) {
			return std::dynamic_pointer_cast<WorldRoot>(Object.GetParent().value_or(nullptr));
		}

		bool HasEditAccess(const ScriptEngine &Script) {
			const auto &Security = GetCurrentScriptSecurityContext();
			return Script.Mode != RuntimeMode::NetworkClient && Security.Domain == ScriptExecutionDomain::Server &&
				   Security.HasCapability(ScriptCapability::MutateDataModel);
		}

		void RequireEditAccess(lua_State *L, const Instance &Object) {
			if (!HasEditAccess(RequireReadAccess(L, Object)))
				throw std::runtime_error("[Terrain:Access] An authoritative server gameplay context is required");
		}

		terrain::SamplePoint ReadEndpoint(lua_State *L, int Index, bool Exclusive) {
			const auto Point = CheckStackValue<glm::vec3>(L, Index);
			const auto Convert = [Exclusive](double Coordinate) {
				const auto Bound = terrain::MaximumSampleCoordinate;
				if (!std::isfinite(Coordinate) || std::trunc(Coordinate) != Coordinate || Coordinate < -Bound ||
					Coordinate > Bound + static_cast<int>(Exclusive))
					throw std::invalid_argument(
						"[Terrain:Read] Region endpoints require in-range integer lattice coordinates"
					);
				return static_cast<std::int64_t>(Coordinate);
			};
			return {Convert(Point.x), Convert(Point.y), Convert(Point.z)};
		}

		std::size_t GetRegionSampleCount(const terrain::SampleBox &Box) {
			const auto X = Box.MaxExclusive.X - Box.Min.X;
			const auto Y = Box.MaxExclusive.Y - Box.Min.Y;
			const auto Z = Box.MaxExclusive.Z - Box.Min.Z;
			if (X <= 0 || Y <= 0 || Z <= 0)
				throw std::invalid_argument("[Terrain:Read] The region must have positive size on every axis");
			const auto Width = static_cast<std::uint64_t>(X);
			const auto Height = static_cast<std::uint64_t>(Y);
			const auto Depth = static_cast<std::uint64_t>(Z);
			// The coordinate limit keeps the product of all three dimensions within uint64_t.
			const auto Samples = Width * Height * Depth;
			if (Samples > MaximumRegionSamples)
				throw std::length_error(std::format(
					"[Terrain:Region] Requested {} samples. Split the region into batches of at most {} samples ({} bytes per packet).",
					Samples, MaximumRegionSamples, MaximumRegionSamples * EncodedSampleBytes));
			return static_cast<std::size_t>(Samples);
		}

		void PushRevision(lua_State *L, terrain::Revision Revision) {
			const auto Text = std::to_string(Revision);
			lua_pushlstring(L, Text.data(), Text.size());
		}

		std::string_view ReadIdentityString(lua_State *L, int Index) {
			if (lua_type(L, Index) != LUA_TSTRING)
				throw std::invalid_argument("[Terrain:Edit] Expected a ticket or revision string");
			std::size_t Size = 0;
			const auto *Text = lua_tolstring(L, Index, &Size);
			return {Text, Size};
		}

		std::uint64_t ReadIdentity(std::string_view Text) {
			if (Text.empty() || Text.size() > 20 || (Text.size() > 1 && Text.front() == '0'))
				throw std::invalid_argument("[Terrain:Edit] Invalid ticket or revision string");
			std::uint64_t Value = 0;
			const auto *End = Text.data() + Text.size();
			const auto Parse = std::from_chars(Text.data(), End, Value);
			if (Parse.ec != std::errc{} || Parse.ptr != End)
				throw std::invalid_argument("[Terrain:Edit] Invalid ticket or revision string");
			return Value;
		}

		std::optional<std::uint64_t> ReadTicket(lua_State *L, const Terrain &Object) {
			const auto Text = ReadIdentityString(L, 2);
			const auto Delimiter = Text.find(':');
			if (Text.size() > 41 || Delimiter == std::string_view::npos)
				throw std::invalid_argument("[Terrain:Edit] Invalid ticket");
			const auto Lifetime = ReadIdentity(Text.substr(0, Delimiter));
			const auto Ticket = ReadIdentity(Text.substr(Delimiter + 1));
			if (Lifetime != Object.GetField().GetLifetime() || Ticket == 0) return std::nullopt;
			return Ticket;
		}

		terrain::WorldPoint ReadWorldPoint(lua_State *L, int Index) {
			const auto Point = CheckStackValue<glm::vec3>(L, Index);
			return {Point.x, Point.y, Point.z};
		}

		terrain::MaterialId ReadMaterial(lua_State *L, int Index) {
			const auto Value = CheckStackValue<double>(L, Index);
			if (!std::isfinite(Value) || std::trunc(Value) != Value || Value <= 0 || Value > UINT32_MAX)
				throw std::invalid_argument("[Terrain:Edit] Material requires a nonzero unsigned 32-bit identity");
			return static_cast<terrain::MaterialId>(Value);
		}

		int SubmitEdit(
			lua_State *L,
			const Instance &Object,
			terrain::EditRequest Request,
			std::optional<terrain::Revision> ExpectedRevision = std::nullopt
		) {
			auto Payload = std::make_shared<const terrain::TerrainMutation>(
				terrain::TerrainMutation{std::move(Request), ExpectedRevision}
			);
			MutationGateway Gateway;
			auto Result = Gateway.Apply(
				SubmitTerrainEditCommand{Object.GetObjectId(), Object.GetReplicationScopeId(), std::move(Payload)}
			);
			if (!Result.Succeeded() || !Result.TerrainTicket)
				throw std::runtime_error(
					Result.Message.empty() ? "[Terrain:Edit] Submission was rejected" : Result.Message
				);
			const auto Ticket = std::to_string(static_cast<const Terrain &>(Object).GetField().GetLifetime()) +
				":" + std::to_string(*Result.TerrainTicket);
			lua_pushlstring(L, Ticket.data(), Ticket.size());
			return 1;
		}

		int EditBall(lua_State *L, const Instance &Object, terrain::BrushMode Mode) {
			RequireEditAccess(L, Object);
			const auto Center = ReadWorldPoint(L, 2);
			const auto Radius = CheckStackValue<double>(L, 3);
			const auto Material = Mode == terrain::BrushMode::Subtract ? 0 : ReadMaterial(L, 4);
			return SubmitEdit(L, Object, terrain::SphereBrush{Center, Radius, Mode, Material});
		}

		terrain::Sample DecodeSample(const std::uint8_t *Data) {
			const auto Density = static_cast<std::uint16_t>(
				static_cast<std::uint16_t>(Data[0]) | static_cast<std::uint16_t>(Data[1]) << 8
			);
			terrain::MaterialId Material = 0;
			for (unsigned Byte = 0; Byte < 4; ++Byte)
				Material |= static_cast<terrain::MaterialId>(Data[2 + Byte]) << (8 * Byte);
			const terrain::Sample Value{std::bit_cast<std::int16_t>(Density), Material};
			if (!terrain::IsValid(Value))
				throw std::invalid_argument("[Terrain:Region] Packed sample contains invalid density or material");
			return Value;
		}

		std::vector<terrain::SampleWrite> ReadRegionWrites(lua_State *L, const terrain::SampleBox &Box, bool Clear) {
			const auto Count = GetRegionSampleCount(Box);
			const std::uint8_t *Data = nullptr;
			if (!Clear) {
				std::size_t Size = 0;
				Data = static_cast<const std::uint8_t *>(lua_tobuffer(L, 4, &Size));
				if (!Data || Size != Count * EncodedSampleBytes)
					throw std::invalid_argument("[Terrain:Region] Packed buffer length does not match the region");
			}
			std::vector<terrain::SampleWrite> Writes;
			Writes.reserve(Count);
			for (auto Z = Box.Min.Z; Z < Box.MaxExclusive.Z; ++Z)
				for (auto Y = Box.Min.Y; Y < Box.MaxExclusive.Y; ++Y)
					for (auto X = Box.Min.X; X < Box.MaxExclusive.X; ++X) {
						const auto Value = Clear ? terrain::AirSample
												 : DecodeSample(Data + Writes.size() * EncodedSampleBytes);
						Writes.push_back({{X, Y, Z}, Value});
					}
			return Writes;
		}
	}

	Terrain::Terrain() : Field(std::make_unique<terrain::Field>()) {
		Name = "Terrain";
	}

	Terrain::~Terrain() = default;

	void Terrain::RetireField() noexcept {
		Field.reset();
	}

	const terrain::Field &Terrain::GetField() const {
		if (GetCurrentExecutionDomain() != ExecutionDomain::Main || !Field || GetDestroyed() || IsDestroying())
			throw std::runtime_error("[Terrain:Access] The terrain field is unavailable");
		return *Field;
	}

	int Terrain::GetGridInfo(lua_State *L, Instance *Object) {
		const auto &Script = RequireReadAccess(L, *Object);
		const bool Available = Script.Mode != RuntimeMode::NetworkClient;
		const auto World = GetWorld(*Object);
		const bool Editable = World && World->HasTerrainRuntime() && World->TerrainEditTickets && HasEditAccess(Script);
		lua_createtable(L, 0, 6);
		if (Available) {
			const auto &Field = static_cast<Terrain *>(Object)->GetField();
			lua_pushnumber(L, Field.GetGrid().GetSpacing());
			lua_setfield(L, -2, "SampleSpacing");
			PushRevision(L, Field.GetRevision());
			lua_setfield(L, -2, "Revision");
		}
		lua_pushboolean(L, Available);
		lua_setfield(L, -2, "FieldAvailable");
		lua_pushboolean(L, Editable);
		lua_setfield(L, -2, "EditingAvailable");
		lua_pushnumber(L, static_cast<double>(terrain::MaximumSampleCoordinate));
		lua_setfield(L, -2, "MaximumSampleCoordinate");
		lua_pushnumber(L, static_cast<double>(MaximumRegionSamples));
		lua_setfield(L, -2, "MaximumRegionSamples");
		lua_setreadonly(L, -1, true);
		return 1;
	}

	int Terrain::ReadRegion(lua_State *L, Instance *Object) {
		const auto &Script = RequireReadAccess(L, *Object);
		if (Script.Mode == RuntimeMode::NetworkClient)
			throw std::runtime_error("[Terrain:Read] Authoritative terrain is unavailable in the network client");
		const terrain::SampleBox Box{ReadEndpoint(L, 2, false), ReadEndpoint(L, 3, true)};
		const auto SampleCount = GetRegionSampleCount(Box);
		const auto &Field = static_cast<Terrain *>(Object)->GetField();
		auto *Output = static_cast<std::uint8_t *>(lua_newbuffer(L, SampleCount * EncodedSampleBytes));
		const auto Revision = Field.GetRevision();
		for (auto Z = Box.Min.Z; Z < Box.MaxExclusive.Z; ++Z) {
			for (auto Y = Box.Min.Y; Y < Box.MaxExclusive.Y; ++Y) {
				for (auto X = Box.Min.X; X < Box.MaxExclusive.X; ++X) {
					const auto Sample = Field.Read({X, Y, Z});
					if (!Sample) throw std::runtime_error("[Terrain:Read] A terrain sample was unavailable");
					const auto Density = static_cast<std::uint16_t>(Sample->Density);
					*Output++ = static_cast<std::uint8_t>(Density);
					*Output++ = static_cast<std::uint8_t>(Density >> 8);
					for (unsigned Shift = 0; Shift < 32; Shift += 8)
						*Output++ = static_cast<std::uint8_t>(Sample->Material >> Shift);
				}
			}
		}
		PushRevision(L, Revision);
		return 2;
	}

	int Terrain::FillBall(lua_State *L, Instance *Object) {
		return EditBall(L, *Object, terrain::BrushMode::Add);
	}

	int Terrain::RemoveBall(lua_State *L, Instance *Object) {
		return EditBall(L, *Object, terrain::BrushMode::Subtract);
	}

	int Terrain::PaintBall(lua_State *L, Instance *Object) {
		return EditBall(L, *Object, terrain::BrushMode::Paint);
	}

	int Terrain::FillBlock(lua_State *L, Instance *Object) {
		RequireEditAccess(L, *Object);
		const auto Center = ReadWorldPoint(L, 2);
		const auto Size = ReadWorldPoint(L, 3);
		const auto Material = ReadMaterial(L, 4);
		return SubmitEdit(
			L,
			*Object,
			terrain::BoxBrush{Center, {Size.X * 0.5, Size.Y * 0.5, Size.Z * 0.5}, terrain::BrushMode::Add, Material}
		);
	}

	int Terrain::WriteRegion(lua_State *L, Instance *Object) {
		RequireEditAccess(L, *Object);
		const terrain::SampleBox Box{ReadEndpoint(L, 2, false), ReadEndpoint(L, 3, true)};
		const auto Expected = lua_isnoneornil(L, 5)
			? std::nullopt : std::optional(ReadIdentity(ReadIdentityString(L, 5)));
		return SubmitEdit(L, *Object, ReadRegionWrites(L, Box, false), Expected);
	}

	int Terrain::ClearRegion(lua_State *L, Instance *Object) {
		RequireEditAccess(L, *Object);
		const terrain::SampleBox Box{ReadEndpoint(L, 2, false), ReadEndpoint(L, 3, true)};
		return SubmitEdit(L, *Object, ReadRegionWrites(L, Box, true));
	}

	int Terrain::SmoothRegion(lua_State *L, Instance *Object) {
		RequireEditAccess(L, *Object);
		const terrain::SampleBox Box{ReadEndpoint(L, 2, false), ReadEndpoint(L, 3, true)};
		(void)GetRegionSampleCount(Box);
		const auto Strength = CheckStackValue<double>(L, 4);
		if (!std::isfinite(Strength) || Strength < 0 || Strength > 1)
			throw std::invalid_argument("[Terrain:Edit] Smooth strength must be between zero and one");
		return SubmitEdit(L, *Object, terrain::SmoothEdit{Box, static_cast<std::uint8_t>(std::lround(255 * Strength))});
	}

	int Terrain::GetEditStatus(lua_State *L, Instance *Object) {
		(void)RequireReadAccess(L, *Object);
		const auto Ticket = ReadTicket(L, *static_cast<Terrain *>(Object));
		const auto World = GetWorld(*Object);
		const auto Receipt = World && Ticket ? World->GetTerrainTicket(*Ticket) : std::nullopt;
		if (!Receipt) {
			lua_pushnil(L);
			return 1;
		}
		lua_createtable(L, 0, 4);
		lua_pushlstring(L, Receipt->Status.data(), Receipt->Status.size());
		lua_setfield(L, -2, "Status");
		if (Receipt->Revision) {
			PushRevision(L, *Receipt->Revision);
			lua_setfield(L, -2, "Revision");
		}
		lua_pushboolean(L, Receipt->Revision.has_value());
		lua_setfield(L, -2, "CollisionReady");
		if (!Receipt->Diagnostic.empty()) {
			lua_pushlstring(L, Receipt->Diagnostic.data(), Receipt->Diagnostic.size());
			lua_setfield(L, -2, "Diagnostic");
		}
		lua_setreadonly(L, -1, true);
		return 1;
	}

	int Terrain::CancelEdit(lua_State *L, Instance *Object) {
		RequireEditAccess(L, *Object);
		const auto Ticket = ReadTicket(L, *static_cast<Terrain *>(Object));
		if (!Ticket) {
			lua_pushboolean(L, false);
			return 1;
		}
		MutationGateway Gateway;
		const auto Result = Gateway.Apply(
			CancelTerrainEditCommand{Object->GetObjectId(), Object->GetReplicationScopeId(), *Ticket}
		);
		lua_pushboolean(L, Result.Succeeded());
		return 1;
	}
}
