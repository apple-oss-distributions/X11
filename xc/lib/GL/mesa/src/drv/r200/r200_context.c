/* $XFree86: xc/lib/GL/mesa/src/drv/r200/r200_context.c,v 1.6 2004/01/23 03:57:05 dawes Exp $ */
/*
Copyright (C) The Weather Channel, Inc.  2002.  All Rights Reserved.

The Weather Channel (TM) funded Tungsten Graphics to develop the
initial release of the Radeon 8500 driver under the XFree86 license.
This notice must be preserved.

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice (including the
next paragraph) shall be included in all copies or substantial
portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE COPYRIGHT OWNER(S) AND/OR ITS SUPPLIERS BE
LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

**************************************************************************/

/*
 * Authors:
 *   Keith Whitwell <keith@tungstengraphics.com>
 */

#include "glheader.h"
#include "api_arrayelt.h"
#include "context.h"
#include "simple_list.h"
#include "imports.h"
#include "matrix.h"
#include "extensions.h"
#include "state.h"

#include "swrast/swrast.h"
#include "swrast_setup/swrast_setup.h"
#include "array_cache/acache.h"

#include "tnl/tnl.h"
#include "tnl/t_pipeline.h"

#include "r200_context.h"
#include "r200_ioctl.h"
#include "r200_state.h"
#include "r200_span.h"
#include "r200_pixel.h"
#include "r200_tex.h"
#include "r200_swtcl.h"
#include "r200_tcl.h"
#include "r200_vtxfmt.h"
#include "r200_maos.h"

#define DRIVER_DATE	"20030328"

#include "vblank.h"
#include "utils.h"
#ifndef R200_DEBUG
int R200_DEBUG = (0);
#endif



/* Return the width and height of the given buffer.
 */
static void r200GetBufferSize( GLframebuffer *buffer,
			       GLuint *width, GLuint *height )
{
   GET_CURRENT_CONTEXT(ctx);
   r200ContextPtr rmesa = R200_CONTEXT(ctx);

   LOCK_HARDWARE( rmesa );
   *width  = rmesa->dri.drawable->w;
   *height = rmesa->dri.drawable->h;
   UNLOCK_HARDWARE( rmesa );
}

/* Return various strings for glGetString().
 */
static const GLubyte *r200GetString( GLcontext *ctx, GLenum name )
{
   r200ContextPtr rmesa = R200_CONTEXT(ctx);
   static char buffer[128];
   unsigned   offset;
   GLuint agp_mode = rmesa->r200Screen->IsPCI ? 0 :
      rmesa->r200Screen->AGPMode;

   switch ( name ) {
   case GL_VENDOR:
      return (GLubyte *)"Tungsten Graphics, Inc.";

   case GL_RENDERER:
      offset = driGetRendererString( buffer, "R200", DRIVER_DATE,
				     agp_mode );

      sprintf( & buffer[ offset ], " %sTCL",
	       !(rmesa->TclFallback & R200_TCL_FALLBACK_TCL_DISABLE)
	       ? "" : "NO-" );

      return (GLubyte *)buffer;

   default:
      return NULL;
   }
}


/* Extension strings exported by the R200 driver.
 */
static const char * const card_extensions[] =
{
    "GL_ARB_multisample",
    "GL_ARB_multitexture",
    "GL_ARB_texture_border_clamp",
    "GL_ARB_texture_compression",
    "GL_ARB_texture_env_add",
    "GL_ARB_texture_env_combine",
    "GL_ARB_texture_env_dot3",
    "GL_ARB_texture_mirrored_repeat",
    "GL_EXT_blend_logic_op",
    "GL_EXT_blend_minmax",
    "GL_EXT_blend_subtract",
    "GL_EXT_secondary_color",
    "GL_EXT_stencil_wrap",
    "GL_EXT_texture_edge_clamp",
    "GL_EXT_texture_env_add",
    "GL_EXT_texture_env_combine",
    "GL_EXT_texture_env_dot3",
    "GL_EXT_texture_filter_anisotropic",
    "GL_EXT_texture_lod_bias",
    "GL_ATI_texture_env_combine3",
    "GL_ATI_texture_mirror_once",
    "GL_IBM_texture_mirrored_repeat",
    "GL_MESA_pack_invert",
    "GL_MESA_ycbcr_texture",
    "GL_NV_blend_square",
    "GL_NV_texture_rectangle",
    "GL_SGIS_generate_mipmap",
    "GL_SGIS_texture_border_clamp",
    "GL_SGIS_texture_edge_clamp",
    NULL
};

