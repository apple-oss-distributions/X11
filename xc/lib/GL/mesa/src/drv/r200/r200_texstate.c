/* $XFree86: xc/lib/GL/mesa/src/drv/r200/r200_texstate.c,v 1.6 2004/01/23 03:57:05 dawes Exp $ */
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
#include "imports.h"
#include "context.h"
#include "macros.h"
#include "texformat.h"
#include "enums.h"

#include "r200_context.h"
#include "r200_state.h"
#include "r200_ioctl.h"
#include "r200_swtcl.h"
#include "r200_tex.h"
#include "r200_tcl.h"


#define R200_TXFORMAT_AL88      R200_TXFORMAT_AI88
#define R200_TXFORMAT_YCBCR     R200_TXFORMAT_YVYU422
#define R200_TXFORMAT_YCBCR_REV R200_TXFORMAT_VYUY422

#define _COLOR(f) \
    [ MESA_FORMAT_ ## f ] = { R200_TXFORMAT_ ## f, 0 }
#define _ALPHA(f) \
    [ MESA_FORMAT_ ## f ] = { R200_TXFORMAT_ ## f | R200_TXFORMAT_ALPHA_IN_MAP, 0 }
#define _YUV(f) \
    [ MESA_FORMAT_ ## f ] = { R200_TXFORMAT_ ## f, R200_YUV_TO_RGB }
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
static void r200SetTexImages( r200ContextPtr rmesa,
			      struct gl_texture_object *tObj )
{
   r200TexObjPtr t = (r200TexObjPtr)tObj->DriverData;
   const struct gl_texture_image *baseImage = tObj->Image[tObj->BaseLevel];
   GLint curOffset;
   GLint i;
   GLint numLevels;
   GLint log2Width, log2Height, log2Depth;

   /* Set the hardware texture format
    */

   t->pp_txformat &= ~(R200_TXFORMAT_FORMAT_MASK |
		       R200_TXFORMAT_ALPHA_IN_MAP);
   t->pp_txfilter &= ~R200_YUV_TO_RGB;

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

   /* Setup remaining cube face blits, if needed */
   if (tObj->Target == GL_TEXTURE_CUBE_MAP) {
      /* Round totalSize up to multiple of BLIT_WIDTH_BYTES */
      const GLuint faceSize = (t->base.totalSize + BLIT_WIDTH_BYTES - 1)
                              & ~(BLIT_WIDTH_BYTES-1);
      const GLuint lines = faceSize / BLIT_WIDTH_BYTES;
      GLuint face;
      /* reuse face 0 x/y/width/height - just adjust y */
      for (face = 1; face < 6; face++) {
         for (i = 0; i < numLevels; i++) {
            t->image[face][i].x =  t->image[0][i].x;
            t->image[face][i].y =  t->image[0][i].y + face * lines;
            t->image[face][i].width  = t->image[0][i].width;
            t->image[face][i].height = t->image[0][i].height;
         }
      }
      t->base.totalSize = 6 * faceSize; /* total texmem needed */
   }


   /* Hardware state:
    */
   t->pp_txfilter &= ~R200_MAX_MIP_LEVEL_MASK;
   t->pp_txfilter |= (numLevels - 1) << R200_MAX_MIP_LEVEL_SHIFT;

   t->pp_txformat &= ~(R200_TXFORMAT_WIDTH_MASK |
		       R200_TXFORMAT_HEIGHT_MASK |
                       R200_TXFORMAT_CUBIC_MAP_ENABLE |
                       R200_TXFORMAT_F5_WIDTH_MASK |
                       R200_TXFORMAT_F5_HEIGHT_MASK);
   t->pp_txformat |= ((log2Width << R200_TXFORMAT_WIDTH_SHIFT) |
		      (log2Height << R200_TXFORMAT_HEIGHT_SHIFT));

   t->pp_txformat_x &= ~(R200_DEPTH_LOG2_MASK | R200_TEXCOORD_MASK);
   if (tObj->Target == GL_TEXTURE_3D) {
      t->pp_txformat_x |= (log2Depth << R200_DEPTH_LOG2_SHIFT);
      t->pp_txformat_x |= R200_TEXCOORD_VOLUME;
   }
   else if (tObj->Target == GL_TEXTURE_CUBE_MAP) {
      ASSERT(log2Width == log2height);
      t->pp_txformat |= ((log2Width << R200_TXFORMAT_F5_WIDTH_SHIFT) |
                         (log2Height << R200_TXFORMAT_F5_HEIGHT_SHIFT) |
                         (R200_TXFORMAT_CUBIC_MAP_ENABLE));
      t->pp_txformat_x |= R200_TEXCOORD_CUBIC_ENV;
      t->pp_cubic_faces = ((log2Width << R200_FACE_WIDTH_1_SHIFT) |
                           (log2Height << R200_FACE_HEIGHT_1_SHIFT) |
                           (log2Width << R200_FACE_WIDTH_2_SHIFT) |
                           (log2Height << R200_FACE_HEIGHT_2_SHIFT) |
                           (log2Width << R200_FACE_WIDTH_3_SHIFT) |
                           (log2Height << R200_FACE_HEIGHT_3_SHIFT) |
                           (log2Width << R200_FACE_WIDTH_4_SHIFT) |
                           (log2Height << R200_FACE_HEIGHT_4_SHIFT));
   }

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

   /* FYI: r200UploadTexImages( rmesa, t ) used to be called here */
}



/* ================================================================
 * Texture combine functions
 */

#define R200_DISABLE		0
#define R200_REPLACE		1
#define R200_MODULATE		2
#define R200_DECAL		3
#define R200_BLEND		4
#define R200_ADD		5
#define R200_MAX_COMBFUNC	6

static GLuint r200_color_combine[][R200_MAX_COMBFUNC] =
{
   /* Unit 0:
    */
   {
      /* Disable combiner stage
       */
      (R200_TXC_ARG_A_ZERO  |
       R200_TXC_ARG_B_ZERO |
       R200_TXC_ARG_C_DIFFUSE_COLOR |
       R200_TXC_OP_MADD),

      /* GL_REPLACE = 0x00802800
       */
      (R200_TXC_ARG_A_ZERO |
       R200_TXC_ARG_B_ZERO |
       R200_TXC_ARG_C_R0_COLOR |
       R200_TXC_OP_MADD),

      /* GL_MODULATE = 0x00800142
       */
      (R200_TXC_ARG_A_DIFFUSE_COLOR | /* current starts in DIFFUSE */
       R200_TXC_ARG_B_R0_COLOR |
       R200_TXC_ARG_C_ZERO |
       R200_TXC_OP_MADD),

      /* GL_DECAL = 0x008c2d42
       */
      (R200_TXC_ARG_A_DIFFUSE_COLOR |
       R200_TXC_ARG_B_R0_COLOR |
       R200_TXC_ARG_C_R0_ALPHA |
       R200_TXC_OP_LERP),

      /* GL_BLEND = 0x008c2902
       */
      (R200_TXC_ARG_A_DIFFUSE_COLOR |
       R200_TXC_ARG_B_TFACTOR_COLOR |
       R200_TXC_ARG_C_R0_COLOR |
       R200_TXC_OP_LERP),

      /* GL_ADD = 0x00812802
       */
      (R200_TXC_ARG_A_DIFFUSE_COLOR |
       R200_TXC_ARG_B_ZERO |
       R200_TXC_ARG_C_R0_COLOR |
       R200_TXC_COMP_ARG_B |
       R200_TXC_OP_MADD),
   },

   /* Unit 1:
    */
   {
      /* Disable combiner stage
       */
      (R200_TXC_ARG_A_ZERO |
       R200_TXC_ARG_B_ZERO |
       R200_TXC_ARG_C_R0_COLOR |
       R200_TXC_OP_MADD),

      /* GL_REPLACE = 0x00803000
       */
      (R200_TXC_ARG_A_ZERO |
       R200_TXC_ARG_B_ZERO |
       R200_TXC_ARG_C_R1_COLOR |
       R200_TXC_OP_MADD),

      /* GL_MODULATE = 0x00800182
       */
      (R200_TXC_ARG_A_R0_COLOR | /* current in R0 thereafter */
       R200_TXC_ARG_B_R1_COLOR |
       R200_TXC_ARG_C_ZERO |
       R200_TXC_OP_MADD),

      /* GL_DECAL = 0x008c3582
       */
      (R200_TXC_ARG_A_R0_COLOR |
       R200_TXC_ARG_B_R1_COLOR |
       R200_TXC_ARG_C_R1_ALPHA |
       R200_TXC_OP_LERP),

      /* GL_BLEND = 0x008c3102
       */
      (R200_TXC_ARG_A_R0_COLOR |
       R200_TXC_ARG_B_TFACTOR_COLOR |
       R200_TXC_ARG_C_R1_COLOR |
       R200_TXC_OP_LERP),

      /* GL_ADD = 0x00813002
       */
      (R200_TXC_ARG_A_R0_COLOR |
       R200_TXC_ARG_B_ZERO |
       R200_TXC_ARG_C_R1_COLOR |
       R200_TXC_COMP_ARG_B |
       R200_TXC_OP_MADD),
   },

   /* Unit 2:
    */
   {
      /* Disable combiner stage
       */
      (R200_TXC_ARG_A_ZERO |
       R200_TXC_ARG_B_ZERO |
       R200_TXC_ARG_C_R0_COLOR |
       R200_TXC_OP_MADD),

      /* GL_REPLACE = 0x00803800
       */
      (R200_TXC_ARG_A_ZERO |
       R200_TXC_ARG_B_ZERO |
       R200_TXC_ARG_C_R2_COLOR |
       R200_TXC_OP_MADD),

      /* GL_MODULATE = 0x008001c2
       */
      (R200_TXC_ARG_A_R0_COLOR |
       R200_TXC_ARG_B_R2_COLOR |
       R200_TXC_ARG_C_ZERO |
       R200_TXC_OP_MADD),

      /* GL_DECAL = 0x008c3dc2
       */
      (R200_TXC_ARG_A_R0_COLOR |
       R200_TXC_ARG_B_R2_COLOR |
       R200_TXC_ARG_C_R2_ALPHA |
       R200_TXC_OP_LERP),

      /* GL_BLEND = 0x008c3902
       */
      (R200_TXC_ARG_A_R0_COLOR |
       R200_TXC_ARG_B_TFACTOR_COLOR |
       R200_TXC_ARG_C_R2_COLOR |
       R200_TXC_OP_LERP),

      /* GL_ADD = 0x00813802
       */
      (R200_TXC_ARG_A_R0_COLOR |
       R200_TXC_ARG_B_ZERO |
       R200_TXC_ARG_C_R2_COLOR |
       R200_TXC_COMP_ARG_B |
       R200_TXC_OP_MADD),
   }
};

static GLuint r200_alpha_combine[][R200_MAX_COMBFUNC] =
{
   /* Unit 0:
    */
   {
      /* Disable combiner stage
       */
      (R200_TXA_ARG_A_ZERO |
       R200_TXA_ARG_B_ZERO |
       R200_TXA_ARG_C_DIFFUSE_ALPHA |
       R200_TXA_OP_MADD),


      /* GL_REPLACE = 0x00800500
       */
      (R200_TXA_ARG_A_ZERO |
       R200_TXA_ARG_B_ZERO |
       R200_TXA_ARG_C_R0_ALPHA |
       R200_TXA_OP_MADD),

      /* GL_MODULATE = 0x00800051
       */
      (R200_TXA_ARG_A_DIFFUSE_ALPHA |
       R200_TXA_ARG_B_R0_ALPHA |
       R200_TXA_ARG_C_ZERO |
       R200_TXA_OP_MADD),

      /* GL_DECAL = 0x00800100
       */
      (R200_TXA_ARG_A_ZERO |
       R200_TXA_ARG_B_ZERO |
       R200_TXA_ARG_C_DIFFUSE_ALPHA |
       R200_TXA_OP_MADD),

      /* GL_BLEND = 0x00800051
       */
      (R200_TXA_ARG_A_DIFFUSE_ALPHA |
       R200_TXA_ARG_B_TFACTOR_ALPHA |
       R200_TXA_ARG_C_R0_ALPHA |
       R200_TXA_OP_LERP),

      /* GL_ADD = 0x00800051
       */
      (R200_TXA_ARG_A_DIFFUSE_ALPHA |
       R200_TXA_ARG_B_ZERO |
       R200_TXA_ARG_C_R0_ALPHA |
       R200_TXA_COMP_ARG_B |
       R200_TXA_OP_MADD),
   },

   /* Unit 1:
    */
   {
      /* Disable combiner stage
       */
      (R200_TXA_ARG_A_ZERO |
       R200_TXA_ARG_B_ZERO |
       R200_TXA_ARG_C_R0_ALPHA |
       R200_TXA_OP_MADD),

      /* GL_REPLACE = 0x00800600
       */
      (R200_TXA_ARG_A_ZERO |
       R200_TXA_ARG_B_ZERO |
       R200_TXA_ARG_C_R1_ALPHA |
       R200_TXA_OP_MADD),

      /* GL_MODULATE = 0x00800061
       */
      (R200_TXA_ARG_A_R0_ALPHA |
       R200_TXA_ARG_B_R1_ALPHA |
       R200_TXA_ARG_C_ZERO |
       R200_TXA_OP_MADD),

      /* GL_DECAL = 0x00800100
       */
      (R200_TXA_ARG_A_ZERO |
       R200_TXA_ARG_B_ZERO |
       R200_TXA_ARG_C_R0_ALPHA |
       R200_TXA_OP_MADD),

      /* GL_BLEND = 0x00800061
       */
      (R200_TXA_ARG_A_R0_ALPHA |
       R200_TXA_ARG_B_TFACTOR_ALPHA |
       R200_TXA_ARG_C_R1_ALPHA |
       R200_TXA_OP_LERP),

      /* GL_ADD = 0x00800061
       */
      (R200_TXA_ARG_A_R0_ALPHA |
       R200_TXA_ARG_B_ZERO |
       R200_TXA_ARG_C_R1_ALPHA |
       R200_TXA_COMP_ARG_B |
       R200_TXA_OP_MADD),
   },

   /* Unit 2:
    */
   {
      /* Disable combiner stage
       */
      (R200_TXA_ARG_A_ZERO |
       R200_TXA_ARG_B_ZERO |
       R200_TXA_ARG_C_R0_ALPHA |
       R200_TXA_OP_MADD),

      /* GL_REPLACE = 0x00800700
       */
      (R200_TXA_ARG_A_ZERO |
       R200_TXA_ARG_B_ZERO |
       R200_TXA_ARG_C_R2_ALPHA |
       R200_TXA_OP_MADD),

      /* GL_MODULATE = 0x00800071
       */
      (R200_TXA_ARG_A_R0_ALPHA |
       R200_TXA_ARG_B_R2_ALPHA |
       R200_TXA_ARG_C_ZERO |
       R200_TXA_OP_MADD),

      /* GL_DECAL = 0x00800100
       */
      (R200_TXA_ARG_A_ZERO |
       R200_TXA_ARG_B_ZERO |
       R200_TXA_ARG_C_R0_ALPHA |
       R200_TXA_OP_MADD),

      /* GL_BLEND = 0x00800071
       */
      (R200_TXA_ARG_A_R0_ALPHA |
       R200_TXA_ARG_B_TFACTOR_ALPHA |
       R200_TXA_ARG_C_R2_ALPHA |
       R200_TXA_OP_LERP),

      /* GL_ADD = 0x00800021
       */
      (R200_TXA_ARG_A_R0_ALPHA |
       R200_TXA_ARG_B_ZERO |
       R200_TXA_ARG_C_R2_ALPHA |
       R200_TXA_COMP_ARG_B |
       R200_TXA_OP_MADD),
   }
};


/* GL_ARB_texture_env_combine support
 */

/* The color tables have combine functions for GL_SRC_COLOR,
 * GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA and GL_ONE_MINUS_SRC_ALPHA.
 */
static GLuint r200_register_color[][R200_MAX_TEXTURE_UNITS] =
{
   {
      R200_TXC_ARG_A_R0_COLOR,
      R200_TXC_ARG_A_R1_COLOR,
      R200_TXC_ARG_A_R2_COLOR
   },
   {
      R200_TXC_ARG_A_R0_COLOR | R200_TXC_COMP_ARG_A,
      R200_TXC_ARG_A_R1_COLOR | R200_TXC_COMP_ARG_A,
      R200_TXC_ARG_A_R2_COLOR | R200_TXC_COMP_ARG_A
   },
   {
      R200_TXC_ARG_A_R0_ALPHA,
      R200_TXC_ARG_A_R1_ALPHA,
      R200_TXC_ARG_A_R2_ALPHA
   },
   {
      R200_TXC_ARG_A_R0_ALPHA | R200_TXC_COMP_ARG_A,
      R200_TXC_ARG_A_R1_ALPHA | R200_TXC_COMP_ARG_A,
      R200_TXC_ARG_A_R2_ALPHA | R200_TXC_COMP_ARG_A
   },
};

static GLuint r200_tfactor_color[] =
{
   R200_TXC_ARG_A_TFACTOR_COLOR,
   R200_TXC_ARG_A_TFACTOR_COLOR | R200_TXC_COMP_ARG_A,
   R200_TXC_ARG_A_TFACTOR_ALPHA,
   R200_TXC_ARG_A_TFACTOR_ALPHA | R200_TXC_COMP_ARG_A
};

static GLuint r200_primary_color[] =
{
   R200_TXC_ARG_A_DIFFUSE_COLOR,
   R200_TXC_ARG_A_DIFFUSE_COLOR | R200_TXC_COMP_ARG_A,
   R200_TXC_ARG_A_DIFFUSE_ALPHA,
   R200_TXC_ARG_A_DIFFUSE_ALPHA | R200_TXC_COMP_ARG_A
};

/* GL_ZERO table - indices 0-3
 * GL_ONE  table - indices 1-4
 */
static GLuint r200_zero_color[] =
{
   R200_TXC_ARG_A_ZERO,
   R200_TXC_ARG_A_ZERO | R200_TXC_COMP_ARG_A,
   R200_TXC_ARG_A_ZERO,
   R200_TXC_ARG_A_ZERO | R200_TXC_COMP_ARG_A,
   R200_TXC_ARG_A_ZERO
};

/* The alpha tables only have GL_SRC_ALPHA and GL_ONE_MINUS_SRC_ALPHA.
 */
static GLuint r200_register_alpha[][R200_MAX_TEXTURE_UNITS] =
{
   {
      R200_TXA_ARG_A_R0_ALPHA,
      R200_TXA_ARG_A_R1_ALPHA,
      R200_TXA_ARG_A_R2_ALPHA
   },
   {
      R200_TXA_ARG_A_R0_ALPHA | R200_TXA_COMP_ARG_A,
      R200_TXA_ARG_A_R1_ALPHA | R200_TXA_COMP_ARG_A,
      R200_TXA_ARG_A_R2_ALPHA | R200_TXA_COMP_ARG_A
   },
};

static GLuint r200_tfactor_alpha[] =
{
   R200_TXA_ARG_A_TFACTOR_ALPHA,
   R200_TXA_ARG_A_TFACTOR_ALPHA | R200_TXA_COMP_ARG_A
};

static GLuint r200_primary_alpha[] =
{
   R200_TXA_ARG_A_DIFFUSE_ALPHA,
   R200_TXA_ARG_A_DIFFUSE_ALPHA | R200_TXA_COMP_ARG_A
};

/* GL_ZERO table - indices 0-1
 * GL_ONE  table - indices 1-2
 */
static GLuint r200_zero_alpha[] =
{
   R200_TXA_ARG_A_ZERO,
   R200_TXA_ARG_A_ZERO | R200_TXA_COMP_ARG_A,
   R200_TXA_ARG_A_ZERO,
};


/* Extract the arg from slot A, shift it into the correct argument slot
 * and set the corresponding complement bit.
 */
#define R200_COLOR_ARG( n, arg )			\
do {							\
   color_combine |=					\
      ((color_arg[n] & R200_TXC_ARG_A_MASK)		\
       << R200_TXC_ARG_##arg##_SHIFT);			\
   color_combine |=					\
      ((color_arg[n] >> R200_TXC_COMP_ARG_A_SHIFT)	\
       << R200_TXC_COMP_ARG_##arg##_SHIFT);		\
} while (0)

#define R200_ALPHA_ARG( n, arg )			\
do {							\
   alpha_combine |=					\
      ((alpha_arg[n] & R200_TXA_ARG_A_MASK)		\
       << R200_TXA_ARG_##arg##_SHIFT);			\
   alpha_combine |=					\
      ((alpha_arg[n] >> R200_TXA_COMP_ARG_A_SHIFT)	\
       << R200_TXA_COMP_ARG_##arg##_SHIFT);		\
} while (0)


/* ================================================================
 * Texture unit state management
 */

static GLboolean r200UpdateTextureEnv( GLcontext *ctx, int unit )
{
   r200ContextPtr rmesa = R200_CONTEXT(ctx);
   const struct gl_texture_unit *texUnit = &ctx->Texture.Unit[unit];
   GLuint color_combine, alpha_combine;
   GLuint color_scale = rmesa->hw.pix[unit].cmd[PIX_PP_TXCBLEND2];
   GLuint alpha_scale = rmesa->hw.pix[unit].cmd[PIX_PP_TXABLEND2];

   /* texUnit->_Current can be NULL if and only if the texture unit is
    * not actually enabled.
    */
   assert( (texUnit->_ReallyEnabled == 0)
	   || (texUnit->_Current != NULL) );

   if ( R200_DEBUG & DEBUG_TEXTURE ) {
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
      color_combine = r200_color_combine[unit][R200_DISABLE];
      alpha_combine = r200_alpha_combine[unit][R200_DISABLE];
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
	    color_combine = r200_color_combine[unit][R200_REPLACE];
	    alpha_combine = r200_alpha_combine[unit][R200_REPLACE];
	    break;
	 case GL_ALPHA:
	    color_combine = r200_color_combine[unit][R200_DISABLE];
	    alpha_combine = r200_alpha_combine[unit][R200_REPLACE];
	    break;
	 case GL_LUMINANCE:
	 case GL_RGB:
	 case GL_YCBCR_MESA:
	    color_combine = r200_color_combine[unit][R200_REPLACE];
	    alpha_combine = r200_alpha_combine[unit][R200_DISABLE];
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
	    color_combine = r200_color_combine[unit][R200_MODULATE];
	    alpha_combine = r200_alpha_combine[unit][R200_MODULATE];
	    break;
	 case GL_ALPHA:
	    color_combine = r200_color_combine[unit][R200_DISABLE];
	    alpha_combine = r200_alpha_combine[unit][R200_MODULATE];
	    break;
	 case GL_RGB:
	 case GL_LUMINANCE:
	 case GL_YCBCR_MESA:
	    color_combine = r200_color_combine[unit][R200_MODULATE];
	    alpha_combine = r200_alpha_combine[unit][R200_DISABLE];
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
	    color_combine = r200_color_combine[unit][R200_DECAL];
	    alpha_combine = r200_alpha_combine[unit][R200_DISABLE];
	    break;
	 case GL_ALPHA:
	 case GL_LUMINANCE:
	 case GL_LUMINANCE_ALPHA:
	 case GL_INTENSITY:
	    color_combine = r200_color_combine[unit][R200_DISABLE];
	    alpha_combine = r200_alpha_combine[unit][R200_DISABLE];
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
	    color_combine = r200_color_combine[unit][R200_BLEND];
	    alpha_combine = r200_alpha_combine[unit][R200_MODULATE];
	    break;
	 case GL_ALPHA:
	    color_combine = r200_color_combine[unit][R200_DISABLE];
	    alpha_combine = r200_alpha_combine[unit][R200_MODULATE];
	    break;
	 case GL_INTENSITY:
	    color_combine = r200_color_combine[unit][R200_BLEND];
	    alpha_combine = r200_alpha_combine[unit][R200_BLEND];
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
	    color_combine = r200_color_combine[unit][R200_ADD];
	    alpha_combine = r200_alpha_combine[unit][R200_MODULATE];
	    break;
	 case GL_ALPHA:
	    color_combine = r200_color_combine[unit][R200_DISABLE];
	    alpha_combine = r200_alpha_combine[unit][R200_MODULATE];
	    break;
	 case GL_INTENSITY:
	    color_combine = r200_color_combine[unit][R200_ADD];
	    alpha_combine = r200_alpha_combine[unit][R200_ADD];
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
	       color_arg[i] = r200_register_color[op][unit];
	       break;
	    case GL_CONSTANT:
	       color_arg[i] = r200_tfactor_color[op];
	       break;
	    case GL_PRIMARY_COLOR:
	       color_arg[i] = r200_primary_color[op];
	       break;
	    case GL_PREVIOUS:
	       if (unit == 0)
		  color_arg[i] = r200_primary_color[op];
	       else
		  color_arg[i] = r200_register_color[op][0];
	       break;
	    case GL_ZERO:
	       color_arg[i] = r200_zero_color[op];
	       break;
	    case GL_ONE:
	       color_arg[i] = r200_zero_color[op+1];
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
	       alpha_arg[i] = r200_register_alpha[op][unit];
	       break;
	    case GL_CONSTANT:
	       alpha_arg[i] = r200_tfactor_alpha[op];
	       break;
	    case GL_PRIMARY_COLOR:
	       alpha_arg[i] = r200_primary_alpha[op];
	       break;
	    case GL_PREVIOUS:
	       if (unit == 0)
		  alpha_arg[i] = r200_primary_alpha[op];
	       else
		  alpha_arg[i] = r200_register_alpha[op][0];
	       break;
	    case GL_ZERO:
	       alpha_arg[i] = r200_zero_alpha[op];
	       break;
	    case GL_ONE:
	       alpha_arg[i] = r200_zero_alpha[op+1];
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
	    color_combine = (R200_TXC_ARG_A_ZERO |
			     R200_TXC_ARG_B_ZERO |
			     R200_TXC_OP_MADD);
	    R200_COLOR_ARG( 0, C );
	    break;
	 case GL_MODULATE:
	    color_combine = (R200_TXC_ARG_C_ZERO |
			     R200_TXC_OP_MADD);
	    R200_COLOR_ARG( 0, A );
	    R200_COLOR_ARG( 1, B );
	    break;
	 case GL_ADD:
	    color_combine = (R200_TXC_ARG_B_ZERO |
			     R200_TXC_COMP_ARG_B | 
			     R200_TXC_OP_MADD);
	    R200_COLOR_ARG( 0, A );
	    R200_COLOR_ARG( 1, C );
	    break;
	 case GL_ADD_SIGNED:
	    color_combine = (R200_TXC_ARG_B_ZERO |
			     R200_TXC_COMP_ARG_B |
			     R200_TXC_BIAS_ARG_C |	/* new */
			     R200_TXC_OP_MADD); /* was ADDSIGNED */
	    R200_COLOR_ARG( 0, A );
	    R200_COLOR_ARG( 1, C );
	    break;
	 case GL_SUBTRACT:
	    color_combine = (R200_TXC_ARG_B_ZERO |
			     R200_TXC_COMP_ARG_B | 
			     R200_TXC_NEG_ARG_C |
			     R200_TXC_OP_MADD);
	    R200_COLOR_ARG( 0, A );
	    R200_COLOR_ARG( 1, C );
	    break;
	 case GL_INTERPOLATE:
	    color_combine = (R200_TXC_OP_LERP);
	    R200_COLOR_ARG( 0, B );
	    R200_COLOR_ARG( 1, A );
	    R200_COLOR_ARG( 2, C );
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
	    /* DOT3 works differently on R200 than on R100.  On R100, just
	     * setting the DOT3 mode did everything for you.  On R200, the
	     * driver has to enable the biasing (the -0.5 in the combine
	     * equation), and it has add the 4x scale factor.  The hardware
	     * only supports up to 8x in the post filter, so 2x part of it
	     * happens on the inputs going into the combiner.
	     */

	    RGBshift++;
	    Ashift = RGBshift;

	    color_combine = (R200_TXC_ARG_C_ZERO |
			     R200_TXC_OP_DOT3 |
			     R200_TXC_BIAS_ARG_A |
			     R200_TXC_BIAS_ARG_B |
			     R200_TXC_SCALE_ARG_A |
			     R200_TXC_SCALE_ARG_B);
	    R200_COLOR_ARG( 0, A );
	    R200_COLOR_ARG( 1, B );
	    break;

	 case GL_MODULATE_ADD_ATI:
	    color_combine = (R200_TXC_OP_MADD);
	    R200_COLOR_ARG( 0, A );
	    R200_COLOR_ARG( 1, C );
	    R200_COLOR_ARG( 2, B );
	    break;
	 case GL_MODULATE_SIGNED_ADD_ATI:
	    color_combine = (R200_TXC_BIAS_ARG_C |	/* new */
			     R200_TXC_OP_MADD); /* was ADDSIGNED */
	    R200_COLOR_ARG( 0, A );
	    R200_COLOR_ARG( 1, C );
	    R200_COLOR_ARG( 2, B );
	    break;
	 case GL_MODULATE_SUBTRACT_ATI:
	    color_combine = (R200_TXC_NEG_ARG_C |
			     R200_TXC_OP_MADD);
	    R200_COLOR_ARG( 0, A );
	    R200_COLOR_ARG( 1, C );
	    R200_COLOR_ARG( 2, B );
	    break;
	 default:
	    return GL_FALSE;
	 }

	 switch ( texUnit->CombineModeA ) {
	 case GL_REPLACE:
	    alpha_combine = (R200_TXA_ARG_A_ZERO |
			     R200_TXA_ARG_B_ZERO |
			     R200_TXA_OP_MADD);
	    R200_ALPHA_ARG( 0, C );
	    break;
	 case GL_MODULATE:
	    alpha_combine = (R200_TXA_ARG_C_ZERO |
			     R200_TXA_OP_MADD);
	    R200_ALPHA_ARG( 0, A );
	    R200_ALPHA_ARG( 1, B );
	    break;
	 case GL_ADD:
	    alpha_combine = (R200_TXA_ARG_B_ZERO |
			     R200_TXA_COMP_ARG_B |
			     R200_TXA_OP_MADD);
	    R200_ALPHA_ARG( 0, A );
	    R200_ALPHA_ARG( 1, C );
	    break;
	 case GL_ADD_SIGNED:
	    alpha_combine = (R200_TXA_ARG_B_ZERO |
			     R200_TXA_COMP_ARG_B |
			     R200_TXA_BIAS_ARG_C |	/* new */
			     R200_TXA_OP_MADD); /* was ADDSIGNED */
	    R200_ALPHA_ARG( 0, A );
	    R200_ALPHA_ARG( 1, C );
	    break;
	 case GL_SUBTRACT:
	    alpha_combine = (R200_TXA_ARG_B_ZERO |
			     R200_TXA_COMP_ARG_B |
			     R200_TXA_NEG_ARG_C |
			     R200_TXA_OP_MADD);
	    R200_ALPHA_ARG( 0, A );
	    R200_ALPHA_ARG( 1, C );
	    break;
	 case GL_INTERPOLATE:
	    alpha_combine = (R200_TXA_OP_LERP);
	    R200_ALPHA_ARG( 0, B );
	    R200_ALPHA_ARG( 1, A );
	    R200_ALPHA_ARG( 2, C );
	    break;

	 case GL_MODULATE_ADD_ATI:
	    alpha_combine = (R200_TXA_OP_MADD);
	    R200_ALPHA_ARG( 0, A );
	    R200_ALPHA_ARG( 1, C );
	    R200_ALPHA_ARG( 2, B );
	    break;
	 case GL_MODULATE_SIGNED_ADD_ATI:
	    alpha_combine = (R200_TXA_BIAS_ARG_C |	/* new */
			     R200_TXA_OP_MADD); /* was ADDSIGNED */
	    R200_ALPHA_ARG( 0, A );
	    R200_ALPHA_ARG( 1, C );
	    R200_ALPHA_ARG( 2, B );
	    break;
	 case GL_MODULATE_SUBTRACT_ATI:
	    alpha_combine = (R200_TXA_NEG_ARG_C |
			     R200_TXA_OP_MADD);
	    R200_ALPHA_ARG( 0, A );
	    R200_ALPHA_ARG( 1, C );
	    R200_ALPHA_ARG( 2, B );
	    break;
	 default:
	    return GL_FALSE;
	 }

	 if ( (texUnit->CombineModeRGB == GL_DOT3_RGB_EXT)
	      || (texUnit->CombineModeRGB == GL_DOT3_RGB) ) {
	    alpha_scale |= R200_TXA_DOT_ALPHA;
	 }

	 /* Step 3:
	  * Apply the scale factor.
	  */
	 color_scale &= ~R200_TXC_SCALE_MASK;
	 alpha_scale &= ~R200_TXA_SCALE_MASK;
	 color_scale |= (RGBshift << R200_TXC_SCALE_SHIFT);
	 alpha_scale |= (Ashift   << R200_TXA_SCALE_SHIFT);

	 /* All done!
	  */
	 break;

      default:
	 return GL_FALSE;
      }
   }

   if ( rmesa->hw.pix[unit].cmd[PIX_PP_TXCBLEND] != color_combine ||
	rmesa->hw.pix[unit].cmd[PIX_PP_TXABLEND] != alpha_combine ||
	rmesa->hw.pix[unit].cmd[PIX_PP_TXCBLEND2] != color_scale ||
	rmesa->hw.pix[unit].cmd[PIX_PP_TXABLEND2] != alpha_scale) {
      R200_STATECHANGE( rmesa, pix[unit] );
      rmesa->hw.pix[unit].cmd[PIX_PP_TXCBLEND] = color_combine;
      rmesa->hw.pix[unit].cmd[PIX_PP_TXABLEND] = alpha_combine;
      rmesa->hw.pix[unit].cmd[PIX_PP_TXCBLEND2] = color_scale;
      rmesa->hw.pix[unit].cmd[PIX_PP_TXABLEND2] = alpha_scale;
   }

   return GL_TRUE;
}

#define TEXOBJ_TXFILTER_MASK (R200_MAX_MIP_LEVEL_MASK |		\
			      R200_MIN_FILTER_MASK | 		\
			      R200_MAG_FILTER_MASK |		\
			      R200_MAX_ANISO_MASK |		\
			      R200_YUV_TO_RGB |			\
			      R200_YUV_TEMPERATURE_MASK |	\
			      R200_CLAMP_S_MASK | 		\
			      R200_CLAMP_T_MASK | 		\
			      R200_BORDER_MODE_D3D )

#define TEXOBJ_TXFORMAT_MASK (R200_TXFORMAT_WIDTH_MASK |	\
			      R200_TXFORMAT_HEIGHT_MASK |	\
			      R200_TXFORMAT_FORMAT_MASK |	\
                              R200_TXFORMAT_F5_WIDTH_MASK |	\
                              R200_TXFORMAT_F5_HEIGHT_MASK |	\
			      R200_TXFORMAT_ALPHA_IN_MAP |	\
			      R200_TXFORMAT_CUBIC_MAP_ENABLE |	\
                              R200_TXFORMAT_NON_POWER2)

#define TEXOBJ_TXFORMAT_X_MASK (R200_DEPTH_LOG2_MASK |		\
                                R200_TEXCOORD_MASK |		\
                                R200_VOLUME_FILTER_MASK)


static void import_tex_obj_state( r200ContextPtr rmesa,
				  int unit,
				  r200TexObjPtr texobj )
{
   GLuint *cmd = R200_DB_STATE( tex[unit] );

   cmd[TEX_PP_TXFILTER] &= ~TEXOBJ_TXFILTER_MASK;
   cmd[TEX_PP_TXFILTER] |= texobj->pp_txfilter & TEXOBJ_TXFILTER_MASK;
   cmd[TEX_PP_TXFORMAT] &= ~TEXOBJ_TXFORMAT_MASK;
   cmd[TEX_PP_TXFORMAT] |= texobj->pp_txformat & TEXOBJ_TXFORMAT_MASK;
   cmd[TEX_PP_TXFORMAT_X] &= ~TEXOBJ_TXFORMAT_X_MASK;
   cmd[TEX_PP_TXFORMAT_X] |= texobj->pp_txformat_x & TEXOBJ_TXFORMAT_X_MASK;
   cmd[TEX_PP_TXSIZE] = texobj->pp_txsize; /* NPOT only! */
   cmd[TEX_PP_TXPITCH] = texobj->pp_txpitch; /* NPOT only! */
   cmd[TEX_PP_TXOFFSET] = texobj->pp_txoffset;
   cmd[TEX_PP_BORDER_COLOR] = texobj->pp_border_color;
   R200_DB_STATECHANGE( rmesa, &rmesa->hw.tex[unit] );

   if (texobj->base.tObj->Target == GL_TEXTURE_CUBE_MAP) {
      GLuint *cube_cmd = R200_DB_STATE( cube[unit] );
      GLuint bytesPerFace = texobj->base.totalSize / 6;
      ASSERT(texobj->totalSize % 6 == 0);
      cube_cmd[CUBE_PP_CUBIC_FACES] = texobj->pp_cubic_faces;
      cube_cmd[CUBE_PP_CUBIC_OFFSET_F1] = texobj->pp_txoffset + 1 * bytesPerFace;
      cube_cmd[CUBE_PP_CUBIC_OFFSET_F2] = texobj->pp_txoffset + 2 * bytesPerFace;
      cube_cmd[CUBE_PP_CUBIC_OFFSET_F3] = texobj->pp_txoffset + 3 * bytesPerFace;
      cube_cmd[CUBE_PP_CUBIC_OFFSET_F4] = texobj->pp_txoffset + 4 * bytesPerFace;
      cube_cmd[CUBE_PP_CUBIC_OFFSET_F5] = texobj->pp_txoffset + 5 * bytesPerFace;
      R200_DB_STATECHANGE( rmesa, &rmesa->hw.cube[unit] );
   }

   texobj->dirty_state &= ~(1<<unit);
}




static void set_texgen_matrix( r200ContextPtr rmesa, 
			       GLuint unit,
			       const GLfloat *s_plane,
			       const GLfloat *t_plane,
			       const GLfloat *r_plane )
{
   static const GLfloat scale_identity[4] = { 1,1,1,1 };

   if (!TEST_EQ_4V( s_plane, scale_identity) ||
       !TEST_EQ_4V( t_plane, scale_identity) ||
       !TEST_EQ_4V( r_plane, scale_identity)) {
      rmesa->TexGenEnabled |= R200_TEXMAT_0_ENABLE<<unit;
      rmesa->TexGenMatrix[unit].m[0]  = s_plane[0];
      rmesa->TexGenMatrix[unit].m[4]  = s_plane[1];
      rmesa->TexGenMatrix[unit].m[8]  = s_plane[2];
      rmesa->TexGenMatrix[unit].m[12] = s_plane[3];

      rmesa->TexGenMatrix[unit].m[1]  = t_plane[0];
      rmesa->TexGenMatrix[unit].m[5]  = t_plane[1];
      rmesa->TexGenMatrix[unit].m[9]  = t_plane[2];
      rmesa->TexGenMatrix[unit].m[13] = t_plane[3];

      /* NOTE: r_plane goes in the 4th row, not 3rd! */
      rmesa->TexGenMatrix[unit].m[3]  = r_plane[0];
      rmesa->TexGenMatrix[unit].m[7]  = r_plane[1];
      rmesa->TexGenMatrix[unit].m[11] = r_plane[2];
      rmesa->TexGenMatrix[unit].m[15] = r_plane[3];

      rmesa->NewGLState |= _NEW_TEXTURE_MATRIX;
   }
}

/* Need this special matrix to get correct reflection map coords */
static void
set_texgen_reflection_matrix( r200ContextPtr rmesa, GLuint unit )
{
   static const GLfloat m[16] = {
      -1,  0,  0,  0,
       0, -1,  0,  0,
       0,  0,  0, -1,
       0,  0, -1,  0 };
   _math_matrix_loadf( &(rmesa->TexGenMatrix[unit]), m);
   _math_matrix_analyse( &(rmesa->TexGenMatrix[unit]) );
   rmesa->TexGenEnabled |= R200_TEXMAT_0_ENABLE<<unit;
}

/* Need this special matrix to get correct normal map coords */
static void
set_texgen_normal_map_matrix( r200ContextPtr rmesa, GLuint unit )
{
   static const GLfloat m[16] = {
      1, 0, 0, 0,
      0, 1, 0, 0,
      0, 0, 0, 1,
      0, 0, 1, 0 };
   _math_matrix_loadf( &(rmesa->TexGenMatrix[unit]), m);
   _math_matrix_analyse( &(rmesa->TexGenMatrix[unit]) );
   rmesa->TexGenEnabled |= R200_TEXMAT_0_ENABLE<<unit;
}


/* Ignoring the Q texcoord for now.
 *
 * Returns GL_FALSE if fallback required.  
 */
static GLboolean r200_validate_texgen( GLcontext *ctx, GLuint unit )
{  
   r200ContextPtr rmesa = R200_CONTEXT(ctx);
   const struct gl_texture_unit *texUnit = &ctx->Texture.Unit[unit];
   GLuint inputshift = R200_TEXGEN_0_INPUT_SHIFT + unit*4;
   GLuint tmp = rmesa->TexGenEnabled;

   rmesa->TexGenCompSel &= ~(R200_OUTPUT_TEX_0 << unit);
   rmesa->TexGenEnabled &= ~(R200_TEXGEN_TEXMAT_0_ENABLE<<unit);
   rmesa->TexGenEnabled &= ~(R200_TEXMAT_0_ENABLE<<unit);
   rmesa->TexGenInputs &= ~(R200_TEXGEN_INPUT_MASK<<inputshift);
   rmesa->TexGenNeedNormals[unit] = 0;

   if (0) 
      fprintf(stderr, "%s unit %d\n", __FUNCTION__, unit);

   if ((texUnit->TexGenEnabled & (S_BIT|T_BIT|R_BIT)) == 0) {
      /* Disabled, no fallback:
       */
      rmesa->TexGenInputs |= 
	 (R200_TEXGEN_INPUT_TEXCOORD_0+unit) << inputshift;
      return GL_TRUE;
   }
   else if (texUnit->TexGenEnabled & Q_BIT) {
      /* Very easy to do this, in fact would remove a fallback case
       * elsewhere, but I haven't done it yet...  Fallback: 
       */
      /*fprintf(stderr, "fallback Q_BIT\n");*/
      return GL_FALSE;
   }
   else if (texUnit->TexGenEnabled == (S_BIT|T_BIT) &&
	    texUnit->GenModeS == texUnit->GenModeT) {
      /* OK */
      rmesa->TexGenEnabled |= R200_TEXGEN_TEXMAT_0_ENABLE << unit;
      /* continue */
   }
   else if (texUnit->TexGenEnabled == (S_BIT|T_BIT|R_BIT) &&
	    texUnit->GenModeS == texUnit->GenModeT &&
            texUnit->GenModeT == texUnit->GenModeR) {
      /* OK */
      rmesa->TexGenEnabled |= R200_TEXGEN_TEXMAT_0_ENABLE << unit;
      /* continue */
   }
   else {
      /* Mixed modes, fallback:
       */
      /* fprintf(stderr, "fallback mixed texgen\n"); */
      return GL_FALSE;
   }

   rmesa->TexGenEnabled |= R200_TEXGEN_TEXMAT_0_ENABLE << unit;

   switch (texUnit->GenModeS) {
   case GL_OBJECT_LINEAR:
      rmesa->TexGenInputs |= R200_TEXGEN_INPUT_OBJ << inputshift;
      set_texgen_matrix( rmesa, unit, 
			 texUnit->ObjectPlaneS,
			 texUnit->ObjectPlaneT,
                         texUnit->ObjectPlaneR);
      break;

   case GL_EYE_LINEAR:
      rmesa->TexGenInputs |= R200_TEXGEN_INPUT_EYE << inputshift;
      set_texgen_matrix( rmesa, unit, 
			 texUnit->EyePlaneS,
			 texUnit->EyePlaneT,
			 texUnit->EyePlaneR);
      break;

   case GL_REFLECTION_MAP_NV:
      rmesa->TexGenNeedNormals[unit] = GL_TRUE;
      rmesa->TexGenInputs |= R200_TEXGEN_INPUT_EYE_REFLECT<<inputshift;
      set_texgen_reflection_matrix(rmesa, unit);
      break;

   case GL_NORMAL_MAP_NV:
      rmesa->TexGenNeedNormals[unit] = GL_TRUE;
      rmesa->TexGenInputs |= R200_TEXGEN_INPUT_EYE_NORMAL<<inputshift;
      set_texgen_normal_map_matrix(rmesa, unit);
      break;

   case GL_SPHERE_MAP:
      rmesa->TexGenNeedNormals[unit] = GL_TRUE;
      rmesa->TexGenInputs |= R200_TEXGEN_INPUT_SPHERE<<inputshift;
      break;

   default:
      /* Unsupported mode, fallback:
       */
      /*  fprintf(stderr, "fallback unsupported texgen\n"); */
      return GL_FALSE;
   }

   rmesa->TexGenCompSel |= R200_OUTPUT_TEX_0 << unit;

   if (tmp != rmesa->TexGenEnabled) {
      rmesa->NewGLState |= _NEW_TEXTURE_MATRIX;
   }

   return GL_TRUE;
}


static void disable_tex( GLcontext *ctx, int unit )
{
   r200ContextPtr rmesa = R200_CONTEXT(ctx);

   if (rmesa->hw.ctx.cmd[CTX_PP_CNTL] & (R200_TEX_0_ENABLE<<unit)) {
      /* Texture unit disabled */
      if ( rmesa->state.texture.unit[unit].texobj != NULL ) {
	 /* The old texture is no longer bound to this texture unit.
	  * Mark it as such.
	  */

	 rmesa->state.texture.unit[unit].texobj->base.bound &= ~(1UL << unit);
	 rmesa->state.texture.unit[unit].texobj = NULL;
      }

      R200_STATECHANGE( rmesa, ctx );
      rmesa->hw.ctx.cmd[CTX_PP_CNTL] &= ~((R200_TEX_0_ENABLE |
					   R200_TEX_BLEND_0_ENABLE) << unit);
      rmesa->hw.ctx.cmd[CTX_PP_CNTL] |= R200_TEX_BLEND_0_ENABLE; 
	 
      R200_STATECHANGE( rmesa, tcl );
      rmesa->hw.vtx.cmd[VTX_TCL_OUTPUT_VTXFMT_1] &= ~(7 << (unit * 3));
	 
      if (rmesa->TclFallback & (R200_TCL_FALLBACK_TEXGEN_0<<unit)) {
	 TCL_FALLBACK( ctx, (R200_TCL_FALLBACK_TEXGEN_0<<unit), GL_FALSE);
      }

      /* Actually want to keep all units less than max active texture
       * enabled, right?  Fix this for >2 texunits.
       */
      /* FIXME: What should happen here if r200UpdateTextureEnv fails? */
      if (unit == 0) 
	 r200UpdateTextureEnv( ctx, unit ); 


      {
	 GLuint inputshift = R200_TEXGEN_0_INPUT_SHIFT + unit*4;
	 GLuint tmp = rmesa->TexGenEnabled;

	 rmesa->TexGenEnabled &= ~(R200_TEXGEN_TEXMAT_0_ENABLE<<unit);
	 rmesa->TexGenEnabled &= ~(R200_TEXMAT_0_ENABLE<<unit);
	 rmesa->TexGenEnabled &= ~(R200_TEXGEN_INPUT_MASK<<inputshift);
	 rmesa->TexGenNeedNormals[unit] = 0;
	 rmesa->TexGenCompSel &= ~(R200_OUTPUT_TEX_0 << unit);
	 rmesa->TexGenInputs &= ~(R200_TEXGEN_INPUT_MASK<<inputshift);

	 if (tmp != rmesa->TexGenEnabled) {
	    rmesa->recheck_texgen[unit] = GL_TRUE;
	    rmesa->NewGLState |= _NEW_TEXTURE_MATRIX;
	 }
      }
   }
}

static GLboolean enable_tex_2d( GLcontext *ctx, int unit )
{
   r200ContextPtr rmesa = R200_CONTEXT(ctx);
   struct gl_texture_unit *texUnit = &ctx->Texture.Unit[unit];
   struct gl_texture_object *tObj = texUnit->_Current;
   r200TexObjPtr t = (r200TexObjPtr) tObj->DriverData;

   /* Need to load the 2d images associated with this unit.
    */
   if (t->pp_txformat & R200_TXFORMAT_NON_POWER2) {
      t->pp_txformat &= ~R200_TXFORMAT_NON_POWER2;
      t->base.dirty_images[0] = ~0;
   }

   ASSERT(tObj->Target == GL_TEXTURE_2D || tObj->Target == GL_TEXTURE_1D);

   if ( t->base.dirty_images[0] ) {
      R200_FIREVERTICES( rmesa );
      r200SetTexImages( rmesa, tObj );
      r200UploadTexImages( rmesa, (r200TexObjPtr) tObj->DriverData, 0 );
      if ( !t->base.memBlock ) 
	 return GL_FALSE;
   }

   return GL_TRUE;
}

#if ENABLE_HW_3D_TEXTURE
static GLboolean enable_tex_3d( GLcontext *ctx, int unit )
{
   r200ContextPtr rmesa = R200_CONTEXT(ctx);
   struct gl_texture_unit *texUnit = &ctx->Texture.Unit[unit];
   struct gl_texture_object *tObj = texUnit->_Current;
   r200TexObjPtr t = (r200TexObjPtr) tObj->DriverData;

   /* Need to load the 3d images associated with this unit.
    */
   if (t->pp_txformat & R200_TXFORMAT_NON_POWER2) {
      t->pp_txformat &= ~R200_TXFORMAT_NON_POWER2;
      t->base.dirty_images[0] = ~0;
   }

   ASSERT(tObj->Target == GL_TEXTURE_3D);

   /* R100 & R200 do not support mipmaps for 3D textures.
    */
   if ( (tObj->MinFilter != GL_NEAREST) && (tObj->MinFilter != GL_LINEAR) ) {
      return GL_FALSE;
   }

   if ( t->base.dirty_images[0] ) {
      R200_FIREVERTICES( rmesa );
      r200SetTexImages( rmesa, tObj );
      r200UploadTexImages( rmesa, (r200TexObjPtr) tObj->DriverData, 0 );
      if ( !t->base.memBlock ) 
	 return GL_FALSE;
   }

   return GL_TRUE;
}
#endif

static GLboolean enable_tex_cube( GLcontext *ctx, int unit )
{
   r200ContextPtr rmesa = R200_CONTEXT(ctx);
   struct gl_texture_unit *texUnit = &ctx->Texture.Unit[unit];
   struct gl_texture_object *tObj = texUnit->_Current;
   r200TexObjPtr t = (r200TexObjPtr) tObj->DriverData;
   GLuint face;

   /* Need to load the 2d images associated with this unit.
    */
   if (t->pp_txformat & R200_TXFORMAT_NON_POWER2) {
      t->pp_txformat &= ~R200_TXFORMAT_NON_POWER2;
      for (face = 0; face < 6; face++)
         t->base.dirty_images[face] = ~0;
   }

   ASSERT(tObj->Target == GL_TEXTURE_CUBE_MAP);

   if ( t->base.dirty_images[0] || t->base.dirty_images[1] ||
        t->base.dirty_images[2] || t->base.dirty_images[3] ||
        t->base.dirty_images[4] || t->base.dirty_images[5] ) {
      /* flush */
      R200_FIREVERTICES( rmesa );
      /* layout memory space, once for all faces */
      r200SetTexImages( rmesa, tObj );
   }

   /* upload (per face) */
   for (face = 0; face < 6; face++) {
      if (t->base.dirty_images[face]) {
         r200UploadTexImages( rmesa, (r200TexObjPtr) tObj->DriverData, face );
      }
   }
      
   if ( !t->base.memBlock ) {
      /* texmem alloc failed, use s/w fallback */
      return GL_FALSE;
   }

   return GL_TRUE;
}

static GLboolean enable_tex_rect( GLcontext *ctx, int unit )
{
   r200ContextPtr rmesa = R200_CONTEXT(ctx);
   struct gl_texture_unit *texUnit = &ctx->Texture.Unit[unit];
   struct gl_texture_object *tObj = texUnit->_Current;
   r200TexObjPtr t = (r200TexObjPtr) tObj->DriverData;

   if (!(t->pp_txformat & R200_TXFORMAT_NON_POWER2)) {
      t->pp_txformat |= R200_TXFORMAT_NON_POWER2;
      t->base.dirty_images[0] = ~0;
   }

   ASSERT(tObj->Target == GL_TEXTURE_RECTANGLE_NV);

   if ( t->base.dirty_images[0] ) {
      R200_FIREVERTICES( rmesa );
      r200SetTexImages( rmesa, tObj );
      r200UploadTexImages( rmesa, (r200TexObjPtr) tObj->DriverData, 0 );
      if ( !t->base.memBlock && !rmesa->prefer_gart_client_texturing ) 
	 return GL_FALSE;
   }

   return GL_TRUE;
}


static GLboolean update_tex_common( GLcontext *ctx, int unit )
{
   r200ContextPtr rmesa = R200_CONTEXT(ctx);
   struct gl_texture_unit *texUnit = &ctx->Texture.Unit[unit];
   struct gl_texture_object *tObj = texUnit->_Current;
   r200TexObjPtr t = (r200TexObjPtr) tObj->DriverData;
   GLenum format;

   /* Fallback if there's a texture border */
   if ( tObj->Image[tObj->BaseLevel]->Border > 0 )
       return GL_FALSE;

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
   if ( 1|| !(rmesa->hw.ctx.cmd[CTX_PP_CNTL] & (R200_TEX_0_ENABLE<<unit))) {
      R200_STATECHANGE( rmesa, ctx );
      rmesa->hw.ctx.cmd[CTX_PP_CNTL] |= (R200_TEX_0_ENABLE | 
					 R200_TEX_BLEND_0_ENABLE) << unit;

      R200_STATECHANGE( rmesa, vtx );
      rmesa->hw.vtx.cmd[VTX_TCL_OUTPUT_VTXFMT_1] |= 4 << (unit * 3);

      rmesa->recheck_texgen[unit] = GL_TRUE;
   }

   if (t->dirty_state & (1<<unit)) {
      import_tex_obj_state( rmesa, unit, t );
   }

   if (rmesa->recheck_texgen[unit]) {
      GLboolean fallback = !r200_validate_texgen( ctx, unit );
      TCL_FALLBACK( ctx, (R200_TCL_FALLBACK_TEXGEN_0<<unit), fallback);
      rmesa->recheck_texgen[unit] = 0;
      rmesa->NewGLState |= _NEW_TEXTURE_MATRIX;
   }

   format = tObj->Image[tObj->BaseLevel]->Format;
   if ( rmesa->state.texture.unit[unit].format != format ||
	rmesa->state.texture.unit[unit].envMode != texUnit->EnvMode ) {
      rmesa->state.texture.unit[unit].format = format;
      rmesa->state.texture.unit[unit].envMode = texUnit->EnvMode;
      if ( ! r200UpdateTextureEnv( ctx, unit ) ) {
	 return GL_FALSE;
      }
   }

   FALLBACK( rmesa, R200_FALLBACK_BORDER_MODE, t->border_fallback );
   return !t->border_fallback;
}



static GLboolean r200UpdateTextureUnit( GLcontext *ctx, int unit )
{
   struct gl_texture_unit *texUnit = &ctx->Texture.Unit[unit];

   if ( texUnit->_ReallyEnabled & (TEXTURE_RECT_BIT) ) {
      return (enable_tex_rect( ctx, unit ) &&
	      update_tex_common( ctx, unit ));
   }
   else if ( texUnit->_ReallyEnabled & (TEXTURE_1D_BIT | TEXTURE_2D_BIT) ) {
      return (enable_tex_2d( ctx, unit ) &&
	      update_tex_common( ctx, unit ));
   }
#if ENABLE_HW_3D_TEXTURE
   else if ( texUnit->_ReallyEnabled & (TEXTURE_3D_BIT) ) {
      return (enable_tex_3d( ctx, unit ) &&
	      update_tex_common( ctx, unit ));
   }
#endif
   else if ( texUnit->_ReallyEnabled & (TEXTURE_CUBE_BIT) ) {
      return (enable_tex_cube( ctx, unit ) &&
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


void r200UpdateTextureState( GLcontext *ctx )
{
   r200ContextPtr rmesa = R200_CONTEXT(ctx);
   GLboolean ok;
   GLuint dbg;

   ok = (r200UpdateTextureUnit( ctx, 0 ) &&
	 r200UpdateTextureUnit( ctx, 1 ));

   FALLBACK( rmesa, R200_FALLBACK_TEXTURE, !ok );

   if (rmesa->TclFallback)
      r200ChooseVertexState( ctx );

   /*
    * T0 hang workaround -------------
    */
#if 1
   if ((rmesa->hw.ctx.cmd[CTX_PP_CNTL] & R200_TEX_ENABLE_MASK) == R200_TEX_0_ENABLE &&
       (rmesa->hw.tex[0].cmd[TEX_PP_TXFILTER] & R200_MIN_FILTER_MASK) > R200_MIN_FILTER_LINEAR) {

      R200_STATECHANGE(rmesa, ctx);
      R200_STATECHANGE(rmesa, tex[1]);
      rmesa->hw.ctx.cmd[CTX_PP_CNTL] |= R200_TEX_1_ENABLE;
      rmesa->hw.tex[1].cmd[TEX_PP_TXFORMAT] &= ~TEXOBJ_TXFORMAT_MASK;
      rmesa->hw.tex[1].cmd[TEX_PP_TXFORMAT] |= 0x08000000;
   }
   else {
      if ((rmesa->hw.ctx.cmd[CTX_PP_CNTL] & R200_TEX_1_ENABLE) &&
	  (rmesa->hw.tex[1].cmd[TEX_PP_TXFORMAT] & 0x08000000)) {
	 R200_STATECHANGE(rmesa, tex[1]);
	 rmesa->hw.tex[1].cmd[TEX_PP_TXFORMAT] &= ~0x08000000;
      }
   }
#endif

#if 1
   /*
    * Texture cache LRU hang workaround -------------
    */
   dbg = 0x0;
   if (((rmesa->hw.ctx.cmd[CTX_PP_CNTL] & R200_TEX_0_ENABLE) &&
	((((rmesa->hw.tex[0].cmd[TEX_PP_TXFILTER] & R200_MIN_FILTER_MASK)) & 
	  0x04) == 0)))
   {
      dbg |= 0x02;
   }

   if (((rmesa->hw.ctx.cmd[CTX_PP_CNTL] & R200_TEX_1_ENABLE) &&
	((((rmesa->hw.tex[1].cmd[TEX_PP_TXFILTER] & R200_MIN_FILTER_MASK)) & 
	  0x04) == 0)))
   {
      dbg |= 0x04;
   }

   if (dbg != rmesa->hw.tam.cmd[TAM_DEBUG3]) {
      R200_STATECHANGE( rmesa, tam );
      rmesa->hw.tam.cmd[TAM_DEBUG3] = dbg;
      if (0) printf("TEXCACHE LRU HANG WORKAROUND %x\n", dbg);
   }
#endif
}

/*
  also tests for higher texunits:

       ((rmesa->hw.ctx.cmd[CTX_PP_CNTL] & R200_TEX_2_ENABLE) &&
	((((rmesa->hw.tex[2].cmd[TEX_PP_TXFILTER] & R200_MIN_FILTER_MASK)) & 0x04) == 0)) ||
       ((rmesa->hw.ctx.cmd[CTX_PP_CNTL] & R200_TEX_4_ENABLE) &&
	((((rmesa->hw.tex[4].cmd[TEX_PP_TXFILTER] & R200_MIN_FILTER_MASK)) & 0x04) == 0)))

       ((rmesa->hw.ctx.cmd[CTX_PP_CNTL] & R200_TEX_3_ENABLE) &&
	((((rmesa->hw.tex[3].cmd[TEX_PP_TXFILTER] & R200_MIN_FILTER_MASK)) & 0x04) == 0)) ||
       ((rmesa->hw.ctx.cmd[CTX_PP_CNTL] & R200_TEX_5_ENABLE) &&
	((((rmesa->hw.tex[5].cmd[TEX_PP_TXFILTER] & R200_MIN_FILTER_MASK)) & 0x04) == 0)))

*/
