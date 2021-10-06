/* $XFree86: xc/programs/Xserver/hw/xfree86/common/xf86VidMode.c,v 1.18 2004/02/13 23:58:38 dawes Exp $ */
/*
 * Copyright (c) 1999-2003 by The XFree86 Project, Inc.
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject
 * to the following conditions:
 *
 *   1.  Redistributions of source code must retain the above copyright
 *       notice, this list of conditions, and the following disclaimer.
 *
 *   2.  Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer
 *       in the documentation and/or other materials provided with the
 *       distribution, and in the same place and form as other copyright,
 *       license and disclaimer information.
 *
 *   3.  The end-user documentation included with the redistribution,
 *       if any, must include the following acknowledgment: "This product
 *       includes software developed by The XFree86 Project, Inc
 *       (http://www.xfree86.org/) and its contributors", in the same
 *       place and form as other third-party acknowledgments.  Alternately,
 *       this acknowledgment may appear in the software itself, in the
 *       same form and location as other such third-party acknowledgments.
 *
 *   4.  Except as contained in this notice, the name of The XFree86
 *       Project, Inc shall not be used in advertising or otherwise to
 *       promote the sale, use or other dealings in this Software without
 *       prior written authorization from The XFree86 Project, Inc.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE XFREE86 PROJECT, INC OR ITS CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY,
 * OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * This file contains the VidMode functions required by the extension.
 * These have been added to avoid the need for the higher level extension
 * code to access the private XFree86 data structures directly. Wherever
 * possible this code uses the functions in xf86Mode.c to do the work,
 * so that two version of code that do similar things don't have to be
 * maintained.
 */

#include "X.h"
#include "os.h"
#include "xf86.h"
#include "xf86Priv.h"

#ifdef XF86VIDMODE
#include "vidmodeproc.h"
#include "xf86cmap.h"

static int VidModeGeneration = 0;
static int VidModeIndex = -1;
static int VidModeCount = 0;
static Bool VidModeClose(int i, ScreenPtr pScreen);

#define VMPTR(p) ((VidModePtr)(p)->devPrivates[VidModeIndex].ptr)

#endif

#ifdef DEBUG
# define DEBUG_P(x) ErrorF(x"\n");
#else
# define DEBUG_P(x) /**/
#endif

Bool
VidModeExtensionInit(ScreenPtr pScreen)
{
#ifdef XF86VIDMODE
    VidModePtr pVidMode;
    
    DEBUG_P("VidModeExtensionInit");

    if (!xf86GetVidModeEnabled()) {
	DEBUG_P("!xf86GetVidModeEnabled()");
	return FALSE;
    }

    if (serverGeneration != VidModeGeneration) {
	if ((VidModeIndex = AllocateScreenPrivateIndex()) < 0) {
	    DEBUG_P("AllocateScreenPrivateIndex() failed");
	    return FALSE;
	}
	VidModeGeneration = serverGeneration;
    }

    if (!(pScreen->devPrivates[VidModeIndex].ptr = xcalloc(sizeof(VidModeRec), 1))) {
	DEBUG_P("xcalloc failed");
	return FALSE;
    }

    pVidMode = VMPTR(pScreen);
    pVidMode->Flags = 0;
    pVidMode->Next = NULL;
    pVidMode->CloseScreen = pScreen->CloseScreen;
    pScreen->CloseScreen = VidModeClose;
    VidModeCount++;
    return TRUE;
#else
    DEBUG_P("no vidmode extension");
    return FALSE;
#endif
}


#ifdef XF86VIDMODE

static Bool
VidModeClose(int i, ScreenPtr pScreen)
{
    VidModePtr pVidMode = VMPTR(pScreen);

    DEBUG_P("VidModeClose");

    /* This shouldn't happen */
    if (!pVidMode)
	return FALSE;

    pScreen->CloseScreen = pVidMode->CloseScreen;

    if (--VidModeCount == 0) {
	if (pScreen->devPrivates[VidModeIndex].ptr)
	  xfree(pScreen->devPrivates[VidModeIndex].ptr);
	pScreen->devPrivates[VidModeIndex].ptr = NULL;
	VidModeIndex = -1;
    }
    return pScreen->CloseScreen(i, pScreen);
}

