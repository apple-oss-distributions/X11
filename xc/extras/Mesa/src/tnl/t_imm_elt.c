
/*
 * Mesa 3-D graphics library
 * Version:  4.1
 *
 * Copyright (C) 1999-2002  Brian Paul   All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * BRIAN PAUL BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN
 * AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Keith Whitwell <keith@tungstengraphics.com>
 */

#include "glheader.h"
#include "colormac.h"
#include "context.h"
#include "imports.h"
#include "mmath.h"
#include "mtypes.h"

#include "math/m_translate.h"

#include "t_context.h"
#include "t_imm_elt.h"


typedef void (*trans_elt_1ui_func)(GLuint *to,
				   CONST void *ptr,
				   GLuint stride,
				   const GLuint *flags,
				   const GLuint *elts,
				   GLuint match,
				   GLuint start,
				   GLuint n );

typedef void (*trans_elt_1ub_func)(GLubyte *to,
				   CONST void *ptr,
				   GLuint stride,
				   const GLuint *flags,
				   const GLuint *elts,
				   GLuint match,
				   GLuint start,
				   GLuint n );

typedef void (*trans_elt_4f_func)(GLfloat (*to)[4],
				  CONST void *ptr,
				  GLuint stride,
				  const GLuint *flags,
				  const GLuint *elts,
				  GLuint match,
				  GLuint start,
				  GLuint n );

static trans_elt_1ui_func _tnl_trans_elt_1ui_tab[MAX_TYPES];
static trans_elt_1ub_func _tnl_trans_elt_1ub_tab[MAX_TYPES];
static trans_elt_4f_func  _tnl_trans_elt_4f_tab[5][MAX_TYPES];
static trans_elt_4f_func  _tnl_trans_elt_4fc_tab[5][MAX_TYPES];


#define PTR_ELT(ptr, elt) (((SRC *)ptr)[elt])



/* Code specific to array element implementation.  There is a small
 * subtlety in the bits CHECK() tests, and the way bits are set in
 * glArrayElement which ensures that if, eg, in the case that the
 * vertex array is disabled and normal array is enabled, and we get
 * either sequence:
 *
 * ArrayElement()    OR   Normal()
 * Normal()               ArrayElement()
 * Vertex()               Vertex()
 *
 * That the correct value for normal is used.
 */
#define TAB(x) _tnl_trans_elt##x##_tab
#define ARGS   const GLuint *flags, const GLuint *elts, GLuint match, \
               GLuint start, GLuint n
#define SRC_START  0
#define DST_START  start
#define CHECK  if ((flags[i]&match) == VERT_BIT_ELT)
#define NEXT_F  (void)1
#define NEXT_F2 f = first + elts[i] * stride;


/* GL_BYTE
 */
#define SRC GLbyte
#define SRC_IDX TYPE_IDX(GL_BYTE)
#define TRX_4F(f,n)   BYTE_TO_FLOAT( PTR_ELT(f,n) )
#define TRX_4FC(f,n)   BYTE_TO_FLOAT( PTR_ELT(f,n) )
#define TRX_UB(ub, f,n)  ub = BYTE_TO_UBYTE( PTR_ELT(f,n) )
#define TRX_UI(f,n)  (PTR_ELT(f,n) < 0 ? 0 : (GLuint)  PTR_ELT(f,n))


#define SZ 4
#define INIT init_trans_4_GLbyte_elt
#define DEST_4F trans_4_GLbyte_4f_elt
#define DEST_4FC trans_4_GLbyte_4fc_elt
#include "math/m_trans_tmp.h"

#define SZ 3
#define INIT init_trans_3_GLbyte_elt
#define DEST_4F trans_3_GLbyte_4f_elt
#define DEST_4FC trans_3_GLbyte_4fc_elt
#include "math/m_trans_tmp.h"

#define SZ 2
#define INIT init_trans_2_GLbyte_elt
#define DEST_4F trans_2_GLbyte_4f_elt
#define DEST_4FC trans_2_GLbyte_4fc_elt
#include "math/m_trans_tmp.h"

