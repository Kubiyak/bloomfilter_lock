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


    template <typename T>
    BloomFilterLock<T>::BloomFilterLock():
        m_active_lock_record(nullptr)
    {
        for (auto i = 0; i < 7; i++)
        {
            m_record_pool.push_back(new _LockRecord);
        }
        m_lock_queue.push(new _LockRecord);
    }


    template <typename T>
    BloomFilterLock<T>::~BloomFilterLock()
    {
        std::unique_lock<T> guard(m_mutex);
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

        auto lock_record = m_active_lock_record;
        if (lock_record)
        {
            m_active_lock_record = nullptr;
            lock_record->close();            
            delete lock_record;
        }

        for (auto r : m_record_pool)
            delete r;
    }

    
    template <typename T>
    _LockRecord* BloomFilterLock<T>::allocate_lock_record()
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


    template <typename T>
    void BloomFilterLock<T>::global_read_lock()
    {
        
        tl_existing_locks.track(this);        
        std::unique_lock<T> lock(m_mutex);
        
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


    template <typename T>
    void BloomFilterLock<T>::global_write_lock()
    {
        tl_existing_locks.track(this);
        std::unique_lock<T> lock(m_mutex);

        if (m_lock_queue.front()->global_write_request())
        {
            wait_at_queue_front(lock);
            return;
        }

        _LockRecord *r = allocate_lock_record();
        r->global_write_request();
        wait_at_queue_back(lock, r);
    }


    template <typename LockType>
    template <typename T>
    void BloomFilterLock<LockType>::multilock(const T& reads, const T& writes)
    {
        multilock(LockIntention(reads, writes));   
    }

    
    template <typename LockType>
    void BloomFilterLock<LockType>::multilock(const LockIntention& l)
    {
        tl_existing_locks.track(this);
        std::unique_lock<LockType> lock(m_mutex);
        if (m_lock_queue.front()->merge_lock_request(l))
        {
            wait_at_queue_front(lock);
            return;
        }

        _LockRecord *r = allocate_lock_record();
        r->merge_lock_request(l);
        wait_at_queue_back(lock, r);    
    }
    
    
    template <typename T>
    void BloomFilterLock<T>::read_lock(Key resource_id)
    {
        tl_existing_locks.track(this);

        std::unique_lock<T> lock(m_mutex);
        if (m_lock_queue.front()->merge_read_lock_request(resource_id))
        {
            wait_at_queue_front(lock);
            return;
        }
        
        _LockRecord *r = allocate_lock_record();
        r->merge_read_lock_request(resource_id);
        wait_at_queue_back(lock, r);
    }


    template <typename T>
    void BloomFilterLock<T>::write_lock(Key resource_id)
    {
        tl_existing_locks.track(this);

        std::unique_lock<T> lock(m_mutex);
        if (m_lock_queue.front()->merge_write_lock_request(resource_id))
        {
            wait_at_queue_front(lock);
            return;
        }
        _LockRecord *r = allocate_lock_record();
        r->merge_write_lock_request(resource_id);
        wait_at_queue_back(lock, r);
    }


    template <typename T>
    void BloomFilterLock<T>::unlock()
    {        
        tl_existing_locks.untrack(this);
        
        // valgrind seems to fail to establish happens-before on the
        // update to m_active_lock_record in a previous unlock op w/o
        // this spinlock. From my understanding of happens-before, it should
        // not strictly be necessary as happens-before for this is established
        // by the spinlock held in the activate call itself.        
        auto released_lock_record = m_active_lock_record;        
        
        if (released_lock_record->release())
        {
            // This thread is responsible for clearing the lock record and activating the next one.                 
            released_lock_record->clear();                                        
            std::unique_lock<T> guard(m_mutex);
            m_active_lock_record = 0;
            if (m_lock_queue.front()->record_type() != _LockRecord::None)
            {
                auto lock_record = m_lock_queue.front();
                m_lock_queue.pop();      
                m_active_lock_record = lock_record;                                    
                lock_record->activate();
            }

            if (m_lock_queue.empty())
                m_lock_queue.push(released_lock_record);           
            else
                m_record_pool.push_back(released_lock_record);
        }
    }

    
    template <typename T>
    thread_local _TLResourceTracker<T> BloomFilterLock<T>::tl_existing_locks;
}