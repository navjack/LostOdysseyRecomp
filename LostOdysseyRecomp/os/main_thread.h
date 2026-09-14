#pragma once

#include <atomic>
#include <chrono>
#include <functional>

// Work that must run on the process main thread, such as Cocoa window creation
// and event pumping on macOS. Other platforms may use it but do not need to.
// On macOS the main dispatch queue is the task queue and Drain runs the main
// run loop, so blocks that system frameworks queue to the main queue run too.
namespace os::main_thread
{
    // Record the calling thread as the main thread. Call at the start of main().
    void Initialize();
    bool IsMainThread();

    // Run task on the main thread and wait for it to finish. Runs inline when
    // called on the main thread (or before Initialize). Rethrows task exceptions.
    void RunSync(const std::function<void()>& task);

    // Main thread only: run queued tasks, waiting up to timeout for the first one.
    void Drain(std::chrono::milliseconds timeout);

    // Main thread only: run queued tasks and idle work until stop becomes true.
    void Run(const std::atomic<bool>& stop, const std::function<void()>& idle);

    // Return from a waiting Drain/Run early, e.g. after setting the stop flag.
    void Wake();
}
