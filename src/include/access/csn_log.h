/*
 * csn_log.h
 *
 * Commit-Sequence-Number log.
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/csn_log.h
 */
#ifndef CSNLOG_H
#define CSNLOG_H

#include "access/xlog.h"
#include "utils/snapshot.h"
#include "storage/sync.h"

extern void CSNLogSetCSN(TransactionId xid, int nsubxids,
							   TransactionId *subxids, XidCSN csn);
extern XidCSN CSNLogGetCSNByXid(TransactionId xid);

extern Size CSNLogShmemSize(void);
extern void CSNLogShmemInit(void);
extern void BootStrapCSNLog(void);
extern void StartupCSNLog(TransactionId oldestActiveXID);
extern void CheckPointCSNLog(void);
extern void ExtendCSNLog(TransactionId newestXact);
extern void TruncateCSNLog(TransactionId oldestXact);
extern int csnlogsyncfiletag(const FileTag *ftag, char *path);

#endif   /* CSNLOG_H */