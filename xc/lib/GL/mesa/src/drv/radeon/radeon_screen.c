/* $XFree86: xc/lib/GL/mesa/src/drv/radeon/radeon_screen.c,v 1.10 2003/12/18 21:56:37 dawes Exp $ */
/**************************************************************************

Copyright 2000, 2001 ATI Technologies Inc., Ontario, Canada, and
                     VA Linux Systems Inc., Fremont, California.

All Rights Reserved.

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
 *   Kevin E. Martin <martin@valinux.com>
 *   Gareth Hughes <gareth@valinux.com>
 *
 */

#include "glheader.h"
#include "imports.h"

#define STANDALONE_MMIO
#include "radeon_context.h"
#include "radeon_screen.h"
#include "radeon_macros.h"

#include "utils.h"
#include "context.h"
#include "vblank.h"

#include "glxextensions.h"

#if 1
/* Including xf86PciInfo.h introduces a bunch of errors...
 */
#define PCI_CHIP_RADEON_QD	0x5144
#define PCI_CHIP_RADEON_QE	0x5145
#define PCI_CHIP_RADEON_QF	0x5146
#define PCI_CHIP_RADEON_QG	0x5147

#define PCI_CHIP_RADEON_QY	0x5159
#define PCI_CHIP_RADEON_QZ	0x515A

#define PCI_CHIP_RADEON_LW	0x4C57 /* mobility 7 - has tcl */

#define PCI_CHIP_RADEON_LY	0x4C59
#define PCI_CHIP_RADEON_LZ	0x4C5A

#define PCI_CHIP_RV200_QW	0x5157 /* Radeon 7500 - not an R200 at all */
#endif

static int getSwapInfo( __DRIdrawablePrivate *dPriv, __DRIswapInfo * sInfo );

/* Create the device specific screen private data struct.
 */
