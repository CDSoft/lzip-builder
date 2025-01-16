/* Plzip - Massively parallel implementation of lzip
   Copyright (C) 2009 Laszlo Ersek.
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
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <queue>
#include <string>
#include <vector>
#include <stdint.h>
#include <unistd.h>
#include <lzlib.h>

#include "lzip.h"

/* When a problem is detected by any thread:
   - the thread sets shared_retval to 1 or 2.
   - the splitter sets eof and returns.
   - the courier discards new packets received or collected.
   - the workers drain the queue and return.
   - the muxer drains the queue and returns.
     (Draining seems to be faster than cleaning up later). */

namespace {

enum { max_packet_size = 1 << 20 };
unsigned long long in_size = 0;
unsigned long long out_size = 0;


struct Packet			// data block
  {
  uint8_t * data;		// data may be null if size == 0
  int size;			// number of bytes in data (if any)
  bool eom;			// end of member
  Packet() : data( 0 ), size( 0 ), eom( false ) {}
  Packet( uint8_t * const d, const int s, const bool e )
    : data( d ), size( s ), eom ( e ) {}
  void delete_data() { if( data ) { delete[] data; data = 0; } }
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
  std::vector< std::queue< Packet > > ipacket_queues;
  std::vector< std::queue< Packet > > opacket_queues;
  int num_working;			// number of workers still running
  const int num_workers;		// number of workers
  const unsigned out_slots;		// max output packets per queue
  pthread_mutex_t imutex;
  pthread_cond_t iav_or_eof;	// input packet available or splitter done
  pthread_mutex_t omutex;
  pthread_cond_t oav_or_exit;	// output packet available or all workers exited
  std::vector< pthread_cond_t > slot_av;	// output slot available
  const Shared_retval & shared_retval;		// discard new packets on error
  bool eof;					// splitter done
  bool trailing_data_found_;			// a worker found trailing data

  Packet_courier( const Packet_courier & );	// declared as private
  void operator=( const Packet_courier & );	// declared as private

public:
  Packet_courier( const Shared_retval & sh_ret, const int workers,
                  const int in_slots, const int oslots )
    : icheck_counter( 0 ), iwait_counter( 0 ),
      ocheck_counter( 0 ), owait_counter( 0 ),
      receive_id( 0 ), deliver_id( 0 ), slot_tally( in_slots ),
      ipacket_queues( workers ), opacket_queues( workers ),
      num_working( workers ), num_workers( workers ),
      out_slots( oslots ), slot_av( workers ), shared_retval( sh_ret ),
      eof( false ), trailing_data_found_( false )
    {
    xinit_mutex( &imutex ); xinit_cond( &iav_or_eof );
    xinit_mutex( &omutex ); xinit_cond( &oav_or_exit );
    for( unsigned i = 0; i < slot_av.size(); ++i ) xinit_cond( &slot_av[i] );
    }

  ~Packet_courier()
    {
    if( shared_retval() )		// cleanup to avoid memory leaks
      for( int i = 0; i < num_workers; ++i )
        {
        while( !ipacket_queues[i].empty() )
          { ipacket_queues[i].front().delete_data(); ipacket_queues[i].pop(); }
        while( !opacket_queues[i].empty() )
          { opacket_queues[i].front().delete_data(); opacket_queues[i].pop(); }
        }
    for( unsigned i = 0; i < slot_av.size(); ++i ) xdestroy_cond( &slot_av[i] );
    xdestroy_cond( &oav_or_exit ); xdestroy_mutex( &omutex );
    xdestroy_cond( &iav_or_eof ); xdestroy_mutex( &imutex );
    }

  /* Make a packet with data received from splitter.
     If eom == true (end of member), move to next queue. */
  void receive_packet( uint8_t * const data, const int size, const bool eom )
    {
    if( shared_retval() ) { delete[] data; return; } // discard packet on error
    const Packet ipacket( data, size, eom );
    slot_tally.get_slot();			// wait for a free slot
    xlock( &imutex );
    ipacket_queues[receive_id].push( ipacket );
    xbroadcast( &iav_or_eof );
    xunlock( &imutex );
    if( eom && ++receive_id >= num_workers ) receive_id = 0;
    }

