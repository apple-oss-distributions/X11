/* $XFree86: xc/lib/GL/mesa/src/drv/mga/mga_xmesa.c,v 1.22 2003/12/08 22:45:30 alanh Exp $ */
/*
 * Copyright 2000-2001 VA Linux Systems, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.  IN NO EVENT SHALL
 * VA LINUX SYSTEMS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Keith Whitwell <keith@tungstengraphics.com>
 */

#ifdef GLX_DIRECT_RENDERING

#include "mga_common.h"
#include "mga_xmesa.h"
#include "context.h"
#include "matrix.h"
#include "simple_list.h"
#include "imports.h"

#include "swrast/swrast.h"
#include "swrast_setup/swrast_setup.h"
#include "tnl/tnl.h"
#include "array_cache/acache.h"

#include "tnl/t_pipeline.h"

#include "mgadd.h"
#include "mgastate.h"
#include "mgatex.h"
#include "mgaspan.h"
#include "mgaioctl.h"
#include "mgatris.h"
#include "mgavb.h"
#include "mgapixel.h"
#include "mga_xmesa.h"
#include "mga_dri.h"


#include "utils.h"
#include "vblank.h"
#include "glxextensions.h"

#ifndef MGA_DEBUG
int MGA_DEBUG = 0;
#endif

static int getSwapInfo( __DRIdrawablePrivate *dPriv, __DRIswapInfo * sInfo );

