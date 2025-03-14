#include <map>
#include <gtest/gtest.h>

#include "btree_test_kvs.hpp"

template < typename K, typename V >
class ShadowMap {
private:
    std::map< K, V > m_map;
    RangeScheduler m_range_scheduler;
    using mutex = iomgr::FiberManagerLib::shared_mutex;
    mutex m_mutex;

public:
    ShadowMap(uint32_t num_keys) : m_range_scheduler(num_keys) {}

    void put_and_check(const K& key, const V& val, const V& old_val, bool expected_success) {
        std::lock_guard lock{m_mutex};
        auto const [it, happened] = m_map.insert(std::make_pair(key, val));
        ASSERT_EQ(happened, expected_success) << "Testcase issue, expected inserted slots to be in shadow map";
        if (!happened) {
            ASSERT_EQ(old_val, it->second) << "Put: Existing value doesn't return correct data for key: " << it->first;
        }
        m_range_scheduler.put_key(key.key());
    }

    void range_upsert(uint64_t start_k, uint32_t count, const V& val) {
        std::lock_guard lock{m_mutex};
        for (uint32_t i{0}; i < count; ++i) {
            K key{start_k + i};
            V range_value{val};
            if constexpr (std::is_same_v< V, TestIntervalValue >) { range_value.shift(i); }
            m_map.insert_or_assign(key, range_value);
        }
        m_range_scheduler.put_keys(start_k, start_k + count - 1);
    }

    void range_update(const K& start_key, uint32_t count, const V& new_val) {
        std::lock_guard lock{m_mutex};
        auto const start_it = m_map.lower_bound(start_key);
        auto it = start_it;
        uint32_t c = 0;
        while ((it != m_map.end()) && (++c <= count)) {
            it->second = new_val;
            ++it;
        }
        m_range_scheduler.remove_keys_from_working(start_key.key(), start_key.key() + count - 1);
    }

    std::pair< K, K > pick_existing_range(const K& start_key, uint32_t max_count) const {
        std::lock_guard lock{m_mutex};
        auto const start_it = m_map.lower_bound(start_key);
        auto it = start_it;
        uint32_t count = 0;
        while ((it != m_map.cend()) && (++count < max_count)) {
            ++it;
        }
        return std::pair(start_it->first, it->first);
    }

    bool exists(const K& key) const {
        std::lock_guard lock{m_mutex};
        return m_map.find(key) != m_map.end();
    }

    bool exists_in_range(const K& key, uint64_t start_k, uint64_t end_k) const {
        std::lock_guard lock{m_mutex};
        const auto itlower = m_map.lower_bound(K{start_k});
        const auto itupper = m_map.upper_bound(K{end_k});
        auto it = itlower;
        while (it != itupper) {
            if (it->first == key) { return true; }
            ++it;
        }
        return false;
    }

    uint64_t size() const {
        std::lock_guard lock{m_mutex};
        return m_map.size();
    }

    uint32_t num_elems_in_range(uint64_t start_k, uint64_t end_k) const {
        const auto itlower = m_map.lower_bound(K{start_k});
        const auto itupper = m_map.upper_bound(K{end_k});
        return std::distance(itlower, itupper);
    }

    void validate_data(const K& key, const V& btree_val) const {
        std::lock_guard lock{m_mutex};
        const auto r = m_map.find(key);
        ASSERT_NE(r, m_map.end()) << "Key " << key.to_string() << " is not present in shadow map";
        ASSERT_EQ(btree_val, r->second) << "Found value in btree doesn't return correct data for key=" << r->first;
    }

    void remove_and_check(const K& key, const V& btree_val) {
        std::lock_guard lock{m_mutex};
        const auto r = m_map.find(key);
        ASSERT_NE(r, m_map.end()) << "Key " << key.to_string() << " is not present in shadow map";
        ASSERT_EQ(btree_val, r->second) << "Found value in btree doesn't return correct data for key=" << r->first;
        m_map.erase(key);
        m_range_scheduler.remove_key(key.key());
    }

    void erase(const K& key) {
        std::lock_guard lock{m_mutex};
        m_map.erase(key);
        m_range_scheduler.remove_key(key.key());
    }

    void range_erase(const K& start_key, uint32_t count) {
        std::lock_guard lock{m_mutex};
        auto it = m_map.lower_bound(start_key);
        uint32_t i{0};
        while ((it != m_map.cend()) && (i++ < count)) {
            it = m_map.erase(it);
        }
        m_range_scheduler.remove_keys(start_key.key(), start_key.key() + count);
    }

    void range_erase(const K& start_key, const K& end_key) {
        std::lock_guard lock{m_mutex};
        auto it = m_map.lower_bound(start_key);
        auto const end_it = m_map.upper_bound(end_key);
        while ((it != m_map.cend()) && (it != end_it)) {
            it = m_map.erase(it);
        }
        m_range_scheduler.remove_keys(start_key.key(), end_key.key());
    }

    mutex& guard() { return m_mutex; }
    std::map< K, V >& map() { return m_map; }
    const std::map< K, V >& map_const() const { return m_map; }

    void foreach (std::function< void(K, V) > func) const {
        std::lock_guard lock{m_mutex};
        for (const auto& [key, value] : m_map) {
            func(key, value);
        }
    }

    std::pair< uint32_t, uint32_t > pick_random_non_existing_keys(uint32_t max_keys) {
        do {
            std::lock_guard lock{m_mutex};
            auto ret = m_range_scheduler.pick_random_non_existing_keys(max_keys);
            if (ret.first != UINT32_MAX) { return ret; }
        } while (true);
    }

    std::pair< uint32_t, uint32_t > pick_random_existing_keys(uint32_t max_keys) {
        do {
            std::lock_guard lock{m_mutex};
            auto ret = m_range_scheduler.pick_random_existing_keys(max_keys);
            if (ret.first != UINT32_MAX) { return ret; }
        } while (true);
    }

    std::pair< uint32_t, uint32_t > pick_random_non_working_keys(uint32_t max_keys) {
        do {
            std::lock_guard lock{m_mutex};
            auto ret = m_range_scheduler.pick_random_non_working_keys(max_keys);
            if (ret.first != UINT32_MAX) { return ret; }
        } while (true);
    }

    void remove_keys_from_working(uint32_t s, uint32_t e) {
        std::lock_guard lock{m_mutex};
        m_range_scheduler.remove_keys_from_working(s, e);
    }

    void remove_keys(uint32_t start_key, uint32_t end_key) {
        std::lock_guard lock{m_mutex};
        m_range_scheduler.remove_keys(start_key, end_key);
    }
};
