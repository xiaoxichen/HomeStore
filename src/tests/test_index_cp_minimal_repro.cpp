/*********************************************************************************
 * MINIMAL REPRODUCER for Index CP stuck bug
 *
 * This is the simplest possible test to reproduce the root collapse CP hang.
 * Use this for quick debugging and verification.
 *
 * Bug: Root collapse creates Meta → CLEAN buffer dependency, CP hangs
 *********************************************************************************/
#include <gtest/gtest.h>
#include <boost/uuid/random_generator.hpp>

#include "common/homestore_config.hpp"
#include "test_common/homestore_test_common.hpp"
#include "btree_helpers/btree_test_helper.hpp"
#include "btree_helpers/btree_test_kvs.hpp"
#include "btree_helpers/btree_decls.h"

using namespace homestore;

SISL_LOGGING_INIT(HOMESTORE_LOG_MODS)
SISL_OPTIONS_ENABLE(logging, test_index_cp_minimal, iomgr, test_common_setup)
SISL_LOGGING_DECL(test_index_cp_minimal)

SISL_OPTION_GROUP(
    test_index_cp_minimal,
    (num_entries, "", "num_entries", "number of entries to test with",
     ::cxxopts::value< uint32_t >()->default_value("4000"), "number"))

class MinimalRootCollapseTest : public ::testing::Test {
public:
    using BtreeType = FixedLenBtree::BtreeType;
    using K = TestFixedKey;
    using V = TestFixedValue;

    std::shared_ptr< BtreeType > m_bt;
    BtreeConfig m_cfg{4096}; // Will be re-initialized in SetUp with actual node size
    test_common::HSTestHelper m_helper;

    class TestIndexServiceCallbacks : public IndexServiceCallbacks {
    public:
        TestIndexServiceCallbacks(MinimalRootCollapseTest* test) : m_test(test) {}
        std::shared_ptr< IndexTableBase > on_index_table_found(superblk< index_table_sb >&& sb) override {
            m_test->m_bt = std::make_shared< BtreeType >(std::move(sb), m_test->m_cfg);
            return m_test->m_bt;
        }
    private:
        MinimalRootCollapseTest* m_test;
    };

    void SetUp() override {
        m_helper.start_homestore(
            "test_minimal",
            {{HS_SERVICE::META, {.size_pct = 10.0}},
             {HS_SERVICE::INDEX, {.size_pct = 70.0, .index_svc_cbs = new TestIndexServiceCallbacks(this)}}});

        m_cfg = BtreeConfig(hs()->index_service().node_size());
        m_cfg.m_merge_turned_on = true; // MUST enable merge for root collapse

        auto uuid = boost::uuids::random_generator()();
        auto parent_uuid = boost::uuids::random_generator()();
        m_bt = std::make_shared< BtreeType >(uuid, parent_uuid, 0, m_cfg);
        hs()->index_service().add_index_table(m_bt);

        LOGINFO("=== Minimal Root Collapse Test Started ===");
        LOGINFO("Node size: {}", hs()->index_service().node_size());
    }

    void TearDown() override {
        m_helper.shutdown_homestore(true);
    }

    void insert(uint32_t key) {
        auto existing_v = std::make_unique< V >();
        auto k = std::make_unique< K >(key);
        auto v = V::generate_rand();
        auto sreq = BtreeSinglePutRequest{k.get(), &v, btree_put_type::INSERT, existing_v.get()};
        auto status = m_bt->put(sreq);
        ASSERT_EQ(status, btree_status_t::success) << "Insert failed for key " << key;
    }

    void remove(uint32_t key) {
        auto existing_v = std::make_unique< V >();
        auto pk = std::make_unique< K >(key);
        auto rreq = BtreeSingleRemoveRequest{pk.get(), existing_v.get()};
        auto status = m_bt->remove(rreq);
        ASSERT_EQ(status, btree_status_t::success) << "Remove failed for key " << key;
    }

    void print_tree_state(const std::string& label) {
        auto [interior_nodes, leaf_nodes] = m_bt->get_num_nodes();
        LOGINFO("{}: depth={} interior={} leaf={}",
                label,
                m_bt->get_btree_depth(),
                interior_nodes,
                leaf_nodes);
    }
};