static GLboolean
mgaInitDriver(__DRIscreenPrivate *sPriv)
{
   mgaScreenPrivate *mgaScreen;
   MGADRIPtr         serverInfo = (MGADRIPtr)sPriv->pDevPriv;

   if ( ! driCheckDriDdxDrmVersions( sPriv, "MGA", 4, 0, 1, 0, 3, 0 ) )
      return GL_FALSE;

   /* Allocate the private area */
   mgaScreen = (mgaScreenPrivate *)MALLOC(sizeof(mgaScreenPrivate));
   if (!mgaScreen) {
      __driUtilMessage("Couldn't malloc screen struct");
      return GL_FALSE;
   }

   mgaScreen->sPriv = sPriv;
   sPriv->private = (void *)mgaScreen;

   if (sPriv->drmMinor >= 1) {
      int ret;
      drmMGAGetParam gp;

      gp.param = MGA_PARAM_IRQ_NR;
      gp.value = &mgaScreen->irq;

      ret = drmCommandWriteRead( sPriv->fd, DRM_MGA_GETPARAM,
				    &gp, sizeof(gp));
      if (ret) {
	    fprintf(stderr, "drmMgaGetParam (MGA_PARAM_IRQ_NR): %d\n", ret);
	    XFree(mgaScreen);
	    sPriv->private = NULL;
	    return GL_FALSE;
      }
   }
   
   mgaScreen->linecomp_sane = (sPriv->ddxMajor > 1) || (sPriv->ddxMinor > 1)
       || ((sPriv->ddxMinor == 1) && (sPriv->ddxPatch > 0));
   if ( driCompareGLXAPIVersion( 20030813 ) >= 0 ) {
      PFNGLXSCRENABLEEXTENSIONPROC glx_enable_extension =
          (PFNGLXSCRENABLEEXTENSIONPROC) glXGetProcAddress( (const GLubyte *) "__glXScrEnableExtension" );
      void * const psc = sPriv->psc->screenConfigs;

      if ( glx_enable_extension != NULL ) {
	 if ( mgaScreen->linecomp_sane ) {
	    (*glx_enable_extension)( psc, "GLX_SGI_swap_control" );
	    (*glx_enable_extension)( psc, "GLX_SGI_video_sync" );
	    (*glx_enable_extension)( psc, "GLX_MESA_swap_control" );
	 }

	 (*glx_enable_extension)( psc, "GLX_MESA_swap_frame_usage" );
      }
   }

   if (serverInfo->chipset != MGA_CARD_TYPE_G200 &&
       serverInfo->chipset != MGA_CARD_TYPE_G400) {
      XFree(mgaScreen);
      sPriv->private = NULL;
      __driUtilMessage("Unrecognized chipset");
      return GL_FALSE;
   }


   mgaScreen->chipset = serverInfo->chipset;
   mgaScreen->width = serverInfo->width;
   mgaScreen->height = serverInfo->height;
   mgaScreen->mem = serverInfo->mem;
   mgaScreen->cpp = serverInfo->cpp;

   mgaScreen->agpMode = serverInfo->agpMode;

   mgaScreen->frontPitch = serverInfo->frontPitch;
   mgaScreen->frontOffset = serverInfo->frontOffset;
   mgaScreen->backOffset = serverInfo->backOffset;
   mgaScreen->backPitch  =  serverInfo->backPitch;
   mgaScreen->depthOffset = serverInfo->depthOffset;
   mgaScreen->depthPitch  =  serverInfo->depthPitch;

   mgaScreen->mmio.handle = serverInfo->registers.handle;
   mgaScreen->mmio.size = serverInfo->registers.size;
   if ( drmMap( sPriv->fd,
		mgaScreen->mmio.handle, mgaScreen->mmio.size,
		&mgaScreen->mmio.map ) < 0 ) {
      FREE( mgaScreen );
      sPriv->private = NULL;
      __driUtilMessage( "Couldn't map MMIO registers" );
      return GL_FALSE;
   }

   mgaScreen->primary.handle = serverInfo->primary.handle;
   mgaScreen->primary.size = serverInfo->primary.size;
   mgaScreen->buffers.handle = serverInfo->buffers.handle;
   mgaScreen->buffers.size = serverInfo->buffers.size;

#if 0
   mgaScreen->agp.handle = serverInfo->agp;
   mgaScreen->agp.size = serverInfo->agpSize;

   if (drmMap(sPriv->fd,
	      mgaScreen->agp.handle,
	      mgaScreen->agp.size,
	      (drmAddress *)&mgaScreen->agp.map) != 0)
   {
      Xfree(mgaScreen);
      sPriv->private = NULL;
      __driUtilMessage("Couldn't map agp region");
      return GL_FALSE;
   }
#endif

   mgaScreen->textureOffset[MGA_CARD_HEAP] = serverInfo->textureOffset;
   mgaScreen->textureOffset[MGA_AGP_HEAP] = (serverInfo->agpTextureOffset |
					     PDEA_pagpxfer_enable | 1);

   mgaScreen->textureSize[MGA_CARD_HEAP] = serverInfo->textureSize;
   mgaScreen->textureSize[MGA_AGP_HEAP] = serverInfo->agpTextureSize;

   mgaScreen->logTextureGranularity[MGA_CARD_HEAP] =
      serverInfo->logTextureGranularity;
   mgaScreen->logTextureGranularity[MGA_AGP_HEAP] =
      serverInfo->logAgpTextureGranularity;

   mgaScreen->texVirtual[MGA_CARD_HEAP] = (char *)(mgaScreen->sPriv->pFB +
					   serverInfo->textureOffset);
   if (drmMap(sPriv->fd,
              serverInfo->agpTextureOffset,
              serverInfo->agpTextureSize,
              (drmAddress *)&mgaScreen->texVirtual[MGA_AGP_HEAP]) != 0)
   {
      Xfree(mgaScreen);
      sPriv->private = NULL;
      __driUtilMessage("Couldn't map agptexture region");
      return GL_FALSE;
   }

#if 0
   mgaScreen->texVirtual[MGA_AGP_HEAP] = (mgaScreen->agp.map +
					  serverInfo->agpTextureOffset);
#endif

   mgaScreen->mAccess = serverInfo->mAccess;

   /* For calculating setupdma addresses.
    */
   mgaScreen->dmaOffset = serverInfo->buffers.handle;

   mgaScreen->bufs = drmMapBufs(sPriv->fd);
   if (!mgaScreen->bufs) {
      /*drmUnmap(mgaScreen->agp_tex.map, mgaScreen->agp_tex.size);*/
      XFree(mgaScreen);
      sPriv->private = NULL;
      __driUtilMessage("Couldn't map dma buffers");
      return GL_FALSE;
   }
   mgaScreen->sarea_priv_offset = serverInfo->sarea_priv_offset;

   return GL_TRUE;
}