radeonScreenPtr radeonCreateScreen( __DRIscreenPrivate *sPriv )
{
   radeonScreenPtr screen;
   RADEONDRIPtr dri_priv = (RADEONDRIPtr)sPriv->pDevPriv;
   unsigned char *RADEONMMIO;

   if ( ! driCheckDriDdxDrmVersions( sPriv, "Radeon", 4, 0, 4, 0, 1, 3 ) )
      return NULL;

   /* Allocate the private area */
   screen = (radeonScreenPtr) CALLOC( sizeof(*screen) );
   if ( !screen ) {
      __driUtilMessage("%s: Could not allocate memory for screen structure",
		       __FUNCTION__);
      return NULL;
   }


   /* This is first since which regions we map depends on whether or
    * not we are using a PCI card.
    */
   screen->IsPCI = dri_priv->IsPCI;

   {
      int ret;
      drmRadeonGetParam gp;

      gp.param = RADEON_PARAM_GART_BUFFER_OFFSET;
      gp.value = &screen->gart_buffer_offset;

      ret = drmCommandWriteRead( sPriv->fd, DRM_RADEON_GETPARAM,
				 &gp, sizeof(gp));
      if (ret) {
	 FREE( screen );
	 fprintf(stderr, "drmRadeonGetParam (RADEON_PARAM_GART_BUFFER_OFFSET): %d\n", ret);
	 return NULL;
      }

      if (sPriv->drmMinor >= 6) {
	 gp.param = RADEON_PARAM_IRQ_NR;
	 gp.value = &screen->irq;

	 ret = drmCommandWriteRead( sPriv->fd, DRM_RADEON_GETPARAM,
				    &gp, sizeof(gp));
	 if (ret) {
	    FREE( screen );
	    fprintf(stderr, "drmRadeonGetParam (RADEON_PARAM_IRQ_NR): %d\n", ret);
	    return NULL;
	 }
      }
   }

   screen->mmio.handle = dri_priv->registerHandle;
   screen->mmio.size   = dri_priv->registerSize;
   if ( drmMap( sPriv->fd,
		screen->mmio.handle,
		screen->mmio.size,
		&screen->mmio.map ) ) {
      FREE( screen );
      __driUtilMessage("%s: drmMap failed\n", __FUNCTION__ );
      return NULL;
   }

   RADEONMMIO = screen->mmio.map;

   screen->status.handle = dri_priv->statusHandle;
   screen->status.size   = dri_priv->statusSize;
   if ( drmMap( sPriv->fd,
		screen->status.handle,
		screen->status.size,
		&screen->status.map ) ) {
      drmUnmap( screen->mmio.map, screen->mmio.size );
      FREE( screen );
      __driUtilMessage("%s: drmMap (2) failed\n", __FUNCTION__ );
      return NULL;
   }
   screen->scratch = (__volatile__ CARD32 *)
      ((GLubyte *)screen->status.map + RADEON_SCRATCH_REG_OFFSET);

   screen->buffers = drmMapBufs( sPriv->fd );
   if ( !screen->buffers ) {
      drmUnmap( screen->status.map, screen->status.size );
      drmUnmap( screen->mmio.map, screen->mmio.size );
      FREE( screen );
      __driUtilMessage("%s: drmMapBufs failed\n", __FUNCTION__ );
      return NULL;
   }

   if ( dri_priv->gartTexHandle && dri_priv->gartTexMapSize ) {
      screen->gartTextures.handle = dri_priv->gartTexHandle;
      screen->gartTextures.size   = dri_priv->gartTexMapSize;
      if ( drmMap( sPriv->fd,
		   screen->gartTextures.handle,
		   screen->gartTextures.size,
		   (drmAddressPtr)&screen->gartTextures.map ) ) {
	 drmUnmapBufs( screen->buffers );
	 drmUnmap( screen->status.map, screen->status.size );
	 drmUnmap( screen->mmio.map, screen->mmio.size );
	 FREE( screen );
	 __driUtilMessage("%s: drmMap failed for GART texture area\n", __FUNCTION__);
	 return NULL;
      }

      screen->gart_texture_offset = dri_priv->gartTexOffset + ( screen->IsPCI
		? INREG( RADEON_AIC_LO_ADDR )
		: ( ( INREG( RADEON_MC_AGP_LOCATION ) & 0x0ffffU ) << 16 ) );
   }

   screen->chipset = 0;
   switch ( dri_priv->deviceID ) {
   default:
      fprintf(stderr, "unknown chip id, assuming full radeon support\n");
   case PCI_CHIP_RADEON_QD:
   case PCI_CHIP_RADEON_QE:
   case PCI_CHIP_RADEON_QF:
   case PCI_CHIP_RADEON_QG:
   case PCI_CHIP_RV200_QW:
   case PCI_CHIP_RADEON_LW:
      screen->chipset |= RADEON_CHIPSET_TCL;
   case PCI_CHIP_RADEON_QY:
   case PCI_CHIP_RADEON_QZ:
   case PCI_CHIP_RADEON_LY:
   case PCI_CHIP_RADEON_LZ:
      break;
   }

   screen->cpp = dri_priv->bpp / 8;
   screen->AGPMode = dri_priv->AGPMode;

   screen->fbLocation	= ( INREG( RADEON_MC_FB_LOCATION ) & 0xffff ) << 16;

   if ( sPriv->drmMinor >= 10 ) {
      drmRadeonSetParam sp;

      sp.param = RADEON_SETPARAM_FB_LOCATION;
      sp.value = screen->fbLocation;

      drmCommandWrite( sPriv->fd, DRM_RADEON_SETPARAM,
		       &sp, sizeof( sp ) );
   }

   screen->frontOffset	= dri_priv->frontOffset;
   screen->frontPitch	= dri_priv->frontPitch;
   screen->backOffset	= dri_priv->backOffset;
   screen->backPitch	= dri_priv->backPitch;
   screen->depthOffset	= dri_priv->depthOffset;
   screen->depthPitch	= dri_priv->depthPitch;

   screen->texOffset[RADEON_CARD_HEAP] = dri_priv->textureOffset
				       + screen->fbLocation;
   screen->texSize[RADEON_CARD_HEAP] = dri_priv->textureSize;
   screen->logTexGranularity[RADEON_CARD_HEAP] =
      dri_priv->log2TexGran;

   if ( !screen->gartTextures.map
	|| getenv( "RADEON_GARTTEXTURING_FORCE_DISABLE" ) ) {
      screen->numTexHeaps = RADEON_NR_TEX_HEAPS - 1;
      screen->texOffset[RADEON_GART_HEAP] = 0;
      screen->texSize[RADEON_GART_HEAP] = 0;
      screen->logTexGranularity[RADEON_GART_HEAP] = 0;
   } else {
      screen->numTexHeaps = RADEON_NR_TEX_HEAPS;
      screen->texOffset[RADEON_GART_HEAP] = screen->gart_texture_offset;
      screen->texSize[RADEON_GART_HEAP] = dri_priv->gartTexMapSize;
      screen->logTexGranularity[RADEON_GART_HEAP] =
	 dri_priv->log2GARTTexGran;
   }

   if ( driCompareGLXAPIVersion( 20030813 ) >= 0 ) {
      PFNGLXSCRENABLEEXTENSIONPROC glx_enable_extension =
          (PFNGLXSCRENABLEEXTENSIONPROC) glXGetProcAddress( (const GLubyte *) "__glXScrEnableExtension" );
      void * const psc = sPriv->psc->screenConfigs;

      if ( glx_enable_extension != NULL ) {
	 if ( screen->irq != 0 ) {
	    (*glx_enable_extension)( psc, "GLX_SGI_swap_control" );
	    (*glx_enable_extension)( psc, "GLX_SGI_video_sync" );
	    (*glx_enable_extension)( psc, "GLX_MESA_swap_control" );
	 }

	 (*glx_enable_extension)( psc, "GLX_MESA_swap_frame_usage" );
      }
   }

   screen->driScreen = sPriv;
   screen->sarea_priv_offset = dri_priv->sarea_priv_offset;
   return screen;
}

/* Destroy the device specific screen private data struct.
 */
