//  Copyright (c) 2014 Thomas Heller
//  Copyright (c) 2014 John Biddiscombe
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef HPX_PARCELSET_PARCELPORT_CONNECTIONS_HPP
#define HPX_PARCELSET_PARCELPORT_CONNECTIONS_HPP

#include <hpx/runtime/parcelset/detail/call_for_each.hpp>

namespace hpx { namespace parcelset {

    template <typename Connection>
    struct connections
    {
        typedef hpx::lcos::local::spinlock mutex_type;
        typedef boost::shared_ptr<Connection> connection_ptr;
        typedef std::vector<connection_ptr> connection_vector;
        typedef
            std::map<
                naming::locality
              , connection_vector // Established connections
            >
            connections_type;
        typedef std::map<naming::locality, std::size_t>
            connections_count_type;

        connections(std::size_t max_connections)
          : max_connections_(max_connections)
          , misses_(0)
          , hits_(0)
          , reclaims_(0)
          , insertions_(0)
        {}

        template <typename ConnectionHandler, typename PostProcessing>
        void trigger_writes(naming::locality const & locality, ConnectionHandler & connection_handler, PostProcessing post_processing)
        {
            typedef typename ConnectionHandler::sender_parcel_buffer_type sender_parcel_buffer_type;
            typedef typename ConnectionHandler::write_handler_type write_handler_type;

            mutex_type::scoped_lock l(mtx_);

            typename connections_type::iterator it = connections_.find(locality);
            if(it == connections_.end())
            {
                it = connections_.insert(connections_.end(), std::make_pair(locality, connection_vector()));
            }

            create_connection(locality, it->second, connection_handler);

            boost::shared_ptr<sender_parcel_buffer_type> buffer;
            detail::call_for_each handlers;

            if(connection_handler.dequeue_out_buffers(locality, buffer, handlers))
            {
                BOOST_FOREACH(connection_ptr & conn, it->second)
                {
                    if(conn->write(buffer, std::move(handlers), post_processing))
                    {
                        return;
                    }
                }
                connection_handler.enqueue_out_buffers(locality, buffer, handlers);
            }
        }

        void remove(naming::locality const & locality, connection_ptr connection)
        {
            mutex_type::scoped_lock l(mtx_);
            typename connections_type::iterator it = connections_.find(locality);

            // This connection has to be part of our map already
            HPX_ASSERT(it != connections_.end());

            typename std::vector<connection_ptr>::iterator jt =
                std::find(
                    it->second.begin()
                  , it->second.end()
                  , connection
                );
            HPX_ASSERT(jt != it->second.end());
            it->second.erase(jt);
        }

        void clear()
        {
            mutex_type::scoped_lock l(mtx_);
            connections_.clear();
        }

        void clear(naming::locality const & locality)
        {
            mutex_type::scoped_lock l(mtx_);
            typename connections_type::iterator it = connections_.find(locality);

            // This connection has to be part of our map already
            HPX_ASSERT(it != connections_.end());
            it->second.clear();
        }

    private:
        // create a connection if there is still room ...
        template <typename ConnectionHandler>
        void create_connection(
            naming::locality const & locality
          , connection_vector & connections
          , ConnectionHandler & connection_handler)
        {
            if(connections.capacity() < max_connections_)
            {
                connections.reserve(max_connections_);
            }

            bool all_busy = true;
            BOOST_FOREACH(connection_ptr & conn, connections)
            {
                if(!conn->busy())
                {
                    all_busy = false;
                }
            }

            if(all_busy)
            {
                error_code ec(lightweight);
                connections.push_back(connection_handler.create_connection(locality, ec));
                // FIXME: handle error

                if (&ec != &throws)
                    ec = make_success_code();
            }
        }

        mutex_type mtx_;
        const std::size_t max_connections_;

    public:
        std::size_t misses_;
        std::size_t hits_;
        std::size_t reclaims_;
        std::size_t insertions_;
        connections_type connections_;
    };

}}

#endif
