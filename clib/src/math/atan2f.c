/* Math functions for i387.
Copyright (C) 1995, 1996, 1997 Free Software Foundation, Inc.
This file is part of the GNU C Library.
Contributed by John C. Bowman <bowman@ipp-garching.mpg.de>, 1995.

The GNU C Library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

The GNU C Library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with the GNU C Library; if not, write to the Free
Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
Boston, MA 02110-1301, USA.
*/

#include <math.h>

#if (_MOLLENOS >= 0x100) && \
	(defined(__x86_64) || defined(_M_AMD64) || \
	defined (__ia64__) || defined (_M_IA64))
_Check_return_
float
__cdecl
atan2f(
_In_ float x,
_In_ float y)
{
	return (float)atan2((double)x, (double)y);
}
#endif