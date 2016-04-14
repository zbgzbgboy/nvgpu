/*
 * GV11B Graphics Copy Engine  (gr host)
 *
 * Copyright (c) 2016, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "gk20a/gk20a.h" /* FERMI and MAXWELL classes defined here */
#include "hw_ce2_gv11b.h"
#include "gp10b/ce2_gp10b.h"
#include "ce2_gv11b.h"

void gv11b_init_ce2(struct gpu_ops *gops)
{
	gp10b_init_ce2(gops);
}
