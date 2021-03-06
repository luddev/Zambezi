#ifndef SEGMENT_POOL_H_GUARD
#define SEGMENT_POOL_H_GUARD

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "pfordelta/opt_p4.h"
#include "bloom/BloomFilter.h"

// Pool size
#define MAX_INT_VALUE ((unsigned int) 0xFFFFFFFF)
// Null pointers to determine the end of a postings list
#define UNDEFINED_POINTER -1l
#define UNKNOWN_SEGMENT -1

// Operators defined based on whether or not the postings are backwards
#define LESS_THAN(X,Y,R) (R == 0 ? (X < Y) : (X > Y))
#define LESS_THAN_EQUAL(X,Y,R) (R == 0 ? (X <= Y) : (X >= Y))
#define GREATER_THAN(X,Y,R) (R == 0 ? (X > Y) : (X < Y))
#define GREATER_THAN_EQUAL(X,Y,R) (R == 0 ? (X >= Y) : (X <= Y))

// Functions to encode/decode segment and offset values
#define DECODE_SEGMENT(P) ((int) (P >> 32))
#define DECODE_OFFSET(P) ((unsigned int) (P & 0xFFFFFFFF))
#define ENCODE_POINTER(S, O) ((((unsigned long) S)<<32) | (unsigned int) O)

typedef struct SegmentPool SegmentPool;

struct SegmentPool {
  unsigned int numberOfPools;
  unsigned int segment;
  unsigned int offset;
  // Whether or not postings are stored backwards
  unsigned int reverse;
  // Segment pool
  int** pool;

  // if Bloom filters enabled
  int bloomEnabled;
  unsigned int nbHash;
  unsigned int bitsPerElement;
};

void writeSegmentPool(SegmentPool* pool, FILE* fp) {
  fwrite(&pool->segment, sizeof(unsigned int), 1, fp);
  fwrite(&pool->offset, sizeof(unsigned int), 1, fp);
  fwrite(&pool->reverse, sizeof(unsigned int), 1, fp);
  fwrite(&pool->bloomEnabled, sizeof(int), 1, fp);
  fwrite(&pool->nbHash, sizeof(unsigned int), 1, fp);
  fwrite(&pool->bitsPerElement, sizeof(unsigned int), 1, fp);

  int i;
  for(i = 0; i < pool->segment; i++) {
    fwrite(pool->pool[i], sizeof(int), MAX_INT_VALUE, fp);
  }
  fwrite(pool->pool[pool->segment], sizeof(int), pool->offset, fp);
}

SegmentPool* readSegmentPool(FILE* fp) {
  SegmentPool* pool = (SegmentPool*) malloc(sizeof(SegmentPool));
  fread(&pool->segment, sizeof(unsigned int), 1, fp);
  fread(&pool->offset, sizeof(unsigned int), 1, fp);
  fread(&pool->reverse, sizeof(unsigned int), 1, fp);
  fread(&pool->bloomEnabled, sizeof(int), 1, fp);
  fread(&pool->nbHash, sizeof(unsigned int), 1, fp);
  fread(&pool->bitsPerElement, sizeof(unsigned int), 1, fp);

  pool->pool = (int**) malloc((pool->segment + 1) * sizeof(int*));
  int i;
  for(i = 0; i < pool->segment; i++) {
    pool->pool[i] = (int*) calloc(MAX_INT_VALUE, sizeof(int));
    fread(pool->pool[i], sizeof(int), MAX_INT_VALUE, fp);
  }
  pool->pool[pool->segment] = (int*) calloc(MAX_INT_VALUE, sizeof(int));
  fread(pool->pool[pool->segment], sizeof(int), pool->offset, fp);
  return pool;
}

/**
 * Read whether or not postings are stored backwards in the segment pool
 */
int readReverseFlag(FILE* fp) {
  int r;
  fseek(fp, 0, SEEK_SET);
  fread(&r, sizeof(unsigned int), 1, fp); //segment
  fread(&r, sizeof(unsigned int), 1, fp); //offset
  fread(&r, sizeof(unsigned int), 1, fp); //reverse
  return r;
}

