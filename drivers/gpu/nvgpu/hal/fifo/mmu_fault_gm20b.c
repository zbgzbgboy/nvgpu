/*
 * Copyright (c) 2014-2019, NVIDIA CORPORATION.  All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <nvgpu/gk20a.h>
#include <nvgpu/timers.h>
#include <nvgpu/log.h>
#include <nvgpu/io.h>
#include <nvgpu/fifo.h>
#include <nvgpu/engines.h>

#include <hal/fifo/mmu_fault_gm20b.h>

#include <nvgpu/hw/gm20b/hw_fifo_gm20b.h>

static inline u32 gm20b_engine_id_to_fault_id(struct gk20a *g,
			u32 engine_id)
{
	u32 fault_id = INVAL_ID;
	struct fifo_engine_info_gk20a *engine_info;

	engine_info = nvgpu_engine_get_active_eng_info(g, engine_id);

	if (engine_info != NULL) {
		fault_id = engine_info->fault_id;
	} else {
		nvgpu_err(g, "engine_id is not in active list/invalid %d",
				engine_id);
	}
	return fault_id;
}

void gm20b_fifo_trigger_mmu_fault(struct gk20a *g,
		unsigned long engine_ids_bitmask)
{
	unsigned long poll_delay = POLL_DELAY_MIN_US;
	unsigned long engine_id;
	int ret;
	struct nvgpu_timeout timeout;
	u32 fault_id;

	/* set trigger mmu fault */
	for_each_set_bit(engine_id, &engine_ids_bitmask, 32UL) {
		if (!nvgpu_engine_check_valid_id(g, (u32)engine_id)) {
			nvgpu_err(g, "faulting unknown engine %ld", engine_id);
			continue;
		}
		fault_id = gm20b_engine_id_to_fault_id(g, (u32)engine_id);

		if (fault_id == INVAL_ID) {
			continue;
		}
		nvgpu_writel(g, fifo_trigger_mmu_fault_r(fault_id),
				     fifo_trigger_mmu_fault_enable_f(1U));
	}

	ret = nvgpu_timeout_init(g, &timeout, nvgpu_get_poll_timeout(g),
			   NVGPU_TIMER_CPU_TIMER);
	if (ret != 0) {
		nvgpu_err(g, "timeout init failed err=%d", ret);
	}

	/* Wait for MMU fault to trigger */
	ret = -EBUSY;
	do {
		if ((nvgpu_readl(g, fifo_intr_0_r()) &
		     fifo_intr_0_mmu_fault_pending_f()) != 0U) {
			ret = 0;
			break;
		}

		nvgpu_usleep_range(poll_delay, poll_delay * 2UL);
		poll_delay = min_t(u32, poll_delay << 1, POLL_DELAY_MAX_US);
	} while (nvgpu_timeout_expired(&timeout) == 0);

	if (ret != 0) {
		nvgpu_err(g, "timeout: failed to trigger mmu fault");
	}

	/* release trigger mmu fault */
	for_each_set_bit(engine_id, &engine_ids_bitmask, 32UL) {
		nvgpu_writel(g, fifo_trigger_mmu_fault_r(engine_id), 0);
	}
}
