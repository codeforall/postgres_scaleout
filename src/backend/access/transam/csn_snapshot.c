/*-------------------------------------------------------------------------
 *
 * csn_snapshot.c
 *		Support for cross-node snapshot isolation.
 *
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/access/transam/csn_snapshot.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/csn_log.h"
#include "access/csn_snapshot.h"
#include "access/transam.h"
#include "access/twophase.h"
#include "access/xact.h"
#include "portability/instr_time.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/snapmgr.h"
#include "miscadmin.h"

/* Raise a warning if imported snapshot_csn exceeds ours by this value. */
#define SNAP_DESYNC_COMPLAIN (1*NSECS_PER_SEC) /* 1 second */

TransactionId 	 xmin_for_csn = InvalidTransactionId;

/*
 * CSNSnapshotState
 *
 * Do not trust local clocks to be strictly monotonical and save last acquired
 * value so later we can compare next timestamp with it. Accessed through
 * GenerateCSN().
 */
typedef struct
{
	SnapshotCSN		 last_max_csn;		/* Record the max csn till now */
	XidCSN			 last_csn_log_wal;	/* for interval we log the assign csn to wal */
	TransactionId 	 xmin_for_csn; 		/*'xmin_for_csn' for when turn xid-snapshot to csn-snapshot*/
	volatile slock_t lock;
} CSNSnapshotState;

static CSNSnapshotState *csnState;


/*
 * GUC to delay advance of oldestXid for this amount of time. Also determines
 * the size CSNSnapshotXidMap circular buffer.
 */
int csn_snapshot_defer_time;

/*
 * Enables this module.
 */
bool enable_csn_snapshot;

/*
 * CSNSnapshotXidMap
 *
 * To be able to install csn snapshot that points to past we need to keep
 * old versions of tuples and therefore delay advance of oldestXid.  Here we
 * keep track of correspondence between snapshot's snapshot_csn and oldestXid
 * that was set at the time when the snapshot was taken.  Much like the
 * snapshot too old's OldSnapshotControlData does, but with finer granularity
 * to seconds.
 *
 * Different strategies can be employed to hold oldestXid (e.g. we can track
 * oldest csn-based snapshot among cluster nodes and map it oldestXid
 * on each node).
 *
 * On each snapshot acquisition CSNSnapshotMapXmin() is called and stores
 * correspondence between current snapshot_csn and oldestXmin in a sparse way:
 * snapshot_csn is rounded to seconds (and here we use the fact that snapshot_csn
 * is just a timestamp) and oldestXmin is stored in the circular buffer where
 * rounded snapshot_csn acts as an offset from current circular buffer head.
 * Size of the circular buffer is controlled by csn_snapshot_defer_time GUC.
 *
 * When csn snapshot arrives we check that its
 * snapshot_csn is still in our map, otherwise we'll error out with "snapshot too
 * old" message.  If snapshot_csn is successfully mapped to oldestXid we move
 * backend's pgxact->xmin to proc->originalXmin and fill pgxact->xmin to
 * mapped oldestXid.  That way GetOldestXmin() can take into account backends
 * with imported csn snapshot and old tuple versions will be preserved.
 *
 * Also while calculating oldestXmin for our map in presence of imported
 * csn snapshots we should use proc->originalXmin instead of pgxact->xmin
 * that was set during import.  Otherwise, we can create a feedback loop:
 * xmin's of imported csn snapshots were calculated using our map and new
 * entries in map going to be calculated based on that xmin's, and there is
 * a risk to stuck forever with one non-increasing oldestXmin.  All other
 * callers of GetOldestXmin() are using pgxact->xmin so the old tuple versions
 * are preserved.
 */
typedef struct CSNSnapshotXidMap
{
	int				 head;				/* offset of current freshest value */
	int				 size;				/* total size of circular buffer */
	CSN_atomic		 last_csn_seconds;	/* last rounded csn that changed
										 * xmin_by_second[] */
	TransactionId   *xmin_by_second;	/* circular buffer of oldestXmin's */
}
CSNSnapshotXidMap;

