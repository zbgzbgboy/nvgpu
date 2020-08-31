/*
 * Copyright (c) 2019-2020, NVIDIA CORPORATION.  All rights reserved.
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
#include <nvgpu/nvgpu_err.h>
#include <nvgpu/io.h>
#include <nvgpu/bug.h>
#include <nvgpu/errno.h>
#include <nvgpu/static_analysis.h>
#include <nvgpu/gr/gr.h>
#include <nvgpu/gr/gr_instances.h>
#include <nvgpu/gr/config.h>
#include <nvgpu/gr/gr_intr.h>
#ifdef CONFIG_NVGPU_GRAPHICS
#include <nvgpu/gr/zbc.h>
#include <nvgpu/gr/zcull.h>
#endif
#include <nvgpu/netlist.h>
#include <nvgpu/gr/gr_falcon.h>
#include <nvgpu/gr/gr_utils.h>
#include <nvgpu/gr/ctx.h>
#include <nvgpu/gr/hwpm_map.h>
#include <nvgpu/gr/obj_ctx.h>
#include <nvgpu/gr/fs_state.h>
#include <nvgpu/gr/fecs_trace.h>
#include <nvgpu/power_features/cg.h>
#include <nvgpu/power_features/pg.h>
#include <nvgpu/mc.h>
#include <nvgpu/device.h>
#if defined(CONFIG_NVGPU_NON_FUSA) && defined(CONFIG_NVGPU_NEXT)
#include <nvgpu/engines.h>
#endif
#include <nvgpu/grmgr.h>

#include "gr_priv.h"

/*
 * Use this until common.gr is completely updated to support multiple
 * GR instances. Once that is supported, nvgpu_grmgr_get_num_gr_instances()
 * should be used to get number of GR instances.
 * Set this to 0 for local MIG testing.
 */
#define NVGPU_GR_NUM_INSTANCES		1

static int gr_alloc_global_ctx_buffers(struct gk20a *g)
{
	struct nvgpu_gr *gr = g->gr;
	int err;
	u32 size;

	nvgpu_log_fn(g, " ");

	size = g->ops.gr.init.get_global_ctx_cb_buffer_size(g);
	nvgpu_log_info(g, "cb_buffer_size : %d", size);

	nvgpu_gr_global_ctx_set_size(gr->global_ctx_buffer,
		NVGPU_GR_GLOBAL_CTX_CIRCULAR, size);
#ifdef CONFIG_NVGPU_VPR
	nvgpu_gr_global_ctx_set_size(gr->global_ctx_buffer,
		NVGPU_GR_GLOBAL_CTX_CIRCULAR_VPR, size);
#endif

	size = g->ops.gr.init.get_global_ctx_pagepool_buffer_size(g);
	nvgpu_log_info(g, "pagepool_buffer_size : %d", size);

	nvgpu_gr_global_ctx_set_size(gr->global_ctx_buffer,
		NVGPU_GR_GLOBAL_CTX_PAGEPOOL, size);
#ifdef CONFIG_NVGPU_VPR
	nvgpu_gr_global_ctx_set_size(gr->global_ctx_buffer,
		NVGPU_GR_GLOBAL_CTX_PAGEPOOL_VPR, size);
#endif
	size = g->ops.gr.init.get_global_attr_cb_size(g,
			nvgpu_gr_config_get_tpc_count(g->gr->config),
			nvgpu_gr_config_get_max_tpc_count(g->gr->config));
	nvgpu_log_info(g, "attr_buffer_size : %u", size);

	nvgpu_gr_global_ctx_set_size(gr->global_ctx_buffer,
		NVGPU_GR_GLOBAL_CTX_ATTRIBUTE, size);
#ifdef CONFIG_NVGPU_VPR
	nvgpu_gr_global_ctx_set_size(gr->global_ctx_buffer,
		NVGPU_GR_GLOBAL_CTX_ATTRIBUTE_VPR, size);
#endif
	size = NVGPU_GR_GLOBAL_CTX_PRIV_ACCESS_MAP_SIZE;
	nvgpu_log_info(g, "priv_access_map_size : %d", size);

	nvgpu_gr_global_ctx_set_size(gr->global_ctx_buffer,
		NVGPU_GR_GLOBAL_CTX_PRIV_ACCESS_MAP, size);

#ifdef CONFIG_NVGPU_FECS_TRACE
	size = nvgpu_gr_fecs_trace_buffer_size(g);
	nvgpu_log_info(g, "fecs_trace_buffer_size : %d", size);

	nvgpu_gr_global_ctx_set_size(gr->global_ctx_buffer,
		NVGPU_GR_GLOBAL_CTX_FECS_TRACE_BUFFER, size);
#endif

#ifdef CONFIG_NVGPU_DGPU
	if (g->ops.gr.init.get_rtv_cb_size != NULL) {
		size = g->ops.gr.init.get_rtv_cb_size(g);
		nvgpu_log_info(g, "rtv_circular_buffer_size : %u", size);

		nvgpu_gr_global_ctx_set_size(gr->global_ctx_buffer,
			NVGPU_GR_GLOBAL_CTX_RTV_CIRCULAR_BUFFER, size);
	}
#endif

	err = nvgpu_gr_global_ctx_buffer_alloc(g, gr->global_ctx_buffer);
	if (err != 0) {
		return err;
	}

	nvgpu_log_fn(g, "done");
	return 0;
}