void radeonDestroyScreen( __DRIscreenPrivate *sPriv )
{
   radeonScreenPtr screen = (radeonScreenPtr)sPriv->private;

   if (!screen)
      return;

   if ( screen->gartTextures.map ) {
      drmUnmap( screen->gartTextures.map, screen->gartTextures.size );
   }
   drmUnmapBufs( screen->buffers );
   drmUnmap( screen->status.map, screen->status.size );
   drmUnmap( screen->mmio.map, screen->mmio.size );

   FREE( screen );
   sPriv->private = NULL;
}


/* Initialize the driver specific screen private data.
 */
static GLboolean
radeonInitDriver( __DRIscreenPrivate *sPriv )
{
   sPriv->private = (void *) radeonCreateScreen( sPriv );
   if ( !sPriv->private ) {
      radeonDestroyScreen( sPriv );
      return GL_FALSE;
   }

   return GL_TRUE;
}



/* Create and initialize the Mesa and driver specific pixmap buffer
 * data.
 */
static GLboolean
radeonCreateBuffer( __DRIscreenPrivate *driScrnPriv,
                    __DRIdrawablePrivate *driDrawPriv,
                    const __GLcontextModes *mesaVis,
                    GLboolean isPixmap )
{
   if (isPixmap) {
      return GL_FALSE; /* not implemented */
   }
   else {
      const GLboolean swDepth = GL_FALSE;
      const GLboolean swAlpha = GL_FALSE;
      const GLboolean swAccum = mesaVis->accumRedBits > 0;
      const GLboolean swStencil = mesaVis->stencilBits > 0 &&
         mesaVis->depthBits != 24;
      driDrawPriv->driverPrivate = (void *)
         _mesa_create_framebuffer( mesaVis,
                                   swDepth,
                                   swStencil,
                                   swAccum,
                                   swAlpha );
      return (driDrawPriv->driverPrivate != NULL);
   }
}


static void
radeonDestroyBuffer(__DRIdrawablePrivate *driDrawPriv)
{
   _mesa_destroy_framebuffer((GLframebuffer *) (driDrawPriv->driverPrivate));
}




/* Fullscreen mode isn't used for much -- could be a way to shrink
 * front/back buffers & get more texture memory if the client has
 * changed the video resolution.
 * 
 * Pageflipping is now done automatically whenever there is a single
 * 3d client.
 */
static GLboolean
radeonOpenCloseFullScreen( __DRIcontextPrivate *driContextPriv )
{
   return GL_TRUE;
}

static struct __DriverAPIRec radeonAPI = {
   .InitDriver      = radeonInitDriver,
   .DestroyScreen   = radeonDestroyScreen,
   .CreateContext   = radeonCreateContext,
   .DestroyContext  = radeonDestroyContext,
   .CreateBuffer    = radeonCreateBuffer,
   .DestroyBuffer   = radeonDestroyBuffer,
   .SwapBuffers     = radeonSwapBuffers,
   .MakeCurrent     = radeonMakeCurrent,
   .UnbindContext   = radeonUnbindContext,
   .OpenFullScreen  = radeonOpenCloseFullScreen,
   .CloseFullScreen = radeonOpenCloseFullScreen,
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
   psp = __driUtilCreateScreen(dpy, scrn, psc, numConfigs, config, &radeonAPI);
   return (void *) psp;
}


/**
 * This function is called by libGL.so as soon as libGL.so is loaded.
 * This is where we'd register new extension functions with the dispatcher.
 *
 * \todo This interface has been deprecated, so we should probably remove
 *       this function before the next XFree86 release.
 */
void
__driRegisterExtensions( void )
{
   PFNGLXENABLEEXTENSIONPROC glx_enable_extension;


   if ( driCompareGLXAPIVersion( 20030317 ) >= 0 ) {
      glx_enable_extension = (PFNGLXENABLEEXTENSIONPROC)
	  glXGetProcAddress( (const GLubyte *) "__glXEnableExtension" );

      if ( glx_enable_extension != NULL ) {
	 (*glx_enable_extension)( "GLX_SGI_swap_control", GL_FALSE );
	 (*glx_enable_extension)( "GLX_SGI_video_sync", GL_FALSE );
	 (*glx_enable_extension)( "GLX_MESA_swap_control", GL_FALSE );
	 (*glx_enable_extension)( "GLX_MESA_swap_frame_usage", GL_FALSE );
      }
   }
}


/**
 * Get information about previous buffer swaps.
 */
static int
getSwapInfo( __DRIdrawablePrivate *dPriv, __DRIswapInfo * sInfo )
{
   radeonContextPtr  rmesa;

   if ( (dPriv == NULL) || (dPriv->driContextPriv == NULL)
	|| (dPriv->driContextPriv->driverPrivate == NULL)
	|| (sInfo == NULL) ) {
      return -1;
   }

   rmesa = (radeonContextPtr) dPriv->driContextPriv->driverPrivate;
   sInfo->swap_count = rmesa->swap_count;
   sInfo->swap_ust = rmesa->swap_ust;
   sInfo->swap_missed_count = rmesa->swap_missed_count;

   sInfo->swap_missed_usage = (sInfo->swap_missed_count != 0)
       ? driCalculateSwapUsage( dPriv, 0, rmesa->swap_missed_ust )
       : 0.0;

   return 0;
}
