#include <stdlib.h>
#include <zlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include "pfordelta/opt_p4.h"
#include "dictionary/Dictionary.h"
#include "buffer/FixedBuffer.h"
#include "buffer/BufferMaps.h"
#include "buffer/FixedIntCounter.h"
#include "buffer/FixedLongCounter.h"
#include "buffer/IntSet.h"
#include "util/ParseCommandLine.h"
#include "scorer/BM25.h"
#include "SegmentPool.h"
#include "Pointers.h"
#include "Config.h"
#include "InvertedIndex.h"

// I/O buffer length
#define LENGTH 32*1024
#define LINE_LENGTH 0x100000

// A set of auxiliary data structures for indexing
typedef struct IndexingData IndexingData;
struct IndexingData {
  // Keeps tail pointers, docid, tf, and position
  // buffer pools, etc.
  BufferMaps* buffer;

  // Keeps the index in the position buffer at which
  // the number of positions for the current block is stored
  FixedIntCounter* psum;

  // Set of unique term ids in a document
  IntSet* uniqueTerms;

  // Whether the index is positional, tf-only, or non-positional
  int positional;

  // Whether buffers can expand when they reach capacity
  int expansionEnabled;

  // If expansion is enabled, then what is the maximum
  // buffer length (in number of blocks)
  int maxBlocks;

  // Contains the raw document
  FixedBuffer* document;

  // Df Cutoff
  int dfCutoff;
};

void destroyIndexingData(IndexingData* data) {
  destroyBufferMaps(data->buffer);
  if(data->psum) {
    destroyFixedIntCounter(data->psum);
  }
  destroyIntSet(data->uniqueTerms);
  destroyFixedBuffer(data->document);
  free(data);
}

/*
 * An optimized function to read a term from a char*
 * @param t Input text
 * @param del Delimiter
 * @param consumed Number of bytes read.
 *        consumed is zero is no byte is available to read
*/
void grabword(char* t, char del, int* consumed) {
  char* s = t;
  *consumed = 0;
  while(*s != '\0' && *s != del) {
    (*consumed)++;
    s++;
  }

  (*consumed) += (*s == del);
  *s = '\0';
}

/*
 * Indexes the contents of a document. Each document
 * must be stored in a single line, in the following format:
 *
 *   <document id> \t <document content>
 *
 * @param index Inverted index data structure
 * @param data Auxiliary data structure
 * @param line Content of a document
 * @param termid Largest term id assigned so far
 * @return largest termid assigned so far
 */