static void
mgaDestroyScreen(__DRIscreenPrivate *sPriv)
{
   mgaScreenPrivate *mgaScreen = (mgaScreenPrivate *) sPriv->private;

   if (MGA_DEBUG&DEBUG_VERBOSE_DRI)
      fprintf(stderr, "mgaDestroyScreen\n");

   /*drmUnmap(mgaScreen->agp_tex.map, mgaScreen->agp_tex.size);*/
   Xfree(mgaScreen);
   sPriv->private = NULL;
}


extern const struct gl_pipeline_stage _mga_render_stage;

static const struct gl_pipeline_stage *mga_pipeline[] = {
   &_tnl_vertex_transform_stage, 
   &_tnl_normal_transform_stage, 
   &_tnl_lighting_stage,	
   &_tnl_fog_coordinate_stage,
   &_tnl_texgen_stage, 
   &_tnl_texture_transform_stage, 
				/* REMOVE: point attenuation stage */
#if 0
   &_mga_render_stage,		/* ADD: unclipped rastersetup-to-dma */
                                /* Need new ioctl for wacceptseq */
#endif
   &_tnl_render_stage,		
   0,
};


static const char * const g400_extensions[] =
{
   "GL_ARB_multitexture",
   "GL_ARB_texture_env_add",
   "GL_EXT_texture_env_add",
   "GL_ARB_texture_env_combine",
   "GL_EXT_texture_env_combine",
   "GL_ARB_texture_env_crossbar",
   "GL_ATI_texture_env_combine3",
   "GL_EXT_texture_edge_clamp",
   "GL_SGIS_texture_edge_clamp",
#if defined (MESA_packed_depth_stencil)
   "GL_MESA_packed_depth_stencil",
#endif
   NULL
};

static const char * const card_extensions[] =
{
   "GL_ARB_multisample",
   "GL_ARB_texture_compression",
   "GL_EXT_blend_logic_op",
   "GL_EXT_fog_coord",
   /* paletted_textures currently doesn't work, but we could fix them later */
#if 0
   "GL_EXT_shared_texture_palette",
   "GL_EXT_paletted_texture",
#endif
   "GL_EXT_secondary_color",
   "GL_EXT_stencil_wrap",
   "GL_MESA_ycbcr_texture",
   "GL_SGIS_generate_mipmap",
   "GL_SGIS_texture_lod",
   "GL_NV_texture_rectangle",
   NULL
};

static const struct dri_debug_control debug_control[] =
{
    { "fall",  DEBUG_VERBOSE_FALLBACK },
    { "tex",   DEBUG_VERBOSE_TEXTURE },
    { "ioctl", DEBUG_VERBOSE_IOCTL },
    { "verb",  DEBUG_VERBOSE_MSG },
    { "dri",   DEBUG_VERBOSE_DRI },
    { NULL,    0 }
};


static int
get_ust_nop( int64_t * ust )
{
   *ust = 1;
   return 0;
}


