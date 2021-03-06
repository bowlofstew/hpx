//  Copyright (c) 2007-2014 Hartmut Kaiser
//
//  Distributed under the Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#if !defined(HPX_PARALLEL_SEGMENTED_ALGORITHM_COUNT_DEC_25_2014_0207PM)
#define HPX_PARALLEL_SEGMENTED_ALGORITHM_COUNT_DEC_25_2014_0207PM

#include <hpx/hpx_fwd.hpp>
#include <hpx/util/void_guard.hpp>
#include <hpx/util/move.hpp>
#include <hpx/traits/segmented_iterator_traits.hpp>

#include <hpx/parallel/config/inline_namespace.hpp>
#include <hpx/parallel/execution_policy.hpp>
#include <hpx/parallel/algorithms/detail/algorithm_result.hpp>
#include <hpx/parallel/algorithms/detail/dispatch.hpp>
#include <hpx/parallel/algorithms/detail/is_negative.hpp>
#include <hpx/parallel/algorithms/remote/dispatch.hpp>
#include <hpx/parallel/algorithms/count.hpp>
#include <hpx/parallel/util/detail/handle_remote_exceptions.hpp>

#include <algorithm>
#include <iterator>

#include <boost/type_traits/is_same.hpp>

namespace hpx { namespace parallel { HPX_INLINE_NAMESPACE(v1)
{
    ///////////////////////////////////////////////////////////////////////////
    // segmented_count
    namespace detail
    {
        ///////////////////////////////////////////////////////////////////////
        /// \cond NOINTERNAL

        // sequential remote implementation
        template <typename Algo, typename ExPolicy, typename SegIter,
            typename T>
        static typename detail::algorithm_result<
            ExPolicy, typename std::iterator_traits<SegIter>::difference_type
        >::type
        segmented_count(Algo && algo, ExPolicy const& policy,
            SegIter first, SegIter last, T const& value, boost::mpl::true_)
        {
            typedef hpx::traits::segmented_iterator_traits<SegIter> traits;
            typedef typename traits::segment_iterator segment_iterator;
            typedef typename traits::local_iterator local_iterator_type;
            typedef typename std::iterator_traits<SegIter>::difference_type
                value_type;
            typedef detail::algorithm_result<ExPolicy, value_type> result;

            using boost::mpl::true_;

            segment_iterator sit = traits::segment(first);
            segment_iterator send = traits::segment(last);

            value_type overall_result = value_type();

            if (sit == send)
            {
                // all elements are on the same partition
                local_iterator_type beg = traits::local(first);
                local_iterator_type end = traits::local(last);
                if (beg != end)
                {
                    overall_result =
                        util::remote::dispatch(traits::get_id(sit),
                            std::forward<Algo>(algo), policy, true_(),
                            beg, end, value);
                }
            }
            else {
                // handle the remaining part of the first partition
                local_iterator_type beg = traits::local(first);
                local_iterator_type end = traits::end(sit);
                if (beg != end)
                {
                    overall_result +=
                        util::remote::dispatch(traits::get_id(sit),
                            std::forward<Algo>(algo), policy, true_(),
                            beg, end, value);
                }

                // handle all of the full partitions
                for (++sit; sit != send; ++sit)
                {
                    beg = traits::begin(sit);
                    end = traits::end(sit);
                    if (beg != end)
                    {
                        overall_result +=
                            util::remote::dispatch(traits::get_id(sit),
                                std::forward<Algo>(algo), policy, true_(),
                                beg, end, value);
                    }
                }

                // handle the beginning of the last partition
                beg = traits::begin(sit);
                end = traits::local(last);
                if (beg != end)
                {
                    overall_result +=
                        util::remote::dispatch(traits::get_id(sit),
                            std::forward<Algo>(algo), policy, true_(),
                            beg, end, value);
                }
            }

            return result::get(std::move(overall_result));
        }

