/**************************************************************************
 *
 * Copyright © 2010 Jakob Bornecrantz
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/


#define USE_TRACE 0
#define WIDTH 300
#define HEIGHT 300
#define NEAR 30
#define FAR 1000
#define FLIP 0

/* pipe_*_state structs */
#include "pipe/p_state.h"
/* pipe_context */
#include "pipe/p_context.h"
/* pipe_screen */
#include "pipe/p_screen.h"
/* PIPE_* */
#include "pipe/p_defines.h"
/* TGSI_SEMANTIC_{POSITION|GENERIC} */
#include "pipe/p_shader_tokens.h"
/* pipe_buffer_* helpers */
#include "util/u_inlines.h"

/* constant state object helper */
#include "cso_cache/cso_context.h"

/* debug_dump_surface_bmp */
#include "util/u_debug.h"
/* util_draw_vertex_buffer helper */
#include "util/u_draw_quad.h"
/* FREE & CALLOC_STRUCT */
#include "util/u_memory.h"
/* util_make_[fragment|vertex]_passthrough_shader */
#include "util/u_simple_shaders.h"

/* softpipe software driver */
#include "softpipe/sp_public.h"

/* null software winsys */
#include "sw/null/null_sw_winsys.h"

/* traceing support see src/gallium/drivers/trace/README for more info. */
#if USE_TRACE
#include "trace/tr_screen.h"
#include "trace/tr_context.h"
#endif

struct program
{
	struct pipe_screen *screen;
	struct pipe_context *pipe;
	struct cso_context *cso;

	struct pipe_blend_state blend;
	struct pipe_depth_stencil_alpha_state depthstencil;
	struct pipe_rasterizer_state rasterizer;
	struct pipe_viewport_state viewport;
	struct pipe_framebuffer_state framebuffer;
	struct pipe_vertex_element velem[2];

	void *vs;
	void *fs;

	float clear_color[4];

	struct pipe_buffer *vbuf;
	struct pipe_texture *target;
};

static void init_prog(struct program *p)
{
	/* create the software rasterizer */
	p->screen = softpipe_create_screen(null_sw_create());
#if USE_TRACE
	p->screen = trace_screen_create(p->screen);
#endif
	p->pipe = p->screen->context_create(p->screen, NULL);
	p->cso = cso_create_context(p->pipe);

	/* set clear color */
	p->clear_color[0] = 0.3;
	p->clear_color[1] = 0.1;
	p->clear_color[2] = 0.3;
	p->clear_color[3] = 1.0;

	/* vertex buffer */
	{
		float vertices[4][2][4] = {
			{
				{ 0.0f, -0.9f, 0.0f, 1.0f },
				{ 1.0f, 0.0f, 0.0f, 1.0f }
			},
			{
				{ -0.9f, 0.9f, 0.0f, 1.0f },
				{ 0.0f, 1.0f, 0.0f, 1.0f }
			},
			{
				{ 0.9f, 0.9f, 0.0f, 1.0f },
				{ 0.0f, 0.0f, 1.0f, 1.0f }
			}
		};

		p->vbuf = pipe_buffer_create(p->screen, 16, PIPE_BUFFER_USAGE_VERTEX, sizeof(vertices));
		pipe_buffer_write(p->screen, p->vbuf, 0, sizeof(vertices), vertices);
	}

	/* render target texture */
	{
		struct pipe_texture tmplt;
		memset(&tmplt, 0, sizeof(tmplt));
		tmplt.target = PIPE_TEXTURE_2D;
		tmplt.format = PIPE_FORMAT_B8G8R8A8_UNORM; /* All drivers support this */
		tmplt.width0 = WIDTH;
		tmplt.height0 = HEIGHT;
		tmplt.depth0 = 1;
		tmplt.last_level = 0;
		tmplt.tex_usage = PIPE_TEXTURE_USAGE_RENDER_TARGET;

		p->target = p->screen->texture_create(p->screen, &tmplt);
	}

	/* disabled blending/masking */
	memset(&p->blend, 0, sizeof(p->blend));
	p->blend.rt[0].colormask = PIPE_MASK_RGBA;

	/* no-op depth/stencil/alpha */
	memset(&p->depthstencil, 0, sizeof(p->depthstencil));

	/* rasterizer */
	memset(&p->rasterizer, 0, sizeof(p->rasterizer));
	p->rasterizer.front_winding = PIPE_WINDING_CW;
	p->rasterizer.cull_mode = PIPE_WINDING_NONE;
	p->rasterizer.gl_rasterization_rules = 1;

	/* drawing destination */
	memset(&p->framebuffer, 0, sizeof(p->framebuffer));
	p->framebuffer.width = WIDTH;
	p->framebuffer.height = HEIGHT;
	p->framebuffer.nr_cbufs = 1;
	p->framebuffer.cbufs[0] = p->screen->get_tex_surface(p->screen, p->target, 0, 0, 0, PIPE_BUFFER_USAGE_GPU_WRITE);

	/* viewport, depth isn't really needed */
	{
		float x = 0;
		float y = 0;
		float z = FAR;
		float half_width = (float)WIDTH / 2.0f;
		float half_height = (float)HEIGHT / 2.0f;
		float half_depth = ((float)FAR - (float)NEAR) / 2.0f;
		float scale, bias;

		if (FLIP) {
			scale = -1.0f;
			bias = (float)HEIGHT;
		} else {
			scale = 1.0f;
			bias = 0.0f;
		}

		p->viewport.scale[0] = half_width;
		p->viewport.scale[1] = half_height * scale;
		p->viewport.scale[2] = half_depth;
		p->viewport.scale[3] = 1.0f;

		p->viewport.translate[0] = half_width + x;
		p->viewport.translate[1] = (half_height + y) * scale + bias;
		p->viewport.translate[2] = half_depth + z;
		p->viewport.translate[3] = 0.0f;
	}

	/* vertex elements state */
	memset(p->velem, 0, sizeof(p->velem));
	p->velem[0].src_offset = 0 * 4 * sizeof(float); /* offset 0, first element */
	p->velem[0].instance_divisor = 0;
	p->velem[0].vertex_buffer_index = 0;
	p->velem[0].src_format = PIPE_FORMAT_R32G32B32A32_FLOAT;

	p->velem[1].src_offset = 1 * 4 * sizeof(float); /* offset 16, second element */
	p->velem[1].instance_divisor = 0;
	p->velem[1].vertex_buffer_index = 0;
	p->velem[1].src_format = PIPE_FORMAT_R32G32B32A32_FLOAT;

	/* vertex shader */
	{
			const uint semantic_names[] = { TGSI_SEMANTIC_POSITION,
							TGSI_SEMANTIC_COLOR };
			const uint semantic_indexes[] = { 0, 0 };
			p->vs = util_make_vertex_passthrough_shader(p->pipe, 2, semantic_names, semantic_indexes);
	}

	/* fragment shader */
	p->fs = util_make_fragment_passthrough_shader(p->pipe);
}