#define SZ 1
#define INIT init_trans_1_GLbyte_elt
#define DEST_4F trans_1_GLbyte_4f_elt
#define DEST_4FC trans_1_GLbyte_4fc_elt
#define DEST_1UB trans_1_GLbyte_1ub_elt
#define DEST_1UI trans_1_GLbyte_1ui_elt
#include "math/m_trans_tmp.h"

#undef SRC
#undef TRX_4F
#undef TRX_4FC
#undef TRX_UB
#undef TRX_UI
#undef SRC_IDX

/* GL_UNSIGNED_BYTE
 */
#define SRC GLubyte
#define SRC_IDX TYPE_IDX(GL_UNSIGNED_BYTE)
#define TRX_4F(f,n)	     UBYTE_TO_FLOAT( PTR_ELT(f,n) )
#define TRX_4FC(f,n)	     UBYTE_TO_FLOAT( PTR_ELT(f,n) )
#define TRX_UB(ub, f,n)	     ub = PTR_ELT(f,n)
#define TRX_UI(f,n)          (GLuint)PTR_ELT(f,n)

/* 4ub->4ub handled in special case below.
 */
#define SZ 4
#define INIT init_trans_4_GLubyte_elt
#define DEST_4F trans_4_GLubyte_4f_elt
#define DEST_4FC trans_4_GLubyte_4fc_elt
#include "math/m_trans_tmp.h"

#define SZ 3
#define INIT init_trans_3_GLubyte_elt
#define DEST_4F trans_3_GLubyte_4f_elt
#define DEST_4FC trans_3_GLubyte_4fc_elt
#include "math/m_trans_tmp.h"


#define SZ 1
#define INIT init_trans_1_GLubyte_elt
#define DEST_4F  trans_1_GLubyte_4f_elt
#define DEST_4FC  trans_1_GLubyte_4fc_elt
#define DEST_1UB trans_1_GLubyte_1ub_elt
#define DEST_1UI trans_1_GLubyte_1ui_elt
#include "math/m_trans_tmp.h"

#undef SRC
#undef SRC_IDX
#undef TRX_4F
#undef TRX_4FC
#undef TRX_UB
#undef TRX_UI


/* GL_SHORT
 */
#define SRC GLshort
#define SRC_IDX TYPE_IDX(GL_SHORT)
#define TRX_4F(f,n)   (GLfloat)( PTR_ELT(f,n) )
#define TRX_4FC(f,n)   SHORT_TO_FLOAT( PTR_ELT(f,n) )
#define TRX_UB(ub, f,n)  ub = SHORT_TO_UBYTE(PTR_ELT(f,n))
#define TRX_UI(f,n)  (PTR_ELT(f,n) < 0 ? 0 : (GLuint)  PTR_ELT(f,n))


#define SZ  4
#define INIT init_trans_4_GLshort_elt
#define DEST_4F trans_4_GLshort_4f_elt
#define DEST_4FC trans_4_GLshort_4fc_elt
#include "math/m_trans_tmp.h"

#define SZ 3
#define INIT init_trans_3_GLshort_elt
#define DEST_4F trans_3_GLshort_4f_elt
#define DEST_4FC trans_3_GLshort_4fc_elt
#include "math/m_trans_tmp.h"

#define SZ 2
#define INIT init_trans_2_GLshort_elt
#define DEST_4F trans_2_GLshort_4f_elt
#define DEST_4FC trans_2_GLshort_4fc_elt
#include "math/m_trans_tmp.h"

#define SZ 1
#define INIT init_trans_1_GLshort_elt
#define DEST_4F trans_1_GLshort_4f_elt
#define DEST_4FC trans_1_GLshort_4fc_elt
#define DEST_1UB trans_1_GLshort_1ub_elt
#define DEST_1UI trans_1_GLshort_1ui_elt
#include "math/m_trans_tmp.h"


#undef SRC
#undef SRC_IDX
#undef TRX_4F
#undef TRX_4FC
#undef TRX_UB
#undef TRX_UI


/* GL_UNSIGNED_SHORT
 */
