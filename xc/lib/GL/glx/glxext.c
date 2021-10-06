/* $XFree86: xc/lib/GL/glx/glxext.c,v 1.23 2004/01/30 20:33:06 alanh Exp $ */

/*
** License Applicability. Except to the extent portions of this file are
** made subject to an alternative license as permitted in the SGI Free
** Software License B, Version 1.1 (the "License"), the contents of this
** file are subject only to the provisions of the License. You may not use
** this file except in compliance with the License. You may obtain a copy
** of the License at Silicon Graphics, Inc., attn: Legal Services, 1600
** Amphitheatre Parkway, Mountain View, CA 94043-1351, or at:
** 
** http://oss.sgi.com/projects/FreeB
** 
** Note that, as provided in the License, the Software is distributed on an
** "AS IS" basis, with ALL EXPRESS AND IMPLIED WARRANTIES AND CONDITIONS
** DISCLAIMED, INCLUDING, WITHOUT LIMITATION, ANY IMPLIED WARRANTIES AND
** CONDITIONS OF MERCHANTABILITY, SATISFACTORY QUALITY, FITNESS FOR A
** PARTICULAR PURPOSE, AND NON-INFRINGEMENT.
** 
** Original Code. The Original Code is: OpenGL Sample Implementation,
** Version 1.2.1, released January 26, 2000, developed by Silicon Graphics,
** Inc. The Original Code is Copyright (c) 1991-2000 Silicon Graphics, Inc.
** Copyright in any portions created by third parties is as indicated
** elsewhere herein. All Rights Reserved.
** 
** Additional Notice Provisions: The application programming interfaces
** established by SGI in conjunction with the Original Code are The
** OpenGL(R) Graphics System: A Specification (Version 1.2.1), released
** April 1, 1999; The OpenGL(R) Graphics System Utility Library (Version
** 1.3), released November 4, 1998; and OpenGL(R) Graphics with the X
** Window System(R) (Version 1.3), released October 19, 1998. This software
** was created using the OpenGL(R) version 1.2.1 Sample Implementation
** published by SGI, but has not been independently verified as being
** compliant with the OpenGL(R) version 1.2.1 Specification.
**
*/

/*                                                            <
 * Direct rendering support added by Precision Insight, Inc.  <
 *                                                            <
 * Authors:                                                   <
 *   Kevin E. Martin <kevin@precisioninsight.com>             <
 *                                                            <
 */     

#include "packrender.h"
#include <stdio.h>
#include <Xext.h>
#include <extutil.h>
#include <assert.h>
#include "indirect_init.h"
#include "glapi.h"
#ifdef XTHREADS
#include "Xthreads.h"
#endif
#include "glxextensions.h"
#include "glcontextmodes.h"

#include <assert.h>

#ifdef DEBUG
void __glXDumpDrawBuffer(__GLXcontext *ctx);
#endif

#ifdef USE_SPARC_ASM
/*
 * This is where our dispatch table's bounds are.
 * And the static mesa_init is taken directly from
 * Mesa's 'sparc.c' initializer.
 *
 * We need something like this here, because this version
 * of openGL/glx never initializes a Mesa context, and so
 * the address of the dispatch table pointer never gets stuffed
 * into the dispatch jump table otherwise.
 *
 * It matters only on SPARC, and only if you are using assembler
 * code instead of C-code indirect dispatch.
 *
 * -- FEM, 04.xii.03
 */
extern unsigned int _mesa_sparc_glapi_begin;
extern unsigned int _mesa_sparc_glapi_end;
extern void __glapi_sparc_icache_flush(unsigned int *);
static void _glx_mesa_init_sparc_glapi_relocs(void);
static int _mesa_sparc_needs_init = 1;
#define INIT_MESA_SPARC { \
    if(_mesa_sparc_needs_init) { \
      _glx_mesa_init_sparc_glapi_relocs(); \
      _mesa_sparc_needs_init = 0; \
  } \
}
#else
#define INIT_MESA_SPARC
#endif

static Bool MakeContextCurrent(Display *dpy, GLXDrawable draw,
    GLXDrawable read, GLXContext gc);

/*
** We setup some dummy structures here so that the API can be used
** even if no context is current.
*/

static GLubyte dummyBuffer[__GLX_BUFFER_LIMIT_SIZE];

/*
** Dummy context used by small commands when there is no current context.
** All the
** gl and glx entry points are designed to operate as nop's when using
** the dummy context structure.
*/
static __GLXcontext dummyContext = {
    &dummyBuffer[0],
    &dummyBuffer[0],
    &dummyBuffer[0],
    &dummyBuffer[__GLX_BUFFER_LIMIT_SIZE],
    sizeof(dummyBuffer),
};


/*
** All indirect rendering contexts will share the same indirect dispatch table.
*/
static __GLapi *IndirectAPI = NULL;


/*
 * Current context management and locking
 */

#if defined(GLX_DIRECT_RENDERING) && defined(XTHREADS)

/* thread safe */
static GLboolean TSDinitialized = GL_FALSE;
static xthread_key_t ContextTSD;

__GLXcontext *__glXGetCurrentContext(void)
{
   if (!TSDinitialized) {
      xthread_key_create(&ContextTSD, NULL);
      TSDinitialized = GL_TRUE;
      return &dummyContext;
   }
   else {
      void *p;
      xthread_get_specific(ContextTSD, &p);
      if (!p)
         return &dummyContext;
      else
         return (__GLXcontext *) p;
   }
}

void __glXSetCurrentContext(__GLXcontext *c)
{
   if (!TSDinitialized) {
      xthread_key_create(&ContextTSD, NULL);
      TSDinitialized = GL_TRUE;
   }
   xthread_set_specific(ContextTSD, c);
}


/* Used by the __glXLock() and __glXUnlock() macros */
xmutex_rec __glXmutex;

#else

/* not thread safe */
__GLXcontext *__glXcurrentContext = &dummyContext;

#endif


/*
** You can set this cell to 1 to force the gl drawing stuff to be
** one command per packet
*/
int __glXDebug = 0;

/*
** forward prototype declarations
*/
int __glXCloseDisplay(Display *dpy, XExtCodes *codes);

static GLboolean FillInVisuals( __GLXscreenConfigs * psc );

/************************************************************************/

/* Extension required boiler plate */

static char *__glXExtensionName = GLX_EXTENSION_NAME;
XExtensionInfo *__glXExtensionInfo = NULL;

static /* const */ char *error_list[] = {
    "GLXBadContext",
    "GLXBadContextState",
    "GLXBadDrawable",
    "GLXBadPixmap",
    "GLXBadContextTag",
    "GLXBadCurrentWindow",
    "GLXBadRenderRequest",
    "GLXBadLargeRequest",
    "GLXUnsupportedPrivateRequest",
};

