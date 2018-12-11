/* Copyright (C) 2007 MySQL AB, Sergei Golubchik & Michael Widenius
   Copyright (c) 2014, 2017, 2018 Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */

/*
  implements Universal Unique Identifiers (UUIDs), as in
    DCE 1.1: Remote Procedure Call,
    Open Group Technical Standard Document Number C706, October 1997,
    (supersedes C309 DCE: Remote Procedure Call 8/1994,
    which was basis for ISO/IEC 11578:1996 specification)

  A UUID has the following structure:

  Field                     NDR Data Type  Octet #          Note
 time_low                   unsigned long    0-3     The low field of the
                                                     timestamp.
 time_mid                   unsigned short   4-5     The middle field of
                                                     the timestamp.
 time_hi_and_version        unsigned short   6-7     The high field of the
                                                     timestamp multiplexed
                                                     with the version number.
 clock_seq_hi_and_reserved  unsigned small   8       The high field of the
                                                     clock sequence multi-
                                                     plexed with the variant.
 clock_seq_low              unsigned small   9       The low field of the
                                                     clock sequence.
 node                       character        10-15   The spatially unique node
                                                     identifier.
*/

#include "mysys_priv.h"
#include <my_rnd.h>
#include <m_string.h>
#include <myisampack.h> /* mi_int2store, mi_int4store */

static my_bool my_uuid_inited= 0;
static struct my_rnd_struct uuid_rand;
static uint nanoseq;
static ulonglong uuid_time= 0;
static longlong interval_timer_offset;
static uchar uuid_suffix[2+6]; /* clock_seq and node */

static mysql_mutex_t LOCK_uuid_generator;

/*
  Number of 100-nanosecond intervals between
  1582-10-15 00:00:00.00 and 1970-01-01 00:00:00.00
*/

#define UUID_TIME_OFFSET ((ulonglong) 141427 * 24 * 60 * 60 * \
                          1000 * 1000 * 10)
#define UUID_VERSION      0x1000
#define UUID_VARIANT      0x8000


/* Helper function */

static void set_clock_seq()
{
  uint16 clock_seq= ((uint)(my_rnd(&uuid_rand)*16383)) | UUID_VARIANT;
  mi_int2store(uuid_suffix, clock_seq);
  interval_timer_offset= (my_hrtime().val * 10 - my_interval_timer()/100 + 
                          UUID_TIME_OFFSET);
}


/**
  Init structures needed for my_uuid

  @func my_uuid_init()
  @param seed1		Seed for random generator
  @param seed2		Seed for random generator

  @note
    Seed1 & seed2 should NOT depend on clock. This is to be able to
    generate a random mac address according to UUID specs.
*/

void my_uuid_init(ulong seed1, ulong seed2)
{
  uchar *mac= uuid_suffix+2;
  ulonglong now;

  if (my_uuid_inited)
    return;
  my_uuid_inited= 1;
  now= my_interval_timer()/100 + interval_timer_offset;
  nanoseq= 0;

  if (my_gethwaddr(mac))
  {
    uint i;
    /*
      Generating random "hardware addr"

      Specs explicitly specify that node identifier should NOT
      correlate with a clock_seq value, so we use a separate
      randominit() here.
    */
    /* purecov: begin inspected */
    my_rnd_init(&uuid_rand, (ulong) (seed2+ now/2), (ulong) (now+rand()));
    for (i=0; i < array_elements(uuid_suffix) -2 ; i++)
      mac[i]= (uchar)(my_rnd(&uuid_rand)*255);
    /* purecov: end */
  }
  my_rnd_init(&uuid_rand, (ulong) (seed1 + now), (ulong) (now/2+ getpid()));
  set_clock_seq();
  mysql_mutex_init(key_LOCK_uuid_generator, &LOCK_uuid_generator, MY_MUTEX_INIT_FAST);
}


/**
   Create a global unique identifier (uuid)

   @func  my_uuid()
   @param to   Store uuid here. Must be of size MY_UUID_SIZE (16)
*/

