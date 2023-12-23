/**************************************************************************
 *
 * Copyright 2007 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

/**
 * Tiling engine.
 *
 * Builds per-tile display lists and executes them on calls to
 * lp_setup_flush().
 */

#include <limits.h>

#include "pipe/p_defines.h"
#include "util/u_framebuffer.h"
#include "util/u_inlines.h"
#include "util/u_memory.h"
#include "util/u_pack_color.h"
#include "util/u_cpu_detect.h"
#include "util/u_viewport.h"
#include "draw/draw_pipe.h"
#include "util/os_time.h"
#include "lp_context.h"
#include "lp_memory.h"
#include "lp_scene.h"
#include "lp_texture.h"
#include "lp_debug.h"
#include "lp_fence.h"
#include "lp_query.h"
#include "lp_rast.h"
#include "lp_setup_context.h"
#include "lp_screen.h"
#include "lp_state.h"
#include "lp_jit.h"
#include "frontend/sw_winsys.h"

#include "draw/draw_context.h"
#include "draw/draw_vbuf.h"


static boolean set_scene_state(struct lp_setup_context *, enum setup_state,
                               const char *reason);
static boolean try_update_scene_state(struct lp_setup_context *setup);


static unsigned
lp_setup_wait_empty_scene(struct lp_setup_context *setup)
{
   /* just use the first scene if we run out */
   if (setup->scenes[0]->fence) {
      debug_printf("%s: wait for scene %d\n",
                   __FUNCTION__, setup->scenes[0]->fence->id);
      lp_fence_wait(setup->scenes[0]->fence);
      lp_scene_end_rasterization(setup->scenes[0]);
   }
   return 0;
}


static void
lp_setup_get_empty_scene(struct lp_setup_context *setup)
{
   assert(setup->scene == NULL);
   unsigned i;

   /* try and find a scene that isn't being used */
   for (i = 0; i < setup->num_active_scenes; i++) {
      if (setup->scenes[i]->fence) {
         if (lp_fence_signalled(setup->scenes[i]->fence)) {
            lp_scene_end_rasterization(setup->scenes[i]);
            break;
         }
      } else {
         break;
      }
   }

   if (setup->num_active_scenes + 1 > MAX_SCENES) {
      i = lp_setup_wait_empty_scene(setup);
   } else if (i == setup->num_active_scenes) {
      /* allocate a new scene */
      struct lp_scene *scene = lp_scene_create(setup);
      if (!scene) {
         /* block and reuse scenes */
         i = lp_setup_wait_empty_scene(setup);
      } else {
         LP_DBG(DEBUG_SETUP, "allocated scene: %d\n", setup->num_active_scenes);
         setup->scenes[setup->num_active_scenes] = scene;
         i = setup->num_active_scenes;
         setup->num_active_scenes++;
      }
   }

   setup->scene = setup->scenes[i];
   setup->scene->permit_linear_rasterizer = setup->permit_linear_rasterizer;
   lp_scene_begin_binning(setup->scene, &setup->fb);
}


static void
first_triangle(struct lp_setup_context *setup,
               const float (*v0)[4],
               const float (*v1)[4],
               const float (*v2)[4])
{
   assert(setup->state == SETUP_ACTIVE);
   lp_setup_choose_triangle(setup);
   setup->triangle(setup, v0, v1, v2);
}


static boolean
first_rectangle(struct lp_setup_context *setup,
                const float (*v0)[4],
                const float (*v1)[4],
                const float (*v2)[4],
                const float (*v3)[4],
                const float (*v4)[4],
                const float (*v5)[4])
{
   assert(setup->state == SETUP_ACTIVE);
   lp_setup_choose_rect(setup);
   return setup->rect(setup, v0, v1, v2, v3, v4, v5);
}


static void
first_line(struct lp_setup_context *setup,
           const float (*v0)[4],
           const float (*v1)[4])
{
   assert(setup->state == SETUP_ACTIVE);
   lp_setup_choose_line(setup);
   setup->line(setup, v0, v1);
}


static void
first_point(struct lp_setup_context *setup,
            const float (*v0)[4])
{
   assert(setup->state == SETUP_ACTIVE);
   lp_setup_choose_point(setup);
   setup->point(setup, v0);
}


void
lp_setup_reset(struct lp_setup_context *setup)
{
   LP_DBG(DEBUG_SETUP, "%s\n", __FUNCTION__);

   /* Reset derived state */
   for (unsigned i = 0; i < ARRAY_SIZE(setup->constants); ++i) {
      setup->constants[i].stored_size = 0;
      setup->constants[i].stored_data = NULL;
   }

   setup->fs.stored = NULL;
   setup->dirty = ~0;

   /* no current bin */
   setup->scene = NULL;

   /* Reset some state:
    */
   memset(&setup->clear, 0, sizeof(setup->clear));

   /* Have an explicit "start-binning" call and get rid of this
    * pointer twiddling?
    */
   setup->line = first_line;
   setup->point = first_point;
   setup->triangle = first_triangle;
   setup->rect = first_rectangle;
}


/** Rasterize all scene's bins */
static void
lp_setup_rasterize_scene(struct lp_setup_context *setup)
{
   struct lp_scene *scene = setup->scene;
   struct llvmpipe_screen *screen = llvmpipe_screen(scene->pipe->screen);

   scene->num_active_queries = setup->active_binned_queries;
   memcpy(scene->active_queries, setup->active_queries,
          scene->num_active_queries * sizeof(scene->active_queries[0]));

   lp_scene_end_binning(scene);

   mtx_lock(&screen->rast_mutex);
   lp_rast_queue_scene(screen->rast, scene);
   mtx_unlock(&screen->rast_mutex);

   lp_setup_reset(setup);

   LP_DBG(DEBUG_SETUP, "%s done \n", __FUNCTION__);
}


static boolean
begin_binning(struct lp_setup_context *setup)
{
   struct lp_scene *scene = setup->scene;

   assert(scene);
   assert(scene->fence == NULL);

   /* Always create a fence:
    */
   scene->fence = lp_fence_create(MAX2(1, setup->num_threads));
   if (!scene->fence)
      return FALSE;

   if (!try_update_scene_state(setup)) {
      return FALSE;
   }

   boolean need_zsload = FALSE;
   if (setup->fb.zsbuf &&
       ((setup->clear.flags & PIPE_CLEAR_DEPTHSTENCIL) != PIPE_CLEAR_DEPTHSTENCIL) &&
        util_format_is_depth_and_stencil(setup->fb.zsbuf->format)) {
      need_zsload = TRUE;
   }

   LP_DBG(DEBUG_SETUP, "%s color clear bufs: %x depth: %s\n", __FUNCTION__,
          setup->clear.flags >> 2,
          need_zsload ? "clear": "load");

   if (setup->clear.flags & PIPE_CLEAR_COLOR) {
      for (unsigned cbuf = 0; cbuf < setup->fb.nr_cbufs; cbuf++) {
         assert(PIPE_CLEAR_COLOR0 == 1 << 2);
         if (setup->clear.flags & (1 << (2 + cbuf))) {
            union lp_rast_cmd_arg clearrb_arg;
            struct lp_rast_clear_rb *cc_scene =
               (struct lp_rast_clear_rb *)
                  lp_scene_alloc(scene, sizeof(struct lp_rast_clear_rb));

            if (!cc_scene) {
               return FALSE;
            }

            cc_scene->cbuf = cbuf;
            cc_scene->color_val = setup->clear.color_val[cbuf];
            clearrb_arg.clear_rb = cc_scene;

            if (!lp_scene_bin_everywhere(scene,
                                         LP_RAST_OP_CLEAR_COLOR,
                                         clearrb_arg)) {
               return FALSE;
            }
         }
      }
   }

   if (setup->fb.zsbuf) {
      if (setup->clear.flags & PIPE_CLEAR_DEPTHSTENCIL) {
         if (!lp_scene_bin_everywhere(scene,
                                      LP_RAST_OP_CLEAR_ZSTENCIL,
                                      lp_rast_arg_clearzs(
                                         setup->clear.zsvalue,
                                         setup->clear.zsmask))) {
            return FALSE;
         }
      }
   }

   setup->clear.flags = 0;
   setup->clear.zsmask = 0;
   setup->clear.zsvalue = 0;

   scene->had_queries = !!setup->active_binned_queries;

   LP_DBG(DEBUG_SETUP, "%s done\n", __FUNCTION__);
   return TRUE;
}


