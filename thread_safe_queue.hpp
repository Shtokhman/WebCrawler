#ifndef WEB_CRAWLER_THREAD_SAFE_QUEUE_HPP
#define WEB_CRAWLER_THREAD_SAFE_QUEUE_HPP

#include <queue>
#include <mutex>
#include <condition_variable>

template<typename T>
class thread_safe_queue {

    std::mutex mut;
    std::queue<T> data_queue;
    std::condition_variable data_cond;

public:
    thread_safe_queue() = default;

    void push(T new_value) {
        std::lock_guard<std::mutex> lk(mut);
        data_queue.push(new_value);
        data_cond.notify_one();
    }

    void wait_and_pop(T & value) {
        std::unique_lock<std::mutex> lk(mut);
        data_cond.wait(lk, [this] { return !data_queue.empty(); });
        value = data_queue.front();
        data_queue.pop();
    }


    [[nodiscard]] bool empty() const {
        std::lock_guard<std::mutex> lk(mut);
        return data_queue.empty();
    }
};

#endif