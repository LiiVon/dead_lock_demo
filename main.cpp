#include <iostream>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include <cassert>
#include <chrono>

class DeadLockDetector
{
private:
    DeadLockDetector() = default;
    std::mutex m_mtx_;
    uint64_t m_lock_id_counter = 1;
    // key:tid value:该线程正在等待的锁集合
    std::unordered_map<uint64_t, std::unordered_set<uint64_t>> m_wait_graph_;

    // key:lock_id value:持有该锁的tid
    std::unordered_map<uint64_t, uint64_t> m_hold_owner_;

public:
    static DeadLockDetector &getInstance()
    {
        static DeadLockDetector instance;
        return instance;
    }
    // 分配锁唯一id
    uint64_t alloc_lock_id()
    {
        std::lock_guard<std::mutex> lock(m_mtx_);
        return m_lock_id_counter++;
    }

    // 线程tid 等待 lock_id
    void add_wait(uint64_t tid, uint64_t lock_id)
    {
        std::lock_guard<std::mutex> lock(m_mtx_);
        m_wait_graph_[tid].insert(lock_id);
    }
    void remove_wait(uint64_t tid, uint64_t lock_id)
    {
        std::lock_guard<std::mutex> lock(m_mtx_);
        if (m_wait_graph_.count(tid) > 0)
        {
            m_wait_graph_[tid].erase(lock_id);
        }
    }

    void add_hold(uint64_t tid, uint64_t lock_id)
    {
        std::lock_guard<std::mutex> lock(m_mtx_);
        m_hold_owner_[lock_id] = tid;
    }
    void remove_hold(uint64_t lock_id)
    {
        std::lock_guard<std::mutex> lock(m_mtx_);
        m_hold_owner_.erase(lock_id);
    }

    // DFS环检测：判断加锁会不会形成死锁环
    bool has_dead_ring(uint64_t cur_tid, uint64_t target_lock, std::unordered_set<uint64_t> &visited_tid)
    {
        // target_lock被哪个线程持有
        if (!m_hold_owner_.count(target_lock))
        {
            return false;
        }
        uint64_t hold_tid = m_hold_owner_[target_lock];
        if (hold_tid == cur_tid)
        {
            return true;
        }
        if (visited_tid.count(hold_tid) > 0)
        {
            return false;
        }
        visited_tid.insert(hold_tid);

        // holde_tid正在等待哪些锁，递归
        if (m_wait_graph_.count(hold_tid) > 0)
        {
            for (auto &lock_id : m_wait_graph_[hold_tid])
            {
                if (has_dead_ring(cur_tid, lock_id, visited_tid))
                {
                    return true;
                }
            }
        }
        return false;
    }

    // 尝试加锁前调用，如果返回true 代表会发生死锁
    bool check_will_deadlock(uint64_t self_tid, uint64_t want_lock_id)
    {
        std::lock_guard<std::mutex> lock(m_mtx_);
        std::unordered_set<uint64_t> visited_tid;
        return has_dead_ring(self_tid, want_lock_id, visited_tid);
    }
};

class SafeMutex
{
private:
    std::mutex m_inner_mtx_;
    uint64_t m_lock_id_;

public:
    SafeMutex() : m_lock_id_(DeadLockDetector::getInstance().alloc_lock_id()) {}

    void lock()
    {
        uint64_t tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
        if (DeadLockDetector::getInstance().check_will_deadlock(tid, m_lock_id_))
        {
            std::cerr << "\n!!!!!!!!!!! WARN: POTENTIAL DEAD LOCK DETECTED !!!!!!!!!!!\n";
            std::cerr << "Thread:" << tid << " try lock lock_id:" << m_lock_id_ << ", will form deadlock ring\n";
            std::cerr << "!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n";
            assert(false); // 检测到死锁直接断言崩溃
        }

        DeadLockDetector::getInstance().add_wait(tid, m_lock_id_);

        m_inner_mtx_.lock();

        DeadLockDetector::getInstance().remove_wait(tid, m_lock_id_);
        DeadLockDetector::getInstance().add_hold(tid, m_lock_id_);
    }

    void unlock()
    {
        uint64_t tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
        DeadLockDetector::getInstance().remove_hold(m_lock_id_);
        m_inner_mtx_.unlock();
    }

    bool try_lock()
    {
        return m_inner_mtx_.try_lock();
    }
};

SafeMutex m1;
SafeMutex m2;

void thread_func_a()
{
    // 线程A：先拿m1，再拿m2
    m1.lock();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    m2.lock();

    m2.unlock();
    m1.unlock();
}

void thread_func_b()
{
    // 线程B：先拿m2，再拿m1，锁顺序反转，触发死锁
    m2.lock();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    m1.lock();

    m1.unlock();
    m2.unlock();
}

int main()
{
    std::thread t1(thread_func_a);
    std::thread t2(thread_func_b);

    t1.join();
    t2.join();
    return 0;
}