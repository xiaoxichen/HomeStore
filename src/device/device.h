/*
 * device.h
 *
 *  Created on: 05-Aug-2016
 *      Author: Hari Kadayam
 */

#pragma once

#define BOOST_UUID_RANDOM_PROVIDER_FORCE_POSIX 1

#include <boost/intrusive/list.hpp>
#include <sys/uio.h>
#include <unistd.h>
#include <exception>
#include <string>
#include <sds_logging/logging.h>
#include <fcntl.h>
#include "blkalloc/blk_allocator.h"
#include <boost/uuid/uuid.hpp>            // uuid class
#include <boost/uuid/uuid_generators.hpp> // generators
#include <boost/uuid/uuid_io.hpp>         // streaming operators etc.
#include "homeds/array/sparse_vector.hpp"
#include "iomgr/iomgr.hpp"
#include "endpoint/drive_endpoint.hpp"
#include <boost/uuid/uuid_generators.hpp>

namespace homestore {

#define MAGIC                            0xCEEDDEEB
#define PRODUCT_NAME                     "OmStore"

/************* Super Block definition ******************/

#define CURRENT_SUPERBLOCK_VERSION           1
#define CURRENT_DM_INFO_VERSION              1

/*******************************************************************************************************
 *  _______________________             _________________________________________________________      *
 * |                       |           |                  |            |             |            |    *
 * |  Super block header   |---------->| Super Block info | Pdev Block | Chunk Block | Vdev Block |    *
 * |_______________________|           |__________________|____________|_____________|____________|    *         
 *                                                                                                     *
 *******************************************************************************************************/


/************* Physical Device Info Block definition ******************/

struct pdevs_block {
    uint64_t          magic;                // Header magic expected to be at the top of block
    uint32_t          num_phys_devs;     // Total number of physical devices in the entire system
    uint32_t          max_phys_devs;
    uint64_t          info_offset;
} __attribute((packed));

struct pdev_info_block {
    uint32_t           dev_num;              // Device ID for this store instance.
    uint32_t           first_chunk_id;       // First chunk id for this physical device
    uint64_t           dev_offset;           // Start offset of the device in global offset
} __attribute((packed));

/************* chunk Info Block definition ******************/

struct chunks_block {
    uint64_t          magic;                // Header magic expected to be at the top of block
    uint32_t          num_chunks;     // Number of physical chunks for this block
    uint32_t          max_num_chunks;
    uint64_t          info_offset;
} __attribute((packed));


struct chunk_info_block {
    uint64_t     chunk_start_offset;  // Start offset of the chunk within a pdev
    uint64_t     chunk_size;          // Chunk size
    uint32_t     chunk_id;            // Chunk id in global scope
    uint32_t     pdev_id;             // Physical device id this chunk is hosted on
    uint32_t     vdev_id;             // Virtual device id this chunk hosts. UINT32_MAX if chunk is free
    uint32_t     prev_chunk_id;       // Prev pointer in the chunk
    uint32_t     next_chunk_id;       // Next pointer in the chunk
    uint32_t     primary_chunk_id;     // Valid chunk id if this is a mirror of some chunk
    bool         slot_allocated;      // Is this slot allocated for any chunks.
    bool         is_sb_chunk;        // This chunk is not assigned to any vdev but super block
} __attribute((packed));

/************* Vdev Info Block definition ******************/
#define MAX_CONTEXT_DATA_SZ 512

struct vdevs_block {
    uint64_t        magic;                // Header magic expected to be at the top of block
    uint32_t        num_vdevs;         // Number of virtual devices
    uint32_t        max_num_vdevs;
    uint32_t        first_vdev_id;     // First vdev id / Head of the vdev list;
    uint64_t        info_offset;
    uint32_t        context_data_size;
} __attribute((packed));

struct vdev_info_block {
    uint32_t      vdev_id;            // Id for this vdev
    uint64_t      size;               // Size of the vdev
    uint32_t      num_mirrors;        // Total number of mirrors
    uint32_t      page_size;           // IO block size for this vdev
    uint32_t      prev_vdev_id;       // Prev pointer of vdevice list
    uint32_t      next_vdev_id;       // Next pointer of vdevice list
    bool          slot_allocated;     // Is this current slot allocated
    bool          failed;             // set to true if disk is replaced
    char          context_data[MAX_CONTEXT_DATA_SZ];    // Application dependent context data
    uint32_t      num_primary_chunks;
} __attribute((packed));

/*******************Super Block Definition*******************/

/* This header should be atomically written to the disks. It should always be smaller then ssd atomic page size */
struct super_block {
    uint64_t            magic;                // Header magic expected to be at the top of block
    uint32_t            version;              // Version Id of this structure
    uint64_t            gen_cnt;
    char                product_name[64];     // Product name
    int                 cur_indx;
    pdev_info_block     this_dev_info;        // Info about this device itself
    chunk_info_block    dm_chunk[2]; // chunk info blocks
    boost::uuids::uuid   system_uuid;
} __attribute((packed));
#define SUPERBLOCK_SIZE (HomeStoreConfig::atomic_phys_page_size)

struct dm_info {
    /* header of pdev, chunk and vdev */
    uint64_t           magic;                // Header magic expected to be at the top of block
    uint32_t           version;
    uint64_t           size;
    pdevs_block        pdev_hdr;
    chunks_block       chunk_hdr;
    vdevs_block        vdev_hdr;
} __attribute((packed));

#define PDEV_INFO_BLK_OFFSET sizeof(dm_info)
#define CHUNK_INFO_BLK_OFFSET (PDEV_INFO_BLK_OFFSET + (sizeof(pdev_info_block) * HomeStoreConfig::max_pdevs))
#define VDEV_INFO_BLK_OFFSET (CHUNK_INFO_BLK_OFFSET + sizeof(chunk_info_block) * HomeStoreConfig::max_chunks)

#define DM_INFO_BLK_SIZE (sizeof(dm_info) + PDEV_INFO_BLK_OFFSET + CHUNK_INFO_BLK_OFFSET + \
                            HomeStoreConfig::max_vdevs * sizeof(vdev_info_block))

