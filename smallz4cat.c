// //////////////////////////////////////////////////////////
// smallz4cat.c
// Copyright (c) 2016 Stephan Brumme. All rights reserved.
// see http://create.stephan-brumme.com/disclaimer.html
//

// This program is a shorter, more readable, albeit slower re-implementation of lz4cat ( https://github.com/Cyan4973/xxHash )

// Limitations:
// - skippable frames and legacy frames are not implemented (and most likely never will)
// - checksums are not verified (see http://create.stephan-brumme.com/xxhash/ for a simple implementation)

// Replace getByteFromIn() and sendToOut() by your own code if you need in-memory LZ4 decompression.
// Corrupted data causes a call to error().

#define _CRT_SECURE_NO_WARNINGS
#include <stdint.h> // uint32_t
#include <stdio.h>  // stdin/stdout/stderr, fopen, ...
#include <stdlib.h> // exit()
#include <string.h> // memcpy


/// error handler
void error(const char* msg)
{
  fprintf(stderr, "ERROR: %s\n", msg);
  exit(1);
}


// ==================== I/O INTERFACE ====================


// read one byte from input, see getByteFromIn() for a basic implementation
typedef unsigned char (*GET_BYTE)  ();
// write several bytes,      see sendToOut()     for a basic implementation
typedef void          (*SEND_BYTES)(const unsigned char*, unsigned int);

/// input stream,  usually stdin
static FILE* in = NULL;
/// read a single byte (with simple buffering)
static unsigned char getByteFromIn()
{
  // modify buffer size as you like ... for most use cases, bigger buffer aren't faster anymore - and even reducing to 1 byte works !
#define READ_BUFFER_SIZE 4*1024
  static unsigned char readBuffer[READ_BUFFER_SIZE];
  static unsigned int  pos       = 0;
  static unsigned int  available = 0;

  // refill buffer
  if (pos == available)
  {
    pos = 0;
    available = fread(readBuffer, 1, READ_BUFFER_SIZE, in);
    if (available == 0)
      error("out of data");
  }

  // return a byte
  return readBuffer[pos++];
}

/// output stream, usually stdout
static FILE* out = NULL;
/// write a block of bytes
static void sendToOut(const unsigned char* data, unsigned int numBytes)
{
  if (data != NULL && numBytes > 0)
    fwrite(data, 1, numBytes, out);
}


// ==================== LZ4 DECOMPRESSOR ====================