int process(InvertedIndex* index, IndexingData* data, char* line, int termid) {
  int docid = 0, consumed;
  grabword(line, '\t', &consumed);
  docid = atoi(line);
  line += consumed;

  if(indexDocumentVectors(index)) {
    resetFixedBuffer(data->document);
  }

  // positions start from 1
  int position = 1;
  clearIntSet(data->uniqueTerms);
  grabword(line, ' ', &consumed);
  while(consumed > 0) {
    // Insert the term into the dictionary.
    int id = setTermId(index->dictionary, line, termid);
    // Add the term to the set of unique terms
    int added = addIntSet(&data->uniqueTerms, id);
    // If term did not exist in the dictionary (i.e., a new term),
    // then increment termid
    if(id == termid) {
      termid++;
    }

    // Update Collection Frequency
    long cf = getCf(index->pointers, id);
    index->pointers->cf->counter[id]++;

    if(indexDocumentVectors(index)) {
      setFixedBuffer(data->document, position - 1, id);
    }

    // If we are to index tf in addition to docid
    if(data->positional == TFONLY) {
      int* curtfBuffer = getTfBufferMaps(data->buffer, id);
      if(!curtfBuffer) {
        curtfBuffer = (int*) calloc(data->dfCutoff + 1, sizeof(int));
        data->buffer->tf[id] = curtfBuffer;
      }
      curtfBuffer[data->buffer->valuePosition[id]]++;
    } else if(data->positional == POSITIONAL) {
      int* curtfBuffer = getTfBufferMaps(data->buffer, id);
      int* curBuffer = data->buffer->position[id];
      // ps is the index in the position buffer that contains
      // the number of positions in the current block (because
      // there could be more than 1 position per term in a document)
      int ps = getFixedIntCounter(data->psum, id);

      // If this is a new term, create initial tf and position buffers
      if(!curBuffer) {
        curBuffer = (int*) calloc(data->dfCutoff, sizeof(int));
        data->buffer->position[id] = curBuffer;
        data->buffer->pvalueLength[id] = data->dfCutoff;
        data->buffer->pvaluePosition[id] = 1;

        curtfBuffer = (int*) calloc(data->dfCutoff + 1, sizeof(int));
        data->buffer->tf[id] = curtfBuffer;
      }

      // If position buffer is too small, expand it.
      if(data->buffer->pvalueLength[id] <= data->buffer->pvaluePosition[id] + 1) {
        int len = data->buffer->pvalueLength[id];
        int newLen = 2 * len;
        while(newLen <= data->buffer->pvaluePosition[id] + 1) {
          newLen *= 2;
        }
        int* tempCurBuffer = (int*) realloc(curBuffer, newLen * sizeof(int));
        memset(tempCurBuffer+len, 0, (newLen - len) * sizeof(int));
        data->buffer->position[id] = tempCurBuffer;
        data->buffer->pvalueLength[id] = newLen;
        curBuffer = data->buffer->position[id];
      }

      int pbufferpos = data->buffer->pvaluePosition[id];
      if(!added) {
        // On second or more occurrence, store pgaps in the buffer pool.
        // Then store the raw position, to be used to compute the next pgap, if any
        curBuffer[pbufferpos] = position - curBuffer[pbufferpos];
        pbufferpos++;
      } else {
        // On first occurrence, store raw position
        curBuffer[pbufferpos++] = position;
      }

      curBuffer[pbufferpos] = position;
      data->buffer->pvaluePosition[id]++;
      data->buffer->position[id][ps]++;
      curtfBuffer[data->buffer->valuePosition[id]]++;
    }

    position++;
    line += consumed;
    grabword(line, ' ', &consumed);
  }

  position--;
  setDocLen(index->pointers, docid, position);
  index->pointers->totalDocLen += position;
  index->pointers->totalDocs++;

  if(indexDocumentVectors(index)) {
    addDocumentVector(index->vectors, data->document->buffer, position, docid);
  }

  // Iterate over all unique terms
  int keyPos = -1;
  while((keyPos = nextIndexIntSet(data->uniqueTerms, keyPos)) != -1) {
    int id = data->uniqueTerms->key[keyPos];

    if(data->positional == TFONLY || data->positional == POSITIONAL) {
      int tf = data->buffer->tf[id][data->buffer->valuePosition[id]];
      int dl = getDocLen(index->pointers, docid);
      float bm25TfScore = _default_bm25tf(tf, dl,
                                          index->pointers->totalDocLen /
                                          ((float) index->pointers->totalDocs));
      float maxBm25TfScore = _default_bm25tf(getMaxTf(index->pointers, id),
                                             getMaxTfDocLen(index->pointers, id),
                                             index->pointers->totalDocLen /
                                             ((float) index->pointers->totalDocs));
      if(bm25TfScore > maxBm25TfScore) {
        setMaxTf(index->pointers, id, tf, dl);
      }
    }

    // Reset the "current position" stored at the end of position buffer
    if(data->positional == POSITIONAL) {
      data->buffer->position[id][data->buffer->pvaluePosition[id]] = 0;
    }

    // Grab the df value for the curren term
    int df = getDf(index->pointers, id);
    // If df is less than df cut-off, then do not index, but
    // continue storing docids into initial, much smaller buffers
    if(df < data->dfCutoff) {
      int* curBuffer = getDocidBufferMaps(data->buffer, id);
      if(!curBuffer) {
        curBuffer = (int*) calloc(data->dfCutoff, sizeof(int));
        data->buffer->docid[id] = curBuffer;
        data->buffer->valueLength[id] = data->dfCutoff;
      }
      data->buffer->docid[id][df] = docid;
      data->buffer->valuePosition[id]++;
      index->pointers->df->counter[id]++;
      continue;
    }

    // If df is greater than df cut-off, however, expand the buffers
    // to block size if necessary.
    int* curBuffer = data->buffer->docid[id];
    if(data->buffer->valueLength[id] < BLOCK_SIZE) {
      int* tempCurBuffer = (int*) realloc(curBuffer, BLOCK_SIZE * sizeof(int));
      memset(tempCurBuffer+data->dfCutoff, 0, (BLOCK_SIZE - data->dfCutoff) * sizeof(int));
      data->buffer->docid[id] = tempCurBuffer;
      data->buffer->valueLength[id] = BLOCK_SIZE;
      data->buffer->valuePosition[id] = data->dfCutoff;
      curBuffer = data->buffer->docid[id];

      if(data->positional == TFONLY || data->positional == POSITIONAL) {
        //expand tfbuffer
        int* tempTfBuffer = (int*) realloc(data->buffer->tf[id], BLOCK_SIZE * sizeof(int));
        memset(tempTfBuffer+data->dfCutoff+1, 0, (BLOCK_SIZE - data->dfCutoff - 1) * sizeof(int));
        data->buffer->tf[id] = tempTfBuffer;
      }

      if(data->positional == POSITIONAL) {
        //expand pbuffer
        int origLen = data->buffer->pvalueLength[id];
        int len = 2 * ((origLen / BLOCK_SIZE) + 1) * BLOCK_SIZE;
        int* tempPBuffer = (int*) realloc(data->buffer->position[id], len * sizeof(int));
        memset(tempPBuffer+origLen, 0, (len - origLen) * sizeof(int));
        data->buffer->position[id] = tempPBuffer;
        data->buffer->pvalueLength[id] = len;
      }
    }

    // Insert docid to the end of current docid buffer
    curBuffer[data->buffer->valuePosition[id]++] = docid;
    // Increment df
    index->pointers->df->counter[id]++;

    // If positional, and a block of docids has been accumulated,
    // then adjust ps (index in position buffer which contains the number of
    // positions in the current block)
    if(data->positional == POSITIONAL) {
      if(data->buffer->valuePosition[id] % BLOCK_SIZE == 0) {
        data->psum->counter[id] = data->buffer->pvaluePosition[id]++;
      }
    }

    // If docid buffer is full, compress and add segments (broken down to blocks)
    // to the inverted index.
    if(data->buffer->valuePosition[id] >= data->buffer->valueLength[id]) {
      // Find the number of blocks
      int nb = data->buffer->valueLength[id] / BLOCK_SIZE;
      // Find the tail pointer
      long pointer = data->buffer->tailPointer[id];
      if(nb == 1) {
        if(data->positional == TFONLY) {
          pointer = compressAndAddTfOnly(index->pool, curBuffer, data->buffer->tf[id],
                                         BLOCK_SIZE, pointer);
        } else if(data->positional == POSITIONAL) {
          pointer = compressAndAddPositional(index->pool, curBuffer, data->buffer->tf[id],
                                             // The first index (0) holds the number
                                             // of positions in the block
                                             &data->buffer->position[id][1],
                                             BLOCK_SIZE, data->buffer->position[id][0],
                                             pointer);
        } else {
          pointer = compressAndAddNonPositional(index->pool, curBuffer,
                                                BLOCK_SIZE, pointer);
        }
        // If no head pointer exists
        if(index->pool->reverse || getHeadPointer(index->pointers, id) == UNDEFINED_POINTER) {
          setHeadPointer(index->pointers, id, pointer);
        }
      } else {
        int j, ps = 0;
        for(j = 0; j < nb; j++) {
          if(data->positional == TFONLY) {
            pointer = compressAndAddTfOnly(index->pool, &curBuffer[j * BLOCK_SIZE],
                                           &data->buffer->tf[id][j * BLOCK_SIZE],
                                           BLOCK_SIZE, pointer);
          } else if(data->positional == POSITIONAL) {
            // The number of positions in the current block is stored at index "ps"
            pointer = compressAndAddPositional(index->pool, &curBuffer[j * BLOCK_SIZE],
                                               &data->buffer->tf[id][j * BLOCK_SIZE],
                                               &data->buffer->position[id][ps + 1],
                                               BLOCK_SIZE, data->buffer->position[id][ps],
                                               pointer);
            ps += data->buffer->position[id][ps] + 1;
          } else {
            pointer = compressAndAddNonPositional(index->pool, &curBuffer[j * BLOCK_SIZE],
                                                  BLOCK_SIZE, pointer);
          }
          if(index->pool->reverse || getHeadPointer(index->pointers, id) == UNDEFINED_POINTER) {
            setHeadPointer(index->pointers, id, pointer);
          }
        }
      }
      data->buffer->tailPointer[id] = pointer;

      // If expansion is enabled and the buffer hasn't reached maximum size,
      // then expand docid and tf buffers, leaving position buffer as is.
      if((data->buffer->valueLength[id] < data->maxBlocks) && data->expansionEnabled) {
        int newLen = data->buffer->valueLength[id] * EXPANSION_RATE;
        free(data->buffer->docid[id]);
        data->buffer->docid[id] = (int*) malloc(newLen * sizeof(int));
        data->buffer->valueLength[id] = newLen;

        if(data->positional == POSITIONAL || data->positional == TFONLY) {
          free(data->buffer->tf[id]);
          data->buffer->tf[id] = (int*) malloc(newLen * sizeof(int));
        }
      }

      // Reset docid buffer to 0
      memset(data->buffer->docid[id], 0, data->buffer->valueLength[id] * sizeof(int));

      // Reset tf buffer to 0, if tf buffer is used
      if(data->positional == POSITIONAL || data->positional == TFONLY) {
        memset(data->buffer->tf[id], 0, data->buffer->valueLength[id] * sizeof(int));
      }

      // Reset position buffer to 0, if position buffer is used
      if(data->positional == POSITIONAL) {
        memset(data->buffer->position[id], 0, data->buffer->pvalueLength[id] * sizeof(int));
        // Reset position buffer index to 1 (index 0 contains the number of positions)
        data->buffer->pvaluePosition[id] = 1;
        data->psum->counter[id] = 0;
      }

      // Reset docid buffer index to 0
      data->buffer->valuePosition[id] = 0;
    }
  }
  return termid;
}

