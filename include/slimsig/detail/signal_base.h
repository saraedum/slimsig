//
//  signal_base.h
//  slimsig
//
//  Created by Christopher Tarquini on 4/21/14.
//  TODO: Clean this up, seperate into smaller files
//

#ifndef slimsig_signal_base_h
#define slimsig_signal_base_h

#include <slimsig/connection.h>
#include <slimsig/detail/slot.h>
#include <slimsig/detail/slot_id.h>
#include <functional>
#include <iterator>
#include <memory>
#include <vector>
#include <algorithm>
#include <cassert>

namespace slimsig {

// Signal Traits
// Basically to support return types later
// Also let's you customize the type used for slot ids, if you find yourself running into limits
template <class Handler>
struct signal_traits;

template <class R, class... Args>
struct signal_traits<R(Args...)> {
    using return_type = R;
    using slot_id_type = std::uint_fast64_t;
    using depth_type = unsigned;
};

template <class Handler, class SignalTraits, class Allocator>
class signal;

template <class SignalTraits, class Allocator, class Handler>
class signal_base;

template <class Handler, class SignalTraits, class Allocator>
void swap(signal<Handler, SignalTraits, Allocator>& lhs, signal<Handler, SignalTraits, Allocator>& rhs);

// utility methods
namespace detail {
    template <class Container, class Callback>
    inline void each(const Container& container, typename Container::size_type begin, typename Container::size_type end, const Callback& fn)
    {
        for (; begin != end; begin++) {
            fn(container[begin]);
        }
    }
    template <class Container, class Callback>
    inline void each_n(const Container& container, typename Container::size_type begin, typename Container::size_type count, const Callback& fn)
    {
        return each<Container, Callback>(container, begin, begin + count, fn);
    }

    template <class Callable, class Return, class... Args>
    struct is_callable_helper {
        template <class T, class R = decltype(std::declval<T>()(std::declval<Args>()...))>
        static auto check(typename std::enable_if<std::is_convertible<R,
                                                                      Return>::value>::type*) -> std::true_type;
        template <class T>
        static std::false_type check(...);
        static constexpr bool value = decltype(check<Callable>(nullptr))::value;
    };
    template <class Callable, class Return, class... Args>
    struct is_callable : std::integral_constant<bool, is_callable_helper<Callable, Return, Args...>::value> {
    };
    template <class Signal, class Functor>
    struct shared_slot;

    template <class Functor, class SignalTraits, class Allocator, class R, class... Args>
    struct shared_slot<Functor, signal_base<SignalTraits, Allocator, R(Args...)>> {
        std::weak_ptr<Functor> handle;
        signal_base<SignalTraits, Allocator, R(Args...)>& signal;
        typename signal_base<SignalTraits, Allocator, R(Args...)>::connection connection;
        R operator()(Args... args)
        {
            auto target = handle.lock();
            if (target) {
                return (*target)(std::forward<Args&&>(args)...);
            }
            else {
                signal.disconnect(connection);
                return;
            }
        }
    };
}

template <class SignalTraits, class Allocator, class R, class... Args>
class signal_base<SignalTraits, Allocator, R(Args...)> {
    struct emit_scope;

public:
    using signal_traits = SignalTraits;
    using return_type = typename signal_traits::return_type;
    using callback = std::function<R(Args...)>;
    using allocator_type = Allocator;
    using depth_type = typename signal_traits::depth_type;
    using slot = basic_slot<R(Args...), typename signal_traits::slot_id_type, typename signal_traits::depth_type>;
    using list_allocator_type = typename std::allocator_traits<Allocator>::template rebind_traits<slot>::allocator_type;
    using slot_list = std::vector<slot, list_allocator_type>;

    using connection = connection<signal_base>;
    using extended_callback = std::function<R(connection& conn, Args...)>;
    using slot_id = typename signal_traits::slot_id_type;
    using slot_reference = typename slot_list::reference;
    using const_slot_reference = typename slot_list::const_reference;
    using size_type = std::size_t;

    template <class Functor, class T = void>
    using enable_if_slot_t = typename std::enable_if<std::is_constructible<callback, Functor>::value,
                                                     T>::type;

public:
    static constexpr auto arity = sizeof...(Args);
    template <std::size_t N>
    struct argument {
        static_assert(N < arity, "error: invalid parameter index.");
        using type = typename std::tuple_element<N, std::tuple<Args...>>::type;
    };

    // allocator constructor
    signal_base(allocator_type alloc)
        : pending(alloc)
        , last_id(0)
        , m_size(0)
        , m_offset(0)
        , m_depth(0)
        , allocator(std::move(alloc)){};