extern const struct gl_pipeline_stage _r200_render_stage;
extern const struct gl_pipeline_stage _r200_tcl_stage;

static const struct gl_pipeline_stage *r200_pipeline[] = {

   /* Try and go straight to t&l
    */
   &_r200_tcl_stage,  

   /* Catch any t&l fallbacks
    */
   &_tnl_vertex_transform_stage,
   &_tnl_normal_transform_stage,
   &_tnl_lighting_stage,
   &_tnl_fog_coordinate_stage,
   &_tnl_texgen_stage,
   &_tnl_texture_transform_stage,

   /* Try again to go to tcl? 
    *     - no good for asymmetric-twoside (do with multipass)
    *     - no good for asymmetric-unfilled (do with multipass)
    *     - good for material
    *     - good for texgen
    *     - need to manipulate a bit of state
    *
    * - worth it/not worth it?
    */
			
   /* Else do them here.
    */
/*    &_r200_render_stage,  */ /* FIXME: bugs with ut2003 */
   &_tnl_render_stage,		/* FALLBACK:  */
   0,
};



/* Initialize the driver's misc functions.
 */
static void r200InitDriverFuncs( GLcontext *ctx )
{
    ctx->Driver.GetBufferSize		= r200GetBufferSize;
    ctx->Driver.ResizeBuffers           = _swrast_alloc_buffers;
    ctx->Driver.GetString		= r200GetString;

    ctx->Driver.Error			= NULL;
    ctx->Driver.DrawPixels		= NULL;
    ctx->Driver.Bitmap			= NULL;
}

static const struct dri_debug_control debug_control[] =
{
    { "fall",  DEBUG_FALLBACKS },
    { "tex",   DEBUG_TEXTURE },
    { "ioctl", DEBUG_IOCTL },
    { "prim",  DEBUG_PRIMS },
    { "vert",  DEBUG_VERTS },
    { "state", DEBUG_STATE },
    { "code",  DEBUG_CODEGEN },
    { "vfmt",  DEBUG_VFMT },
    { "vtxf",  DEBUG_VFMT },
    { "verb",  DEBUG_VERBOSE },
    { "dri",   DEBUG_DRI },
    { "dma",   DEBUG_DMA },
    { "san",   DEBUG_SANITY },
    { "sync",  DEBUG_SYNC },
    { "pix",   DEBUG_PIXEL },
    { "mem",   DEBUG_MEMORY },
    { NULL,    0 }
};


static int
get_ust_nop( int64_t * ust )
{
   *ust = 1;
   return 0;
}


/* Create the device specific context.
 */
