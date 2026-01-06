/* Tarlz - Archiver with multimember lzip compression
   Copyright (C) 2013-2026 Antonio Diaz Diaz.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#define _FILE_OFFSET_BITS 64

#include <cctype>
#include <cerrno>
#include <unistd.h>

#include "tarlz.h"


unsigned long long parse_octal( const uint8_t * const ptr, const int size )
  {
  unsigned long long result = 0;
  int i = 0;
  while( i < size && std::isspace( ptr[i] ) ) ++i;
  for( ; i < size && ptr[i] >= '0' && ptr[i] <= '7'; ++i )
    { result <<= 3; result += ptr[i] - '0'; }
  return result;
  }


/* Return the number of bytes really read.
   If (value returned < size) and (errno == 0), means EOF was reached.
*/
long readblock( const int fd, uint8_t * const buf, const long size )
  {
  long sz = 0;
  errno = 0;
  while( sz < size )
    {
    const long n = read( fd, buf + sz, size - sz );
    if( n > 0 ) sz += n;
    else if( n == 0 ) break;				// EOF
    else if( errno != EINTR ) break;
    errno = 0;
    }
  return sz;
  }


/* Return the number of bytes really written.
   If (value returned < size), it is always an error.
*/
int writeblock( const int fd, const uint8_t * const buf, const int size )
  {
  int sz = 0;
  errno = 0;
  while( sz < size )
    {
    const int n = write( fd, buf + sz, size - sz );
    if( n > 0 ) sz += n;
    else if( n < 0 && errno != EINTR ) break;
    errno = 0;
    }
  return sz;
  }