static void close_prog(struct program *p)
{
	/* unset all state */
	cso_release_all(p->cso);

	p->pipe->delete_vs_state(p->pipe, p->vs);
	p->pipe->delete_fs_state(p->pipe, p->fs);

	pipe_surface_reference(&p->framebuffer.cbufs[0], NULL);
	pipe_texture_reference(&p->target, NULL);
	pipe_buffer_reference(&p->vbuf, NULL);

	cso_destroy_context(p->cso);
	p->pipe->destroy(p->pipe);
	p->screen->destroy(p->screen);

	FREE(p);
}

static void draw(struct program *p)
{
	/* set the render target */
	cso_set_framebuffer(p->cso, &p->framebuffer);

	/* clear the render target */
	p->pipe->clear(p->pipe, PIPE_CLEAR_COLOR, p->clear_color, 0, 0);

	/* set misc state we care about */
	cso_set_blend(p->cso, &p->blend);
	cso_set_depth_stencil_alpha(p->cso, &p->depthstencil);
	cso_set_rasterizer(p->cso, &p->rasterizer);
	cso_set_viewport(p->cso, &p->viewport);

	/* shaders */
	cso_set_fragment_shader_handle(p->cso, p->fs);
	cso_set_vertex_shader_handle(p->cso, p->vs);

	/* vertex element data */
	cso_set_vertex_elements(p->cso, 2, p->velem);

	util_draw_vertex_buffer(p->pipe,
	                        p->vbuf, 0,
	                        PIPE_PRIM_TRIANGLES,
	                        3,  /* verts */
	                        2); /* attribs/vert */

	p->pipe->flush(p->pipe, PIPE_FLUSH_RENDER_CACHE, NULL);

	debug_dump_surface_bmp(p->pipe, "result.bmp", p->framebuffer.cbufs[0]);
}

int main(int argc, char** argv)
{
	struct program *p = CALLOC_STRUCT(program);

	init_prog(p);
	draw(p);
	close_prog(p);

	return 0;
}
