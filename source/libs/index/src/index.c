/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "index.h"
#include "indexInt.h"
#include "index_cache.h"
#include "index_tfile.h"
#include "tdef.h"
#include "tsched.h"

#ifdef USE_LUCENE
#include "lucene++/Lucene_c.h"
#endif

#define INDEX_NUM_OF_THREADS 4
#define INDEX_QUEUE_SIZE 200

void* indexQhandle = NULL;

int32_t indexInit() {
  indexQhandle = taosInitScheduler(INDEX_QUEUE_SIZE, INDEX_NUM_OF_THREADS, "index");
  return indexQhandle == NULL ? -1 : 0;
  // do nothing
}
void indexCleanUp() { taosCleanUpScheduler(indexQhandle); }

static int uidCompare(const void* a, const void* b) {
  uint64_t u1 = *(uint64_t*)a;
  uint64_t u2 = *(uint64_t*)b;
  if (u1 == u2) {
    return 0;
  } else {
    return u1 < u2 ? -1 : 1;
  }
}
typedef struct SIdxColInfo {
  int colId;  // generated by index internal
  int cVersion;
} SIdxColInfo;

static pthread_once_t isInit = PTHREAD_ONCE_INIT;
// static void           indexInit();
static int indexTermSearch(SIndex* sIdx, SIndexTermQuery* term, SArray** result);

static void indexInterResultsDestroy(SArray* results);
static int  indexMergeFinalResults(SArray* interResults, EIndexOperatorType oType, SArray* finalResult);

static int indexGenTFile(SIndex* index, IndexCache* cache, SArray* batch);

int indexOpen(SIndexOpts* opts, const char* path, SIndex** index) {
  // pthread_once(&isInit, indexInit);
  SIndex* sIdx = calloc(1, sizeof(SIndex));
  if (sIdx == NULL) { return -1; }

#ifdef USE_LUCENE
  index_t* index = index_open(path);
  sIdx->index = index;
#endif

#ifdef USE_INVERTED_INDEX
  // sIdx->cache = (void*)indexCacheCreate(sIdx);
  sIdx->tindex = indexTFileCreate(path);
  sIdx->colObj = taosHashInit(8, taosGetDefaultHashFunction(TSDB_DATA_TYPE_BINARY), true, HASH_ENTRY_LOCK);
  sIdx->cVersion = 1;
  sIdx->path = calloc(1, strlen(path) + 1);
  memcpy(sIdx->path, path, strlen(path));
  pthread_mutex_init(&sIdx->mtx, NULL);

  *index = sIdx;

  return 0;
#endif

  *index = NULL;
  return -1;
}

void indexClose(SIndex* sIdx) {
#ifdef USE_LUCENE
  index_close(sIdex->index);
  sIdx->index = NULL;
#endif

#ifdef USE_INVERTED_INDEX
  void* iter = taosHashIterate(sIdx->colObj, NULL);
  while (iter) {
    IndexCache** pCache = iter;
    if (*pCache) { indexCacheUnRef(*pCache); }
    iter = taosHashIterate(sIdx->colObj, iter);
  }
  taosHashCleanup(sIdx->colObj);
  pthread_mutex_destroy(&sIdx->mtx);
#endif
  free(sIdx->path);
  free(sIdx);
  return;
}

