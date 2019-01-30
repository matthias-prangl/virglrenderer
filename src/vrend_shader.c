/**************************************************************************
 *
 * Copyright (C) 2014 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#include "tgsi/tgsi_info.h"
#include "tgsi/tgsi_iterate.h"
#include "tgsi/tgsi_scan.h"
#include "util/u_memory.h"
#include "util/u_math.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <errno.h>
#include "vrend_shader.h"
#include "vrend_debug.h"

#include "vrend_strbuf.h"

/* start convert of tgsi to glsl */

#define INTERP_PREFIX "                           "
#define INVARI_PREFIX "invariant"

#define SHADER_REQ_NONE 0
#define SHADER_REQ_SAMPLER_RECT       (1 << 0)
#define SHADER_REQ_CUBE_ARRAY         (1 << 1)
#define SHADER_REQ_INTS               (1 << 2)
#define SHADER_REQ_SAMPLER_MS         (1 << 3)
#define SHADER_REQ_INSTANCE_ID        (1 << 4)
#define SHADER_REQ_LODQ               (1 << 5)
#define SHADER_REQ_TXQ_LEVELS         (1 << 6)
#define SHADER_REQ_TG4                (1 << 7)
#define SHADER_REQ_VIEWPORT_IDX       (1 << 8)
#define SHADER_REQ_STENCIL_EXPORT     (1 << 9)
#define SHADER_REQ_LAYER              (1 << 10)
#define SHADER_REQ_SAMPLE_SHADING     (1 << 11)
#define SHADER_REQ_GPU_SHADER5        (1 << 12)
#define SHADER_REQ_DERIVATIVE_CONTROL (1 << 13)
#define SHADER_REQ_FP64               (1 << 14)
#define SHADER_REQ_IMAGE_LOAD_STORE   (1 << 15)
#define SHADER_REQ_ES31_COMPAT        (1 << 16)
#define SHADER_REQ_IMAGE_SIZE         (1 << 17)
#define SHADER_REQ_TXQS               (1 << 18)
#define SHADER_REQ_FBFETCH            (1 << 19)
#define SHADER_REQ_SHADER_CLOCK       (1 << 20)
#define SHADER_REQ_PSIZE              (1 << 21)
#define SHADER_REQ_IMAGE_ATOMIC       (1 << 22)

struct vrend_shader_io {
   unsigned                name;
   unsigned                gpr;
   unsigned                done;
   int                        sid;
   unsigned                interpolate;
   int first;
   unsigned                location;
   bool                    invariant;
   bool                    precise;
   bool glsl_predefined_no_emit;
   bool glsl_no_index;
   bool glsl_gl_block;
   bool override_no_wm;
   bool is_int;
   bool fbfetch_used;
   char glsl_name[128];
   unsigned stream;
};

struct vrend_shader_sampler {
   int tgsi_sampler_type;
   enum tgsi_return_type tgsi_sampler_return;
};

struct vrend_shader_table {
   uint32_t key;
   const char *string;
};

struct vrend_shader_image {
   struct tgsi_declaration_image decl;
   enum tgsi_return_type image_return;
   bool vflag;
};

#define MAX_IMMEDIATE 1024
struct immed {
   int type;
   union imm {
      uint32_t ui;
      int32_t i;
      float f;
   } val[4];
};

struct vrend_temp_range {
   int first;
   int last;
   int array_id;
};

struct vrend_io_range {
   int first;
   int last;
   int array_id;
   bool used;
};

struct dump_ctx {
   struct tgsi_iterate_context iter;
   struct vrend_shader_cfg *cfg;
   struct tgsi_shader_info info;
   int prog_type;
   int size;
   struct vrend_strbuf glsl_main;
   struct vrend_strbuf glsl_hdr;
   uint instno;

   uint32_t num_interps;
   uint32_t num_inputs;
   uint32_t attrib_input_mask;
   struct vrend_shader_io inputs[64];
   uint32_t num_outputs;
   struct vrend_shader_io outputs[64];
   uint32_t num_system_values;
   struct vrend_shader_io system_values[32];

   struct vrend_io_range generic_input_range;
   struct vrend_io_range patch_input_range;
   struct vrend_io_range generic_output_range;
   struct vrend_io_range patch_output_range;

   uint32_t num_temp_ranges;
   struct vrend_temp_range *temp_ranges;

   struct vrend_shader_sampler samplers[32];
   uint32_t samplers_used;

   uint32_t ssbo_used_mask;
   uint32_t ssbo_atomic_mask;
   uint32_t ssbo_array_base;
   uint32_t ssbo_atomic_array_base;
   uint32_t ssbo_integer_mask;

   struct vrend_shader_image images[32];
   uint32_t images_used_mask;

   struct vrend_array *image_arrays;
   uint32_t num_image_arrays;

   struct vrend_array *sampler_arrays;
   uint32_t num_sampler_arrays;

   int num_consts;
   int num_imm;
   struct immed imm[MAX_IMMEDIATE];
   unsigned fragcoord_input;

   uint32_t req_local_mem;
   bool integer_memory;

   uint32_t ubo_base;
   uint32_t ubo_used_mask;
   int ubo_sizes[32];
   uint32_t num_address;

   uint32_t num_abo;
   int abo_idx[32];
   int abo_sizes[32];
   int abo_offsets[32];

   uint32_t shader_req_bits;

   struct pipe_stream_output_info *so;
   char **so_names;
   bool write_so_outputs[PIPE_MAX_SO_OUTPUTS];
   bool uses_sampler_buf;
   bool write_all_cbufs;
   uint32_t shadow_samp_mask;

   int fs_coord_origin, fs_pixel_center;

   int gs_in_prim, gs_out_prim, gs_max_out_verts;
   int gs_num_invocations;

   struct vrend_shader_key *key;
   int num_in_clip_dist;
   int num_clip_dist;
   int glsl_ver_required;
   int color_in_mask;
   /* only used when cull is enabled */
   uint8_t num_cull_dist_prop, num_clip_dist_prop;
   bool front_face_emitted;

   bool has_clipvertex;
   bool has_clipvertex_so;
   bool vs_has_pervertex;
   bool write_mul_utemp;
   bool write_mul_itemp;
   bool has_sample_input;
   bool early_depth_stencil;
   bool has_file_memory;

   int tcs_vertices_out;
   int tes_prim_mode;
   int tes_spacing;
   int tes_vertex_order;
   int tes_point_mode;

   uint16_t local_cs_block_size[3];
};

static const struct vrend_shader_table shader_req_table[] = {
    { SHADER_REQ_SAMPLER_RECT, "ARB_texture_rectangle" },
    { SHADER_REQ_CUBE_ARRAY, "ARB_texture_cube_map_array" },
    { SHADER_REQ_INTS, "ARB_shader_bit_encoding" },
    { SHADER_REQ_SAMPLER_MS, "ARB_texture_multisample" },
    { SHADER_REQ_INSTANCE_ID, "ARB_draw_instanced" },
    { SHADER_REQ_LODQ, "ARB_texture_query_lod" },
    { SHADER_REQ_TXQ_LEVELS, "ARB_texture_query_levels" },
    { SHADER_REQ_TG4, "ARB_texture_gather" },
    { SHADER_REQ_VIEWPORT_IDX, "ARB_viewport_array" },
    { SHADER_REQ_STENCIL_EXPORT, "ARB_shader_stencil_export" },
    { SHADER_REQ_LAYER, "ARB_fragment_layer_viewport" },
    { SHADER_REQ_SAMPLE_SHADING, "ARB_sample_shading" },
    { SHADER_REQ_GPU_SHADER5, "ARB_gpu_shader5" },
    { SHADER_REQ_DERIVATIVE_CONTROL, "ARB_derivative_control" },
    { SHADER_REQ_FP64, "ARB_gpu_shader_fp64" },
    { SHADER_REQ_IMAGE_LOAD_STORE, "ARB_shader_image_load_store" },
    { SHADER_REQ_ES31_COMPAT, "ARB_ES3_1_compatibility" },
    { SHADER_REQ_IMAGE_SIZE, "ARB_shader_image_size" },
    { SHADER_REQ_TXQS, "ARB_shader_texture_image_samples" },
    { SHADER_REQ_FBFETCH, "EXT_shader_framebuffer_fetch" },
    { SHADER_REQ_SHADER_CLOCK, "ARB_shader_clock" },
};

enum vrend_type_qualifier {
   TYPE_CONVERSION_NONE = 0,
   FLOAT = 1,
   VEC2 = 2,
   VEC3 = 3,
   VEC4 = 4,
   INT = 5,
   IVEC2 = 6,
   IVEC3 = 7,
   IVEC4 = 8,
   UINT = 9,
   UVEC2 = 10,
   UVEC3 = 11,
   UVEC4 = 12,
   FLOAT_BITS_TO_UINT = 13,
   UINT_BITS_TO_FLOAT = 14,
   FLOAT_BITS_TO_INT = 15,
   INT_BITS_TO_FLOAT = 16,
   DOUBLE = 17,
   DVEC2 = 18,
};

struct dest_info {
  enum vrend_type_qualifier dtypeprefix;
  enum vrend_type_qualifier dstconv;
  enum vrend_type_qualifier udstconv;
  enum vrend_type_qualifier idstconv;
  bool dst_override_no_wm[2];
};

struct source_info {
   enum vrend_type_qualifier svec4;
   uint32_t sreg_index;
   bool tg4_has_component;
   bool override_no_wm[3];
   bool override_no_cast[3];
   int imm_value;
};

static const struct vrend_shader_table conversion_table[] =
{
   {TYPE_CONVERSION_NONE, ""},
   {FLOAT, "float"},
   {VEC2, "vec2"},
   {VEC3, "vec3"},
   {VEC4, "vec4"},
   {INT, "int"},
   {IVEC2, "ivec2"},
   {IVEC3, "ivec3"},
   {IVEC4, "ivec4"},
   {UINT, "uint"},
   {UVEC2, "uvec2"},
   {UVEC3, "uvec3"},
   {UVEC4, "uvec4"},
   {FLOAT_BITS_TO_UINT, "floatBitsToUint"},
   {UINT_BITS_TO_FLOAT, "uintBitsToFloat"},
   {FLOAT_BITS_TO_INT, "floatBitsToInt"},
   {INT_BITS_TO_FLOAT, "intBitsToFloat"},
   {DOUBLE, "double"},
   {DVEC2, "dvec2"},
};

static inline const char *get_string(enum vrend_type_qualifier key)
{
   if (key >= ARRAY_SIZE(conversion_table)) {
      printf("Unable to find the correct conversion\n");
      return conversion_table[TYPE_CONVERSION_NONE].string;
   }

   return conversion_table[key].string;
}

static inline const char *get_wm_string(unsigned wm)
{
   switch(wm) {
   case TGSI_WRITEMASK_NONE:
      return "";
   case TGSI_WRITEMASK_X:
      return ".x";
   case TGSI_WRITEMASK_XY:
      return ".xy";
   case TGSI_WRITEMASK_XYZ:
      return ".xyz";
   case TGSI_WRITEMASK_W:
      return ".w";
   default:
      printf("Unable to unknown writemask\n");
      return "";
   }
}

const char *get_internalformat_string(int virgl_format, enum tgsi_return_type *stype);

static inline const char *tgsi_proc_to_prefix(int shader_type)
{
   switch (shader_type) {
   case TGSI_PROCESSOR_VERTEX: return "vs";
   case TGSI_PROCESSOR_FRAGMENT: return "fs";
   case TGSI_PROCESSOR_GEOMETRY: return "gs";
   case TGSI_PROCESSOR_TESS_CTRL: return "tc";
   case TGSI_PROCESSOR_TESS_EVAL: return "te";
   case TGSI_PROCESSOR_COMPUTE: return "cs";
   default:
      return NULL;
   };
}

static inline const char *prim_to_name(int prim)
{
   switch (prim) {
   case PIPE_PRIM_POINTS: return "points";
   case PIPE_PRIM_LINES: return "lines";
   case PIPE_PRIM_LINE_STRIP: return "line_strip";
   case PIPE_PRIM_LINES_ADJACENCY: return "lines_adjacency";
   case PIPE_PRIM_TRIANGLES: return "triangles";
   case PIPE_PRIM_TRIANGLE_STRIP: return "triangle_strip";
   case PIPE_PRIM_TRIANGLES_ADJACENCY: return "triangles_adjacency";
   case PIPE_PRIM_QUADS: return "quads";
   default: return "UNKNOWN";
   };
}

static inline const char *prim_to_tes_name(int prim)
{
   switch (prim) {
   case PIPE_PRIM_QUADS: return "quads";
   case PIPE_PRIM_TRIANGLES: return "triangles";
   case PIPE_PRIM_LINES: return "isolines";
   default: return "UNKNOWN";
   }
}

static const char *get_spacing_string(int spacing)
{
   switch (spacing) {
   case PIPE_TESS_SPACING_FRACTIONAL_ODD:
      return "fractional_odd_spacing";
   case PIPE_TESS_SPACING_FRACTIONAL_EVEN:
      return "fractional_even_spacing";
   case PIPE_TESS_SPACING_EQUAL:
   default:
      return "equal_spacing";
   }
}

static inline int gs_input_prim_to_size(int prim)
{
   switch (prim) {
   case PIPE_PRIM_POINTS: return 1;
   case PIPE_PRIM_LINES: return 2;
   case PIPE_PRIM_LINES_ADJACENCY: return 4;
   case PIPE_PRIM_TRIANGLES: return 3;
   case PIPE_PRIM_TRIANGLES_ADJACENCY: return 6;
   default: return -1;
   };
}

static inline bool fs_emit_layout(struct dump_ctx *ctx)
{
   if (ctx->fs_pixel_center)
      return true;
   /* if coord origin is 0 and invert is 0 - emit origin_upper_left,
      if coord_origin is 0 and invert is 1 - emit nothing (lower)
      if coord origin is 1 and invert is 0 - emit nothing (lower)
      if coord_origin is 1 and invert is 1 - emit origin upper left */
   if (!(ctx->fs_coord_origin ^ ctx->key->invert_fs_origin))
      return true;
   return false;
}

static const char *get_stage_input_name_prefix(struct dump_ctx *ctx, int processor)
{
   const char *name_prefix;
   switch (processor) {
   case TGSI_PROCESSOR_FRAGMENT:
      if (ctx->key->gs_present)
         name_prefix = "gso";
      else if (ctx->key->tes_present)
         name_prefix = "teo";
      else
         name_prefix = "vso";
      break;
   case TGSI_PROCESSOR_GEOMETRY:
      if (ctx->key->tes_present)
         name_prefix = "teo";
      else
         name_prefix = "vso";
      break;
   case TGSI_PROCESSOR_TESS_EVAL:
      if (ctx->key->tcs_present)
         name_prefix = "tco";
      else
         name_prefix = "vso";
      break;
   case TGSI_PROCESSOR_TESS_CTRL:
       name_prefix = "vso";
       break;
   case TGSI_PROCESSOR_VERTEX:
   default:
      name_prefix = "in";
      break;
   }
   return name_prefix;
}

static const char *get_stage_output_name_prefix(int processor)
{
   const char *name_prefix;
   switch (processor) {
   case TGSI_PROCESSOR_FRAGMENT:
      name_prefix = "fsout";
      break;
   case TGSI_PROCESSOR_GEOMETRY:
      name_prefix = "gso";
      break;
   case TGSI_PROCESSOR_VERTEX:
      name_prefix = "vso";
      break;
   case TGSI_PROCESSOR_TESS_CTRL:
      name_prefix = "tco";
      break;
   case TGSI_PROCESSOR_TESS_EVAL:
      name_prefix = "teo";
      break;
   default:
      name_prefix = "out";
      break;
   }
   return name_prefix;
}

static void require_glsl_ver(struct dump_ctx *ctx, int glsl_ver)
{
   if (glsl_ver > ctx->glsl_ver_required)
      ctx->glsl_ver_required = glsl_ver;
}

static void emit_buf(struct dump_ctx *ctx, const char *buf)
{
   strbuf_append(&ctx->glsl_main, buf);
}

static void indent_buf(struct dump_ctx *ctx)
{
   return strbuf_indent(&ctx->glsl_main);
}

static void outdent_buf(struct dump_ctx *ctx)
{
   return strbuf_outdent(&ctx->glsl_main);
}

static void set_buf_error(struct dump_ctx *ctx)
{
   strbuf_set_error(&ctx->glsl_main);
}

__attribute__((format(printf, 2, 3)))
static void emit_buff(struct dump_ctx *ctx, const char *fmt, ...)
{
   va_list va;
   va_start(va, fmt);
   strbuf_vappendf(&ctx->glsl_main, fmt, va);
   va_end(va);
}

static void emit_hdr(struct dump_ctx *ctx, const char *buf)
{
   strbuf_append(&ctx->glsl_hdr, buf);
}

static void set_hdr_error(struct dump_ctx *ctx)
{
   strbuf_set_error(&ctx->glsl_hdr);
}

__attribute__((format(printf, 2, 3)))
static void emit_hdrf(struct dump_ctx *ctx, const char *fmt, ...)
{
   va_list va;
   va_start(va, fmt);
   strbuf_vappendf(&ctx->glsl_hdr, fmt, va);
   va_end(va);
}

static bool allocate_temp_range(struct dump_ctx *ctx, int first, int last,
                                int array_id)
{
   int idx = ctx->num_temp_ranges;

   ctx->temp_ranges = realloc(ctx->temp_ranges, sizeof(struct vrend_temp_range) * (idx + 1));
   if (!ctx->temp_ranges)
      return false;

   ctx->temp_ranges[idx].first = first;
   ctx->temp_ranges[idx].last = last;
   ctx->temp_ranges[idx].array_id = array_id;
   ctx->num_temp_ranges++;
   return true;
}

static struct vrend_temp_range *find_temp_range(struct dump_ctx *ctx, int index)
{
   uint32_t i;
   for (i = 0; i < ctx->num_temp_ranges; i++) {
      if (index >= ctx->temp_ranges[i].first &&
          index <= ctx->temp_ranges[i].last)
         return &ctx->temp_ranges[i];
   }
   return NULL;
}

static bool add_images(struct dump_ctx *ctx, int first, int last,
                       struct tgsi_declaration_image *img_decl)
{
   int i;

   for (i = first; i <= last; i++) {
      ctx->images[i].decl = *img_decl;
      ctx->images[i].vflag = false;
      ctx->images_used_mask |= (1 << i);

      if (ctx->images[i].decl.Resource == TGSI_TEXTURE_CUBE_ARRAY)
         ctx->shader_req_bits |= SHADER_REQ_CUBE_ARRAY;
      else if (ctx->images[i].decl.Resource == TGSI_TEXTURE_2D_MSAA ||
          ctx->images[i].decl.Resource == TGSI_TEXTURE_2D_ARRAY_MSAA)
         ctx->shader_req_bits |= SHADER_REQ_SAMPLER_MS;
      else if (ctx->images[i].decl.Resource == TGSI_TEXTURE_BUFFER)
         ctx->uses_sampler_buf = true;
      else if (ctx->images[i].decl.Resource == TGSI_TEXTURE_RECT)
         ctx->shader_req_bits |= SHADER_REQ_SAMPLER_RECT;
   }

   if (ctx->info.indirect_files & (1 << TGSI_FILE_IMAGE)) {
      if (ctx->num_image_arrays) {
         struct vrend_array *last_array = &ctx->image_arrays[ctx->num_image_arrays - 1];
         /*
          * If this set of images is consecutive to the last array,
          * and has compatible return and decls, then increase the array size.
          */
         if ((last_array->first + last_array->array_size == first) &&
             !memcmp(&ctx->images[last_array->first].decl, &ctx->images[first].decl, sizeof(ctx->images[first].decl)) &&
             ctx->images[last_array->first].image_return == ctx->images[first].image_return) {
            last_array->array_size += last - first + 1;
            return true;
         }
      }

      /* allocate a new image array for this range of images */
      ctx->num_image_arrays++;
      ctx->image_arrays = realloc(ctx->image_arrays, sizeof(struct vrend_array) * ctx->num_image_arrays);
      if (!ctx->image_arrays)
         return false;
      ctx->image_arrays[ctx->num_image_arrays - 1].first = first;
      ctx->image_arrays[ctx->num_image_arrays - 1].array_size = last - first + 1;
   }
   return true;
}

static bool add_sampler_array(struct dump_ctx *ctx, int first, int last)
{
   int idx = ctx->num_sampler_arrays;
   ctx->num_sampler_arrays++;
   ctx->sampler_arrays = realloc(ctx->sampler_arrays, sizeof(struct vrend_array) * ctx->num_sampler_arrays);
   if (!ctx->sampler_arrays)
      return false;

   ctx->sampler_arrays[idx].first = first;
   ctx->sampler_arrays[idx].array_size = last - first + 1;
   return true;
}

static int lookup_sampler_array(struct dump_ctx *ctx, int index)
{
   uint32_t i;
   for (i = 0; i < ctx->num_sampler_arrays; i++) {
      int last = ctx->sampler_arrays[i].first + ctx->sampler_arrays[i].array_size - 1;
      if (index >= ctx->sampler_arrays[i].first &&
          index <= last) {
         return ctx->sampler_arrays[i].first;
      }
   }
   return -1;
}

int shader_lookup_sampler_array(struct vrend_shader_info *sinfo, int index)
{
   int i;
   for (i = 0; i < sinfo->num_sampler_arrays; i++) {
      int last = sinfo->sampler_arrays[i].first + sinfo->sampler_arrays[i].array_size - 1;
      if (index >= sinfo->sampler_arrays[i].first &&
          index <= last) {
         return sinfo->sampler_arrays[i].first;
      }
   }
   return -1;
}

static bool add_samplers(struct dump_ctx *ctx, int first, int last, int sview_type, enum tgsi_return_type sview_rtype)
{
   if (sview_rtype == TGSI_RETURN_TYPE_SINT ||
       sview_rtype == TGSI_RETURN_TYPE_UINT)
      ctx->shader_req_bits |= SHADER_REQ_INTS;

   for (int i = first; i <= last; i++) {
      ctx->samplers[i].tgsi_sampler_return = sview_rtype;
      ctx->samplers[i].tgsi_sampler_type = sview_type;
   }

   if (ctx->info.indirect_files & (1 << TGSI_FILE_SAMPLER)) {
      if (ctx->num_sampler_arrays) {
         struct vrend_array *last_array = &ctx->sampler_arrays[ctx->num_sampler_arrays - 1];
         if ((last_array->first + last_array->array_size == first) &&
             ctx->samplers[last_array->first].tgsi_sampler_type == sview_type &&
             ctx->samplers[last_array->first].tgsi_sampler_return == sview_rtype) {
            last_array->array_size += last - first + 1;
            return true;
         }
      }

      /* allocate a new image array for this range of images */
      return add_sampler_array(ctx, first, last);
   }
   return true;
}

static bool ctx_indirect_inputs(struct dump_ctx *ctx)
{
   if (ctx->info.indirect_files & (1 << TGSI_FILE_INPUT))
      return true;
   if (ctx->key->num_indirect_generic_inputs || ctx->key->num_indirect_patch_inputs)
      return true;
   return false;
}

static bool ctx_indirect_outputs(struct dump_ctx *ctx)
{
   if (ctx->info.indirect_files & (1 << TGSI_FILE_OUTPUT))
      return true;
   if (ctx->key->num_indirect_generic_outputs || ctx->key->num_indirect_patch_outputs)
      return true;
   return false;
}

static int lookup_image_array(struct dump_ctx *ctx, int index)
{
   uint32_t i;
   for (i = 0; i < ctx->num_image_arrays; i++) {
      if (index >= ctx->image_arrays[i].first &&
          index <= ctx->image_arrays[i].first + ctx->image_arrays[i].array_size - 1) {
         return ctx->image_arrays[i].first;
      }
   }
   return -1;
}

static boolean
iter_declaration(struct tgsi_iterate_context *iter,
                 struct tgsi_full_declaration *decl )
{
   struct dump_ctx *ctx = (struct dump_ctx *)iter;
   int i;
   int color_offset = 0;
   const char *name_prefix = "";
   bool add_two_side = false;
   bool indirect = false;