u32 nvgpu_gr_get_no_of_sm(struct gk20a *g)
{
	return nvgpu_gr_config_get_no_of_sm(g->gr->config);
}

u32 nvgpu_gr_gpc_offset(struct gk20a *g, u32 gpc)
{
	u32 gpc_stride = nvgpu_get_litter_value(g, GPU_LIT_GPC_STRIDE);
	u32 gpc_offset = nvgpu_safe_mult_u32(gpc_stride , gpc);

	return gpc_offset;
}

u32 nvgpu_gr_tpc_offset(struct gk20a *g, u32 tpc)
{
	u32 tpc_in_gpc_stride = nvgpu_get_litter_value(g,
					GPU_LIT_TPC_IN_GPC_STRIDE);
	u32 tpc_offset = nvgpu_safe_mult_u32(tpc_in_gpc_stride, tpc);

	return tpc_offset;
}

u32 nvgpu_gr_sm_offset(struct gk20a *g, u32 sm)
{
	u32 sm_pri_stride = nvgpu_get_litter_value(g, GPU_LIT_SM_PRI_STRIDE);
	u32 sm_offset = nvgpu_safe_mult_u32(sm_pri_stride, sm);

	return sm_offset;
}

u32 nvgpu_gr_rop_offset(struct gk20a *g, u32 rop)
{
	u32 rop_pri_stride = nvgpu_get_litter_value(g, GPU_LIT_ROP_STRIDE);
	u32 rop_offset = nvgpu_safe_mult_u32(rop_pri_stride, rop);

	return rop_offset;
}

static void disable_gr_interrupts(struct gk20a *g)
{
	/** Disable gr intr */
	g->ops.gr.intr.enable_interrupts(g, false);

	/** Disable all exceptions */
	g->ops.gr.intr.enable_exceptions(g, g->gr->config, false);

	/** Disable interrupts at MC level */
	nvgpu_mc_intr_stall_unit_config(g, MC_INTR_UNIT_GR,
					MC_INTR_DISABLE);
	nvgpu_mc_intr_nonstall_unit_config(g, MC_INTR_UNIT_GR,
					   MC_INTR_DISABLE);
}

int nvgpu_gr_suspend(struct gk20a *g)
{
	int ret = 0;

	nvgpu_log_fn(g, " ");

	ret = g->ops.gr.init.wait_empty(g);
	if (ret != 0) {
		return ret;
	}

	/* Disable fifo access */
	g->ops.gr.init.fifo_access(g, false);

	disable_gr_interrupts(g);

	g->ops.gr.intr.flush_channel_tlb(g);

	g->gr->initialized = false;

	nvgpu_log_fn(g, "done");
	return ret;
}