//
// The absolute minimal test to reproduce the bug
//
TEST_F(MinimalRootCollapseTest, ReproduceBug) {
    auto num_entries = SISL_OPTIONS["num_entries"].as< uint32_t >();
    LOGINFO("Step 1: Insert {} entries to build multi-level tree", num_entries);
    for (uint32_t i = 0; i < num_entries; ++i) {
        insert(i);
    }
    print_tree_state("After inserts");

    ASSERT_GT(m_bt->get_btree_depth(), 1) << "Need multi-level tree for root collapse";

    LOGINFO("Step 2: Flush to persist initial state");
    test_common::HSTestHelper::trigger_cp(true);

    LOGINFO("Step 3: Remove most entries to trigger root collapse");
    for (uint32_t i = num_entries - 1; i >= 10; --i) {
        remove(i);
    }
    print_tree_state("After removals");

    LOGINFO("Step 4: Trigger CP - THIS WILL HANG if bug exists!");
    LOGINFO("        Set timeout (e.g., 'timeout 30s ./test') to detect hang");

    auto start = std::chrono::steady_clock::now();

    // THIS IS THE CRITICAL LINE - will hang with the bug
    test_common::HSTestHelper::trigger_cp(true);

    auto elapsed_ms = std::chrono::duration_cast< std::chrono::milliseconds >(
        std::chrono::steady_clock::now() - start).count();

    LOGINFO("CP completed in {}ms", elapsed_ms);

    ASSERT_LT(elapsed_ms, 10000) << "CP took > 10s - likely stuck on CLEAN buffer dependency";

    LOGINFO("=== TEST PASSED - Bug not present ===");
}

//
// Debug variant with detailed logging
//
TEST_F(MinimalRootCollapseTest, DebugRootCollapse) {
    LOGINFO("=== DEBUG VERSION - Extra Logging ===");

    // Smaller tree for easier debugging
    LOGINFO("Inserting 100 entries");
    for (uint32_t i = 0; i < 100; ++i) {
        insert(i);
        if (i % 20 == 0) {
            print_tree_state(fmt::format("After {} inserts", i));
        }
    }

    auto initial_depth = m_bt->get_btree_depth();
    LOGINFO("Initial tree depth: {}", initial_depth);

    test_common::HSTestHelper::trigger_cp(true);

    LOGINFO("Removing entries 99 down to 5");
    for (uint32_t i = 99; i >= 5; --i) {
        remove(i);

        if (m_bt->get_btree_depth() < initial_depth) {
            LOGINFO("!!! ROOT COLLAPSE DETECTED at key {} !!!", i);
            LOGINFO("    Old depth: {}, New depth: {}", initial_depth, m_bt->get_btree_depth());
            initial_depth = m_bt->get_btree_depth();
        }

        if (i % 10 == 0) {
            print_tree_state(fmt::format("After removing down to {}", i));
        }
    }

    LOGINFO("Triggering CP after root collapse...");
    auto start = std::chrono::steady_clock::now();

    test_common::HSTestHelper::trigger_cp(true);

    auto elapsed_ms = std::chrono::duration_cast< std::chrono::milliseconds >(
        std::chrono::steady_clock::now() - start).count();

    LOGINFO("CP completed in {}ms", elapsed_ms);
    ASSERT_LT(elapsed_ms, 10000);

    LOGINFO("=== DEBUG TEST PASSED ===");
}

int main(int argc, char* argv[]) {
    int parsed_argc{argc};
    ::testing::InitGoogleTest(&parsed_argc, argv);
    SISL_OPTIONS_LOAD(parsed_argc, argv, logging, test_index_cp_minimal, iomgr, test_common_setup);
    sisl::logging::SetLogger("test_index_cp_minimal");
    spdlog::set_pattern("[%D %T%z] [%^%L%$] [%t] %v");

    LOGINFO("╔═══════════════════════════════════════════════════════════╗");
    LOGINFO("║      MINIMAL INDEX CP ROOT COLLAPSE BUG REPRODUCER       ║");
    LOGINFO("║                                                           ║");
    LOGINFO("║  If this test HANGS at 'Step 4', the bug is present.    ║");
    LOGINFO("║  Use: timeout 30s ./test_index_cp_minimal                ║");
    LOGINFO("╚═══════════════════════════════════════════════════════════╝");

    return RUN_ALL_TESTS();
}