int __glXCloseDisplay(Display *dpy, XExtCodes *codes)
{
  GLXContext gc;

  gc = __glXGetCurrentContext();
  if (dpy == gc->currentDpy) {
    __glXSetCurrentContext(&dummyContext);
#ifdef GLX_DIRECT_RENDERING
    _glapi_set_dispatch(NULL);  /* no-op functions */
#endif
    __glXFreeContext(gc);
  }

  return XextRemoveDisplay(__glXExtensionInfo, dpy);
}


static XEXT_GENERATE_ERROR_STRING(__glXErrorString, __glXExtensionName,
				  __GLX_NUMBER_ERRORS, error_list)

static /* const */ XExtensionHooks __glXExtensionHooks = {
    NULL,				/* create_gc */
    NULL,				/* copy_gc */
    NULL,				/* flush_gc */
    NULL,				/* free_gc */
    NULL,				/* create_font */
    NULL,				/* free_font */
    __glXCloseDisplay,			/* close_display */
    NULL,				/* wire_to_event */
    NULL,				/* event_to_wire */
    NULL,				/* error */
    __glXErrorString,			/* error_string */
};

static
XEXT_GENERATE_FIND_DISPLAY(__glXFindDisplay, __glXExtensionInfo,
			   __glXExtensionName, &__glXExtensionHooks,
			   __GLX_NUMBER_EVENTS, NULL)

/************************************************************************/

/*
** Free the per screen configs data as well as the array of
** __glXScreenConfigs.
*/
static void FreeScreenConfigs(__GLXdisplayPrivate *priv)
{
    __GLXscreenConfigs *psc;
    GLint i, screens;

    /* Free screen configuration information */
    psc = priv->screenConfigs;
    screens = ScreenCount(priv->dpy);
    for (i = 0; i < screens; i++, psc++) {
	if (psc->configs) {
	    Xfree((char*) psc->configs);
	    if(psc->effectiveGLXexts)
		Xfree(psc->effectiveGLXexts);

	    if ( psc->old_configs != NULL ) {
		Xfree( psc->old_configs );
		psc->old_configs = NULL;
		psc->numOldConfigs = 0;
	    }

	    psc->configs = 0;	/* NOTE: just for paranoia */
	}

#ifdef GLX_DIRECT_RENDERING
	/* Free the direct rendering per screen data */
	if (psc->driScreen.private)
	    (*psc->driScreen.destroyScreen)(priv->dpy, i,
					    psc->driScreen.private);
	psc->driScreen.private = NULL;
#endif
    }
    XFree((char*) priv->screenConfigs);
}

/*
** Release the private memory referred to in a display private
** structure.  The caller will free the extension structure.
*/
static int __glXFreeDisplayPrivate(XExtData *extension)
{
    __GLXdisplayPrivate *priv;

    priv = (__GLXdisplayPrivate*) extension->private_data;
    FreeScreenConfigs(priv);
    if(priv->serverGLXvendor) {
	Xfree((char*)priv->serverGLXvendor);
	priv->serverGLXvendor = 0x0; /* to protect against double free's */
    }
    if(priv->serverGLXversion) {
	Xfree((char*)priv->serverGLXversion);
	priv->serverGLXversion = 0x0; /* to protect against double free's */
    }

#if 0 /* GLX_DIRECT_RENDERING */
    /* Free the direct rendering per display data */
    if (priv->driDisplay.private)
	(*priv->driDisplay.destroyDisplay)(priv->dpy,
					   priv->driDisplay.private);
    priv->driDisplay.private = NULL;
#endif

#ifdef GLX_DIRECT_RENDERING
    XFree(priv->driDisplay.createScreen);
#endif

    Xfree((char*) priv);
    return 0;
}

/************************************************************************/

/*
** Query the version of the GLX extension.  This procedure works even if
** the client extension is not completely set up.
*/
static Bool QueryVersion(Display *dpy, int opcode, int *major, int *minor)
{
    xGLXQueryVersionReq *req;
    xGLXQueryVersionReply reply;

    /* Send the glXQueryVersion request */
    LockDisplay(dpy);
    GetReq(GLXQueryVersion,req);
    req->reqType = opcode;
    req->glxCode = X_GLXQueryVersion;
    req->majorVersion = GLX_MAJOR_VERSION;
    req->minorVersion = GLX_MINOR_VERSION;
    _XReply(dpy, (xReply*) &reply, 0, False);
    UnlockDisplay(dpy);
    SyncHandle();

    if (reply.majorVersion != GLX_MAJOR_VERSION) {
	/*
	** The server does not support the same major release as this
	** client.
	*/
	return GL_FALSE;
    }
    *major = reply.majorVersion;
    *minor = min(reply.minorVersion, GLX_MINOR_VERSION);
    return GL_TRUE;
}


static GLboolean
FillInVisuals( __GLXscreenConfigs * psc )
{
    int glx_visual_count;
    int i;


    glx_visual_count = 0;
    for ( i = 0 ; i < psc->numConfigs ; i++ ) {
	if ( (psc->configs[i].visualID != GLX_DONT_CARE)
	     && (psc->configs[i].sampleBuffers == 0)
	     && (psc->configs[i].samples == 0)
	     && (psc->configs[i].drawableType == GLX_WINDOW_BIT)
	     && ((psc->configs[i].xRenderable == GL_TRUE)
		 || (psc->configs[i].xRenderable == GLX_DONT_CARE)) ) {
	    glx_visual_count++;
	}
    }

    psc->old_configs = (__GLXvisualConfig *)
	Xmalloc( sizeof( __GLXvisualConfig ) * glx_visual_count );
    if ( psc->old_configs == NULL ) {
	return GL_FALSE;
    }

    glx_visual_count = 0;
    for ( i = 0 ; i < psc->numConfigs ; i++ ) {
	if ( (psc->configs[i].visualID != GLX_DONT_CARE)
	     && (psc->configs[i].sampleBuffers == 0)
	     && (psc->configs[i].samples == 0)
	     && (psc->configs[i].drawableType == GLX_WINDOW_BIT)
	     && ((psc->configs[i].xRenderable == GL_TRUE)
		 || (psc->configs[i].xRenderable == GLX_DONT_CARE)) ) {

#define COPY_VALUE(src_tag,dst_tag) \
    psc->old_configs[glx_visual_count]. dst_tag = psc->configs[i]. src_tag

	    COPY_VALUE( visualID,  vid );
	    COPY_VALUE( rgbMode,   rgba );
	    COPY_VALUE( stereoMode, stereo );
	    COPY_VALUE( doubleBufferMode, doubleBuffer );
	    
	    psc->old_configs[glx_visual_count].class = 
		_gl_convert_to_x_visual_type( psc->configs[i].visualType );

	    COPY_VALUE( level, level );
	    COPY_VALUE( numAuxBuffers, auxBuffers );

	    COPY_VALUE( redBits,        redSize );
	    COPY_VALUE( greenBits,      greenSize );
	    COPY_VALUE( blueBits,       blueSize );
	    COPY_VALUE( alphaBits,      alphaSize );
	    COPY_VALUE( rgbBits,        bufferSize );
	    COPY_VALUE( accumRedBits,   accumRedSize );
	    COPY_VALUE( accumGreenBits, accumGreenSize );
	    COPY_VALUE( accumBlueBits,  accumBlueSize );
	    COPY_VALUE( accumAlphaBits, accumAlphaSize );
	    COPY_VALUE( depthBits,      depthSize );
	    COPY_VALUE( stencilBits,    stencilSize );

	    COPY_VALUE( visualRating, visualRating );
	    COPY_VALUE( transparentPixel, transparentPixel );
	    COPY_VALUE( transparentRed,   transparentRed );
	    COPY_VALUE( transparentGreen, transparentGreen );
	    COPY_VALUE( transparentBlue,  transparentBlue );
	    COPY_VALUE( transparentAlpha, transparentAlpha );
	    COPY_VALUE( transparentIndex, transparentIndex );

#undef COPY_VALUE

	    glx_visual_count++;
	}
    }

    psc->numOldConfigs = glx_visual_count;
    return GL_TRUE;
}