static GLboolean
mgaCreateContext( const __GLcontextModes *mesaVis,
                  __DRIcontextPrivate *driContextPriv,
                  void *sharedContextPrivate )
{
   int i;
   unsigned   maxlevels;
   GLcontext *ctx, *shareCtx;
   mgaContextPtr mmesa;
   __DRIscreenPrivate *sPriv = driContextPriv->driScreenPriv;
   mgaScreenPrivate *mgaScreen = (mgaScreenPrivate *)sPriv->private;
   MGASAREAPrivPtr saPriv=(MGASAREAPrivPtr)(((char*)sPriv->pSAREA)+
					      mgaScreen->sarea_priv_offset);

   if (MGA_DEBUG&DEBUG_VERBOSE_DRI)
      fprintf(stderr, "mgaCreateContext\n");

   /* allocate mga context */
   mmesa = (mgaContextPtr) CALLOC(sizeof(mgaContext));
   if (!mmesa) {
      return GL_FALSE;
   }

   /* Allocate the Mesa context */
   if (sharedContextPrivate)
      shareCtx = ((mgaContextPtr) sharedContextPrivate)->glCtx;
   else 
      shareCtx = NULL;
   mmesa->glCtx = _mesa_create_context(mesaVis, shareCtx, (void *) mmesa, GL_TRUE);
   if (!mmesa->glCtx) {
      FREE(mmesa);
      return GL_FALSE;
   }
   driContextPriv->driverPrivate = mmesa;

   /* Init mga state */
   mmesa->hHWContext = driContextPriv->hHWContext;
   mmesa->driFd = sPriv->fd;
   mmesa->driHwLock = &sPriv->pSAREA->lock;

   mmesa->mgaScreen = mgaScreen;
   mmesa->driScreen = sPriv;
   mmesa->sarea = (void *)saPriv;
   mmesa->glBuffer = NULL;

   (void) memset( mmesa->texture_heaps, 0, sizeof( mmesa->texture_heaps ) );
   make_empty_list( & mmesa->swapped );

   mmesa->nr_heaps = mgaScreen->texVirtual[MGA_AGP_HEAP] ? 2 : 1;
   for ( i = 0 ; i < mmesa->nr_heaps ; i++ ) {
      mmesa->texture_heaps[i] = driCreateTextureHeap( i, mmesa,
	    mgaScreen->textureSize[i],
	    6,
	    MGA_NR_TEX_REGIONS,
	    mmesa->sarea->texList[i],
	    & mmesa->sarea->texAge[i],
	    & mmesa->swapped,
	    sizeof( mgaTextureObject_t ),
	    (destroy_texture_object_t *) mgaDestroyTexObj );
   }

   /* Set the maximum texture size small enough that we can guarentee
    * that both texture units can bind a maximal texture and have them
    * on the card at once.
    */
   ctx = mmesa->glCtx;
   if ( mgaScreen->chipset == MGA_CARD_TYPE_G200 ) {
      ctx->Const.MaxTextureUnits = 1;
      maxlevels = G200_TEX_MAXLEVELS;

   }
   else {
      ctx->Const.MaxTextureUnits = 2;
      maxlevels = G400_TEX_MAXLEVELS;
   }

   driCalculateMaxTextureLevels( mmesa->texture_heaps,
				 mmesa->nr_heaps,
				 & ctx->Const,
				 4,
				 11, /* max 2D texture size is 2048x2048 */
				 0,  /* 3D textures unsupported. */
				 0,  /* cube textures unsupported. */
				 11, /* max texture rect size is 2048x2048 */
				 maxlevels,
				 GL_FALSE );

   ctx->Const.MinLineWidth = 1.0;
   ctx->Const.MinLineWidthAA = 1.0;
   ctx->Const.MaxLineWidth = 10.0;
   ctx->Const.MaxLineWidthAA = 10.0;
   ctx->Const.LineWidthGranularity = 1.0;

   mmesa->default32BitTextures = (mesaVis->rgbBits >= 24);
   mmesa->hw_stencil = mesaVis->stencilBits && mesaVis->depthBits == 24;

   switch (mesaVis->depthBits) {
   case 16: 
      mmesa->depth_scale = 1.0/(GLdouble)0xffff; 
      mmesa->depth_clear_mask = ~0;
      mmesa->ClearDepth = 0xffff;
      break;
   case 24:
      mmesa->depth_scale = 1.0/(GLdouble)0xffffff;
      if (mmesa->hw_stencil) {
	 mmesa->depth_clear_mask = 0xffffff00;
	 mmesa->stencil_clear_mask = 0x000000ff;
      } else
	 mmesa->depth_clear_mask = ~0;
      mmesa->ClearDepth = 0xffffff00;
      break;
   case 32:
      mmesa->depth_scale = 1.0/(GLdouble)0xffffffff;
      mmesa->depth_clear_mask = ~0;
      mmesa->ClearDepth = 0xffffffff;
      break;
   };

   mmesa->haveHwStipple = GL_FALSE;
   mmesa->RenderIndex = -1;		/* impossible value */
   mmesa->dirty = ~0;
   mmesa->vertex_format = 0;   
   mmesa->CurrentTexObj[0] = 0;
   mmesa->CurrentTexObj[1] = 0;
   mmesa->tmu_source[0] = 0;
   mmesa->tmu_source[1] = 1;

   mmesa->texAge[0] = 0;
   mmesa->texAge[1] = 0;
   
   /* Initialize the software rasterizer and helper modules.
    */
   _swrast_CreateContext( ctx );
   _ac_CreateContext( ctx );
   _tnl_CreateContext( ctx );
   
   _swsetup_CreateContext( ctx );

   /* Install the customized pipeline:
    */
   _tnl_destroy_pipeline( ctx );
   _tnl_install_pipeline( ctx, mga_pipeline );

   /* Configure swrast to match hardware characteristics:
    */
   _swrast_allow_pixel_fog( ctx, GL_FALSE );
   _swrast_allow_vertex_fog( ctx, GL_TRUE );

   mmesa->primary_offset = mmesa->mgaScreen->primary.handle;

   ctx->DriverCtx = (void *) mmesa;
   mmesa->glCtx = ctx;

   driInitExtensions( ctx, card_extensions, GL_FALSE );

   if (MGA_IS_G400(MGA_CONTEXT(ctx))) {
      driInitExtensions( ctx, g400_extensions, GL_FALSE );
   }

   mgaDDInitStateFuncs( ctx );
   mgaDDInitTextureFuncs( ctx );
   mgaDDInitSpanFuncs( ctx );
   mgaDDInitDriverFuncs( ctx );
   mgaDDInitIoctlFuncs( ctx );
   mgaDDInitPixelFuncs( ctx );
   mgaDDInitTriFuncs( ctx );

   mgaInitVB( ctx );
   mgaInitState( mmesa );

   driContextPriv->driverPrivate = (void *) mmesa;

#if DO_DEBUG
   MGA_DEBUG = driParseDebugString( getenv( "MGA_DEBUG" ),
				    debug_control );
#endif

   mmesa->vblank_flags = ((mmesa->mgaScreen->irq == 0) 
			  || !mmesa->mgaScreen->linecomp_sane)
       ? VBLANK_FLAG_NO_IRQ : driGetDefaultVBlankFlags();

   mmesa->get_ust = (PFNGLXGETUSTPROC) glXGetProcAddress( (const GLubyte *) "__glXGetUST" );
   if ( mmesa->get_ust == NULL ) {
      mmesa->get_ust = get_ust_nop;
   }

   (*mmesa->get_ust)( & mmesa->swap_ust );

   return GL_TRUE;
}

