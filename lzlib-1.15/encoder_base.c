/* Lzlib - Compression library for the lzip format
   Copyright (C) 2009-2025 Antonio Diaz Diaz.

   This library is free software. Redistribution and use in source and
   binary forms, with or without modification, are permitted provided
   that the following conditions are met:

   1. Redistributions of source code must retain the above copyright
   notice, this list of conditions, and the following disclaimer.

   2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions, and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
*/

static bool Mb_normalize_pos( Matchfinder_base * const mb )
  {
  if( mb->pos > mb->stream_pos )
    { mb->pos = mb->stream_pos; return false; }
  if( !mb->at_stream_end )
    {
    int i;
    /* offset is int32_t for the min below */
    const int32_t offset = mb->pos - mb->before_size - mb->dictionary_size;
    const int size = mb->stream_pos - offset;
    memmove( mb->buffer, mb->buffer + offset, size );
    mb->partial_data_pos += offset;
    mb->pos -= offset;		/* pos = before_size + dictionary_size */
    mb->stream_pos -= offset;
    for( i = 0; i < mb->num_prev_positions; ++i )
      mb->prev_positions[i] -= min( mb->prev_positions[i], offset );
    for( i = 0; i < mb->pos_array_size; ++i )
      mb->pos_array[i] -= min( mb->pos_array[i], offset );
    }
  return true;
  }


static bool Mb_init( Matchfinder_base * const mb, const int before_size,
                     const int dict_size, const int after_size,
                     const int dict_factor, const int num_prev_positions23,
                     const int pos_array_factor )
  {
  const int buffer_size_limit =
    ( dict_factor * dict_size ) + before_size + after_size;
  int i;

  mb->partial_data_pos = 0;
  mb->before_size = before_size;
  mb->after_size = after_size;
  mb->pos = 0;
  mb->cyclic_pos = 0;
  mb->stream_pos = 0;
  mb->num_prev_positions23 = num_prev_positions23;
  mb->at_stream_end = false;
  mb->sync_flush_pending = false;

  mb->buffer_size = max( 65536, buffer_size_limit );
  mb->buffer = (uint8_t *)malloc( mb->buffer_size );
  if( !mb->buffer ) return false;
  mb->saved_dictionary_size = dict_size;
  mb->dictionary_size = dict_size;
  mb->pos_limit = mb->buffer_size - after_size;
  unsigned size = 1 << max( 16, real_bits( mb->dictionary_size - 1 ) - 2 );
  if( mb->dictionary_size > 1 << 26 ) size >>= 1;	/* 64 MiB */
  mb->key4_mask = size - 1;		/* increases with dictionary size */
  size += num_prev_positions23;
  mb->num_prev_positions = size;

  mb->pos_array_size = pos_array_factor * ( mb->dictionary_size + 1 );
  size += mb->pos_array_size;
  if( size * sizeof mb->prev_positions[0] <= size ) mb->prev_positions = 0;
  else mb->prev_positions =
    (int32_t *)malloc( size * sizeof mb->prev_positions[0] );
  if( !mb->prev_positions ) { free( mb->buffer ); return false; }
  mb->pos_array = mb->prev_positions + mb->num_prev_positions;
  for( i = 0; i < mb->num_prev_positions; ++i ) mb->prev_positions[i] = 0;
  return true;
  }


static void Mb_adjust_array( Matchfinder_base * const mb )
  {
  int size = 1 << max( 16, real_bits( mb->dictionary_size - 1 ) - 2 );
  if( mb->dictionary_size > 1 << 26 ) size >>= 1;	/* 64 MiB */
  mb->key4_mask = size - 1;
  size += mb->num_prev_positions23;
  mb->num_prev_positions = size;
  mb->pos_array = mb->prev_positions + mb->num_prev_positions;
  }


static void Mb_adjust_dictionary_size( Matchfinder_base * const mb )
  {
  if( mb->stream_pos < mb->dictionary_size )
    {
    mb->dictionary_size = max( min_dictionary_size, mb->stream_pos );
    Mb_adjust_array( mb );
    mb->pos_limit = mb->buffer_size;
    }
  }


