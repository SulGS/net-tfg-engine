#pragma once
#include <queue>
#include <mutex>
#include <condition_variable>

template<typename T>
class ThreadSafeQueue {
public:
    void Push(const T& item) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            queue.push(item);
        }
        cv.notify_one();
    }

    bool WaitPop(T& out) {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&] { return !queue.empty() || stopFlag; });

        if (queue.empty())
            return false; // stopping

        out = queue.front();
        queue.pop();
        return true;
    }

    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopFlag = true;
        }
        cv.notify_all();
    }

private:
    std::queue<T> queue;
    std::mutex mutex;
    std::condition_variable cv;
    bool stopFlag = false;
};