#define SRC GLushort
#define SRC_IDX TYPE_IDX(GL_UNSIGNED_SHORT)
#define TRX_4F(f,n)   (GLfloat)( PTR_ELT(f,n) )
#define TRX_4FC(f,n)   USHORT_TO_FLOAT( PTR_ELT(f,n) )
#define TRX_UB(ub,f,n)  ub = (GLubyte) (PTR_ELT(f,n) >> 8)
#define TRX_UI(f,n)  (GLuint)   PTR_ELT(f,n)


#define SZ 4
#define INIT init_trans_4_GLushort_elt
#define DEST_4F trans_4_GLushort_4f_elt
#define DEST_4FC trans_4_GLushort_4fc_elt
#include "math/m_trans_tmp.h"

#define SZ 3
#define INIT init_trans_3_GLushort_elt
#define DEST_4F trans_3_GLushort_4f_elt
#define DEST_4FC trans_3_GLushort_4fc_elt
#include "math/m_trans_tmp.h"

#define SZ 2
#define INIT init_trans_2_GLushort_elt
#define DEST_4F trans_2_GLushort_4f_elt
#define DEST_4FC trans_2_GLushort_4fc_elt
#include "math/m_trans_tmp.h"

#define SZ 1
#define INIT init_trans_1_GLushort_elt
#define DEST_4F trans_1_GLushort_4f_elt
#define DEST_4FC trans_1_GLushort_4fc_elt
#define DEST_1UB trans_1_GLushort_1ub_elt
#define DEST_1UI trans_1_GLushort_1ui_elt
#include "math/m_trans_tmp.h"

#undef SRC
#undef SRC_IDX
#undef TRX_4F
#undef TRX_4FC
#undef TRX_UB
#undef TRX_UI


/* GL_INT
 */
#define SRC GLint
#define SRC_IDX TYPE_IDX(GL_INT)
#define TRX_4F(f,n)   (GLfloat)( PTR_ELT(f,n) )
#define TRX_4FC(f,n)   INT_TO_FLOAT( PTR_ELT(f,n) )
#define TRX_UB(ub, f,n)  ub = INT_TO_UBYTE(PTR_ELT(f,n))
#define TRX_UI(f,n)  (PTR_ELT(f,n) < 0 ? 0 : (GLuint)  PTR_ELT(f,n))


#define SZ 4
#define INIT init_trans_4_GLint_elt
#define DEST_4F trans_4_GLint_4f_elt
#define DEST_4FC trans_4_GLint_4fc_elt
#include "math/m_trans_tmp.h"

#define SZ 3
#define INIT init_trans_3_GLint_elt
#define DEST_4F trans_3_GLint_4f_elt
#define DEST_4FC trans_3_GLint_4fc_elt
#include "math/m_trans_tmp.h"

#define SZ 2
#define INIT init_trans_2_GLint_elt
#define DEST_4F trans_2_GLint_4f_elt
#define DEST_4FC trans_2_GLint_4fc_elt
#include "math/m_trans_tmp.h"

#define SZ 1
#define INIT init_trans_1_GLint_elt
#define DEST_4F trans_1_GLint_4f_elt
#define DEST_4FC trans_1_GLint_4fc_elt
#define DEST_1UB trans_1_GLint_1ub_elt
#define DEST_1UI trans_1_GLint_1ui_elt
#include "math/m_trans_tmp.h"


#undef SRC
#undef SRC_IDX
#undef TRX_4F
#undef TRX_4FC
#undef TRX_UB
#undef TRX_UI


/* GL_UNSIGNED_INT
 */
#define SRC GLuint
#define SRC_IDX TYPE_IDX(GL_UNSIGNED_INT)
#define TRX_4F(f,n)   (GLfloat)( PTR_ELT(f,n) )
#define TRX_4FC(f,n)   UINT_TO_FLOAT( PTR_ELT(f,n) )
#define TRX_UB(ub, f,n)  ub = (GLubyte) (PTR_ELT(f,n) >> 24)
#define TRX_UI(f,n)		PTR_ELT(f,n)


#define SZ 4
#define INIT init_trans_4_GLuint_elt
#define DEST_4F trans_4_GLuint_4f_elt
#define DEST_4FC trans_4_GLuint_4fc_elt
#include "math/m_trans_tmp.h"