   switch (decl->Declaration.File) {
   case TGSI_FILE_INPUT:
      for (uint32_t j = 0; j < ctx->num_inputs; j++) {
         if (ctx->inputs[j].name == decl->Semantic.Name &&
             ctx->inputs[j].sid == decl->Semantic.Index &&
             ctx->inputs[j].first == decl->Range.First)
            return true;
      }
      i = ctx->num_inputs++;
      indirect = ctx_indirect_inputs(ctx);
      if (ctx->num_inputs > ARRAY_SIZE(ctx->inputs)) {
         fprintf(stderr, "Number of inputs exceeded, max is %lu\n", ARRAY_SIZE(ctx->inputs));
         return false;
      }
      if (iter->processor.Processor == TGSI_PROCESSOR_VERTEX) {
         ctx->attrib_input_mask |= (1 << decl->Range.First);
      }
      ctx->inputs[i].name = decl->Semantic.Name;
      ctx->inputs[i].sid = decl->Semantic.Index;
      ctx->inputs[i].interpolate = decl->Interp.Interpolate;
      ctx->inputs[i].location = decl->Interp.Location;
      ctx->inputs[i].first = decl->Range.First;
      ctx->inputs[i].glsl_predefined_no_emit = false;
      ctx->inputs[i].glsl_no_index = false;
      ctx->inputs[i].override_no_wm = false;
      ctx->inputs[i].glsl_gl_block = false;

      if (iter->processor.Processor == TGSI_PROCESSOR_FRAGMENT &&
          decl->Interp.Location == TGSI_INTERPOLATE_LOC_SAMPLE) {
         ctx->shader_req_bits |= SHADER_REQ_GPU_SHADER5;
         ctx->has_sample_input = true;
      }

      switch (ctx->inputs[i].name) {
      case TGSI_SEMANTIC_COLOR:
         if (iter->processor.Processor == TGSI_PROCESSOR_FRAGMENT) {
            if (ctx->glsl_ver_required < 140) {
               if (decl->Semantic.Index == 0)
                  name_prefix = "gl_Color";
               else if (decl->Semantic.Index == 1)
                  name_prefix = "gl_SecondaryColor";
               else
                  fprintf(stderr, "got illegal color semantic index %d\n", decl->Semantic.Index);
               ctx->inputs[i].glsl_no_index = true;
            } else {
               if (ctx->key->color_two_side) {
                  int j = ctx->num_inputs++;
                  if (ctx->num_inputs > ARRAY_SIZE(ctx->inputs)) {
                     fprintf(stderr, "Number of inputs exceeded, max is %lu\n", ARRAY_SIZE(ctx->inputs));
                     return false;
                  }

                  ctx->inputs[j].name = TGSI_SEMANTIC_BCOLOR;
                  ctx->inputs[j].sid = decl->Semantic.Index;
                  ctx->inputs[j].interpolate = decl->Interp.Interpolate;
                  ctx->inputs[j].location = decl->Interp.Location;
                  ctx->inputs[j].first = decl->Range.First;
                  ctx->inputs[j].glsl_predefined_no_emit = false;
                  ctx->inputs[j].glsl_no_index = false;
                  ctx->inputs[j].override_no_wm = false;

                  ctx->color_in_mask |= (1 << decl->Semantic.Index);

                  if (ctx->front_face_emitted == false) {
                     int k = ctx->num_inputs++;
                     if (ctx->num_inputs > ARRAY_SIZE(ctx->inputs)) {
                        fprintf(stderr, "Number of inputs exceeded, max is %lu\n", ARRAY_SIZE(ctx->inputs));
                        return false;
                     }

                     ctx->inputs[k].name = TGSI_SEMANTIC_FACE;
                     ctx->inputs[k].sid = 0;
                     ctx->inputs[k].interpolate = 0;
                     ctx->inputs[k].location = TGSI_INTERPOLATE_LOC_CENTER;
                     ctx->inputs[k].first = 0;
                     ctx->inputs[k].override_no_wm = false;
                     ctx->inputs[k].glsl_predefined_no_emit = true;
                     ctx->inputs[k].glsl_no_index = true;
                  }
                  add_two_side = true;
               }
               name_prefix = "ex";
            }
            break;
         }
         /* fallthrough */
      case TGSI_SEMANTIC_PRIMID:
         if (iter->processor.Processor == TGSI_PROCESSOR_GEOMETRY) {
            name_prefix = "gl_PrimitiveIDIn";
            ctx->inputs[i].glsl_predefined_no_emit = true;
            ctx->inputs[i].glsl_no_index = true;
            ctx->inputs[i].override_no_wm = true;
            ctx->shader_req_bits |= SHADER_REQ_INTS;
            break;
         } else if (iter->processor.Processor == TGSI_PROCESSOR_FRAGMENT) {
            name_prefix = "gl_PrimitiveID";
            ctx->inputs[i].glsl_predefined_no_emit = true;
            ctx->inputs[i].glsl_no_index = true;
            require_glsl_ver(ctx, 150);
            break;
         }
         /* fallthrough */
      case TGSI_SEMANTIC_VIEWPORT_INDEX:
         if (iter->processor.Processor == TGSI_PROCESSOR_FRAGMENT) {
            ctx->inputs[i].glsl_predefined_no_emit = true;
            ctx->inputs[i].glsl_no_index = true;
            ctx->inputs[i].is_int = true;
            ctx->inputs[i].override_no_wm = true;
            name_prefix = "gl_ViewportIndex";
            if (ctx->glsl_ver_required >= 140)
               ctx->shader_req_bits |= SHADER_REQ_LAYER;
            break;
         }
         /* fallthrough */
      case TGSI_SEMANTIC_LAYER:
         if (iter->processor.Processor == TGSI_PROCESSOR_FRAGMENT) {
            name_prefix = "gl_Layer";
            ctx->inputs[i].glsl_predefined_no_emit = true;
            ctx->inputs[i].glsl_no_index = true;
            ctx->inputs[i].is_int = true;
            ctx->inputs[i].override_no_wm = true;
            ctx->shader_req_bits |= SHADER_REQ_LAYER;
            break;
         }
         /* fallthrough */
      case TGSI_SEMANTIC_PSIZE:
         if (iter->processor.Processor == TGSI_PROCESSOR_GEOMETRY ||
             iter->processor.Processor == TGSI_PROCESSOR_TESS_CTRL ||
             iter->processor.Processor == TGSI_PROCESSOR_TESS_EVAL) {
            name_prefix = "gl_PointSize";
            ctx->inputs[i].glsl_predefined_no_emit = true;
            ctx->inputs[i].glsl_no_index = true;
            ctx->inputs[i].override_no_wm = true;
            ctx->inputs[i].glsl_gl_block = true;
            ctx->shader_req_bits |= SHADER_REQ_PSIZE;
            break;
         }
         /* fallthrough */
      case TGSI_SEMANTIC_CLIPDIST:
         if (iter->processor.Processor == TGSI_PROCESSOR_GEOMETRY ||
             iter->processor.Processor == TGSI_PROCESSOR_TESS_CTRL ||
             iter->processor.Processor == TGSI_PROCESSOR_TESS_EVAL) {
            name_prefix = "gl_ClipDistance";
            ctx->inputs[i].glsl_predefined_no_emit = true;
            ctx->inputs[i].glsl_no_index = true;
            ctx->inputs[i].glsl_gl_block = true;
            ctx->num_in_clip_dist += 4;
            break;
         } else if (iter->processor.Processor == TGSI_PROCESSOR_FRAGMENT) {
            name_prefix = "gl_ClipDistance";
            ctx->inputs[i].glsl_predefined_no_emit = true;
            ctx->inputs[i].glsl_no_index = true;
            ctx->num_in_clip_dist += 4;
            break;
         }
         /* fallthrough */
      case TGSI_SEMANTIC_POSITION:
         if (iter->processor.Processor == TGSI_PROCESSOR_GEOMETRY ||
             iter->processor.Processor == TGSI_PROCESSOR_TESS_CTRL ||
             iter->processor.Processor == TGSI_PROCESSOR_TESS_EVAL) {
            name_prefix = "gl_Position";
            ctx->inputs[i].glsl_predefined_no_emit = true;
            ctx->inputs[i].glsl_no_index = true;
            ctx->inputs[i].glsl_gl_block = true;
            break;
         } else if (iter->processor.Processor == TGSI_PROCESSOR_FRAGMENT) {
            name_prefix = "gl_FragCoord";
            ctx->inputs[i].glsl_predefined_no_emit = true;
            ctx->inputs[i].glsl_no_index = true;
            break;
         }
         /* fallthrough */
      case TGSI_SEMANTIC_FACE:
         if (iter->processor.Processor == TGSI_PROCESSOR_FRAGMENT) {
            if (ctx->front_face_emitted) {
               ctx->num_inputs--;
               return true;
            }
            name_prefix = "gl_FrontFacing";
            ctx->inputs[i].glsl_predefined_no_emit = true;
            ctx->inputs[i].glsl_no_index = true;
            ctx->front_face_emitted = true;
            break;
         }
         /* fallthrough */
      case TGSI_SEMANTIC_PATCH:
	 if (indirect && ctx->inputs[i].name == TGSI_SEMANTIC_PATCH) {
            ctx->inputs[i].glsl_predefined_no_emit = true;
	    if (ctx->inputs[i].sid < ctx->patch_input_range.first || ctx->patch_input_range.used == false) {
	       ctx->patch_input_range.first = ctx->inputs[i].sid;
	       ctx->patch_input_range.array_id = i;
	       ctx->patch_input_range.used = true;
	    }
	    if (ctx->inputs[i].sid > ctx->patch_input_range.last)
               ctx->patch_input_range.last = ctx->inputs[i].sid;
         }
         /* fallthrough */
      case TGSI_SEMANTIC_GENERIC:
         if (iter->processor.Processor == TGSI_PROCESSOR_FRAGMENT) {
            if (ctx->key->coord_replace & (1 << ctx->inputs[i].sid)) {
               if (ctx->cfg->use_gles)
                  name_prefix = "vec4(gl_PointCoord.x, mix(1.0 - gl_PointCoord.y, gl_PointCoord.y, clamp(winsys_adjust_y, 0.0, 1.0)), 0.0, 1.0)";
               else
                  name_prefix = "vec4(gl_PointCoord, 0.0, 1.0)";
               ctx->inputs[i].glsl_predefined_no_emit = true;
               ctx->inputs[i].glsl_no_index = true;
               break;
            }
         }
         if (indirect && ctx->inputs[i].name == TGSI_SEMANTIC_GENERIC) {
            ctx->inputs[i].glsl_predefined_no_emit = true;
            if (ctx->inputs[i].sid < ctx->generic_input_range.first || ctx->generic_input_range.used == false) {
               ctx->generic_input_range.first = ctx->inputs[i].sid;
               ctx->generic_input_range.array_id = i;
               ctx->generic_input_range.used = true;
            }
            if (ctx->inputs[i].sid > ctx->generic_input_range.last)
               ctx->generic_input_range.last = ctx->inputs[i].sid;
         }
         /* fallthrough */
      default:
         name_prefix = get_stage_input_name_prefix(ctx, iter->processor.Processor);
         break;
      }

      if (ctx->inputs[i].glsl_no_index)
         snprintf(ctx->inputs[i].glsl_name, 128, "%s", name_prefix);
      else {
         if (ctx->inputs[i].name == TGSI_SEMANTIC_FOG)
            snprintf(ctx->inputs[i].glsl_name, 64, "%s_f%d", name_prefix, ctx->inputs[i].sid);
         else if (ctx->inputs[i].name == TGSI_SEMANTIC_COLOR)
            snprintf(ctx->inputs[i].glsl_name, 64, "%s_c%d", name_prefix, ctx->inputs[i].sid);
         else if (ctx->inputs[i].name == TGSI_SEMANTIC_GENERIC)
            snprintf(ctx->inputs[i].glsl_name, 64, "%s_g%d", name_prefix, ctx->inputs[i].sid);
         else if (ctx->inputs[i].name == TGSI_SEMANTIC_PATCH)
            snprintf(ctx->inputs[i].glsl_name, 64, "%s_p%d", name_prefix, ctx->inputs[i].sid);
         else
            snprintf(ctx->inputs[i].glsl_name, 64, "%s_%d", name_prefix, ctx->inputs[i].first);
      }
      if (add_two_side) {
         snprintf(ctx->inputs[i + 1].glsl_name, 64, "%s_bc%d", name_prefix, ctx->inputs[i + 1].sid);
         if (!ctx->front_face_emitted) {
            snprintf(ctx->inputs[i + 2].glsl_name, 64, "%s", "gl_FrontFacing");
            ctx->front_face_emitted = true;
         }
      }
      break;
   case TGSI_FILE_OUTPUT:
      for (uint32_t j = 0; j < ctx->num_outputs; j++) {
         if (ctx->outputs[j].name == decl->Semantic.Name &&
             ctx->outputs[j].sid == decl->Semantic.Index &&
             ctx->outputs[j].first == decl->Range.First)
            return true;
      }
      i = ctx->num_outputs++;
      indirect = ctx_indirect_outputs(ctx);
      if (ctx->num_outputs > ARRAY_SIZE(ctx->outputs)) {
         fprintf(stderr, "Number of outputs exceeded, max is %lu\n", ARRAY_SIZE(ctx->outputs));
         return false;
      }

      ctx->outputs[i].name = decl->Semantic.Name;
      ctx->outputs[i].sid = decl->Semantic.Index;
      ctx->outputs[i].interpolate = decl->Interp.Interpolate;
      ctx->outputs[i].invariant = decl->Declaration.Invariant;
      ctx->outputs[i].precise = false;
      ctx->outputs[i].first = decl->Range.First;
      ctx->outputs[i].glsl_predefined_no_emit = false;
      ctx->outputs[i].glsl_no_index = false;
      ctx->outputs[i].override_no_wm = false;
      ctx->outputs[i].is_int = false;
      ctx->outputs[i].fbfetch_used = false;

      switch (ctx->outputs[i].name) {
      case TGSI_SEMANTIC_POSITION:
         if (iter->processor.Processor == TGSI_PROCESSOR_VERTEX ||
             iter->processor.Processor == TGSI_PROCESSOR_GEOMETRY ||
             iter->processor.Processor == TGSI_PROCESSOR_TESS_CTRL ||
             iter->processor.Processor == TGSI_PROCESSOR_TESS_EVAL) {
            if (ctx->outputs[i].first > 0)
               fprintf(stderr,"Illegal position input\n");
            name_prefix = "gl_Position";
            ctx->outputs[i].glsl_predefined_no_emit = true;
            ctx->outputs[i].glsl_no_index = true;
            if (iter->processor.Processor == TGSI_PROCESSOR_TESS_CTRL)
               ctx->outputs[i].glsl_gl_block = true;
         } else if (iter->processor.Processor == TGSI_PROCESSOR_FRAGMENT) {
            name_prefix = "gl_FragDepth";
            ctx->outputs[i].glsl_predefined_no_emit = true;
            ctx->outputs[i].glsl_no_index = true;
            ctx->outputs[i].override_no_wm = true;
         }
         break;
      case TGSI_SEMANTIC_STENCIL:
         if (iter->processor.Processor == TGSI_PROCESSOR_FRAGMENT) {
            name_prefix = "gl_FragStencilRefARB";
            ctx->outputs[i].glsl_predefined_no_emit = true;
            ctx->outputs[i].glsl_no_index = true;
            ctx->outputs[i].override_no_wm = true;
            ctx->outputs[i].is_int = true;
            ctx->shader_req_bits |= (SHADER_REQ_INTS | SHADER_REQ_STENCIL_EXPORT);
         }
         break;
      case TGSI_SEMANTIC_CLIPDIST:
         name_prefix = "gl_ClipDistance";
         ctx->outputs[i].glsl_predefined_no_emit = true;
         ctx->outputs[i].glsl_no_index = true;
         ctx->num_clip_dist += 4;
         if (iter->processor.Processor == TGSI_PROCESSOR_VERTEX &&
             (ctx->key->gs_present || ctx->key->tcs_present))
            require_glsl_ver(ctx, 150);
         if (iter->processor.Processor == TGSI_PROCESSOR_TESS_CTRL)
            ctx->outputs[i].glsl_gl_block = true;
         break;
      case TGSI_SEMANTIC_CLIPVERTEX:
         name_prefix = "gl_ClipVertex";
         ctx->outputs[i].glsl_predefined_no_emit = true;
         ctx->outputs[i].glsl_no_index = true;
         ctx->outputs[i].override_no_wm = true;
         if (ctx->glsl_ver_required >= 140)
            ctx->has_clipvertex = true;
         break;
      case TGSI_SEMANTIC_SAMPLEMASK:
         if (iter->processor.Processor == TGSI_PROCESSOR_FRAGMENT) {
            ctx->outputs[i].glsl_predefined_no_emit = true;
            ctx->outputs[i].glsl_no_index = true;
            ctx->outputs[i].override_no_wm = true;
            ctx->outputs[i].is_int = true;
            ctx->shader_req_bits |= (SHADER_REQ_INTS | SHADER_REQ_SAMPLE_SHADING);
            name_prefix = "gl_SampleMask";
            break;
         }
         break;
      case TGSI_SEMANTIC_COLOR:
         if (iter->processor.Processor == TGSI_PROCESSOR_VERTEX) {
            if (ctx->glsl_ver_required < 140) {
               ctx->outputs[i].glsl_no_index = true;
               if (ctx->outputs[i].sid == 0)
                  name_prefix = "gl_FrontColor";
               else if (ctx->outputs[i].sid == 1)
                  name_prefix = "gl_FrontSecondaryColor";
            } else
               name_prefix = "ex";
            break;
         }
         /* fallthrough */
      case TGSI_SEMANTIC_BCOLOR:
         if (iter->processor.Processor == TGSI_PROCESSOR_VERTEX) {
            if (ctx->glsl_ver_required < 140) {
               ctx->outputs[i].glsl_no_index = true;
               if (ctx->outputs[i].sid == 0)
                  name_prefix = "gl_BackColor";
               else if (ctx->outputs[i].sid == 1)
                  name_prefix = "gl_BackSecondaryColor";
               break;
            } else
               name_prefix = "ex";
            break;
         }
         /* fallthrough */
      case TGSI_SEMANTIC_PSIZE:
         if (iter->processor.Processor == TGSI_PROCESSOR_VERTEX ||
             iter->processor.Processor == TGSI_PROCESSOR_GEOMETRY ||
             iter->processor.Processor == TGSI_PROCESSOR_TESS_CTRL ||
             iter->processor.Processor == TGSI_PROCESSOR_TESS_EVAL) {
            ctx->outputs[i].glsl_predefined_no_emit = true;
            ctx->outputs[i].glsl_no_index = true;
            ctx->outputs[i].override_no_wm = true;
            ctx->shader_req_bits |= SHADER_REQ_PSIZE;
            name_prefix = "gl_PointSize";
            if (iter->processor.Processor == TGSI_PROCESSOR_TESS_CTRL)
               ctx->outputs[i].glsl_gl_block = true;
            break;
         }
         /* fallthrough */
      case TGSI_SEMANTIC_LAYER:
         if (iter->processor.Processor == TGSI_PROCESSOR_GEOMETRY) {
            ctx->outputs[i].glsl_predefined_no_emit = true;
            ctx->outputs[i].glsl_no_index = true;
            ctx->outputs[i].override_no_wm = true;
            ctx->outputs[i].is_int = true;
            name_prefix = "gl_Layer";
            break;
         }
         /* fallthrough */
      case TGSI_SEMANTIC_PRIMID:
         if (iter->processor.Processor == TGSI_PROCESSOR_GEOMETRY) {
            ctx->outputs[i].glsl_predefined_no_emit = true;
            ctx->outputs[i].glsl_no_index = true;
            ctx->outputs[i].override_no_wm = true;
            ctx->outputs[i].is_int = true;
            name_prefix = "gl_PrimitiveID";
            break;
         }
         /* fallthrough */
      case TGSI_SEMANTIC_VIEWPORT_INDEX:
         if (iter->processor.Processor == TGSI_PROCESSOR_GEOMETRY) {
            ctx->outputs[i].glsl_predefined_no_emit = true;
            ctx->outputs[i].glsl_no_index = true;
            ctx->outputs[i].override_no_wm = true;
            ctx->outputs[i].is_int = true;
            name_prefix = "gl_ViewportIndex";
            if (ctx->glsl_ver_required >= 140)
               ctx->shader_req_bits |= SHADER_REQ_VIEWPORT_IDX;
            break;
         }
         /* fallthrough */
      case TGSI_SEMANTIC_TESSOUTER:
         if (iter->processor.Processor == TGSI_PROCESSOR_TESS_CTRL) {
            ctx->outputs[i].glsl_predefined_no_emit = true;
            ctx->outputs[i].glsl_no_index = true;
            ctx->outputs[i].override_no_wm = true;
            name_prefix = "gl_TessLevelOuter";
            break;
         }
         /* fallthrough */
      case TGSI_SEMANTIC_TESSINNER:
         if (iter->processor.Processor == TGSI_PROCESSOR_TESS_CTRL) {
            ctx->outputs[i].glsl_predefined_no_emit = true;
            ctx->outputs[i].glsl_no_index = true;
            ctx->outputs[i].override_no_wm = true;
            name_prefix = "gl_TessLevelInner";
            break;
         }
         /* fallthrough */
      case TGSI_SEMANTIC_GENERIC:
         if (iter->processor.Processor == TGSI_PROCESSOR_VERTEX)
            if (ctx->outputs[i].name == TGSI_SEMANTIC_GENERIC)
               color_offset = -1;
         if (indirect && ctx->outputs[i].name == TGSI_SEMANTIC_GENERIC) {
            ctx->outputs[i].glsl_predefined_no_emit = true;
            require_glsl_ver(ctx, 150);
            if (ctx->outputs[i].sid < ctx->generic_output_range.first || ctx->generic_output_range.used == false) {
               ctx->generic_output_range.array_id = i;
               ctx->generic_output_range.first = ctx->outputs[i].sid;
               ctx->generic_output_range.used = true;
            }
            if (ctx->outputs[i].sid > ctx->generic_output_range.last)
               ctx->generic_output_range.last = ctx->outputs[i].sid;
         }
         /* fallthrough */
      case TGSI_SEMANTIC_PATCH:
         if (indirect && ctx->outputs[i].name == TGSI_SEMANTIC_PATCH) {
            ctx->outputs[i].glsl_predefined_no_emit = true;
            require_glsl_ver(ctx, 150);
            if (ctx->outputs[i].sid < ctx->patch_output_range.first || ctx->patch_output_range.used == false) {
               ctx->patch_output_range.array_id = i;
               ctx->patch_output_range.first = ctx->outputs[i].sid;
               ctx->patch_output_range.used = true;
            }
            if (ctx->outputs[i].sid > ctx->patch_output_range.last)
               ctx->patch_output_range.last = ctx->outputs[i].sid;
         }
         /* fallthrough */
      default:
         name_prefix = get_stage_output_name_prefix(iter->processor.Processor);
         break;
      }

      if (ctx->outputs[i].glsl_no_index)
         snprintf(ctx->outputs[i].glsl_name, 64, "%s", name_prefix);
      else {
         if (ctx->outputs[i].name == TGSI_SEMANTIC_FOG)
            snprintf(ctx->outputs[i].glsl_name, 64, "%s_f%d", name_prefix, ctx->outputs[i].sid);
         else if (ctx->outputs[i].name == TGSI_SEMANTIC_COLOR)
            snprintf(ctx->outputs[i].glsl_name, 64, "%s_c%d", name_prefix, ctx->outputs[i].sid);
         else if (ctx->outputs[i].name == TGSI_SEMANTIC_BCOLOR)
            snprintf(ctx->outputs[i].glsl_name, 64, "%s_bc%d", name_prefix, ctx->outputs[i].sid);
         else if (ctx->outputs[i].name == TGSI_SEMANTIC_PATCH)
            snprintf(ctx->outputs[i].glsl_name, 64, "%s_p%d", name_prefix, ctx->outputs[i].sid);
         else if (ctx->outputs[i].name == TGSI_SEMANTIC_GENERIC)
            snprintf(ctx->outputs[i].glsl_name, 64, "%s_g%d", name_prefix, ctx->outputs[i].sid);
         else
            snprintf(ctx->outputs[i].glsl_name, 64, "%s_%d", name_prefix, ctx->outputs[i].first + color_offset);

      }
      break;
   case TGSI_FILE_TEMPORARY:
      if (!allocate_temp_range(ctx, decl->Range.First, decl->Range.Last,
                               decl->Array.ArrayID))
         return false;
      break;
   case TGSI_FILE_SAMPLER:
      ctx->samplers_used |= (1 << decl->Range.Last);
      break;
   case TGSI_FILE_SAMPLER_VIEW:
      if (decl->Range.Last >= ARRAY_SIZE(ctx->samplers)) {
         fprintf(stderr, "Sampler view exceeded, max is %lu\n", ARRAY_SIZE(ctx->samplers));
         return false;
      }
      if (!add_samplers(ctx, decl->Range.First, decl->Range.Last, decl->SamplerView.Resource, decl->SamplerView.ReturnTypeX))
         return false;
      break;
   case TGSI_FILE_IMAGE:
      ctx->shader_req_bits |= SHADER_REQ_IMAGE_LOAD_STORE;
      if (decl->Range.Last >= ARRAY_SIZE(ctx->images)) {
         fprintf(stderr, "Image view exceeded, max is %lu\n", ARRAY_SIZE(ctx->images));
         return false;
      }
      if (!add_images(ctx, decl->Range.First, decl->Range.Last, &decl->Image))
         return false;
      break;
   case TGSI_FILE_BUFFER:
      if (decl->Range.First >= 32) {
         fprintf(stderr, "Buffer view exceeded, max is 32\n");
         return false;
      }
      ctx->ssbo_used_mask |= (1 << decl->Range.First);
      if (decl->Declaration.Atomic) {
         if (decl->Range.First < ctx->ssbo_atomic_array_base)
            ctx->ssbo_atomic_array_base = decl->Range.First;
         ctx->ssbo_atomic_mask |= (1 << decl->Range.First);
      } else {
         if (decl->Range.First < ctx->ssbo_array_base)
            ctx->ssbo_array_base = decl->Range.First;
      }
      break;
   case TGSI_FILE_CONSTANT:
      if (decl->Declaration.Dimension && decl->Dim.Index2D != 0) {
         if (decl->Dim.Index2D > 31) {
            fprintf(stderr, "Number of uniforms exceeded, max is 32\n");
            return false;
         }
         if (ctx->ubo_used_mask & (1 << decl->Dim.Index2D)) {
            fprintf(stderr, "UBO #%d is already defined\n", decl->Dim.Index2D);
            return false;
         }
         ctx->ubo_used_mask |= (1 << decl->Dim.Index2D);
         ctx->ubo_sizes[decl->Dim.Index2D] = decl->Range.Last + 1;
      } else {
         /* if we have a normal single const set then ubo base should be 1 */
         ctx->ubo_base = 1;
         if (decl->Range.Last) {
            if (decl->Range.Last + 1 > ctx->num_consts)
               ctx->num_consts = decl->Range.Last + 1;
         } else
            ctx->num_consts++;
      }
      break;
   case TGSI_FILE_ADDRESS:
      ctx->num_address = decl->Range.Last + 1;
      break;
   case TGSI_FILE_SYSTEM_VALUE:
      i = ctx->num_system_values++;
      if (ctx->num_system_values > ARRAY_SIZE(ctx->system_values)) {
         fprintf(stderr, "Number of system values exceeded, max is %lu\n", ARRAY_SIZE(ctx->system_values));
         return false;
      }

      ctx->system_values[i].name = decl->Semantic.Name;
      ctx->system_values[i].sid = decl->Semantic.Index;
      ctx->system_values[i].glsl_predefined_no_emit = true;
      ctx->system_values[i].glsl_no_index = true;
      ctx->system_values[i].override_no_wm = true;
      ctx->system_values[i].first = decl->Range.First;
      if (decl->Semantic.Name == TGSI_SEMANTIC_INSTANCEID) {
         name_prefix = "gl_InstanceID";
         ctx->shader_req_bits |= SHADER_REQ_INSTANCE_ID | SHADER_REQ_INTS;
      } else if (decl->Semantic.Name == TGSI_SEMANTIC_VERTEXID) {
         name_prefix = "gl_VertexID";
         ctx->shader_req_bits |= SHADER_REQ_INTS;
      } else if (decl->Semantic.Name == TGSI_SEMANTIC_HELPER_INVOCATION) {
         name_prefix = "gl_HelperInvocation";
         ctx->shader_req_bits |= SHADER_REQ_ES31_COMPAT;
      } else if (decl->Semantic.Name == TGSI_SEMANTIC_SAMPLEID) {
         name_prefix = "gl_SampleID";
         ctx->shader_req_bits |= (SHADER_REQ_SAMPLE_SHADING | SHADER_REQ_INTS);
      } else if (decl->Semantic.Name == TGSI_SEMANTIC_SAMPLEPOS) {
         name_prefix = "gl_SamplePosition";
         ctx->shader_req_bits |= SHADER_REQ_SAMPLE_SHADING;
      } else if (decl->Semantic.Name == TGSI_SEMANTIC_INVOCATIONID) {
         name_prefix = "gl_InvocationID";
         ctx->shader_req_bits |= (SHADER_REQ_INTS | SHADER_REQ_GPU_SHADER5);
      } else if (decl->Semantic.Name == TGSI_SEMANTIC_SAMPLEMASK) {
         name_prefix = "gl_SampleMaskIn[0]";
         ctx->shader_req_bits |= (SHADER_REQ_INTS | SHADER_REQ_GPU_SHADER5);
      } else if (decl->Semantic.Name == TGSI_SEMANTIC_PRIMID) {
         name_prefix = "gl_PrimitiveID";
         ctx->shader_req_bits |= (SHADER_REQ_INTS | SHADER_REQ_GPU_SHADER5);
      } else if (decl->Semantic.Name == TGSI_SEMANTIC_TESSCOORD) {
         name_prefix = "gl_TessCoord";
         ctx->system_values[i].override_no_wm = false;
      } else if (decl->Semantic.Name == TGSI_SEMANTIC_VERTICESIN) {
         ctx->shader_req_bits |= SHADER_REQ_INTS;
         name_prefix = "gl_PatchVerticesIn";
      } else if (decl->Semantic.Name == TGSI_SEMANTIC_TESSOUTER) {
         name_prefix = "gl_TessLevelOuter";
      } else if (decl->Semantic.Name == TGSI_SEMANTIC_TESSINNER) {
         name_prefix = "gl_TessLevelInner";
      } else if (decl->Semantic.Name == TGSI_SEMANTIC_THREAD_ID) {
         name_prefix = "gl_LocalInvocationID";
         ctx->system_values[i].override_no_wm = false;
      } else if (decl->Semantic.Name == TGSI_SEMANTIC_BLOCK_ID) {
         name_prefix = "gl_WorkGroupID";
         ctx->system_values[i].override_no_wm = false;
      } else if (decl->Semantic.Name == TGSI_SEMANTIC_GRID_SIZE) {
         name_prefix = "gl_NumWorkGroups";
         ctx->system_values[i].override_no_wm = false;
      } else {
         fprintf(stderr, "unsupported system value %d\n", decl->Semantic.Name);
         name_prefix = "unknown";
      }
      snprintf(ctx->system_values[i].glsl_name, 64, "%s", name_prefix);
      break;
   case TGSI_FILE_MEMORY:
      ctx->has_file_memory = true;
      break;
   case TGSI_FILE_HW_ATOMIC:
      if (ctx->num_abo >= ARRAY_SIZE(ctx->abo_idx)) {
         fprintf(stderr, "Number of atomic counter buffers exceeded, max is %lu\n", ARRAY_SIZE(ctx->abo_idx));
         return false;
      }
      ctx->abo_idx[ctx->num_abo] = decl->Dim.Index2D;
      ctx->abo_sizes[ctx->num_abo] = decl->Range.Last - decl->Range.First + 1;
      ctx->abo_offsets[ctx->num_abo] = decl->Range.First;
      ctx->num_abo++;
      break;
   default:
      fprintf(stderr,"unsupported file %d declaration\n", decl->Declaration.File);
      break;
   }

   return true;
}

static boolean
iter_property(struct tgsi_iterate_context *iter,
              struct tgsi_full_property *prop)
{
   struct dump_ctx *ctx = (struct dump_ctx *) iter;

   if (prop->Property.PropertyName == TGSI_PROPERTY_FS_COLOR0_WRITES_ALL_CBUFS) {
      if (prop->u[0].Data == 1)
         ctx->write_all_cbufs = true;
   }

   if (prop->Property.PropertyName == TGSI_PROPERTY_FS_COORD_ORIGIN) {
      ctx->fs_coord_origin = prop->u[0].Data;
   }

   if (prop->Property.PropertyName == TGSI_PROPERTY_FS_COORD_PIXEL_CENTER) {
      ctx->fs_pixel_center = prop->u[0].Data;
   }

   if (prop->Property.PropertyName == TGSI_PROPERTY_GS_INPUT_PRIM) {
      ctx->gs_in_prim = prop->u[0].Data;
   }

   if (prop->Property.PropertyName == TGSI_PROPERTY_GS_OUTPUT_PRIM) {
      ctx->gs_out_prim = prop->u[0].Data;
   }

   if (prop->Property.PropertyName == TGSI_PROPERTY_GS_MAX_OUTPUT_VERTICES) {
      ctx->gs_max_out_verts = prop->u[0].Data;
   }

   if (prop->Property.PropertyName == TGSI_PROPERTY_GS_INVOCATIONS) {
      ctx->gs_num_invocations = prop->u[0].Data;
   }

   if (prop->Property.PropertyName == TGSI_PROPERTY_NUM_CLIPDIST_ENABLED) {
      ctx->num_clip_dist_prop = prop->u[0].Data;
   }

   if (prop->Property.PropertyName == TGSI_PROPERTY_NUM_CULLDIST_ENABLED) {
      ctx->num_cull_dist_prop = prop->u[0].Data;
   }

   if (prop->Property.PropertyName == TGSI_PROPERTY_TCS_VERTICES_OUT) {
      ctx->tcs_vertices_out = prop->u[0].Data;
   }

   if (prop->Property.PropertyName == TGSI_PROPERTY_TES_PRIM_MODE) {
      ctx->tes_prim_mode = prop->u[0].Data;
   }

   if (prop->Property.PropertyName == TGSI_PROPERTY_TES_SPACING) {
      ctx->tes_spacing = prop->u[0].Data;
   }

   if (prop->Property.PropertyName == TGSI_PROPERTY_TES_VERTEX_ORDER_CW) {
      ctx->tes_vertex_order = prop->u[0].Data;
   }

   if (prop->Property.PropertyName == TGSI_PROPERTY_TES_POINT_MODE) {
      ctx->tes_point_mode = prop->u[0].Data;
   }

   if (prop->Property.PropertyName == TGSI_PROPERTY_FS_EARLY_DEPTH_STENCIL) {
      ctx->early_depth_stencil = prop->u[0].Data > 0;
      if (ctx->early_depth_stencil) {
         require_glsl_ver(ctx, 150);
         ctx->shader_req_bits |= SHADER_REQ_IMAGE_LOAD_STORE;
      }
   }

   if (prop->Property.PropertyName == TGSI_PROPERTY_CS_FIXED_BLOCK_WIDTH)
      ctx->local_cs_block_size[0] = prop->u[0].Data;
   if (prop->Property.PropertyName == TGSI_PROPERTY_CS_FIXED_BLOCK_HEIGHT)
      ctx->local_cs_block_size[1] = prop->u[0].Data;
   if (prop->Property.PropertyName == TGSI_PROPERTY_CS_FIXED_BLOCK_DEPTH)
      ctx->local_cs_block_size[2] = prop->u[0].Data;
   return true;
}

static boolean
iter_immediate(
   struct tgsi_iterate_context *iter,
   struct tgsi_full_immediate *imm )
{
   struct dump_ctx *ctx = (struct dump_ctx *) iter;
   int i;
   uint32_t first = ctx->num_imm;

   if (first >= ARRAY_SIZE(ctx->imm)) {
      fprintf(stderr, "Number of immediates exceeded, max is: %lu\n", ARRAY_SIZE(ctx->imm));
      return false;
   }

   ctx->imm[first].type = imm->Immediate.DataType;
   for (i = 0; i < 4; i++) {
      if (imm->Immediate.DataType == TGSI_IMM_FLOAT32) {
         ctx->imm[first].val[i].f = imm->u[i].Float;
      } else if (imm->Immediate.DataType == TGSI_IMM_UINT32 ||
                 imm->Immediate.DataType == TGSI_IMM_FLOAT64) {
         ctx->shader_req_bits |= SHADER_REQ_INTS;
         ctx->imm[first].val[i].ui = imm->u[i].Uint;
      } else if (imm->Immediate.DataType == TGSI_IMM_INT32) {
         ctx->shader_req_bits |= SHADER_REQ_INTS;
         ctx->imm[first].val[i].i = imm->u[i].Int;
      }
   }
   ctx->num_imm++;
   return true;
}

static char get_swiz_char(int swiz)
{
   switch(swiz){
   case TGSI_SWIZZLE_X: return 'x';
   case TGSI_SWIZZLE_Y: return 'y';
   case TGSI_SWIZZLE_Z: return 'z';
   case TGSI_SWIZZLE_W: return 'w';
   default: return 0;
   }
}

static void emit_cbuf_writes(struct dump_ctx *ctx)
{
   int i;

   for (i = ctx->num_outputs; i < ctx->cfg->max_draw_buffers; i++) {
      emit_buff(ctx, "fsout_c%d = fsout_c0;\n", i);
   }
}

static void emit_a8_swizzle(struct dump_ctx *ctx)
{
   emit_buf(ctx, "fsout_c0.x = fsout_c0.w;\n");
}

static const char *atests[PIPE_FUNC_ALWAYS + 1] = {
   "false",
   "<",
   "==",
   "<=",
   ">",
   "!=",
   ">=",
   "true"
};

static void emit_alpha_test(struct dump_ctx *ctx)
{
   char comp_buf[128];

   if (!ctx->num_outputs)
      return;

   if (!ctx->write_all_cbufs) {
      /* only emit alpha stanza if first output is 0 */
      if (ctx->outputs[0].sid != 0)
         return;
   }
   switch (ctx->key->alpha_test) {
   case PIPE_FUNC_NEVER:
   case PIPE_FUNC_ALWAYS:
      snprintf(comp_buf, 128, "%s", atests[ctx->key->alpha_test]);
      break;
   case PIPE_FUNC_LESS:
   case PIPE_FUNC_EQUAL:
   case PIPE_FUNC_LEQUAL:
   case PIPE_FUNC_GREATER:
   case PIPE_FUNC_NOTEQUAL:
   case PIPE_FUNC_GEQUAL:
      snprintf(comp_buf, 128, "%s %s %f", "fsout_c0.w", atests[ctx->key->alpha_test], ctx->key->alpha_ref_val);
      break;
   default:
      fprintf(stderr, "invalid alpha-test: %x\n", ctx->key->alpha_test);
      set_buf_error(ctx);
      return;
   }

   emit_buff(ctx, "if (!(%s)) {\n\tdiscard;\n}\n", comp_buf);
}

