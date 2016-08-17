// //////////////////////////////////////////////////////////
// smallz4.cpp
// Copyright (c) 2016 Stephan Brumme. All rights reserved.
// see http://create.stephan-brumme.com/disclaimer.html
//

#define _CRT_SECURE_NO_WARNINGS

#include <stdint.h> // uint16_t, uint32_t, ...
#include <stdio.h>  // stdin/stdout/stderr, fopen, ...
#include <stdlib.h> // exit()
#include <string.h> // memcpy (for buffered send() only)

#include <vector>


/// error handler
void error(const char* msg)
{
  fprintf(stderr, "ERROR: %s\n", msg);
  exit(1);
}


// ==================== I/O INTERFACE ====================


// read several bytes, see getBytesFromIn() for a basic implementation
typedef size_t (*GET_BYTES) (unsigned char* position, size_t numBytes);
// write one or more bytes to output, see sendBytesToOut() for a basic implementation
typedef void   (*SEND_BYTE) (unsigned char data);
typedef void   (*SEND_BYTES)(const unsigned char* data, size_t numBytes);

/// input stream,  usually stdin
static FILE* in = NULL;
/// read several bytes, return number of actually read bytes
static size_t getBytesFromIn(unsigned char* data, size_t numBytes)
{
  if (data != NULL && numBytes > 0)
    return fread(data, 1, numBytes, in);
  return 0;
}

/// output stream, usually stdout
static FILE* out = NULL;
/// write a block of bytes
static void sendBytesToOut(const unsigned char* data, size_t numBytes)
{
  if (data != NULL && numBytes > 0)
    fwrite(data, 1, numBytes, out);
}
/// write a single byte
static void sendByteToOut(unsigned char data)
{
  sendBytesToOut(&data, 1);
}


// ==================== LZ4 COMPRESSOR ====================


// ----- constants and types -----

static const char* Version = "0.4";
/// a block can be 4 MB
typedef uint32_t Length;
/// matches must start within the most recent 64k
typedef uint16_t Distance;

/// maximum match distance
const Distance MaxDistance = 65534;
/// marker for "no match"
const Distance NoPrevious  = MaxDistance + 1;

/// each match's length must be >= 4
const size_t   MinMatch = 4;
/// no matching within the last few bytes
const size_t   BlockEndNoMatch  = 12;
/// last bytes must be literals
const size_t   BlockEndLiterals =  5;

/// match finder's hash table size (2^HashBits entries, must be less than 32)
const uint8_t  HashBits       = 20;
/// stop match finding after MaxChainLength steps (default is unlimited => optimal parsing)
const uint32_t MaxChainLength = MaxDistance;
/// greedy mode for short chains (compression level <= 3) instead of optimal parsing / lazy evaluation
const uint32_t ShortChainsGreedy = 3;
/// lazy evaluation for medium-sized chains (compression level > 3 and <= 6)
const uint32_t ShortChainsLazy   = 6;
/// refer to location of the previous match (implicit hash chain)
const size_t   PreviousSize   = 1 << 16;

/// input buffer size, can be any number but zero ;-)
const size_t   BufferSize = 64*1024;

/// maximum block size as defined in LZ4 spec
const uint32_t MaxBlockSizeArray[] = { 0,0,0,0,64*1024,256*1024,1024*1024,4*1024*1024 };
/// I only work with the biggest maximum block size (7)
const uint32_t MaxBlockSizeId = 7; // header checksum is precalculated only for 7, too
const uint32_t MaxBlockSize = MaxBlockSizeArray[MaxBlockSizeId];

/// match
struct Match
{
  /// default is "no match", just a literal (1 byte)
  Match() : distance(NoPrevious), length(1) {}

  /// true, if long enough
  bool isMatch() const
  {
    return length >= MinMatch;
  }

  /// start of match
  Distance distance;
  /// length of match
  Length   length;
};

//  ----- globals -----

/// how many matches are checked in findLongestMatch
static uint32_t maxChainLength = MaxDistance; // => no limit, but can be changed by command-line

//  ----- code -----

