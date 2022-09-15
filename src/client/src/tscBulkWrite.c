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

#include "osAtomic.h"

#include "tsclient.h"
#include "tscBulkWrite.h"
#include "tscSubquery.h"
#include "tscLog.h"

/**
 * Represents the callback function and its context.
 */
typedef struct {
  __async_cb_func_t fp;
  void*             param;
} Runnable;

/**
 * The context of `batchResultCallback`.
 */
typedef struct {
  size_t   count;
  Runnable runnable[];
} BatchCallBackContext;

/**
 * Get the number of insertion row in the sql statement.
 *
 * @param pSql      the sql statement.
 * @return int32_t  the number of insertion row.
 */
inline static int32_t statementGetInsertionRows(SSqlObj* pSql) { return pSql->cmd.insertParam.numOfRows; }

/**
 * Return the error result to the callback function, and release the sql object.
 *
 * @param pSql  the sql object.
 * @param code  the error code of the error result.
 */
inline static void tscReturnsError(SSqlObj *pSql, int code) {
  if (pSql == NULL) {
    return;
  }

  pSql->res.code = code;
  tscAsyncResultOnError(pSql);
}

/**
 * Proxy function to perform sequentially insert operation.
 *
 * @param param     the context of `batchResultCallback`.
 * @param tres      the result object.
 * @param code      the error code.
 */
static void batchResultCallback(void* param, TAOS_RES* tres, int32_t code) {
  BatchCallBackContext* context = param;
  SSqlObj*              res = tres;

  // handle corner case [context == null].
  if (context == NULL) {
    tscError("context in `batchResultCallback` is null, which should not happen");
    if (tres) {
      taosReleaseRef(tscObjRef, res->self);
    }
    return;
  }

  // handle corner case [res == null].
  if (res == NULL) {
    tscError("tres in `batchResultCallback` is null, which should not happen");
    free(context);
    return;
  }

  // handle results.
  tscDebug("async batch result callback, number of item: %zu", context->count);
  for (int i = 0; i < context->count; ++i) {
    // the result object is shared by many sql objects.
    // therefore, we need to increase the ref count.
    taosAcquireRef(tscObjRef, res->self);

    Runnable* runnable = &context->runnable[i];
    runnable->fp(runnable->param, res, res == NULL ? code : taos_errno(res));
  }

  taosReleaseRef(tscObjRef, res->self);
  free(param);
}

int32_t dispatcherStatementMerge(SArray* statements, SSqlObj** result) {
  if (statements == NULL) {
    return TSDB_CODE_SUCCESS;
  }

  size_t count = taosArrayGetSize(statements);
  if (count == 0) {
    return TSDB_CODE_SUCCESS;
  }

  // create the callback context.
  BatchCallBackContext* context = calloc(1, sizeof(BatchCallBackContext) + count * sizeof(Runnable));
  if (context == NULL) {
    return TSDB_CODE_TSC_OUT_OF_MEMORY;
  }

  tscDebug("create batch call back context: %p", context);

  // initialize the callback context.
  context->count = count;
  for (size_t i = 0; i < count; ++i) {
    SSqlObj*  statement = *((SSqlObj**)taosArrayGet(statements, i));
    Runnable* callback = &context->runnable[i];

    callback->fp = statement->fp;
    callback->param = statement->param;
  }

  // merge the statements into single one.
  tscDebug("start to merge %zu sql objs", count);
  int32_t code = tscMergeKVPayLoadSqlObj(statements, result);
  if (code != TSDB_CODE_SUCCESS) {
    const char* msg = tstrerror(code);
    tscDebug("failed to merge sql objects: %s", msg);
    free(context);
  } else {
    // set the merged sql object callback.
    (*result)->fp = batchResultCallback;
    (*result)->fetchFp = (*result)->fp;
    (*result)->param = context;
  }
  return code;
}

SArray* dispatcherPollAll(SAsyncBulkWriteDispatcher* dispatcher) {
  if (!atomic_load_32(&dispatcher->bufferSize)) {
    return NULL;
  }
  
  pthread_mutex_lock(&dispatcher->mutex);
  SArray* statements = taosArrayInit(0, sizeof(SSqlObj*));
  if (statements == NULL) {
    pthread_mutex_unlock(&dispatcher->mutex);
    tscError("failed to poll all items: out of memory");
    return NULL;
  }

  // get all the sql statements from the buffer.
  while (true) {
    SListNode* node = tdListPopHead(dispatcher->buffer);
    if (!node) {
      break;
    }

    // get the SSqlObj* from the node.
    SSqlObj* item;
    memcpy(&item, node->data, sizeof(SSqlObj*));
    listNodeFree(node);
    atomic_fetch_sub_32(&dispatcher->bufferSize, 1);
    atomic_fetch_sub_32(&dispatcher->currentSize, statementGetInsertionRows(item));
    taosArrayPush(statements, &item);
  }
  
  pthread_mutex_unlock(&dispatcher->mutex);
  return statements;
}