#define INVALID_PDEV_ID   UINT32_MAX
#define INVALID_VDEV_ID   UINT32_MAX
#define INVALID_CHUNK_ID  UINT32_MAX
#define INVALID_DEV_ID  UINT32_MAX

class PhysicalDev;

class DeviceManager;
typedef std::function< void (int status, uint8_t* cookie) > comp_callback;

class PhysicalDevChunk {
public:
    friend class DeviceManager;

    PhysicalDevChunk(PhysicalDev *pdev, chunk_info_block *cinfo);
    PhysicalDevChunk(PhysicalDev *pdev, uint32_t chunk_id, uint64_t start_offset, uint64_t size, chunk_info_block *cinfo);

    const PhysicalDev *get_physical_dev() const {
        return m_pdev;
    }

    DeviceManager *device_manager() const;

    PhysicalDev *get_physical_dev_mutable() {
        return m_pdev;
    };

    void set_blk_allocator(std::shared_ptr< BlkAllocator > alloc) {
        m_allocator = alloc;
    }

    std::shared_ptr<BlkAllocator> get_blk_allocator() {
        return m_allocator;
    }

    void set_sb_chunk() {
        m_chunk_info->is_sb_chunk = true;
    }
    void set_start_offset(uint64_t offset) {
        m_chunk_info->chunk_start_offset = offset;
    }

    uint64_t get_start_offset() const {
        return m_chunk_info->chunk_start_offset;
    }

    void set_size(uint64_t size) {
        m_chunk_info->chunk_size = size;
    }

    uint64_t get_size() const {
        return m_chunk_info->chunk_size;
    }

    bool is_busy() const {
        return (m_chunk_info->vdev_id != INVALID_VDEV_ID || m_chunk_info->is_sb_chunk);
    }

