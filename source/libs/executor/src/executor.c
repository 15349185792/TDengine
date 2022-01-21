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

#include "executor.h"
#include "tq.h"
#include "executorimpl.h"
#include "planner.h"

static int32_t doSetStreamBlock(SOperatorInfo* pOperator, void* input, uint64_t reqId) {
  ASSERT(pOperator != NULL);
  if (pOperator->operatorType != OP_StreamScan) {
    if (pOperator->numOfDownstream == 0) {
      qError("failed to find stream scan operator to set the input data block, reqId:0x%" PRIx64, reqId);
      return TSDB_CODE_QRY_APP_ERROR;
    }

    if (pOperator->numOfDownstream > 1) {  // not handle this in join query
      qError("join not supported for stream block scan, reqId:0x%" PRIx64, reqId);
      return TSDB_CODE_QRY_APP_ERROR;
    }

    return doSetStreamBlock(pOperator->pDownstream[0], input, reqId);
  } else {
    SStreamBlockScanInfo* pInfo = pOperator->info;
    tqReadHandleSetMsg(pInfo->readerHandle, input, 0);
    return TSDB_CODE_SUCCESS;
  }
}

int32_t qSetStreamInput(qTaskInfo_t tinfo, void* input) {
  if (tinfo == NULL) {
    return TSDB_CODE_QRY_APP_ERROR;
  }

  if (input == NULL) {
    return TSDB_CODE_SUCCESS;
  }

  SExecTaskInfo* pTaskInfo = (SExecTaskInfo*) tinfo;

  int32_t code = doSetStreamBlock(pTaskInfo->pRoot, input, GET_TASKID(pTaskInfo));
  if (code != TSDB_CODE_SUCCESS) {
    qError("failed to set the stream block data, reqId:0x%"PRIx64, GET_TASKID(pTaskInfo));
  } else {
    qDebug("set the stream block successfully, reqId:0x%"PRIx64, GET_TASKID(pTaskInfo));
  }

  return code;
}

qTaskInfo_t qCreateStreamExecTaskInfo(SSubQueryMsg* pMsg, void* streamReadHandle) {
  if (pMsg == NULL || streamReadHandle == NULL) {
    return NULL;
  }

  // print those info into log
  pMsg->sId = be64toh(pMsg->sId);
  pMsg->queryId = be64toh(pMsg->queryId);
  pMsg->taskId = be64toh(pMsg->taskId);
  pMsg->contentLen = ntohl(pMsg->contentLen);

  struct SSubplan* plan = NULL;
  int32_t          code = qStringToSubplan(pMsg->msg, &plan);
  if (code != TSDB_CODE_SUCCESS) {
    terrno = code;
    return NULL;
  }

  qTaskInfo_t pTaskInfo = NULL;
  code = qCreateExecTask(streamReadHandle, 0, plan, &pTaskInfo, NULL);
  if (code != TSDB_CODE_SUCCESS) {
    // TODO: destroy SSubplan & pTaskInfo
    terrno = code;
    return NULL;
  }

  return pTaskInfo;
}