/// find longest match of data[pos] between data[begin] and data[end], use match chain stored in previous
static Match findLongestMatch(const unsigned char* data, uint32_t pos, uint32_t begin, uint32_t end, const Distance* previous)
{
  Match result;

  // compression level: look only at the first n entries of the match chain
  int32_t stepsLeft = maxChainLength;

  // pointer to position that is matched against everything in data
  const unsigned char* current = data + pos - begin;

  // get distance to previous match, abort if -1 => not existing
  Distance distance = previous[pos % PreviousSize];
  uint32_t totalDistance = 0;
  while (distance != NoPrevious)
  {
    // too far back ?
    totalDistance += distance;
    if (totalDistance > MaxDistance)
      break;

    // stop searching on lower compression levels
    if (stepsLeft-- <= 0)
      break;

    // prepare next position
    distance = previous[(pos - totalDistance) % PreviousSize];

    // quick check of last 4 bytes vs. the best match so far (these two lines are just a performance optimization)
    if (result.length >= 8 && *(uint32_t*)(current - totalDistance + result.length - 4) != *(uint32_t*)(current + result.length - 4))
      continue;

    // step forward until a difference is found (or end of data is hit)
    Length length = 4; // first four bytes are a guaranteed match

    // check four bytes at once
    while (pos + length + 3 < end && *(uint32_t*)(current - totalDistance + length) == *(uint32_t*)(current + length))
      length += 4;
    // check the last 1/2/3 bytes
    while (pos + length     < end &&            *(current - totalDistance + length) ==            *(current + length))
      length++;

    // match longer than before ?
    if (length > result.length)
    {
      result.distance = totalDistance;
      result.length   = length;
    }
  }

  return result;
}


/// create shortest output
/** data points to block's begin; we need it to extract literals **/
static std::vector<unsigned char> selectBestMatches(const std::vector<Match>& matches, const unsigned char* data)
{
  // store encoded data
  std::vector<unsigned char> result;
  result.reserve(MaxBlockSize / 2);

  // indices of current literal run
  size_t literalsFrom = 0;
  size_t literalsTo   = 0; // point beyond last literal of the current run

  // walk through the whole block
  for (size_t offset = 0; offset < matches.size(); ) // increment inside of loop
  {
    // get best cost-weighted match
    Match match = matches[offset];

    // if no match, then count literals instead
    if (!match.isMatch())
    {
      // first literal
      if (literalsFrom == literalsTo)
        literalsFrom = literalsTo = offset;

      // one more literal
      literalsTo++;
      // ... and definitely no match
      match.length = 1;
    }

    offset += match.length;
    bool lastToken = (offset == matches.size());
    // continue if simple literal
    if (!match.isMatch() && !lastToken)
      continue;

    // emit token

    // count literals
    size_t numLiterals = literalsTo - literalsFrom;

    // store literals' length
    unsigned char token = (numLiterals < 15) ? numLiterals : 15;
    token <<= 4;

    // store match length (4 is implied because it's the minimum match length)
    size_t matchLength = match.length - 4;
    if (!lastToken)
      token |= (matchLength < 15) ? matchLength : 15;

    result.push_back(token);

    // >= 15 literals ? (extra bytes to store length)
    if (numLiterals >= 15)
    {
      // 15 is already encoded in token
      numLiterals -= 15;
      // emit 255 until remainder is below 255
      while (numLiterals >= 255)
      {
        result.push_back(255);
        numLiterals -= 255;
      }
      // and the last byte (can be zero, too)
      result.push_back(numLiterals);
    }
    // copy literals
    if (literalsFrom != literalsTo)
    {
      result.insert(result.end(), data + literalsFrom, data + literalsTo);
      literalsFrom = literalsTo = 0;
    }

    // last token doesn't have a match
    if (lastToken)
      break;

    // distance stored in 16 bits / little endian
    result.push_back( match.distance       & 0xFF);
    result.push_back((match.distance >> 8) & 0xFF);

    // >= 15+4 bytes matched (4 is implied because it's the minimum match length)
    if (matchLength >= 15)
    {
      // 15 is already encoded in token
      matchLength -= 15;
      // emit 255 until remainder is below 255
      while (matchLength >= 255)
      {
        result.push_back(255);
        matchLength -= 255;
      }
      // and the last byte (can be zero, too)
      result.push_back(matchLength);
    }
  }

  return result;
}


