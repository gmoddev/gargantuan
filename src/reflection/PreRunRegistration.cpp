// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/reflection/PreRunRegistration.hpp"

#include "gargantuan/reflection/RuntimeSchemaLifecycle.hpp"

#include <Luau/Compiler.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <lua.h>
#include <luacode.h>
#include <lualib.h>
#include <unordered_set>
#include <vector>

namespace gargantuan {
	namespace {
		struct PreRunState {
			RuntimeSchemaLifecycle *Lifecycle = nullptr;
			const RuntimeSchemaBootstrapAuthority *Authority = nullptr;
			ScriptSecurityContext Security;
			std::chrono::steady_clock::time_point Deadline;
			std::size_t AllocatedBytes = 0;
			std::size_t DefinitionCount = 0;
			std::size_t PayloadBytes = 0;
			PreRunDiagnosticCode Failure = PreRunDiagnosticCode::RuntimeError;
			bool HasFailure = false;
			std::string SourceName;
			std::string CurrentDefinition;
		};

		void *BudgetAllocator(void *userData, void *pointer, std::size_t oldSize, std::size_t newSize) {
			auto &state = *static_cast<PreRunState *>(userData);
			if (newSize == 0) {
				std::free(pointer);
				state.AllocatedBytes = oldSize >= state.AllocatedBytes ? 0 : state.AllocatedBytes - oldSize;
				return nullptr;
			}
			const auto retained = oldSize >= state.AllocatedBytes ? 0 : state.AllocatedBytes - oldSize;
			if (newSize > MaximumPreRunMemoryBytes - std::min(retained, MaximumPreRunMemoryBytes)) {
				state.Failure = PreRunDiagnosticCode::MemoryBudgetExceeded;
				state.HasFailure = true;
				return nullptr;
			}
			auto *replacement = std::realloc(pointer, newSize);
			if (!replacement) {
				state.Failure = PreRunDiagnosticCode::MemoryBudgetExceeded;
				state.HasFailure = true;
				return nullptr;
			}
			state.AllocatedBytes = retained + newSize;
			return replacement;
		}

		PreRunState &GetState(lua_State *L) {
			auto *state = static_cast<PreRunState *>(lua_callbacks(L)->userdata);
			if (!state) luaL_error(L, "PreRun registration state is unavailable");
			return *state;
		}

		void Interrupt(lua_State *L, int) {
			auto &state = GetState(L);
			if (std::chrono::steady_clock::now() <= state.Deadline) return;
			state.Failure = PreRunDiagnosticCode::ExecutionBudgetExceeded;
			state.HasFailure = true;
			lua_break(L);
		}

		std::string ReadBoundedString(lua_State *L, int index, std::size_t maximum, std::string_view field) {
			if (lua_type(L, index) != LUA_TSTRING)
				luaL_error(L, "%s must be a string", std::string(field).c_str());
			std::size_t length = 0;
			const auto *value = lua_tolstring(L, index, &length);
			if (length == 0 || length > maximum)
				luaL_error(L, "%s has an invalid byte length", std::string(field).c_str());
			return {value, length};
		}

		bool IsPathWithin(const std::filesystem::path &Root, const std::filesystem::path &Candidate) {
			const auto Relative = Candidate.lexically_relative(Root);
			if (Relative.empty() || Relative.is_absolute()) return false;
			for (const auto &Component : Relative)
				if (Component == "..") return false;
			return true;
		}

		void RejectUnknownFields(lua_State *L, int tableIndex, std::initializer_list<std::string_view> allowed) {
			tableIndex = lua_absindex(L, tableIndex);
			std::size_t fields = 0;
			lua_pushnil(L);
			while (lua_next(L, tableIndex) != 0) {
				++fields;
				if (fields > allowed.size()) luaL_error(L, "registration table has unexpected fields");
				std::size_t keyLength = 0;
				const auto *key = lua_tolstring(L, -2, &keyLength);
				if (!key) luaL_error(L, "registration table keys must be strings");
				const std::string_view name(key, keyLength);
				if (std::find(allowed.begin(), allowed.end(), name) == allowed.end())
					luaL_error(L, "unknown registration field %s", std::string(name).c_str());
				lua_pop(L, 1);
			}
		}

