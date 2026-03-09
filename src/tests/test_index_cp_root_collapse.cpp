// Unit tests to reproduce Index CP stuck issue when root collapse creates
// dependencies on CLEAN buffers that never get flushed.
#include <gtest/gtest.h>
#include <boost/uuid/random_generator.hpp>

#include <sisl/utility/enum.hpp>
#include "common/homestore_config.hpp"
#include "common/resource_mgr.hpp"
#include "test_common/homestore_test_common.hpp"
#include "test_common/range_scheduler.hpp"
#include "btree_helpers/btree_test_helper.hpp"
#include "btree_helpers/btree_test_kvs.hpp"
#include "btree_helpers/btree_decls.h"

using namespace homestore;

SISL_LOGGING_INIT(HOMESTORE_LOG_MODS)
SISL_OPTIONS_ENABLE(logging, test_index_cp_root_collapse, iomgr, test_common_setup)
SISL_LOGGING_DECL(test_index_cp_root_collapse)

SISL_OPTION_GROUP(
    test_index_cp_root_collapse,
    (num_entries, "", "num_entries", "number of entries to test with",
     ::cxxopts::value< uint32_t >()->default_value("4000"), "number"),
    (node_size, "", "node_size", "btree node size",
     ::cxxopts::value< uint32_t >()->default_value("8192"), "number"),
    (init_device, "", "init_device", "init device", ::cxxopts::value< bool >()->default_value("1"), ""),
    (cleanup_after_shutdown, "", "cleanup_after_shutdown", "cleanup after shutdown",
     ::cxxopts::value< bool >()->default_value("1"), ""))

template < typename TestType >
struct RootCollapseTest : public test_common::HSTestHelper, public BtreeTestHelper< TestType >, public ::testing::Test {
    using T = TestType;
    using K = typename TestType::KeyType;
    using V = typename TestType::ValueType;

    class TestIndexServiceCallbacks : public IndexServiceCallbacks {
    public:
        TestIndexServiceCallbacks(RootCollapseTest* test) : m_test(test) {}
        std::shared_ptr< IndexTableBase > on_index_table_found(superblk< index_table_sb >&& sb) override {
            LOGINFO("Index table recovered");
            m_test->m_bt = std::make_shared< typename T::BtreeType >(std::move(sb), m_test->m_cfg);
            return m_test->m_bt;
        }

    private:
        RootCollapseTest* m_test;
    };

    RootCollapseTest() : testing::Test() {}

    void SetUp() override {
        this->start_homestore(
            "test_index_cp_root_collapse",
            {{HS_SERVICE::META, {.size_pct = 10.0}},
             {HS_SERVICE::INDEX, {.size_pct = 70.0, .index_svc_cbs = new TestIndexServiceCallbacks(this)}}});

        LOGINFO("Node size {} ", hs()->index_service().node_size());
        this->m_cfg = BtreeConfig(hs()->index_service().node_size());
        this->m_cfg.m_merge_turned_on = true;

        auto uuid = boost::uuids::random_generator()();
        auto parent_uuid = boost::uuids::random_generator()();

        HS_SETTINGS_FACTORY().modifiable_settings([](auto& s) {
            s.generic.cache_max_throttle_cnt = 10000;
            HS_SETTINGS_FACTORY().save();
        });

        this->m_bt = std::make_shared< typename T::BtreeType >(uuid, parent_uuid, 0, this->m_cfg);
        hs()->index_service().add_index_table(this->m_bt);
    }

    void put_scattered(uint64_t k, btree_put_type put_type = btree_put_type::INSERT) {
        auto existing_v = std::make_unique< V >();
        K key = K{k};
        V value = V::generate_rand();
        auto sreq = BtreeSinglePutRequest{&key, &value, put_type, existing_v.get()};
        sreq.enable_route_tracing();

        const auto ret = this->m_bt->put(sreq);
        ASSERT_EQ(ret, btree_status_t::success) << "Put key=" << k << " failed with error=" << enum_name(ret);
        this->m_shadow_map.force_put_scattered(key, value);
    }

    void remove_scattered(uint64_t k) {
        auto existing_v = std::make_unique< V >();
        auto pk = std::make_unique< K >(k);

        auto rreq = BtreeSingleRemoveRequest{pk.get(), existing_v.get()};
        rreq.enable_route_tracing();
        bool removed = (this->m_bt->remove(rreq) == btree_status_t::success);

        if (!removed) {
            LOGERROR("Failed to remove key {}", k);
            this->print_keys();
        }
        ASSERT_TRUE(removed) << "Removal of key " << k << " failed";
        this->m_shadow_map.erase_scattered(*pk);
    }

    void TearDown() override {
        this->shutdown_homestore(SISL_OPTIONS["cleanup_after_shutdown"].as< bool >());
    }

