#include "gargantuan/classes/FileLink.hpp"

#include "gargantuan/Log.hpp"
#include "gargantuan/assets/InstanceSerialization.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/Folder.hpp"
#include "gargantuan/classes/ModuleScript.hpp"
#include "gargantuan/classes/Script.hpp"
#include "gargantuan/filesystem/Paths.hpp"

#include <algorithm>
#include <format>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace gargantuan {
	namespace {
		using CandidateResult = SourceMountResult<std::optional<std::shared_ptr<Instance>>>;

		struct SynchronizingGuard {
			bool &Synchronizing;
			~SynchronizingGuard() { Synchronizing = false; }
		};

		SourceMountError CandidateError(
			SourceMountErrorCode Code,
			const std::filesystem::path &Path,
			std::string Message
		) {
			return {Code, Path, std::move(Message)};
		}

		std::string FormatDeserializationErrors(const InstanceSerialization::DeserializationState &State) {
			std::string Result;
			const auto Count = std::min<std::size_t>(State.Errors.size(), 4);
			for (std::size_t Index = 0; Index < Count; ++Index) {
				if (!Result.empty()) Result += "; ";
				Result += State.Errors[Index];
				if (Result.size() > 1024) {
					Result.resize(1024);
					Result += "...";
					break;
				}
			}
			return Result.empty() ? "nested Instance model is invalid" : Result;
		}

		SourceMountStatus ValidateNestedInstance(
			const std::shared_ptr<Instance> &Root,
			const std::filesystem::path &Path,
			SourceMount &Mount,
			SourceMountBudget &Budget
		) {
			std::vector<std::pair<std::shared_ptr<Instance>, std::size_t>> Pending{{Root, 1}};
			std::unordered_set<const Instance *> Visited;
			while (!Pending.empty()) {
				auto [Node, Depth] = std::move(Pending.back());
				Pending.pop_back();
				if (!Node || !Visited.insert(Node.get()).second)
					return std::unexpected(CandidateError(
						SourceMountErrorCode::RecursiveMount, Path, "nested Instance model contains a hierarchy cycle"
					));
				if (auto Status = Mount.ValidateNestedInstanceDepth(Path, Depth); !Status) return Status;
				if (std::dynamic_pointer_cast<FileLink>(Node))
					return std::unexpected(CandidateError(
						SourceMountErrorCode::RecursiveMount, Path,
						"nested Instance models cannot contain FileLink mounts"
					));
				if (std::dynamic_pointer_cast<DataModel>(Node) || Node->GetDataModel())
					return std::unexpected(CandidateError(
						SourceMountErrorCode::MalformedInstance, Path,
						"nested Instance model must be a detached non-DataModel subtree"
					));
				if (auto Status = Mount.ReserveObjects(Path, 1, Budget); !Status) return Status;
				for (const auto &Child : Node->GetChildren()) Pending.emplace_back(Child, Depth + 1);
			}
			return {};
		}

		template <typename ScriptType>
		CandidateResult BuildScript(
			SourceMount &Mount,
			const SourceMountEntry &Entry,
			std::string_view Suffix,
			SourceMountBudget &Budget
		) {
			auto Source = Mount.ReadFile(Entry.RelativePath, MaximumScriptSourceBytes, Budget);
			if (!Source) return std::unexpected(Source.error());
			if (auto Status = Mount.ReserveObjects(Entry.RelativePath, 1, Budget); !Status)
				return std::unexpected(Status.error());
			auto Result = std::make_shared<ScriptType>();
			Result->SetName(Entry.Name.substr(0, Entry.Name.size() - Suffix.size()));
			Result->ChunkName = std::format("@SourceMount/{}", Paths::ToUtf8(Entry.RelativePath));
			Result->SetSource(std::move(*Source));
			return std::optional<std::shared_ptr<Instance>>(Result);
		}

		CandidateResult BuildCandidate(
			SourceMount &Mount,
			const SourceMountEntry &Entry,
			std::size_t Depth,
			SourceMountBudget &Budget
		) {
			if (Entry.Type == FileType::Directory) {
				if (auto Status = Mount.ReserveObjects(Entry.RelativePath, 1, Budget); !Status)
					return std::unexpected(Status.error());
				auto FolderValue = std::make_shared<Folder>();
				FolderValue->SetName(Entry.Name);
				auto Children = Mount.GetChildren(Entry.RelativePath, Depth, Budget);
				if (!Children) return std::unexpected(Children.error());
				for (const auto &ChildEntry : *Children) {
					auto Child = BuildCandidate(Mount, ChildEntry, Depth + 1, Budget);
					if (!Child) return std::unexpected(Child.error());
					if (*Child) (**Child)->SetParent(FolderValue);
				}
				return std::optional<std::shared_ptr<Instance>>(FolderValue);
			}

			if (Entry.Type != FileType::File) return std::optional<std::shared_ptr<Instance>>{};
			if (Entry.Name.ends_with(".instance.json")) {
				auto Contents = Mount.ReadFile(Entry.RelativePath, MaximumProtocolDocumentBytes, Budget);
				if (!Contents) return std::unexpected(Contents.error());
				std::istringstream Stream(*Contents);
				auto State = InstanceSerialization::DeserializeDetached(
					InstanceSerialization::InstanceFormat::Json, Stream
				);
				if (!State.Ok || !State.Instance)
					return std::unexpected(CandidateError(
						SourceMountErrorCode::MalformedInstance, Entry.RelativePath,
						FormatDeserializationErrors(State)
					));
				if (auto Status = ValidateNestedInstance(State.Instance, Entry.RelativePath, Mount, Budget); !Status)
					return std::unexpected(Status.error());
				return std::optional<std::shared_ptr<Instance>>(std::move(State.Instance));
			}
			if (Entry.Name.ends_with(".client.luau")) {
				auto Result = BuildScript<Script>(Mount, Entry, ".client.luau", Budget);
				if (Result && *Result) std::dynamic_pointer_cast<Script>(**Result)->SetRunContext(Enums::RunContext::Client);
				return Result;
			}
			if (Entry.Name.ends_with(".server.luau")) {
				auto Result = BuildScript<Script>(Mount, Entry, ".server.luau", Budget);
				if (Result && *Result) std::dynamic_pointer_cast<Script>(**Result)->SetRunContext(Enums::RunContext::Server);
				return Result;
			}
			if (Entry.Name.ends_with(".luau")) return BuildScript<ModuleScript>(Mount, Entry, ".luau", Budget);
			return std::optional<std::shared_ptr<Instance>>{};
		}
	}

	FileLink::FileLink() {
		GetPropertyChangedSignal("Path")->Connect([this](std::monostate) {
			LOG_DEBUG(App, "[Project:SourceMount] FileLink path changed to %s", this->GetPath().c_str());
		});
	}

	SourceMountResult<std::size_t> FileLink::Synchronize(SourceMount &Mount) {
		std::filesystem::path RelativePath;
		try {
			RelativePath = std::filesystem::u8path(GetPath());
		} catch (...) {
			return std::unexpected(CandidateError(
				SourceMountErrorCode::InvalidPath, {}, "FileLink path is not valid UTF-8"
			));
		}
		auto Parent = GetParent();
		if (!Parent)
			return std::unexpected(CandidateError(
				SourceMountErrorCode::MissingParent, RelativePath, "FileLink must have a parent before synchronization"
			));
		if (Synchronizing)
			return std::unexpected(CandidateError(
				SourceMountErrorCode::SynchronizationInProgress, RelativePath,
				"FileLink synchronization is already in progress"
			));

		Synchronizing = true;
		SynchronizingGuard Guard{Synchronizing};
		LOG_INFO(App, "[Project:SourceMount] Synchronizing FileLink '%s'", GetPath().c_str());

		SourceMountBudget Budget;
		auto Entries = Mount.GetChildren(RelativePath, 0, Budget);
		if (!Entries) return std::unexpected(Entries.error());

		std::vector<std::shared_ptr<Instance>> CandidateSiblings;
		try {
			for (const auto &Entry : *Entries) {
				auto Candidate = BuildCandidate(Mount, Entry, 1, Budget);
				if (!Candidate) return std::unexpected(Candidate.error());
				if (*Candidate) {
					(**Candidate)->SetArchivable(false);
					CandidateSiblings.push_back(std::move(**Candidate));
				}
			}
		} catch (const std::exception &Error) {
			return std::unexpected(CandidateError(
				SourceMountErrorCode::MalformedInstance, RelativePath,
				std::string("candidate construction failed: ") + Error.what()
			));
		} catch (...) {
			return std::unexpected(CandidateError(
				SourceMountErrorCode::MalformedInstance, RelativePath, "candidate construction failed"
			));
		}

		std::vector<std::shared_ptr<Instance>> Published;
		Published.reserve(CandidateSiblings.size());
		try {
			for (const auto &Candidate : CandidateSiblings) {
				Published.push_back(Candidate);
				Candidate->SetParent(*Parent);
			}
		} catch (...) {
			for (auto Iterator = Published.rbegin(); Iterator != Published.rend(); ++Iterator) {
				try {
					if (!(*Iterator)->GetDestroyed()) (*Iterator)->Destroy();
				} catch (...) {
				}
			}
			return std::unexpected(CandidateError(
				SourceMountErrorCode::CommitFailure, RelativePath,
				"candidate subtree could not be committed to the DataModel"
			));
		}

		auto Superseded = std::move(OwnedSiblings);
		OwnedSiblings = std::move(CandidateSiblings);
		for (const auto &Child : Superseded) {
			if (!Child->GetDestroyed()) Child->Destroy();
		}
		return OwnedSiblings.size();
	}
}
