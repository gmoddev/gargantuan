#include "gargantuan/runtime/JobSystem.hpp"
#include "gargantuan/runtime/ExecutionDomain.hpp"

#include <algorithm>
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
		std::vector<Job> Queue;
		std::size_t QueueHead = 0;
		std::size_t QueueSize = 0;
		std::vector<std::thread> Workers;
		std::size_t WorkerCapacity = 0;
		std::size_t Active = 0;
		bool Accepting = true;
		bool Stopping = false;
	};

	JobSystem::JobSystem(std::size_t WorkerCount, StartPolicy Policy) : State(std::make_unique<Impl>()) {
		State->Queue.resize(64);
		State->WorkerCapacity = WorkerCount == 0 ? std::max(1u, std::thread::hardware_concurrency()) : WorkerCount;
		if (Policy == StartPolicy::Immediate) {
			try {
				StartWorkers();
			} catch (...) {
				Shutdown(false);
				throw;
			}
		}
	}

	void JobSystem::StartWorkers() {
		// Construction is private to the owner. After construction, Submit
		// serializes startup with other submissions and shutdown.
		State->Workers.reserve(State->WorkerCapacity);
		while (State->Workers.size() < State->WorkerCapacity) {
			State->Workers.emplace_back([this] {
				ExecutionDomainScope domain(ExecutionDomain::Worker);
				while (true) {
					Impl::Job job;
					{
						std::unique_lock lock(State->Mutex);
						State->Available.wait(lock, [this] { return State->Stopping || State->QueueSize != 0; });
						if (State->Stopping && State->QueueSize == 0) return;
						job = std::move(State->Queue[State->QueueHead]);
						State->Queue[State->QueueHead] = {};
						State->QueueHead = (State->QueueHead + 1) % State->Queue.size();
						--State->QueueSize;
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
						if (--State->Active == 0 && State->QueueSize == 0) State->Drained.notify_all();
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
		// A partially started pool stays owned and idle if creation throws.
		// No job or group reservation is published until startup succeeds.
		if (State->Workers.size() < State->WorkerCapacity) StartWorkers();
		if (State->QueueSize == State->Queue.size()) {
			std::vector<Impl::Job> Expanded(State->Queue.size() * 2);
			for (std::size_t Index = 0; Index < State->QueueSize; ++Index)
				Expanded[Index] = std::move(State->Queue[(State->QueueHead + Index) % State->Queue.size()]);
			State->Queue = std::move(Expanded);
			State->QueueHead = 0;
		}
		const auto Position = (State->QueueHead + State->QueueSize) % State->Queue.size();
		if (group) group->Add();
		State->Queue[Position] = {std::move(job), group};
		++State->QueueSize;
		State->Available.notify_one();
	}

	void JobSystem::Drain() {
		std::unique_lock lock(State->Mutex);
		State->Drained.wait(lock, [this] { return State->QueueSize == 0 && State->Active == 0; });
	}

	void JobSystem::Shutdown(bool drain) {
		if (!State) return;
		std::vector<std::thread> Workers;
		{
			std::scoped_lock lock(State->Mutex);
			if (!State->Accepting && State->Stopping) return;
			State->Accepting = false;
			if (!drain) {
				for (std::size_t Index = 0; Index < State->QueueSize; ++Index) {
					auto &Job = State->Queue[(State->QueueHead + Index) % State->Queue.size()];
					if (Job.Group) Job.Group->Complete(nullptr);
					Job = {};
				}
				State->QueueHead = 0;
				State->QueueSize = 0;
			}
			State->Stopping = true;
			Workers = std::move(State->Workers);
		}
		State->Available.notify_all();
		State->Drained.notify_all();
		for (auto &worker : Workers) {
			if (worker.joinable()) worker.join();
		}
	}

	std::size_t JobSystem::GetWorkerCount() const {
		std::scoped_lock Lock(State->Mutex);
		return State->Workers.size();
	}
}
