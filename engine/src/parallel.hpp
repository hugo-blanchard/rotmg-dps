#pragma once

#include <cstddef>
#include <functional>

// Runs work on the engine's threads.
namespace rotmg {

/// Calls fn(0) to fn(n - 1), spread over the engine's threads, and returns
/// once every call has. The calling thread takes part. A call from inside
/// another ParallelFor runs its work on the calling thread alone, so work
/// can nest without waiting on itself. The first exception thrown is
/// rethrown once every call has finished.
void ParallelFor(std::size_t n, const std::function<void(std::size_t)>& fn);

/// Whether the calling thread is one of the engine's helpers rather than
/// the one that started the work.
bool OnHelperThread();

}  // namespace rotmg
