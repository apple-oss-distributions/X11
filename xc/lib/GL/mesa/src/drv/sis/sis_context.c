/**************************************************************************

Copyright 2000 Silicon Integrated Systems Corp, Inc., HsinChu, Taiwan.
Copyright 2003 Eric Anholt
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
ERIC ANHOLT OR SILICON INTEGRATED SYSTEMS CORP BE LIABLE FOR ANY CLAIM,
DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
USE OR OTHER DEALINGS IN THE SOFTWARE.

**************************************************************************/
/* $XFree86: xc/lib/GL/mesa/src/drv/sis/sis_context.c,v 1.3 2003/12/09 00:15:21 alanh Exp $ */

/*
 * Authors:
 *   Sung-Ching Lin <sclin@sis.com.tw>
 *   Eric Anholt <anholt@FreeBSD.org>
 */

#include "sis_dri.h"

#include "sis_context.h"
#include "sis_state.h"
#include "sis_dd.h"
#include "sis_span.h"
#include "sis_stencil.h"
#include "sis_tex.h"
#include "sis_tris.h"
#include "sis_vb.h"

#include "imports.h"
#include "matrix.h"
#include "extensions.h"
#include "utils.h"

#include "swrast/swrast.h"
#include "swrast_setup/swrast_setup.h"
#include "array_cache/acache.h"

#include "tnl/tnl.h"
#include "tnl/t_pipeline.h"

int GlobalCurrentHwcx = -1;
int GlobalHwcxCountBase = 1;
int GlobalCmdQueueLen = 0;

static const char * const card_extensions[] =
{
   "GL_ARB_multitexture",
   "GL_EXT_texture_lod_bias",
   NULL
};

void
WaitEngIdle (sisContextPtr smesa)
{
   GLuint engineState;

   do {
      engineState = MMIO_READ(REG_CommandQueue);
   } while ((engineState & SiS_EngIdle) != SiS_EngIdle);
}

void
Wait2DEngIdle (sisContextPtr smesa)
{
   GLuint engineState;

   do {
      engineState = MMIO_READ(REG_CommandQueue);
   } while ((engineState & SiS_EngIdle2d) != SiS_EngIdle2d);
}

/* To be called from mWait3DCmdQueue.  Separate function for profiling
 * purposes, and speed doesn't matter because we're spinning anyway.
 */
void
WaitingFor3dIdle(sisContextPtr smesa, int wLen)
{
   while (*(smesa->CurrentQueueLenPtr) < wLen) {
      *(smesa->CurrentQueueLenPtr) =
         (MMIO_READ(REG_CommandQueue) & MASK_QueueLen) - 20;
   }
}

