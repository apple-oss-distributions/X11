/* $Xorg: XChgDCtl.c,v 1.4 2001/02/09 02:03:50 xorgcvs Exp $ */

/************************************************************

Copyright 1989, 1998  The Open Group

Permission to use, copy, modify, distribute, and sell this software and its
documentation for any purpose is hereby granted without fee, provided that
the above copyright notice appear in all copies and that both that
copyright notice and this permission notice appear in supporting
documentation.

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
OPEN GROUP BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

Except as contained in this notice, the name of The Open Group shall not be
used in advertising or otherwise to promote the sale, use or other dealings
in this Software without prior written authorization from The Open Group.

Copyright 1989 by Hewlett-Packard Company, Palo Alto, California.

			All Rights Reserved

Permission to use, copy, modify, and distribute this software and its
documentation for any purpose and without fee is hereby granted,
provided that the above copyright notice appear in all copies and that
both that copyright notice and this permission notice appear in
supporting documentation, and that the name of Hewlett-Packard not be
used in advertising or publicity pertaining to distribution of the
software without specific, written prior permission.

HEWLETT-PACKARD DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE, INCLUDING
ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO EVENT SHALL
HEWLETT-PACKARD BE LIABLE FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR
ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
SOFTWARE.

********************************************************/
/* $XFree86: xc/lib/Xi/XChgDCtl.c,v 3.4 2002/10/16 00:37:28 dawes Exp $ */

/***********************************************************************
 *
 * XChangeDeviceControl - Change the control attributes of an extension
 * input device.
 *
 */

#include <X11/extensions/XI.h>
#include <X11/extensions/XIproto.h>
#include <X11/Xlibint.h>
#include <X11/extensions/XInput.h>
#include <X11/extensions/extutil.h>
#include "XIint.h"

int
XChangeDeviceControl (dpy, dev, control, d)
    register	Display 	*dpy;
    XDevice			*dev;
    int				control;
    XDeviceControl		*d;
    {
    int length;
    xChangeDeviceControlReq	*req;
    xChangeDeviceControlReply	rep;
    XExtDisplayInfo *info = XInput_find_display (dpy);

    LockDisplay (dpy);
    if (_XiCheckExtInit(dpy, XInput_Add_XChangeDeviceControl) == -1)
	return (NoSuchExtension);

    GetReq(ChangeDeviceControl,req);
    req->reqType = info->codes->major_opcode;
    req->ReqType = X_ChangeDeviceControl;
    req->deviceid = dev->device_id;
    req->control = control;

    switch (control)
	{
	case DEVICE_RESOLUTION:
	    {
	    XDeviceResolutionControl	*R;
	    xDeviceResolutionCtl	r;

	    R = (XDeviceResolutionControl *) d;
	    r.control = DEVICE_RESOLUTION;
	    r.length = sizeof (xDeviceResolutionCtl) + 
		R->num_valuators * sizeof(int);
	    r.first_valuator = R->first_valuator;
	    r.num_valuators = R->num_valuators;
	    req->length += ((unsigned)(r.length + 3) >> 2);
	    length = sizeof (xDeviceResolutionCtl);
	    Data (dpy, (char *) &r, length);
	    length  = r.num_valuators * sizeof(int);
	    Data (dpy, (char *) R->resolutions, length);
	    if (! _XReply (dpy, (xReply *) &rep, 0, xTrue)) 
		{
		UnlockDisplay(dpy);
		SyncHandle();
		return (NoSuchExtension);
		}
	    else
		return (rep.status);
	    }
	default:
	    {
	    xDeviceCtl		u;

	    u.control = d->control;
	    u.length = d->length - sizeof (int);
	    length = ((unsigned)(u.length + 3) >> 2);
	    req->length += length;
	    length <<= 2;
	    Data (dpy, (char *) &u, length);
	    }
	}

    UnlockDisplay(dpy);
    SyncHandle();
    return (Success);
    }