void 
__glXInitializeVisualConfigFromTags( __GLcontextModes *config, int count, 
				     const INT32 *bp, Bool tagged_only,
				     Bool fbconfig_style_tags )
{
    int i;

    if (!tagged_only) {
	/* Copy in the first set of properties */
	config->visualID = *bp++;

	config->visualType = _gl_convert_from_x_visual_type( *bp++ );

	config->rgbMode = *bp++;

	config->redBits = *bp++;
	config->greenBits = *bp++;
	config->blueBits = *bp++;
	config->alphaBits = *bp++;
	config->accumRedBits = *bp++;
	config->accumGreenBits = *bp++;
	config->accumBlueBits = *bp++;
	config->accumAlphaBits = *bp++;

	config->doubleBufferMode = *bp++;
	config->stereoMode = *bp++;

	config->rgbBits = *bp++;
	config->depthBits = *bp++;
	config->stencilBits = *bp++;
	config->numAuxBuffers = *bp++;
	config->level = *bp++;

	count -= __GLX_MIN_CONFIG_PROPS;
    }
    else {
	config->visualID = (XID) GLX_DONT_CARE;
	config->visualType = GLX_DONT_CARE;
	config->rgbMode = ( fbconfig_style_tags )
	    ? GL_TRUE /* glXChooseFBConfig() */
	    : GL_FALSE; /* glXChooseVisual() */

	config->redBits = 0;
	config->greenBits = 0;
	config->blueBits = 0;
	config->alphaBits = 0;
	config->accumRedBits = 0;
	config->accumGreenBits = 0;
	config->accumBlueBits = 0;
	config->accumAlphaBits = 0;

	config->doubleBufferMode = ( fbconfig_style_tags )
	    ? GLX_DONT_CARE /* glXChooseFBConfig() */
	    : GL_FALSE; /* glXChooseVisual() */
	config->stereoMode = GL_FALSE;

	config->rgbBits = 0;
	config->depthBits = 0;
	config->stencilBits = 0;
	config->numAuxBuffers = 0;
	config->level = 0;
    }

    /*
    ** Additional properties may be in a list at the end
    ** of the reply.  They are in pairs of property type
    ** and property value.
    */
    config->visualRating = GLX_DONT_CARE;
    config->visualSelectGroup = 0;
    config->transparentPixel = GLX_NONE;
    config->transparentRed = GLX_DONT_CARE;
    config->transparentGreen = GLX_DONT_CARE;
    config->transparentBlue = GLX_DONT_CARE;
    config->transparentAlpha = GLX_DONT_CARE;
    config->transparentIndex = GLX_DONT_CARE;

    config->floatMode = GL_FALSE;
    config->drawableType = GLX_WINDOW_BIT;
    config->renderType = (config->rgbMode) ? GLX_RGBA_BIT : GLX_COLOR_INDEX_BIT;
    config->xRenderable = GLX_DONT_CARE;
    config->fbconfigID = (GLXFBConfigID)(GLX_DONT_CARE);

    config->maxPbufferWidth = 0;
    config->maxPbufferHeight = 0;
    config->maxPbufferPixels = 0;
    config->optimalPbufferWidth = 0;
    config->optimalPbufferHeight = 0;

    config->sampleBuffers = 0;
    config->samples = 0;
    config->swapMethod = GLX_SWAP_UNDEFINED_OML;

#define FETCH_OR_SET(tag) \
    config-> tag = ( fbconfig_style_tags ) ? *bp++ : 1

    for (i = 0; i < count; i += 2 ) {
	switch(*bp++) {
	  case GLX_RGBA:
	    FETCH_OR_SET( rgbMode );
	    config->renderType = (config->rgbMode) ? GLX_RGBA_BIT : GLX_COLOR_INDEX_BIT;
	    break;
	  case GLX_BUFFER_SIZE:
	    config->rgbBits = *bp++;
	    break;
	  case GLX_LEVEL:
	    config->level = *bp++;
	    break;
	  case GLX_DOUBLEBUFFER:
	    FETCH_OR_SET( doubleBufferMode );
	    break;
	  case GLX_STEREO:
	    FETCH_OR_SET( stereoMode );
	    break;
	  case GLX_AUX_BUFFERS:
	    config->numAuxBuffers = *bp++;
	    break;
	  case GLX_RED_SIZE:
	    config->redBits = *bp++;
	    break;
	  case GLX_GREEN_SIZE:
	    config->greenBits = *bp++;
	    break;
	  case GLX_BLUE_SIZE:
	    config->blueBits = *bp++;
	    break;
	  case GLX_ALPHA_SIZE:
	    config->alphaBits = *bp++;
	    break;
	  case GLX_DEPTH_SIZE:
	    config->depthBits = *bp++;
	    break;
	  case GLX_STENCIL_SIZE:
	    config->stencilBits = *bp++;
	    break;
	  case GLX_ACCUM_RED_SIZE:
	    config->accumRedBits = *bp++;
	    break;
	  case GLX_ACCUM_GREEN_SIZE:
	    config->accumGreenBits = *bp++;
	    break;
	  case GLX_ACCUM_BLUE_SIZE:
	    config->accumBlueBits = *bp++;
	    break;
	  case GLX_ACCUM_ALPHA_SIZE:
	    config->accumAlphaBits = *bp++;
	    break;
	  case GLX_VISUAL_CAVEAT_EXT:
	    config->visualRating = *bp++;    
	    break;
	  case GLX_X_VISUAL_TYPE:
	    config->visualType = *bp++;
	    break;
	  case GLX_TRANSPARENT_TYPE:
	    config->transparentPixel = *bp++;    
	    break;
	  case GLX_TRANSPARENT_INDEX_VALUE:
	    config->transparentIndex = *bp++;    
	    break;
	  case GLX_TRANSPARENT_RED_VALUE:
	    config->transparentRed = *bp++;    
	    break;
	  case GLX_TRANSPARENT_GREEN_VALUE:
	    config->transparentGreen = *bp++;    
	    break;
	  case GLX_TRANSPARENT_BLUE_VALUE:
	    config->transparentBlue = *bp++;    
	    break;
	  case GLX_TRANSPARENT_ALPHA_VALUE:
	    config->transparentAlpha = *bp++;    
	    break;
	  case GLX_VISUAL_ID:
	    config->visualID = *bp++;
	    break;
	  case GLX_DRAWABLE_TYPE:
	    config->drawableType = *bp++;
	    break;
	  case GLX_RENDER_TYPE:
	    config->renderType = *bp++;
	    break;
	  case GLX_X_RENDERABLE:
	    config->xRenderable = *bp++;
	    break;
	  case GLX_FBCONFIG_ID:
	    config->fbconfigID = *bp++;
	    break;
	  case GLX_MAX_PBUFFER_WIDTH:
	    config->maxPbufferWidth = *bp++;
	    break;
	  case GLX_MAX_PBUFFER_HEIGHT:
	    config->maxPbufferHeight = *bp++;
	    break;
	  case GLX_MAX_PBUFFER_PIXELS:
	    config->maxPbufferPixels = *bp++;
	    break;
	  case GLX_OPTIMAL_PBUFFER_WIDTH_SGIX:
	    config->optimalPbufferWidth = *bp++;
	    break;
	  case GLX_OPTIMAL_PBUFFER_HEIGHT_SGIX:
	    config->optimalPbufferHeight = *bp++;
	    break;
	  case GLX_VISUAL_SELECT_GROUP_SGIX:
	    config->visualSelectGroup = *bp++;
	    break;
	  case GLX_SWAP_METHOD_OML:
	    config->swapMethod = *bp++;
	    break;
	  case GLX_SAMPLE_BUFFERS_SGIS:
	    config->sampleBuffers = *bp++;
	    break;
	  case GLX_SAMPLES_SGIS:
	    config->samples = *bp++;
	    break;
	  case None:
	    i = count;
	    break;
	  default:
	    break;
	}
    }

    config->haveAccumBuffer = ((config->accumRedBits +
			       config->accumGreenBits +
			       config->accumBlueBits +
			       config->accumAlphaBits) > 0);
    config->haveDepthBuffer = (config->depthBits > 0);
    config->haveStencilBuffer = (config->stencilBits > 0);
}