static int gr_init_setup_hw(struct gk20a *g)
{
	struct nvgpu_gr *gr = g->gr;
	int err;

	nvgpu_log_fn(g, " ");

#if defined(CONFIG_NVGPU_HAL_NON_FUSA) && defined(CONFIG_NVGPU_NEXT)
	if (g->ops.gr.init.eng_config != NULL) {
		g->ops.gr.init.eng_config(g);
	}
#endif

	g->ops.gr.init.gpc_mmu(g);

	/* load gr floorsweeping registers */
	g->ops.gr.init.pes_vsc_stream(g);

#ifdef CONFIG_NVGPU_GRAPHICS
	err = nvgpu_gr_zcull_init_hw(g, gr->zcull, gr->config);
	if (err != 0) {
		goto out;
	}
#endif /* CONFIG_NVGPU_GRAPHICS */

	if (g->ops.priv_ring.set_ppriv_timeout_settings != NULL) {
		g->ops.priv_ring.set_ppriv_timeout_settings(g);
	}

	/** Enable fecs error interrupts */
	g->ops.gr.falcon.fecs_host_int_enable(g);

	g->ops.gr.intr.enable_hww_exceptions(g);

	/*
	 * SM HWWs are enabled during golden context creation, which happens
	 * at the time of first context creation i.e. first GPU job submission.
	 * Hence, injection of SM HWWs should only be attempted afterwards.
	 */

	/** Enable TPC exceptions per GPC */
	g->ops.gr.intr.enable_gpc_exceptions(g, gr->config);

	/** TBD: enable per BE exceptions */

	/* enable ECC for L1/SM */
	if (g->ops.gr.init.ecc_scrub_reg != NULL) {
		err = g->ops.gr.init.ecc_scrub_reg(g, gr->config);
		if (err != 0) {
			goto out;
		}
	}

	/** Reset and enable exceptions */
	g->ops.gr.intr.enable_exceptions(g, gr->config, true);

#ifdef CONFIG_NVGPU_GRAPHICS
	err = nvgpu_gr_zbc_load_table(g, gr->zbc);
	if (err != 0) {
		goto out;
	}
#endif /* CONFIG_NVGPU_GRAPHICS */

	/*
	 * Disable both surface and LG coalesce.
	 */
	if (g->ops.gr.init.su_coalesce != NULL) {
		g->ops.gr.init.su_coalesce(g, 0);
	}
	if (g->ops.gr.init.lg_coalesce != NULL) {
		g->ops.gr.init.lg_coalesce(g, 0);
	}

#ifdef CONFIG_NVGPU_GRAPHICS
	if (g->ops.gr.init.preemption_state != NULL) {
		err = g->ops.gr.init.preemption_state(g);
		if (err != 0) {
			goto out;
		}
	}
#endif

	/* floorsweep anything left */
	err = nvgpu_gr_fs_state_init(g, gr->config);
	if (err != 0) {
		goto out;
	}

	err = g->ops.gr.init.wait_idle(g);
out:
	nvgpu_log_fn(g, "done");
	return err;
}

static void gr_remove_support(struct gk20a *g)
{
	struct nvgpu_gr *gr = g->gr;

	nvgpu_log_fn(g, " ");

	nvgpu_gr_global_ctx_buffer_free(g, gr->global_ctx_buffer);
	nvgpu_gr_global_ctx_desc_free(g, gr->global_ctx_buffer);

	nvgpu_gr_ctx_desc_free(g, gr->gr_ctx_desc);

	nvgpu_gr_config_deinit(g, gr->config);

	nvgpu_netlist_deinit_ctx_vars(g);

#ifdef CONFIG_NVGPU_DEBUGGER
	nvgpu_gr_hwpm_map_deinit(g, gr->hwpm_map);
#endif

#ifdef CONFIG_NVGPU_GRAPHICS
	nvgpu_gr_zbc_deinit(g, gr->zbc);
	nvgpu_gr_zcull_deinit(g, gr->zcull);
#endif /* CONFIG_NVGPU_GRAPHICS */

	nvgpu_gr_obj_ctx_deinit(g, gr->golden_image);

	nvgpu_gr_free(g);
}

static int gr_init_access_map(struct gk20a *g, struct nvgpu_gr *gr)
{
	struct nvgpu_mem *mem;
	u32 nr_pages =
		DIV_ROUND_UP(NVGPU_GR_GLOBAL_CTX_PRIV_ACCESS_MAP_SIZE,
			     PAGE_SIZE);
	u32 nr_pages_size = nvgpu_safe_mult_u32(PAGE_SIZE, nr_pages);
#ifdef CONFIG_NVGPU_SET_FALCON_ACCESS_MAP
	u32 *whitelist = NULL;
	u32 w, num_entries = 0U;
#endif

	mem = nvgpu_gr_global_ctx_buffer_get_mem(gr->global_ctx_buffer,
			NVGPU_GR_GLOBAL_CTX_PRIV_ACCESS_MAP);
	if (mem == NULL) {
		return -EINVAL;
	}

	nvgpu_memset(g, mem, 0, 0, nr_pages_size);

#ifdef CONFIG_NVGPU_SET_FALCON_ACCESS_MAP
	g->ops.gr.init.get_access_map(g, &whitelist, &num_entries);

	for (w = 0U; w < num_entries; w++) {
		u32 map_bit, map_byte, map_shift, x;
		map_bit = whitelist[w] >> 2;
		map_byte = map_bit >> 3;
		map_shift = map_bit & 0x7U; /* i.e. 0-7 */
		nvgpu_log_info(g, "access map addr:0x%x byte:0x%x bit:%d",
			       whitelist[w], map_byte, map_shift);
		x = nvgpu_mem_rd32(g, mem, (u64)map_byte / (u64)sizeof(u32));
		x |= BIT32(
			   (map_byte % (u32)sizeof(u32) * BITS_PER_BYTE_U32)
			  + map_shift);
		nvgpu_mem_wr32(g, mem, (u64)map_byte / (u64)sizeof(u32), x);
	}
#endif

	return 0;
}

