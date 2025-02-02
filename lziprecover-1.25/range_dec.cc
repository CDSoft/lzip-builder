/* Lziprecover - Data recovery tool for the lzip format
   Copyright (C) 2009-2025 Antonio Diaz Diaz.

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
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>

#include "lzip.h"
#include "decoder.h"
#include "lzip_index.h"


namespace {

bool decompress_member( const int infd, const Cl_options & cl_opts,
          const Pretty_print & pp, const unsigned long long mpos,
          const unsigned long long outskip, const unsigned long long outend )
  {
  Range_decoder rdec( infd );
  Lzip_header header;
  rdec.read_data( header.data, header.size );
  if( rdec.finished() )			// End Of File
    { pp( "File ends unexpectedly at member header." ); return false; }
  if( !header.check_magic() ) { pp( bad_magic_msg ); return false; }
  if( !header.check_version() )
    { pp( bad_version( header.version() ) ); return false; }
  const unsigned dictionary_size = header.dictionary_size();
  if( !isvalid_ds( dictionary_size ) ) { pp( bad_dict_msg ); return false; }

  if( verbosity >= 2 ) pp();

  LZ_decoder decoder( rdec, dictionary_size, outfd, outskip, outend );
  const int result = decoder.decode_member( pp, cl_opts.ignore_errors );
  if( result != 0 )
    {
    if( verbosity >= 0 && result <= 2 )
      {
      pp();
      std::fprintf( stderr, "%s at pos %llu\n", ( result == 2 ) ?
                    "File ends unexpectedly" : "Decoder error",
                    mpos + rdec.member_position() );
      }
    return false;
    }
  if( decoder.data_position() < outend - outskip )
    {
    if( verbosity >= 0 )
      { pp(); std::fprintf( stderr,
              "%sMember at pos %llu contains only %llu bytes of %llu requested.\n",
              ( verbosity >= 2 ) ? "\n" : "", mpos,
              decoder.data_position() - outskip, outend - outskip ); }
    return false;
    }
  if( verbosity >= 2 ) std::fputs( "done\n", stderr );
  return true;
  }

} // end namespace


const char * format_num( unsigned long long num,
                         unsigned long long limit,
                         const int set_prefix )
  {
  enum { buffers = 8, bufsize = 32, n = 10 };
  const char * const si_prefix[n] =
    { "k", "M", "G", "T", "P", "E", "Z", "Y", "R", "Q" };
  const char * const binary_prefix[n] =
    { "Ki", "Mi", "Gi", "Ti", "Pi", "Ei", "Zi", "Yi", "Ri", "Qi" };
  static char buffer[buffers][bufsize];	// circle of static buffers for printf
  static int current = 0;
  static bool si = true;

  if( set_prefix ) si = set_prefix > 0;
  unsigned long long den = 1;
  const unsigned factor = si ? 1000 : 1024;
  char * const buf = buffer[current++]; current %= buffers;
  const char * const * prefix = si ? si_prefix : binary_prefix;
  const char * p = "";

  for( int i = 0; i < n && num / den >= factor && den * factor > den; ++i )
    { if( num / den <= limit && num % ( den * factor ) != 0 ) break;
      den *= factor; p = prefix[i]; }
  if( num % den == 0 )
    snprintf( buf, bufsize, "%llu %s", num / den, p );
  else
    snprintf( buf, bufsize, "%3.2f %s", (double)num / den, p );
  return buf;
  }


int range_decompress( const std::string & input_filename,
                      const std::string & default_output_filename,
                      const Cl_options & cl_opts, Block range,
                      const bool force, const bool to_stdout )
  {
  const char * const filename = input_filename.c_str();
  struct stat in_stats;
  const int infd = open_instream( filename, &in_stats, false, true );
  if( infd < 0 ) return 1;

  const Lzip_index lzip_index( infd, cl_opts, cl_opts.ignore_errors,
                               cl_opts.ignore_errors );
  if( lzip_index.retval() != 0 )
    { show_file_error( filename, lzip_index.error().c_str() );
      return lzip_index.retval(); }

  const long long udata_size = lzip_index.udata_size();
  if( range.end() > udata_size )
    range.size( std::max( 0LL, udata_size - range.pos() ) );
  if( range.size() <= 0 )
    { if( udata_size > 0 )
        show_file_error( filename, "Nothing to do; range is empty." );
      return 0; }

  if( to_stdout || default_output_filename.empty() ) outfd = STDOUT_FILENO;
  else
    {
    output_filename = default_output_filename;
    set_signal_handler();
    if( !open_outstream( force, true, false, false, true ) ) return 1;
    }

  if( verbosity >= 1 )
    std::fprintf( stderr, "Decompressing range %sB to %sB (%sB of %sBytes)\n",
                  format_num( range.pos() ),
                  format_num( range.pos() + range.size() ),
                  format_num( range.size() ), format_num( udata_size ) );

  Pretty_print pp( input_filename );
  bool error = false;
  for( long i = 0; i < lzip_index.members(); ++i )
    {
    const Block & db = lzip_index.dblock( i );
    if( range.overlaps( db ) )
      {
      if( verbosity >= 3 && lzip_index.members() > 1 )
        std::fprintf( stderr, "Decompressing member %3ld\n", i + 1 );
      const long long outskip = std::max( 0LL, range.pos() - db.pos() );
      const long long outend = std::min( db.size(), range.end() - db.pos() );
      const long long mpos = lzip_index.mblock( i ).pos();
      if( !safe_seek( infd, mpos, input_filename ) ) cleanup_and_fail( 1 );
      if( !decompress_member( infd, cl_opts, pp, mpos, outskip, outend ) )
        { if( cl_opts.ignore_errors ) error = true; else cleanup_and_fail( 2 ); }
      pp.reset();
      }
    }
  if( close( infd ) != 0 )
    { show_file_error( filename, "Error closing input file", errno );
      cleanup_and_fail( 1 ); }
  if( !close_outstream( &in_stats ) ) cleanup_and_fail( 1 );
  if( verbosity >= 2 && !error )
    std::fputs( "Byte range decompressed successfully.\n", stderr );
  return 0;				// either no error or ignored
  }
