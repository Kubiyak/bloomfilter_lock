/****************************************************************************************************
 * bloomfilter_lock:
 * A framework for scalable read/write locking.
 * Released under the terms of the MIT License: https://opensource.org/licenses/MIT
 ***************************************************************************************************/

#include "bloomfilter_lock.hpp"


namespace bloomfilter_lock
{
    
    bool _LockRecord::merge_lock_request(const LockIntention& l)
    {
        // a count of 0 is guaranteed accurate.
        if (m_record_type == ReadOnly)
            return l.m_min_writes == 0;
    
        if (m_record_type == None)
        {
            m_record_type = ReadWrite;
            m_num_requests = 1;
            m_lock_intention = l;
            return true;
        }
    
        if (m_record_type == Exclusive)
            return false;
    
        if (l.m_min_writes > 8)
            return false;
        
        if (not m_lock_intention.merge(l))
            return false;
        
        m_num_requests += 1;
        if (m_num_requests > 8)
            m_record_type = Exclusive;
        return true;
    }
    

    bool _LockRecord::merge_read_lock_request(Key id)
    {
        return merge_lock_request(LockIntention({id}, {Key(0)}));
    }

    
    bool _LockRecord::merge_write_lock_request(Key id)
    {
        return merge_lock_request(LockIntention({id}, {id}));
    }

        
    BloomFilterLock::BloomFilterLock():
        m_active_lock_record(0)
    {
        for (auto i = 0; i < 7; i++)
        {
            m_record_pool.push_back(new _LockRecord);
        }
        m_lock_queue.push(new _LockRecord);
    }


    BloomFilterLock::~BloomFilterLock()
    {
        std::unique_lock<std::mutex> guard(m_mutex);
        if (m_closing)
        {
            // Double destructor call?
            return;
        }

        m_closing = true;

        while (!m_lock_queue.empty())
        {
            _LockRecord *r = m_lock_queue.front();
            r->close();
            m_lock_queue.pop();
            r->clear();
            delete r;
        }

        if (m_active_lock_record)
        {
            m_active_lock_record->close();
            delete m_active_lock_record;
        }

        for (auto r : m_record_pool)
            delete r;
    }


    _LockRecord* BloomFilterLock::allocate_lock_record()
    {
        // m_mutex must be held when calling this.
        _LockRecord *result = 0;
        if (m_record_pool.size())
        {
            result = m_record_pool.back();
            m_record_pool.pop_back();
        }
        else
        {
            result = new _LockRecord;
        }
        return result;
    }


    void BloomFilterLock::global_read_lock()
    {
        
        tl_existing_locks.track(this);        
        std::unique_lock<std::mutex> lock(m_mutex);
        
        // Attempt to merge in a read request into the head of the lock queue.
        if (m_lock_queue.front()->global_read_request())
        {
            wait_at_queue_front(lock);
            return;
        }

        if (m_lock_queue.size() > 1)
        {
            if (m_lock_queue.back()->global_read_request())
            {
                wait_at_queue_back(lock);
                return;
            }
        }
        
        _LockRecord *r = allocate_lock_record();
        wait_at_queue_back(lock, r);
    }


    void BloomFilterLock::global_write_lock()
    {
        tl_existing_locks.track(this);
        std::unique_lock<std::mutex> lock(m_mutex);

        if (m_lock_queue.front()->global_write_request())
        {
            wait_at_queue_front(lock);
            return;
        }

        _LockRecord *r = allocate_lock_record();
        r->global_write_request();
        wait_at_queue_back(lock, r);
    }


    template<typename T>
    void BloomFilterLock::multilock(const T& reads, const T& writes)
    {
        multilock(LockIntention(reads, writes));   
    }

    
    void BloomFilterLock::multilock(const LockIntention& l)
    {
        tl_existing_locks.track(this);
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_lock_queue.front()->merge_lock_request(l))
        {
            wait_at_queue_front(lock);
            return;
        }

        _LockRecord *r = allocate_lock_record();
        r->merge_lock_request(l);
        wait_at_queue_back(lock, r);    
    }
    
    
    void BloomFilterLock::read_lock(Key resource_id)
    {
        tl_existing_locks.track(this);

        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_lock_queue.front()->merge_read_lock_request(resource_id))
        {
            wait_at_queue_front(lock);
            return;
        }
        
        _LockRecord *r = allocate_lock_record();
        r->merge_read_lock_request(resource_id);
        wait_at_queue_back(lock, r);
    }


    void BloomFilterLock::write_lock(Key resource_id)
    {
        tl_existing_locks.track(this);

        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_lock_queue.front()->merge_write_lock_request(resource_id))
        {
            wait_at_queue_front(lock);
            return;
        }
        _LockRecord *r = allocate_lock_record();
        r->merge_write_lock_request(resource_id);
        wait_at_queue_back(lock, r);
    }


    void BloomFilterLock::unlock()
    {
        tl_existing_locks.untrack(this);

        if (m_active_lock_record->release())
        {
            // This thread is responsible for clearing the lock record and activating the next one.

            m_active_lock_record->clear();
            _LockRecord* old_active_record = m_active_lock_record;

            std::unique_lock<std::mutex> lock(m_mutex);
            m_active_lock_record = 0;

            if (!m_lock_queue.empty())
            {
                if (m_lock_queue.front()->record_type() != _LockRecord::None)
                {
                    m_active_lock_record = m_lock_queue.front();
                    m_lock_queue.pop();
                    m_active_lock_record->activate();
                }
            }

            if (m_lock_queue.empty())
                m_lock_queue.push(old_active_record);           
            else
                m_record_pool.push_back(old_active_record);
        }
    }

    thread_local _TLResourceTracker BloomFilterLock::tl_existing_locks;
}