/// decompress everything in input stream (accessed via getByte) and write to output stream (via sendBytes)
void unlz4(GET_BYTE getByte, SEND_BYTES sendBytes)
{
  // signature
  unsigned char signature1 = getByte();
  unsigned char signature2 = getByte();
  unsigned char signature3 = getByte();
  unsigned char signature4 = getByte();
  uint32_t signature = (signature4 << 24) | (signature3 << 16) | (signature2 << 8) | signature1;
  if (signature != 0x184D2204) // legacy format: 0x184C2102
    error("invalid signature");

  // flags (version is ignored)
  unsigned char flags = getByte();
  unsigned char hasBlockChecksum   = flags & 16;
  unsigned char hasContentSize     = flags &  8;
  unsigned char hasContentChecksum = flags &  4;

  // ignore blocksize
  getByte();

  if (hasContentSize)
  {
    // ignore, skip 8 bytes
    getByte(); getByte(); getByte(); getByte();
    getByte(); getByte(); getByte(); getByte();
  }

  // ignore header checksum
  getByte();

  // don't lower this value, backreferences can be 64kb far away
#define HISTORY_SIZE 64*1024
  // contains the latest decoded data
  unsigned char history[HISTORY_SIZE];
  // next free position in history[]
  unsigned int  pos = 0;

  // parse all blocks until blockSize == 0
  while (1)
  {
    // block size
    uint32_t blockSize = getByte();
    blockSize |= (uint32_t)getByte() <<  8;
    blockSize |= (uint32_t)getByte() << 16;
    blockSize |= (uint32_t)getByte() << 24;

    // highest bit set ?
    uint32_t isCompressed = (blockSize & 0x80000000) == 0;
    blockSize &= 0x7FFFFFFF;

    // stop after last block
    if (blockSize == 0)
      break; // only way to escape the endless while(1) loop

    if (isCompressed)
    {
      // decompress block
      uint32_t blockOffset = 0;
      while (blockOffset < blockSize)
      {
        // get a token
        unsigned char token = getByte();
        blockOffset++;

        // determine number of literals
        uint32_t numLiterals = (token >> 4) & 0x0F;
        if (numLiterals == 15)
        {
          // number of literals length encoded in more than 1 byte
          unsigned char current;
          do
          {
            current = getByte();
            numLiterals += current;
            blockOffset++;
          } while (current == 255);
        }

        blockOffset += numLiterals;
        // copy all those literals
        while (numLiterals-- > 0)
        {
          history[pos++] = getByte();

          // flush output buffer
          if (pos == HISTORY_SIZE)
          {
            sendBytes(history, HISTORY_SIZE);
            pos = 0;
          }
        }

        // last token has only literals
        if (blockOffset == blockSize)
          break;

        // overflow
        if (blockOffset >  blockSize - 12)
          error("match not allowed close to the end of a block");

        // match distance is encoded by two bytes (little endian)
        blockOffset += 2;
        uint32_t delta = getByte();
        delta |= (uint32_t)getByte() << 8;
        // zero isn't allowed
        if (delta == 0)
          error("invalid offset");

        // match length (must be >= 4, therefore length is stored minus 4)
        uint32_t matchLength = 4 + (token & 0x0F);
        if (matchLength == 4 + 0x0F)
        {
          unsigned char current;
          do // match length encoded in more than 1 byte
          {
            current = getByte();
            matchLength += current;
            blockOffset++;
          } while (current == 255);
        }

        // copy match
        uint32_t reference = (pos >= delta) ? pos - delta : HISTORY_SIZE + pos - delta;
        if (pos + matchLength < HISTORY_SIZE && reference + matchLength < HISTORY_SIZE)
        {
          // fast copy
          if (pos >= reference + matchLength || reference >= pos + matchLength)
          {
            // non-overlapping
            memcpy(history + pos, history + reference, matchLength);
            pos += matchLength;
          }
          else
          {
            // overlapping
            while (matchLength-- > 0)
              history[pos++] = history[reference++];
          }
        }
        else
        {
          // slower copy, have to take care of buffer limits
          while (matchLength-- > 0)
          {
            // copy single byte
            history[pos++] = history[reference++];

            // cannot write anymore ? => wrap around
            if (pos == HISTORY_SIZE)
            {
              // flush output buffer
              sendBytes(history, HISTORY_SIZE);
              pos = 0;
            }
            // cannot read anymore ? => wrap around
            if (reference == HISTORY_SIZE)
              reference = 0;
          }
        }
      }
    }
    else
    {
      // copy uncompressd data and add to history, too (if next block is compressed and some matches refer to this block)
      while (blockSize-- > 0)
      {
        // copy a byte ...
        history[pos++] = getByte();
        // ... until buffer is full => send to output
        if (pos == HISTORY_SIZE)
        {
          sendBytes(history, HISTORY_SIZE);
          pos = 0;
        }
      }
    }

    if (hasBlockChecksum)
    {
      // ignore checksum, skip 4 bytes
      getByte(); getByte(); getByte(); getByte();
    }
  }

  if (hasContentChecksum)
  {
    // ignore checksum, skip 4 bytes
    getByte(); getByte(); getByte(); getByte();
  }

  // flush output buffer
  sendBytes(history, pos);
}


// ==================== COMMAND-LINE HANDLING ====================


/// parse command-line
int main(int argc, const char* argv[])
{
  // default input/output streams
  in = stdin; out = stdout;

  // file is given as first parameter or stdin if no parameter is given (or "-")
  if (argc == 2 && argv[1][0] != '-')
  {
    in = fopen(argv[1], "rb");
    if (!in)
      error("file not found");
  }

  // and go !
  unlz4(getByteFromIn, sendToOut);
  return 0;
}
