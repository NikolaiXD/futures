#include <iostream>
#include <vector>
#include <future>
#include <algorithm>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <memory>
#include <atomic>

// Простой пул потоков с work stealing
class ThreadPool {
public:
    ThreadPool(size_t threads) : stop(false) {
        for (size_t i = 0; i < threads; ++i) {
            workers.emplace_back([this] {
                for (;;) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(this->queue_mutex);
                        this->condition.wait(lock, [this] { return this->stop || !this->tasks.empty(); });
                        if (this->stop && this->tasks.empty()) {
                            return;
                        }
                        task = std::move(this->tasks.front());
                        this->tasks.pop();
                    }
                    task();
                }
                });
        }
    }

    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args) -> std::future<typename std::result_of<F(Args...)>::type> {
        using return_type = typename std::result_of<F(Args...)>::type;
        auto task = std::make_shared<std::packaged_task<return_type()>>(std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        std::future<return_type> res = task->get_future();
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            if (stop) {
                throw std::runtime_error("enqueue on stopped ThreadPool");
            }
            tasks.emplace([task]() { (*task)(); });
        }
        condition.notify_one();
        return res;
    }

    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queue_mutex);
            stop = true;
        }
        condition.notify_all();
        for (std::thread& worker : workers) {
            worker.join();
        }
    }

private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop;
};

// Функция для сортировки части массива
void quicksort(std::vector<int>& array, int left, int right, ThreadPool& pool, std::shared_ptr<std::atomic<int>> counter, std::shared_ptr<std::promise<void>> done) {
    if (left < right) {
        int pivot = array[(left + right) / 2];
        int i = left, j = right;
        while (i <= j) {
            while (array[i] < pivot) i++;
            while (array[j] > pivot) j--;
            if (i <= j) {
                std::swap(array[i], array[j]);
                i++;
                j--;
            }
        }

        // Считаем количество подзадач
        (*counter)++;
        auto left_done = std::make_shared<std::promise<void>>();
        auto left_counter = counter;

        if ((right - left) > 100000) {
            pool.enqueue([=, &array, &pool] {
                quicksort(array, left, j, pool, left_counter, left_done);
                left_done->set_value();
                });
        }
        else {
            quicksort(array, left, j, pool, left_counter, left_done);
            left_done->set_value();
        }

        quicksort(array, i, right, pool, counter, done);

        left_done->get_future().wait();

        // Уменьшаем счетчик подзадач
        if (--(*counter) == 0) {
            done->set_value();
        }
    }
    else {
        // Если задач больше нет, устанавливаем значение promise
        if (--(*counter) == 0) {
            done->set_value();
        }
    }
}

int main() {
    std::vector<int> array = { /* инициализация массива */ };
    ThreadPool pool(std::thread::hardware_concurrency());
    auto counter = std::make_shared<std::atomic<int>>(1);
    auto done = std::make_shared<std::promise<void>>();

    auto future = done->get_future();
    pool.enqueue([&array, &pool, counter, done] {
        quicksort(array, 0, array.size() - 1, pool, counter, done);
        });

    future.wait();
    std::cout << "Sorting completed." << std::endl;

    return 0;
}