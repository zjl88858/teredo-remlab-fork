/*
 * test-strlcpy.c - strlcpy() replacement test
 */

/***********************************************************************
 *  Copyright © 2006 Rémi Denis-Courmont.                              *
 *  This program is free software; you can redistribute and/or modify  *
 *  it under the terms of the GNU General Public License as published  *
 *  by the Free Software Foundation; version 2 of the license, or (at  *
 *  your option) any later version.                                    *
 *                                                                     *
 *  This program is distributed in the hope that it will be useful,    *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of     *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.               *
 *  See the GNU General Public License for more details.               *
 *                                                                     *
 *  You should have received a copy of the GNU General Public License  *
 *  along with this program; if not, you can get it from:              *
 *  http://www.gnu.org/copyleft/gpl.html                               *
 ***********************************************************************/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <string.h>

int main (void)
{
	char buf[6];

	return (
	    (strlcpy (NULL, "1234", 0) != 4)
	 || (strlcpy (buf, "1234", 0) != 4)
	 || (strlcpy (buf, "1234", 1) != 4)
	 || strcmp (buf, "")
	 || (strlcpy (buf, "1234", 4) != 4)
	 || strcmp (buf, "123")
	 || (strlcpy (buf, "1234", 5) != 4)
	 || strcmp (buf, "1234")
	 || (strlcpy (buf, "1234", 6) != 4)
	 || strcmp (buf, "1234"));
}
