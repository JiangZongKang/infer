#ifndef __CPM_HPP__
#define __CPM_HPP__

// Comsumer Producer Model

#include <algorithm>
#include <condition_variable>
#include <future>
#include <memory>
#include <queue>
#include <thread>

namespace cpm
{

  // 模板类
  template <typename Result, typename Input, typename Model>
  class Instance
  {
  protected:
    struct Item
    {
      Input input;
      // pro是一个指向std::promise类型对象的shared_ptr智能指针
      // std::promise是C++中用于异步编程的一种机制，可用于在一个线程中设置一个值，并在另外一个线程中获取这个值
      // pro用于存储一个异步操作的结果，被定义为shared_ptr，可在多个地方被共享，并且没有任何引用时会自动释放内存
      std::shared_ptr<std::promise<Result>> pro;
    };

    // std::condition_variable是C++中的一个同步原语，用于线程之间的条件变量通信
    // 允许一个或者多个线程等待某个条件成立，直到其他线程满足条件后通知等待的线程继续执行
    // cond_用作线程间的条件变量
    std::condition_variable cond_;
    // std::queue是C++中的一个容器适配器，用于实现队列数据结构，提供先进先出的元素访问方式
    // input_queue_用作一个队列，存储Item类型的对象
    // 通过调用input_queue_.push()将Item类型的对象添加到队列尾部, 并通过调用input_queue_.pop()从队列头部中移除一个Item类型的对象
    std::queue<Item> input_queue_;
    // std::mutex是C++中的一个线程安全的互斥量, 用于实现线程之间的互斥访问，保护共享资源，防止多个线程同时访问和修改这些资源，避免数据竞争和不一致的结果
    // queue_lock_用作一个互斥量,用于保护对input_queue_变量的访问
    // 通过调用queue_lock_.lock()可锁定互斥量，防止其他线程进入临界区；通过调用queue_lock_.unlock()解锁互斥量，允许其他线程进入临界区
    // 互斥量的使用可以保证在多线程的环境下对共享队列的访问是安全的，避免竞态条件和数据不一致的问题
    std::mutex queue_lock_;
    // std::thread是C++中的一个类，用于创建和管理线程
    // 允许在程序中同时执行多个任务，并且可以与其他线程进行通信和同步
    // 将其包在智能指针中可以方便的管理线程的生命周期，调用worker_.join()可以等待线程执行完成 
    std::shared_ptr<std::thread> worker_;
    // volatile是一个修饰符，告诉编译器该变量可能会被意外的修改，以防止编译器对该变量的优化
    // run_可能会在其他线程中被修改，使用volatile可以确保在每次访问该变量时都从内存中读取最新的值
    volatile bool run_ = false;
    // volatile关键字主要用于多线程编程与外部的硬件交互场景，确保变量的访问是可见性和一致性的
    volatile int max_items_processed_ = 0;
    // stream是一个指向void的指针，初始化为nullptr
    void *stream_ = nullptr;

  public:
    virtual ~Instance() { stop(); }

    void stop()
    {
      run_ = false;
      // 唤醒一个等待在该条件变量上的线程，让该线程继续执行
      cond_.notify_one();
      {
        // std::unique_lock 是C++标准库提供的一种互斥锁的封装类。它提供了更灵活的互斥锁操作，比如可以手动地锁定和解锁互斥锁。
        // 定义 std::unique_lock 对象 l 并传入互斥锁 queue_lock_，可以使用 l 对象来自动管理互斥锁的锁定和解锁。
        // 当 l 对象超出作用域时，会自动释放互斥锁，从而避免了手动管理互斥锁的麻烦和可能的错误。
        // 确保在进入互斥区域时只有一个线程能够访问 queue_lock_ 保护的代码块
        std::unique_lock<std::mutex> l(queue_lock_);
        while (!input_queue_.empty())
        {
          // input_queue_ 是一个队列，通过调用 front() 方法可以获取队列的第一个元素
          // 声明item为引用，则不会进行复制操作，而是直接将 item 绑定到队列中的第一个元素上。这样可以避免不必要的复制，提高代码的效率和性能。
          auto &item = input_queue_.front();
          if (item.pro)
            // pro 作为 Item 结构体的成员变量，通过箭头运算符 -> 来访问 std::promise 对象的成员函数 set_value()。
            // 通过调用 set_value() 方法，可以将一个 Result 类型的值设置给 std::promise 对象，以供其他地方使用 std::future 来获取这个值。
            item.pro->set_value(Result());
          // 从 input_queue_（输入队列）中移除队列的第一个元素
          // pop() 是队列（或者称为先进先出队列）的一个成员函数，它的作用是将队列的第一个元素移除。
          // 目的是在 stop() 函数中清空输入队列，以确保在停止之前没有未处理的输入。
          // 通过连续调用 pop() 方法，将队列中的元素一个一个地移除，直到队列为空。
          input_queue_.pop();
        }
      };

      if (worker_)
      {
        // 调用 join() 方法来等待 worker_ 所代表的工作线程完成
        // join() 方法会使当前线程阻塞，直到被调用的线程完成其执行。这样可以确保在继续执行后续代码之前，工作线程已经完成了任务。
        worker_->join();
        // 使用 reset() 方法将 worker_ 重置为空指针。
        // reset() 方法会释放 worker_ 所持有的资源，并将其重置为空指针。这样可以清理和重置工作线程对象，以便在后续的代码中重新使用或销毁。
        worker_.reset();
      }
    }

