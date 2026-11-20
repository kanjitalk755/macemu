/*
 *  vec_data.h - std::vector<T>::data() for C++98
 *
 *  Basilisk II (C) 1997-2008 Christian Bauer
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

/*
 *  std::vector gained data() in C++11; the tree builds as C++98.  Taking
 *  &v[0] directly is the usual substitute but trips the debug-iterator
 *  assertions when the vector is empty, which several call sites here allow
 *  (they pair the pointer with a zero length).  vec_data() hands back a null
 *  pointer in that case instead.
 */

#ifndef VEC_DATA_H
#define VEC_DATA_H

#include <vector>

template <class T>
inline T *vec_data(std::vector<T> &v)
{
	return v.empty() ? (T *)0 : &v[0];
}

template <class T>
inline const T *vec_data(const std::vector<T> &v)
{
	return v.empty() ? (const T *)0 : &v[0];
}

#endif /* VEC_DATA_H */
