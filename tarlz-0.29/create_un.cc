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

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <queue>
#include <unistd.h>
#include <sys/stat.h>
#include <ftw.h>

#include "tarlz.h"
#include "arg_parser.h"
#include "common_mutex.h"
#include "create.h"


namespace {

const Cl_options * gcl_opts = 0;	// local vars needed by add_member_un
enum { max_packet_size = 1 << 20 };	// must be a multiple of header_size
class Packet_courier;
Packet_courier * courierp = 0;


struct Ipacket			// filename and nftw flag for worker
  {
  const std::string filename;
  const int flag;

  Ipacket( const char * const name, const int flg )
    : filename( name ), flag( flg ) {}
  };

struct Opacket			// tar data to be written to the archive
  {
  const uint8_t * data;		// data == 0 means end of tar member
  int size;			// number of bytes in data (if any)

  Opacket() : data( 0 ), size( 0 ) {}
  Opacket( uint8_t * const d, const int s ) : data( d ), size( s ) {}
  };


class Packet_courier			// moves packets around
  {
public:
  unsigned icheck_counter;
  unsigned iwait_counter;
  unsigned ocheck_counter;
  unsigned owait_counter;
private:
  int receive_id;		// worker queue currently receiving packets
  int deliver_id;		// worker queue currently delivering packets
  Slot_tally slot_tally;		// limits the number of input packets
  std::vector< std::queue< const Ipacket * > > ipacket_queues;
  std::vector< std::queue< Opacket > > opacket_queues;
  int num_working;			// number of workers still running
  const int num_workers;		// number of workers
  const unsigned out_slots;		// max output packets per queue
  pthread_mutex_t imutex;
  pthread_cond_t iav_or_eof;	// input packet available or sender done
  pthread_mutex_t omutex;
  pthread_cond_t oav_or_exit;	// output packet available or all workers exited
  std::vector< pthread_cond_t > slot_av;	// output slot available
  bool eof;					// sender done

  Packet_courier( const Packet_courier & );	// declared as private
  void operator=( const Packet_courier & );	// declared as private

public:
  Packet_courier( const int workers, const int in_slots, const int oslots )
    : icheck_counter( 0 ), iwait_counter( 0 ),
      ocheck_counter( 0 ), owait_counter( 0 ),
      receive_id( 0 ), deliver_id( 0 ), slot_tally( in_slots ),
      ipacket_queues( workers ), opacket_queues( workers ),
      num_working( workers ), num_workers( workers ),
      out_slots( oslots ), slot_av( workers ), eof( false )
    {
    xinit_mutex( &imutex ); xinit_cond( &iav_or_eof );
    xinit_mutex( &omutex ); xinit_cond( &oav_or_exit );
    for( unsigned i = 0; i < slot_av.size(); ++i ) xinit_cond( &slot_av[i] );
    }

  ~Packet_courier()
    {
    for( unsigned i = 0; i < slot_av.size(); ++i ) xdestroy_cond( &slot_av[i] );
    xdestroy_cond( &oav_or_exit ); xdestroy_mutex( &omutex );
    xdestroy_cond( &iav_or_eof ); xdestroy_mutex( &imutex );
    }

  // Receive an ipacket from sender and move to next queue.
  void receive_packet( const Ipacket * const ipacket )
    {
    slot_tally.get_slot();		// wait for a free slot
    xlock( &imutex );
    ipacket_queues[receive_id].push( ipacket );
    if( ++receive_id >= num_workers ) receive_id = 0;
    xbroadcast( &iav_or_eof );
    xunlock( &imutex );
    }

