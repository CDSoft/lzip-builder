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
#include <cerrno>
#include <cstdio>
#include <queue>
#include <stdint.h>		// for lzlib.h
#include <unistd.h>
#include <utime.h>
#include <sys/stat.h>
#if !defined __FreeBSD__ && !defined __OpenBSD__ && !defined __NetBSD__ && \
    !defined __DragonFly__ && !defined __APPLE__ && !defined __OS2__
#include <sys/sysmacros.h>	// for major, minor, makedev
#else
#include <sys/types.h>		// for major, minor, makedev
#endif
#include <lzlib.h>

#include "tarlz.h"
#include "arg_parser.h"
#include "lzip_index.h"
#include "archive_reader.h"
#include "common_mutex.h"
#include "decode.h"

/* Parallel decode does not skip; it exits at the first error.
   When a problem is detected by any worker:
   - the worker requests mastership and returns.
   - the courier discards new packets received or collected.
   - the other workers return.
   - the muxer drains the queue and returns. */

namespace {

const char * const other_msg = "Another worker found an error.";

/* line is preformatted and newline terminated except for prefix and errors.
   ok with an empty line is a no-op. */
struct Packet			// member name and metadata or error message
  {
  enum Status { ok, member_done, diag, prefix, error1, error2 };

  long member_id;	// lzip member containing the header of this tar member
  std::string line;	// member name and metadata ready to print, if any
  Status status;	// diagnostics and errors go to stderr
  int errcode;		// for errors
  Packet( const long i, const char * const msg, const Status s, const int e )
    : member_id( i ), line( msg ), status( s ), errcode( e ) {}
  };


class Packet_courier			// moves packets around
  {
public:
  unsigned ocheck_counter;
  unsigned owait_counter;
private:
  long error_member_id;		// first lzip member with error/misalign/eoa/eof
  int deliver_id;		// worker queue currently delivering packets
  int master_id;		// worker in charge if error/misalign/eoa/eof
  std::vector< std::queue< const Packet * > > opacket_queues;
  int num_working;			// number of workers still running
  const int num_workers;		// number of workers
  const unsigned out_slots;		// max output packets per queue
  pthread_mutex_t omutex;
  pthread_cond_t oav_or_exit;	// output packet available or all workers exited
  std::vector< pthread_cond_t > slot_av;	// output slot available
  pthread_cond_t check_master;
  bool eoa_found_;				// EOA blocks found

  Packet_courier( const Packet_courier & );	// declared as private
  void operator=( const Packet_courier & );	// declared as private

public:
  Packet_courier( const int workers, const int slots )
    : ocheck_counter( 0 ), owait_counter( 0 ), error_member_id( -1 ),
      deliver_id( 0 ), master_id( -1 ), opacket_queues( workers ),
      num_working( workers ), num_workers( workers ),
      out_slots( slots ), slot_av( workers ), eoa_found_( false )
    {
    xinit_mutex( &omutex ); xinit_cond( &oav_or_exit );
    for( unsigned i = 0; i < slot_av.size(); ++i ) xinit_cond( &slot_av[i] );
    xinit_cond( &check_master );
    }

  ~Packet_courier()
    {
    xdestroy_cond( &check_master );
    for( unsigned i = 0; i < slot_av.size(); ++i ) xdestroy_cond( &slot_av[i] );
    xdestroy_cond( &oav_or_exit ); xdestroy_mutex( &omutex );
    }

  bool eoa_found() const { return eoa_found_; }
  void report_eoa() { eoa_found_ = true; }

  bool mastership_granted() const { return master_id >= 0; }

  bool request_mastership( const long member_id, const int worker_id )
    {
    xlock( &omutex );
    if( mastership_granted() )			// already granted
      { xunlock( &omutex ); return master_id == worker_id; }
    if( error_member_id < 0 || error_member_id > member_id )
      error_member_id = member_id;
    while( !mastership_granted() &&
           ( worker_id != deliver_id || !opacket_queues[deliver_id].empty() ) )
      xwait( &check_master, &omutex );
    if( !mastership_granted() &&
        // redundant conditions useful for the compiler
        worker_id == deliver_id && opacket_queues[deliver_id].empty() )
      {
      master_id = worker_id;			// grant mastership
      for( int i = 0; i < num_workers; ++i )	// delete all packets
        while( !opacket_queues[i].empty() )
          opacket_queues[i].pop();
      xbroadcast( &check_master );
      xunlock( &omutex );
      return true;
      }
    xunlock( &omutex );
    return false;		// mastership granted to another worker
    }

