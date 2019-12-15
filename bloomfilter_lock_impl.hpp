/****************************************************************************************************
 * bloomfilter_lock:
 * A framework for scalable read/write locking.
 * Released under the terms of the MIT License: https://opensource.org/licenses/MIT
 ***************************************************************************************************/

namespace bloomfilter_lock
{
    template<typename T>
    bool LockRecord::merge_lock_request(const T& reads, const T& writes)
    {

        if (m_record_type == ReadOnly)
            return !writes.size();

        if (m_record_type == None)
        {
            m_record_type = ReadWrite;
            m_num_requests = 1;
            m_lock_intention.set(reads, writes);
        }

        if (m_record_type == Exclusive)
            return false;

        // Probability of a failed merge is very high if there are many write requests.
        if (writes.size() > 8)
        {
            return false;
        }

        if (not merge_lock_request(reads, writes))
            return false;
        
        m_num_requests += 1;
        if (m_num_requests > 8)
            m_record_type = Exclusive;
        
        return true;
    }


    bool LockRecord::merge_read_lock_request(Key id)
    {
        return merge_lock_request({id}, {Key(0)});
    }

    
    bool LockRecord::merge_write_lock_request(Key id)
    {
        return merge_lock_request({id}, {id});
    }

        
    BloomFilterLock::BloomFilterLock():
        m_active_lock_record(0)
    {
        for (auto i = 0; i < 7; i++)
        {
            m_record_pool.push_back(new LockRecord);
        }
        m_lock_queue.push(new LockRecord);
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
            LockRecord *r = m_lock_queue.front();
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


    LockRecord* BloomFilterLock::allocate_lock_record()
    {
        // m_mutex must be held when calling this.
        LockRecord *result = 0;
        if (m_record_pool.size())
        {
            result = m_record_pool.back();
            m_record_pool.pop_back();
        }
        else
        {
            result = new LockRecord;
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

        LockRecord *r = allocate_lock_record();
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

        LockRecord *r = allocate_lock_record();
        r->global_write_request();
        wait_at_queue_back(lock, r);
    }


    template<typename T>
    void BloomFilterLock::multilock(const T& reads, const T& writes)
    {
        tl_existing_locks.track(this);

        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_lock_queue.front()->merge_lock_request(reads, writes))
        {
            wait_at_queue_front(lock);
            return;
        }

        LockRecord *r = allocate_lock_record();
        r->merge_lock_request(reads, writes);
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
        LockRecord *r = allocate_lock_record();
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
        LockRecord *r = allocate_lock_record();
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
            LockRecord* old_active_record = m_active_lock_record;

            std::unique_lock<std::mutex> lock(m_mutex);
            m_active_lock_record = 0;

            if (!m_lock_queue.empty())
            {
                if (m_lock_queue.front()->record_type() != LockRecord::None)
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