int32_t dispatcherTryOffer(SAsyncBulkWriteDispatcher* dispatcher, SSqlObj* pSql) {
  // the buffer is full.
  if (atomic_load_32(&dispatcher->currentSize) >= dispatcher->batchSize) {
    return -1;
  }

  // offer the node to the buffer.
  pthread_mutex_lock(&dispatcher->mutex);
  if (tdListAppend(dispatcher->buffer, &pSql)) {
    pthread_mutex_unlock(&dispatcher->mutex);
    return -1;
  }

  tscDebug("sql obj %p has been write to insert buffer", pSql);

  atomic_fetch_add_32(&dispatcher->bufferSize, 1);
  int32_t numOfRows = statementGetInsertionRows(pSql);
  int32_t currentSize = atomic_add_fetch_32(&dispatcher->currentSize, numOfRows);
  pthread_mutex_unlock(&dispatcher->mutex);
  return currentSize;
}

void dispatcherExecute(SArray* statements) {
  int32_t code = TSDB_CODE_SUCCESS;
  // no item in the buffer (items has been taken by other threads).
  if (!statements || !taosArrayGetSize(statements)) {
    return;
  }

  // merge the statements into single one.
  SSqlObj* merged = NULL;
  code = dispatcherStatementMerge(statements, &merged);
  if (code != TSDB_CODE_SUCCESS) {
    goto _error;
  }

  tscDebug("merging %zu sql objs into %p", taosArrayGetSize(statements), merged);
  tscHandleMultivnodeInsert(merged);
  taosArrayDestroy(&statements);
  return;

  _error:
  tscError("send async batch sql obj failed, reason: %s", tstrerror(code));

  // handling the failures.
  for (int i = 0; i < taosArrayGetSize(statements); ++i) {
    SSqlObj* item = *((SSqlObj**)taosArrayGet(statements, i));
    tscReturnsError(item, code);
  }
  taosArrayDestroy(&statements);
}

/**
 * The thread to manage batching timeout.
 */
static void* dispatcherTimeoutCallback(void* arg) {
  SAsyncBulkWriteDispatcher* dispatcher = arg;
  setThreadName("tscBackground");

  while (!atomic_load_8(&dispatcher->shutdown)) {
    int64_t t0 = taosGetTimestampNs();

    atomic_store_8(&dispatcher->exclusive, true);
    SArray* statements = dispatcherPollAll(dispatcher);
    atomic_store_8(&dispatcher->exclusive, false);

    dispatcherExecute(statements);

    int64_t t1 = taosGetTimestampNs();
    int64_t durationMs = (t1 - t0) / 1000000;

    // Similar to scheduleAtFixedRate in Java, if the execution time exceed
    // `timeoutMs` milliseconds, then there will be no sleep.
    if (durationMs < dispatcher->timeoutMs) {
      taosMsleep((int32_t)(dispatcher->timeoutMs - durationMs));
    }
  }
  return NULL;
}

SAsyncBulkWriteDispatcher* createAsyncBulkWriteDispatcher(int32_t batchSize, int32_t timeoutMs) {
  SAsyncBulkWriteDispatcher* dispatcher = calloc(1, sizeof(SAsyncBulkWriteDispatcher));
  if (!dispatcher) {
    return NULL;
  }

  dispatcher->batchSize = batchSize;
  dispatcher->timeoutMs = timeoutMs;

  atomic_store_32(&dispatcher->bufferSize, 0);
  atomic_store_32(&dispatcher->currentSize, 0);
  atomic_store_8(&dispatcher->shutdown, false);
  atomic_store_8(&dispatcher->exclusive, false);

  // init the buffer.
  dispatcher->buffer = tdListNew(sizeof(SSqlObj*));
  if (!dispatcher->buffer) {
    tfree(dispatcher);
    return NULL;
  }

  // init the mutex.
  pthread_mutex_init(&dispatcher->mutex, NULL);

  // init background thread.
  if (pthread_create(&dispatcher->background, NULL, dispatcherTimeoutCallback, dispatcher)) {
    tdListFree(dispatcher->buffer);
    tfree(dispatcher);
    return NULL;
  }

  return dispatcher;
}