int indexPut(SIndex* index, SIndexMultiTerm* fVals, uint64_t uid) {
#ifdef USE_LUCENE
  index_document_t* doc = index_document_create();

  char buf[16] = {0};
  sprintf(buf, "%d", uid);

  for (int i = 0; i < taosArrayGetSize(fVals); i++) {
    SIndexTerm* p = taosArrayGetP(fVals, i);
    index_document_add(doc, (const char*)(p->key), p->nKey, (const char*)(p->val), p->nVal, 1);
  }
  index_document_add(doc, NULL, 0, buf, strlen(buf), 0);

  index_put(index->index, doc);
  index_document_destroy(doc);
#endif

#ifdef USE_INVERTED_INDEX

  // TODO(yihao): reduce the lock range
  pthread_mutex_lock(&index->mtx);
  for (int i = 0; i < taosArrayGetSize(fVals); i++) {
    SIndexTerm*  p = taosArrayGetP(fVals, i);
    IndexCache** cache = taosHashGet(index->colObj, p->colName, p->nColName);
    if (cache == NULL) {
      IndexCache* pCache = indexCacheCreate(index, p->colName, p->colType);
      taosHashPut(index->colObj, p->colName, p->nColName, &pCache, sizeof(void*));
    }
  }
  pthread_mutex_unlock(&index->mtx);

  for (int i = 0; i < taosArrayGetSize(fVals); i++) {
    SIndexTerm*  p = taosArrayGetP(fVals, i);
    IndexCache** cache = taosHashGet(index->colObj, p->colName, p->nColName);
    assert(*cache != NULL);
    int ret = indexCachePut(*cache, p, uid);
    if (ret != 0) { return ret; }
  }

#endif
  return 0;
}
int indexSearch(SIndex* index, SIndexMultiTermQuery* multiQuerys, SArray* result) {
#ifdef USE_LUCENE
  EIndexOperatorType opera = multiQuerys->opera;

  int    nQuery = taosArrayGetSize(multiQuerys->query);
  char** fields = malloc(sizeof(char*) * nQuery);
  char** keys = malloc(sizeof(char*) * nQuery);
  int*   types = malloc(sizeof(int) * nQuery);

  for (int i = 0; i < nQuery; i++) {
    SIndexTermQuery* p = taosArrayGet(multiQuerys->query, i);
    SIndexTerm*      term = p->field_value;

    fields[i] = calloc(1, term->nKey + 1);
    keys[i] = calloc(1, term->nVal + 1);

    memcpy(fields[i], term->key, term->nKey);
    memcpy(keys[i], term->val, term->nVal);
    types[i] = (int)(p->type);
  }
  int* tResult = NULL;
  int  tsz = 0;
  index_multi_search(index->index, (const char**)fields, (const char**)keys, types, nQuery, opera, &tResult, &tsz);

  for (int i = 0; i < tsz; i++) { taosArrayPush(result, &tResult[i]); }

  for (int i = 0; i < nQuery; i++) {
    free(fields[i]);
    free(keys[i]);
  }
  free(fields);
  free(keys);
  free(types);
#endif

#ifdef USE_INVERTED_INDEX
  EIndexOperatorType opera = multiQuerys->opera;  // relation of querys

  SArray* interResults = taosArrayInit(4, POINTER_BYTES);
  int     nQuery = taosArrayGetSize(multiQuerys->query);
  for (size_t i = 0; i < nQuery; i++) {
    SIndexTermQuery* qTerm = taosArrayGet(multiQuerys->query, i);
    SArray*          tResult = NULL;
    indexTermSearch(index, qTerm, &tResult);
    taosArrayPush(interResults, (void*)&tResult);
  }
  indexMergeFinalResults(interResults, opera, result);
  indexInterResultsDestroy(interResults);

#endif
  return 1;
}

int indexDelete(SIndex* index, SIndexMultiTermQuery* query) {
#ifdef USE_INVERTED_INDEX
#endif

  return 1;
}
int indexRebuild(SIndex* index, SIndexOpts* opts){
#ifdef USE_INVERTED_INDEX
#endif

}

SIndexOpts* indexOptsCreate() {
#ifdef USE_LUCENE
#endif
  return NULL;
}
void indexOptsDestroy(SIndexOpts* opts) {
#ifdef USE_LUCENE
#endif
  return;
}
/*
 * @param: oper
 *
 */
SIndexMultiTermQuery* indexMultiTermQueryCreate(EIndexOperatorType opera) {
  SIndexMultiTermQuery* p = (SIndexMultiTermQuery*)malloc(sizeof(SIndexMultiTermQuery));
  if (p == NULL) { return NULL; }
  p->opera = opera;
  p->query = taosArrayInit(4, sizeof(SIndexTermQuery));
  return p;
}
void indexMultiTermQueryDestroy(SIndexMultiTermQuery* pQuery) {
  for (int i = 0; i < taosArrayGetSize(pQuery->query); i++) {
    SIndexTermQuery* p = (SIndexTermQuery*)taosArrayGet(pQuery->query, i);
    indexTermDestroy(p->term);
  }
  taosArrayDestroy(pQuery->query);
  free(pQuery);
};
int indexMultiTermQueryAdd(SIndexMultiTermQuery* pQuery, SIndexTerm* term, EIndexQueryType qType) {
  SIndexTermQuery q = {.qType = qType, .term = term};
  taosArrayPush(pQuery->query, &q);
  return 0;
}

