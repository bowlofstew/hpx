//  Copyright (c) 2014 Thomas Heller
//  Copyright (c) 2007-2014 Hartmut Kaiser
//  Copyright (c) 2007 Richard D Guidry Jr
//  Copyright (c) 2011 Bryce Lelbach
//  Copyright (c) 2011 Katelyn Kufahl
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef HPX_PARCELSET_PARCELPORT_IMPL_HPP
#define HPX_PARCELSET_PARCELPORT_IMPL_HPP

#include <hpx/runtime/parcelset/parcelport.hpp>
#include <hpx/runtime/parcelset/encode_parcels.hpp>
#include <hpx/runtime/parcelset/connections.hpp>
#include <hpx/runtime/parcelset/allocator.hpp>
#include <hpx/runtime/parcelset/detail/call_for_each.hpp>
#include <hpx/runtime/threads/thread.hpp>
#include <hpx/state.hpp>
#include <hpx/util/io_service_pool.hpp>
#include <hpx/util/connection_cache.hpp>
#include <hpx/util/runtime_configuration.hpp>

///////////////////////////////////////////////////////////////////////////////
namespace hpx
{
    bool is_starting();
    bool is_stopped();
}

namespace hpx { namespace parcelset
{
    ///////////////////////////////////////////////////////////////////////////
    template <typename ConnectionHandler>
    struct connection_handler_traits;

