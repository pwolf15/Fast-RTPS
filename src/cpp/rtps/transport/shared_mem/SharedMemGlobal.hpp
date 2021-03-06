// Copyright 2019 Proyectos y Sistemas de Mantenimiento SL (eProsima).
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef _FASTDDS_SHAREDMEM_GLOBAL_H_
#define _FASTDDS_SHAREDMEM_GLOBAL_H_

#include <vector>
#include <mutex>
#include <memory>

#include <rtps/transport/shared_mem/SharedMemSegment.hpp>
#include <rtps/transport/shared_mem/MultiProducerConsumerRingBuffer.hpp>

#define THREADID "(ID:" << std::this_thread::get_id() <<") "

namespace eprosima {
namespace fastdds {
namespace rtps {

/**
 * This class defines the global resources for shared-memory communication.
 * Mainly the shared-memory ports and its operations.
 */
class SharedMemGlobal
{
public:

    struct BufferDescriptor;
    typedef std::function<void(const std::vector<
                const SharedMemGlobal::BufferDescriptor*>&,
                const std::string& domain_name)> PortFailureHandler;

    // Long names for SHM files could cause problems on some platforms
    static constexpr uint32_t MAX_DOMAIN_NAME_LENGTH = 16; 

    SharedMemGlobal(
        const std::string& domain_name,
        PortFailureHandler failure_handler)
        : domain_name_(domain_name)
    {
        if (domain_name.length() > MAX_DOMAIN_NAME_LENGTH)
        {
            throw std::runtime_error(
                    domain_name +
                    " too long for domain name (max " +
                    std::to_string(MAX_DOMAIN_NAME_LENGTH) +
                    " characters");
        }

        Port::on_failure_buffer_descriptors_handler(failure_handler);
    }

    ~SharedMemGlobal()
    {

    }

    /**
     * Identifies a data buffer given its segment_id (shared-memory segment global_name)
     * and offset inside the segment
     */
    struct BufferDescriptor
    {
        SharedMemSegment::Id source_segment_id;
        SharedMemSegment::offset buffer_node_offset;
    };

    typedef MultiProducerConsumerRingBuffer<BufferDescriptor>::Listener Listener;
    typedef MultiProducerConsumerRingBuffer<BufferDescriptor>::Cell PortCell;

    struct PortNode
    {
        UUID<8> uuid;
        uint32_t port_id;

        SharedMemSegment::condition_variable empty_cv;
        SharedMemSegment::mutex empty_cv_mutex;

        SharedMemSegment::offset buffer;
        SharedMemSegment::offset buffer_node;
        std::atomic<uint32_t> ref_counter;

        uint32_t waiting_count;

        static constexpr size_t LISTENERS_STATUS_SIZE = 1024;
        struct ListenerStatus
        {
            uint8_t is_waiting              : 1;
            uint8_t counter                 : 3;
            uint8_t last_verified_counter   : 3;
            uint8_t pad                     : 1;
        };
        ListenerStatus listeners_status[LISTENERS_STATUS_SIZE];
        uint32_t num_listeners;
        std::atomic<std::chrono::high_resolution_clock::rep> last_listeners_status_check_time_ms;
        uint32_t healthy_check_timeout_ms;
        uint32_t port_wait_timeout_ms;
        uint32_t max_buffer_descriptors;

        bool is_port_ok;
        bool is_opened_read_exclusive;
        bool is_opened_for_reading;

        char domain_name[MAX_DOMAIN_NAME_LENGTH+1];
    };

    /**
     * A shared-memory port is a communication channel where data can be written / read.
     * A port has a port_id and a global name derived from the port_id and the domain.
     * System processes can open a port by knowing its name.
     */
    class Port
    {
        friend class MockPortSharedMemGlobal;

    private:

        std::shared_ptr<SharedMemSegment> port_segment_;

        PortNode* node_;

        std::unique_ptr<MultiProducerConsumerRingBuffer<BufferDescriptor>> buffer_;

        uint64_t overflows_count_;