/* This basically bins and then flushes any outstanding full-screen
 * clears.
 *
 * TODO: fast path for fullscreen clears and no triangles.
 */
static boolean
execute_clears(struct lp_setup_context *setup)
{
   LP_DBG(DEBUG_SETUP, "%s\n", __FUNCTION__);

   return begin_binning(setup);
}


static const char *states[] = {
   "FLUSHED",
   "CLEARED",
   "ACTIVE "
};


static boolean
set_scene_state(struct lp_setup_context *setup,
                enum setup_state new_state,
                const char *reason)
{
   const unsigned old_state = setup->state;

   if (old_state == new_state)
      return TRUE;

   if (LP_DEBUG & DEBUG_SCENE) {
      debug_printf("%s old %s new %s%s%s\n",
                   __FUNCTION__,
                   states[old_state],
                   states[new_state],
                   (new_state == SETUP_FLUSHED) ? ": " : "",
                   (new_state == SETUP_FLUSHED) ? reason : "");

      if (new_state == SETUP_FLUSHED && setup->scene)
         lp_debug_draw_bins_by_cmd_length(setup->scene);
   }

   /* wait for a free/empty scene
    */
   if (old_state == SETUP_FLUSHED)
      lp_setup_get_empty_scene(setup);

   switch (new_state) {
   case SETUP_CLEARED:
      break;

   case SETUP_ACTIVE:
      if (!begin_binning(setup))
         goto fail;
      break;

   case SETUP_FLUSHED:
      if (old_state == SETUP_CLEARED)
         if (!execute_clears(setup))
            goto fail;

      lp_setup_rasterize_scene(setup);
      assert(setup->scene == NULL);
      break;

   default:
      assert(0 && "invalid setup state mode");
      goto fail;
   }

   setup->state = new_state;
   return TRUE;

fail:
   if (setup->scene) {
      lp_scene_end_rasterization(setup->scene);
      setup->scene = NULL;
   }

   setup->state = SETUP_FLUSHED;
   lp_setup_reset(setup);
   return FALSE;
}


void
lp_setup_flush(struct lp_setup_context *setup,
               const char *reason)
{
   set_scene_state(setup, SETUP_FLUSHED, reason);
}


void
lp_setup_bind_framebuffer(struct lp_setup_context *setup,
                          const struct pipe_framebuffer_state *fb)
{
   LP_DBG(DEBUG_SETUP, "%s\n", __FUNCTION__);

   /* Flush any old scene.
    */
   set_scene_state(setup, SETUP_FLUSHED, __FUNCTION__);

   /*
    * Ensure the old scene is not reused.
    */
   assert(!setup->scene);

   /* Set new state.  This will be picked up later when we next need a
    * scene.
    */
   util_copy_framebuffer_state(&setup->fb, fb);
   setup->framebuffer.x0 = 0;
   setup->framebuffer.y0 = 0;
   setup->framebuffer.x1 = fb->width-1;
   setup->framebuffer.y1 = fb->height-1;
   setup->dirty |= LP_SETUP_NEW_SCISSOR;
}


/*
 * Try to clear one color buffer of the attached fb, either by binning a clear
 * command or queuing up the clear for later (when binning is started).
 */
static boolean
lp_setup_try_clear_color_buffer(struct lp_setup_context *setup,
                                const union pipe_color_union *color,
                                unsigned cbuf)
{
   union lp_rast_cmd_arg clearrb_arg;
   union util_color uc;
   const enum pipe_format format = setup->fb.cbufs[cbuf]->format;

   LP_DBG(DEBUG_SETUP, "%s state %d\n", __FUNCTION__, setup->state);

   util_pack_color_union(format, &uc, color);

   if (setup->state == SETUP_ACTIVE) {
      struct lp_scene *scene = setup->scene;

      /* Add the clear to existing scene.  In the unusual case where
       * both color and depth-stencil are being cleared when there's
       * already been some rendering, we could discard the currently
       * binned scene and start again, but I don't see that as being
       * a common usage.
       */
      struct lp_rast_clear_rb *cc_scene =
         (struct lp_rast_clear_rb *)
            lp_scene_alloc_aligned(scene, sizeof(struct lp_rast_clear_rb), 8);

      if (!cc_scene) {
         return FALSE;
      }

      cc_scene->cbuf = cbuf;
      cc_scene->color_val = uc;
      clearrb_arg.clear_rb = cc_scene;

      if (!lp_scene_bin_everywhere(scene,
                                   LP_RAST_OP_CLEAR_COLOR,
                                   clearrb_arg)) {
         return FALSE;
      }
   }
   else {
      /* Put ourselves into the 'pre-clear' state, specifically to try
       * and accumulate multiple clears to color and depth_stencil
       * buffers which the app or gallium frontend might issue
       * separately.
       */
      set_scene_state(setup, SETUP_CLEARED, __FUNCTION__);

      assert(PIPE_CLEAR_COLOR0 == (1 << 2));
      setup->clear.flags |= 1 << (cbuf + 2);
      setup->clear.color_val[cbuf] = uc;
   }

   return TRUE;
}


static boolean
lp_setup_try_clear_zs(struct lp_setup_context *setup,
                      double depth,
                      unsigned stencil,
                      unsigned flags)
{
   LP_DBG(DEBUG_SETUP, "%s state %d\n", __FUNCTION__, setup->state);

   enum pipe_format format = setup->fb.zsbuf->format;

   const uint32_t zmask32 = (flags & PIPE_CLEAR_DEPTH) ? ~0 : 0;
   const uint8_t smask8 = (flags & PIPE_CLEAR_STENCIL) ? ~0 : 0;

   uint64_t zsvalue = util_pack64_z_stencil(format, depth, stencil);
   uint64_t zsmask = util_pack64_mask_z_stencil(format, zmask32, smask8);

   zsvalue &= zsmask;

   if (format == PIPE_FORMAT_Z24X8_UNORM ||
       format == PIPE_FORMAT_X8Z24_UNORM) {
      /*
       * Make full mask if there's "X" bits so we can do full
       * clear (without rmw).
       */
      uint32_t zsmask_full = util_pack_mask_z_stencil(format, ~0, ~0);
      zsmask |= ~zsmask_full;
   }

   if (setup->state == SETUP_ACTIVE) {
      struct lp_scene *scene = setup->scene;

      /* Add the clear to existing scene.  In the unusual case where
       * both color and depth-stencil are being cleared when there's
       * already been some rendering, we could discard the currently
       * binned scene and start again, but I don't see that as being
       * a common usage.
       */
      if (!lp_scene_bin_everywhere(scene,
                                   LP_RAST_OP_CLEAR_ZSTENCIL,
                                   lp_rast_arg_clearzs(zsvalue, zsmask)))
         return FALSE;
   }
   else {
      /* Put ourselves into the 'pre-clear' state, specifically to try
       * and accumulate multiple clears to color and depth_stencil
       * buffers which the app or gallium frontend might issue
       * separately.
       */
      set_scene_state(setup, SETUP_CLEARED, __FUNCTION__);

      setup->clear.flags |= flags;

      setup->clear.zsmask |= zsmask;
      setup->clear.zsvalue =
         (setup->clear.zsvalue & ~zsmask) | (zsvalue & zsmask);
   }

   return TRUE;
}