  // distribute a packet to a worker
  Packet distribute_packet( const int worker_id )
    {
    xlock( &imutex );
    ++icheck_counter;
    while( ipacket_queues[worker_id].empty() && !eof )
      {
      ++iwait_counter;
      xwait( &iav_or_eof, &imutex );
      }
    if( !ipacket_queues[worker_id].empty() )
      {
      const Packet ipacket = ipacket_queues[worker_id].front();
      ipacket_queues[worker_id].pop();
      xunlock( &imutex ); slot_tally.leave_slot(); return ipacket;
      }
    xunlock( &imutex );		// no more packets
    xlock( &omutex );		// notify muxer when last worker exits
    if( --num_working == 0 ) xsignal( &oav_or_exit );
    xunlock( &omutex );
    return Packet();
    }

  // make a packet with data received from a worker, discard data on error
  void collect_packet( const int worker_id, uint8_t * const data,
                       const int size, const bool eom )
    {
    Packet opacket( data, size, eom );
    xlock( &omutex );
    if( data )
      while( opacket_queues[worker_id].size() >= out_slots )
        {
        if( shared_retval() ) { delete[] data; goto out; }
        xwait( &slot_av[worker_id], &omutex );
        }
    opacket_queues[worker_id].push( opacket );
    if( worker_id == deliver_id ) xsignal( &oav_or_exit );
out: xunlock( &omutex );
    }

  /* deliver packets to muxer
     if opacket.eom, move to next queue
     if opacket.data == 0, skip opacket */
  void deliver_packets( std::vector< Packet > & packet_vector )
    {
    packet_vector.clear();
    xlock( &omutex );
    ++ocheck_counter;
    do {
      while( opacket_queues[deliver_id].empty() && num_working > 0 )
        { ++owait_counter; xwait( &oav_or_exit, &omutex ); }
      while( !opacket_queues[deliver_id].empty() )
        {
        Packet opacket = opacket_queues[deliver_id].front();
        opacket_queues[deliver_id].pop();
        if( opacket_queues[deliver_id].size() + 1 == out_slots )
          xsignal( &slot_av[deliver_id] );
        if( opacket.eom && ++deliver_id >= num_workers ) deliver_id = 0;
        if( opacket.data ) packet_vector.push_back( opacket );
        }
      }
    while( packet_vector.empty() && num_working > 0 );
    xunlock( &omutex );
    }

  void add_sizes( const unsigned long long partial_in_size,
                  const unsigned long long partial_out_size )
    {
    xlock( &imutex );
    in_size += partial_in_size;
    out_size += partial_out_size;
    xunlock( &imutex );
    }

  void set_trailing_flag() { trailing_data_found_ = true; }
  bool trailing_data_found() { return trailing_data_found_; }