    void set_free() {
        set_vdev_id(INVALID_VDEV_ID);
        m_chunk_info->primary_chunk_id = INVALID_CHUNK_ID;
        m_chunk_info->is_sb_chunk = false;
    }

    
    uint32_t get_vdev_id() const {
        return m_chunk_info->vdev_id;
    }

    void set_vdev_id(uint32_t vdev_id) {
        m_chunk_info->vdev_id = vdev_id;
    }

    void set_next_chunk_id(uint32_t next_chunk_id) {
        m_chunk_info->next_chunk_id = next_chunk_id;
    }

    void set_next_chunk(PhysicalDevChunk *next_chunk) {
        set_next_chunk_id(next_chunk ? next_chunk->get_chunk_id() : INVALID_CHUNK_ID);
    }

    uint32_t get_next_chunk_id() const {
        return m_chunk_info->next_chunk_id;
    }

    PhysicalDevChunk *get_next_chunk() const;

    void set_prev_chunk_id(uint32_t prev_chunk_id) {
        m_chunk_info->prev_chunk_id = prev_chunk_id;
    }

    void set_prev_chunk(PhysicalDevChunk *prev_chunk) {
        set_prev_chunk_id(prev_chunk ? prev_chunk->get_chunk_id() : INVALID_CHUNK_ID);
    }

    uint32_t get_prev_chunk_id() const {
        return m_chunk_info->prev_chunk_id;
    }

    PhysicalDevChunk *get_prev_chunk() const;

    chunk_info_block *get_chunk_info() {
        return m_chunk_info;
    }
    uint16_t get_chunk_id() const {
        return (uint16_t)m_chunk_info->chunk_id;
    }

    void free_slot() {
        m_chunk_info->slot_allocated = false;
    }

    PhysicalDevChunk *get_primary_chunk() const;
    
    void set_primary_chunk_id(uint32_t primary_id) {
        m_chunk_info->primary_chunk_id = primary_id;
    }

    std::string to_string() {
        std::stringstream ss;
        ss << "chunk_id = " << get_chunk_id() << " pdev_id = " << m_chunk_info->pdev_id
           << " vdev_id = " << m_chunk_info->vdev_id << " start_offset = " << m_chunk_info->chunk_start_offset
           << " size = " << m_chunk_info->chunk_size << " prev_chunk_id = " << m_chunk_info->prev_chunk_id
           << " next_chunk_id = " << m_chunk_info->next_chunk_id << " busy? = " << is_busy()
           << " slot_allocated? = " << m_chunk_info->slot_allocated;
        return ss.str();
    }

private:
    chunk_info_block *m_chunk_info;
    PhysicalDev *m_pdev;
    std::shared_ptr<BlkAllocator> m_allocator;
    uint64_t m_vdev_metadata_size;
};

class PhysicalDev {
    friend class PhysicalDevChunk;
    friend class DeviceManager;
public:

    PhysicalDev(DeviceManager *mgr, std::string &devname, int const oflags, std::shared_ptr<iomgr::ioMgr> iomgr,
                homeio::comp_callback &cb, boost::uuids::uuid &uuid, uint32_t dev_num, 
                uint64_t dev_offset, uint32_t is_file, bool is_init, uint64_t dm_info_size, bool *is_inited);
    ~PhysicalDev() = default;

    void update(uint32_t dev_num, uint64_t dev_offset, uint32_t first_chunk_id);
    void attach_superblock_chunk(PhysicalDevChunk *chunk);
    uint64_t sb_gen_cnt();
    
    int get_devfd() const {
        return m_devfd;
    }

    std::string get_devname() const {
        return m_devname;
    }

    uint64_t get_size() const {
        return m_devsize;
    }

    void set_dev_offset(uint64_t offset) {
        m_info_blk.dev_offset = offset;
    }

    uint32_t get_first_chunk_id() {
        return m_info_blk.first_chunk_id;
    }

    uint64_t get_dev_offset() const {
        return m_info_blk.dev_offset;
    }