void
lp_setup_clear(struct lp_setup_context *setup,
               const union pipe_color_union *color,
               double depth,
               unsigned stencil,
               unsigned flags)
{
   /*
    * Note any of these (max 9) clears could fail (but at most there should
    * be just one failure!). This avoids doing the previous succeeded
    * clears again (we still clear tiles twice if a clear command succeeded
    * partially for one buffer).
    */
   if (flags & PIPE_CLEAR_DEPTHSTENCIL) {
      unsigned flagszs = flags & PIPE_CLEAR_DEPTHSTENCIL;
      if (!lp_setup_try_clear_zs(setup, depth, stencil, flagszs)) {
         set_scene_state( setup, SETUP_FLUSHED, __FUNCTION__ );

         if (!lp_setup_try_clear_zs(setup, depth, stencil, flagszs))
            assert(0);
      }
   }

   if (flags & PIPE_CLEAR_COLOR) {
      assert(PIPE_CLEAR_COLOR0 == (1 << 2));
      for (unsigned i = 0; i < setup->fb.nr_cbufs; i++) {
         if ((flags & (1 << (2 + i))) && setup->fb.cbufs[i]) {
            if (!lp_setup_try_clear_color_buffer(setup, color, i)) {
               set_scene_state( setup, SETUP_FLUSHED, __FUNCTION__ );

               if (!lp_setup_try_clear_color_buffer(setup, color, i))
                  assert(0);
            }
         }
      }
   }
}


void
lp_setup_bind_rasterizer( struct lp_setup_context *setup,
                          const struct pipe_rasterizer_state *rast)
{
   LP_DBG(DEBUG_SETUP, "%s\n", __FUNCTION__);

   setup->ccw_is_frontface = rast->front_ccw;
   setup->cullmode = rast->cull_face;
   setup->triangle = first_triangle;
   setup->rect = first_rectangle;
   setup->multisample = rast->multisample;
   setup->pixel_offset = rast->half_pixel_center ? 0.5f : 0.0f;
   setup->bottom_edge_rule = rast->bottom_edge_rule;

   if (setup->scissor_test != rast->scissor) {
      setup->dirty |= LP_SETUP_NEW_SCISSOR;
      setup->scissor_test = rast->scissor;
   }

   setup->flatshade_first = rast->flatshade_first;
   setup->line_width = rast->line_width;
   setup->rectangular_lines = rast->line_rectangular;

   setup->point_size = rast->point_size;
   setup->sprite_coord_enable = rast->sprite_coord_enable;
   setup->sprite_coord_origin = rast->sprite_coord_mode;
   setup->point_tri_clip = rast->point_size_per_vertex;
   setup->point_size_per_vertex = rast->point_size_per_vertex;
   setup->legacy_points = !rast->point_quad_rasterization && !setup->multisample;
}


void
lp_setup_set_setup_variant(struct lp_setup_context *setup,
                           const struct lp_setup_variant *variant)
{
   LP_DBG(DEBUG_SETUP, "%s\n", __FUNCTION__);

   setup->setup.variant = variant;
}


void
lp_setup_set_fs_variant(struct lp_setup_context *setup,
                        struct lp_fragment_shader_variant *variant)
{
   LP_DBG(DEBUG_SETUP, "%s %p\n", __FUNCTION__, variant);

   setup->fs.current.variant = variant;
   setup->dirty |= LP_SETUP_NEW_FS;
}


void
lp_setup_set_fs_constants(struct lp_setup_context *setup,
                          unsigned num,
                          struct pipe_constant_buffer *buffers)
{
   LP_DBG(DEBUG_SETUP, "%s %p\n", __FUNCTION__, (void *) buffers);

   assert(num <= ARRAY_SIZE(setup->constants));

   unsigned i;
   for (i = 0; i < num; ++i) {
      util_copy_constant_buffer(&setup->constants[i].current,
                                &buffers[i], false);
   }
   for (; i < ARRAY_SIZE(setup->constants); i++) {
      util_copy_constant_buffer(&setup->constants[i].current, NULL, false);
   }
   setup->dirty |= LP_SETUP_NEW_CONSTANTS;
}


void
lp_setup_set_fs_ssbos(struct lp_setup_context *setup,
                      unsigned num,
                      struct pipe_shader_buffer *buffers,
                      uint32_t ssbo_write_mask)
{
   LP_DBG(DEBUG_SETUP, "%s %p\n", __FUNCTION__, (void *) buffers);

   assert(num <= ARRAY_SIZE(setup->ssbos));

   unsigned i;
   for (i = 0; i < num; ++i) {
      util_copy_shader_buffer(&setup->ssbos[i].current, &buffers[i]);
   }
   for (; i < ARRAY_SIZE(setup->ssbos); i++) {
      util_copy_shader_buffer(&setup->ssbos[i].current, NULL);
   }
   setup->ssbo_write_mask = ssbo_write_mask;
   setup->dirty |= LP_SETUP_NEW_SSBOS;
}


void
lp_setup_set_fs_images(struct lp_setup_context *setup,
                       unsigned num,
                       struct pipe_image_view *images)
{
   unsigned i;

   LP_DBG(DEBUG_SETUP, "%s %p\n", __FUNCTION__, (void *) images);

   assert(num <= ARRAY_SIZE(setup->images));

   for (i = 0; i < num; ++i) {
      const struct pipe_image_view *image = &images[i];
      util_copy_image_view(&setup->images[i].current, &images[i]);

      struct pipe_resource *res = image->resource;
      struct llvmpipe_resource *lp_res = llvmpipe_resource(res);
      struct lp_jit_image *jit_image = &setup->fs.current.jit_context.images[i];

      if (!lp_res)
         continue;

      if (!lp_res->dt) {
         /* regular texture - setup array of mipmap level offsets */
         if (llvmpipe_resource_is_texture(res)) {
            jit_image->base = lp_res->tex_data;
         } else {
            jit_image->base = lp_res->data;
         }

         jit_image->width = res->width0;
         jit_image->height = res->height0;
         jit_image->depth = res->depth0;
         jit_image->num_samples = res->nr_samples;

         if (llvmpipe_resource_is_texture(res)) {
            uint32_t mip_offset = lp_res->mip_offsets[image->u.tex.level];
            const uint32_t bw = util_format_get_blockwidth(image->resource->format);
            const uint32_t bh = util_format_get_blockheight(image->resource->format);

            jit_image->width = DIV_ROUND_UP(jit_image->width, bw);
            jit_image->height = DIV_ROUND_UP(jit_image->height, bh);
            jit_image->width = u_minify(jit_image->width, image->u.tex.level);
            jit_image->height = u_minify(jit_image->height, image->u.tex.level);

            if (res->target == PIPE_TEXTURE_1D_ARRAY ||
                res->target == PIPE_TEXTURE_2D_ARRAY ||
                res->target == PIPE_TEXTURE_3D ||
                res->target == PIPE_TEXTURE_CUBE ||
                res->target == PIPE_TEXTURE_CUBE_ARRAY) {
               /*
                * For array textures, we don't have first_layer, instead
                * adjust last_layer (stored as depth) plus the mip level offsets
                * (as we have mip-first layout can't just adjust base ptr).
                * XXX For mip levels, could do something similar.
                */
               jit_image->depth = image->u.tex.last_layer - image->u.tex.first_layer + 1;
               mip_offset += image->u.tex.first_layer * lp_res->img_stride[image->u.tex.level];
            } else
               jit_image->depth = u_minify(jit_image->depth, image->u.tex.level);

            jit_image->row_stride = lp_res->row_stride[image->u.tex.level];
            jit_image->img_stride = lp_res->img_stride[image->u.tex.level];
            jit_image->sample_stride = lp_res->sample_stride;
            jit_image->base = (uint8_t *)jit_image->base + mip_offset;
         }
         else {
            unsigned view_blocksize = util_format_get_blocksize(image->format);
            jit_image->width = image->u.buf.size / view_blocksize;
            jit_image->base = (uint8_t *)jit_image->base + image->u.buf.offset;
         }
      }
   }
   for (; i < ARRAY_SIZE(setup->images); i++) {
      util_copy_image_view(&setup->images[i].current, NULL);
   }
   setup->dirty |= LP_SETUP_NEW_FS;
}


