#ifndef DOCUMENT_VECTOR_H_GUARD
#define DOCUMENT_VECTOR_H_GUARD

#include <stdlib.h>
#include <stdio.h>
#include "Config.h"
#include "buffer/FixedIntCounter.h"
#include "pfordelta/opt_p4.h"

typedef struct DocumentVector DocumentVector;

struct DocumentVector {
  unsigned int** document;
  unsigned int* length;
  unsigned int capacity;
};

void writeDocumentVector(DocumentVector* vectors, FILE* fp) {
  fwrite(&vectors->capacity, sizeof(unsigned int), 1, fp);
  int i;
  for(i = 0; i < vectors->capacity; i++) {
    if(vectors->document[i]) {
      fwrite(&i, sizeof(int), 1, fp);
      fwrite(&vectors->length[i], sizeof(int), 1, fp);
      fwrite(vectors->document[i], sizeof(int), vectors->length[i], fp);
    }
  }
  i = -1;
  fwrite(&i, sizeof(int), 1, fp);
}

DocumentVector* readDocumentVector(FILE* fp) {
  DocumentVector* vectors = (DocumentVector*) malloc(sizeof(DocumentVector));
  fread(&vectors->capacity, sizeof(unsigned int), 1, fp);
  vectors->document = (unsigned int**) calloc(vectors->capacity, sizeof(unsigned int*));
  vectors->length = (unsigned int*) calloc(vectors->capacity, sizeof(unsigned int));

  int i;
  fread(&i, sizeof(int), 1, fp);
  while(i >= 0) {
    fread(&vectors->length[i], sizeof(unsigned int), 1, fp);
    vectors->document[i] = (unsigned int*) calloc(vectors->length[i], sizeof(unsigned int));
    fread(vectors->document[i], sizeof(unsigned int), vectors->length[i], fp);
    fread(&i, sizeof(int), 1, fp);
  }
  return vectors;
}

DocumentVector* createDocumentVector(unsigned int initialSize) {
  DocumentVector* vectors = (DocumentVector*)
    malloc(sizeof(DocumentVector));
  vectors->capacity = initialSize;
  vectors->length = (unsigned int*) calloc(initialSize, sizeof(unsigned int));
  vectors->document = (unsigned int**) calloc(initialSize, sizeof(unsigned int*));
  int i;
  return vectors;
}

void destroyDocumentVector(DocumentVector* vectors) {
  int i;
  for(i = 0; i < vectors->capacity; i++) {
    if(vectors->document[i]) {
      free(vectors->document[i]);
    }
  }
  free(vectors->document);
  free(vectors->length);
  free(vectors);
}

void expandDocumentVector(DocumentVector* vectors) {
  unsigned int** tempDocument = (unsigned int**) realloc(vectors->document,
      vectors->capacity * 2 * sizeof(unsigned int*));
  unsigned int* tempLength = (unsigned int*) realloc(vectors->length,
      vectors->capacity * 2 * sizeof(unsigned int));
  int i;
  for(i = vectors->capacity; i < vectors->capacity * 2; i++) {
    tempDocument[i] = NULL;
    tempLength = 0;
  }
  vectors->document = tempDocument;
  vectors->length = tempLength;
  vectors->capacity *= 2;
}

int containsDocumentVector(DocumentVector* vectors, int k) {
  return vectors->document[k] != NULL;
}

void getDocumentVector(DocumentVector* vectors, unsigned int* document, int length, int k) {
  if(k >= vectors->capacity || !vectors->document[k]) {
    document = NULL;
    return;
  }
  unsigned int aux[BLOCK_SIZE * 4];
  int nb = vectors->document[k][0], i, pos = 1;
  unsigned int* buffer = (unsigned int*) calloc(nb * BLOCK_SIZE, sizeof(unsigned int));
  for(i = 0; i < nb; i++) {
    detailed_p4_decode(&buffer[i * BLOCK_SIZE], &vectors->document[k][pos + 1], aux, 0, 0);
    pos += vectors->document[k][pos] + 1;
    memset(aux, 0, BLOCK_SIZE * 4 * sizeof(unsigned int));
  }
  memcpy(document, buffer, length * sizeof(unsigned int));
}

void addDocumentVector(DocumentVector* vectors, unsigned int* document,
                       unsigned int length, int k) {
  if(k >= vectors->capacity) {
    expandDocumentVector(vectors);
  }

  int nb = length / BLOCK_SIZE;
  int res = length % BLOCK_SIZE;
  unsigned int* block = (unsigned int*) calloc((nb + 1) * BLOCK_SIZE * 2, sizeof(unsigned int));
  int csize = 1, i = 0;

  for(i = 0; i < nb; i++) {
    int tempSize = OPT4(&document[i * BLOCK_SIZE], BLOCK_SIZE, &block[csize + 1], 0);
    block[csize] = tempSize;
    csize += tempSize + 1;
  }
  if(res > 0) {
    unsigned int* a = (unsigned int*) calloc(BLOCK_SIZE, sizeof(unsigned int));
    memcpy(a, &document[nb * BLOCK_SIZE], res * sizeof(unsigned int));
    int tempSize = OPT4(a, res, &block[csize + 1], 0);
    block[csize] = tempSize;
    csize += tempSize + 1;
    free(a);
    i++;
  }
  vectors->length[k] = csize;
  vectors->document[k] = (unsigned int*) calloc(csize, sizeof(unsigned int));
  vectors->document[k][0] = i;
  for(i = 1; i < csize; i++) {
    vectors->document[k][i] = block[i];
  }
  free(block);
}

#endif