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

#include <cctype>
#include <cerrno>
#include <cstdio>

#include "tarlz.h"
#include "common_mutex.h"


const CRC32 crc32c( true );


namespace {

unsigned record_size( const unsigned keyword_size, const unsigned value_size )
  {
  /* length + ' ' + keyword + '=' + value + '\n'
     minimize length; prefer "99<97_bytes>" to "100<97_bytes>" */
  unsigned size = 1 + keyword_size + 1 + value_size + 1;
  size += decimal_digits( decimal_digits( size ) + size );
  return size;
  }


long long parse_decimal( const char * const ptr, const char ** const tailp,
                const int size, const unsigned long long limit = LLONG_MAX )
  {
  unsigned long long result = 0;
  int i = 0;
  while( i < size && std::isspace( (unsigned char)ptr[i] ) ) ++i;
  if( !std::isdigit( (unsigned char)ptr[i] ) ) { *tailp = ptr; return -1; }
  for( ; i < size && std::isdigit( (unsigned char)ptr[i] ); ++i )
    {
    const unsigned long long prev = result;
    result *= 10; result += ptr[i] - '0';
    if( result < prev || result > limit || result > LLONG_MAX )	// overflow
      { *tailp = ptr; return -1; }
    }
  *tailp = ptr + i;
  return result;
  }


unsigned parse_record_crc( const char * const ptr )
  {
  unsigned crc = 0;
  for( int i = 0; i < 8; ++i )
    {
    crc <<= 4;
    if( ptr[i] >= '0' && ptr[i] <= '9' ) crc += ptr[i] - '0';
    else if( ptr[i] >= 'A' && ptr[i] <= 'F' ) crc += ptr[i] + 10 - 'A';
    else if( ptr[i] >= 'a' && ptr[i] <= 'f' ) crc += ptr[i] + 10 - 'a';
    else { crc = 0; break; }		// invalid digit in crc string
    }
  return crc;
  }


unsigned char xdigit( const unsigned value )	// hex digit for 'value'
  {
  if( value <= 9 ) return '0' + value;
  if( value <= 15 ) return 'A' + value - 10;
  return 0;
  }

void print_hex( char * const buf, int size, unsigned long long num )
  { while( --size >= 0 ) { buf[size] = xdigit( num & 0x0F ); num >>= 4; } }

void print_decimal( char * const buf, int size, unsigned long long num )
  { while( --size >= 0 ) { buf[size] = num % 10 + '0'; num /= 10; } }

int print_size_keyword( char * const buf, const int size, const char * keyword )
  {
  // "size keyword=value\n"
  int pos = decimal_digits( size );
  print_decimal( buf, pos, size ); buf[pos++] = ' ';
  while( *keyword ) { buf[pos++] = *keyword; ++keyword; } buf[pos++] = '=';
  return pos;
  }

bool print_record( char * const buf, const int size, const char * keyword,
                   const std::string & value )
  {
  int pos = print_size_keyword( buf, size, keyword );
  std::memcpy( buf + pos, value.c_str(), value.size() );
  pos += value.size(); buf[pos++] = '\n';
  return pos == size;
  }

bool print_record( char * const buf, const int size, const char * keyword,
                   const unsigned long long value )
  {
  int pos = print_size_keyword( buf, size, keyword );
  const int vd = decimal_digits( value );
  print_decimal( buf + pos, vd, value ); pos += vd; buf[pos++] = '\n';
  return pos == size;
  }

bool print_record( char * const buf, const int size, const char * keyword,
                   const Etime & value )
  {
  int pos = print_size_keyword( buf, size, keyword );
  pos += value.print( buf + pos ); buf[pos++] = '\n';
  return pos == size;
  }

} // end namespace


unsigned Etime::decimal_size() const
  {
  unsigned size = 1 + ( sec_ < 0 );	// first digit + negative sign
  for( long long n = sec_; n >= 10 || n <= -10; n /= 10 ) ++size;
  if( nsec_ > 0 && nsec_ <= 999999999 )
    { size += 2;		// decimal point + first fractional digit
      for( int n = nsec_; n >= 10; n /= 10 ) ++size; }
  return size;
  }

unsigned Etime::print( char * const buf ) const
  {
  int len = 0;
  if( nsec_ > 0 && nsec_ <= 999999999 )
    { for( int n = nsec_; n > 0; n /= 10 ) buf[len++] = n % 10 + '0';
      buf[len++] = '.'; }
  long long n = sec_;
  do { long long on = n; n /= 10; buf[len++] = llabs( on - 10 * n ) + '0'; }
  while( n != 0 );
  if( sec_ < 0 ) buf[len++] = '-';
  for( int i = 0; i < len / 2; ++i ) std::swap( buf[i], buf[len-i-1] );
  return len;
  }