/**
 * Create a new segment pool.
 *
 * @param numberOfPools Number of pools, where each pool is an array of integers
 * @param reverse Whether to store postings in reverse order (e.g., to index tweets)
 * @param bloomEnabled Whether or not to use Bloom filter chains (to be used with BWAND)
 * @param nbHash If Bloom filter chains are enabled,
 *        this indicates the number of hash functions
 * @param bitsPerElement If Bloom filter chains are enabled,
 *        this indicates number of bits per element
 * @return A new segment pool kept in the main memory
 */
SegmentPool* createSegmentPool(int numberOfPools, int reverse, int bloomEnabled,
                                 int nbHash, int bitsPerElement) {
  SegmentPool* pool = (SegmentPool*) malloc(sizeof(SegmentPool));
  pool->pool = (int**) malloc(numberOfPools * sizeof(int*));
  int i;
  for(i = 0; i < numberOfPools; i++) {
    pool->pool[i] = (int*) calloc(MAX_INT_VALUE, sizeof(int));
  }
  pool->segment = 0;
  pool->offset = 0;
  pool->numberOfPools = numberOfPools;
  pool->bloomEnabled = bloomEnabled;
  pool->nbHash = nbHash;
  pool->bitsPerElement = bitsPerElement;
  pool->reverse = reverse;
  return pool;
}

void destroySegmentPool(SegmentPool* pool) {
  int i;
  for(i = 0; i < pool->numberOfPools; i++) {
    free(pool->pool[i]);
  }
  free(pool->pool);
  free(pool);
}

/**
 * Whether or not the index contains term frequency (tf) information
 */
int isTermFrequencyPresent(SegmentPool* pool) {
  int reqspace = pool->pool[0][0];
  int csize = pool->pool[0][4];
  if(csize + 5 == reqspace) {
    return 0;
  }
  return 1;
}

/**
 * Whether or not the index is a positional inverted index.
 */
int isPositional(SegmentPool* pool) {
  int reqspace = pool->pool[0][0];
  int csize = pool->pool[0][4];
  if(csize + 5 == reqspace) {
    return 0;
  }
  int tfcsize = pool->pool[0][csize + 5];
  if(csize + tfcsize + 6 == reqspace) {
    return 0;
  }
  return 1;
}

/**
 * Compress and write a segment into a non-positional segment pool,
 * and link it to the previous segment (if present)
 *
 * @param pool Segment pool
 * @param data Document ids
 * @param len Number of document ids
 * @param tailPointer Pointer to the previous segment
 * @return Pointer to the new segment
 */
long compressAndAddNonPositional(SegmentPool* pool, unsigned int* data,
                                 unsigned int len, long tailPointer) {
  int lastSegment = -1;
  unsigned int lastOffset = 0;
  if(tailPointer != UNDEFINED_POINTER) {
    lastSegment = DECODE_SEGMENT(tailPointer);
    lastOffset = DECODE_OFFSET(tailPointer);
  }

  // Construct a Bloom filter if required
  unsigned int* filter = 0;
  unsigned int filterSize = 0;
  if(pool->bloomEnabled) {
    filterSize = computeBloomFilterLength(len, pool->bitsPerElement);
    filter = (unsigned int*) calloc(filterSize, sizeof(unsigned int));
    int i;
    for(i = 0; i < len; i++) {
      insertIntoBloomFilter(filter, filterSize, pool->nbHash, data[i]);
    }
  }

  unsigned int maxDocId = pool->reverse ? data[0] : data[len - 1];
  unsigned int* block = (unsigned int*) calloc(BLOCK_SIZE*2, sizeof(unsigned int));
  if(pool->reverse) {
    int i, t, m = len/2;
    for(i = 0; i < m; i++) {
      t = data[i];
      data[i] = data[len - i - 1];
      data[len - i - 1] = t;
    }
  }
  unsigned int csize = OPT4(data, len, block, 1);

  int reqspace = csize + filterSize + 8;
  if(reqspace > (MAX_INT_VALUE - pool->offset)) {
    pool->segment++;
    pool->offset = 0;
  }

  pool->pool[pool->segment][pool->offset] = reqspace;
  pool->pool[pool->segment][pool->offset + 1] = UNKNOWN_SEGMENT;
  pool->pool[pool->segment][pool->offset + 2] = 0;
  pool->pool[pool->segment][pool->offset + 3] = maxDocId;
  // where Bloom filters are stored, if stored at all
  pool->pool[pool->segment][pool->offset + 4] = csize + 7;
  pool->pool[pool->segment][pool->offset + 5] = len;
  pool->pool[pool->segment][pool->offset + 6] = csize;

  memcpy(&pool->pool[pool->segment][pool->offset + 7],
         block, csize * sizeof(int));

  if(filter) {
    pool->pool[pool->segment][pool->offset + csize + 7] = filterSize;
    memcpy(&pool->pool[pool->segment][pool->offset + csize + 8],
           filter, filterSize * sizeof(int));
  }

  if(lastSegment >= 0) {
    if(!pool->reverse) {
      pool->pool[lastSegment][lastOffset + 1] = pool->segment;
      pool->pool[lastSegment][lastOffset + 2] = pool->offset;
    } else {
      pool->pool[pool->segment][pool->offset + 1] = lastSegment;
      pool->pool[pool->segment][pool->offset + 2] = lastOffset;
    }
  }

  long newPointer = ENCODE_POINTER(pool->segment, pool->offset);
  pool->offset += reqspace;

  free(block);
  if(filter) free(filter);
  return newPointer;
}

