#pragma once

#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>

namespace gargantuan {
	class JobGroup {
	  public:
		void Wait();
		[[nodiscard]] bool IsComplete() const;
		[[nodiscard]] std::exception_ptr GetFirstException() const;

	  private:
		friend class JobSystem;
		void Add();
		void Complete(std::exception_ptr exception);
		mutable std::mutex Mutex;
		std::condition_variable Completed;
		std::size_t Pending = 0;
		std::exception_ptr FirstException;
	};

	class JobSystem {
	  public:
		enum class StartPolicy { Immediate, OnFirstSubmission };
		explicit JobSystem(std::size_t WorkerCount = 0, StartPolicy Policy = StartPolicy::Immediate);
		~JobSystem();

		void Submit(std::function<void()> job, const std::shared_ptr<JobGroup> &group = nullptr);
		void Drain();
		void Shutdown(bool drain = true);
		[[nodiscard]] std::size_t GetWorkerCount() const;

		JobSystem(const JobSystem &) = delete;
		JobSystem &operator=(const JobSystem &) = delete;

	  private:
		struct Impl;
		std::unique_ptr<Impl> State;
		void StartWorkers();
	};
}