SIndexTerm* indexTermCreate(int64_t suid, SIndexOperOnColumn oper, uint8_t colType, const char* colName,
                            int32_t nColName, const char* colVal, int32_t nColVal) {
  SIndexTerm* t = (SIndexTerm*)calloc(1, (sizeof(SIndexTerm)));
  if (t == NULL) { return NULL; }

  t->suid = suid;
  t->operType = oper;
  t->colType = colType;

  t->colName = (char*)calloc(1, nColName + 1);
  memcpy(t->colName, colName, nColName);
  t->nColName = nColName;

  t->colVal = (char*)calloc(1, nColVal + 1);
  memcpy(t->colVal, colVal, nColVal);
  t->nColVal = nColVal;
  return t;
}
void indexTermDestroy(SIndexTerm* p) {
  free(p->colName);
  free(p->colVal);
  free(p);
}

SIndexMultiTerm* indexMultiTermCreate() { return taosArrayInit(4, sizeof(SIndexTerm*)); }

int indexMultiTermAdd(SIndexMultiTerm* terms, SIndexTerm* term) {
  taosArrayPush(terms, &term);
  return 0;
}
void indexMultiTermDestroy(SIndexMultiTerm* terms) {
  for (int32_t i = 0; i < taosArrayGetSize(terms); i++) {
    SIndexTerm* p = taosArrayGetP(terms, i);
    indexTermDestroy(p);
  }
  taosArrayDestroy(terms);
}

static int indexTermSearch(SIndex* sIdx, SIndexTermQuery* query, SArray** result) {
  SIndexTerm* term = query->term;
  const char* colName = term->colName;
  int32_t     nColName = term->nColName;

  // Get col info
  IndexCache* cache = NULL;
  pthread_mutex_lock(&sIdx->mtx);
  IndexCache** pCache = taosHashGet(sIdx->colObj, colName, nColName);
  if (pCache == NULL) {
    pthread_mutex_unlock(&sIdx->mtx);
    return -1;
  }
  cache = *pCache;
  pthread_mutex_unlock(&sIdx->mtx);

  *result = taosArrayInit(4, sizeof(uint64_t));
  // TODO: iterator mem and tidex
  STermValueType s = kTypeValue;
  if (0 == indexCacheSearch(cache, query, *result, &s)) {
    if (s == kTypeDeletion) {
      indexInfo("col: %s already drop by other opera", term->colName);
      // coloum already drop by other oper, no need to query tindex
      return 0;
    } else {
      if (0 != indexTFileSearch(sIdx->tindex, query, *result)) {
        indexError("corrupt at index(TFile) col:%s val: %s", term->colName, term->colVal);
        return -1;
      }
    }
  } else {
    indexError("corrupt at index(cache) col:%s val: %s", term->colName, term->colVal);
    return -1;
  }
  return 0;
}
static void indexInterResultsDestroy(SArray* results) {
  if (results == NULL) { return; }

  size_t sz = taosArrayGetSize(results);
  for (size_t i = 0; i < sz; i++) {
    SArray* p = taosArrayGetP(results, i);
    taosArrayDestroy(p);
  }
  taosArrayDestroy(results);
}
static int indexMergeFinalResults(SArray* interResults, EIndexOperatorType oType, SArray* fResults) {
  // refactor, merge interResults into fResults by oType
  SArray* first = taosArrayGetP(interResults, 0);
  taosArraySort(first, uidCompare);
  taosArrayRemoveDuplicate(first, uidCompare, NULL);

  if (oType == MUST) {
    // just one column index, enhance later
    taosArrayAddAll(fResults, first);
  } else if (oType == SHOULD) {
    // just one column index, enhance later
    taosArrayAddAll(fResults, first);
    // tag1 condistion || tag2 condition
  } else if (oType == NOT) {
    // just one column index, enhance later
    taosArrayAddAll(fResults, first);
    // not use currently
  }
  return 0;
}