GLboolean r200CreateContext( const __GLcontextModes *glVisual,
			     __DRIcontextPrivate *driContextPriv,
			     void *sharedContextPrivate)
{
   __DRIscreenPrivate *sPriv = driContextPriv->driScreenPriv;
   r200ScreenPtr screen = (r200ScreenPtr)(sPriv->private);
   r200ContextPtr rmesa;
   GLcontext *ctx, *shareCtx;
   int i;

   assert(glVisual);
   assert(driContextPriv);
   assert(screen);

   /* Allocate the R200 context */
   rmesa = (r200ContextPtr) CALLOC( sizeof(*rmesa) );
   if ( !rmesa )
      return GL_FALSE;

   /* Allocate the Mesa context */
   if (sharedContextPrivate)
      shareCtx = ((r200ContextPtr) sharedContextPrivate)->glCtx;
   else
      shareCtx = NULL;
   rmesa->glCtx = _mesa_create_context(glVisual, shareCtx, (void *) rmesa, GL_TRUE);
   if (!rmesa->glCtx) {
      FREE(rmesa);
      return GL_FALSE;
   }
   driContextPriv->driverPrivate = rmesa;

   /* Init r200 context data */
   rmesa->dri.context = driContextPriv;
   rmesa->dri.screen = sPriv;
   rmesa->dri.drawable = NULL; /* Set by XMesaMakeCurrent */
   rmesa->dri.hwContext = driContextPriv->hHWContext;
   rmesa->dri.hwLock = &sPriv->pSAREA->lock;
   rmesa->dri.fd = sPriv->fd;
   rmesa->dri.drmMinor = sPriv->drmMinor;

   rmesa->r200Screen = screen;
   rmesa->sarea = (RADEONSAREAPrivPtr)((GLubyte *)sPriv->pSAREA +
				       screen->sarea_priv_offset);


   rmesa->dma.buf0_address = rmesa->r200Screen->buffers->list[0].address;

   (void) memset( rmesa->texture_heaps, 0, sizeof( rmesa->texture_heaps ) );
   make_empty_list( & rmesa->swapped );

   rmesa->nr_heaps = 1 /* screen->numTexHeaps */ ;
   for ( i = 0 ; i < rmesa->nr_heaps ; i++ ) {
      rmesa->texture_heaps[i] = driCreateTextureHeap( i, rmesa,
	    screen->texSize[i],
	    12,
	    RADEON_NR_TEX_REGIONS,
	    rmesa->sarea->texList[i],
	    & rmesa->sarea->texAge[i],
	    & rmesa->swapped,
	    sizeof( r200TexObj ),
	    (destroy_texture_object_t *) r200DestroyTexObj );
   }

   rmesa->swtcl.RenderIndex = ~0;
   rmesa->lost_context = 1;

   /* Set the maximum texture size small enough that we can guarentee that
    * all texture units can bind a maximal texture and have them both in
    * texturable memory at once.
    */

   ctx = rmesa->glCtx;
   ctx->Const.MaxTextureUnits = 2;

   driCalculateMaxTextureLevels( rmesa->texture_heaps,
				 rmesa->nr_heaps,
				 & ctx->Const,
				 4,
				 11, /* max 2D texture size is 2048x2048 */
#if ENABLE_HW_3D_TEXTURE
				 8,  /* max 3D texture size is 256^3 */
#else
				 0,  /* 3D textures unsupported */
#endif
				 11, /* max cube texture size is 2048x2048 */
				 11, /* max texture rectangle size is 2048x2048 */
				 12,
				 GL_FALSE );

   ctx->Const.MaxTextureMaxAnisotropy = 16.0;

   /* No wide points.
    */
   ctx->Const.MinPointSize = 1.0;
   ctx->Const.MinPointSizeAA = 1.0;
   ctx->Const.MaxPointSize = 1.0;
   ctx->Const.MaxPointSizeAA = 1.0;

   ctx->Const.MinLineWidth = 1.0;
   ctx->Const.MinLineWidthAA = 1.0;
   ctx->Const.MaxLineWidth = 10.0;
   ctx->Const.MaxLineWidthAA = 10.0;
   ctx->Const.LineWidthGranularity = 0.0625;

   /* Initialize the software rasterizer and helper modules.
    */
   _swrast_CreateContext( ctx );
   _ac_CreateContext( ctx );
   _tnl_CreateContext( ctx );
   _swsetup_CreateContext( ctx );
   _ae_create_context( ctx );

   /* Install the customized pipeline:
    */
   _tnl_destroy_pipeline( ctx );
   _tnl_install_pipeline( ctx, r200_pipeline );
   ctx->Driver.FlushVertices = r200FlushVertices;

   /* Try and keep materials and vertices separate:
    */
   _tnl_isolate_materials( ctx, GL_TRUE );


   /* Configure swrast to match hardware characteristics:
    */
   _swrast_allow_pixel_fog( ctx, GL_FALSE );
   _swrast_allow_vertex_fog( ctx, GL_TRUE );


   _math_matrix_ctr( &rmesa->TexGenMatrix[0] );
   _math_matrix_ctr( &rmesa->TexGenMatrix[1] );
   _math_matrix_ctr( &rmesa->tmpmat );
   _math_matrix_set_identity( &rmesa->TexGenMatrix[0] );
   _math_matrix_set_identity( &rmesa->TexGenMatrix[1] );
   _math_matrix_set_identity( &rmesa->tmpmat );

   driInitExtensions( ctx, card_extensions, GL_TRUE );
   if (rmesa->r200Screen->drmSupportsCubeMaps)
      _mesa_enable_extension( ctx, "GL_ARB_texture_cube_map" );

   r200InitDriverFuncs( ctx );
   r200InitIoctlFuncs( ctx );
   r200InitStateFuncs( ctx );
   r200InitSpanFuncs( ctx );
   r200InitPixelFuncs( ctx );
   r200InitTextureFuncs( ctx );
   r200InitState( rmesa );
   r200InitSwtcl( ctx );

   rmesa->iw.irq_seq = -1;
   rmesa->irqsEmitted = 0;
   rmesa->do_irqs = (rmesa->dri.drmMinor >= 6 && 
		     !getenv("R200_NO_IRQS") &&
		     rmesa->r200Screen->irq);

   if (!rmesa->do_irqs)
      fprintf(stderr, 
	      "IRQ's not enabled, falling back to busy waits: %d %d %d\n",
	      rmesa->dri.drmMinor,
	      !!getenv("R200_NO_IRQS"),
	      rmesa->r200Screen->irq);


   rmesa->do_usleeps = !getenv("R200_NO_USLEEPS");

   rmesa->vblank_flags = (rmesa->r200Screen->irq != 0)
       ? driGetDefaultVBlankFlags() : VBLANK_FLAG_NO_IRQ;

   rmesa->prefer_gart_client_texturing = 
      (getenv("R200_GART_CLIENT_TEXTURES") != 0);
   
   rmesa->get_ust = (PFNGLXGETUSTPROC) glXGetProcAddress( (const GLubyte *) "__glXGetUST" );
   if ( rmesa->get_ust == NULL ) {
      rmesa->get_ust = get_ust_nop;
   }

   (*rmesa->get_ust)( & rmesa->swap_ust );


#if DO_DEBUG
   R200_DEBUG  = driParseDebugString( getenv( "R200_DEBUG" ),
				      debug_control );
   R200_DEBUG |= driParseDebugString( getenv( "RADEON_DEBUG" ),
				      debug_control );
#endif

   if (getenv("R200_NO_RAST")) {
      fprintf(stderr, "disabling 3D acceleration\n");
      FALLBACK(rmesa, R200_FALLBACK_DISABLE, 1); 
   }
   else if (getenv("R200_NO_TCL")) {
      fprintf(stderr, "disabling TCL support\n");
      TCL_FALLBACK(rmesa->glCtx, R200_TCL_FALLBACK_TCL_DISABLE, 1); 
   }
   else {
      if (!getenv("R200_NO_VTXFMT")) {
	 r200VtxfmtInit( ctx );
      }
      _tnl_need_dlist_norm_lengths( ctx, GL_FALSE );
   }
   return GL_TRUE;
}