    template <typename ConnectionHandler>
    class HPX_EXPORT parcelport_impl
      : public parcelport
    {
        typedef
            typename connection_handler_traits<ConnectionHandler>::connection_type
            connection;
    public:

        template <typename Trait>
        struct get_buffer_type
        {
            typedef typename Trait::buffer_type type;
        };

        typedef
            typename boost::mpl::eval_if<
                typename connection_handler_traits<ConnectionHandler>::provides_get_buffer
              , get_buffer_type<connection_handler_traits<ConnectionHandler> >
              , boost::mpl::identity<std::vector<char, allocator> >
            >::type
            buffer_type;
        typedef
            parcel_buffer<buffer_type, util::serialization_chunk>
            sender_parcel_buffer_type;

        typedef
            parcel_buffer<buffer_type, std::vector<char> >
            receiver_parcel_buffer_type;

        template <typename Connection>
        struct get_parcel_buffer_type
        {
            typedef typename
                boost::mpl::if_<
                    typename boost::is_same<Connection, connection>::type
                  , sender_parcel_buffer_type
                  , receiver_parcel_buffer_type
                >::type
                type;
        };

        static const char * connection_handler_name()
        {
            return connection_handler_traits<ConnectionHandler>::name();
        }

        static std::size_t thread_pool_size(util::runtime_configuration const& ini)
        {
            std::string key("hpx.parcel.");
            key += connection_handler_name();

            std::string thread_pool_size =
                ini.get_entry(key + ".io_pool_size", "2");
            return boost::lexical_cast<std::size_t>(thread_pool_size);
        }

        static const char *pool_name()
        {
            return connection_handler_traits<ConnectionHandler>::pool_name();
        }

        static const char *pool_name_postfix()
        {
            return connection_handler_traits<ConnectionHandler>::pool_name_postfix();
        }

        static std::size_t max_connections(util::runtime_configuration const& ini)
        {
            std::string key("hpx.parcel.");
            key += connection_handler_name();

            std::string max_connections =
                ini.get_entry(key + ".max_connections",
                    HPX_PARCEL_MAX_CONNECTIONS);
            return boost::lexical_cast<std::size_t>(max_connections);
        }

        static std::size_t max_connections_per_loc(util::runtime_configuration const& ini)
        {
            std::string key("hpx.parcel.");
            key += connection_handler_name();

            std::string max_connections_per_locality =
                ini.get_entry(key + ".max_connections_per_locality",
                    HPX_PARCEL_MAX_CONNECTIONS_PER_LOCALITY);
            return boost::lexical_cast<std::size_t>(max_connections_per_locality);
        }

        static std::size_t memory_chunk_size(util::runtime_configuration const& ini)
        {
            std::string memory_chunk_size =
                ini.get_entry("hpx.parcel.memory_chunk_size", "2048");
            return boost::lexical_cast<std::size_t>(memory_chunk_size);
        }

        static std::size_t max_memory_chunks(util::runtime_configuration const& ini)
        {
            std::string max_memory_chunks =
                ini.get_entry("hpx.parcel.max_memory_chunks", "512");
            return boost::lexical_cast<std::size_t>(max_memory_chunks);
        }

    public:
        /// Construct the parcelport on the given locality.
        parcelport_impl(util::runtime_configuration const& ini,
            HPX_STD_FUNCTION<void(std::size_t, char const*)> const& on_start_thread,
            HPX_STD_FUNCTION<void()> const& on_stop_thread)
          : parcelport(ini, connection_handler_name())
          , io_service_pool_(thread_pool_size(ini),
                on_start_thread, on_stop_thread, pool_name(), pool_name_postfix())
          //, connection_cache_(max_connections(ini), max_connections_per_loc(ini))
          , connections_(max_connections_per_loc(ini))
          , archive_flags_(boost::archive::no_header)
          , memory_pool_(memory_chunk_size(ini), max_memory_chunks(ini))
        {
#ifdef BOOST_BIG_ENDIAN
            std::string endian_out = get_config_entry("hpx.parcel.endian_out", "big");
#else
            std::string endian_out = get_config_entry("hpx.parcel.endian_out", "little");
#endif
            if (endian_out == "little")
                archive_flags_ |= util::endian_little;
            else if (endian_out == "big")
                archive_flags_ |= util::endian_big;
            else {
                HPX_ASSERT(endian_out =="little" || endian_out == "big");
            }

            if (!this->allow_array_optimizations()) {
                archive_flags_ |= util::disable_array_optimization;
                archive_flags_ |= util::disable_data_chunking;
            }
            else {
                if (!this->allow_zero_copy_optimizations())
                    archive_flags_ |= util::disable_data_chunking;
            }

            std::string num_encode_threads = get_config_entry("hpx.parcel.num_encode_threads", "4");
            num_encode_threads_ = boost::lexical_cast<std::size_t>(num_encode_threads);
            encode_threads_.reserve(num_encode_threads_);
        }

        ~parcelport_impl()
        {
            //connection_cache_.clear();
            connections_.clear();
        }

        bool run(bool blocking = true)
        {
            io_service_pool_.run(false);    // start pool

            bool success = connection_handler().do_run();

            if (blocking)
                io_service_pool_.join();

            return success;
        }

        void stop(bool blocking = true)
        {
            // make sure no more work is pending, wait for service pool to get empty
            io_service_pool_.stop();
            if (blocking) {
                //connection_cache_.shutdown();
                connection_handler().do_stop();
                io_service_pool_.join();
                //connection_cache_.clear();
                connections_.clear();
                io_service_pool_.clear();
            }
        }

        void put_parcel(parcel p, write_handler_type f)
        {
            naming::locality const& locality_id = p.get_destination_locality();

            // enqueue the outgoing parcel ...
            enqueue_parcel(locality_id, std::move(p), std::move(f));

            // start actual parcel encoding
            schedule_parcel_encoding();
        }

        void put_parcels(std::vector<parcel> parcels,
            std::vector<write_handler_type> handlers)
        {
            if (parcels.size() != handlers.size())
            {
                HPX_THROW_EXCEPTION(bad_parameter, "parcelport::put_parcels",
                    "mismatched number of parcels and handlers");
                return;
            }

            naming::locality const& locality_id =
                parcels[0].get_destination_locality();


#if defined(HPX_DEBUG)
            // make sure all parcels go to the same locality
            for (std::size_t i = 1; i != parcels.size(); ++i)
            {
                HPX_ASSERT(locality_id == parcels[i].get_destination_locality());
            }
#endif

            // enqueue the outgoing parcels ...
            HPX_ASSERT(parcels.size() == handlers.size());
            enqueue_parcels(locality_id, std::move(parcels), std::move(handlers));

            // start actual parcel encoding
            schedule_parcel_encoding();
        }

        void send_early_parcel(parcel& p)
        {
            send_early_parcel_impl<ConnectionHandler>(p);
        }

        util::io_service_pool* get_thread_pool(char const* name)
        {
            if (0 == std::strcmp(name, io_service_pool_.get_name()))
                return &io_service_pool_;
            return 0;
        }

        void do_background_work()
        {
            do_background_work_impl<ConnectionHandler>();
            //schedule_parcel_encoding();
            trigger_writes();
        }

        /// support enable_shared_from_this
        boost::shared_ptr<parcelport_impl> shared_from_this()
        {
            return boost::static_pointer_cast<parcelport_impl>(
                parcelset::parcelport::shared_from_this());
        }

        boost::shared_ptr<parcelport_impl const> shared_from_this() const
        {
            return boost::static_pointer_cast<parcelport_impl const>(
                parcelset::parcelport::shared_from_this());
        }

        virtual std::string get_locality_name() const
        {
            return connection_handler().get_locality_name();
        }

        /// Cache specific functionality
        void remove_from_connection_cache(naming::locality const& loc)
        {
            //connection_cache_.clear(loc);
            connections_.clear(loc);
        }

        /// Temporarily enable/disable all parcel handling activities in the
        /// parcelport
        void enable(bool new_state)
        {
            enable_parcel_handling_ = new_state;
            do_enable_parcel_handling_impl<ConnectionHandler>(new_state);
            if (new_state)
                do_background_work();
        }

        boost::shared_ptr<sender_parcel_buffer_type>
        get_sender_buffer(parcel const & p = parcel(), std::size_t arg_size = 0)
        {
            return get_sender_buffer_impl<ConnectionHandler>(p, arg_size);
        }

        boost::shared_ptr<receiver_parcel_buffer_type>
        get_receiver_buffer(parcel const & p = parcel(), std::size_t arg_size = 0)
        {
            return get_receiver_buffer_impl<ConnectionHandler>(p, arg_size);
        }

        /////////////////////////////////////////////////////////////////////////
        // Return the given connection cache statistic
        boost::int64_t get_connection_cache_statistics(
            connection_cache_statistics_type t, bool reset)
        {
            switch (t) {
            /*
                case connection_cache_insertions:
                    return connection_cache_.get_cache_insertions(reset);

                case connection_cache_evictions:
                    return connection_cache_.get_cache_evictions(reset);

                case connection_cache_hits:
                    return connection_cache_.get_cache_hits(reset);

                case connection_cache_misses:
                    return connection_cache_.get_cache_misses(reset);

                case connection_cache_reclaims:
                    return connection_cache_.get_cache_reclaims(reset);

            */
                case connection_cache_insertions:
                    return connections_.insertions_;

                case connection_cache_hits:
                    return connections_.hits_;

                case connection_cache_misses:
                    return connections_.misses_;

                case connection_cache_reclaims:
                    return connections_.reclaims_;
                default:
                    break;
            }

            /*
            HPX_THROW_EXCEPTION(bad_parameter,
                "parcelport_impl::get_connection_cache_statistics",
                "invalid connection cache statistics type");
            */
            return 0;
        }

        bool dequeue_out_buffers(
            naming::locality const & locality
          , boost::shared_ptr<sender_parcel_buffer_type> & buffer
          , detail::call_for_each & handlers
        )
        {
            typedef typename out_buffer_map_type::iterator iterator;
            if(!enable_parcel_handling_)
                return false;

            {
                lcos::local::spinlock::scoped_lock l(out_buffer_mtx_);
                iterator it = out_buffer_map_.find(locality);

                // do nothing if out buffers have already been picked up
                // by another thread.
                if(it == out_buffer_map_.end())
                {
                    return false;
                }
                HPX_ASSERT(!it->second.empty());

                HPX_ASSERT(it->first == locality);
                HPX_ASSERT(!buffer);
                HPX_ASSERT(handlers.fv_.empty());

                typename out_buffer_type::value_type & out_buffer = it->second.back();
                buffer = out_buffer.first;
                std::swap(handlers, out_buffer.second);
                it->second.pop_back();
                if(it->second.empty())
                {
                    out_buffer_map_.erase(it);
                    parcel_destinations_.erase(locality);
                }

                return true;
            }
        }

        void enqueue_out_buffers(
            naming::locality const & locality
          , boost::shared_ptr<sender_parcel_buffer_type> buffer
          , detail::call_for_each handlers
        )
        {
            lcos::local::spinlock::scoped_lock l(out_buffer_mtx_);

            typedef typename out_buffer_map_type::mapped_type mapped_type;

            mapped_type & out_buffers = out_buffer_map_[locality];
            out_buffers.push_back(
                std::make_pair(
                    buffer
                  , std::move(handlers)
                )
            );

            parcel_destinations_.insert(locality);
        }

    private:
        ConnectionHandler & connection_handler()
        {
            return static_cast<ConnectionHandler &>(*this);
        }

        ConnectionHandler const & connection_handler() const
        {
            return static_cast<ConnectionHandler const &>(*this);
        }

        ///////////////////////////////////////////////////////////////////////////
        // the code below is needed to bootstrap the parcel layer
        void early_pending_parcel_handler(
            boost::system::error_code const& ec, parcel const & p)
        {
            if (ec) {
                // all errors during early parcel handling are fatal
                try {
                    HPX_THROW_EXCEPTION(network_error, "early_write_handler",
                        "error while handling early parcel: " +
                            ec.message() + "(" +
                            boost::lexical_cast<std::string>(ec.value())+ ")");
                }
                catch (hpx::exception const& e) {
                    hpx::detail::report_exception_and_terminate(e);
                }
                return;
            }
        }

        template <typename ConnectionHandler_>
        typename boost::enable_if<
            typename connection_handler_traits<
                ConnectionHandler_
            >::send_early_parcel
        >::type
        send_early_parcel_impl(parcel& p)
        {
            put_parcel(
                p
              , boost::bind(
                    &parcelport_impl::early_pending_parcel_handler
                  , this
                  , ::_1
                  , p
                )
            );
        }

        template <typename ConnectionHandler_>
        typename boost::disable_if<
            typename connection_handler_traits<
                ConnectionHandler_
            >::send_early_parcel
        >::type
        send_early_parcel_impl(parcel& p)
        {
            HPX_THROW_EXCEPTION(network_error, "send_early_parcel",
                "This parcelport does not support sending early parcels");
        }

        template <typename ConnectionHandler_>
        typename boost::enable_if<
            typename connection_handler_traits<
                ConnectionHandler_
            >::do_background_work
        >::type
        do_background_work_impl()
        {
            connection_handler().background_work();
        }

        template <typename ConnectionHandler_>
        typename boost::disable_if<
            typename connection_handler_traits<
                ConnectionHandler_
            >::do_background_work
        >::type
        do_background_work_impl()
        {
        }

        ///////////////////////////////////////////////////////////////////////
        template <typename ConnectionHandler_>
        typename boost::enable_if<
            typename connection_handler_traits<
                ConnectionHandler_
            >::do_enable_parcel_handling
        >::type
        do_enable_parcel_handling_impl(bool new_state)
        {
            connection_handler().enable_parcel_handling(new_state);
        }

        template <typename ConnectionHandler_>
        typename boost::disable_if<
            typename connection_handler_traits<
                ConnectionHandler_
            >::do_enable_parcel_handling
        >::type
        do_enable_parcel_handling_impl(bool new_state)
        {
        }

        template <typename ConnectionHandler_>
        typename boost::enable_if<
            typename connection_handler_traits<
                ConnectionHandler_
            >::provides_get_buffer
          , boost::shared_ptr<sender_parcel_buffer_type>
        >::type
        get_sender_buffer_impl(parcel const & p = parcel(), std::size_t arg_size = 0)
        {
            return connection_handler().get_sender_buffer(p, arg_size);
        }

        template <typename ConnectionHandler_>
        typename boost::disable_if<
            typename connection_handler_traits<
                ConnectionHandler_
            >::provides_get_buffer
          , boost::shared_ptr<sender_parcel_buffer_type>
        >::type
        get_sender_buffer_impl(parcel const & p = parcel(), std::size_t arg_size = 0)
        {
            boost::shared_ptr<sender_parcel_buffer_type> buffer;
            buffer = boost::make_shared<sender_parcel_buffer_type>(allocator(memory_pool_));
            buffer->data_.reserve(arg_size);
            return buffer;
        }

        template <typename ConnectionHandler_>
        typename boost::enable_if<
            typename connection_handler_traits<
                ConnectionHandler_
            >::provides_get_buffer
          , boost::shared_ptr<receiver_parcel_buffer_type>
        >::type
        get_receiver_buffer_impl(parcel const & p = parcel(), std::size_t arg_size = 0)
        {
            return connection_handler().get_receiver_buffer(p, arg_size);
        }

        template <typename ConnectionHandler_>
        typename boost::disable_if<
            typename connection_handler_traits<
                ConnectionHandler_
            >::provides_get_buffer
          , boost::shared_ptr<receiver_parcel_buffer_type>
        >::type
        get_receiver_buffer_impl(parcel const & p = parcel(), std::size_t arg_size = 0)
        {
            boost::shared_ptr<receiver_parcel_buffer_type> buffer;
            buffer = boost::make_shared<receiver_parcel_buffer_type>(allocator(memory_pool_));
            buffer->data_.reserve(arg_size);
            return buffer;
        }

        ///////////////////////////////////////////////////////////////////////
        void enqueue_parcel(naming::locality const& locality_id,
            parcel&& p, write_handler_type&& f)
        {
            typedef pending_parcels_map::mapped_type mapped_type;

            lcos::local::spinlock::scoped_lock l(mtx_);

            mapped_type& e = pending_parcels_[locality_id];
            e.first.push_back(std::move(p));
            e.second.fv_.push_back(detail::call_for_each(std::move(f)));
        }

        void enqueue_parcels(naming::locality const& locality_id,
            std::vector<parcel>&& parcels,
            std::vector<write_handler_type>&& handlers)
        {
            typedef pending_parcels_map::mapped_type mapped_type;

            lcos::local::spinlock::scoped_lock l(mtx_);

            HPX_ASSERT(parcels.size() == handlers.size());

            mapped_type& e = pending_parcels_[locality_id];
            if (e.first.empty())
            {
                HPX_ASSERT(e.second.fv_.empty());
#if HPX_GCC_VERSION >= 40600 && HPX_GCC_VERSION < 40700
                // GCC4.6 gets incredibly confused
                std::swap(e.first, static_cast<std::vector<parcel>&>(parcels));
#else
                std::swap(e.first, parcels);
#endif
                e.second = detail::call_for_each(std::move(handlers));
            }
            else
            {
                HPX_ASSERT(e.first.size() == e.second.fv_.size());
                std::size_t new_size = e.first.size() + parcels.size();
                e.first.reserve(new_size);
                e.second.fv_.reserve(new_size);

                std::move(parcels.begin(), parcels.end(),
                    std::back_inserter(e.first));
                std::move(handlers.begin(), handlers.end(),
                    std::back_inserter(e.second.fv_));
            }
        }

        void schedule_parcel_encoding()
        {
            if(!enable_parcel_handling_) return;

            // Are we allowed to launch new threads?
            if(hpx::is_running() && async_serialization())
            {
                lcos::local::spinlock::scoped_lock l(encode_threads_mtx_);

                // Check if we can start another thread
                if(encode_threads_.size() > num_encode_threads_)
                {
                    // The number of active threads exceeds the maximum ...
                    // We need to schedule another encode thread, if all are
                    // suspended. This is a counter measurement to #1217
                    std::size_t num_suspended = 0;
                    BOOST_FOREACH(threads::thread_id_type & id, encode_threads_)
                    {
                        if(threads::get_thread_state(id) == threads::suspended)
                            ++num_suspended;
                    }

                    // If at least one is running, we don't need to schedule
                    // a new one.
                    if(num_suspended < encode_threads_.size())
                        return;
                }

                error_code ec(lightweight);
                std::size_t thread_num = get_worker_thread_num();
                threads::thread_id_type encode_thread = applier::register_thread_nullary(
                    util::bind(
                        &parcelport_impl::encode_parcels_impl
                      , this
                    )
                  , "encode_parcels"
                  , threads::pending
                  , true
                  , threads::thread_priority_boost
                  , thread_num
                  , threads::thread_stacksize_default
                  , ec
                );
                if(ec)
                {
                    // FIXME: throw exception on error ...
                }
                else
                {
                    encode_threads_.push_back(encode_thread);
                }

                return;
            }

            if(threads::get_self_ptr() != 0)
            {
                lcos::local::spinlock::scoped_lock l(encode_threads_mtx_);
                encode_threads_.push_back(threads::get_self_id());
            }
            //std::cout << "non async parcel encoding ...\n";
            encode_parcels_impl();
        }

        struct encoding_parcels
        {
            encoding_parcels(
                hpx::lcos::local::spinlock & mtx
              , std::vector<threads::thread_id_type> & encode_threads)
              : mtx_(mtx)
              , thread_id_(threads::get_self_id())
              , encode_threads_(encode_threads)
            {
#if defined(HPX_DEBUG)
                if(threads::get_self_ptr() == 0) return;
                hpx::lcos::local::spinlock::scoped_lock l(mtx_);
                HPX_ASSERT(encode_threads_.size() > 0);
#endif
            }

            ~encoding_parcels()
            {
                if(threads::get_self_ptr() == 0) return;
                hpx::lcos::local::spinlock::scoped_lock l(mtx_);
                HPX_ASSERT(encode_threads_.size() > 0);

                std::vector<threads::thread_id_type>::iterator it;
                it = std::find(encode_threads_.begin(), encode_threads_.end(), thread_id_);
                HPX_ASSERT(it != encode_threads_.end());
                encode_threads_.erase(it);
            }

            hpx::lcos::local::spinlock & mtx_;
            threads::thread_id_type thread_id_;
            std::vector<threads::thread_id_type> & encode_threads_;
        };

        void encode_parcels_impl()
        {
            // reschedule ourself as HPX thread if we are not already running in
            // a HPX thread
            if(threads::get_self_ptr() == 0 && !hpx::is_starting())
            {
                error_code ec(lightweight);
                threads::thread_id_type encode_thread = applier::register_thread_nullary(
                    util::bind(
                        &parcelport_impl::encode_parcels_impl
                      , this
                    )
                  , "encode_parcels"
                  , threads::pending
                  , true
                  , threads::thread_priority_boost
                  , std::size_t(-1)
                  , threads::thread_stacksize_default
                  , ec
                );
                if(ec)
                {
                    // FIXME: throw exception on error ...
                }
                else
                {
                    lcos::local::spinlock::scoped_lock l(encode_threads_mtx_);
                    encode_threads_.push_back(encode_thread);
                }
                return;
            }

            encoding_parcels ep(
                encode_threads_mtx_
              , encode_threads_);

            naming::locality locality;
            std::vector<parcel> parcels;
            detail::call_for_each handlers;
            bool has_work = true;
            hpx::util::high_resolution_timer t;
            std::size_t k = 0;
            while(has_work || (!has_work && t.elapsed() < 2.0))
            {
                {
                    lcos::local::spinlock::scoped_lock l(mtx_);
                    pending_parcels_map::iterator it = pending_parcels_.begin();

                    while(it != pending_parcels_.end())
                    {
                        locality = it->first;
                        if(dequeue_parcels(it, parcels, handlers))
                        {
                            has_work = true;
                            t.restart();
                            break;
                        }
                        ++it;
                    }
                }
                if(parcels.empty())
                {
                    break;
                    /*
                    has_work = false;
                    hpx::lcos::local::spinlock::yield(k);
                    ++k;
                    continue;
                    */
                }

                boost::shared_ptr<sender_parcel_buffer_type> buffer =
                    encode_parcels(parcels, *this, archive_flags_, this->enable_security());

                enqueue_out_buffers(locality, buffer, std::move(handlers));

                // Ok, trigger writes ...
                trigger_writes(locality);

                parcels.clear();
                handlers.fv_.clear();
            }
            //trigger_writes();
        }


        bool dequeue_parcels(pending_parcels_map::iterator it,
            std::vector<parcel>& parcels,
            detail::call_for_each& handlers)
        {
            typedef pending_parcels_map::iterator iterator;

            if (!enable_parcel_handling_)
                return false;


            if(it == pending_parcels_.end())
            {
                return false;
            }

            naming::locality locality_id = it->first;
            // do nothing if parcels have already been picked up by
            // another thread
            if (!it->second.first.empty())
            {
                HPX_ASSERT(handlers.fv_.size() == parcels.size());
                std::swap(parcels, it->second.first);
                std::swap(handlers, it->second.second);

                HPX_ASSERT(!handlers.fv_.empty());
            }
            else
            {
                HPX_ASSERT(it->second.second.fv_.empty());
                return false;
            }

            return true;
        }

        void send_pending_buffers(
            boost::system::error_code const & ec
          , naming::locality const & locality
          , boost::shared_ptr<connection> sender_connection)
        {
#if defined(HPX_TRACK_STATE_OF_OUTGOING_TCP_CONNECTION)
            client_connection->set_state(parcelport_connection::state_scheduled_thread);
#endif
            HPX_ASSERT(locality == sender_connection->destination());
            if(ec)
            {
                //std::cout << "error?\n";
                connections_.remove(locality, sender_connection);
                trigger_writes(locality);
                return;
            }

            boost::shared_ptr<sender_parcel_buffer_type> buffer;
            detail::call_for_each handlers;

            if(dequeue_out_buffers(locality, buffer, handlers))
            {
                bool success =
                    sender_connection->write(
                        buffer
                      , std::move(handlers)
                      , util::bind(
                            &parcelport_impl::send_pending_buffers
                          , this
                          , util::placeholders::_1, util::placeholders::_2, util::placeholders::_3
                        )
                    );
                if(!success)
                {
                    enqueue_out_buffers(locality, buffer, handlers);
                }
            }
        }

        void trigger_writes()
        {
            pending_parcels_destinations destinations;
            {
                lcos::local::spinlock::scoped_lock l(out_buffer_mtx_);
                std::swap(destinations, parcel_destinations_);
            }

            BOOST_FOREACH(naming::locality const & loc, destinations)
            {
                trigger_writes(loc);
            }
        }

        void trigger_writes(naming::locality const & locality)
        {
            connections_.trigger_writes(
                locality
              , connection_handler()
              , util::bind(
                    &parcelport_impl::send_pending_buffers
                  , this
                  , util::placeholders::_1, util::placeholders::_2, util::placeholders::_3
                )
            );
        }

    protected:
        /// The pool of io_service objects used to perform asynchronous operations.
        util::io_service_pool io_service_pool_;

        /// The connection cache for sending connections
        //util::connection_cache<connection, naming::locality> connection_cache_;
        connections<connection> connections_;

        int archive_flags_;
        memory_pool memory_pool_;
        hpx::lcos::local::spinlock encode_threads_mtx_;
        std::vector<threads::thread_id_type> encode_threads_;
        std::size_t num_encode_threads_;

        hpx::lcos::local::spinlock out_buffer_mtx_;
        typedef
            std::vector<
                std::pair<
                    boost::shared_ptr<sender_parcel_buffer_type>
                  , detail::call_for_each
                >
            >
            out_buffer_type;
        typedef
            std::map<naming::locality, out_buffer_type>
            out_buffer_map_type;
        out_buffer_map_type out_buffer_map_;
    };
}}

#endif