  // distribute an ipacket to a worker
  const Ipacket * distribute_packet( const int worker_id )
    {
    const Ipacket * ipacket = 0;
    xlock( &imutex );
    ++icheck_counter;
    while( ipacket_queues[worker_id].empty() && !eof )
      {
      ++iwait_counter;
      xwait( &iav_or_eof, &imutex );
      }
    if( !ipacket_queues[worker_id].empty() )
      {
      ipacket = ipacket_queues[worker_id].front();
      ipacket_queues[worker_id].pop();
      }
    xunlock( &imutex );
    if( ipacket ) slot_tally.leave_slot();
    else
      {
      // notify muxer when last worker exits
      xlock( &omutex );
      if( --num_working == 0 ) xsignal( &oav_or_exit );
      xunlock( &omutex );
      }
    return ipacket;
    }

  // collect an opacket from a worker
  void collect_packet( const Opacket & opacket, const int worker_id )
    {
    xlock( &omutex );
    if( opacket.data )
      {
      while( opacket_queues[worker_id].size() >= out_slots )
        xwait( &slot_av[worker_id], &omutex );
      }
    opacket_queues[worker_id].push( opacket );
    if( worker_id == deliver_id ) xsignal( &oav_or_exit );
    xunlock( &omutex );
    }

  /* Deliver opackets to muxer.
     If opacket.data == 0, skip opacket and move to next queue. */
  void deliver_packets( std::vector< Opacket > & opacket_vector )
    {
    opacket_vector.clear();
    xlock( &omutex );
    ++ocheck_counter;
    do {
      while( opacket_queues[deliver_id].empty() && num_working > 0 )
        { ++owait_counter; xwait( &oav_or_exit, &omutex ); }
      while( !opacket_queues[deliver_id].empty() )
        {
        Opacket opacket = opacket_queues[deliver_id].front();
        opacket_queues[deliver_id].pop();
        if( opacket_queues[deliver_id].size() + 1 == out_slots )
          xsignal( &slot_av[deliver_id] );
        if( opacket.data ) opacket_vector.push_back( opacket );
        else if( ++deliver_id >= num_workers ) deliver_id = 0;
        }
      }
    while( opacket_vector.empty() && num_working > 0 );
    xunlock( &omutex );
    }

  void finish()			// sender has no more packets to send
    {
    xlock( &imutex );
    eof = true;
    xbroadcast( &iav_or_eof );
    xunlock( &imutex );
    }

