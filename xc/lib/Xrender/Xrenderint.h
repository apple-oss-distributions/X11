/*
 * $XFree86: xc/lib/Xrender/Xrenderint.h,v 1.4 2002/11/05 23:22:35 keithp Exp $
 *
 * Copyright ? 2000 SuSE, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of SuSE not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  SuSE makes no representations about the
 * suitability of this software for any purpose.  It is provided "as is"
 * without express or implied warranty.
 *
 * SuSE DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE, INCLUDING ALL
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO EVENT SHALL SuSE
 * BE LIABLE FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN 
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Author:  Keith Packard, SuSE, Inc.
 */

#ifndef _XRENDERINT_H_
#define _XRENDERINT_H_

#define NEED_EVENTS
#define NEED_REPLIES
#include <X11/Xlibint.h>
#include <X11/Xutil.h>
#include "Xext.h"			/* in ../include */
#include "extutil.h"			/* in ../include */
#include "Xrender.h"
#include "renderproto.h"

typedef struct {
    Visual		*visual;
    XRenderPictFormat	*format;
} XRenderVisual;

typedef struct {
    int			depth;
    int			nvisuals;
    XRenderVisual	*visuals;
} XRenderDepth;

typedef struct {
    XRenderDepth	*depths;
    int			ndepths;
    XRenderPictFormat	*fallback;
    int			subpixel;
} XRenderScreen;

typedef struct _XRenderInfo {
    int			major_version;
    int			minor_version;
    XRenderPictFormat	*format;
    int			nformat;
    XRenderScreen	*screen;
    int			nscreen;
    XRenderDepth	*depth;
    int			ndepth;
    XRenderVisual	*visual;
    int			nvisual;
    int			*subpixel;
    int			nsubpixel;
    char		**filter;
    int			nfilter;
    short    		*filter_alias;
    int			nfilter_alias;
} XRenderInfo;

extern XExtensionInfo XRenderExtensionInfo;
extern char XRenderExtensionName[];

#define RenderCheckExtension(dpy,i,val) \
  XextCheckExtension (dpy, i, XRenderExtensionName, val)

#define RenderSimpleCheckExtension(dpy,i) \
  XextSimpleCheckExtension (dpy, i, XRenderExtensionName)

XExtDisplayInfo *
XRenderFindDisplay (Display *dpy);

/*
 * Xlib uses long for 32-bit values.  Xrender uses int.  This
 * matters on alpha.  Note that this macro assumes that int is 32 bits
 * except on WORD64 machines where it is 64 bits.
 */

#ifdef WORD64
#define DataInt32(dpy,d,len)	Data32(dpy,(long *) (d),len)
#else
#define DataInt32(dpy,d,len)	Data(dpy,(char *) (d),len)
#endif

#endif /* _XRENDERINT_H_ */
