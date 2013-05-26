/*
 * drivers/video/tegra/host/gr2d/gr2d_t30.h
 *
 * Tegra Graphics Host 2D Tegra3 specific parts
 *
 * Copyright (c) 2012, NVIDIA Corporation.
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
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __NVHOST_2D_T30_H
#define __NVHOST_2D_T30_H

struct nvhost_device;

void nvhost_gr2d_t30_finalize_poweron(struct nvhost_device *dev);

#endif
