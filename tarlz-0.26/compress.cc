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

#define _FILE_OFFSET_BITS 64

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <stdint.h>		// for lzlib.h
#include <unistd.h>
#include <utime.h>
#include <sys/stat.h>
#include <lzlib.h>

#include "tarlz.h"
#include "arg_parser.h"


namespace {

/* Variables used in signal handler context.
   They are not declared volatile because the handler never returns. */
std::string output_filename;
int outfd = -1;
bool delete_output_on_interrupt = false;


void set_signals( void (*action)(int) )
  {
  std::signal( SIGHUP, action );
  std::signal( SIGINT, action );
  std::signal( SIGTERM, action );
  }


void cleanup_and_fail( const int retval )
  {
  set_signals( SIG_IGN );			// ignore signals
  if( delete_output_on_interrupt )
    {
    delete_output_on_interrupt = false;
    show_file_error( output_filename.c_str(),
                     "Deleting output file, if it exists." );
    if( outfd >= 0 ) { close( outfd ); outfd = -1; }
    if( std::remove( output_filename.c_str() ) != 0 && errno != ENOENT )
      show_error( "warning: deletion of output file failed", errno );
    }
  std::exit( retval );
  }


extern "C" void signal_handler( int )
  {
  show_error( "Control-C or similar caught, quitting." );
  cleanup_and_fail( 1 );
  }


const char * ne_output_filename()	// non-empty output file name
  {
  return output_filename.size() ? output_filename.c_str() : "(stdout)";
  }


bool check_tty_in( const char * const input_filename, const int infd )
  {
  if( isatty( infd ) )				// for example /dev/tty
    { show_file_error( input_filename,
                       "I won't read archive data from a terminal." );
      close( infd ); return false; }
  return true;
  }

bool check_tty_out()
  {
  if( isatty( outfd ) )				// for example /dev/tty
    { show_file_error( ne_output_filename(),
                       "I won't write compressed data to a terminal." );
      return false; }
  return true;
  }


// Set permissions, owner, and times.
void close_and_set_permissions( const struct stat * const in_statsp )
  {
  bool warning = false;
  if( in_statsp )
    {
    const mode_t mode = in_statsp->st_mode;
    // fchown in many cases returns with EPERM, which can be safely ignored.
    if( fchown( outfd, in_statsp->st_uid, in_statsp->st_gid ) == 0 )
      { if( fchmod( outfd, mode ) != 0 ) warning = true; }
    else
      if( errno != EPERM ||
          fchmod( outfd, mode & ~( S_ISUID | S_ISGID | S_ISVTX ) ) != 0 )
        warning = true;
    }
  if( close( outfd ) != 0 )
    { show_file_error( output_filename.c_str(), "Error closing output file",
                       errno ); cleanup_and_fail( 1 ); }
  outfd = -1;
  delete_output_on_interrupt = false;
  if( in_statsp )
    {
    struct utimbuf t;
    t.actime = in_statsp->st_atime;
    t.modtime = in_statsp->st_mtime;
    if( utime( output_filename.c_str(), &t ) != 0 ) warning = true;
    }
  if( warning && verbosity >= 1 )
    show_file_error( output_filename.c_str(),
                     "warning: can't change output file attributes", errno );
  }


bool archive_write( const uint8_t * const buf, const int size,
                    LZ_Encoder * const encoder )
  {
  static bool flushed = true;		// avoid flushing empty lzip members

  if( size <= 0 && flushed ) return true;
  flushed = size <= 0;
  enum { obuf_size = 65536 };
  uint8_t obuf[obuf_size];
  int sz = 0;
  if( flushed ) LZ_compress_finish( encoder );	// flush encoder
  while( sz < size || flushed )
    {
    if( sz < size )
      { const int wr = LZ_compress_write( encoder, buf + sz, size - sz );
        if( wr < 0 ) internal_error( "library error (LZ_compress_write)." );
        sz += wr; }
    if( sz >= size && !flushed ) break;		// minimize dictionary size
    const int rd = LZ_compress_read( encoder, obuf, obuf_size );
    if( rd < 0 ) internal_error( "library error (LZ_compress_read)." );
    if( rd == 0 && sz >= size ) break;
    if( writeblock( outfd, obuf, rd ) != rd )
      { show_file_error( ne_output_filename(), wr_err_msg, errno );
        return false; }
    }
  if( LZ_compress_finished( encoder ) == 1 &&
      LZ_compress_restart_member( encoder, LLONG_MAX ) < 0 )
    internal_error( "library error (LZ_compress_restart_member)." );
  return true;
  }


bool tail_compress( const Cl_options & cl_opts,
                    const int infd, Tar_header header,
                    LZ_Encoder * const encoder )
  {
  if( cl_opts.solidity != solid && !archive_write( 0, 0, encoder ) )
    return false;	// flush encoder before compressing EOA blocks
  int size = header_size;
  bool zero = true;	// true until nonzero data found after EOA blocks
  while( true )
    {
    if( size > 0 && !archive_write( header, size, encoder ) )
      { close( infd ); return false; }
    if( size < header_size ) break;			// EOF
    size = readblock( infd, header, header_size );
    if( errno ) return false;
    if( zero && !block_is_zero( header, size ) )
      { zero = false;		// flush encoder after compressing EOA blocks
        if( cl_opts.solidity != solid && !archive_write( 0, 0, encoder ) )
          return false; }
    }
  return true;
  }


int compress_archive( const Cl_options & cl_opts,
                      const std::string & input_filename,
                      LZ_Encoder * const encoder,
                      const bool to_stdout, const bool to_file )
  {
  const bool one_to_one = !to_stdout && !to_file;
  const bool from_stdin = input_filename == "-";
  const char * const filename = from_stdin ? "(stdin)" : input_filename.c_str();
  const int infd = from_stdin ? STDIN_FILENO : open_instream( filename );
  if( infd < 0 || !check_tty_in( filename, infd ) ) return 1;
  if( one_to_one )
    {
    if( from_stdin ) { outfd = STDOUT_FILENO; output_filename.clear(); }
    else
      {
      output_filename = input_filename + ".lz";
      outfd = open_outstream( output_filename, true, 0, false );
      if( outfd < 0 ) { close( infd ); return 1; }
      delete_output_on_interrupt = true;
      }
    if( !check_tty_out() ) { close( infd ); return 1; }	// don't delete a tty
    }
  if( verbosity >= 1 ) std::fprintf( stderr, "%s\n", filename );

  unsigned long long partial_data_size = 0;	// size of current block
  Extended extended;			// metadata from extended records
  Resizable_buffer rbuf;		// headers and extended records buffer
  if( !rbuf.size() ) { show_error( mem_msg ); return 1; }
  const char * const rderr_msg = "Read error";
  bool first_header = true;

  while( true )				// process one tar member per iteration
    {
    int total_header_size = header_size;	// e_header + edata + u_header
    const int rd = readblock( infd, rbuf.u8(), header_size );
    if( rd == 0 && errno == 0 )			// missing EOA blocks
      { if( !first_header ) break;
        show_file_error( filename, "Archive is empty." );
        close( infd ); return 2; }
    if( rd != header_size )
      { show_file_error( filename, rderr_msg, errno ); close( infd ); return 1; }
    first_header = false;

    const bool is_header = check_ustar_chksum( rbuf.u8() );
    const bool is_zero = !is_header && block_is_zero( rbuf.u8(), header_size );
    if( to_file && outfd < 0 && ( is_header || is_zero ) )
      {
      // open outfd after checking infd
      if( !make_dirs( output_filename ) )
        { show_file_error( output_filename.c_str(), intdir_msg, errno );
          return 1; }
      outfd = open_outstream( output_filename, true, 0, false );
      // check tty only once and don't try to delete a tty
      if( outfd < 0 || !check_tty_out() ) { close( infd ); return 1; }
      delete_output_on_interrupt = true;
      }

    if( !is_header )				// maybe EOA block
      {
      if( is_zero )				// first EOA block
        { tail_compress( cl_opts, infd, rbuf.u8(), encoder ); break; }
      show_file_error( filename, bad_hdr_msg ); close( infd ); return 2;
      }

    const Typeflag typeflag = (Typeflag)rbuf()[typeflag_o];
    if( typeflag == tf_extended || typeflag == tf_global )
      {
      const long long edsize = parse_octal( rbuf.u8() + size_o, size_l );
      const long long bufsize = round_up( edsize );
      // overflow or no extended data
      if( edsize <= 0 || edsize >= 1LL << 33 || bufsize > max_edata_size )
        { show_file_error( filename, bad_hdr_msg ); close( infd ); return 2; }
      if( !rbuf.resize( total_header_size + bufsize ) )
        { show_file_error( filename, mem_msg ); close( infd ); return 1; }
      if( readblock( infd, rbuf.u8() + total_header_size, bufsize ) != bufsize )
        { show_file_error( filename, rderr_msg, errno ); close( infd ); return 1; }
      total_header_size += bufsize;
      if( typeflag == tf_extended )	// do not parse global headers
        {
        if( !extended.parse( rbuf() + header_size, edsize, false ) )
          { show_file_error( filename, extrec_msg ); close( infd ); return 2; }
        // read ustar header
        if( !rbuf.resize( total_header_size + header_size ) )
          { show_file_error( filename, mem_msg ); close( infd ); return 1; }
        if( readblock( infd, rbuf.u8() + total_header_size, header_size ) != header_size )
          { show_file_error( filename, errno ? rderr_msg : end_msg, errno );
            close( infd ); return errno ? 1 : 2; }
        if( !check_ustar_chksum( rbuf.u8() ) )
          { show_file_error( filename, bad_hdr_msg ); close( infd ); return 2; }
        const Typeflag typeflag2 = (Typeflag)(rbuf() + total_header_size)[typeflag_o];
        if( typeflag2 == tf_extended || typeflag2 == tf_global )
          { const char * msg = ( typeflag2 == tf_global ) ? fv_msg2 : fv_msg3;
            show_file_error( filename, msg ); close( infd ); return 2; }
        total_header_size += header_size;
        }
      }

    const long long file_size = round_up( extended.get_file_size_and_reset(
                                rbuf.u8() + total_header_size - header_size ) );
    if( cl_opts.solidity == bsolid &&
        block_is_full( total_header_size - header_size, file_size,
                       cl_opts.data_size, partial_data_size ) &&
        !archive_write( 0, 0, encoder ) ) { close( infd ); return 1; }
    if( !archive_write( rbuf.u8(), total_header_size, encoder ) )
      { close( infd ); return 1; }

    if( file_size )
      {
      const long long bufsize = 32 * header_size;
      uint8_t buf[bufsize];
      long long rest = file_size;	// file_size already rounded up
      while( rest > 0 )
        {
        int size = std::min( rest, bufsize );
        const int rd = readblock( infd, buf, size );
        rest -= rd;
        if( rd != size )
          {
          show_atpos_error( filename, file_size - rest, true );
          close( infd ); return 1;
          }
        if( !archive_write( buf, size, encoder ) ) { close( infd ); return 1; }
        }
      }
    if( cl_opts.solidity == no_solid && !archive_write( 0, 0, encoder ) )
      { close( infd ); return 1; }	// one tar member per lzip member
    }
  // flush and restart encoder (for next archive)
  if( !archive_write( 0, 0, encoder ) ) { close( infd ); return 1; }
  const bool need_close = delete_output_on_interrupt &&
                          ( one_to_one || ( to_file && !from_stdin ) );
  struct stat in_stats;
  const struct stat * const in_statsp =
        ( need_close && fstat( infd, &in_stats ) == 0 ) ? &in_stats : 0;
  if( close( infd ) != 0 )
    { show_file_error( filename, eclosf_msg, errno ); return 1; }
  if( need_close ) close_and_set_permissions( in_statsp );
  return 0;
  }

} // end namespace