/* Destroy the device specific context.
 */
/* Destroy the Mesa and driver specific context data.
 */
void r200DestroyContext( __DRIcontextPrivate *driContextPriv )
{
   GET_CURRENT_CONTEXT(ctx);
   r200ContextPtr rmesa = (r200ContextPtr) driContextPriv->driverPrivate;
   r200ContextPtr current = ctx ? R200_CONTEXT(ctx) : NULL;

   /* check if we're deleting the currently bound context */
   if (rmesa == current) {
      R200_FIREVERTICES( rmesa );
      _mesa_make_current2(NULL, NULL, NULL);
   }

   /* Free r200 context resources */
   assert(rmesa); /* should never be null */
   if ( rmesa ) {
      GLboolean   release_texture_heaps;


      release_texture_heaps = (rmesa->glCtx->Shared->RefCount == 1);
      _swsetup_DestroyContext( rmesa->glCtx );
      _tnl_DestroyContext( rmesa->glCtx );
      _ac_DestroyContext( rmesa->glCtx );
      _swrast_DestroyContext( rmesa->glCtx );

      r200DestroySwtcl( rmesa->glCtx );
      r200ReleaseArrays( rmesa->glCtx, ~0 );

      if (rmesa->dma.current.buf) {
	 r200ReleaseDmaRegion( rmesa, &rmesa->dma.current, __FUNCTION__ );
	 r200FlushCmdBuf( rmesa, __FUNCTION__ );
      }

      if (!rmesa->TclFallback & R200_TCL_FALLBACK_TCL_DISABLE)
	 if (!getenv("R200_NO_VTXFMT"))
	    r200VtxfmtDestroy( rmesa->glCtx );

      /* free the Mesa context */
      rmesa->glCtx->DriverCtx = NULL;
      _mesa_destroy_context( rmesa->glCtx );

      if (rmesa->state.scissor.pClipRects) {
	 FREE(rmesa->state.scissor.pClipRects);
	 rmesa->state.scissor.pClipRects = 0;
      }

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
}




void
r200SwapBuffers( __DRIdrawablePrivate *dPriv )
{
   if (dPriv->driContextPriv && dPriv->driContextPriv->driverPrivate) {
      r200ContextPtr rmesa;
      GLcontext *ctx;
      rmesa = (r200ContextPtr) dPriv->driContextPriv->driverPrivate;
      ctx = rmesa->glCtx;
      if (ctx->Visual.doubleBufferMode) {
         _mesa_notifySwapBuffers( ctx );  /* flush pending rendering comands */
         if ( rmesa->doPageFlip ) {
            r200PageFlip( dPriv );
         }
         else {
            r200CopyBuffer( dPriv );
         }
      }
   }
   else {
      /* XXX this shouldn't be an error but we can't handle it for now */
      _mesa_problem(NULL, "%s: drawable has no context!", __FUNCTION__);
   }
}


/* Force the context `c' to be the current context and associate with it
 * buffer `b'.
 */
GLboolean
r200MakeCurrent( __DRIcontextPrivate *driContextPriv,
                   __DRIdrawablePrivate *driDrawPriv,
                   __DRIdrawablePrivate *driReadPriv )
{
   if ( driContextPriv ) {
      r200ContextPtr newCtx = 
	 (r200ContextPtr) driContextPriv->driverPrivate;

      if (R200_DEBUG & DEBUG_DRI)
	 fprintf(stderr, "%s ctx %p\n", __FUNCTION__, (void *)newCtx->glCtx);

      if ( newCtx->dri.drawable != driDrawPriv ) {
	 newCtx->dri.drawable = driDrawPriv;
	 r200UpdateWindow( newCtx->glCtx );
	 r200UpdateViewportOffset( newCtx->glCtx );
      }

      _mesa_make_current2( newCtx->glCtx,
			   (GLframebuffer *) driDrawPriv->driverPrivate,
			   (GLframebuffer *) driReadPriv->driverPrivate );

      if ( !newCtx->glCtx->Viewport.Width ) {
	 _mesa_set_viewport( newCtx->glCtx, 0, 0,
			     driDrawPriv->w, driDrawPriv->h );
      }

      if (newCtx->vb.enabled)
	 r200VtxfmtMakeCurrent( newCtx->glCtx );

      _mesa_update_state( newCtx->glCtx );
      r200ValidateState( newCtx->glCtx );

   } else {
      if (R200_DEBUG & DEBUG_DRI)
	 fprintf(stderr, "%s ctx is null\n", __FUNCTION__);
      _mesa_make_current( 0, 0 );
   }

   if (R200_DEBUG & DEBUG_DRI)
      fprintf(stderr, "End %s\n", __FUNCTION__);
   return GL_TRUE;
}

/* Force the context `c' to be unbound from its buffer.
 */
GLboolean
r200UnbindContext( __DRIcontextPrivate *driContextPriv )
{
   r200ContextPtr rmesa = (r200ContextPtr) driContextPriv->driverPrivate;

   if (R200_DEBUG & DEBUG_DRI)
      fprintf(stderr, "%s ctx %p\n", __FUNCTION__, (void *)rmesa->glCtx);

   r200VtxfmtUnbindContext( rmesa->glCtx );
   return GL_TRUE;
}