static void
mgaDestroyContext(__DRIcontextPrivate *driContextPriv)
{
   mgaContextPtr mmesa = (mgaContextPtr) driContextPriv->driverPrivate;

   if (MGA_DEBUG&DEBUG_VERBOSE_DRI)
      fprintf( stderr, "[%s:%d] mgaDestroyContext start\n",
	       __FILE__, __LINE__ );

   assert(mmesa); /* should never be null */
   if (mmesa) {
      GLboolean   release_texture_heaps;


      release_texture_heaps = (mmesa->glCtx->Shared->RefCount == 1);
      _swsetup_DestroyContext( mmesa->glCtx );
      _tnl_DestroyContext( mmesa->glCtx );
      _ac_DestroyContext( mmesa->glCtx );
      _swrast_DestroyContext( mmesa->glCtx );

      mgaFreeVB( mmesa->glCtx );

      /* free the Mesa context */
      mmesa->glCtx->DriverCtx = NULL;
      _mesa_destroy_context(mmesa->glCtx);
       
      if ( release_texture_heaps ) {
         /* This share group is about to go away, free our private
          * texture object data.
          */
         int i;

         for ( i = 0 ; i < mmesa->nr_heaps ; i++ ) {
	    driDestroyTextureHeap( mmesa->texture_heaps[ i ] );
	    mmesa->texture_heaps[ i ] = NULL;
         }

	 assert( is_empty_list( & mmesa->swapped ) );
      }

      FREE(mmesa);
   }

   if (MGA_DEBUG&DEBUG_VERBOSE_DRI)
      fprintf( stderr, "[%s:%d] mgaDestroyContext done\n",
	       __FILE__, __LINE__ );
}