/*
** Allocate the memory for the per screen configs for each screen.
** If that works then fetch the per screen configs data.
*/
static Bool AllocAndFetchScreenConfigs(Display *dpy, __GLXdisplayPrivate *priv)
{
    xGLXGetVisualConfigsReq *req;
    xGLXGetFBConfigsReq *fb_req;
    xGLXVendorPrivateReq *vpreq;
    xGLXGetFBConfigsSGIXReq *sgi_req;
    xGLXGetVisualConfigsReply reply;
    __GLXscreenConfigs *psc;
    __GLcontextModes *config;
    GLint i, j, nprops, screens;
    INT32 buf[__GLX_TOTAL_CONFIG], *props;
    unsigned supported_request = 0;
    unsigned prop_size;

    /*
    ** First allocate memory for the array of per screen configs.
    */
    screens = ScreenCount(dpy);
    psc = (__GLXscreenConfigs*) Xmalloc(screens * sizeof(__GLXscreenConfigs));
    if (!psc) {
	return GL_FALSE;
    }
    memset(psc, 0, screens * sizeof(__GLXscreenConfigs));
    priv->screenConfigs = psc;
    
    priv->serverGLXversion = __glXInternalQueryServerString(dpy,
	priv->majorOpcode, 0, GLX_VERSION);
    if ( priv->serverGLXversion == NULL ) {
	FreeScreenConfigs(priv);
	return GL_FALSE;
    }

    if ( atof( priv->serverGLXversion ) >= 1.3 ) {
	supported_request = 1;
    }

    /*
    ** Now fetch each screens configs structures.  If a screen supports
    ** GL (by returning a numVisuals > 0) then allocate memory for our
    ** config structure and then fill it in.
    */
    for (i = 0; i < screens; i++, psc++) {
	if ( supported_request != 1 ) {
	    psc->serverGLXexts = __glXInternalQueryServerString(dpy,
		priv->majorOpcode, i, GLX_EXTENSIONS);
	    if ( strstr( psc->serverGLXexts, "GLX_SGIX_fbconfig" ) != NULL ) {
		supported_request = 2;
	    }
	    else {
		supported_request = 3;
	    }
	}


	LockDisplay(dpy);
	switch( supported_request ) {
	    case 1:
	    GetReq(GLXGetFBConfigs,fb_req);
	    fb_req->reqType = priv->majorOpcode;
	    fb_req->glxCode = X_GLXGetFBConfigs;
	    fb_req->screen = i;
	    break;
	    
	    case 2:
	    GetReqExtra(GLXVendorPrivate,
			sz_xGLXGetFBConfigsSGIXReq-sz_xGLXVendorPrivateReq,vpreq);
	    sgi_req = (xGLXGetFBConfigsSGIXReq *) vpreq;
	    sgi_req->reqType = priv->majorOpcode;
	    sgi_req->glxCode = X_GLXVendorPrivateWithReply;
	    sgi_req->vendorCode = X_GLXvop_GetFBConfigsSGIX;
	    sgi_req->screen = i;
	    break;

	    case 3:
	    GetReq(GLXGetVisualConfigs,req);
	    req->reqType = priv->majorOpcode;
	    req->glxCode = X_GLXGetVisualConfigs;
	    req->screen = i;
	    break;
 	}

	if (!_XReply(dpy, (xReply*) &reply, 0, False)) {
	    /* Something is busted. Punt. */
	    UnlockDisplay(dpy);
	    FreeScreenConfigs(priv);
	    return GL_FALSE;
	}

	UnlockDisplay(dpy);
	if (!reply.numVisuals) {
	    /* This screen does not support GL rendering */
	    UnlockDisplay(dpy);
	    continue;
	}

	/* FIXME: Is the __GLX_MIN_CONFIG_PROPS test correct for
	 * FIXME: FBconfigs? 
	 */
	/* Check number of properties */
	nprops = reply.numProps;
	if ((nprops < __GLX_MIN_CONFIG_PROPS) ||
	    (nprops > __GLX_MAX_CONFIG_PROPS)) {
	    /* Huh?  Not in protocol defined limits.  Punt */
	    UnlockDisplay(dpy);
	    SyncHandle();
	    FreeScreenConfigs(priv);
	    return GL_FALSE;
	}

	/* Allocate memory for our config structure */
	psc->configs = (__GLcontextModes*)
	    Xmalloc(reply.numVisuals * sizeof(__GLcontextModes));
	psc->numConfigs = reply.numVisuals;
	if (!psc->configs) {
	    UnlockDisplay(dpy);
	    SyncHandle();
	    FreeScreenConfigs(priv);
	    return GL_FALSE;
	}

	/* Allocate memory for the properties, if needed */
	if ( supported_request != 3 ) {
	    nprops *= 2;
	}

	prop_size = nprops * __GLX_SIZE_INT32;

	if (prop_size <= sizeof(buf)) {
 	    props = buf;
 	} else {
	    props = (INT32 *) Xmalloc(prop_size);
 	} 

	/* Read each config structure and convert it into our format */
	config = psc->configs;
	for (j = 0; j < reply.numVisuals; j++, config++) {
	    _XRead(dpy, (char *)props, prop_size);

	    __glXInitializeVisualConfigFromTags( config, nprops, props,
						 (supported_request != 3),
						 GL_TRUE );
	    config->screen = i;
	}
	if (props != buf) {
	    Xfree((char *)props);
	}
	UnlockDisplay(dpy);

#ifdef GLX_DIRECT_RENDERING
        /* Initialize per screen dynamic client GLX extensions */
	psc->ext_list_first_time = GL_TRUE;
	/* Initialize the direct rendering per screen data and functions */
	if (priv->driDisplay.private &&
		priv->driDisplay.createScreen &&
		priv->driDisplay.createScreen[i]) {
	    /* screen initialization (bootstrap the driver) */
	    if ( (psc->old_configs == NULL)
		 && !FillInVisuals(psc) ) {
		FreeScreenConfigs(priv);
		return GL_FALSE;
	    }

	    psc->driScreen.screenConfigs = (void *)psc;
	    psc->driScreen.private =
		(*(priv->driDisplay.createScreen[i]))(dpy, i, &psc->driScreen,
						 psc->numOldConfigs,
						 psc->old_configs);
	}
#endif
    }
    SyncHandle();
    return GL_TRUE;
}

