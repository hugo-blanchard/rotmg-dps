#include "parallel.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace rotmg {
namespace {

// The most threads work is spread over, the calling one included. The
// WebAssembly build starts at least as many workers when it loads (see
// CMakeLists.txt), since a thread started in the middle of a search would
// never get to run.
constexpr unsigned kMaxThreads = 8;

thread_local bool helper = false;

struct Job {
  const std::function<void(std::size_t)>* fn = nullptr;
  std::size_t n = 0;
  std::atomic<std::size_t> next{0};
  std::atomic<std::size_t> done{0};
  std::mutex error_mu;
  std::exception_ptr error;

  bool Claimable() const { return next.load() < n; }
};

// Helper threads that take calls from any job posted, newest first. A
// thread waiting for its own job's calls to finish takes calls from other
// jobs meanwhile, so jobs can be posted from inside jobs without waiting on
// themselves.
class Pool {
 public:
  static Pool& Get() {
    static Pool pool;
    return pool;
  }

  std::size_t Helpers() const { return threads_.size(); }

  void Run(std::size_t n, const std::function<void(std::size_t)>& fn) {
    auto job = std::make_shared<Job>();
    job->fn = &fn;
    job->n = n;
    {
      std::lock_guard lock(mu_);
      jobs_.push_back(job);
    }
    changed_.notify_all();
    while (job->Claimable()) {
      Take(*job);
    }
    while (job->done.load() < n) {
      if (std::shared_ptr<Job> other = Claim()) {
        Take(*other);
        continue;
      }
      std::unique_lock lock(mu_);
      changed_.wait(lock, [&] { return job->done.load() == n || Claimable(); });
    }
    if (job->error) {
      std::rethrow_exception(job->error);
    }
  }

  ~Pool() {
    {
      std::lock_guard lock(mu_);
      stop_ = true;
    }
    changed_.notify_all();
    for (std::thread& t : threads_) {
      t.join();
    }
  }

 private:
  Pool() {
    const unsigned threads = std::clamp(std::thread::hardware_concurrency(), 1u, kMaxThreads);
    for (unsigned i = 1; i < threads; ++i) {
      threads_.emplace_back([this] { Loop(); });
    }
  }

  void Loop() {
    helper = true;
    while (true) {
      std::shared_ptr<Job> job = Claim();
      if (job) {
        Take(*job);
        continue;
      }
      std::unique_lock lock(mu_);
      changed_.wait(lock, [&] { return stop_ || Claimable(); });
      if (stop_) {
        return;
      }
    }
  }

  // Whether any job has calls left to take; under mu_.
  bool Claimable() const {
    return std::ranges::any_of(jobs_, [](const std::shared_ptr<Job>& j) { return j->Claimable(); });
  }

  // The newest job with calls left, dropping finished ones from the list.
  std::shared_ptr<Job> Claim() {
    std::lock_guard lock(mu_);
    std::erase_if(jobs_, [](const std::shared_ptr<Job>& j) { return !j->Claimable(); });
    return jobs_.empty() ? nullptr : jobs_.back();
  }

  // Runs one call of the job, if one is left.
  void Take(Job& job) {
    const std::size_t i = job.next.fetch_add(1);
    if (i >= job.n) {
      return;
    }
    try {
      (*job.fn)(i);
    } catch (...) {
      std::lock_guard lock(job.error_mu);
      if (!job.error) {
        job.error = std::current_exception();
      }
    }
    if (job.done.fetch_add(1) + 1 == job.n) {
      std::lock_guard lock(mu_);
      changed_.notify_all();
    }
  }

  std::mutex mu_;
  std::condition_variable changed_;
  std::vector<std::shared_ptr<Job>> jobs_;
  bool stop_ = false;
  std::vector<std::thread> threads_;
};

}  // namespace

void ParallelFor(std::size_t n, const std::function<void(std::size_t)>& fn) {
  if (n == 0) {
    return;
  }
  if (n == 1 || Pool::Get().Helpers() == 0) {
    for (std::size_t i = 0; i < n; ++i) {
      fn(i);
    }
    return;
  }
  Pool::Get().Run(n, fn);
}

bool OnHelperThread() { return helper; }

}  // namespace rotmg
