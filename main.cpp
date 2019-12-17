/******************************************************************************************************
 * bloomfilter_lock:
 * A framework for scalable read/write locking.
 * Released under the terms of the MIT License: https://opensource.org/licenses/MIT
 *****************************************************************************************************/
#include <algorithm>
#include <chrono>
#include <functional>
#include "bloomfilter_lock.hpp"
#include <random>
#include <stdio.h>
#include <thread>
#include <bits/stdc++.h> 
#include <cstdlib>

typedef std::chrono::high_resolution_clock::time_point hres_t;
typedef std::chrono::duration<size_t, std::ratio<1, 1000000>> duration_t; // micro-seconds.

//using BloomFilterLock = bloomfilter_lock::BloomFilterLock<std::mutex>;
using BloomFilterLock = bloomfilter_lock::BloomFilterLock<bloomfilter_lock::_SpinLock>;

struct task
{

    std::mutex& m_mutex;
    std::condition_variable& m_cv;
    bool& m_run;
    BloomFilterLock& m_bloomfilter_lock;

    task(BloomFilterLock& lock, std::mutex& m, std::condition_variable& v, bool& run_var):
        m_mutex(m),
        m_cv(v),
        m_run(run_var),
        m_bloomfilter_lock(lock)
    {
    }

    void operator()()
    {
        // Create some IDs for use with the lock.
        std::default_random_engine generator(rand());
        std::uniform_int_distribution<int> distribution(1, INT_MAX);
        auto resource1 = distribution(generator) | 0x01; // the bitwise or is to ensure the generated key does not get mapped to 0
        auto resource2 = distribution(generator) | 0x01;
        
        std::vector<bloomfilter_lock::Key> reads;
        std::vector<bloomfilter_lock::Key> writes;

        reads.push_back(resource1);
        writes.push_back(resource2);

        bloomfilter_lock::LockIntention intention(reads, writes);
        
        // Wait for go...
        std::unique_lock<std::mutex> l(m_mutex);
        while (!m_run)
        {
            m_cv.wait(l);
        }

        l.unlock();

        size_t count = 500000;
        hres_t vi_start = std::chrono::high_resolution_clock::now ();

        for (auto i = 0; i < count; ++i)
        {
            m_bloomfilter_lock.multilock(intention);
            m_bloomfilter_lock.unlock();
            m_bloomfilter_lock.global_read_lock();
            m_bloomfilter_lock.unlock();
            m_bloomfilter_lock.global_read_lock();
            m_bloomfilter_lock.unlock();
        }

        hres_t vi_end = std::chrono::high_resolution_clock::now ();
        duration_t timespan = std::chrono::duration_cast<duration_t> (vi_end - vi_start);

        l.lock();
        fprintf(stderr, "Time for %ld lock cycles: %ld micro-seconds\n", count*3, timespan.count());
    }
};


int main()
{
    BloomFilterLock l;
    auto num_cores = std::thread::hardware_concurrency();
    
    size_t concurrency = num_cores > 2 ? num_cores - 1 : num_cores;
    //size_t concurrency = 2;
    std::mutex m;
    std::condition_variable v;
    bool runbool = false;

    std::vector<std::thread> threads;

    for (auto i = 0; i < concurrency; ++i)
    {
        task t (l, m, v, runbool);
        threads.push_back(std::thread(t));
    }

    std::this_thread::sleep_for(std::chrono::seconds(5));
    runbool = true;
    v.notify_all();

    std::for_each(threads.begin(), threads.end(), std::mem_fn(&std::thread::join));
    return 0;
}
