/*
 * Copyright (c) 2017 Red Hat, Inc.
 *
 * This file is licensed to you under your choice of the GNU Lesser
 * General Public License, version 2.1 or any later version (LGPLv2.1 or
 * later), or the Apache License 2.0.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <pthread.h>
#include <errno.h>
#include <unistd.h>

#include "libtcmu_log.h"
#include "libtcmu_common.h"
#include "tcmu-runner.h"
#include "tcmur_device.h"
#include "tcmur_cmd_handler.h"
#include "tcmur_work.h"
#include "tcmu_runner_priv.h"
#include "target.h"

bool tcmu_dev_in_recovery(struct tcmu_device *dev)
{
	struct tcmur_device *rdev = tcmu_dev_get_private(dev);
	int in_recov = false;

	pthread_mutex_lock(&rdev->state_lock);
	if (rdev->flags & TCMUR_DEV_FLAG_IN_RECOVERY)
		in_recov = true;
	pthread_mutex_unlock(&rdev->state_lock);
	return in_recov;
}

/*
 * TCMUR_DEV_FLAG_IN_RECOVERY must be set before calling
 */
int __tcmu_reopen_dev(struct tcmu_device *dev, int retries)
{
	struct tcmur_device *rdev = tcmu_dev_get_private(dev);
	struct tcmur_handler *rhandler = tcmu_get_runner_handler(dev);
	int ret, attempt = 0;
	bool needs_close = false;

	pthread_mutex_lock(&rdev->state_lock);
	if (rdev->flags & TCMUR_DEV_FLAG_STOPPING) {
		ret = 0;
		goto done;
	}
	pthread_mutex_unlock(&rdev->state_lock);

	/*
	 * There are no SCSI commands running but there may be
	 * async lock requests in progress that might be accessing
	 * the device.
	 */
	tcmur_flush_work(rdev->event_work);

	pthread_mutex_lock(&rdev->state_lock);
	if (rdev->flags & TCMUR_DEV_FLAG_IS_OPEN)
		needs_close = true;
	rdev->flags &= ~TCMUR_DEV_FLAG_IS_OPEN;
	pthread_mutex_unlock(&rdev->state_lock);

	if (pthread_self() != rdev->cmdproc_thread)
		/*
		 * The cmdproc thread could be starting to execute a new IO.
		 * Make sure sync cmd handler callbacks for cmds like INQUIRY
		 * are completed.
		 */
		tcmu_dev_flush_ring(dev);

	tcmu_dev_dbg(dev, "Waiting for outstanding commands to complete\n");
	ret = aio_wait_for_empty_queue(rdev);
	if (ret) {
		pthread_mutex_lock(&rdev->state_lock);
		goto done;
	}

	if (needs_close) {
		tcmu_dev_dbg(dev, "Closing device.\n");
		rhandler->close(dev);
	}

	pthread_mutex_lock(&rdev->state_lock);
	ret = -EIO;
	while (ret != 0 && !(rdev->flags & TCMUR_DEV_FLAG_STOPPING) &&
	       (retries < 0 || attempt <= retries)) {
		pthread_mutex_unlock(&rdev->state_lock);

		tcmu_dev_dbg(dev, "Opening device. Attempt %d\n", attempt);
		ret = rhandler->open(dev, true);
		if (ret) {
			/* Avoid busy loop ? */
			sleep(1);
		}

		pthread_mutex_lock(&rdev->state_lock);
		if (!ret) {
			rdev->flags |= TCMUR_DEV_FLAG_IS_OPEN;
			rdev->lock_lost = false;
		}
		attempt++;
	}

done:
	rdev->flags &= ~TCMUR_DEV_FLAG_IN_RECOVERY;
	pthread_mutex_unlock(&rdev->state_lock);

	return ret;
}

/*
 * tcmu_reopen_dev - close and open device.
 * @dev: device to reopen
 * @retries: number of times to retry open() call. -1 indicates infinite.
 */