    void set_dev_id(uint32_t id) {
        m_info_blk.dev_num = id;
    }

    uint32_t get_dev_id() const {
        return m_info_blk.dev_num;
    }

    DeviceManager *device_manager() const {
        return m_mgr;
    }

    std::string to_string();
    /* Attach the given chunk to the list of chunks in the physical device. Parameter after provides the position
     * it needs to attach after. If null, attach to the end */
    void attach_chunk(PhysicalDevChunk *chunk, PhysicalDevChunk *after);

    /* Merge previous and next chunk from the chunk, if either one or both of them free. Returns the array of
     * chunk id which were merged and can be freed if needed */
    std::array<uint32_t, 2> merge_free_chunks(PhysicalDevChunk *chunk);

    /* Find a free chunk which closestly match for the required size */
    PhysicalDevChunk *find_free_chunk(uint64_t req_size);

    void write(const char *data, uint32_t size, uint64_t offset, uint8_t *cookie);
    void writev(const struct iovec *iov, int iovcnt, uint32_t size, 
						uint64_t offset, uint8_t *cookie);

    void read(char *data, uint32_t size, uint64_t offset, uint8_t *cookie);
    void readv(const struct iovec *iov, int iovcnt, uint32_t size, 
						uint64_t offset, uint8_t *cookie);

    void sync_write(const char *data, uint32_t size, uint64_t offset);
    void sync_writev(const struct iovec *iov, int iovcnt, uint32_t size, 
						uint64_t offset);

    void sync_read(char *data, uint32_t size, uint64_t offset);
    void sync_readv(const struct iovec *iov, int iovcnt, uint32_t size, uint64_t offset);
    pdev_info_block get_info_blk();
    void read_dm_chunk(char *mem, uint64_t size);
    void write_dm_chunk(uint64_t gen_cnt, char *mem, uint64_t size);

private:
    inline void write_superblock();
    inline void read_superblock();

    /* Load the physical device info from persistent storage. If its not a valid device, it will throw
     * std::system_exception. Returns true if the device has already formatted for Omstore, false otherwise. */
    bool load_super_block();

    /* Format the physical device info. Intended to use first time or anytime we need to reformat the drives. Throws
     * std::system_exception if there is any write errors */
    void write_super_block(uint64_t gen_cnt);

    /* Validate if this device is a homestore validated device. If there is any corrupted device, then it
     * throws std::system_exception */
    bool validate_device();

private:
    DeviceManager     *m_mgr;              // Back pointer to physical device
    int                m_devfd;
    std::string        m_devname;
    super_block        *m_super_blk; // Persisent header block
    uint64_t           m_devsize;
    static homeio::DriveEndPoint *ep; // one instance for all physical devices
    homeio::comp_callback comp_cb;
    std::shared_ptr<iomgr::ioMgr> m_iomgr;
    struct pdev_info_block m_info_blk;
    int m_cur_indx;
    PhysicalDevChunk *m_dm_chunk[2];
    bool m_superblock_valid;
    boost::uuids::uuid m_system_uuid;
};

class AbstractVirtualDev {
public:
    virtual void add_chunk(PhysicalDevChunk *chunk) = 0;
};

class DeviceManager {
    typedef std::function< void (DeviceManager *, vdev_info_block *) > NewVDevCallback;
    typedef std::function< void(PhysicalDevChunk *) > chunk_add_callback;

    friend class PhysicalDev;
    friend class PhysicalDevChunk;

public:
    DeviceManager(NewVDevCallback vcb, uint32_t const vdev_metadata_size, std::shared_ptr<iomgr::ioMgr> iomgr, 
                  homeio::comp_callback comp_cb, bool is_file, boost::uuids::uuid system_uuid);

    virtual ~DeviceManager() = default;

    /* Initial routine to call upon bootup or everytime new physical devices to be added dynamically */
    void add_devices(std::vector< dev_info > &devices, bool is_init);