		int RegisterEnum(lua_State *L) {
			auto &state = GetState(L);
			try {
				if (!state.Security.HasCapability(ScriptCapability::DefineSchema))
					throw std::runtime_error("Schema registration requires DefineSchema");
				luaL_checktype(L, 2, LUA_TTABLE);
				RejectUnknownFields(L, 2, {"Namespace", "Name", "Version", "Items"});
				lua_getfield(L, 2, "Namespace");
				auto schemaNamespace = ReadBoundedString(L, -1, MaximumSchemaNamespaceBytes, "Namespace");
				lua_pop(L, 1);
				lua_getfield(L, 2, "Name");
				auto name = ReadBoundedString(L, -1, MaximumSchemaDefinitionNameBytes, "Name");
				lua_pop(L, 1);
				state.CurrentDefinition = schemaNamespace + "." + name;
				lua_getfield(L, 2, "Version");
				if (lua_type(L, -1) != LUA_TNUMBER) luaL_error(L, "Version must be a positive integer");
				const auto rawVersion = lua_tonumber(L, -1);
				if (!std::isfinite(rawVersion) || std::trunc(rawVersion) != rawVersion || rawVersion < 1 ||
					rawVersion > std::numeric_limits<std::uint32_t>::max())
					luaL_error(L, "Version must be a positive 32-bit integer");
				const auto version = static_cast<std::uint32_t>(rawVersion);
				lua_pop(L, 1);
				lua_getfield(L, 2, "Items");
				luaL_checktype(L, -1, LUA_TTABLE);
				const auto itemsIndex = lua_absindex(L, -1);
				std::vector<SchemaEnumItem> items;
				items.reserve(std::min<std::size_t>(lua_objlen(L, itemsIndex), MaximumCustomEnumItems));
				lua_pushnil(L);
				while (lua_next(L, itemsIndex) != 0) {
					if (items.size() >= MaximumCustomEnumItems) luaL_error(L, "enum exceeds its item limit");
					auto itemName = ReadBoundedString(L, -2, MaximumCustomEnumItemNameBytes, "enum item name");
					if (lua_type(L, -1) != LUA_TNUMBER) luaL_error(L, "enum item value must be an integer");
					const auto rawValue = lua_tonumber(L, -1);
					if (!std::isfinite(rawValue) || std::trunc(rawValue) != rawValue ||
						rawValue < std::numeric_limits<std::int32_t>::min() || rawValue > std::numeric_limits<std::int32_t>::max())
						luaL_error(L, "enum item value must be a signed 32-bit integer");
					items.push_back({std::move(itemName), static_cast<std::int32_t>(rawValue)});
					lua_pop(L, 1);
				}
				lua_pop(L, 1);
				if (++state.DefinitionCount > MaximumCustomEnumDefinitions)
					throw std::runtime_error("PreRun exceeds its schema definition count limit");
				std::size_t payloadBytes = schemaNamespace.size() + name.size();
				for (const auto &item : items) payloadBytes += item.Name.size() + sizeof(item.Value);
				if (payloadBytes > MaximumCustomSchemaPayloadBytes - std::min(state.PayloadBytes, MaximumCustomSchemaPayloadBytes))
					throw std::runtime_error("PreRun exceeds its aggregate registration payload limit");
				state.PayloadBytes += payloadBytes;
				SchemaEnumDefinition definition{
					.Id = SchemaId::FromEnumName(schemaNamespace, name),
					.Namespace = std::move(schemaNamespace),
					.Name = std::move(name),
					.DefinitionVersion = version,
					.Provenance = SchemaProvenance::Game,
					.OriginDetail = state.SourceName,
					.Items = std::move(items),
				};
				state.Lifecycle->RegisterEnum(*state.Authority, std::move(definition), state.Security);
				return 0;
			} catch (const std::exception &exception) {
				state.Failure = PreRunDiagnosticCode::SchemaRegistrationError;
				state.HasFailure = true;
				luaL_error(L, "%s", exception.what());
				return 0;
			}
		}