static int gr_init_config(struct gk20a *g, struct nvgpu_gr *gr)
{
	gr->config = nvgpu_gr_config_init(g);
	if (gr->config == NULL) {
		return -ENOMEM;
	}

	nvgpu_log_info(g, "bundle_cb_default_size: %d",
		g->ops.gr.init.get_bundle_cb_default_size(g));
	nvgpu_log_info(g, "min_gpm_fifo_depth: %d",
		g->ops.gr.init.get_min_gpm_fifo_depth(g));
	nvgpu_log_info(g, "bundle_cb_token_limit: %d",
		g->ops.gr.init.get_bundle_cb_token_limit(g));
	nvgpu_log_info(g, "attrib_cb_default_size: %d",
		g->ops.gr.init.get_attrib_cb_default_size(g));
	nvgpu_log_info(g, "attrib_cb_size: %d",
		g->ops.gr.init.get_attrib_cb_size(g,
			nvgpu_gr_config_get_tpc_count(gr->config)));
	nvgpu_log_info(g, "alpha_cb_default_size: %d",
		g->ops.gr.init.get_alpha_cb_default_size(g));
	nvgpu_log_info(g, "alpha_cb_size: %d",
		g->ops.gr.init.get_alpha_cb_size(g,
			nvgpu_gr_config_get_tpc_count(gr->config)));

	return 0;
}

static int nvgpu_gr_init_ctx_state(struct gk20a *g)
{
	int err = 0;

	if ((g->gr->golden_image != NULL) &&
		nvgpu_gr_obj_ctx_is_golden_image_ready(g->gr->golden_image)) {
		return err;
	}

	err = nvgpu_gr_falcon_init_ctx_state(g, g->gr->falcon);
	if (err != 0) {
		nvgpu_err(g, "gr ctx_state init failed");
	}

	return err;
}

static int gr_init_ctx_and_map_zbc(struct gk20a *g)
{
	struct nvgpu_gr *gr = g->gr;
	int err = 0;

	gr->gr_ctx_desc = nvgpu_gr_ctx_desc_alloc(g);
	if (gr->gr_ctx_desc == NULL) {
		err = -ENOMEM;
		goto clean_up;
	}

#ifdef CONFIG_NVGPU_GRAPHICS
	nvgpu_gr_ctx_set_size(g->gr->gr_ctx_desc, NVGPU_GR_CTX_PREEMPT_CTXSW,
			nvgpu_gr_falcon_get_preempt_image_size(g->gr->falcon));
#endif
	gr->global_ctx_buffer = nvgpu_gr_global_ctx_desc_alloc(g);
	if (gr->global_ctx_buffer == NULL) {
		err = -ENOMEM;
		goto clean_up;
	}

	err = gr_alloc_global_ctx_buffers(g);
	if (err != 0) {
		goto clean_up;
	}

	err = gr_init_access_map(g, gr);
	if (err != 0) {
		goto clean_up;
	}

#ifdef CONFIG_NVGPU_GRAPHICS
	err = nvgpu_gr_zbc_init(g, &gr->zbc);
	if (err != 0) {
		goto clean_up;
	}
#endif /* CONFIG_NVGPU_GRAPHICS */

	return 0;

clean_up:
	return err;
}

static int gr_init_ecc_init(struct gk20a *g)
{
	int err = 0;

	if ((g->ops.gr.ecc.gpc_tpc_ecc_init != NULL) && !g->ecc.initialized) {
		err = g->ops.gr.ecc.gpc_tpc_ecc_init(g);
		if (err != 0) {
			nvgpu_err(g, "failed to init gr gpc/tpc ecc");
			return err;
		}
	}

	return err;
}

