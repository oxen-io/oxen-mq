#pragma once

#include "connections.h"

#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include <optional>

namespace oxenmq {

using namespace std::chrono_literals;

namespace detail {
    struct no_data_t final {};
    inline constexpr no_data_t no_data{};

    template <typename UserData>
    struct SubData {
        std::chrono::steady_clock::time_point expiry;
        UserData user_data;
        explicit SubData(std::chrono::steady_clock::time_point _exp)
            : expiry{_exp}, user_data{} {}
    };

    template <>
    struct SubData<void> {
        std::chrono::steady_clock::time_point expiry;
    };
}


/**
 * OMQ Subscription class.  Handles pub/sub connections such that the user only needs to call
 * methods to subscribe and publish.
 *
 * FIXME: do we want an unsubscribe, or is expiry / conn management sufficient?
 *
 * Type UserData can contain whatever information the user may need at publish time, for example if
 * the subscription is for logs the subscriber can specify log levels or categories, and the
 * publisher can choose to send or not based on those.  The UserData type, if provided and non-void,
 * must be default constructible, must be comparable with ==, and must be movable.
 */
template <typename UserData = void>
class Subscription {
    static constexpr bool have_user_data = !std::is_void_v<UserData>;
    using UserData_if_present = std::conditional_t<have_user_data, UserData, detail::no_data_t>;
    using subdata_t = detail::SubData<UserData>;

    std::unordered_map<ConnectionID, subdata_t> subs;
    std::shared_mutex _mutex;
    const std::string description; // description of the sub for logging
    const std::chrono::milliseconds sub_duration; // extended by re-subscribe

public:

    Subscription() = delete;
    Subscription(std::string description, std::chrono::milliseconds sub_duration = 30min)
        : description{std::move(description)}, sub_duration{sub_duration} {}

    // returns true if new sub, false if refresh sub.  throws on error.  `data` will be checked
    // against the existing data: if there is existing data and it compares `==` to the given value,
    // false is returned (and the existing data is not replaced).  Otherwise the given data gets
    // stored for this connection (replacing existing data, if present), and true is returned.
    bool subscribe(const ConnectionID& conn, UserData_if_present data) {
        std::unique_lock lock{_mutex};
        auto expiry = std::chrono::steady_clock::now() + sub_duration;
        auto [value, added] = subs.emplace(conn, subdata_t{expiry});
        if (added) {
            if constexpr (have_user_data)
                value->second.user_data = std::move(data);
            return true;
        }

        value->second.expiry = expiry;

        if constexpr (have_user_data) {
            // if user_data changed, consider it a new sub rather than refresh, and update
            // user_data in the mapped value.
            if (!(value->second.user_data == data)) {
                value->second.user_data = std::move(data);
                return true;
            }
        }
        return false;
    }

    // no-user-data version, only available for Subscription<void> (== Subscription without a
    // UserData type).
    template <bool enable = !have_user_data, std::enable_if_t<enable, int> = 0>
    bool subscribe(const ConnectionID& conn) {
        return subscribe(conn, detail::no_data);
    }

    // unsubscribe a connection ID.  return the user data, if a sub was present.
    template <bool enable = have_user_data, std::enable_if_t<enable, int> = 0>
    std::optional<UserData> unsubscribe(const ConnectionID& conn) {
        std::unique_lock lock{_mutex};

        auto node = subs.extract(conn);
        if (!node.empty())
            return node.mapped().user_data;

        return std::nullopt;
    }

    // no-user-data version, only available for Subscription<void> (== Subscription without a
    // UserData type).
    template <bool enable = !have_user_data, std::enable_if_t<enable, int> = 0>
    bool unsubscribe(const ConnectionID& conn) {
        std::unique_lock lock{_mutex};
        auto node = subs.extract(conn);
        return !node.empty(); // true if removed, false if wasn't present
    }

    // force removal of expired subscriptions.  removal will otherwise only happen on publish
    void remove_expired() {
        std::unique_lock lock{_mutex};
        auto now = std::chrono::steady_clock::now();
        for (auto itr = subs.begin(); itr != subs.end();) {
            if (itr->second.expiry < now)
                itr = subs.erase(itr);
            else
                itr++;
        }
    }

    // Func is any callable which takes:
    // - (const ConnectionID&, const UserData&) for Subscription<UserData> with non-void UserData
    // - (const ConnectionID&) for Subscription<void>.
    template <typename Func>
    void publish(Func&& func) {
        std::vector<ConnectionID> to_remove;
        {
            std::shared_lock lock(_mutex);
            if (subs.empty())
                return;

            auto now = std::chrono::steady_clock::now();

            for (const auto& [conn, sub] : subs) {
                if (sub.expiry < now)
                    to_remove.push_back(conn);
                else if constexpr (have_user_data)
                    func(conn, sub.user_data);
                else
                    func(conn);
            }
        }

        if (to_remove.empty())
            return;

        std::unique_lock lock{_mutex};
        auto now = std::chrono::steady_clock::now();
        for (auto& conn : to_remove) {
            auto it = subs.find(conn);
            if (it != subs.end() && it->second.expiry < now /* recheck: client might have resubscribed in between locks */) {
                subs.erase(it);
            }
        }
    }

};


} // namespace oxenmq

// vim:sw=4:et