void
lp_setup_set_alpha_ref_value(struct lp_setup_context *setup,
                             float alpha_ref_value)
{
   LP_DBG(DEBUG_SETUP, "%s %f\n", __FUNCTION__, alpha_ref_value);

   if (setup->fs.current.jit_context.alpha_ref_value != alpha_ref_value) {
      setup->fs.current.jit_context.alpha_ref_value = alpha_ref_value;
      setup->dirty |= LP_SETUP_NEW_FS;
   }
}


void
lp_setup_set_stencil_ref_values(struct lp_setup_context *setup,
                                const ubyte refs[2])
{
   LP_DBG(DEBUG_SETUP, "%s %d %d\n", __FUNCTION__, refs[0], refs[1]);

   if (setup->fs.current.jit_context.stencil_ref_front != refs[0] ||
       setup->fs.current.jit_context.stencil_ref_back != refs[1]) {
      setup->fs.current.jit_context.stencil_ref_front = refs[0];
      setup->fs.current.jit_context.stencil_ref_back = refs[1];
      setup->dirty |= LP_SETUP_NEW_FS;
   }
}


void
lp_setup_set_blend_color(struct lp_setup_context *setup,
                         const struct pipe_blend_color *blend_color)
{
   LP_DBG(DEBUG_SETUP, "%s\n", __FUNCTION__);

   assert(blend_color);

   if (memcmp(&setup->blend_color.current,
              blend_color, sizeof *blend_color) != 0) {
      memcpy(&setup->blend_color.current, blend_color, sizeof *blend_color);
      setup->dirty |= LP_SETUP_NEW_BLEND_COLOR;
   }
}


void
lp_setup_set_scissors(struct lp_setup_context *setup,
                      const struct pipe_scissor_state *scissors)
{
   LP_DBG(DEBUG_SETUP, "%s\n", __FUNCTION__);

   assert(scissors);

   for (unsigned i = 0; i < PIPE_MAX_VIEWPORTS; ++i) {
      setup->scissors[i].x0 = scissors[i].minx;
      setup->scissors[i].x1 = scissors[i].maxx-1;
      setup->scissors[i].y0 = scissors[i].miny;
      setup->scissors[i].y1 = scissors[i].maxy-1;
   }
   setup->dirty |= LP_SETUP_NEW_SCISSOR;
}


void
lp_setup_set_sample_mask(struct lp_setup_context *setup,
                         uint32_t sample_mask)
{
   if (setup->fs.current.jit_context.sample_mask != sample_mask) {
      setup->fs.current.jit_context.sample_mask = sample_mask;
      setup->dirty |= LP_SETUP_NEW_FS;
   }
}


void
lp_setup_set_rasterizer_discard(struct lp_setup_context *setup,
                                boolean rasterizer_discard)
{
   if (setup->rasterizer_discard != rasterizer_discard) {
      setup->rasterizer_discard = rasterizer_discard;
      setup->line = first_line;
      setup->point = first_point;
      setup->triangle = first_triangle;
      setup->rect = first_rectangle;
   }
}


void
lp_setup_set_vertex_info(struct lp_setup_context *setup,
                         struct vertex_info *vertex_info)
{
   /* XXX: just silently holding onto the pointer:
    */
   setup->vertex_info = vertex_info;
}


void
lp_setup_set_linear_mode(struct lp_setup_context *setup,
                         boolean mode)
{
   /* The linear rasterizer requires sse2 both at compile and runtime,
    * in particular for the code in lp_rast_linear_fallback.c.  This
    * is more than ten-year-old technology, so it's a reasonable
    * baseline.
    */
#if defined(PIPE_ARCH_SSE)
   setup->permit_linear_rasterizer = (mode &&
                                      util_get_cpu_caps()->has_sse2);
#else
   setup->permit_linear_rasterizer = FALSE;
#endif
}


/**
 * Called during state validation when LP_NEW_VIEWPORT is set.
 */
void
lp_setup_set_viewports(struct lp_setup_context *setup,
                       unsigned num_viewports,
                       const struct pipe_viewport_state *viewports)
{
   struct llvmpipe_context *lp = llvmpipe_context(setup->pipe);

   LP_DBG(DEBUG_SETUP, "%s\n", __FUNCTION__);

   assert(num_viewports <= PIPE_MAX_VIEWPORTS);
   assert(viewports);

   /*
    * Linear rasterizer path for scissor/viewport intersection.
    *
    * Calculate "scissor" rect from the (first) viewport.
    * Just like stored scissor rects need inclusive coords.
    * For rounding, assume half pixel center (d3d9 should not end up
    * with fractional viewports) - quite obviously for msaa we'd need
    * fractional values here (and elsewhere for the point bounding box).
    *
    * See: lp_setup.c::try_update_scene_state
    */
   const float half_height = fabsf(viewports[0].scale[1]);
   const float x0 = viewports[0].translate[0] - viewports[0].scale[0];
   const float y0 = viewports[0].translate[1] - half_height;

   setup->vpwh.x0 = (int)(x0 + 0.499f);
   setup->vpwh.x1 = (int)(viewports[0].scale[0] * 2.0f + x0 - 0.501f);
   setup->vpwh.y0 = (int)(y0 + 0.499f);
   setup->vpwh.y1 = (int)(half_height * 2.0f + y0 - 0.501f);
   setup->dirty |= LP_SETUP_NEW_SCISSOR;

   /*
    * For use in lp_state_fs.c, propagate the viewport values for all viewports.
    */
   for (unsigned i = 0; i < num_viewports; i++) {
      float min_depth, max_depth;
      util_viewport_zmin_zmax(&viewports[i], lp->rasterizer->clip_halfz,
                              &min_depth, &max_depth);

      if (setup->viewports[i].min_depth != min_depth ||
          setup->viewports[i].max_depth != max_depth) {
          setup->viewports[i].min_depth = min_depth;
          setup->viewports[i].max_depth = max_depth;
          setup->dirty |= LP_SETUP_NEW_VIEWPORTS;
      }
   }
}


/**
 * Called directly by llvmpipe_set_sampler_views
 */
void
lp_setup_set_fragment_sampler_views(struct lp_setup_context *setup,
                                    unsigned num,
                                    struct pipe_sampler_view **views)
{
   LP_DBG(DEBUG_SETUP, "%s\n", __FUNCTION__);

   assert(num <= PIPE_MAX_SHADER_SAMPLER_VIEWS);

   const unsigned max_tex_num = MAX2(num, setup->fs.current_tex_num);