int tcmu_reopen_dev(struct tcmu_device *dev, int retries)
{
	struct tcmur_device *rdev = tcmu_dev_get_private(dev);

	pthread_mutex_lock(&rdev->state_lock);
	if (rdev->flags & TCMUR_DEV_FLAG_IN_RECOVERY) {
		pthread_mutex_unlock(&rdev->state_lock);
		return -EBUSY;
	}
	rdev->flags |= TCMUR_DEV_FLAG_IN_RECOVERY;
	pthread_mutex_unlock(&rdev->state_lock);

	return __tcmu_reopen_dev(dev, retries);
}

void tcmu_cancel_recovery(struct tcmu_device *dev)
{
	struct tcmur_device *rdev = tcmu_dev_get_private(dev);

	/*
	 * Only file and qcow can be canceled in their open/close calls, but
	 * they do not support recovery, so wait here for rbd/glfs type of
	 * handlers to fail/complete normally to avoid a segfault.
	 */
	tcmu_dev_dbg(dev, "Waiting on recovery thread\n");
	pthread_mutex_lock(&rdev->state_lock);
	while (rdev->flags & TCMUR_DEV_FLAG_IN_RECOVERY) {
		pthread_mutex_unlock(&rdev->state_lock);
		sleep(1);
		pthread_mutex_lock(&rdev->state_lock);
	}
	pthread_mutex_unlock(&rdev->state_lock);
	tcmu_dev_dbg(dev, "Recovery thread wait done\n");
}

static void __tcmu_report_event(void *data)
{
	struct tcmu_device *dev = data;
	struct tcmur_device *rdev = tcmu_dev_get_private(dev);
	struct tcmur_handler *rhandler = tcmu_get_runner_handler(dev);
	int ret;

	/*
	 * For cmd timeouts and unbalanced systems we will get a burst so wait
	 * a second to batch up the updates.
	 */
	sleep(1);

	pthread_mutex_lock(&rdev->state_lock);
	ret = rhandler->report_event(dev);
	if (ret)
		tcmu_dev_err(dev, "Could not report events. Error %d.\n", ret);
	pthread_mutex_unlock(&rdev->state_lock);
}

static void tcmu_report_event(struct tcmu_device *dev)
{
	struct tcmur_handler *rhandler = tcmu_get_runner_handler(dev);
	struct tcmur_device *rdev = tcmu_dev_get_private(dev);
	int ret;

	if (!rhandler->report_event)
		return;

	ret = tcmur_run_work(rdev->event_work, dev, __tcmu_report_event);
	if (!ret)
		return;

	if (ret != -EBUSY)
		tcmu_dev_err(dev, "Could not execute event work. Error %d", ret);
}

static bool __tcmu_notify_conn_lost(struct tcmu_device *dev)
{
	struct tcmur_device *rdev = tcmu_dev_get_private(dev);

	/*
	 * Although there are 2 checks for STOPPING in __tcmu_reopen_dev
	 * which is called a little later by the recovery thread, STOPPING
	 * checking is still needed here.
	 *
	 * In device removal, tcmu_get_alua_grps will never get access to
	 * configfs dir resource which is holded by kernel in configfs_rmdir,
	 * thus tcmulib_cmd->done() will never get a chance to clear
	 * tracked_aio_ops. This will cause a deadlock in dev_removed
	 * which is polling tracked_aio_ops.
	 */
	if ((rdev->flags & TCMUR_DEV_FLAG_STOPPING) ||
		(rdev->flags & TCMUR_DEV_FLAG_IN_RECOVERY))
		return false;

	tcmu_dev_err(dev, "Handler connection lost (lock state %d)\n",
		     rdev->lock_state);

	if (!tcmu_add_dev_to_recovery_list(dev)) {
		rdev->flags |= TCMUR_DEV_FLAG_IN_RECOVERY;
		rdev->conn_lost_cnt++;
		return true;
	}

	return false;
}

/**
 * tcmu_notify_conn_lost - notify runner the device instace has lost its
 *			   connection to its backend storage.
 * @dev: device that has lost its connection
 *
 * Handlers should call this function when they detect they cannot reach their
 * backend storage/medium/cache, so new commands will not be queued until
 * the device has been reopened.
 */
