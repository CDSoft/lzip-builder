/* Tarlz - Archiver with multimember lzip compression
   Copyright (C) 2013-2025 Antonio Diaz Diaz.

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

#include <cerrno>
#include <cstdio>
#include <ctime>
#include <unistd.h>
#include <sys/stat.h>

#include "tarlz.h"
#include "arg_parser.h"
#include "decode.h"


namespace {

enum { mode_string_size = 10,
       group_string_size = 1 + uname_l + 1 + gname_l + 1 };	// 67

void format_mode_string( const Tar_header header, char buf[mode_string_size] )
  {
  const Typeflag typeflag = (Typeflag)header[typeflag_o];

  std::memcpy( buf, "----------", mode_string_size );
  switch( typeflag )
    {
    case tf_regular: break;
    case tf_link: buf[0] = 'h'; break;
    case tf_symlink: buf[0] = 'l'; break;
    case tf_chardev: buf[0] = 'c'; break;
    case tf_blockdev: buf[0] = 'b'; break;
    case tf_directory: buf[0] = 'd'; break;
    case tf_fifo: buf[0] = 'p'; break;
    case tf_hiperf: buf[0] = 'C'; break;
    default: buf[0] = '?';
    }
  const mode_t mode = parse_octal( header + mode_o, mode_l );	// 12 bits
  const bool setuid = mode & S_ISUID;
  const bool setgid = mode & S_ISGID;
  const bool sticky = mode & S_ISVTX;
  if( mode & S_IRUSR ) buf[1] = 'r';
  if( mode & S_IWUSR ) buf[2] = 'w';
  if( mode & S_IXUSR ) buf[3] = setuid ? 's' : 'x';
  else if( setuid ) buf[3] = 'S';
  if( mode & S_IRGRP ) buf[4] = 'r';
  if( mode & S_IWGRP ) buf[5] = 'w';
  if( mode & S_IXGRP ) buf[6] = setgid ? 's' : 'x';
  else if( setgid ) buf[6] = 'S';
  if( mode & S_IROTH ) buf[7] = 'r';
  if( mode & S_IWOTH ) buf[8] = 'w';
  if( mode & S_IXOTH ) buf[9] = sticky ? 't' : 'x';
  else if( sticky ) buf[9] = 'T';
  }


int format_user_group_string( const Extended & extended,
                              const Tar_header header,
                              char buf[group_string_size] )
  {
  int len;
  if( header[uname_o] && header[gname_o] )
    len = snprintf( buf, group_string_size,
                    " %.32s/%.32s", header + uname_o, header + gname_o );
  else
    len = snprintf( buf, group_string_size, " %llu/%llu",
                    extended.get_uid(), extended.get_gid() );
  return len;
  }


// return true if dir is a parent directory of name
bool compare_prefix_dir( const char * const dir, const char * const name )
  {
  int len = 0;
  while( dir[len] && dir[len] == name[len] ) ++len;
  return !dir[len] && len > 0 && ( dir[len-1] == '/' || name[len] == '/' );
  }


// compare two file names ignoring trailing slashes
bool compare_tslash( const char * const name1, const char * const name2 )
  {
  const char * p = name1;
  const char * q = name2;
  while( *p && *p == *q ) { ++p; ++q; }
  while( *p == '/' ) ++p;
  while( *q == '/' ) ++q;
  return !*p && !*q;
  }

} // end namespace


bool block_is_zero( const uint8_t * const buf, const int size )
  {
  for( int i = 0; i < size; ++i ) if( buf[i] != 0 ) return false;
  return true;
  }


bool format_member_name( const Extended & extended, const Tar_header header,
                         Resizable_buffer & rbuf, const bool long_format )
  {
  if( !long_format )
    {
    if( !rbuf.resize( extended.path().size() + 2 ) ) return false;
    snprintf( rbuf(), rbuf.size(), "%s\n", extended.path().c_str() );
    return true;
    }
  format_mode_string( header, rbuf() );
  const int group_string_len =
    format_user_group_string( extended, header, rbuf() + mode_string_size );
  int offset = mode_string_size + group_string_len;
  const time_t mtime = extended.mtime().sec();
  struct tm t;
  char buf[32];	// if local time and UTC fail, use seconds since epoch
  if( localtime_r( &mtime, &t ) || gmtime_r( &mtime, &t ) )
    snprintf( buf, sizeof buf, "%04d-%02u-%02u %02u:%02u", 1900 + t.tm_year,
              1 + t.tm_mon, t.tm_mday, t.tm_hour, t.tm_min );
  else snprintf( buf, sizeof buf, "%lld", extended.mtime().sec() );
  const Typeflag typeflag = (Typeflag)header[typeflag_o];
  const bool islink = typeflag == tf_link || typeflag == tf_symlink;
  const char * const link_string = !islink ? "" :
                       ( ( typeflag == tf_link ) ? " link to " : " -> " );
  // print "user/group size" in a field of width 19 with 8 or more for size
  if( typeflag == tf_chardev || typeflag == tf_blockdev )
    {
    const unsigned devmajor = parse_octal( header + devmajor_o, devmajor_l );
    const unsigned devminor = parse_octal( header + devminor_o, devminor_l );
    const int width = std::max( 1,
      std::max( 8, 19 - group_string_len ) - 1 - decimal_digits( devminor ) );
    offset += snprintf( rbuf() + offset, rbuf.size() - offset, " %*u,%u",
                        width, devmajor, devminor );
    }
  else
    {
    const int width = std::max( 8, 19 - group_string_len );
    offset += snprintf( rbuf() + offset, rbuf.size() - offset, " %*llu",
                        width, extended.file_size() );
    }
  for( int i = 0; i < 2; ++i )	// resize rbuf if not large enough
    {
    const int len = snprintf( rbuf() + offset, rbuf.size() - offset,
              " %s %s%s%s\n", buf, extended.path().c_str(), link_string,
              islink ? extended.linkpath().c_str() : "" );
    if( len + offset < (int)rbuf.size() ) { offset += len; break; }
    if( !rbuf.resize( len + offset + 1 ) ) return false;
    }
  if( rbuf()[0] == '?' )
    {
    if( !rbuf.resize( offset + 25 + 1 ) ) return false;
    snprintf( rbuf() + offset - 1, rbuf.size() - offset,
              ": Unknown file type 0x%02X\n", typeflag );
    }
  return true;
  }


bool show_member_name( const Extended & extended, const Tar_header header,
                       const int vlevel, Resizable_buffer & rbuf )
  {
  if( verbosity >= vlevel )
    {
    if( !format_member_name( extended, header, rbuf, verbosity > vlevel ) )
      { show_error( mem_msg ); return false; }
    std::fputs( rbuf(), stdout );
    std::fflush( stdout );
    }
  return true;
  }


/* Return true if file must be skipped.
   Execute -C options if cwd_fd >= 0 (diff or extract). */