        // parallel remote implementation
        template <typename Algo, typename ExPolicy, typename SegIter,
            typename T>
        static typename detail::algorithm_result<
            ExPolicy, typename std::iterator_traits<SegIter>::difference_type
        >::type
        segmented_count(Algo && algo, ExPolicy const& policy,
            SegIter first, SegIter last, T const& value, boost::mpl::false_)
        {
            typedef hpx::traits::segmented_iterator_traits<SegIter> traits;
            typedef typename traits::segment_iterator segment_iterator;
            typedef typename traits::local_iterator local_iterator_type;

            typedef typename std::iterator_traits<SegIter>::iterator_category
                iterator_category;
            typedef typename boost::mpl::bool_<boost::is_same<
                    iterator_category, std::input_iterator_tag
                >::value> forced_seq;
            typedef typename std::iterator_traits<SegIter>::difference_type
                value_type;
            typedef detail::algorithm_result<ExPolicy, value_type> result;

            segment_iterator sit = traits::segment(first);
            segment_iterator send = traits::segment(last);

            std::vector<shared_future<value_type> > segments;
            segments.reserve(std::distance(sit, send));

            if (sit == send)
            {
                // all elements are on the same partition
                local_iterator_type beg = traits::local(first);
                local_iterator_type end = traits::local(last);
                if (beg != end)
                {
                    segments.push_back(
                        util::remote::dispatch_async(traits::get_id(sit),
                            std::forward<Algo>(algo), policy, forced_seq(),
                            beg, end, value));
                }
            }
            else {
                // handle the remaining part of the first partition
                local_iterator_type beg = traits::local(first);
                local_iterator_type end = traits::end(sit);
                if (beg != end)
                {
                    segments.push_back(
                        util::remote::dispatch_async(traits::get_id(sit),
                            std::forward<Algo>(algo), policy, forced_seq(),
                            beg, end, value));
                }

                // handle all of the full partitions
                for (++sit; sit != send; ++sit)
                {
                    beg = traits::begin(sit);
                    end = traits::end(sit);
                    if (beg != end)
                    {
                        segments.push_back(
                            util::remote::dispatch_async(traits::get_id(sit),
                                std::forward<Algo>(algo), policy, forced_seq(),
                                beg, end, value));
                    }
                }

                // handle the beginning of the last partition
                beg = traits::begin(sit);
                end = traits::local(last);
                if (beg != end)
                {
                    segments.push_back(
                        util::remote::dispatch_async(traits::get_id(sit),
                            std::forward<Algo>(algo), policy, forced_seq(),
                            beg, end, value));
                }
            }

            return result::get(
                lcos::local::dataflow(
                    hpx::util::unwrapped([=](std::vector<value_type> && r)
                    {
                        return std::accumulate(r.begin(), r.end(), value_type());
                    }),
                    segments));
        }

        ///////////////////////////////////////////////////////////////////////
        // segmented implementation
        template <typename ExPolicy, typename InIter, typename T>
        inline typename detail::algorithm_result<
            ExPolicy, typename std::iterator_traits<InIter>::difference_type
        >::type
        count_(ExPolicy&& policy, InIter first, InIter last, T const& value,
            boost::mpl::true_)
        {
            typedef typename parallel::is_sequential_execution_policy<
                    ExPolicy
                >::type is_seq;

            typedef typename std::iterator_traits<InIter>::difference_type
                difference_type;

            if (first == last)
            {
                return detail::algorithm_result<
                    ExPolicy, difference_type>::get(difference_type());
            }

            return segmented_count(
                count<difference_type>(), std::forward<ExPolicy>(policy),
                first, last, value, is_seq());
        }

        // forward declare the non-segmented version of this algorithm
        template <typename ExPolicy, typename InIter, typename T>
        typename detail::algorithm_result<
            ExPolicy, typename std::iterator_traits<InIter>::difference_type
        >::type
        count_(ExPolicy&& policy, InIter first, InIter last, T const& value,
            boost::mpl::false_);

        /// \endcond
    }

    ///////////////////////////////////////////////////////////////////////////
    // segmented_count_if
    namespace detail
    {
        ///////////////////////////////////////////////////////////////////////
        /// \cond NOINTERNAL

        // sequential remote implementation
        template <typename Algo, typename ExPolicy, typename SegIter,
            typename F>
        static typename detail::algorithm_result<
            ExPolicy, typename std::iterator_traits<SegIter>::difference_type
        >::type
        segmented_count_if(Algo && algo, ExPolicy const& policy,
            SegIter first, SegIter last, F && f, boost::mpl::true_)
        {
            typedef hpx::traits::segmented_iterator_traits<SegIter> traits;
            typedef typename traits::segment_iterator segment_iterator;
            typedef typename traits::local_iterator local_iterator_type;
            typedef typename std::iterator_traits<SegIter>::difference_type
                value_type;
            typedef detail::algorithm_result<ExPolicy, value_type> result;

            using boost::mpl::true_;

            segment_iterator sit = traits::segment(first);
            segment_iterator send = traits::segment(last);

            value_type overall_result = value_type();

            if (sit == send)
            {
                // all elements are on the same partition
                local_iterator_type beg = traits::local(first);
                local_iterator_type end = traits::local(last);
                if (beg != end)
                {
                    overall_result =
                        util::remote::dispatch(traits::get_id(sit),
                            std::forward<Algo>(algo), policy, true_(),
                            beg, end, std::forward<F>(f));
                }
            }
            else {
                // handle the remaining part of the first partition
                local_iterator_type beg = traits::local(first);
                local_iterator_type end = traits::end(sit);
                if (beg != end)
                {
                    overall_result +=
                        util::remote::dispatch(traits::get_id(sit),
                            std::forward<Algo>(algo), policy, true_(),
                            beg, end, std::forward<F>(f));
                }

                // handle all of the full partitions
                for (++sit; sit != send; ++sit)
                {
                    beg = traits::begin(sit);
                    end = traits::end(sit);
                    if (beg != end)
                    {
                        overall_result +=
                            util::remote::dispatch(traits::get_id(sit),
                                std::forward<Algo>(algo), policy, true_(),
                                beg, end, std::forward<F>(f));
                    }
                }

                // handle the beginning of the last partition
                beg = traits::begin(sit);
                end = traits::local(last);
                if (beg != end)
                {
                    overall_result +=
                        util::remote::dispatch(traits::get_id(sit),
                            std::forward<Algo>(algo), policy, true_(),
                            beg, end, std::forward<F>(f));
                }
            }

            return result::get(std::move(overall_result));
        }