void show_atpos_error( const char * const filename, const long long pos,
                       const bool isarchive )
  {
  if( verbosity < 0 ) return;
  std::fprintf( stderr, "%s: %s: %s %s at pos %llu%s%s\n", program_name,
                filename, isarchive ? "Archive" : "File",
                ( errno > 0 ) ? "read error" : "ends unexpectedly", pos,
                ( errno > 0 ) ? ": " : "",
                ( errno > 0 ) ? std::strerror( errno ) : "" );
  }


int compress( const Cl_options & cl_opts )
  {
  if( cl_opts.num_files > 1 && cl_opts.output_filename.size() )
    { show_file_error( cl_opts.output_filename.c_str(),
      "Only can compress one archive when using '-o'." ); return 1; }
  const bool to_stdout = cl_opts.output_filename == "-";
  if( to_stdout )				// check tty only once
    { outfd = STDOUT_FILENO; if( !check_tty_out() ) return 1; }
  else outfd = -1;
  const bool to_file = !to_stdout && cl_opts.output_filename.size();
  if( to_file ) output_filename = cl_opts.output_filename;
  if( !to_stdout && ( cl_opts.filenames_given || to_file ) )
    set_signals( signal_handler );

  LZ_Encoder * encoder = LZ_compress_open(
                 option_mapping[cl_opts.level].dictionary_size,
                 option_mapping[cl_opts.level].match_len_limit, LLONG_MAX );
  if( !encoder || LZ_compress_errno( encoder ) != LZ_ok )
    {
    if( !encoder || LZ_compress_errno( encoder ) == LZ_mem_error )
      show_error( mem_msg2 );
    else
      internal_error( "invalid argument to encoder." );
    return 1;
    }

  if( !cl_opts.filenames_given )
    return compress_archive( cl_opts, "-", encoder, to_stdout, to_file );
  int retval = 0;
  bool stdin_used = false;
  for( int i = 0; i < cl_opts.parser.arguments(); ++i )
    if( nonempty_arg( cl_opts.parser, i ) )	// skip opts, empty names
      {
      if( cl_opts.parser.argument( i ) == "-" )
        { if( stdin_used ) continue; else stdin_used = true; }
      const int tmp = compress_archive( cl_opts, cl_opts.parser.argument( i ),
                                        encoder, to_stdout, to_file );
      if( tmp )
        { set_retval( retval, tmp );
          if( delete_output_on_interrupt ) cleanup_and_fail( retval ); }
      }
  // flush and close encoder if needed
  if( outfd >= 0 && archive_write( 0, 0, encoder ) &&
      LZ_compress_close( encoder ) < 0 )
    { show_error( "LZ_compress_close failed." ); set_retval( retval, 1 ); }
  if( outfd >= 0 && close( outfd ) != 0 )			// to_stdout
    {
    show_error( "Error closing stdout", errno );
    set_retval( retval, 1 );
    }
  return retval;
  }