  bool finished()		// all packets delivered to muxer
    {
    if( !slot_tally.all_free() || !eof || num_working != 0 ) return false;
    for( int i = 0; i < num_workers; ++i )
      if( !ipacket_queues[i].empty() ) return false;
    for( int i = 0; i < num_workers; ++i )
      if( !opacket_queues[i].empty() ) return false;
    return true;
    }
  };


// send one ipacket to courier and print filename
int add_member_un( const char * const filename, const struct stat *,
                   const int flag, struct FTW * )
  {
  if( Exclude::excluded( filename ) ) return 0;		// skip excluded files
  courierp->receive_packet( new Ipacket( filename, flag ) );
  if( verbosity >= 1 ) std::fprintf( stderr, "%s\n", filename );
  return 0;
  }


struct Sender_arg
  {
  const Cl_options * cl_opts;
  Packet_courier * courier;
  };


// Send file names to be archived to the courier for distribution to workers
extern "C" void * sender( void * arg )
  {
  const Sender_arg & tmp = *(const Sender_arg *)arg;
  const Cl_options & cl_opts = *tmp.cl_opts;
  Packet_courier & courier = *tmp.courier;

  for( int i = 0; i < cl_opts.parser.arguments(); ++i )	// parse command line
    if( parse_cl_arg( cl_opts, i, add_member_un ) == 1 ) exit_fail_mt();
  courier.finish();			// no more packets to send
  return 0;
  }


/* If isize > 0, write ibuf to opackets and send them to courier.
   Else if obuf is full, send it in an opacket to courier.
   Allocate new obuf each time obuf is full.
*/
void loop_store( const uint8_t * const ibuf, const int isize,
                 uint8_t * & obuf, int & opos, Packet_courier & courier,
                 const int worker_id, const bool finish = false )
  {
  int ipos = 0;
  if( opos < 0 || opos > max_packet_size )
    internal_error( "bad buffer index in loop_store." );
  do {
    const int sz = std::min( isize - ipos, max_packet_size - opos );
    if( sz > 0 )
      { std::memcpy( obuf + opos, ibuf + ipos, sz ); ipos += sz; opos += sz; }
    // obuf is full or last opacket in tar member
    if( opos >= max_packet_size || ( opos > 0 && finish && ipos >= isize ) )
      {
      if( opos > max_packet_size )
        internal_error( "opacket size exceeded in worker." );
      courier.collect_packet( Opacket( obuf, opos ), worker_id );
      opos = 0; obuf = new( std::nothrow ) uint8_t[max_packet_size];
      if( !obuf ) { show_error( mem_msg2 ); exit_fail_mt(); }
      }
    } while( ipos < isize );			// ibuf not empty
  if( ipos > isize ) internal_error( "ipacket size exceeded in worker." );
  if( ipos < isize ) internal_error( "input not fully consumed in worker." );
  }


struct Worker_arg
  {
  Packet_courier * courier;
  int worker_id;
  };


/* Get ipackets from courier, store headers and file data in opackets, and
   give them to courier.
*/
extern "C" void * cworker_un( void * arg )
  {
  const Worker_arg & tmp = *(const Worker_arg *)arg;
  Packet_courier & courier = *tmp.courier;
  const int worker_id = tmp.worker_id;

  uint8_t * data = 0;
  Resizable_buffer rbuf;			// extended header + data
  if( !rbuf.size() ) { show_error( mem_msg2 ); exit_fail_mt(); }

  int opos = 0;
  while( true )
    {
    const Ipacket * const ipacket = courier.distribute_packet( worker_id );
    if( !ipacket ) break;		// no more packets to process

    const char * const filename = ipacket->filename.c_str();
    const int flag = ipacket->flag;
    long long file_size;
    Extended extended;			// metadata for extended records
    Tar_header header;
    std::string estr;
    if( !fill_headers( estr, filename, extended, header, file_size, flag ) )
      { if( estr.size() ) std::fputs( estr.c_str(), stderr ); goto next; }
    print_removed_prefix( extended.removed_prefix );
    { const int infd = file_size ? open_instream( filename ) : -1;
    if( file_size && infd < 0 )			// can't read file data
      { set_error_status( 1 ); goto next; }	// skip file

    if( !data )				// init data just before using it
      {
      data = new( std::nothrow ) uint8_t[max_packet_size];
      if( !data ) { show_error( mem_msg2 ); exit_fail_mt(); }
      }

    { const int ebsize = extended.format_block( rbuf );	// may be 0
    if( ebsize < 0 )
      { show_error( extended.full_size_error() ); exit_fail_mt(); }
    if( ebsize > 0 )				// store extended block
      loop_store( rbuf.u8(), ebsize, data, opos, courier, worker_id );
    // store ustar header
    loop_store( header, header_size, data, opos, courier, worker_id ); }

    if( file_size )
      {
      long long rest = file_size;
      while( rest > 0 )
        {
        const int size = std::min( rest, (long long)(max_packet_size - opos) );
        const int rd = readblock( infd, data + opos, size );
        opos += rd; rest -= rd;
        if( rd != size )
          { show_atpos_error( filename, file_size - rest, false );
            close( infd ); exit_fail_mt(); }
        if( rest == 0 )				// last read
          {
          const int rem = file_size % header_size;
          if( rem > 0 )
            { const int padding = header_size - rem;
              std::memset( data + opos, 0, padding ); opos += padding; }
          }
        if( opos >= max_packet_size )		// store size bytes of file
          loop_store( 0, 0, data, opos, courier, worker_id );
        }
      if( close( infd ) != 0 )
        { show_file_error( filename, eclosf_msg, errno ); exit_fail_mt(); }
      }
    if( gcl_opts->warn_newer && archive_attrs.is_newer( filename ) )
      { show_file_error( filename, "File is newer than the archive." );
        set_error_status( 1 ); }
    loop_store( 0, 0, data, opos, courier, worker_id, true ); }
next:
    courier.collect_packet( Opacket(), worker_id );	// end of member token
    delete ipacket;
    }
  if( data ) delete[] data;
  return 0;
  }


/* Get from courier the processed and sorted packets, and write
   their contents to the output archive.
*/
void muxer( Packet_courier & courier, const int outfd )
  {
  std::vector< Opacket > opacket_vector;
  while( true )
    {
    courier.deliver_packets( opacket_vector );
    if( opacket_vector.empty() ) break;	// queue is empty. all workers exited

    for( unsigned i = 0; i < opacket_vector.size(); ++i )
      {
      Opacket & opacket = opacket_vector[i];
      if( !writeblock_wrapper( outfd, opacket.data, opacket.size ) )
        exit_fail_mt();
      delete[] opacket.data;
      }
    }
  }

} // end namespace