        inline void notify_unicast(
                bool was_buffer_empty_before_push)
        {
            if (was_buffer_empty_before_push)
            {
                node_->empty_cv.notify_one();
            }
        }

        inline void notify_multicast()
        {
            node_->empty_cv.notify_all();
        }

        /**
         * Singleton with a thread that periodically checks all opened ports
         * to verify if some listener is dead.
         */
        class Watchdog
        {
        public:

            struct PortContext
            {
                std::shared_ptr<SharedMemSegment> port_segment;
                PortNode* node;
                MultiProducerConsumerRingBuffer<BufferDescriptor>* buffer;
            };

            static Watchdog& get()
            {
                static Watchdog watch_dog;
                return watch_dog;
            }

            /**
             * Sets the on_failure_buffer_descriptors_handler_.
             * This is done only the first time the function is called,
             * as the handler must be a static inmutable function.
             */
            void on_failure_buffer_descriptors_handler(
                    std::function<void(const std::vector<
                        const BufferDescriptor*>&,
                    const std::string& domain_name)> handler)
            {
                if (!is_on_failure_buffer_descriptors_handler_set_)
                {
                    std::lock_guard<std::mutex> lock(watched_ports_mutex_);

                    // Checking is_on_failure_buffer_descriptors_handler_set_ twice can be weird but avoid
                    // the use of a recursive_mutex here.
                    if (!is_on_failure_buffer_descriptors_handler_set_)
                    {
                        on_failure_buffer_descriptors_handler_ = handler;
                        is_on_failure_buffer_descriptors_handler_set_ = true;
                    }
                }
            }

            /**
             * Called by the Port constructor, adds a port to the watching list.
             */
            void add_port_to_watch(
                    std::shared_ptr<PortContext>&& port)
            {
                std::lock_guard<std::mutex> lock(watched_ports_mutex_);
                watched_ports_.push_back(port);
            }

            /**
             * Called by the Port destructor, removes a port from the watching list.
             */
            void remove_port_from_watch(
                    PortNode* port_node)
            {
                std::lock_guard<std::mutex> lock(watched_ports_mutex_);

                auto it = watched_ports_.begin();

                while (it != watched_ports_.end())
                {
                    if ((*it)->node == port_node)
                    {
                        watched_ports_.erase(it);
                        break;
                    }

                    ++it;
                }

            }

            /**
             * Forces Wake-up of the checking thread
             */
            void wake_up()
            {
                {
                    std::lock_guard<std::mutex> lock(wake_run_mutex_);
                    wake_run_ = true;
                }

                wake_run_cv_.notify_one();
            }

        private:

            std::vector<std::shared_ptr<PortContext> > watched_ports_;
            std::thread thread_run_;
            std::mutex watched_ports_mutex_;

            std::condition_variable wake_run_cv_;
            std::mutex wake_run_mutex_;
            bool wake_run_;

            bool exit_thread_;

            std::function<void(
                        const std::vector<const BufferDescriptor*>&,
                        const std::string& domain_name)> on_failure_buffer_descriptors_handler_;

            bool is_on_failure_buffer_descriptors_handler_set_;

            Watchdog()
                : wake_run_(false)
                , exit_thread_(false)
                , is_on_failure_buffer_descriptors_handler_set_(false)
            {
                thread_run_ = std::thread(&Watchdog::run, this);
            }

            ~Watchdog()
            {
                exit_thread_ = true;
                wake_up();
                thread_run_.join();
            }

            bool update_status_all_listeners(
                    PortNode* port_node)
            {
                for (uint32_t i = 0; i < port_node->num_listeners; i++)
                {
                    auto& status = port_node->listeners_status[i];
                    // Check only currently waiting listeners
                    if (status.is_waiting)
                    {
                        if (status.counter != status.last_verified_counter)
                        {
                            status.last_verified_counter = status.counter;
                        }
                        else         // Counter is freeze => this listener is blocked!!!
                        {
                            return false;
                        }
                    }
                }

                port_node->last_listeners_status_check_time_ms.exchange(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::high_resolution_clock::now().time_since_epoch()).count());

