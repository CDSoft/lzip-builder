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

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <utime.h>
#include <sys/stat.h>
#if !defined __FreeBSD__ && !defined __OpenBSD__ && !defined __NetBSD__ && \
    !defined __DragonFly__ && !defined __APPLE__ && !defined __OS2__
#include <sys/sysmacros.h>	// major, minor, makedev
#else
#include <sys/types.h>		// major, minor, makedev
#endif

#include "tarlz.h"
#include <lzlib.h>		// uint8_t defined in tarlz.h
#include "arg_parser.h"
#include "lzip_index.h"
#include "archive_reader.h"
#include "decode.h"

#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif

namespace {

Resizable_buffer grbuf;

bool skip_warn( const bool reset = false, const unsigned chksum = 0 )
  {
  static bool skipping = false;		// avoid duplicate warnings

  if( reset ) { skipping = false; return false; }
  if( skipping ) return false;
  skipping = true;
  if( chksum != 0 )
    { if( verbosity < 1 ) show_error( "Corrupt header." );
      else std::fprintf( stderr, "%s: Corrupt header: ustar chksum = %06o\n",
                         program_name, chksum ); }
  show_error( "Skipping to next header." ); return true;
  }


void read_error( const Archive_reader & ar )
  {
  show_file_error( ar.ad.namep, ar.e_msg(), ar.e_code() );
  if( ar.e_skip() ) skip_warn();
  }


int skip_member( Archive_reader & ar, const Extended & extended,
                 const Typeflag typeflag )
  {
  if( data_may_follow( typeflag ) )
    { const int ret = ar.skip_member( extended );
      if( ret != 0 ) { read_error( ar ); if( ar.fatal() ) return ret; } }
  return 0;
  }


int compare_member( const Cl_options & cl_opts, Archive_reader & ar,
                    const Extended & extended, const Tar_header header )
  {
  if( !show_member_name( extended, header, 1, grbuf ) ) return 1;
  std::string estr, ostr;
  const bool stat_differs =
    !compare_file_type( estr, ostr, cl_opts, extended, header );
  if( estr.size() ) std::fputs( estr.c_str(), stderr );
  if( ostr.size() ) { std::fputs( ostr.c_str(), stdout ); std::fflush( stdout ); }
  if( extended.file_size() <= 0 ) return 0;
  const Typeflag typeflag = (Typeflag)header[typeflag_o];
  if( ( typeflag != tf_regular && typeflag != tf_hiperf ) || stat_differs )
    return skip_member( ar, extended, typeflag );
  // else compare file contents
  const char * const filename = extended.path().c_str();
  const int infd2 = open_instream( filename );
  if( infd2 < 0 )
    { set_error_status( 1 ); return skip_member( ar, extended, typeflag ); }
  int retval = compare_file_contents( estr, ostr, ar, extended.file_size(),
                                      filename, infd2 );
  if( retval ) { read_error( ar ); if( !ar.fatal() ) retval = 0; }
  else { if( estr.size() ) std::fputs( estr.c_str(), stderr );
         if( ostr.size() )
           { std::fputs( ostr.c_str(), stdout ); std::fflush( stdout ); } }
  return retval;
  }


int list_member( Archive_reader & ar,
                 const Extended & extended, const Tar_header header )
  {
  if( !show_member_name( extended, header, 0, grbuf ) ) return 1;
  return skip_member( ar, extended, (Typeflag)header[typeflag_o] );
  }


int extract_member( const Cl_options & cl_opts, Archive_reader & ar,
                    const Extended & extended, const Tar_header header )
  {
  const char * const filename = extended.path().c_str();
  const Typeflag typeflag = (Typeflag)header[typeflag_o];
  if( contains_dotdot( filename ) )
    {
    show_file_error( filename, dotdot_msg );
    return skip_member( ar, extended, typeflag );
    }
  mode_t mode = parse_octal( header + mode_o, mode_l );	 // 12 bits
  if( geteuid() != 0 && !cl_opts.preserve_permissions ) mode &= ~get_umask();
  int outfd = -1;

  if( !show_member_name( extended, header, 1, grbuf ) ) return 1;
  if( !make_dirs( filename ) )
    {
    show_file_error( filename, intdir_msg, errno );
    set_error_status( 1 );
    return skip_member( ar, extended, typeflag );
    }
  // remove file or empty dir before extraction to prevent following links
  std::remove( filename );

  switch( typeflag )
    {
    case tf_regular:
    case tf_hiperf:
      outfd = open_outstream( filename, true, 0, false );
      if( outfd < 0 )
        { set_error_status( 1 ); return skip_member( ar, extended, typeflag ); }
      break;
    case tf_link:
    case tf_symlink:
      {
      const char * const linkname = extended.linkpath().c_str();
      const bool hard = typeflag == tf_link;
      if( ( hard && link( linkname, filename ) != 0 ) ||
          ( !hard && symlink( linkname, filename ) != 0 ) )
        {
        print_error( errno, cantln_msg, hard ? "" : "sym", linkname, filename );
        set_error_status( 1 );
        }
      } break;
    case tf_directory:
      if( mkdir( filename, mode ) != 0 && errno != EEXIST )
        {
        show_file_error( filename, mkdir_msg, errno );
        set_error_status( 1 );
        }
      break;
    case tf_chardev:
    case tf_blockdev:
      {
      const unsigned dev =
        makedev( parse_octal( header + devmajor_o, devmajor_l ),
                 parse_octal( header + devminor_o, devminor_l ) );
      const int dmode = ( typeflag == tf_chardev ? S_IFCHR : S_IFBLK ) | mode;
      if( mknod( filename, dmode, dev ) != 0 )
        {
        show_file_error( filename, mknod_msg, errno );
        set_error_status( 1 );
        }
      break;
      }
    case tf_fifo:
      if( mkfifo( filename, mode ) != 0 )
        {
        show_file_error( filename, mkfifo_msg, errno );
        set_error_status( 1 );
        }
      break;
    default:
      print_error( 0, uftype_msg, filename, typeflag );
      set_error_status( 2 );
      return skip_member( ar, extended, typeflag );
    }

  const bool islink = typeflag == tf_link || typeflag == tf_symlink;
  errno = 0;
  if( !islink &&
      ( !uid_gid_in_range( extended.get_uid(), extended.get_gid() ) ||
        chown( filename, extended.get_uid(), extended.get_gid() ) != 0 ) )
    {
    if( outfd >= 0 ) mode &= ~( S_ISUID | S_ISGID | S_ISVTX );
    // chown in many cases returns with EPERM, which can be safely ignored.
    if( errno != EPERM && errno != EINVAL )
      { show_file_error( filename, chown_msg, errno ); set_error_status( 1 ); }
    }

  if( outfd >= 0 ) fchmod( outfd, mode );		// ignore errors

  if( data_may_follow( typeflag ) )
    {
    const int bufsize = 32 * header_size;
    uint8_t buf[bufsize];
    long long rest = extended.file_size();
    const int rem = rest % header_size;
    const int padding = rem ? header_size - rem : 0;
    while( rest > 0 )
      {
      const int rsize = ( rest >= bufsize ) ? bufsize : rest + padding;
      const int ret = ar.read( buf, rsize );
      if( ret != 0 )
        {
        read_error( ar );
        if( outfd >= 0 )
          {
          if( cl_opts.keep_damaged )
            { writeblock( outfd, buf, std::min( rest, (long long)ar.e_size() ) );
              close( outfd ); }
          else { close( outfd ); unlink( filename ); }
          }
        if( ar.fatal() ) return ret; else return 0;
        }
      const int wsize = ( rest >= bufsize ) ? bufsize : rest;
      if( outfd >= 0 && writeblock( outfd, buf, wsize ) != wsize )
        { show_file_error( filename, wr_err_msg, errno ); return 1; }
      rest -= wsize;
      }
    }
  if( outfd >= 0 && close( outfd ) != 0 )
    { show_file_error( filename, eclosf_msg, errno ); return 1; }
  if( !islink )
    {
    struct utimbuf t;
    t.actime = extended.atime().sec();
    t.modtime = extended.mtime().sec();
    utime( filename, &t );			// ignore errors
    }
  return 0;
  }


void format_file_diff( std::string & ostr, const char * const filename,
                       const char * const msg )
  { if( verbosity >= 0 )
      { ostr += filename; ostr += ": "; ostr += msg; ostr += '\n'; } }


bool option_C_after_filename_or_T( const Arg_parser & parser )
  {
  for( int i = 0; i < parser.arguments(); ++i )
    if( nonempty_arg( parser, i ) || parser.code( i ) == 'T' )
      while( ++i < parser.arguments() )
        if( parser.code( i ) == 'C' ) return true;
  return false;
  }

} // end namespace