Bool
VidModeAvailable(int scrnIndex)
{
    ScrnInfoPtr pScrn;
    VidModePtr pVidMode;

    DEBUG_P("VidModeAvailable");

    if (VidModeIndex < 0) {
	DEBUG_P("VidModeIndex < 0");
	return FALSE;
    }
 
    pScrn = xf86Screens[scrnIndex];
    if (pScrn == NULL) {
	DEBUG_P("pScrn == NULL");
	return FALSE;
    }
    
    pVidMode = VMPTR(pScrn->pScreen);
    if (pVidMode)
	return TRUE;
    else {
	DEBUG_P("pVidMode == NULL");
	return FALSE;
    }
}

Bool
VidModeGetCurrentModeline(int scrnIndex, pointer *mode, int *dotClock)
{
    ScrnInfoPtr pScrn;

    DEBUG_P("VidModeGetCurrentModeline");

    if (!VidModeAvailable(scrnIndex))
	return FALSE;

    pScrn = xf86Screens[scrnIndex];
    *mode = (pointer)(pScrn->currentMode);
    *dotClock = pScrn->currentMode->Clock;

    return TRUE;
}

int
VidModeGetDotClock(int scrnIndex, int Clock)
{
    ScrnInfoPtr pScrn;

    DEBUG_P("VidModeGetDotClock");

    if (!VidModeAvailable(scrnIndex))
	return 0;

    pScrn = xf86Screens[scrnIndex];
    if ((pScrn->progClock) || (Clock > MAXCLOCKS))
	return Clock;
    else  
	return pScrn->clock[Clock];
}

int
VidModeGetNumOfClocks(int scrnIndex, Bool *progClock)
{
    ScrnInfoPtr pScrn;

    DEBUG_P("VidModeGetNumOfClocks");

    if (!VidModeAvailable(scrnIndex))
	return 0;

    pScrn = xf86Screens[scrnIndex];
    if (pScrn->progClock){
	*progClock = TRUE;
	return 0;
    } else {
	*progClock = FALSE;
	return pScrn->numClocks;
    }
}

Bool
VidModeGetClocks(int scrnIndex, int *Clocks)
{
    ScrnInfoPtr pScrn;
    int i;

    DEBUG_P("VidModeGetClocks");

    if (!VidModeAvailable(scrnIndex))
	return FALSE;

    pScrn = xf86Screens[scrnIndex];

    if (pScrn->progClock)
	return FALSE;

    for (i = 0;  i < pScrn->numClocks;  i++)
	*Clocks++ = pScrn->clock[i];

    return TRUE;
}


Bool
VidModeGetFirstModeline(int scrnIndex, pointer *mode, int *dotClock)
{
    ScrnInfoPtr pScrn;
    VidModePtr pVidMode;

    DEBUG_P("VidModeGetFirstModeline");

    if (!VidModeAvailable(scrnIndex))
	return FALSE;

    pScrn = xf86Screens[scrnIndex];
    pVidMode = VMPTR(pScrn->pScreen);
    pVidMode->First = pScrn->modes;
    pVidMode->Next =  pVidMode->First->next;

    if (pVidMode->First->status == MODE_OK) {
      *mode = (pointer)(pVidMode->First);
      *dotClock = VidModeGetDotClock(scrnIndex, pVidMode->First->Clock);
      return TRUE;
    }

    return VidModeGetNextModeline(scrnIndex, mode, dotClock);
}

Bool
VidModeGetNextModeline(int scrnIndex, pointer *mode, int *dotClock)
{
    ScrnInfoPtr pScrn;
    VidModePtr pVidMode;
    DisplayModePtr p;

    DEBUG_P("VidModeGetNextModeline");

    if (!VidModeAvailable(scrnIndex))
	return FALSE;

    pScrn = xf86Screens[scrnIndex];
    pVidMode = VMPTR(pScrn->pScreen);

    for (p = pVidMode->Next; p != NULL && p != pVidMode->First; p = p->next) {
	if (p->status == MODE_OK) {
	    pVidMode->Next = p->next;
	    *mode = (pointer)p;
	    *dotClock = VidModeGetDotClock(scrnIndex, p->Clock);
	    return TRUE;
	}
    }
    
    return FALSE;
}

Bool
VidModeDeleteModeline(int scrnIndex, pointer mode)
{
    ScrnInfoPtr pScrn;

    DEBUG_P("VidModeDeleteModeline");

    if ((mode == NULL) || (!VidModeAvailable(scrnIndex)))
	return FALSE;

    pScrn = xf86Screens[scrnIndex];
    xf86DeleteMode(&(pScrn->modes), (DisplayModePtr)mode);
    return TRUE;
}