static CSNSnapshotXidMap *csnXidMap;


/* Estimate shared memory space needed */
Size
CSNSnapshotShmemSize(void)
{
	Size	size = 0;

	if (enable_csn_snapshot || csn_snapshot_defer_time > 0)
	{
		size += MAXALIGN(sizeof(CSNSnapshotState));
	}

	if (csn_snapshot_defer_time > 0)
	{
		size += sizeof(CSNSnapshotXidMap);
		size += csn_snapshot_defer_time*sizeof(TransactionId);
		size = MAXALIGN(size);
	}

	return size;
}

/* Init shared memory structures */
void
CSNSnapshotShmemInit()
{
	bool found;

	if (enable_csn_snapshot || csn_snapshot_defer_time > 0)
	{
		csnState = ShmemInitStruct("csnState",
								sizeof(CSNSnapshotState),
								&found);
		if (!found)
		{
			csnState->last_max_csn = 0;
			csnState->last_csn_log_wal = 0;
			SpinLockInit(&csnState->lock);
		}
	}

	if (csn_snapshot_defer_time > 0)
	{
		csnXidMap = ShmemInitStruct("csnXidMap",
								   sizeof(CSNSnapshotXidMap),
								   &found);
		if (!found)
		{
			int i;

			pg_atomic_init_u64(&csnXidMap->last_csn_seconds, 0);
			csnXidMap->head = 0;
			csnXidMap->size = csn_snapshot_defer_time;
			csnXidMap->xmin_by_second =
							ShmemAlloc(sizeof(TransactionId)*csnXidMap->size);

			for (i = 0; i < csnXidMap->size; i++)
				csnXidMap->xmin_by_second[i] = InvalidTransactionId;
		}
	}
}

/*
 * CSNSnapshotStartup
 *
 * Set csnXidMap entries to oldestActiveXID during startup.
 */
void
CSNSnapshotStartup(TransactionId oldestActiveXID)
{
	/*
	 * Run only if we have initialized shared memory and csnXidMap
	 * is enabled.
	 */
	if (IsNormalProcessingMode() && csn_snapshot_defer_time > 0)
	{
		int i;

		Assert(TransactionIdIsValid(oldestActiveXID));
		for (i = 0; i < csnXidMap->size; i++)
			csnXidMap->xmin_by_second[i] = oldestActiveXID;
		ProcArraySetCSNSnapshotXmin(oldestActiveXID);
	}
}

/*
 * CSNSnapshotMapXmin
 *
 * Maintain circular buffer of oldestXmins for several seconds in past. This
 * buffer allows to shift oldestXmin in the past when backend is importing
 * CSN snapshot. Otherwise old versions of tuples that were needed for
 * this transaction can be recycled by other processes (vacuum, HOT, etc).
 *
 * Locking here is not trivial. Called upon each snapshot creation after
 * ProcArrayLock is released. Such usage creates several race conditions. It
 * is possible that backend who got csn called CSNSnapshotMapXmin()
 * only after other backends managed to get snapshot and complete
 * CSNSnapshotMapXmin() call, or even committed. This is safe because
 *
 *		* We already hold our xmin in MyPgXact, so our snapshot will not be
 *		  harmed even though ProcArrayLock is released.
 *
 *		* snapshot_csn is always pessmistically rounded up to the next
 *		  second.
 *
 *		* For performance reasons, xmin value for particular second is filled
 *		  only once. Because of that instead of writing to buffer just our
 *		  xmin (which is enough for our snapshot), we bump oldestXmin there --
 *		  it mitigates the possibility of damaging someone else's snapshot by
 *		  writing to the buffer too advanced value in case of slowness of
 *		  another backend who generated csn earlier, but didn't manage to
 *		  insert it before us.
 *
 *		* if CSNSnapshotMapXmin() founds a gap in several seconds between
 *		  current call and latest completed call then it should fill that gap
 *		  with latest known values instead of new one. Otherwise it is
 *		  possible (however highly unlikely) that this gap also happend
 *		  between taking snapshot and call to CSNSnapshotMapXmin() for some
 *		  backend. And we are at risk to fill circullar buffer with
 *		  oldestXmin's that are bigger then they actually were.
 */