    /* This is not very efficient implementation of get_all_devices(), however, this is expected to be called during
     * the start of the devices and for that purpose its efficient enough */
    std::vector< PhysicalDev *> get_all_devices() {
        std::vector< PhysicalDev *> vec;
        std::lock_guard<decltype(m_dev_mutex)> lock(m_dev_mutex);

        vec.reserve(m_pdevs.size());
        for (auto &pdev : m_pdevs) {
            if (pdev) vec.push_back(pdev.get());
        }
        return vec;
    }

    /* Allocate a chunk for required size on the given physical dev and associate the chunk to provide virtual device.
     * Returns the allocated PhysicalDevChunk */
    PhysicalDevChunk *alloc_chunk(PhysicalDev *pdev, uint32_t vdev_id, uint64_t req_size, uint32_t primary_id);

    /* Free the chunk for later user */
    void free_chunk(PhysicalDevChunk *chunk);

    /* Allocate a new vdev for required size */
    vdev_info_block *alloc_vdev(uint32_t req_size, uint32_t nmirrors, uint32_t blk_size, uint32_t nchunks, char *blob, uint64_t size);

    /* Free up the vdev_id */
    void free_vdev(vdev_info_block *vb);

    /* Given an ID, get the chunk */
    PhysicalDevChunk *get_chunk(uint32_t chunk_id) const {
        return (chunk_id == INVALID_CHUNK_ID) ? nullptr : m_chunks[chunk_id].get();
    }

    PhysicalDevChunk *get_chunk_mutable(uint32_t chunk_id) {
        return (chunk_id == INVALID_CHUNK_ID) ? nullptr : m_chunks[chunk_id].get();
    }

    PhysicalDev *get_pdev(uint32_t pdev_id) const {
        return (pdev_id == INVALID_PDEV_ID) ? nullptr : m_pdevs[pdev_id].get();
    }

    void add_chunks(uint32_t vid, chunk_add_callback cb);
    void inited();
    void write_info_blocks();
    void update_vb_context(uint32_t vdev_id, uint8_t *blob);

private:
    void load_and_repair_devices(std::vector< dev_info > &devices);
    void init_devices(std::vector< dev_info > &devices);

    void read_info_blocks(uint32_t dev_id);

    chunk_info_block *alloc_new_chunk_slot(uint32_t *pslot_num);
    vdev_info_block *alloc_new_vdev_slot();

    PhysicalDevChunk *create_new_chunk(PhysicalDev *pdev, uint64_t start_offset, uint64_t size,
                                       PhysicalDevChunk *prev_chunk);
    void remove_chunk(uint32_t chunk_id);

private:
    int          m_open_flags;
    homeio::comp_callback comp_cb;
    NewVDevCallback  m_new_vdev_cb;
    std::shared_ptr<iomgr::ioMgr> m_iomgr;
    std::atomic<uint64_t> m_gen_cnt;
    bool m_is_file;

    char *m_chunk_memory;

    /* This memory is carved out of chunk memory. Any changes in any of the block should end up writing all the blocks 
     * on disk.
     */
    dm_info *m_dm_info;
    pdevs_block  *m_pdev_hdr;
    chunks_block *m_chunk_hdr;
    vdevs_block  *m_vdev_hdr;
    pdev_info_block *m_pdev_info;
    chunk_info_block *m_chunk_info;
    vdev_info_block *m_vdev_info;

    std::mutex   m_dev_mutex;

    homeds::sparse_vector< std::unique_ptr< PhysicalDev > > m_pdevs;
    homeds::sparse_vector< std::unique_ptr< PhysicalDevChunk > > m_chunks;
    homeds::sparse_vector< AbstractVirtualDev * > m_vdevs;
    uint32_t m_last_vdevid;
    uint32_t m_vdev_metadata_size; // Appln metadata size for vdev
    uint32_t m_pdev_id;
    bool m_scan_cmpltd;
    uint64_t m_dm_info_size;
    boost::uuids::uuid m_system_uuid;
};

} // namespace homestore