GLboolean
sisCreateContext( const __GLcontextModes *glVisual,
		  __DRIcontextPrivate *driContextPriv,
                  void *sharedContextPrivate )
{
   GLcontext *ctx, *shareCtx;
   __DRIscreenPrivate *sPriv = driContextPriv->driScreenPriv;
   sisContextPtr smesa;
   sisScreenPtr sisScreen;
   int i;

   smesa = (sisContextPtr)CALLOC( sizeof(*smesa) );
   if ( smesa == NULL )
      return GL_FALSE;

   /* Allocate the Mesa context */
   if (sharedContextPrivate)
      shareCtx = ((sisContextPtr)sharedContextPrivate)->glCtx;
   else 
      shareCtx = NULL;
   smesa->glCtx = _mesa_create_context( glVisual, shareCtx, (void *) smesa,
      GL_TRUE);
   if (smesa->glCtx == NULL) {
      FREE(smesa);
      return GL_FALSE;
   }
   driContextPriv->driverPrivate = smesa;
   ctx = smesa->glCtx;

   sisScreen = smesa->sisScreen = (sisScreenPtr)(sPriv->private);

   smesa->driContext = driContextPriv;
   smesa->driScreen = sPriv;
   smesa->driDrawable = NULL;
   smesa->hHWContext = driContextPriv->hHWContext;
   smesa->driHwLock = &sPriv->pSAREA->lock;
   smesa->driFd = sPriv->fd;
  
   smesa->virtualX = sisScreen->screenX;
   smesa->virtualY = sisScreen->screenY;
   smesa->bytesPerPixel = sisScreen->cpp;
   smesa->IOBase = sisScreen->mmio.map;
   smesa->Chipset = sisScreen->deviceID;
   smesa->irqEnabled = sisScreen->irqEnabled;

   smesa->FbBase = sPriv->pFB;
   smesa->displayWidth = sPriv->fbWidth;
   smesa->frontPitch = sPriv->fbStride;

   smesa->sarea = (SISSAREAPriv *)((char *)sPriv->pSAREA +
				   sisScreen->sarea_priv_offset);

#if defined(SIS_DUMP)
   IOBase4Debug = GET_IOBase (smesa);
#endif

   /* support ARGB8888 and RGB565 */
   switch (smesa->bytesPerPixel)
   {
   case 4:
      smesa->redMask = 0x00ff0000;
      smesa->greenMask = 0x0000ff00;
      smesa->blueMask = 0x000000ff;
      smesa->alphaMask = 0xff000000;
      smesa->colorFormat = DST_FORMAT_ARGB_8888;
      break;
   case 2:
      smesa->redMask = 0xf800;
      smesa->greenMask = 0x07e0;
      smesa->blueMask = 0x001f;
      smesa->alphaMask = 0;
      smesa->colorFormat = DST_FORMAT_RGB_565;
      break;
   default:
      assert (0);
   }

   /* TODO: index mode */

   smesa->CurrentQueueLenPtr = &(smesa->sarea->QueueLength);
   smesa->FrameCountPtr = &(smesa->sarea->FrameCount);

   /* set AGP */
   smesa->AGPSize = sisScreen->agp.size;
   smesa->AGPBase = sisScreen->agp.map;
   smesa->AGPAddr = sisScreen->agp.handle;

   /* set AGP command buffer */
   smesa->AGPCmdModeEnabled = GL_FALSE;
   if (smesa->AGPSize != 0 && getenv("SIS_NO_AGP") == NULL) {	
      if (sisScreen->AGPCmdBufSize != 0) {
         smesa->AGPCmdBufBase = smesa->AGPBase + sisScreen->AGPCmdBufOffset;
         smesa->AGPCmdBufAddr = smesa->AGPAddr + sisScreen->AGPCmdBufOffset;
         smesa->AGPCmdBufSize = sisScreen->AGPCmdBufSize;

         smesa->pAGPCmdBufNext = (GLint *)&(smesa->sarea->AGPCmdBufNext);
         smesa->AGPCmdModeEnabled = GL_TRUE;
      }
   }

   smesa->GlobalFlag = 0L;

   smesa->Fallback = 0;

   /* Initialize the software rasterizer and helper modules.
    */
   _swrast_CreateContext( ctx );
   _ac_CreateContext( ctx );
   _tnl_CreateContext( ctx );
   _swsetup_CreateContext( ctx );

   _swrast_allow_pixel_fog( ctx, GL_TRUE );
   _swrast_allow_vertex_fog( ctx, GL_FALSE );

   sisDDInitStateFuncs( ctx );
   sisDDInitState( smesa );	/* Initializes smesa->zFormat, important */
   sisInitVB( ctx );
   sisInitTriFuncs( ctx );
   sisDDInitDriverFuncs( ctx );
   sisDDInitSpanFuncs( ctx );
   sisDDInitStencilFuncs( ctx );
   sisDDInitTextureFuncs( ctx );

   driInitExtensions( ctx, card_extensions, GL_FALSE );

   /* TODO */
   /* smesa->blockWrite = SGRAMbw = IsBlockWrite (); */
   smesa->blockWrite = GL_FALSE;

   for (i = 0; i < SIS_MAX_TEXTURES; i++) {
      smesa->TexStates[i] = 0;
      smesa->PrevTexFormat[i] = 0;
   }

   return GL_TRUE;
}