/**
 * Compress and write a segment into a non-positional segment pool with term frequencies,
 * and link it to the previous segment (if present)
 *
 * @param pool Segment pool
 * @param data Document ids
 * @param tf Term frequencies
 * @param len Number of document ids
 * @param tailPointer Pointer to the previous segment
 * @return Pointer to the new segment
 */

long compressAndAddTfOnly(SegmentPool* pool, unsigned int* data,
                          unsigned int* tf, unsigned int len, long tailPointer) {
  int lastSegment = -1;
  unsigned int lastOffset = 0;
  if(tailPointer != UNDEFINED_POINTER) {
    lastSegment = DECODE_SEGMENT(tailPointer);
    lastOffset = DECODE_OFFSET(tailPointer);
  }

  // Construct a Bloom filter if required
  unsigned int* filter = 0;
  unsigned int filterSize = 0;
  if(pool->bloomEnabled) {
    filterSize = computeBloomFilterLength(len, pool->bitsPerElement);
    filter = (unsigned int*) calloc(filterSize, sizeof(unsigned int));
    int i;
    for(i = 0; i < len; i++) {
      insertIntoBloomFilter(filter, filterSize, pool->nbHash, data[i]);
    }
  }

  unsigned int maxDocId = pool->reverse ? data[0] : data[len - 1];

  if(pool->reverse) {
    int i, t, m = len/2;
    for(i = 0; i < m; i++) {
      t = data[i];
      data[i] = data[len - i - 1];
      data[len - i - 1] = t;

      t = tf[i];
      tf[i] = tf[len - i - 1];
      tf[len - i - 1] = t;
    }
  }

  unsigned int* block = (unsigned int*) calloc(BLOCK_SIZE*2, sizeof(unsigned int));
  unsigned int* tfblock = (unsigned int*) calloc(BLOCK_SIZE*2, sizeof(unsigned int));
  unsigned int csize = OPT4(data, len, block, 1);
  unsigned int tfcsize = OPT4(tf, len, tfblock, 0);

  int reqspace = csize + tfcsize + filterSize + 9;
  if(reqspace > (MAX_INT_VALUE - pool->offset)) {
    pool->segment++;
    pool->offset = 0;
  }

  pool->pool[pool->segment][pool->offset] = reqspace;
  pool->pool[pool->segment][pool->offset + 1] = UNKNOWN_SEGMENT;
  pool->pool[pool->segment][pool->offset + 2] = 0;
  pool->pool[pool->segment][pool->offset + 3] = maxDocId;
  pool->pool[pool->segment][pool->offset + 4] = csize + tfcsize + 8;
  pool->pool[pool->segment][pool->offset + 5] = len;
  pool->pool[pool->segment][pool->offset + 6] = csize;

  memcpy(&pool->pool[pool->segment][pool->offset + 7],
         block, csize * sizeof(int));

  pool->pool[pool->segment][pool->offset + 7 + csize] = tfcsize;
  memcpy(&pool->pool[pool->segment][pool->offset + 8 + csize],
         tfblock, tfcsize * sizeof(int));

  if(filter) {
    pool->pool[pool->segment][pool->offset + csize + tfcsize + 8] = filterSize;
    memcpy(&pool->pool[pool->segment][pool->offset + 9 + csize + tfcsize],
           filter, filterSize * sizeof(int));
  }

  if(lastSegment >= 0) {
    if(!pool->reverse) {
      pool->pool[lastSegment][lastOffset + 1] = pool->segment;
      pool->pool[lastSegment][lastOffset + 2] = pool->offset;
    } else {
      pool->pool[pool->segment][pool->offset + 1] = lastSegment;
      pool->pool[pool->segment][pool->offset + 2] = lastOffset;
    }
  }

  long newPointer = ENCODE_POINTER(pool->segment, pool->offset);
  pool->offset += reqspace;

  free(block);
  free(tfblock);
  if(filter) free(filter);

  return newPointer;
}