/*
 * Reads one line from the input text.
 *
 * @param t Input text
 * @param buffer Buffer to store the extracted line into.
 * @param consumed Number of bytes read.
 * @return Whether the extracted line ends with a new line character
 */
int grabline(char* t, char* buffer, int* consumed) {
  int c = 0;
  char* s = t;
  *consumed = 0;
  while(*s != '\0' && *s != '\n') {
    (*consumed)++;
    s++;
  }
  if(*consumed == 0) return 0;

  memcpy(buffer, t, *consumed);
  buffer[*consumed] = '\0';
  *consumed += (*s == '\n');
  return *s == '\n';
}

int main (int argc, char** args) {
  // Index root path
  char* outputPath = getValueCL(argc, args, "-index");
  struct stat st = {0};
  if(stat(outputPath, &st) == -1) {
    mkdir(outputPath, 0700);
  }
  // Maximum buffer length (in blocks)
  int maxBlocks = atoi(getValueCL(argc, args, "-mb")) * BLOCK_SIZE;
  // Non-positional, docid and tf, or positional index
  int positional = NONPOSITIONAL;
  if(isPresentCL(argc, args, "-positional")) {
    positional = POSITIONAL;
  } else if(isPresentCL(argc, args, "-tf")) {
    positional = TFONLY;
  }
  // Whether to store Bloom filter representation of docids
  int bloomEnabled = isPresentCL(argc, args, "-bloom");
  unsigned int nbHash, bitsPerElement;
  if(bloomEnabled) {
    nbHash = atoi(getValueCL(argc, args, "-k"));
    bitsPerElement = atoi(getValueCL(argc, args, "-r"));
  }

  int reverse = 0;
  if(isPresentCL(argc, args, "-reverse")) {
    reverse = 1;
  }

  int documentVectors = 0;
  if(isPresentCL(argc, args, "-vectors")) {
    documentVectors = 1;
  }

  int dfCutoff = DF_CUTOFF;
  if(isPresentCL(argc, args, "-dfCutoff")) {
    dfCutoff = atoi(getValueCL(argc, args, "-dfCutoff"));
  }

  // List of input files (must be the last argument)
  int inputBeginIndex = isPresentCL(argc, args, "-input") + 1;

  // Creating and initializing the inverted index and its auxiliary data structures
  InvertedIndex* index = createInvertedIndex(reverse, documentVectors,
                                             bloomEnabled, nbHash, bitsPerElement);
  IndexingData* data = (IndexingData*) malloc(sizeof(IndexingData));
  data->buffer = createBufferMaps(DEFAULT_VOCAB_SIZE, positional);
  if(positional == POSITIONAL) {
    data->psum = createFixedIntCounter(DEFAULT_VOCAB_SIZE, 0);
  } else {
    data->psum = NULL;
  }
  data->uniqueTerms = createIntSet(2048);
  data->document = createFixedBuffer(2048);
  data->expansionEnabled = (maxBlocks > BLOCK_SIZE);
  data->maxBlocks = maxBlocks;
  data->positional = positional;
  data->dfCutoff = dfCutoff;

  // Start term id from 0
  int termid = 0;

  // I/O buffers
  unsigned char* oldBuffer = (unsigned char*) calloc(LINE_LENGTH * 2, sizeof(unsigned char));
  unsigned char* iobuffer = (unsigned char*) calloc(LENGTH, sizeof(unsigned char));
  unsigned char* line = (unsigned char*) calloc(LINE_LENGTH, sizeof(unsigned char));
  gzFile * file;

  struct timeval start, end;
  gettimeofday(&start, NULL);

  int fp = 0;
  int len = 0;
  for(fp = inputBeginIndex; fp < argc; fp++) {
    file = gzopen(args[fp], "r");
    int oldBufferIndex = 0;

    while (1) {
      // Read one chunk into iobuffer from the current file
      int bytes_read;
      bytes_read = gzread (file, iobuffer, LENGTH - 1);
      iobuffer[bytes_read] = '\0';

      int consumed;
      int start = 0;
      int c;
      // If the first character is the new line character,
      // process the previously extracted line.
      if(iobuffer[0] == '\n') {
        consumed = 1;
        c = 1;
      } else {
        // Otherwise, read one line from the input and store it
        // into the line buffer at the appropriate position.
        c = grabline(iobuffer, line+len, &consumed);
        len += consumed;
      }
      // While there are more bytes available
      while(c > 0) {
        // If a full line has been read
        if(iobuffer[start+consumed - 1] == '\n') {
          // Concatenate the old buffer with the current buffer to
          // reconstruct the entire line
          if(oldBufferIndex > 0) {
            memcpy(oldBuffer+oldBufferIndex, line, consumed);
            termid = process(index, data, oldBuffer, termid);
            memset(oldBuffer, 0, oldBufferIndex);
            oldBufferIndex = 0;
            len = 0;
          } else {
            termid = process(index, data, line, termid);
            len = 0;
          }
        } else {
          // Otherwise, add the current text to "oldBuffer"
          // and reset the "line" buffer
          memcpy(oldBuffer+oldBufferIndex, line, consumed);
          oldBufferIndex += consumed;
          len = 0;
        }

        start += consumed;
        c = grabline(iobuffer+start, line + len, &consumed);
        len += consumed;
      }
      if (bytes_read < LENGTH - 1) {
        if (gzeof (file)) {
          break;
        }
      }
    }
    gzclose (file);

    gettimeofday(&end, NULL);
    printf("Files processed: %d Time: %6.0f\n", (fp - inputBeginIndex + 1),
           ((float) (end.tv_sec - start.tv_sec)));
    fflush(stdout);
  }

  // Indexing is done. But iterate over all terms, and dump
  // the remaining postings in the buffer pools to the actual
  // inverted index (if df > df_cut-off)
  unsigned int termsInBuffer = 0;
  int term = -1;
  while((term = nextIndexBufferMaps(data->buffer, term, BLOCK_SIZE)) != -1) {
    termsInBuffer++;
    int pos = data->buffer->valuePosition[term];

    if(pos > 0) {
      int nb = pos / BLOCK_SIZE;
      int res = pos % BLOCK_SIZE;
      int ps = 0;

      int* curBuffer = data->buffer->docid[term];
      long pointer = data->buffer->tailPointer[term];
      int j;
      for(j = 0; j < nb; j++) {
        if(positional == TFONLY) {
          pointer =
            compressAndAddTfOnly(index->pool, &curBuffer[j * BLOCK_SIZE],
                                 &data->buffer->tf[term][j * BLOCK_SIZE],
                                 BLOCK_SIZE, pointer);
        } else if(positional == POSITIONAL) {
          pointer =
            compressAndAddPositional(index->pool, &curBuffer[j * BLOCK_SIZE],
                                     &data->buffer->tf[term][j * BLOCK_SIZE],
                                     &data->buffer->position[term][ps + 1],
                                     BLOCK_SIZE, data->buffer->position[term][ps],
                                     pointer);
          ps += data->buffer->position[term][ps] + 1;
        } else {
          pointer =
            compressAndAddNonPositional(index->pool, &curBuffer[j * BLOCK_SIZE],
                                        BLOCK_SIZE, pointer);
        }
        if(index->pool->reverse || getHeadPointer(index->pointers, term) == UNDEFINED_POINTER) {
          setHeadPointer(index->pointers, term, pointer);
        }
      }

      if(res > 0) {
        if(positional == TFONLY) {
          pointer =
            compressAndAddTfOnly(index->pool, &curBuffer[nb * BLOCK_SIZE],
                                 &data->buffer->tf[term][nb * BLOCK_SIZE],
                                 res, pointer);
        } else if(positional == POSITIONAL) {
          pointer =
            compressAndAddPositional(index->pool, &curBuffer[nb * BLOCK_SIZE],
                                     &data->buffer->tf[term][nb * BLOCK_SIZE],
                                     &data->buffer->position[term][ps + 1],
                                     res, data->buffer->position[term][ps],
                                     pointer);
        } else {
          pointer =
            compressAndAddNonPositional(index->pool, &curBuffer[nb * BLOCK_SIZE],
                                        res, pointer);
        }
        if(index->pool->reverse || getHeadPointer(index->pointers, term) == UNDEFINED_POINTER) {
          setHeadPointer(index->pointers, term, pointer);
        }
      }
    }
  }

  gettimeofday(&end, NULL);
  printf("Time: %6.0f\n", ((float) (end.tv_sec - start.tv_sec)));
  printf("Terms in buffer: %u\n", termsInBuffer);
  fflush(stdout);

  // Write the inverted index to the specified output path
  writeInvertedIndex(index, outputPath);

  // Free the allocated memory
  destroyInvertedIndex(index);
  destroyIndexingData(data);
  free(oldBuffer);
  free(iobuffer);
  free(line);
  return 0;
}