static void emit_pstipple_pass(struct dump_ctx *ctx)
{
   emit_buf(ctx, "stip_temp = texture(pstipple_sampler, vec2(gl_FragCoord.x / 32, gl_FragCoord.y / 32)).x;\n");
   emit_buf(ctx, "if (stip_temp > 0) {\n\tdiscard;\n}\n");
}

static void emit_color_select(struct dump_ctx *ctx)
{
   if (!ctx->key->color_two_side || !(ctx->color_in_mask & 0x3))
      return;

   if (ctx->color_in_mask & 1)
      emit_buf(ctx, "realcolor0 = gl_FrontFacing ? ex_c0 : ex_bc0;\n");

   if (ctx->color_in_mask & 2)
      emit_buf(ctx, "realcolor1 = gl_FrontFacing ? ex_c1 : ex_bc1;\n");
}

static void emit_prescale(struct dump_ctx *ctx)
{
   emit_buf(ctx, "gl_Position.y = gl_Position.y * winsys_adjust_y;\n");
}

static void prepare_so_movs(struct dump_ctx *ctx)
{
   uint32_t i;
   for (i = 0; i < ctx->so->num_outputs; i++) {
      ctx->write_so_outputs[i] = true;
      if (ctx->so->output[i].start_component != 0)
         continue;
      if (ctx->so->output[i].num_components != 4)
         continue;
      if (ctx->outputs[ctx->so->output[i].register_index].name == TGSI_SEMANTIC_CLIPDIST)
         continue;
      if (ctx->outputs[ctx->so->output[i].register_index].name == TGSI_SEMANTIC_POSITION)
         continue;

      ctx->outputs[ctx->so->output[i].register_index].stream = ctx->so->output[i].stream;
      if (ctx->prog_type == TGSI_PROCESSOR_GEOMETRY && ctx->so->output[i].stream)
         ctx->shader_req_bits |= SHADER_REQ_GPU_SHADER5;

      ctx->write_so_outputs[i] = false;
   }
}

static void emit_so_movs(struct dump_ctx *ctx)
{
   uint32_t i, j;
   char outtype[15] = {0};
   char writemask[6];

   if (ctx->so->num_outputs >= PIPE_MAX_SO_OUTPUTS) {
      fprintf(stderr, "Num outputs exceeded, max is %u\n", PIPE_MAX_SO_OUTPUTS);
      set_buf_error(ctx);
      return;
   }

   for (i = 0; i < ctx->so->num_outputs; i++) {
      if (ctx->so->output[i].start_component != 0) {
         int wm_idx = 0;
         writemask[wm_idx++] = '.';
         for (j = 0; j < ctx->so->output[i].num_components; j++) {
            unsigned idx = ctx->so->output[i].start_component + j;
            if (idx >= 4)
               break;
            if (idx <= 2)
               writemask[wm_idx++] = 'x' + idx;
            else
               writemask[wm_idx++] = 'w';
         }
         writemask[wm_idx] = '\0';
      } else
         writemask[0] = 0;

      if (!ctx->write_so_outputs[i]) {
         if (ctx->so_names[i])
            free(ctx->so_names[i]);
         if (ctx->so->output[i].register_index > ctx->num_outputs)
            ctx->so_names[i] = NULL;
         else if (ctx->outputs[ctx->so->output[i].register_index].name == TGSI_SEMANTIC_CLIPVERTEX && ctx->has_clipvertex) {
            ctx->so_names[i] = strdup("clipv_tmp");
            ctx->has_clipvertex_so = true;
         } else {
            ctx->so_names[i] = strdup(ctx->outputs[ctx->so->output[i].register_index].glsl_name);
         }
      } else {
         char ntemp[8];
         snprintf(ntemp, 8, "tfout%d", i);
         ctx->so_names[i] = strdup(ntemp);
      }
      if (ctx->so->output[i].num_components == 1) {
         if (ctx->outputs[ctx->so->output[i].register_index].is_int)
            snprintf(outtype, 15, "intBitsToFloat");
         else
            snprintf(outtype, 15, "float");
      } else
         snprintf(outtype, 15, "vec%d", ctx->so->output[i].num_components);

      if (ctx->so->output[i].register_index >= 255)
         continue;

      if (ctx->outputs[ctx->so->output[i].register_index].name == TGSI_SEMANTIC_CLIPDIST) {
         emit_buff(ctx, "tfout%d = %s(clip_dist_temp[%d]%s);\n", i, outtype, ctx->outputs[ctx->so->output[i].register_index].sid,
                  writemask);
      } else {
         if (ctx->write_so_outputs[i])
            emit_buff(ctx, "tfout%d = %s(%s%s);\n", i, outtype, ctx->outputs[ctx->so->output[i].register_index].glsl_name, writemask);
      }
   }
}

static void emit_clip_dist_movs(struct dump_ctx *ctx)
{
   int i;
   bool has_prop = (ctx->num_clip_dist_prop + ctx->num_cull_dist_prop) > 0;
   int ndists;
   const char *prefix="";

   if (ctx->prog_type == PIPE_SHADER_TESS_CTRL)
      prefix = "gl_out[gl_InvocationID].";
   if (ctx->num_clip_dist == 0 && ctx->key->clip_plane_enable) {
      for (i = 0; i < 8; i++) {
         emit_buff(ctx, "%sgl_ClipDistance[%d] = dot(%s, clipp[%d]);\n", prefix, i, ctx->has_clipvertex ? "clipv_tmp" : "gl_Position", i);
      }
      return;
   }
   ndists = ctx->num_clip_dist;
   if (has_prop)
      ndists = ctx->num_clip_dist_prop + ctx->num_cull_dist_prop;
   for (i = 0; i < ndists; i++) {
      int clipidx = i < 4 ? 0 : 1;
      char swiz = i & 3;
      char wm = 0;
      switch (swiz) {
      default:
      case 0: wm = 'x'; break;
      case 1: wm = 'y'; break;
      case 2: wm = 'z'; break;
      case 3: wm = 'w'; break;
      }
      bool is_cull = false;
      if (has_prop) {
         if (i >= ctx->num_clip_dist_prop && i < ctx->num_clip_dist_prop + ctx->num_cull_dist_prop)
            is_cull = true;
      }
      const char *clip_cull = is_cull ? "Cull" : "Clip";
      emit_buff(ctx, "%sgl_%sDistance[%d] = clip_dist_temp[%d].%c;\n", prefix, clip_cull,
               is_cull ? i - ctx->num_clip_dist_prop : i, clipidx, wm);
   }
}

#define emit_arit_op2(op) emit_buff(ctx, "%s = %s(%s((%s %s %s))%s);\n", dsts[0], get_string(dinfo.dstconv), get_string(dinfo.dtypeprefix), srcs[0], op, srcs[1], writemask)
#define emit_op1(op) emit_buff(ctx, "%s = %s(%s(%s(%s))%s);\n", dsts[0], get_string(dinfo.dstconv), get_string(dinfo.dtypeprefix), op, srcs[0], writemask)
#define emit_compare(op) emit_buff(ctx, "%s = %s(%s((%s(%s(%s), %s(%s))))%s);\n", dsts[0], get_string(dinfo.dstconv), get_string(dinfo.dtypeprefix), op, get_string(sinfo.svec4), srcs[0], get_string(sinfo.svec4), srcs[1], writemask)

#define emit_ucompare(op) emit_buff(ctx, "%s = %s(uintBitsToFloat(%s(%s(%s(%s), %s(%s))%s) * %s(0xffffffff)));\n", dsts[0], get_string(dinfo.dstconv), get_string(dinfo.udstconv), op, get_string(sinfo.svec4), srcs[0], get_string(sinfo.svec4), srcs[1], writemask, get_string(dinfo.udstconv))

static void handle_vertex_proc_exit(struct dump_ctx *ctx)
{
    if (ctx->so && !ctx->key->gs_present && !ctx->key->tes_present)
       emit_so_movs(ctx);

    emit_clip_dist_movs(ctx);

    if (!ctx->key->gs_present && !ctx->key->tes_present)
       emit_prescale(ctx);
}

static void handle_fragment_proc_exit(struct dump_ctx *ctx)
{
    if (ctx->key->pstipple_tex)
       emit_pstipple_pass(ctx);

    if (ctx->key->cbufs_are_a8_bitmask)
       emit_a8_swizzle(ctx);

    if (ctx->key->add_alpha_test)
       emit_alpha_test(ctx);

    if (ctx->write_all_cbufs)
       emit_cbuf_writes(ctx);
}

static void set_texture_reqs(struct dump_ctx *ctx,
			     struct tgsi_full_instruction *inst,
			     uint32_t sreg_index,
			     bool *is_shad)
{
   if (sreg_index >= ARRAY_SIZE(ctx->samplers)) {
      fprintf(stderr, "Sampler view exceeded, max is %lu\n", ARRAY_SIZE(ctx->samplers));
      set_buf_error(ctx);
      return;
   }
   ctx->samplers[sreg_index].tgsi_sampler_type = inst->Texture.Texture;

   switch (inst->Texture.Texture) {
   case TGSI_TEXTURE_1D:
   case TGSI_TEXTURE_2D:
   case TGSI_TEXTURE_3D:
   case TGSI_TEXTURE_CUBE:
   case TGSI_TEXTURE_1D_ARRAY:
   case TGSI_TEXTURE_2D_ARRAY:
      break;
   case TGSI_TEXTURE_SHADOWCUBE_ARRAY:
      *is_shad = true;
      /* fallthrough */
   case TGSI_TEXTURE_CUBE_ARRAY:
      ctx->shader_req_bits |= SHADER_REQ_CUBE_ARRAY;
      break;
   case TGSI_TEXTURE_2D_MSAA:
   case TGSI_TEXTURE_2D_ARRAY_MSAA:
      ctx->shader_req_bits |= SHADER_REQ_SAMPLER_MS;
      break;
   case TGSI_TEXTURE_BUFFER:
      ctx->uses_sampler_buf = true;
      break;
   case TGSI_TEXTURE_SHADOWRECT:
      *is_shad = true;
      /* fallthrough */
   case TGSI_TEXTURE_RECT:
      ctx->shader_req_bits |= SHADER_REQ_SAMPLER_RECT;
      break;
   case TGSI_TEXTURE_SHADOW1D:
   case TGSI_TEXTURE_SHADOW2D:
   case TGSI_TEXTURE_SHADOWCUBE:
   case TGSI_TEXTURE_SHADOW1D_ARRAY:
   case TGSI_TEXTURE_SHADOW2D_ARRAY:
      *is_shad = true;
      break;
   default:
      fprintf(stderr, "unhandled texture: %x\n", inst->Texture.Texture);
      set_buf_error(ctx);
      return;
   }

   if (ctx->cfg->glsl_version >= 140)
      if ((ctx->shader_req_bits & SHADER_REQ_SAMPLER_RECT) || ctx->uses_sampler_buf)
         require_glsl_ver(ctx, 140);
}

/* size queries are pretty much separate */
static void emit_txq(struct dump_ctx *ctx,
                     struct tgsi_full_instruction *inst,
                     uint32_t sreg_index,
                     char srcs[4][255],
                     char dsts[3][255],
                     const char *writemask)
{
   unsigned twm = TGSI_WRITEMASK_NONE;
   char bias[128] = {0};
   const int sampler_index = 1;
   bool is_shad;
   enum vrend_type_qualifier dtypeprefix = INT_BITS_TO_FLOAT;

   set_texture_reqs(ctx, inst, sreg_index, &is_shad);

   /* no lod parameter for txq for these */
   if (inst->Texture.Texture != TGSI_TEXTURE_RECT &&
       inst->Texture.Texture != TGSI_TEXTURE_SHADOWRECT &&
       inst->Texture.Texture != TGSI_TEXTURE_BUFFER &&
       inst->Texture.Texture != TGSI_TEXTURE_2D_MSAA &&
       inst->Texture.Texture != TGSI_TEXTURE_2D_ARRAY_MSAA)
      snprintf(bias, 128, ", int(%s.w)", srcs[0]);

   /* need to emit a textureQueryLevels */
   if (inst->Dst[0].Register.WriteMask & 0x8) {

      if (inst->Texture.Texture != TGSI_TEXTURE_BUFFER &&
          inst->Texture.Texture != TGSI_TEXTURE_RECT &&
          inst->Texture.Texture != TGSI_TEXTURE_2D_MSAA &&
          inst->Texture.Texture != TGSI_TEXTURE_2D_ARRAY_MSAA) {
         ctx->shader_req_bits |= SHADER_REQ_TXQ_LEVELS;
         if (inst->Dst[0].Register.WriteMask & 0x7)
            twm = TGSI_WRITEMASK_W;
         emit_buff(ctx, "%s%s = %s(textureQueryLevels(%s));\n", dsts[0], get_wm_string(twm), get_string(dtypeprefix), srcs[sampler_index]);
      }

      if (inst->Dst[0].Register.WriteMask & 0x7) {
         switch (inst->Texture.Texture) {
         case TGSI_TEXTURE_1D:
         case TGSI_TEXTURE_BUFFER:
         case TGSI_TEXTURE_SHADOW1D:
            twm = TGSI_WRITEMASK_X;
            break;
         case TGSI_TEXTURE_1D_ARRAY:
         case TGSI_TEXTURE_SHADOW1D_ARRAY:
         case TGSI_TEXTURE_2D:
         case TGSI_TEXTURE_SHADOW2D:
         case TGSI_TEXTURE_RECT:
         case TGSI_TEXTURE_SHADOWRECT:
         case TGSI_TEXTURE_CUBE:
         case TGSI_TEXTURE_SHADOWCUBE:
         case TGSI_TEXTURE_2D_MSAA:
            twm = TGSI_WRITEMASK_XY;
            break;
         case TGSI_TEXTURE_3D:
         case TGSI_TEXTURE_2D_ARRAY:
         case TGSI_TEXTURE_SHADOW2D_ARRAY:
         case TGSI_TEXTURE_SHADOWCUBE_ARRAY:
         case TGSI_TEXTURE_CUBE_ARRAY:
         case TGSI_TEXTURE_2D_ARRAY_MSAA:
            twm = TGSI_WRITEMASK_XYZ;
            break;
         }
      }
   }

   if (inst->Dst[0].Register.WriteMask & 0x7) {
      bool txq_returns_vec = (inst->Texture.Texture != TGSI_TEXTURE_BUFFER &&
                              inst->Texture.Texture != TGSI_TEXTURE_1D &&
                              inst->Texture.Texture != TGSI_TEXTURE_SHADOW1D);
      emit_buff(ctx, "%s%s = %s(textureSize(%s%s))%s;\n", dsts[0], get_wm_string(twm), get_string(dtypeprefix), srcs[sampler_index], bias,
            txq_returns_vec ? writemask : "");
   }
}

/* sample queries are pretty much separate */
static void emit_txqs(struct dump_ctx *ctx,
                      struct tgsi_full_instruction *inst,
                      uint32_t sreg_index,
                      char srcs[4][255],
                      char dsts[3][255])
{
   const int sampler_index = 0;
   bool is_shad;
   enum vrend_type_qualifier dtypeprefix = INT_BITS_TO_FLOAT;

   ctx->shader_req_bits |= SHADER_REQ_TXQS;
   set_texture_reqs(ctx, inst, sreg_index, &is_shad);

   if (inst->Texture.Texture != TGSI_TEXTURE_2D_MSAA &&
       inst->Texture.Texture != TGSI_TEXTURE_2D_ARRAY_MSAA) {
      set_buf_error(ctx);
      return;
   }

   emit_buff(ctx, "%s = %s(textureSamples(%s));\n", dsts[0],
            get_string(dtypeprefix), srcs[sampler_index]);
}

static const char *get_tex_inst_ext(struct tgsi_full_instruction *inst)
{
   const char *tex_ext = "";
   if (inst->Instruction.Opcode == TGSI_OPCODE_LODQ) {
      tex_ext = "QueryLOD";
   } else if (inst->Instruction.Opcode == TGSI_OPCODE_TXP) {
      if (inst->Texture.Texture == TGSI_TEXTURE_CUBE ||
          inst->Texture.Texture == TGSI_TEXTURE_2D_ARRAY ||
          inst->Texture.Texture == TGSI_TEXTURE_1D_ARRAY)
         tex_ext = "";
      else if (inst->Texture.NumOffsets == 1)
         tex_ext = "ProjOffset";
      else
         tex_ext = "Proj";
   } else if (inst->Instruction.Opcode == TGSI_OPCODE_TXL ||
              inst->Instruction.Opcode == TGSI_OPCODE_TXL2) {
      if (inst->Texture.NumOffsets == 1)
         tex_ext = "LodOffset";
      else
         tex_ext = "Lod";
   } else if (inst->Instruction.Opcode == TGSI_OPCODE_TXD) {
      if (inst->Texture.NumOffsets == 1)
         tex_ext = "GradOffset";
      else
         tex_ext = "Grad";
   } else if (inst->Instruction.Opcode == TGSI_OPCODE_TG4) {
      if (inst->Texture.NumOffsets == 4)
         tex_ext = "GatherOffsets";
      else if (inst->Texture.NumOffsets == 1)
         tex_ext = "GatherOffset";
      else
         tex_ext = "Gather";
   } else {
      if (inst->Texture.NumOffsets == 1)
         tex_ext = "Offset";
      else
         tex_ext = "";
   }
   return tex_ext;
}

static bool fill_offset_buffer(struct dump_ctx *ctx,
                               struct tgsi_full_instruction *inst,
                               char *offbuf)
{
   if (inst->TexOffsets[0].File == TGSI_FILE_IMMEDIATE) {
      struct immed *imd = &ctx->imm[inst->TexOffsets[0].Index];
      switch (inst->Texture.Texture) {
      case TGSI_TEXTURE_1D:
      case TGSI_TEXTURE_1D_ARRAY:
      case TGSI_TEXTURE_SHADOW1D:
      case TGSI_TEXTURE_SHADOW1D_ARRAY:
         snprintf(offbuf, 256, ", int(%d)", imd->val[inst->TexOffsets[0].SwizzleX].i);
         break;
      case TGSI_TEXTURE_RECT:
      case TGSI_TEXTURE_SHADOWRECT:
      case TGSI_TEXTURE_2D:
      case TGSI_TEXTURE_2D_ARRAY:
      case TGSI_TEXTURE_SHADOW2D:
      case TGSI_TEXTURE_SHADOW2D_ARRAY:
         snprintf(offbuf, 256, ", ivec2(%d, %d)", imd->val[inst->TexOffsets[0].SwizzleX].i, imd->val[inst->TexOffsets[0].SwizzleY].i);
         break;
      case TGSI_TEXTURE_3D:
         snprintf(offbuf, 256, ", ivec3(%d, %d, %d)", imd->val[inst->TexOffsets[0].SwizzleX].i, imd->val[inst->TexOffsets[0].SwizzleY].i,
                  imd->val[inst->TexOffsets[0].SwizzleZ].i);
         break;
      default:
         fprintf(stderr, "unhandled texture: %x\n", inst->Texture.Texture);
         return false;
      }
   } else if (inst->TexOffsets[0].File == TGSI_FILE_TEMPORARY) {
      struct vrend_temp_range *range = find_temp_range(ctx, inst->TexOffsets[0].Index);
      int idx = inst->TexOffsets[0].Index - range->first;
      switch (inst->Texture.Texture) {
      case TGSI_TEXTURE_1D:
      case TGSI_TEXTURE_1D_ARRAY:
      case TGSI_TEXTURE_SHADOW1D:
      case TGSI_TEXTURE_SHADOW1D_ARRAY:
         snprintf(offbuf, 256, ", int(floatBitsToInt(temp%d[%d].%c))",
                  range->first, idx,
                  get_swiz_char(inst->TexOffsets[0].SwizzleX));
         break;
      case TGSI_TEXTURE_RECT:
      case TGSI_TEXTURE_SHADOWRECT:
      case TGSI_TEXTURE_2D:
      case TGSI_TEXTURE_2D_ARRAY:
      case TGSI_TEXTURE_SHADOW2D:
         case TGSI_TEXTURE_SHADOW2D_ARRAY:
         snprintf(offbuf, 256, ", ivec2(floatBitsToInt(temp%d[%d].%c), floatBitsToInt(temp%d[%d].%c))",
                  range->first, idx,
                  get_swiz_char(inst->TexOffsets[0].SwizzleX),
                  range->first, idx,
                  get_swiz_char(inst->TexOffsets[0].SwizzleY));
            break;
      case TGSI_TEXTURE_3D:
         snprintf(offbuf, 256, ", ivec3(floatBitsToInt(temp%d[%d].%c), floatBitsToInt(temp%d[%d].%c), floatBitsToInt(temp%d[%d].%c)",
                  range->first, idx,
                  get_swiz_char(inst->TexOffsets[0].SwizzleX),
                  range->first, idx,
                  get_swiz_char(inst->TexOffsets[0].SwizzleY),
                  range->first, idx,
                  get_swiz_char(inst->TexOffsets[0].SwizzleZ));
         break;
      default:
         fprintf(stderr, "unhandled texture: %x\n", inst->Texture.Texture);
         return false;
         break;
      }
   } else if (inst->TexOffsets[0].File == TGSI_FILE_INPUT) {
      for (uint32_t j = 0; j < ctx->num_inputs; j++) {
         if (ctx->inputs[j].first != inst->TexOffsets[0].Index)
            continue;
         switch (inst->Texture.Texture) {
         case TGSI_TEXTURE_1D:
         case TGSI_TEXTURE_1D_ARRAY:
         case TGSI_TEXTURE_SHADOW1D:
         case TGSI_TEXTURE_SHADOW1D_ARRAY:
            snprintf(offbuf, 256, ", int(floatBitsToInt(%s.%c))",
                     ctx->inputs[j].glsl_name,
                     get_swiz_char(inst->TexOffsets[0].SwizzleX));
            break;
         case TGSI_TEXTURE_RECT:
         case TGSI_TEXTURE_SHADOWRECT:
         case TGSI_TEXTURE_2D:
         case TGSI_TEXTURE_2D_ARRAY:
         case TGSI_TEXTURE_SHADOW2D:
         case TGSI_TEXTURE_SHADOW2D_ARRAY:
            snprintf(offbuf, 256, ", ivec2(floatBitsToInt(%s.%c), floatBitsToInt(%s.%c))",
                     ctx->inputs[j].glsl_name,
                     get_swiz_char(inst->TexOffsets[0].SwizzleX),
                     ctx->inputs[j].glsl_name,
                     get_swiz_char(inst->TexOffsets[0].SwizzleY));
            break;
         case TGSI_TEXTURE_3D:
            snprintf(offbuf, 256, ", ivec3(floatBitsToInt(%s.%c), floatBitsToInt(%s.%c), floatBitsToInt(%s.%c)",
                     ctx->inputs[j].glsl_name,
                     get_swiz_char(inst->TexOffsets[0].SwizzleX),
                     ctx->inputs[j].glsl_name,
                     get_swiz_char(inst->TexOffsets[0].SwizzleY),
                     ctx->inputs[j].glsl_name,
                     get_swiz_char(inst->TexOffsets[0].SwizzleZ));
            break;
         default:
            fprintf(stderr, "unhandled texture: %x\n", inst->Texture.Texture);
            return false;
            break;
         }
      }
   }
   return true;
}

static void translate_tex(struct dump_ctx *ctx,
                          struct tgsi_full_instruction *inst,
                          struct source_info *sinfo,
                          struct dest_info *dinfo,
                          char srcs[4][255],
                          char dsts[3][255],
                          const char *writemask)
{
   enum vrend_type_qualifier txfi = TYPE_CONVERSION_NONE;
   unsigned twm = TGSI_WRITEMASK_NONE, gwm = TGSI_WRITEMASK_NONE;
   enum vrend_type_qualifier dtypeprefix = TYPE_CONVERSION_NONE;
   bool is_shad = false;
   char offbuf[256] = {0};
   char bias[256] = {0};
   int sampler_index;
   const char *tex_ext;

   set_texture_reqs(ctx, inst, sinfo->sreg_index, &is_shad);

   switch (ctx->samplers[sinfo->sreg_index].tgsi_sampler_return) {
   case TGSI_RETURN_TYPE_SINT:
      /* if dstconv isn't an int */
      if (dinfo->dstconv != INT)
         dtypeprefix = INT_BITS_TO_FLOAT;
      break;
   case TGSI_RETURN_TYPE_UINT:
      /* if dstconv isn't an int */
      if (dinfo->dstconv != INT)
         dtypeprefix = UINT_BITS_TO_FLOAT;
      break;
   default:
      break;
   }

   sampler_index = 1;

   if (inst->Instruction.Opcode == TGSI_OPCODE_LODQ)
      ctx->shader_req_bits |= SHADER_REQ_LODQ;

   switch (inst->Texture.Texture) {
   case TGSI_TEXTURE_1D:
   case TGSI_TEXTURE_BUFFER:
      if (inst->Instruction.Opcode == TGSI_OPCODE_TXP)
         twm = TGSI_WRITEMASK_NONE;
      else
         twm = TGSI_WRITEMASK_X;
      txfi = INT;
      break;
   case TGSI_TEXTURE_1D_ARRAY:
      twm = TGSI_WRITEMASK_XY;
      txfi = IVEC2;
      break;
   case TGSI_TEXTURE_2D:
   case TGSI_TEXTURE_RECT:
      if (inst->Instruction.Opcode == TGSI_OPCODE_TXP)
         twm = TGSI_WRITEMASK_NONE;
      else
         twm = TGSI_WRITEMASK_XY;
      txfi = IVEC2;
      break;
   case TGSI_TEXTURE_SHADOW1D:
   case TGSI_TEXTURE_SHADOW2D:
   case TGSI_TEXTURE_SHADOW1D_ARRAY:
   case TGSI_TEXTURE_SHADOWRECT:
   case TGSI_TEXTURE_3D:
      if (inst->Instruction.Opcode == TGSI_OPCODE_TXP)
         twm = TGSI_WRITEMASK_NONE;
      else if (inst->Instruction.Opcode == TGSI_OPCODE_TG4)
         twm = TGSI_WRITEMASK_XY;
      else
         twm = TGSI_WRITEMASK_XYZ;
      txfi = IVEC3;
      break;
   case TGSI_TEXTURE_CUBE:
   case TGSI_TEXTURE_2D_ARRAY:
      twm = TGSI_WRITEMASK_XYZ;
      txfi = IVEC3;
      break;
   case TGSI_TEXTURE_2D_MSAA:
      twm = TGSI_WRITEMASK_XY;
      txfi = IVEC2;
      break;
   case TGSI_TEXTURE_2D_ARRAY_MSAA:
      twm = TGSI_WRITEMASK_XYZ;
      txfi = IVEC3;
      break;

   case TGSI_TEXTURE_SHADOWCUBE:
   case TGSI_TEXTURE_SHADOW2D_ARRAY:
   case TGSI_TEXTURE_SHADOWCUBE_ARRAY:
   case TGSI_TEXTURE_CUBE_ARRAY:
   default:
      if (inst->Instruction.Opcode == TGSI_OPCODE_TG4 &&
          inst->Texture.Texture != TGSI_TEXTURE_CUBE_ARRAY
          && inst->Texture.Texture != TGSI_TEXTURE_SHADOWCUBE_ARRAY)
         twm = TGSI_WRITEMASK_XYZ;
      else
         twm = TGSI_WRITEMASK_NONE;
      txfi = TYPE_CONVERSION_NONE;
      break;
   }

   if (inst->Instruction.Opcode == TGSI_OPCODE_TXD) {
      switch (inst->Texture.Texture) {
      case TGSI_TEXTURE_1D:
      case TGSI_TEXTURE_SHADOW1D:
      case TGSI_TEXTURE_1D_ARRAY:
      case TGSI_TEXTURE_SHADOW1D_ARRAY:
         gwm = TGSI_WRITEMASK_X;
         break;
      case TGSI_TEXTURE_2D:
      case TGSI_TEXTURE_SHADOW2D:
      case TGSI_TEXTURE_2D_ARRAY:
      case TGSI_TEXTURE_SHADOW2D_ARRAY:
      case TGSI_TEXTURE_RECT:
      case TGSI_TEXTURE_SHADOWRECT:
         gwm = TGSI_WRITEMASK_XY;
         break;
      case TGSI_TEXTURE_3D:
      case TGSI_TEXTURE_CUBE:
      case TGSI_TEXTURE_SHADOWCUBE:
      case TGSI_TEXTURE_CUBE_ARRAY:
         gwm = TGSI_WRITEMASK_XYZ;
         break;
      default:
         gwm = TGSI_WRITEMASK_NONE;
         break;
      }
   }

   if (inst->Instruction.Opcode == TGSI_OPCODE_TXB2 || inst->Instruction.Opcode == TGSI_OPCODE_TXL2 || inst->Instruction.Opcode == TGSI_OPCODE_TEX2) {
      sampler_index = 2;
      if (inst->Instruction.Opcode != TGSI_OPCODE_TEX2)
         snprintf(bias, 64, ", %s.x", srcs[1]);
      else if (inst->Texture.Texture == TGSI_TEXTURE_SHADOWCUBE_ARRAY)
         snprintf(bias, 64, ", float(%s)", srcs[1]);
   } else if (inst->Instruction.Opcode == TGSI_OPCODE_TXB || inst->Instruction.Opcode == TGSI_OPCODE_TXL)
      snprintf(bias, 64, ", %s.w", srcs[0]);
   else if (inst->Instruction.Opcode == TGSI_OPCODE_TXF) {
      if (inst->Texture.Texture == TGSI_TEXTURE_1D ||
          inst->Texture.Texture == TGSI_TEXTURE_2D ||
          inst->Texture.Texture == TGSI_TEXTURE_2D_MSAA ||
          inst->Texture.Texture == TGSI_TEXTURE_2D_ARRAY_MSAA ||
          inst->Texture.Texture == TGSI_TEXTURE_3D ||
          inst->Texture.Texture == TGSI_TEXTURE_1D_ARRAY ||
          inst->Texture.Texture == TGSI_TEXTURE_2D_ARRAY) {
         snprintf(bias, 64, ", int(%s.w)", srcs[0]);
      }
   } else if (inst->Instruction.Opcode == TGSI_OPCODE_TXD) {
      snprintf(bias, 128, ", %s%s, %s%s", srcs[1], get_wm_string(gwm), srcs[2], get_wm_string(gwm));
      sampler_index = 3;
   } else if (inst->Instruction.Opcode == TGSI_OPCODE_TG4) {
      sampler_index = 2;
      ctx->shader_req_bits |= SHADER_REQ_TG4;
      if (!ctx->cfg->use_gles) {
         if (inst->Texture.NumOffsets > 1 || is_shad || (ctx->shader_req_bits & SHADER_REQ_SAMPLER_RECT))
            ctx->shader_req_bits |= SHADER_REQ_GPU_SHADER5;
      }
      if (inst->Texture.NumOffsets == 1) {
         if (inst->TexOffsets[0].File != TGSI_FILE_IMMEDIATE)
            ctx->shader_req_bits |= SHADER_REQ_GPU_SHADER5;
      }
      if (is_shad) {
         if (inst->Texture.Texture == TGSI_TEXTURE_SHADOWCUBE ||
             inst->Texture.Texture == TGSI_TEXTURE_SHADOW2D_ARRAY)
            snprintf(bias, 64, ", %s.w", srcs[0]);
         else if (inst->Texture.Texture == TGSI_TEXTURE_SHADOWCUBE_ARRAY)
            snprintf(bias, 64, ", %s.x", srcs[1]);
         else
            snprintf(bias, 64, ", %s.z", srcs[0]);
      } else if (sinfo->tg4_has_component) {
         if (inst->Texture.NumOffsets == 0) {
            if (inst->Texture.Texture == TGSI_TEXTURE_2D ||
                inst->Texture.Texture == TGSI_TEXTURE_RECT ||
                inst->Texture.Texture == TGSI_TEXTURE_CUBE ||
                inst->Texture.Texture == TGSI_TEXTURE_2D_ARRAY ||
                inst->Texture.Texture == TGSI_TEXTURE_CUBE_ARRAY)
               snprintf(bias, 64, ", int(%s)", srcs[1]);
         } else if (inst->Texture.NumOffsets) {
            if (inst->Texture.Texture == TGSI_TEXTURE_2D ||
                inst->Texture.Texture == TGSI_TEXTURE_RECT ||
                inst->Texture.Texture == TGSI_TEXTURE_2D_ARRAY)
               snprintf(bias, 64, ", int(%s)", srcs[1]);
         }
      }
   } else
      bias[0] = 0;

   tex_ext = get_tex_inst_ext(inst);

   if (inst->Texture.NumOffsets == 1) {
      if (inst->TexOffsets[0].Index >= (int)ARRAY_SIZE(ctx->imm)) {
         fprintf(stderr, "Immediate exceeded, max is %lu\n", ARRAY_SIZE(ctx->imm));
         set_buf_error(ctx);
         return;
      }

      if (!fill_offset_buffer(ctx, inst, offbuf)) {
         set_buf_error(ctx);
         return;
      }

      if (inst->Instruction.Opcode == TGSI_OPCODE_TXL || inst->Instruction.Opcode == TGSI_OPCODE_TXL2 || inst->Instruction.Opcode == TGSI_OPCODE_TXD || (inst->Instruction.Opcode == TGSI_OPCODE_TG4 && is_shad)) {
         char tmp[256];
         strcpy(tmp, offbuf);
         strcpy(offbuf, bias);
         strcpy(bias, tmp);
      }
   }
   if (inst->Instruction.Opcode == TGSI_OPCODE_TXF) {
      emit_buff(ctx, "%s = %s(%s(texelFetch%s(%s, %s(%s%s)%s%s)%s));\n", dsts[0], get_string(dinfo->dstconv), get_string(dtypeprefix), tex_ext, srcs[sampler_index], get_string(txfi), srcs[0], get_wm_string(twm), bias, offbuf, dinfo->dst_override_no_wm[0] ? "" : writemask);
   } else if (ctx->cfg->glsl_version < 140 && (ctx->shader_req_bits & SHADER_REQ_SAMPLER_RECT)) {
      /* rect is special in GLSL 1.30 */
      if (inst->Texture.Texture == TGSI_TEXTURE_RECT)
         emit_buff(ctx, "%s = texture2DRect(%s, %s.xy)%s;\n", dsts[0], srcs[sampler_index], srcs[0], writemask);
      else if (inst->Texture.Texture == TGSI_TEXTURE_SHADOWRECT)
         emit_buff(ctx, "%s = shadow2DRect(%s, %s.xyz)%s;\n", dsts[0], srcs[sampler_index], srcs[0], writemask);
   } else if (is_shad && inst->Instruction.Opcode != TGSI_OPCODE_TG4) { /* TGSI returns 1.0 in alpha */
      const char *cname = tgsi_proc_to_prefix(ctx->prog_type);
      const struct tgsi_full_src_register *src = &inst->Src[sampler_index];
      emit_buff(ctx, "%s = %s(%s(vec4(vec4(texture%s(%s, %s%s%s%s)) * %sshadmask%d + %sshadadd%d)%s));\n", dsts[0], get_string(dinfo->dstconv), get_string(dtypeprefix), tex_ext, srcs[sampler_index], srcs[0], get_wm_string(twm), offbuf, bias, cname, src->Register.Index, cname, src->Register.Index, writemask);
   } else {
      /* OpenGL ES do not support 1D texture
       * so we use a 2D texture with a parameter set to 0.5
       */
      if (ctx->cfg->use_gles && inst->Texture.Texture == TGSI_TEXTURE_1D) {
         if (inst->Instruction.Opcode == TGSI_OPCODE_TXP)
            emit_buff(ctx, "%s = %s(%s(texture2D(%s, vec2(%s.x / %s.w, 0.5))%s));\n", dsts[0], get_string(dinfo->dstconv), get_string(dtypeprefix), srcs[sampler_index], srcs[0], srcs[0], dinfo->dst_override_no_wm[0] ? "" : writemask);
         else
            emit_buff(ctx, "%s = %s(%s(texture2D(%s, vec2(%s%s%s%s, 0.5))%s));\n", dsts[0], get_string(dinfo->dstconv), get_string(dtypeprefix), srcs[sampler_index], srcs[0], get_wm_string(twm), offbuf, bias, dinfo->dst_override_no_wm[0] ? "" : writemask);
      } else {
         emit_buff(ctx, "%s = %s(%s(texture%s(%s, %s%s%s%s)%s));\n", dsts[0], get_string(dinfo->dstconv), get_string(dtypeprefix), tex_ext, srcs[sampler_index], srcs[0], get_wm_string(twm), offbuf, bias, dinfo->dst_override_no_wm[0] ? "" : writemask);
      }
   }
}