#define SZ 3
#define INIT init_trans_3_GLuint_elt
#define DEST_4F trans_3_GLuint_4f_elt
#define DEST_4FC trans_3_GLuint_4fc_elt
#include "math/m_trans_tmp.h"

#define SZ 2
#define INIT init_trans_2_GLuint_elt
#define DEST_4F trans_2_GLuint_4f_elt
#define DEST_4FC trans_2_GLuint_4fc_elt
#include "math/m_trans_tmp.h"

#define SZ 1
#define INIT init_trans_1_GLuint_elt
#define DEST_4F trans_1_GLuint_4f_elt
#define DEST_4FC trans_1_GLuint_4fc_elt
#define DEST_1UB trans_1_GLuint_1ub_elt
#define DEST_1UI trans_1_GLuint_1ui_elt
#include "math/m_trans_tmp.h"

#undef SRC
#undef SRC_IDX
#undef TRX_4F
#undef TRX_4FC
#undef TRX_UB
#undef TRX_UI


/* GL_DOUBLE
 */
#define SRC GLdouble
#define SRC_IDX TYPE_IDX(GL_DOUBLE)
#define TRX_4F(f,n)    (GLfloat) PTR_ELT(f,n)
#define TRX_4FC(f,n)    (GLfloat) PTR_ELT(f,n)
#define TRX_UB(ub,f,n) UNCLAMPED_FLOAT_TO_UBYTE(ub, PTR_ELT(f,n))
#define TRX_UI(f,n)    (GLuint) (GLint) PTR_ELT(f,n)
#define TRX_1F(f,n)    (GLfloat) PTR_ELT(f,n)


#define SZ 4
#define INIT init_trans_4_GLdouble_elt
#define DEST_4F trans_4_GLdouble_4f_elt
#define DEST_4FC trans_4_GLdouble_4fc_elt
#include "math/m_trans_tmp.h"

#define SZ 3
#define INIT init_trans_3_GLdouble_elt
#define DEST_4F trans_3_GLdouble_4f_elt
#define DEST_4FC trans_3_GLdouble_4fc_elt
#include "math/m_trans_tmp.h"

#define SZ 2
#define INIT init_trans_2_GLdouble_elt
#define DEST_4F trans_2_GLdouble_4f_elt
#define DEST_4FC trans_2_GLdouble_4fc_elt
#include "math/m_trans_tmp.h"

#define SZ 1
#define INIT init_trans_1_GLdouble_elt
#define DEST_4F trans_1_GLdouble_4f_elt
#define DEST_4FC trans_1_GLdouble_4fc_elt
#define DEST_1UB trans_1_GLdouble_1ub_elt
#define DEST_1UI trans_1_GLdouble_1ui_elt
#include "math/m_trans_tmp.h"

#undef SRC
#undef SRC_IDX
#undef TRX_4F
#undef TRX_4FC
#undef TRX_UB
#undef TRX_UI

/* GL_FLOAT
 */
#define SRC GLfloat
#define SRC_IDX TYPE_IDX(GL_FLOAT)
#define TRX_4F(f,n)    (GLfloat) PTR_ELT(f,n)
#define TRX_4FC(f,n)    (GLfloat) PTR_ELT(f,n)
#define TRX_UB(ub,f,n) UNCLAMPED_FLOAT_TO_UBYTE(ub, PTR_ELT(f,n))
#define TRX_UI(f,n)    (GLuint) (GLint) PTR_ELT(f,n)
#define TRX_1F(f,n)    (GLfloat) PTR_ELT(f,n)


#define SZ 4
#define INIT init_trans_4_GLfloat_elt
#define DEST_4F  trans_4_GLfloat_4f_elt
#define DEST_4FC  trans_4_GLfloat_4fc_elt
#include "math/m_trans_tmp.h"

#define SZ 3
#define INIT init_trans_3_GLfloat_elt
#define DEST_4F  trans_3_GLfloat_4f_elt
#define DEST_4FC  trans_3_GLfloat_4fc_elt
#include "math/m_trans_tmp.h"

#define SZ 2
#define INIT init_trans_2_GLfloat_elt
#define DEST_4F trans_2_GLfloat_4f_elt
#define DEST_4FC trans_2_GLfloat_4fc_elt
#include "math/m_trans_tmp.h"

