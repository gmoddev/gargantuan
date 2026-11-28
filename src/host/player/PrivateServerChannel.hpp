// Copyright (c) 2026 Team Fireworks
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "gargantuan/runtime/ApplicationSession.hpp"
#include <SDL3/SDL.h>
#include <filesystem>
#include <span>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace gargantuan::host {
	// Only the control pipes cross the process boundary. The Windows job owns
	// the child from creation and closing it also handles abrupt parent exit.
	class PrivateServerChannel final {
	  public:
		PrivateServerChannel() = default;
		PrivateServerChannel(const PrivateServerChannel &) = delete;
		PrivateServerChannel &operator=(const PrivateServerChannel &) = delete;
#if defined(_WIN32)
		void Start(const std::filesystem::path &Executable, const std::vector<const char *> &Arguments) {
			SECURITY_ATTRIBUTES Security{sizeof(Security), nullptr, TRUE};
			Handle Input, Output, Errors;
			if (!CreatePipe(&Input.Value, &WritePipe.Value, &Security, 4096) ||
				!CreatePipe(&ReadPipe.Value, &Output.Value, &Security, 4096) ||
				!SetHandleInformation(WritePipe.Value, HANDLE_FLAG_INHERIT, 0) ||
				!SetHandleInformation(ReadPipe.Value, HANDLE_FLAG_INHERIT, 0))
				throw SessionAdmissionFailure::Unavailable;
			Errors.Value = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
				&Security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			Job.Value = CreateJobObjectW(nullptr, nullptr);
			JOBOBJECT_EXTENDED_LIMIT_INFORMATION Limits{};
			Limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
			if (Errors.Value == INVALID_HANDLE_VALUE || !Job.Value ||
				!SetInformationJobObject(Job.Value, JobObjectExtendedLimitInformation, &Limits, sizeof(Limits)))
				throw SessionAdmissionFailure::Unavailable;
			SIZE_T Size = 0;
			InitializeProcThreadAttributeList(nullptr, 2, 0, &Size);
			std::vector<std::byte> Buffer(Size);
			auto *List = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(Buffer.data());
			if (!InitializeProcThreadAttributeList(List, 2, 0, &Size)) throw SessionAdmissionFailure::Unavailable;
			std::unique_ptr<_PROC_THREAD_ATTRIBUTE_LIST, decltype(&DeleteProcThreadAttributeList)> Attributes(List, DeleteProcThreadAttributeList);
			HANDLE Inherited[]{Input.Value, Output.Value, Errors.Value};
			if (!UpdateProcThreadAttribute(List, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, Inherited, sizeof(Inherited), nullptr, nullptr) ||
				!UpdateProcThreadAttribute(List, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, &Job.Value, sizeof(Job.Value), nullptr, nullptr))
				throw SessionAdmissionFailure::Unavailable;
			STARTUPINFOEXW Startup{};
			Startup.StartupInfo.cb = sizeof(Startup);
			Startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
			Startup.StartupInfo.hStdInput = Input.Value;
			Startup.StartupInfo.hStdOutput = Output.Value;
			Startup.StartupInfo.hStdError = Errors.Value;
			Startup.lpAttributeList = List;
			std::wstring Command, Environment;
			for (const char *Arg : Arguments) {
				if (!Arg) break;
				if (!Command.empty()) Command += L' ';
				Command += L'"';
				std::size_t Slashes = 0;
				for (const wchar_t C : std::filesystem::u8path(Arg).wstring()) {
					if (C == L'\\') { ++Slashes; continue; }
					Command.append(C == L'"' ? Slashes * 2 + 1 : Slashes, L'\\');
					Command += C;
					Slashes = 0;
				}
				Command.append(Slashes * 2, L'\\');
				Command += L'"';
			}
			for (const auto *Name : {L"LOCALAPPDATA", L"SystemRoot", L"TEMP", L"TMP", L"USERPROFILE"}) {
				const DWORD Count = GetEnvironmentVariableW(Name, nullptr, 0);
				if (!Count) continue;
				std::wstring Value(Count, L'\0');
				if (GetEnvironmentVariableW(Name, Value.data(), Count) != Count - 1) throw SessionAdmissionFailure::Unavailable;
				Environment.append(Name).append(L"=").append(Value);
			}
			Environment += L'\0';
			PROCESS_INFORMATION Child{};
			if (!CreateProcessW(Executable.c_str(), Command.data(), nullptr, nullptr, TRUE,
				CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT, Environment.data(),
				Executable.parent_path().c_str(), &Startup.StartupInfo, &Child)) throw SessionAdmissionFailure::Unavailable;
			Process.Value = Child.hProcess;
			CloseHandle(Child.hThread);
		}
		bool Exited(int *Code = nullptr) const {
			if (WaitForSingleObject(Process.Value, 0) != WAIT_OBJECT_0) return false;
			DWORD Exit{};
			if (Code) *Code = GetExitCodeProcess(Process.Value, &Exit) ? static_cast<int>(Exit) : -1;
			return true;
		}
		void Send(char Command) {
			DWORD Count{};
			if (!WriteFile(WritePipe.Value, &Command, 1, &Count, nullptr) || Count != 1) throw SessionAdmissionFailure::Disconnected;
		}
		std::size_t Read(std::span<std::byte> Bytes) {
			DWORD Available{}, Count{};
			if (!PeekNamedPipe(ReadPipe.Value, nullptr, 0, nullptr, &Available, nullptr)) throw SessionAdmissionFailure::Disconnected;
			if (!Available) return 0;
			if (!ReadFile(ReadPipe.Value, Bytes.data(), std::min<DWORD>(Available, static_cast<DWORD>(Bytes.size())),
					&Count, nullptr)) throw SessionAdmissionFailure::Disconnected;
			return Count;
		}
	  private:
		struct Handle {
			HANDLE Value{};
			~Handle() { if (Value && Value != INVALID_HANDLE_VALUE) CloseHandle(Value); }
		};
		Handle Job, Process, ReadPipe, WritePipe;
#else
		void Start(const std::filesystem::path &, const std::vector<const char *> &Arguments) {
			const auto Properties = SDL_CreateProperties();
			const auto Env = SDL_CreateEnvironment(false);
			bool Valid = Properties && Env;
			for (const auto *Name : {"HOME", "LANG", "TMPDIR", "XDG_DATA_HOME"}) {
				if (const char *Value = SDL_getenv(Name)) Valid &= SDL_SetEnvironmentVariable(Env, Name, Value, true);
			}
			if (Valid && SDL_SetPointerProperty(Properties, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, Env) &&
				SDL_SetPointerProperty(Properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, const_cast<const char **>(Arguments.data())) &&
				SDL_SetNumberProperty(Properties, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_APP) &&
				SDL_SetNumberProperty(Properties, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP) &&
				SDL_SetNumberProperty(Properties, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL))
				Process = SDL_CreateProcessWithProperties(Properties);
			SDL_DestroyProperties(Properties);
			SDL_DestroyEnvironment(Env);
			if (!Process) throw SessionAdmissionFailure::Unavailable;
		}
		~PrivateServerChannel() {
			if (Process) {
				if (!Exited()) (void)SDL_KillProcess(Process, true);
				SDL_DestroyProcess(Process);
			}
		}
		bool Exited(int *Code = nullptr) const { return SDL_WaitProcess(Process, false, Code); }
		void Send(char Command) {
			auto *Pipe = SDL_GetProcessInput(Process);
			if (!Pipe || SDL_WriteIO(Pipe, &Command, 1) != 1) throw SessionAdmissionFailure::Disconnected;
		}
		std::size_t Read(std::span<std::byte> Bytes) {
			auto *Pipe = SDL_GetProcessOutput(Process);
			if (!Pipe) throw SessionAdmissionFailure::Disconnected;
			const auto Count = SDL_ReadIO(Pipe, Bytes.data(), Bytes.size());
			const auto Status = SDL_GetIOStatus(Pipe);
			if (!Count && Status != SDL_IO_STATUS_READY && Status != SDL_IO_STATUS_NOT_READY)
				throw SessionAdmissionFailure::Disconnected;
			return Count;
		}
	  private:
		SDL_Process *Process{};
#endif
	};
}