static int gr_init_setup_sw(struct gk20a *g)
{
	struct nvgpu_gr *gr = g->gr;
	int err = 0;

	nvgpu_log_fn(g, " ");

	if (gr->sw_ready) {
		nvgpu_log_fn(g, "skip init");
		return 0;
	}

	gr->g = g;

#if defined(CONFIG_NVGPU_RECOVERY) || defined(CONFIG_NVGPU_DEBUGGER)
	nvgpu_mutex_init(&gr->ctxsw_disable_mutex);
	gr->ctxsw_disable_count = 0;
#endif

	err = nvgpu_gr_obj_ctx_init(g, &gr->golden_image,
			nvgpu_gr_falcon_get_golden_image_size(g->gr->falcon));
	if (err != 0) {
		goto clean_up;
	}

#ifdef CONFIG_NVGPU_DEBUGGER
	err = nvgpu_gr_hwpm_map_init(g, &g->gr->hwpm_map,
			nvgpu_gr_falcon_get_pm_ctxsw_image_size(g->gr->falcon));
	if (err != 0) {
		nvgpu_err(g, "hwpm_map init failed");
		goto clean_up;
	}
#endif

#ifdef CONFIG_NVGPU_GRAPHICS
	err = nvgpu_gr_config_init_map_tiles(g, gr->config);
	if (err != 0) {
		goto clean_up;
	}

	err = nvgpu_gr_zcull_init(g, &gr->zcull,
			nvgpu_gr_falcon_get_zcull_image_size(g->gr->falcon),
			g->gr->config);
	if (err != 0) {
		goto clean_up;
	}
#endif /* CONFIG_NVGPU_GRAPHICS */

	err = gr_init_ctx_and_map_zbc(g);
	if (err != 0) {
		goto clean_up;
	}

	err = gr_init_ecc_init(g);
	if (err != 0) {
		goto clean_up;
	}

	gr->remove_support = gr_remove_support;
	gr->sw_ready = true;

	nvgpu_log_fn(g, "done");
	return 0;

clean_up:
	nvgpu_err(g, "fail");
	gr_remove_support(g);
	return err;
}

static int gr_init_prepare_hw_impl(struct gk20a *g)
{
	struct netlist_av_list *sw_non_ctx_load =
		nvgpu_netlist_get_sw_non_ctx_load_av_list(g);
	u32 i;
	int err = 0;

	nvgpu_log_fn(g, " ");

	/** Enable interrupts */
	g->ops.gr.intr.enable_interrupts(g, true);

	/* enable fifo access */
	g->ops.gr.init.fifo_access(g, true);

	/* load non_ctx init */
	nvgpu_log_info(g, "begin: netlist: sw_non_ctx_load: register writes");
	for (i = 0; i < sw_non_ctx_load->count; i++) {
		nvgpu_writel(g, sw_non_ctx_load->l[i].addr,
			sw_non_ctx_load->l[i].value);
	}

#if defined(CONFIG_NVGPU_NON_FUSA) && defined(CONFIG_NVGPU_NEXT)
	nvgpu_next_gr_init_reset_enable_hw_non_ctx_local(g);
	nvgpu_next_gr_init_reset_enable_hw_non_ctx_global(g);
#endif
	nvgpu_log_info(g, "end: netlist: sw_non_ctx_load: register writes");

	err = g->ops.gr.falcon.wait_mem_scrubbing(g);
	if (err != 0) {
		goto out;
	}

	err = g->ops.gr.init.wait_idle(g);
	if (err != 0) {
		goto out;
	}

out:
	if (err != 0) {
		nvgpu_err(g, "fail");
	} else {
		nvgpu_log_fn(g, "done");
	}

	return err;
}

static int gr_init_prepare_hw(struct gk20a *g)
{
	nvgpu_log_fn(g, " ");

	/** Enable interrupts at MC level */
	nvgpu_mc_intr_stall_unit_config(g, MC_INTR_UNIT_GR, MC_INTR_ENABLE);
	nvgpu_mc_intr_nonstall_unit_config(g, MC_INTR_UNIT_GR, MC_INTR_ENABLE);

	return nvgpu_gr_exec_with_ret_for_each_instance(g,
			gr_init_prepare_hw_impl(g));
}

static int gr_reset_engine(struct gk20a *g)
{
#if defined(CONFIG_NVGPU_NON_FUSA) && defined(CONFIG_NVGPU_NEXT)
	int err;

	if (g->ops.gr.init.reset_gpcs != NULL) {
		const struct nvgpu_device *dev =
			nvgpu_device_get(g, NVGPU_DEVTYPE_GRAPHICS,
				 nvgpu_gr_get_syspipe_id(g, g->mig.cur_gr_instance));

		g->ops.mc.reset(g, g->ops.mc.reset_mask(g, NVGPU_UNIT_PERFMON));

		err = g->ops.mc.reset_engine_enable(g, dev->reset_id, false);
		if (err != 0) {
			nvgpu_err(g, "GR reset disable failed");
			return err;
		}

		err = g->ops.gr.init.reset_gpcs(g);
		if (err != 0) {
			nvgpu_err(g, "GR reset GPCs failed");
			g->ops.mc.reset_engine_enable(g, dev->reset_id, true);
			return err;
		}

		err = g->ops.mc.reset_engine_enable(g, dev->reset_id, true);
		if (err != 0) {
			nvgpu_err(g, "GR reset enable failed");
			return err;
		}
	} else {
#endif
		/* reset gr engine */
		g->ops.mc.reset(g, g->ops.mc.reset_mask(g, NVGPU_UNIT_GRAPH) |
				g->ops.mc.reset_mask(g, NVGPU_UNIT_BLG) |
				g->ops.mc.reset_mask(g, NVGPU_UNIT_PERFMON));

#if defined(CONFIG_NVGPU_NON_FUSA) && defined(CONFIG_NVGPU_NEXT)
	}
#endif
	return 0;
}