void
CSNSnapshotMapXmin(SnapshotCSN snapshot_csn)
{
	int offset, gap, i;
	SnapshotCSN csn_seconds;
	SnapshotCSN last_csn_seconds;
	volatile TransactionId oldest_deferred_xmin;
	TransactionId current_oldest_xmin, previous_oldest_xmin;

	/* Callers should check config values */
	Assert(csn_snapshot_defer_time > 0);
	Assert(csnXidMap != NULL);
	/*
	 * Round up snapshot_csn to the next second -- pessimistically and safely.
	 */
	csn_seconds = (snapshot_csn / NSECS_PER_SEC + 1);

	/*
	 * Fast-path check. Avoid taking exclusive CSNSnapshotXidMapLock lock
	 * if oldestXid was already written to xmin_by_second[] for this rounded
	 * snapshot_csn.
	 */
	if (pg_atomic_read_u64(&csnXidMap->last_csn_seconds) >= csn_seconds)
		return;

	/* Ok, we have new entry (or entries) */
	LWLockAcquire(CSNSnapshotXidMapLock, LW_EXCLUSIVE);

	/* Re-check last_csn_seconds under lock */
	last_csn_seconds = pg_atomic_read_u64(&csnXidMap->last_csn_seconds);
	if (last_csn_seconds >= csn_seconds)
	{
		LWLockRelease(CSNSnapshotXidMapLock);
		return;
	}
	pg_atomic_write_u64(&csnXidMap->last_csn_seconds, csn_seconds);

	/*
	 * Count oldest_xmin.
	 *
	 * It was possible to calculate oldest_xmin during corresponding snapshot
	 * creation, but GetSnapshotData() intentionally reads only PgXact, but not
	 * PgProc. And we need info about originalXmin (see comment to csnXidMap)
	 * which is stored in PgProc because of threats in comments around PgXact
	 * about extending it with new fields. So just calculate oldest_xmin again,
	 * that anyway happens quite rarely.
	 */
	current_oldest_xmin = GetOldestTransactionIdConsideredRunning();

	previous_oldest_xmin = csnXidMap->xmin_by_second[csnXidMap->head];

	Assert(TransactionIdIsNormal(current_oldest_xmin));
	Assert(TransactionIdIsNormal(previous_oldest_xmin));

	gap = csn_seconds - last_csn_seconds;
	offset = csn_seconds % csnXidMap->size;

	/* Sanity check before we update head and gap */
	Assert( gap >= 1 );
	Assert( (csnXidMap->head + gap) % csnXidMap->size == offset );

	gap = gap > csnXidMap->size ? csnXidMap->size : gap;
	csnXidMap->head = offset;

	/* Fill new entry with current_oldest_xmin */
	csnXidMap->xmin_by_second[offset] = current_oldest_xmin;

	/*
	 * If we have gap then fill it with previous_oldest_xmin for reasons
	 * outlined in comment above this function.
	 */
	for (i = 1; i < gap; i++)
	{
		offset = (offset + csnXidMap->size - 1) % csnXidMap->size;
		csnXidMap->xmin_by_second[offset] = previous_oldest_xmin;
	}

	oldest_deferred_xmin =
		csnXidMap->xmin_by_second[ (csnXidMap->head + 1) % csnXidMap->size ];

	LWLockRelease(CSNSnapshotXidMapLock);

	/*
	 * Advance procArray->csn_snapshot_xmin after we released
	 * CSNSnapshotXidMapLock. Since we gather not xmin but oldestXmin, it
	 * never goes backwards regardless of how slow we can do that.
	 */
	Assert(TransactionIdFollowsOrEquals(oldest_deferred_xmin,
										ProcArrayGetCSNSnapshotXmin()));
	ProcArraySetCSNSnapshotXmin(oldest_deferred_xmin);
}