void
sisDestroyContext ( __DRIcontextPrivate *driContextPriv )
{
   sisContextPtr smesa = (sisContextPtr)driContextPriv->driverPrivate;

   assert( smesa != NULL );

   if ( smesa != NULL ) {
      _swsetup_DestroyContext( smesa->glCtx );
      _tnl_DestroyContext( smesa->glCtx );
      _ac_DestroyContext( smesa->glCtx );
      _swrast_DestroyContext( smesa->glCtx );

      /* free the Mesa context */
      /* XXX: Is the next line needed?  The DriverCtx (smesa) reference is
       * needed for sisDDDeleteTexture, since it needs to call the FB/AGP free
       * function.
       */
      /* smesa->glCtx->DriverCtx = NULL; */
      _mesa_destroy_context(smesa->glCtx);
   }

   FREE( smesa );
}

GLboolean
sisMakeCurrent( __DRIcontextPrivate *driContextPriv,
                __DRIdrawablePrivate *driDrawPriv,
                __DRIdrawablePrivate *driReadPriv )
{
   if ( driContextPriv ) {
      GET_CURRENT_CONTEXT(ctx);
      sisContextPtr oldSisCtx = ctx ? SIS_CONTEXT(ctx) : NULL;
      sisContextPtr newSisCtx = (sisContextPtr) driContextPriv->driverPrivate;

      if ( newSisCtx != oldSisCtx) {
         newSisCtx->GlobalFlag = GFLAG_ALL;
      }

      newSisCtx->driDrawable = driDrawPriv;

      _mesa_make_current2( newSisCtx->glCtx,
                         (GLframebuffer *) driDrawPriv->driverPrivate,
                         (GLframebuffer *) driReadPriv->driverPrivate );

      sisUpdateBufferSize( newSisCtx );
      sisUpdateClipping( newSisCtx->glCtx );

      if ( newSisCtx->glCtx->Viewport.Width == 0 ) {
         _mesa_set_viewport(newSisCtx->glCtx, 0, 0,
                            driDrawPriv->w, driDrawPriv->h);
      }
   } else {
      _mesa_make_current( 0, 0 );
   }

   return GL_TRUE;
}

GLboolean
sisUnbindContext( __DRIcontextPrivate *driContextPriv )
{
   return GL_TRUE;
}

void
sis_update_render_state( sisContextPtr smesa )
{
   __GLSiSHardware *prev = &smesa->prev;

   mWait3DCmdQueue (45);

   if (smesa->GlobalFlag & GFLAG_ENABLESETTING) {
      if (!smesa->clearTexCache) {
	 MMIO(REG_3D_TEnable, prev->hwCapEnable);
      } else {
	 MMIO(REG_3D_TEnable, prev->hwCapEnable | MASK_TextureCacheClear);
	 MMIO(REG_3D_TEnable, prev->hwCapEnable);
	 smesa->clearTexCache = GL_FALSE;
      }
   }

   if (smesa->GlobalFlag & GFLAG_ENABLESETTING2)
      MMIO(REG_3D_TEnable2, prev->hwCapEnable2);

   /* Z Setting */
   if (smesa->GlobalFlag & GFLAG_ZSETTING)
   {
      MMIO(REG_3D_ZSet, prev->hwZ);
      MMIO(REG_3D_ZStWriteMask, prev->hwZMask);
      MMIO(REG_3D_ZAddress, prev->hwOffsetZ);
   }

   /* Alpha Setting */
   if (smesa->GlobalFlag & GFLAG_ALPHASETTING)
      MMIO(REG_3D_AlphaSet, prev->hwAlpha);

   if (smesa->GlobalFlag & GFLAG_DESTSETTING) {
      MMIO(REG_3D_DstSet, prev->hwDstSet);
      MMIO(REG_3D_DstAlphaWriteMask, prev->hwDstMask);
      MMIO(REG_3D_DstAddress, prev->hwOffsetDest);
   }

   /* Line Setting */
#if 0
   if (smesa->GlobalFlag & GFLAG_LINESETTING) 
      MMIO(REG_3D_LinePattern, prev->hwLinePattern);
#endif

   /* Fog Setting */
   if (smesa->GlobalFlag & GFLAG_FOGSETTING)
   {
      MMIO(REG_3D_FogSet, prev->hwFog);
      MMIO(REG_3D_FogInverseDistance, prev->hwFogInverse);
      MMIO(REG_3D_FogFarDistance, prev->hwFogFar);
      MMIO(REG_3D_FogFactorDensity, prev->hwFogDensity);
   }

   /* Stencil Setting */
   if (smesa->GlobalFlag & GFLAG_STENCILSETTING) {
      MMIO(REG_3D_StencilSet, prev->hwStSetting);
      MMIO(REG_3D_StencilSet2, prev->hwStSetting2);
   }

   /* Miscellaneous Setting */
   if (smesa->GlobalFlag & GFLAG_DSTBLEND)
      MMIO(REG_3D_DstBlendMode, prev->hwDstSrcBlend);
   if (smesa->GlobalFlag & GFLAG_CLIPPING) {
      MMIO(REG_3D_ClipTopBottom, prev->clipTopBottom);
      MMIO(REG_3D_ClipLeftRight, prev->clipLeftRight);
   }

  smesa->GlobalFlag &= ~GFLAG_RENDER_STATES;
}