        // parallel remote implementation
        template <typename Algo, typename ExPolicy, typename SegIter,
            typename F>
        static typename detail::algorithm_result<
            ExPolicy, typename std::iterator_traits<SegIter>::difference_type
        >::type
        segmented_count_if(Algo && algo, ExPolicy const& policy,
            SegIter first, SegIter last, F && f, boost::mpl::false_)
        {
            typedef hpx::traits::segmented_iterator_traits<SegIter> traits;
            typedef typename traits::segment_iterator segment_iterator;
            typedef typename traits::local_iterator local_iterator_type;

            typedef typename std::iterator_traits<SegIter>::iterator_category
                iterator_category;
            typedef typename boost::mpl::bool_<boost::is_same<
                    iterator_category, std::input_iterator_tag
                >::value> forced_seq;
            typedef typename std::iterator_traits<SegIter>::difference_type
                value_type;
            typedef detail::algorithm_result<ExPolicy, value_type> result;

            segment_iterator sit = traits::segment(first);
            segment_iterator send = traits::segment(last);

            std::vector<shared_future<value_type> > segments;
            segments.reserve(std::distance(sit, send));

            if (sit == send)
            {
                // all elements are on the same partition
                local_iterator_type beg = traits::local(first);
                local_iterator_type end = traits::local(last);
                if (beg != end)
                {
                    segments.push_back(
                        util::remote::dispatch_async(traits::get_id(sit),
                            std::forward<Algo>(algo), policy, forced_seq(),
                            beg, end, std::forward<F>(f)));
                }
            }
            else {
                // handle the remaining part of the first partition
                local_iterator_type beg = traits::local(first);
                local_iterator_type end = traits::end(sit);
                if (beg != end)
                {
                    segments.push_back(
                        util::remote::dispatch_async(traits::get_id(sit),
                            std::forward<Algo>(algo), policy, forced_seq(),
                            beg, end, std::forward<F>(f)));
                }

                // handle all of the full partitions
                for (++sit; sit != send; ++sit)
                {
                    beg = traits::begin(sit);
                    end = traits::end(sit);
                    if (beg != end)
                    {
                        segments.push_back(
                            util::remote::dispatch_async(traits::get_id(sit),
                                std::forward<Algo>(algo), policy, forced_seq(),
                                beg, end, std::forward<F>(f)));
                    }
                }

                // handle the beginning of the last partition
                beg = traits::begin(sit);
                end = traits::local(last);
                if (beg != end)
                {
                    segments.push_back(
                        util::remote::dispatch_async(traits::get_id(sit),
                            std::forward<Algo>(algo), policy, forced_seq(),
                            beg, end, std::forward<F>(f)));
                }
            }

            return result::get(
                lcos::local::dataflow(
                    [=](std::vector<shared_future<value_type> > && r)
                    {
                        // handle any remote exceptions, will throw on error
                        std::list<boost::exception_ptr> errors;
                        parallel::util::detail::handle_remote_exceptions<
                            ExPolicy
                        >::call(r, errors);

                        return std::accumulate(
                            r.begin(), r.end(), value_type(),
                            [](value_type const& val, shared_future<value_type>& curr)
                            {
                                return val + curr.get();
                            });
                    },
                    std::move(segments)));
        }

        template <typename ExPolicy, typename InIter, typename F>
        inline typename detail::algorithm_result<
            ExPolicy, typename std::iterator_traits<InIter>::difference_type
        >::type
        count_if_(ExPolicy && policy, InIter first, InIter last, F && f,
            boost::mpl::true_)
        {
            typedef typename parallel::is_sequential_execution_policy<
                    ExPolicy
                >::type is_seq;

            typedef typename std::iterator_traits<InIter>::difference_type
                difference_type;

            if (first == last)
            {
                return detail::algorithm_result<
                    ExPolicy, difference_type>::get(difference_type());
            }

            return segmented_count_if(
                count_if<difference_type>(), std::forward<ExPolicy>(policy),
                first, last, std::forward<F>(f), is_seq());
        }

        // forward declare the non-segmented version of this algorithm
        template <typename ExPolicy, typename InIter, typename F>
        typename detail::algorithm_result<
            ExPolicy, typename std::iterator_traits<InIter>::difference_type
        >::type
        count_if_(ExPolicy && policy, InIter first, InIter last, F && f,
            boost::mpl::false_);

        /// \endcond
    }
}}}

#endif