    void build_tree_for_collapse(uint32_t num_groups, uint32_t keys_per_group = 256) {
        LOGINFO("Building tree with {} groups × {} keys/group = {} total entries",
                num_groups, keys_per_group, num_groups * keys_per_group);
        for (uint32_t group = 0; group < num_groups; ++group) {
            uint64_t group_prefix = (0xaa + group) << 24;
            for (uint32_t offset = 0; offset < keys_per_group; ++offset) {
                uint64_t key = group_prefix | offset;
                put_scattered(key, btree_put_type::INSERT);
            }
        }

        auto [interior_nodes, leaf_nodes] = this->m_bt->get_num_nodes();
        LOGINFO("Tree built - depth: {}, interior nodes: {}, leaf nodes: {}",
                this->m_bt->get_btree_depth(),
                interior_nodes,
                leaf_nodes);
    }

    void trigger_root_collapse(uint32_t groups_to_keep, uint32_t num_groups, uint32_t keys_per_group = 256) {
        LOGINFO("Removing groups to trigger root collapse (keeping first {} groups out of {})",
                groups_to_keep, num_groups);
        for (uint32_t group = num_groups - 1; group >= groups_to_keep && group < num_groups; --group) {
            uint64_t group_prefix = (0xaa + group) << 24;
            LOGINFO("Removing group {} (prefix 0x{:08x})", group, group_prefix);

            for (uint32_t offset = 0; offset < keys_per_group; ++offset) {
                uint64_t key = group_prefix | offset;
                remove_scattered(key);
            }
        }

        auto [interior_nodes, leaf_nodes] = this->m_bt->get_num_nodes();
        LOGINFO("After removal - depth: {}, interior nodes: {}, leaf nodes: {}",
                this->m_bt->get_btree_depth(),
                interior_nodes,
                leaf_nodes);
    }

    void verify_cp_completes(uint32_t timeout_ms = 30000) {
        LOGINFO("Triggering CP flush with timeout {}ms", timeout_ms);

        auto start = std::chrono::steady_clock::now();
        test_common::HSTestHelper::trigger_cp(true /* wait */);
        auto elapsed = std::chrono::duration_cast< std::chrono::milliseconds >(
            std::chrono::steady_clock::now() - start).count();

        LOGINFO("CP completed in {}ms", elapsed);
        ASSERT_LT(elapsed, timeout_ms) << "CP took too long";
    }
};

using BtreeTypes = ::testing::Types<
    FixedLenBtree,
    VarKeySizeBtree,
    VarValueSizeBtree,
    VarObjSizeBtree
>;

TYPED_TEST_SUITE(RootCollapseTest, BtreeTypes);

TYPED_TEST(RootCollapseTest, BasicRootCollapse) {
    LOGINFO("=== TEST 1: Basic Root Collapse ===");
    uint32_t num_groups = 32;
    uint32_t keys_per_group = 512;
    this->build_tree_for_collapse(num_groups, keys_per_group);
    auto initial_depth = this->m_bt->get_btree_depth();
    LOGINFO("Initial tree depth: {}", initial_depth);
    ASSERT_GT(initial_depth, 1) << "Tree should have multiple levels for this test";
    test_common::HSTestHelper::trigger_cp(true);
    this->trigger_root_collapse(0, num_groups, keys_per_group);
    test_common::HSTestHelper::trigger_cp(true);

    auto final_depth = this->m_bt->get_btree_depth();
    auto [interior_final, leaf_final] = this->m_bt->get_num_nodes();
    LOGINFO("Final tree depth: {}, {} interior, {} leaf nodes", final_depth, interior_final, leaf_final);
    ASSERT_LT(final_depth, initial_depth) << "Tree depth should have decreased";

    this->verify_cp_completes();
    ASSERT_EQ(this->m_shadow_map.size(), 0u) << "Shadow map should be empty";
}

TYPED_TEST(RootCollapseTest, MultipleRootCollapses) {
    LOGINFO("=== TEST 2: Multiple Root Collapses ===");
    uint32_t num_groups = 32;
    uint32_t keys_per_group = 512;
    this->build_tree_for_collapse(num_groups, keys_per_group);
    auto initial_depth = this->m_bt->get_btree_depth();
    LOGINFO("Initial tree depth: {}", initial_depth);

    test_common::HSTestHelper::trigger_cp(true);
    uint32_t keep_groups = num_groups;
    for (int stage = 0; stage < 3; ++stage) {
        keep_groups = keep_groups / 2;
        LOGINFO("Stage {}: Removing groups to keep {}", stage, keep_groups);

        this->trigger_root_collapse(keep_groups, num_groups, keys_per_group);
        num_groups = keep_groups; // Update for next iteration

        LOGINFO("After stage {} - depth: {}", stage, this->m_bt->get_btree_depth());
        this->verify_cp_completes();
    }

    this->get_all();
}