                return true;
            }

            void run()
            {
                while (!exit_thread_)
                {
                    {
                        std::unique_lock<std::mutex> lock(wake_run_mutex_);

                        wake_run_cv_.wait_for(
                            lock,
                            std::chrono::seconds(1),
                            [&] {
                            return wake_run_;
                        });

                        wake_run_ = false;
                    }

                    auto now = std::chrono::high_resolution_clock::now();

                    std::lock_guard<std::mutex> lock(watched_ports_mutex_);

                    auto port_it =  watched_ports_.begin();
                    while (port_it != watched_ports_.end())
                    {
                        // If more than 'healthy_check_timeout_ms' milliseconds elapsed since last check
                        if (std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()
                                - (*port_it)->node->last_listeners_status_check_time_ms.load()
                                > (*port_it)->node->healthy_check_timeout_ms)
                        {
                            std::vector<const BufferDescriptor*> descriptors_enqueued;

                            try
                            {
                                std::unique_lock<SharedMemSegment::mutex> lock_port((*port_it)->node->empty_cv_mutex);
                                if (!update_status_all_listeners((*port_it)->node))
                                {
                                    if ((*port_it)->node->is_port_ok)
                                    {
                                        (*port_it)->node->is_port_ok = false;
                                        (*port_it)->buffer->copy(&descriptors_enqueued);
                                        assert(on_failure_buffer_descriptors_handler_);
                                        on_failure_buffer_descriptors_handler_(descriptors_enqueued,
                                                (*port_it)->node->domain_name);
                                    }
                                }

                                ++port_it;
                            }
                            catch (std::exception& e)
                            {
                                (*port_it)->node->is_port_ok = false;

                                logWarning(RTPS_TRANSPORT_SHM, "Port " << (*port_it)->node->port_id
                                    << " error: " << e.what());

                                // Remove the port from watch
                                port_it = watched_ports_.erase(port_it);
                            }
                        }
                        else
                        {
                            ++port_it;
                        }
                    }
                }
            }
        };

        bool check_status_all_listeners() const
        {
            for (uint32_t i = 0; i < node_->num_listeners; i++)
            {
                auto& status = node_->listeners_status[i];
                // Check only currently waiting listeners
                if (status.is_waiting)
                {
                    if (status.counter == status.last_verified_counter)
                    {
                        return false;
                    }
                }
            }

            return true;
        }
        
    public:

        /**
         * Defines open sharing mode of a shared-memory port:
         *
         * ReadShared (multiple listeners / multiple writers): Once a port is opened ReadShared cannot be opened ReadExclusive.
         *
         * ReadExclusive (one listener / multipler writers): Once a port is opened ReadExclusive cannot be opened ReadShared.
         *
         * Write (multiple writers): A port can always be opened for writing.
         */
        enum class OpenMode 
        { 
            ReadShared, 
            ReadExclusive, 
            Write 
        };

        static std::string open_mode_to_string(
                OpenMode open_mode)
        {
            switch (open_mode)
            {
                case OpenMode::ReadShared: return "ReadShared";
                case OpenMode::ReadExclusive: return "ReadExclusive";
                case OpenMode::Write: return "Write";
            }

            return "";
        }

        Port(
                std::shared_ptr<SharedMemSegment>&& port_segment,
                PortNode* node)
            : port_segment_(std::move(port_segment))
            , node_(node)
            , overflows_count_(0)
        {
            auto buffer_base = static_cast<MultiProducerConsumerRingBuffer<BufferDescriptor>::Cell*>(
                port_segment_->get_address_from_offset(node_->buffer));

            auto buffer_node = static_cast<MultiProducerConsumerRingBuffer<BufferDescriptor>::Node*>(
                port_segment_->get_address_from_offset(node_->buffer_node));

            buffer_ = std::unique_ptr<MultiProducerConsumerRingBuffer<BufferDescriptor>>(
                new MultiProducerConsumerRingBuffer<BufferDescriptor>(buffer_base, buffer_node));

            node_->ref_counter.fetch_add(1);

            auto port_context = std::make_shared<Watchdog::PortContext>();
            *port_context = {port_segment_, node_, buffer_.get()};
            Watchdog::get().add_port_to_watch(std::move(port_context));
        }

