// SPDX-FileCopyrightText: © 2024 Tenstorrent Inc.
//
// SPDX-License-Identifier: Apache-2.0

#include "dev_msgs.h"
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <vector>

#include "core_config.h"  // ProgrammableCoreType
#include "dev_mem_map.h"  // MEM_LOCAL_BASE
#include "hal_types.hpp"
#include "hw/inc/wormhole/eth_l1_address_map.h"
#include "llrt/hal.hpp"
#include "noc/noc_overlay_parameters.h"
#include "noc/noc_parameters.h"
#include "tensix.h"
#include "wormhole/wh_hal.hpp"

// Reserved DRAM addresses
// Host writes (4B value) to and reads from DRAM_BARRIER_BASE across all channels to ensure previous writes have been
// committed
constexpr static std::uint32_t DRAM_BARRIER_BASE = 0;
constexpr static std::uint32_t DRAM_BARRIER_SIZE =
    ((sizeof(uint32_t) + DRAM_ALIGNMENT - 1) / DRAM_ALIGNMENT) * DRAM_ALIGNMENT;

constexpr static std::uint32_t DRAM_PROFILER_BASE = DRAM_BARRIER_BASE + DRAM_BARRIER_SIZE;
#if defined(TRACY_ENABLE)
constexpr static std::uint32_t MAX_NUM_UNHARVESTED_TENSIX_CORES = 80;
constexpr static std::uint32_t MAX_NUM_ETH_CORES = 16;
constexpr static std::uint32_t MAX_NUM_CORES = MAX_NUM_UNHARVESTED_TENSIX_CORES + MAX_NUM_ETH_CORES;
constexpr static std::uint32_t NUM_DRAM_CHANNELS = 12;
constexpr static std::uint32_t CEIL_NUM_CORES_PER_DRAM_CHANNEL =
    (MAX_NUM_CORES + NUM_DRAM_CHANNELS - 1) / NUM_DRAM_CHANNELS;
constexpr static std::uint32_t DRAM_PROFILER_SIZE =
    (((PROFILER_FULL_HOST_BUFFER_SIZE_PER_RISC * MAX_RISCV_PER_CORE * CEIL_NUM_CORES_PER_DRAM_CHANNEL) +
      DRAM_ALIGNMENT - 1) /
     DRAM_ALIGNMENT) *
    DRAM_ALIGNMENT;
#else
constexpr static std::uint32_t DRAM_PROFILER_SIZE = 0;
#endif

constexpr static std::uint32_t DRAM_UNRESERVED_BASE = DRAM_PROFILER_BASE + DRAM_PROFILER_SIZE;
constexpr static std::uint32_t DRAM_UNRESERVED_SIZE = MEM_DRAM_SIZE - DRAM_UNRESERVED_BASE;

static constexpr float EPS_WHB0 = 1.19209e-7f;
static constexpr float NAN_WHB0 = 7.0040e+19;
static constexpr float INF_WHB0 = 1.7014e+38;