TYPED_TEST(RootCollapseTest, RootCollapseWithConcurrentOps) {
    LOGINFO("=== TEST 3: Root Collapse with Concurrent Operations ===");
    uint32_t num_groups = 32;
    uint32_t keys_per_group = 512;
    this->build_tree_for_collapse(num_groups, keys_per_group);
    test_common::HSTestHelper::trigger_cp(true);

    this->trigger_root_collapse(4, num_groups, keys_per_group);
    uint64_t new_group_prefix = 0xff000000;
    for (uint32_t offset = 0; offset < 128; ++offset) {
        uint64_t key = new_group_prefix | offset;
        this->put_scattered(key, btree_put_type::INSERT);
    }

    this->verify_cp_completes();
    this->get_all();
}

TYPED_TEST(RootCollapseTest, CollapseToLeaf) {
    LOGINFO("=== TEST 4: Collapse to Single Leaf ===");
    uint32_t num_groups = 8;
    uint32_t keys_per_group = 512;
    this->build_tree_for_collapse(num_groups, keys_per_group);
    test_common::HSTestHelper::trigger_cp(true);

    auto initial_depth = this->m_bt->get_btree_depth();
    uint64_t group1_prefix = (0xaa + 1) << 24;
    for (uint32_t offset = 0; offset < keys_per_group; ++offset) {
        uint64_t key = group1_prefix | offset;
        this->remove_scattered(key);
    }
    uint64_t group0_prefix = (0xaa + 0) << 24;
    for (uint32_t offset = 10; offset < keys_per_group; ++offset) {
        uint64_t key = group0_prefix | offset;
        this->remove_scattered(key);
    }

    this->verify_cp_completes();
    this->get_all();
}

TYPED_TEST(RootCollapseTest, RootCollapseWithRapidCP) {
    LOGINFO("=== TEST 5: Root Collapse with Rapid CP Cycling ===");

    uint32_t num_groups = 32;
    uint32_t keys_per_group = 512;
    this->build_tree_for_collapse(num_groups, keys_per_group);
    test_common::HSTestHelper::trigger_cp(true);
    for (uint32_t group = num_groups - 1; group >= 2 && group < num_groups; --group) {
        LOGINFO("Removing group {}", group);

        uint64_t group_prefix = (0xaa + group) << 24;
        for (uint32_t offset = 0; offset < keys_per_group; ++offset) {
            uint64_t key = group_prefix | offset;
            this->remove_scattered(key);
        }

        test_common::HSTestHelper::trigger_cp(false);
    }

    this->verify_cp_completes();
    this->get_all();
}

TYPED_TEST(RootCollapseTest, MultiTreeRootCollapse) {
    LOGINFO("=== TEST 6: Multi-Tree Root Collapse ===");

    uint32_t num_groups = 8;
    uint32_t keys_per_group = 256;
    auto uuid2 = boost::uuids::random_generator()();
    auto parent_uuid2 = boost::uuids::random_generator()();
    auto bt2 = std::make_shared< typename TypeParam::BtreeType >(uuid2, parent_uuid2, 0, this->m_cfg);
    hs()->index_service().add_index_table(bt2);

    this->build_tree_for_collapse(num_groups, keys_per_group);
    for (uint32_t group = 0; group < num_groups; ++group) {
        uint64_t group_prefix = (0xaa + group) << 24;
        for (uint32_t offset = 0; offset < keys_per_group; ++offset) {
            uint64_t key = group_prefix | offset;
            auto existing_v = std::make_unique< typename TypeParam::ValueType >();
            auto pkey = std::make_unique< typename TypeParam::KeyType >(key);
            auto val = TypeParam::ValueType::generate_rand();
            auto sreq = BtreeSinglePutRequest{pkey.get(), &val, btree_put_type::UPSERT, existing_v.get()};
            bt2->put(sreq);
        }
    }

    test_common::HSTestHelper::trigger_cp(true);

    this->trigger_root_collapse(1, num_groups, keys_per_group);
    for (uint32_t group = num_groups - 1; group >= 1 && group < num_groups; --group) {
        uint64_t group_prefix = (0xaa + group) << 24;
        for (uint32_t offset = 0; offset < keys_per_group; ++offset) {
            uint64_t key = group_prefix | offset;
            auto existing_v = std::make_unique< typename TypeParam::ValueType >();
            auto pk = std::make_unique< typename TypeParam::KeyType >(key);
            auto rreq = BtreeSingleRemoveRequest{pk.get(), existing_v.get()};
            bt2->remove(rreq);
        }
    }

    this->verify_cp_completes();
    hs()->index_service().remove_index_table(bt2);
    this->get_all();
}

int main(int argc, char* argv[]) {
    int parsed_argc{argc};
    ::testing::InitGoogleTest(&parsed_argc, argv);
    SISL_OPTIONS_LOAD(parsed_argc, argv, logging, test_index_cp_root_collapse, iomgr, test_common_setup);
    sisl::logging::SetLogger("test_index_cp_root_collapse");
    spdlog::set_pattern("[%D %T%z] [%^%L%$] [%t] %v");

    return RUN_ALL_TESTS();
}