static void
create_swizzled_clipdist(struct dump_ctx *ctx,
                         char *result,
                         const struct tgsi_full_src_register *src,
                         int input_idx,
                         bool gl_in,
                         const char *stypeprefix,
                         const char *prefix,
                         const char *arrayname)
{
   char clipdistvec[4][64] = {};
   int idx;
   bool has_prev_vals = (ctx->key->prev_stage_num_cull_out + ctx->key->prev_stage_num_clip_out) > 0;
   int num_culls = has_prev_vals ? ctx->key->prev_stage_num_cull_out : 0;
   int num_clips = has_prev_vals ? ctx->key->prev_stage_num_clip_out : ctx->num_in_clip_dist;
   for (unsigned cc = 0; cc < 4; cc++) {
      const char *cc_name = ctx->inputs[input_idx].glsl_name;
      idx = ctx->inputs[input_idx].sid * 4;
      if (cc == 0)
         idx += src->Register.SwizzleX;
      else if (cc == 1)
         idx += src->Register.SwizzleY;
      else if (cc == 2)
         idx += src->Register.SwizzleZ;
      else if (cc == 3)
         idx += src->Register.SwizzleW;

      if (num_culls) {
         if (idx >= num_clips) {
            idx -= num_clips;
            cc_name = "gl_CullDistance";
         }
         if (ctx->key->prev_stage_num_cull_out)
            if (idx >= ctx->key->prev_stage_num_cull_out)
               idx = 0;
      } else {
         if (ctx->key->prev_stage_num_clip_out)
            if (idx >= ctx->key->prev_stage_num_clip_out)
               idx = 0;
      }
      if (gl_in)
         snprintf(clipdistvec[cc], 64, "%sgl_in%s.%s[%d]", prefix, arrayname, cc_name, idx);
      else
         snprintf(clipdistvec[cc], 64, "%s%s%s[%d]", prefix, arrayname, cc_name, idx);
   }
   snprintf(result, 255, "%s(vec4(%s,%s,%s,%s))", stypeprefix, clipdistvec[0], clipdistvec[1], clipdistvec[2], clipdistvec[3]);
}

static enum vrend_type_qualifier get_coord_prefix(int resource, bool *is_ms)
{
   switch(resource) {
   case TGSI_TEXTURE_1D:
   case TGSI_TEXTURE_BUFFER:
      return INT;
   case TGSI_TEXTURE_2D:
   case TGSI_TEXTURE_RECT:
   case TGSI_TEXTURE_1D_ARRAY:
      return IVEC2;
   case TGSI_TEXTURE_3D:
   case TGSI_TEXTURE_CUBE:
   case TGSI_TEXTURE_2D_ARRAY:
   case TGSI_TEXTURE_CUBE_ARRAY:
      return IVEC3;
   case TGSI_TEXTURE_2D_MSAA:
      *is_ms = true;
      return IVEC2;
   case TGSI_TEXTURE_2D_ARRAY_MSAA:
      *is_ms = true;
      return IVEC3;
   default:
      return TYPE_CONVERSION_NONE;
   }
}

static bool is_integer_memory(struct dump_ctx *ctx, enum tgsi_file_type file_type, uint32_t index)
{
   switch(file_type) {
   case TGSI_FILE_BUFFER:
      return !!(ctx->ssbo_integer_mask & (1 << index));
   case TGSI_FILE_MEMORY:
      return ctx->integer_memory;
   default:
      fprintf(stderr, "Invalid file type");
   }

   return false;
}

static void
translate_store(struct dump_ctx *ctx,
                struct tgsi_full_instruction *inst,
                struct source_info *sinfo,
                char srcs[4][255],
                char dsts[3][255])
{
   const struct tgsi_full_dst_register *dst = &inst->Dst[0];

   if (dst->Register.File == TGSI_FILE_IMAGE) {
      bool is_ms = false;
      enum vrend_type_qualifier coord_prefix = get_coord_prefix(ctx->images[dst->Register.Index].decl.Resource, &is_ms);
      enum tgsi_return_type itype;
      char ms_str[32] = {};
      enum vrend_type_qualifier stypeprefix = TYPE_CONVERSION_NONE;
      const char *conversion = sinfo->override_no_cast[0] ? "" : get_string(FLOAT_BITS_TO_INT);
      get_internalformat_string(inst->Memory.Format, &itype);
      if (is_ms) {
         snprintf(ms_str, 32, "int(%s.w),", srcs[0]);
      }
      switch (itype) {
      case TGSI_RETURN_TYPE_UINT:
         stypeprefix = FLOAT_BITS_TO_UINT;
         break;
      case TGSI_RETURN_TYPE_SINT:
         stypeprefix = FLOAT_BITS_TO_INT;
         break;
      default:
         break;
      }
      emit_buff(ctx, "imageStore(%s,%s(%s(%s)),%s%s(%s));\n", dsts[0], get_string(coord_prefix),
               conversion, srcs[0], ms_str, get_string(stypeprefix), srcs[1]);
   } else if (dst->Register.File == TGSI_FILE_BUFFER || dst->Register.File == TGSI_FILE_MEMORY) {
      enum vrend_type_qualifier dtypeprefix;
      dtypeprefix = (is_integer_memory(ctx, dst->Register.File, dst->Register.Index)) ? FLOAT_BITS_TO_INT : FLOAT_BITS_TO_UINT;
      const char *conversion = sinfo->override_no_cast[1] ? "" : get_string(dtypeprefix);
      if (inst->Dst[0].Register.WriteMask & 0x1) {
         emit_buff(ctx, "%s[uint(floatBitsToUint(%s))>>2] = %s(%s).x;\n", dsts[0], srcs[0], conversion, srcs[1]);
      }
      if (inst->Dst[0].Register.WriteMask & 0x2) {
         emit_buff(ctx, "%s[(uint(floatBitsToUint(%s))>>2)+1u] = %s(%s).y;\n", dsts[0], srcs[0], conversion, srcs[1]);
      }
      if (inst->Dst[0].Register.WriteMask & 0x4) {
         emit_buff(ctx, "%s[(uint(floatBitsToUint(%s))>>2)+2u] = %s(%s).z;\n", dsts[0], srcs[0], conversion, srcs[1]);
      }
      if (inst->Dst[0].Register.WriteMask & 0x8) {
         emit_buff(ctx, "%s[(uint(floatBitsToUint(%s))>>2)+3u] = %s(%s).w;\n", dsts[0], srcs[0], conversion, srcs[1]);
      }
   }
}

static void
translate_load(struct dump_ctx *ctx,
               struct tgsi_full_instruction *inst,
               struct source_info *sinfo,
               struct dest_info *dinfo,
               char srcs[4][255],
               char dsts[3][255],
               const char *writemask)
{
   const struct tgsi_full_src_register *src = &inst->Src[0];
   if (src->Register.File == TGSI_FILE_IMAGE) {
      bool is_ms = false;
      enum vrend_type_qualifier coord_prefix = get_coord_prefix(ctx->images[sinfo->sreg_index].decl.Resource, &is_ms);
      enum vrend_type_qualifier dtypeprefix = TYPE_CONVERSION_NONE;
      const char *conversion = sinfo->override_no_cast[1] ? "" : get_string(FLOAT_BITS_TO_INT);
      enum tgsi_return_type itype;
      get_internalformat_string(ctx->images[sinfo->sreg_index].decl.Format, &itype);
      char ms_str[32] = {};
      const char *wm = dinfo->dst_override_no_wm[0] ? "" : writemask;
      if (is_ms) {
         snprintf(ms_str, 32, ", int(%s.w)", srcs[1]);
      }
      switch (itype) {
      case TGSI_RETURN_TYPE_UINT:
         dtypeprefix = UINT_BITS_TO_FLOAT;
         break;
      case TGSI_RETURN_TYPE_SINT:
         dtypeprefix = INT_BITS_TO_FLOAT;
         break;
      default:
         break;
      }
      emit_buff(ctx, "%s = %s(imageLoad(%s, %s(%s(%s))%s)%s);\n", dsts[0], get_string(dtypeprefix), srcs[0],
               get_string(coord_prefix), conversion, srcs[1], ms_str, wm);
   } else if (src->Register.File == TGSI_FILE_BUFFER ||
              src->Register.File == TGSI_FILE_MEMORY) {
      char mydst[255], atomic_op[9], atomic_src[10];
      enum vrend_type_qualifier dtypeprefix;
      strcpy(mydst, dsts[0]);
      char *wmp = strchr(mydst, '.');
      if (wmp)
         wmp[0] = 0;
      emit_buff(ctx, "ssbo_addr_temp = uint(floatBitsToUint(%s)) >> 2;\n", srcs[1]);

      atomic_op[0] = atomic_src[0] = '\0';
      if (ctx->ssbo_atomic_mask & (1 << src->Register.Index)) {
         /* Emulate atomicCounter with atomicOr. */
         strcpy(atomic_op, "atomicOr");
         strcpy(atomic_src, ", uint(0)");
      }

      dtypeprefix = (is_integer_memory(ctx, src->Register.File, src->Register.Index)) ? INT_BITS_TO_FLOAT : UINT_BITS_TO_FLOAT;
      if (inst->Dst[0].Register.WriteMask & 0x1) {
         emit_buff(ctx, "%s.x = (%s(%s(%s[ssbo_addr_temp]%s)));\n", mydst, get_string(dtypeprefix), atomic_op, srcs[0], atomic_src);
      }
      if (inst->Dst[0].Register.WriteMask & 0x2) {
         emit_buff(ctx, "%s.y = (%s(%s(%s[ssbo_addr_temp + 1u]%s)));\n", mydst, get_string(dtypeprefix), atomic_op, srcs[0], atomic_src);
      }
      if (inst->Dst[0].Register.WriteMask & 0x4) {
         emit_buff(ctx, "%s.z = (%s(%s(%s[ssbo_addr_temp + 2u]%s)));\n", mydst, get_string(dtypeprefix), atomic_op, srcs[0], atomic_src);
      }
      if (inst->Dst[0].Register.WriteMask & 0x8) {
         emit_buff(ctx, "%s.w = (%s(%s(%s[ssbo_addr_temp + 3u]%s)));\n", mydst, get_string(dtypeprefix), atomic_op, srcs[0], atomic_src);
      }
   } else if (src->Register.File == TGSI_FILE_HW_ATOMIC) {
      emit_buff(ctx, "%s = uintBitsToFloat(atomicCounter(%s));\n", dsts[0], srcs[0]);
   }
}

static const char *get_atomic_opname(int tgsi_opcode, bool *is_cas)
{
   const char *opname;
   *is_cas = false;
   switch (tgsi_opcode) {
   case TGSI_OPCODE_ATOMUADD:
      opname = "Add";
      break;
   case TGSI_OPCODE_ATOMXCHG:
      opname = "Exchange";
      break;
   case TGSI_OPCODE_ATOMCAS:
      opname = "CompSwap";
      *is_cas = true;
      break;
   case TGSI_OPCODE_ATOMAND:
      opname = "And";
      break;
   case TGSI_OPCODE_ATOMOR:
      opname = "Or";
      break;
   case TGSI_OPCODE_ATOMXOR:
      opname = "Xor";
      break;
   case TGSI_OPCODE_ATOMUMIN:
      opname = "Min";
      break;
   case TGSI_OPCODE_ATOMUMAX:
      opname = "Max";
      break;
   case TGSI_OPCODE_ATOMIMIN:
      opname = "Min";
      break;
   case TGSI_OPCODE_ATOMIMAX:
      opname = "Max";
      break;
   default:
      fprintf(stderr, "illegal atomic opcode");
      return NULL;
   }
   return opname;
}

static void
translate_resq(struct dump_ctx *ctx, struct tgsi_full_instruction *inst,
               char srcs[4][255], char dsts[3][255], const char *writemask)
{
   const struct tgsi_full_src_register *src = &inst->Src[0];

   if (src->Register.File == TGSI_FILE_IMAGE) {
      if (inst->Dst[0].Register.WriteMask & 0x8) {
         ctx->shader_req_bits |= SHADER_REQ_TXQS | SHADER_REQ_INTS;
         emit_buff(ctx, "%s = %s(imageSamples(%s));\n", dsts[0], get_string(INT_BITS_TO_FLOAT), srcs[0]);
      }
      if (inst->Dst[0].Register.WriteMask & 0x7) {
         ctx->shader_req_bits |= SHADER_REQ_IMAGE_SIZE | SHADER_REQ_INTS;
         bool skip_emit_writemask = inst->Memory.Texture == TGSI_TEXTURE_BUFFER ||
                                    inst->Memory.Texture == TGSI_TEXTURE_1D;
         emit_buff(ctx, "%s = %s(imageSize(%s)%s);\n", dsts[0], get_string(INT_BITS_TO_FLOAT), srcs[0],
                  skip_emit_writemask ? "" : writemask);
      }
   } else if (src->Register.File == TGSI_FILE_BUFFER) {
      emit_buff(ctx, "%s = %s(int(%s.length()) << 2);\n", dsts[0], get_string(INT_BITS_TO_FLOAT), srcs[0]);
   }
}

static void
translate_atomic(struct dump_ctx *ctx,
                 struct tgsi_full_instruction *inst,
                 struct source_info *sinfo,
                 char srcs[4][255],
                 char dsts[3][255])
{
   const struct tgsi_full_src_register *src = &inst->Src[0];
   const char *opname;
   enum vrend_type_qualifier stypeprefix = TYPE_CONVERSION_NONE;
   enum vrend_type_qualifier dtypeprefix = TYPE_CONVERSION_NONE;
   enum vrend_type_qualifier stypecast = TYPE_CONVERSION_NONE;
   bool is_cas;
   char cas_str[128] = {};

   if (src->Register.File == TGSI_FILE_IMAGE) {
     enum tgsi_return_type itype;
     get_internalformat_string(ctx->images[sinfo->sreg_index].decl.Format, &itype);
     switch (itype) {
     default:
     case TGSI_RETURN_TYPE_UINT:
        stypeprefix = FLOAT_BITS_TO_UINT;
        dtypeprefix = UINT_BITS_TO_FLOAT;
        stypecast = UINT;
        break;
     case TGSI_RETURN_TYPE_SINT:
        stypeprefix = FLOAT_BITS_TO_INT;
        dtypeprefix = INT_BITS_TO_FLOAT;
        stypecast = INT;
        break;
     case TGSI_RETURN_TYPE_FLOAT:
        ctx->shader_req_bits |= SHADER_REQ_ES31_COMPAT;
        stypecast = FLOAT;
        break;
     }
   } else {
     stypeprefix = FLOAT_BITS_TO_UINT;
     dtypeprefix = UINT_BITS_TO_FLOAT;
     stypecast = UINT;
   }

   opname = get_atomic_opname(inst->Instruction.Opcode, &is_cas);
   if (!opname) {
      set_buf_error(ctx);
      return;
   }

   if (is_cas)
      snprintf(cas_str, 128, ", %s(%s(%s))", get_string(stypecast), get_string(stypeprefix), srcs[3]);

   if (src->Register.File == TGSI_FILE_IMAGE) {
      bool is_ms = false;
      enum vrend_type_qualifier coord_prefix = get_coord_prefix(ctx->images[sinfo->sreg_index].decl.Resource, &is_ms);
      const char *conversion = sinfo->override_no_cast[1] ? "" : get_string(FLOAT_BITS_TO_INT);
      char ms_str[32] = {};
      if (is_ms) {
         snprintf(ms_str, 32, ", int(%s.w)", srcs[1]);
      }
      emit_buff(ctx, "%s = %s(imageAtomic%s(%s, %s(%s(%s))%s, %s(%s(%s))%s));\n", dsts[0],
               get_string(dtypeprefix), opname, srcs[0], get_string(coord_prefix), conversion,
               srcs[1], ms_str, get_string(stypecast), get_string(stypeprefix), srcs[2], cas_str);
      ctx->shader_req_bits |= SHADER_REQ_IMAGE_ATOMIC;
   }
   if (src->Register.File == TGSI_FILE_BUFFER || src->Register.File == TGSI_FILE_MEMORY) {
      enum vrend_type_qualifier type;
      if ((is_integer_memory(ctx, src->Register.File, src->Register.Index))) {
	 type = INT;
	 dtypeprefix = INT_BITS_TO_FLOAT;
	 stypeprefix = FLOAT_BITS_TO_INT;
      } else {
	 type = UINT;
	 dtypeprefix = UINT_BITS_TO_FLOAT;
	 stypeprefix = FLOAT_BITS_TO_UINT;
      }

      emit_buff(ctx, "%s = %s(atomic%s(%s[int(floatBitsToInt(%s)) >> 2], %s(%s(%s).x)%s));\n", dsts[0], get_string(dtypeprefix), opname, srcs[0], srcs[1], get_string(type), get_string(stypeprefix), srcs[2], cas_str);
   }
   if(src->Register.File == TGSI_FILE_HW_ATOMIC) {
      if (sinfo->imm_value == -1)
         emit_buff(ctx, "%s = %s(atomicCounterDecrement(%s) + 1u);\n", dsts[0], get_string(dtypeprefix), srcs[0]);
      else if (sinfo->imm_value == 1)
         emit_buff(ctx, "%s = %s(atomicCounterIncrement(%s));\n", dsts[0], get_string(dtypeprefix), srcs[0]);
      else
         emit_buff(ctx, "%s = %s(atomicCounter%sARB(%s, floatBitsToUint(%s).x%s));\n", dsts[0], get_string(dtypeprefix), opname, srcs[0], srcs[2], cas_str);
   }

}

static bool
get_destination_info(struct dump_ctx *ctx,
                     const struct tgsi_full_instruction *inst,
                     struct dest_info *dinfo,
                     char dsts[3][255],
                     char fp64_dsts[3][255],
                     char *writemask)
{
   const struct tgsi_full_dst_register *dst_reg;
   enum tgsi_opcode_type dtype = tgsi_opcode_infer_dst_type(inst->Instruction.Opcode);

   if (dtype == TGSI_TYPE_SIGNED || dtype == TGSI_TYPE_UNSIGNED)
      ctx->shader_req_bits |= SHADER_REQ_INTS;

   if (dtype == TGSI_TYPE_DOUBLE) {
      /* we need the uvec2 conversion for doubles */
      ctx->shader_req_bits |= SHADER_REQ_INTS | SHADER_REQ_FP64;
   }

   if (inst->Instruction.Opcode == TGSI_OPCODE_TXQ) {
      dinfo->dtypeprefix = INT_BITS_TO_FLOAT;
   } else {
      switch (dtype) {
      case TGSI_TYPE_UNSIGNED:
         dinfo->dtypeprefix = UINT_BITS_TO_FLOAT;
         break;
      case TGSI_TYPE_SIGNED:
         dinfo->dtypeprefix = INT_BITS_TO_FLOAT;
         break;
      default:
         break;
      }
   }

   for (uint32_t i = 0; i < inst->Instruction.NumDstRegs; i++) {
      char fp64_writemask[6] = {0};
      dst_reg = &inst->Dst[i];
      dinfo->dst_override_no_wm[i] = false;
      if (dst_reg->Register.WriteMask != TGSI_WRITEMASK_XYZW) {
         int wm_idx = 0, dbl_wm_idx = 0;
         writemask[wm_idx++] = '.';
         fp64_writemask[dbl_wm_idx++] = '.';

         if (dst_reg->Register.WriteMask & 0x1)
            writemask[wm_idx++] = 'x';
         if (dst_reg->Register.WriteMask & 0x2)
            writemask[wm_idx++] = 'y';
         if (dst_reg->Register.WriteMask & 0x4)
            writemask[wm_idx++] = 'z';
         if (dst_reg->Register.WriteMask & 0x8)
            writemask[wm_idx++] = 'w';

         if (dtype == TGSI_TYPE_DOUBLE) {
           if (dst_reg->Register.WriteMask & 0x3)
             fp64_writemask[dbl_wm_idx++] = 'x';
           if (dst_reg->Register.WriteMask & 0xc)
             fp64_writemask[dbl_wm_idx++] = 'y';
         }

         if (dtype == TGSI_TYPE_DOUBLE) {
            if (dbl_wm_idx == 2)
               dinfo->dstconv = DOUBLE;
            else
               dinfo->dstconv = DVEC2;
         } else {
            dinfo->dstconv = FLOAT + wm_idx - 2;
            dinfo->udstconv = UINT + wm_idx - 2;
            dinfo->idstconv = INT + wm_idx - 2;
         }
      } else {
         if (dtype == TGSI_TYPE_DOUBLE)
            dinfo->dstconv = DVEC2;
         else
            dinfo->dstconv = VEC4;
         dinfo->udstconv = UVEC4;
         dinfo->idstconv = IVEC4;
      }

      if (dst_reg->Register.File == TGSI_FILE_OUTPUT) {
         for (uint32_t j = 0; j < ctx->num_outputs; j++) {
            if (ctx->outputs[j].first == dst_reg->Register.Index) {

               if (inst->Instruction.Precise) {
                  if (!ctx->outputs[j].invariant) {
                     ctx->outputs[j].precise = true;
                     ctx->shader_req_bits |= SHADER_REQ_GPU_SHADER5;
                  }
               }

               if (ctx->glsl_ver_required >= 140 && ctx->outputs[j].name == TGSI_SEMANTIC_CLIPVERTEX) {
                  snprintf(dsts[i], 255, "clipv_tmp");
               } else if (ctx->outputs[j].name == TGSI_SEMANTIC_CLIPDIST) {
                  snprintf(dsts[i], 255, "clip_dist_temp[%d]", ctx->outputs[j].sid);
               } else if (ctx->outputs[j].name == TGSI_SEMANTIC_TESSOUTER ||
                          ctx->outputs[j].name == TGSI_SEMANTIC_TESSINNER ||
                          ctx->outputs[j].name == TGSI_SEMANTIC_SAMPLEMASK) {
                  int idx;
                  switch (dst_reg->Register.WriteMask) {
                  case 0x1: idx = 0; break;
                  case 0x2: idx = 1; break;
                  case 0x4: idx = 2; break;
                  case 0x8: idx = 3; break;
                  default:
                     idx = 0;
                     break;
                  }
                  snprintf(dsts[i], 255, "%s[%d]", ctx->outputs[j].glsl_name, idx);
                  if (ctx->outputs[j].is_int) {
                     dinfo->dtypeprefix = FLOAT_BITS_TO_INT;
                     dinfo->dstconv = INT;
                  }
               } else {
                  if (ctx->outputs[j].glsl_gl_block) {
                     snprintf(dsts[i], 255, "gl_out[%s].%s%s",
                              ctx->prog_type == TGSI_PROCESSOR_TESS_CTRL ? "gl_InvocationID" : "0",
                              ctx->outputs[j].glsl_name,
                              ctx->outputs[j].override_no_wm ? "" : writemask);
                  } else if (ctx->prog_type == TGSI_PROCESSOR_TESS_CTRL && ctx->outputs[j].name != TGSI_SEMANTIC_PATCH) {
                     if (ctx_indirect_outputs(ctx)) {
                        if (dst_reg->Register.Indirect)
                           snprintf(dsts[i], 255, "oblk[gl_InvocationID].%s%d[addr%d + %d]%s", get_stage_output_name_prefix(ctx->prog_type), ctx->generic_output_range.first, dst_reg->Indirect.Index, dst_reg->Register.Index - ctx->generic_output_range.array_id, ctx->outputs[j].override_no_wm ? "" : writemask);
                        else
                           snprintf(dsts[i], 255, "oblk[gl_InvocationID].%s%d[%d]%s", get_stage_output_name_prefix(ctx->prog_type), ctx->generic_output_range.first, dst_reg->Register.Index - ctx->generic_output_range.array_id, ctx->outputs[j].override_no_wm ? "" : writemask);

                     } else
                        snprintf(dsts[i], 255, "%s[gl_InvocationID]%s", ctx->outputs[j].glsl_name, ctx->outputs[j].override_no_wm ? "" : writemask);
                  } else if (ctx_indirect_outputs(ctx) && ctx->outputs[j].name == TGSI_SEMANTIC_GENERIC) {
                     if (dst_reg->Register.Indirect)
                        snprintf(dsts[i], 255, "oblk.%s%d[addr%d + %d]%s", get_stage_output_name_prefix(ctx->prog_type), ctx->generic_output_range.first, dst_reg->Indirect.Index, dst_reg->Register.Index - ctx->generic_output_range.array_id, ctx->outputs[j].override_no_wm ? "" : writemask);
                     else
                        snprintf(dsts[i], 255, "oblk.%s%d[%d]%s", get_stage_output_name_prefix(ctx->prog_type), ctx->generic_output_range.first, dst_reg->Register.Index - ctx->generic_output_range.array_id, ctx->outputs[j].override_no_wm ? "" : writemask);
                     dinfo->dst_override_no_wm[i] = ctx->outputs[j].override_no_wm;
                  } else if (ctx_indirect_outputs(ctx) && ctx->outputs[j].name == TGSI_SEMANTIC_PATCH) {
                     if (dst_reg->Register.Indirect)
                        snprintf(dsts[i], 255, "%sp%d[addr%d + %d]%s", get_stage_output_name_prefix(ctx->prog_type), ctx->patch_output_range.first, dst_reg->Indirect.Index, dst_reg->Register.Index - ctx->patch_output_range.array_id, ctx->outputs[j].override_no_wm ? "" : writemask);
                     else
                        snprintf(dsts[i], 255, "%sp%d[%d]%s", get_stage_output_name_prefix(ctx->prog_type), ctx->patch_output_range.first, dst_reg->Register.Index - ctx->patch_output_range.array_id, ctx->outputs[j].override_no_wm ? "" : writemask);
                     dinfo->dst_override_no_wm[i] = ctx->outputs[j].override_no_wm;
                  } else {
                     snprintf(dsts[i], 255, "%s%s", ctx->outputs[j].glsl_name, ctx->outputs[j].override_no_wm ? "" : writemask);
                     dinfo->dst_override_no_wm[i] = ctx->outputs[j].override_no_wm;
                  }
                  if (ctx->outputs[j].is_int) {
                     if (dinfo->dtypeprefix == TYPE_CONVERSION_NONE)
                        dinfo->dtypeprefix = FLOAT_BITS_TO_INT;
                     dinfo->dstconv = INT;
                  }
                  if (ctx->outputs[j].name == TGSI_SEMANTIC_PSIZE) {
                     dinfo->dstconv = FLOAT;
                     break;
                  }
               }
            }
         }
      }
      else if (dst_reg->Register.File == TGSI_FILE_TEMPORARY) {
         struct vrend_temp_range *range = find_temp_range(ctx, dst_reg->Register.Index);
         if (!range)
            return false;
         if (dst_reg->Register.Indirect) {
            snprintf(dsts[i], 255, "temp%d[addr0 + %d]%s", range->first, dst_reg->Register.Index - range->first, writemask);
         } else
            snprintf(dsts[i], 255, "temp%d[%d]%s", range->first, dst_reg->Register.Index - range->first, writemask);
      }
      else if (dst_reg->Register.File == TGSI_FILE_IMAGE) {
         const char *cname = tgsi_proc_to_prefix(ctx->prog_type);
	 if (ctx->info.indirect_files & (1 << TGSI_FILE_IMAGE)) {
            int basearrayidx = lookup_image_array(ctx, dst_reg->Register.Index);
            if (dst_reg->Register.Indirect) {
               assert(dst_reg->Indirect.File == TGSI_FILE_ADDRESS);
               snprintf(dsts[i], 255, "%simg%d[addr%d + %d]", cname, basearrayidx, dst_reg->Indirect.Index, dst_reg->Register.Index - basearrayidx);
            } else
               snprintf(dsts[i], 255, "%simg%d[%d]", cname, basearrayidx, dst_reg->Register.Index - basearrayidx);
         } else
            snprintf(dsts[i], 255, "%simg%d", cname, dst_reg->Register.Index);
      } else if (dst_reg->Register.File == TGSI_FILE_BUFFER) {
         const char *cname = tgsi_proc_to_prefix(ctx->prog_type);
         if (ctx->info.indirect_files & (1 << TGSI_FILE_BUFFER)) {
            bool atomic_ssbo = ctx->ssbo_atomic_mask & (1 << dst_reg->Register.Index);
            const char *atomic_str = atomic_ssbo ? "atomic" : "";
            int base = atomic_ssbo ? ctx->ssbo_atomic_array_base : ctx->ssbo_array_base;
            if (dst_reg->Register.Indirect) {
               snprintf(dsts[i], 255, "%sssboarr%s[addr%d+%d].%sssbocontents%d", cname, atomic_str, dst_reg->Indirect.Index, dst_reg->Register.Index - base, cname, base);
            } else
               snprintf(dsts[i], 255, "%sssboarr%s[%d].%sssbocontents%d", cname, atomic_str, dst_reg->Register.Index - base, cname, base);
         } else
            snprintf(dsts[i], 255, "%sssbocontents%d", cname, dst_reg->Register.Index);
      } else if (dst_reg->Register.File == TGSI_FILE_MEMORY) {
         snprintf(dsts[i], 255, "values");
      } else if (dst_reg->Register.File == TGSI_FILE_ADDRESS) {
         snprintf(dsts[i], 255, "addr%d", dst_reg->Register.Index);
      }

      if (dtype == TGSI_TYPE_DOUBLE) {
         strcpy(fp64_dsts[i], dsts[i]);
         snprintf(dsts[i], 255, "fp64_dst[%d]%s", i, fp64_writemask);
         writemask[0] = 0;
      }

   }

   return true;
}