mode_t get_umask()
  {
  static mode_t mask = 0;		// read once, cache the result
  static bool first_call = true;
  if( first_call ) { first_call = false; mask = umask( 0 ); umask( mask );
                     mask &= S_IRWXU | S_IRWXG | S_IRWXO; }
  return mask;
  }


bool compare_file_type( std::string & estr, std::string & ostr,
                        const Cl_options & cl_opts,
                        const Extended & extended, const Tar_header header )
  {
  const char * const filename = extended.path().c_str();
  const Typeflag typeflag = (Typeflag)header[typeflag_o];
  struct stat st;
  bool diff = false, size_differs = false, type_differs = true;
  if( hstat( filename, &st, cl_opts.dereference ) != 0 )
    format_file_error( estr, filename, "warning: can't stat", errno );
  else if( ( typeflag == tf_regular || typeflag == tf_hiperf ) &&
           !S_ISREG( st.st_mode ) )
    format_file_diff( ostr, filename, "Is not a regular file" );
  else if( typeflag == tf_symlink && !S_ISLNK( st.st_mode ) )
    format_file_diff( ostr, filename, "Is not a symlink" );
  else if( typeflag == tf_chardev && !S_ISCHR( st.st_mode ) )
    format_file_diff( ostr, filename, "Is not a character device" );
  else if( typeflag == tf_blockdev && !S_ISBLK( st.st_mode ) )
    format_file_diff( ostr, filename, "Is not a block device" );
  else if( typeflag == tf_directory && !S_ISDIR( st.st_mode ) )
    format_file_diff( ostr, filename, "Is not a directory" );
  else if( typeflag == tf_fifo && !S_ISFIFO( st.st_mode ) )
    format_file_diff( ostr, filename, "Is not a FIFO" );
  else
    {
    type_differs = false;
    if( typeflag != tf_symlink && !cl_opts.ignore_metadata )
      {
      const mode_t mode = parse_octal( header + mode_o, mode_l );  // 12 bits
      if( mode != ( st.st_mode & ( S_ISUID | S_ISGID | S_ISVTX |
                                   S_IRWXU | S_IRWXG | S_IRWXO ) ) )
        { format_file_diff( ostr, filename, "Mode differs" ); diff = true; }
      }
    if( !cl_opts.ignore_ids && !cl_opts.ignore_metadata )
      {
      if( extended.get_uid() != (long long)st.st_uid )
        { format_file_diff( ostr, filename, "Uid differs" ); diff = true; }
      if( extended.get_gid() != (long long)st.st_gid )
        { format_file_diff( ostr, filename, "Gid differs" ); diff = true; }
      }
    if( typeflag != tf_symlink )
      {
      if( typeflag != tf_directory && !cl_opts.ignore_metadata &&
          extended.mtime().sec() != (long long)st.st_mtime )
        {
        if( (time_t)extended.mtime().sec() == st.st_mtime )
          { if( !cl_opts.ignore_overflow ) { diff = true;
              format_file_diff( ostr, filename, "Mod time overflow" ); } }
        else { diff = true;
               format_file_diff( ostr, filename, "Mod time differs" ); }
        }
      if( ( typeflag == tf_regular || typeflag == tf_hiperf ) &&
          extended.file_size() != st.st_size )	// don't compare contents
        { format_file_diff( ostr, filename, "Size differs" ); size_differs = true; }
      if( ( typeflag == tf_chardev || typeflag == tf_blockdev ) &&
          ( parse_octal( header + devmajor_o, devmajor_l ) !=
            (unsigned)major( st.st_rdev ) ||
            parse_octal( header + devminor_o, devminor_l ) !=
            (unsigned)minor( st.st_rdev ) ) )
        { format_file_diff( ostr, filename, "Device number differs" ); diff = true; }
      }
    else
      {
      char * const buf = new char[st.st_size+1];
      long len = readlink( filename, buf, st.st_size );
      bool e = len != st.st_size;
      if( !e )
        {
        while( len > 1 && buf[len-1] == '/' ) --len;	// trailing '/'
        buf[len] = 0;
        if( extended.linkpath() != buf ) e = true;
        }
      delete[] buf;
      if( e ) { format_file_diff( ostr, filename, "Symlink differs" ); diff = true; }
      }
    }
  if( diff || size_differs || type_differs ) set_error_status( 1 );
  return !( size_differs || type_differs );
  }