  void worker_finished()
    {
    // notify muxer when last worker exits
    xlock( &omutex );
    if( --num_working == 0 ) xsignal( &oav_or_exit );
    xunlock( &omutex );
    }

  /* Collect a packet from a worker.
     If a packet is rejected, the worker must terminate. */
  bool collect_packet( const long member_id, const int worker_id,
                       const char * const msg, const Packet::Status status,
                       const int errcode = 0 )
    {
    const Packet * const opacket = new Packet( member_id, msg, status, errcode );
    xlock( &omutex );
    if( ( mastership_granted() && master_id != worker_id ) ||
        ( error_member_id >= 0 && error_member_id < opacket->member_id ) )
      { xunlock( &omutex ); delete opacket; return false; }	// reject packet
    while( opacket_queues[worker_id].size() >= out_slots )
      xwait( &slot_av[worker_id], &omutex );
    opacket_queues[worker_id].push( opacket );
    if( worker_id == deliver_id ) xsignal( &oav_or_exit );
    xunlock( &omutex );
    return true;
    }

  /* Deliver packets to muxer.
     If packet.status == Packet::member_done, move to next queue.
     If packet.line.empty(), wait again (empty lzip member or -q). */
  void deliver_packets( std::vector< const Packet * > & opacket_vector )
    {
    opacket_vector.clear();
    xlock( &omutex );
    ++ocheck_counter;
    do {
      while( opacket_queues[deliver_id].empty() && num_working > 0 )
        {
        ++owait_counter;
        if( !mastership_granted() && error_member_id >= 0 )
          xbroadcast( &check_master );	// mastership requested not yet granted
        xwait( &oav_or_exit, &omutex );
        }
      while( !opacket_queues[deliver_id].empty() )
        {
        const Packet * opacket = opacket_queues[deliver_id].front();
        opacket_queues[deliver_id].pop();
        if( opacket_queues[deliver_id].size() + 1 == out_slots )
          xsignal( &slot_av[deliver_id] );
        if( opacket->status == Packet::member_done && !mastership_granted() )
          { if( ++deliver_id >= num_workers ) deliver_id = 0; }
        if( !opacket->line.empty() ) opacket_vector.push_back( opacket );
        else delete opacket;
        }
      }
    while( opacket_vector.empty() && num_working > 0 );
    xunlock( &omutex );
    }

  bool finished()		// all packets delivered to muxer
    {
    if( num_working != 0 ) return false;
    for( int i = 0; i < num_workers; ++i )
      if( !opacket_queues[i].empty() ) return false;
    return true;
    }
  };


// prevent two threads from extracting the same file at the same time
class Name_monitor
  {
  std::vector< unsigned > crc_vector;
  std::vector< std::string > name_vector;
  pthread_mutex_t mutex;

public:
  Name_monitor( const int num_workers )
    : crc_vector( num_workers ), name_vector( num_workers )
    { if( num_workers > 0 ) xinit_mutex( &mutex ); }