bool Etime::parse( const char * const ptr, const char ** const tailp,
                   const int size )
  {
  char * tail;
  errno = 0;
  long long s = strtoll( ptr, &tail, 10 );
  if( tail == ptr || tail - ptr > size || errno ||
      ( *tail != 0 && *tail != '\n' && *tail != '.' ) ) return false;
  int ns = 0;
  if( *tail == '.' )		// parse nanoseconds and any extra digits
    {
    ++tail;
    if( tail - ptr >= size || !std::isdigit( (unsigned char)*tail ) )
      return false;
    for( int factor = 100000000;
         tail - ptr < size && std::isdigit( (unsigned char)*tail );
         ++tail, factor /= 10 )
      ns += factor * ( *tail - '0' );
    }
  sec_ = s; nsec_ = ns; if( tailp ) *tailp = tail;
  return true;
  }


std::vector< std::string > Extended::unknown_keywords;
const std::string Extended::crc_record( "22 GNU.crc32=00000000\n" );

void Extended::calculate_sizes() const
  {
  if( linkpath_.size() > max_edata_size || path_.size() > max_edata_size )
    { full_size_ = -3; return; }
  linkpath_recsize_ = linkpath_.size() ? record_size( 8, linkpath_.size() ) : 0;
  path_recsize_ = path_.size() ? record_size( 4, path_.size() ) : 0;
  file_size_recsize_ =
    ( file_size_ > 0 ) ? record_size( 4, decimal_digits( file_size_ ) ) : 0;
  uid_recsize_ = ( uid_ >= 0 ) ? record_size( 3, decimal_digits( uid_ ) ) : 0;
  gid_recsize_ = ( gid_ >= 0 ) ? record_size( 3, decimal_digits( gid_ ) ) : 0;
  atime_recsize_ =
    atime_.out_of_ustar_range() ? record_size( 5, atime_.decimal_size() ) : 0;
  mtime_recsize_ =
    mtime_.out_of_ustar_range() ? record_size( 5, mtime_.decimal_size() ) : 0;
  const long long tmp = linkpath_recsize_ + path_recsize_ +
                        file_size_recsize_ + uid_recsize_ + gid_recsize_ +
                        atime_recsize_ + mtime_recsize_ + crc_record.size();
  if( tmp > max_edata_size ) { full_size_ = -3; return; }
  edsize_ = tmp;
  padded_edsize_ = round_up( edsize_ );
  if( padded_edsize_ > max_edata_size ) { full_size_ = -3; return; }
  full_size_ = header_size + padded_edsize_;
  }


// print a diagnostic for each unknown keyword once per keyword
void Extended::unknown_keyword( const char * const buf, const int size,
                        std::vector< std::string > * const msg_vecp ) const
  {
  // prevent two threads from modifying the list of keywords at the same time
  static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  int eq_pos = 0;				// position of '=' in buf

  while( eq_pos < size && buf[eq_pos] != '=' ) ++eq_pos;
  const std::string keyword( buf, eq_pos );
  xlock( &mutex );
  for( unsigned i = 0; i < unknown_keywords.size(); ++i )
    if( keyword == unknown_keywords[i] ) { xunlock( &mutex ); return; }
  unknown_keywords.push_back( keyword );
  xunlock( &mutex );
  const char * str = "Ignoring unknown extended header keyword '%s'";
  if( !msg_vecp ) print_error( 0, str, keyword.c_str() );
  else
    { msg_vecp->push_back( std::string() );
      format_error( msg_vecp->back(), 0, str, keyword.c_str() ); }
  }


/* Return the size of the extended block, or 0 if empty.
   Return -1 if error, -2 if out of memory, -3 if block too long. */