static void fill_blkarray(struct dump_ctx *ctx,
                          const struct tgsi_full_src_register *src,
                          char *blkarray)
{
   if (ctx->prog_type == TGSI_PROCESSOR_TESS_CTRL)
      strcpy(blkarray, "[gl_InvocationID]");
   else {
      if (src->Register.Dimension) {
         if (src->Dimension.Indirect)
            snprintf(blkarray, 32, "[addr%d + %d]", src->DimIndirect.Index, src->Dimension.Index);
         else
            snprintf(blkarray, 32, "[%d]", src->Dimension.Index);
      } else
         strcpy(blkarray, "[0]");
   }
}

static bool
get_source_info(struct dump_ctx *ctx,
                const struct tgsi_full_instruction *inst,
                struct source_info *sinfo,
                char srcs[4][255], char src_swizzle0[10])
{
   bool stprefix = false;

   enum vrend_type_qualifier stypeprefix = TYPE_CONVERSION_NONE;
   enum tgsi_opcode_type stype = tgsi_opcode_infer_src_type(inst->Instruction.Opcode);

   if (stype == TGSI_TYPE_SIGNED || stype == TGSI_TYPE_UNSIGNED)
      ctx->shader_req_bits |= SHADER_REQ_INTS;
   if (stype == TGSI_TYPE_DOUBLE)
      ctx->shader_req_bits |= SHADER_REQ_INTS | SHADER_REQ_FP64;

   switch (stype) {
   case TGSI_TYPE_DOUBLE:
      stypeprefix = FLOAT_BITS_TO_UINT;
      sinfo->svec4 = DVEC2;
      stprefix = true;
      break;
   case TGSI_TYPE_UNSIGNED:
      stypeprefix = FLOAT_BITS_TO_UINT;
      sinfo->svec4 = UVEC4;
      stprefix = true;
      break;
   case TGSI_TYPE_SIGNED:
      stypeprefix = FLOAT_BITS_TO_INT;
      sinfo->svec4 = IVEC4;
      stprefix = true;
      break;
   default:
      break;
   }

   for (uint32_t i = 0; i < inst->Instruction.NumSrcRegs; i++) {
      const struct tgsi_full_src_register *src = &inst->Src[i];
      char swizzle[8] = {0};
      char prefix[6] = {0};
      char arrayname[16] = {0};
      char fp64_src[255];
      int swz_idx = 0, pre_idx = 0;
      boolean isfloatabsolute = src->Register.Absolute && stype != TGSI_TYPE_DOUBLE;

      sinfo->override_no_wm[i] = false;
      sinfo->override_no_cast[i] = false;
      if (isfloatabsolute)
         swizzle[swz_idx++] = ')';

      if (src->Register.Negate)
         prefix[pre_idx++] = '-';
      if (isfloatabsolute)
         strcpy(&prefix[pre_idx++], "abs(");

      if (src->Register.Dimension) {
         if (src->Dimension.Indirect) {
            assert(src->DimIndirect.File == TGSI_FILE_ADDRESS);
            sprintf(arrayname, "[addr%d]", src->DimIndirect.Index);
         } else
            sprintf(arrayname, "[%d]", src->Dimension.Index);
      }

      if (src->Register.SwizzleX != TGSI_SWIZZLE_X ||
          src->Register.SwizzleY != TGSI_SWIZZLE_Y ||
          src->Register.SwizzleZ != TGSI_SWIZZLE_Z ||
          src->Register.SwizzleW != TGSI_SWIZZLE_W) {
         swizzle[swz_idx++] = '.';
         swizzle[swz_idx++] = get_swiz_char(src->Register.SwizzleX);
         swizzle[swz_idx++] = get_swiz_char(src->Register.SwizzleY);
         swizzle[swz_idx++] = get_swiz_char(src->Register.SwizzleZ);
         swizzle[swz_idx++] = get_swiz_char(src->Register.SwizzleW);
      }
      if (src->Register.File == TGSI_FILE_INPUT) {
         for (uint32_t j = 0; j < ctx->num_inputs; j++)
            if (ctx->inputs[j].first == src->Register.Index) {
               if (ctx->key->color_two_side && ctx->inputs[j].name == TGSI_SEMANTIC_COLOR)
                  snprintf(srcs[i], 255, "%s(%s%s%d%s%s)", get_string(stypeprefix), prefix, "realcolor", ctx->inputs[j].sid, arrayname, swizzle);
               else if (ctx->inputs[j].glsl_gl_block) {
                  /* GS input clipdist requires a conversion */
                  if (ctx->inputs[j].name == TGSI_SEMANTIC_CLIPDIST) {
                     create_swizzled_clipdist(ctx, srcs[i], src, j, true, get_string(stypeprefix), prefix, arrayname);
                  } else {
                     snprintf(srcs[i], 255, "%s(vec4(%sgl_in%s.%s)%s)", get_string(stypeprefix), prefix, arrayname, ctx->inputs[j].glsl_name, swizzle);
                  }
               }
               else if (ctx->inputs[j].name == TGSI_SEMANTIC_PRIMID)
                  snprintf(srcs[i], 255, "%s(vec4(intBitsToFloat(%s)))", get_string(stypeprefix), ctx->inputs[j].glsl_name);
               else if (ctx->inputs[j].name == TGSI_SEMANTIC_FACE)
                  snprintf(srcs[i], 255, "%s(%s ? 1.0 : -1.0)", get_string(stypeprefix), ctx->inputs[j].glsl_name);
               else if (ctx->inputs[j].name == TGSI_SEMANTIC_CLIPDIST) {
                  create_swizzled_clipdist(ctx, srcs[i], src, j, false, get_string(stypeprefix), prefix, arrayname);
               } else {
                  enum vrend_type_qualifier srcstypeprefix = stypeprefix;
                  if ((stype == TGSI_TYPE_UNSIGNED || stype == TGSI_TYPE_SIGNED) &&
                      ctx->inputs[j].is_int)
                     srcstypeprefix = TYPE_CONVERSION_NONE;

                  if (inst->Instruction.Opcode == TGSI_OPCODE_INTERP_SAMPLE && i == 1) {
                     snprintf(srcs[i], 255, "floatBitsToInt(%s%s%s%s)", prefix, ctx->inputs[j].glsl_name, arrayname, swizzle);
                  } else if (ctx->inputs[j].name == TGSI_SEMANTIC_GENERIC &&
                             ctx_indirect_inputs(ctx)) {
                     char blkarray[32] = {};
                     fill_blkarray(ctx, src, blkarray);
                     if (src->Register.Indirect)
                        snprintf(srcs[i], 255, "%s(%sblk%s.%s%d[addr%d + %d]%s)", get_string(srcstypeprefix), prefix, blkarray, get_stage_input_name_prefix(ctx, ctx->prog_type), ctx->generic_input_range.first, src->Indirect.Index, src->Register.Index - ctx->generic_input_range.array_id, ctx->inputs[j].is_int ? "" : swizzle);
                     else
                        snprintf(srcs[i], 255, "%s(%sblk%s.%s%d[%d]%s)", get_string(srcstypeprefix), prefix, blkarray, get_stage_input_name_prefix(ctx, ctx->prog_type), ctx->generic_input_range.first, src->Register.Index - ctx->generic_input_range.array_id, ctx->inputs[j].is_int ? "" : swizzle);
                  } else if (ctx->inputs[j].name == TGSI_SEMANTIC_PATCH &&
                             ctx_indirect_inputs(ctx)) {
                     if (src->Register.Indirect)
                        snprintf(srcs[i], 255, "%s(%s%sp%d[addr%d + %d]%s)", get_string(srcstypeprefix), prefix, get_stage_input_name_prefix(ctx, ctx->prog_type), ctx->patch_input_range.first, src->Indirect.Index, src->Register.Index - ctx->patch_input_range.array_id, ctx->inputs[j].is_int ? "" : swizzle);
                     else
                        snprintf(srcs[i], 255, "%s(%s%sp%d[%d]%s)", get_string(srcstypeprefix), prefix, get_stage_input_name_prefix(ctx, ctx->prog_type), ctx->patch_input_range.first, src->Register.Index - ctx->patch_input_range.array_id, ctx->inputs[j].is_int ? "" : swizzle);
                  } else
                     snprintf(srcs[i], 255, "%s(%s%s%s%s)", get_string(srcstypeprefix), prefix, ctx->inputs[j].glsl_name, arrayname, ctx->inputs[j].is_int ? "" : swizzle);
               }
               if ((inst->Instruction.Opcode == TGSI_OPCODE_INTERP_SAMPLE ||
                    inst->Instruction.Opcode == TGSI_OPCODE_INTERP_OFFSET ||
                    inst->Instruction.Opcode == TGSI_OPCODE_INTERP_CENTROID) &&
                   i == 0) {
                  snprintf(srcs[0], 255, "%s", ctx->inputs[j].glsl_name);
                  snprintf(src_swizzle0, 10, "%s", swizzle);
               }
               sinfo->override_no_wm[i] = ctx->inputs[j].override_no_wm;
               break;
            }
      } else if (src->Register.File == TGSI_FILE_OUTPUT) {
         for (uint32_t j = 0; j < ctx->num_outputs; j++) {
            if (ctx->outputs[j].first == src->Register.Index) {
               if (inst->Instruction.Opcode == TGSI_OPCODE_FBFETCH) {
                  ctx->outputs[j].fbfetch_used = true;
                  ctx->shader_req_bits |= SHADER_REQ_FBFETCH;
               }

               enum vrend_type_qualifier srcstypeprefix = stypeprefix;
               if (stype == TGSI_TYPE_UNSIGNED && ctx->outputs[j].is_int)
                  srcstypeprefix = TYPE_CONVERSION_NONE;
               if (ctx->outputs[j].glsl_gl_block) {
                  if (ctx->outputs[j].name == TGSI_SEMANTIC_CLIPDIST) {
		     snprintf(srcs[i], 255, "clip_dist_temp[%d]", ctx->outputs[j].sid);
                  }
               } else if (ctx->outputs[j].name == TGSI_SEMANTIC_GENERIC &&
                          ctx_indirect_outputs(ctx)) {
                  char blkarray[32] = {};
                  fill_blkarray(ctx, src, blkarray);
                  if (src->Register.Indirect)
                     snprintf(srcs[i], 255, "%s(%soblk%s.%s%d[addr%d + %d]%s)", get_string(srcstypeprefix), prefix, blkarray, get_stage_output_name_prefix(ctx->prog_type), ctx->generic_output_range.first, src->Indirect.Index, src->Register.Index - ctx->generic_output_range.array_id, ctx->outputs[j].is_int ? "" : swizzle);
                  else
                     snprintf(srcs[i], 255, "%s(%soblk%s.%s%d[%d]%s)", get_string(srcstypeprefix), prefix, blkarray, get_stage_output_name_prefix(ctx->prog_type), ctx->generic_output_range.first, src->Register.Index - ctx->generic_output_range.array_id, ctx->outputs[j].is_int ? "" : swizzle);
               } else if (ctx->outputs[j].name == TGSI_SEMANTIC_PATCH &&
                          ctx_indirect_outputs(ctx)) {
                  if (src->Register.Indirect)
                     snprintf(srcs[i], 255, "%s(%s%sp%d[addr%d + %d]%s)", get_string(srcstypeprefix), prefix, get_stage_output_name_prefix(ctx->prog_type), ctx->patch_output_range.first, src->Indirect.Index, src->Register.Index - ctx->patch_output_range.array_id, ctx->outputs[j].is_int ? "" : swizzle);
                  else
                     snprintf(srcs[i], 255, "%s(%s%sp%d[%d]%s)", get_string(srcstypeprefix), prefix, get_stage_output_name_prefix(ctx->prog_type), ctx->patch_output_range.first, src->Register.Index - ctx->patch_output_range.array_id, ctx->outputs[j].is_int ? "" : swizzle);
               } else {
                  snprintf(srcs[i], 255, "%s(%s%s%s%s)", get_string(srcstypeprefix), prefix, ctx->outputs[j].glsl_name, arrayname, ctx->outputs[j].is_int ? "" : swizzle);
               }
            }
         }
      } else if (src->Register.File == TGSI_FILE_TEMPORARY) {
         struct vrend_temp_range *range = find_temp_range(ctx, src->Register.Index);
         if (!range)
            return false;
         if (inst->Instruction.Opcode == TGSI_OPCODE_INTERP_SAMPLE && i == 1) {
            stprefix = true;
            stypeprefix = FLOAT_BITS_TO_INT;
         }

         if (src->Register.Indirect) {
            assert(src->Indirect.File == TGSI_FILE_ADDRESS);
            snprintf(srcs[i], 255, "%s%c%stemp%d[addr%d + %d]%s%c", get_string(stypeprefix), stprefix ? '(' : ' ', prefix, range->first, src->Indirect.Index, src->Register.Index - range->first, swizzle, stprefix ? ')' : ' ');
         } else
            snprintf(srcs[i], 255, "%s%c%stemp%d[%d]%s%c", get_string(stypeprefix), stprefix ? '(' : ' ', prefix, range->first, src->Register.Index - range->first, swizzle, stprefix ? ')' : ' ');
      } else if (src->Register.File == TGSI_FILE_CONSTANT) {
         const char *cname = tgsi_proc_to_prefix(ctx->prog_type);
         int dim = 0;
         if (src->Register.Dimension && src->Dimension.Index != 0) {
            dim = src->Dimension.Index;
            if (src->Dimension.Indirect) {
               assert(src->DimIndirect.File == TGSI_FILE_ADDRESS);
               ctx->shader_req_bits |= SHADER_REQ_GPU_SHADER5;
               if (src->Register.Indirect) {
                  assert(src->Indirect.File == TGSI_FILE_ADDRESS);
                  snprintf(srcs[i], 255, "%s(%s%suboarr[addr%d].ubocontents[addr%d + %d]%s)", get_string(stypeprefix), prefix, cname, src->DimIndirect.Index, src->Indirect.Index, src->Register.Index, swizzle);
               } else
                  snprintf(srcs[i], 255, "%s(%s%suboarr[addr%d].ubocontents[%d]%s)", get_string(stypeprefix), prefix, cname, src->DimIndirect.Index, src->Register.Index, swizzle);
            } else {
               if (ctx->info.dimension_indirect_files & (1 << TGSI_FILE_CONSTANT)) {
                  if (src->Register.Indirect) {
                     snprintf(srcs[i], 255, "%s(%s%suboarr[%d].ubocontents[addr%d + %d]%s)", get_string(stypeprefix), prefix, cname, dim - ctx->ubo_base, src->Indirect.Index, src->Register.Index, swizzle);
                  } else
                     snprintf(srcs[i], 255, "%s(%s%suboarr[%d].ubocontents[%d]%s)", get_string(stypeprefix), prefix, cname, dim - ctx->ubo_base, src->Register.Index, swizzle);
               } else {
                  if (src->Register.Indirect) {
                     snprintf(srcs[i], 255, "%s(%s%subo%dcontents[addr0 + %d]%s)", get_string(stypeprefix), prefix, cname, dim, src->Register.Index, swizzle);
                  } else
                     snprintf(srcs[i], 255, "%s(%s%subo%dcontents[%d]%s)", get_string(stypeprefix), prefix, cname, dim, src->Register.Index, swizzle);
               }
            }
         } else {
            enum vrend_type_qualifier csp = TYPE_CONVERSION_NONE;
            ctx->shader_req_bits |= SHADER_REQ_INTS;
            if (inst->Instruction.Opcode == TGSI_OPCODE_INTERP_SAMPLE && i == 1)
               csp = IVEC4;
            else if (stype == TGSI_TYPE_FLOAT || stype == TGSI_TYPE_UNTYPED)
               csp = UINT_BITS_TO_FLOAT;
            else if (stype == TGSI_TYPE_SIGNED)
               csp = IVEC4;

            if (src->Register.Indirect) {
               snprintf(srcs[i], 255, "%s%s(%sconst%d[addr0 + %d]%s)", prefix, get_string(csp), cname, dim, src->Register.Index, swizzle);
            } else
               snprintf(srcs[i], 255, "%s%s(%sconst%d[%d]%s)", prefix, get_string(csp), cname, dim, src->Register.Index, swizzle);
         }
      } else if (src->Register.File == TGSI_FILE_SAMPLER) {
         const char *cname = tgsi_proc_to_prefix(ctx->prog_type);
         if (ctx->info.indirect_files & (1 << TGSI_FILE_SAMPLER)) {
            int basearrayidx = lookup_sampler_array(ctx, src->Register.Index);
            if (src->Register.Indirect) {
               snprintf(srcs[i], 255, "%ssamp%d[addr%d+%d]%s", cname, basearrayidx, src->Indirect.Index, src->Register.Index - basearrayidx, swizzle);
            } else {
               snprintf(srcs[i], 255, "%ssamp%d[%d]%s", cname, basearrayidx, src->Register.Index - basearrayidx, swizzle);
            }
         } else {
            snprintf(srcs[i], 255, "%ssamp%d%s", cname, src->Register.Index, swizzle);
         }
         sinfo->sreg_index = src->Register.Index;
      } else if (src->Register.File == TGSI_FILE_IMAGE) {
         const char *cname = tgsi_proc_to_prefix(ctx->prog_type);
         if (ctx->info.indirect_files & (1 << TGSI_FILE_IMAGE)) {
            int basearrayidx = lookup_image_array(ctx, src->Register.Index);
            if (src->Register.Indirect) {
               assert(src->Indirect.File == TGSI_FILE_ADDRESS);
               snprintf(srcs[i], 255, "%simg%d[addr%d + %d]", cname, basearrayidx, src->Indirect.Index, src->Register.Index - basearrayidx);
            } else
               snprintf(srcs[i], 255, "%simg%d[%d]", cname, basearrayidx, src->Register.Index - basearrayidx);
         } else
            snprintf(srcs[i], 255, "%simg%d%s", cname, src->Register.Index, swizzle);
         sinfo->sreg_index = src->Register.Index;
      } else if (src->Register.File == TGSI_FILE_BUFFER) {
         const char *cname = tgsi_proc_to_prefix(ctx->prog_type);
         if (ctx->info.indirect_files & (1 << TGSI_FILE_BUFFER)) {
            bool atomic_ssbo = ctx->ssbo_atomic_mask & (1 << src->Register.Index);
            const char *atomic_str = atomic_ssbo ? "atomic" : "";
            int base = atomic_ssbo ? ctx->ssbo_atomic_array_base : ctx->ssbo_array_base;
            if (src->Register.Indirect) {
               snprintf(srcs[i], 255, "%sssboarr%s[addr%d+%d].%sssbocontents%d%s", cname, atomic_str, src->Indirect.Index, src->Register.Index - base, cname, base, swizzle);
            } else {
               snprintf(srcs[i], 255, "%sssboarr%s[%d].%sssbocontents%d%s", cname, atomic_str, src->Register.Index - base, cname, base, swizzle);
            }
         } else {
            snprintf(srcs[i], 255, "%sssbocontents%d%s", cname, src->Register.Index, swizzle);
         }
         sinfo->sreg_index = src->Register.Index;
      } else if (src->Register.File == TGSI_FILE_MEMORY) {
         snprintf(srcs[i], 255, "values");
         sinfo->sreg_index = src->Register.Index;
      } else if (src->Register.File == TGSI_FILE_IMMEDIATE) {
         if (src->Register.Index >= (int)ARRAY_SIZE(ctx->imm)) {
            fprintf(stderr, "Immediate exceeded, max is %lu\n", ARRAY_SIZE(ctx->imm));
            return false;
         }
         struct immed *imd = &ctx->imm[src->Register.Index];
         int idx = src->Register.SwizzleX;
         char temp[48];
         enum vrend_type_qualifier vtype = VEC4;
         enum vrend_type_qualifier imm_stypeprefix = stypeprefix;

         if ((inst->Instruction.Opcode == TGSI_OPCODE_TG4 && i == 1) ||
             (inst->Instruction.Opcode == TGSI_OPCODE_INTERP_SAMPLE && i == 1))
            stype = TGSI_TYPE_SIGNED;

         if (imd->type == TGSI_IMM_UINT32 || imd->type == TGSI_IMM_INT32) {
            if (imd->type == TGSI_IMM_UINT32)
               vtype = UVEC4;
            else
               vtype = IVEC4;

            if (stype == TGSI_TYPE_UNSIGNED && imd->type == TGSI_IMM_INT32)
               imm_stypeprefix = UVEC4;
            else if (stype == TGSI_TYPE_SIGNED && imd->type == TGSI_IMM_UINT32)
               imm_stypeprefix = IVEC4;
            else if (stype == TGSI_TYPE_FLOAT || stype == TGSI_TYPE_UNTYPED) {
               if (imd->type == TGSI_IMM_INT32)
                  imm_stypeprefix = INT_BITS_TO_FLOAT;
               else
                  imm_stypeprefix = UINT_BITS_TO_FLOAT;
            } else if (stype == TGSI_TYPE_UNSIGNED || stype == TGSI_TYPE_SIGNED)
               imm_stypeprefix = TYPE_CONVERSION_NONE;
         } else if (imd->type == TGSI_IMM_FLOAT64) {
            vtype = UVEC4;
            if (stype == TGSI_TYPE_DOUBLE)
               imm_stypeprefix = TYPE_CONVERSION_NONE;
            else
               imm_stypeprefix = UINT_BITS_TO_FLOAT;
         }

         /* build up a vec4 of immediates */
         snprintf(srcs[i], 255, "%s(%s%s(", get_string(imm_stypeprefix), prefix, get_string(vtype));
         for (uint32_t j = 0; j < 4; j++) {
            if (j == 0)
               idx = src->Register.SwizzleX;
            else if (j == 1)
               idx = src->Register.SwizzleY;
            else if (j == 2)
               idx = src->Register.SwizzleZ;
            else if (j == 3)
               idx = src->Register.SwizzleW;

            if (inst->Instruction.Opcode == TGSI_OPCODE_TG4 && i == 1 && j == 0) {
               if (imd->val[idx].ui > 0) {
                  sinfo->tg4_has_component = true;
                  if (!ctx->cfg->use_gles)
                     ctx->shader_req_bits |= SHADER_REQ_GPU_SHADER5;
               }
            }

            switch (imd->type) {
            case TGSI_IMM_FLOAT32:
               if (isinf(imd->val[idx].f) || isnan(imd->val[idx].f)) {
                  ctx->shader_req_bits |= SHADER_REQ_INTS;
                  snprintf(temp, 48, "uintBitsToFloat(%uU)", imd->val[idx].ui);
               } else
                  snprintf(temp, 25, "%.8g", imd->val[idx].f);
               break;
            case TGSI_IMM_UINT32:
               snprintf(temp, 25, "%uU", imd->val[idx].ui);
               break;
            case TGSI_IMM_INT32:
               snprintf(temp, 25, "%d", imd->val[idx].i);
               sinfo->imm_value = imd->val[idx].i;
               break;
            case TGSI_IMM_FLOAT64:
               snprintf(temp, 48, "%uU", imd->val[idx].ui);
               break;
            default:
               fprintf(stderr, "unhandled imm type: %x\n", imd->type);
               return false;
            }
            strncat(srcs[i], temp, 255);
            if (j < 3)
               strcat(srcs[i], ",");
            else {
               snprintf(temp, 4, "))%c", isfloatabsolute ? ')' : 0);
               strncat(srcs[i], temp, 255);
            }
         }
      } else if (src->Register.File == TGSI_FILE_SYSTEM_VALUE) {
         for (uint32_t j = 0; j < ctx->num_system_values; j++)
            if (ctx->system_values[j].first == src->Register.Index) {
               if (ctx->system_values[j].name == TGSI_SEMANTIC_VERTEXID ||
                   ctx->system_values[j].name == TGSI_SEMANTIC_INSTANCEID ||
                   ctx->system_values[j].name == TGSI_SEMANTIC_PRIMID ||
                   ctx->system_values[j].name == TGSI_SEMANTIC_VERTICESIN ||
                   ctx->system_values[j].name == TGSI_SEMANTIC_INVOCATIONID ||
                   ctx->system_values[j].name == TGSI_SEMANTIC_SAMPLEID) {
                  if (inst->Instruction.Opcode == TGSI_OPCODE_INTERP_SAMPLE && i == 1)
                     snprintf(srcs[i], 255, "ivec4(%s)", ctx->system_values[j].glsl_name);
                  else
                     snprintf(srcs[i], 255, "%s(vec4(intBitsToFloat(%s)))", get_string(stypeprefix), ctx->system_values[j].glsl_name);
               } else if (ctx->system_values[j].name == TGSI_SEMANTIC_HELPER_INVOCATION) {
                  snprintf(srcs[i], 255, "uvec4(%s)", ctx->system_values[j].glsl_name);
               } else if (ctx->system_values[j].name == TGSI_SEMANTIC_TESSINNER ||
                        ctx->system_values[j].name == TGSI_SEMANTIC_TESSOUTER) {
                  snprintf(srcs[i], 255, "%s(vec4(%s[%d], %s[%d], %s[%d], %s[%d]))",
                           prefix,
                           ctx->system_values[j].glsl_name, src->Register.SwizzleX,
                           ctx->system_values[j].glsl_name, src->Register.SwizzleY,
                           ctx->system_values[j].glsl_name, src->Register.SwizzleZ,
                           ctx->system_values[j].glsl_name, src->Register.SwizzleW);
               } else if (ctx->system_values[j].name == TGSI_SEMANTIC_SAMPLEPOS) {
                  /* gl_SamplePosition is a vec2, but TGSI_SEMANTIC_SAMPLEPOS
                   * is a vec4 with z = w = 0
                   */
                  const char *components[4] = {
                     "gl_SamplePosition.x", "gl_SamplePosition.y", "0.0", "0.0"
                  };
                  snprintf(srcs[i], 255, "%s(vec4(%s, %s, %s, %s))",
                           prefix,
                           components[src->Register.SwizzleX],
                           components[src->Register.SwizzleY],
                           components[src->Register.SwizzleZ],
                           components[src->Register.SwizzleW]);
               } else if (ctx->system_values[j].name == TGSI_SEMANTIC_TESSCOORD) {
                  snprintf(srcs[i], 255, "%s(vec4(%s.%c, %s.%c, %s.%c, %s.%c))",
                           prefix,
                           ctx->system_values[j].glsl_name, get_swiz_char(src->Register.SwizzleX),
                           ctx->system_values[j].glsl_name, get_swiz_char(src->Register.SwizzleY),
                           ctx->system_values[j].glsl_name, get_swiz_char(src->Register.SwizzleZ),
                           ctx->system_values[j].glsl_name, get_swiz_char(src->Register.SwizzleW));
               } else if (ctx->system_values[j].name == TGSI_SEMANTIC_GRID_SIZE ||
                          ctx->system_values[j].name == TGSI_SEMANTIC_THREAD_ID ||
                          ctx->system_values[j].name == TGSI_SEMANTIC_BLOCK_ID) {
                  snprintf(srcs[i], 255, "uvec4(%s.%c, %s.%c, %s.%c, %s.%c)",
                           ctx->system_values[j].glsl_name, get_swiz_char(src->Register.SwizzleX),
                           ctx->system_values[j].glsl_name, get_swiz_char(src->Register.SwizzleY),
                           ctx->system_values[j].glsl_name, get_swiz_char(src->Register.SwizzleZ),
                           ctx->system_values[j].glsl_name, get_swiz_char(src->Register.SwizzleW));
                  sinfo->override_no_cast[i] = true;
               } else if (ctx->system_values[j].name == TGSI_SEMANTIC_SAMPLEMASK) {
                  const char *vec_type = "ivec4";
                  if (ctx->cfg->use_gles &&
                      (inst->Instruction.Opcode == TGSI_OPCODE_AND) &&
                      (stype == TGSI_TYPE_UNSIGNED))
                     vec_type = "uvec4";
                  ctx->shader_req_bits |= SHADER_REQ_SAMPLE_SHADING | SHADER_REQ_INTS;
                  snprintf(srcs[i], 255, "%s(%s, %s, %s, %s)",
                     vec_type,
                     src->Register.SwizzleX == TGSI_SWIZZLE_X ? ctx->system_values[j].glsl_name : "0",
                     src->Register.SwizzleY == TGSI_SWIZZLE_X ? ctx->system_values[j].glsl_name : "0",
                     src->Register.SwizzleZ == TGSI_SWIZZLE_X ? ctx->system_values[j].glsl_name : "0",
                     src->Register.SwizzleW == TGSI_SWIZZLE_X ? ctx->system_values[j].glsl_name : "0");
               } else
                  snprintf(srcs[i], 255, "%s%s", prefix, ctx->system_values[j].glsl_name);
               sinfo->override_no_wm[i] = ctx->system_values[j].override_no_wm;
               break;
            }
      } else if (src->Register.File == TGSI_FILE_HW_ATOMIC) {
         for (uint32_t j = 0; j < ctx->num_abo; j++) {
            if (src->Dimension.Index == ctx->abo_idx[j] &&
                src->Register.Index >= ctx->abo_offsets[j] &&
                src->Register.Index < ctx->abo_offsets[j] + ctx->abo_sizes[j]) {
               if (ctx->abo_sizes[j] > 1) {
                  int offset = src->Register.Index - ctx->abo_offsets[j];
                  if (src->Register.Indirect) {
                     assert(src->Indirect.File == TGSI_FILE_ADDRESS);
                     snprintf(srcs[i], 255, "ac%d[addr%d + %d]", j, src->Indirect.Index, offset);
                  } else
                     snprintf(srcs[i], 255, "ac%d[%d]", j, offset);
               } else
                  snprintf(srcs[i], 255, "ac%d", j);
               break;
            }
         }
         sinfo->sreg_index = src->Register.Index;
      }

      if (stype == TGSI_TYPE_DOUBLE) {
         boolean isabsolute = src->Register.Absolute;
         strcpy(fp64_src, srcs[i]);
         snprintf(srcs[i], 255, "fp64_src[%d]", i);
         emit_buff(ctx, "%s.x = %spackDouble2x32(uvec2(%s%s))%s;\n", srcs[i], isabsolute ? "abs(" : "", fp64_src, swizzle, isabsolute ? ")" : "");
      }
   }

   return true;
}