/*
** Initialize the client side extension code.
*/
__GLXdisplayPrivate *__glXInitialize(Display* dpy)
{
    XExtDisplayInfo *info = __glXFindDisplay(dpy);
    XExtData **privList, *private, *found;
    __GLXdisplayPrivate *dpyPriv;
    XEDataObject dataObj;
    int major, minor;

#if defined(GLX_DIRECT_RENDERING) && defined(XTHREADS)
    {
        static int firstCall = 1;
        if (firstCall) {
            /* initialize the GLX mutexes */
# if !defined(GLX_USE_APPLEGL)
            xmutex_init(&__glXmutex);
# else
	    /* Need a recursive mutex for our surface-notification.. */
	    pthread_mutexattr_t attr;
	    pthread_mutexattr_init (&attr);
	    pthread_mutexattr_settype (&attr, PTHREAD_MUTEX_RECURSIVE);
	    pthread_mutex_init (&__glXmutex, &attr);
	    pthread_mutexattr_destroy (&attr);
# endif

            firstCall = 0;
        }
    }
#endif

    INIT_MESA_SPARC
    /* The one and only long long lock */
    __glXLock();

    if (!XextHasExtension(info)) {
	/* No GLX extension supported by this server. Oh well. */
	__glXUnlock();
	XMissingExtension(dpy, __glXExtensionName);
	return 0;
    }

    /* See if a display private already exists.  If so, return it */
    dataObj.display = dpy;
    privList = XEHeadOfExtensionList(dataObj);
    found = XFindOnExtensionList(privList, info->codes->extension);
    if (found) {
	__glXUnlock();
	return (__GLXdisplayPrivate *) found->private_data;
    }

    /* See if the versions are compatible */
    if (!QueryVersion(dpy, info->codes->major_opcode, &major, &minor)) {
	/* The client and server do not agree on versions.  Punt. */
	__glXUnlock();
	return 0;
    }

    /*
    ** Allocate memory for all the pieces needed for this buffer.
    */
    private = (XExtData *) Xmalloc(sizeof(XExtData));
    if (!private) {
	__glXUnlock();
	return 0;
    }
    dpyPriv = (__GLXdisplayPrivate *) Xmalloc(sizeof(__GLXdisplayPrivate));
    if (!dpyPriv) {
	__glXUnlock();
	Xfree((char*) private);
	return 0;
    }

    /*
    ** Init the display private and then read in the screen config
    ** structures from the server.
    */
    dpyPriv->majorOpcode = info->codes->major_opcode;
    dpyPriv->majorVersion = major;
    dpyPriv->minorVersion = minor;
    dpyPriv->dpy = dpy;

    dpyPriv->serverGLXvendor = 0x0; 
    dpyPriv->serverGLXversion = 0x0;

#ifdef GLX_DIRECT_RENDERING
    /*
    ** Initialize the direct rendering per display data and functions.
    ** Note: This _must_ be done before calling any other DRI routines
    ** (e.g., those called in AllocAndFetchScreenConfigs).
    */
    if (getenv("LIBGL_ALWAYS_INDIRECT")) {
        /* Assinging zero here assures we'll never go direct */
        dpyPriv->driDisplay.private = 0;
        dpyPriv->driDisplay.destroyDisplay = 0;
        dpyPriv->driDisplay.createScreen = 0;
    }
    else {
        dpyPriv->driDisplay.private =
            driCreateDisplay(dpy, &dpyPriv->driDisplay);
    }
#endif

    if (!AllocAndFetchScreenConfigs(dpy, dpyPriv)) {
	__glXUnlock();
	Xfree((char*) dpyPriv);
	Xfree((char*) private);
	return 0;
    }

    /*
    ** Fill in the private structure.  This is the actual structure that
    ** hangs off of the Display structure.  Our private structure is
    ** referred to by this structure.  Got that?
    */
    private->number = info->codes->extension;
    private->next = 0;
    private->free_private = __glXFreeDisplayPrivate;
    private->private_data = (char *) dpyPriv;
    XAddToExtensionList(privList, private);

    if (dpyPriv->majorVersion == 1 && dpyPriv->minorVersion >= 1) {
        __glXClientInfo(dpy, dpyPriv->majorOpcode);
    }
    __glXUnlock();

    return dpyPriv;
}

