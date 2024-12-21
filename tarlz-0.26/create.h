/* Tarlz - Archiver with multimember lzip compression
   Copyright (C) 2013-2024 Antonio Diaz Diaz.

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

class Archive_attrs
  {
  struct stat ast;		// archive attributes at time of init
  bool initialized;
  bool isreg;

public:
  Archive_attrs() : initialized( false ), isreg( false ) {}
  bool init( const int fd )
    {
    if( fstat( fd, &ast ) != 0 ) return false;
    if( S_ISREG( ast.st_mode ) ) isreg = true;
    initialized = true;
    return true;
    }
  bool is_the_archive( const struct stat & st ) const
    { return isreg && st.st_dev == ast.st_dev && st.st_ino == ast.st_ino; }
  bool is_newer( const struct stat & st ) const
    { return initialized && st.st_mtime > ast.st_mtime; }
  bool is_newer( const char * const filename ) const
    {
    if( !initialized ) return false;
    struct stat st;
    return lstat( filename, &st ) != 0 || st.st_mtime > ast.st_mtime;
    }
  };

extern Archive_attrs archive_attrs;

const char * const cant_stat = "Can't stat input file";
