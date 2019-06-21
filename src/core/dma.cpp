/*
 * Copyright (C) 2018 Frederic Meyer. All rights reserved.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "cpu.hpp"
#include "dma.hpp"

using namespace ARM;
using namespace NanoboyAdvance::GBA;

/* Retrieves DMA with highest priority from a DMA bitset. */
static constexpr int g_dma_from_bitset[] = {
    /* 0b0000 */ -1,
    /* 0b0001 */  0,
    /* 0b0010 */  1,
    /* 0b0011 */  0,
    /* 0b0100 */  2,
    /* 0b0101 */  0,
    /* 0b0110 */  1,
    /* 0b0111 */  0,
    /* 0b1000 */  3,
    /* 0b1001 */  0,
    /* 0b1010 */  1,
    /* 0b1011 */  0,
    /* 0b1100 */  2,
    /* 0b1101 */  0,
    /* 0b1110 */  1,
    /* 0b1111 */  0
};

void DMAController::Reset() {
    dma_hblank_mask = 0;
    dma_vblank_mask = 0;
    dma_run_set = 0;
    dma_current = 0;
    dma_interleaved = false;
    
    for (int id = 0; id < 4; id++) {
        dma[id].enable = false;
        dma[id].repeat = false;
        dma[id].interrupt = false;
        dma[id].gamepak  = false;
        dma[id].length   = 0;
        dma[id].dst_addr = 0;
        dma[id].src_addr = 0;
        dma[id].internal.length   = 0;
        dma[id].internal.dst_addr = 0;
        dma[id].internal.src_addr = 0;
        dma[id].size     = DMA_HWORD;
        dma[id].time     = DMA_IMMEDIATE;
        dma[id].dst_cntl = DMA_INCREMENT;
        dma[id].src_cntl = DMA_INCREMENT;
    }
}

auto DMAController::Read(int id, int offset) -> std::uint8_t {
    /* TODO: are SAD/DAD/CNT_L readable? */
    switch (offset) {
        /* DMAXCNT_H */
        case 10: {
            return (dma[id].dst_cntl << 5) |
                   (dma[id].src_cntl << 7);
        }
        case 11: {
            return (dma[id].src_cntl >> 1) |
                   (dma[id].size     << 2) |
                   (dma[id].time     << 4) |
                   (dma[id].repeat    ? 2   : 0) |
                   (dma[id].gamepak   ? 8   : 0) |
                   (dma[id].interrupt ? 64  : 0) |
                   (dma[id].enable    ? 128 : 0);
        }
        default: return 0;
    }
}

void DMAController::Write(int id, int offset, std::uint8_t value) {
    switch (offset) {
        /* DMAXSAD */
        case 0: dma[id].src_addr = (dma[id].src_addr & 0xFFFFFF00) | (value<<0 ); break;
        case 1: dma[id].src_addr = (dma[id].src_addr & 0xFFFF00FF) | (value<<8 ); break;
        case 2: dma[id].src_addr = (dma[id].src_addr & 0xFF00FFFF) | (value<<16); break;
        case 3: dma[id].src_addr = (dma[id].src_addr & 0x00FFFFFF) | (value<<24); break;

        /* DMAXDAD */
        case 4: dma[id].dst_addr = (dma[id].dst_addr & 0xFFFFFF00) | (value<<0 ); break;
        case 5: dma[id].dst_addr = (dma[id].dst_addr & 0xFFFF00FF) | (value<<8 ); break;
        case 6: dma[id].dst_addr = (dma[id].dst_addr & 0xFF00FFFF) | (value<<16); break;
        case 7: dma[id].dst_addr = (dma[id].dst_addr & 0x00FFFFFF) | (value<<24); break;

        /* DMAXCNT_L */
        case 8: dma[id].length = (dma[id].length & 0xFF00) | (value<<0); break;
        case 9: dma[id].length = (dma[id].length & 0x00FF) | (value<<8); break;

        /* DMAXCNT_H */
        case 10:
            dma[id].dst_cntl = static_cast<DMAControl>((value >> 5) & 3);
            dma[id].src_cntl = static_cast<DMAControl>((dma[id].src_cntl & 0b10) | (value>>7));
            break;
        case 11: {
            bool enable_previous = dma[id].enable;

            dma[id].src_cntl  = static_cast<DMAControl>((dma[id].src_cntl & 0b01) | ((value & 1)<<1));
            dma[id].size      = static_cast<DMASize>((value>>2) & 1);
            dma[id].time      = static_cast<DMATime>((value>>4) & 3);
            dma[id].repeat    = value & 2;
            dma[id].gamepak   = value & 8;
            dma[id].interrupt = value & 64;
            dma[id].enable    = value & 128;

            if (dma[id].time == DMA_HBLANK) {
                dma_hblank_mask |=  (1<<id);
                dma_vblank_mask &= ~(1<<id);
            } else if (dma[id].time == DMA_VBLANK) { 
                dma_hblank_mask &= ~(1<<id);
                dma_vblank_mask |=  (1<<id);
            } else {
                dma_hblank_mask &= ~(1<<id);
                dma_vblank_mask &= ~(1<<id);
            }
                
            /* DMA state is latched on "rising" enable bit. */
            if (!enable_previous && dma[id].enable) {
                /* Latch sanitized values into internal DMA state. */
                dma[id].internal.dst_addr = dma[id].dst_addr & s_dma_dst_mask[id];
                dma[id].internal.src_addr = dma[id].src_addr & s_dma_src_mask[id];
                dma[id].internal.length   = dma[id].length   & s_dma_len_mask[id];

                if (dma[id].internal.length == 0) {
                    dma[id].internal.length = s_dma_len_mask[id] + 1;
                }

                /* Schedule DMA if is setup for immediate execution. */
                if (dma[id].time == DMA_IMMEDIATE) {
                    MarkDMAForExecution(id);
                }
            }
            break;
        }
    }
}