void
sis_update_texture_state (sisContextPtr smesa)
{
   __GLSiSHardware *prev = &smesa->prev;

   mWait3DCmdQueue (55);
   if (smesa->clearTexCache || (smesa->GlobalFlag & GFLAG_TEXTUREADDRESS)) {
      MMIO(REG_3D_TEnable, prev->hwCapEnable | MASK_TextureCacheClear);
      MMIO(REG_3D_TEnable, prev->hwCapEnable);
      smesa->clearTexCache = GL_FALSE;
   }

   /* Texture Setting */
   if (smesa->GlobalFlag & CFLAG_TEXTURERESET)
      MMIO(REG_3D_TextureSet, prev->texture[0].hwTextureSet);

   if (smesa->GlobalFlag & GFLAG_TEXTUREMIPMAP)
      MMIO(REG_3D_TextureMip, prev->texture[0].hwTextureMip);

  /*
  MMIO(REG_3D_TextureTransparencyColorHigh, prev->texture[0].hwTextureClrHigh);
  MMIO(REG_3D_TextureTransparencyColorLow, prev->texture[0].hwTextureClrLow);
  */

   if (smesa->GlobalFlag & GFLAG_TEXBORDERCOLOR)
      MMIO(REG_3D_TextureBorderColor, prev->texture[0].hwTextureBorderColor);

   if (smesa->GlobalFlag & GFLAG_TEXTUREADDRESS) {
      switch ((prev->texture[0].hwTextureSet & MASK_TextureLevel) >> 8)
      {
      case 11:
         MMIO(REG_3D_TextureAddress11, prev->texture[0].texOffset11);
      case 10:
         MMIO(REG_3D_TextureAddress10, prev->texture[0].texOffset10);
         MMIO(REG_3D_TexturePitch10, prev->texture[0].texPitch10);
      case 9:
         MMIO(REG_3D_TextureAddress9, prev->texture[0].texOffset9);
      case 8:
         MMIO(REG_3D_TextureAddress8, prev->texture[0].texOffset8);
         MMIO(REG_3D_TexturePitch8, prev->texture[0].texPitch89);
      case 7:
         MMIO(REG_3D_TextureAddress7, prev->texture[0].texOffset7);
      case 6:
         MMIO(REG_3D_TextureAddress6, prev->texture[0].texOffset6);
         MMIO(REG_3D_TexturePitch6, prev->texture[0].texPitch67);
      case 5:
         MMIO(REG_3D_TextureAddress5, prev->texture[0].texOffset5);
      case 4:
         MMIO(REG_3D_TextureAddress4, prev->texture[0].texOffset4);
         MMIO(REG_3D_TexturePitch4, prev->texture[0].texPitch45);
      case 3:
         MMIO(REG_3D_TextureAddress3, prev->texture[0].texOffset3);
      case 2:
         MMIO(REG_3D_TextureAddress2, prev->texture[0].texOffset2);
         MMIO(REG_3D_TexturePitch2, prev->texture[0].texPitch23);
      case 1:
         MMIO(REG_3D_TextureAddress1, prev->texture[0].texOffset1);
      case 0:
	  MMIO(REG_3D_TextureAddress0, prev->texture[0].texOffset0);
	  MMIO(REG_3D_TexturePitch0, prev->texture[0].texPitch01);
      }
   }
   if (smesa->GlobalFlag & CFLAG_TEXTURERESET_1)
      MMIO(REG_3D_Texture1Set, prev->texture[1].hwTextureSet);
   if (smesa->GlobalFlag & GFLAG_TEXTUREMIPMAP_1)
      MMIO(REG_3D_Texture1Mip, prev->texture[1].hwTextureMip);

   if (smesa->GlobalFlag & GFLAG_TEXBORDERCOLOR_1) {
      MMIO(REG_3D_Texture1BorderColor,
	    prev->texture[1].hwTextureBorderColor);
   }
   if (smesa->GlobalFlag & GFLAG_TEXTUREADDRESS_1) {
      switch ((prev->texture[1].hwTextureSet & MASK_TextureLevel) >> 8)
      {
      case 11:
         MMIO(REG_3D_Texture1Address11, prev->texture[1].texOffset11);
      case 10:
         MMIO(REG_3D_Texture1Address10, prev->texture[1].texOffset10);
         MMIO(REG_3D_Texture1Pitch10, prev->texture[1].texPitch10);
      case 9:
         MMIO(REG_3D_Texture1Address9, prev->texture[1].texOffset9);
      case 8:
         MMIO(REG_3D_Texture1Address8, prev->texture[1].texOffset8);
         MMIO(REG_3D_Texture1Pitch8, prev->texture[1].texPitch89);
      case 7:
         MMIO(REG_3D_Texture1Address7, prev->texture[1].texOffset7);
      case 6:
         MMIO(REG_3D_Texture1Address6, prev->texture[1].texOffset6);
         MMIO(REG_3D_Texture1Pitch6, prev->texture[1].texPitch67);
      case 5:
         MMIO(REG_3D_Texture1Address5, prev->texture[1].texOffset5);
      case 4:
         MMIO(REG_3D_Texture1Address4, prev->texture[1].texOffset4);
         MMIO(REG_3D_Texture1Pitch4, prev->texture[1].texPitch45);
      case 3:
         MMIO(REG_3D_Texture1Address3, prev->texture[1].texOffset3);
      case 2:
         MMIO(REG_3D_Texture1Address2, prev->texture[1].texOffset2);
         MMIO(REG_3D_Texture1Pitch2, prev->texture[1].texPitch23);
      case 1:
         MMIO(REG_3D_Texture1Address1, prev->texture[1].texOffset1);
      case 0:
         MMIO(REG_3D_Texture1Address0, prev->texture[1].texOffset0);
         MMIO(REG_3D_Texture1Pitch0, prev->texture[1].texPitch01);
      }
   }

   /* texture environment */
   if (smesa->GlobalFlag & GFLAG_TEXTUREENV) {
      MMIO(REG_3D_TextureBlendFactor, prev->hwTexEnvColor);
      MMIO(REG_3D_TextureColorBlendSet0, prev->hwTexBlendColor0);
      MMIO(REG_3D_TextureAlphaBlendSet0, prev->hwTexBlendAlpha0);
   }
   if (smesa->GlobalFlag & GFLAG_TEXTUREENV_1) {
      MMIO(REG_3D_TextureBlendFactor, prev->hwTexEnvColor);
      MMIO(REG_3D_TextureColorBlendSet1, prev->hwTexBlendColor1);
      MMIO(REG_3D_TextureAlphaBlendSet1, prev->hwTexBlendAlpha1);
   }

   smesa->GlobalFlag &= ~GFLAG_TEXTURE_STATES;
}

void
sis_fatal_error (void)
{
   /* free video memory, or the framebuffer device will do it automatically */

   fprintf(stderr, "Fatal errors in sis_dri.so\n");
   exit (-1);
}