static void indexMergeSameKey(SArray* result, TFileValue* tv) {
  int32_t sz = result ? taosArrayGetSize(result) : 0;
  if (sz > 0) {
    // TODO(yihao): remove duplicate tableid
    TFileValue* lv = taosArrayGetP(result, sz - 1);
    if (strcmp(lv->colVal, tv->colVal) == 0) {
      taosArrayAddAll(lv->tableId, tv->tableId);
      tfileValueDestroy(tv);
    } else {
      taosArrayPush(result, &tv);
    }
  } else {
    taosArrayPush(result, &tv);
  }
}
static void indexDestroyTempResult(SArray* result) {
  int32_t sz = result ? taosArrayGetSize(result) : 0;
  for (size_t i = 0; i < sz; i++) {
    TFileValue* tv = taosArrayGetP(result, i);
    tfileValueDestroy(tv);
  }
  taosArrayDestroy(result);
}
int indexFlushCacheTFile(SIndex* sIdx, void* cache) {
  if (sIdx == NULL) { return -1; }
  indexWarn("suid %" PRIu64 " merge cache into tindex", sIdx->suid);

  IndexCache*  pCache = (IndexCache*)cache;
  TFileReader* pReader = tfileGetReaderByCol(sIdx->tindex, pCache->colName);
  // handle flush
  Iterate* cacheIter = indexCacheIteratorCreate(pCache);
  Iterate* tfileIter = tfileIteratorCreate(pReader);

  SArray* result = taosArrayInit(1024, sizeof(void*));

  bool cn = cacheIter ? cacheIter->next(cacheIter) : false;
  bool tn = tfileIter ? tfileIter->next(tfileIter) : false;
  while (cn == true && tn == true) {
    IterateValue* cv = cacheIter->getValue(cacheIter);
    IterateValue* tv = tfileIter->getValue(tfileIter);

    // dump value
    int comp = strcmp(cv->colVal, tv->colVal);
    if (comp == 0) {
      TFileValue* tfv = tfileValueCreate(cv->colVal);
      taosArrayAddAll(tfv->tableId, cv->val);
      taosArrayAddAll(tfv->tableId, tv->val);
      indexMergeSameKey(result, tfv);

      cn = cacheIter->next(cacheIter);
      tn = tfileIter->next(tfileIter);
      continue;
    } else if (comp < 0) {
      TFileValue* tfv = tfileValueCreate(cv->colVal);
      taosArrayAddAll(tfv->tableId, cv->val);

      indexMergeSameKey(result, tfv);
      // copy to final Result;
      cn = cacheIter->next(cacheIter);
    } else {
      TFileValue* tfv = tfileValueCreate(tv->colVal);
      taosArrayAddAll(tfv->tableId, tv->val);

      indexMergeSameKey(result, tfv);
      // copy to final result
      tn = tfileIter->next(tfileIter);
    }
  }
  while (cn == true) {
    IterateValue* cv = cacheIter->getValue(cacheIter);
    TFileValue*   tfv = tfileValueCreate(cv->colVal);
    taosArrayAddAll(tfv->tableId, cv->val);
    indexMergeSameKey(result, tfv);
    cn = cacheIter->next(cacheIter);
  }
  while (tn == true) {
    IterateValue* tv = tfileIter->getValue(tfileIter);
    TFileValue*   tfv = tfileValueCreate(tv->colVal);
    if (tv->val == NULL) {
      // HO
      printf("NO....");
    }
    taosArrayAddAll(tfv->tableId, tv->val);
    indexMergeSameKey(result, tfv);
    tn = tfileIter->next(tfileIter);
  }
  int ret = indexGenTFile(sIdx, pCache, result);
  indexDestroyTempResult(result);
  indexCacheDestroyImm(pCache);

  indexCacheIteratorDestroy(cacheIter);
  tfileIteratorDestroy(tfileIter);

  tfileReaderUnRef(pReader);
  indexCacheUnRef(pCache);
  return 0;
}
void iterateValueDestroy(IterateValue* value, bool destroy) {
  if (destroy) {
    taosArrayDestroy(value->val);
    value->val = NULL;
  } else {
    if (value->val != NULL) { taosArrayClear(value->val); }
  }
  // free(value->colVal);
  value->colVal = NULL;
}
static int indexGenTFile(SIndex* sIdx, IndexCache* cache, SArray* batch) {
  int32_t version = CACHE_VERSION(cache);
  uint8_t colType = cache->type;

  TFileWriter* tw = tfileWriterOpen(sIdx->path, sIdx->suid, version, cache->colName, colType);
  if (tw == NULL) {
    indexError("failed to open file to write");
    return -1;
  }

  int ret = tfileWriterPut(tw, batch, true);
  if (ret != 0) {
    indexError("failed to write into tindex ");
    goto END;
  }
  tfileWriterClose(tw);

  TFileReader* reader = tfileReaderOpen(sIdx->path, sIdx->suid, version, cache->colName);

  char          buf[128] = {0};
  TFileHeader*  header = &reader->header;
  TFileCacheKey key = {.suid = header->suid,
                       .colName = header->colName,
                       .nColName = strlen(header->colName),
                       .colType = header->colType};
  pthread_mutex_lock(&sIdx->mtx);

  IndexTFile* ifile = (IndexTFile*)sIdx->tindex;
  tfileCachePut(ifile->cache, &key, reader);

  pthread_mutex_unlock(&sIdx->mtx);
  return ret;
END:
  tfileWriterClose(tw);
}