  bool reserve_name( const unsigned worker_id, const std::string & filename )
    {
    // compare the CRCs of the names; compare the names if the CRCs collide
    const unsigned crc =
      crc32c.compute_crc( (const uint8_t *)filename.c_str(), filename.size() );
    xlock( &mutex );
    for( unsigned i = 0; i < crc_vector.size(); ++i )
      if( crc_vector[i] == crc && crc != 0 && i != worker_id &&
          name_vector[i] == filename )
        { xunlock( &mutex ); return false; }	// filename already reserved
    crc_vector[worker_id] = crc; name_vector[worker_id] = filename;
    xunlock( &mutex );
    return true;
    }
  };


struct Trival				// triple result value
  {
  const char * msg;
  int errcode;
  int retval;
  explicit Trival( const char * const s = 0, const int e = 0, const int r = 0 )
    : msg( s ), errcode( e ), retval( r ) {}
  };


Trival skip_member_lz( Archive_reader_i & ar, Packet_courier & courier,
                       const Extended & extended, const long member_id,
                       const int worker_id, const Typeflag typeflag )
  {
  if( data_may_follow( typeflag ) )
    { const int ret = ar.skip_member( extended );
      if( ret != 0 ) return Trival( ar.e_msg(), ar.e_code(), ret ); }
  if( ar.at_member_end() &&
      !courier.collect_packet( member_id, worker_id, "", Packet::member_done ) )
    return Trival( other_msg, 0, 1);
  return Trival();
  }


Trival compare_member_lz( const Cl_options & cl_opts,
                          Archive_reader_i & ar, Packet_courier & courier,
                          const Extended & extended, const Tar_header header,
                          Resizable_buffer & rbuf, const long member_id,
                          const int worker_id )
  {
  if( verbosity < 1 ) rbuf()[0] = 0;
  else if( !format_member_name( extended, header, rbuf, verbosity > 1 ) )
    return Trival( mem_msg, 0, 1 );
  std::string estr, ostr;
  const bool stat_differs =
    !compare_file_type( estr, ostr, cl_opts, extended, header );
  if( ( rbuf()[0] && !courier.collect_packet( member_id, worker_id, rbuf(),
                                              Packet::ok ) ) ||
      ( estr.size() && !courier.collect_packet( member_id, worker_id,
                                              estr.c_str(), Packet::diag ) ) ||
      ( ostr.size() && !courier.collect_packet( member_id, worker_id,
                                              ostr.c_str(), Packet::ok ) ) ||
      ( extended.file_size() <= 0 && ar.at_member_end() &&
        !courier.collect_packet( member_id, worker_id, "", Packet::member_done ) ) )
    return Trival( other_msg, 0, 1 );
  if( extended.file_size() <= 0 ) return Trival();
  const Typeflag typeflag = (Typeflag)header[typeflag_o];
  if( ( typeflag != tf_regular && typeflag != tf_hiperf ) || stat_differs )
    return skip_member_lz( ar, courier, extended, member_id, worker_id, typeflag );
  // else compare file contents
  const char * const filename = extended.path().c_str();
  const int infd2 = open_instream( filename );
  if( infd2 < 0 ) { set_error_status( 1 );
    return skip_member_lz( ar, courier, extended, member_id, worker_id, typeflag ); }
  const int ret = compare_file_contents( estr, ostr, ar, extended.file_size(),
                                         filename, infd2 );
  if( ret != 0 ) return Trival( ar.e_msg(), ar.e_code(), ret );
  if( ( estr.size() && !courier.collect_packet( member_id, worker_id,
                                              estr.c_str(), Packet::diag ) ) ||
      ( ostr.size() && !courier.collect_packet( member_id, worker_id,
                                              ostr.c_str(), Packet::ok ) ) ||
      ( ar.at_member_end() &&
        !courier.collect_packet( member_id, worker_id, "", Packet::member_done ) ) )
    return Trival( other_msg, 0, 1 );
  return Trival();
  }


Trival list_member_lz( Archive_reader_i & ar, Packet_courier & courier,
                       const Extended & extended, const Tar_header header,
                       Resizable_buffer & rbuf, const long member_id,
                       const int worker_id )
  {
  if( verbosity < 0 ) rbuf()[0] = 0;
  else if( !format_member_name( extended, header, rbuf, verbosity > 0 ) )
    return Trival( mem_msg, 0, 1 );
  const int ret = data_may_follow( (Typeflag)header[typeflag_o] ) ?
    ar.skip_member( extended ) : 0;	// print name even on read error
  if( !courier.collect_packet( member_id, worker_id, rbuf(),
                   ar.at_member_end() ? Packet::member_done : Packet::ok ) )
    return Trival( other_msg, 0, 1 );
  if( ret != 0 ) return Trival( ar.e_msg(), ar.e_code(), ret );
  return Trival();
  }


Trival extract_member_lz( const Cl_options & cl_opts,
                          Archive_reader_i & ar, Packet_courier & courier,
                          const Extended & extended, const Tar_header header,
                          Resizable_buffer & rbuf, const long member_id,
                          const int worker_id, Name_monitor & name_monitor )
  {
  const char * const filename = extended.path().c_str();
  const Typeflag typeflag = (Typeflag)header[typeflag_o];
  if( contains_dotdot( filename ) )
    {
    if( format_file_error( rbuf, filename, dotdot_msg ) &&
        !courier.collect_packet( member_id, worker_id, rbuf(), Packet::diag ) )
      return Trival( other_msg, 0, 1 );
    return skip_member_lz( ar, courier, extended, member_id, worker_id, typeflag );
    }
  // skip member if another copy is already being extracted by another thread
  if( !name_monitor.reserve_name( worker_id, extended.path() ) )
    {
    if( verbosity >= 3 && format_file_error( rbuf, filename,
                       "Is being extracted by another thread, skipping." ) &&
        !courier.collect_packet( member_id, worker_id, rbuf(), Packet::diag ) )
      return Trival( other_msg, 0, 1 );
    return skip_member_lz( ar, courier, extended, member_id, worker_id, typeflag );
    }
  mode_t mode = parse_octal( header + mode_o, mode_l );	 // 12 bits
  if( geteuid() != 0 && !cl_opts.preserve_permissions ) mode &= ~get_umask();
  int outfd = -1;

  if( verbosity >= 1 )
    {
    if( !format_member_name( extended, header, rbuf, verbosity > 1 ) )
      return Trival( mem_msg, 0, 1 );
    if( !courier.collect_packet( member_id, worker_id, rbuf(), Packet::ok ) )
      return Trival( other_msg, 0, 1 );
    }
  struct stat st;
  bool exists = lstat( filename, &st ) == 0;
  if( !exists && !make_dirs( filename ) )
    {
    if( format_file_error( rbuf, filename, intdir_msg, errno ) &&
        !courier.collect_packet( member_id, worker_id, rbuf(), Packet::diag ) )
      return Trival( other_msg, 0, 1 );
    set_error_status( 1 );
    return skip_member_lz( ar, courier, extended, member_id, worker_id, typeflag );
    }
  /* Remove file before extraction to prevent following links.
     Don't remove an empty dir; another thread may need it. */
  if( exists && ( typeflag != tf_directory || !S_ISDIR( st.st_mode ) ) )
    { exists = false; std::remove( filename ); }

  switch( typeflag )
    {
    case tf_regular:
    case tf_hiperf:
      outfd = open_outstream( filename, true, &rbuf, false );
      if( outfd < 0 )
        {
        if( verbosity >= 0 &&
            !courier.collect_packet( member_id, worker_id, rbuf(), Packet::diag ) )
          return Trival( other_msg, 0, 1 );
        set_error_status( 1 );
        return skip_member_lz( ar, courier, extended, member_id, worker_id,
                               typeflag );
        }
      break;
    case tf_link:
    case tf_symlink:
      {
      const char * const linkname = extended.linkpath().c_str();
      const bool hard = typeflag == tf_link;
      if( ( hard && link( linkname, filename ) != 0 ) ||
          ( !hard && symlink( linkname, filename ) != 0 ) )
        {
        if( format_error( rbuf, errno, cantln_msg, hard ? "" : "sym",
                          linkname, filename ) &&
            !courier.collect_packet( member_id, worker_id, rbuf(), Packet::diag ) )
          return Trival( other_msg, 0, 1 );
        set_error_status( 1 );
        }
      } break;
    case tf_directory:
      if( !exists && mkdir( filename, mode ) != 0 && errno != EEXIST )
        {
        if( format_file_error( rbuf, filename, mkdir_msg, errno ) &&
            !courier.collect_packet( member_id, worker_id, rbuf(), Packet::diag ) )
          return Trival( other_msg, 0, 1 );
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
        if( format_file_error( rbuf, filename, mknod_msg, errno ) &&
            !courier.collect_packet( member_id, worker_id, rbuf(), Packet::diag ) )
          return Trival( other_msg, 0, 1 );
        set_error_status( 1 );
        }
      break;
      }
    case tf_fifo:
      if( mkfifo( filename, mode ) != 0 )
        {
        if( format_file_error( rbuf, filename, mkfifo_msg, errno ) &&
            !courier.collect_packet( member_id, worker_id, rbuf(), Packet::diag ) )
          return Trival( other_msg, 0, 1 );
        set_error_status( 1 );
        }
      break;
    default:
      if( format_error( rbuf, 0, uftype_msg, filename, typeflag ) &&
          !courier.collect_packet( member_id, worker_id, rbuf(), Packet::diag ) )
        return Trival( other_msg, 0, 1 );
      set_error_status( 2 );
      return skip_member_lz( ar, courier, extended, member_id, worker_id,
                             typeflag );
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
      {
      if( format_file_error( rbuf, filename, chown_msg, errno ) &&
          !courier.collect_packet( member_id, worker_id, rbuf(), Packet::diag ) )
        return Trival( other_msg, 0, 1 );
      set_error_status( 1 );
      }
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
        if( outfd >= 0 )
          {
          if( cl_opts.keep_damaged )
            { writeblock( outfd, buf, std::min( rest, (long long)ar.e_size() ) );
              close( outfd ); }
          else { close( outfd ); unlink( filename ); }
          }
        return Trival( ar.e_msg(), ar.e_code(), ret );
        }
      const int wsize = ( rest >= bufsize ) ? bufsize : rest;
      if( outfd >= 0 && writeblock( outfd, buf, wsize ) != wsize )
        { format_file_error( rbuf, filename, wr_err_msg, errno );
          return Trival( rbuf(), 0, 1 ); }
      rest -= wsize;
      }
    }
  if( outfd >= 0 && close( outfd ) != 0 )
      { format_file_error( rbuf, filename, eclosf_msg, errno );
        return Trival( rbuf(), 0, 1 ); }
  if( !islink )
    {
    struct utimbuf t;
    t.actime = extended.atime().sec();
    t.modtime = extended.mtime().sec();
    utime( filename, &t );			// ignore errors
    }
  if( ar.at_member_end() &&
      !courier.collect_packet( member_id, worker_id, "", Packet::member_done ) )
    return Trival( other_msg, 0, 1 );
  return Trival();
  }