/*
 * CSNSnapshotToXmin
 *
 * Get oldestXmin that took place when snapshot_csn was taken.
 */
TransactionId
CSNSnapshotToXmin(SnapshotCSN snapshot_csn)
{
	TransactionId xmin;
	SnapshotCSN csn_seconds;
	volatile SnapshotCSN last_csn_seconds;

	/* Callers should check config values */
	Assert(csn_snapshot_defer_time > 0);
	Assert(csnXidMap != NULL);

	/* Round down to get conservative estimates */
	csn_seconds = (snapshot_csn / NSECS_PER_SEC);

	LWLockAcquire(CSNSnapshotXidMapLock, LW_SHARED);
	last_csn_seconds = pg_atomic_read_u64(&csnXidMap->last_csn_seconds);
	if (csn_seconds > last_csn_seconds)
	{
		/* we don't have entry for this snapshot_csn yet, return latest known */
		xmin = csnXidMap->xmin_by_second[csnXidMap->head];
	}
	else if (last_csn_seconds - csn_seconds < csnXidMap->size)
	{
		/* we are good, retrieve value from our map */
		Assert(last_csn_seconds % csnXidMap->size == csnXidMap->head);
		xmin = csnXidMap->xmin_by_second[csn_seconds % csnXidMap->size];
	}
	else
	{
		/* requested snapshot_csn is too old, let caller know */
		xmin = InvalidTransactionId;
	}
	LWLockRelease(CSNSnapshotXidMapLock);

	return xmin;
}

/*
 * GenerateCSN
 *
 * Generate SnapshotCSN which is actually a local time. Also we are forcing
 * this time to be always increasing. Since now it is not uncommon to have
 * millions of read transactions per second we are trying to use nanoseconds
 * if such time resolution is available.
 */
SnapshotCSN
GenerateCSN(bool locked)
{
	instr_time	current_time;
	SnapshotCSN	csn;

	Assert(enable_csn_snapshot || csn_snapshot_defer_time > 0);

	/*
	 * TODO: create some macro that add small random shift to current time.
	 */
	INSTR_TIME_SET_CURRENT(current_time);
	csn = (SnapshotCSN) INSTR_TIME_GET_NANOSEC(current_time);

	/* TODO: change to atomics? */
	if (!locked)
		SpinLockAcquire(&csnState->lock);

	if (csn <= csnState->last_max_csn)
		csn = ++csnState->last_max_csn;
	else
		csnState->last_max_csn = csn;

	WriteAssignCSNXlogRec(csn);

	if (!locked)
		SpinLockRelease(&csnState->lock);

	return csn;
}

/*
 * TransactionIdGetXidCSN
 *
 * Get XidCSN for specified TransactionId taking care about special xids,
 * xids beyond TransactionXmin and InDoubt states.
 */
