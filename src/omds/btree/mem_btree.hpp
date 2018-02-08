/*
 * mem_btree.hpp
 *
 *  Created on: 14-Jun-2016
 *      Author: Hari Kadayam
 *
 *  Copyright © 2016 Kadayam, Hari. All rights reserved.
 */
#pragma once

#include <iostream>
#include <assert.h>
#include <pthread.h>
#include <vector>
#include <atomic>

#include "omds/memory/composite_allocator.hpp"
#include "omds/memory/chunk_allocator.hpp"
#include "omds/memory/sys_allocator.hpp"
#include "omds/utility/atomic_counter.hpp"
#include "btree_specific_impl.hpp"
#include "btree_node.h"
#include "physical_node.hpp"

namespace omds { namespace btree {

struct mem_btree_node_header {
    omds::atomic_counter<uint16_t> refcount;
} __attribute__((__packed__));

#define MemBtreeNodeDeclType BtreeNode<MEM_BTREE, K, V, InteriorNodeType, LeafNodeType, NodeSize>

template<
        typename K,
        typename V,
        btree_node_type InteriorNodeType,
        btree_node_type LeafNodeType,
        size_t NodeSize
        >
class BtreeSpecificImpl<MEM_BTREE, K, V, InteriorNodeType, LeafNodeType, NodeSize>
{
public:
    using HeaderType = mem_btree_node_header;

    static uint8_t *get_physical(const MemBtreeNodeDeclType *bn) {
        return (uint8_t *)((uint8_t *)bn + sizeof(MemBtreeNodeDeclType));
    }

    static uint32_t get_node_area_size() {
        return NodeSize - sizeof(MemBtreeNodeDeclType) - sizeof(LeafPhysicalNodeDeclType);
    }

    static boost::intrusive_ptr<MemBtreeNodeDeclType> alloc_node(bool is_leaf) {
        uint8_t *mem = BtreeNodeAllocator< NodeSize >::allocate();
        auto bn = new (mem) MemBtreeNodeDeclType();

        if (is_leaf) {
            auto n = new(mem + sizeof(MemBtreeNodeDeclType)) VariantNode<LeafNodeType, K, V, NodeSize>((bnodeid_t)mem, true);
        } else {
            auto n = new(mem + sizeof(MemBtreeNodeDeclType)) VariantNode<InteriorNodeType, K, V, NodeSize>((bnodeid_t)mem, true);
        }
        ref_node(bn);
        return (boost::intrusive_ptr<MemBtreeNodeDeclType>((MemBtreeNodeDeclType *)mem));
    }

    static boost::intrusive_ptr<MemBtreeNodeDeclType> read_node(bnodeid_t id) {
        auto bn = (MemBtreeNodeDeclType *)(uint8_t *)id.m_x;
        return boost::intrusive_ptr<MemBtreeNodeDeclType>(bn);
    }

    static void write_node(boost::intrusive_ptr<MemBtreeNodeDeclType> bn) {
    }

    static void free_node(boost::intrusive_ptr<MemBtreeNodeDeclType> bn) {
        if (deref_node(bn.get())) {
            bn->~MemBtreeNodeDeclType();
            BtreeNodeAllocator<NodeSize>::deallocate((uint8_t *)bn.get());
        }
    }

    static void ref_node(MemBtreeNodeDeclType *bn) {
        auto mbh = (mem_btree_node_header *)bn;
        mbh->refcount.increment();
    }

    static bool deref_node(MemBtreeNodeDeclType *bn) {
        auto mbh = (mem_btree_node_header *)bn;
        return mbh->refcount.decrement_testz();
    }
};

} }
