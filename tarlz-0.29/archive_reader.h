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

struct Archive_descriptor
  {
  const std::string name;
  const char * const namep;	// printable archive name
  const int infd;
  const Lzip_index lzip_index;
  const bool seekable;
  const bool indexed;		// archive is a compressed regular file

  Archive_descriptor( const std::string & archive_name );
  };


class Archive_reader_base	// base of serial and indexed readers
  {
public:
  const Archive_descriptor & ad;
protected:
  LZ_Decoder * decoder;		// destructor closes it if needed
  const char * e_msg_;		// message for show_file_error
  int e_code_;			// copy of errno
  int e_size_;			// partial size read in case of read error
  bool e_skip_;			// corrupt header skipped
  bool fatal_;

  int err( const int retval, const char * const msg = "", const int code = 0,
           const int size = 0, const bool skip = false )
    { e_msg_ = msg; e_code_ = code; e_size_ = size; e_skip_ = skip;
      if( retval >= 0 ) return retval;
      fatal_ = true; if( !*e_msg_ ) e_msg_ = "Fatal error"; return -retval; }

  Archive_reader_base( const Archive_descriptor & d )
    : ad( d ), decoder( 0 ), e_msg_( "" ), e_code_( 0 ), e_size_( 0 ),
      e_skip_( false ), fatal_( false ) {}

public:
  virtual ~Archive_reader_base()
    { if( decoder != 0 ) LZ_decompress_close( decoder ); }

  const char * e_msg() const { return e_msg_; }
  int e_code() const { return e_code_; }
  int e_size() const { return e_size_; }
  bool e_skip() const { return e_skip_; }
  bool fatal() const { return fatal_; }

  /* Read 'size' uncompressed bytes, decompressing the input if needed.
     Return value: 0 = OK, 1 = OOM or read error, 2 = EOF or invalid data.
     If !OK, fills all the e_* variables. */
  virtual int read( uint8_t * const buf, const int size ) = 0;

  int parse_records( Extended & extended,
                     const Tar_header header, Resizable_buffer & rbuf,
                     const char * const default_msg, const bool permissive,
                     std::vector< std::string > * const msg_vecp = 0 );
  };


class Archive_reader : public Archive_reader_base	// serial reader
  {
  bool first_read;
  bool uncompressed_seekable;		// value set by first read call
  bool at_eof;

public:
  Archive_reader( const Archive_descriptor & d )
    : Archive_reader_base( d ), first_read( true ),
      uncompressed_seekable( false ), at_eof( false ) {}

  int read( uint8_t * const buf, const int size );
  int skip_member( const Extended & extended );
  };


/* If the archive is compressed seekable (indexed), several indexed readers
   can be constructed sharing the same Archive_descriptor, for example to
   decode the archive in parallel.
*/
class Archive_reader_i : public Archive_reader_base	// indexed reader
  {
  long long data_pos_;		// current decompressed position in archive
  long long mdata_end_;		// current member decompressed end
  long long archive_pos;	// current position in archive for pread
  long member_id;		// current member unless reading beyond

public:
  Archive_reader_i( const Archive_descriptor & d )
    : Archive_reader_base( d ),
      data_pos_( 0 ), mdata_end_( 0 ), archive_pos( 0 ), member_id( 0 )
    {
    decoder = LZ_decompress_open();
    if( !decoder || LZ_decompress_errno( decoder ) != LZ_ok )
      { LZ_decompress_close( decoder ); decoder = 0; fatal_ = true; }
    }

  long long data_pos() const { return data_pos_; }
  long long mdata_end() const { return mdata_end_; }
  bool at_member_end() const { return data_pos_ == mdata_end_; }

  // Reset decoder and set position to the start of the member.
  void set_member( const long i );

  int read( uint8_t * const buf, const int size );
  int skip_member( const Extended & extended );
  };


const char * const empty_member_msg = "Empty lzip member not allowed.";