struct Worker_arg
  {
  const Cl_options * cl_opts;
  const Archive_descriptor * ad;
  Packet_courier * courier;
  Name_monitor * name_monitor;
  std::vector< char > * name_pending;
  int worker_id;
  int num_workers;
  };


/* Read lzip members from archive, decode their tar members, and give the
   packets produced to courier.
*/
extern "C" void * dworker( void * arg )
  {
  const Worker_arg & tmp = *(const Worker_arg *)arg;
  const Cl_options & cl_opts = *tmp.cl_opts;
  const Archive_descriptor & ad = *tmp.ad;
  Packet_courier & courier = *tmp.courier;
  Name_monitor & name_monitor = *tmp.name_monitor;
  std::vector< char > & name_pending = *tmp.name_pending;
  const int worker_id = tmp.worker_id;
  const int num_workers = tmp.num_workers;

  bool master = false;
  Resizable_buffer rbuf;
  Archive_reader_i ar( ad );			// 1 of N parallel readers
  if( !rbuf.size() || ar.fatal() )
    { if( courier.request_mastership( worker_id, worker_id ) )
        courier.collect_packet( worker_id, worker_id, mem_msg, Packet::error1 );
      goto done; }

  for( long i = worker_id; !master && i < ad.lzip_index.members(); i += num_workers )
    {
    if( ad.lzip_index.dblock( i ).size() <= 0 )		// empty lzip member
      {
      if( courier.collect_packet( i, worker_id, "", Packet::member_done ) )
        continue; else break;
      }

    long long data_end = ad.lzip_index.dblock( i ).end();
    Extended extended;			// metadata from extended records
    bool prev_extended = false;		// prev header was extended
    ar.set_member( i );			// prepare for new member
    while( true )			// process one tar header per iteration
      {
      if( ar.data_pos() >= data_end )	// dblock.end or udata_size
        {
        if( ar.data_pos() == data_end && !prev_extended ) break;
        // member end exceeded or ends in extended, process rest of file
        if( !courier.request_mastership( i, worker_id ) ) goto done;
        master = true;
        if( data_end >= ad.lzip_index.udata_size() )
          { courier.collect_packet( i, worker_id, end_msg, Packet::error2 );
            goto done; }
        data_end = ad.lzip_index.udata_size();
        if( ar.data_pos() == data_end && !prev_extended ) break;
        }
      Tar_header header;
      const int ret = ar.read( header, header_size );
      if( ret != 0 )
        { if( courier.request_mastership( i, worker_id ) )
            courier.collect_packet( i, worker_id, ar.e_msg(),
                    ( ret == 1 ) ? Packet::error1 : Packet::error2, ar.e_code() );
          goto done; }
      if( !check_ustar_chksum( header ) )		// error or EOA
        {
        if( !courier.request_mastership( i, worker_id ) ) goto done;
        if( block_is_zero( header, header_size ) )	// EOA
          {
          if( !prev_extended || cl_opts.permissive ) courier.report_eoa();
          else courier.collect_packet( i, worker_id, fv_msg1, Packet::error2 );
          goto done;
          }
        courier.collect_packet( i, worker_id, ( ar.data_pos() > header_size ) ?
                                bad_hdr_msg : posix_lz_msg, Packet::error2 );
        goto done;
        }

      const Typeflag typeflag = (Typeflag)header[typeflag_o];
      if( typeflag == tf_global )
        {
        const char * msg = 0; int ret = 2;
        Extended dummy;		// global headers are parsed and ignored
        if( prev_extended && !cl_opts.permissive ) msg = fv_msg2;
        else if( ( ret = ar.parse_records( dummy, header, rbuf, gblrec_msg,
                 true ) ) != 0 ) msg = ar.e_msg();
        else
          {
          if( ar.data_pos() == data_end &&	// end of lzip member or EOF
              !courier.collect_packet( i, worker_id, "", Packet::member_done ) )
            goto done;
          continue;
          }
        if( courier.request_mastership( i, worker_id ) )
          courier.collect_packet( i, worker_id, msg, ( ret == 1 ) ?
                                  Packet::error1 : Packet::error2 );
        goto done;
        }
      if( typeflag == tf_extended )
        {
        std::vector< std::string > msg_vec;
        const char * msg = 0; int ret = 2; bool good = false;
        if( prev_extended && !cl_opts.permissive ) msg = fv_msg3;
        else if( ( ret = ar.parse_records( extended, header, rbuf, extrec_msg,
                     cl_opts.permissive, &msg_vec ) ) != 0 ) msg = ar.e_msg();
        else if( !extended.crc_present() && cl_opts.missing_crc )
          { msg = miscrc_msg; ret = 2; }
        else { prev_extended = true; good = true; }
        for( unsigned j = 0; j < msg_vec.size(); ++j )
          if( !courier.collect_packet( i, worker_id, msg_vec[j].c_str(),
              Packet::diag ) ) { good = false; break; }
        if( good ) continue;
        if( courier.request_mastership( i, worker_id ) )
          courier.collect_packet( i, worker_id, msg, ( ret == 1 ) ?
                                  Packet::error1 : Packet::error2 );
        goto done;
        }
      prev_extended = false;

      extended.fill_from_ustar( header );	// copy metadata from header

      /* Skip members with an empty name in the ustar header. If there is an
         extended header in a previous lzip member, its worker will request
         mastership and the skip may fail here. Else the ustar-only unnamed
         member will be ignored. */
      std::string rpmsg;			// removed prefix
      Trival trival;
      if( check_skip_filename( cl_opts, name_pending, extended.path().c_str(),
                               -1, &rpmsg ) )
        trival = skip_member_lz( ar, courier, extended, i, worker_id, typeflag );
      else
        {
        if( verbosity >= 0 && rpmsg.size() &&
            !courier.collect_packet( i, worker_id, rpmsg.c_str(), Packet::prefix ) )
          { trival = Trival( other_msg, 0, 1 ); goto fatal; }
        if( print_removed_prefix( extended.removed_prefix, &rpmsg ) &&
            !courier.collect_packet( i, worker_id, rpmsg.c_str(), Packet::prefix ) )
          { trival = Trival( other_msg, 0, 1 ); goto fatal; }
        if( cl_opts.program_mode == m_list )
          trival = list_member_lz( ar, courier, extended, header, rbuf, i, worker_id );
        else if( extended.path().empty() )
          trival = skip_member_lz( ar, courier, extended, i, worker_id, typeflag );
        else if( cl_opts.program_mode == m_diff )
          trival = compare_member_lz( cl_opts, ar, courier, extended, header,
                                      rbuf, i, worker_id );
        else trival = extract_member_lz( cl_opts, ar, courier, extended, header,
                                         rbuf, i, worker_id, name_monitor );
        }
      if( trival.retval )				// fatal error
fatal:  { if( courier.request_mastership( i, worker_id ) )
            courier.collect_packet( i, worker_id, trival.msg,
                    ( trival.retval == 1 ) ? Packet::error1 : Packet::error2,
                    trival.errcode );
          goto done; }
      extended.reset();
      }
    }
done:
  courier.worker_finished();
  return 0;
  }