/// walk backwards through all matches and compute number of compressed bytes from current position to the end of the block
/** note: matches are modified (shortened length) if necessary **/
void estimateCosts(std::vector<Match>& matches)
{
  size_t blockEnd = matches.size();

  typedef uint32_t Cost;
  // minimum cost from this position to the end of the current block
  std::vector<Cost> cost(matches.size(), 0);
  // "cost" represents the number of bytes needed

  // backwards optimal parsing
  size_t posLastMatch = matches.size();
  for (int i = matches.size() - (1 + BlockEndLiterals); i >= 0; i--) // ignore the last 5 bytes, they are always literals
  {
    // watch out for long literal strings that need extra bytes
    Length numLiterals = posLastMatch - i;
    // assume no match
    Cost minCost = cost[i + 1] + 1;
    // an extra byte for every 255 literals required to store length (first 14 bytes are "for free")
    if (numLiterals >= 15 && (numLiterals - 15) % 255 == 0)
      minCost++;

    // if encoded as a literal
    Length bestLength = 1;

    // analyze longest match
    Match match = matches[i];

    // match must not cross block borders
    if (match.isMatch() && i + match.length + BlockEndLiterals > blockEnd)
      match.length = blockEnd - (i + BlockEndLiterals);

    // try all match lengths
    for (Length length = MinMatch; length <= match.length; length++)
    {
      // token (1 byte) + offset (2 bytes)
      Cost currentCost = cost[i + length] + 1 + 2;

      // very long matches need extra bytes for encoding match length
      if (length > 18)
        currentCost += 1 + (length - 18) / 255;

      // better choice ?
      if (currentCost <= minCost)
      {
        // regarding the if-condition:
        // "<"  prefers literals and shorter matches
        // "<=" prefers longer matches
        // they should produce the same number of bytes (because of the same cost)
        // ... but every now and then it doesn't !
        // that's why: too many consecutive literals require an extra length byte
        // (which we took into consideration a few lines above)
        // but we only looked at literals beyond the current position
        // if there are many literal in front of the current position
        // then it may be better to emit a match with the same cost as the literals at the current position
        // => it "breaks" the long chain of literals and removes the extra length byte
        minCost    = currentCost;
        bestLength = length;
        // performance-wise, a long match is usually faster during decoding than multiple short matches
        // on the other hand, literals are faster than short matches as well (assuming same cost)
      }

      // TODO: very long self-referencing matches can slow down the program A LOT
      if (match.distance == 1 && match.length > 18 + 255)
      {
        // assume that longest match is always the best match
        bestLength = match.length;
        minCost    = cost[i + match.length] + 1 + 2 + 1 + (match.length - 18) / 255;
        break;
      }
    }

    // remember position of last match to detect number of consecutive literals
    if (bestLength >= MinMatch)
      posLastMatch = i;

    // store lowest cost so far
    cost[i] = minCost;
    // and adjust best match
    matches[i].length = bestLength;
    if (bestLength == 1)
      matches[i].distance = NoPrevious;
    // note: if bestLength is smaller than the previous matches[i].length then there might be a closer match
    //       which could be more cache-friendly (=> faster decoding)
  }
}