/*
** Setup for sending a GLX command on dpy.  Make sure the extension is
** initialized.  Try to avoid calling __glXInitialize as its kinda slow.
*/
CARD8 __glXSetupForCommand(Display *dpy)
{
    GLXContext gc;
    __GLXdisplayPrivate *priv;

    /* If this thread has a current context, flush its rendering commands */
    gc = __glXGetCurrentContext();
    if (gc->currentDpy) {
	/* Flush rendering buffer of the current context, if any */
	(void) __glXFlushRenderBuffer(gc, gc->pc);

	if (gc->currentDpy == dpy) {
	    /* Use opcode from gc because its right */
            INIT_MESA_SPARC
	    return gc->majorOpcode;
	} else {
	    /*
	    ** Have to get info about argument dpy because it might be to
	    ** a different server
	    */
	}
    }

    /* Forced to lookup extension via the slow initialize route */
    priv = __glXInitialize(dpy);
    if (!priv) {
	return 0;
    }
    return priv->majorOpcode;
}

/*
** Flush the drawing command transport buffer.
*/
GLubyte *__glXFlushRenderBuffer(__GLXcontext *ctx, GLubyte *pc)
{
    Display *dpy;
    xGLXRenderReq *req;
    GLint size;

    if (!(dpy = ctx->currentDpy)) {
	/* Using the dummy context */
	ctx->pc = ctx->buf;
	return ctx->pc;
    }

    size = pc - ctx->buf;
    if (size) {
	/* Send the entire buffer as an X request */
	LockDisplay(dpy);
	GetReq(GLXRender,req); 
	req->reqType = ctx->majorOpcode;
	req->glxCode = X_GLXRender; 
	req->contextTag = ctx->currentContextTag;
	req->length += (size + 3) >> 2;
	_XSend(dpy, (char *)ctx->buf, size);
	UnlockDisplay(dpy);
	SyncHandle();
    }

    /* Reset pointer and return it */
    ctx->pc = ctx->buf;
    return ctx->pc;
}

/**
 * Send a command that is too large for the GLXRender protocol request.
 * 
 * Send a large command, one that is too large for some reason to
 * send using the GLXRender protocol request.  One reason to send
 * a large command is to avoid copying the data.
 * 
 * \param ctx        GLX context
 * \param header     Header data.
 * \param headerLen  Size, in bytes, of the header data.  It is assumed that
 *                   the header data will always be small enough to fit in
 *                   a single X protocol packet.
 * \param data       Command data.
 * \param dataLen    Size, in bytes, of the command data.
 */
void __glXSendLargeCommand(__GLXcontext *ctx,
			   const GLvoid *header, GLint headerLen,
			   const GLvoid *data, GLint dataLen)
{
    Display *dpy = ctx->currentDpy;
    xGLXRenderLargeReq *req;
    GLint maxSize, amount;
    GLint totalRequests, requestNumber;

    /*
    ** Calculate the maximum amount of data can be stuffed into a single
    ** packet.  sz_xGLXRenderReq is added because bufSize is the maximum
    ** packet size minus sz_xGLXRenderReq.
    */
    maxSize = (ctx->bufSize + sz_xGLXRenderReq) - sz_xGLXRenderLargeReq;
    totalRequests = 1 + (dataLen / maxSize);
    if (dataLen % maxSize) totalRequests++;

    /*
    ** Send all of the command, except the large array, as one request.
    */
    assert( headerLen <= maxSize );
    LockDisplay(dpy);
    GetReq(GLXRenderLarge,req); 
    req->reqType = ctx->majorOpcode;
    req->glxCode = X_GLXRenderLarge; 
    req->contextTag = ctx->currentContextTag;
    req->length += (headerLen + 3) >> 2;
    req->requestNumber = 1;
    req->requestTotal = totalRequests;
    req->dataBytes = headerLen;
    Data(dpy, (const void *)header, headerLen);

    /*
    ** Send enough requests until the whole array is sent.
    */
    requestNumber = 2;
    while (dataLen > 0) {
	amount = dataLen;
	if (amount > maxSize) {
	    amount = maxSize;
	}
	GetReq(GLXRenderLarge,req); 
	req->reqType = ctx->majorOpcode;
	req->glxCode = X_GLXRenderLarge; 
	req->contextTag = ctx->currentContextTag;
	req->length += (amount + 3) >> 2;
	req->requestNumber = requestNumber++;
	req->requestTotal = totalRequests;
	req->dataBytes = amount;
	Data(dpy, (const void *)data, amount);
	dataLen -= amount;
	data = ((const char*) data) + amount;
    }
    UnlockDisplay(dpy);
    SyncHandle();
}

/************************************************************************/

GLXContext glXGetCurrentContext(void)
{
    GLXContext cx = __glXGetCurrentContext();
    
    if (cx == &dummyContext) {
	return NULL;
    } else {
	return cx;
    }
}

GLXDrawable glXGetCurrentDrawable(void)
{
    GLXContext gc = __glXGetCurrentContext();
    return gc->currentDrawable;
}


/************************************************************************/

#ifdef GLX_DIRECT_RENDERING
/* Return the DRI per screen structure */
__DRIscreen *__glXFindDRIScreen(Display *dpy, int scrn)
{
    __DRIscreen *pDRIScreen = NULL;
    XExtDisplayInfo *info = __glXFindDisplay(dpy);
    XExtData **privList, *found;
    __GLXdisplayPrivate *dpyPriv;
    XEDataObject dataObj;

    __glXLock();
    dataObj.display = dpy;
    privList = XEHeadOfExtensionList(dataObj);
    found = XFindOnExtensionList(privList, info->codes->extension);
    __glXUnlock();

    if (found) {
	dpyPriv = (__GLXdisplayPrivate *)found->private_data;
	pDRIScreen = &dpyPriv->screenConfigs[scrn].driScreen;
    }

    return pDRIScreen;
}
#endif

/************************************************************************/

