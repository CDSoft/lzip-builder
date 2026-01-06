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

#include <fnmatch.h>

#include "tarlz.h"


namespace Exclude {

std::vector< std::string > patterns;		// list of patterns

} // end namespace Exclude


void Exclude::add_pattern( const std::string & arg )
  { patterns.push_back( arg ); }


bool Exclude::excluded( const char * const filename )
  {
  if( patterns.empty() ) return false;
  const char * p = filename;
  do {
    for( unsigned i = 0; i < patterns.size(); ++i )
      // ignore a trailing sequence starting with '/' in filename
#ifdef FNM_LEADING_DIR
      if( fnmatch( patterns[i].c_str(), p, FNM_LEADING_DIR ) == 0 ) return true;
#else
      if( fnmatch( patterns[i].c_str(), p, 0 ) == 0 ||
          fnmatch( ( patterns[i] + "/*" ).c_str(), p, 0 ) == 0 ) return true;
#endif
    while( *p && *p != '/' ) ++p;		// skip component
    while( *p == '/' ) ++p;			// skip slashes
    } while( *p );
  return false;
  }