/// compress everything in input stream (accessed via getByte) and write to output stream (via send)
void lz4(GET_BYTES getBytes, SEND_BYTES sendBytes, SEND_BYTE sendByte)
{
  // ==================== write header ====================
  // magic bytes
  sendByte(0x04); sendByte(0x22); sendByte(0x4D); sendByte(0x18);

  // flags
  sendByte(1 << 6);
  // max blocksize
  sendByte(MaxBlockSizeId << 4);
  // header checksum
  sendByte(0xDF);

  // ==================== declarations ====================
  // read the file in chunks/blocks, data will contain only bytes which are relevant for the current block
  std::vector<unsigned char> data;
  // file position corresponding to data[0]
  size_t dataZero = 0;
  // last already read position
  size_t numRead  = 0;

  // passthru data (but still wrap in LZ4 format)
  bool uncompressed = (maxChainLength == 0);

  // last time we saw a hash
  const uint32_t HashSize = 1 << HashBits;
  std::vector<size_t> lastHash(HashSize, NoPrevious);
  const uint32_t HashMultiplier = 22695477; // taken from https://en.wikipedia.org/wiki/Linear_congruential_generator
  const uint8_t  HashShift = 32 - HashBits;

  // previous position which starts with the same bytes
  std::vector<Distance> previousHash (PreviousSize, NoPrevious); // long chains based on my simple hash
  std::vector<Distance> previousExact(PreviousSize, NoPrevious); // shorter chains based on exact matching of the first four bytes

  // first and last offset of a block (next is end-of-block plus 1)
  size_t lastBlock = 0;
  size_t nextBlock = 0;
  while (true)
  {
    // ==================== start new block ====================
    // read more bytes from input
    while (numRead - nextBlock < MaxBlockSize)
    {
      // change buffer size as you like
      static unsigned char buffer[BufferSize];
      size_t incoming = getBytesFromIn(&buffer[0], BufferSize);
      if (incoming == 0)
        break;

      numRead += incoming;
      data.insert(data.end(), buffer, buffer + incoming);
    }

    // no more data ? => WE'RE DONE !
    if (nextBlock == numRead)
      break;

    // determine block borders
    lastBlock  = nextBlock;
    nextBlock += MaxBlockSize;
    // not beyond end-of-file
    if (nextBlock > numRead)
      nextBlock = numRead;

    size_t blockSize = nextBlock - lastBlock;
    // first byte of the currently processed block (std::vector data may contain the last 64k of the previous block, too)
    const unsigned char* dataBlock = &data[lastBlock - dataZero];

    // ==================== full match finder ====================

    // greedy mode is much faster but produces larger output
    bool isGreedy = (maxChainLength <= ShortChainsGreedy);
    // lazy evaluation: if there is a match, then try running match finder on next position, too, but not after that
    bool isLazy   = !isGreedy && (maxChainLength <= ShortChainsLazy);
    // skip match finding on the next x bytes in greedy mode
    size_t skipMatches = 0;
    // allow match finding on the next byte but skip afterwards (in lazy mode)
    bool   lazyEvaluation = false;

    std::vector<Match> matches(blockSize);
    // find longest matches for each position
    for (size_t i = 0; i < blockSize; i++)
    {
      // no matches at the end of the block (or matching disabled by command-line option -0 )
      if (i + BlockEndNoMatch >= blockSize || uncompressed)
        continue;

      // detect self-matching
      if (i > 0 && dataBlock[i] == dataBlock[i - 1])
      {
        Match prevMatch = matches[i - 1];
        // predecessor had the same match ?
        if (prevMatch.distance == 1 && prevMatch.length > 1024) // TODO: handle very long self-referencing matches
        {
          // just copy predecessor without further (expensive) optimizations
          prevMatch.length--;
          matches[i] = prevMatch;
          continue;
        }
      }

      // read next four bytes
      uint32_t four = *(uint32_t*)(dataBlock + i);
      // convert to a shorter hash
      uint32_t hash = (four * HashMultiplier) >> HashShift;

      // get last occurrence of these bits
      uint32_t last  = lastHash[hash];
      // and store current position
      lastHash[hash] = i + lastBlock;

      // no predecessor or too far away ?
      uint32_t distance = i + lastBlock - last;
      if (last == NoPrevious || distance > MaxDistance)
      {
        previousHash [i % PreviousSize] = NoPrevious;
        previousExact[i % PreviousSize] = NoPrevious;
        continue;
      }

      // build hash chain, i.e. store distance to last match
      previousHash[i % PreviousSize] = distance;

      // skip pseudo-matches (hash collisions) and build a second chain where the first four bytes must match exactly
      while (distance != NoPrevious)
      {
        uint32_t curFour = *(uint32_t*)(&data[last - dataZero]); // may be in the previous block, too
        // actual match found, first 4 bytes are identical
        if (curFour == four)
          break;

        // prevent from accidently hopping on an old, wrong hash chain
        uint32_t curHash = (curFour * HashMultiplier) >> HashShift;
        if (curHash != hash)
        {
          distance = NoPrevious;
          break;
        }

        // try next pseudo-match
        Distance next = previousHash[last % PreviousSize];

        // pointing to outdated hash chain entry ?
        distance += next;
        if (distance > MaxDistance)
        {
          previousHash[last % PreviousSize] = NoPrevious;
          distance = NoPrevious;
          break;
        }

        // closest match is out of range ?
        last -= next;
        if (next == NoPrevious || last < dataZero)
        {
          distance = NoPrevious;
          break;
        }
      }

      // no match at all ?
      if (distance == NoPrevious)
      {
        previousExact[i % PreviousSize] = NoPrevious;
        continue;
      }

      // store distance to previous match
      previousExact[i % PreviousSize] = distance;

      // TODO: long consecutive literals
      /*if (distance == 1 && i > 0)
      {
        if (matches[i - 1].length > MinMatch)
        {
          matches[i] = matches[i - 1];
          //continue;
        }
      }*/

      // skip match finding if in greedy mode
      if (skipMatches > 0)
      {
        skipMatches--;
        if (!lazyEvaluation)
          continue;
        lazyEvaluation = false;
      }

      // and look for longest match
      Match longest = findLongestMatch(&data[0], i + lastBlock, dataZero, nextBlock - BlockEndLiterals, &previousExact[0]);
      matches[i] = longest;

      // no match finding needed for the next few bytes in greedy/lazy mode
      if (longest.isMatch() && (isLazy || isGreedy))
      {
        lazyEvaluation = (skipMatches == 0);
        skipMatches = longest.length;
      }
    }

    // ==================== estimate costs (number of compressed bytes) ====================

    // not needed in greedy mode and/or very short blocks
    if (matches.size() > BlockEndNoMatch && maxChainLength > ShortChainsGreedy)
      estimateCosts(matches);

    // ==================== select best matches ====================

    std::vector<unsigned char> block;
    if (!uncompressed)
      block = selectBestMatches(matches, &data[lastBlock - dataZero]);

    // ==================== write to disk ====================

    // and write to disk, automatically decide whether compressed or uncompressed
    size_t uncompressedSize = nextBlock - lastBlock;
    // did compression do harm ?
    bool useCompression = block.size() < uncompressedSize && !uncompressed;

    // block size
    uint32_t numBytes = useCompression ? block.size() : uncompressedSize;
    sendByte(  numBytes         & 0xFF);
    sendByte( (numBytes >>  8)  & 0xFF);
    sendByte( (numBytes >> 16)  & 0xFF);
    sendByte(((numBytes >> 24)  & 0x7F) | (useCompression ? 0 : 0x80));

    if (useCompression)
      sendBytes(&block[0], numBytes);
    else // uncompressed ? => copy input data
      sendBytes(&data[lastBlock - dataZero], numBytes);

    // remove already processed data except for the last 64kb which could be used for intra-block matches
    if (data.size() > MaxDistance)
    {
      size_t remove = data.size() - MaxDistance;
      dataZero += remove;
      data.erase(data.begin(), data.begin() + remove);
    }
  }

  // add an empty block
  sendByte(0); sendByte(0); sendByte(0); sendByte(0);
}