static int gr_reset_hw_and_load_prod(struct gk20a *g)
{
	int err;

	err = nvgpu_gr_exec_with_ret_for_each_instance(g, gr_reset_engine(g));
	if (err != 0) {
		return err;
	}

	nvgpu_gr_exec_for_all_instances(g, nvgpu_cg_init_gr_load_gating_prod(g));

	/* Disable elcg until it gets enabled later in the init*/
	nvgpu_cg_elcg_disable_no_wait(g);

	return 0;
}

int nvgpu_gr_enable_hw(struct gk20a *g)
{
	int err;

	nvgpu_log_fn(g, " ");

	err = gr_reset_hw_and_load_prod(g);
	if (err != 0) {
		return err;
	}

	err = gr_init_prepare_hw(g);
	if (err != 0) {
		return err;
	}

	nvgpu_log_fn(g, "done");

	return 0;
}

#ifdef CONFIG_NVGPU_ENGINE_RESET
int nvgpu_gr_reset(struct gk20a *g)
{
	int err;
	struct nvgpu_mutex *fecs_mutex =
		nvgpu_gr_falcon_get_fecs_mutex(g->gr->falcon);

	g->gr->initialized = false;

	nvgpu_mutex_acquire(fecs_mutex);

	err = nvgpu_gr_enable_hw(g);
	if (err != 0) {
		nvgpu_mutex_release(fecs_mutex);
		return err;
	}

	err = gr_init_setup_hw(g);
	if (err != 0) {
		nvgpu_mutex_release(fecs_mutex);
		return err;
	}

	err = nvgpu_gr_falcon_init_ctxsw(g, g->gr->falcon);
	if (err != 0) {
		nvgpu_mutex_release(fecs_mutex);
		return err;
	}

	nvgpu_mutex_release(fecs_mutex);

	/* this appears query for sw states but fecs actually init
	   ramchain, etc so this is hw init */
	err = nvgpu_gr_init_ctx_state(g);
	if (err != 0) {
		return err;
	}

#ifdef CONFIG_NVGPU_POWER_PG
	if (g->can_elpg) {
		err = nvgpu_gr_falcon_bind_fecs_elpg(g);
		if (err != 0) {
			return err;
		}
	}
#endif

	nvgpu_cg_init_gr_load_gating_prod(g);

	nvgpu_cg_elcg_enable_no_wait(g);

	/* GR is inialized, signal possible waiters */
	g->gr->initialized = true;
	nvgpu_cond_signal(&g->gr->init_wq);
	return err;
}
#endif

int nvgpu_gr_init_support(struct gk20a *g)
{
	int err = 0;

	nvgpu_log_fn(g, " ");

	g->gr->initialized = false;

	/* This is prerequisite for calling sm_id_config_early hal. */
	if (!g->gr->sw_ready) {
		err = gr_init_config(g, g->gr);
		if (err != 0) {
			return err;
		}
	}

#if defined(CONFIG_NVGPU_NEXT)
	/*
	 * Move sm id programming before loading ctxsw and gpccs firmwares. This
	 * is the actual sequence expected by ctxsw ucode.
	 */
	if (g->ops.gr.init.sm_id_config_early != NULL) {
		err = g->ops.gr.init.sm_id_config_early(g, g->gr->config);
		if (err != 0) {
			return err;
		}
	}
#endif

	err = nvgpu_gr_falcon_init_ctxsw(g, g->gr->falcon);
	if (err != 0) {
		gr_intr_report_ctxsw_error(g, GPU_FECS_CTXSW_INIT_ERROR, 0, 0);
		return err;
	}

	/* this appears query for sw states but fecs actually init
	   ramchain, etc so this is hw init */
	err = nvgpu_gr_init_ctx_state(g);
	if (err != 0) {
		return err;
	}

#ifdef CONFIG_NVGPU_POWER_PG
	if (g->can_elpg) {
		err = nvgpu_gr_falcon_bind_fecs_elpg(g);
		if (err != 0) {
			return err;
		}
	}
#endif

	err = gr_init_setup_sw(g);
	if (err != 0) {
		return err;
	}

	err = gr_init_setup_hw(g);
	if (err != 0) {
		return err;
	}

	nvgpu_cg_elcg_enable_no_wait(g);

	/* GR is inialized, signal possible waiters */
	g->gr->initialized = true;
	nvgpu_cond_signal(&g->gr->init_wq);

	return 0;
}