int Extended::format_block( Resizable_buffer & rbuf ) const
  {
  const int bufsize = full_size();		// recalculate sizes if needed
  if( bufsize <= 0 ) return bufsize;		// error or no extended data
  if( !rbuf.resize( bufsize ) ) return -2;	// extended block buffer
  uint8_t * const header = rbuf.u8();		// extended header
  char * const buf = rbuf() + header_size;	// extended records
  init_tar_header( header );
  header[typeflag_o] = tf_extended;		// fill only required fields
  print_octal( header + size_o, size_l - 1, edsize_ );
  print_octal( header + chksum_o, chksum_l - 1, ustar_chksum( header ) );

  if( path_recsize_ && !print_record( buf, path_recsize_, "path", path_ ) )
    return -1;
  int pos = path_recsize_;
  if( linkpath_recsize_ &&
      !print_record( buf + pos, linkpath_recsize_, "linkpath", linkpath_ ) )
    return -1;
  pos += linkpath_recsize_;
  if( file_size_recsize_ &&
      !print_record( buf + pos, file_size_recsize_, "size", file_size_ ) )
    return -1;
  pos += file_size_recsize_;
  if( uid_recsize_ && !print_record( buf + pos, uid_recsize_, "uid", uid_ ) )
    return -1;
  pos += uid_recsize_;
  if( gid_recsize_ && !print_record( buf + pos, gid_recsize_, "gid", gid_ ) )
    return -1;
  pos += gid_recsize_;
  if( atime_recsize_ &&
      !print_record( buf + pos, atime_recsize_, "atime", atime_ ) )
    return -1;
  pos += atime_recsize_;
  if( mtime_recsize_ &&
      !print_record( buf + pos, mtime_recsize_, "mtime", mtime_ ) )
    return -1;
  pos += mtime_recsize_;
  const unsigned crc_size = Extended::crc_record.size();
  std::memcpy( buf + pos, Extended::crc_record.c_str(), crc_size );
  pos += crc_size;
  if( pos != edsize_ ) return -1;
  print_hex( buf + edsize_ - 9, 8,
             crc32c.windowed_crc( (const uint8_t *)buf, edsize_ - 9, edsize_ ) );
  if( padded_edsize_ > edsize_ )			// set padding to zero
    std::memset( buf + edsize_, 0, padded_edsize_ - edsize_ );
  crc_present_ = true;
  return bufsize;
  }


const char * Extended::full_size_error() const
  {
  const char * const eferec_msg = "Error formatting extended records.";
  switch( full_size_ )
    {
    case -1: return eferec_msg;
    case -2: return mem_msg2;
    case -3: return longrec_msg;
    default: internal_error( "invalid call to full_size_error." );
             return 0;				// keep compiler quiet
    }
  }


bool Extended::parse( const char * const buf, const int edsize,
                      const bool permissive,
                      std::vector< std::string > * const msg_vecp )
  {
  reset(); full_size_ = -4;			// invalidate cached sizes
  for( int pos = 0; pos < edsize; )		// parse records
    {
    const char * tail;
    const int rsize =
      parse_decimal( buf + pos, &tail, edsize - pos, edsize - pos );
    if( rsize <= 0 || tail[0] != ' ' || buf[pos+rsize-1] != '\n' ) return false;
    ++tail;	// point to keyword
    // rest = length of (keyword + '=' + value) without the final newline
    const int rest = ( buf + ( pos + rsize - 1 ) ) - tail;
    if( rest > 5 && std::memcmp( tail, "path=", 5 ) == 0 )
      {
      if( path_.size() && !permissive ) return false;
      int len = rest - 5;
      while( len > 1 && tail[5+len-1] == '/' ) --len;	// trailing '/'
      path_.assign( tail + 5, len );
      // this also truncates path_ at the first embedded null character
      path_.assign( remove_leading_dotslash( path_.c_str(), &removed_prefix ) );
      }
    else if( rest > 9 && std::memcmp( tail, "linkpath=", 9 ) == 0 )
      {
      if( linkpath_.size() && !permissive ) return false;
      int len = rest - 9;
      while( len > 1 && tail[9+len-1] == '/' ) --len;	// trailing '/'
      linkpath_.assign( tail + 9, len );
      }
    else if( rest > 5 && std::memcmp( tail, "size=", 5 ) == 0 )
      {
      if( file_size_ != 0 && !permissive ) return false;
      file_size_ = parse_decimal( tail + 5, &tail, rest - 5, max_file_size );
      // overflow, parse error, or size fits in ustar header
      if( file_size_ < 1LL << 33 || tail != buf + ( pos + rsize - 1 ) )
        return false;
      }
    else if( rest > 4 && std::memcmp( tail, "uid=", 4 ) == 0 )
      {
      if( uid_ >= 0 && !permissive ) return false;
      uid_ = parse_decimal( tail + 4, &tail, rest - 4 );
      // overflow, parse error, or uid fits in ustar header
      if( uid_ < 1 << 21 || tail != buf + ( pos + rsize - 1 ) ) return false;
      }
    else if( rest > 4 && std::memcmp( tail, "gid=", 4 ) == 0 )
      {
      if( gid_ >= 0 && !permissive ) return false;
      gid_ = parse_decimal( tail + 4, &tail, rest - 4 );
      // overflow, parse error, or gid fits in ustar header
      if( gid_ < 1 << 21 || tail != buf + ( pos + rsize - 1 ) ) return false;
      }
    else if( rest > 6 && std::memcmp( tail, "atime=", 6 ) == 0 )
      {
      if( atime_.isvalid() && !permissive ) return false;
      if( !atime_.parse( tail + 6, &tail, rest - 6 ) ||		// parse error
          tail != buf + ( pos + rsize - 1 ) ) return false;
      }
    else if( rest > 6 && std::memcmp( tail, "mtime=", 6 ) == 0 )
      {
      if( mtime_.isvalid() && !permissive ) return false;
      if( !mtime_.parse( tail + 6, &tail, rest - 6 ) ||		// parse error
          tail != buf + ( pos + rsize - 1 ) ) return false;
      }
    else if( rest > 10 && std::memcmp( tail, "GNU.crc32=", 10 ) == 0 )
      {
      if( crc_present_ && !permissive ) return false;
      if( rsize != (int)crc_record.size() ) return false;
      crc_present_ = true;
      const unsigned stored_crc = parse_record_crc( tail + 10 );
      const unsigned computed_crc =
        crc32c.windowed_crc( (const uint8_t *)buf, pos + rsize - 9, edsize );
      if( stored_crc != computed_crc )
        {
        if( verbosity < 1 ) return false;
        const char * str = "CRC mismatch in extended records; stored %08X, computed %08X";
        if( !msg_vecp ) print_error( 0, str, stored_crc, computed_crc );
        else
          { msg_vecp->push_back( std::string() );
            format_error( msg_vecp->back(), 0, str, stored_crc, computed_crc ); }
        return false;
        }
      }
    else if( ( rest < 8 || std::memcmp( tail, "comment=", 8 ) != 0 ) &&
             verbosity >= 1 ) unknown_keyword( tail, rest, msg_vecp );
    pos += rsize;
    }
  return true;
  }


