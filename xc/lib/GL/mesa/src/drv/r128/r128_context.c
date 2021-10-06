/* $XFree86: xc/lib/GL/mesa/src/drv/r128/r128_context.c,v 1.10 2003/12/08 22:45:30 alanh Exp $ */
/**************************************************************************

Copyright 1999, 2000 ATI Technologies Inc. and Precision Insight, Inc.,
                                               Cedar Park, Texas.
All Rights Reserved.

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
on the rights to use, copy, modify, merge, publish, distribute, sub
license, and/or sell copies of the Software, and to permit persons to whom
the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice (including the next
paragraph) shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
ATI, PRECISION INSIGHT AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
USE OR OTHER DEALINGS IN THE SOFTWARE.

**************************************************************************/

/*
 * Authors:
 *   Kevin E. Martin <martin@valinux.com>
 *   Gareth Hughes <gareth@valinux.com>
 *
 */

#include "glheader.h"
#include "context.h"
#include "simple_list.h"
#include "imports.h"
#include "matrix.h"
#include "extensions.h"

#include "swrast/swrast.h"
#include "swrast_setup/swrast_setup.h"
#include "array_cache/acache.h"

#include "tnl/tnl.h"
#include "tnl/t_pipeline.h"

#include "r128_context.h"
#include "r128_ioctl.h"
#include "r128_dd.h"
#include "r128_state.h"
#include "r128_span.h"
#include "r128_tex.h"
#include "r128_tris.h"
#include "r128_vb.h"

#include "vblank.h"
#include "utils.h"
#include "texmem.h"

#ifndef R128_DEBUG
int R128_DEBUG = 0;
#endif

static const char * const card_extensions[] =
{
   "GL_ARB_multitexture",
   "GL_ARB_texture_env_add",
   "GL_ARB_texture_mirrored_repeat",
   "GL_EXT_texture_edge_clamp",
   "GL_EXT_texture_env_add",
   "GL_IBM_texture_mirrored_repeat",
   "GL_SGIS_generate_mipmap",
   "GL_SGIS_texture_edge_clamp",
   NULL
};

static const struct dri_debug_control debug_control[] =
{
    { "ioctl", DEBUG_VERBOSE_IOCTL },
    { "verb",  DEBUG_VERBOSE_MSG },
    { "dri",   DEBUG_VERBOSE_DRI },
    { "2d",    DEBUG_VERBOSE_2D },
    { "sync",  DEBUG_ALWAYS_SYNC },
    { "api",   DEBUG_VERBOSE_API },
    { NULL,    0 }
};

/* Create the device specific context.
 */
GLboolean r128CreateContext( const __GLcontextModes *glVisual,
			     __DRIcontextPrivate *driContextPriv,
                             void *sharedContextPrivate )
{
   GLcontext *ctx, *shareCtx;
   __DRIscreenPrivate *sPriv = driContextPriv->driScreenPriv;
   r128ContextPtr rmesa;
   r128ScreenPtr r128scrn;
   int i;

   /* Allocate the r128 context */
   rmesa = (r128ContextPtr) CALLOC( sizeof(*rmesa) );
   if ( !rmesa )
      return GL_FALSE;

   /* Allocate the Mesa context */
   if (sharedContextPrivate)
      shareCtx = ((r128ContextPtr) sharedContextPrivate)->glCtx;
   else 
      shareCtx = NULL;
   rmesa->glCtx = _mesa_create_context(glVisual, shareCtx, (void *) rmesa, GL_TRUE);
   if (!rmesa->glCtx) {
      FREE(rmesa);
      return GL_FALSE;
   }
   driContextPriv->driverPrivate = rmesa;
   ctx = rmesa->glCtx;

   rmesa->driContext = driContextPriv;
   rmesa->driScreen = sPriv;
   rmesa->driDrawable = NULL;
   rmesa->hHWContext = driContextPriv->hHWContext;
   rmesa->driHwLock = &sPriv->pSAREA->lock;
   rmesa->driFd = sPriv->fd;

   r128scrn = rmesa->r128Screen = (r128ScreenPtr)(sPriv->private);

   rmesa->sarea = (R128SAREAPrivPtr)((char *)sPriv->pSAREA +
				     r128scrn->sarea_priv_offset);

   rmesa->CurrentTexObj[0] = NULL;
   rmesa->CurrentTexObj[1] = NULL;

   (void) memset( rmesa->texture_heaps, 0, sizeof( rmesa->texture_heaps ) );
   make_empty_list( & rmesa->swapped );

   rmesa->nr_heaps = r128scrn->numTexHeaps;
   for ( i = 0 ; i < rmesa->nr_heaps ; i++ ) {
      rmesa->texture_heaps[i] = driCreateTextureHeap( i, rmesa,
	    r128scrn->texSize[i],
	    12,
	    R128_NR_TEX_REGIONS,
	    rmesa->sarea->texList[i],
	    & rmesa->sarea->texAge[i],
	    & rmesa->swapped,
	    sizeof( r128TexObj ),
	    (destroy_texture_object_t *) r128DestroyTexObj );

      driSetTextureSwapCounterLocation( rmesa->texture_heaps[i],
					& rmesa->c_textureSwaps );
   }


   rmesa->RenderIndex = -1;		/* Impossible value */
   rmesa->vert_buf = NULL;
   rmesa->num_verts = 0;

   /* Set the maximum texture size small enough that we can guarentee that
    * all texture units can bind a maximal texture and have them both in
    * texturable memory at once.
    */

   ctx->Const.MaxTextureUnits = 2;

   driCalculateMaxTextureLevels( rmesa->texture_heaps,
				 rmesa->nr_heaps,
				 & ctx->Const,
				 4,
				 10, /* max 2D texture size is 1024x1024 */
				 0,  /* 3D textures unsupported. */
				 0,  /* cube textures unsupported. */
				 0,  /* texture rectangles unsupported. */
				 11,
				 GL_FALSE );

   /* No wide points.
    */
   ctx->Const.MinPointSize = 1.0;
   ctx->Const.MinPointSizeAA = 1.0;
   ctx->Const.MaxPointSize = 1.0;
   ctx->Const.MaxPointSizeAA = 1.0;

   /* No wide lines.
    */
   ctx->Const.MinLineWidth = 1.0;
   ctx->Const.MinLineWidthAA = 1.0;
   ctx->Const.MaxLineWidth = 1.0;
   ctx->Const.MaxLineWidthAA = 1.0;
   ctx->Const.LineWidthGranularity = 1.0;

#if ENABLE_PERF_BOXES
   rmesa->boxes = (getenv( "LIBGL_PERFORMANCE_BOXES" ) != NULL);
#endif

   /* Initialize the software rasterizer and helper modules.
    */
   _swrast_CreateContext( ctx );
   _ac_CreateContext( ctx );
   _tnl_CreateContext( ctx );
   _swsetup_CreateContext( ctx );

   /* Install the customized pipeline:
    */
/*     _tnl_destroy_pipeline( ctx ); */
/*     _tnl_install_pipeline( ctx, r128_pipeline ); */

   /* Configure swrast to match hardware characteristics:
    */
   _swrast_allow_pixel_fog( ctx, GL_FALSE );
   _swrast_allow_vertex_fog( ctx, GL_TRUE );

   driInitExtensions( ctx, card_extensions, GL_TRUE );
   if (sPriv->drmMinor >= 4)
      _mesa_enable_extension( ctx, "GL_MESA_ycbcr_texture" );

   r128InitVB( ctx );
   r128InitTriFuncs( ctx );
   r128DDInitDriverFuncs( ctx );
   r128DDInitIoctlFuncs( ctx );
   r128DDInitStateFuncs( ctx );
   r128DDInitSpanFuncs( ctx );
   r128DDInitTextureFuncs( ctx );
   r128DDInitState( rmesa );

   rmesa->do_irqs = (rmesa->r128Screen->irq && !getenv("R128_NO_IRQS"));

   rmesa->vblank_flags = (rmesa->r128Screen->irq != 0)
       ? driGetDefaultVBlankFlags() : VBLANK_FLAG_NO_IRQ;

   driContextPriv->driverPrivate = (void *)rmesa;

#if DO_DEBUG
   R128_DEBUG = driParseDebugString( getenv( "R128_DEBUG" ),
				     debug_control );
#endif

   return GL_TRUE;
}