static boolean
iter_instruction(struct tgsi_iterate_context *iter,
                 struct tgsi_full_instruction *inst)
{
   struct dump_ctx *ctx = (struct dump_ctx *)iter;
   struct dest_info dinfo = { 0 };
   struct source_info sinfo = { 0 };
   char srcs[4][255], dsts[3][255];
   char fp64_dsts[3][255];
   uint instno = ctx->instno++;
   char writemask[6] = {0};
   char src_swizzle0[10];

   sinfo.svec4 = VEC4;

   if (ctx->prog_type == -1)
      ctx->prog_type = iter->processor.Processor;

   if (instno == 0) {
      emit_buf(ctx, "void main(void)\n{\n");
      if (iter->processor.Processor == TGSI_PROCESSOR_FRAGMENT) {
         emit_color_select(ctx);
      }
      if (ctx->so)
         prepare_so_movs(ctx);
   }

   if (!get_destination_info(ctx, inst, &dinfo, dsts, fp64_dsts, writemask))
      return false;

   if (!get_source_info(ctx, inst, &sinfo, srcs, src_swizzle0))
      return false;

   switch (inst->Instruction.Opcode) {
   case TGSI_OPCODE_SQRT:
   case TGSI_OPCODE_DSQRT:
      emit_buff(ctx, "%s = sqrt(vec4(%s))%s;\n", dsts[0], srcs[0], writemask);
      break;
   case TGSI_OPCODE_LRP:
      emit_buff(ctx, "%s = mix(vec4(%s), vec4(%s), vec4(%s))%s;\n", dsts[0], srcs[2], srcs[1], srcs[0], writemask);
      break;
   case TGSI_OPCODE_DP2:
      emit_buff(ctx, "%s = %s(dot(vec2(%s), vec2(%s)));\n", dsts[0], get_string(dinfo.dstconv), srcs[0], srcs[1]);
      break;
   case TGSI_OPCODE_DP3:
      emit_buff(ctx, "%s = %s(dot(vec3(%s), vec3(%s)));\n", dsts[0], get_string(dinfo.dstconv), srcs[0], srcs[1]);
      break;
   case TGSI_OPCODE_DP4:
      emit_buff(ctx, "%s = %s(dot(vec4(%s), vec4(%s)));\n", dsts[0], get_string(dinfo.dstconv), srcs[0], srcs[1]);
      break;
   case TGSI_OPCODE_DPH:
      emit_buff(ctx, "%s = %s(dot(vec4(vec3(%s), 1.0), vec4(%s)));\n", dsts[0], get_string(dinfo.dstconv), srcs[0], srcs[1]);
      break;
   case TGSI_OPCODE_MAX:
   case TGSI_OPCODE_DMAX:
   case TGSI_OPCODE_IMAX:
   case TGSI_OPCODE_UMAX:
      emit_buff(ctx, "%s = %s(%s(max(%s, %s)));\n", dsts[0], get_string(dinfo.dstconv), get_string(dinfo.dtypeprefix), srcs[0], srcs[1]);
      break;
   case TGSI_OPCODE_MIN:
   case TGSI_OPCODE_DMIN:
   case TGSI_OPCODE_IMIN:
   case TGSI_OPCODE_UMIN:
      emit_buff(ctx, "%s = %s(%s(min(%s, %s)));\n", dsts[0], get_string(dinfo.dstconv), get_string(dinfo.dtypeprefix), srcs[0], srcs[1]);
      break;
   case TGSI_OPCODE_ABS:
   case TGSI_OPCODE_IABS:
   case TGSI_OPCODE_DABS:
      emit_op1("abs");
      break;
   case TGSI_OPCODE_KILL_IF:
      emit_buff(ctx, "if (any(lessThan(%s, vec4(0.0))))\ndiscard;\n", srcs[0]);
      break;
   case TGSI_OPCODE_IF:
   case TGSI_OPCODE_UIF:
      emit_buff(ctx, "if (any(bvec4(%s))) {\n", srcs[0]);
      indent_buf(ctx);
      break;
   case TGSI_OPCODE_ELSE:
      outdent_buf(ctx);
      emit_buf(ctx, "} else {\n");
      indent_buf(ctx);
      break;
   case TGSI_OPCODE_ENDIF:
      emit_buf(ctx, "}\n");
      outdent_buf(ctx);
      break;
   case TGSI_OPCODE_KILL:
      emit_buff(ctx, "discard;\n");
      break;
   case TGSI_OPCODE_DST:
      emit_buff(ctx, "%s = vec4(1.0, %s.y * %s.y, %s.z, %s.w);\n", dsts[0],
               srcs[0], srcs[1], srcs[0], srcs[1]);
      break;
   case TGSI_OPCODE_LIT:
      emit_buff(ctx, "%s = %s(vec4(1.0, max(%s.x, 0.0), step(0.0, %s.x) * pow(max(0.0, %s.y), clamp(%s.w, -128.0, 128.0)), 1.0)%s);\n", dsts[0], get_string(dinfo.dstconv), srcs[0], srcs[0], srcs[0], srcs[0], writemask);
      break;
   case TGSI_OPCODE_EX2:
      emit_op1("exp2");
      break;
   case TGSI_OPCODE_LG2:
      emit_op1("log2");
      break;
   case TGSI_OPCODE_EXP:
      emit_buff(ctx, "%s = %s(vec4(pow(2.0, floor(%s.x)), %s.x - floor(%s.x), exp2(%s.x), 1.0)%s);\n", dsts[0], get_string(dinfo.dstconv), srcs[0], srcs[0], srcs[0], srcs[0], writemask);
      break;
   case TGSI_OPCODE_LOG:
      emit_buff(ctx, "%s = %s(vec4(floor(log2(%s.x)), %s.x / pow(2.0, floor(log2(%s.x))), log2(%s.x), 1.0)%s);\n", dsts[0], get_string(dinfo.dstconv), srcs[0], srcs[0], srcs[0], srcs[0], writemask);
      break;
   case TGSI_OPCODE_COS:
      emit_op1("cos");
      break;
   case TGSI_OPCODE_SIN:
      emit_op1("sin");
      break;
   case TGSI_OPCODE_SCS:
      emit_buff(ctx, "%s = %s(vec4(cos(%s.x), sin(%s.x), 0, 1)%s);\n", dsts[0], get_string(dinfo.dstconv),
               srcs[0], srcs[0], writemask);
      break;
   case TGSI_OPCODE_DDX:
      emit_op1("dFdx");
      break;
   case TGSI_OPCODE_DDY:
      emit_op1("dFdy");
      break;
   case TGSI_OPCODE_DDX_FINE:
      ctx->shader_req_bits |= SHADER_REQ_DERIVATIVE_CONTROL;
      emit_op1("dFdxFine");
      break;
   case TGSI_OPCODE_DDY_FINE:
      ctx->shader_req_bits |= SHADER_REQ_DERIVATIVE_CONTROL;
      emit_op1("dFdyFine");
      break;
   case TGSI_OPCODE_RCP:
      emit_buff(ctx, "%s = %s(1.0/(%s));\n", dsts[0], get_string(dinfo.dstconv), srcs[0]);
      break;
   case TGSI_OPCODE_DRCP:
      emit_buff(ctx, "%s = %s(1.0LF/(%s));\n", dsts[0], get_string(dinfo.dstconv), srcs[0]);
      break;
   case TGSI_OPCODE_FLR:
      emit_op1("floor");
      break;
   case TGSI_OPCODE_ROUND:
      emit_op1("round");
      break;
   case TGSI_OPCODE_ISSG:
      emit_op1("sign");
      break;
   case TGSI_OPCODE_CEIL:
      emit_op1("ceil");
      break;
   case TGSI_OPCODE_FRC:
   case TGSI_OPCODE_DFRAC:
      emit_op1("fract");
      break;
   case TGSI_OPCODE_TRUNC:
      emit_op1("trunc");
      break;
   case TGSI_OPCODE_SSG:
      emit_op1("sign");
      break;
   case TGSI_OPCODE_RSQ:
   case TGSI_OPCODE_DRSQ:
      emit_buff(ctx, "%s = %s(inversesqrt(%s.x));\n", dsts[0], get_string(dinfo.dstconv), srcs[0]);
      break;
   case TGSI_OPCODE_FBFETCH:
   case TGSI_OPCODE_MOV:
      emit_buff(ctx, "%s = %s(%s(%s%s));\n", dsts[0], get_string(dinfo.dstconv), get_string(dinfo.dtypeprefix), srcs[0], sinfo.override_no_wm[0] ? "" : writemask);
      break;
   case TGSI_OPCODE_ADD:
   case TGSI_OPCODE_DADD:
      emit_arit_op2("+");
      break;
   case TGSI_OPCODE_UADD:
      emit_buff(ctx, "%s = %s(%s(ivec4((uvec4(%s) + uvec4(%s))))%s);\n", dsts[0], get_string(dinfo.dstconv), get_string(dinfo.dtypeprefix), srcs[0], srcs[1], writemask);
      break;
   case TGSI_OPCODE_SUB:
      emit_arit_op2("-");
      break;
   case TGSI_OPCODE_MUL:
   case TGSI_OPCODE_DMUL:
      emit_arit_op2("*");
      break;
   case TGSI_OPCODE_DIV:
   case TGSI_OPCODE_DDIV:
      emit_arit_op2("/");
      break;
   case TGSI_OPCODE_UMUL:
      emit_buff(ctx, "%s = %s(%s((uvec4(%s) * uvec4(%s)))%s);\n", dsts[0], get_string(dinfo.dstconv), get_string(dinfo.dtypeprefix), srcs[0], srcs[1], writemask);
      break;
   case TGSI_OPCODE_UMOD:
      emit_buff(ctx, "%s = %s(%s((uvec4(%s) %% uvec4(%s)))%s);\n", dsts[0], get_string(dinfo.dstconv), get_string(dinfo.dtypeprefix), srcs[0], srcs[1], writemask);
      break;
   case TGSI_OPCODE_IDIV:
      emit_buff(ctx, "%s = %s(%s((ivec4(%s) / ivec4(%s)))%s);\n", dsts[0], get_string(dinfo.dstconv), get_string(dinfo.dtypeprefix), srcs[0], srcs[1], writemask);
      break;
   case TGSI_OPCODE_UDIV:
      emit_buff(ctx, "%s = %s(%s((uvec4(%s) / uvec4(%s)))%s);\n", dsts[0], get_string(dinfo.dstconv), get_string(dinfo.dtypeprefix), srcs[0], srcs[1], writemask);
      break;
   case TGSI_OPCODE_ISHR:
   case TGSI_OPCODE_USHR:
      emit_arit_op2(">>");
      break;
   case TGSI_OPCODE_SHL:
      emit_arit_op2("<<");
      break;
   case TGSI_OPCODE_MAD:
      emit_buff(ctx, "%s = %s((%s * %s + %s)%s);\n", dsts[0], get_string(dinfo.dstconv), srcs[0], srcs[1], srcs[2], writemask);
      break;
   case TGSI_OPCODE_UMAD:
   case TGSI_OPCODE_DMAD:
      emit_buff(ctx, "%s = %s(%s((%s * %s + %s)%s));\n", dsts[0], get_string(dinfo.dstconv), get_string(dinfo.dtypeprefix), srcs[0], srcs[1], srcs[2], writemask);
      break;
   case TGSI_OPCODE_OR:
      emit_arit_op2("|");
      break;
   case TGSI_OPCODE_AND:
      emit_arit_op2("&");
      break;
   case TGSI_OPCODE_XOR:
      emit_arit_op2("^");
      break;
   case TGSI_OPCODE_MOD:
      emit_arit_op2("%");
      break;
   case TGSI_OPCODE_TEX:
   case TGSI_OPCODE_TEX2:
   case TGSI_OPCODE_TXB:
   case TGSI_OPCODE_TXL:
   case TGSI_OPCODE_TXB2:
   case TGSI_OPCODE_TXL2:
   case TGSI_OPCODE_TXD:
   case TGSI_OPCODE_TXF:
   case TGSI_OPCODE_TG4:
   case TGSI_OPCODE_TXP:
   case TGSI_OPCODE_LODQ:
      translate_tex(ctx, inst, &sinfo, &dinfo, srcs, dsts, writemask);
      break;
   case TGSI_OPCODE_TXQ:
      emit_txq(ctx, inst, sinfo.sreg_index, srcs, dsts, writemask);
      break;
   case TGSI_OPCODE_TXQS:
      emit_txqs(ctx, inst, sinfo.sreg_index, srcs, dsts);
      break;
   case TGSI_OPCODE_I2F:
      emit_buff(ctx, "%s = %s(ivec4(%s)%s);\n", dsts[0], get_string(dinfo.dstconv), srcs[0], writemask);
      break;
   case TGSI_OPCODE_I2D:
      emit_buff(ctx, "%s = %s(ivec4(%s));\n", dsts[0], get_string(dinfo.dstconv), srcs[0]);
      break;
   case TGSI_OPCODE_D2F:
      emit_buff(ctx, "%s = %s(%s);\n", dsts[0], get_string(dinfo.dstconv), srcs[0]);
      break;
   case TGSI_OPCODE_U2F:
      emit_buff(ctx, "%s = %s(uvec4(%s)%s);\n", dsts[0], get_string(dinfo.dstconv), srcs[0], writemask);
      break;
   case TGSI_OPCODE_U2D:
      emit_buff(ctx, "%s = %s(uvec4(%s));\n", dsts[0], get_string(dinfo.dstconv), srcs[0]);
      break;
   case TGSI_OPCODE_F2I:
      emit_buff(ctx, "%s = %s(%s(ivec4(%s))%s);\n", dsts[0], get_string(dinfo.dstconv), get_string(dinfo.dtypeprefix), srcs[0], writemask);
      break;
   case TGSI_OPCODE_D2I:
      emit_buff(ctx, "%s = %s(%s(%s(%s)));\n", dsts[0], get_string(dinfo.dstconv), get_string(dinfo.dtypeprefix), get_string(dinfo.idstconv), srcs[0]);
      break;
   case TGSI_OPCODE_F2U:
      emit_buff(ctx, "%s = %s(%s(uvec4(%s))%s);\n", dsts[0], get_string(dinfo.dstconv), get_string(dinfo.dtypeprefix), srcs[0], writemask);
      break;
   case TGSI_OPCODE_D2U:
      emit_buff(ctx, "%s = %s(%s(%s(%s)));\n", dsts[0], get_string(dinfo.dstconv), get_string(dinfo.dtypeprefix), get_string(dinfo.udstconv), srcs[0]);
      break;
   case TGSI_OPCODE_F2D:
      emit_buff(ctx, "%s = %s(%s(%s));\n", dsts[0], get_string(dinfo.dstconv), get_string(dinfo.dtypeprefix), srcs[0]);
      break;
   case TGSI_OPCODE_NOT:
      emit_buff(ctx, "%s = %s(uintBitsToFloat(~(uvec4(%s))));\n", dsts[0], get_string(dinfo.dstconv), srcs[0]);
      break;
   case TGSI_OPCODE_INEG:
      emit_buff(ctx, "%s = %s(intBitsToFloat(-(ivec4(%s))));\n", dsts[0], get_string(dinfo.dstconv), srcs[0]);
      break;
   case TGSI_OPCODE_DNEG:
      emit_buff(ctx, "%s = %s(-%s);\n", dsts[0], get_string(dinfo.dstconv), srcs[0]);
      break;
   case TGSI_OPCODE_SEQ:
      emit_compare("equal");
      break;
   case TGSI_OPCODE_USEQ:
   case TGSI_OPCODE_FSEQ:
   case TGSI_OPCODE_DSEQ:
      if (inst->Instruction.Opcode == TGSI_OPCODE_DSEQ)
         strcpy(writemask, ".x");
      emit_ucompare("equal");
      break;
   case TGSI_OPCODE_SLT:
      emit_compare("lessThan");
      break;
   case TGSI_OPCODE_ISLT:
   case TGSI_OPCODE_USLT:
   case TGSI_OPCODE_FSLT:
   case TGSI_OPCODE_DSLT:
      if (inst->Instruction.Opcode == TGSI_OPCODE_DSLT)
         strcpy(writemask, ".x");
      emit_ucompare("lessThan");
      break;
   case TGSI_OPCODE_SNE:
      emit_compare("notEqual");
      break;
   case TGSI_OPCODE_USNE:
   case TGSI_OPCODE_FSNE:
   case TGSI_OPCODE_DSNE:
      if (inst->Instruction.Opcode == TGSI_OPCODE_DSNE)
         strcpy(writemask, ".x");
      emit_ucompare("notEqual");
      break;
   case TGSI_OPCODE_SGE:
      emit_compare("greaterThanEqual");
      break;
   case TGSI_OPCODE_ISGE:
   case TGSI_OPCODE_USGE:
   case TGSI_OPCODE_FSGE:
   case TGSI_OPCODE_DSGE:
      if (inst->Instruction.Opcode == TGSI_OPCODE_DSGE)
          strcpy(writemask, ".x");
      emit_ucompare("greaterThanEqual");
      break;
   case TGSI_OPCODE_POW:
      emit_buff(ctx, "%s = %s(pow(%s, %s));\n", dsts[0], get_string(dinfo.dstconv), srcs[0], srcs[1]);
      break;
   case TGSI_OPCODE_CMP:
      emit_buff(ctx, "%s = mix(%s, %s, greaterThanEqual(%s, vec4(0.0)))%s;\n", dsts[0], srcs[1], srcs[2], srcs[0], writemask);
      break;
   case TGSI_OPCODE_UCMP:
      emit_buff(ctx, "%s = mix(%s, %s, notEqual(floatBitsToUint(%s), uvec4(0.0)))%s;\n", dsts[0], srcs[2], srcs[1], srcs[0], writemask);
      break;
   case TGSI_OPCODE_END:
      if (iter->processor.Processor == TGSI_PROCESSOR_VERTEX) {
         handle_vertex_proc_exit(ctx);
      } else if (iter->processor.Processor == TGSI_PROCESSOR_TESS_CTRL) {
         emit_clip_dist_movs(ctx);
      } else if (iter->processor.Processor == TGSI_PROCESSOR_TESS_EVAL) {
	 if (ctx->so && !ctx->key->gs_present)
            emit_so_movs(ctx);
         emit_clip_dist_movs(ctx);
         if (!ctx->key->gs_present) {
            emit_prescale(ctx);
         }
      } else if (iter->processor.Processor == TGSI_PROCESSOR_FRAGMENT) {
         handle_fragment_proc_exit(ctx);
      }
      emit_buf(ctx, "}\n");
      break;
   case TGSI_OPCODE_RET:
      if (iter->processor.Processor == TGSI_PROCESSOR_VERTEX) {
         handle_vertex_proc_exit(ctx);
      } else if (iter->processor.Processor == TGSI_PROCESSOR_FRAGMENT) {
         handle_fragment_proc_exit(ctx);
      }
      emit_buf(ctx, "return;\n");
      break;
   case TGSI_OPCODE_ARL:
      emit_buff(ctx, "%s = int(floor(%s)%s);\n", dsts[0], srcs[0], writemask);
      break;
   case TGSI_OPCODE_UARL:
      emit_buff(ctx, "%s = int(%s);\n", dsts[0], srcs[0]);
      break;
   case TGSI_OPCODE_XPD:
      emit_buff(ctx, "%s = %s(cross(vec3(%s), vec3(%s)));\n", dsts[0], get_string(dinfo.dstconv), srcs[0], srcs[1]);
      break;
   case TGSI_OPCODE_BGNLOOP:
      emit_buf(ctx, "do {\n");
      indent_buf(ctx);
      break;
   case TGSI_OPCODE_ENDLOOP:
      outdent_buf(ctx);
      emit_buf(ctx, "} while(true);\n");
      break;
   case TGSI_OPCODE_BRK:
      emit_buf(ctx, "break;\n");
      break;
   case TGSI_OPCODE_EMIT: {
      struct immed *imd = &ctx->imm[(inst->Src[0].Register.Index)];
      if (ctx->so && ctx->key->gs_present)
         emit_so_movs(ctx);
      emit_clip_dist_movs(ctx);
      emit_prescale(ctx);
      if (imd->val[inst->Src[0].Register.SwizzleX].ui > 0) {
         ctx->shader_req_bits |= SHADER_REQ_GPU_SHADER5;
         emit_buff(ctx, "EmitStreamVertex(%d);\n", imd->val[inst->Src[0].Register.SwizzleX].ui);
      } else
         emit_buf(ctx, "EmitVertex();\n");
      break;
   }
   case TGSI_OPCODE_ENDPRIM: {
      struct immed *imd = &ctx->imm[(inst->Src[0].Register.Index)];
      if (imd->val[inst->Src[0].Register.SwizzleX].ui > 0) {
         ctx->shader_req_bits |= SHADER_REQ_GPU_SHADER5;
         emit_buff(ctx, "EndStreamPrimitive(%d);\n", imd->val[inst->Src[0].Register.SwizzleX].ui);
      } else
         emit_buf(ctx, "EndPrimitive();\n");
      break;
   }
   case TGSI_OPCODE_INTERP_CENTROID:
      emit_buff(ctx, "%s = %s(%s(vec4(interpolateAtCentroid(%s))%s));\n", dsts[0], get_string(dinfo.dstconv), get_string(dinfo.dtypeprefix), srcs[0], src_swizzle0);
      ctx->shader_req_bits |= SHADER_REQ_GPU_SHADER5;
      break;
   case TGSI_OPCODE_INTERP_SAMPLE:
      emit_buff(ctx, "%s = %s(%s(vec4(interpolateAtSample(%s, %s.x))%s));\n", dsts[0], get_string(dinfo.dstconv), get_string(dinfo.dtypeprefix), srcs[0], srcs[1], src_swizzle0);
      ctx->shader_req_bits |= SHADER_REQ_GPU_SHADER5;
      break;
   case TGSI_OPCODE_INTERP_OFFSET:
      emit_buff(ctx, "%s = %s(%s(vec4(interpolateAtOffset(%s, %s.xy))%s));\n", dsts[0], get_string(dinfo.dstconv), get_string(dinfo.dtypeprefix), srcs[0], srcs[1], src_swizzle0);
      ctx->shader_req_bits |= SHADER_REQ_GPU_SHADER5;
      break;
   case TGSI_OPCODE_UMUL_HI:
      emit_buff(ctx, "umulExtended(%s, %s, umul_temp, mul_utemp);\n", srcs[0], srcs[1]);
      emit_buff(ctx, "%s = %s(%s(umul_temp));\n", dsts[0], get_string(dinfo.dstconv), get_string(dinfo.dtypeprefix));
      ctx->shader_req_bits |= SHADER_REQ_GPU_SHADER5;
      ctx->write_mul_utemp = true;
      break;
   case TGSI_OPCODE_IMUL_HI:
      emit_buff(ctx, "imulExtended(%s, %s, imul_temp, mul_itemp);\n", srcs[0], srcs[1]);
      emit_buff(ctx, "%s = %s(%s(imul_temp));\n", dsts[0], get_string(dinfo.dstconv), get_string(dinfo.dtypeprefix));
      ctx->shader_req_bits |= SHADER_REQ_GPU_SHADER5;
      ctx->write_mul_itemp = true;
      break;

   case TGSI_OPCODE_IBFE:
      emit_buff(ctx, "%s = %s(%s(bitfieldExtract(%s, int(%s.x), int(%s.x))));\n", dsts[0], get_string(dinfo.dstconv), get_string(dinfo.dtypeprefix), srcs[0], srcs[1], srcs[2]);
      ctx->shader_req_bits |= SHADER_REQ_GPU_SHADER5;
      break;
   case TGSI_OPCODE_UBFE:
      emit_buff(ctx, "%s = %s(%s(bitfieldExtract(%s, int(%s.x), int(%s.x))));\n", dsts[0], get_string(dinfo.dstconv), get_string(dinfo.dtypeprefix), srcs[0], srcs[1], srcs[2]);
      ctx->shader_req_bits |= SHADER_REQ_GPU_SHADER5;
      break;
   case TGSI_OPCODE_BFI:
      emit_buff(ctx, "%s = %s(uintBitsToFloat(bitfieldInsert(%s, %s, int(%s), int(%s))));\n", dsts[0], get_string(dinfo.dstconv), srcs[0], srcs[1], srcs[2], srcs[3]);
      ctx->shader_req_bits |= SHADER_REQ_GPU_SHADER5;
      break;
   case TGSI_OPCODE_BREV:
      emit_buff(ctx, "%s = %s(%s(bitfieldReverse(%s)));\n", dsts[0], get_string(dinfo.dstconv), get_string(dinfo.dtypeprefix), srcs[0]);
      ctx->shader_req_bits |= SHADER_REQ_GPU_SHADER5;
      break;
   case TGSI_OPCODE_POPC:
      emit_buff(ctx, "%s = %s(%s(bitCount(%s)));\n", dsts[0], get_string(dinfo.dstconv), get_string(dinfo.dtypeprefix), srcs[0]);
      ctx->shader_req_bits |= SHADER_REQ_GPU_SHADER5;
      break;
   case TGSI_OPCODE_LSB:
      emit_buff(ctx, "%s = %s(%s(findLSB(%s)));\n", dsts[0], get_string(dinfo.dstconv), get_string(dinfo.dtypeprefix), srcs[0]);
      ctx->shader_req_bits |= SHADER_REQ_GPU_SHADER5;
      break;
   case TGSI_OPCODE_IMSB:
   case TGSI_OPCODE_UMSB:
      emit_buff(ctx, "%s = %s(%s(findMSB(%s)));\n", dsts[0], get_string(dinfo.dstconv), get_string(dinfo.dtypeprefix), srcs[0]);
      ctx->shader_req_bits |= SHADER_REQ_GPU_SHADER5;
      break;
   case TGSI_OPCODE_BARRIER:
      emit_buf(ctx, "barrier();\n");
      break;
   case TGSI_OPCODE_MEMBAR: {
      struct immed *imd = &ctx->imm[(inst->Src[0].Register.Index)];
      uint32_t val = imd->val[inst->Src[0].Register.SwizzleX].ui;
      uint32_t all_val = (TGSI_MEMBAR_SHADER_BUFFER |
                          TGSI_MEMBAR_ATOMIC_BUFFER |
                          TGSI_MEMBAR_SHADER_IMAGE |
                          TGSI_MEMBAR_SHARED);

      if (val & TGSI_MEMBAR_THREAD_GROUP) {
         emit_buf(ctx, "groupMemoryBarrier();\n");
      } else {
         if ((val & all_val) == all_val) {
            emit_buf(ctx, "memoryBarrier();\n");
         } else {
            if (val & TGSI_MEMBAR_SHADER_BUFFER) {
               emit_buf(ctx, "memoryBarrierBuffer();\n");
            }
            if (val & TGSI_MEMBAR_ATOMIC_BUFFER) {
               emit_buf(ctx, "memoryBarrierAtomic();\n");
            }
            if (val & TGSI_MEMBAR_SHADER_IMAGE) {
               emit_buf(ctx, "memoryBarrierImage();\n");
            }
            if (val & TGSI_MEMBAR_SHARED) {
               emit_buf(ctx, "memoryBarrierShared();\n");
            }
         }
      }
      break;
   }
   case TGSI_OPCODE_STORE:
      translate_store(ctx, inst, &sinfo, srcs, dsts);
      break;
   case TGSI_OPCODE_LOAD:
      translate_load(ctx, inst, &sinfo, &dinfo, srcs, dsts, writemask);
      break;
   case TGSI_OPCODE_ATOMUADD:
   case TGSI_OPCODE_ATOMXCHG:
   case TGSI_OPCODE_ATOMCAS:
   case TGSI_OPCODE_ATOMAND:
   case TGSI_OPCODE_ATOMOR:
   case TGSI_OPCODE_ATOMXOR:
   case TGSI_OPCODE_ATOMUMIN:
   case TGSI_OPCODE_ATOMUMAX:
   case TGSI_OPCODE_ATOMIMIN:
   case TGSI_OPCODE_ATOMIMAX:
      translate_atomic(ctx, inst, &sinfo, srcs, dsts);
      break;
   case TGSI_OPCODE_RESQ:
      translate_resq(ctx, inst, srcs, dsts, writemask);
      break;
   case TGSI_OPCODE_CLOCK:
      ctx->shader_req_bits |= SHADER_REQ_SHADER_CLOCK;
      emit_buff(ctx, "%s = uintBitsToFloat(clock2x32ARB());\n", dsts[0]);
      break;
   default:
      fprintf(stderr,"failed to convert opcode %d\n", inst->Instruction.Opcode);
      break;
   }

   for (uint32_t i = 0; i < 1; i++) {
      enum tgsi_opcode_type dtype = tgsi_opcode_infer_dst_type(inst->Instruction.Opcode);
      if (dtype == TGSI_TYPE_DOUBLE) {
         emit_buff(ctx, "%s = uintBitsToFloat(unpackDouble2x32(%s));\n", fp64_dsts[0], dsts[0]);
      }
   }
   if (inst->Instruction.Saturate) {
      emit_buff(ctx, "%s = clamp(%s, 0.0, 1.0);\n", dsts[0], dsts[0]);
   }
   if (strbuf_get_error(&ctx->glsl_main))
       return false;
   return true;
}

