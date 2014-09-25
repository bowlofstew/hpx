//  Copyright (c)      2013 Thomas Heller
//  Copyright (c) 2007-2013 Hartmut Kaiser
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#ifndef HPX_RUNTIME_PARCELSET_DETAIL_CALL_FOR_EACH_HPP
#define HPX_RUNTIME_PARCELSET_DETAIL_CALL_FOR_EACH_HPP

#include <hpx/config.hpp>

#include <vector>

///////////////////////////////////////////////////////////////////////////////
namespace hpx { namespace parcelset
{
    ///////////////////////////////////////////////////////////////////////////
    namespace detail
    {
        struct call_for_each
        {
            typedef void result_type;

            typedef HPX_STD_FUNCTION<
                void(boost::system::error_code const&)
            > write_handler_type;

            typedef std::vector<write_handler_type> data_type;
            data_type fv_;

            call_for_each() {}

            explicit call_for_each(write_handler_type && handler)
              : fv_(1, std::move(handler))
            {}

            explicit call_for_each(data_type&& fv)
              : fv_(std::move(fv))
            {}



            result_type operator()(
                boost::system::error_code const& e) const
            {
                BOOST_FOREACH(write_handler_type f, fv_)
                {
                    f(e);
                }
            }

            result_type operator()(
                boost::system::error_code const& e,
                std::size_t) const
            {
                BOOST_FOREACH(write_handler_type f, fv_)
                {
                    f(e);
                }
            }
        };
    }
}}

#endif