Bool
VidModeZoomViewport(int scrnIndex, int zoom)
{
    ScrnInfoPtr pScrn;

    DEBUG_P("VidModeZoomViewPort");

    if (!VidModeAvailable(scrnIndex))
	return FALSE;

    pScrn = xf86Screens[scrnIndex];
    xf86ZoomViewport(pScrn->pScreen, zoom);
    return TRUE;
}

Bool
VidModeSetViewPort(int scrnIndex, int x, int y)
{
    ScrnInfoPtr pScrn;

    DEBUG_P("VidModeSetViewPort");

    if (!VidModeAvailable(scrnIndex))
	return FALSE;

    pScrn = xf86Screens[scrnIndex];
    pScrn->frameX0 = min( max(x, 0),
	                 pScrn->virtualX - pScrn->currentMode->HDisplay );
    pScrn->frameX1 = pScrn->frameX0 + pScrn->currentMode->HDisplay - 1;
    pScrn->frameY0 = min( max(y, 0),
	                 pScrn->virtualY - pScrn->currentMode->VDisplay );
    pScrn->frameY1 = pScrn->frameY0 + pScrn->currentMode->VDisplay - 1;
    if (pScrn->AdjustFrame != NULL)
	(pScrn->AdjustFrame)(scrnIndex, pScrn->frameX0, pScrn->frameY0, 0);

    return TRUE;
}

Bool
VidModeGetViewPort(int scrnIndex, int *x, int *y)
{
    ScrnInfoPtr pScrn;

    DEBUG_P("VidModeGetViewPort");

    if (!VidModeAvailable(scrnIndex))
	return FALSE;

    pScrn = xf86Screens[scrnIndex];
    *x = pScrn->frameX0;
    *y = pScrn->frameY0;
    return TRUE;
}

Bool
VidModeSwitchMode(int scrnIndex, pointer mode)
{
    ScrnInfoPtr pScrn;
    DisplayModePtr pTmpMode;
    Bool retval;

    DEBUG_P("VidModeSwitchMode");

    if (!VidModeAvailable(scrnIndex))
	return FALSE;

    pScrn = xf86Screens[scrnIndex];
    /* save in case we fail */
    pTmpMode = pScrn->currentMode;
    /* Force a mode switch */
    pScrn->currentMode = NULL;
    retval = xf86SwitchMode(pScrn->pScreen, mode);
    /* we failed: restore it */
    if (retval == FALSE)
	pScrn->currentMode = pTmpMode;
    return retval;
}

Bool
VidModeLockZoom(int scrnIndex, Bool lock)
{
    ScrnInfoPtr pScrn;

    DEBUG_P("VidModeLockZoom");

    if (!VidModeAvailable(scrnIndex))
	return FALSE;

    pScrn = xf86Screens[scrnIndex];

    if (xf86Info.dontZoom)
	return FALSE;

    xf86LockZoom(pScrn->pScreen, lock);
    return TRUE;
}

Bool
VidModeGetMonitor(int scrnIndex, pointer *monitor)
{
    ScrnInfoPtr pScrn;

    DEBUG_P("VidModeGetMonitor");

    if (!VidModeAvailable(scrnIndex))
	return FALSE;

    pScrn = xf86Screens[scrnIndex];
    *monitor = (pointer)(pScrn->monitor);

    return TRUE;
}

ModeStatus
VidModeCheckModeForMonitor(int scrnIndex, pointer mode)
{
    ScrnInfoPtr pScrn;

    DEBUG_P("VidModeCheckModeForMonitor");

    if ((mode == NULL) || (!VidModeAvailable(scrnIndex)))
	return MODE_ERROR;

    pScrn = xf86Screens[scrnIndex];

    return xf86CheckModeForMonitor((DisplayModePtr)mode, pScrn->monitor);
}

ModeStatus
VidModeCheckModeForDriver(int scrnIndex, pointer mode)
{
    ScrnInfoPtr pScrn;

    DEBUG_P("VidModeCheckModeForDriver");

    if ((mode == NULL) || (!VidModeAvailable(scrnIndex)))
	return MODE_ERROR;

    pScrn = xf86Screens[scrnIndex];

    return xf86CheckModeForDriver(pScrn, (DisplayModePtr)mode, 0);
}