static GLboolean
mgaCreateBuffer( __DRIscreenPrivate *driScrnPriv,
                 __DRIdrawablePrivate *driDrawPriv,
                 const __GLcontextModes *mesaVis,
                 GLboolean isPixmap )
{
   if (isPixmap) {
      return GL_FALSE; /* not implemented */
   }
   else {
      GLboolean swStencil = (mesaVis->stencilBits > 0 && 
			     mesaVis->depthBits != 24);

      driDrawPriv->driverPrivate = (void *) 
         _mesa_create_framebuffer(mesaVis,
                                  GL_FALSE,  /* software depth buffer? */
                                  swStencil,
                                  mesaVis->accumRedBits > 0,
                                  mesaVis->alphaBits > 0 );

      return (driDrawPriv->driverPrivate != NULL);
   }
}


static void
mgaDestroyBuffer(__DRIdrawablePrivate *driDrawPriv)
{
   _mesa_destroy_framebuffer((GLframebuffer *) (driDrawPriv->driverPrivate));
}

static void
mgaSwapBuffers(__DRIdrawablePrivate *dPriv)
{
   if (dPriv->driContextPriv && dPriv->driContextPriv->driverPrivate) {
      mgaContextPtr mmesa;
      GLcontext *ctx;
      mmesa = (mgaContextPtr) dPriv->driContextPriv->driverPrivate;
      ctx = mmesa->glCtx;

      if (ctx->Visual.doubleBufferMode) {
         _mesa_notifySwapBuffers( ctx );
         mgaCopyBuffer( dPriv );
      }
   } else {
      /* XXX this shouldn't be an error but we can't handle it for now */
      _mesa_problem(NULL, "%s: drawable has no context!\n", __FUNCTION__);
   }
}

static GLboolean
mgaUnbindContext(__DRIcontextPrivate *driContextPriv)
{
   mgaContextPtr mmesa = (mgaContextPtr) driContextPriv->driverPrivate;
   if (mmesa)
      mmesa->dirty = ~0;

   return GL_TRUE;
}

static GLboolean
mgaOpenCloseFullScreen(__DRIcontextPrivate *driContextPriv)
{
    return GL_TRUE;
}


/* This looks buggy to me - the 'b' variable isn't used anywhere...
 * Hmm - It seems that the drawable is already hooked in to
 * driDrawablePriv.
 *
 * But why are we doing context initialization here???
 */
static GLboolean
mgaMakeCurrent(__DRIcontextPrivate *driContextPriv,
               __DRIdrawablePrivate *driDrawPriv,
               __DRIdrawablePrivate *driReadPriv)
{
   if (driContextPriv) {
      mgaContextPtr mmesa = (mgaContextPtr) driContextPriv->driverPrivate;

      if (mmesa->driDrawable != driDrawPriv) {
	 mmesa->driDrawable = driDrawPriv;
	 mmesa->dirty = ~0; 
	 mmesa->dirty_cliprects = (MGA_FRONT|MGA_BACK); 
      }

      _mesa_make_current2(mmesa->glCtx,
                          (GLframebuffer *) driDrawPriv->driverPrivate,
                          (GLframebuffer *) driReadPriv->driverPrivate);

      if (!mmesa->glCtx->Viewport.Width)
	 _mesa_set_viewport(mmesa->glCtx, 0, 0,
                            driDrawPriv->w, driDrawPriv->h);

   }
   else {
      _mesa_make_current(NULL, NULL);
   }

   return GL_TRUE;
}