        ~Port()
        {
            Watchdog::get().remove_port_from_watch(node_);

            if (node_->ref_counter.fetch_sub(1) == 1 && node_->is_port_ok)
            {                
                auto segment_name = port_segment_->name();

                logInfo(RTPS_TRANSPORT_SHM, THREADID << "Port " << node_->port_id
                        << segment_name.c_str() << " removed." << "overflows_count " 
                        << overflows_count_);

                if (overflows_count_)
                {
                    logWarning(RTPS_TRANSPORT_SHM, "Port " << node_->port_id
                        << segment_name.c_str() << " had overflows_count " 
                        << overflows_count_);
                }

                port_segment_.reset();

                SharedMemSegment::remove(segment_name.c_str());
                SharedMemSegment::named_mutex::remove((segment_name + "_mutex").c_str());
            }
        }

        static void on_failure_buffer_descriptors_handler(
                std::function<void(
                    const std::vector<const BufferDescriptor*>&,
                    const std::string& domain_name)> handler)

        {
            Watchdog::get().on_failure_buffer_descriptors_handler(handler);
        }

        /**
         * Try to enqueue a buffer descriptor in the port.
         * If the port queue is full returns inmediatelly with false value.
         * @param[in] buffer_descriptor buffer descriptor to be enqueued
         * @param[out] listeners_active false if no active listeners => buffer not enqueued
         * @return false in overflow case, true otherwise.
         */
        bool try_push(
                const BufferDescriptor& buffer_descriptor,
                bool* listeners_active)
        {
            std::unique_lock<SharedMemSegment::mutex> lock_empty(node_->empty_cv_mutex);

            if (!node_->is_port_ok)
            {
                throw std::runtime_error("the port is marked as not ok!");
            }

            try
            {
                bool was_opened_as_unicast_port = node_->is_opened_read_exclusive;
                bool was_buffer_empty_before_push = buffer_->is_buffer_empty();
                bool was_someone_listening = (node_->waiting_count > 0);

                *listeners_active = buffer_->push(buffer_descriptor);

                lock_empty.unlock();

                if (was_someone_listening)
                {
                    if (was_opened_as_unicast_port)
                    {
                        notify_unicast(was_buffer_empty_before_push);
                    }
                    else
                    {
                        notify_multicast();
                    }
                }

                return true;
            }
            catch (const std::exception&)
            {
                lock_empty.unlock();
                overflows_count_++;
            }
            return false;
        }

        /**
         * Waits while the port is empty and listener is not closed
         * @param[in] listener reference to the listener that will wait for an incoming buffer descriptor.
         * @param[in] is_listener_closed this reference can become true in the middle of the waiting process,
         * @param[in] listener_index to update the port's listener_status,
         * if that happens wait is aborted.
         */
        void wait_pop(
                Listener& listener,
                const std::atomic<bool>& is_listener_closed,
                uint32_t listener_index)
        {
            try
            {
                std::unique_lock<SharedMemSegment::mutex> lock(node_->empty_cv_mutex);

                if (!node_->is_port_ok)
                {
                    throw std::runtime_error("port marked as not ok");
                }

                auto& status = node_->listeners_status[listener_index];
                // Update this listener status
                status.is_waiting = 1;
                status.counter = status.last_verified_counter + 1;
                node_->waiting_count++;

                do
                {
                    boost::system_time const timeout =
                            boost::get_system_time()+ boost::posix_time::milliseconds(node_->port_wait_timeout_ms);

                    if (node_->empty_cv.timed_wait(lock, timeout, [&] 
                        {
                            return is_listener_closed.load() || listener.head() != nullptr;
                        }))
                    {
                        break; // Codition met, Break the while
                    }
                    else // Timeout
                    {
                        if (!node_->is_port_ok)
                        {
                            throw std::runtime_error("port marked as not ok");
                        }

                        status.counter = status.last_verified_counter + 1;
                    }
                } while (1);

                node_->waiting_count--;
                status.is_waiting = 0;

            }
            catch (const std::exception&)
            {
                node_->is_port_ok = false;
                throw;
            }
        }