bool compare_file_contents( std::string & estr, std::string & ostr,
                            Archive_reader_base & ar, const long long file_size,
                            const char * const filename, const int infd2 )
  {
  long long rest = file_size;
  const int rem = rest % header_size;
  const int padding = rem ? header_size - rem : 0;
  const int bufsize = 32 * header_size;
  uint8_t buf1[bufsize];
  uint8_t buf2[bufsize];
  int retval = 0;
  bool diff = false;
  estr.clear(); ostr.clear();
  while( rest > 0 )
    {
    const int rsize1 = ( rest >= bufsize ) ? bufsize : rest + padding;
    const int rsize2 = ( rest >= bufsize ) ? bufsize : rest;
    if( ( retval = ar.read( buf1, rsize1 ) ) != 0 ) { diff = true; break; }
    if( !diff )
      {
      const int rd = readblock( infd2, buf2, rsize2 );
      if( rd != rsize2 )
        {
        if( errno ) format_file_error( estr, filename, rd_err_msg, errno );
        else format_file_diff( ostr, filename, "EOF found in file" );
        diff = true;
        }
      else
        {
        int i = 0; while( i < rsize2 && buf1[i] == buf2[i] ) ++i;
        if( i < rsize2 )
          { format_file_diff( ostr, filename, "Contents differ" ); diff = true; }
        }
      }
    if( rest < bufsize ) break;
    rest -= rsize1;
    }
  close( infd2 );
  if( diff ) set_error_status( 1 );
  return retval;
  }