void tcmu_notify_conn_lost(struct tcmu_device *dev)
{
	struct tcmur_device *rdev = tcmu_dev_get_private(dev);
	bool report;

	pthread_mutex_lock(&rdev->state_lock);
	report =__tcmu_notify_conn_lost(dev);
	pthread_mutex_unlock(&rdev->state_lock);

	if (report)
		tcmu_report_event(dev);
}

static void __tcmu_notify_lock_lost(struct tcmu_device *dev)
{
	struct tcmur_device *rdev = tcmu_dev_get_private(dev);

	rdev->lock_lost = true;
	rdev->lock_state = TCMUR_DEV_LOCK_UNLOCKED;
	rdev->lock_lost_cnt++;

	tcmu_report_event(dev);
}

/**
 * tcmu_notify_lock_lost - notify runner the device instance has lost the lock
 * @dev: device that has lost the lock
 *
 * Handlers should call this function when they detect they have lost
 * the lock, so runner can re-acquire. It must be called before completing
 * a command that had caused the failure.
 */
void tcmu_notify_lock_lost(struct tcmu_device *dev)
{
	struct tcmur_device *rdev = tcmu_dev_get_private(dev);

	pthread_mutex_lock(&rdev->state_lock);
	tcmu_dev_warn(dev, "Async lock drop. Old state %d\n", rdev->lock_state);
	/*
	 * We could be getting stale IO completions. If we are trying to
	 * reaquire the lock do not change state.
	 */
	if (rdev->lock_state != TCMUR_DEV_LOCK_WRITE_LOCKING) {
		__tcmu_notify_lock_lost(dev);
	}
	pthread_mutex_unlock(&rdev->state_lock);
}

void tcmu_release_dev_lock(struct tcmu_device *dev)
{
	struct tcmur_handler *rhandler = tcmu_get_runner_handler(dev);
	struct tcmur_device *rdev = tcmu_dev_get_private(dev);
	int ret;

	pthread_mutex_lock(&rdev->state_lock);
	if (rdev->lock_state != TCMUR_DEV_LOCK_WRITE_LOCKED) {
		pthread_mutex_unlock(&rdev->state_lock);
		return;
	}

	if (!(rdev->flags & TCMUR_DEV_FLAG_IS_OPEN)) {
		tcmu_dev_dbg(dev, "Device is closed so unlock is not needed\n");
		rdev->lock_state = TCMUR_DEV_LOCK_UNLOCKED;
		pthread_mutex_unlock(&rdev->state_lock);
		return;
	}

	pthread_mutex_unlock(&rdev->state_lock);

	ret = rhandler->unlock(dev);
	if (ret != TCMU_STS_OK)
		tcmu_dev_warn(dev, "Lock not cleanly released. Ret %d.\n",
			      ret);
	/*
	 * If we don't have a clean unlock we still report success and set
	 * to unlocked to prevent new IO from executing in case the lock
	 * is in a state where it cannot be fenced.
	 */
	pthread_mutex_lock(&rdev->state_lock);
	rdev->lock_state = TCMUR_DEV_LOCK_UNLOCKED;
	pthread_mutex_unlock(&rdev->state_lock);
}