   for (unsigned i = 0; i < max_tex_num; i++) {
      const struct pipe_sampler_view *view = i < num ? views[i] : NULL;

      /* We are going to overwrite/unref the current texture further below. If
       * set, make sure to unmap its resource to avoid leaking previous
       * mapping.  */
      if (setup->fs.current_tex[i])
         llvmpipe_resource_unmap(setup->fs.current_tex[i], 0, 0);

      if (view) {
         struct pipe_resource *res = view->texture;
         struct llvmpipe_resource *lp_tex = llvmpipe_resource(res);
         struct lp_jit_texture *jit_tex;
         jit_tex = &setup->fs.current.jit_context.textures[i];

         /* We're referencing the texture's internal data, so save a
          * reference to it.
          */
         pipe_resource_reference(&setup->fs.current_tex[i], res);

         if (!lp_tex->dt) {
            /* regular texture - setup array of mipmap level offsets */
            unsigned first_level = 0;
            unsigned last_level = 0;

            if (llvmpipe_resource_is_texture(res)) {
               first_level = view->u.tex.first_level;
               last_level = view->u.tex.last_level;
               assert(first_level <= last_level);
               assert(last_level <= res->last_level);
               jit_tex->base = lp_tex->tex_data;
            }
            else {
               jit_tex->base = lp_tex->data;
            }

            if (LP_PERF & PERF_TEX_MEM) {
               /* use dummy tile memory */
               jit_tex->base = lp_dummy_tile;
               jit_tex->width = TILE_SIZE/8;
               jit_tex->height = TILE_SIZE/8;
               jit_tex->depth = 1;
               jit_tex->first_level = 0;
               jit_tex->last_level = 0;
               jit_tex->mip_offsets[0] = 0;
               jit_tex->row_stride[0] = 0;
               jit_tex->img_stride[0] = 0;
               jit_tex->num_samples = 0;
               jit_tex->sample_stride = 0;
            }
            else {
               jit_tex->width = res->width0;
               jit_tex->height = res->height0;
               jit_tex->depth = res->depth0;
               jit_tex->first_level = first_level;
               jit_tex->last_level = last_level;
               jit_tex->num_samples = res->nr_samples;
               jit_tex->sample_stride = 0;

               if (llvmpipe_resource_is_texture(res)) {
                  for (unsigned j = first_level; j <= last_level; j++) {
                     jit_tex->mip_offsets[j] = lp_tex->mip_offsets[j];
                     jit_tex->row_stride[j] = lp_tex->row_stride[j];
                     jit_tex->img_stride[j] = lp_tex->img_stride[j];
                  }

                  jit_tex->sample_stride = lp_tex->sample_stride;

                  if (res->target == PIPE_TEXTURE_1D_ARRAY ||
                      res->target == PIPE_TEXTURE_2D_ARRAY ||
                      res->target == PIPE_TEXTURE_CUBE ||
                      res->target == PIPE_TEXTURE_CUBE_ARRAY ||
                      (res->target == PIPE_TEXTURE_3D && view->target == PIPE_TEXTURE_2D)) {
                     /*
                      * For array textures, we don't have first_layer, instead
                      * adjust last_layer (stored as depth) plus the mip level
                      * offsets (as we have mip-first layout can't just adjust
                      * base ptr).  XXX For mip levels, could do something
                      * similar.
                      */
                     jit_tex->depth = view->u.tex.last_layer - view->u.tex.first_layer + 1;
                     for (unsigned j = first_level; j <= last_level; j++) {
                        jit_tex->mip_offsets[j] += view->u.tex.first_layer *
                                                   lp_tex->img_stride[j];
                     }
                     if (view->target == PIPE_TEXTURE_CUBE ||
                         view->target == PIPE_TEXTURE_CUBE_ARRAY) {
                        assert(jit_tex->depth % 6 == 0);
                     }
                     assert(view->u.tex.first_layer <= view->u.tex.last_layer);
                     if (res->target == PIPE_TEXTURE_3D)
                        assert(view->u.tex.last_layer < res->depth0);
                     else
                        assert(view->u.tex.last_layer < res->array_size);
                  }
               }
               else {
                  /*
                   * For buffers, we don't have "offset", instead adjust
                   * the size (stored as width) plus the base pointer.
                   */
                  const unsigned view_blocksize =
                     util_format_get_blocksize(view->format);
                  /* probably don't really need to fill that out */
                  jit_tex->mip_offsets[0] = 0;
                  jit_tex->row_stride[0] = 0;
                  jit_tex->img_stride[0] = 0;

                  /* everything specified in number of elements here. */
                  jit_tex->width = view->u.buf.size / view_blocksize;
                  jit_tex->base = (uint8_t *)jit_tex->base + view->u.buf.offset;
                  /* XXX Unsure if we need to sanitize parameters? */
                  assert(view->u.buf.offset + view->u.buf.size <= res->width0);
               }
            }
         }
         else {
            /* display target texture/surface */
            jit_tex->base = llvmpipe_resource_map(res, 0, 0, LP_TEX_USAGE_READ);
            jit_tex->row_stride[0] = lp_tex->row_stride[0];
            jit_tex->img_stride[0] = lp_tex->img_stride[0];
            jit_tex->mip_offsets[0] = 0;
            jit_tex->width = res->width0;
            jit_tex->height = res->height0;
            jit_tex->depth = res->depth0;
            jit_tex->first_level = jit_tex->last_level = 0;
            jit_tex->num_samples = res->nr_samples;
            jit_tex->sample_stride = 0;
            assert(jit_tex->base);
         }
      }
      else {
         pipe_resource_reference(&setup->fs.current_tex[i], NULL);
      }
   }
   setup->fs.current_tex_num = num;

   setup->dirty |= LP_SETUP_NEW_FS;
}


/**
 * Called during state validation when LP_NEW_SAMPLER is set.
 */
void
lp_setup_set_fragment_sampler_state(struct lp_setup_context *setup,
                                    unsigned num,
                                    struct pipe_sampler_state **samplers)
{
   LP_DBG(DEBUG_SETUP, "%s\n", __FUNCTION__);

   assert(num <= PIPE_MAX_SAMPLERS);

   for (unsigned i = 0; i < PIPE_MAX_SAMPLERS; i++) {
      const struct pipe_sampler_state *sampler = i < num ? samplers[i] : NULL;

      if (sampler) {
         struct lp_jit_sampler *jit_sam;
         jit_sam = &setup->fs.current.jit_context.samplers[i];

         jit_sam->min_lod = sampler->min_lod;
         jit_sam->max_lod = sampler->max_lod;
         jit_sam->lod_bias = sampler->lod_bias;
         jit_sam->max_aniso = sampler->max_anisotropy;
         COPY_4V(jit_sam->border_color, sampler->border_color.f);
      }
   }

   setup->dirty |= LP_SETUP_NEW_FS;
}


/**
 * Is the given texture referenced by any scene?
 * Note: we have to check all scenes including any scenes currently
 * being rendered and the current scene being built.
 */
unsigned
lp_setup_is_resource_referenced(const struct lp_setup_context *setup,
                                const struct pipe_resource *texture)
{
   /* check the render targets */
   for (unsigned i = 0; i < setup->fb.nr_cbufs; i++) {
      if (setup->fb.cbufs[i] && setup->fb.cbufs[i]->texture == texture)
         return LP_REFERENCED_FOR_READ | LP_REFERENCED_FOR_WRITE;
   }
   if (setup->fb.zsbuf && setup->fb.zsbuf->texture == texture) {
      return LP_REFERENCED_FOR_READ | LP_REFERENCED_FOR_WRITE;
   }

   /* check resources referenced by active scenes */
   for (unsigned i = 0; i < setup->num_active_scenes; i++) {
      struct lp_scene *scene = setup->scenes[i];
      /* check the render targets */
      for (unsigned j = 0; j < scene->fb.nr_cbufs; j++) {
         if (scene->fb.cbufs[j] && scene->fb.cbufs[j]->texture == texture)
            return LP_REFERENCED_FOR_READ | LP_REFERENCED_FOR_WRITE;
      }
      if (scene->fb.zsbuf && scene->fb.zsbuf->texture == texture) {
         return LP_REFERENCED_FOR_READ | LP_REFERENCED_FOR_WRITE;
      }

      /* check resources referenced by the scene */
      unsigned ref = lp_scene_is_resource_referenced(scene, texture);
      if (ref)
         return ref;
   }

   return LP_UNREFERENCED;
}