/* Get from courier the processed and sorted packets.
   Print the member lines on stdout and the diagnostics and errors on stderr.
*/
int muxer( const char * const archive_namep, Packet_courier & courier )
  {
  std::vector< const Packet * > opacket_vector;
  int retval = 0;
  while( retval == 0 )			// exit loop at first error packet
    {
    courier.deliver_packets( opacket_vector );
    if( opacket_vector.empty() ) break;	// queue is empty. all workers exited

    for( unsigned i = 0; i < opacket_vector.size(); ++i )
      {
      const Packet * const opacket = opacket_vector[i];
      switch( opacket->status )
        {
        case Packet::error1:
        case Packet::error2:
          show_file_error( archive_namep, opacket->line.c_str(), opacket->errcode );
          retval = ( opacket->status == Packet::error1 ) ? 1 : 2; break;
        case Packet::prefix: show_error( opacket->line.c_str() ); break;
        case Packet::diag: std::fputs( opacket->line.c_str(), stderr ); break;
        default: if( opacket->line.size() )
          { std::fputs( opacket->line.c_str(), stdout ); std::fflush( stdout ); }
        }
      delete opacket;
      }
    }
  if( retval == 0 && !courier.eoa_found() )	// no worker found EOA blocks
    { show_file_error( archive_namep, end_msg ); retval = 2; }
  return retval;
  }

} // end namespace