/* If not already initialized, copy linkpath, path, file_size, uid, gid,
   atime, and mtime from ustar header. */
void Extended::fill_from_ustar( const Tar_header header )
  {
  if( linkpath_.empty() )		// copy linkpath from ustar header
    {
    int len = 0;
    while( len < linkname_l && header[linkname_o+len] ) ++len;
    while( len > 1 && header[linkname_o+len-1] == '/' ) --len;	// trailing '/'
    if( len > 0 )
      {
      linkpath_.assign( (const char *)header + linkname_o, len );
      full_size_ = -4;
      }
    }

  if( path_.empty() )			// copy path from ustar header
    {					// the entire path may be in prefix
    char stored_name[prefix_l+1+name_l+1];
    int len = 0;
    while( len < prefix_l && header[prefix_o+len] )
      { stored_name[len] = header[prefix_o+len]; ++len; }
    if( len && header[name_o] ) stored_name[len++] = '/';
    for( int i = 0; i < name_l && header[name_o+i]; ++i )
      { stored_name[len] = header[name_o+i]; ++len; }
    while( len > 0 && stored_name[len-1] == '/' ) --len;	// trailing '/'
    stored_name[len] = 0;
    path( remove_leading_dotslash( stored_name, &removed_prefix ) );
    }

  const Typeflag typeflag = (Typeflag)header[typeflag_o];
  if( file_size_ == 0 &&		// copy file_size from ustar header
      ( typeflag == tf_regular || typeflag == tf_hiperf ) )
    file_size( parse_octal( header + size_o, size_l ) );
  if( uid_ < 0 ) uid_ = parse_octal( header + uid_o, uid_l );
  if( gid_ < 0 ) gid_ = parse_octal( header + gid_o, gid_l );
  if( !atime_.isvalid() )
    atime_.set( parse_octal( header + mtime_o, mtime_l ) );	// 33 bits
  if( !mtime_.isvalid() )
    mtime_.set( parse_octal( header + mtime_o, mtime_l ) );	// 33 bits
  }


/* Return file size from record or from ustar header, and reset file_size_.
   Used for fast parsing of headers in uncompressed archives. */
long long Extended::get_file_size_and_reset( const Tar_header header )
  {
  const long long tmp = file_size_;
  file_size( 0 );				// reset full_size_
  const Typeflag typeflag = (Typeflag)header[typeflag_o];
  if( typeflag != tf_regular && typeflag != tf_hiperf ) return 0;
  if( tmp > 0 ) return tmp;
  return parse_octal( header + size_o, size_l );
  }
