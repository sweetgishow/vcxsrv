/*
 * Copyright © 2022 Imagination Technologies Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/* This file is based on rgx_fwif_shared.h and rgx_fwif_client.h. It contains
 * information about the firmware that is needed by the driver.
 */

#ifndef PVR_ROGUE_FW_H
#define PVR_ROGUE_FW_H

/**
 * Maximum PB free list size supported by RGX and Services.
 *
 * Maximum PB free list size must ensure that no PM address space can be fully
 * used, because if the full address space was used it would wrap and corrupt
 * itself. Since there are two freelists (local is always minimum sized) this
 * can be described as following three conditions being met:
 *
 *  Minimum PB + Maximum PB < ALIST PM address space size (16GB)
 *  Minimum PB + Maximum PB < TE PM address space size (16GB) / NUM_TE_PIPES
 *  Minimum PB + Maximum PB < VCE PM address space size (16GB) / NUM_VCE_PIPES
 *
 * Since the max of NUM_TE_PIPES and NUM_VCE_PIPES is 4, we have a hard limit
 * of 4GB minus the Minimum PB. For convenience we take the smaller power-of-2
 * value of 2GB. This is far more than any normal application would request
 * or use.
 */
#define ROGUE_FREE_LIST_MAX_SIZE (2ULL * 1024ULL * 1024ULL * 1024ULL)

/* FIXME: This will change based on the firmware configuration, which will vary
 * depending on the BVNC and firmware version. The powervr KM driver allows this
 * information to be queried, but the pvrsrvkm KM driver doesn't. This
 * information should really be queried from the winsys.
 */
/* Indicates the number of Render Target Datas in a Render Target Dataset. */
#define ROGUE_NUM_RTDATAS 2U

#endif /* PVR_ROGUE_FW_H */