		void OpenSafeLibraries(lua_State *L) {
			lua_pop(L, luaopen_base(L));
			lua_pop(L, luaopen_math(L));
			lua_pop(L, luaopen_string(L));
			lua_pop(L, luaopen_table(L));
			lua_pop(L, luaopen_utf8(L));
			lua_newtable(L);
			lua_pushcfunction(L, RegisterEnum, "Schema.RegisterEnum");
			lua_setfield(L, -2, "RegisterEnum");
			lua_setreadonly(L, -1, true);
			lua_setglobal(L, "Schema");
			luaL_sandbox(L);
		}

		[[noreturn]] void Fail(PreRunState &state, PreRunDiagnosticCode fallback, std::string message) {
			throw PreRunRegistrationError({
				.Code = state.HasFailure ? state.Failure : fallback,
				.Source = state.SourceName,
				.Definition = state.CurrentDefinition,
				.Message = std::move(message),
			});
		}
	}

	PreRunRegistrationError::PreRunRegistrationError(PreRunDiagnostic diagnostic) :
		std::runtime_error(
			std::string(GetPreRunDiagnosticCodeName(diagnostic.Code)) + " in " + diagnostic.Source +
			(diagnostic.Definition.empty() ? "" : " for " + diagnostic.Definition) + ": " + diagnostic.Message
		), Diagnostic(std::move(diagnostic)) {}

	void ExecutePreRunRegistration(
		RuntimeSchemaLifecycle &lifecycle,
		const RuntimeSchemaBootstrapAuthority &authority,
		std::string_view source,
		std::string sourceName,
		ScriptSecurityContext securityContext
	) {
		PreRunState state{
			.Lifecycle = &lifecycle,
			.Authority = &authority,
			.Security = std::move(securityContext),
			.Deadline = std::chrono::steady_clock::now() + MaximumPreRunExecutionTime,
			.SourceName = std::move(sourceName),
		};
		try {
			if (source.size() > MaximumPreRunSourceBytes)
				Fail(state, PreRunDiagnosticCode::SourceTooLarge, "source exceeds its byte limit");
			lua_State *L = lua_newstate(BudgetAllocator, &state);
			if (!L) Fail(state, PreRunDiagnosticCode::MemoryBudgetExceeded, "failed to allocate PreRun VM");
			auto close = [&] { lua_close(L); };
			try {
				lua_callbacks(L)->userdata = &state;
				lua_callbacks(L)->interrupt = Interrupt;
				OpenSafeLibraries(L);
				size_t bytecodeSize = 0;
				char *bytecode = luau_compile(source.data(), source.size(), nullptr, &bytecodeSize);
				if (!bytecode) Fail(state, PreRunDiagnosticCode::CompileError, "Luau compiler returned no bytecode");
				const auto loadStatus = luau_load(L, state.SourceName.c_str(), bytecode, bytecodeSize, 0);
				std::free(bytecode);
				if (loadStatus != LUA_OK) {
					const auto *message = lua_tostring(L, -1);
					Fail(state, PreRunDiagnosticCode::CompileError, message ? message : "Luau compilation failed");
				}
				ScriptSecurityScope securityScope(state.Security);
				const auto status = lua_pcall(L, 0, 0, 0);
				if (status != LUA_OK) {
					const auto *message = lua_tostring(L, -1);
					Fail(state, PreRunDiagnosticCode::RuntimeError, message ? message : "PreRun execution failed");
				}
				close();
			} catch (...) {
				close();
				throw;
			}
		} catch (...) {
			if (lifecycle.HasCandidate()) lifecycle.AbortCandidate(authority);
			throw;
		}
	}