/**
 * Compress and write a segment into a positional segment pool,
 * and link it to the previous segment (if present)
 *
 * @param pool Segment pool
 * @param data Document ids
 * @param tf Term frequencies
 * @param positions List of gap-encoded term positions
 * @param len Number of document ids
 * @param plen Number of positions
 * @param tailPointer Pointer to the previous segment
 * @return Pointer to the new segment
 */

long compressAndAddPositional(SegmentPool* pool, unsigned int* data,
    unsigned int* tf, unsigned int* positions,
    unsigned int len, unsigned int plen, long tailPointer) {
  int lastSegment = -1;
  unsigned int lastOffset = 0;
  if(tailPointer != UNDEFINED_POINTER) {
    lastSegment = DECODE_SEGMENT(tailPointer);
    lastOffset = DECODE_OFFSET(tailPointer);
  }

  // Construct a Bloom filter if required
  unsigned int* filter = 0;
  unsigned int filterSize = 0;
  if(pool->bloomEnabled) {
    filterSize = computeBloomFilterLength(len, pool->bitsPerElement);
    filter = (unsigned int*) calloc(filterSize, sizeof(unsigned int));
    int i;
    for(i = 0; i < len; i++) {
      insertIntoBloomFilter(filter, filterSize, pool->nbHash, data[i]);
    }
  }

  unsigned int maxDocId = pool->reverse ? data[0] : data[len - 1];

  if(pool->reverse) {
    int i, t, m = len/2;

    unsigned int* rpositions = (unsigned int*) calloc(plen, sizeof(unsigned int*));
    int curPos = plen, rpos = 0;
    for(i = len - 1; i >= 0; i--) {
      for(t = curPos - tf[i]; t < curPos; t++) {
        rpositions[rpos++] = positions[t];
      }
      curPos -= tf[i];
    }
    positions = rpositions;

    for(i = 0; i < m; i++) {
      t = data[i];
      data[i] = data[len - i - 1];
      data[len - i - 1] = t;

      t = tf[i];
      tf[i] = tf[len - i - 1];
      tf[len - i - 1] = t;
    }
  }

  int pblocksize = 3 * ((plen / BLOCK_SIZE) + 1) * BLOCK_SIZE;
  unsigned int* block = (unsigned int*) calloc(BLOCK_SIZE*2, sizeof(unsigned int));
  unsigned int* tfblock = (unsigned int*) calloc(BLOCK_SIZE*2, sizeof(unsigned int));
  unsigned int* pblock = (unsigned int*) calloc(pblocksize, sizeof(unsigned int));
  unsigned int csize = OPT4(data, len, block, 1);
  unsigned int tfcsize = OPT4(tf, len, tfblock, 0);

  // compressing positions
  unsigned int pcsize = 0;
  int nb = plen / BLOCK_SIZE;
  int res = plen % BLOCK_SIZE;
  int i = 0;

  for(i = 0; i < nb; i++) {
    int tempPcsize = OPT4(&positions[i * BLOCK_SIZE], BLOCK_SIZE, &pblock[pcsize+1], 0);
    pblock[pcsize] = tempPcsize;
    pcsize += tempPcsize + 1;
  }

  if(res > 0) {
    unsigned int* a = (unsigned int*) calloc(BLOCK_SIZE, sizeof(unsigned int));
    memcpy(a, &positions[nb * BLOCK_SIZE], res * sizeof(unsigned int));
    int tempPcsize = OPT4(a, res, &pblock[pcsize+1], 0);
    pblock[pcsize] = tempPcsize;
    pcsize += tempPcsize + 1;
    i++;
    free(a);
  }
  // end compressing positions

  int reqspace = csize + tfcsize + pcsize + filterSize + 11;
  if(reqspace > (MAX_INT_VALUE - pool->offset)) {
    pool->segment++;
    pool->offset = 0;
  }

  pool->pool[pool->segment][pool->offset] = reqspace;
  pool->pool[pool->segment][pool->offset + 1] = UNKNOWN_SEGMENT;
  pool->pool[pool->segment][pool->offset + 2] = 0;
  pool->pool[pool->segment][pool->offset + 3] = maxDocId;
  pool->pool[pool->segment][pool->offset + 4] = csize + tfcsize + pcsize + 10;
  pool->pool[pool->segment][pool->offset + 5] = len;
  pool->pool[pool->segment][pool->offset + 6] = csize;

  memcpy(&pool->pool[pool->segment][pool->offset + 7],
         block, csize * sizeof(int));

  pool->pool[pool->segment][pool->offset + 7 + csize] = tfcsize;
  memcpy(&pool->pool[pool->segment][pool->offset + 8 + csize],
         tfblock, tfcsize * sizeof(int));

  pool->pool[pool->segment][pool->offset + 8 + csize + tfcsize] = plen;
  pool->pool[pool->segment][pool->offset + 9 + csize + tfcsize] = i;
  memcpy(&pool->pool[pool->segment][pool->offset + 10 + csize + tfcsize],
         pblock, pcsize * sizeof(int));

  if(filter) {
    pool->pool[pool->segment][pool->offset + csize + tfcsize + pcsize + 10] = filterSize;
    memcpy(&pool->pool[pool->segment][pool->offset + 11 + csize + tfcsize + pcsize],
         filter, filterSize * sizeof(int));
  }

  if(lastSegment >= 0) {
    if(!pool->reverse) {
      pool->pool[lastSegment][lastOffset + 1] = pool->segment;
      pool->pool[lastSegment][lastOffset + 2] = pool->offset;
    } else {
      pool->pool[pool->segment][pool->offset + 1] = lastSegment;
      pool->pool[pool->segment][pool->offset + 2] = lastOffset;
    }
  }

  long newPointer = ENCODE_POINTER(pool->segment, pool->offset);
  pool->offset += reqspace;

  if(pool->reverse) {
    free(positions);
  }
  free(block);
  free(tfblock);
  free(pblock);
  if(filter) free(filter);

  return newPointer;
}

