#pragma once

#include <thread>

#ifndef _WIN32
#include <exception>
#include <functional>
#include <memory>
#include <pthread.h>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <utility>
#endif

namespace os
{
#ifdef _WIN32
using HostThread = std::thread;
#else
// Host thread for work that runs recompiled guest code. std::thread cannot set a
// stack size and macOS gives secondary threads 512 KiB, while recompiled code
// recurses deeply on the host stack; use the 8 MiB main-thread size instead.
class HostThread
{
public:
    static constexpr size_t kStackSize = 8 * 1024 * 1024;

    HostThread() noexcept = default;

    // Like std::thread, the callable and arguments are decay-copied and may be move-only.
    template <class Function, class... Args>
    explicit HostThread(Function&& function, Args&&... args)
    {
        using Task = std::tuple<std::decay_t<Function>, std::decay_t<Args>...>;
        auto task = std::make_unique<Task>(std::forward<Function>(function), std::forward<Args>(args)...);
        pthread_attr_t attributes;
        pthread_attr_init(&attributes);
        pthread_attr_setstacksize(&attributes, kStackSize);
        const int error = pthread_create(&m_handle, &attributes, &Entry<Task>, task.get());
        pthread_attr_destroy(&attributes);
        if (error != 0)
            throw std::system_error(error, std::generic_category(), "pthread_create");
        task.release();
        m_joinable = true;
    }

    HostThread(HostThread&& other) noexcept
        : m_handle(other.m_handle), m_joinable(std::exchange(other.m_joinable, false))
    {
    }

    HostThread& operator=(HostThread&& other) noexcept
    {
        if (m_joinable)
            std::terminate();
        m_handle = other.m_handle;
        m_joinable = std::exchange(other.m_joinable, false);
        return *this;
    }

    HostThread(const HostThread&) = delete;
    HostThread& operator=(const HostThread&) = delete;

    ~HostThread()
    {
        if (m_joinable)
            std::terminate();
    }

    bool joinable() const noexcept { return m_joinable; }

    void join()
    {
        pthread_join(m_handle, nullptr);
        m_joinable = false;
    }

    void detach()
    {
        pthread_detach(m_handle);
        m_joinable = false;
    }

    pthread_t native_handle() const noexcept { return m_handle; }

private:
    template <class Task>
    static void* Entry(void* argument)
    {
        std::unique_ptr<Task> task(static_cast<Task*>(argument));
        std::apply([](auto& function, auto&... args) { std::invoke(std::move(function), std::move(args)...); }, *task);
        return nullptr;
    }

    pthread_t m_handle{};
    bool m_joinable = false;
};
#endif
}