/**
 * Called by vbuf code when we're about to draw something.
 *
 * This function stores all dirty state in the current scene's display list
 * memory, via lp_scene_alloc().  We can not pass pointers of mutable state to
 * the JIT functions, as the JIT functions will be called later on, most likely
 * on a different thread.
 *
 * When processing dirty state it is imperative that we don't refer to any
 * pointers previously allocated with lp_scene_alloc() in this function (or any
 * function) as they may belong to a scene freed since then.
 */
static boolean
try_update_scene_state(struct lp_setup_context *setup)
{
   static const float fake_const_buf[4];
   boolean new_scene = (setup->fs.stored == NULL);
   struct lp_scene *scene = setup->scene;

   assert(scene);

   if (setup->dirty & LP_SETUP_NEW_VIEWPORTS) {
      /*
       * Record new depth range state for changes due to viewport updates.
       *
       * TODO: Collapse the existing viewport and depth range information
       *       into one structure, for access by JIT.
       */
      struct lp_jit_viewport *stored;

      stored = (struct lp_jit_viewport *)
         lp_scene_alloc(scene, sizeof setup->viewports);

      if (!stored) {
         assert(!new_scene);
         return FALSE;
      }

      memcpy(stored, setup->viewports, sizeof setup->viewports);

      setup->fs.current.jit_context.viewports = stored;
      setup->dirty |= LP_SETUP_NEW_FS;
   }

   if (setup->dirty & LP_SETUP_NEW_BLEND_COLOR) {
      /* Alloc u8_blend_color (16 x i8) and f_blend_color (4 or 8 x f32) */
      const unsigned size = 4 * 16 * sizeof(uint8_t)
                          + (LP_MAX_VECTOR_LENGTH / 4) * sizeof(float);

      uint8_t *stored =
         lp_scene_alloc_aligned(scene, size, LP_MIN_VECTOR_ALIGN);

      if (!stored) {
         assert(!new_scene);
         return FALSE;
      }

      /* Store floating point colour (after ubyte colors (see below)) */
      float *fstored = (float *) (stored + 4 * 16);
      for (unsigned i = 0; i < (LP_MAX_VECTOR_LENGTH / 4); ++i) {
         fstored[i] = setup->blend_color.current.color[i % 4];
      }

      /* smear each blend color component across 16 ubyte elements */
      for (unsigned i = 0; i < 4; ++i) {
         uint8_t c = float_to_ubyte(setup->blend_color.current.color[i]);
         for (unsigned j = 0; j < 16; ++j) {
            stored[i*16 + j] = c;
         }
      }

      setup->blend_color.stored = stored;
      setup->fs.current.jit_context.u8_blend_color = stored;
      setup->fs.current.jit_context.f_blend_color = fstored;
      setup->dirty |= LP_SETUP_NEW_FS;
   }

   struct llvmpipe_context *llvmpipe = llvmpipe_context(setup->pipe);
   if (llvmpipe->dirty & LP_NEW_FS_CONSTANTS)
      lp_setup_set_fs_constants(llvmpipe->setup,
                                ARRAY_SIZE(llvmpipe->constants[PIPE_SHADER_FRAGMENT]),
                                llvmpipe->constants[PIPE_SHADER_FRAGMENT]);

   if (setup->dirty & LP_SETUP_NEW_CONSTANTS) {
      for (unsigned i = 0; i < ARRAY_SIZE(setup->constants); ++i) {
         struct pipe_resource *buffer = setup->constants[i].current.buffer;
         const unsigned current_size = MIN2(setup->constants[i].current.buffer_size,
                                            LP_MAX_TGSI_CONST_BUFFER_SIZE);
         const ubyte *current_data = NULL;

         STATIC_ASSERT(DATA_BLOCK_SIZE >= LP_MAX_TGSI_CONST_BUFFER_SIZE);

         if (buffer) {
            /* resource buffer */
            current_data = (ubyte *) llvmpipe_resource_data(buffer);
         }
         else if (setup->constants[i].current.user_buffer) {
            /* user-space buffer */
            current_data = (ubyte *) setup->constants[i].current.user_buffer;
         }

         if (current_data && current_size >= sizeof(float)) {
            current_data += setup->constants[i].current.buffer_offset;

            /* TODO: copy only the actually used constants? */

            if (setup->constants[i].stored_size != current_size ||
               !setup->constants[i].stored_data ||
               memcmp(setup->constants[i].stored_data,
                      current_data,
                      current_size) != 0) {

               void *stored = lp_scene_alloc(scene, current_size);
               if (!stored) {
                  assert(!new_scene);
                  return FALSE;
               }

               memcpy(stored,
                      current_data,
                      current_size);
               setup->constants[i].stored_size = current_size;
               setup->constants[i].stored_data = stored;
            }
            setup->fs.current.jit_context.constants[i].f =
               setup->constants[i].stored_data;
         }
         else {
            setup->constants[i].stored_size = 0;
            setup->constants[i].stored_data = NULL;
            setup->fs.current.jit_context.constants[i].f = fake_const_buf;
         }

         const int num_constants =
            DIV_ROUND_UP(setup->constants[i].stored_size,
                         lp_get_constant_buffer_stride(scene->pipe->screen));
         setup->fs.current.jit_context.constants[i].num_elements = num_constants;
         setup->dirty |= LP_SETUP_NEW_FS;
      }
   }

   if (setup->dirty & LP_SETUP_NEW_SSBOS) {
      for (unsigned i = 0; i < ARRAY_SIZE(setup->ssbos); ++i) {
         struct pipe_resource *buffer = setup->ssbos[i].current.buffer;
         const ubyte *current_data = NULL;

         /* resource buffer */
         if (buffer)
            current_data = (ubyte *) llvmpipe_resource_data(buffer);

         if (current_data) {
            current_data += setup->ssbos[i].current.buffer_offset;

            setup->fs.current.jit_context.ssbos[i].u =
               (const uint32_t *)current_data;
            setup->fs.current.jit_context.ssbos[i].num_elements =
               setup->ssbos[i].current.buffer_size;
         } else {
            setup->fs.current.jit_context.ssbos[i].u = NULL;
            setup->fs.current.jit_context.ssbos[i].num_elements = 0;
         }
         setup->dirty |= LP_SETUP_NEW_FS;
      }
   }

   if (setup->dirty & LP_SETUP_NEW_FS) {
      if (!setup->fs.stored ||
          memcmp(setup->fs.stored,
                 &setup->fs.current,
                 sizeof setup->fs.current) != 0) {
         /* The fs state that's been stored in the scene is different from
          * the new, current state.  So allocate a new lp_rast_state object
          * and append it to the bin's setup data buffer.
          */
         struct lp_rast_state *stored =
            (struct lp_rast_state *) lp_scene_alloc(scene, sizeof *stored);
         if (!stored) {
            assert(!new_scene);
            return FALSE;
         }

         memcpy(&stored->jit_context,
                &setup->fs.current.jit_context,
                sizeof setup->fs.current.jit_context);

         stored->jit_context.aniso_filter_table =
            lp_build_sample_aniso_filter_table();
         stored->variant = setup->fs.current.variant;

         if (!lp_scene_add_frag_shader_reference(scene,
                                                 setup->fs.current.variant)) {
            return FALSE;
         }

         setup->fs.stored = stored;

         /* The scene now references the textures in the rasterization
          * state record.  Note that now.
          */
         for (unsigned i = 0; i < ARRAY_SIZE(setup->fs.current_tex); i++) {
            if (setup->fs.current_tex[i]) {
               if (!lp_scene_add_resource_reference(scene,
                                                    setup->fs.current_tex[i],
                                                    new_scene, false)) {
                  assert(!new_scene);
                  return FALSE;
               }
            }
         }

         for (unsigned i = 0; i < ARRAY_SIZE(setup->ssbos); i++) {
            if (setup->ssbos[i].current.buffer) {
               if (!lp_scene_add_resource_reference(scene,
                               setup->ssbos[i].current.buffer,
                               new_scene, setup->ssbo_write_mask & (1 << i))) {
                  assert(!new_scene);
                  return FALSE;
               }
            }
         }

         for (unsigned i = 0; i < ARRAY_SIZE(setup->images); i++) {
            if (setup->images[i].current.resource) {
               if (!lp_scene_add_resource_reference(scene,
                                                    setup->images[i].current.resource,
                                                    new_scene,
                                                    setup->images[i].current.shader_access & PIPE_IMAGE_ACCESS_WRITE)) {
                  assert(!new_scene);
                  return FALSE;
               }
            }
         }
      }
   }

   if (setup->dirty & LP_SETUP_NEW_SCISSOR) {
      for (unsigned i = 0; i < PIPE_MAX_VIEWPORTS; ++i) {
         setup->draw_regions[i] = setup->framebuffer;
         if (setup->scissor_test) {
            u_rect_possible_intersection(&setup->scissors[i],
                                         &setup->draw_regions[i]);
         }
      }
      if (setup->permit_linear_rasterizer) {
         /* NOTE: this only takes first vp into account. */
         boolean need_vp_scissoring =
            !!memcmp(&setup->vpwh, &setup->framebuffer,
                     sizeof(setup->framebuffer));

         assert(setup->viewport_index_slot < 0);
         if (need_vp_scissoring) {
            u_rect_possible_intersection(&setup->vpwh,
                                         &setup->draw_regions[0]);
         }
      }
      else if (setup->point_tri_clip) {
         /*
          * for d3d-style point clipping, we're going to need
          * the fake vp scissor too. Hence do the intersection with vp,
          * but don't indicate this. As above this will only work for first vp
          * which should be ok because we instruct draw to only skip point
          * clipping when there's only one viewport (this works because d3d10
          * points are always single pixel).
          * (Also note that if we have permit_linear_rasterizer this will
          * cause large points to always get vp scissored, regardless the
          * point_tri_clip setting.)
          */
         boolean need_vp_scissoring =
            !!memcmp(&setup->vpwh, &setup->framebuffer,
                     sizeof(setup->framebuffer));
         if (need_vp_scissoring) {
            u_rect_possible_intersection(&setup->vpwh,
                                         &setup->draw_regions[0]);
         }
      }
   }

   setup->dirty = 0;

   assert(setup->fs.stored);
   return TRUE;
}