// init the courier, then start the sender and the workers and call the muxer
int encode_un( const Cl_options & cl_opts, const char * const archive_namep,
               const int outfd )
  {
  const int in_slots = cl_opts.out_slots;		// max files per queue
  const int num_workers = cl_opts.num_workers;
  const int total_in_slots = (INT_MAX / num_workers >= in_slots) ?
                             num_workers * in_slots : INT_MAX;
  gcl_opts = &cl_opts;

  /* If an error happens after any threads have been started, exit must be
     called before courier goes out of scope. */
  Packet_courier courier( num_workers, total_in_slots, cl_opts.out_slots );
  courierp = &courier;			// needed by add_member_un

  Sender_arg sender_arg;
  sender_arg.cl_opts = &cl_opts;
  sender_arg.courier = &courier;

  pthread_t sender_thread;
  int errcode = pthread_create( &sender_thread, 0, sender, &sender_arg );
  if( errcode )
    { show_error( "Can't create sender thread", errcode ); return 1; }

  Worker_arg * worker_args = new( std::nothrow ) Worker_arg[num_workers];
  pthread_t * worker_threads = new( std::nothrow ) pthread_t[num_workers];
  if( !worker_args || !worker_threads )
    { show_error( mem_msg ); exit_fail_mt(); }
  for( int i = 0; i < num_workers; ++i )
    {
    worker_args[i].courier = &courier;
    worker_args[i].worker_id = i;
    errcode = pthread_create( &worker_threads[i], 0, cworker_un, &worker_args[i] );
    if( errcode )
      { show_error( "Can't create worker threads", errcode ); exit_fail_mt(); }
    }

  muxer( courier, outfd );

  for( int i = num_workers - 1; i >= 0; --i )
    {
    errcode = pthread_join( worker_threads[i], 0 );
    if( errcode )
      { show_error( "Can't join worker threads", errcode ); exit_fail_mt(); }
    }
  delete[] worker_threads;
  delete[] worker_args;

  errcode = pthread_join( sender_thread, 0 );
  if( errcode )
    { show_error( "Can't join sender thread", errcode ); exit_fail_mt(); }

  // write End-Of-Archive records
  int retval = !write_eoa_records( outfd, false );

  if( close( outfd ) != 0 && retval == 0 )
    { show_file_error( archive_namep, eclosa_msg, errno ); retval = 1; }

  if( cl_opts.debug_level & 1 )
    std::fprintf( stderr,
      "any worker tried to consume from sender %8u times\n"
      "any worker had to wait                  %8u times\n"
      "muxer tried to consume from workers     %8u times\n"
      "muxer had to wait                       %8u times\n",
      courier.icheck_counter,
      courier.iwait_counter,
      courier.ocheck_counter,
      courier.owait_counter );

  if( !courier.finished() ) internal_error( conofin_msg );
  return final_exit_status( retval );
  }
