// resogun: host threads that may call guest code (AvPlayer allocator callbacks).
// Each thread gets a guest TCB (GS base, TLS) from the C runtime before running.
#pragma once
#include <cstdio>
#include <functional>
#include <mutex>
#include <stop_token>
#include <system_error>
#include <thread>
#include "common/types.h"

extern "C" void runtime_thread_attach_host(const char* name);

namespace Libraries::Kernel {
// AvPlayer joins one Thread from several threads at once: a decoder thread joins itself when its
// stream ends while the demuxer thread (or the game's sceAvPlayerStop) joins it too. A bare
// std::jthread is not safe for that: the self-join detaches, which closes the Win32 handle the
// other thread is waiting on (libc++ reported ERROR_INVALID_HANDLE as "thread::join failed: No
// such device or address", and a reused handle value could be waited on or closed instead).
// resogun: the jthread is only touched under a mutex; a join from another thread takes it out of
// the object first and waits on its own copy, so the lock is never held while blocking.
class Thread {
public:
    Thread() = default;
    ~Thread() { Stop(); }
    void Run(std::function<void(std::stop_token)>&& func) {
        std::jthread old;
        {
            std::lock_guard lock(mutex);
            old = std::move(thread);
            thread = std::jthread([func = std::move(func)](std::stop_token stop) {
                runtime_thread_attach_host("resogun:hle");
                func(stop);
            });
        }
        Finish(old, true);
    }
    // A thread may join its own Thread object (AvPlayer does when a stream ends): it detaches,
    // and the object no longer reports a running thread.
    void Join() { Take(false); }
    bool Joinable() const {
        std::lock_guard lock(mutex);
        return thread.joinable();
    }
    void Stop() { Take(true); }

private:
    void Take(bool stop) {
        std::jthread taken;
        {
            std::lock_guard lock(mutex);
            if (!thread.joinable()) return;
            taken = std::move(thread);
        }
        Finish(taken, stop);
    }
    // Called from guest code, which a C++ exception cannot unwind: a failed join is logged and
    // the thread detached instead.
    static void Finish(std::jthread& t, bool stop) {
        if (!t.joinable()) return;
        if (stop) t.request_stop();
        try {
            if (t.get_id() == std::this_thread::get_id()) t.detach();
            else t.join();
        } catch (const std::system_error& error) {
            std::fprintf(stderr, "HLE thread: join failed (%s)\n", error.what());
            if (t.joinable()) t.detach();
        }
    }

    mutable std::mutex mutex;
    std::jthread thread;
};
} // namespace Libraries::Kernel
