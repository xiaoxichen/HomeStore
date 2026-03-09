# Index CP Root Collapse Bug - Unit Tests

## Bug Summary

**Issue**: Index CP gets stuck when B-tree root collapse creates dependencies on CLEAN buffers

**Root Cause**:
1. When B-tree root collapses (tree height reduces), `on_root_changed()` is called
2. Meta buffer is marked DIRTY and added to CP's dirty list
3. `transact_bufs()` creates dependency: `Meta → new_root`
4. **BUG**: `new_root` buffer remains CLEAN (never calls `write_node_impl`)
5. During CP flush, only DIRTY buffers are flushed
6. Meta buffer waits forever (`down_wait# > 0`) for CLEAN buffer that never flushes
7. **CP hangs indefinitely**

## Test Files

### test_index_cp_root_collapse.cpp

Contains 6 comprehensive test scenarios to reproduce the bug:

#### TEST 1: BasicRootCollapse ⭐ (Core Bug Reproduction)
- **What**: Simplest reproduction of the bug
- **How**:
  1. Build multi-level tree (root → interior → leaves)
  2. Remove most entries to trigger root collapse
  3. New root promoted (CLEAN buffer)
  4. Trigger CP flush
- **Expected Without Fix**: CP hangs/times out waiting for CLEAN buffer
- **Expected With Fix**: CP completes normally

#### TEST 2: MultipleRootCollapses
- **What**: Multiple consecutive tree height reductions
- **How**: Build deep tree (3+ levels), remove in stages, trigger collapse multiple times
- **Stress**: Multiple CLEAN buffer dependencies in same CP

#### TEST 3: RootCollapseWithConcurrentOps
- **What**: Root collapse mixed with other operations in same CP
- **How**: Trigger collapse, then add new entries in same CP
- **Stress**: Ensures root collapse doesn't interfere with normal dirty buffers

#### TEST 4: CollapseToLeaf
- **What**: Most aggressive collapse - tree shrinks to single leaf
- **How**: Build small tree, remove almost all entries
- **Stress**: Edge case of complete tree shrink

#### TEST 5: RootCollapseWithRapidCP
- **What**: Rapid CP cycling during removals
- **How**: Remove in batches with non-blocking CP triggers between batches
- **Stress**: Timing window between dependency creation and CP switch

#### TEST 6: MultiTreeRootCollapse
- **What**: Multiple index tables collapsing simultaneously
- **How**: Create 2+ trees, trigger collapse in both
- **Stress**: Multiple Meta buffers with CLEAN dependencies in same CP

## How to Run

### Build
```bash
cd HomeStore/build
cmake ..
make test_index_cp_root_collapse
```

### Run All Tests
```bash
./bin/test_index_cp_root_collapse
```

### Run Specific Test
```bash
./bin/test_index_cp_root_collapse --gtest_filter="*BasicRootCollapse*"
```

### Run with Verbose Logging
```bash
RUST_LOG=trace ./bin/test_index_cp_root_collapse --log_level=trace
```

### Run with Timeout Detection (to catch hangs)
```bash
timeout 60s ./bin/test_index_cp_root_collapse
# If it times out, CP is stuck!
```

## Expected Behavior

### WITHOUT the fix:
- Tests will **HANG** at `verify_cp_completes()`
- Timeout after 30 seconds (default)
- Logs show: `Meta buffer down_wait#=1` never decrements
- CLEAN buffer never enters flush queue

### WITH the fix:
- All tests **PASS** quickly (< 5 seconds each)
- CP completes normally
- Logs show proper flush sequence

## Key Indicators of Bug

Watch for these log patterns when bug is present:

```
# Root collapse happens
root changed for index old_root=X new_root=Y

# Meta buffer added with dependency
add to dirty list cp N [Meta] Buf=0x... down_wait#=0
# Then down_wait# increments to 1 after transact_bufs

# During CP flush - Meta stuck waiting
[Meta] Buf=0x... state=DIRTY down_wait#=1  # <- Stuck!

# new_root buffer is CLEAN, not in flush queue
Buf=0x... state=CLEAN  # <- This buffer is what Meta waits for!
```

## The Fix

The correct fix (to be implemented in `index_table.hpp`):

```cpp
btree_status_t on_root_changed(BtreeNodePtr const& new_root, void* context) {
    m_sb->root_node = new_root->node_id();
    // ... update superblock ...

    // FIX: Add new_root to dirty list BEFORE creating dependency
    write_node_impl(new_root, context);  // ← NEW LINE

    // Then add Meta to dirty list
    if (!wb_cache().refresh_meta_buf(m_sb_buffer, context)) {
        return btree_status_t::cp_mismatch;
    }

    // Finally create dependency (both now in dirty list)
    auto& root_buf = static_cast<IndexBtreeNode*>(new_root.get())->m_idx_buf;
    wb_cache().transact_bufs(ordinal(), m_sb_buffer, root_buf, {}, {}, context);

    return btree_status_t::success;
}
```

**Order**: Child → Parent → Dependencies (matches `transact_nodes` pattern)

## Additional Detection

Add this validation in `wb_cache.cpp::get_next_bufs_internal()`:

```cpp
#ifdef _PRERELEASE
// Validate buffer dependencies before flushing
for (auto const& down_buf : (*buf)->m_down_buffers) {
    bool valid =
        (down_buf->state() == index_buf_state_t::DIRTY &&
         down_buf->m_dirtied_cp_id == cp_ctx->id()) ||
        (down_buf->state() == index_buf_state_t::CLEAN &&
         down_buf->m_dirtied_cp_id < cp_ctx->id());

    HS_REL_ASSERT(valid,
        "CP {} dependency violation:\n  Up: {}\n  Down: {} (state={}, cp={})",
        cp_ctx->id(), (*buf)->to_string(), down_buf->to_string(),
        down_buf->state() == index_buf_state_t::CLEAN ? "CLEAN" : "DIRTY",
        down_buf->m_dirtied_cp_id);
}
#endif
```

## References

- Bug analysis document: [analyze.md]
- Code locations:
  - `check_collapse_root`: `btree/detail/btree_remove_impl.ipp:180`
  - `on_root_changed`: `index/index_table.hpp:373`
  - `transact_bufs`: `index/wb_cache.cpp:265`
  - `link_buf`: `index/wb_cache.cpp:363`
  - CP flush logic: `index/wb_cache.cpp:1035`