static boolean
prolog(struct tgsi_iterate_context *iter)
{
   struct dump_ctx *ctx = (struct dump_ctx *)iter;

   if (ctx->prog_type == -1)
      ctx->prog_type = iter->processor.Processor;

   if (iter->processor.Processor == TGSI_PROCESSOR_VERTEX &&
       ctx->key->gs_present)
      require_glsl_ver(ctx, 150);

   return true;
}

/* reserve space for: "#extension GL_ARB_gpu_shader5 : require\n" */
#define PAD_GPU_SHADER5(ctx) \
   emit_hdr(ctx, "                                       \n")
#define PAD_GPU_MSINTERPOL(ctx) \
   emit_hdr(ctx, "                                                            \n")

static void emit_ext(struct dump_ctx *ctx, const char *name,
                     const char *verb)
{
   emit_hdrf(ctx, "#extension GL_%s : %s\n", name, verb);
}

static void emit_header(struct dump_ctx *ctx)
{
   if (ctx->cfg->use_gles) {
      emit_hdrf(ctx, "#version %d es\n", ctx->cfg->glsl_version);

      if (ctx->cfg->glsl_version < 320 &&
          (ctx->prog_type == TGSI_PROCESSOR_VERTEX ||
           ctx->prog_type == TGSI_PROCESSOR_GEOMETRY ||
           ctx->prog_type == TGSI_PROCESSOR_TESS_EVAL)) {
         PAD_GPU_SHADER5(ctx);
         PAD_GPU_MSINTERPOL(ctx);
      }

      if (ctx->shader_req_bits & SHADER_REQ_SAMPLER_MS)
         emit_ext(ctx, "OES_texture_storage_multisample_2d_array", "require");

      if (ctx->prog_type == TGSI_PROCESSOR_FRAGMENT) {
         if (ctx->shader_req_bits & SHADER_REQ_FBFETCH)
            emit_ext(ctx, "EXT_shader_framebuffer_fetch", "require");
      }

      if (ctx->prog_type == TGSI_PROCESSOR_GEOMETRY) {
         emit_ext(ctx, "EXT_geometry_shader", "require");
         if (ctx->shader_req_bits & SHADER_REQ_PSIZE)
            emit_ext(ctx, "OES_geometry_point_size", "enable");
      }

      if ((ctx->prog_type == TGSI_PROCESSOR_TESS_CTRL ||
           ctx->prog_type == TGSI_PROCESSOR_TESS_EVAL)) {
         if (ctx->cfg->glsl_version < 320)
            emit_ext(ctx, "OES_tessellation_shader", "require");
         emit_ext(ctx, "OES_tessellation_point_size", "enable");
      }

      if (ctx->cfg->glsl_version < 320) {
         if (ctx->shader_req_bits & SHADER_REQ_SAMPLE_SHADING)
            emit_ext(ctx, "OES_sample_variables", "require");
         if (ctx->shader_req_bits & SHADER_REQ_GPU_SHADER5) {
            emit_ext(ctx, "OES_gpu_shader5", "require");
            emit_ext(ctx, "OES_shader_multisample_interpolation",
                           "require");
         }
         if (ctx->shader_req_bits & SHADER_REQ_CUBE_ARRAY)
            emit_ext(ctx, "OES_texture_cube_map_array", "require");
         if (ctx->shader_req_bits & SHADER_REQ_LAYER)
            emit_ext(ctx, "EXT_geometry_shader", "require");
         if (ctx->shader_req_bits & SHADER_REQ_IMAGE_ATOMIC)
            emit_ext(ctx, "OES_shader_image_atomic", "require");
      }


      PAD_GPU_SHADER5(ctx);
      emit_hdr(ctx, "precision highp float;\n");
      emit_hdr(ctx, "precision highp int;\n");
   } else {
      if (ctx->prog_type == TGSI_PROCESSOR_COMPUTE) {
         emit_hdr(ctx, "#version 330\n");
         emit_ext(ctx, "ARB_compute_shader", "require");
      } else {
         if (ctx->prog_type == TGSI_PROCESSOR_GEOMETRY ||
             ctx->prog_type == TGSI_PROCESSOR_TESS_EVAL ||
             ctx->prog_type == TGSI_PROCESSOR_TESS_CTRL ||
             ctx->glsl_ver_required == 150)
            emit_hdr(ctx, "#version 150\n");
         else if (ctx->glsl_ver_required == 140)
            emit_hdr(ctx, "#version 140\n");
         else
            emit_hdr(ctx, "#version 130\n");
         if (ctx->prog_type == TGSI_PROCESSOR_VERTEX ||
             ctx->prog_type == TGSI_PROCESSOR_GEOMETRY ||
             ctx->prog_type == TGSI_PROCESSOR_TESS_EVAL)
            PAD_GPU_SHADER5(ctx);
      }

      if (ctx->prog_type == TGSI_PROCESSOR_TESS_CTRL ||
          ctx->prog_type == TGSI_PROCESSOR_TESS_EVAL)
         emit_ext(ctx, "ARB_tessellation_shader", "require");

      if (ctx->prog_type == TGSI_PROCESSOR_VERTEX && ctx->cfg->use_explicit_locations)
         emit_ext(ctx, "ARB_explicit_attrib_location", "require");
      if (ctx->prog_type == TGSI_PROCESSOR_FRAGMENT && fs_emit_layout(ctx))
         emit_ext(ctx, "ARB_fragment_coord_conventions", "require");

      if (ctx->ubo_used_mask)
         emit_ext(ctx, "ARB_uniform_buffer_object", "require");

      if (ctx->num_cull_dist_prop || ctx->key->prev_stage_num_cull_out)
         emit_ext(ctx, "ARB_cull_distance", "require");
      if (ctx->ssbo_used_mask)
         emit_ext(ctx, "ARB_shader_storage_buffer_object", "require");

      if (ctx->num_abo) {
         emit_ext(ctx, "ARB_shader_atomic_counters", "require");
         emit_ext(ctx, "ARB_shader_atomic_counter_ops", "require");
      }

      for (uint32_t i = 0; i < ARRAY_SIZE(shader_req_table); i++) {
         if (shader_req_table[i].key == SHADER_REQ_SAMPLER_RECT && ctx->glsl_ver_required >= 140)
            continue;

         if (ctx->shader_req_bits & shader_req_table[i].key) {
            emit_ext(ctx, shader_req_table[i].string, "require");
         }
      }
   }
}

char vrend_shader_samplerreturnconv(enum tgsi_return_type type)
{
   switch (type) {
   case TGSI_RETURN_TYPE_SINT:
      return 'i';
   case TGSI_RETURN_TYPE_UINT:
      return 'u';
   default:
      return ' ';
   }
}

const char *vrend_shader_samplertypeconv(bool use_gles, int sampler_type, int *is_shad)
{
   switch (sampler_type) {
   case TGSI_TEXTURE_BUFFER: return "Buffer";
   case TGSI_TEXTURE_1D:
      if (!use_gles)
         return "1D";
      /* fallthrough */
   case TGSI_TEXTURE_2D: return "2D";
   case TGSI_TEXTURE_3D: return "3D";
   case TGSI_TEXTURE_CUBE: return "Cube";
   case TGSI_TEXTURE_RECT: return "2DRect";
   case TGSI_TEXTURE_SHADOW1D:
      if (!use_gles) {
         *is_shad = 1;
         return "1DShadow";
      }
      /* fallthrough */
   case TGSI_TEXTURE_SHADOW2D: *is_shad = 1; return "2DShadow";
   case TGSI_TEXTURE_SHADOWRECT: *is_shad = 1; return "2DRectShadow";
   case TGSI_TEXTURE_1D_ARRAY:
      if (!use_gles)
         return "1DArray";
      /* fallthrough */
   case TGSI_TEXTURE_2D_ARRAY: return "2DArray";
   case TGSI_TEXTURE_SHADOW1D_ARRAY:
      if (!use_gles) {
         *is_shad = 1;
         return "1DArrayShadow";
      }
      /* fallthrough */
   case TGSI_TEXTURE_SHADOW2D_ARRAY: *is_shad = 1; return "2DArrayShadow";
   case TGSI_TEXTURE_SHADOWCUBE: *is_shad = 1; return "CubeShadow";
   case TGSI_TEXTURE_CUBE_ARRAY: return "CubeArray";
   case TGSI_TEXTURE_SHADOWCUBE_ARRAY: *is_shad = 1; return "CubeArrayShadow";
   case TGSI_TEXTURE_2D_MSAA: return "2DMS";
   case TGSI_TEXTURE_2D_ARRAY_MSAA: return "2DMSArray";
   default: return NULL;
   }
}

static const char *get_interp_string(struct vrend_shader_cfg *cfg, int interpolate, bool flatshade)
{
   switch (interpolate) {
   case TGSI_INTERPOLATE_LINEAR:
   if (!cfg->use_gles)
      return "noperspective ";
   else
      return "";
   case TGSI_INTERPOLATE_PERSPECTIVE:
      return "smooth ";
   case TGSI_INTERPOLATE_CONSTANT:
      return "flat ";
   case TGSI_INTERPOLATE_COLOR:
      if (flatshade)
         return "flat ";
      /* fallthrough */
   default:
      return NULL;
   }
}

static const char *get_aux_string(unsigned location)
{
   switch (location) {
   case TGSI_INTERPOLATE_LOC_CENTER:
   default:
      return "";
   case TGSI_INTERPOLATE_LOC_CENTROID:
      return "centroid ";
   case TGSI_INTERPOLATE_LOC_SAMPLE:
      return "sample ";
   }
}

static void emit_sampler_decl(struct dump_ctx *ctx,
                              uint32_t i, uint32_t range,
                              const struct vrend_shader_sampler *sampler)
{
   char ptc;
   int is_shad = 0;
   const char *sname, *precision, *stc;

   sname = tgsi_proc_to_prefix(ctx->prog_type);

   precision = (ctx->cfg->use_gles) ? "highp " : " ";

   ptc = vrend_shader_samplerreturnconv(sampler->tgsi_sampler_return);
   stc = vrend_shader_samplertypeconv(ctx->cfg->use_gles, sampler->tgsi_sampler_type, &is_shad);

   /* GLES does not support 1D textures -- we use a 2D texture and set the parameter set to 0.5 */
   if (ctx->cfg->use_gles && sampler->tgsi_sampler_type == TGSI_TEXTURE_1D)
      emit_hdrf(ctx, "uniform highp %csampler2D %ssamp%d;\n", ptc, sname, i);
   else if (range)
      emit_hdrf(ctx, "uniform %s%csampler%s %ssamp%d[%d];\n", precision, ptc, stc, sname, i, range);
   else
      emit_hdrf(ctx, "uniform %s%csampler%s %ssamp%d;\n", precision,  ptc, stc, sname, i);

   if (is_shad) {
      emit_hdrf(ctx, "uniform %svec4 %sshadmask%d;\n", precision,  sname, i);
      emit_hdrf(ctx, "uniform %svec4 %sshadadd%d;\n", precision,  sname, i);
      ctx->shadow_samp_mask |= (1 << i);
   }
}

const char *get_internalformat_string(int virgl_format, enum tgsi_return_type *stype)
{
   switch (virgl_format) {
   case PIPE_FORMAT_R11G11B10_FLOAT:
      *stype = TGSI_RETURN_TYPE_FLOAT;
      return "r11f_g11f_b10f";
   case PIPE_FORMAT_R10G10B10A2_UNORM:
      *stype = TGSI_RETURN_TYPE_UNORM;
      return "rgb10_a2";
   case PIPE_FORMAT_R10G10B10A2_UINT:
      *stype = TGSI_RETURN_TYPE_UINT;
      return "rgb10_a2ui";
   case PIPE_FORMAT_R8_UNORM:
      *stype = TGSI_RETURN_TYPE_UNORM;
      return "r8";
   case PIPE_FORMAT_R8_SNORM:
      *stype = TGSI_RETURN_TYPE_SNORM;
      return "r8_snorm";
   case PIPE_FORMAT_R8_UINT:
      *stype = TGSI_RETURN_TYPE_UINT;
      return "r8ui";
   case PIPE_FORMAT_R8_SINT:
      *stype = TGSI_RETURN_TYPE_SINT;
      return "r8i";
   case PIPE_FORMAT_R8G8_UNORM:
      *stype = TGSI_RETURN_TYPE_UNORM;
      return "rg8";
   case PIPE_FORMAT_R8G8_SNORM:
      *stype = TGSI_RETURN_TYPE_SNORM;
      return "rg8_snorm";
   case PIPE_FORMAT_R8G8_UINT:
      *stype = TGSI_RETURN_TYPE_UINT;
      return "rg8ui";
   case PIPE_FORMAT_R8G8_SINT:
      *stype = TGSI_RETURN_TYPE_SINT;
      return "rg8i";
   case PIPE_FORMAT_R8G8B8A8_UNORM:
      *stype = TGSI_RETURN_TYPE_UNORM;
      return "rgba8";
   case PIPE_FORMAT_R8G8B8A8_SNORM:
      *stype = TGSI_RETURN_TYPE_SNORM;
      return "rgba8_snorm";
   case PIPE_FORMAT_R8G8B8A8_UINT:
      *stype = TGSI_RETURN_TYPE_UINT;
      return "rgba8ui";
   case PIPE_FORMAT_R8G8B8A8_SINT:
      *stype = TGSI_RETURN_TYPE_SINT;
      return "rgba8i";
   case PIPE_FORMAT_R16_UNORM:
      *stype = TGSI_RETURN_TYPE_UNORM;
      return "r16";
   case PIPE_FORMAT_R16_SNORM:
      *stype = TGSI_RETURN_TYPE_SNORM;
      return "r16_snorm";
   case PIPE_FORMAT_R16_UINT:
      *stype = TGSI_RETURN_TYPE_UINT;
      return "r16ui";
   case PIPE_FORMAT_R16_SINT:
      *stype = TGSI_RETURN_TYPE_SINT;
      return "r16i";
   case PIPE_FORMAT_R16_FLOAT:
      *stype = TGSI_RETURN_TYPE_FLOAT;
      return "r16f";
   case PIPE_FORMAT_R16G16_UNORM:
      *stype = TGSI_RETURN_TYPE_UNORM;
      return "rg16";
   case PIPE_FORMAT_R16G16_SNORM:
      *stype = TGSI_RETURN_TYPE_SNORM;
      return "rg16_snorm";
   case PIPE_FORMAT_R16G16_UINT:
      *stype = TGSI_RETURN_TYPE_UINT;
      return "rg16ui";
   case PIPE_FORMAT_R16G16_SINT:
      *stype = TGSI_RETURN_TYPE_SINT;
      return "rg16i";
   case PIPE_FORMAT_R16G16_FLOAT:
      *stype = TGSI_RETURN_TYPE_FLOAT;
      return "rg16f";
   case PIPE_FORMAT_R16G16B16A16_UNORM:
      *stype = TGSI_RETURN_TYPE_UNORM;
      return "rgba16";
   case PIPE_FORMAT_R16G16B16A16_SNORM:
      *stype = TGSI_RETURN_TYPE_SNORM;
      return "rgba16_snorm";
   case PIPE_FORMAT_R16G16B16A16_FLOAT:
      *stype = TGSI_RETURN_TYPE_FLOAT;
      return "rgba16f";
   case PIPE_FORMAT_R32_FLOAT:
      *stype = TGSI_RETURN_TYPE_FLOAT;
      return "r32f";
   case PIPE_FORMAT_R32_UINT:
      *stype = TGSI_RETURN_TYPE_UINT;
      return "r32ui";
   case PIPE_FORMAT_R32_SINT:
      *stype = TGSI_RETURN_TYPE_SINT;
      return "r32i";
   case PIPE_FORMAT_R32G32_FLOAT:
      *stype = TGSI_RETURN_TYPE_FLOAT;
      return "rg32f";
   case PIPE_FORMAT_R32G32_UINT:
      *stype = TGSI_RETURN_TYPE_UINT;
      return "rg32ui";
   case PIPE_FORMAT_R32G32_SINT:
      *stype = TGSI_RETURN_TYPE_SINT;
      return "rg32i";
   case PIPE_FORMAT_R32G32B32A32_FLOAT:
      *stype = TGSI_RETURN_TYPE_FLOAT;
      return "rgba32f";
   case PIPE_FORMAT_R32G32B32A32_UINT:
      *stype = TGSI_RETURN_TYPE_UINT;
      return "rgba32ui";
   case PIPE_FORMAT_R16G16B16A16_UINT:
      *stype = TGSI_RETURN_TYPE_UINT;
      return "rgba16ui";
   case PIPE_FORMAT_R16G16B16A16_SINT:
      *stype = TGSI_RETURN_TYPE_SINT;
      return "rgba16i";
   case PIPE_FORMAT_R32G32B32A32_SINT:
      *stype = TGSI_RETURN_TYPE_SINT;
      return "rgba32i";
   case PIPE_FORMAT_NONE:
      *stype = TGSI_RETURN_TYPE_UNORM;
      return "";
   default:
      *stype = TGSI_RETURN_TYPE_UNORM;
      fprintf(stderr, "illegal format %d\n", virgl_format);
      return "";
   }
}

static void emit_image_decl(struct dump_ctx *ctx,
                            uint32_t i, uint32_t range,
                            const struct vrend_shader_image *image)
{
   char ptc;
   int is_shad = 0;
   const char *sname, *stc, *formatstr;
   enum tgsi_return_type itype;
   const char *volatile_str = image->vflag ? "volatile " : "";
   const char *precision = ctx->cfg->use_gles ? "highp " : "";
   const char *access = "";
   formatstr = get_internalformat_string(image->decl.Format, &itype);
   ptc = vrend_shader_samplerreturnconv(itype);
   sname = tgsi_proc_to_prefix(ctx->prog_type);
   stc = vrend_shader_samplertypeconv(ctx->cfg->use_gles, image->decl.Resource, &is_shad);

   if (!image->decl.Writable)
      access = "readonly ";
   else if (!image->decl.Format ||
            (ctx->cfg->use_gles &&
             (image->decl.Format != PIPE_FORMAT_R32_FLOAT) &&
             (image->decl.Format != PIPE_FORMAT_R32_SINT) &&
             (image->decl.Format != PIPE_FORMAT_R32_UINT)))
      access = "writeonly ";

   if (ctx->cfg->use_gles) { /* TODO: enable on OpenGL 4.2 and up also */
      emit_hdrf(ctx, "layout(binding=%d%s%s) ",
               i, formatstr[0] != '\0' ? ", " : "", formatstr);
   } else if (formatstr[0] != '\0') {
      emit_hdrf(ctx, "layout(%s) ", formatstr);
   }

   if (range)
      emit_hdrf(ctx, "%s%suniform %s%cimage%s %simg%d[%d];\n",
               access, volatile_str, precision, ptc, stc, sname, i, range);
   else
      emit_hdrf(ctx, "%s%suniform %s%cimage%s %simg%d;\n",
               access, volatile_str, precision, ptc, stc, sname, i);
}

static void emit_ios_common(struct dump_ctx *ctx)
{
   uint i;
   const char *sname = tgsi_proc_to_prefix(ctx->prog_type);

   for (i = 0; i < ctx->num_temp_ranges; i++) {
      emit_hdrf(ctx, "vec4 temp%d[%d];\n", ctx->temp_ranges[i].first, ctx->temp_ranges[i].last - ctx->temp_ranges[i].first + 1);
   }

   if (ctx->write_mul_utemp) {
      emit_hdr(ctx, "uvec4 mul_utemp;\n");
      emit_hdr(ctx, "uvec4 umul_temp;\n");
   }

   if (ctx->write_mul_itemp) {
      emit_hdr(ctx, "ivec4 mul_itemp;\n");
      emit_hdr(ctx, "ivec4 imul_temp;\n");
   }

   if (ctx->ssbo_used_mask || ctx->has_file_memory) {
     emit_hdr(ctx, "uint ssbo_addr_temp;\n");
   }

   if (ctx->shader_req_bits & SHADER_REQ_FP64) {
      emit_hdr(ctx, "dvec2 fp64_dst[3];\n");
      emit_hdr(ctx, "dvec2 fp64_src[4];\n");
   }

   for (i = 0; i < ctx->num_address; i++) {
      emit_hdrf(ctx, "int addr%d;\n", i);
   }
   if (ctx->num_consts) {
      const char *cname = tgsi_proc_to_prefix(ctx->prog_type);
      emit_hdrf(ctx, "uniform uvec4 %sconst0[%d];\n", cname, ctx->num_consts);
   }

   if (ctx->ubo_used_mask) {
      const char *cname = tgsi_proc_to_prefix(ctx->prog_type);

      if (ctx->info.dimension_indirect_files & (1 << TGSI_FILE_CONSTANT)) {
         require_glsl_ver(ctx, 150);
         int first = ffs(ctx->ubo_used_mask) - 1;
         unsigned num_ubo = util_bitcount(ctx->ubo_used_mask);
         emit_hdrf(ctx, "uniform %subo { vec4 ubocontents[%d]; } %suboarr[%d];\n", cname, ctx->ubo_sizes[first], cname, num_ubo);
      } else {
         unsigned mask = ctx->ubo_used_mask;
         while (mask) {
            uint32_t i = u_bit_scan(&mask);
            emit_hdrf(ctx, "uniform %subo%d { vec4 %subo%dcontents[%d]; };\n", cname, i, cname, i, ctx->ubo_sizes[i]);
         }
      }
   }

   if (ctx->info.indirect_files & (1 << TGSI_FILE_SAMPLER)) {
      for (i = 0; i < ctx->num_sampler_arrays; i++) {
         uint32_t first = ctx->sampler_arrays[i].first;
         uint32_t range = ctx->sampler_arrays[i].array_size;
         emit_sampler_decl(ctx, first, range, ctx->samplers + first);
      }
   } else {
      uint nsamp = util_last_bit(ctx->samplers_used);
      for (i = 0; i < nsamp; i++) {

         if ((ctx->samplers_used & (1 << i)) == 0)
            continue;

         emit_sampler_decl(ctx, i, 0, ctx->samplers + i);
      }
   }

   if (ctx->info.indirect_files & (1 << TGSI_FILE_IMAGE)) {
      for (i = 0; i < ctx->num_image_arrays; i++) {
         uint32_t first = ctx->image_arrays[i].first;
         uint32_t range = ctx->image_arrays[i].array_size;
         emit_image_decl(ctx, first, range, ctx->images + first);
      }
   } else {
      uint32_t mask = ctx->images_used_mask;
      while (mask) {
         i = u_bit_scan(&mask);
         emit_image_decl(ctx, i, 0, ctx->images + i);
      }
   }

   for (i = 0; i < ctx->num_abo; i++){
      if (ctx->abo_sizes[i] > 1)
         emit_hdrf(ctx, "layout (binding = %d, offset = %d) uniform atomic_uint ac%d[%d];\n", ctx->abo_idx[i], ctx->abo_offsets[i] * 4, i, ctx->abo_sizes[i]);
      else
         emit_hdrf(ctx, "layout (binding = %d, offset = %d) uniform atomic_uint ac%d;\n", ctx->abo_idx[i], ctx->abo_offsets[i] * 4, i);
   }

   if (ctx->info.indirect_files & (1 << TGSI_FILE_BUFFER)) {
      uint32_t mask = ctx->ssbo_used_mask;
      while (mask) {
         int start, count;
         u_bit_scan_consecutive_range(&mask, &start, &count);
         const char *atomic = (ctx->ssbo_atomic_mask & (1 << start)) ? "atomic" : "";
         emit_hdrf(ctx, "layout (binding = %d, std430) buffer %sssbo%d { uint %sssbocontents%d[]; } %sssboarr%s[%d];\n", start, sname, start, sname, start, sname, atomic, count);
      }
   } else {
      uint32_t mask = ctx->ssbo_used_mask;
      while (mask) {
         uint32_t id = u_bit_scan(&mask);
         enum vrend_type_qualifier type = (ctx->ssbo_integer_mask & (1 << id)) ? INT : UINT;
         emit_hdrf(ctx, "layout (binding = %d, std430) buffer %sssbo%d { %s %sssbocontents%d[]; };\n", id, sname, id,
                  get_string(type), sname, id);
      }
   }

}

static void emit_ios_streamout(struct dump_ctx *ctx)
{
   if (ctx->so) {
      char outtype[6] = {0};
      for (uint i = 0; i < ctx->so->num_outputs; i++) {
         if (!ctx->write_so_outputs[i])
            continue;
         if (ctx->so->output[i].num_components == 1)
            snprintf(outtype, 6, "float");
         else
            snprintf(outtype, 6, "vec%d", ctx->so->output[i].num_components);
	 if (ctx->prog_type == TGSI_PROCESSOR_TESS_CTRL)
            emit_hdrf(ctx, "out %s tfout%d[];\n", outtype, i);
         else if (ctx->so->output[i].stream && ctx->prog_type == TGSI_PROCESSOR_GEOMETRY)
            emit_hdrf(ctx, "layout (stream=%d) out %s tfout%d;\n", ctx->so->output[i].stream, outtype, i);
         else
            emit_hdrf(ctx, "out %s tfout%d;\n", outtype, i);
      }
   }
}

static inline void emit_winsys_correction(struct dump_ctx *ctx)
{
   emit_hdr(ctx, "uniform float winsys_adjust_y;\n");
}

static void emit_ios_vs(struct dump_ctx *ctx)
{
   uint32_t i;
   char postfix[8];
   const char *prefix = "", *auxprefix = "";
   bool fcolor_emitted[2], bcolor_emitted[2];

   ctx->num_interps = 0;

   if (ctx->key->color_two_side) {
      fcolor_emitted[0] = fcolor_emitted[1] = false;
      bcolor_emitted[0] = bcolor_emitted[1] = false;
   }

   for (i = 0; i < ctx->num_inputs; i++) {
      if (!ctx->inputs[i].glsl_predefined_no_emit) {
         if (ctx->cfg->use_explicit_locations) {
            emit_hdrf(ctx, "layout(location=%d) ", ctx->inputs[i].first);
         }
         prefix = "";
         auxprefix = "";
         postfix[0] = 0;
         emit_hdrf(ctx, "%s%sin vec4 %s%s;\n", prefix, auxprefix, ctx->inputs[i].glsl_name, postfix);
      }
   }

   if (ctx_indirect_outputs(ctx)) {
      const char *name_prefix = get_stage_output_name_prefix(ctx->prog_type);
      if (ctx->generic_output_range.used) {
         emit_hdrf(ctx, "out block { vec4 %s%d[%d]; } oblk;\n", name_prefix, ctx->generic_output_range.first, ctx->generic_output_range.last - ctx->generic_output_range.first + 1);
      }
   }

   for (i = 0; i < ctx->num_outputs; i++) {
      if (ctx->key->color_two_side && ctx->outputs[i].sid < 2) {
         if (ctx->outputs[i].name == TGSI_SEMANTIC_COLOR)
            fcolor_emitted[ctx->outputs[i].sid] = true;
         if (ctx->outputs[i].name == TGSI_SEMANTIC_BCOLOR)
            bcolor_emitted[ctx->outputs[i].sid] = true;
      }

      if (!ctx->outputs[i].glsl_predefined_no_emit) {
         if (ctx->outputs[i].name == TGSI_SEMANTIC_GENERIC ||
             ctx->outputs[i].name == TGSI_SEMANTIC_COLOR ||
             ctx->outputs[i].name == TGSI_SEMANTIC_BCOLOR) {
            ctx->num_interps++;
            prefix = INTERP_PREFIX;
         } else
            prefix = "";

         /* ugly leave spaces to patch interp in later */
         emit_hdrf(ctx, "%s%s%s%s vec4 %s;\n",
                   prefix,
                   ctx->outputs[i].precise ? "precise " : "",
                   ctx->outputs[i].invariant ? "invariant " : "",
                   ctx->outputs[i].fbfetch_used ? "inout" : "out",
                   ctx->outputs[i].glsl_name);
      } else if (ctx->outputs[i].invariant || ctx->outputs[i].precise) {
         emit_hdrf(ctx, "%s%s;\n",
                   ctx->outputs[i].precise ? "precise " :
                                             (ctx->outputs[i].invariant ? "invariant " : ""),
                   ctx->outputs[i].glsl_name);
      }
   }

   if (ctx->key->color_two_side) {
      for (i = 0; i < 2; i++) {
         if (fcolor_emitted[i] && !bcolor_emitted[i]) {
            emit_hdrf(ctx, "%sout vec4 ex_bc%d;\n", INTERP_PREFIX, i);
         }
         if (bcolor_emitted[i] && !fcolor_emitted[i]) {
            emit_hdrf(ctx, "%sout vec4 ex_c%d;\n", INTERP_PREFIX, i);
         }
      }
   }

   emit_winsys_correction(ctx);

   if (ctx->has_clipvertex) {
      emit_hdrf(ctx, "%svec4 clipv_tmp;\n", ctx->has_clipvertex_so ? "out " : "");
   }
   if (ctx->num_clip_dist || ctx->key->clip_plane_enable) {
      bool has_prop = (ctx->num_clip_dist_prop + ctx->num_cull_dist_prop) > 0;
      int num_clip_dists = ctx->num_clip_dist ? ctx->num_clip_dist : 8;
      int num_cull_dists = 0;
      char cull_buf[64] = { 0 };
      char clip_buf[64] = { 0 };
      if (has_prop) {
         num_clip_dists = ctx->num_clip_dist_prop;
         num_cull_dists = ctx->num_cull_dist_prop;
         if (num_clip_dists)
            snprintf(clip_buf, 64, "out float gl_ClipDistance[%d];\n", num_clip_dists);
         if (num_cull_dists)
            snprintf(cull_buf, 64, "out float gl_CullDistance[%d];\n", num_cull_dists);
      } else
         snprintf(clip_buf, 64, "out float gl_ClipDistance[%d];\n", num_clip_dists);
      if (ctx->key->clip_plane_enable) {
         emit_hdr(ctx, "uniform vec4 clipp[8];\n");
      }
      if (ctx->key->gs_present || ctx->key->tes_present) {
         ctx->vs_has_pervertex = true;
         emit_hdrf(ctx, "out gl_PerVertex {\n vec4 gl_Position;\n float gl_PointSize;\n%s%s};\n", clip_buf, cull_buf);
      } else {
         emit_hdrf(ctx, "%s%s", clip_buf, cull_buf);
      }
      emit_hdr(ctx, "vec4 clip_dist_temp[2];\n");
   }
}


