/*********************************************************************************
 * Modifications Copyright 2017-2019 eBay Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed
 * under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations under the License.
 *
 *********************************************************************************/
#include <vector>

#include <iomgr/iomgr.hpp>
#include <homestore/crc.h>
#include <sisl/logging/logging.h>

#include <boost/uuid/random_generator.hpp>

#include <homestore/homestore_decl.hpp>
#include "device/chunk.h"
#include "device/device.h"
#include "device/physical_dev.hpp"
#include "device/virtual_dev.hpp"
#include "common/homestore_utils.hpp"
#include "common/homestore_assert.hpp"

namespace homestore {

static int determine_open_flags(io_flag oflags) {
    int open_flags;

    switch (oflags) {
    case io_flag::BUFFERED_IO:
        open_flags = O_RDWR | O_CREAT;
        break;
    case io_flag::READ_ONLY:
        open_flags = O_RDONLY;
        break;
    case io_flag::DIRECT_IO:
        open_flags = O_RDWR | O_CREAT | O_DIRECT;
        break;
    default:
        open_flags = O_RDWR | O_CREAT;
    }

    return open_flags;
}

static bool is_hdd(const std::string& devname) {
    const iomgr::drive_type dtype = iomgr::DriveInterface::get_drive_type(devname);
    if (dtype == iomgr::drive_type::block_hdd || dtype == iomgr::drive_type::file_on_hdd) { return true; }
    return false;
}

static void populate_vdev_info(const vdev_parameters& vparam, uint32_t vdev_id,
                               const std::vector< PhysicalDev* >& pdevs, vdev_info* out_info);

DeviceManager::DeviceManager(const std::vector< dev_info >& devs, vdev_create_cb_t vdev_create_cb) :
        m_dev_infos{devs}, m_vdev_create_cb{std::move(vdev_create_cb)} {
    bool found_hdd_dev{false};
    for (const auto& dev_info : devs) {
        if (is_hdd(dev_info.dev_name)) {
            HomeStoreStaticConfig::instance().hdd_drive_present = true;
            found_hdd_dev = true;
            break;
        }
    }

    if (found_hdd_dev) {
        if ((HS_STATIC_CONFIG(input.data_open_flags) == io_flag::DIRECT_IO) &&
            !HS_DYNAMIC_CONFIG(device->direct_io_mode)) {
            // override direct i/o for HDD's
            LOGINFO("Overridding HDD open flags from DIRECT_IO to BUFFERED_IO");
            m_hdd_open_flags = determine_open_flags(io_flag::BUFFERED_IO);
        } else {
            m_hdd_open_flags = determine_open_flags(HS_STATIC_CONFIG(input.data_open_flags));
        }
    }
    m_ssd_open_flags = determine_open_flags(HS_STATIC_CONFIG(input.fast_open_flags));

    // Read from all the devices and check if there is a valid superblock present in those devices.
    m_first_time_boot = true;
    for (const auto& d : devs) {
        first_block fblk = PhysicalDev::read_first_block(d.dev_name, device_open_flags(d.dev_name));
        if (fblk.is_valid()) {
            if (fblk.hdr.gen_number > m_first_blk_hdr.gen_number) { m_first_blk_hdr = fblk.hdr; }
            m_first_time_boot = false;
            break;
        }
    }
}

void DeviceManager::format_devices() {
    ++m_first_blk_hdr.gen_number;
    m_first_blk_hdr.version = first_block_header::CURRENT_SUPERBLOCK_VERSION;
    std::strncpy(m_first_blk_hdr.product_name, first_block_header::PRODUCT_NAME,
                 first_block_header::s_product_name_size);
    m_first_blk_hdr.num_pdevs = uint32_cast(m_dev_infos.size());
    m_first_blk_hdr.max_vdevs = hs_super_blk::MAX_VDEVS_IN_SYSTEM;
    m_first_blk_hdr.max_system_chunks = hs_super_blk::MAX_CHUNKS_IN_SYSTEM;
    m_first_blk_hdr.system_uuid = boost::uuids::random_generator()();

    // Get common iomgr_attributes
    for (auto& dinfo : m_dev_infos) {
        auto attr = iomgr::DriveInterface::get_attributes(dinfo.dev_name);
        if (dinfo.dev_size == 0) { dinfo.dev_size = PhysicalDev::get_dev_size(dinfo.dev_name); }
        auto sb_size = hs_super_blk::total_used_size(dinfo);
        auto buf = hs_utils::iobuf_alloc(sb_size, sisl::buftag::superblk, attr.align_size);
        std::memset(buf, 0, sb_size);

        first_block* fblk = r_cast< first_block* >(buf);
        fblk->magic = first_block::HOMESTORE_MAGIC;
        fblk->checksum = 0;          // Computed while writing the first block
        fblk->hdr = m_first_blk_hdr; // Entire header is copied as is
        auto pdev_id = populate_pdev_info(dinfo, attr, m_first_blk_hdr.system_uuid, fblk->this_pdev_hdr);
        fblk->checksum = crc32_ieee(init_crc32, uintptr_cast(fblk), first_block::s_atomic_fb_size);

        auto pdev = std::make_unique< PhysicalDev >(dinfo, device_open_flags(dinfo.dev_name), fblk->this_pdev_hdr);

        LOGINFO("Formatting Homestore on Device={} with first block as: [{}] total_super_blk_size={}", dinfo.dev_name,
                fblk->to_string(), sb_size);
        pdev->write_super_block(buf, sb_size, hs_super_blk::first_block_offset());

        auto it = m_pdevs_by_type.find(dinfo.dev_type);
        if (it == m_pdevs_by_type.end()) {
            bool happened;
            std::tie(it, happened) = m_pdevs_by_type.insert(std::pair{dinfo.dev_type, std::vector< PhysicalDev* >{}});
        }
        it->second.push_back(pdev.get());

        pdev->format_chunks();
        m_all_pdevs[pdev_id] = std::move(pdev);

        hs_utils::iobuf_free(buf, sisl::buftag::superblk);
    }
}

void DeviceManager::load_devices() {
    RELEASE_ASSERT_EQ(m_first_blk_hdr.version, first_block_header::CURRENT_SUPERBLOCK_VERSION,
                      "We don't support superblock version upgrade yet");

    RELEASE_ASSERT_EQ(m_first_blk_hdr.num_pdevs, m_dev_infos.size(),
                      "WARNING: The homestore is formatted with {} devices, but restarted with {} devices. Homestore "
                      "does not support dynamic addition/removal of devices",
                      m_first_blk_hdr.num_pdevs, m_dev_infos.size());

    for (const auto& d : m_dev_infos) {
        first_block fblk = PhysicalDev::read_first_block(d.dev_name, device_open_flags(d.dev_name));
        pdev_info_header* pinfo = &fblk.this_pdev_hdr;

        RELEASE_ASSERT_EQ(pinfo->get_system_uuid_str(), m_first_blk_hdr.get_system_uuid_str(),
                          "Device {} has uuid stamp different than this instance uuid. Perhaps device from other "
                          "homestore is provided?",
                          d.dev_name);

        auto pdev = std::make_unique< PhysicalDev >(d, device_open_flags(d.dev_name), *pinfo);
        LOGINFO("Loading Homestore from Device={} with first block as: [{}]", d.dev_name, fblk.to_string());

        auto it = m_pdevs_by_type.find(d.dev_type);
        if (it == m_pdevs_by_type.end()) {
            bool happened;
            std::tie(it, happened) = m_pdevs_by_type.insert(std::pair{d.dev_type, std::vector< PhysicalDev* >{}});
        }
        it->second.push_back(pdev.get());

        m_all_pdevs[pinfo->pdev_id] = std::move(pdev);
    }

    load_vdevs();
}

void DeviceManager::close_devices() {
    for (auto& pdev : m_all_pdevs) {
        if (pdev) { pdev->close_device(); }
    }
}

shared< VirtualDev > DeviceManager::create_vdev(vdev_parameters&& vparam) {
    std::unique_lock lg{m_vdev_mutex};

    // Allocate a new vdev_id
    auto vdev_id = m_vdev_id_bm.get_next_reset_bit(0u);
    if (vdev_id == sisl::Bitset::npos) { throw std::out_of_range("System has no room for additional vdev"); }
    m_vdev_id_bm.set_bit(vdev_id);

    // Determine if we have a devices available on requested dev_tier. If so use them, else fallback to data tier
    std::vector< PhysicalDev* > pdevs = pdevs_by_type_internal(vparam.dev_type);
    RELEASE_ASSERT_GT(pdevs.size(), 0, "Unable to find any pdevs for even data tier, can't create vdev");

    // Identify the number of chunks
    if (vparam.multi_pdev_opts == vdev_multi_pdev_opts_t::ALL_PDEV_STRIPED) {
        auto total_streams = std::accumulate(pdevs.begin(), pdevs.end(), 0u,
                                             [](int r, const PhysicalDev* a) { return r + a->num_streams(); });
        vparam.num_chunks = sisl::round_up(vparam.num_chunks, total_streams);
    } else if (vparam.multi_pdev_opts == vdev_multi_pdev_opts_t::ALL_PDEV_MIRRORED) {
        vparam.num_chunks = sisl::round_up(vparam.num_chunks, pdevs[0]->num_streams()) * pdevs.size();
    } else if (vparam.multi_pdev_opts == vdev_multi_pdev_opts_t::SINGLE_FIRST_PDEV) {
        pdevs.erase(pdevs.begin() + 1, pdevs.end()); // Just pick first device
    } else {
        pdevs.erase(pdevs.begin() + 1, pdevs.end()); // TODO: Pick random one
    }

    // Adjust the maximum number chunks requested before round up vdev size.
    uint32_t max_num_chunks = 0;
    for (const auto& d : m_dev_infos) {
        max_num_chunks += hs_super_blk::max_chunks_in_pdev(d);
    }
    auto input_num_chunks = vparam.num_chunks;
    vparam.num_chunks = std::min(vparam.num_chunks, max_num_chunks);
    if (input_num_chunks != vparam.num_chunks) {
        LOGINFO("{} Virtual device is attempted to be created with num_chunks={}, it needs to be adjust to "
                "new_num_chunks={}",
                vparam.vdev_name, in_bytes(input_num_chunks), in_bytes(vparam.num_chunks));
    }

    auto input_vdev_size = vparam.vdev_size;
    vparam.vdev_size = sisl::round_up(vparam.vdev_size, vparam.num_chunks * vparam.blk_size);
    if (input_vdev_size != vparam.vdev_size) {
        LOGINFO("{} Virtual device is attempted to be created with size={}, it needs to be rounded to new_size={}",
                vparam.vdev_name, in_bytes(input_vdev_size), in_bytes(vparam.vdev_size));
    }

    uint32_t chunk_size = vparam.vdev_size / vparam.num_chunks;

    LOGINFO(
        "New Virtal Dev={} of size={} with id={} is attempted to be created with multi_pdev_opts={}. The params are "
        "adjusted as follows: VDev_Size={} Num_pdevs={} Total_chunks_across_all_pdevs={} Each_Chunk_Size={}",
        vparam.vdev_name, in_bytes(input_vdev_size), vdev_id, vparam.multi_pdev_opts, in_bytes(vparam.vdev_size),
        pdevs.size(), vparam.num_chunks, in_bytes(chunk_size));

    // Convert the vparameters to the vdev_info
    auto buf = hs_utils::iobuf_alloc(vdev_info::size, sisl::buftag::superblk, pdevs[0]->align_size());
    auto vinfo = new (buf) vdev_info();
    populate_vdev_info(vparam, vdev_id, pdevs, vinfo);

    // Do a callback for the upper layer to create the vdev instance from vdev_info
    shared< VirtualDev > vdev = m_vdev_create_cb(*vinfo, false /* load_existing */);
    m_vdevs[vdev_id] = vdev;

    // Create initial chunk based on current size
    for (auto& pdev : pdevs) {
        std::vector< uint32_t > chunk_ids;

        // Create chunk ids for all chunks in each of these pdevs
        for (uint32_t c{0}; c < vparam.num_chunks / pdevs.size(); ++c) {
            auto chunk_id = m_chunk_id_bm.get_next_reset_bit(0u);
            if (chunk_id == sisl::Bitset::npos) { throw std::out_of_range("System has no room for additional chunks"); }
            m_chunk_id_bm.set_bit(chunk_id);
            chunk_ids.push_back(chunk_id);
        }

        // Create all chunks at one shot and add each one to the vdev
        auto chunks = pdev->create_chunks(chunk_ids, vdev_id, chunk_size);
        for (auto& chunk : chunks) {
            vdev->add_chunk(chunk, true /* fresh_chunk */);
            m_chunks[chunk->chunk_id()] = chunk;
        }
    }

    // Locate and write the vdev info in the super blk area of all pdevs this vdev will be created on
    for (auto& pdev : pdevs) {
        uint64_t offset = hs_super_blk::vdev_sb_offset() + (vdev_id * vdev_info::size);
        pdev->write_super_block(buf, vdev_info::size, offset);
    }

    vinfo->~vdev_info();
    hs_utils::iobuf_free(buf, sisl::buftag::superblk);
    LOGINFO("Virtal Dev={} of size={} successfully created", vparam.vdev_name, in_bytes(vparam.vdev_size));
    return vdev;
}

void DeviceManager::load_vdevs() {
    std::unique_lock lg{m_vdev_mutex};

    for (auto& [dtype, pdevs] : m_pdevs_by_type) {
        auto vdev_infos = read_vdev_infos(pdevs);

        for (auto& vinfo : vdev_infos) {
            m_vdev_id_bm.set_bit(vinfo.vdev_id);
            m_vdevs[vinfo.vdev_id] = m_vdev_create_cb(vinfo, true /* load_existing */);
        }
    }

    // There are some vdevs load their chunks in each of pdev
    if (m_vdevs.size()) {
        for (auto& pdev : m_all_pdevs) {
            pdev->load_chunks([this](cshared< Chunk >& chunk) -> bool {
                // Found a chunk for which vdev information is missing
                if (m_vdevs[chunk->vdev_id()] == nullptr) {
                    LOGWARN("Found a chunk id={}, which is expected to be part of vdev_id={}, but that vdev "
                            "information is missing, may be before vdev is created, system crashed. Need upper layer "
                            "to retry vdev create",
                            chunk->chunk_id(), chunk->vdev_id());
                    return false;
                }
                m_chunk_id_bm.set_bit(chunk->chunk_id());
                m_chunks[chunk->chunk_id()] = chunk;
                m_vdevs[chunk->vdev_id()]->add_chunk(chunk, false /* fresh_chunk */);
                return true;
            });
        }
    }
}

uint32_t DeviceManager::populate_pdev_info(const dev_info& dinfo, const iomgr::drive_attributes& attr,
                                           const uuid_t& uuid, pdev_info_header& pinfo) {
    bool hdd = is_hdd(dinfo.dev_name);

    pinfo.pdev_id = m_cur_pdev_id++;
    pinfo.mirror_super_block = hdd ? 0x01 : 0x00;
    pinfo.max_pdev_chunks = hs_super_blk::max_chunks_in_pdev(dinfo);

    auto sb_size = hs_super_blk::total_size(dinfo);
    pinfo.data_offset = hs_super_blk::first_block_offset() + sb_size;
    pinfo.size = dinfo.dev_size - pinfo.data_offset - (hdd ? sb_size : 0);
    pinfo.dev_attr = attr;
    pinfo.system_uuid = uuid;

    return pinfo.pdev_id;
}

uint64_t DeviceManager::total_capacity() const {
    uint64_t cap{0};
    for (const auto& pdev : m_all_pdevs) {
        cap += pdev->data_size();
    }
    return cap;
}

uint64_t DeviceManager::total_capacity(HSDevType dtype) const {
    uint64_t cap{0};
    const auto& pdevs = pdevs_by_type_internal(dtype);
    for (const auto& pdev : pdevs) {
        cap += pdev->data_size();
    }
    return cap;
}

static void populate_vdev_info(const vdev_parameters& vparam, uint32_t vdev_id,
                               const std::vector< PhysicalDev* >& pdevs, vdev_info* out_info) {
    out_info->vdev_size = vparam.vdev_size;
    out_info->vdev_id = vdev_id;
    out_info->num_mirrors = (vparam.multi_pdev_opts == vdev_multi_pdev_opts_t::ALL_PDEV_MIRRORED) ? pdevs.size() : 0;
    out_info->blk_size = vparam.blk_size;
    out_info->num_primary_chunks =
        (vparam.multi_pdev_opts == vdev_multi_pdev_opts_t::ALL_PDEV_STRIPED) ? pdevs.size() : 1u;
    out_info->set_allocated();
    out_info->set_dev_type(vparam.dev_type);
    out_info->set_pdev_choice(vparam.multi_pdev_opts);
    out_info->set_name(vparam.vdev_name);
    out_info->set_user_private(vparam.context_data);
    out_info->alloc_type = s_cast< uint8_t >(vparam.alloc_type);
    out_info->chunk_sel_type = s_cast< uint8_t >(vparam.chunk_sel_type);
    out_info->compute_checksum();
}

std::vector< vdev_info > DeviceManager::read_vdev_infos(const std::vector< PhysicalDev* >& pdevs) {
    std::vector< vdev_info > ret_vinfos;
    auto buf =
        hs_utils::iobuf_alloc(hs_super_blk::vdev_super_block_size(), sisl::buftag::superblk, pdevs[0]->align_size());

    // TODO: Read from all pdevs and validate that they are correct
    pdevs[0]->read_super_block(buf, hs_super_blk::vdev_super_block_size(), hs_super_blk::vdev_sb_offset());

    uint8_t* ptr = buf;
    for (uint32_t v{0}; v < hs_super_blk::MAX_VDEVS_IN_SYSTEM; ++v, ptr += vdev_info::size) {
        vdev_info* vinfo = r_cast< vdev_info* >(ptr);
        if (vinfo->checksum != 0) {
            auto expected_crc = vinfo->checksum;
            vinfo->checksum = 0;
            auto crc = crc16_t10dif(hs_init_crc_16, r_cast< const unsigned char* >(vinfo), sizeof(vdev_info));
            RELEASE_ASSERT_EQ(crc, expected_crc, "VDev id={} mismatch on crc", v);
            vinfo->checksum = crc;
        }

        if (vinfo->slot_allocated) { ret_vinfos.push_back(*vinfo); }
    }

    hs_utils::iobuf_free(buf, sisl::buftag::superblk);
    return ret_vinfos;
}

int DeviceManager::device_open_flags(const std::string& devname) const {
    return is_hdd(devname) ? m_hdd_open_flags : m_ssd_open_flags;
}

std::vector< PhysicalDev* > DeviceManager::get_pdevs_by_dev_type(HSDevType dtype) const {
    return m_pdevs_by_type.at(dtype);
}

const std::vector< PhysicalDev* >& DeviceManager::pdevs_by_type_internal(HSDevType dtype) const {
    auto it = m_pdevs_by_type.find(dtype);
    if (it == m_pdevs_by_type.cend()) { it = m_pdevs_by_type.find(HSDevType::Data); }
    return it->second;
}

uint32_t DeviceManager::atomic_page_size(HSDevType dtype) const {
    return pdevs_by_type_internal(dtype)[0]->atomic_page_size();
}

uint32_t DeviceManager::optimal_page_size(HSDevType dtype) const {
    return pdevs_by_type_internal(dtype)[0]->optimal_page_size();
}

std::vector< shared< VirtualDev > > DeviceManager::get_vdevs() const {
    std::vector< shared< VirtualDev > > ret_v;
    for (const auto& vdev : m_vdevs) {
        if (vdev != nullptr) { ret_v.push_back(vdev); }
    }
    return ret_v;
}

// Some of the hs_super_blk details
uint64_t hs_super_blk::vdev_super_block_size() { return (hs_super_blk::MAX_VDEVS_IN_SYSTEM * vdev_info::size); }

uint64_t hs_super_blk::chunk_super_block_size(const dev_info& dinfo) {
    return chunk_info_bitmap_size(dinfo) + (max_chunks_in_pdev(dinfo) * chunk_info::size);
}

} // namespace homestore
