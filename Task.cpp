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
#include <chrono>

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
void quicksort(std::vector<int>& array, int left, int right, ThreadPool& pool) {
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

        if ((right - left) > 1) { // Изменено условие для простоты проверки
            std::shared_ptr<std::atomic<int>> counter = std::make_shared<std::atomic<int>>(1);
            auto future_left = pool.enqueue([&array, &pool, left, j, counter] {
                quicksort(array, left, j, pool);
                --(*counter);
                });

            quicksort(array, i, right, pool);

            while (*counter > 0) {
                std::this_thread::yield(); // Ждем завершения подзадач
            }
        }
        else {
            quicksort(array, left, j, pool);
            quicksort(array, i, right, pool);
        }
    }
}

int main() {
    std::vector<int> array = { 5, 3, 8, 1, 2, 7, 10, 4, 9, 6 }; // Массив из 10 элементов для быстрой проверки

    ThreadPool pool(std::thread::hardware_concurrency());

    auto start = std::chrono::high_resolution_clock::now();

    pool.enqueue([&array, &pool] {
        quicksort(array, 0, array.size() - 1, pool);
        });

    auto end = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed = end - start;

    std::cout << "Sorting completed in " << elapsed.count() << " seconds." << std::endl;

    // Выводим отсортированный массив для проверки
    std::cout << "Sorted array: ";
    for (int num : array) {
        std::cout << num << " ";
    }
    std::cout << std::endl;

    return 0;
}