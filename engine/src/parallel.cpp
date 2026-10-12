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
// WebAssembly build starts this many workers when it loads (see
// CMakeLists.txt), since a thread started in the middle of a search would
// never get to run.
constexpr unsigned kMaxThreads = 8;

thread_local bool helper = false;
thread_local int depth = 0;

struct Job {
  const std::function<void(std::size_t)>* fn = nullptr;
  std::size_t n = 0;
  std::atomic<std::size_t> next{0};
  std::atomic<std::size_t> done{0};
  std::mutex error_mu;
  std::exception_ptr error;
};

// Helper threads that wait for one job at a time and take its calls in
// turn with the thread that posted it.
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
      job_ = job;
      ++generation_;
    }
    posted_.notify_all();
    Work(*job);
    {
      std::unique_lock lock(mu_);
      finished_.wait(lock, [&] { return job->done.load() == n; });
      job_.reset();
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
    posted_.notify_all();
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
    std::size_t seen = 0;
    while (true) {
      std::shared_ptr<Job> job;
      {
        std::unique_lock lock(mu_);
        posted_.wait(lock, [&] { return stop_ || (job_ && generation_ != seen); });
        if (stop_) {
          return;
        }
        seen = generation_;
        job = job_;
      }
      Work(*job);
    }
  }

  void Work(Job& job) {
    while (true) {
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
        finished_.notify_all();
      }
    }
  }

  std::mutex mu_;
  std::condition_variable posted_;
  std::condition_variable finished_;
  std::shared_ptr<Job> job_;
  std::size_t generation_ = 0;
  bool stop_ = false;
  std::vector<std::thread> threads_;
};

}  // namespace

void ParallelFor(std::size_t n, const std::function<void(std::size_t)>& fn) {
  if (n == 0) {
    return;
  }
  if (helper || depth > 0 || n == 1 || Pool::Get().Helpers() == 0) {
    for (std::size_t i = 0; i < n; ++i) {
      fn(i);
    }
    return;
  }
  ++depth;
  try {
    Pool::Get().Run(n, fn);
  } catch (...) {
    --depth;
    throw;
  }
  --depth;
}

bool OnHelperThread() { return helper; }

}  // namespace rotmg