boolean
lp_setup_update_state(struct lp_setup_context *setup,
                      boolean update_scene)
{
   /* Some of the 'draw' pipeline stages may have changed some driver state.
    * Make sure we've processed those state changes before anything else.
    *
    * XXX this is the only place where llvmpipe_context is used in the
    * setup code.  This may get refactored/changed...
    */
   {
      struct llvmpipe_context *lp = llvmpipe_context(setup->pipe);
      if (lp->dirty) {
         llvmpipe_update_derived(lp);
      }

      if (lp->setup->dirty) {
         llvmpipe_update_setup(lp);
      }

      assert(setup->setup.variant);

      /* Will probably need to move this somewhere else, just need
       * to know about vertex shader point size attribute.
       */
      setup->psize_slot = lp->psize_slot;
      setup->viewport_index_slot = lp->viewport_index_slot;
      setup->layer_slot = lp->layer_slot;
      setup->face_slot = lp->face_slot;

      assert(lp->dirty == 0);

      assert(lp->setup_variant.key.size ==
             setup->setup.variant->key.size);

      assert(memcmp(&lp->setup_variant.key,
                    &setup->setup.variant->key,
                    setup->setup.variant->key.size) == 0);
   }

   if (update_scene && setup->state != SETUP_ACTIVE) {
      if (!set_scene_state(setup, SETUP_ACTIVE, __FUNCTION__))
         return FALSE;
   }

   /* Only call into update_scene_state() if we already have a
    * scene:
    */
   if (update_scene && setup->scene) {
      assert(setup->state == SETUP_ACTIVE);

      if (try_update_scene_state(setup))
         return TRUE;

      /* Update failed, try to restart the scene.
       *
       * Cannot call lp_setup_flush_and_restart() directly here
       * because of potential recursion.
       */
      if (!set_scene_state(setup, SETUP_FLUSHED, __FUNCTION__))
         return FALSE;

      if (!set_scene_state(setup, SETUP_ACTIVE, __FUNCTION__))
         return FALSE;

      if (!setup->scene)
         return FALSE;

      return try_update_scene_state(setup);
   }

   return TRUE;
}



/* Only caller is lp_setup_vbuf_destroy()
 */
void
lp_setup_destroy(struct lp_setup_context *setup)
{
   lp_setup_reset(setup);

   util_unreference_framebuffer_state(&setup->fb);

   for (unsigned i = 0; i < ARRAY_SIZE(setup->fs.current_tex); i++) {
      struct pipe_resource **res_ptr = &setup->fs.current_tex[i];
      if (*res_ptr)
         llvmpipe_resource_unmap(*res_ptr, 0, 0);
      pipe_resource_reference(res_ptr, NULL);
   }

   for (unsigned i = 0; i < ARRAY_SIZE(setup->constants); i++) {
      pipe_resource_reference(&setup->constants[i].current.buffer, NULL);
   }

   for (unsigned i = 0; i < ARRAY_SIZE(setup->ssbos); i++) {
      pipe_resource_reference(&setup->ssbos[i].current.buffer, NULL);
   }

   /* free the scenes in the 'empty' queue */
   for (unsigned i = 0; i < setup->num_active_scenes; i++) {
      struct lp_scene *scene = setup->scenes[i];

      if (scene->fence)
         lp_fence_wait(scene->fence);

      lp_scene_destroy(scene);
   }

   LP_DBG(DEBUG_SETUP, "number of scenes used: %d\n", setup->num_active_scenes);
   slab_destroy(&setup->scene_slab);

   FREE(setup);
}


/**
 * Create a new primitive tiling engine.  Plug it into the backend of
 * the draw module.  Currently also creates a rasterizer to use with
 * it.
 */
struct lp_setup_context *
lp_setup_create(struct pipe_context *pipe,
                struct draw_context *draw)
{
   struct llvmpipe_screen *screen = llvmpipe_screen(pipe->screen);
   struct lp_setup_context *setup = CALLOC_STRUCT(lp_setup_context);
   if (!setup) {
      goto no_setup;
   }

   lp_setup_init_vbuf(setup);

   /* Used only in update_state():
    */
   setup->pipe = pipe;

   setup->num_threads = screen->num_threads;
   setup->vbuf = draw_vbuf_stage(draw, &setup->base);
   if (!setup->vbuf) {
      goto no_vbuf;
   }

   draw_set_rasterize_stage(draw, setup->vbuf);
   draw_set_render(draw, &setup->base);

   slab_create(&setup->scene_slab,
               sizeof(struct lp_scene),
               INITIAL_SCENES);
   /* create just one scene for starting point */
   setup->scenes[0] = lp_scene_create(setup);
   if (!setup->scenes[0]) {
      goto no_scenes;
   }
   setup->num_active_scenes++;

   setup->triangle = first_triangle;
   setup->line     = first_line;
   setup->point    = first_point;

   setup->dirty = ~0;

   /* Initialize empty default fb correctly, so the rect is empty */
   setup->framebuffer.x1 = -1;
   setup->framebuffer.y1 = -1;

   return setup;

no_scenes:
   for (unsigned i = 0; i < MAX_SCENES; i++) {
      if (setup->scenes[i]) {
         lp_scene_destroy(setup->scenes[i]);
      }
   }

