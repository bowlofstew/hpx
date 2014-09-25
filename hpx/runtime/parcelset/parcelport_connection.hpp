//  Copyright (c) 2014 Thomas Heller
//  Copyright (c) 2007-2014 Hartmut Kaiser
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef HPX_PARCELSET_PARCELPORT_CONNECTION_HPP
#define HPX_PARCELSET_PARCELPORT_CONNECTION_HPP

#include <hpx/runtime/parcelset/parcel_buffer.hpp>

#include <boost/enable_shared_from_this.hpp>
#include <boost/noncopyable.hpp>

namespace hpx { namespace parcelset {

    class parcelport;
    template <typename ConnectionHandler>
    class parcelport_impl;

    boost::uint64_t get_max_inbound_size(parcelport&);

    template <typename Connection, typename ConnectionHandler>
    struct parcelport_connection
      : boost::enable_shared_from_this<Connection>
      , private boost::noncopyable
    {
#if defined(HPX_TRACK_STATE_OF_OUTGOING_TCP_CONNECTION)
        enum state
        {
            state_initialized,
            state_reinitialized,
            state_set_parcel,
            state_async_write,
            state_handle_write,
            state_handle_read_ack,
            state_scheduled_thread,
            state_send_pending,
            state_reclaimed,
            state_deleting
        };
#endif
        ConnectionHandler & parcelport_;
        /// the other (receiving) end of this connection
        naming::locality there_;
        boost::atomic<bool> handling_messages_;

#if defined(HPX_HAVE_SECURITY) && defined(HPX_TRACK_STATE_OF_OUTGOING_TCP_CONNECTION)
        parcelport_connection(ConnectionHandler & parcelport, naming::locality there = naming::locality())
          : parcelport_(parcelport)
          , there_(there)
          , handling_messages_(false)
          , first_message_(true)
          , state_(state_initialized)
        {}
        bool first_message_;
        state state_;
#endif

#if defined(HPX_HAVE_SECURITY) && !defined(HPX_TRACK_STATE_OF_OUTGOING_TCP_CONNECTION)
        parcelport_connection(ConnectionHandler & parcelport, naming::locality there = naming::locality())
          : parcelport_(parcelport)
          , there_(there)
          , handling_messages_(false)
          , first_message_(true)
        {}
        bool first_message_;
#endif

#if !defined(HPX_HAVE_SECURITY) && defined(HPX_TRACK_STATE_OF_OUTGOING_TCP_CONNECTION)
        parcelport_connection(ConnectionHandler & parcelport, naming::locality there = naming::locality())
          : parcelport_(parcelport)
          , there_(there)
          , handling_messages_(false)
          , state_(state_initialized)
        {}
        state state_;
#endif

#if !defined(HPX_HAVE_SECURITY) && !defined(HPX_TRACK_STATE_OF_OUTGOING_TCP_CONNECTION)
        parcelport_connection(ConnectionHandler & parcelport, naming::locality there = naming::locality())
          : parcelport_(parcelport)
          , there_(there)
          , handling_messages_(false)
        {}
#endif

        virtual ~parcelport_connection() {}

        ////////////////////////////////////////////////////////////////////////
        typedef typename ConnectionHandler::template get_parcel_buffer_type<Connection>::type parcel_buffer_type;

        /// buffer for data
        boost::shared_ptr<parcel_buffer_type> buffer_;

        template <typename ParcelPostprocess>
        void done(
            ParcelPostprocess pp
          , boost::system::error_code const& ec
          , naming::locality const& locality_id
          , boost::shared_ptr<Connection> sender_connection
        )
        {
            HPX_ASSERT(sender_connection.get() == this);
            HPX_ASSERT(handling_messages_);
            handling_messages_ = false;
            pp(ec, locality_id, sender_connection);
        }

        template <typename Handler, typename ParcelPostprocess>
        bool write(boost::shared_ptr<parcel_buffer_type> buffer, Handler handler, ParcelPostprocess parcel_postprocess)
        {
            HPX_ASSERT(there_);
            // Atomically set handling_messages_ to true, if another work item hasn't
            // started executing before us.
            bool false_ = false;
            if (!handling_messages_.compare_exchange_strong(false_, true))
            {
                return false;
            }

            HPX_ASSERT(handling_messages_);
            buffer_ = buffer;

            void (parcelport_connection::*f)(
                ParcelPostprocess
              , boost::system::error_code const&
              , naming::locality const&
              , boost::shared_ptr<Connection>) = &parcelport_connection::done<ParcelPostprocess>;
            static_cast<Connection &>(*this).async_write(
                handler
              , util::bind(
                    f
                  , this
                  , util::protect(std::move(parcel_postprocess))
                  , util::placeholders::_1, util::placeholders::_2, util::placeholders::_3
                )
            );
            return true;
        }

        bool busy()
        {
            return handling_messages_;
        }
    };
}}

#endif