void destroyAsyncDispatcher(SAsyncBulkWriteDispatcher* dispatcher) {
  if (dispatcher == NULL) {
    return;
  }
  
  // mark shutdown.
  atomic_store_8(&dispatcher->shutdown, true);
  
  // make sure the timeout thread exit.
  pthread_join(dispatcher->background, NULL);
  
  // poll and send all the statements in the buffer.
  while (atomic_load_32(&dispatcher->bufferSize)) {
    SArray* statements = dispatcherPollAll(dispatcher);
    dispatcherExecute(statements);
  }
  
  // destroy the buffer.
  tdListFree(dispatcher->buffer);

  // destroy the mutex.
  pthread_mutex_destroy(&dispatcher->mutex);

  free(dispatcher);
}
bool tscSupportBulkInsertion(SSqlObj* pSql) {
  if (pSql == NULL || !pSql->enableBatch) {
    return false;
  }

  SSqlCmd*    pCmd = &pSql->cmd;
  SQueryInfo* pQueryInfo = tscGetQueryInfo(pCmd);

  // only support insert statement.
  if (!TSDB_QUERY_HAS_TYPE(pQueryInfo->type, TSDB_QUERY_TYPE_INSERT)) {
    return false;
  }

  SInsertStatementParam* pInsertParam = &pCmd->insertParam;

  // file insert not support.
  if (TSDB_QUERY_HAS_TYPE(pInsertParam->insertType, TSDB_QUERY_TYPE_FILE_INSERT)) {
    return false;
  }

  // only support kv payload.
  if (pInsertParam->payloadType != PAYLOAD_TYPE_KV) {
    return false;
  }

  return true;
}

bool dispatcherTryBatching(SAsyncBulkWriteDispatcher* dispatcher, SSqlObj* pSql) {
  if (atomic_load_8(&dispatcher->shutdown)) {
    return false;
  }

  // the sql object doesn't support bulk insertion.
  if (!tscSupportBulkInsertion(pSql)) {
    return false;
  }

  // the buffer is exclusive.
  if (atomic_load_8(&dispatcher->exclusive)) {
    return false;
  }

  // try to offer pSql to the buffer.
  int32_t currentSize = dispatcherTryOffer(dispatcher, pSql);
  if (currentSize < 0) {
    return false;
  }

  // the buffer is full or reach batch size.
  if (currentSize >= dispatcher->batchSize) {
    SArray* statements = dispatcherPollAll(dispatcher);
    dispatcherExecute(statements);
  }
  return true;
}

/**
 * Destroy the SAsyncBulkWriteDispatcher create by SThreadLocalDispatcher.
 * @param arg 
 */
static void destroyDispatcher(void* arg) {
  SAsyncBulkWriteDispatcher* dispatcher = arg;
  if (!dispatcher) {
    return;
  }
  
  destroyAsyncDispatcher(dispatcher);
}

SThreadLocalDispatcher* createThreadLocalDispatcher(int32_t batchSize, int32_t timeoutMs) {
  SThreadLocalDispatcher* dispatcher = calloc(1, sizeof(SThreadLocalDispatcher));
  if (!dispatcher) {
    return NULL;
  }

  dispatcher->batchSize = batchSize;
  dispatcher->timeoutMs = timeoutMs;

  if (pthread_key_create(&dispatcher->key, destroyDispatcher)) {
    free(dispatcher);
    return NULL;
  }
  return dispatcher;
}

SAsyncBulkWriteDispatcher* dispatcherThreadLocal(SThreadLocalDispatcher* dispatcher) {
  SAsyncBulkWriteDispatcher* value = pthread_getspecific(dispatcher->key);
  if (value) {
    return value;
  }

  value = createAsyncBulkWriteDispatcher(dispatcher->batchSize, dispatcher->timeoutMs);
  if (value) {
    pthread_setspecific(dispatcher->key, value);
    return value;
  }

  return NULL;
}

void destroyThreadLocalDispatcher(SThreadLocalDispatcher* dispatcher) {
  if (dispatcher) {
    pthread_key_delete(dispatcher->key);
    free(dispatcher);
  }
}