/* Destroy the device specific context.
 */
void r128DestroyContext( __DRIcontextPrivate *driContextPriv  )
{
   r128ContextPtr rmesa = (r128ContextPtr) driContextPriv->driverPrivate;

   assert(rmesa);  /* should never be null */
   if ( rmesa ) {
      GLboolean   release_texture_heaps;


      release_texture_heaps = (rmesa->glCtx->Shared->RefCount == 1);

      _swsetup_DestroyContext( rmesa->glCtx );
      _tnl_DestroyContext( rmesa->glCtx );
      _ac_DestroyContext( rmesa->glCtx );
      _swrast_DestroyContext( rmesa->glCtx );

      r128FreeVB( rmesa->glCtx );

      /* free the Mesa context */
      rmesa->glCtx->DriverCtx = NULL;
      _mesa_destroy_context(rmesa->glCtx);

      if ( release_texture_heaps ) {
         /* This share group is about to go away, free our private
          * texture object data.
          */
         int i;

         for ( i = 0 ; i < rmesa->nr_heaps ; i++ ) {
	    driDestroyTextureHeap( rmesa->texture_heaps[ i ] );
	    rmesa->texture_heaps[ i ] = NULL;
         }

	 assert( is_empty_list( & rmesa->swapped ) );
      }

      FREE( rmesa );
   }

#if 0
   /* Use this to force shared object profiling. */
   glx_fini_prof();
#endif
}


/* Force the context `c' to be the current context and associate with it
 * buffer `b'.
 */
GLboolean
r128MakeCurrent( __DRIcontextPrivate *driContextPriv,
                 __DRIdrawablePrivate *driDrawPriv,
                 __DRIdrawablePrivate *driReadPriv )
{
   if ( driContextPriv ) {
      GET_CURRENT_CONTEXT(ctx);
      r128ContextPtr oldR128Ctx = ctx ? R128_CONTEXT(ctx) : NULL;
      r128ContextPtr newR128Ctx = (r128ContextPtr) driContextPriv->driverPrivate;

      if ( newR128Ctx != oldR128Ctx ) {
	 newR128Ctx->new_state |= R128_NEW_CONTEXT;
	 newR128Ctx->dirty = R128_UPLOAD_ALL;
      }

      newR128Ctx->driDrawable = driDrawPriv;

      _mesa_make_current2( newR128Ctx->glCtx,
                           (GLframebuffer *) driDrawPriv->driverPrivate,
                           (GLframebuffer *) driReadPriv->driverPrivate );


      newR128Ctx->new_state |= R128_NEW_WINDOW | R128_NEW_CLIP;

      if ( !newR128Ctx->glCtx->Viewport.Width ) {
	 _mesa_set_viewport(newR128Ctx->glCtx, 0, 0,
                            driDrawPriv->w, driDrawPriv->h);
      }
   } else {
      _mesa_make_current( 0, 0 );
   }

   return GL_TRUE;
}


/* Force the context `c' to be unbound from its buffer.
 */
GLboolean
r128UnbindContext( __DRIcontextPrivate *driContextPriv )
{
   return GL_TRUE;
}