/**
 * Given the current pointer, this function returns
 * the next pointer. If the current pointer points to
 * the last block (i.e., there is no "next" block),
 * then this function returns UNDEFINED_POINTER.
 */
long nextPointer(SegmentPool* pool, long pointer) {
  if(pointer == UNDEFINED_POINTER) {
    return UNDEFINED_POINTER;
  }
  int pSegment = DECODE_SEGMENT(pointer);
  unsigned int pOffset = DECODE_OFFSET(pointer);

  if(pool->pool[pSegment][pOffset + 1] == UNKNOWN_SEGMENT) {
    return UNDEFINED_POINTER;
  }

  return ENCODE_POINTER(pool->pool[pSegment][pOffset + 1],
                        pool->pool[pSegment][pOffset + 2]);
}

/**
 * Decompresses the docid block from the segment pointed to by "pointer,"
 * into the "outBlock" buffer. Block size is 128.
 *
 * Note that outBlock must be at least 128 integers long.
 */
int decompressDocidBlock(SegmentPool* pool, unsigned int* outBlock, long pointer) {
  int pSegment = DECODE_SEGMENT(pointer);
  unsigned int pOffset = DECODE_OFFSET(pointer);

  unsigned int aux[BLOCK_SIZE*4];
  unsigned int* block = &pool->pool[pSegment][pOffset + 7];
  detailed_p4_decode(outBlock, block, aux, 1, pool->reverse);

  return pool->pool[pSegment][pOffset + 5];
}