    signal_base(size_t capacity, allocator_type alloc = allocator_type{})
        : signal_base(std::move(alloc))
    {
        pending.reserve(capacity);
    };

    signal_base(signal_base&& other)
    {
        this->swap(other);
    }
    signal_base& operator=(signal_base&& other)
    {
        this->swap(other);
        return *this;
    }
    // no copy
    signal_base(const signal_base&) = delete;
    signal_base& operator=(const signal_base&) = delete;
    void swap(signal_base& rhs)
    {
        using std::swap;
        using std::back_inserter;
        using std::copy_if;
        using std::not1;
        if (this != &rhs) {
#if !defined(NDEBUG) || (defined(SLIMSIG_SWAP_GUARD) && SLIMSIG_SWAP_GUARD)
            if (is_running() || rhs.is_running())
                throw std::logic_error("Signals can not be swapped or moved while emitting");
#endif
            swap(pending, rhs.pending);
            swap(last_id, rhs.last_id);
            swap(m_size, rhs.m_size);
            swap(m_offset, rhs.m_offset);
            if (std::allocator_traits<allocator_type>::propagate_on_container_swap::value)
                swap(allocator, rhs.allocator);
            swap(m_depth, rhs.m_depth);
        }
    }
    /**
   *  Calls each connected slot with the passed arguments
   *
   *  @param args Arguments passed to connected slots
   *
   *  @return return_type
   */
    return_type emit(Args... args)
    {
        using detail::each;
        // scope guard
        emit_scope scope{*this};

        auto end = pending.size();
        assert(m_offset <= end);
        if (end - m_offset == 0)
            return;
        assert(end > 0);
        each(pending, m_offset, --end, [&](const_slot_reference slot) {
      if (slot || slot.depth() >= m_depth) slot(args...);
        });

        auto& slot = pending[end];
        if (slot || slot.depth() >= m_depth)
            slot(std::forward<Args&&>(args)...);
    }
    /**
   *  Alias for signal#emit()
   *
   *  @param args Arguments passed to connected slots
   *
   *  @return return_type
   */
    inline return_type operator()(Args... args)
    {
        return emit(std::forward<Args&&>(args)...);
    }
    /**
   *  Connects a function to the signal
   *
   *  @param slot std::function<R(Args...)>
   *
   *  @return connection Unique connection identifier used to disconnect and query a slots status
   */
    inline connection connect(std::function<R(Args...)> slot)
    {
        auto sid = prepare_connection();
        emplace(sid, std::move(slot));
        return {sid};
    };
    /**
   *  Connects a extended functor to the signal
   *
   *  @param slot Callable<R(signal&, connection, Args...)>
   *
   *  @return connection Unique connection identifier used to disconnect and query a slots status
   */
    template <class Slot>
    inline enable_if_slot_t<Slot, connection> connect_extended(extended_callback slot)
    {
        struct extended_slot {
            Slot fn;
            signal_base& signal;
            connection connection;
            R operator()(Args... args)
            {
                return fn(signal, connection, std::forward<Args&&>(args)...);
            }
        };
        return create_connection<extended_slot>(std::move(slot));
    }
    template <class Functor>
    inline enable_if_slot_t<Functor, connection> connect(std::weak_ptr<Functor> slot)
    {
        using shared_slot = detail::shared_slot<signal_base, Functor>;
        return create_connection<shared_slot>(std::move(slot));
    };
    inline connection connect(std::weak_ptr<std::function<R(Args...)>> slot)
    {
        using shared_slot = detail::shared_slot<signal_base, callback>;
        return create_connection<shared_slot>(std::move(slot));
    }

    template <class TP, class Alloc>
    inline connection connect(std::shared_ptr<signal<R(Args...), TP, Alloc>>& signal)
    {
        using signal_type = slimsig::signal<R(Args...), TP, Alloc>;
        struct signal_slot {
            std::weak_ptr<signal_type> handle;
            signal_base& signal;
            connection connection;
            R operator()(Args... args)
            {
                auto target = handle.lock();
                if (target) {
                    return target->emit(std::forward<Args>(args)...);
                }
                else {
                    signal.disconnect(connection);
                    return;
                }
            }
        };
        return create_connection<signal_slot>(std::move(signal));
    }

    template <class Slot>
    inline enable_if_slot_t<Slot, connection> connect_once(Slot slot)
    {
        struct fire_once {
            Slot fn;
            signal_base& signal;
            connection conn;

            R operator()(Args&&... args)
            {

                auto scoped_connection = make_scoped_connection(signal, std::move(conn));
                return fn(std::forward<Args&&>(args)...);
            }
        };
        return create_connection<fire_once>(std::move(slot));
    }

