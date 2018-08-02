// //////////////////////////////////////////////////////////
// smallz4cat.c
// Copyright (c) 2016-2018 Stephan Brumme. All rights reserved.
// see https://create.stephan-brumme.com/smallz4/
//
// "MIT License":
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the Software
// is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
// INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
// PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
// HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
// SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

// This program is a shorter, more readable, albeit slower re-implementation of lz4cat ( https://github.com/Cyan4973/xxHash )

// Limitations:
// - skippable frames and legacy frames are not implemented (and most likely never will)
// - checksums are not verified (see https://create.stephan-brumme.com/xxhash/ for a simple implementation)

// Replace getByteFromIn() and sendToOut() by your own code if you need in-memory LZ4 decompression.
// Corrupted data causes a call to error().

// suppress warnings when compiled by Visual C++
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


// read one byte from input, see getByteFromIn()  for a basic implementation
typedef unsigned char (*GET_BYTE)  ();
// write several bytes,      see sendBytesToOut() for a basic implementation
typedef void          (*SEND_BYTES)(const unsigned char*, unsigned int);

/// input stream,  usually stdin
static FILE* in = NULL;
/// read a single byte (with simple buffering)
static unsigned char getByteFromIn()
{
  // modify buffer size as you like ... for most use cases, bigger buffer aren't faster anymore - and even reducing to 1 byte works !
#define READ_BUFFER_SIZE 4*1024
  static unsigned char readBuffer[READ_BUFFER_SIZE];
  static size_t        pos       = 0;
  static size_t        available = 0;

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
static void sendBytesToOut(const unsigned char* data, unsigned int numBytes)
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
  int isModern = (signature == 0x184D2204);
  int isLegacy = (signature == 0x184C2102);
  if (!isModern && !isLegacy)
    error("invalid signature");

  unsigned char hasBlockChecksum   = 0;
  unsigned char hasContentSize     = 0;
  unsigned char hasContentChecksum = 0;
  if (isModern)
  {
    // flags (version is ignored)
    unsigned char flags = getByte();
    hasBlockChecksum   = flags & 16;
    hasContentSize     = flags &  8;
    hasContentChecksum = flags &  4;

    // dictionary compression a recently introduced feature, not implemented yet
    if (flags & 1)
      error("dictionary not supported");

    // ignore blocksize
    getByte();

    if (hasContentSize)
    {
      // ignore, skip 8 bytes
      getByte(); getByte(); getByte(); getByte();
      getByte(); getByte(); getByte(); getByte();
    }

    // ignore header checksum (xxhash32 of everything up this point & 0xFF)
    getByte();
  }

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
    unsigned char isCompressed = isLegacy || (blockSize & 0x80000000) == 0;
    if (isModern)
      blockSize &= 0x7FFFFFFF;

    // stop after last block
    if (blockSize == 0)
      break;

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

      // all legacy blocks must be completely filled - except for the last one
      if (isLegacy && blockSize < 8*1024*1024)
        break;
    }
    else
    {
      // copy uncompressed data and add to history, too (if next block is compressed and some matches refer to this block)
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

  // first command-line parameter is our input filename / but ignore "-" which stands for STDIN
  if (argc == 2 && argv[1][0] != '-' && argv[1][1] != '\0')
  {
    in = fopen(argv[1], "rb");
    if (!in)
      error("file not found");
  }

  // and go !
  unlz4(getByteFromIn, sendBytesToOut);
  return 0;
}