namespace tt {

namespace tt_metal {

void Hal::initialize_wh(bool is_base_routing_fw_enabled) {
    static_assert(static_cast<int>(HalProgrammableCoreType::TENSIX) == static_cast<int>(ProgrammableCoreType::TENSIX));
    static_assert(
        static_cast<int>(HalProgrammableCoreType::ACTIVE_ETH) == static_cast<int>(ProgrammableCoreType::ACTIVE_ETH));
    static_assert(
        static_cast<int>(HalProgrammableCoreType::IDLE_ETH) == static_cast<int>(ProgrammableCoreType::IDLE_ETH));

    static_assert(MaxProcessorsPerCoreType <= MAX_RISCV_PER_CORE);

    HalCoreInfoType tensix_mem_map = wormhole::create_tensix_mem_map();
    this->core_info_.push_back(tensix_mem_map);

    HalCoreInfoType active_eth_mem_map = wormhole::create_active_eth_mem_map(is_base_routing_fw_enabled);
    this->core_info_.push_back(active_eth_mem_map);

    HalCoreInfoType idle_eth_mem_map = wormhole::create_idle_eth_mem_map();
    this->core_info_.push_back(idle_eth_mem_map);

    this->dram_bases_.resize(static_cast<std::size_t>(HalDramMemAddrType::COUNT));
    this->dram_sizes_.resize(static_cast<std::size_t>(HalDramMemAddrType::COUNT));
    this->dram_bases_[static_cast<std::size_t>(HalDramMemAddrType::BARRIER)] = DRAM_BARRIER_BASE;
    this->dram_sizes_[static_cast<std::size_t>(HalDramMemAddrType::BARRIER)] = DRAM_BARRIER_SIZE;
    this->dram_bases_[static_cast<std::size_t>(HalDramMemAddrType::PROFILER)] = DRAM_PROFILER_BASE;
    this->dram_sizes_[static_cast<std::size_t>(HalDramMemAddrType::PROFILER)] = DRAM_PROFILER_SIZE;
    this->dram_bases_[static_cast<std::size_t>(HalDramMemAddrType::UNRESERVED)] = DRAM_UNRESERVED_BASE;
    this->dram_sizes_[static_cast<std::size_t>(HalDramMemAddrType::UNRESERVED)] = DRAM_UNRESERVED_SIZE;

    this->mem_alignments_.resize(static_cast<std::size_t>(HalMemType::COUNT));
    this->mem_alignments_[static_cast<std::size_t>(HalMemType::L1)] = L1_ALIGNMENT;
    this->mem_alignments_[static_cast<std::size_t>(HalMemType::DRAM)] = DRAM_ALIGNMENT;
    this->mem_alignments_[static_cast<std::size_t>(HalMemType::HOST)] = PCIE_ALIGNMENT;

    this->mem_alignments_with_pcie_.resize(static_cast<std::size_t>(HalMemType::COUNT));
    this->mem_alignments_with_pcie_[static_cast<std::size_t>(HalMemType::L1)] = std::lcm(L1_ALIGNMENT, PCIE_ALIGNMENT);
    this->mem_alignments_with_pcie_[static_cast<std::size_t>(HalMemType::DRAM)] =
        std::lcm(DRAM_ALIGNMENT, PCIE_ALIGNMENT);
    this->mem_alignments_with_pcie_[static_cast<std::size_t>(HalMemType::HOST)] =
        std::lcm(PCIE_ALIGNMENT, PCIE_ALIGNMENT);

    this->relocate_func_ = [](uint64_t addr, uint64_t local_init_addr) {
        if ((addr & MEM_LOCAL_BASE) == MEM_LOCAL_BASE) {
            // Move addresses in the local memory range to l1 (copied by kernel)
            return (addr & ~MEM_LOCAL_BASE) + local_init_addr;
        } else if ((addr & MEM_NCRISC_IRAM_BASE) == MEM_NCRISC_IRAM_BASE) {
            // Move addresses in the NCRISC memory range to l1 (copied by kernel)
            return (addr & ~MEM_NCRISC_IRAM_BASE) + MEM_NCRISC_INIT_IRAM_L1_BASE_SCRATCH;
        }

        // No relocation needed
        return addr;
    };

    this->erisc_iram_relocate_func_ = [](uint64_t addr) {
        if (addr == static_cast<uint32_t>(eth_iram_mem::address_map::ERISC_IRAM_BASE)) {
            // IRAM enabled program starts from ERISC_IRAM_BASE. This relocation is for where to put the program.
            // At first the program is placed on ERISC_IRAM_BASE, then erisc.cc copies to local IRAM.
            return (uint64_t)eth_l1_mem::address_map::KERNEL_BASE;
        }
        return addr;
    };

    this->valid_reg_addr_func_ = [](uint32_t addr) {
        return (
            ((addr >= NOC_OVERLAY_START_ADDR) &&
             (addr < NOC_OVERLAY_START_ADDR + NOC_STREAM_REG_SPACE_SIZE * NOC_NUM_STREAMS)) ||
            ((addr >= NOC0_REGS_START_ADDR) && (addr < NOC0_REGS_START_ADDR + 0x1000)) ||
            ((addr >= NOC1_REGS_START_ADDR) && (addr < NOC1_REGS_START_ADDR + 0x1000)) ||
            (addr == RISCV_DEBUG_REG_SOFT_RESET_0));
    };

    this->noc_xy_encoding_func_ = [](uint32_t x, uint32_t y) { return NOC_XY_ENCODING(x, y); };
    this->noc_multicast_encoding_func_ = [](uint32_t x_start, uint32_t y_start, uint32_t x_end, uint32_t y_end) {
        return NOC_MULTICAST_ENCODING(x_start, y_start, x_end, y_end);
    };
    this->noc_mcast_addr_start_x_func_ = [](uint64_t addr) -> uint64_t { return NOC_MCAST_ADDR_START_X(addr); };
    this->noc_mcast_addr_start_y_func_ = [](uint64_t addr) -> uint64_t { return NOC_MCAST_ADDR_START_Y(addr); };
    this->noc_mcast_addr_end_x_func_ = [](uint64_t addr) -> uint64_t { return NOC_MCAST_ADDR_END_X(addr); };
    this->noc_mcast_addr_end_y_func_ = [](uint64_t addr) -> uint64_t { return NOC_MCAST_ADDR_END_Y(addr); };
    this->noc_ucast_addr_x_func_ = [](uint64_t addr) -> uint64_t { return NOC_UNICAST_ADDR_X(addr); };
    this->noc_ucast_addr_y_func_ = [](uint64_t addr) -> uint64_t { return NOC_UNICAST_ADDR_Y(addr); };
    this->noc_local_addr_func_ = [](uint64_t addr) -> uint64_t { return NOC_LOCAL_ADDR(addr); };

    this->eth_fw_arg_addr_func_ = [&](uint32_t arg_index) -> uint32_t { return 0; };

    this->num_nocs_ = NUM_NOCS;
    this->noc_node_id_ = NOC_NODE_ID;
    this->noc_node_id_mask_ = NOC_NODE_ID_MASK;
    this->noc_addr_node_id_bits_ = NOC_ADDR_NODE_ID_BITS;
    this->noc_encoding_reg_ = COORDINATE_VIRTUALIZATION_ENABLED ? NOC_CFG(NOC_ID_LOGICAL) : NOC_NODE_ID;
    this->noc_coord_reg_offset_ = NOC_COORD_REG_OFFSET;
    this->noc_overlay_start_addr_ = NOC_OVERLAY_START_ADDR;
    this->noc_stream_reg_space_size_ = NOC_STREAM_REG_SPACE_SIZE;
    this->noc_stream_remote_dest_buf_size_reg_index_ = STREAM_REMOTE_DEST_BUF_SIZE_REG_INDEX;
    this->noc_stream_remote_dest_buf_start_reg_index_ = STREAM_REMOTE_DEST_BUF_START_REG_INDEX;
    this->noc_stream_remote_dest_buf_space_available_reg_index_ = STREAM_REMOTE_DEST_BUF_SPACE_AVAILABLE_REG_INDEX;
    this->noc_stream_remote_dest_buf_space_available_update_reg_index_ =
        STREAM_REMOTE_DEST_BUF_SPACE_AVAILABLE_UPDATE_REG_INDEX;
    this->coordinate_virtualization_enabled_ = COORDINATE_VIRTUALIZATION_ENABLED;
    this->virtual_worker_start_x_ = VIRTUAL_TENSIX_START_X;
    this->virtual_worker_start_y_ = VIRTUAL_TENSIX_START_Y;
    this->eth_fw_is_cooperative_ = true;
    this->intermesh_eth_links_enabled_ = true;  // Intermesh routing is enabled on Wormhole
    this->virtualized_core_types_ = {AddressableCoreType::TENSIX, AddressableCoreType::ETH};
    this->tensix_harvest_axis_ = static_cast<HalTensixHarvestAxis>(tensix_harvest_axis);

    this->eps_ = EPS_WHB0;
    this->nan_ = NAN_WHB0;
    this->inf_ = INF_WHB0;

    this->noc_x_id_translate_table_ = {
        NOC_CFG(NOC_X_ID_TRANSLATE_TABLE_0),
        NOC_CFG(NOC_X_ID_TRANSLATE_TABLE_1),
        NOC_CFG(NOC_X_ID_TRANSLATE_TABLE_2),
        NOC_CFG(NOC_X_ID_TRANSLATE_TABLE_3)};

    this->noc_y_id_translate_table_ = {
        NOC_CFG(NOC_Y_ID_TRANSLATE_TABLE_0),
        NOC_CFG(NOC_Y_ID_TRANSLATE_TABLE_1),
        NOC_CFG(NOC_Y_ID_TRANSLATE_TABLE_2),
        NOC_CFG(NOC_Y_ID_TRANSLATE_TABLE_3)};
}

}  // namespace tt_metal
}  // namespace tt
