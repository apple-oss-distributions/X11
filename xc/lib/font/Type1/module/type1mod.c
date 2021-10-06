/*
 * Copyright (C) 1998 The XFree86 Project, Inc.
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
/* $XFree86: xc/lib/font/Type1/module/type1mod.c,v 1.11 2004/02/13 23:58:29 dawes Exp $ */

#include "misc.h"

#include "fontmod.h"
#include "xf86Module.h"

static MODULESETUPPROTO(type1Setup);

    /*
     * This is the module data function that is accessed when loading 
     * libtype1 as a module.
     */

static XF86ModuleVersionInfo VersRec =
{
	"type1",
	MODULEVENDORSTRING,
	MODINFOSTRING1,
	MODINFOSTRING2,
	XF86_VERSION_CURRENT,
	1, 0, 2,
	ABI_CLASS_FONT,			/* Font module */
	ABI_FONT_VERSION,
	MOD_CLASS_FONT,
	{0,0,0,0}       /* signature, to be patched into the file by a tool */
};

XF86ModuleData type1ModuleData = { &VersRec, type1Setup, NULL };

extern void Type1RegisterFontFileFunctions(void);
#ifdef BUILDCID
extern void CIDRegisterFontFileFunctions(void);
#endif

FontModule type1Module = {
    Type1RegisterFontFileFunctions,
    "Type1",
    NULL
};

#ifdef BUILDCID
FontModule CIDModule = {
    CIDRegisterFontFileFunctions,
    "CID",
    NULL
};
#endif

static pointer
type1Setup(pointer module, pointer opts, int *errmaj, int *errmin)
{
    type1Module.module = module;
    LoadFont(&type1Module);
#ifdef BUILDCID
    CIDModule.module = module;
    LoadFont(&CIDModule);
#endif

    /* Need a non-NULL return */
    return (pointer)1;
}
