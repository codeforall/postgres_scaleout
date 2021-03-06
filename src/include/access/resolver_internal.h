/*-------------------------------------------------------------------------
 *
 * resolver_internal.h
 *	  Internal headers shared by fdwxact resolvers.
 *
 * Portions Copyright (c) 2020, PostgreSQL Global Development Group
 *
 * src/include/access/resolver_internal.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef RESOLVER_INTERNAL_H
#define RESOLVER_INTERNAL_H

#include "storage/latch.h"
#include "storage/shmem.h"
#include "storage/spin.h"
#include "utils/timestamp.h"

/*
 * Each foreign transaction resolver has a FdwXactResolver struct in
 * shared memory.  This struct is protected by FdwXactResolverLaunchLock.
 */
typedef struct FdwXactResolver
{
	pid_t		pid;			/* this resolver's PID, or 0 if not active */
	Oid			dbid;			/* database oid */

	/* Indicates if this slot is used of free */
	bool		in_use;

	/* Protect shared variables shown above */
	slock_t		mutex;

	/*
	 * Pointer to the resolver's patch. Used by backends to wake up this
	 * resolver when it has work to do. NULL if the resolver isn't active.
	 */
	Latch	   *latch;
} FdwXactResolver;

/* There is one FdwXactRslvCtlData struct for the whole database cluster */
typedef struct FdwXactRslvCtlData
{
	/* Foreign transaction resolution queue. Protected by FdwXactLock */
	SHM_QUEUE	fdwxact_queue;

	/* Supervisor process and latch */
	pid_t		launcher_pid;
	Latch	   *launcher_latch;

	FdwXactResolver resolvers[FLEXIBLE_ARRAY_MEMBER];
} FdwXactRslvCtlData;
#define SizeOfFdwXactRslvCtlData \
	(offsetof(FdwXactRslvCtlData, resolvers) + sizeof(FdwXactResolver))

extern FdwXactRslvCtlData *FdwXactRslvCtl;

extern FdwXactResolver *MyFdwXactResolver;
extern FdwXactRslvCtlData *FdwXactRslvCtl;

#endif							/* RESOLVER_INTERNAL_H */