#define SZ 1
#define INIT init_trans_1_GLfloat_elt
#define DEST_4F  trans_1_GLfloat_4f_elt
#define DEST_4FC  trans_1_GLfloat_4fc_elt
#define DEST_1UB trans_1_GLfloat_1ub_elt
#define DEST_1UI trans_1_GLfloat_1ui_elt
#include "math/m_trans_tmp.h"

#undef SRC
#undef SRC_IDX
#undef TRX_4F
#undef TRX_4FC
#undef TRX_UB
#undef TRX_UI




static void init_translate_elt(void)
{
   MEMSET( TAB(_1ui), 0, sizeof(TAB(_1ui)) );
   MEMSET( TAB(_1ub), 0, sizeof(TAB(_1ub)) );
   MEMSET( TAB(_4f),  0, sizeof(TAB(_4f)) );
   MEMSET( TAB(_4fc),  0, sizeof(TAB(_4fc)) );

   init_trans_4_GLbyte_elt();
   init_trans_3_GLbyte_elt();
   init_trans_2_GLbyte_elt();
   init_trans_1_GLbyte_elt();
   init_trans_1_GLubyte_elt();
   init_trans_3_GLubyte_elt();
   init_trans_4_GLubyte_elt();
   init_trans_4_GLshort_elt();
   init_trans_3_GLshort_elt();
   init_trans_2_GLshort_elt();
   init_trans_1_GLshort_elt();
   init_trans_4_GLushort_elt();
   init_trans_3_GLushort_elt();
   init_trans_2_GLushort_elt();
   init_trans_1_GLushort_elt();
   init_trans_4_GLint_elt();
   init_trans_3_GLint_elt();
   init_trans_2_GLint_elt();
   init_trans_1_GLint_elt();
   init_trans_4_GLuint_elt();
   init_trans_3_GLuint_elt();
   init_trans_2_GLuint_elt();
   init_trans_1_GLuint_elt();
   init_trans_4_GLdouble_elt();
   init_trans_3_GLdouble_elt();
   init_trans_2_GLdouble_elt();
   init_trans_1_GLdouble_elt();
   init_trans_4_GLfloat_elt();
   init_trans_3_GLfloat_elt();
   init_trans_2_GLfloat_elt();
   init_trans_1_GLfloat_elt();
}


#undef TAB
#undef CLASS
#undef ARGS
#undef CHECK
#undef START



void _tnl_imm_elt_init( void )
{
   init_translate_elt();
}


static void _tnl_trans_elt_1ui(GLuint *to,
			const struct gl_client_array *from,
			const GLuint *flags,
			const GLuint *elts,
			GLuint match,
			GLuint start,
			GLuint n )
{
   _tnl_trans_elt_1ui_tab[TYPE_IDX(from->Type)]( to,
					       from->Ptr,
					       from->StrideB,
					       flags,
					       elts,
					       match,
					       start,
					       n );

}


static void _tnl_trans_elt_1ub(GLubyte *to,
			const struct gl_client_array *from,
			const GLuint *flags,
			const GLuint *elts,
			GLuint match,
			GLuint start,
			GLuint n )
{
   _tnl_trans_elt_1ub_tab[TYPE_IDX(from->Type)]( to,
                                                 from->Ptr,
                                                 from->StrideB,
                                                 flags,
                                                 elts,
                                                 match,
                                                 start,
                                                 n );

}


static void _tnl_trans_elt_4f(GLfloat (*to)[4],
                              const struct gl_client_array *from,
                              const GLuint *flags,
                              const GLuint *elts,
                              GLuint match,
                              GLuint start,
                              GLuint n )
{
   _tnl_trans_elt_4f_tab[from->Size][TYPE_IDX(from->Type)]( to,
					      from->Ptr,
					      from->StrideB,
					      flags,
					      elts,
					      match,
					      start,
					      n );

}