int decode( const Cl_options & cl_opts )
  {
  if( !grbuf.size() ) { show_error( mem_msg ); return 1; }
  // open archive before changing working directory
  const Archive_descriptor ad( cl_opts.archive_name );
  if( ad.infd < 0 ) return 1;
  if( ad.name.size() && ad.indexed && ad.lzip_index.multi_empty() )
    { show_file_error( ad.namep, empty_msg ); close( ad.infd ); return 2; }

  const bool c_present = cl_opts.option_C_present &&
                         cl_opts.program_mode != m_list;
  const bool c_after_name = c_present &&
                            option_C_after_filename_or_T( cl_opts.parser );
  // save current working directory for sequential decoding
  const int cwd_fd = c_after_name ? open( ".", O_RDONLY | O_DIRECTORY ) : -1;
  if( c_after_name && cwd_fd < 0 )
    { show_error( "Can't save current working directory", errno ); return 1; }
  if( c_present && !c_after_name )		// execute all -C options
    for( int i = 0; i < cl_opts.parser.arguments(); ++i )
      {
      if( cl_opts.parser.code( i ) != 'C' ) continue;
      const char * const dir = cl_opts.parser.argument( i ).c_str();
      if( chdir( dir ) != 0 )
        { show_file_error( dir, chdir_msg, errno ); return 1; }
      }
  // file names to be compared, extracted or listed
  Cl_names cl_names( cl_opts.parser );

  /* CWD is not per-thread; multithreaded decode can't be used if an option
     -C appears in the command line after a file name or after an option -T.
     Multithreaded --list is faster even with 1 thread and 1 file in archive
     but multithreaded --diff and --extract probably need at least 2 of each. */
  if( cl_opts.num_workers > 0 && !c_after_name && ad.indexed &&
      ad.lzip_index.members() >= 2 )	// 2 lzip members may be 1 file + EOA
    return decode_lz( cl_opts, ad, cl_names );

  Archive_reader ar( ad );		// serial reader
  Extended extended;			// metadata from extended records
  int retval = 0;
  bool prev_extended = false;		// prev header was extended
  while( true )				// process one tar header per iteration
    {
    Tar_header header;
    const int ret = ar.read( header, header_size );
    if( ret != 0 ) { read_error( ar ); if( ar.fatal() ) { retval = ret; break; } }
    if( ret != 0 || !check_ustar_chksum( header ) )	// error or EOA
      {
      if( ret == 0 && block_is_zero( header, header_size ) )	// EOA
        {
        if( !prev_extended || cl_opts.permissive ) break;
        show_file_error( ad.namep, fv_msg1 );
        retval = 2; break;
        }
      skip_warn( false, ustar_chksum( header ) );
      set_error_status( 2 ); continue;
      }
    skip_warn( true );				// reset warning

    const Typeflag typeflag = (Typeflag)header[typeflag_o];
    if( typeflag == tf_global )
      {
      if( prev_extended && !cl_opts.permissive )
        { show_file_error( ad.namep, fv_msg2 ); retval = 2; break; }
      Extended dummy;		// global headers are parsed and ignored
      const int ret = ar.parse_records( dummy, header, grbuf, gblrec_msg, true );
      if( ret != 0 )
        { show_file_error( ad.namep, ar.e_msg(), ar.e_code() );
          if( ar.fatal() ) { retval = ret; break; }
          set_error_status( ret ); }
      continue;
      }
    if( typeflag == tf_extended )
      {
      if( prev_extended && !cl_opts.permissive )
        { show_file_error( ad.namep, fv_msg3 ); retval = 2; break; }
      const int ret = ar.parse_records( extended, header, grbuf, extrec_msg,
                                        cl_opts.permissive );
      if( ret != 0 )
        { show_file_error( ad.namep, ar.e_msg(), ar.e_code() );
          if( ar.fatal() ) { retval = ret; break; }
          extended.reset(); set_error_status( ret ); }
      else if( !extended.crc_present() && cl_opts.missing_crc )
        { show_file_error( ad.namep, miscrc_msg ); retval = 2; break; }
      prev_extended = true; continue;
      }
    prev_extended = false;

    extended.fill_from_ustar( header );		// copy metadata from header

    try {
      // members without name are skipped except when listing
      if( check_skip_filename( cl_opts, cl_names, extended.path().c_str(),
          cwd_fd ) ) retval = skip_member( ar, extended, typeflag );
      else
        {
        print_removed_prefix( extended.removed_prefix );
        if( cl_opts.program_mode == m_list )
          retval = list_member( ar, extended, header );
        else if( extended.path().empty() )
          retval = skip_member( ar, extended, typeflag );
        else if( cl_opts.program_mode == m_diff )
          retval = compare_member( cl_opts, ar, extended, header );
        else retval = extract_member( cl_opts, ar, extended, header );
        }
      }
    catch( Chdir_error & ) { retval = 1; }
    extended.reset();
    if( retval )
      { show_error( "Error is not recoverable: exiting now." ); break; }
    }

  if( close( ad.infd ) != 0 && retval == 0 )
    { show_file_error( ad.namep, eclosa_msg, errno ); retval = 1; }
  if( cwd_fd >= 0 ) close( cwd_fd );

  if( retval == 0 && cl_names.names_remain( cl_opts.parser ) ) retval = 1;
  return final_exit_status( retval, cl_opts.program_mode != m_diff );
  }