static Bool SendMakeCurrentRequest( Display *dpy, CARD8 opcode,
    GLXContextID gc, GLXContextTag old_gc, GLXDrawable draw, GLXDrawable read,
    xGLXMakeCurrentReply * reply );

static Bool SendMakeCurrentRequest( Display *dpy, CARD8 opcode,
				    GLXContextID gc_id, GLXContextTag gc_tag,
				    GLXDrawable draw, GLXDrawable read,
				    xGLXMakeCurrentReply * reply )
{
    opcode = __glXSetupForCommand(dpy);
    if (!opcode) {
	return GL_FALSE;
    }

    LockDisplay(dpy);
    if ( draw == read ) {
	xGLXMakeCurrentReq *req;

	GetReq(GLXMakeCurrent,req);
	req->reqType = opcode;
	req->glxCode = X_GLXMakeCurrent;
	req->drawable = draw;
	req->context = gc_id;
	req->oldContextTag = gc_tag;
    }
    else {
	__GLXdisplayPrivate *priv = __glXInitialize(dpy);

	/* If the server can support the GLX 1.3 version, we should
	 * perfer that.  Not only that, some servers support GLX 1.3 but
	 * not the SGI extension.
	 */

	if ( (priv->majorVersion > 1) || (priv->minorVersion >= 3) ) {
	    xGLXMakeContextCurrentReq *req;

	    GetReq(GLXMakeContextCurrent,req);
	    req->reqType = opcode;
	    req->glxCode = X_GLXMakeContextCurrent;
	    req->drawable = draw;
	    req->readdrawable = read;
	    req->context = gc_id;
	    req->oldContextTag = gc_tag;
	}
	else {
	    xGLXVendorPrivateWithReplyReq *vpreq;
	    xGLXMakeCurrentReadSGIReq *req;

	    GetReqExtra(GLXVendorPrivateWithReply,
			sz_xGLXMakeCurrentReadSGIReq-sz_xGLXVendorPrivateWithReplyReq,vpreq);
	    req = (xGLXMakeCurrentReadSGIReq *)vpreq;
	    req->reqType = opcode;
	    req->glxCode = X_GLXVendorPrivateWithReply;
	    req->vendorCode = X_GLXvop_MakeCurrentReadSGI;
	    req->drawable = draw;
	    req->readable = read;
	    req->context = gc_id;
	    req->oldContextTag = gc_tag;
	}
    }

    return _XReply(dpy, (xReply*) reply, 0, False);
}


/*
** Make a particular context current.
** NOTE: this is in this file so that it can access dummyContext.
*/
static Bool MakeContextCurrent(Display *dpy, 
			       GLXDrawable draw, GLXDrawable read,
			       GLXContext gc)
{
    xGLXMakeCurrentReply reply;
    GLXContext oldGC;
    CARD8 opcode, oldOpcode;
    Bool sentRequestToOldDpy = False;
    Bool bindReturnValue = True;

    opcode = __glXSetupForCommand(dpy);
    if (!opcode) {
	return GL_FALSE;
    }

    /*
    ** Make sure that the new context has a nonzero ID.  In the request,
    ** a zero context ID is used only to mean that we bind to no current
    ** context.
    */
    if ((gc != NULL) && (gc->xid == None)) {
	return GL_FALSE;
    }

    oldGC = __glXGetCurrentContext();
    oldOpcode = (gc == oldGC) ? opcode : __glXSetupForCommand(dpy);
    if (!oldOpcode) {
	return GL_FALSE;
    }

    if ((dpy != oldGC->currentDpy || (gc && gc->isDirect)) &&
	!oldGC->isDirect && oldGC != &dummyContext) {
	/*
	** We are either switching from one dpy to another and have to
	** send a request to the previous dpy to unbind the previous
	** context, or we are switching away from a indirect context to
	** a direct context and have to send a request to the dpy to
	** unbind the previous context.
	*/
	sentRequestToOldDpy = True;
	if ( ! SendMakeCurrentRequest( oldGC->currentDpy, oldOpcode, None,
				       oldGC->currentContextTag, None, None,
				       &reply ) ) {
	    /* The make current failed.  Just return GL_FALSE. */
	    UnlockDisplay(dpy);
	    SyncHandle();
	    return GL_FALSE;
	}

	oldGC->currentContextTag = 0;
    }
    
#ifdef GLX_DIRECT_RENDERING
    /* Unbind the old direct rendering context */
    if (oldGC->isDirect) {
	if (oldGC->driContext.private) {
	    if (!(*oldGC->driContext.unbindContext2)(oldGC->currentDpy,
						     oldGC->screen,
						     oldGC->currentDrawable,
						     oldGC->currentReadable,
						     oldGC)) {
		/* The make current failed.  Just return GL_FALSE. */
		return GL_FALSE;
	    }
	}
	oldGC->currentContextTag = 0;
    }

    /* Bind the direct rendering context to the drawable */
    if (gc && gc->isDirect) {
	if (gc->driContext.private) {
	    bindReturnValue =
		(*gc->driContext.bindContext2)(dpy, gc->screen, draw, read, gc);
	}
    } else {
#endif
        _glapi_check_multithread();
	/* Send a glXMakeCurrent request to bind the new context. */
	LockDisplay(dpy);

	bindReturnValue = SendMakeCurrentRequest( dpy, opcode, 
						  gc ? gc->xid : None,
						  oldGC->currentContextTag,
						  draw, read, &reply );
#ifdef GLX_DIRECT_RENDERING
    }
#endif


    if (!bindReturnValue) {
	/* The make current failed. */
	if (gc && !gc->isDirect) {
	    SyncHandle();
	}

#ifdef GLX_DIRECT_RENDERING
	/* If the old context was direct rendering, then re-bind to it. */
	if (oldGC->isDirect) {
	    if (oldGC->driContext.private) {
		if (!(*oldGC->driContext.bindContext2)(oldGC->currentDpy,
						       oldGC->screen,
						       oldGC->currentDrawable,
						       oldGC->currentReadable,
						       oldGC)) {
		    /*
		    ** The request failed; this cannot happen with the
		    ** current API.  If in the future the API is
		    ** extended to allow context sharing between
		    ** clients, then this may fail (because another
		    ** client may have grabbed the context); in that
		    ** case, we cannot undo the previous request, and
		    ** cannot adhere to the "no-op" behavior.
		    */
		}
	    }
	} else
#endif
	/*
	** If we had just sent a request to a previous dpy, we have to
	** undo that request (because if a command fails, it should act
	** like a no-op) by making current to the previous context and
	** drawable.
	*/
	if (sentRequestToOldDpy) {
	    if ( !SendMakeCurrentRequest( oldGC->currentDpy, oldOpcode,
					  oldGC->xid, 0, 
					  oldGC->currentDrawable,
					  oldGC->currentReadable, &reply ) ) {
		UnlockDisplay(dpy);
		SyncHandle();
		/*
		** The request failed; this cannot happen with the
		** current API.  If in the future the API is extended to
		** allow context sharing between clients, then this may
		** fail (because another client may have grabbed the
		** context); in that case, we cannot undo the previous
		** request, and cannot adhere to the "no-op" behavior.
		*/
	    }
            else {
		UnlockDisplay(dpy);
            }
	    oldGC->currentContextTag = reply.contextTag;
	}
	return GL_FALSE;
    }

    /* Update our notion of what is current */
    __glXLock();
    if (gc == oldGC) {
	/*
	** Even though the contexts are the same the drawable might have
	** changed.  Note that gc cannot be the dummy, and that oldGC
	** cannot be NULL, therefore if they are the same, gc is not
	** NULL and not the dummy.
	*/
	gc->currentDrawable = draw;
	gc->currentReadable = read;
    } else {
	if (oldGC != &dummyContext) {
	    /* Old current context is no longer current to anybody */
	    oldGC->currentDpy = 0;
	    oldGC->currentDrawable = None;
	    oldGC->currentReadable = None;
	    oldGC->currentContextTag = 0;

	    if (oldGC->xid == None) {
		/* 
		** We are switching away from a context that was
		** previously destroyed, so we need to free the memory
		** for the old handle.
		*/
#ifdef GLX_DIRECT_RENDERING
		/* Destroy the old direct rendering context */
		if (oldGC->isDirect) {
		    if (oldGC->driContext.private) {
			(*oldGC->driContext.destroyContext)
			    (dpy, oldGC->screen, oldGC->driContext.private);
			oldGC->driContext.private = NULL;
		    }
		}
#endif
		__glXFreeContext(oldGC);
	    }
	}
	if (gc) {
	    __glXSetCurrentContext(gc);
#ifdef GLX_DIRECT_RENDERING
            if (!gc->isDirect) {
               if (!IndirectAPI)
                  IndirectAPI = __glXNewIndirectAPI();
               _glapi_set_dispatch(IndirectAPI);
# ifdef GLX_USE_APPLEGL
               do {
                   extern void XAppleDRIUseIndirectDispatch(void);
                   XAppleDRIUseIndirectDispatch();
               } while (0);
# endif
            }
#else
            /* if not direct rendering, always need indirect dispatch */
            if (!IndirectAPI)
               IndirectAPI = __glXNewIndirectAPI();
            _glapi_set_dispatch(IndirectAPI);
#endif
	    gc->currentDpy = dpy;
	    gc->currentDrawable = draw;
	    gc->currentReadable = read;
#ifdef GLX_DIRECT_RENDERING
	    if (gc->isDirect) reply.contextTag = -1;
#endif
	    gc->currentContextTag = reply.contextTag;
	} else {
	    __glXSetCurrentContext(&dummyContext);
#ifdef GLX_DIRECT_RENDERING
            _glapi_set_dispatch(NULL);  /* no-op functions */
#endif
	}
    }
    __glXUnlock();
    return GL_TRUE;
}