int tcmu_get_lock_tag(struct tcmu_device *dev, uint16_t *tag)
{
	struct tcmur_handler *rhandler = tcmu_get_runner_handler(dev);
	struct tcmur_device *rdev = tcmu_dev_get_private(dev);
	int retry = 0, ret;

	if (rdev->failover_type != TCMUR_DEV_FAILOVER_EXPLICIT)
		return 0;

	pthread_mutex_lock(&rdev->state_lock);
	if (!(rdev->flags & TCMUR_DEV_FLAG_IS_OPEN)) {
		/*
		 * Return tmp error until the recovery thread is able to
		 * start up.
		 */
		pthread_mutex_unlock(&rdev->state_lock);
		return TCMU_STS_BUSY;
	}
	pthread_mutex_unlock(&rdev->state_lock);

retry:
	ret = rhandler->get_lock_tag(dev, tag);
	tcmu_dev_dbg(dev, "Got rc %d tag %hu\n", ret, *tag);

	switch (ret) {
	case TCMU_STS_OK:
		break;
	case TCMU_STS_NO_LOCK_HOLDERS:
		/* No lock holder yet */
		break;
	case TCMU_STS_FENCED:
		/*
		 * This is safe without blocking/flushing because it
		 * is called from the main IO thread and will wait for
		 * commands started before it via the aio wait call.
		 */
		tcmu_dev_dbg(dev, "Could not access dev. Try reopen.\n");
		ret = tcmu_reopen_dev(dev, 0);
		if (!ret && retry < 1) {
			retry++;
			goto retry;
		}
		/* fallthrough */
	case TCMU_STS_TIMEOUT:
	default:
		tcmu_dev_dbg(dev, "Could not reach device to get locker id\n");
		/*
		 * In spc4r37 and newer
		 * "5.15.2.7 Target port asymmetric access state reporting"
		 * states that the initiator should consider the info
		 * returned through our enabled port current for that
		 * enabled port. If a RTPG sent through another port
		 * returns different info, then the info for the enabled
		 * port returned through the enabled port should be
		 * considered current.
		 *
		 * ESX though assumes the all port info in a RTPG to be
		 * current so we drop the session here to prevent sending
		 * inconsistent info. We probably want to do this regardless
		 * of ESX, because that value is returned when the handler
		 * cannot connect to the cluster so all requests are
		 * going to fail.
		 */
		tcmu_notify_conn_lost(dev);
		/*
		 * To try and not return inconsistent info and not look
		 * like a hard device error, fail the command so it is
		 * retried and the retry will be handled like other commands
		 * during session level recovery.
		 */
		return TCMU_STS_BUSY;
	}

	return ret;
}

int tcmu_acquire_dev_lock(struct tcmu_device *dev, uint16_t tag)
{
	struct tcmur_handler *rhandler = tcmu_get_runner_handler(dev);
	struct tcmur_device *rdev = tcmu_dev_get_private(dev);
	int retries = 0, ret = TCMU_STS_HW_ERR;
	bool reopen;

	tcmu_dev_dbg(dev, "Waiting for outstanding commands to complete\n");
	if (aio_wait_for_empty_queue(rdev)) {
		tcmu_dev_err(dev, "Not able to flush queue before taking lock.\n");
		goto done;
	}

	/*
	 * Since we are here the lock state must be one of:
	 * for implicit:
	 *    TCMUR_DEV_LOCK_READ_LOCKING
	 *    TCMUR_DEV_LOCK_WRITE_LOCKING
	 *
	 * for explicit:
	 *    TCMUR_DEV_LOCK_UNLOCKED
	 *    TCMUR_DEV_LOCK_UNKNOWN
	 */

	reopen = false;
	pthread_mutex_lock(&rdev->state_lock);
	if (rdev->lock_lost || !(rdev->flags & TCMUR_DEV_FLAG_IS_OPEN))
		reopen = true;
	pthread_mutex_unlock(&rdev->state_lock);

retry:
	tcmu_dev_dbg(dev, "lock call state %d retries %d. tag %hu reopen %d\n",
		     rdev->lock_state, retries, tag, reopen);

	if (reopen) {
		tcmu_dev_dbg(dev, "Try to reopen device. %d\n", retries);
		ret = tcmu_reopen_dev(dev, 0);
		if (ret) {
			tcmu_dev_err(dev, "Could not reopen device while taking lock. Err %d.\n",
				     ret);
			ret = TCMU_STS_TIMEOUT;
			goto drop_conn;
		}
	}

	pthread_mutex_lock(&rdev->state_lock);
	if (rdev->lock_state == TCMUR_DEV_LOCK_READ_LOCKING) {
		pthread_mutex_unlock(&rdev->state_lock);
		ret = TCMU_STS_OK;
		goto done;
	}
	pthread_mutex_unlock(&rdev->state_lock);

	ret = rhandler->lock(dev, tag);
	if (ret == TCMU_STS_FENCED) {
		if (retries < 1) {
			reopen = true;
			retries++;
			goto retry;
		}
		/*
		 * If we can't unfence ourself the lock is probably bouncing
		 * due to path initialization. Allow the other node to hold the
		 * lock for now, and the initiator will drive retries.
		 */
	}

drop_conn:
	if (ret == TCMU_STS_TIMEOUT) {
		tcmu_dev_dbg(dev, "Fail handler device connection.\n");
		tcmu_notify_conn_lost(dev);
	}

done:
	/* Block and flush stale IO from the kernel device and ring. */
	tcmu_cfgfs_dev_exec_action(dev, "block_dev", 1);
	/*
	 * Handle race where cmd could be in tcmur_generic_handle_cmd before
	 * the aio handler. For explicit ALUA, we execute the lock call from
	 * the main io processing thread, so we only flush here for implicit.
	 */
	if (pthread_self() != rdev->cmdproc_thread)
		tcmu_dev_flush_ring(dev);

	/* TODO: set UA based on bgly's patches */
	pthread_mutex_lock(&rdev->state_lock);
	if (ret != TCMU_STS_OK) {
		rdev->lock_state = TCMUR_DEV_LOCK_UNLOCKED;
		tcmu_dev_info(dev, "Lock acquisition unsuccessful\n");
	} else {
		if (rdev->lock_state == TCMUR_DEV_LOCK_READ_LOCKING) {
			rdev->lock_state = TCMUR_DEV_LOCK_READ_LOCKED;
			tcmu_dev_info(dev, "Read lock acquisition successful\n");
		} else if (rdev->lock_state == TCMUR_DEV_LOCK_WRITE_LOCKING) {
			rdev->lock_state = TCMUR_DEV_LOCK_WRITE_LOCKED;
			tcmu_dev_info(dev, "Write lock acquisition successful\n");
		} else {
			/*
			 * For explicit transition it will always acquire the write lock.
			 */
			rdev->lock_state = TCMUR_DEV_LOCK_WRITE_LOCKED;
			tcmu_dev_info(dev, "Write lock acquisition successful\n");
		}
	}

	tcmu_cfgfs_dev_exec_action(dev, "block_dev", 0);

	pthread_mutex_unlock(&rdev->state_lock);

	return ret;
}