bool check_skip_filename( const Cl_options & cl_opts,
                          std::vector< char > & name_pending,
                          const char * const filename, const int cwd_fd,
                          std::string * const msgp )
  {
  static int c_idx = -1;		// parser index of last -C executed
  if( Exclude::excluded( filename ) ) return true;	// skip excluded files
  if( cl_opts.num_files <= 0 ) return false;	// no files specified, no skip
  bool skip = true;	// else skip all but the files (or trees) specified
  bool chdir_pending = false;

  for( int i = 0; i < cl_opts.parser.arguments(); ++i )
    {
    if( cl_opts.parser.code( i ) == 'C' ) { chdir_pending = true; continue; }
    if( !nonempty_arg( cl_opts.parser, i ) ) continue;	// skip opts, empty names
    std::string removed_prefix;			// prefix of cl argument
    const char * const name = remove_leading_dotslash(
                 cl_opts.parser.argument( i ).c_str(), &removed_prefix );
    if( compare_prefix_dir( name, filename ) ||
        compare_tslash( name, filename ) )
      {
      print_removed_prefix( removed_prefix, msgp );
      skip = false; name_pending[i] = false;
      // only serial decoder sets cwd_fd >= 0 to process -C options
      if( chdir_pending && cwd_fd >= 0 )
        {
        if( c_idx > i )
          { if( fchdir( cwd_fd ) != 0 )
            { show_error( "Error changing to initial working directory", errno );
              throw Chdir_error(); } c_idx = -1; }
        for( int j = c_idx + 1; j < i; ++j )
          {
          if( cl_opts.parser.code( j ) != 'C' ) continue;
          const char * const dir = cl_opts.parser.argument( j ).c_str();
          if( chdir( dir ) != 0 )
            { show_file_error( dir, chdir_msg, errno ); throw Chdir_error(); }
          c_idx = j;
          }
        }
      break;
      }
    }
  return skip;
  }


bool make_dirs( const std::string & name )
  {
  int i = name.size();
  while( i > 0 && name[i-1] == '/' ) --i;	// remove trailing slashes
  while( i > 0 && name[i-1] != '/' ) --i;	// remove last component
  while( i > 0 && name[i-1] == '/' ) --i;	// remove more slashes
  const int dirsize = i;		// first slash before last component
  struct stat st;

  if( dirsize > 0 && lstat( std::string( name, 0, dirsize ).c_str(), &st ) == 0 )
    { if( !S_ISDIR( st.st_mode ) ) { errno = ENOTDIR; return false; }
      return true; }
  for( i = 0; i < dirsize; )	// if dirsize == 0, dirname is '/' or empty
    {
    while( i < dirsize && name[i] == '/' ) ++i;
    const int first = i;
    while( i < dirsize && name[i] != '/' ) ++i;
    if( first < i )
      {
      const std::string partial( name, 0, i );
      const mode_t mode = S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
      if( lstat( partial.c_str(), &st ) == 0 )
        { if( !S_ISDIR( st.st_mode ) ) { errno = ENOTDIR; return false; } }
      else if( mkdir( partial.c_str(), mode ) != 0 && errno != EEXIST )
        return false;	// if EEXIST, another thread or process created the dir
      }
    }
  return true;
  }