Bool GLX_PREFIX(glXMakeCurrent)(Display *dpy, GLXDrawable draw, GLXContext gc)
{
    return MakeContextCurrent( dpy, draw, draw, gc );
}

GLX_ALIAS(Bool, glXMakeCurrentReadSGI,
	  (Display *dpy, GLXDrawable d, GLXDrawable r, GLXContext ctx),
	  (dpy, d, r, ctx), MakeContextCurrent)

GLX_ALIAS(Bool, glXMakeContextCurrent,
	  (Display *dpy, GLXDrawable d, GLXDrawable r, GLXContext ctx),
	  (dpy, d, r, ctx), MakeContextCurrent)


#ifdef DEBUG
void __glXDumpDrawBuffer(__GLXcontext *ctx)
{
    GLubyte *p = ctx->buf;
    GLubyte *end = ctx->pc;
    GLushort opcode, length;

    while (p < end) {
	/* Fetch opcode */
	opcode = *((GLushort*) p);
	length = *((GLushort*) (p + 2));
	printf("%2x: %5d: ", opcode, length);
	length -= 4;
	p += 4;
	while (length > 0) {
	    printf("%08x ", *((unsigned *) p));
	    p += 4;
	    length -= 4;
	}
	printf("\n");
    }	    
}
#endif

#ifdef  USE_SPARC_ASM
/*
 * Used only when we are sparc, using sparc assembler.
 *
 */

static void
_glx_mesa_init_sparc_glapi_relocs(void)
{
	unsigned int *insn_ptr, *end_ptr;
	unsigned long disp_addr;

	insn_ptr = &_mesa_sparc_glapi_begin;
	end_ptr = &_mesa_sparc_glapi_end;
	disp_addr = (unsigned long) &_glapi_Dispatch;

	/*
         * Verbatim from Mesa sparc.c.  It's needed because there doesn't
         * seem to be a better way to do this:
         *
         * UNCONDITIONAL_JUMP ( (*_glapi_Dispatch) + entry_offset )
         *
         * This code is patching in the ADDRESS of the pointer to the
         * dispatch table.  Hence, it must be called exactly once, because
         * that address is not going to change.
         *
         * What it points to can change, but Mesa (and hence, we) assume
         * that there is only one pointer.
         *
	 */
	while (insn_ptr < end_ptr) {
#if ( defined(__sparc_v9__) && ( !defined(__linux__) || defined(__linux_64__) ) )	
/*
	This code patches for 64-bit addresses.  This had better
	not happen for Sparc/Linux, no matter what architecture we
	are building for.  So, don't do this.

        The 'defined(__linux_64__)' is used here as a placeholder for
        when we do do 64-bit usermode on sparc linux.
	*/
		insn_ptr[0] |= (disp_addr >> (32 + 10));
		insn_ptr[1] |= ((disp_addr & 0xffffffff) >> 10);
		__glapi_sparc_icache_flush(&insn_ptr[0]);
		insn_ptr[2] |= ((disp_addr >> 32) & ((1 << 10) - 1));
		insn_ptr[3] |= (disp_addr & ((1 << 10) - 1));
		__glapi_sparc_icache_flush(&insn_ptr[2]);
		insn_ptr += 11;
#else
		insn_ptr[0] |= (disp_addr >> 10);
		insn_ptr[1] |= (disp_addr & ((1 << 10) - 1));
		__glapi_sparc_icache_flush(&insn_ptr[0]);
		insn_ptr += 5;
#endif
	}
}
#endif  /* sparc ASM in use */