XidCSN
TransactionIdGetXidCSN(TransactionId xid)
{
	XidCSN 			 xid_csn;

	Assert(enable_csn_snapshot);

	/* Handle permanent TransactionId's for which we don't have mapping */
	if (!TransactionIdIsNormal(xid))
	{
		if (xid == InvalidTransactionId)
			return AbortedXidCSN;
		if (xid == FrozenTransactionId || xid == BootstrapTransactionId)
			return FrozenXidCSN;
		Assert(false); /* Should not happend */
	}

	/*
	 * If we just switch a xid-snapsot to a csn_snapshot, we should handle a start
	 * xid for csn basse check. Just in case we have prepared transaction which
	 * hold the TransactionXmin but without CSN.
	 */
	if(InvalidTransactionId == xmin_for_csn)
	{
		SpinLockAcquire(&csnState->lock);
		if(InvalidTransactionId != csnState->xmin_for_csn)
			xmin_for_csn = csnState->xmin_for_csn;
		else
			xmin_for_csn = FrozenTransactionId;

		SpinLockRelease(&csnState->lock);
	}

	if (InvalidTransactionId != xmin_for_csn && FrozenTransactionId != xmin_for_csn &&
							 TransactionIdPrecedes(xmin_for_csn, TransactionXmin))
	{
		xmin_for_csn = TransactionXmin;
	}

	/*
	 * For xids which less then TransactionXmin CSNLog can be already
	 * trimmed but we know that such transaction is definetly not concurrently
	 * running according to any snapshot including timetravel ones. Callers
	 * should check TransactionDidCommit after.
	 */
	if (TransactionIdPrecedes(xid, xmin_for_csn))
		return FrozenXidCSN;

	/* Read XidCSN from SLRU */
	xid_csn = CSNLogGetCSNByXid(xid);

	/*
	 * If we faced InDoubt state then transaction is beeing committed and we
	 * should wait until XidCSN will be assigned so that visibility check
	 * could decide whether tuple is in snapshot. See also comments in
	 * CSNSnapshotPrecommit().
	 */
	if (XidCSNIsInDoubt(xid_csn))
	{
		XactLockTableWait(xid, NULL, NULL, XLTW_None);
		xid_csn = CSNLogGetCSNByXid(xid);
		Assert(XidCSNIsNormal(xid_csn) ||
				XidCSNIsAborted(xid_csn));
	}

	Assert(XidCSNIsNormal(xid_csn) ||
			XidCSNIsInProgress(xid_csn) ||
			XidCSNIsAborted(xid_csn));

	return xid_csn;
}

/*
 * XidInvisibleInCSNSnapshot
 *
 * Version of XidInMVCCSnapshot for transactions. For non-imported
 * csn snapshots this should give same results as XidInLocalMVCCSnapshot
 * (except that aborts will be shown as invisible without going to clog) and to
 * ensure such behaviour XidInMVCCSnapshot is coated with asserts that checks
 * identicalness of XidInvisibleInCSNSnapshot/XidInLocalMVCCSnapshot in
 * case of ordinary snapshot.
 */
bool
XidInvisibleInCSNSnapshot(TransactionId xid, Snapshot snapshot)
{
	XidCSN csn;

	Assert(enable_csn_snapshot);

	csn = TransactionIdGetXidCSN(xid);

	if (XidCSNIsNormal(csn))
	{
		if (csn < snapshot->snapshot_csn)
			return false;
		else
			return true;
	}
	else if (XidCSNIsFrozen(csn))
	{
		/* It is bootstrap or frozen transaction */
		return false;
	}
	else
	{
		/* It is aborted or in-progress */
		Assert(XidCSNIsAborted(csn) || XidCSNIsInProgress(csn));
		if (XidCSNIsAborted(csn))
			Assert(TransactionIdDidAbort(xid));
		return true;
	}
}


/*****************************************************************************
 * Functions to handle transactions commit.
 *
 * For local transactions CSNSnapshotPrecommit sets InDoubt state before
 * ProcArrayEndTransaction is called and transaction data potetntially becomes
 * visible to other backends. ProcArrayEndTransaction (or ProcArrayRemove in
 * twophase case) then acquires xid_csn under ProcArray lock and stores it
 * in proc->assignedXidCsn. It's important that xid_csn for commit is
 * generated under ProcArray lock, otherwise snapshots won't
 * be equivalent. Consequent call to CSNSnapshotCommit will write
 * proc->assignedXidCsn to CSNLog.
 *
 *
 * CSNSnapshotAbort is slightly different comparing to commit because abort
 * can skip InDoubt phase and can be called for transaction subtree.
 *****************************************************************************/


/*
 * CSNSnapshotAbort
 *
 * Abort transaction in CsnLog. We can skip InDoubt state for aborts
 * since no concurrent transactions allowed to see aborted data anyway.
 */