void tcmu_update_dev_lock_state(struct tcmu_device *dev)
{
	struct tcmur_handler *rhandler = tcmu_get_runner_handler(dev);
	struct tcmur_device *rdev = tcmu_dev_get_private(dev);
	int state;

	if (!rhandler->get_lock_state)
		return;

	pthread_mutex_lock(&rdev->state_lock);
	if (!(rdev->flags & TCMUR_DEV_FLAG_IS_OPEN)) {
		tcmu_dev_dbg(dev, "device closed.\n");
		state = TCMUR_DEV_LOCK_UNKNOWN;
		goto check_state;
	}
	pthread_mutex_unlock(&rdev->state_lock);

	state = rhandler->get_lock_state(dev);
	pthread_mutex_lock(&rdev->state_lock);
check_state:
	if (rdev->lock_state == TCMUR_DEV_LOCK_WRITE_LOCKED &&
	    state != TCMUR_DEV_LOCK_WRITE_LOCKED) {
		tcmu_dev_dbg(dev, "Updated out of sync lock state.\n");
		__tcmu_notify_lock_lost(dev);
	}
	pthread_mutex_unlock(&rdev->state_lock);
}

void tcmur_dev_set_private(struct tcmu_device *dev, void *private)
{
	struct tcmur_device *rdev = tcmu_dev_get_private(dev);

	rdev->hm_private = private;
}

void *tcmur_dev_get_private(struct tcmu_device *dev)
{
	struct tcmur_device *rdev = tcmu_dev_get_private(dev);

	return rdev->hm_private;
}

void tcmu_notify_cmd_timed_out(struct tcmu_device *dev)
{
	struct tcmur_device *rdev = tcmu_dev_get_private(dev);

	pthread_mutex_lock(&rdev->state_lock);
	rdev->cmd_timed_out_cnt++;
	__tcmu_notify_conn_lost(dev);
	pthread_mutex_unlock(&rdev->state_lock);

	tcmu_report_event(dev);
}
