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
#include<bits/stdc++.h> 

typedef std::chrono::high_resolution_clock::time_point hres_t;
typedef std::chrono::duration<size_t, std::ratio<1, 1000000>> duration_t; // micro-seconds.

struct task
{

    std::mutex& m_mutex;
    std::condition_variable& m_cv;
    bool& m_run;
    bloomfilter_lock::BloomFilterLock& m_bloomfilter_lock;

    task(bloomfilter_lock::BloomFilterLock& lock, std::mutex& m, std::condition_variable& v, bool& run_var):
        m_mutex(m),
        m_cv(v),
        m_run(run_var),
        m_bloomfilter_lock(lock)
    {
    }

    void operator()()
    {
        // Create some IDs for use with the lock.
        std::default_random_engine generator;
        std::uniform_int_distribution<int> distribution(1, INT_MAX);
        auto resource1 = distribution(generator);
        auto resource2 = distribution(generator);
       
        std::vector<bloomfilter_lock::Key> reads;
        std::vector<bloomfilter_lock::Key> writes;

        reads.push_back(resource1);
        writes.push_back(resource2);

        // Wait for go...
        std::unique_lock<std::mutex> l(m_mutex);
        while (!m_run)
        {
            m_cv.wait(l);
        }

        l.unlock();

        size_t count = 1000000;
        hres_t vi_start = std::chrono::high_resolution_clock::now ();

        for (auto i = 0; i < count; ++i)
        {
            m_bloomfilter_lock.multilock(reads, writes);
            m_bloomfilter_lock.unlock();
            m_bloomfilter_lock.global_read_lock();
            m_bloomfilter_lock.unlock();
        }

        hres_t vi_end = std::chrono::high_resolution_clock::now ();
        duration_t timespan = std::chrono::duration_cast<duration_t> (vi_end - vi_start);

        l.lock();
        fprintf(stderr, "Time for %ld lock cycles: %ld micro-seconds\n", count*2, timespan.count());
    }
};


int main()
{
    bloomfilter_lock::BloomFilterLock l;
    size_t concurrency = std::thread::hardware_concurrency();
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