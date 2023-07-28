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
      cond_.notify_one();
      {
        std::unique_lock<std::mutex> l(queue_lock_);
        while (!input_queue_.empty())
        {
          auto &item = input_queue_.front();
          if (item.pro)
            item.pro->set_value(Result());
          input_queue_.pop();
        }
      };

      if (worker_)
      {
        worker_->join();
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