        inline bool is_port_ok() const
        {
            return node_->is_port_ok;
        }

        inline uint32_t port_id() const
        {
            return node_->port_id;
        }

        inline OpenMode open_mode() const
        {
            if(node_->is_opened_for_reading)
            {
                return node_->is_opened_read_exclusive ? OpenMode::ReadExclusive : OpenMode::ReadShared;
            }
            
            return OpenMode::Write;
        }

        inline uint32_t healthy_check_timeout_ms() const
        {
            return node_->healthy_check_timeout_ms;
        }

        inline uint32_t max_buffer_descriptors() const
        {
            return node_->max_buffer_descriptors;
        }

        /**
         * Set the caller's 'is_closed' flag (protecting empty_cv_mutex) and
         * forces wake-up all listeners on this port.
         * This function is used when destroying a listener waiting for messages
         * in the port.
         * @param is_listener_closed pointer to the atomic is_closed flag of the Listener object.
         */
        void close_listener(
                std::atomic<bool>* is_listener_closed)
        {
            {
                std::lock_guard<SharedMemSegment::mutex> lock(node_->empty_cv_mutex);
                is_listener_closed->exchange(true);
            }

            node_->empty_cv.notify_all();
        }

        /**
         * Removes the head buffer-descriptor from the listener's queue
         * @param [in] listener reference to the listener that will pop the buffer descriptor.
         * @param [out] was_cell_freed is true if the port's cell is freed because all listeners has poped the cell
         * @throw std::runtime_error if buffer is empty
         */
        void pop(
                Listener& listener,
                bool& was_cell_freed)
        {
            was_cell_freed = listener.pop();
        }

        /**
         * Register a new listener
         * The new listener's read pointer is equal to the ring-buffer write pointer at the registering moment.
         * @param [out] listener_index pointer to where the index of the listener is returned. This index is
         * used to reference the elements from the listeners_status array.
         * @return A shared_ptr to the listener.
         * The listener will be unregistered when shared_ptr is destroyed.
         */
        std::shared_ptr<Listener> create_listener(uint32_t* listener_index)
        {
            std::lock_guard<SharedMemSegment::mutex> lock(node_->empty_cv_mutex);

            *listener_index = node_->num_listeners++;

            return buffer_->register_listener();
        }

        /**
         * Decrement the number of listeners by one
         */
        void unregister_listener()
        {
            std::lock_guard<SharedMemSegment::mutex> lock(node_->empty_cv_mutex);

            node_->num_listeners--;
        }

        /**
         * Performs a check on the opened port.
         * When a process crashes with a port opened the port can be left inoperative.
         * @throw std::exception if the port is inoperative.
         */
        void healthy_check()
        {
            if (!node_->is_port_ok)
            {
                throw std::runtime_error("port is marked as not ok");
            }

            auto t0 = std::chrono::high_resolution_clock::now();

            // If in any moment during the timeout all waiting listeners are OK
            // then the port is OK
            bool is_check_ok = false;
            while ( !is_check_ok &&
                    std::chrono::duration_cast<std::chrono::milliseconds>
                        (std::chrono::high_resolution_clock::now() - t0).count() < node_->healthy_check_timeout_ms)
            {
                {
                    std::unique_lock<SharedMemSegment::mutex> lock(node_->empty_cv_mutex);
                    is_check_ok = check_status_all_listeners();

                    if (!node_->is_port_ok)
                    {
                        throw std::runtime_error("port marked as not ok");
                    }
                }

                if (!is_check_ok)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(node_->port_wait_timeout_ms));
                }
            }

            if (!is_check_ok || !node_->is_port_ok)
            {
                throw std::runtime_error("healthy_check failed");
            }
        }
    }; // Port