int decompressTfBlock(SegmentPool* pool, unsigned int* outBlock, long pointer) {
  int pSegment = DECODE_SEGMENT(pointer);
  unsigned int pOffset = DECODE_OFFSET(pointer);

  unsigned int aux[BLOCK_SIZE*4];
  unsigned int csize = pool->pool[pSegment][pOffset + 6];
  unsigned int* block = &pool->pool[pSegment][pOffset + csize + 8];
  detailed_p4_decode(outBlock, block, aux, 0, pool->reverse);

  return pool->pool[pSegment][pOffset + 5];
}

/**
 * Retrieved the number of positions stored in the block
 * pointed to by "pointer".
 */
int numberOfPositionBlocks(SegmentPool* pool, long pointer) {
  int pSegment = DECODE_SEGMENT(pointer);
  unsigned int pOffset = DECODE_OFFSET(pointer);

  unsigned int csize = pool->pool[pSegment][pOffset + 6];
  unsigned int tfsize = pool->pool[pSegment][pOffset + 7 + csize];
  return pool->pool[pSegment][pOffset + csize + tfsize + 9];
}

/**
 * Decompressed the position block into the "outBlock."
 * Note that outBlock's length must be:
 *
 *     numberOfPositionBlocks() * BLOCK_SIZE,
 *
 * where BLOCK_SIZE is 128.
 */
int decompressPositionBlock(SegmentPool* pool, unsigned int* outBlock, long pointer) {
  int pSegment = DECODE_SEGMENT(pointer);
  unsigned int pOffset = DECODE_OFFSET(pointer);

  unsigned int aux[BLOCK_SIZE*4];
  unsigned int csize = pool->pool[pSegment][pOffset + 6];
  unsigned int tfsize = pool->pool[pSegment][pOffset + 7 + csize];
  unsigned int nb = pool->pool[pSegment][pOffset + csize + tfsize + 9];

  int i;
  unsigned int index = pOffset + csize + tfsize + 10;
  for(i = 0; i < nb; i++) {
    unsigned int sb = pool->pool[pSegment][index];
    unsigned int* block = &pool->pool[pSegment][index + 1];
    detailed_p4_decode(&outBlock[i * BLOCK_SIZE], block, aux, 0, pool->reverse);
    memset(aux, 0, BLOCK_SIZE * 4 * sizeof(unsigned int));
    index += sb + 1;
  }
  return pool->pool[pSegment][pOffset + csize + tfsize + 8];
}

void decompressPositions(SegmentPool* pool, unsigned int* tf,
                         int index, long pointer, int* out) {
  int pSegment = DECODE_SEGMENT(pointer);
  unsigned int pOffset = DECODE_OFFSET(pointer);

  unsigned int aux[BLOCK_SIZE*4];
  unsigned int csize = pool->pool[pSegment][pOffset + 6];
  unsigned int tfsize = pool->pool[pSegment][pOffset + 7 + csize];
  int nb = 0;
  int i;
  for(i = 0; i < index; i++) {
    nb += tf[i];
  }
  int lnb = nb + tf[i] - 1;
  int r = nb % BLOCK_SIZE;
  nb = nb / BLOCK_SIZE;
  lnb = lnb / BLOCK_SIZE;

  unsigned int pos = pOffset + csize + tfsize + 10;
  for(i = 0; i < nb; i++) {
    pos += pool->pool[pSegment][pos] + 1;
  }
  int cindex = 0, left = tf[index], tocopy = tf[index], rindex = r;
  for(i = nb; i <= lnb; i++) {
    if(rindex + tocopy > BLOCK_SIZE) {
      tocopy = BLOCK_SIZE - rindex;
    }
    unsigned int* block = &pool->pool[pSegment][pos + 1];
    unsigned int* temp = (unsigned int*) calloc(BLOCK_SIZE * 2, sizeof(unsigned int));
    detailed_p4_decode(temp, block, aux, 0, pool->reverse);
    memcpy(&out[cindex], &temp[rindex], tocopy * sizeof(int));
    pos += pool->pool[pSegment][pos] + 1;
    free(temp);

    cindex += tocopy;
    left -= tocopy;
    tocopy = left;
    rindex = 0;
  }
  for(i = 1; i < tf[index]; i++) {
    out[i] += out[i - 1];
  }
}