void mgaGetLock( mgaContextPtr mmesa, GLuint flags )
{
   __DRIdrawablePrivate *dPriv = mmesa->driDrawable;
   MGASAREAPrivPtr sarea = mmesa->sarea;
   int me = mmesa->hHWContext;
   int i;

   drmGetLock(mmesa->driFd, mmesa->hHWContext, flags);

   if (*(dPriv->pStamp) != mmesa->lastStamp) {
      mmesa->lastStamp = *(dPriv->pStamp);
      mmesa->SetupNewInputs |= VERT_BIT_CLIP;
      mmesa->dirty_cliprects = (MGA_FRONT|MGA_BACK);
      mgaUpdateRects( mmesa, (MGA_FRONT|MGA_BACK) );
   }

   mmesa->dirty |= MGA_UPLOAD_CONTEXT | MGA_UPLOAD_CLIPRECTS;

    mmesa->sarea->dirty |= MGA_UPLOAD_CONTEXT;

   if (sarea->ctxOwner != me) {
      mmesa->dirty |= (MGA_UPLOAD_CONTEXT | MGA_UPLOAD_TEX0 |
		       MGA_UPLOAD_TEX1 | MGA_UPLOAD_PIPE);
      sarea->ctxOwner=me;
   }

   for ( i = 0 ; i < mmesa->nr_heaps ; i++ ) {
      DRI_AGE_TEXTURES( mmesa->texture_heaps[ i ] );
   }

   sarea->last_quiescent = -1;	/* just kill it for now */
}


static const struct __DriverAPIRec mgaAPI = {
   .InitDriver      = mgaInitDriver,
   .DestroyScreen   = mgaDestroyScreen,
   .CreateContext   = mgaCreateContext,
   .DestroyContext  = mgaDestroyContext,
   .CreateBuffer    = mgaCreateBuffer,
   .DestroyBuffer   = mgaDestroyBuffer,
   .SwapBuffers     = mgaSwapBuffers,
   .MakeCurrent     = mgaMakeCurrent,
   .UnbindContext   = mgaUnbindContext,
   .OpenFullScreen  = mgaOpenCloseFullScreen,
   .CloseFullScreen = mgaOpenCloseFullScreen,
   .GetSwapInfo     = getSwapInfo,
   .GetMSC          = driGetMSC32,
   .WaitForMSC      = driWaitForMSC32,
   .WaitForSBC      = NULL,
   .SwapBuffersMSC  = NULL
};


/*
 * This is the bootstrap function for the driver.
 * The __driCreateScreen name is the symbol that libGL.so fetches.
 * Return:  pointer to a __DRIscreenPrivate.
 */
void *__driCreateScreen(Display *dpy, int scrn, __DRIscreen *psc,
                        int numConfigs, __GLXvisualConfig *config)
{
   __DRIscreenPrivate *psp;
   psp = __driUtilCreateScreen(dpy, scrn, psc, numConfigs, config, &mgaAPI);
   return (void *) psp;
}


/**
 * Get information about previous buffer swaps.
 */
static int
getSwapInfo( __DRIdrawablePrivate *dPriv, __DRIswapInfo * sInfo )
{
   mgaContextPtr  mmesa;

   if ( (dPriv == NULL) || (dPriv->driContextPriv == NULL)
	|| (dPriv->driContextPriv->driverPrivate == NULL)
	|| (sInfo == NULL) ) {
      return -1;
   }

   mmesa = (mgaContextPtr) dPriv->driContextPriv->driverPrivate;
   sInfo->swap_count = mmesa->swap_count;
   sInfo->swap_ust = mmesa->swap_ust;
   sInfo->swap_missed_count = mmesa->swap_missed_count;

   sInfo->swap_missed_usage = (sInfo->swap_missed_count != 0)
       ? driCalculateSwapUsage( dPriv, 0, mmesa->swap_missed_ust )
       : 0.0;

   return 0;
}
#endif
