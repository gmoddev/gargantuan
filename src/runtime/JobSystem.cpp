#include "gargantuan/runtime/JobSystem.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"

#include <algorithm>
#include <deque>
#include <stdexcept>
#include <thread>
#include <vector>

namespace gargantuan {
	void JobGroup::Add() {
		std::scoped_lock lock(Mutex);
		++Pending;
	}
	void JobGroup::Complete(std::exception_ptr exception) {
		std::scoped_lock lock(Mutex);
		if (exception && !FirstException) FirstException = exception;
		if (--Pending == 0) Completed.notify_all();
	}

	void JobGroup::Wait() {
		std::unique_lock lock(Mutex);
		Completed.wait(lock, [this] { return Pending == 0; });
	}

	bool JobGroup::IsComplete() const {
		std::scoped_lock lock(Mutex);
		return Pending == 0;
	}

	std::exception_ptr JobGroup::GetFirstException() const {
		std::scoped_lock lock(Mutex);
		return FirstException;
	}

	struct JobSystem::Impl {
		struct Job {
			std::function<void()> Function;
			std::shared_ptr<JobGroup> Group;
		};
		std::mutex Mutex;
		std::condition_variable Available;
		std::condition_variable Drained;
		std::deque<Job> Queue;
		std::vector<std::thread> Workers;
		std::size_t Active = 0;
		bool Accepting = true;
		bool Stopping = false;
	};

	JobSystem::JobSystem(std::size_t workerCount) : State(std::make_unique<Impl>()) {
		if (workerCount == 0) workerCount = std::max(1u, std::thread::hardware_concurrency());
		for (std::size_t i = 0; i < workerCount; ++i) {
			State->Workers.emplace_back([this] {
				ExecutionDomainScope domain(ExecutionDomain::Worker);
				while (true) {
					Impl::Job job;
					{
						std::unique_lock lock(State->Mutex);
						State->Available.wait(lock, [this] { return State->Stopping || !State->Queue.empty(); });
						if (State->Stopping && State->Queue.empty()) return;
						job = std::move(State->Queue.front());
						State->Queue.pop_front();
						++State->Active;
					}
					std::exception_ptr exception;
					try {
						job.Function();
					} catch (...) {
						exception = std::current_exception();
					}
					if (job.Group) job.Group->Complete(exception);
					{
						std::scoped_lock lock(State->Mutex);
						if (--State->Active == 0 && State->Queue.empty()) State->Drained.notify_all();
					}
				}
			});
		}
	}

	JobSystem::~JobSystem() {
		Shutdown(true);
	}

	void JobSystem::Submit(std::function<void()> job, const std::shared_ptr<JobGroup> &group) {
		if (!job) throw std::invalid_argument("Cannot submit an empty job");
		std::scoped_lock lock(State->Mutex);
		if (!State->Accepting) throw std::runtime_error("JobSystem is shutting down");
		if (group) group->Add();
		State->Queue.push_back({std::move(job), group});
		State->Available.notify_one();
	}

	void JobSystem::Drain() {
		std::unique_lock lock(State->Mutex);
		State->Drained.wait(lock, [this] { return State->Queue.empty() && State->Active == 0; });
	}

	void JobSystem::Shutdown(bool drain) {
		if (!State) return;
		{
			std::scoped_lock lock(State->Mutex);
			if (!State->Accepting && State->Stopping) return;
			State->Accepting = false;
			if (!drain) {
				for (auto &job : State->Queue) {
					if (job.Group) job.Group->Complete(nullptr);
				}
				State->Queue.clear();
			}
			State->Stopping = true;
		}
		State->Available.notify_all();
		for (auto &worker : State->Workers) {
			if (worker.joinable()) worker.join();
		}
		State->Workers.clear();
	}

	std::size_t JobSystem::GetWorkerCount() const {
		return State->Workers.size();
	}
}