/**
 * If Bloom filter chains are present, perform a membership test
 *
 * @param pool Segment pool
 * @param docid Test document id
 * @param pointer Pointer to segment
 * @return Whether or not input docid exists in the Bloom filter chain
 */
int containsDocid(SegmentPool* pool, unsigned int docid, long* pointer) {
  if(*pointer == UNDEFINED_POINTER) {
    return 0;
  }
  int pSegment = DECODE_SEGMENT(*pointer);
  unsigned int pOffset = DECODE_OFFSET(*pointer);

  while(LESS_THAN(pool->pool[pSegment][pOffset + 3], docid, pool->reverse)) {
    int nSegment = pool->pool[pSegment][pOffset + 1];
    int nOffset = pool->pool[pSegment][pOffset + 2];
    pSegment = nSegment;
    pOffset = nOffset;
    if(pSegment == UNKNOWN_SEGMENT) {
      *pointer = UNDEFINED_POINTER;
      return 0;
    }
  }

  if(pool->pool[pSegment][pOffset + 3] == docid) {
    return 1;
  }

  unsigned int bloomOffset = pool->pool[pSegment][pOffset + 4];
  *pointer = ENCODE_POINTER(pSegment, pOffset);
  return containsBloomFilter(&pool->pool[pSegment][pOffset + bloomOffset + 1],
                             pool->pool[pSegment][pOffset + bloomOffset],
                             pool->nbHash, docid);
}

/**
 * Reads postings for a term from an index stored on hard-disk,
 * and stores it into "pool."
 *
 * @param pointer Head Pointer.
 */
long readPostingsForTerm(SegmentPool* pool, long pointer, FILE* fp) {
  int sSegment = -1, ppSegment = -1;
  unsigned int sOffset = 0, ppOffset = 0;
  int pSegment = DECODE_SEGMENT(pointer);
  unsigned int pOffset = DECODE_OFFSET(pointer);

  while(pSegment != UNKNOWN_SEGMENT) {
    long pos = ((pSegment * (unsigned long) MAX_INT_VALUE) + pOffset) * 4 + 24;

    fseek(fp, pos, SEEK_SET);
    int reqspace = 0;
    fread(&reqspace, sizeof(int), 1, fp);

    if(reqspace > (MAX_INT_VALUE - pool->offset)) {
      pool->segment++;
      pool->offset = 0;
    }

    pool->pool[pool->segment][pool->offset] = reqspace;
    fread(&pool->pool[pool->segment][pool->offset + 1], sizeof(unsigned int),
          reqspace - 1, fp);

    pSegment = pool->pool[pool->segment][pool->offset + 1];
    pOffset = (unsigned int) pool->pool[pool->segment][pool->offset + 2];

    if(ppSegment != -1) {
      pool->pool[ppSegment][ppOffset + 1] = pool->segment;
      pool->pool[ppSegment][ppOffset + 2] = pool->offset;
    }

    if(sSegment == -1) {
      sSegment = pool->segment;
      sOffset = pool->offset;
    }

    ppSegment = pool->segment;
    ppOffset = pool->offset;

    pool->offset += reqspace;
  }
  return ENCODE_POINTER(sSegment, sOffset);
}

/**
 * Read number of hash functions and number of bits per element,
 * if Bloom filter chains are present in the index.
 */
void readBloomStats(FILE* fp, int* bloomEnabled,
                    unsigned int* nbHash, unsigned int* bitsPerElement) {
  unsigned int temp;
  fseek(fp, 0, SEEK_SET);
  fread(&temp, sizeof(unsigned int), 1, fp); //segment
  fread(&temp, sizeof(unsigned int), 1, fp); //offset
  fread(&temp, sizeof(unsigned int), 1, fp); //reverse
  fread(bloomEnabled, sizeof(int), 1, fp);
  fread(nbHash, sizeof(unsigned int), 1, fp);
  fread(bitsPerElement, sizeof(unsigned int), 1, fp);
}

#endif