	std::filesystem::path GetProjectPreRunPath(const std::filesystem::path &projectRoot) {
		return projectRoot / ".gargantuan" / "prerun.luau";
	}

	std::optional<std::string> ReadProjectPreRunSource(const std::filesystem::path &projectRoot) {
		const auto RequestedPath = GetProjectPreRunPath(projectRoot);
		try {
			const auto CanonicalRoot = std::filesystem::weakly_canonical(projectRoot);
			if (!std::filesystem::is_directory(CanonicalRoot))
				throw PreRunRegistrationError({
					PreRunDiagnosticCode::RuntimeError, RequestedPath.string(), {}, "project root is not a directory"
				});
			if (!std::filesystem::exists(RequestedPath)) return std::nullopt;
			const auto CanonicalPath = std::filesystem::canonical(RequestedPath);
			if (!IsPathWithin(CanonicalRoot, CanonicalPath))
				throw PreRunRegistrationError({
					PreRunDiagnosticCode::RuntimeError, RequestedPath.string(), {}, "source resolves outside the project root"
				});
			if (!std::filesystem::is_regular_file(CanonicalPath))
				throw PreRunRegistrationError({
					PreRunDiagnosticCode::RuntimeError, RequestedPath.string(), {}, "source is not a regular file"
				});
			const auto Size = std::filesystem::file_size(CanonicalPath);
			if (Size > MaximumPreRunSourceBytes)
				throw PreRunRegistrationError({
					PreRunDiagnosticCode::SourceTooLarge, RequestedPath.string(), {}, "source exceeds its byte limit"
				});

			std::ifstream Stream(CanonicalPath, std::ios::binary);
			if (!Stream)
				throw PreRunRegistrationError({
					PreRunDiagnosticCode::RuntimeError, RequestedPath.string(), {}, "source could not be opened"
				});
			std::string Contents(MaximumPreRunSourceBytes + 1, '\0');
			Stream.read(Contents.data(), static_cast<std::streamsize>(Contents.size()));
			const auto BytesRead = static_cast<std::size_t>(Stream.gcount());
			if (BytesRead > MaximumPreRunSourceBytes)
				throw PreRunRegistrationError({
					PreRunDiagnosticCode::SourceTooLarge, RequestedPath.string(), {}, "source exceeds its byte limit"
				});
			if (Stream.bad())
				throw PreRunRegistrationError({
					PreRunDiagnosticCode::RuntimeError, RequestedPath.string(), {}, "source could not be read"
				});
			Contents.resize(BytesRead);
			return Contents;
		} catch (const PreRunRegistrationError &) {
			throw;
		} catch (const std::filesystem::filesystem_error &Error) {
			throw PreRunRegistrationError({
				PreRunDiagnosticCode::RuntimeError, RequestedPath.string(), {},
				"source path could not be resolved: " + std::string(Error.what())
			});
		}
	}

	std::string_view GetPreRunDiagnosticCodeName(PreRunDiagnosticCode code) {
		switch (code) {
			case PreRunDiagnosticCode::SourceTooLarge: return "PreRunSourceTooLarge";
			case PreRunDiagnosticCode::CompileError: return "PreRunCompileError";
			case PreRunDiagnosticCode::ExecutionBudgetExceeded: return "PreRunBudgetExceeded";
			case PreRunDiagnosticCode::MemoryBudgetExceeded: return "PreRunMemoryBudgetExceeded";
			case PreRunDiagnosticCode::SchemaRegistrationError: return "SchemaRegistrationError";
			case PreRunDiagnosticCode::RuntimeError: return "PreRunRuntimeError";
		}
		throw std::invalid_argument("Unknown PreRun diagnostic code");
	}
}
