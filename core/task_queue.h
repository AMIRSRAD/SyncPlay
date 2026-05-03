#pragma once

#include <deque>
#include <functional>
#include <mutex>

class TaskQueue {
public:
    void push(std::function<void()> fn) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push_back(std::move(fn));
    }

    void drain() {
        std::deque<std::function<void()>> work;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            work.swap(m_queue);
        }
        for (auto& fn : work) {
            if (fn)
                fn();
        }
    }

    void clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.clear();
    }

private:
    std::deque<std::function<void()>> m_queue;
    std::mutex m_mutex;
};
