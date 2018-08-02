// //////////////////////////////////////////////////////////
// smallz4.cpp
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

// suppress warnings when compiled by Visual C++
#define _CRT_SECURE_NO_WARNINGS

#include "smallz4.h"

#include <cstdio>     // stdin/stdout/stderr, fopen, ...
#include <cstdlib>    // exit
#ifdef _WIN32
  #include <io.h>     // isatty()
#else
  #include <unistd.h> // isatty()
  #define _fileno fileno
  #define _isatty isatty
#endif


/// error handler
static void error(const char* msg)
{
  fprintf(stderr, "ERROR: %s\n", msg);
  exit(1);
}


// ==================== I/O INTERFACE ====================


/// input stream,  usually stdin
FILE* in = 0;
/// read several bytes and store at "data", return number of actually read bytes (return only zero if end of data reached)
size_t getBytesFromIn(void* data, size_t numBytes)
{
  if (data && numBytes > 0)
    return fread(data, 1, numBytes, in);
  return 0;
}

/// output stream, usually stdout
FILE* out = 0;
/// write a block of bytes
void sendBytesToOut(const void* data, size_t numBytes)
{
  if (data && numBytes > 0)
    fwrite(data, 1, numBytes, out);
}


// ==================== COMMAND-LINE HANDLING ====================


// show simple help
static void showHelp(const char* program)
{
  printf("smalLZ4 %s: compressor with optimal parsing, fully compatible with LZ4 by Yann Collet (see https://lz4.org)\n"
    "\n"
    "Basic usage:\n"
    "  %s [flags] [input] [output]\n"
    "\n"
    "This program writes to STDOUT if output isn't specified\n"
    "and reads from STDIN if input isn't specified, either.\n"
    "\n"
    "Examples:\n"
    "  %s   < abc.txt > abc.txt.lz4    # use STDIN and STDOUT\n"
    "  %s     abc.txt > abc.txt.lz4    # read from file and write to STDOUT\n"
    "  %s     abc.txt   abc.txt.lz4    # read from and write to file\n"
    "  cat abc.txt | %s - abc.txt.lz4  # read from STDIN and write to file\n"
    "  %s -6  abc.txt   abc.txt.lz4    # compression level 6 (instead of default 9)\n"
    "  %s -f  abc.txt   abc.txt.lz4    # overwrite an existing file\n"
    "  %s -f7 abc.txt   abc.txt.lz4    # compression level 7 and overwrite an existing file\n"
    "\n"
    "Flags:\n"
    "  -0, -1 ... -9   Set compression level, default: 9 (see below)\n"
    "  -h              Display this help message\n"
    "  -f              Overwrite an existing file\n"
    "\n"
    "Compression levels:\n"
    " -0               No compression\n"
    " -1 ... -%d        Greedy search, check 1 to %d matches\n"
    " -%d ... -8        Lazy matching with optimal parsing, check %d to 8 matches\n"
    " -9               Optimal parsing, check all possible matches\n"
    "\n"
    "Written in 2016-2018 by Stephan Brumme https://create.stephan-brumme.com/smallz4/\n"
    , smallz4::getVersion()
    , program, program, program, program, program, program, program, program,
    smallz4::ShortChainsGreedy,     smallz4::ShortChainsGreedy,
    smallz4::ShortChainsGreedy + 1, smallz4::ShortChainsGreedy + 1);
}


/// parse command-line
int main(int argc, const char* argv[])
{
  // show help if no parameters and stdin isn't a pipe
  if (argc == 1 && _isatty(_fileno(stdin)) != 0)
  {
    showHelp(argv[0]);
    return 0;
  }

  unsigned int maxChainLength = 65536; // "unlimited" because search window contains only 2^16 bytes

  // overwrite output ?
  bool overwrite = false;

  // parse flags
  int nextArgument = 1;
  while (argc > nextArgument && argv[nextArgument][0] == '-')
  {
    int argPos = 1;
    while (argv[nextArgument][argPos] != '\0')
    {
      switch (argv[nextArgument][argPos++])
      {
      // show help
      case 'h':
        showHelp(argv[0]);
        return 0;

      // force overwrite
      case 'f':
        overwrite = true;
        break;

      // set compression level
      case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8':
        maxChainLength = argv[nextArgument][1] - '0'; // "0" => 0, "1" => 1, ..., "8" => 8
        break;

      // unlimited hash chain length
      case '9':
        // default maxChainLength is already "unlimited"
        break;

      default:
        error("unknown flag");
      }
    }

    nextArgument++;
  }

  // default input/output streams
  in = stdin; out = stdout;

  // input file is given as first parameter or stdin if no parameter is given (or "-")
  if (argc > nextArgument && argv[nextArgument][0] != '-')
  {
    in = fopen(argv[nextArgument], "rb");
    if (!in)
      error("file not found");
    nextArgument++;
  }

  // output file is given as second parameter or stdout if no parameter is given (or "-")
  if (argc == nextArgument + 1 && argv[nextArgument][0] != '-')
  {
    // check if file already exists
    if (!overwrite && fopen(argv[nextArgument], "rb"))
      error("output file already exists");

    out = fopen(argv[nextArgument], "wb");
    if (!out)
      error("cannot create file");
  }

  // and go !
  smallz4::lz4(getBytesFromIn, sendBytesToOut, maxChainLength);
  return 0;
}
