/*
 *  gfxaccel_arc_shim.cpp - No-op ARC wrappers (OpenGL backend; no ObjC pools)
 * 
 *	(C) 2026 RandoOnSteam (battlemageloveryt@gmail.com)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "sysdeps.h"
#include "rave_engine.h"
#include "gl_engine.h"

uint32_t RaveDispatchARC(uint32_t r3, uint32_t r4, uint32_t r5,
						 uint32_t r6, uint32_t r7, uint32_t r8)
{
	return RaveDispatch(r3, r4, r5, r6, r7, r8);
}

uint32_t GLDispatchARC(uint32_t r3, uint32_t r4, uint32_t r5,
					   uint32_t r6, uint32_t r7, uint32_t r8,
					   uint32_t r9, uint32_t r10,
					   const uint32_t *float_bits, int num_float_args)
{
	return GLDispatch(r3, r4, r5, r6, r7, r8, r9, r10, float_bits, num_float_args);
}