    /**
     * Open a shared-memory port. If the port doesn't exist in the system a port with port_id is created,
     * otherwise the existing port is opened.
     *
     * @param [in] port_id Identifies the port
     * @param [in] max_buffer_descriptors Capacity of the port (only used if the port is created)
     * @param [in] healthy_check_timeout_ms Timeout for healthy check test
     * @param [in] open_mode Can be ReadShared, ReadExclusive or Write (see Port::OpenMode enum).
     *
     * @remarks This function performs a test to validate whether the existing port is OK, if the test
     * goes wrong the existing port is removed from shared-memory and a new port is created.
     */
    std::shared_ptr<Port> open_port(
            uint32_t port_id,
            uint32_t max_buffer_descriptors,
            uint32_t healthy_check_timeout_ms,
            Port::OpenMode open_mode = Port::OpenMode::ReadShared)
    {
        std::string err_reason;
        std::shared_ptr<Port> port;

        auto port_segment_name = domain_name_ + "_port" + std::to_string(port_id);

        logInfo(RTPS_TRANSPORT_SHM, THREADID << "Opening " 
            << port_segment_name);

        std::unique_ptr<SharedMemSegment::named_mutex> port_mutex =
                SharedMemSegment::open_or_create_and_lock_named_mutex(port_segment_name + "_mutex");

        std::unique_lock<SharedMemSegment::named_mutex> port_lock(*port_mutex, std::adopt_lock);

        try
        {
            // Try to open
            auto port_segment = std::shared_ptr<SharedMemSegment>(
                new SharedMemSegment(boost::interprocess::open_only, port_segment_name.c_str()));

            SharedMemGlobal::PortNode* port_node;

            try
            {
                port_node = port_segment->get().find<PortNode>("port_node").first;
                port = std::make_shared<Port>(std::move(port_segment), port_node);
            }
            catch (std::exception&)
            {
                logWarning(RTPS_TRANSPORT_SHM, THREADID << "Port "
                    << port_id << " Couldn't find port_node ");

                SharedMemSegment::remove(port_segment_name.c_str());

                logWarning(RTPS_TRANSPORT_SHM, THREADID << "Port "
                    << port_id << " Removed.");

                throw;
            }

            try
            {                
                port->healthy_check();

                if ( (port_node->is_opened_read_exclusive && open_mode != Port::OpenMode::Write) ||
                        (port_node->is_opened_for_reading && open_mode == Port::OpenMode::ReadExclusive))
                {
                    std::stringstream ss;

                    ss << port_node->port_id << " (" << port_node->uuid.to_string() <<
                                        ") because is already opened ReadExclusive";

                    err_reason = ss.str();
                    port.reset();
                }
                else
                {
                    port_node->is_opened_read_exclusive |= (open_mode == Port::OpenMode::ReadExclusive);
                    port_node->is_opened_for_reading |= (open_mode != Port::OpenMode::Write);

                    logInfo(RTPS_TRANSPORT_SHM, THREADID << "Port "
                                                         << port_node->port_id << " (" << port_node->uuid.to_string() <<
                                        ") Opened" << Port::open_mode_to_string(open_mode));
                }
            }
            catch (std::exception&)
            {
                auto port_uuid = port_node->uuid.to_string();
                
                logWarning(RTPS_TRANSPORT_SHM, THREADID << "Existing Port "
                                                        << port_id << " (" << port_uuid << ") NOT Healthy.");           

                SharedMemSegment::remove(port_segment_name.c_str());

                logWarning(RTPS_TRANSPORT_SHM, THREADID << "Port "
                                                        << port_id << " (" << port_uuid << ") Removed.");

                throw;
            }
        }
        catch (std::exception&)
        {
            // Doesn't exist => create it
            // The segment will contain the node, the buffer and the internal allocator structures (512bytes estimated)
            uint32_t extra = 512;
            uint32_t segment_size = sizeof(PortNode) + sizeof(PortCell) * max_buffer_descriptors;

            try
            {
                auto port_segment = std::unique_ptr<SharedMemSegment>(
                    new SharedMemSegment(boost::interprocess::create_only, port_segment_name.c_str(),
                    segment_size + extra));

                // Memset the whole segment to zero in order to force physical map of the buffer
                auto payload = port_segment->get().allocate(segment_size);
                memset(payload, 0, segment_size);
                port_segment->get().deallocate(payload);

                port = init_port(port_id, port_segment, max_buffer_descriptors, open_mode, healthy_check_timeout_ms);
            }
            catch (std::exception& e)
            {
                logError(RTPS_TRANSPORT_SHM, "Failed to create port segment " << port_segment_name
                                                                              << ": " << e.what());

                throw;
            }
        }

        if (port == nullptr)
        {
            throw std::runtime_error("Coulnd't open port " + err_reason);
        }

        return port;
    }

private:

    std::string domain_name_;

    std::shared_ptr<Port> init_port(
            uint32_t port_id,
            std::unique_ptr<SharedMemSegment>& segment,
            uint32_t max_buffer_descriptors,
            Port::OpenMode open_mode,
            uint32_t healthy_check_timeout_ms)
    {
        std::shared_ptr<Port> port;
        PortNode* port_node = nullptr;
        MultiProducerConsumerRingBuffer<BufferDescriptor>::Node* buffer_node = nullptr;

        try
        {
            // Port node allocation
            port_node = segment->get().construct<PortNode>("port_node")();
            port_node->port_id = port_id;
            port_node->is_port_ok = true;
            UUID<8>::generate(port_node->uuid);
            port_node->waiting_count = 0;
            port_node->is_opened_read_exclusive = (open_mode == Port::OpenMode::ReadExclusive);
            port_node->is_opened_for_reading = (open_mode != Port::OpenMode::Write);
            port_node->num_listeners = 0;
            port_node->healthy_check_timeout_ms = healthy_check_timeout_ms;
            port_node->last_listeners_status_check_time_ms = 
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::high_resolution_clock::now().time_since_epoch()).count();
            port_node->port_wait_timeout_ms = healthy_check_timeout_ms / 3;
            port_node->max_buffer_descriptors = max_buffer_descriptors;
            memset(port_node->listeners_status, 0, sizeof(port_node->listeners_status));
#ifdef _MSC_VER
            strncpy_s(port_node->domain_name, sizeof(port_node->domain_name), 
                domain_name_.c_str(), sizeof(port_node->domain_name)-1); 
#else
            strncpy(port_node->domain_name, domain_name_.c_str(), sizeof(port_node->domain_name)-1); 
#endif
            port_node->domain_name[sizeof(port_node->domain_name)-1] = 0;

            // Buffer cells allocation
            auto buffer = segment->get().construct<MultiProducerConsumerRingBuffer<BufferDescriptor>::Cell>(
                boost::interprocess::anonymous_instance)[max_buffer_descriptors]();            
            port_node->buffer = segment->get_offset_from_address(buffer);            

            // Buffer node allocation
            buffer_node = segment->get().construct<MultiProducerConsumerRingBuffer<BufferDescriptor>::Node>(
                boost::interprocess::anonymous_instance)();

            MultiProducerConsumerRingBuffer<BufferDescriptor>::init_node(buffer_node, max_buffer_descriptors);

            port_node->buffer_node = segment->get_offset_from_address(buffer_node);

            port = std::make_shared<Port>(std::move(segment), port_node);

            logInfo(RTPS_TRANSPORT_SHM, THREADID << "Port "
                << port_node->port_id << " (" << port_node->uuid.to_string() 
                << Port::open_mode_to_string(open_mode) << ") Created.");
        }
        catch (const std::exception&)
        {
            if (port_node)
            {
                segment->get().destroy_ptr(port_node);
            }

            if (buffer_node)
            {
                segment->get().destroy_ptr(buffer_node);
            }

            throw;
        }

        return port;
    }
};

} // namespace rtps
} // namespace fastdds
} // namespace eprosima

#endif // _FASTDDS_SHAREDMEM_GLOBAL_H_