    bool connected(const connection& conn)
    {
        return connected(conn.m_slot_id);
    };
    void disconnect(connection& conn)
    {
        disconnect(conn.m_slot_id);
    };
    void disconnect_all()
    {
        using std::for_each;
        if (is_running()) {
            m_offset = pending.size();
            m_size = 0;
        }
        else {
            pending.clear();
            m_size = 0;
        }
    }

    const allocator_type& get_allocator() const
    {
        return allocator;
    }
    inline bool empty() const
    {
        return m_size == 0;
    }
    inline size_type slot_count() const
    {
        return m_size;
    }
    inline size_type max_size() const
    {
        return std::min(std::numeric_limits<slot_id>::max(), pending.max_size());
    }
    inline size_type remaining_slots() const
    {
        return max_size() - last_id;
    }

    static constexpr depth_type max_depth()
    {
        return std::numeric_limits<depth_type>::max();
    }
    depth_type get_depth() const
    {
        return m_depth;
    }
    bool is_running() const
    {
        return m_depth > 0;
    }

    ~signal_base()
    {
    }
    template <class FN, class TP, class Alloc>
    friend class signal;
    template <class Signal>
    friend class slimsig::connection;

private:
    struct emit_scope {
        signal_base& signal;
        emit_scope(signal_base& context)
            : signal(context)
        {
            ++signal.m_depth;
        }
        emit_scope() = delete;
        emit_scope(const emit_scope&) = delete;
        emit_scope(emit_scope&&) = delete;
        ~emit_scope()
        {
            using std::move;
            using std::for_each;
            using std::remove_if;
            auto depth = --signal.m_depth;
            // if we completed iteration (depth = 0) collapse all the levels into the head list
            if (depth == 0) {
                auto m_size = signal.m_size;
                auto& pending = signal.pending;
                // if the size is different than the expected size
                // we have some slots we need to remove
                if (m_size != pending.size()) {
                    // remove slots from disconnect_all
                    pending.erase(pending.begin(), pending.begin() + signal.m_offset);
                    pending.erase(remove_if(pending.begin(), pending.end(), &is_disconnected), pending.end());
                }
                signal.m_offset = 0;
                assert(m_size == pending.size());
            }
        }
    };

    static bool is_disconnected(const_slot_reference slot) { return !bool(slot); };

    inline bool connected(slot_id index)
    {
        using std::lower_bound;
        auto end = pending.cend();
        auto slot = lower_bound(pending.cbegin() + m_offset, end, index, [](const_slot_reference slot, const slot_id& index) {
      return slot < index;
        });

        return slot != end && slot->m_slot_id == index && slot->connected();
    };

    inline void disconnect(slot_id index)
    {
        using std::lower_bound;
        auto end = pending.end();
        auto slot = lower_bound(pending.begin() + m_offset, end, index, [](slot_reference slot, const slot_id& index) {
      return slot < index;
        });
        if (slot != end && slot->m_slot_id == index) {
            if (slot->connected()) {
                slot->disconnect(m_depth);
                m_size -= 1;
            }
            else {
                std::cout << "super fucky" << std::endl;
            }
        }
        else {
            assert(0 && "somethings fucky");
        }
    };

    template <class C, class T>
    [[gnu::always_inline]] inline connection create_connection(T&& slot)
    {
        auto sid = prepare_connection();
        emplace(sid, std::allocator_arg, allocator, C{std::move(slot), *this, {sid}});
        return connection{sid};
    };

    [[gnu::always_inline]] inline slot_id prepare_connection()
    {
        // lazy initialize to put off heap allocations if the user
        // has not connected a slot
        assert((slot_id(last_id) + 1 != std::numeric_limits<slot_id>::max()) && "All available slot ids for this signal have been exhausted. This may be a sign you are misusing signals");
        return last_id++;
    };

    template <class... SlotArgs>
    [[gnu::always_inline]] inline void emplace(SlotArgs&&... args)
    {
        pending.emplace_back(std::forward<SlotArgs&&>(args)...);
        m_size++;
    };

protected:
    slot_list pending;

private:
    slot_id last_id;
    std::size_t m_size;
    std::size_t m_offset;
    depth_type m_depth;
    allocator_type allocator;
};

template <class Handler, class SignalTraits, class Allocator>
inline void swap(signal<Handler, SignalTraits, Allocator>& lhs, signal<Handler, SignalTraits, Allocator>& rhs)
{
    using std::swap;
    lhs.swap(rhs);
}
}

#endif