void
VidModeSetCrtcForMode(int scrnIndex, pointer mode)
{
    ScrnInfoPtr pScrn;
    DisplayModePtr ScreenModes;
    
    DEBUG_P("VidModeSetCrtcForMode");

    if ((mode == NULL) || (!VidModeAvailable(scrnIndex)))
	return;

    /* Ugly hack so that the xf86Mode.c function can be used without change */
    pScrn = xf86Screens[scrnIndex];
    ScreenModes = pScrn->modes;
    pScrn->modes = (DisplayModePtr)mode;
    
    xf86SetCrtcForModes(pScrn, pScrn->adjustFlags);
    pScrn->modes = ScreenModes;
    return;
}

Bool
VidModeAddModeline(int scrnIndex, pointer mode)
{
    ScrnInfoPtr pScrn;
    
    DEBUG_P("VidModeAddModeline");

    if ((mode == NULL) || (!VidModeAvailable(scrnIndex)))
	return FALSE;

    pScrn = xf86Screens[scrnIndex];

    ((DisplayModePtr)mode)->name         = strdup(""); /* freed by deletemode */
    ((DisplayModePtr)mode)->status       = MODE_OK;
    ((DisplayModePtr)mode)->next         = pScrn->modes->next;
    ((DisplayModePtr)mode)->prev         = pScrn->modes;
    pScrn->modes->next                   = (DisplayModePtr)mode;
    if( ((DisplayModePtr)mode)->next != NULL )
      ((DisplayModePtr)mode)->next->prev   = (DisplayModePtr)mode;

    return TRUE;
}

int
VidModeGetNumOfModes(int scrnIndex)
{
    pointer mode = NULL;
    int dotClock= 0, nummodes = 0;
  
    DEBUG_P("VidModeGetNumOfModes");

    if (!VidModeGetFirstModeline(scrnIndex, &mode, &dotClock))
	return nummodes;

    do {
	nummodes++;
	if (!VidModeGetNextModeline(scrnIndex, &mode, &dotClock))
	    return nummodes;
    } while (TRUE);
}

Bool
VidModeSetGamma(int scrnIndex, float red, float green, float blue)
{
    ScrnInfoPtr pScrn;
    Gamma gamma;

    DEBUG_P("VidModeSetGamma");

    if (!VidModeAvailable(scrnIndex))
	return FALSE;

    pScrn = xf86Screens[scrnIndex];
    gamma.red = red;
    gamma.green = green;
    gamma.blue = blue;
    if (xf86ChangeGamma(pScrn->pScreen, gamma) != Success)
	return FALSE;
    else
	return TRUE;
}

Bool
VidModeGetGamma(int scrnIndex, float *red, float *green, float *blue)
{
    ScrnInfoPtr pScrn;

    DEBUG_P("VidModeGetGamma");

    if (!VidModeAvailable(scrnIndex))
	return FALSE;

    pScrn = xf86Screens[scrnIndex];
    *red = pScrn->gamma.red;
    *green = pScrn->gamma.green;
    *blue = pScrn->gamma.blue;
    return TRUE;
}

Bool
VidModeSetGammaRamp(int scrnIndex, int size, CARD16 *r, CARD16 *g, CARD16 *b)
{
    ScrnInfoPtr pScrn;

    if (!VidModeAvailable(scrnIndex))
        return FALSE;
 
    pScrn = xf86Screens[scrnIndex];
    xf86ChangeGammaRamp(pScrn->pScreen, size, r, g, b);
    return TRUE;
}

Bool
VidModeGetGammaRamp(int scrnIndex, int size, CARD16 *r, CARD16 *g, CARD16 *b)
{
    ScrnInfoPtr pScrn;

    if (!VidModeAvailable(scrnIndex))
        return FALSE;

    pScrn = xf86Screens[scrnIndex];
    xf86GetGammaRamp(pScrn->pScreen, size, r, g, b);
    return TRUE;
}

int
VidModeGetGammaRampSize(int scrnIndex)
{
    if (!VidModeAvailable(scrnIndex))
        return 0;

    return xf86GetGammaRampSize(xf86Screens[scrnIndex]->pScreen);
}

pointer
VidModeCreateMode(void)
{
    DisplayModePtr mode;
  
    mode = xalloc(sizeof(DisplayModeRec));
    if (mode != NULL) {
	mode->name          = "";
	mode->VScan         = 1;    /* divides refresh rate. default = 1 */
	mode->Private       = NULL;
	mode->next          = mode;
	mode->prev          = mode;
    }
    return mode;
}

