#include "main_thread.h"

#include <exception>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <pthread.h>
#else
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <thread>
#endif

namespace os::main_thread
{
#ifdef __APPLE__
// The main dispatch queue is the task queue. AppKit, SDL and plume also queue
// work there (plume's CocoaWindow uses dispatch_sync), so every kind of
// main-thread work is served by the same main run loop.
void Initialize()
{
}

bool IsMainThread()
{
    return pthread_main_np() != 0;
}

void RunSync(const std::function<void()>& task)
{
    if (IsMainThread())
    {
        task();
        return;
    }
    struct Context
    {
        const std::function<void()>* task;
        std::exception_ptr error;
    } context{ &task, nullptr };
    dispatch_sync_f(dispatch_get_main_queue(), &context, [](void* argument) {
        auto* context = static_cast<Context*>(argument);
        try { (*context->task)(); }
        catch (...) { context->error = std::current_exception(); }
    });
    if (context.error)
        std::rethrow_exception(context.error);
}

void Drain(std::chrono::milliseconds timeout)
{
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, std::chrono::duration<double>(timeout).count(), true);
}

void Wake()
{
    dispatch_async_f(dispatch_get_main_queue(), nullptr, [](void*) {});
}
#else
namespace
{
    std::thread::id g_mainThread;
    std::mutex g_mutex;
    std::condition_variable g_wake;
    std::deque<std::function<void()>> g_tasks;
    bool g_woken = false;
}

void Initialize()
{
    g_mainThread = std::this_thread::get_id();
}

bool IsMainThread()
{
    return g_mainThread == std::thread::id{} || std::this_thread::get_id() == g_mainThread;
}

void RunSync(const std::function<void()>& task)
{
    if (IsMainThread())
    {
        task();
        return;
    }
    std::packaged_task<void()> packaged(task);
    auto done = packaged.get_future();
    {
        std::lock_guard lock(g_mutex);
        g_tasks.emplace_back([&packaged] { packaged(); });
    }
    g_wake.notify_all();
    done.get();
}

void Drain(std::chrono::milliseconds timeout)
{
    std::unique_lock lock(g_mutex);
    if (g_tasks.empty() && !g_woken)
        g_wake.wait_for(lock, timeout, [] { return !g_tasks.empty() || g_woken; });
    g_woken = false;
    auto tasks = std::move(g_tasks);
    g_tasks.clear();
    lock.unlock();
    for (auto& task : tasks)
        task();
}

void Wake()
{
    {
        std::lock_guard lock(g_mutex);
        g_woken = true;
    }
    g_wake.notify_all();
}
#endif

void Run(const std::atomic<bool>& stop, const std::function<void()>& idle)
{
    // 8 ms bounds event latency like the Windows window thread's message wait.
    while (!stop.load())
    {
        Drain(std::chrono::milliseconds(8));
        if (idle)
            idle();
    }
    Drain(std::chrono::milliseconds(0));
}
}