  void finish( const int workers_started )
    {
    xlock( &imutex );		// splitter has no more packets to send
    eof = true;
    xbroadcast( &iav_or_eof );
    xunlock( &imutex );
    xlock( &omutex );		// notify muxer if all workers have exited
    num_working -= num_workers - workers_started;	// workers spared
    if( num_working <= 0 ) xsignal( &oav_or_exit );
    xunlock( &omutex );
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


struct Worker_arg
  {
  Packet_courier * courier;
  const Pretty_print * pp;
  Shared_retval * shared_retval;
  int worker_id;
  bool ignore_trailing;
  bool loose_trailing;
  bool testing;
  bool nocopy;		// avoid copying decompressed data when testing
  void assign( Packet_courier & co, const Pretty_print & pp_,
               Shared_retval & sr, const bool it, const bool lt,
               const bool t, const bool nc )
    { courier = &co; pp = &pp_; shared_retval = &sr; worker_id = 0;
      ignore_trailing = it; loose_trailing = lt; testing = t; nocopy = nc; }
  };

struct Splitter_arg
  {
  Worker_arg worker_arg;
  Worker_arg * const worker_args;
  pthread_t * const worker_threads;
  const unsigned long long cfile_size;
  const int infd;
  unsigned dictionary_size;	// returned by splitter to main thread
  int num_workers;		// returned by splitter to main thread
  Splitter_arg( Packet_courier & co, const Pretty_print & pp_,
                Shared_retval & sr, const bool it, const bool lt,
                const bool t, const bool nc, Worker_arg * wa, pthread_t * wt,
                const unsigned long long cfs, const int ifd, const int nw )
    : worker_args( wa ), worker_threads( wt ), cfile_size( cfs ),
      infd( ifd ), dictionary_size( 0 ), num_workers( nw )
    { worker_arg.assign( co, pp_, sr, it, lt, t, nc ); }
  };


/* Consume packets from courier, decompress their contents and, if not
   testing, give to courier the packets produced.
*/
extern "C" void * dworker_s( void * arg )
  {
  const Worker_arg & tmp = *(const Worker_arg *)arg;
  Packet_courier & courier = *tmp.courier;
  const Pretty_print & pp = *tmp.pp;
  Shared_retval & shared_retval = *tmp.shared_retval;
  const int worker_id = tmp.worker_id;
  const bool ignore_trailing = tmp.ignore_trailing;
  const bool loose_trailing = tmp.loose_trailing;
  const bool testing = tmp.testing;
  const bool nocopy = tmp.nocopy;

  unsigned long long partial_in_size = 0, partial_out_size = 0;
  int new_pos = 0;
  bool draining = false;	// either trailing data or an error were found
  uint8_t * new_data = 0;
  LZ_Decoder * const decoder = LZ_decompress_open();
  if( !decoder || LZ_decompress_errno( decoder ) != LZ_ok )
    { draining = true; if( shared_retval.set_value( 1 ) ) pp( mem_msg ); }

  while( true )
    {
    Packet ipacket = courier.distribute_packet( worker_id );
    if( !ipacket.data ) break;		// no more packets to process

    int written = 0;
    while( !draining )		// else discard trailing data or drain queue
      {
      if( LZ_decompress_write_size( decoder ) > 0 && written < ipacket.size )
        {
        const int wr = LZ_decompress_write( decoder, ipacket.data + written,
                                            ipacket.size - written );
        if( wr < 0 ) internal_error( "library error (LZ_decompress_write)." );
        written += wr;
        if( written > ipacket.size )
          internal_error( "ipacket size exceeded in worker." );
        }
      if( ipacket.eom && written == ipacket.size )
        LZ_decompress_finish( decoder );
      unsigned long long total_in = 0;	// detect empty member + corrupt header
      while( !draining )		// read and pack decompressed data
        {
        if( !nocopy && !new_data &&
            !( new_data = new( std::nothrow ) uint8_t[max_packet_size] ) )
          { draining = true; if( shared_retval.set_value( 1 ) ) pp( mem_msg );
            break; }
        const int rd = LZ_decompress_read( decoder,
                                           nocopy ? 0 : new_data + new_pos,
                                           max_packet_size - new_pos );
        if( rd < 0 )			// trailing data or decoder error
          {
          draining = true;
          const enum LZ_Errno lz_errno = LZ_decompress_errno( decoder );
          if( lz_errno == LZ_header_error )
            {
            courier.set_trailing_flag();
            if( !ignore_trailing )
              { if( shared_retval.set_value( 2 ) ) pp( trailing_msg ); }
            }
          else if( lz_errno == LZ_data_error &&
                   LZ_decompress_member_position( decoder ) == 0 )
            {
            courier.set_trailing_flag();
            if( !loose_trailing )
              { if( shared_retval.set_value( 2 ) ) pp( corrupt_mm_msg ); }
            else if( !ignore_trailing )
              { if( shared_retval.set_value( 2 ) ) pp( trailing_msg ); }
            }
          else
            decompress_error( decoder, pp, shared_retval, worker_id );
          }
        else new_pos += rd;
        if( new_pos > max_packet_size )
          internal_error( "opacket size exceeded in worker." );
        if( LZ_decompress_member_finished( decoder ) == 1 )
          {
          partial_in_size += LZ_decompress_member_position( decoder );
          partial_out_size += LZ_decompress_data_position( decoder );
          }
        const bool eom = draining || LZ_decompress_finished( decoder ) == 1;
        if( new_pos == max_packet_size || eom )
          {
          if( !testing )			// make data packet
            {
            courier.collect_packet( worker_id, ( new_pos > 0 ) ? new_data : 0,
                                    new_pos, eom );
            if( new_pos > 0 ) new_data = 0;
            }
          new_pos = 0;
          if( eom )
            { LZ_decompress_reset( decoder );	// prepare for next member
              break; }
          }
        if( rd == 0 )
          {
          const unsigned long long size = LZ_decompress_total_in_size( decoder );
          if( total_in == size ) break; else total_in = size;
          }
        }
      if( !ipacket.data || written == ipacket.size ) break;
      }
    ipacket.delete_data();
    }

  if( new_data ) delete[] new_data;
  courier.add_sizes( partial_in_size, partial_out_size );
  if( LZ_decompress_member_position( decoder ) != 0 &&
      shared_retval.set_value( 1 ) )
    pp( "Error, some data remains in decoder." );
  if( LZ_decompress_close( decoder ) < 0 && shared_retval.set_value( 1 ) )
    pp( "LZ_decompress_close failed." );
  return 0;
  }


bool start_worker( const Worker_arg & worker_arg,
                   Worker_arg * const worker_args,
                   pthread_t * const worker_threads, const int worker_id,
                   Shared_retval & shared_retval )
  {
  worker_args[worker_id] = worker_arg;
  worker_args[worker_id].worker_id = worker_id;
  const int errcode = pthread_create( &worker_threads[worker_id], 0,
                                      dworker_s, &worker_args[worker_id] );
  if( errcode && shared_retval.set_value( 1 ) )
    show_error( "Can't create worker threads", errcode );
  return errcode == 0;
  }


/* Split data from input file into chunks and pass them to courier for
   packaging and distribution to workers.
   Start a worker per member up to a maximum of num_workers.
*/
extern "C" void * dsplitter( void * arg )
  {
  Splitter_arg & tmp = *(Splitter_arg *)arg;
  const Worker_arg & worker_arg = tmp.worker_arg;
  Packet_courier & courier = *worker_arg.courier;
  const Pretty_print & pp = *worker_arg.pp;
  Shared_retval & shared_retval = *worker_arg.shared_retval;
  Worker_arg * const worker_args = tmp.worker_args;
  pthread_t * const worker_threads = tmp.worker_threads;
  const int infd = tmp.infd;
  int worker_id = 0;			// number of workers started
  const int hsize = Lzip_header::size;
  const int tsize = Lzip_trailer::size;
  const int buffer_size = max_packet_size;
  // buffer with room for trailer, header, data, and sentinel "LZIP"
  const int base_buffer_size = tsize + hsize + buffer_size + 4;
  uint8_t * const base_buffer = new( std::nothrow ) uint8_t[base_buffer_size];
  if( !base_buffer )
    {
mem_fail:
    if( shared_retval.set_value( 1 ) ) pp( mem_msg );
fail:
    delete[] base_buffer;
    courier.finish( worker_id );	// no more packets to send
    tmp.num_workers = worker_id;
    return 0;
    }
  uint8_t * const buffer = base_buffer + tsize;

  int size = readblock( infd, buffer, buffer_size + hsize ) - hsize;
  bool at_stream_end = ( size < buffer_size );
  if( size != buffer_size && errno )
    { if( shared_retval.set_value( 1 ) )
      { pp(); show_error( "Read error", errno ); } goto fail; }
  if( size + hsize < min_member_size )
    { if( shared_retval.set_value( 2 ) ) show_file_error( pp.name(),
        ( size <= 0 ) ? "File ends unexpectedly at member header." :
        "Input file is truncated." ); goto fail; }
  const Lzip_header & header = *(const Lzip_header *)buffer;
  if( !header.check_magic() )
    { if( shared_retval.set_value( 2 ) )
      { show_file_error( pp.name(), bad_magic_msg ); } goto fail; }
  if( !header.check_version() )
    { if( shared_retval.set_value( 2 ) )
      { pp( bad_version( header.version() ) ); } goto fail; }
  tmp.dictionary_size = header.dictionary_size();
  if( !isvalid_ds( tmp.dictionary_size ) )
    { if( shared_retval.set_value( 2 ) ) { pp( bad_dict_msg ); } goto fail; }
  if( verbosity >= 1 ) pp();
  show_progress( 0, tmp.cfile_size, &pp );			// init

  unsigned long long partial_member_size = 0;
  bool worker_pending = true;	// start 1 worker per first packet of member
  while( true )
    {
    if( shared_retval() ) break;	// stop sending packets on error
    int pos = 0;			// current searching position
    std::memcpy( buffer + hsize + size, lzip_magic, 4 );	// sentinel
    for( int newpos = 1; newpos <= size; ++newpos )
      {
      while( buffer[newpos]   != lzip_magic[0] ||
             buffer[newpos+1] != lzip_magic[1] ||
             buffer[newpos+2] != lzip_magic[2] ||
             buffer[newpos+3] != lzip_magic[3] ) ++newpos;
      if( newpos <= size )
        {
        const Lzip_trailer & trailer =
          *(const Lzip_trailer *)(buffer + newpos - tsize);
        const unsigned long long member_size = trailer.member_size();
        if( partial_member_size + newpos - pos == member_size &&
            trailer.check_consistency() )
          {						// header found
          const Lzip_header & header = *(const Lzip_header *)(buffer + newpos);
          if( !header.check_version() )
            { if( shared_retval.set_value( 2 ) )
              { pp( bad_version( header.version() ) ); } goto fail; }
          const unsigned dictionary_size = header.dictionary_size();
          if( !isvalid_ds( dictionary_size ) )
            { if( shared_retval.set_value( 2 ) ) pp( bad_dict_msg );
              goto fail; }
          if( tmp.dictionary_size < dictionary_size )
            tmp.dictionary_size = dictionary_size;
          uint8_t * const data = new( std::nothrow ) uint8_t[newpos - pos];
          if( !data ) goto mem_fail;
          std::memcpy( data, buffer + pos, newpos - pos );
          courier.receive_packet( data, newpos - pos, true );	// eom
          partial_member_size = 0;
          pos = newpos;
          if( worker_pending )
            { if( !start_worker( worker_arg, worker_args, worker_threads,
                                 worker_id, shared_retval ) ) goto fail;
              ++worker_id; }
          worker_pending = worker_id < tmp.num_workers;
          show_progress( member_size );
          }
        }
      }

    if( at_stream_end )
      {
      uint8_t * data = new( std::nothrow ) uint8_t[size + hsize - pos];
      if( !data ) goto mem_fail;
      std::memcpy( data, buffer + pos, size + hsize - pos );
      courier.receive_packet( data, size + hsize - pos, true );	// eom
      if( worker_pending &&
          start_worker( worker_arg, worker_args, worker_threads,
                        worker_id, shared_retval ) ) ++worker_id;
      break;
      }
    if( pos < buffer_size )
      {
      partial_member_size += buffer_size - pos;
      uint8_t * data = new( std::nothrow ) uint8_t[buffer_size - pos];
      if( !data ) goto mem_fail;
      std::memcpy( data, buffer + pos, buffer_size - pos );
      courier.receive_packet( data, buffer_size - pos, false );
      if( worker_pending )
        { if( !start_worker( worker_arg, worker_args, worker_threads,
                             worker_id, shared_retval ) ) break;
          ++worker_id; worker_pending = false; }
      }
    if( courier.trailing_data_found() ) break;
    std::memcpy( base_buffer, base_buffer + buffer_size, tsize + hsize );
    size = readblock( infd, buffer + hsize, buffer_size );
    at_stream_end = ( size < buffer_size );
    if( size != buffer_size && errno )
      { if( shared_retval.set_value( 1 ) )
        { pp(); show_error( "Read error", errno ); } break; }
    }
  delete[] base_buffer;
  courier.finish( worker_id );		// no more packets to send
  tmp.num_workers = worker_id;
  return 0;
  }


/* Get from courier the processed and sorted packets, and write their
   contents to the output file. Drain queue on error.
*/
void muxer( Packet_courier & courier, const Pretty_print & pp,
            Shared_retval & shared_retval, const int outfd )
  {
  std::vector< Packet > packet_vector;
  while( true )
    {
    courier.deliver_packets( packet_vector );
    if( packet_vector.empty() ) break;	// queue is empty. all workers exited

    for( unsigned i = 0; i < packet_vector.size(); ++i )
      {
      Packet & opacket = packet_vector[i];
      if( shared_retval() == 0 &&
          writeblock( outfd, opacket.data, opacket.size ) != opacket.size &&
          shared_retval.set_value( 1 ) )
        { pp(); show_error( wr_err_msg, errno ); }
      opacket.delete_data();
      }
    }
  }

} // end namespace


/* Init the courier, then start the splitter and the workers and, if not
   testing, call the muxer.
*/
int dec_stream( const unsigned long long cfile_size, const int num_workers,
                const int infd, const int outfd, const Cl_options & cl_opts,
                const Pretty_print & pp, const int debug_level,
                const int in_slots, const int out_slots )
  {
  const int total_in_slots = ( INT_MAX / num_workers >= in_slots ) ?
                             num_workers * in_slots : INT_MAX;
  in_size = 0;
  out_size = 0;
  Shared_retval shared_retval;
  Packet_courier courier( shared_retval, num_workers, total_in_slots, out_slots );

  if( debug_level & 2 ) std::fputs( "decompress stream.\n", stderr );

  Worker_arg * worker_args = new( std::nothrow ) Worker_arg[num_workers];
  pthread_t * worker_threads = new( std::nothrow ) pthread_t[num_workers];
  if( !worker_args || !worker_threads )
    { pp( mem_msg ); delete[] worker_threads; delete[] worker_args; return 1; }

#if defined LZ_API_VERSION && LZ_API_VERSION >= 1012
  const bool nocopy = ( outfd < 0 && LZ_api_version() >= 1012 );
#else
  const bool nocopy = false;
#endif

  Splitter_arg splitter_arg( courier, pp, shared_retval,
    cl_opts.ignore_trailing, cl_opts.loose_trailing, outfd < 0, nocopy,
    worker_args, worker_threads, cfile_size, infd, num_workers );

  pthread_t splitter_thread;
  int errcode = pthread_create( &splitter_thread, 0, dsplitter, &splitter_arg );
  if( errcode )
    { show_error( "Can't create splitter thread", errcode );
      delete[] worker_threads; delete[] worker_args; return 1; }

  if( outfd >= 0 ) muxer( courier, pp, shared_retval, outfd );

  errcode = pthread_join( splitter_thread, 0 );
  if( errcode && shared_retval.set_value( 1 ) )
    show_error( "Can't join splitter thread", errcode );

  for( int i = splitter_arg.num_workers; --i >= 0; )
    {					// join only the workers started
    errcode = pthread_join( worker_threads[i], 0 );
    if( errcode && shared_retval.set_value( 1 ) )
      show_error( "Can't join worker threads", errcode );
    }
  delete[] worker_threads;
  delete[] worker_args;

  if( shared_retval() ) return shared_retval();	// some thread found a problem

  show_results( in_size, out_size, splitter_arg.dictionary_size, outfd < 0 );

  if( debug_level & 1 )
    {
    std::fprintf( stderr,
      "workers started                           %8u\n"
      "any worker tried to consume from splitter %8u times\n"
      "any worker had to wait                    %8u times\n",
      splitter_arg.num_workers,
      courier.icheck_counter, courier.iwait_counter );
    if( outfd >= 0 )
      std::fprintf( stderr,
        "muxer tried to consume from workers       %8u times\n"
        "muxer had to wait                         %8u times\n",
        courier.ocheck_counter, courier.owait_counter );
    }

  if( !courier.finished() ) internal_error( "courier not finished." );
  return 0;
  }