void
VidModeCopyMode(pointer modefrom, pointer modeto)
{
  memcpy(modeto, modefrom, sizeof(DisplayModeRec));
}


int
VidModeGetModeValue(pointer mode, int valtyp)
{
  int ret = 0;
  
  switch (valtyp) {
    case VIDMODE_H_DISPLAY:
	ret = ((DisplayModePtr) mode)->HDisplay;
	break;
    case VIDMODE_H_SYNCSTART:
	ret = ((DisplayModePtr)mode)->HSyncStart;
	break;
    case VIDMODE_H_SYNCEND:
	ret = ((DisplayModePtr)mode)->HSyncEnd;
	break;
    case VIDMODE_H_TOTAL:
	ret = ((DisplayModePtr)mode)->HTotal;
	break;
    case VIDMODE_H_SKEW:
	ret = ((DisplayModePtr)mode)->HSkew;
	break;
    case VIDMODE_V_DISPLAY:
	ret = ((DisplayModePtr)mode)->VDisplay;
	break;
    case VIDMODE_V_SYNCSTART:
	ret = ((DisplayModePtr)mode)->VSyncStart;
	break;
    case VIDMODE_V_SYNCEND:
	ret = ((DisplayModePtr)mode)->VSyncEnd;
	break;
    case VIDMODE_V_TOTAL:
	ret = ((DisplayModePtr)mode)->VTotal;
	break;
    case VIDMODE_FLAGS:
	ret = ((DisplayModePtr)mode)->Flags;
	break;
    case VIDMODE_CLOCK:
	ret = ((DisplayModePtr)mode)->Clock;
	break;
  }
  return ret;
}

void
VidModeSetModeValue(pointer mode, int valtyp, int val)
{
  switch (valtyp) {
    case VIDMODE_H_DISPLAY:
	((DisplayModePtr)mode)->HDisplay = val;
	break;
    case VIDMODE_H_SYNCSTART:
	((DisplayModePtr)mode)->HSyncStart = val;
	break;
    case VIDMODE_H_SYNCEND:
	((DisplayModePtr)mode)->HSyncEnd = val;
	break;
    case VIDMODE_H_TOTAL:
	((DisplayModePtr)mode)->HTotal = val;
	break;
    case VIDMODE_H_SKEW:
	((DisplayModePtr)mode)->HSkew = val;
	break;
    case VIDMODE_V_DISPLAY:
	((DisplayModePtr)mode)->VDisplay = val;
	break;
    case VIDMODE_V_SYNCSTART:
	((DisplayModePtr)mode)->VSyncStart = val;
	break;
    case VIDMODE_V_SYNCEND:
	((DisplayModePtr)mode)->VSyncEnd = val;
	break;
    case VIDMODE_V_TOTAL:
	((DisplayModePtr)mode)->VTotal = val;
	break;
    case VIDMODE_FLAGS:
	((DisplayModePtr)mode)->Flags = val;
	break;
    case VIDMODE_CLOCK:
	((DisplayModePtr)mode)->Clock = val;
	break;
  }
  return;
}

vidMonitorValue
VidModeGetMonitorValue(pointer monitor, int valtyp, int indx)
{
  vidMonitorValue ret;
  
  switch (valtyp) {
    case VIDMODE_MON_VENDOR:
	ret.ptr = (((MonPtr)monitor)->vendor);
	break;
    case VIDMODE_MON_MODEL:
	ret.ptr = (((MonPtr)monitor)->model);
	break;
    case VIDMODE_MON_NHSYNC:
	ret.i = ((MonPtr)monitor)->nHsync;
	break;
    case VIDMODE_MON_NVREFRESH:
	ret.i = ((MonPtr)monitor)->nVrefresh;
	break;
    case VIDMODE_MON_HSYNC_LO:
	ret.f = (100.0 * ((MonPtr)monitor)->hsync[indx].lo);
	break;
    case VIDMODE_MON_HSYNC_HI:
	ret.f = (100.0 * ((MonPtr)monitor)->hsync[indx].hi);
	break;
    case VIDMODE_MON_VREFRESH_LO:
	ret.f = (100.0 * ((MonPtr)monitor)->vrefresh[indx].lo);
	break;
    case VIDMODE_MON_VREFRESH_HI:
	ret.f = (100.0 * ((MonPtr)monitor)->vrefresh[indx].hi);
	break;
  }
  return ret;
}


#endif /* XF86VIDMODE */