int nvgpu_gr_alloc(struct gk20a *g)
{
	struct nvgpu_gr *gr = NULL;
	int err;
	u32 i;

	/* if gr exists return */
	if (g->gr != NULL) {
		return 0;
	}

	g->num_gr_instances = NVGPU_GR_NUM_INSTANCES;
	if (g->num_gr_instances == 0U) {
		g->num_gr_instances = nvgpu_grmgr_get_num_gr_instances(g);
	}
	if (g->num_gr_instances == 0U) {
		nvgpu_err(g, "No GR engine enumerated");
		return -EINVAL;
	}

	/* Allocate memory for gr struct */
	g->gr = nvgpu_kzalloc(g, sizeof(*gr) * g->num_gr_instances);
	if (g->gr == NULL) {
		return -ENOMEM;
	}

	g->mig.cur_gr_instance = 0U; /* default */

	for (i = 0U; i < g->num_gr_instances; i++) {
		gr = &g->gr[i];

		gr->syspipe_id = nvgpu_grmgr_get_gr_syspipe_id(g, i);
		if (gr->syspipe_id == U32_MAX) {
			nvgpu_err(g, "failed to get syspipe id");
			err = -EINVAL;
			goto fail;
		}

		gr->falcon = nvgpu_gr_falcon_init_support(g);
		if (gr->falcon == NULL) {
			nvgpu_err(g, "failed to init gr falcon");
			err = -ENOMEM;
			goto fail;
		}

		gr->intr = nvgpu_gr_intr_init_support(g);
		if (gr->intr == NULL) {
			nvgpu_err(g, "failed to init gr intr support");
			err = -ENOMEM;
			goto fail;
		}

		nvgpu_cond_init(&gr->init_wq);
#ifdef CONFIG_NVGPU_NON_FUSA
		nvgpu_gr_override_ecc_val(gr, g->fecs_feature_override_ecc_val);
#endif
	}

	/*
	 * Initialize FECS ECC counters here before acr_construct_execute as the
	 * FECS ECC errors during FECS load need to be handled and reported
	 * using the ECC counters.
	 */
	if (g->ops.gr.ecc.fecs_ecc_init != NULL) {
		err = g->ops.gr.ecc.fecs_ecc_init(g);
		if (err != 0) {
			nvgpu_err(g, "failed to init gr fecs ecc");
			goto fail;
		}
	}

	return 0;

fail:
	nvgpu_gr_free(g);
	return err;
}

void nvgpu_gr_free(struct gk20a *g)
{
	struct nvgpu_gr *gr = NULL;
	u32 i;

	if (g->gr == NULL) {
		return;
	}

	for (i = 0U; i < g->num_gr_instances; i++) {
		gr = &g->gr[i];

		nvgpu_gr_falcon_remove_support(g, gr->falcon);
		gr->falcon = NULL;

		nvgpu_gr_intr_remove_support(g, gr->intr);
		gr->intr = NULL;
	}

	nvgpu_kfree(g, g->gr);
	g->gr = NULL;
}

u32 nvgpu_gr_get_syspipe_id(struct gk20a *g, u32 gr_instance_id)
{
	return g->gr[gr_instance_id].syspipe_id;
}

#if defined(CONFIG_NVGPU_RECOVERY) || defined(CONFIG_NVGPU_DEBUGGER)
/**
 * Stop processing (stall) context switches at FECS:-
 * If fecs is sent stop_ctxsw method, elpg entry/exit cannot happen
 * and may timeout. It could manifest as different error signatures
 * depending on when stop_ctxsw fecs method gets sent with respect
 * to pmu elpg sequence. It could come as pmu halt or abort or
 * maybe ext error too.
 */
int nvgpu_gr_disable_ctxsw(struct gk20a *g)
{
	struct nvgpu_gr *gr = g->gr;
	int err = 0;

	nvgpu_log(g, gpu_dbg_fn | gpu_dbg_gpu_dbg, " ");

	nvgpu_mutex_acquire(&gr->ctxsw_disable_mutex);

	/* check for gr->ctxsw_disable_count overflow */
	if (INT_MAX == gr->ctxsw_disable_count) {
		nvgpu_err(g, "ctxsw_disable_count overflow");
		err = -ERANGE;
		goto out;
	}

	gr->ctxsw_disable_count++;
	if (gr->ctxsw_disable_count == 1) {
#ifdef CONFIG_NVGPU_POWER_PG
		err = nvgpu_pg_elpg_disable(g);
		if (err != 0) {
			nvgpu_err(g,
				"failed to disable elpg for stop_ctxsw");
			/* stop ctxsw command is not sent */
			gr->ctxsw_disable_count--;
		} else
#endif
		{
			err = g->ops.gr.falcon.ctrl_ctxsw(g,
				NVGPU_GR_FALCON_METHOD_CTXSW_STOP, 0U, NULL);
			if (err != 0) {
				nvgpu_err(g, "failed to stop fecs ctxsw");
				/* stop ctxsw failed */
				gr->ctxsw_disable_count--;
			}
		}
	} else {
		nvgpu_log_info(g, "ctxsw disabled, ctxsw_disable_count: %d",
			gr->ctxsw_disable_count);
	}
out:
	nvgpu_mutex_release(&gr->ctxsw_disable_mutex);

	return err;
}