   setup->vbuf->destroy(setup->vbuf);
no_vbuf:
   FREE(setup);
no_setup:
   return NULL;
}


/**
 * Put a BeginQuery command into all bins.
 */
void
lp_setup_begin_query(struct lp_setup_context *setup,
                     struct llvmpipe_query *pq)
{
   set_scene_state(setup, SETUP_ACTIVE, "begin_query");

   if (!(pq->type == PIPE_QUERY_OCCLUSION_COUNTER ||
         pq->type == PIPE_QUERY_OCCLUSION_PREDICATE ||
         pq->type == PIPE_QUERY_OCCLUSION_PREDICATE_CONSERVATIVE ||
         pq->type == PIPE_QUERY_PIPELINE_STATISTICS ||
         pq->type == PIPE_QUERY_TIME_ELAPSED))
      return;

   /* init the query to its beginning state */
   assert(setup->active_binned_queries < LP_MAX_ACTIVE_BINNED_QUERIES);
   /* exceeding list size so just ignore the query */
   if (setup->active_binned_queries >= LP_MAX_ACTIVE_BINNED_QUERIES) {
      return;
   }
   assert(setup->active_queries[setup->active_binned_queries] == NULL);
   setup->active_queries[setup->active_binned_queries] = pq;
   setup->active_binned_queries++;

   assert(setup->scene);
   if (setup->scene) {
      if (!lp_scene_bin_everywhere(setup->scene,
                                   LP_RAST_OP_BEGIN_QUERY,
                                   lp_rast_arg_query(pq))) {

         if (!lp_setup_flush_and_restart(setup))
            return;

         if (!lp_scene_bin_everywhere(setup->scene,
                                      LP_RAST_OP_BEGIN_QUERY,
                                      lp_rast_arg_query(pq))) {
            return;
         }
      }
      setup->scene->had_queries |= TRUE;
   }
}


/**
 * Put an EndQuery command into all bins.
 */
void
lp_setup_end_query(struct lp_setup_context *setup, struct llvmpipe_query *pq)
{
   set_scene_state(setup, SETUP_ACTIVE, "end_query");

   assert(setup->scene);
   if (setup->scene) {
      /* pq->fence should be the fence of the *last* scene which
       * contributed to the query result.
       */
      lp_fence_reference(&pq->fence, setup->scene->fence);

      if (pq->type == PIPE_QUERY_OCCLUSION_COUNTER ||
          pq->type == PIPE_QUERY_OCCLUSION_PREDICATE ||
          pq->type == PIPE_QUERY_OCCLUSION_PREDICATE_CONSERVATIVE ||
          pq->type == PIPE_QUERY_PIPELINE_STATISTICS ||
          pq->type == PIPE_QUERY_TIMESTAMP ||
          pq->type == PIPE_QUERY_TIME_ELAPSED) {
         if (pq->type == PIPE_QUERY_TIMESTAMP &&
               !(setup->scene->tiles_x | setup->scene->tiles_y)) {
            /*
             * If there's a zero width/height framebuffer, there's no bins and
             * hence no rast task is ever run. So fill in something here instead.
             */
            pq->end[0] = os_time_get_nano();
         }

         if (!lp_scene_bin_everywhere(setup->scene,
                                      LP_RAST_OP_END_QUERY,
                                      lp_rast_arg_query(pq))) {
            if (!lp_setup_flush_and_restart(setup))
               goto fail;

            if (!lp_scene_bin_everywhere(setup->scene,
                                         LP_RAST_OP_END_QUERY,
                                         lp_rast_arg_query(pq))) {
               goto fail;
            }
         }
         setup->scene->had_queries |= TRUE;
      }
   }
   else {
      struct llvmpipe_screen *screen = llvmpipe_screen(setup->pipe->screen);
      mtx_lock(&screen->rast_mutex);
      lp_rast_fence(screen->rast, &pq->fence);
      mtx_unlock(&screen->rast_mutex);
   }

fail:
   /* Need to do this now not earlier since it still needs to be marked as
    * active when binning it would cause a flush.
    */
   if (pq->type == PIPE_QUERY_OCCLUSION_COUNTER ||
      pq->type == PIPE_QUERY_OCCLUSION_PREDICATE ||
      pq->type == PIPE_QUERY_OCCLUSION_PREDICATE_CONSERVATIVE ||
      pq->type == PIPE_QUERY_PIPELINE_STATISTICS ||
      pq->type == PIPE_QUERY_TIME_ELAPSED) {
      unsigned i;

      /* remove from active binned query list */
      for (i = 0; i < setup->active_binned_queries; i++) {
         if (setup->active_queries[i] == pq)
            break;
      }
      assert(i < setup->active_binned_queries);
      if (i == setup->active_binned_queries)
         return;
      setup->active_binned_queries--;
      setup->active_queries[i] = setup->active_queries[setup->active_binned_queries];
      setup->active_queries[setup->active_binned_queries] = NULL;
   }
}


boolean
lp_setup_flush_and_restart(struct lp_setup_context *setup)
{
   if (0) debug_printf("%s\n", __FUNCTION__);

   assert(setup->state == SETUP_ACTIVE);

   if (!set_scene_state(setup, SETUP_FLUSHED, __FUNCTION__))
      return FALSE;

   if (!lp_setup_update_state(setup, TRUE))
      return FALSE;

   return TRUE;
}


void
lp_setup_add_scissor_planes(const struct u_rect *scissor,
                            struct lp_rast_plane *plane_s,
                            boolean s_planes[4], bool multisample)
{
   /*
    * When rasterizing scissored tris, use the intersection of the
    * triangle bounding box and the scissor rect to generate the
    * scissor planes.
    *
    * This permits us to cut off the triangle "tails" that are present
    * in the intermediate recursive levels caused when two of the
    * triangles edges don't diverge quickly enough to trivially reject
    * exterior blocks from the triangle.
    *
    * It's not really clear if it's worth worrying about these tails,
    * but since we generate the planes for each scissored tri, it's
    * free to trim them in this case.
    *
    * Note that otherwise, the scissor planes only vary in 'C' value,
    * and even then only on state-changes.  Could alternatively store
    * these planes elsewhere.
    * (Or only store the c value together with a bit indicating which
    * scissor edge this is, so rasterization would treat them differently
    * (easier to evaluate) to ordinary planes.)
    */
   int adj = multisample ? 127 : 0;
   if (s_planes[0]) {
      int x0 = scissor->x0 - 1;
      plane_s->dcdx = ~0U << 8;
      plane_s->dcdy = 0;
      plane_s->c = x0 << 8;
      plane_s->c += adj;
      plane_s->c = -plane_s->c; /* flip sign */
      plane_s->eo = 1 << 8;
      plane_s++;
   }
   if (s_planes[1]) {
      int x1 = scissor->x1;
      plane_s->dcdx = 1 << 8;
      plane_s->dcdy = 0;
      plane_s->c = x1 << 8;
      plane_s->c += 127 + adj;
      plane_s->eo = 0 << 8;
      plane_s++;
   }
   if (s_planes[2]) {
      int y0 = scissor->y0 - 1;
      plane_s->dcdx = 0;
      plane_s->dcdy = 1 << 8;
      plane_s->c = y0 << 8;
      plane_s->c += adj;
      plane_s->c = -plane_s->c; /* flip sign */
      plane_s->eo = 1 << 8;
      plane_s++;
   }
   if (s_planes[3]) {
      int y1 = scissor->y1;
      plane_s->dcdx = 0;
      plane_s->dcdy = ~0U << 8;
      plane_s->c = y1 << 8;
      plane_s->c += 127 + adj;
      plane_s->eo = 0;
      plane_s++;
   }
}