void my_uuid(uchar *to)
{
  ulonglong tv;
  uint32 time_low;
  uint16 time_mid, time_hi_and_version;

  DBUG_ASSERT(my_uuid_inited);

  mysql_mutex_lock(&LOCK_uuid_generator);
  tv= my_interval_timer()/100 + interval_timer_offset + nanoseq;

  if (likely(tv > uuid_time))
  {
    /*
      Current time is ahead of last timestamp, as it should be.
      If we "borrowed time", give it back, just as long as we
      stay ahead of the previous timestamp.
    */
    if (nanoseq)
    {
      ulong delta;
      DBUG_ASSERT((tv > uuid_time) && (nanoseq > 0));
      /*
        -1 so we won't make tv= uuid_time for nanoseq >= (tv - uuid_time)
      */
      delta= MY_MIN(nanoseq, (ulong)(tv - uuid_time -1));
      tv-= delta;
      nanoseq-= delta;
    }
  }
  else
  {
    if (unlikely(tv == uuid_time))
    {
      /*
        For low-res system clocks. If several requests for UUIDs
        end up on the same tick, we add a nano-second to make them
        different.
        ( current_timestamp + nanoseq * calls_in_this_period )
        may end up > next_timestamp; this is OK. Nonetheless, we'll
        try to unwind nanoseq when we get a chance to.
        If nanoseq overflows, we'll start over with a new numberspace
        (so the if() below is needed so we can avoid the ++tv and thus
        match the follow-up if() if nanoseq overflows!).
      */
      if (likely(++nanoseq))
        ++tv;
    }

    if (unlikely(tv <= uuid_time))
    {
      /*
        If the admin changes the system clock (or due to Daylight
        Saving Time), the system clock may be turned *back* so we
        go through a period once more for which we already gave out
        UUIDs.  To avoid duplicate UUIDs despite potentially identical
        times, we make a new random component.
        We also come here if the nanoseq "borrowing" overflows.
        In either case, we throw away any nanoseq borrowing since it's
        irrelevant in the new numberspace.
      */
      set_clock_seq();
      tv= my_interval_timer()/100 + interval_timer_offset;
      nanoseq= 0;
      DBUG_PRINT("uuid",("making new numberspace"));
    }
  }

  uuid_time=tv;
  mysql_mutex_unlock(&LOCK_uuid_generator);

  time_low=            (uint32) (tv & 0xFFFFFFFF);
  time_mid=            (uint16) ((tv >> 32) & 0xFFFF);
  time_hi_and_version= (uint16) ((tv >> 48) | UUID_VERSION);

  /*
    Note, that the standard does NOT specify byte ordering in
    multi-byte fields. it's implementation defined (but must be
    the same for all fields).
    We use big-endian, so we can use memcmp() to compare UUIDs
    and for straightforward UUID to string conversion.
  */
  mi_int4store(to, time_low);
  mi_int2store(to+4, time_mid);
  mi_int2store(to+6, time_hi_and_version);
  bmove(to+8, uuid_suffix, sizeof(uuid_suffix));
}


/**
   Convert uuid to string representation

   @func  my_uuid2str()
   @param guid uuid
   @param s    Output buffer.Must be at least MY_UUID_STRING_LENGTH+1 large.
*/
void my_uuid2str(const uchar *guid, char *s)
{
  int i;
  for (i=0; i < MY_UUID_SIZE; i++)
  {
    *s++= _dig_vec_lower[guid[i] >>4];
    *s++= _dig_vec_lower[guid[i] & 15];
    /* Set '-' at intervals 3, 5, 7 and 9 */
    if ((1 << i) & ((1 << 3) | (1 << 5) | (1 << 7) | (1 << 9)))
      *s++= '-';
  }
}

void my_uuid_end()
{
  if (my_uuid_inited)
  {
    my_uuid_inited= 0;
    mysql_mutex_destroy(&LOCK_uuid_generator);
  }
}

static const int my_uuid_bytes_per_section[] = {4, 2, 2, 2, 6};

#define NUMBER_OF_SECTIONS (sizeof(my_uuid_bytes_per_section)/sizeof(my_uuid_bytes_per_section[0]))

static const int my_uuid_hex_to_byte[] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0,  1,  2,  3,  4,  5,  6,  7,  8,
    9,  -1, -1, -1, -1, -1, -1, -1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1,
};

static my_bool my_uuid_read_section(int section_len, const char **section_string,
                                    unsigned char ** out_binary_str)
{
  int j;
  for (j = 0; j < section_len; j++) {
    int hi = my_uuid_hex_to_byte[**section_string];
    if (hi == -1) return TRUE;
    (*section_string)++;
    int lo = my_uuid_hex_to_byte[**section_string];
    if (lo == -1) return TRUE;
    (*section_string)++;
    if (out_binary_str) {
      unsigned char *u = *out_binary_str;
      *u = ((hi << 4) + lo);
      (*out_binary_str)++;
    }
  }
  return FALSE;
}

/**
   Convert string to uuid binary representation

   @func  my_uuid_parse()
   @param in_string  input string
   @param len        length of input string
   @param out_str    Output buffer. Must be at least MY_UUID_SIZE large.
   @retval 0 if successful
*/
int my_uuid_parse(const char *in_string, size_t len,
                  unsigned char *out_str)
{
  int i;
  unsigned char **p_out_str = out_str ? &out_str : NULL;

  switch (len) {
    // UUID without dashes. ex 12345678123456781234567812345678
    case MY_UUID_STRING_LENGTH - 4:
      if (my_uuid_read_section((MY_UUID_STRING_LENGTH - 4) / 2, &in_string, p_out_str)) return 1;
      break;
    // UUID with braces ex {12345678-1234-5678-1234-567812345678}
    case MY_UUID_STRING_LENGTH + 2:
      if (*in_string != '{' || in_string[MY_UUID_STRING_LENGTH + 1] != '}') return 1;
      in_string++;
      // intentionally fall through
    // standard UUID ex 12345678-1234-5678-1234-567812345678
    case MY_UUID_STRING_LENGTH:
      for (i = 0; i < NUMBER_OF_SECTIONS - 1; i++) {
        if (my_uuid_read_section(my_uuid_bytes_per_section[i], &in_string, p_out_str)) return 1;
        if (*in_string == '-')
          in_string++;
        else
          return 1;
      }
      if (my_uuid_read_section(my_uuid_bytes_per_section[NUMBER_OF_SECTIONS - 1], &in_string,
                       p_out_str))
        return 1;
      break;
    default:
      return 1;
  }
  return 0;
}

my_bool my_uuid_is_valid(const char *s, size_t len)
{
  return my_uuid_parse(s, len, NULL) == 0;
}
