/*
 * Copyright 2017, Red Hat, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
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
#include "target.h"

bool tcmu_dev_in_recovery(struct tcmu_device *dev)
{
	struct tcmur_device *rdev = tcmu_get_daemon_dev_private(dev);
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
int __tcmu_reopen_dev(struct tcmu_device *dev)
{
	struct tcmur_device *rdev = tcmu_get_daemon_dev_private(dev);
	struct tcmur_handler *rhandler = tcmu_get_runner_handler(dev);
	int ret;

	tcmu_dev_dbg(dev, "Waiting for outstanding commands to complete\n");
	ret = aio_wait_for_empty_queue(rdev);

	pthread_mutex_lock(&rdev->state_lock);
	if (ret)
		goto done;

	if (rdev->flags & TCMUR_DEV_FLAG_SHUTTING_DOWN) {
		ret = 0;
		goto done;
	}
	pthread_mutex_unlock(&rdev->state_lock);

	/*
	 * There are no SCSI commands running but there may be
	 * async lock requests in progress that might be accessing
	 * the device.
	 */
	tcmu_cancel_lock_thread(dev);

	/*
	 * Force a reacquisition of the lock when we have reopend the
	 * device, so it can update state. If we are being called from
	 * the lock code path then do not change state.
	 */
	pthread_mutex_lock(&rdev->state_lock);
	if (rdev->lock_state != TCMUR_DEV_LOCK_LOCKING)
		rdev->lock_state = TCMUR_DEV_LOCK_UNLOCKED;
	pthread_mutex_unlock(&rdev->state_lock);

	tcmu_dev_dbg(dev, "Closing device.\n");
	rhandler->close(dev);

	pthread_mutex_lock(&rdev->state_lock);
	rdev->flags &= ~TCMUR_DEV_FLAG_IS_OPEN;
	ret = -EIO;
	while (ret != 0 && !(rdev->flags & TCMUR_DEV_FLAG_SHUTTING_DOWN)) {
		pthread_mutex_unlock(&rdev->state_lock);

		tcmu_dev_dbg(dev, "Opening device.\n");
		ret = rhandler->open(dev);

		pthread_mutex_lock(&rdev->state_lock);
		if (!ret) {
			rdev->flags |= TCMUR_DEV_FLAG_IS_OPEN;
		}
	}

done:
	rdev->flags &= ~TCMUR_DEV_FLAG_IN_RECOVERY;
	pthread_mutex_unlock(&rdev->state_lock);

	return ret;
}

/*
 * tcmu_reopen_dev - close and open device.
 * @dev: device to reopen
 */
int tcmu_reopen_dev(struct tcmu_device *dev)
{
	struct tcmur_device *rdev = tcmu_get_daemon_dev_private(dev);

	pthread_mutex_lock(&rdev->state_lock);
	if (rdev->flags & TCMUR_DEV_FLAG_IN_RECOVERY) {
		pthread_mutex_unlock(&rdev->state_lock);
		return -EBUSY;
	}
	rdev->flags |= TCMUR_DEV_FLAG_IN_RECOVERY;
	pthread_mutex_unlock(&rdev->state_lock);

	return __tcmu_reopen_dev(dev);
}

int tcmu_cancel_recovery(struct tcmu_device *dev)
{
	struct tcmur_device *rdev = tcmu_get_daemon_dev_private(dev);
	void *join_retval;
	int ret;

	pthread_mutex_lock(&rdev->state_lock);
	if (!(rdev->flags & TCMUR_DEV_FLAG_IN_RECOVERY)) {
		pthread_mutex_unlock(&rdev->state_lock);
		return 0;
	}
        pthread_mutex_unlock(&rdev->state_lock);
	/*
	 * Only file and qcow can be canceled in their open/close calls, but
	 * they do not support recovery, so wait here for rbd/glfs type of
	 * handlers to fail/complete normally to avoid a segfault.
	 */
	tcmu_dev_dbg(dev, "Waiting on recovery thread\n");

	ret = pthread_join(rdev->recovery_thread, &join_retval);
	if (ret)
		 tcmu_dev_err(dev, "pthread_join failed with value %d\n", ret);

	/*
	 * Wait for reopen calls from non conn lost events like
	 * reconfigure and lock.
	 */
	pthread_mutex_lock(&rdev->state_lock);
	while (rdev->flags & TCMUR_DEV_FLAG_IN_RECOVERY) {
		pthread_mutex_unlock(&rdev->state_lock);
		sleep(1);
		pthread_mutex_lock(&rdev->state_lock);
	}
	pthread_mutex_unlock(&rdev->state_lock);

	return ret;
}

static void *dev_recovery_thread_fn(void *arg)
{
	tcmu_reset_tpgs(arg);
	return NULL;
}

/**
 * tcmu_notify_conn_lost - notify runner the device instace has lost its
 * 			   connection to its backend storage.
 * @dev: device that has lost its connection
 *
 * Handlers should call this function when they detect they cannot reach their
 * backend storage/medium/cache, so new commands will not be queued until
 * the device has been reopened.
 */
void tcmu_notify_conn_lost(struct tcmu_device *dev)
{
	struct tcmur_device *rdev = tcmu_get_daemon_dev_private(dev);
	int ret;

	pthread_mutex_lock(&rdev->state_lock);
	if (rdev->flags & TCMUR_DEV_FLAG_IN_RECOVERY)
		goto unlock;

	tcmu_dev_err(dev, "Handler connection lost (lock state %d)\n",
		     rdev->lock_state);

	ret = pthread_create(&rdev->recovery_thread, NULL,
			     dev_recovery_thread_fn, dev);
	if (ret) {
		tcmu_dev_err(dev, "Could not start device recovery thread (err %d)\n",
			     ret);
	} else {
		rdev->flags |= TCMUR_DEV_FLAG_IN_RECOVERY;
	}

unlock:
	pthread_mutex_unlock(&rdev->state_lock);
}

/**
 * tcmu_notify_lock_lost - notify runner the device instance has lost the lock
 * @dev: device that has lost the lock
 *
 * Handlers should call this function when they detect they have lost
 * the lock, so runner can re-acquire.
 */
void tcmu_notify_lock_lost(struct tcmu_device *dev)
{
	struct tcmur_device *rdev = tcmu_get_daemon_dev_private(dev);

	pthread_mutex_lock(&rdev->state_lock);
	tcmu_dev_err(dev, "Async lock drop. Old state %d\n", rdev->lock_state);
	/*
	 * We could be getting stale IO completions. If we are trying to
	 * reaquire the lock do not change state.
	 */
	if (rdev->lock_state != TCMUR_DEV_LOCK_LOCKING)
		rdev->lock_state = TCMUR_DEV_LOCK_UNLOCKED;
	pthread_mutex_unlock(&rdev->state_lock);
}

int tcmu_cancel_lock_thread(struct tcmu_device *dev)
{
	struct tcmur_device *rdev = tcmu_get_daemon_dev_private(dev);
	void *join_retval;
	int ret;

	pthread_mutex_lock(&rdev->state_lock);
	if (rdev->lock_state != TCMUR_DEV_LOCK_LOCKING) {
		pthread_mutex_unlock(&rdev->state_lock);
		return 0;
	}
	pthread_mutex_unlock(&rdev->state_lock);
	/*
	 * It looks like lock calls are not cancelable, so
	 * we wait here to avoid crashes.
	 */
	tcmu_dev_dbg(dev, "Waiting on lock thread\n");
	ret = pthread_join(rdev->lock_thread, &join_retval);
	if (ret)
		tcmu_dev_err(dev, "pthread_join failed with value %d\n", ret);
	return ret;
}