// init the courier, then start the workers and call the muxer.
int decode_lz( const Cl_options & cl_opts, const Archive_descriptor & ad,
               std::vector< char > & name_pending )
  {
  const int out_slots = 65536;		// max small files (<=512B) in 64 MiB
  const int num_workers =		// limited to number of members
    std::min( (long)cl_opts.num_workers, ad.lzip_index.members() );
  if( cl_opts.program_mode == m_extract ) get_umask();	// cache the umask
  Name_monitor
    name_monitor( ( cl_opts.program_mode == m_extract ) ? num_workers : 0 );

  Packet_courier courier( num_workers, out_slots );

  Worker_arg * worker_args = new( std::nothrow ) Worker_arg[num_workers];
  pthread_t * worker_threads = new( std::nothrow ) pthread_t[num_workers];
  if( !worker_args || !worker_threads ) { show_error( mem_msg ); return 1; }
  for( int i = 0; i < num_workers; ++i )
    {
    worker_args[i].cl_opts = &cl_opts;
    worker_args[i].ad = &ad;
    worker_args[i].courier = &courier;
    worker_args[i].name_monitor = &name_monitor;
    worker_args[i].name_pending = &name_pending;
    worker_args[i].worker_id = i;
    worker_args[i].num_workers = num_workers;
    const int errcode =
      pthread_create( &worker_threads[i], 0, dworker, &worker_args[i] );
    if( errcode )
      { show_error( "Can't create worker threads", errcode ); exit_fail_mt(); }
    }

  int retval = muxer( ad.namep, courier );

  for( int i = num_workers - 1; i >= 0; --i )
    {
    const int errcode = pthread_join( worker_threads[i], 0 );
    if( errcode )
      { show_error( "Can't join worker threads", errcode ); exit_fail_mt(); }
    }
  delete[] worker_threads;
  delete[] worker_args;

  if( close( ad.infd ) != 0 )
    { show_file_error( ad.namep, eclosa_msg, errno ); set_retval( retval, 1 ); }

  if( retval == 0 )
    for( int i = 0; i < cl_opts.parser.arguments(); ++i )
      if( nonempty_arg( cl_opts.parser, i ) && name_pending[i] )
        { show_file_error( cl_opts.parser.argument( i ).c_str(), nfound_msg );
          retval = 1; }

  if( cl_opts.debug_level & 1 )
    std::fprintf( stderr,
      "muxer tried to consume from workers       %8u times\n"
      "muxer had to wait                         %8u times\n",
      courier.ocheck_counter,
      courier.owait_counter );

  if( !courier.finished() ) internal_error( conofin_msg );
  return final_exit_status( retval, cl_opts.program_mode != m_diff );
  }