static void emit_ios_fs(struct dump_ctx *ctx)
{
   uint32_t i;
   const char *prefix = "", *auxprefix = "";

   if (fs_emit_layout(ctx)) {
      bool upper_left = !(ctx->fs_coord_origin ^ ctx->key->invert_fs_origin);
      char comma = (upper_left && ctx->fs_pixel_center) ? ',' : ' ';

      emit_hdrf(ctx, "layout(%s%c%s) in vec4 gl_FragCoord;\n",
                upper_left ? "origin_upper_left" : "",
                comma,
                ctx->fs_pixel_center ? "pixel_center_integer" : "");
   }
   if (ctx->early_depth_stencil) {
      emit_hdr(ctx, "layout(early_fragment_tests) in;\n");
   }

   if (ctx_indirect_inputs(ctx) && ctx->generic_input_range.used) {
      const char *name_prefix = get_stage_input_name_prefix(ctx, ctx->prog_type);
      int size = ctx->generic_input_range.last - ctx->generic_input_range.first + 1;
      if (size < ctx->key->num_indirect_generic_inputs)
         ctx->key->num_indirect_generic_inputs = size;  // This is wrong but needed for debugging
      emit_hdrf(ctx, "in block { vec4 %s%d[%d]; } blk;\n", name_prefix, ctx->generic_input_range.first, size);
   }

   for (i = 0; i < ctx->num_inputs; i++) {
      if (!ctx->inputs[i].glsl_predefined_no_emit) {
         prefix = "";
         auxprefix = "";

         if (ctx->inputs[i].name == TGSI_SEMANTIC_GENERIC ||
              ctx->inputs[i].name == TGSI_SEMANTIC_COLOR ||
              ctx->inputs[i].name == TGSI_SEMANTIC_BCOLOR) {
            prefix = get_interp_string(ctx->cfg, ctx->inputs[i].interpolate, ctx->key->flatshade);
            if (!prefix)
               prefix = "";
            auxprefix = get_aux_string(ctx->inputs[i].location);
            ctx->num_interps++;
         }
         emit_hdrf(ctx, "%s%sin vec4 %s;\n", prefix, auxprefix, ctx->inputs[i].glsl_name);
      }

      if (ctx->cfg->use_gles && !ctx->key->winsys_adjust_y_emitted &&
          (ctx->key->coord_replace & (1 << ctx->inputs[i].sid))) {
         ctx->key->winsys_adjust_y_emitted = true;
         emit_hdr(ctx, "uniform float winsys_adjust_y;\n");
      }
   }

   if (ctx->key->color_two_side) {
      if (ctx->color_in_mask & 1)
         emit_hdr(ctx, "vec4 realcolor0;\n");
      if (ctx->color_in_mask & 2)
         emit_hdr(ctx, "vec4 realcolor1;\n");
   }

   if (ctx->write_all_cbufs) {
      for (i = 0; i < (uint32_t)ctx->cfg->max_draw_buffers; i++) {
         if (ctx->cfg->use_gles)
            emit_hdrf(ctx, "layout (location=%d) out vec4 fsout_c%d;\n", i, i);
         else
            emit_hdrf(ctx, "out vec4 fsout_c%d;\n", i);
      }
   } else {
      for (i = 0; i < ctx->num_outputs; i++) {

         if (!ctx->outputs[i].glsl_predefined_no_emit) {
            prefix = "";

            /* ugly leave spaces to patch interp in later */
            emit_hdrf(ctx, "%s%s%s%s vec4 %s;\n",
                      prefix,
                      ctx->outputs[i].precise ? "precise " : "",
                      ctx->outputs[i].invariant ? "invariant " : "",
                      ctx->outputs[i].fbfetch_used ? "inout" : "out",
                      ctx->outputs[i].glsl_name);
         } else if (ctx->outputs[i].invariant || ctx->outputs[i].precise) {
            emit_hdrf(ctx, "%s%s;\n",
                      ctx->outputs[i].precise ? "precise " :
                      (ctx->outputs[i].invariant ? "invariant " : ""),
                      ctx->outputs[i].glsl_name);
         }
      }
   }

   if (ctx->num_in_clip_dist) {
      if (ctx->key->prev_stage_num_clip_out) {
         emit_hdrf(ctx, "in float gl_ClipDistance[%d];\n", ctx->key->prev_stage_num_clip_out);
      }
      if (ctx->key->prev_stage_num_cull_out) {
         emit_hdrf(ctx, "in float gl_CullDistance[%d];\n", ctx->key->prev_stage_num_cull_out);
      }
   }
}

static void emit_ios_geom(struct dump_ctx *ctx)
{
   uint32_t i;
   char invocbuf[25];

   if (ctx->gs_num_invocations)
      snprintf(invocbuf, 25, ", invocations = %d", ctx->gs_num_invocations);

   emit_hdrf(ctx, "layout(%s%s) in;\n", prim_to_name(ctx->gs_in_prim),
             ctx->gs_num_invocations > 1 ? invocbuf : "");
   emit_hdrf(ctx, "layout(%s, max_vertices = %d) out;\n", prim_to_name(ctx->gs_out_prim), ctx->gs_max_out_verts);


   for (i = 0; i < ctx->num_inputs; i++) {
      if (!ctx->inputs[i].glsl_predefined_no_emit)
         emit_hdrf(ctx, "in vec4 %s[%d];\n", ctx->inputs[i].glsl_name, gs_input_prim_to_size(ctx->gs_in_prim));
   }

   for (i = 0; i < ctx->num_outputs; i++) {

      if (!ctx->outputs[i].glsl_predefined_no_emit) {
         const char *prefix = "";
         if (ctx->outputs[i].name == TGSI_SEMANTIC_GENERIC ||
             ctx->outputs[i].name == TGSI_SEMANTIC_COLOR ||
             ctx->outputs[i].name == TGSI_SEMANTIC_BCOLOR) {
            ctx->num_interps++;
            prefix = INTERP_PREFIX;
         }

         /* ugly leave spaces to patch interp in later */
         if (ctx->outputs[i].stream)
            emit_hdrf(ctx, "layout (stream = %d) %s%s%sout vec4 %s;\n", ctx->outputs[i].stream, prefix,
                      ctx->outputs[i].precise ? "precise " : "",
                      ctx->outputs[i].invariant ? "invariant " : "",
                      ctx->outputs[i].glsl_name);
         else
            emit_hdrf(ctx, "%s%s%s%s vec4 %s;\n",
                      prefix,
                      ctx->outputs[i].precise ? "precise " : "",
                      ctx->outputs[i].invariant ? "invariant " : "",
                      ctx->outputs[i].fbfetch_used ? "inout" : "out",
                      ctx->outputs[i].glsl_name);
      } else if (ctx->outputs[i].invariant || ctx->outputs[i].precise) {
         emit_hdrf(ctx, "%s%s;\n",
                   ctx->outputs[i].precise ? "precise " :
                   (ctx->outputs[i].invariant ? "invariant " : ""),
                   ctx->outputs[i].glsl_name);
      }
   }

   emit_winsys_correction(ctx);

   if (ctx->num_in_clip_dist || ctx->key->clip_plane_enable || ctx->key->prev_stage_pervertex_out) {
      int clip_dist, cull_dist;
      char clip_var[64] = {}, cull_var[64] = {};

      clip_dist = ctx->key->prev_stage_num_clip_out ? ctx->key->prev_stage_num_clip_out : ctx->num_in_clip_dist;
      cull_dist = ctx->key->prev_stage_num_cull_out;

      if (clip_dist)
         snprintf(clip_var, 64, "float gl_ClipDistance[%d];\n", clip_dist);
      if (cull_dist)
         snprintf(cull_var, 64, "float gl_CullDistance[%d];\n", cull_dist);

      emit_hdrf(ctx, "in gl_PerVertex {\n vec4 gl_Position;\n float gl_PointSize; \n %s%s\n} gl_in[];\n", clip_var, cull_var);
   }
   if (ctx->num_clip_dist) {
      bool has_prop = (ctx->num_clip_dist_prop + ctx->num_cull_dist_prop) > 0;
      int num_clip_dists = ctx->num_clip_dist ? ctx->num_clip_dist : 8;
      int num_cull_dists = 0;
      char cull_buf[64] = { 0 };
      char clip_buf[64] = { 0 };
      if (has_prop) {
         num_clip_dists = ctx->num_clip_dist_prop;
         num_cull_dists = ctx->num_cull_dist_prop;
         if (num_clip_dists)
            snprintf(clip_buf, 64, "out float gl_ClipDistance[%d];\n", num_clip_dists);
         if (num_cull_dists)
            snprintf(cull_buf, 64, "out float gl_CullDistance[%d];\n", num_cull_dists);
      } else
         snprintf(clip_buf, 64, "out float gl_ClipDistance[%d];\n", num_clip_dists);
      emit_hdrf(ctx, "%s%s\n", clip_buf, cull_buf);
      emit_hdrf(ctx, "vec4 clip_dist_temp[2];\n");
   }
}


static void emit_ios_tcs(struct dump_ctx *ctx)
{
   uint32_t i;

   if (ctx_indirect_inputs(ctx)) {
      const char *name_prefix = get_stage_input_name_prefix(ctx, ctx->prog_type);

      if (ctx->generic_input_range.used) {
         int size = ctx->generic_input_range.last - ctx->generic_input_range.first + 1;
         if (size < ctx->key->num_indirect_generic_inputs)
            size = ctx->key->num_indirect_generic_inputs;
         emit_hdrf(ctx, "in block { vec4 %s%d[%d]; } blk[];\n", name_prefix, ctx->generic_input_range.first, size);
      }
   }

   for (i = 0; i < ctx->num_inputs; i++) {
      if (!ctx->inputs[i].glsl_predefined_no_emit) {
         const char *postfix =  (ctx->inputs[i].name != TGSI_SEMANTIC_PATCH) ? "[]" : "";
         emit_hdrf(ctx, "in vec4 %s%s;\n", ctx->inputs[i].glsl_name, postfix);
      }
   }

   emit_hdrf(ctx, "layout(vertices = %d) out;\n", ctx->tcs_vertices_out);

   if (ctx_indirect_outputs(ctx)) {
      const char *name_prefix = get_stage_output_name_prefix(ctx->prog_type);
      if (ctx->generic_output_range.used) {
         emit_hdrf(ctx, "out block { vec4 %s%d[%d]; } oblk[];\n", name_prefix, ctx->generic_output_range.first, ctx->generic_output_range.last - ctx->generic_output_range.first + 1);
      }
      if (ctx->patch_output_range.used) {
         emit_hdrf(ctx, "patch out vec4 %sp%d[%d];\n", name_prefix, ctx->patch_output_range.first, ctx->patch_output_range.last - ctx->patch_output_range.first + 1);
      }
   }

   for (i = 0; i < ctx->num_outputs; i++) {
      if (!ctx->outputs[i].glsl_predefined_no_emit) {
         /* ugly leave spaces to patch interp in later */
         if (ctx->outputs[i].name == TGSI_SEMANTIC_PATCH)
            emit_hdrf(ctx, "patch out vec4 %s;\n", ctx->outputs[i].glsl_name);
         else
            emit_hdrf(ctx, "out vec4 %s[];\n", ctx->outputs[i].glsl_name);
      } else if (ctx->outputs[i].invariant || ctx->outputs[i].precise) {
         emit_hdrf(ctx, "%s%s;\n",
                   ctx->outputs[i].precise ? "precise " :
                   (ctx->outputs[i].invariant ? "invariant " : ""),
                   ctx->outputs[i].glsl_name);
      }
   }

   if (ctx->num_in_clip_dist || ctx->key->prev_stage_pervertex_out) {
      int clip_dist, cull_dist;
      char clip_var[64] = "", cull_var[64] = "";

      clip_dist = ctx->key->prev_stage_num_clip_out ? ctx->key->prev_stage_num_clip_out : ctx->num_in_clip_dist;
      cull_dist = ctx->key->prev_stage_num_cull_out;

      if (clip_dist)
         snprintf(clip_var, 64, "float gl_ClipDistance[%d];\n", clip_dist);
      if (cull_dist)
         snprintf(cull_var, 64, "float gl_CullDistance[%d];\n", cull_dist);

      emit_hdrf(ctx, "in gl_PerVertex {\n vec4 gl_Position;\n float gl_PointSize; \n %s%s} gl_in[];\n", clip_var, cull_var);
   }
   if (ctx->num_clip_dist) {
      emit_hdrf(ctx, "out gl_PerVertex {\n vec4 gl_Position;\n float gl_PointSize;\n float gl_ClipDistance[%d];\n} gl_out[];\n", ctx->num_clip_dist ? ctx->num_clip_dist : 8);
      emit_hdr(ctx, "vec4 clip_dist_temp[2];\n");
   }
}

static void emit_ios_tes(struct dump_ctx *ctx)
{
   uint32_t i;

   if (ctx_indirect_inputs(ctx)) {
      const char *name_prefix = get_stage_input_name_prefix(ctx, ctx->prog_type);
      if (ctx->patch_input_range.used) {
         int size = ctx->patch_input_range.last - ctx->patch_input_range.first + 1;
         if (size < ctx->key->num_indirect_patch_inputs)
            size = ctx->key->num_indirect_patch_inputs;
         emit_hdrf(ctx, "patch in vec4 %sp%d[%d];\n", name_prefix, ctx->patch_input_range.first, size);
      }

      if (ctx->generic_input_range.used) {
         int size = ctx->generic_input_range.last - ctx->generic_input_range.first + 1;
         if (size < ctx->key->num_indirect_generic_inputs)
            size = ctx->key->num_indirect_generic_inputs;
         emit_hdrf(ctx, "in block { vec4 %s%d[%d]; } blk[];\n", name_prefix, ctx->generic_input_range.first, size);
      }
   }

   for (i = 0; i < ctx->num_inputs; i++) {
      if (!ctx->inputs[i].glsl_predefined_no_emit) {
         const char *prefix = "";
         const char *postfix = "";
         if (ctx->inputs[i].name == TGSI_SEMANTIC_PATCH)
            prefix = "patch ";
         else
            postfix = "[]";
         emit_hdrf(ctx, "%sin vec4 %s%s;\n", prefix, ctx->inputs[i].glsl_name, postfix);
      }
   }

   emit_hdrf(ctx, "layout(%s, %s, %s%s) in;\n",
             prim_to_tes_name(ctx->tes_prim_mode),
             get_spacing_string(ctx->tes_spacing),
             ctx->tes_vertex_order ? "cw" : "ccw",
             ctx->tes_point_mode ? ", point_mode" : "");

   for (i = 0; i < ctx->num_outputs; i++) {
      if (!ctx->outputs[i].glsl_predefined_no_emit) {
         const char *prefix = "";
         if (ctx->outputs[i].name == TGSI_SEMANTIC_GENERIC ||
             ctx->outputs[i].name == TGSI_SEMANTIC_COLOR ||
             ctx->outputs[i].name == TGSI_SEMANTIC_BCOLOR) {
            ctx->num_interps++;
            prefix = INTERP_PREFIX;
         }

         /* ugly leave spaces to patch interp in later */
         emit_hdrf(ctx, "%s%s%s%s vec4 %s;\n",
                   prefix,
                   ctx->outputs[i].precise ? "precise " : "",
                   ctx->outputs[i].invariant ? "invariant " : "",
                   ctx->outputs[i].fbfetch_used ? "inout" : "out",
                   ctx->outputs[i].glsl_name);
      } else if (ctx->outputs[i].invariant || ctx->outputs[i].precise) {
         emit_hdrf(ctx, "%s%s;\n",
                   ctx->outputs[i].precise ? "precise " :
                    (ctx->outputs[i].invariant ? "invariant " : ""),
                   ctx->outputs[i].glsl_name);
      }
   }

   emit_winsys_correction(ctx);

   if (ctx->num_in_clip_dist || ctx->key->prev_stage_pervertex_out) {
      int clip_dist, cull_dist;
      char clip_var[64] = "", cull_var[64] = "";

      clip_dist = ctx->key->prev_stage_num_clip_out ? ctx->key->prev_stage_num_clip_out : ctx->num_in_clip_dist;
      cull_dist = ctx->key->prev_stage_num_cull_out;

      if (clip_dist)
         snprintf(clip_var, 64, "float gl_ClipDistance[%d];\n", clip_dist);
      if (cull_dist)
         snprintf(cull_var, 64, "float gl_CullDistance[%d];\n", cull_dist);

      emit_hdrf(ctx, "in gl_PerVertex {\n vec4 gl_Position;\n float gl_PointSize; \n %s%s} gl_in[];\n", clip_var, cull_var);
   }
   if (ctx->num_clip_dist) {
      emit_hdrf(ctx, "out gl_PerVertex {\n vec4 gl_Position;\n float gl_PointSize;\n float gl_ClipDistance[%d];\n} gl_out[];\n", ctx->num_clip_dist ? ctx->num_clip_dist : 8);
      emit_hdr(ctx, "vec4 clip_dist_temp[2];\n");
   }
}


static void emit_ios_cs(struct dump_ctx *ctx)
{
   emit_hdrf(ctx, "layout (local_size_x = %d, local_size_y = %d, local_size_z = %d) in;\n",
             ctx->local_cs_block_size[0], ctx->local_cs_block_size[1], ctx->local_cs_block_size[2]);

   if (ctx->req_local_mem) {
      enum vrend_type_qualifier type = ctx->integer_memory ? INT : UINT;
      emit_hdrf(ctx, "shared %s values[%d];\n", get_string(type), ctx->req_local_mem / 4);
   }
}

static void emit_ios(struct dump_ctx *ctx)
{
   ctx->num_interps = 0;

   if (ctx->so && ctx->so->num_outputs >= PIPE_MAX_SO_OUTPUTS) {
      fprintf(stderr, "Num outputs exceeded, max is %u\n", PIPE_MAX_SO_OUTPUTS);
      set_hdr_error(ctx);
      return;
   }

   switch (ctx->prog_type) {
   case TGSI_PROCESSOR_VERTEX:
      emit_ios_vs(ctx);
      break;
   case TGSI_PROCESSOR_FRAGMENT:
      emit_ios_fs(ctx);
      break;
   case TGSI_PROCESSOR_GEOMETRY:
      emit_ios_geom(ctx);
      break;
   case TGSI_PROCESSOR_TESS_CTRL:
      emit_ios_tcs(ctx);
      break;
   case TGSI_PROCESSOR_TESS_EVAL:
      emit_ios_tes(ctx);
      break;
   case TGSI_PROCESSOR_COMPUTE:
      emit_ios_cs(ctx);
      break;
   default:
      fprintf(stderr, "Unknown shader processor %d\n", ctx->prog_type);
      set_hdr_error(ctx);
      return;
   }

   emit_ios_streamout(ctx);
   emit_ios_common(ctx);

   if (ctx->prog_type == TGSI_PROCESSOR_FRAGMENT &&
       ctx->key->pstipple_tex == true) {
      emit_hdr(ctx, "uniform sampler2D pstipple_sampler;\nfloat stip_temp;\n");
   }
}

static boolean fill_fragment_interpolants(struct dump_ctx *ctx, struct vrend_shader_info *sinfo)
{
   uint32_t i, index = 0;

   for (i = 0; i < ctx->num_inputs; i++) {
      if (ctx->inputs[i].glsl_predefined_no_emit)
         continue;

      if (ctx->inputs[i].name != TGSI_SEMANTIC_GENERIC &&
          ctx->inputs[i].name != TGSI_SEMANTIC_COLOR)
         continue;

      if (index >= ctx->num_interps) {
         fprintf(stderr, "mismatch in number of interps %d %d\n", index, ctx->num_interps);
         return true;
      }
      sinfo->interpinfo[index].semantic_name = ctx->inputs[i].name;
      sinfo->interpinfo[index].semantic_index = ctx->inputs[i].sid;
      sinfo->interpinfo[index].interpolate = ctx->inputs[i].interpolate;
      sinfo->interpinfo[index].location = ctx->inputs[i].location;
      index++;
   }
   return true;
}

static boolean fill_interpolants(struct dump_ctx *ctx, struct vrend_shader_info *sinfo)
{
   boolean ret;

   if (!ctx->num_interps)
      return true;
   if (ctx->prog_type == TGSI_PROCESSOR_VERTEX || ctx->prog_type == TGSI_PROCESSOR_GEOMETRY)
      return true;

   free(sinfo->interpinfo);
   sinfo->interpinfo = calloc(ctx->num_interps, sizeof(struct vrend_interp_info));
   if (!sinfo->interpinfo)
      return false;

   ret = fill_fragment_interpolants(ctx, sinfo);
   if (ret == false)
      goto out_fail;

   return true;
 out_fail:
   free(sinfo->interpinfo);
   return false;
}

static boolean analyze_instruction(struct tgsi_iterate_context *iter,
                                   struct tgsi_full_instruction *inst)
{
   struct dump_ctx *ctx = (struct dump_ctx *)iter;
   uint32_t opcode = inst->Instruction.Opcode;
   if (opcode == TGSI_OPCODE_ATOMIMIN || opcode == TGSI_OPCODE_ATOMIMAX) {
       const struct tgsi_full_src_register *src = &inst->Src[0];
       if (src->Register.File == TGSI_FILE_BUFFER)
         ctx->ssbo_integer_mask |= 1 << src->Register.Index;
       if (src->Register.File == TGSI_FILE_MEMORY)
         ctx->integer_memory = true;
   }

   return true;
}

char *vrend_convert_shader(struct vrend_context *rctx,
                           struct vrend_shader_cfg *cfg,
                           const struct tgsi_token *tokens,
                           uint32_t req_local_mem,
                           struct vrend_shader_key *key,
                           struct vrend_shader_info *sinfo)
{
   struct dump_ctx ctx;
   char *glsl_final = NULL;
   boolean bret;

   memset(&ctx, 0, sizeof(struct dump_ctx));

   /* First pass to deal with edge cases. */
   ctx.iter.iterate_instruction = analyze_instruction;
   bret = tgsi_iterate_shader(tokens, &ctx.iter);
   if (bret == false)
      return NULL;

   ctx.iter.prolog = prolog;
   ctx.iter.iterate_instruction = iter_instruction;
   ctx.iter.iterate_declaration = iter_declaration;
   ctx.iter.iterate_immediate = iter_immediate;
   ctx.iter.iterate_property = iter_property;
   ctx.iter.epilog = NULL;
   ctx.key = key;
   ctx.cfg = cfg;
   ctx.prog_type = -1;
   ctx.num_image_arrays = 0;
   ctx.image_arrays = NULL;
   ctx.num_sampler_arrays = 0;
   ctx.sampler_arrays = NULL;
   ctx.ssbo_array_base = 0xffffffff;
   ctx.ssbo_atomic_array_base = 0xffffffff;
   ctx.has_sample_input = false;
   ctx.req_local_mem = req_local_mem;
   tgsi_scan_shader(tokens, &ctx.info);
   /* if we are in core profile mode we should use GLSL 1.40 */
   if (cfg->use_core_profile && cfg->glsl_version >= 140)
      require_glsl_ver(&ctx, 140);

   if (sinfo->so_info.num_outputs) {
      ctx.so = &sinfo->so_info;
      ctx.so_names = calloc(sinfo->so_info.num_outputs, sizeof(char *));
      if (!ctx.so_names)
         goto fail;
   } else
      ctx.so_names = NULL;

   if (ctx.info.dimension_indirect_files & (1 << TGSI_FILE_CONSTANT))
      require_glsl_ver(&ctx, 150);

   if (ctx.info.indirect_files & (1 << TGSI_FILE_BUFFER) ||
       ctx.info.indirect_files & (1 << TGSI_FILE_IMAGE)) {
      require_glsl_ver(&ctx, 150);
      ctx.shader_req_bits |= SHADER_REQ_GPU_SHADER5;
   }
   if (ctx.info.indirect_files & (1 << TGSI_FILE_SAMPLER))
      ctx.shader_req_bits |= SHADER_REQ_GPU_SHADER5;

   if (!strbuf_alloc(&ctx.glsl_main, 4096))
      goto fail;

   bret = tgsi_iterate_shader(tokens, &ctx.iter);
   if (bret == false)
      goto fail;

   if (strbuf_get_error(&ctx.glsl_main))
      goto fail;

   if (!strbuf_alloc(&ctx.glsl_hdr, 1024))
      goto fail;

   emit_header(&ctx);
   emit_ios(&ctx);

   if (strbuf_get_error(&ctx.glsl_hdr))
      goto fail;

   glsl_final = malloc(strbuf_get_len(&ctx.glsl_hdr) + strbuf_get_len(&ctx.glsl_main) + 1);
   if (!glsl_final)
      goto fail;

   bret = fill_interpolants(&ctx, sinfo);
   if (bret == false)
      goto fail;

   memcpy(glsl_final, ctx.glsl_hdr.buf, strbuf_get_len(&ctx.glsl_hdr));
   memcpy(glsl_final + strbuf_get_len(&ctx.glsl_hdr), ctx.glsl_main.buf, strbuf_get_len(&ctx.glsl_main));
   glsl_final[strbuf_get_len(&ctx.glsl_hdr) + strbuf_get_len(&ctx.glsl_main)] = '\0';

   VREND_DEBUG(dbg_shader_glsl, rctx, "GLSL: %s\n", glsl_final);

   free(ctx.temp_ranges);
   strbuf_free(&ctx.glsl_main);
   strbuf_free(&ctx.glsl_hdr);
   sinfo->num_ucp = ctx.key->clip_plane_enable ? 8 : 0;
   sinfo->has_pervertex_out = ctx.vs_has_pervertex;
   sinfo->has_sample_input = ctx.has_sample_input;
   bool has_prop = (ctx.num_clip_dist_prop + ctx.num_cull_dist_prop) > 0;
   sinfo->num_clip_out = has_prop ? ctx.num_clip_dist_prop : (ctx.num_clip_dist ? ctx.num_clip_dist : 8);
   sinfo->num_cull_out = has_prop ? ctx.num_cull_dist_prop : 0;
   sinfo->samplers_used_mask = ctx.samplers_used;
   sinfo->images_used_mask = ctx.images_used_mask;
   sinfo->num_consts = ctx.num_consts;
   sinfo->ubo_used_mask = ctx.ubo_used_mask;

   sinfo->ssbo_used_mask = ctx.ssbo_used_mask;

   sinfo->ubo_indirect = ctx.info.dimension_indirect_files & (1 << TGSI_FILE_CONSTANT);
   if (ctx_indirect_inputs(&ctx)) {
      if (ctx.generic_input_range.used)
         sinfo->num_indirect_generic_inputs = ctx.generic_input_range.last - ctx.generic_input_range.first + 1;
      if (ctx.patch_input_range.used)
         sinfo->num_indirect_patch_inputs = ctx.patch_input_range.last - ctx.patch_input_range.first + 1;
   }
   if (ctx_indirect_outputs(&ctx)) {
      if (ctx.generic_output_range.used)
         sinfo->num_indirect_generic_outputs = ctx.generic_output_range.last - ctx.generic_output_range.first + 1;
      if (ctx.patch_output_range.used)
         sinfo->num_indirect_patch_outputs = ctx.patch_output_range.last - ctx.patch_output_range.first + 1;
   }

   sinfo->num_inputs = ctx.num_inputs;
   sinfo->num_interps = ctx.num_interps;
   sinfo->num_outputs = ctx.num_outputs;
   sinfo->shadow_samp_mask = ctx.shadow_samp_mask;
   sinfo->glsl_ver = ctx.glsl_ver_required;
   sinfo->gs_out_prim = ctx.gs_out_prim;
   sinfo->tes_prim = ctx.tes_prim_mode;
   sinfo->tes_point_mode = ctx.tes_point_mode;

   if (sinfo->so_names || ctx.so_names) {
      if (sinfo->so_names) {
         for (unsigned i = 0; i < sinfo->so_info.num_outputs; ++i)
            free(sinfo->so_names[i]);
         free(sinfo->so_names);
      }
   }

   sinfo->so_names = ctx.so_names;
   sinfo->attrib_input_mask = ctx.attrib_input_mask;
   sinfo->sampler_arrays = ctx.sampler_arrays;
   sinfo->num_sampler_arrays = ctx.num_sampler_arrays;
   sinfo->image_arrays = ctx.image_arrays;
   sinfo->num_image_arrays = ctx.num_image_arrays;
   return glsl_final;
 fail:
   strbuf_free(&ctx.glsl_main);
   free(glsl_final);
   strbuf_free(&ctx.glsl_hdr);
   free(ctx.so_names);
   free(ctx.temp_ranges);
   return NULL;
}

static void replace_interp(char *program,
                           const char *var_name,
                           const char *pstring, const char *auxstring)
{
   char *ptr;
   int mylen = strlen(INTERP_PREFIX) + strlen("out vec4 ");

   ptr = strstr(program, var_name);

   if (!ptr)
      return;

   ptr -= mylen;

   memset(ptr, ' ', strlen(INTERP_PREFIX));
   memcpy(ptr, pstring, strlen(pstring));
   memcpy(ptr + strlen(pstring), auxstring, strlen(auxstring));
}

static const char *gpu_shader5_string = "#extension GL_ARB_gpu_shader5 : require\n";

static void require_gpu_shader5(char *program)
{
   /* the first line is the #version line */
   char *ptr = strchr(program, '\n');
   if (!ptr)
      return;
   ptr++;

   memcpy(ptr, gpu_shader5_string, strlen(gpu_shader5_string));
}

static const char *gpu_shader5_and_msinterp_string =
      "#extension GL_OES_gpu_shader5 : require\n"
      "#extension GL_OES_shader_multisample_interpolation : require\n";

static void require_gpu_shader5_and_msinterp(char *program)
{
   /* the first line is the #version line */
   char *ptr = strchr(program, '\n');
   if (!ptr)
      return;
   ptr++;

   memcpy(ptr, gpu_shader5_and_msinterp_string, strlen(gpu_shader5_and_msinterp_string));
}

bool vrend_patch_vertex_shader_interpolants(struct vrend_context *rctx,
                                            struct vrend_shader_cfg *cfg, char *program,
                                            struct vrend_shader_info *vs_info,
                                            struct vrend_shader_info *fs_info,
                                            const char *oprefix, bool flatshade)
{
   int i;
   const char *pstring, *auxstring;
   char glsl_name[64];
   if (!vs_info || !fs_info)
      return true;

   if (!fs_info->interpinfo)
      return true;

   if (fs_info->has_sample_input) {
      if (!cfg->use_gles && (cfg->glsl_version >= 320))
          require_gpu_shader5(program);

      if (cfg->use_gles && (cfg->glsl_version < 320))
         require_gpu_shader5_and_msinterp(program);
   }

   for (i = 0; i < fs_info->num_interps; i++) {
      pstring = get_interp_string(cfg, fs_info->interpinfo[i].interpolate, flatshade);
      if (!pstring)
         continue;

      auxstring = get_aux_string(fs_info->interpinfo[i].location);

      switch (fs_info->interpinfo[i].semantic_name) {
      case TGSI_SEMANTIC_COLOR:
      case TGSI_SEMANTIC_BCOLOR:
         /* color is a bit trickier */
         if (fs_info->glsl_ver < 140) {
            if (fs_info->interpinfo[i].semantic_index == 1) {
               replace_interp(program, "gl_FrontSecondaryColor", pstring, auxstring);
               replace_interp(program, "gl_BackSecondaryColor", pstring, auxstring);
            } else {
               replace_interp(program, "gl_FrontColor", pstring, auxstring);
               replace_interp(program, "gl_BackColor", pstring, auxstring);
            }
         } else {
            snprintf(glsl_name, 64, "ex_c%d", fs_info->interpinfo[i].semantic_index);
            replace_interp(program, glsl_name, pstring, auxstring);
            snprintf(glsl_name, 64, "ex_bc%d", fs_info->interpinfo[i].semantic_index);
            replace_interp(program, glsl_name, pstring, auxstring);
         }
         break;
      case TGSI_SEMANTIC_GENERIC:
         snprintf(glsl_name, 64, "%s_g%d", oprefix, fs_info->interpinfo[i].semantic_index);
         replace_interp(program, glsl_name, pstring, auxstring);
         break;
      default:
         fprintf(stderr,"unhandled semantic: %x\n", fs_info->interpinfo[i].semantic_name);
         return false;
      }
   }

   VREND_DEBUG(dbg_shader_glsl, rctx, "GLSL: post interp:  %s\n", program);

   return true;
}