// ==================== COMMAND-LINE HANDLING ====================


/// parse command-line
int main(int argc, const char* argv[])
{
  // overwrite output ?
  bool overwrite = false;

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
        printf("smalLZ4 %s: LZ4 compressor with optimal parsing, fully compatible with LZ4\n"
               "\n"
               "Basic usage:\n"
               "  %s [flags] [input] [output]\n"
               "\n"
               "This program writes to STDOUT if output isn't specified\n"
               "and reads from STDIN if input isn't specified, either.\n"
               "\n"
               "Examples:\n"
               "  %s   < abc.txt > abc.txt.lz4      # use STDIN and STDOUT\n"
               "  %s     abc.txt > abc.txt.lz4      # read from file and write to STDOUT\n"
               "  %s     abc.txt   abc.txt.lz4      # read from and write to file\n"
               "  cat abc.txt | %s - abc.txt.lz4    # read from STDIN and write to file\n"
               "  %s -6  abc.txt   abc.txt.lz4      # compression level 6 (instead of default 9)\n"
               "  %s -f  abc.txt   abc.txt.lz4      # overwrite an existing file\n"
               "  %s -f7 abc.txt   abc.txt.lz4      # compression level 7 and overwrite an existing file\n"
               "\n"
               "Flags:\n"
               "  -0, -1 ... -9     Set compression level, default: 9 (see below)\n"
               "  -h                Display this help message\n"
               "  -f                Overwrite an existing file\n"
               "\n"
               "Compression levels:\n"
               " -0                 No compression\n"
               " -1 ... -%d          Greedy search, check 1 to %d matches\n"
               " -%d ... -8          Lazy matching with optimal parsing, check %d to 8 matches\n"
               " -9                 Optimal parsing, check all possible matches\n"
               "\n"
               "(C) 2016 Stephan Brumme, http://create.stephan-brumme.com/smallz4/\n"
               , Version
               , argv[0], argv[0], argv[0], argv[0], argv[0], argv[0], argv[0], argv[0],
               ShortChainsGreedy,     ShortChainsGreedy,
               ShortChainsGreedy + 1, ShortChainsGreedy + 1);
        return 0;

      // force overwrite
      case 'f':
        overwrite = true;
        break;

      // set compression level
      case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8':
        maxChainLength = argv[nextArgument][1] - '0';
        break;

      // unlimited hash chain length
      case '9':
        maxChainLength = MaxDistance;
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
  lz4(getBytesFromIn, sendBytesToOut, sendByteToOut);
  return 0;
}