    virtual std::shared_future<Result> commit(const Input &input)
    {
      Item item;
      item.input = input;
      item.pro.reset(new std::promise<Result>());
      {
        std::unique_lock<std::mutex> __lock_(queue_lock_);
        input_queue_.push(item);
      }
      cond_.notify_one();
      return item.pro->get_future();
    }

    virtual std::vector<std::shared_future<Result>> commits(const std::vector<Input> &inputs)
    {
      std::vector<std::shared_future<Result>> output;
      {
        std::unique_lock<std::mutex> __lock_(queue_lock_);
        for (int i = 0; i < (int)inputs.size(); ++i)
        {
          Item item;
          item.input = inputs[i];
          item.pro.reset(new std::promise<Result>());
          output.emplace_back(item.pro->get_future());
          input_queue_.push(item);
        }
      }
      cond_.notify_one();
      return output;
    }

    template <typename LoadMethod>
    bool start(const LoadMethod &loadmethod, int max_items_processed = 1, void *stream = nullptr)
    {
      stop();

      this->stream_ = stream;
      this->max_items_processed_ = max_items_processed;
      std::promise<bool> status;
      worker_ = std::make_shared<std::thread>(&Instance::worker<LoadMethod>, this,
                                              std::ref(loadmethod), std::ref(status));
      return status.get_future().get();
    }

  private:
    template <typename LoadMethod>
    void worker(const LoadMethod &loadmethod, std::promise<bool> &status)
    {
      std::shared_ptr<Model> model = loadmethod();
      if (model == nullptr)
      {
        status.set_value(false);
        return;
      }

      run_ = true;
      status.set_value(true);

      std::vector<Item> fetch_items;
      std::vector<Input> inputs;
      while (get_items_and_wait(fetch_items, max_items_processed_))
      {
        inputs.resize(fetch_items.size());
        std::transform(fetch_items.begin(), fetch_items.end(), inputs.begin(),
                       [](Item &item)
                       { return item.input; });

        auto ret = model->forwards(inputs, stream_);
        for (int i = 0; i < (int)fetch_items.size(); ++i)
        {
          if (i < (int)ret.size())
          {
            fetch_items[i].pro->set_value(ret[i]);
          }
          else
          {
            fetch_items[i].pro->set_value(Result());
          }
        }
        inputs.clear();
        fetch_items.clear();
      }
      model.reset();
      run_ = false;
    }

    virtual bool get_items_and_wait(std::vector<Item> &fetch_items, int max_size)
    {
      std::unique_lock<std::mutex> l(queue_lock_);
      cond_.wait(l, [&]()
                 { return !run_ || !input_queue_.empty(); });

      if (!run_)
        return false;

      fetch_items.clear();
      for (int i = 0; i < max_size && !input_queue_.empty(); ++i)
      {
        fetch_items.emplace_back(std::move(input_queue_.front()));
        input_queue_.pop();
      }
      return true;
    }

    virtual bool get_item_and_wait(Item &fetch_item)
    {
      std::unique_lock<std::mutex> l(queue_lock_);
      cond_.wait(l, [&]()
                 { return !run_ || !input_queue_.empty(); });

      if (!run_)
        return false;

      fetch_item = std::move(input_queue_.front());
      input_queue_.pop();
      return true;
    }
  };
}; // namespace cpm

#endif // __CPM_HPP__