static void Mb_reset( Matchfinder_base * const mb )
  {
  int i;
  if( mb->stream_pos > mb->pos )
    memmove( mb->buffer, mb->buffer + mb->pos, mb->stream_pos - mb->pos );
  mb->partial_data_pos = 0;
  mb->stream_pos -= mb->pos;
  mb->pos = 0;
  mb->cyclic_pos = 0;
  mb->at_stream_end = false;
  mb->sync_flush_pending = false;
  mb->dictionary_size = mb->saved_dictionary_size;
  Mb_adjust_array( mb );
  mb->pos_limit = mb->buffer_size - mb->after_size;
  for( i = 0; i < mb->num_prev_positions; ++i ) mb->prev_positions[i] = 0;
  }


/* End Of Stream marker => (dis == 0xFFFFFFFFU, len == min_match_len) */
static void LZeb_try_full_flush( LZ_encoder_base * const eb )
  {
  if( eb->member_finished || Cb_free_bytes( &eb->renc.cb ) <
      max_marker_size + eb->renc.ff_count + Lt_size ) return;
  eb->member_finished = true;
  const int pos_state = Mb_data_position( &eb->mb ) & pos_state_mask;
  const State state = eb->state;
  Re_encode_bit( &eb->renc, &eb->bm_match[state][pos_state], 1 );
  Re_encode_bit( &eb->renc, &eb->bm_rep[state], 0 );
  LZeb_encode_pair( eb, 0xFFFFFFFFU, min_match_len, pos_state );
  Re_flush( &eb->renc );
  Lzip_trailer trailer;
  Lt_set_data_crc( trailer, LZeb_crc( eb ) );
  Lt_set_data_size( trailer, Mb_data_position( &eb->mb ) );
  Lt_set_member_size( trailer, Re_member_position( &eb->renc ) + Lt_size );
  int i; for( i = 0; i < Lt_size; ++i ) Cb_put_byte( &eb->renc.cb, trailer[i] );
  }


/* Sync Flush marker => (dis == 0xFFFFFFFFU, len == min_match_len + 1) */
static void LZeb_try_sync_flush( LZ_encoder_base * const eb )
  {
  const unsigned min_size = eb->renc.ff_count + max_marker_size;
  if( eb->member_finished ||
      Cb_free_bytes( &eb->renc.cb ) < min_size + max_marker_size ) return;
  eb->mb.sync_flush_pending = false;
  const unsigned long long old_mpos = Re_member_position( &eb->renc );
  const int pos_state = Mb_data_position( &eb->mb ) & pos_state_mask;
  const State state = eb->state;
  do {		/* size of markers must be >= rd_min_available_bytes + 5 */
    Re_encode_bit( &eb->renc, &eb->bm_match[state][pos_state], 1 );
    Re_encode_bit( &eb->renc, &eb->bm_rep[state], 0 );
    LZeb_encode_pair( eb, 0xFFFFFFFFU, min_match_len + 1, pos_state );
    Re_flush( &eb->renc );
    }
  while( Re_member_position( &eb->renc ) - old_mpos < min_size );
  }


static void LZeb_reset( LZ_encoder_base * const eb,
                        const unsigned long long member_size )
  {
  const unsigned long long min_member_size = min_dictionary_size;
  const unsigned long long max_member_size = 0x0008000000000000ULL; /* 2 PiB */
  Mb_reset( &eb->mb );
  eb->member_size_limit = min( max( min_member_size, member_size ),
                          max_member_size ) - Lt_size - max_marker_size;
  eb->crc = 0xFFFFFFFFU;
  Bm_array_init( eb->bm_literal[0], (1 << literal_context_bits) * 0x300 );
  Bm_array_init( eb->bm_match[0], states * pos_states );
  Bm_array_init( eb->bm_rep, states );
  Bm_array_init( eb->bm_rep0, states );
  Bm_array_init( eb->bm_rep1, states );
  Bm_array_init( eb->bm_rep2, states );
  Bm_array_init( eb->bm_len[0], states * pos_states );
  Bm_array_init( eb->bm_dis_slot[0], len_states * (1 << dis_slot_bits) );
  Bm_array_init( eb->bm_dis, modeled_distances - end_dis_model + 1 );
  Bm_array_init( eb->bm_align, dis_align_size );
  Lm_init( &eb->match_len_model );
  Lm_init( &eb->rep_len_model );
  Re_reset( &eb->renc, eb->mb.dictionary_size );
  int i; for( i = 0; i < num_rep_distances; ++i ) eb->reps[i] = 0;
  eb->state = 0;
  eb->member_finished = false;
  }