void DMAController::MarkDMAForExecution(int id) {
    // Defer execution of immediate DMA if another higher priority DMA is still running.
    // Otherwise go ahead at set is as the currently running DMA.
    if (dma_run_set == 0) {
        dma_current = id;
    } else if (id < dma_current) {
        dma_current = id;
        dma_interleaved = true;
    }

    // Mark DMA as enabled.
    dma_run_set |= (1 << id);
}

void DMAController::TriggerHBlankDMA() {
//    for (int i = 0; i < 4; i++) {
//        auto& dma = mmio.dma[i];
//        if (dma.enable && dma.time == DMA_HBLANK) {
//            MarkDMAForExecution(i);
//        }
//    }
    int hblank_dma = g_dma_from_bitset[dma_run_set & dma_hblank_mask];
    
    if (hblank_dma >= 0)
        MarkDMAForExecution(hblank_dma);
}

void DMAController::TriggerVBlankDMA() {
//    for (int i = 0; i < 4; i++) {
//        auto& dma = mmio.dma[i];
//        if (dma.enable && dma.time == DMA_VBLANK) {
//            MarkDMAForExecution(i);
//        }
//    }
    int vblank_dma = g_dma_from_bitset[dma_run_set & dma_vblank_mask];
    
    if (vblank_dma >= 0)
        MarkDMAForExecution(vblank_dma);
}

void DMAController::Run() {
    auto& dma = this->dma[dma_current];
    
    const auto src_cntl = dma.src_cntl;
    const auto dst_cntl = dma.dst_cntl;
    const bool words = dma.size == DMA_WORD;
    
    /* TODO: what happens if src_cntl equals DMA_RELOAD? */
    const int modify_table[2][4] = {
        { 2, -2, 0, 2 },
        { 4, -4, 0, 4 }
    };
    
    const int src_modify = modify_table[dma.size][src_cntl];
    const int dst_modify = modify_table[dma.size][dst_cntl];

    std::uint32_t word;
    
    /* Run DMA until completion or interruption. */
    if (words) {
        while (dma.internal.length != 0) {
            if (cpu->run_until <= 0) return;
            
            /* Stop if DMA was interleaved by higher priority DMA. */
            if (dma_interleaved) {
                dma_interleaved = false;
                return;
            }
            
            word = cpu->ReadWord(dma.internal.src_addr, ACCESS_SEQ);
            cpu->WriteWord(dma.internal.dst_addr, word, ACCESS_SEQ);
            
            dma.internal.src_addr += src_modify;
            dma.internal.dst_addr += dst_modify;
            dma.internal.length--;
        }
    } else {
        while (dma.internal.length != 0) {
            if (cpu->run_until <= 0) return;
            
            /* Stop if DMA was interleaved by higher priority DMA. */
            if (dma_interleaved) {
                dma_interleaved = false;
                return;
            }
            
            word = cpu->ReadHalf(dma.internal.src_addr, ACCESS_SEQ);
            cpu->WriteHalf(dma.internal.dst_addr, word, ACCESS_SEQ);
            
            dma.internal.src_addr += src_modify;
            dma.internal.dst_addr += dst_modify;
            dma.internal.length--;
        }
    }
    
    /* If this code path is reached, the DMA has completed. */
    
    if (dma.interrupt) {
        cpu->mmio.irq_if |= CPU::INT_DMA0 << dma_current;
    }
    
    if (dma.repeat) {
        /* Reload the internal length counter. */
        dma.internal.length = dma.length & s_dma_len_mask[dma_current];
        if (dma.internal.length == 0) {
            dma.internal.length = s_dma_len_mask[dma_current] + 1;
        }

        /* Reload destination address if specified. */
        if (dst_cntl == DMA_RELOAD) {
            dma.internal.dst_addr = dma.dst_addr & s_dma_dst_mask[dma_current];
        }

        /* If DMA is specified to be non-immediate, wait for it to be retriggered. */
        if (dma.time != DMA_IMMEDIATE) {
            dma_run_set &= ~(1 << dma_current);
        }
    } else {
        dma.enable = false;
        dma_run_set &= ~(1 << dma_current);
    }
    
    if (dma_run_set > 0) {
        dma_current = g_dma_from_bitset[dma_run_set];
    }
}