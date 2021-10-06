/* $XFree86: xc/lib/GL/mesa/src/drv/radeon/radeon_texstate.c,v 1.9 2004/01/23 03:57:06 dawes Exp $ */
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
 */

#include "glheader.h"
#include "imports.h"
#include "colormac.h"
#include "context.h"
#include "macros.h"
#include "texformat.h"
#include "enums.h"

#include "radeon_context.h"
#include "radeon_state.h"
#include "radeon_ioctl.h"
#include "radeon_swtcl.h"
#include "radeon_tex.h"
#include "radeon_tcl.h"


#define RADEON_TXFORMAT_AL88      RADEON_TXFORMAT_AI88
#define RADEON_TXFORMAT_YCBCR     RADEON_TXFORMAT_YVYU422
#define RADEON_TXFORMAT_YCBCR_REV RADEON_TXFORMAT_VYUY422

#define _COLOR(f) \
    [ MESA_FORMAT_ ## f ] = { RADEON_TXFORMAT_ ## f, 0 }
#define _ALPHA(f) \
    [ MESA_FORMAT_ ## f ] = { RADEON_TXFORMAT_ ## f | RADEON_TXFORMAT_ALPHA_IN_MAP, 0 }
#define _YUV(f) \
   [ MESA_FORMAT_ ## f ] = { RADEON_TXFORMAT_ ## f, RADEON_YUV_TO_RGB }
#define _INVALID(f) \
    [ MESA_FORMAT_ ## f ] = { 0xffffffff, 0 }
#define VALID_FORMAT(f) ( ((f) <= MESA_FORMAT_YCBCR_REV) \
			     && (tx_table[f].format != 0xffffffff) )

static const struct {
   GLuint format, filter;
}
tx_table[] =
{
   _ALPHA(RGBA8888),
   _ALPHA(ARGB8888),
   _INVALID(RGB888),
   _COLOR(RGB565),
   _ALPHA(ARGB4444),
   _ALPHA(ARGB1555),
   _ALPHA(AL88),
   _INVALID(A8),
   _INVALID(L8),
   _COLOR(I8),
   _INVALID(CI8),
   _YUV(YCBCR),
   _YUV(YCBCR_REV),
};

#undef _COLOR
#undef _ALPHA
#undef _INVALID

/**
 * This function computes the number of bytes of storage needed for
 * the given texture object (all mipmap levels, all cube faces).
 * The \c image[face][level].x/y/width/height parameters for upload/blitting
 * are computed here.  \c pp_txfilter, \c pp_txformat, etc. will be set here
 * too.
 * 
 * \param rmesa Context pointer
 * \param tObj GL texture object whose images are to be posted to
 *                 hardware state.
 */
static void radeonSetTexImages( radeonContextPtr rmesa,
				struct gl_texture_object *tObj )
{
   radeonTexObjPtr t = (radeonTexObjPtr)tObj->DriverData;
   const struct gl_texture_image *baseImage = tObj->Image[tObj->BaseLevel];
   GLint curOffset;
   GLint i;
   GLint numLevels;
   GLint log2Width, log2Height, log2Depth;

   /* Set the hardware texture format
    */

   t->pp_txformat &= ~(RADEON_TXFORMAT_FORMAT_MASK |
		       RADEON_TXFORMAT_ALPHA_IN_MAP);
   t->pp_txfilter &= ~RADEON_YUV_TO_RGB;

   if ( VALID_FORMAT( baseImage->TexFormat->MesaFormat ) ) {
      t->pp_txformat |= tx_table[ baseImage->TexFormat->MesaFormat ].format;
      t->pp_txfilter |= tx_table[ baseImage->TexFormat->MesaFormat ].filter;
   }
   else {
      _mesa_problem(NULL, "unexpected texture format in %s", __FUNCTION__);
      return;
   }


   /* Compute which mipmap levels we really want to send to the hardware.
    */

   driCalculateTextureFirstLastLevel( (driTextureObject *) t );
   log2Width  = tObj->Image[t->base.firstLevel]->WidthLog2;
   log2Height = tObj->Image[t->base.firstLevel]->HeightLog2;
   log2Depth  = tObj->Image[t->base.firstLevel]->DepthLog2;

   numLevels = t->base.lastLevel - t->base.firstLevel + 1;

   assert(numLevels <= RADEON_MAX_TEXTURE_LEVELS);

   /* Calculate mipmap offsets and dimensions for blitting (uploading)
    * The idea is that we lay out the mipmap levels within a block of
    * memory organized as a rectangle of width BLIT_WIDTH_BYTES.
    */
   curOffset = 0;

   for (i = 0; i < numLevels; i++) {
      const struct gl_texture_image *texImage;
      GLuint size;

      texImage = tObj->Image[i + t->base.firstLevel];
      if ( !texImage )
	 break;

      /* find image size in bytes */
      if (texImage->IsCompressed) {
         size = texImage->CompressedSize;
      }
      else if (tObj->Target == GL_TEXTURE_RECTANGLE_NV) {
      	 size = ((texImage->Width * texImage->TexFormat->TexelBytes + 63)
      	         & ~63) * texImage->Height;
      }
      else {
         int w = texImage->Width * texImage->TexFormat->TexelBytes;
         if (w < 32)
            w = 32;
         size = w * texImage->Height * texImage->Depth;
      }
      assert(size > 0);


      /* Align to 32-byte offset.  It is faster to do this unconditionally
       * (no branch penalty).
       */

      curOffset = (curOffset + 0x1f) & ~0x1f;

      t->image[0][i].x = curOffset % BLIT_WIDTH_BYTES;
      t->image[0][i].y = curOffset / BLIT_WIDTH_BYTES;
      t->image[0][i].width  = MIN2(size, BLIT_WIDTH_BYTES);
      t->image[0][i].height = size / t->image[0][i].width;

#if 0
      /* for debugging only and only  applicable to non-rectangle targets */
      assert(size % t->image[0][i].width == 0);
      assert(t->image[0][i].x == 0
             || (size < BLIT_WIDTH_BYTES && t->image[0][i].height == 1));
#endif

      if (0)
         fprintf(stderr,
                 "level %d: %dx%d x=%d y=%d w=%d h=%d size=%d at %d\n",
                 i, texImage->Width, texImage->Height,
                 t->image[0][i].x, t->image[0][i].y,
                 t->image[0][i].width, t->image[0][i].height, size, curOffset);

      curOffset += size;

   }

   /* Align the total size of texture memory block.
    */
   t->base.totalSize = (curOffset + RADEON_OFFSET_MASK) & ~RADEON_OFFSET_MASK;

   /* Hardware state:
    */
   t->pp_txfilter &= ~RADEON_MAX_MIP_LEVEL_MASK;
   t->pp_txfilter |= (numLevels - 1) << RADEON_MAX_MIP_LEVEL_SHIFT;

   t->pp_txformat &= ~(RADEON_TXFORMAT_WIDTH_MASK |
		       RADEON_TXFORMAT_HEIGHT_MASK |
                       RADEON_TXFORMAT_CUBIC_MAP_ENABLE);
   t->pp_txformat |= ((log2Width << RADEON_TXFORMAT_WIDTH_SHIFT) |
		      (log2Height << RADEON_TXFORMAT_HEIGHT_SHIFT));

   t->pp_txsize = (((tObj->Image[t->base.firstLevel]->Width - 1) << 0) |
                   ((tObj->Image[t->base.firstLevel]->Height - 1) << 16));

   /* Only need to round to nearest 32 for textures, but the blitter
    * requires 64-byte aligned pitches, and we may/may not need the
    * blitter.   NPOT only!
    */
   if (baseImage->IsCompressed)
      t->pp_txpitch = (tObj->Image[t->base.firstLevel]->Width + 63) & ~(63);
   else
      t->pp_txpitch = ((tObj->Image[t->base.firstLevel]->Width * baseImage->TexFormat->TexelBytes) + 63) & ~(63);
   t->pp_txpitch -= 32;

   t->dirty_state = TEX_ALL;

   /* FYI: radeonUploadTexImages( rmesa, t ); used to be called here */
}



/* ================================================================
 * Texture combine functions
 */

#define RADEON_DISABLE		0
#define RADEON_REPLACE		1
#define RADEON_MODULATE		2
#define RADEON_DECAL		3
#define RADEON_BLEND		4
#define RADEON_ADD		5
#define RADEON_MAX_COMBFUNC	6

static GLuint radeon_color_combine[][RADEON_MAX_COMBFUNC] =
{
   /* Unit 0:
    */
   {
      /* Disable combiner stage
       */
      (RADEON_COLOR_ARG_A_ZERO |
       RADEON_COLOR_ARG_B_ZERO |
       RADEON_COLOR_ARG_C_CURRENT_COLOR |
       RADEON_BLEND_CTL_ADD |
       RADEON_SCALE_1X |
       RADEON_CLAMP_TX),

      /* GL_REPLACE = 0x00802800
       */
      (RADEON_COLOR_ARG_A_ZERO |
       RADEON_COLOR_ARG_B_ZERO |
       RADEON_COLOR_ARG_C_T0_COLOR |
       RADEON_BLEND_CTL_ADD |
       RADEON_SCALE_1X |
       RADEON_CLAMP_TX),

      /* GL_MODULATE = 0x00800142
       */
      (RADEON_COLOR_ARG_A_CURRENT_COLOR |
       RADEON_COLOR_ARG_B_T0_COLOR |
       RADEON_COLOR_ARG_C_ZERO |
       RADEON_BLEND_CTL_ADD |
       RADEON_SCALE_1X |
       RADEON_CLAMP_TX),

      /* GL_DECAL = 0x008c2d42
       */
      (RADEON_COLOR_ARG_A_CURRENT_COLOR |
       RADEON_COLOR_ARG_B_T0_COLOR |
       RADEON_COLOR_ARG_C_T0_ALPHA |
       RADEON_BLEND_CTL_BLEND |
       RADEON_SCALE_1X |
       RADEON_CLAMP_TX),

      /* GL_BLEND = 0x008c2902
       */
      (RADEON_COLOR_ARG_A_CURRENT_COLOR |
       RADEON_COLOR_ARG_B_TFACTOR_COLOR |
       RADEON_COLOR_ARG_C_T0_COLOR |
       RADEON_BLEND_CTL_BLEND |
       RADEON_SCALE_1X |
       RADEON_CLAMP_TX),

      /* GL_ADD = 0x00812802
       */
      (RADEON_COLOR_ARG_A_CURRENT_COLOR |
       RADEON_COLOR_ARG_B_ZERO |
       RADEON_COLOR_ARG_C_T0_COLOR |
       RADEON_COMP_ARG_B |
       RADEON_BLEND_CTL_ADD |
       RADEON_SCALE_1X |
       RADEON_CLAMP_TX),
   },

   /* Unit 1:
    */
   {
      /* Disable combiner stage
       */
      (RADEON_COLOR_ARG_A_ZERO |
       RADEON_COLOR_ARG_B_ZERO |
       RADEON_COLOR_ARG_C_CURRENT_COLOR |
       RADEON_BLEND_CTL_ADD |
       RADEON_SCALE_1X |
       RADEON_CLAMP_TX),

      /* GL_REPLACE = 0x00803000
       */
      (RADEON_COLOR_ARG_A_ZERO |
       RADEON_COLOR_ARG_B_ZERO |
       RADEON_COLOR_ARG_C_T1_COLOR |
       RADEON_BLEND_CTL_ADD |
       RADEON_SCALE_1X |
       RADEON_CLAMP_TX),

      /* GL_MODULATE = 0x00800182
       */
      (RADEON_COLOR_ARG_A_CURRENT_COLOR |
       RADEON_COLOR_ARG_B_T1_COLOR |
       RADEON_COLOR_ARG_C_ZERO |
       RADEON_BLEND_CTL_ADD |
       RADEON_SCALE_1X |
       RADEON_CLAMP_TX),

      /* GL_DECAL = 0x008c3582
       */
      (RADEON_COLOR_ARG_A_CURRENT_COLOR |
       RADEON_COLOR_ARG_B_T1_COLOR |
       RADEON_COLOR_ARG_C_T1_ALPHA |
       RADEON_BLEND_CTL_BLEND |
       RADEON_SCALE_1X |
       RADEON_CLAMP_TX),

      /* GL_BLEND = 0x008c3102
       */
      (RADEON_COLOR_ARG_A_CURRENT_COLOR |
       RADEON_COLOR_ARG_B_TFACTOR_COLOR |
       RADEON_COLOR_ARG_C_T1_COLOR |
       RADEON_BLEND_CTL_BLEND |
       RADEON_SCALE_1X |
       RADEON_CLAMP_TX),

      /* GL_ADD = 0x00813002
       */
      (RADEON_COLOR_ARG_A_CURRENT_COLOR |
       RADEON_COLOR_ARG_B_ZERO |
       RADEON_COLOR_ARG_C_T1_COLOR |
       RADEON_COMP_ARG_B |
       RADEON_BLEND_CTL_ADD |
       RADEON_SCALE_1X |
       RADEON_CLAMP_TX),
   },

   /* Unit 2:
    */
   {
      /* Disable combiner stage
       */
      (RADEON_COLOR_ARG_A_ZERO |
       RADEON_COLOR_ARG_B_ZERO |
       RADEON_COLOR_ARG_C_CURRENT_COLOR |
       RADEON_BLEND_CTL_ADD |
       RADEON_SCALE_1X |
       RADEON_CLAMP_TX),

      /* GL_REPLACE = 0x00803800
       */
      (RADEON_COLOR_ARG_A_ZERO |
       RADEON_COLOR_ARG_B_ZERO |
       RADEON_COLOR_ARG_C_T2_COLOR |
       RADEON_BLEND_CTL_ADD |
       RADEON_SCALE_1X |
       RADEON_CLAMP_TX),

      /* GL_MODULATE = 0x008001c2
       */
      (RADEON_COLOR_ARG_A_CURRENT_COLOR |
       RADEON_COLOR_ARG_B_T2_COLOR |
       RADEON_COLOR_ARG_C_ZERO |
       RADEON_BLEND_CTL_ADD |
       RADEON_SCALE_1X |
       RADEON_CLAMP_TX),

      /* GL_DECAL = 0x008c3dc2
       */
      (RADEON_COLOR_ARG_A_CURRENT_COLOR |
       RADEON_COLOR_ARG_B_T2_COLOR |
       RADEON_COLOR_ARG_C_T2_ALPHA |
       RADEON_BLEND_CTL_BLEND |
       RADEON_SCALE_1X |
       RADEON_CLAMP_TX),

      /* GL_BLEND = 0x008c3902
       */
      (RADEON_COLOR_ARG_A_CURRENT_COLOR |
       RADEON_COLOR_ARG_B_TFACTOR_COLOR |
       RADEON_COLOR_ARG_C_T2_COLOR |
       RADEON_BLEND_CTL_BLEND |
       RADEON_SCALE_1X |
       RADEON_CLAMP_TX),

      /* GL_ADD = 0x00813802
       */
      (RADEON_COLOR_ARG_A_CURRENT_COLOR |
       RADEON_COLOR_ARG_B_ZERO |
       RADEON_COLOR_ARG_C_T2_COLOR |
       RADEON_COMP_ARG_B |
       RADEON_BLEND_CTL_ADD |
       RADEON_SCALE_1X |
       RADEON_CLAMP_TX),
   }
};

static GLuint radeon_alpha_combine[][RADEON_MAX_COMBFUNC] =
{
   /* Unit 0:
    */
   {
      /* Disable combiner stage
       */
      (RADEON_ALPHA_ARG_A_ZERO |
       RADEON_ALPHA_ARG_B_ZERO |
       RADEON_ALPHA_ARG_C_CURRENT_ALPHA |
       RADEON_BLEND_CTL_ADD |
       RADEON_SCALE_1X |
       RADEON_CLAMP_TX),

      /* GL_REPLACE = 0x00800500
       */
      (RADEON_ALPHA_ARG_A_ZERO |
       RADEON_ALPHA_ARG_B_ZERO |
       RADEON_ALPHA_ARG_C_T0_ALPHA |
       RADEON_BLEND_CTL_ADD |
       RADEON_SCALE_1X |
       RADEON_CLAMP_TX),

      /* GL_MODULATE = 0x00800051
       */
      (RADEON_ALPHA_ARG_A_CURRENT_ALPHA |
       RADEON_ALPHA_ARG_B_T0_ALPHA |
       RADEON_ALPHA_ARG_C_ZERO |
       RADEON_BLEND_CTL_ADD |
       RADEON_SCALE_1X |
       RADEON_CLAMP_TX),

      /* GL_DECAL = 0x00800100
       */
      (RADEON_ALPHA_ARG_A_ZERO |
       RADEON_ALPHA_ARG_B_ZERO |
       RADEON_ALPHA_ARG_C_CURRENT_ALPHA |
       RADEON_BLEND_CTL_ADD |
       RADEON_SCALE_1X |
       RADEON_CLAMP_TX),

      /* GL_BLEND = 0x00800051
       */
      (RADEON_ALPHA_ARG_A_CURRENT_ALPHA |
       RADEON_ALPHA_ARG_B_TFACTOR_ALPHA |
       RADEON_ALPHA_ARG_C_T0_ALPHA |
       RADEON_BLEND_CTL_BLEND |
       RADEON_SCALE_1X |
       RADEON_CLAMP_TX),

      /* GL_ADD = 0x00800051
       */
      (RADEON_ALPHA_ARG_A_CURRENT_ALPHA |
       RADEON_ALPHA_ARG_B_ZERO |
       RADEON_ALPHA_ARG_C_T0_ALPHA |
       RADEON_COMP_ARG_B |
       RADEON_BLEND_CTL_ADD |
       RADEON_SCALE_1X |
       RADEON_CLAMP_TX),
   },

   /* Unit 1:
    */
   {
      /* Disable combiner stage
       */
      (RADEON_ALPHA_ARG_A_ZERO |
       RADEON_ALPHA_ARG_B_ZERO |
       RADEON_ALPHA_ARG_C_CURRENT_ALPHA |
       RADEON_BLEND_CTL_ADD |
       RADEON_SCALE_1X |
       RADEON_CLAMP_TX),

      /* GL_REPLACE = 0x00800600
       */
      (RADEON_ALPHA_ARG_A_ZERO |
       RADEON_ALPHA_ARG_B_ZERO |
       RADEON_ALPHA_ARG_C_T1_ALPHA |
       RADEON_BLEND_CTL_ADD |
       RADEON_SCALE_1X |
       RADEON_CLAMP_TX),

      /* GL_MODULATE = 0x00800061
       */
      (RADEON_ALPHA_ARG_A_CURRENT_ALPHA |
       RADEON_ALPHA_ARG_B_T1_ALPHA |
       RADEON_ALPHA_ARG_C_ZERO |
       RADEON_BLEND_CTL_ADD |
       RADEON_SCALE_1X |
       RADEON_CLAMP_TX),

      /* GL_DECAL = 0x00800100
       */
      (RADEON_ALPHA_ARG_A_ZERO |
       RADEON_ALPHA_ARG_B_ZERO |
       RADEON_ALPHA_ARG_C_CURRENT_ALPHA |
       RADEON_BLEND_CTL_ADD |
       RADEON_SCALE_1X |
       RADEON_CLAMP_TX),

      /* GL_BLEND = 0x00800061
       */
      (RADEON_ALPHA_ARG_A_CURRENT_ALPHA |
       RADEON_ALPHA_ARG_B_TFACTOR_ALPHA |
       RADEON_ALPHA_ARG_C_T1_ALPHA |
       RADEON_BLEND_CTL_BLEND |
       RADEON_SCALE_1X |
       RADEON_CLAMP_TX),

      /* GL_ADD = 0x00800061
       */
      (RADEON_ALPHA_ARG_A_CURRENT_ALPHA |
       RADEON_ALPHA_ARG_B_ZERO |
       RADEON_ALPHA_ARG_C_T1_ALPHA |
       RADEON_COMP_ARG_B |
       RADEON_BLEND_CTL_ADD |
       RADEON_SCALE_1X |
       RADEON_CLAMP_TX),
   },

   /* Unit 2:
    */
   {
      /* Disable combiner stage
       */
      (RADEON_ALPHA_ARG_A_ZERO |
       RADEON_ALPHA_ARG_B_ZERO |
       RADEON_ALPHA_ARG_C_CURRENT_ALPHA |
       RADEON_BLEND_CTL_ADD |
       RADEON_SCALE_1X |
       RADEON_CLAMP_TX),

      /* GL_REPLACE = 0x00800700
       */
      (RADEON_ALPHA_ARG_A_ZERO |
       RADEON_ALPHA_ARG_B_ZERO |
       RADEON_ALPHA_ARG_C_T2_ALPHA |
       RADEON_BLEND_CTL_ADD |
       RADEON_SCALE_1X |
       RADEON_CLAMP_TX),

      /* GL_MODULATE = 0x00800071
       */
      (RADEON_ALPHA_ARG_A_CURRENT_ALPHA |
       RADEON_ALPHA_ARG_B_T2_ALPHA |
       RADEON_ALPHA_ARG_C_ZERO |
       RADEON_BLEND_CTL_ADD |
       RADEON_SCALE_1X |
       RADEON_CLAMP_TX),

      /* GL_DECAL = 0x00800100
       */
      (RADEON_ALPHA_ARG_A_ZERO |
       RADEON_ALPHA_ARG_B_ZERO |
       RADEON_ALPHA_ARG_C_CURRENT_ALPHA |
       RADEON_BLEND_CTL_ADD |
       RADEON_SCALE_1X |
       RADEON_CLAMP_TX),

      /* GL_BLEND = 0x00800071
       */
      (RADEON_ALPHA_ARG_A_CURRENT_ALPHA |
       RADEON_ALPHA_ARG_B_TFACTOR_ALPHA |
       RADEON_ALPHA_ARG_C_T2_ALPHA |
       RADEON_BLEND_CTL_BLEND |
       RADEON_SCALE_1X |
       RADEON_CLAMP_TX),

      /* GL_ADD = 0x00800021
       */
      (RADEON_ALPHA_ARG_A_CURRENT_ALPHA |
       RADEON_ALPHA_ARG_B_ZERO |
       RADEON_ALPHA_ARG_C_T2_ALPHA |
       RADEON_COMP_ARG_B |
       RADEON_BLEND_CTL_ADD |
       RADEON_SCALE_1X |
       RADEON_CLAMP_TX),
   }
};


/* GL_ARB_texture_env_combine support
 */

/* The color tables have combine functions for GL_SRC_COLOR,
 * GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA and GL_ONE_MINUS_SRC_ALPHA.
 */
static GLuint radeon_texture_color[][RADEON_MAX_TEXTURE_UNITS] =
{
   {
      RADEON_COLOR_ARG_A_T0_COLOR,
      RADEON_COLOR_ARG_A_T1_COLOR,
      RADEON_COLOR_ARG_A_T2_COLOR
   },
   {
      RADEON_COLOR_ARG_A_T0_COLOR | RADEON_COMP_ARG_A,
      RADEON_COLOR_ARG_A_T1_COLOR | RADEON_COMP_ARG_A,
      RADEON_COLOR_ARG_A_T2_COLOR | RADEON_COMP_ARG_A
   },
   {
      RADEON_COLOR_ARG_A_T0_ALPHA,
      RADEON_COLOR_ARG_A_T1_ALPHA,
      RADEON_COLOR_ARG_A_T2_ALPHA
   },
   {
      RADEON_COLOR_ARG_A_T0_ALPHA | RADEON_COMP_ARG_A,
      RADEON_COLOR_ARG_A_T1_ALPHA | RADEON_COMP_ARG_A,
      RADEON_COLOR_ARG_A_T2_ALPHA | RADEON_COMP_ARG_A
   },
};

static GLuint radeon_tfactor_color[] =
{
   RADEON_COLOR_ARG_A_TFACTOR_COLOR,
   RADEON_COLOR_ARG_A_TFACTOR_COLOR | RADEON_COMP_ARG_A,
   RADEON_COLOR_ARG_A_TFACTOR_ALPHA,
   RADEON_COLOR_ARG_A_TFACTOR_ALPHA | RADEON_COMP_ARG_A
};

static GLuint radeon_primary_color[] =
{
   RADEON_COLOR_ARG_A_DIFFUSE_COLOR,
   RADEON_COLOR_ARG_A_DIFFUSE_COLOR | RADEON_COMP_ARG_A,
   RADEON_COLOR_ARG_A_DIFFUSE_ALPHA,
   RADEON_COLOR_ARG_A_DIFFUSE_ALPHA | RADEON_COMP_ARG_A
};

static GLuint radeon_previous_color[] =
{
   RADEON_COLOR_ARG_A_CURRENT_COLOR,
   RADEON_COLOR_ARG_A_CURRENT_COLOR | RADEON_COMP_ARG_A,
   RADEON_COLOR_ARG_A_CURRENT_ALPHA,
   RADEON_COLOR_ARG_A_CURRENT_ALPHA | RADEON_COMP_ARG_A
};

/* GL_ZERO table - indices 0-3
 * GL_ONE  table - indices 1-4
 */
static GLuint radeon_zero_color[] =
{
   RADEON_COLOR_ARG_A_ZERO,
   RADEON_COLOR_ARG_A_ZERO | RADEON_COMP_ARG_A,
   RADEON_COLOR_ARG_A_ZERO,
   RADEON_COLOR_ARG_A_ZERO | RADEON_COMP_ARG_A,
   RADEON_COLOR_ARG_A_ZERO
};


/* The alpha tables only have GL_SRC_ALPHA and GL_ONE_MINUS_SRC_ALPHA.
 */
static GLuint radeon_texture_alpha[][RADEON_MAX_TEXTURE_UNITS] =
{
   {
      RADEON_ALPHA_ARG_A_T0_ALPHA,
      RADEON_ALPHA_ARG_A_T1_ALPHA,
      RADEON_ALPHA_ARG_A_T2_ALPHA
   },
   {
      RADEON_ALPHA_ARG_A_T0_ALPHA | RADEON_COMP_ARG_A,
      RADEON_ALPHA_ARG_A_T1_ALPHA | RADEON_COMP_ARG_A,
      RADEON_ALPHA_ARG_A_T2_ALPHA | RADEON_COMP_ARG_A
   },
};

static GLuint radeon_tfactor_alpha[] =
{
   RADEON_ALPHA_ARG_A_TFACTOR_ALPHA,
   RADEON_ALPHA_ARG_A_TFACTOR_ALPHA | RADEON_COMP_ARG_A
};

static GLuint radeon_primary_alpha[] =
{
   RADEON_ALPHA_ARG_A_DIFFUSE_ALPHA,
   RADEON_ALPHA_ARG_A_DIFFUSE_ALPHA | RADEON_COMP_ARG_A
};

static GLuint radeon_previous_alpha[] =
{
   RADEON_ALPHA_ARG_A_CURRENT_ALPHA,
   RADEON_ALPHA_ARG_A_CURRENT_ALPHA | RADEON_COMP_ARG_A
};

/* GL_ZERO table - indices 0-1
 * GL_ONE  table - indices 1-2
 */
static GLuint radeon_zero_alpha[] =
{
   RADEON_ALPHA_ARG_A_ZERO,
   RADEON_ALPHA_ARG_A_ZERO | RADEON_COMP_ARG_A,
   RADEON_ALPHA_ARG_A_ZERO
};


/* Extract the arg from slot A, shift it into the correct argument slot
 * and set the corresponding complement bit.
 */
#define RADEON_COLOR_ARG( n, arg )			\
do {							\
   color_combine |=					\
      ((color_arg[n] & RADEON_COLOR_ARG_MASK)		\
       << RADEON_COLOR_ARG_##arg##_SHIFT);		\
   color_combine |=					\
      ((color_arg[n] >> RADEON_COMP_ARG_SHIFT)		\
       << RADEON_COMP_ARG_##arg##_SHIFT);		\
} while (0)

#define RADEON_ALPHA_ARG( n, arg )			\
do {							\
   alpha_combine |=					\
      ((alpha_arg[n] & RADEON_ALPHA_ARG_MASK)		\
       << RADEON_ALPHA_ARG_##arg##_SHIFT);		\
   alpha_combine |=					\
      ((alpha_arg[n] >> RADEON_COMP_ARG_SHIFT)		\
       << RADEON_COMP_ARG_##arg##_SHIFT);		\
} while (0)


/* ================================================================
 * Texture unit state management
 */

static GLboolean radeonUpdateTextureEnv( GLcontext *ctx, int unit )
{
   radeonContextPtr rmesa = RADEON_CONTEXT(ctx);
   const struct gl_texture_unit *texUnit = &ctx->Texture.Unit[unit];
   GLuint color_combine, alpha_combine;

   /* texUnit->_Current can be NULL if and only if the texture unit is
    * not actually enabled.
    */
   assert( (texUnit->_ReallyEnabled == 0)
	   || (texUnit->_Current != NULL) );

   if ( RADEON_DEBUG & DEBUG_TEXTURE ) {
      fprintf( stderr, "%s( %p, %d )\n", __FUNCTION__, (void *)ctx, unit );
   }

   /* Set the texture environment state.  Isn't this nice and clean?
    * The chip will automagically set the texture alpha to 0xff when
    * the texture format does not include an alpha component.  This
    * reduces the amount of special-casing we have to do, alpha-only
    * textures being a notable exception.
    */
   if ( !texUnit->_ReallyEnabled ) {
      /* Don't cache these results.
       */
      rmesa->state.texture.unit[unit].format = 0;
      rmesa->state.texture.unit[unit].envMode = 0;
      color_combine = radeon_color_combine[unit][RADEON_DISABLE];
      alpha_combine = radeon_alpha_combine[unit][RADEON_DISABLE];
   }
   else {
      const struct gl_texture_object *tObj = texUnit->_Current;
      const GLenum format = tObj->Image[tObj->BaseLevel]->Format;
      GLuint color_arg[3], alpha_arg[3];
      GLuint i, numColorArgs = 0, numAlphaArgs = 0;
      GLuint RGBshift = texUnit->CombineScaleShiftRGB;
      GLuint Ashift = texUnit->CombineScaleShiftA;

      switch ( texUnit->EnvMode ) {
      case GL_REPLACE:
         switch ( format ) {
	 case GL_RGBA:
         case GL_LUMINANCE_ALPHA:
         case GL_INTENSITY:
	    color_combine = radeon_color_combine[unit][RADEON_REPLACE];
	    alpha_combine = radeon_alpha_combine[unit][RADEON_REPLACE];
	    break;
	 case GL_ALPHA:
	    color_combine = radeon_color_combine[unit][RADEON_DISABLE];
	    alpha_combine = radeon_alpha_combine[unit][RADEON_REPLACE];
	    break;
	 case GL_LUMINANCE:
	 case GL_RGB:
	 case GL_YCBCR_MESA:
	    color_combine = radeon_color_combine[unit][RADEON_REPLACE];
	    alpha_combine = radeon_alpha_combine[unit][RADEON_DISABLE];
	    break;
	 case GL_COLOR_INDEX:
	 default:
	    return GL_FALSE;
	 }
	 break;

      case GL_MODULATE:
	 switch ( format ) {
	 case GL_RGBA:
	 case GL_LUMINANCE_ALPHA:
	 case GL_INTENSITY:
	    color_combine = radeon_color_combine[unit][RADEON_MODULATE];
	    alpha_combine = radeon_alpha_combine[unit][RADEON_MODULATE];
	    break;
	 case GL_ALPHA:
	    color_combine = radeon_color_combine[unit][RADEON_DISABLE];
	    alpha_combine = radeon_alpha_combine[unit][RADEON_MODULATE];
	    break;
	 case GL_RGB:
	 case GL_LUMINANCE:
	 case GL_YCBCR_MESA:
	    color_combine = radeon_color_combine[unit][RADEON_MODULATE];
	    alpha_combine = radeon_alpha_combine[unit][RADEON_DISABLE];
	    break;
	 case GL_COLOR_INDEX:
	 default:
	    return GL_FALSE;
	 }
	 break;

      case GL_DECAL:
	 switch ( format ) {
	 case GL_RGBA:
	 case GL_RGB:
	 case GL_YCBCR_MESA:
	    color_combine = radeon_color_combine[unit][RADEON_DECAL];
	    alpha_combine = radeon_alpha_combine[unit][RADEON_DISABLE];
	    break;
	 case GL_ALPHA:
	 case GL_LUMINANCE:
	 case GL_LUMINANCE_ALPHA:
	 case GL_INTENSITY:
	    color_combine = radeon_color_combine[unit][RADEON_DISABLE];
	    alpha_combine = radeon_alpha_combine[unit][RADEON_DISABLE];
	    break;
	 case GL_COLOR_INDEX:
	 default:
	    return GL_FALSE;
	 }
	 break;

      case GL_BLEND:
	 switch ( format ) {
	 case GL_RGBA:
	 case GL_RGB:
	 case GL_LUMINANCE:
	 case GL_LUMINANCE_ALPHA:
	 case GL_YCBCR_MESA:
	    color_combine = radeon_color_combine[unit][RADEON_BLEND];
	    alpha_combine = radeon_alpha_combine[unit][RADEON_MODULATE];
	    break;
	 case GL_ALPHA:
	    color_combine = radeon_color_combine[unit][RADEON_DISABLE];
	    alpha_combine = radeon_alpha_combine[unit][RADEON_MODULATE];
	    break;
	 case GL_INTENSITY:
	    color_combine = radeon_color_combine[unit][RADEON_BLEND];
	    alpha_combine = radeon_alpha_combine[unit][RADEON_BLEND];
	    break;
	 case GL_COLOR_INDEX:
	 default:
	    return GL_FALSE;
	 }
	 break;

      case GL_ADD:
	 switch ( format ) {
	 case GL_RGBA:
	 case GL_RGB:
	 case GL_LUMINANCE:
	 case GL_LUMINANCE_ALPHA:
	 case GL_YCBCR_MESA:
	    color_combine = radeon_color_combine[unit][RADEON_ADD];
	    alpha_combine = radeon_alpha_combine[unit][RADEON_MODULATE];
	    break;
	 case GL_ALPHA:
	    color_combine = radeon_color_combine[unit][RADEON_DISABLE];
	    alpha_combine = radeon_alpha_combine[unit][RADEON_MODULATE];
	    break;
	 case GL_INTENSITY:
	    color_combine = radeon_color_combine[unit][RADEON_ADD];
	    alpha_combine = radeon_alpha_combine[unit][RADEON_ADD];
	    break;
	 case GL_COLOR_INDEX:
	 default:
	    return GL_FALSE;
	 }
	 break;

      case GL_COMBINE:
	 /* Don't cache these results.
	  */
	 rmesa->state.texture.unit[unit].format = 0;
	 rmesa->state.texture.unit[unit].envMode = 0;

	 /* Step 0:
	  * Calculate how many arguments we need to process.
	  */
	 switch ( texUnit->CombineModeRGB ) {
	 case GL_REPLACE:
	    numColorArgs = 1;
	    break;
	 case GL_MODULATE:
	 case GL_ADD:
	 case GL_ADD_SIGNED:
	 case GL_SUBTRACT:
	 case GL_DOT3_RGB:
	 case GL_DOT3_RGBA:
	 case GL_DOT3_RGB_EXT:
	 case GL_DOT3_RGBA_EXT:
	    numColorArgs = 2;
	    break;
	 case GL_INTERPOLATE:
	 case GL_MODULATE_ADD_ATI:
	 case GL_MODULATE_SIGNED_ADD_ATI:
	 case GL_MODULATE_SUBTRACT_ATI:
	    numColorArgs = 3;
	    break;
	 default:
	    return GL_FALSE;
	 }

	 switch ( texUnit->CombineModeA ) {
	 case GL_REPLACE:
	    numAlphaArgs = 1;
	    break;
	 case GL_MODULATE:
	 case GL_ADD:
	 case GL_ADD_SIGNED:
	 case GL_SUBTRACT:
	    numAlphaArgs = 2;
	    break;
	 case GL_INTERPOLATE:
	 case GL_MODULATE_ADD_ATI:
	 case GL_MODULATE_SIGNED_ADD_ATI:
	 case GL_MODULATE_SUBTRACT_ATI:
	    numAlphaArgs = 3;
	    break;
	 default:
	    return GL_FALSE;
	 }

	 /* Step 1:
	  * Extract the color and alpha combine function arguments.
	  */
	 for ( i = 0 ; i < numColorArgs ; i++ ) {
	    const GLuint op = texUnit->CombineOperandRGB[i] - GL_SRC_COLOR;
	    assert(op >= 0);
	    assert(op <= 3);
	    switch ( texUnit->CombineSourceRGB[i] ) {
	    case GL_TEXTURE:
	       color_arg[i] = radeon_texture_color[op][unit];
	       break;
	    case GL_CONSTANT:
	       color_arg[i] = radeon_tfactor_color[op];
	       break;
	    case GL_PRIMARY_COLOR:
	       color_arg[i] = radeon_primary_color[op];
	       break;
	    case GL_PREVIOUS:
	       color_arg[i] = radeon_previous_color[op];
	       break;
	    case GL_ZERO:
	       color_arg[i] = radeon_zero_color[op];
	       break;
	    case GL_ONE:
	       color_arg[i] = radeon_zero_color[op+1];
	       break;
	    default:
	       return GL_FALSE;
	    }
	 }

	 for ( i = 0 ; i < numAlphaArgs ; i++ ) {
	    const GLuint op = texUnit->CombineOperandA[i] - GL_SRC_ALPHA;
	    assert(op >= 0);
	    assert(op <= 1);
	    switch ( texUnit->CombineSourceA[i] ) {
	    case GL_TEXTURE:
	       alpha_arg[i] = radeon_texture_alpha[op][unit];
	       break;
	    case GL_CONSTANT:
	       alpha_arg[i] = radeon_tfactor_alpha[op];
	       break;
	    case GL_PRIMARY_COLOR:
	       alpha_arg[i] = radeon_primary_alpha[op];
	       break;
	    case GL_PREVIOUS:
	       alpha_arg[i] = radeon_previous_alpha[op];
	       break;
	    case GL_ZERO:
	       alpha_arg[i] = radeon_zero_alpha[op];
	       break;
	    case GL_ONE:
	       alpha_arg[i] = radeon_zero_alpha[op+1];
	       break;
	    default:
	       return GL_FALSE;
	    }
	 }

	 /* Step 2:
	  * Build up the color and alpha combine functions.
	  */
	 switch ( texUnit->CombineModeRGB ) {
	 case GL_REPLACE:
	    color_combine = (RADEON_COLOR_ARG_A_ZERO |
			     RADEON_COLOR_ARG_B_ZERO |
			     RADEON_BLEND_CTL_ADD |
			     RADEON_CLAMP_TX);
	    RADEON_COLOR_ARG( 0, C );
	    break;
	 case GL_MODULATE:
	    color_combine = (RADEON_COLOR_ARG_C_ZERO |
			     RADEON_BLEND_CTL_ADD |
			     RADEON_CLAMP_TX);
	    RADEON_COLOR_ARG( 0, A );
	    RADEON_COLOR_ARG( 1, B );
	    break;
	 case GL_ADD:
	    color_combine = (RADEON_COLOR_ARG_B_ZERO |
			     RADEON_COMP_ARG_B |
			     RADEON_BLEND_CTL_ADD |
			     RADEON_CLAMP_TX);
	    RADEON_COLOR_ARG( 0, A );
	    RADEON_COLOR_ARG( 1, C );
	    break;
	 case GL_ADD_SIGNED:
	    color_combine = (RADEON_COLOR_ARG_B_ZERO |
			     RADEON_COMP_ARG_B |
			     RADEON_BLEND_CTL_ADDSIGNED |
			     RADEON_CLAMP_TX);
	    RADEON_COLOR_ARG( 0, A );
	    RADEON_COLOR_ARG( 1, C );
	    break;
	 case GL_SUBTRACT:
	    color_combine = (RADEON_COLOR_ARG_B_ZERO |
			     RADEON_COMP_ARG_B |
			     RADEON_BLEND_CTL_SUBTRACT |
			     RADEON_CLAMP_TX);
	    RADEON_COLOR_ARG( 0, A );
	    RADEON_COLOR_ARG( 1, C );
	    break;
	 case GL_INTERPOLATE:
	    color_combine = (RADEON_BLEND_CTL_BLEND |
			     RADEON_CLAMP_TX);
	    RADEON_COLOR_ARG( 0, B );
	    RADEON_COLOR_ARG( 1, A );
	    RADEON_COLOR_ARG( 2, C );
	    break;

	 case GL_DOT3_RGB_EXT:
	 case GL_DOT3_RGBA_EXT:
	    /* The EXT version of the DOT3 extension does not support the
	     * scale factor, but the ARB version (and the version in OpenGL
	     * 1.3) does.
	     */
	    RGBshift = 0;
	    Ashift = 0;
	    /* FALLTHROUGH */

	 case GL_DOT3_RGB:
	 case GL_DOT3_RGBA:
	    /* The R100 / RV200 only support a 1X multiplier in hardware
	     * w/the ARB version.
	     */
	    if ( RGBshift != (RADEON_SCALE_1X >> RADEON_SCALE_SHIFT) ) {
	       return GL_FALSE;
	    }

	    RGBshift += 2;
	    Ashift = RGBshift;

	    color_combine = (RADEON_COLOR_ARG_C_ZERO |
			     RADEON_BLEND_CTL_DOT3 |
			     RADEON_CLAMP_TX);
	    RADEON_COLOR_ARG( 0, A );
	    RADEON_COLOR_ARG( 1, B );
	    break;

	 case GL_MODULATE_ADD_ATI:
	    color_combine = (RADEON_BLEND_CTL_ADD |
			     RADEON_CLAMP_TX);
	    RADEON_COLOR_ARG( 0, A );
	    RADEON_COLOR_ARG( 1, C );
	    RADEON_COLOR_ARG( 2, B );
	    break;
	 case GL_MODULATE_SIGNED_ADD_ATI:
	    color_combine = (RADEON_BLEND_CTL_ADDSIGNED |
			     RADEON_CLAMP_TX);
	    RADEON_COLOR_ARG( 0, A );
	    RADEON_COLOR_ARG( 1, C );
	    RADEON_COLOR_ARG( 2, B );
	    break;
	 case GL_MODULATE_SUBTRACT_ATI:
	    color_combine = (RADEON_BLEND_CTL_SUBTRACT |
			     RADEON_CLAMP_TX);
	    RADEON_COLOR_ARG( 0, A );
	    RADEON_COLOR_ARG( 1, C );
	    RADEON_COLOR_ARG( 2, B );
	    break;
	 default:
	    return GL_FALSE;
	 }

	 switch ( texUnit->CombineModeA ) {
	 case GL_REPLACE:
	    alpha_combine = (RADEON_ALPHA_ARG_A_ZERO |
			     RADEON_ALPHA_ARG_B_ZERO |
			     RADEON_BLEND_CTL_ADD |
			     RADEON_CLAMP_TX);
	    RADEON_ALPHA_ARG( 0, C );
	    break;
	 case GL_MODULATE:
	    alpha_combine = (RADEON_ALPHA_ARG_C_ZERO |
			     RADEON_BLEND_CTL_ADD |
			     RADEON_CLAMP_TX);
	    RADEON_ALPHA_ARG( 0, A );
	    RADEON_ALPHA_ARG( 1, B );
	    break;
	 case GL_ADD:
	    alpha_combine = (RADEON_ALPHA_ARG_B_ZERO |
			     RADEON_COMP_ARG_B |
			     RADEON_BLEND_CTL_ADD |
			     RADEON_CLAMP_TX);
	    RADEON_ALPHA_ARG( 0, A );
	    RADEON_ALPHA_ARG( 1, C );
	    break;
	 case GL_ADD_SIGNED:
	    alpha_combine = (RADEON_ALPHA_ARG_B_ZERO |
			     RADEON_COMP_ARG_B |
			     RADEON_BLEND_CTL_ADDSIGNED |
			     RADEON_CLAMP_TX);
	    RADEON_ALPHA_ARG( 0, A );
	    RADEON_ALPHA_ARG( 1, C );
	    break;
	 case GL_SUBTRACT:
	    alpha_combine = (RADEON_COLOR_ARG_B_ZERO |
			     RADEON_COMP_ARG_B |
			     RADEON_BLEND_CTL_SUBTRACT |
			     RADEON_CLAMP_TX);
	    RADEON_ALPHA_ARG( 0, A );
	    RADEON_ALPHA_ARG( 1, C );
	    break;
	 case GL_INTERPOLATE:
	    alpha_combine = (RADEON_BLEND_CTL_BLEND |
			     RADEON_CLAMP_TX);
	    RADEON_ALPHA_ARG( 0, B );
	    RADEON_ALPHA_ARG( 1, A );
	    RADEON_ALPHA_ARG( 2, C );
	    break;

	 case GL_MODULATE_ADD_ATI:
	    alpha_combine = (RADEON_BLEND_CTL_ADD |
			     RADEON_CLAMP_TX);
	    RADEON_ALPHA_ARG( 0, A );
	    RADEON_ALPHA_ARG( 1, C );
	    RADEON_ALPHA_ARG( 2, B );
	    break;
	 case GL_MODULATE_SIGNED_ADD_ATI:
	    alpha_combine = (RADEON_BLEND_CTL_ADDSIGNED |
			     RADEON_CLAMP_TX);
	    RADEON_ALPHA_ARG( 0, A );
	    RADEON_ALPHA_ARG( 1, C );
	    RADEON_ALPHA_ARG( 2, B );
	    break;
	 case GL_MODULATE_SUBTRACT_ATI:
	    alpha_combine = (RADEON_BLEND_CTL_SUBTRACT |
			     RADEON_CLAMP_TX);
	    RADEON_ALPHA_ARG( 0, A );
	    RADEON_ALPHA_ARG( 1, C );
	    RADEON_ALPHA_ARG( 2, B );
	    break;
	 default:
	    return GL_FALSE;
	 }

	 if ( (texUnit->CombineModeRGB == GL_DOT3_RGB_EXT)
	      || (texUnit->CombineModeRGB == GL_DOT3_RGB) ) {
	    alpha_combine |= RADEON_DOT_ALPHA_DONT_REPLICATE;
	 }

	 /* Step 3:
	  * Apply the scale factor.
	  */
	 color_combine |= (RGBshift << RADEON_SCALE_SHIFT);
	 alpha_combine |= (Ashift   << RADEON_SCALE_SHIFT);

	 /* All done!
	  */
	 break;

      default:
	 return GL_FALSE;
      }
   }

   if ( rmesa->hw.tex[unit].cmd[TEX_PP_TXCBLEND] != color_combine ||
	rmesa->hw.tex[unit].cmd[TEX_PP_TXABLEND] != alpha_combine ) {
      RADEON_STATECHANGE( rmesa, tex[unit] );
      rmesa->hw.tex[unit].cmd[TEX_PP_TXCBLEND] = color_combine;
      rmesa->hw.tex[unit].cmd[TEX_PP_TXABLEND] = alpha_combine;
   }

   return GL_TRUE;
}

#define TEXOBJ_TXFILTER_MASK (RADEON_MAX_MIP_LEVEL_MASK |	\
			      RADEON_MIN_FILTER_MASK | 		\
			      RADEON_MAG_FILTER_MASK |		\
			      RADEON_MAX_ANISO_MASK |		\
			      RADEON_YUV_TO_RGB |		\
			      RADEON_YUV_TEMPERATURE_MASK |	\
			      RADEON_CLAMP_S_MASK | 		\
			      RADEON_CLAMP_T_MASK | 		\
			      RADEON_BORDER_MODE_D3D )

#define TEXOBJ_TXFORMAT_MASK (RADEON_TXFORMAT_WIDTH_MASK |	\
			      RADEON_TXFORMAT_HEIGHT_MASK |	\
			      RADEON_TXFORMAT_FORMAT_MASK |	\
                              RADEON_TXFORMAT_F5_WIDTH_MASK |	\
                              RADEON_TXFORMAT_F5_HEIGHT_MASK |	\
			      RADEON_TXFORMAT_ALPHA_IN_MAP |	\
			      RADEON_TXFORMAT_CUBIC_MAP_ENABLE |	\
                              RADEON_TXFORMAT_NON_POWER2)


static void import_tex_obj_state( radeonContextPtr rmesa,
				  int unit,
				  radeonTexObjPtr texobj )
{
   GLuint *cmd = RADEON_DB_STATE( tex[unit] );

   cmd[TEX_PP_TXFILTER] &= ~TEXOBJ_TXFILTER_MASK;
   cmd[TEX_PP_TXFILTER] |= texobj->pp_txfilter & TEXOBJ_TXFILTER_MASK;
   cmd[TEX_PP_TXFORMAT] &= ~TEXOBJ_TXFORMAT_MASK;
   cmd[TEX_PP_TXFORMAT] |= texobj->pp_txformat & TEXOBJ_TXFORMAT_MASK;
   cmd[TEX_PP_TXOFFSET] = texobj->pp_txoffset;
   cmd[TEX_PP_BORDER_COLOR] = texobj->pp_border_color;
   RADEON_DB_STATECHANGE( rmesa, &rmesa->hw.tex[unit] );

   if (texobj->base.tObj->Target == GL_TEXTURE_RECTANGLE_NV) {
      GLuint *txr_cmd = RADEON_DB_STATE( txr[unit] );
      txr_cmd[TXR_PP_TEX_SIZE] = texobj->pp_txsize; /* NPOT only! */
      txr_cmd[TXR_PP_TEX_PITCH] = texobj->pp_txpitch; /* NPOT only! */
      RADEON_DB_STATECHANGE( rmesa, &rmesa->hw.txr[unit] );
   }

   texobj->dirty_state &= ~(1<<unit);
}




static void set_texgen_matrix( radeonContextPtr rmesa, 
			       GLuint unit,
			       const GLfloat *s_plane,
			       const GLfloat *t_plane )
{
   static const GLfloat scale_identity[4] = { 1,1,1,1 };

   if (!TEST_EQ_4V( s_plane, scale_identity) ||
       !TEST_EQ_4V( t_plane, scale_identity)) {
      rmesa->TexGenEnabled |= RADEON_TEXMAT_0_ENABLE<<unit;
      rmesa->TexGenMatrix[unit].m[0]  = s_plane[0];
      rmesa->TexGenMatrix[unit].m[4]  = s_plane[1];
      rmesa->TexGenMatrix[unit].m[8]  = s_plane[2];
      rmesa->TexGenMatrix[unit].m[12] = s_plane[3];

      rmesa->TexGenMatrix[unit].m[1]  = t_plane[0];
      rmesa->TexGenMatrix[unit].m[5]  = t_plane[1];
      rmesa->TexGenMatrix[unit].m[9]  = t_plane[2];
      rmesa->TexGenMatrix[unit].m[13] = t_plane[3];
      rmesa->NewGLState |= _NEW_TEXTURE_MATRIX;
   }
}

/* Ignoring the Q texcoord for now.
 *
 * Returns GL_FALSE if fallback required.  
 */
static GLboolean radeon_validate_texgen( GLcontext *ctx, GLuint unit )
{  
   radeonContextPtr rmesa = RADEON_CONTEXT(ctx);
   struct gl_texture_unit *texUnit = &ctx->Texture.Unit[unit];
   GLuint inputshift = RADEON_TEXGEN_0_INPUT_SHIFT + unit*4;
   GLuint tmp = rmesa->TexGenEnabled;

   rmesa->TexGenEnabled &= ~(RADEON_TEXGEN_TEXMAT_0_ENABLE<<unit);
   rmesa->TexGenEnabled &= ~(RADEON_TEXMAT_0_ENABLE<<unit);
   rmesa->TexGenEnabled &= ~(RADEON_TEXGEN_INPUT_MASK<<inputshift);
   rmesa->TexGenNeedNormals[unit] = 0;

   if ((texUnit->TexGenEnabled & (S_BIT|T_BIT)) == 0) {
      /* Disabled, no fallback:
       */
      rmesa->TexGenEnabled |= 
	 (RADEON_TEXGEN_INPUT_TEXCOORD_0+unit) << inputshift;
      return GL_TRUE;
   }
   else if (texUnit->TexGenEnabled & Q_BIT) {
      /* Very easy to do this, in fact would remove a fallback case
       * elsewhere, but I haven't done it yet...  Fallback: 
       */
      fprintf(stderr, "fallback Q_BIT\n");
      return GL_FALSE;
   }
   else if ((texUnit->TexGenEnabled & (S_BIT|T_BIT)) != (S_BIT|T_BIT) ||
	    texUnit->GenModeS != texUnit->GenModeT) {
      /* Mixed modes, fallback:
       */
      /* fprintf(stderr, "fallback mixed texgen\n"); */
      return GL_FALSE;
   }
   else
      rmesa->TexGenEnabled |= RADEON_TEXGEN_TEXMAT_0_ENABLE << unit;

   switch (texUnit->GenModeS) {
   case GL_OBJECT_LINEAR:
      rmesa->TexGenEnabled |= RADEON_TEXGEN_INPUT_OBJ << inputshift;
      set_texgen_matrix( rmesa, unit, 
			 texUnit->ObjectPlaneS,
			 texUnit->ObjectPlaneT);
      break;

   case GL_EYE_LINEAR:
      rmesa->TexGenEnabled |= RADEON_TEXGEN_INPUT_EYE << inputshift;
      set_texgen_matrix( rmesa, unit, 
			 texUnit->EyePlaneS,
			 texUnit->EyePlaneT);
      break;

   case GL_REFLECTION_MAP_NV:
      rmesa->TexGenNeedNormals[unit] = GL_TRUE;
      rmesa->TexGenEnabled |= RADEON_TEXGEN_INPUT_EYE_REFLECT<<inputshift;
      break;

   case GL_NORMAL_MAP_NV:
      rmesa->TexGenNeedNormals[unit] = GL_TRUE;
      rmesa->TexGenEnabled |= RADEON_TEXGEN_INPUT_EYE_NORMAL<<inputshift;
      break;

   case GL_SPHERE_MAP:
   default:
      /* Unsupported mode, fallback:
       */
      /*  fprintf(stderr, "fallback unsupported texgen\n"); */
      return GL_FALSE;
   }

   if (tmp != rmesa->TexGenEnabled) {
      rmesa->NewGLState |= _NEW_TEXTURE_MATRIX;
   }

   return GL_TRUE;
}


static void disable_tex( GLcontext *ctx, int unit )
{
   radeonContextPtr rmesa = RADEON_CONTEXT(ctx);

   if (rmesa->hw.ctx.cmd[CTX_PP_CNTL] & (RADEON_TEX_0_ENABLE<<unit)) {
      /* Texture unit disabled */
      if ( rmesa->state.texture.unit[unit].texobj != NULL ) {
	 /* The old texture is no longer bound to this texture unit.
	  * Mark it as such.
	  */

	 rmesa->state.texture.unit[unit].texobj->base.bound &= ~(1UL << unit);
	 rmesa->state.texture.unit[unit].texobj = NULL;
      }

      RADEON_STATECHANGE( rmesa, ctx );
      rmesa->hw.ctx.cmd[CTX_PP_CNTL] &= 
	  ~((RADEON_TEX_0_ENABLE | RADEON_TEX_BLEND_0_ENABLE) << unit);

      RADEON_STATECHANGE( rmesa, tcl );
      switch (unit) {
      case 0:
	 rmesa->hw.tcl.cmd[TCL_OUTPUT_VTXFMT] &= ~(RADEON_TCL_VTX_ST0 |
						   RADEON_TCL_VTX_Q0);
	    break;
      case 1:
	 rmesa->hw.tcl.cmd[TCL_OUTPUT_VTXFMT] &= ~(RADEON_TCL_VTX_ST1 |
						   RADEON_TCL_VTX_Q1);
	 break;
      default:
	 break;
      }


      if (rmesa->TclFallback & (RADEON_TCL_FALLBACK_TEXGEN_0<<unit)) {
	 TCL_FALLBACK( ctx, (RADEON_TCL_FALLBACK_TEXGEN_0<<unit), GL_FALSE);
	 rmesa->recheck_texgen[unit] = GL_TRUE;
      }



      {
	 GLuint inputshift = RADEON_TEXGEN_0_INPUT_SHIFT + unit*4;
	 GLuint tmp = rmesa->TexGenEnabled;

	 rmesa->TexGenEnabled &= ~(RADEON_TEXGEN_TEXMAT_0_ENABLE<<unit);
	 rmesa->TexGenEnabled &= ~(RADEON_TEXMAT_0_ENABLE<<unit);
	 rmesa->TexGenEnabled &= ~(RADEON_TEXGEN_INPUT_MASK<<inputshift);
	 rmesa->TexGenNeedNormals[unit] = 0;
	 rmesa->TexGenEnabled |= 
	     (RADEON_TEXGEN_INPUT_TEXCOORD_0+unit) << inputshift;

	 if (tmp != rmesa->TexGenEnabled) {
	    rmesa->recheck_texgen[unit] = GL_TRUE;
	    rmesa->NewGLState |= _NEW_TEXTURE_MATRIX;
	 }
      }
   }
}

static GLboolean enable_tex_2d( GLcontext *ctx, int unit )
{
   radeonContextPtr rmesa = RADEON_CONTEXT(ctx);
   struct gl_texture_unit *texUnit = &ctx->Texture.Unit[unit];
   struct gl_texture_object *tObj = texUnit->_Current;
   radeonTexObjPtr t = (radeonTexObjPtr) tObj->DriverData;

   /* Need to load the 2d images associated with this unit.
    */
   if (t->pp_txformat & RADEON_TXFORMAT_NON_POWER2) {
      t->pp_txformat &= ~RADEON_TXFORMAT_NON_POWER2;
      t->base.dirty_images[0] = ~0;
   }

   ASSERT(tObj->Target == GL_TEXTURE_2D || tObj->Target == GL_TEXTURE_1D);

   if ( t->base.dirty_images[0] ) {
      RADEON_FIREVERTICES( rmesa );
      radeonSetTexImages( rmesa, tObj );
      radeonUploadTexImages( rmesa, (radeonTexObjPtr) tObj->DriverData, 0 );
      if ( !t->base.memBlock ) 
	return GL_FALSE;
   }

   return GL_TRUE;
}

static GLboolean enable_tex_rect( GLcontext *ctx, int unit )
{
   radeonContextPtr rmesa = RADEON_CONTEXT(ctx);
   struct gl_texture_unit *texUnit = &ctx->Texture.Unit[unit];
   struct gl_texture_object *tObj = texUnit->_Current;
   radeonTexObjPtr t = (radeonTexObjPtr) tObj->DriverData;

   if (!(t->pp_txformat & RADEON_TXFORMAT_NON_POWER2)) {
      t->pp_txformat |= RADEON_TXFORMAT_NON_POWER2;
      t->base.dirty_images[0] = ~0;
   }

   ASSERT(tObj->Target == GL_TEXTURE_RECTANGLE_NV);

   if ( t->base.dirty_images[0] ) {
      RADEON_FIREVERTICES( rmesa );
      radeonSetTexImages( rmesa, tObj );
      radeonUploadTexImages( rmesa, (radeonTexObjPtr) tObj->DriverData, 0 );
      if ( !t->base.memBlock /* && !rmesa->prefer_gart_client_texturing  FIXME */ ) {
	 fprintf(stderr, "%s: upload failed\n", __FUNCTION__);
	 return GL_FALSE;
      }
   }

   return GL_TRUE;
}


static GLboolean update_tex_common( GLcontext *ctx, int unit )
{
   radeonContextPtr rmesa = RADEON_CONTEXT(ctx);
   struct gl_texture_unit *texUnit = &ctx->Texture.Unit[unit];
   struct gl_texture_object *tObj = texUnit->_Current;
   radeonTexObjPtr t = (radeonTexObjPtr) tObj->DriverData;
   GLenum format;

   /* Fallback if there's a texture border */
   if ( tObj->Image[tObj->BaseLevel]->Border > 0 ) {
      fprintf(stderr, "%s: border\n", __FUNCTION__);
      return GL_FALSE;
   }

   /* Update state if this is a different texture object to last
    * time.
    */
   if ( rmesa->state.texture.unit[unit].texobj != t ) {
      if ( rmesa->state.texture.unit[unit].texobj != NULL ) {
	 /* The old texture is no longer bound to this texture unit.
	  * Mark it as such.
	  */

	 rmesa->state.texture.unit[unit].texobj->base.bound &= 
	     ~(1UL << unit);
      }

      rmesa->state.texture.unit[unit].texobj = t;
      t->base.bound |= (1UL << unit);
      t->dirty_state |= 1<<unit;
      driUpdateTextureLRU( (driTextureObject *) t ); /* XXX: should be locked! */
   }


   /* Newly enabled?
    */
   if ( !(rmesa->hw.ctx.cmd[CTX_PP_CNTL] & (RADEON_TEX_0_ENABLE<<unit))) {
      RADEON_STATECHANGE( rmesa, ctx );
      rmesa->hw.ctx.cmd[CTX_PP_CNTL] |= 
	  (RADEON_TEX_0_ENABLE | RADEON_TEX_BLEND_0_ENABLE) << unit;

      RADEON_STATECHANGE( rmesa, tcl );

      if (unit == 0)
	  rmesa->hw.tcl.cmd[TCL_OUTPUT_VTXFMT] |= RADEON_TCL_VTX_ST0;
      else 
	  rmesa->hw.tcl.cmd[TCL_OUTPUT_VTXFMT] |= RADEON_TCL_VTX_ST1;

      rmesa->recheck_texgen[unit] = GL_TRUE;
   }

   if (t->dirty_state & (1<<unit)) {
      import_tex_obj_state( rmesa, unit, t );
   }

   if (rmesa->recheck_texgen[unit]) {
      GLboolean fallback = !radeon_validate_texgen( ctx, unit );
      TCL_FALLBACK( ctx, (RADEON_TCL_FALLBACK_TEXGEN_0<<unit), fallback);
      rmesa->recheck_texgen[unit] = 0;
      rmesa->NewGLState |= _NEW_TEXTURE_MATRIX;
   }

   format = tObj->Image[tObj->BaseLevel]->Format;
   if ( rmesa->state.texture.unit[unit].format != format ||
	rmesa->state.texture.unit[unit].envMode != texUnit->EnvMode ) {
      rmesa->state.texture.unit[unit].format = format;
      rmesa->state.texture.unit[unit].envMode = texUnit->EnvMode;
      if ( ! radeonUpdateTextureEnv( ctx, unit ) ) {
	 return GL_FALSE;
      }
   }

   FALLBACK( rmesa, RADEON_FALLBACK_BORDER_MODE, t->border_fallback );
   return !t->border_fallback;
}



static GLboolean radeonUpdateTextureUnit( GLcontext *ctx, int unit )
{
   struct gl_texture_unit *texUnit = &ctx->Texture.Unit[unit];

   TCL_FALLBACK( ctx, RADEON_TCL_FALLBACK_TEXRECT_0 << unit, 0 );

   if ( texUnit->_ReallyEnabled & (TEXTURE_RECT_BIT) ) {
      TCL_FALLBACK( ctx, RADEON_TCL_FALLBACK_TEXRECT_0 << unit, 1 );

      return (enable_tex_rect( ctx, unit ) &&
	      update_tex_common( ctx, unit ));
   }
   else if ( texUnit->_ReallyEnabled & (TEXTURE_1D_BIT | TEXTURE_2D_BIT) ) {
      return (enable_tex_2d( ctx, unit ) &&
	      update_tex_common( ctx, unit ));
   }
   else if ( texUnit->_ReallyEnabled ) {
      return GL_FALSE;
   }
   else {
      disable_tex( ctx, unit );
      return GL_TRUE;
   }
}

void radeonUpdateTextureState( GLcontext *ctx )
{
   radeonContextPtr rmesa = RADEON_CONTEXT(ctx);
   GLboolean ok;

   ok = (radeonUpdateTextureUnit( ctx, 0 ) &&
	 radeonUpdateTextureUnit( ctx, 1 ));

   FALLBACK( rmesa, RADEON_FALLBACK_TEXTURE, !ok );

   if (rmesa->TclFallback)
      radeonChooseVertexState( ctx );
}