static void _tnl_trans_elt_4fc(GLfloat (*to)[4],
			       const struct gl_client_array *from,
			       const GLuint *flags,
			       const GLuint *elts,
			       GLuint match,
			       GLuint start,
			       GLuint n )
{
   _tnl_trans_elt_4fc_tab[from->Size][TYPE_IDX(from->Type)]( to,
					      from->Ptr,
					      from->StrideB,
					      flags,
					      elts,
					      match,
					      start,
					      n );

}



/* Batch function to translate away all the array elements in the
 * input buffer prior to transform.  Done only the first time a vertex
 * buffer is executed or compiled.
 *
 * KW: Have to do this after each glEnd if arrays aren't locked.
 */
void _tnl_translate_array_elts( GLcontext *ctx, struct immediate *IM,
				GLuint start, GLuint count )
{
   GLuint *flags = IM->Flag;
   const GLuint *elts = IM->Elt;
   GLuint translate = ctx->Array._Enabled;
   GLuint i;

   if (MESA_VERBOSE & VERBOSE_IMMEDIATE)
      _mesa_debug(ctx, "exec_array_elements %d .. %d\n", start, count);

   if (translate & VERT_BIT_POS) {
      _tnl_trans_elt_4f( IM->Attrib[VERT_ATTRIB_POS],
			 &ctx->Array.Vertex,
			 flags, elts, (VERT_BIT_ELT|VERT_BIT_POS),
			 start, count);

      if (ctx->Array.Vertex.Size == 4)
	 translate |= VERT_BITS_OBJ_234;
      else if (ctx->Array.Vertex.Size == 3)
	 translate |= VERT_BITS_OBJ_23;
   }


   if (translate & VERT_BIT_NORMAL)
      _tnl_trans_elt_4f( IM->Attrib[VERT_ATTRIB_NORMAL],
			 &ctx->Array.Normal,
			 flags, elts, (VERT_BIT_ELT|VERT_BIT_NORMAL),
			 start, count);

   if (translate & VERT_BIT_EDGEFLAG)
      _tnl_trans_elt_1ub( IM->EdgeFlag,
			  &ctx->Array.EdgeFlag,
			  flags, elts, (VERT_BIT_ELT|VERT_BIT_EDGEFLAG),
			  start, count);

   if (translate & VERT_BIT_COLOR0) {
      _tnl_trans_elt_4fc( IM->Attrib[VERT_ATTRIB_COLOR0],
			  &ctx->Array.Color,
			  flags, elts, (VERT_BIT_ELT|VERT_BIT_COLOR0),
			  start, count);
   }

   if (translate & VERT_BIT_COLOR1) {
      _tnl_trans_elt_4fc( IM->Attrib[VERT_ATTRIB_COLOR1],
			  &ctx->Array.SecondaryColor,
			  flags, elts, (VERT_BIT_ELT|VERT_BIT_COLOR1),
			  start, count);
   }

   if (translate & VERT_BIT_FOG)
      _tnl_trans_elt_4f( IM->Attrib[VERT_ATTRIB_FOG],
			 &ctx->Array.FogCoord,
			 flags, elts, (VERT_BIT_ELT|VERT_BIT_FOG),
			 start, count);

   if (translate & VERT_BIT_INDEX)
      _tnl_trans_elt_1ui( IM->Index,
			  &ctx->Array.Index,
			  flags, elts, (VERT_BIT_ELT|VERT_BIT_INDEX),
			  start, count);

   if (translate & VERT_BITS_TEX_ANY) {
      for (i = 0 ; i < ctx->Const.MaxTextureUnits ; i++)
	 if (translate & VERT_BIT_TEX(i)) {
	    _tnl_trans_elt_4f( IM->Attrib[VERT_ATTRIB_TEX0 + i],
			       &ctx->Array.TexCoord[i],
			       flags, elts, (VERT_BIT_ELT|VERT_BIT_TEX(i)),
			       start, count);

	    if (ctx->Array.TexCoord[i].Size == 4)
	       IM->TexSize |= TEX_SIZE_4(i);
	    else if (ctx->Array.TexCoord[i].Size == 3)
	       IM->TexSize |= TEX_SIZE_3(i);
	 }
   }

   for (i = start ; i < count ; i++)
      if (flags[i] & VERT_BIT_ELT) flags[i] |= translate;

   IM->FlushElt = 0;
}