/* Start processing (continue) context switches at FECS */
int nvgpu_gr_enable_ctxsw(struct gk20a *g)
{
	struct nvgpu_gr *gr = g->gr;
	int err = 0;

	nvgpu_log(g, gpu_dbg_fn | gpu_dbg_gpu_dbg, " ");

	nvgpu_mutex_acquire(&gr->ctxsw_disable_mutex);
	if (gr->ctxsw_disable_count == 0) {
		goto ctxsw_already_enabled;
	}
	gr->ctxsw_disable_count--;
	nvgpu_assert(gr->ctxsw_disable_count >= 0);
	if (gr->ctxsw_disable_count == 0) {
		err = g->ops.gr.falcon.ctrl_ctxsw(g,
				NVGPU_GR_FALCON_METHOD_CTXSW_START, 0U, NULL);
		if (err != 0) {
			nvgpu_err(g, "failed to start fecs ctxsw");
		}
#ifdef CONFIG_NVGPU_POWER_PG
		else {
			if (nvgpu_pg_elpg_enable(g) != 0) {
				nvgpu_err(g,
					"failed to enable elpg for start_ctxsw");
			}
		}
#endif
	} else {
		nvgpu_log_info(g, "ctxsw_disable_count: %d is not 0 yet",
			gr->ctxsw_disable_count);
	}
ctxsw_already_enabled:
	nvgpu_mutex_release(&gr->ctxsw_disable_mutex);

	return err;
}
#endif

void nvgpu_gr_remove_support(struct gk20a *g)
{
	if (g->gr != NULL && g->gr->remove_support != NULL) {
		g->gr->remove_support(g);
	}
}

void nvgpu_gr_sw_ready(struct gk20a *g, bool enable)
{
	if (g->gr != NULL) {
		g->gr->sw_ready = enable;
	}
}

#ifdef CONFIG_NVGPU_HAL_NON_FUSA
/* Wait until GR is initialized */
void nvgpu_gr_wait_initialized(struct gk20a *g)
{
	NVGPU_COND_WAIT(&g->gr->init_wq, g->gr->initialized, 0U);
}
#endif

bool nvgpu_gr_is_tpc_addr(struct gk20a *g, u32 addr)
{
	u32 tpc_in_gpc_base =
		nvgpu_get_litter_value(g, GPU_LIT_TPC_IN_GPC_BASE);
	u32 tpc_in_gpc_stride =
		nvgpu_get_litter_value(g, GPU_LIT_TPC_IN_GPC_STRIDE);
	u32 num_tpc_per_gpc =
		nvgpu_get_litter_value(g, GPU_LIT_NUM_TPC_PER_GPC);
	u32 tpc_in_gpc_shared_base =
		nvgpu_get_litter_value(g, GPU_LIT_TPC_IN_GPC_SHARED_BASE);
	bool is_tpc_addr_shared = ((addr >= tpc_in_gpc_shared_base) &&
			(addr < (tpc_in_gpc_shared_base + tpc_in_gpc_stride)));

	return (((addr >= tpc_in_gpc_base) &&
		(addr < (tpc_in_gpc_base +
			(num_tpc_per_gpc * tpc_in_gpc_stride)))) ||
		is_tpc_addr_shared);
}

u32 nvgpu_gr_get_tpc_num(struct gk20a *g, u32 addr)
{
	u32 i, start;
	u32 num_tpcs =
		nvgpu_get_litter_value(g, GPU_LIT_NUM_TPC_PER_GPC);
	u32 tpc_in_gpc_base =
		nvgpu_get_litter_value(g, GPU_LIT_TPC_IN_GPC_BASE);
	u32 tpc_in_gpc_stride =
		nvgpu_get_litter_value(g, GPU_LIT_TPC_IN_GPC_STRIDE);

	for (i = 0; i < num_tpcs; i++) {
		start = tpc_in_gpc_base + (i * tpc_in_gpc_stride);
		if ((addr >= start) &&
		    (addr < (start + tpc_in_gpc_stride))) {
			return i;
		}
	}
	return 0;
}
