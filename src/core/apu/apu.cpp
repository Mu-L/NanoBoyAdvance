/*
 * Copyright (C) 2018 Frederic Meyer. All rights reserved.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "apu.hpp"
#include "../cpu.hpp"

#include <cstdio>

using namespace NanoboyAdvance::GBA;

void APU::Reset() {
    mmio.fifo[0].Reset();
    mmio.fifo[1].Reset();
    mmio.soundcnt.Reset();
}
    
void APU::LatchFIFO(int id, int times) {
    auto& fifo = mmio.fifo[id];
    
    for (int time = 0; time < times; time++) {
        latch[id] = fifo.Read();
        std::printf("latch[%d]=%d\n", id, latch[id]);
        
        // HACK: we should match FIFO the DMA.
        if (fifo.Count() <= 16) {
            cpu->dma.RunFIFO(1 + id);
        }
    }
}