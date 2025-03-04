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

#include "tarlz.h"
#include "common_mutex.h"


namespace {

int error_status = 0;

} // end namespace


void xinit_mutex( pthread_mutex_t * const mutex )
  {
  const int errcode = pthread_mutex_init( mutex, 0 );
  if( errcode )
    { show_error( "pthread_mutex_init", errcode ); exit_fail_mt(); }
  }

void xinit_cond( pthread_cond_t * const cond )
  {
  const int errcode = pthread_cond_init( cond, 0 );
  if( errcode )
    { show_error( "pthread_cond_init", errcode ); exit_fail_mt(); }
  }


void xdestroy_mutex( pthread_mutex_t * const mutex )
  {
  const int errcode = pthread_mutex_destroy( mutex );
  if( errcode )
    { show_error( "pthread_mutex_destroy", errcode ); exit_fail_mt(); }
  }

void xdestroy_cond( pthread_cond_t * const cond )
  {
  const int errcode = pthread_cond_destroy( cond );
  if( errcode )
    { show_error( "pthread_cond_destroy", errcode ); exit_fail_mt(); }
  }


void xlock( pthread_mutex_t * const mutex )
  {
  const int errcode = pthread_mutex_lock( mutex );
  if( errcode )
    { show_error( "pthread_mutex_lock", errcode ); exit_fail_mt(); }
  }


void xunlock( pthread_mutex_t * const mutex )
  {
  const int errcode = pthread_mutex_unlock( mutex );
  if( errcode )
    { show_error( "pthread_mutex_unlock", errcode ); exit_fail_mt(); }
  }


void xwait( pthread_cond_t * const cond, pthread_mutex_t * const mutex )
  {
  const int errcode = pthread_cond_wait( cond, mutex );
  if( errcode )
    { show_error( "pthread_cond_wait", errcode ); exit_fail_mt(); }
  }


void xsignal( pthread_cond_t * const cond )
  {
  const int errcode = pthread_cond_signal( cond );
  if( errcode )
    { show_error( "pthread_cond_signal", errcode ); exit_fail_mt(); }
  }


void xbroadcast( pthread_cond_t * const cond )
  {
  const int errcode = pthread_cond_broadcast( cond );
  if( errcode )
    { show_error( "pthread_cond_broadcast", errcode ); exit_fail_mt(); }
  }


/* This can be called from any thread, main thread or sub-threads alike,
   since they all call common helper functions that call exit_fail_mt()
   in case of an error.
*/
void exit_fail_mt( const int retval )
  {
  // calling 'exit' more than once results in undefined behavior
  static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

  pthread_mutex_lock( &mutex );		// ignore errors to avoid loop
  std::exit( retval );
  }


/* If msgp is null, print the message, else return the message in *msgp.
   If prefix is already in the list, print nothing or return empty *msgp.
   Return true if a message is printed or returned in *msgp. */
bool print_removed_prefix( const std::string & prefix,
                           std::string * const msgp )
  {
  // prevent two threads from modifying the list of prefixes at the same time
  static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  static std::vector< std::string > prefixes;	// list of prefixes

  if( verbosity < 0 || prefix.empty() )
    { if( msgp ) msgp->clear(); return false; }
  xlock( &mutex );
  for( unsigned i = 0; i < prefixes.size(); ++i )
    if( prefixes[i] == prefix )
      { xunlock( &mutex ); if( msgp ) msgp->clear(); return false; }
  prefixes.push_back( prefix );
  xunlock( &mutex );
  std::string msg( "Removing leading '" ); msg += prefix;
  msg += "' from member names.";	// from archive or command line
  if( msgp ) *msgp = msg; else show_error( msg.c_str() );
  return true;
  }


void set_error_status( const int retval )
  {
  // prevent two threads from modifying the error_status at the same time
  static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

  xlock( &mutex );
  if( error_status < retval ) error_status = retval;
  xunlock( &mutex );
  }


int final_exit_status( int retval, const bool show_msg )
  {
  if( retval == 0 && error_status )
    { if( show_msg )
        show_error( "Exiting with failure status due to previous errors." );
      retval = error_status; }
  return retval;
  }