void
CSNSnapshotAbort(PGPROC *proc, TransactionId xid,
					int nsubxids, TransactionId *subxids)
{
	if (!enable_csn_snapshot)
		return;

	CSNLogSetCSN(xid, nsubxids, subxids, AbortedXidCSN, true);

	/*
	 * Clean assignedXidCsn anyway, as it was possibly set in
	 * XidSnapshotAssignCsnCurrent.
	 */
	pg_atomic_write_u64(&proc->assignedXidCsn, InProgressXidCSN);
}

/*
 * CSNSnapshotPrecommit
 *
 * Set InDoubt status for local transaction that we are going to commit.
 * This step is needed to achieve consistency between local snapshots and
 * csn-based snapshots. We don't hold ProcArray lock while writing
 * csn for transaction in SLRU but instead we set InDoubt status before
 * transaction is deleted from ProcArray so the readers who will read csn
 * in the gap between ProcArray removal and XidCSN assignment can wait
 * until XidCSN is finally assigned. See also TransactionIdGetXidCSN().
 *
 * This should be called only from parallel group leader before backend is
 * deleted from ProcArray.
 */
void
CSNSnapshotPrecommit(PGPROC *proc, TransactionId xid,
					int nsubxids, TransactionId *subxids)
{
	XidCSN oldassignedXidCsn = InProgressXidCSN;
	bool in_progress;

	if (!enable_csn_snapshot)
		return;

	/* Set InDoubt status if it is local transaction */
	in_progress = pg_atomic_compare_exchange_u64(&proc->assignedXidCsn,
												 &oldassignedXidCsn,
												 InDoubtXidCSN);
	if (in_progress)
	{
		Assert(XidCSNIsInProgress(oldassignedXidCsn));
		CSNLogSetCSN(xid, nsubxids,
						   subxids, InDoubtXidCSN, true);
	}
	else
	{
		/* Otherwise we should have valid XidCSN by this time */
		Assert(XidCSNIsNormal(oldassignedXidCsn));
		Assert(XidCSNIsInDoubt(CSNLogGetCSNByXid(xid)));
	}
}

/*
 * CSNSnapshotCommit
 *
 * Write XidCSN that were acquired earlier to CsnLog. Should be
 * preceded by CSNSnapshotPrecommit() so readers can wait until we finally
 * finished writing to SLRU.
 *
 * Should be called after ProcArrayEndTransaction, but before releasing
 * transaction locks, so that TransactionIdGetXidCSN can wait on this
 * lock for XidCSN.
 */
void
CSNSnapshotCommit(PGPROC *proc, TransactionId xid,
					int nsubxids, TransactionId *subxids)
{
	volatile XidCSN assigned_xid_csn;

	if (!enable_csn_snapshot)
		return;

	if (!TransactionIdIsValid(xid))
	{
		assigned_xid_csn = pg_atomic_read_u64(&proc->assignedXidCsn);
		Assert(XidCSNIsInProgress(assigned_xid_csn));
		return;
	}

	/* Finally write resulting XidCSN in SLRU */
	assigned_xid_csn = pg_atomic_read_u64(&proc->assignedXidCsn);
	Assert(XidCSNIsNormal(assigned_xid_csn));
	CSNLogSetCSN(xid, nsubxids,
						   subxids, assigned_xid_csn, true);

	/* Reset for next transaction */
	pg_atomic_write_u64(&proc->assignedXidCsn, InProgressXidCSN);
}

void
set_last_max_csn(XidCSN xidcsn)
{
	csnState->last_max_csn = xidcsn;
}

void
set_last_log_wal_csn(XidCSN xidcsn)
{
	csnState->last_csn_log_wal = xidcsn;
}

XidCSN
get_last_log_wal_csn(void)
{
	XidCSN			 last_csn_log_wal;

	last_csn_log_wal = csnState->last_csn_log_wal;

	return last_csn_log_wal;
}

/*
 * 'xmin_for_csn' for when turn xid-snapshot to csn-snapshot
 */
void
set_xmin_for_csn(void)
{
	csnState->xmin_for_csn = XidFromFullTransactionId(ShmemVariableCache->nextXid);
}
