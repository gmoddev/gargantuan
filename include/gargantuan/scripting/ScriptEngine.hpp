#pragma once

#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Instance.hpp"
#include "gargantuan/runtime/RuntimeMode.hpp"
#include "gargantuan/scripting/ExecutionBudget.hpp"
#include "gargantuan/scripting/ThreadEngine.hpp"

#include <Luau/Compiler.h>
#include <chrono>
#include <functional>
#include <list>
#include <lua.h>
#include <luacode.h>
#include <lualib.h>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace gargantuan {
	class IPersistenceProvider;
	class PersistenceRuntime;
	class Script;
	class ModuleScript;
	class RemoteFunction;
	class SignalConnection;
	class TweenRuntime;

	int OpenLibBase(lua_State *L);
	int OpenLibRequire(lua_State *L);
	int OpenLibTask(lua_State *L);

	int OpenLibAxes(lua_State *L);
	int OpenLibCFrame(lua_State *L);
	int OpenLibColor3(lua_State *L);
	int OpenLibColorSequence(lua_State *L);
	int OpenLibColorSequenceKeypoint(lua_State *L);
	int OpenLibEnum(lua_State *L);
	int OpenLibFaces(lua_State *L);
	int OpenLibFont(lua_State *L);
	int OpenLibInstance(lua_State *L);
	int OpenLibNumberRange(lua_State *L);
	int OpenLibNumberSequence(lua_State *L);
	int OpenLibNumberSequenceKeypoint(lua_State *L);
	int OpenLibRandom(lua_State *L);
	int OpenLibRaycastParams(lua_State *L);
	int OpenLibRect(lua_State *L);
	int OpenLibRegion3(lua_State *L);
	int OpenLibSignal(lua_State *L);
	int OpenLibTweenInfo(lua_State *L);
	int OpenLibUDim(lua_State *L);
	int OpenLibUDim2(lua_State *L);
	int OpenLibVector2(lua_State *L);
	int OpenLibVector3(lua_State *L);

	class ScriptEngine : public std::enable_shared_from_this<ScriptEngine> {
	  public:
		ScriptEngine(
			std::shared_ptr<DataModel> game,
			std::function<void(std::string, std::string)> RuntimeDiagnostic = {},
			RuntimeMode Mode = RuntimeMode::Offline
		);
		~ScriptEngine();

		lua_State *L = nullptr;
		ThreadEngine Threads;
		std::chrono::milliseconds GameplayExecutionBudget{250};
		lua_CompileOptions CompileOptions;
		std::unordered_set<std::shared_ptr<Script>> ScriptQueue;
		std::list<std::pair<ObjectId, std::weak_ptr<Script>>> ManagedScripts;

		std::shared_ptr<gargantuan::DataModel> DataModel;
		std::shared_ptr<Instance> RequireCurrentInstance = nullptr;
		std::unordered_map<std::string, std::shared_ptr<Instance>> RequirePathCache;
		std::function<void(std::string, std::string)> RuntimeDiagnostic;
		std::vector<std::weak_ptr<SignalConnection>> SignalConnections;
		std::vector<std::weak_ptr<RemoteFunction>> RemoteFunctions;
		RuntimeMode Mode = RuntimeMode::Offline;
		[[nodiscard]] bool IsStopping() const { return Stopping; }
		std::shared_ptr<Instance> FindRequiredInstanceByPath(const char *path);
		void EmitRuntimeDiagnostic(std::string Severity, std::string Message) const noexcept;
		void SetScriptFailureObserver(std::function<void(const Script &, int, std::string)> Observer);
		bool ArmRootScriptDebug(const std::shared_ptr<Script> &Target);
		void ResumeRootScriptDebug(bool Step);
		[[nodiscard]] int GetPausedRootLine() const;
		[[nodiscard]] bool HasRootDebugBudget() const;
		void TrackSignalConnection(const std::shared_ptr<SignalConnection> &Connection);
		void TrackRemoteFunction(const std::shared_ptr<RemoteFunction> &Function);
		void TrackManagedScriptThread(lua_State *Thread, const std::shared_ptr<Script> &ScriptValue);
		void UntrackManagedScriptThread(lua_State *Thread);
		void CompleteManagedScriptThreadResume(lua_State *Thread, int Status, int PreResumeStatus);
		void ShutdownRuntime();
		void ConfigurePersistence(std::shared_ptr<IPersistenceProvider> Provider);
		std::shared_ptr<PersistenceRuntime> GetPersistenceRuntime() const { return Persistence; }
		TweenRuntime &GetTweenRuntime();
		void StepTweens(float DeltaTime);
		[[nodiscard]] std::weak_ptr<Script> FindManagedScriptThread(lua_State *Thread) const;
		void RunBootstrapScript(const std::shared_ptr<Script> &ScriptValue);
		[[nodiscard]] bool CanRunScript(const Script &ScriptValue) const;

		void Step();

		static ScriptEngine *Get(lua_State *L);
		static void DumpStack(lua_State *L);

	  private:
		using ManagedScriptIterator = decltype(ManagedScripts)::iterator;
		std::unordered_map<ObjectId, ManagedScriptIterator> ManagedScriptIds;
		std::map<std::weak_ptr<Script>, ManagedScriptIterator, std::owner_less<std::weak_ptr<Script>>>
			DetachedManagedScripts;
		std::unordered_map<lua_State *, std::weak_ptr<Script>> ManagedScriptThreads;
		std::shared_ptr<TweenRuntime> Tweens;
		std::shared_ptr<PersistenceRuntime> Persistence;
		std::function<void(const Script &, int, std::string)> ObserveScriptFailure;
		struct RootDebugState {
			std::weak_ptr<Script> Target;
			GameplayResumeBudget Budget;
			ScriptSecurityContext Security;
			int Line = 0;
		};
		std::optional<RootDebugState> RootDebug;
		ManagedScriptIterator ManagedScriptPruneCursor = ManagedScripts.end();
		void TrackManagedScript(const std::shared_ptr<Script> &ScriptValue);
		void PruneExpiredManagedScripts();
		void ReportScriptFailure(Script &Failed, int Line);
		bool Stopping = false;
	};
}
