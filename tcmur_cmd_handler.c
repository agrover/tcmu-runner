/*
 * Copyright (c) 2017 Red Hat, Inc.
 *
 * This file is licensed to you under your choice of the GNU Lesser
 * General Public License, version 2.1 or any later version (LGPLv2.1 or
 * later), or the Apache License 2.0.
 */

#define _GNU_SOURCE
#include <scsi/scsi.h>
#include <errno.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "ccan/list/list.h"

#include "darray.h"
#include "libtcmu.h"
#include "libtcmu_log.h"
#include "libtcmu_priv.h"
#include "libtcmu_common.h"
#include "tcmur_aio.h"
#include "tcmur_device.h"
#include "tcmu-runner.h"
#include "tcmu_runner_priv.h"
#include "tcmur_cmd_handler.h"
#include "alua.h"

static void _cleanup_spin_lock(void *arg)
{
	pthread_spin_unlock(arg);
}

void tcmur_tcmulib_cmd_complete(struct tcmu_device *dev,
				struct tcmulib_cmd *cmd, int rc)
{
	struct tcmur_device *rdev = tcmu_dev_get_private(dev);
	struct tcmur_cmd *tcmur_cmd = cmd->hm_private;
	struct timespec curr_time;

	pthread_cleanup_push(_cleanup_spin_lock, (void *)&rdev->lock);
	pthread_spin_lock(&rdev->lock);

	if (tcmur_cmd->timed_out) {
		if (tcmur_get_time(dev, &curr_time)) {
			tcmu_dev_info(dev, "Timed out command id %hu completed with status %d.\n",
				      cmd->cmd_id, rc);
		} else {
			tcmu_dev_info(dev, "Timed out command id %hu completed after %f seconds with status %d.\n",
				      cmd->cmd_id,
				      difftime(curr_time.tv_sec,
					       tcmur_cmd->start_time.tv_sec),
				      rc);
		}
	}

	list_del(&tcmur_cmd->cmds_list_entry);

	tcmulib_command_complete(dev, cmd, rc);

	pthread_spin_unlock(&rdev->lock);
	pthread_cleanup_pop(0);
}

static void aio_command_finish(struct tcmu_device *dev, struct tcmulib_cmd *cmd,
			       int rc)
{
	struct tcmur_device *rdev = tcmu_dev_get_private(dev);
	int wake_up;

	tcmur_tcmulib_cmd_complete(dev, cmd, rc);
	track_aio_request_finish(rdev, &wake_up);
	while (wake_up) {
		tcmulib_processing_complete(dev);
		track_aio_wakeup_finish(rdev, &wake_up);
	}
}

void tcmur_cmd_complete(struct tcmu_device *dev, void *data, int rc)
{
	struct tcmur_cmd *tcmur_cmd = data;

	tcmur_cmd->done(dev, tcmur_cmd, rc);
}

static void tcmur_cmd_iovec_reset(struct tcmur_cmd *tcmur_cmd,
				  size_t data_length)
{
	tcmur_cmd->iovec->iov_base = tcmur_cmd->iov_base_copy;
	tcmur_cmd->iovec->iov_len = data_length;
}

static void tcmur_cmd_state_free(struct tcmur_cmd *tcmur_cmd)
{
	free(tcmur_cmd->cmd_state);
}

static int tcmur_cmd_state_init(struct tcmur_cmd *tcmur_cmd, int state_length,
				size_t data_length)
{
	void *state;
	int iov_length = 0;

	if (data_length)
		iov_length = data_length + sizeof(struct iovec);

	state = calloc(1, state_length + iov_length);
	if (!state)
		return -ENOMEM;

	tcmur_cmd->cmd_state = state;
	tcmur_cmd->requested = data_length;

	if (data_length) {
		struct iovec *iov = state + state_length;

		iov->iov_base = iov + 1;
		iov->iov_len = data_length;

		tcmur_cmd->iov_base_copy = iov->iov_base;
		tcmur_cmd->iov_cnt = 1;
		tcmur_cmd->iovec = iov;
	}

	return 0;
}

static inline int check_iovec_length(struct tcmu_device *dev,
				     struct tcmulib_cmd *cmd, uint32_t sectors)
{
	size_t iov_length = tcmu_iovec_length(cmd->iovec, cmd->iov_cnt);

	if (iov_length != tcmu_lba_to_byte(dev, sectors)) {
		tcmu_dev_err(dev, "iov len mismatch: iov len %zu, xfer len %u, block size %u\n",
			     iov_length, sectors, tcmu_dev_get_block_size(dev));
		return TCMU_STS_HW_ERR;
	}
	return TCMU_STS_OK;
}

static inline int check_lbas(struct tcmu_device *dev,
			     uint64_t start_lba, uint64_t lba_cnt)
{
	uint64_t dev_last_lba = tcmu_dev_get_num_lbas(dev);

	if (start_lba + lba_cnt > dev_last_lba || start_lba + lba_cnt < start_lba) {
		tcmu_dev_err(dev, "cmd exceeds last lba %"PRIu64" (lba %"PRIu64", xfer len %"PRIu64")\n",
			     dev_last_lba, start_lba, lba_cnt);
		return TCMU_STS_RANGE;
	}

	return TCMU_STS_OK;
}

static int check_lba_and_length(struct tcmu_device *dev,
				struct tcmulib_cmd *cmd, uint32_t sectors)
{
	uint8_t *cdb = cmd->cdb;
	uint64_t start_lba = tcmu_cdb_get_lba(cdb);
	int ret;

	ret = check_iovec_length(dev, cmd, sectors);
	if (ret)
		return ret;

	ret = check_lbas(dev, start_lba, sectors);
	if (ret)
		return ret;

	return TCMU_STS_OK;
}

static void handle_generic_cbk(struct tcmu_device *dev,
			       struct tcmur_cmd *tcmur_cmd, int ret)
{
	aio_command_finish(dev, tcmur_cmd->lib_cmd, ret);
}

static int read_work_fn(struct tcmu_device *dev, void *data)
{
	struct tcmur_handler *rhandler = tcmu_get_runner_handler(dev);
	struct tcmur_cmd *tcmur_cmd = data;
	struct tcmulib_cmd *cmd = tcmur_cmd->lib_cmd;

	return rhandler->read(dev, tcmur_cmd, cmd->iovec, cmd->iov_cnt,
			      tcmu_iovec_length(cmd->iovec, cmd->iov_cnt),
			      tcmu_cdb_to_byte(dev, cmd->cdb));
}

static int write_work_fn(struct tcmu_device *dev, void *data)
{
	struct tcmur_handler *rhandler = tcmu_get_runner_handler(dev);
	struct tcmur_cmd *tcmur_cmd = data;
	struct tcmulib_cmd *cmd = tcmur_cmd->lib_cmd;

	return rhandler->write(dev, tcmur_cmd, cmd->iovec, cmd->iov_cnt,
				tcmu_iovec_length(cmd->iovec, cmd->iov_cnt),
				tcmu_cdb_to_byte(dev, cmd->cdb));
}

struct unmap_state {
	pthread_mutex_t lock;
	unsigned int refcount;
	int status;
};

struct unmap_descriptor {
	uint64_t offset;
	uint64_t length;
};

static int unmap_init(struct tcmu_device *dev, struct tcmulib_cmd *cmd)
{
	struct tcmur_cmd *tcmur_cmd = cmd->hm_private;
	struct unmap_state *state;
	int ret;

	if (tcmur_cmd_state_init(tcmur_cmd, sizeof(*state), 0))
		return TCMU_STS_NO_RESOURCE;
	state = tcmur_cmd->cmd_state;

	ret = pthread_mutex_init(&state->lock, NULL);
	if (ret == -1) {
		tcmu_dev_err(dev, "Failed to init spin lock in state!\n");
		ret = TCMU_STS_HW_ERR;
		goto out_free_state;
	}

	/* released by allocator when done submitting unmaps */
	state->refcount = 1;
	state->status = TCMU_STS_OK;
	return TCMU_STS_OK;

out_free_state:
	tcmur_cmd_state_free(tcmur_cmd);
	return ret;
}

static void unmap_put(struct tcmu_device *dev, struct tcmulib_cmd *cmd,
		      int ret)
{
	struct tcmur_cmd *tcmur_cmd = cmd->hm_private;
	struct unmap_state *state = tcmur_cmd->cmd_state;
	int status;

	pthread_mutex_lock(&state->lock);

	if (state->status != TCMU_STS_OK && ret)
		state->status = ret;

	if (--state->refcount > 0) {
		pthread_mutex_unlock(&state->lock);
		return;
	}
	status = state->status;
	pthread_mutex_unlock(&state->lock);

	pthread_mutex_destroy(&state->lock);
	tcmur_cmd_state_free(tcmur_cmd);

	aio_command_finish(dev, cmd, status);
}

static void handle_unmap_cbk(struct tcmu_device *dev,
			     struct tcmur_cmd *tcmur_ucmd, int ret)
{
	struct unmap_descriptor *desc = tcmur_ucmd->cmd_state;
	struct tcmulib_cmd *cmd = tcmur_ucmd->lib_cmd;

	free(desc);
	free(tcmur_ucmd);

	unmap_put(dev, cmd, ret);
}

static int unmap_work_fn(struct tcmu_device *dev, void *data)
{
	struct tcmur_handler *rhandler = tcmu_get_runner_handler(dev);
	struct tcmur_cmd *tcmur_ucmd = data;
	struct unmap_descriptor *desc = tcmur_ucmd->cmd_state;
	uint64_t offset = desc->offset, length = desc->length;

	return rhandler->unmap(dev, tcmur_ucmd, offset, length);
}

static int align_and_split_unmap(struct tcmu_device *dev,
				 struct tcmur_cmd *tcmur_cmd,
				 uint64_t lba, uint64_t nlbas)
{
	struct unmap_state *state = tcmur_cmd->cmd_state;
	uint64_t opt_unmap_gran;
	uint64_t unmap_gran_align, mask;
	int ret = TCMU_STS_NOT_HANDLED;
	int j = 0;
	struct unmap_descriptor *desc;
	struct tcmur_cmd *tcmur_ucmd;
	uint64_t lbas;

	if (!dev->split_unmaps) {
		/*
		 * Handler does not support vectored unmaps, but prefers to
		 * break up unmaps itself, so pass the entire segment to it.
		 */
		opt_unmap_gran = tcmu_dev_get_max_unmap_len(dev);
		mask = 0;
	} else {
		/*
		 * Align the start lba of a unmap request and split the
		 * large num blocks into OPTIMAL UNMAP GRANULARITY size.
		 *
		 * NOTE: here we always asumme the OPTIMAL UNMAP GRANULARITY
		 * equals to UNMAP GRANULARITY ALIGNMENT to simplify the
		 * algorithm. In the future, for new devices that have different
		 * values the following align and split algorithm should be
		 * changed.
		 */

		/* OPTIMAL UNMAP GRANULARITY */
		opt_unmap_gran = tcmu_dev_get_opt_unmap_gran(dev);

		/* UNMAP GRANULARITY ALIGNMENT */
		unmap_gran_align = tcmu_dev_get_unmap_gran_align(dev);
		mask = unmap_gran_align - 1;
	}

	lbas = opt_unmap_gran - (lba & mask);
	lbas = min(lbas, nlbas);

	tcmu_dev_dbg(dev, "OPTIMAL UNMAP GRANULARITY: %"PRIu64", UNMAP GRANULARITY ALIGNMENT mask: %"PRIu64", lbas: %"PRIu64"\n",
		     opt_unmap_gran, mask, lbas);

	while (nlbas) {
		desc = calloc(1, sizeof(*desc));
		if (!desc) {
			tcmu_dev_err(dev, "Failed to calloc desc!\n");
			return TCMU_STS_NO_RESOURCE;
		}
		desc->offset = tcmu_lba_to_byte(dev, lba);
		desc->length = tcmu_lba_to_byte(dev, lbas);

		tcmur_ucmd = calloc(1, sizeof(*tcmur_ucmd));
		if (!tcmur_ucmd) {
			tcmu_dev_err(dev, "Failed to calloc unmap cmd!\n");
			ret = TCMU_STS_NO_RESOURCE;
			goto free_desc;
		}
		tcmur_ucmd->cmd_state = desc;
		tcmur_ucmd->lib_cmd = tcmur_cmd->lib_cmd;
		tcmur_ucmd->done = handle_unmap_cbk;

		/* The first one */
		if (j++ == 0)
			tcmu_dev_dbg(dev, "The first split: start lba: %"PRIu64", end lba: %"PRIu64", lbas: %"PRIu64"\n",
				     lba, lba + lbas - 1, lbas);

		/* The last one */
		if (nlbas == lbas) {
			tcmu_dev_dbg(dev, "The last split: start lba: %"PRIu64", end lba: %"PRIu64", lbas: %"PRIu64"\n",
				     lba, lba + lbas - 1, lbas);
			tcmu_dev_dbg(dev, "There are totally %d splits\n", j);
		}

		pthread_mutex_lock(&state->lock);
		state->refcount++;
		pthread_mutex_unlock(&state->lock);

		ret = aio_request_schedule(dev, tcmur_ucmd, unmap_work_fn,
					   tcmur_cmd_complete);
		if (ret != TCMU_STS_ASYNC_HANDLED)
			goto free_ucmd;

		nlbas -= lbas;
		lba += lbas;
		lbas = min(opt_unmap_gran, nlbas);
	}

	return ret;

free_ucmd:
	pthread_mutex_lock(&state->lock);
	state->refcount--;
	pthread_mutex_unlock(&state->lock);
	free(tcmur_ucmd);
free_desc:
	free(desc);
	return ret;
}

static int handle_unmap_internal(struct tcmu_device *dev, struct tcmulib_cmd *cmd,
				 uint16_t bddl, uint8_t *par)
{
	struct tcmur_cmd *tcmur_cmd = cmd->hm_private;
	struct unmap_state *state;
	uint16_t offset = 0;
	int ret = TCMU_STS_OK, i = 0;

	ret = unmap_init(dev, cmd);
	if (ret)
		return ret;
	state = tcmur_cmd->cmd_state;

	/* The first descriptor list offset is 8 in Data-Out buffer */
	par += 8;
	while (bddl) {
		uint64_t lba;
		uint64_t nlbas;

		lba = be64toh(*((uint64_t *)&par[offset]));
		nlbas = be32toh(*((uint32_t *)&par[offset + 8]));

		tcmu_dev_dbg(dev, "Parameter list %d, start lba: %"PRIu64", end lba: %"PRIu64", nlbas: %"PRIu64"\n",
			     i++, lba, lba + nlbas - 1, nlbas);

		if (nlbas > tcmu_dev_get_max_unmap_len(dev)) {
			tcmu_dev_err(dev, "Illegal parameter list LBA count %"PRIu64" exceeds:%u\n",
				     nlbas, tcmu_dev_get_max_unmap_len(dev));
			ret = TCMU_STS_INVALID_PARAM_LIST;
			goto done;
		}

		ret = check_lbas(dev, lba, nlbas);
		if (ret)
			goto done;

		if (nlbas) {
			ret = align_and_split_unmap(dev, tcmur_cmd, lba, nlbas);
			if (ret != TCMU_STS_ASYNC_HANDLED)
				goto done;
		}

		/* The unmap block descriptor data length is 16 */
		offset += 16;
		bddl -= 16;
	}

done:
	/*
	 * unmap_put will do the right thing, so always return
	 * TCMU_STS_ASYNC_HANDLED
	*/
	pthread_mutex_lock(&state->lock);
	if (ret == TCMU_STS_ASYNC_HANDLED) {
		ret = TCMU_STS_OK;
	} else {
		state->status = ret;
	}
	pthread_mutex_unlock(&state->lock);

	unmap_put(dev, cmd, ret);
	return TCMU_STS_ASYNC_HANDLED;
}

static int handle_unmap(struct tcmu_device *dev, struct tcmulib_cmd *origcmd)
{
	struct tcmur_handler *rhandler = tcmu_get_runner_handler(dev);
	uint8_t *cdb = origcmd->cdb;
	size_t copied, data_length = tcmu_cdb_get_xfer_length(cdb);
	uint8_t *par;
	uint16_t dl, bddl;
	int ret;

	if (!rhandler->unmap)
		return TCMU_STS_INVALID_CMD;

	/*
	 * ANCHOR bit check
	 *
	 * The ANCHOR in the Logical Block Provisioning VPD page is not
	 * supported, so the ANCHOR bit shouldn't be set here.
	 */
	if (cdb[1] & 0x01) {
		tcmu_dev_err(dev, "Illegal request: anchor is not supported for now!\n");
		return TCMU_STS_INVALID_CDB;
	}

	/*
	 * PARAMETER LIST LENGTH field.
	 *
	 * The PARAMETER LIST LENGTH field specifies the length in bytes of
	 * the UNMAP parameter data that shall be sent from the application
	 * client to the device server.
	 *
	 * A PARAMETER LIST LENGTH set to zero specifies that no data shall
	 * be sent.
	 */
	if (!data_length) {
		tcmu_dev_dbg(dev, "Data-Out Buffer length is zero, just return okay\n");
		return TCMU_STS_OK;
	}

	/*
	 * From sbc4r13, section 5.32.1 UNMAP command overview.
	 *
	 * The PARAMETER LIST LENGTH should be greater than eight,
	 */
	if (data_length < 8) {
		tcmu_dev_err(dev, "Illegal parameter list length %zu and it should be >= 8\n",
			     data_length);
		return TCMU_STS_INVALID_PARAM_LIST_LEN;
	}

	par = calloc(1, data_length);
	if (!par) {
		tcmu_dev_err(dev, "The state parameter is NULL!\n");
		return TCMU_STS_NO_RESOURCE;
	}
	copied = tcmu_memcpy_from_iovec(par, data_length, origcmd->iovec,
					origcmd->iov_cnt);
	if (copied != data_length) {
		tcmu_dev_err(dev, "Failed to copy the Data-Out Buffer !\n");
		ret = TCMU_STS_INVALID_PARAM_LIST_LEN;
		goto out_free_par;
	}

	/*
	 * If any UNMAP block descriptors in the UNMAP block descriptor
	 * list are truncated due to the parameter list length in the CDB,
	 * then that UNMAP block descriptor shall be ignored.
	 *
	 * So it will allow dl + 2 != data_length and bddl + 8 != data_length.
	 */
	dl = be16toh(*((uint16_t *)&par[0]));
	bddl = be16toh(*((uint16_t *)&par[2]));

	tcmu_dev_dbg(dev, "Data-Out Buffer Length: %zu, dl: %hu, bddl: %hu\n",
		     data_length, dl, bddl);

	/*
	 * If the unmap block descriptor data length is not a multiple
	 * of 16, then the last unmap block descriptor is incomplete
	 * and shall be ignored.
	 */
	bddl &= ~0xF;

	/*
	 * If the UNMAP BLOCK DESCRIPTOR DATA LENGTH is set to zero, then
	 * no unmap block descriptors are included in the UNMAP parameter
	 * list.
	 */
	if (!bddl) {
		ret = TCMU_STS_OK;
		goto out_free_par;
	}

	if (bddl / 16 > VPD_MAX_UNMAP_BLOCK_DESC_COUNT) {
		tcmu_dev_err(dev, "Illegal parameter list count %hu exceeds :%u\n",
			     bddl / 16, VPD_MAX_UNMAP_BLOCK_DESC_COUNT);
		ret = TCMU_STS_INVALID_PARAM_LIST;
		goto out_free_par;
	}

	ret = handle_unmap_internal(dev, origcmd, bddl, par);

out_free_par:
	free(par);
	return ret;
}

struct write_same {
	uint64_t cur_lba;
	uint64_t lba_cnt;
};

static int writesame_work_fn(struct tcmu_device *dev, void *data)
{
	struct tcmur_handler *rhandler = tcmu_get_runner_handler(dev);
	struct tcmur_cmd *tcmur_cmd = data;
	struct write_same *write_same = tcmur_cmd->cmd_state;

	tcmur_cmd_iovec_reset(tcmur_cmd, tcmur_cmd->requested);
	/*
	 * Write contents of the logical block data(from the Data-Out Buffer)
	 * to each LBA in the specified LBA range.
	 */
	return rhandler->write(dev, tcmur_cmd, tcmur_cmd->iovec,
			       tcmur_cmd->iov_cnt, tcmur_cmd->requested,
			       tcmu_lba_to_byte(dev, write_same->cur_lba));
}

static void handle_writesame_cbk(struct tcmu_device *dev,
				  struct tcmur_cmd *tcmur_cmd, int ret)
{
	struct tcmulib_cmd *cmd = tcmur_cmd->lib_cmd;
	struct write_same *write_same = tcmur_cmd->cmd_state;
	uint64_t write_lbas = tcmu_byte_to_lba(dev, tcmur_cmd->requested);
	uint64_t left_lbas;
	int rc;

	/* write failed - bail out */
	if (ret != TCMU_STS_OK)
		goto finish_err;

	write_same->cur_lba += write_lbas;
	write_same->lba_cnt -= write_lbas;
	left_lbas = write_same->lba_cnt;

	if (!left_lbas)
		goto finish_err;

	if (left_lbas <= write_lbas) {
		tcmu_dev_dbg(dev, "Last lba: %"PRIu64", write lbas: %"PRIu64"\n",
			     write_same->cur_lba, left_lbas);

		tcmur_cmd->requested = tcmu_lba_to_byte(dev, left_lbas);
	} else {
		tcmu_dev_dbg(dev, "Next lba: %"PRIu64", write lbas: %"PRIu64"\n",
			     write_same->cur_lba, write_lbas);
	}

	rc = aio_request_schedule(dev, tcmur_cmd, writesame_work_fn,
				  tcmur_cmd_complete);
	if (rc != TCMU_STS_ASYNC_HANDLED) {
		tcmu_dev_err(dev, "Write same async handle cmd failure\n");
		ret = TCMU_STS_WR_ERR;
		goto finish_err;
	}

	return;

finish_err:
	tcmur_cmd_state_free(tcmur_cmd);
	aio_command_finish(dev, cmd, ret);
}

static int handle_writesame_check(struct tcmu_device *dev, struct tcmulib_cmd *cmd)
{
	uint8_t *cdb = cmd->cdb;
	uint32_t lba_cnt = tcmu_cdb_get_xfer_length(cdb);
	uint32_t block_size = tcmu_dev_get_block_size(dev);
	uint64_t start_lba = tcmu_cdb_get_lba(cdb);
	int ret;

	if (cmd->iov_cnt != 1 || cmd->iovec->iov_len != block_size) {
		tcmu_dev_err(dev, "Illegal Data-Out: iov_cnt %zu length: %zu\n",
			     cmd->iov_cnt, cmd->iovec->iov_len);
		return TCMU_STS_INVALID_CDB;
	}

	/*
	 * From sbc4r13, section 5.50 WRITE SAME (16) command
	 *
	 * A write same (WSNZ) bit has beed set to one, so the device server
	 * won't support a value of zero here.
	 */
	if (!lba_cnt) {
		tcmu_dev_err(dev, "The WSNZ = 1 & WRITE_SAME blocks = 0 is not supported!\n");
		return TCMU_STS_INVALID_CDB;
	}

	/*
	 * The MAXIMUM WRITE SAME LENGTH field in Block Limits VPD page (B0h)
	 * limit the maximum block number for the WRITE SAME.
	 */
	if (lba_cnt > VPD_MAX_WRITE_SAME_LENGTH) {
		tcmu_dev_err(dev, "blocks: %u exceeds MAXIMUM WRITE SAME LENGTH: %u\n",
			     lba_cnt, VPD_MAX_WRITE_SAME_LENGTH);
		return TCMU_STS_INVALID_CDB;
	}

	/*
	 * The logical block address plus the number of blocks shouldn't
	 * exceeds the capacity of the medium
	 */
	ret = check_lbas(dev, start_lba, lba_cnt);
	if (ret)
		return ret;

	tcmu_dev_dbg(dev, "Start lba: %"PRIu64", number of lba: %u, last lba: %"PRIu64"\n",
		     start_lba, lba_cnt, start_lba + lba_cnt - 1);

	return TCMU_STS_OK;
}

static int handle_unmap_in_writesame(struct tcmu_device *dev,
				     struct tcmulib_cmd *cmd)
{
	struct tcmur_cmd *tcmur_cmd = cmd->hm_private;
	uint8_t *cdb = cmd->cdb;
	uint64_t lba = tcmu_cdb_get_lba(cdb);
	uint64_t nlbas = tcmu_cdb_get_xfer_length(cdb);
	uint32_t align = tcmu_dev_get_unmap_gran_align(dev);
	struct unmap_state *state;
	int ret;

	/* If not aligned then falls back to the writesame without unmap */
	if (lba % align || nlbas % align) {
		tcmu_dev_dbg(dev,
			     "Start lba: %"PRIu64" or nlbas: %"PRIu64" not aligned to %"PRIu32"\n",
			     lba, nlbas, align);
		tcmu_dev_dbg(dev, "Falls back to writesame without unmap!\n");
		return TCMU_STS_NOT_HANDLED;
	}

	tcmu_dev_dbg(dev, "Do UNMAP in WRITE_SAME cmd!\n");

	ret = unmap_init(dev, cmd);
	if (ret)
		return ret;
	state = tcmur_cmd->cmd_state;

	ret = align_and_split_unmap(dev, tcmur_cmd, lba, nlbas);
	if (ret == TCMU_STS_ASYNC_HANDLED) {
		ret = TCMU_STS_OK;
	} else {
		state->status = ret;
	}

	unmap_put(dev, cmd, ret);
	return TCMU_STS_ASYNC_HANDLED;
}

static int tcmur_writesame_work_fn(struct tcmu_device *dev, void *data)
{
	struct tcmur_cmd *tcmur_cmd = data;
	struct tcmulib_cmd *cmd = tcmur_cmd->lib_cmd;
	tcmur_writesame_fn_t write_same_fn = tcmur_cmd->cmd_state;
	uint8_t *cdb = cmd->cdb;
	uint64_t off = tcmu_cdb_to_byte(dev, cdb);
	uint64_t len = tcmu_lba_to_byte(dev, tcmu_cdb_get_xfer_length(cdb));

	/*
	 * Write contents of the logical block data(from the Data-Out Buffer)
	 * to each LBA in the specified LBA range.
	 */
	return write_same_fn(dev, tcmur_cmd, off, len, cmd->iovec,
			     cmd->iov_cnt);
}

static int handle_writesame(struct tcmu_device *dev, struct tcmulib_cmd *cmd)
{
	struct tcmur_handler *rhandler = tcmu_get_runner_handler(dev);
	struct tcmur_cmd *tcmur_cmd = cmd->hm_private;
	uint8_t *cdb = cmd->cdb;
	uint32_t lba_cnt = tcmu_cdb_get_xfer_length(cdb);
	uint32_t block_size = tcmu_dev_get_block_size(dev);
	uint64_t start_lba = tcmu_cdb_get_lba(cdb);
	uint64_t write_lbas;
	size_t max_xfer_length, length = 1024 * 1024;
	struct write_same *write_same;
	int i, ret;

	if (tcmu_dev_in_recovery(dev))
		return TCMU_STS_BUSY;

	ret = alua_check_state(dev, cmd, false);
	if (ret)
		return ret;

	ret = handle_writesame_check(dev, cmd);
	if (ret)
		return ret;

	if (rhandler->unmap && (cmd->cdb[1] & 0x08)) {
		ret = handle_unmap_in_writesame(dev, cmd);
		if (ret != TCMU_STS_NOT_HANDLED)
			return ret;
	}

	if (rhandler->writesame) {
		tcmur_cmd->cmd_state = rhandler->writesame;
		tcmur_cmd->done = handle_generic_cbk;
		return aio_request_schedule(dev, tcmur_cmd,
					    tcmur_writesame_work_fn,
					    tcmur_cmd_complete);
	}

	max_xfer_length = tcmu_dev_get_max_xfer_len(dev) * block_size;
	length = round_up(length, max_xfer_length);
	length = min(length, (size_t)tcmu_lba_to_byte(dev, lba_cnt));

	if (tcmur_cmd_state_init(tcmur_cmd, sizeof(*write_same), length)) {
		tcmu_dev_err(dev, "Failed to calloc write_same data!\n");
		return TCMU_STS_NO_RESOURCE;
	}
	tcmur_cmd->done = handle_writesame_cbk;

	write_lbas = tcmu_byte_to_lba(dev, length);
	for (i = 0; i < write_lbas; i++)
		memcpy(tcmur_cmd->iovec->iov_base + i * block_size,
		       cmd->iovec->iov_base, block_size);

	write_same = tcmur_cmd->cmd_state;
	write_same->cur_lba = start_lba;
	write_same->lba_cnt = lba_cnt;

	tcmu_dev_dbg(dev, "First lba: %"PRIu64", write lbas: %"PRIu64"\n",
		     start_lba, write_lbas);

	return aio_request_schedule(dev, tcmur_cmd, writesame_work_fn,
				    tcmur_cmd_complete);
}

/* async write verify */
struct write_verify_state {
	struct iovec *w_iovec;
	size_t w_iov_cnt;
};

static void handle_write_verify_read_cbk(struct tcmu_device *dev,
					 struct tcmur_cmd *tcmur_cmd, int ret)
{
	struct write_verify_state *state = tcmur_cmd->cmd_state;
	struct tcmulib_cmd *cmd = tcmur_cmd->lib_cmd;
	uint32_t cmp_offset;

	/* failed read - bail out */
	if (ret != TCMU_STS_OK)
		goto done;

	ret = TCMU_STS_OK;
	cmp_offset = tcmu_iovec_compare(tcmur_cmd->iov_base_copy,
					state->w_iovec, tcmur_cmd->requested);
	if (cmp_offset != -1) {
		tcmu_dev_err(dev, "Verify failed at offset %u\n", cmp_offset);
		ret =  TCMU_STS_MISCOMPARE;
		tcmu_sense_set_info(cmd->sense_buf, cmp_offset);
	}

done:
	tcmur_cmd_state_free(tcmur_cmd);
	aio_command_finish(dev, cmd, ret);
}

static int write_verify_read_work_fn(struct tcmu_device *dev, void *data)
{
	struct tcmur_handler *rhandler = tcmu_get_runner_handler(dev);
	struct tcmur_cmd *tcmur_cmd = data;
	struct tcmulib_cmd *cmd = tcmur_cmd->lib_cmd;

	return rhandler->read(dev, tcmur_cmd, tcmur_cmd->iovec,
			      tcmur_cmd->iov_cnt, tcmur_cmd->requested,
			      tcmu_cdb_to_byte(dev, cmd->cdb));
}

static void handle_write_verify_write_cbk(struct tcmu_device *dev,
					  struct tcmur_cmd *tcmur_cmd,
					  int ret)
{
	struct tcmulib_cmd *cmd = tcmur_cmd->lib_cmd;

	/* write error - bail out */
	if (ret != TCMU_STS_OK)
		goto finish_err;

	tcmur_cmd->done = handle_write_verify_read_cbk;

	ret = aio_request_schedule(dev, tcmur_cmd, write_verify_read_work_fn,
				   tcmur_cmd_complete);
	if (ret != TCMU_STS_ASYNC_HANDLED)
		goto finish_err;
	return;

finish_err:
	tcmur_cmd_state_free(tcmur_cmd);
	aio_command_finish(dev, cmd, ret);
}

static int handle_write_verify(struct tcmu_device *dev, struct tcmulib_cmd *cmd)
{
	struct tcmur_cmd *tcmur_cmd = cmd->hm_private;
	uint8_t *cdb = cmd->cdb;
	size_t length = tcmu_lba_to_byte(dev, tcmu_cdb_get_xfer_length(cdb));
	struct write_verify_state *state;
	int i, ret, state_len;

	ret = check_lba_and_length(dev, cmd, tcmu_cdb_get_xfer_length(cdb));
	if (ret)
		return ret;

	state_len = sizeof(*state) + (cmd->iov_cnt * sizeof(struct iovec));

	if (tcmur_cmd_state_init(tcmur_cmd, state_len, length))
		return TCMU_STS_NO_RESOURCE;
	tcmur_cmd->done = handle_write_verify_write_cbk;

	state = tcmur_cmd->cmd_state;
	/*
	 * Copy cmd iovec for later comparision in case handler modifies
	 * pointers/lens.
	 */
	state->w_iovec = (void *)state + sizeof(*state);
	state->w_iov_cnt = cmd->iov_cnt;
	for (i = 0; i < cmd->iov_cnt; i++) {
		state->w_iovec[i].iov_base = cmd->iovec[i].iov_base;
		state->w_iovec[i].iov_len = cmd->iovec[i].iov_len;
	}

	ret = aio_request_schedule(dev, tcmur_cmd, write_work_fn,
				   tcmur_cmd_complete);
	if (ret != TCMU_STS_ASYNC_HANDLED)
		goto free_state;

	return TCMU_STS_ASYNC_HANDLED;

free_state:
	tcmur_cmd_state_free(tcmur_cmd);
	return ret;
}

#define XCOPY_HDR_LEN                   16
#define XCOPY_TARGET_DESC_LEN           32
#define XCOPY_SEGMENT_DESC_B2B_LEN      28
#define XCOPY_NAA_IEEE_REGEX_LEN        16

struct xcopy {
	struct tcmu_device *origdev;
	struct tcmu_device *src_dev;
	uint8_t src_tid_wwn[XCOPY_NAA_IEEE_REGEX_LEN];
	struct tcmu_device *dst_dev;
	uint8_t dst_tid_wwn[XCOPY_NAA_IEEE_REGEX_LEN];

	uint64_t src_lba;
	uint64_t dst_lba;
	uint32_t stdi;
	uint32_t dtdi;
	uint32_t lba_cnt;
	uint32_t copy_lbas;
};

/* For now only supports block -> block type */
static int xcopy_parse_segment_descs(uint8_t *seg_descs, struct xcopy *xcopy,
				     uint8_t sdll)
{
	uint8_t *seg_desc = seg_descs;
	uint8_t desc_len;

	/*
	 * From spc4r31, section 6.3.7.5 Block device to block device
	 * operations
	 *
	 * The segment descriptor size should be 28 bytes
	 */
	if (sdll % XCOPY_SEGMENT_DESC_B2B_LEN != 0) {
		tcmu_err("Illegal block --> block type segment descriptor length %u\n",
			 sdll);
		return TCMU_STS_INVALID_PARAM_LIST;
	}

	/* From spc4r36q, section 6.4.3.5 SEGMENT DESCRIPTOR LIST LENGTH field
	 * If the number of segment descriptors exceeds the allowed number, the copy
	 * manager shall terminate the command with CHECK CONDITION status, with the
	 * sense key set to ILLEGAL REQUEST, and the additional sense code set to
	 * TOO MANY SEGMENT DESCRIPTORS.
	 */
	if (sdll > RCR_OP_MAX_SEGMENT_DESC_COUNT * XCOPY_SEGMENT_DESC_B2B_LEN) {
		tcmu_err("Only %u segment descriptor(s) supported, but there are %u\n",
			 RCR_OP_MAX_SEGMENT_DESC_COUNT,
			 sdll / XCOPY_SEGMENT_DESC_B2B_LEN);
		return TCMU_STS_TOO_MANY_SEG_DESC;
	}

	/* EXTENDED COPY segment descriptor type codes block --> block */
	if (seg_desc[0] != XCOPY_SEG_DESC_TYPE_CODE_B2B) {
		tcmu_err("Unsupport segment descriptor type code 0x%x\n",
			 seg_desc[0]);
		return TCMU_STS_NOTSUPP_SEG_DESC_TYPE;
	}

	/*
	 * For block -> block type the length is 4-byte header + 0x18-byte
	 * data.
	 */
	desc_len = be16toh(*(uint16_t *)&seg_desc[2]);
	if (desc_len != 0x18) {
		tcmu_err("Invalid length for block->block type 0x%x\n",
			 desc_len);
		return TCMU_STS_INVALID_PARAM_LIST;
	}

	/*
	 * From spc4r31, section 6.3.7.1 Segment descriptors introduction
	 *
	 * The SOURCE TARGET DESCRIPTOR INDEX field contains an index into
	 * the target descriptor list (see 6.3.1) identifying the source
	 * copy target device. The DESTINATION TARGET DESCRIPTOR INDEX field
	 * contains an index into the target descriptor list (see 6.3.1)
	 * identifying the destination copy target device.
	 */
	xcopy->stdi = be16toh(*(uint16_t *)&seg_desc[4]);
	xcopy->dtdi = be16toh(*(uint16_t *)&seg_desc[6]);
	tcmu_dbg("Segment descriptor: stdi: %hu dtdi: %hu\n", xcopy->stdi,
		 xcopy->dtdi);

	xcopy->lba_cnt = be16toh(*(uint16_t *)&seg_desc[10]);
	xcopy->src_lba = be64toh(*(uint64_t *)&seg_desc[12]);
	xcopy->dst_lba = be64toh(*(uint64_t *)&seg_desc[20]);
	tcmu_dbg("Segment descriptor: lba_cnt: %u src_lba: %"PRIu64" dst_lba: %"PRIu64"\n",
		 xcopy->lba_cnt, xcopy->src_lba, xcopy->dst_lba);

	return TCMU_STS_OK;
}

static int xcopy_gen_naa_ieee(struct tcmu_device *udev, uint8_t *wwn)
{
	char *buf, *p;
	bool next = true;
	int ind = 0;

	/* Set type 6 and use OpenFabrics IEEE Company ID: 00 14 05 */
	wwn[ind++] = 0x60;
	wwn[ind++] = 0x01;
	wwn[ind++] = 0x40;
	wwn[ind] = 0x50;

	/* Parse the udev vpd unit serial number */
	buf = tcmu_cfgfs_dev_get_wwn(udev);
	if (!buf)
		return -1;
	p = buf;

	/*
	 * Generate up to 36 bits of VENDOR SPECIFIC IDENTIFIER starting on
	 * byte 3 bit 3-0 for NAA IEEE Registered Extended DESIGNATOR field
	 * format, followed by 64 bits of VENDOR SPECIFIC IDENTIFIER EXTENSION
	 * to complete the payload.  These are based from VPD=0x80 PRODUCT SERIAL
	 * NUMBER set via vpd_unit_serial in target_core_configfs.c to ensure
	 * per device uniqeness.
	 */
	for (; *p && ind < XCOPY_NAA_IEEE_REGEX_LEN; p++) {
		uint8_t val;

		if (!char_to_hex(&val, *p))
			continue;

		if (next) {
			next = false;
			wwn[ind++] |= val;
		} else {
			next = true;
			wwn[ind] = val << 4;
		}
	}

	free(buf);
	return TCMU_STS_OK;
}

static int xcopy_locate_udev(struct tcmulib_context *ctx,
			     const uint8_t *dev_wwn,
			     struct tcmu_device **udev)
{
	struct tcmu_device **dev_ptr;
	struct tcmu_device *dev;
	uint8_t wwn[XCOPY_NAA_IEEE_REGEX_LEN];

	darray_foreach(dev_ptr, ctx->devices) {
		dev = *dev_ptr;

		memset(wwn, 0, XCOPY_NAA_IEEE_REGEX_LEN);
		if (xcopy_gen_naa_ieee(dev, wwn))
			return -1;

		if (memcmp(wwn, dev_wwn, XCOPY_NAA_IEEE_REGEX_LEN))
			continue;

		*udev = dev;
		tcmu_dev_dbg(dev, "Located tcmu devivce: %s\n",
			     dev->tcm_dev_name);

		return 0;
	}

	return -1;
}

/* Identification descriptor target */
static int xcopy_parse_target_id(struct tcmu_device *udev,
				  struct xcopy *xcopy,
				  uint8_t *tgt_desc,
				  int32_t index)
{
	uint8_t wwn[XCOPY_NAA_IEEE_REGEX_LEN];

	/*
	 * Generate an IEEE Registered Extended designator based upon the
	 * device the XCOPY specified.
	 */
	memset(wwn, 0, XCOPY_NAA_IEEE_REGEX_LEN);
	if (xcopy_gen_naa_ieee(udev, wwn))
		return TCMU_STS_HW_ERR;

	/*
	 * CODE SET: for now only binary type code is supported.
	 */
	if ((tgt_desc[4] & 0x0f) != 0x1) {
		tcmu_dev_err(udev, "Id target CODE DET only support binary type!\n");
		return TCMU_STS_INVALID_PARAM_LIST;
	}

	/*
	 * ASSOCIATION: for now only LUN type code is supported.
	 */
	if ((tgt_desc[5] & 0x30) != 0x00) {
		tcmu_dev_err(udev, "Id target ASSOCIATION other than LUN not supported!\n");
		return TCMU_STS_INVALID_PARAM_LIST;
	}

	/*
	 * DESIGNATOR TYPE: for now only NAA type code is supported.
	 *
	 * The designator type define please see: such as
	 * From spc4r31, section 7.8.6.1 Device Identification VPD page
	 * overview
	 */
	if ((tgt_desc[5] & 0x0f) != 0x3) {
		tcmu_dev_err(udev, "Id target DESIGNATOR TYPE other than NAA not supported!\n");
		return TCMU_STS_INVALID_PARAM_LIST;
	}
	/*
	 * Check for matching 16 byte length for NAA IEEE Registered Extended
	 * Assigned designator
	 */
	if (tgt_desc[7] != 16) {
		tcmu_dev_err(udev, "Id target DESIGNATOR LENGTH should be 16, but it's: %d\n",
			     tgt_desc[7]);
		return TCMU_STS_INVALID_PARAM_LIST;
	}

	/*
	 * Check for NAA IEEE Registered Extended Assigned header.
	 */
	if ((tgt_desc[8] >> 4) != 0x06) {
		tcmu_dev_err(udev, "Id target NAA designator type: 0x%x\n",
			     tgt_desc[8] >> 4);
		return TCMU_STS_INVALID_PARAM_LIST;
	}

	/*
	 * Source designator matches the local device
	 */
	if (index == xcopy->stdi) {
		memcpy(&xcopy->src_tid_wwn[0], &tgt_desc[8],
		       XCOPY_NAA_IEEE_REGEX_LEN);

		if (!memcmp(wwn, xcopy->src_tid_wwn, XCOPY_NAA_IEEE_REGEX_LEN))
			xcopy->src_dev = udev;
	}

	/*
	 * Destination designator matches the local device.
	 */
	if (index == xcopy->dtdi) {
		memcpy(xcopy->dst_tid_wwn, &tgt_desc[8],
		       XCOPY_NAA_IEEE_REGEX_LEN);

		if (!memcmp(wwn, xcopy->dst_tid_wwn, XCOPY_NAA_IEEE_REGEX_LEN))
			xcopy->dst_dev = udev;
	}

	return TCMU_STS_OK;
}

static int xcopy_parse_target_descs(struct tcmu_device *udev,
				    struct xcopy *xcopy,
				    uint8_t *tgt_desc,
				    uint16_t tdll)
{
	int i, ret;

	if (tdll % XCOPY_TARGET_DESC_LEN) {
		tcmu_dev_err(udev,
			"CSCD descriptor list length %u not a multiple of %u\n",
			(unsigned int)tdll, XCOPY_TARGET_DESC_LEN);
		return TCMU_STS_NOTSUPP_TGT_DESC_TYPE;
	}
	/* From spc4r36q,section 6.4.3.4 CSCD DESCRIPTOR LIST LENGTH field
	 * If the number of CSCD descriptors exceeds the allowed number, the copy
	 * manager shall terminate the command with CHECK CONDITION status, with
	 * the sense key set to ILLEGAL REQUEST, and the additional sense code
	 * set to TOO MANY TARGET DESCRIPTORS.
	 */
	if (tdll > RCR_OP_MAX_TARGET_DESC_COUNT * XCOPY_TARGET_DESC_LEN) {
		tcmu_dev_err(udev, "Only %u target descriptor(s) supported, but there are %u\n",
			     RCR_OP_MAX_TARGET_DESC_COUNT, tdll / XCOPY_TARGET_DESC_LEN);
		return TCMU_STS_TOO_MANY_TGT_DESC;
	}

	for (i = 0; tdll >= XCOPY_TARGET_DESC_LEN; i++) {
		/*
		 * Only Identification Descriptor Target Descriptor support
		 * for now.
		 */
		if (tgt_desc[0] == XCOPY_TARGET_DESC_TYPE_CODE_ID) {
			ret = xcopy_parse_target_id(udev, xcopy, tgt_desc, i);
			if (ret != TCMU_STS_OK)
				return ret;

			tgt_desc += XCOPY_TARGET_DESC_LEN;
			tdll -= XCOPY_TARGET_DESC_LEN;
		} else {
			tcmu_dev_err(udev, "Unsupport target descriptor type code 0x%x\n",
				     tgt_desc[0]);
			return TCMU_STS_NOTSUPP_TGT_DESC_TYPE;
		}
	}

	ret = TCMU_STS_CP_TGT_DEV_NOTCONN;
	if (xcopy->src_dev)
		ret = xcopy_locate_udev(udev->ctx, xcopy->dst_tid_wwn,
					&xcopy->dst_dev);
	else if (xcopy->dst_dev)
		ret = xcopy_locate_udev(udev->ctx, xcopy->src_tid_wwn,
					&xcopy->src_dev);

	if (ret) {
		tcmu_err("Target device not found, the index are %hu and %hu\n",
			 xcopy->stdi, xcopy->dtdi);
		return TCMU_STS_CP_TGT_DEV_NOTCONN;
	}

	tcmu_dev_dbg(xcopy->src_dev, "Source device NAA IEEE WWN: 0x%16phN\n",
		     xcopy->src_tid_wwn);
	tcmu_dev_dbg(xcopy->dst_dev, "Destination device NAA IEEE WWN: 0x%16phN\n",
		     xcopy->dst_tid_wwn);

	return TCMU_STS_OK;
}

static int xcopy_parse_parameter_list(struct tcmu_device *dev,
				      struct tcmulib_cmd *cmd,
				      struct xcopy *xcopy)
{
	uint8_t *cdb = cmd->cdb;
	size_t data_length = tcmu_cdb_get_xfer_length(cdb);
	struct iovec *iovec = cmd->iovec;
	size_t iov_cnt = cmd->iov_cnt;
	uint32_t inline_dl;
	uint8_t *seg_desc, *tgt_desc, *par;
	uint16_t sdll, tdll;
	uint64_t num_lbas;
	int ret;

	/*
	 * The PARAMETER LIST LENGTH field specifies the length in bytes
	 * of the parameter data that shall be contained in the Data-Out
	 * Buffer.
	*/
	par = calloc(1, data_length);
	if (!par) {
		tcmu_dev_err(dev, "calloc parameter list buffer error\n");
		return TCMU_STS_NO_RESOURCE;
	}

	tcmu_memcpy_from_iovec(par, data_length, iovec, iov_cnt);

	/*
	 * From spc4r31, section 6.18.4 OPERATING PARAMETERS service action
	 *
	 * A supports no list identifier (SNLID) bit set to one indicates
	 * the copy manager supports an EXTENDED COPY (see 6.3) command
	 * parameter list in which the LIST ID USAGE field is set to 11b
	 * and the LIST IDENTIFIER field is set to zero as described in
	 * table 105 (see 6.3.1).
	 *
	 * From spc4r31, section 6.3.1 EXTENDED COPY command introduction
	 *
	 * LIST ID USAGE == 11b, then the LIST IDENTIFIER field should be
	 * as zero.
	 */
	tcmu_dev_dbg(dev, "LIST ID USAGE: 0x%x, LIST IDENTIFIER: 0x%x\n",
		     (par[1] & 0x18) >> 3, par[0]);
	if ((par[1] & 0x18) != 0x18 || par[0]) {
		tcmu_dev_err(dev, "LIST ID USAGE: 0x%x, LIST IDENTIFIER: 0x%x\n",
			     (par[1] & 0x18) >> 3, par[0]);
		ret = TCMU_STS_INVALID_PARAM_LIST;
		goto err;
	}

	/*
	 * From spc4r31, section 6.3.6.1 Target descriptors introduction
	 *
	 * All target descriptors (see table 108) are 32 bytes or 64 bytes
	 * in length
	 * From spc4r36q, section6.4.3.4
	 * An EXTENDED COPY command may reference one or more CSCDs.
	 */
	tdll = be16toh(*(uint16_t *)&par[2]);
	if (tdll < 32 || tdll % 32 != 0) {
		tcmu_dev_err(dev, "Illegal target descriptor length %u\n",
			     tdll);
		ret = TCMU_STS_INVALID_PARAM_LIST_LEN;
		goto err;
	}

	/*
	 * From spc4r31, section 6.3.7.1 Segment descriptors introduction
	 *
	 * Segment descriptors (see table 120) begin with an eight byte header.
	 */
	sdll = be32toh(*(uint32_t *)&par[8]);
	if (sdll < 8) {
		tcmu_dev_err(dev, "Illegal segment descriptor length %u\n",
			     tdll);
		ret = TCMU_STS_INVALID_PARAM_LIST_LEN;
		goto err;
	}

	/*
	 * The maximum length of the target and segment descriptors permitted
	 * within a parameter list is indicated by the MAXIMUM DESCRIPTOR LIST
	 * LENGTH field in the copy managers operating parameters.
	 */
	if (tdll + sdll > RCR_OP_MAX_DESC_LIST_LEN) {
		tcmu_dev_err(dev, "descriptor list length %u exceeds maximum %u\n",
			     tdll + sdll, RCR_OP_MAX_DESC_LIST_LEN);
		ret = TCMU_STS_INVALID_PARAM_LIST_LEN;
		goto err;
	}

	/*
	 * The INLINE DATA LENGTH field contains the number of bytes of inline
	 * data, after the last segment descriptor.
	 * */
	inline_dl = be32toh(*(uint32_t *)&par[12]);
	if (inline_dl != 0) {
		tcmu_dev_err(dev, "non-zero xcopy inline_dl %u unsupported\n",
			     inline_dl);
		ret = TCMU_STS_INVALID_PARAM_LIST_LEN;
		goto err;
	}

	/* From spc4r31, section 6.3.1 EXTENDED COPY command introduction
	 *
	 * The EXTENDED COPY parameter list (see table 104) begins with a 16
	 * byte header.
	 *
	 * The data length in CDB should be equal to tdll + sdll + inline_dl
	 * + parameter list header length
	 */
	if (data_length < (XCOPY_HDR_LEN + tdll + sdll + inline_dl)) {
		tcmu_dev_err(dev, "Illegal list length: length from CDB is %zu,"
			     " but here the length is %u\n",
			     data_length, tdll + sdll + inline_dl);
		ret = TCMU_STS_INVALID_PARAM_LIST_LEN;
		goto err;
	}

	tcmu_dev_dbg(dev, "Processing XCOPY with tdll: %hu sdll: %u inline_dl: %u\n",
		     tdll, sdll, inline_dl);

	/*
	 * Parse the segment descripters and for now we only support block
	 * -> block type.
	 *
	 * The max seg_desc number support is 1(see RCR_OP_MAX_SG_DESC_COUNT)
	 */
	seg_desc = par + XCOPY_HDR_LEN + tdll;
	ret = xcopy_parse_segment_descs(seg_desc, xcopy, sdll);
	if (ret != TCMU_STS_OK)
		goto err;

	/*
	 * Parse the target descripter
	 *
	 * The max seg_desc number support is 2(see RCR_OP_MAX_TARGET_DESC_COUNT)
	 */
	tgt_desc = par + XCOPY_HDR_LEN;
	ret = xcopy_parse_target_descs(dev, xcopy, tgt_desc, tdll);
	if (ret != TCMU_STS_OK)
		goto err;

	/*
	 * tcmu-runner can't determine whether the device(s) referred to in an
	 * XCOPY request should be accessible to the initiator via transport
	 * settings, ACLs, etc. XXX Consequently, we need to fail any
	 * cross-device requests for safety reasons.
	 */
	if (dev != xcopy->src_dev || dev != xcopy->dst_dev) {
		tcmu_dev_err(dev, "Cross-device XCOPY not supported\n");
		ret = TCMU_STS_CP_TGT_DEV_NOTCONN;
		goto err;
	}

	if (tcmu_dev_get_block_size(xcopy->src_dev) !=
	    tcmu_dev_get_block_size(xcopy->dst_dev)) {
		tcmu_dev_err(dev, "The block size of src dev %u != dst dev %u\n",
			     tcmu_dev_get_block_size(xcopy->src_dev),
			     tcmu_dev_get_block_size(xcopy->dst_dev));
		ret = TCMU_STS_INVALID_CP_TGT_DEV_TYPE;
		goto err;
	}

	num_lbas = tcmu_dev_get_num_lbas(xcopy->src_dev);
	if (xcopy->src_lba + xcopy->lba_cnt > num_lbas) {
		tcmu_dev_err(xcopy->src_dev,
			     "src target exceeds last lba %"PRIu64" (lba %"PRIu64", copy len %u\n",
			     num_lbas, xcopy->src_lba, xcopy->lba_cnt);
		ret = TCMU_STS_RANGE;
		goto err;
	}

	num_lbas = tcmu_dev_get_num_lbas(xcopy->dst_dev);
	if (xcopy->dst_lba + xcopy->lba_cnt > num_lbas) {
		tcmu_dev_err(xcopy->dst_dev,
			     "dst target exceeds last lba %"PRIu64" (lba %"PRIu64", copy len %u)\n",
			     num_lbas, xcopy->dst_lba, xcopy->lba_cnt);
		ret = TCMU_STS_RANGE;
		goto err;
	}

	free(par);
	return TCMU_STS_OK;

err:
	free(par);

	return ret;
}

static int xcopy_read_work_fn(struct tcmu_device *src_dev, void *data);
static void handle_xcopy_read_cbk(struct tcmu_device *src_dev,
				  struct tcmur_cmd *tcmur_cmd, int ret);

static void handle_xcopy_write_cbk(struct tcmu_device *dst_dev,
				  struct tcmur_cmd *tcmur_cmd, int ret)
{
	struct xcopy *xcopy = tcmur_cmd->cmd_state;
	struct tcmu_device *src_dev = xcopy->src_dev;

	/* write failed - bail out */
	if (ret != TCMU_STS_OK) {
		tcmu_dev_err(src_dev, "Failed to write to dst device!\n");
		goto out;
	}

	xcopy->lba_cnt -= xcopy->copy_lbas;
	if (!xcopy->lba_cnt)
		goto out;

	xcopy->src_lba += xcopy->copy_lbas;
	xcopy->dst_lba += xcopy->copy_lbas;
	xcopy->copy_lbas = min(xcopy->lba_cnt, xcopy->copy_lbas);
	tcmur_cmd->requested = tcmu_lba_to_byte(src_dev, xcopy->copy_lbas);

	tcmur_cmd->done = handle_xcopy_read_cbk;
	ret = aio_request_schedule(xcopy->src_dev, tcmur_cmd,
				   xcopy_read_work_fn, tcmur_cmd_complete);
	if (ret != TCMU_STS_ASYNC_HANDLED)
		goto out;

	return;

out:
	aio_command_finish(xcopy->origdev, tcmur_cmd->lib_cmd, ret);
	tcmur_cmd_state_free(tcmur_cmd);
}

static int xcopy_write_work_fn(struct tcmu_device *dst_dev, void *data)
{
	struct tcmur_handler *rhandler = tcmu_get_runner_handler(dst_dev);
	struct tcmur_cmd *tcmur_cmd = data;
	struct xcopy *xcopy = tcmur_cmd->cmd_state;

	tcmur_cmd_iovec_reset(tcmur_cmd, tcmur_cmd->requested);

	return rhandler->write(dst_dev, tcmur_cmd, tcmur_cmd->iovec,
			       tcmur_cmd->iov_cnt, tcmur_cmd->requested,
			       tcmu_lba_to_byte(dst_dev, xcopy->dst_lba));
}

static void handle_xcopy_read_cbk(struct tcmu_device *src_dev,
				  struct tcmur_cmd *tcmur_cmd,
				  int ret)
{
	struct xcopy *xcopy = tcmur_cmd->cmd_state;

	/* read failed - bail out */
	if (ret != TCMU_STS_OK) {
		tcmu_dev_err(src_dev, "Failed to read from src device!\n");
		goto err;
	}

	tcmur_cmd->done = handle_xcopy_write_cbk;

	ret = aio_request_schedule(xcopy->dst_dev, tcmur_cmd,
				   xcopy_write_work_fn, tcmur_cmd_complete);
	if (ret != TCMU_STS_ASYNC_HANDLED)
		goto err;

	return;

err:
	aio_command_finish(xcopy->origdev, tcmur_cmd->lib_cmd, ret);
	tcmur_cmd_state_free(tcmur_cmd);
}

static int xcopy_read_work_fn(struct tcmu_device *src_dev, void *data)
{
	struct tcmur_handler *rhandler = tcmu_get_runner_handler(src_dev);
	struct tcmur_cmd *tcmur_cmd = data;
	struct xcopy *xcopy = tcmur_cmd->cmd_state;

	tcmu_dev_dbg(src_dev,
		     "Copying %u sectors from src (lba:%"PRIu64") to dst (lba:%"PRIu64")\n",
		     xcopy->copy_lbas, xcopy->src_lba, xcopy->dst_lba);

	tcmur_cmd_iovec_reset(tcmur_cmd, tcmur_cmd->requested);

	return rhandler->read(src_dev, tcmur_cmd, tcmur_cmd->iovec,
			      tcmur_cmd->iov_cnt, tcmur_cmd->requested,
			      tcmu_lba_to_byte(src_dev, xcopy->src_lba));
}

/* async xcopy */
static int handle_xcopy(struct tcmu_device *dev, struct tcmulib_cmd *cmd)
{
	struct tcmur_cmd *tcmur_cmd = cmd->hm_private;
	uint8_t *cdb = cmd->cdb;
	size_t data_length = tcmu_cdb_get_xfer_length(cdb);
	uint32_t max_sectors, src_max_sectors, dst_max_sectors;
	struct xcopy *xcopy, xcopy_parse;
	int ret;

	/* spc4r36q section6.4 and 6.5
	 * EXTENDED_COPY(LID4) :service action 0x01;
	 * EXTENDED_COPY(LID1) :service action 0x00.
	 */
	if ((cdb[1] & 0x1f) != 0x00) {
		tcmu_dev_err(dev, "EXTENDED_COPY(LID4) not supported\n");
		return TCMU_STS_INVALID_CMD;
	}
	/*
	 * A parameter list length of zero specifies that copy manager
	 * shall not transfer any data or alter any internal state.
	 */
	if (data_length == 0)
		return TCMU_STS_OK;

	/*
	 * The EXTENDED COPY parameter list begins with a 16 byte header
	 * that contains the LIST IDENTIFIER field.
	 */
	if (data_length < XCOPY_HDR_LEN) {
		tcmu_dev_err(dev, "Illegal parameter list: length %zu < hdr_len %u\n",
			     data_length, XCOPY_HDR_LEN);
		return TCMU_STS_INVALID_PARAM_LIST_LEN;
	}

	memset(&xcopy_parse, 0, sizeof(xcopy_parse));
	/* Parse and check the parameter list */
	ret = xcopy_parse_parameter_list(dev, cmd, &xcopy_parse);
	if (ret != 0)
		return ret;

	/* Nothing to do with BLOCK DEVICE NUMBER OF BLOCKS set to zero */
	if (!xcopy_parse.lba_cnt)
		return TCMU_STS_OK;

	src_max_sectors = tcmu_dev_get_opt_xcopy_rw_len(xcopy_parse.src_dev);
	dst_max_sectors = tcmu_dev_get_opt_xcopy_rw_len(xcopy_parse.dst_dev);

	max_sectors = min(src_max_sectors, dst_max_sectors);
	xcopy_parse.copy_lbas = min(max_sectors, xcopy_parse.lba_cnt);

	if (tcmur_cmd_state_init(tcmur_cmd, sizeof(*xcopy),
				 tcmu_lba_to_byte(xcopy_parse.src_dev,
						  xcopy_parse.copy_lbas))) {
		tcmu_dev_err(dev, "calloc xcopy data error\n");
		return TCMU_STS_NO_RESOURCE;
	}
	tcmur_cmd->done = handle_xcopy_read_cbk;

	xcopy = tcmur_cmd->cmd_state;
	memcpy(xcopy, &xcopy_parse, sizeof(*xcopy));
	xcopy->origdev = dev;

	ret = aio_request_schedule(xcopy->src_dev, tcmur_cmd,
				   xcopy_read_work_fn, tcmur_cmd_complete);
	if (ret == TCMU_STS_ASYNC_HANDLED)
		return ret;

	tcmur_cmd_state_free(tcmur_cmd);
	return ret;
}

/* async compare_and_write */

static void handle_caw_write_cbk(struct tcmu_device *dev,
				 struct tcmur_cmd *tcmur_cmd, int ret)
{
	struct tcmur_device *rdev = tcmu_dev_get_private(dev);
	struct tcmulib_cmd *cmd = tcmur_cmd->lib_cmd;

	pthread_mutex_unlock(&rdev->caw_lock);
	tcmur_cmd_state_free(tcmur_cmd);
	aio_command_finish(dev, cmd, ret);
}

static int caw_work_fn(struct tcmu_device *dev, void *data)
{
	struct tcmur_handler *rhandler = tcmu_get_runner_handler(dev);
	struct tcmur_cmd *tcmur_cmd = data;
	struct tcmulib_cmd *cmd = tcmur_cmd->lib_cmd;

	if (tcmur_cmd->done == handle_caw_write_cbk) {
		return rhandler->write(dev, tcmur_cmd, cmd->iovec, cmd->iov_cnt,
				       tcmur_cmd->requested,
				       tcmu_cdb_to_byte(dev, cmd->cdb));

	} else {
		return rhandler->read(dev, tcmur_cmd, tcmur_cmd->iovec,
				       tcmur_cmd->iov_cnt, tcmur_cmd->requested,
				       tcmu_cdb_to_byte(dev, cmd->cdb));
	}
}

static void handle_caw_read_cbk(struct tcmu_device *dev,
				struct tcmur_cmd *tcmur_cmd, int ret)
{
	struct tcmur_device *rdev = tcmu_dev_get_private(dev);
	struct tcmulib_cmd *cmd = tcmur_cmd->lib_cmd;
	uint32_t cmp_offset;

	/* read failed - bail out */
	if (ret != TCMU_STS_OK)
		goto finish_err;

	cmp_offset = tcmu_iovec_compare(tcmur_cmd->iov_base_copy, cmd->iovec,
					tcmur_cmd->requested);
	if (cmp_offset != -1) {
		/* verify failed - bail out */
		ret = TCMU_STS_MISCOMPARE;
		tcmu_sense_set_info(cmd->sense_buf, cmp_offset);
		goto finish_err;
	}

	/* perform write */
	tcmu_cmd_seek(cmd, tcmur_cmd->requested);
	tcmur_cmd->done = handle_caw_write_cbk;

	ret = aio_request_schedule(dev, tcmur_cmd, caw_work_fn,
				   tcmur_cmd_complete);
	if (ret != TCMU_STS_ASYNC_HANDLED)
		goto finish_err;

	return;

finish_err:
	pthread_mutex_unlock(&rdev->caw_lock);
	tcmur_cmd_state_free(tcmur_cmd);
	aio_command_finish(dev, cmd, ret);
}

static int handle_caw_check(struct tcmu_device *dev, struct tcmulib_cmd *cmd)
{
	int ret;
	uint64_t start_lba = tcmu_cdb_get_lba(cmd->cdb);
	uint8_t sectors = cmd->cdb[13];

	/* From sbc4r12a section 5.3 COMPARE AND WRITE command
	 * If the number of logical blocks exceeds the value in the
	 * MAXIMUM COMPARE AND WRITE LENGTH field(see 6.64 block limits VPD page)
	 * then the device server shall terminate the command with CHECK CONDITION
	 * status with the sense key set to ILLEGAL REQUEST and the additional sense
	 * code set to INVALID FIELD IN CDB.
	 */
	if (sectors > MAX_CAW_LENGTH) {
		tcmu_dev_err(dev, "Received caw length %u greater than max caw length %u.\n",
			     sectors, MAX_CAW_LENGTH);
		return TCMU_STS_INVALID_CDB;
	}
	/* double sectors since we have two buffers */
	ret = check_iovec_length(dev, cmd, sectors * 2);
	if (ret)
		return ret;

	ret = check_lbas(dev, start_lba, sectors);
	if (ret)
		return ret;

	return TCMU_STS_OK;
}

static int tcmur_caw_fn(struct tcmu_device *dev, void *data)
{
	struct tcmur_cmd *tcmur_cmd = data;
	struct tcmulib_cmd *cmd = tcmur_cmd->lib_cmd;
	tcmur_caw_fn_t caw_fn = tcmur_cmd->cmd_state;
	uint64_t off = tcmu_cdb_to_byte(dev, cmd->cdb);
	size_t half = (tcmu_iovec_length(cmd->iovec, cmd->iov_cnt)) / 2;

	return caw_fn(dev, tcmur_cmd, off, half, cmd->iovec, cmd->iov_cnt);
}

static int handle_caw(struct tcmu_device *dev, struct tcmulib_cmd *cmd)
{
	struct tcmur_handler *rhandler = tcmu_get_runner_handler(dev);
	struct tcmur_cmd *tcmur_cmd = cmd->hm_private;
	size_t half = (tcmu_iovec_length(cmd->iovec, cmd->iov_cnt)) / 2;
	struct tcmur_device *rdev = tcmu_dev_get_private(dev);
	uint8_t sectors = cmd->cdb[13];
	int ret;

	if (tcmu_dev_in_recovery(dev))
		return TCMU_STS_BUSY;

	ret = alua_check_state(dev, cmd, false);
	if (ret)
		return ret;

	/* From sbc4r12a section 5.3 COMPARE AND WRITE command
	 * A NUMBER OF LOGICAL BLOCKS field set to zero specifies that no
	 * read operations shall be performed, no logical block data shall
	 * be transferred from the Data-Out Buffer, no compare operations
	 * shall be performed, and no write operations shall be performed.
	 * This condition shall not be considered an error.
	 */
	if (!sectors) {
		tcmu_dev_dbg(dev, "NUMBER OF LOGICAL BLOCKS is zero, just return ok.\n");
		return TCMU_STS_OK;
	}

	ret = handle_caw_check(dev, cmd);
	if (ret)
		return ret;

	if (rhandler->caw) {
		tcmur_cmd->cmd_state = rhandler->caw;
		tcmur_cmd->done = handle_generic_cbk;
		return aio_request_schedule(dev, tcmur_cmd, tcmur_caw_fn,
					    tcmur_cmd_complete);
	}

	if (tcmur_cmd_state_init(tcmur_cmd, 0, half))
		return TCMU_STS_NO_RESOURCE;

	tcmur_cmd->done = handle_caw_read_cbk;

	pthread_mutex_lock(&rdev->caw_lock);

	ret = aio_request_schedule(dev, tcmur_cmd, caw_work_fn,
				   tcmur_cmd_complete);
	if (ret == TCMU_STS_ASYNC_HANDLED)
		return TCMU_STS_ASYNC_HANDLED;

	pthread_mutex_unlock(&rdev->caw_lock);
	tcmur_cmd_state_free(tcmur_cmd);
	return ret;
}

/* async flush */
static int flush_work_fn(struct tcmu_device *dev, void *data)
{
	struct tcmur_handler *rhandler = tcmu_get_runner_handler(dev);

	return rhandler->flush(dev, data);
}

static int handle_flush(struct tcmu_device *dev, struct tcmulib_cmd *cmd)
{
	struct tcmur_handler *rhandler = tcmu_get_runner_handler(dev);
	struct tcmur_cmd *tcmur_cmd = cmd->hm_private;

	if (!rhandler->flush)
		return TCMU_STS_INVALID_CMD;

	tcmur_cmd->done = handle_generic_cbk;
	return aio_request_schedule(dev, tcmur_cmd, flush_work_fn,
				    tcmur_cmd_complete);
}

static int handle_recv_copy_result(struct tcmu_device *dev, struct tcmulib_cmd *cmd)
{
	struct iovec *iovec = cmd->iovec;
	size_t iov_cnt = cmd->iov_cnt;
	uint8_t buf[128];
	uint16_t val16;
	uint32_t val32;

	memset(buf, 0, sizeof(buf));

	/*
	 * From spc4r31, section 6.18.4 OPERATING PARAMETERS service
	 * action
	 */

	/*
	 * SNLID = 1: the copy manager will support an EXTENDED COPY
	 * command parameter list in which the LIST ID USAGE field is
	 * set to 11b
	 */
	buf[4] = 0x01;

	/*
	 * MAXIMUM TARGET COUNT: the max number of target descriptors
	 * that the copy manager allows in a single EXTENDED COPY
	 * target descriptor list.
	 */
	val16 = htobe16(RCR_OP_MAX_TARGET_DESC_COUNT);
	memcpy(&buf[8], &val16, 2);

	/*
	 * MAXIMUM SEGMENT COUNT: the max number of segment descriptors
	 * that the copy manager allows in a single EXTENDED COPY
	 * segment descriptor list.
	 */
	val16 = htobe16(RCR_OP_MAX_SEGMENT_DESC_COUNT);
	memcpy(&buf[10], &val16, 2);

	/*
	 * MAXIMUM DESCRIPTOR LIST LENGTH: the max length, in bytes,
	 * of the target descriptor list and segment descriptor list.
	 */
	val32 = htobe32(RCR_OP_MAX_DESC_LIST_LEN);
	memcpy(&buf[12], &val32, 4);

	/*
	 * MAXIMUM SEGMENT LENGTH: the length, in bytes, of the largest
	 * amount of data that the copy manager supports writing via a
	 * single segment.
	 */
	val32 = htobe32(RCR_OP_MAX_SEGMENT_LEN);
	memcpy(&buf[16], &val32, 4);

	/*
	 * MAXIMUM CONCURRENT COPIES: the max number of EXTENDED COPY
	 * commands with the LIST ID USAGE field set to 00b or 10b that
	 * are supported for concurrent processing by the copy manager.
	 */
	val16 = htobe16(RCR_OP_TOTAL_CONCURR_COPIES);
	memcpy(&buf[34], &val16, 2);

	/*
	 * MAXIMUM CONCURRENT COPIES: the max number of EXTENDED COPY
	 * commands with the LIST ID USAGE field set to 00b or 10b that
	 * are supported for concurrent processing by the copy manager.
	 */
	buf[36] = RCR_OP_MAX_CONCURR_COPIES;

	/*
	 * DATA SEGMENT GRANULARITY: the length of the smallest data
	 * block that copy manager permits in a non-inline segment
	 * descriptor. In power of two.
	 */
	buf[37] = RCR_OP_DATA_SEG_GRAN_LOG2;

	/*
	 * INLINE DATA GRANULARITY: the length of the of the smallest
	 * block of inline data that the copy manager permits being
	 * written by a segment descriptor containing the 04h descriptor
	 * type code (see 6.3.7.7). In power of two.
	 */
	buf[38] = RCR_OP_INLINE_DATA_GRAN_LOG2;

	/*
	 * HELD DATA GRANULARITY: the length of the smallest block of
	 * held data that the copy manager shall transfer to the
	 * application client in response to a RECEIVE COPY RESULTS
	 * command with RECEIVE DATA service action (see 6.18.3).
	 * In power of two.
	 */
	buf[39] = RCR_OP_HELD_DATA_GRAN_LOG2;

	/*
	 * IMPLEMENTED DESCRIPTOR LIST LENGTH: the length, in bytes, of
	 * the list of implemented descriptor type codes.
	 */
	buf[43] = RCR_OP_IMPLE_DES_LIST_LENGTH;

	/*
	 * The list of implemented descriptor type codes: one byte for
	 * each segment or target DESCRIPTOR TYPE CODE value (see 6.3.5)
	 * supported by the copy manager,
	 */
	buf[44] = XCOPY_SEG_DESC_TYPE_CODE_B2B; /* block --> block */
	buf[45] = XCOPY_TARGET_DESC_TYPE_CODE_ID; /* Identification descriptor */

	/* AVAILABLE DATA (n-3)*/
	val32 = htobe32(42);
	memcpy(&buf[0], &val32, 4);

	tcmu_memcpy_into_iovec(iovec, iov_cnt, buf, sizeof(buf));

	return TCMU_STS_OK;
}

/* async write */
static int handle_write(struct tcmu_device *dev, struct tcmulib_cmd *cmd)
{
	struct tcmur_cmd *tcmur_cmd = cmd->hm_private;
	int ret;

	ret = check_lba_and_length(dev, cmd, tcmu_cdb_get_xfer_length(cmd->cdb));
	if (ret)
		return ret;

	tcmur_cmd->done = handle_generic_cbk;
	return aio_request_schedule(dev, tcmur_cmd, write_work_fn,
				    tcmur_cmd_complete);
}

/* async read */
static int handle_read(struct tcmu_device *dev, struct tcmulib_cmd *cmd)
{
	struct tcmur_cmd *tcmur_cmd = cmd->hm_private;
	int ret;

	ret = check_lba_and_length(dev, cmd, tcmu_cdb_get_xfer_length(cmd->cdb));
	if (ret)
		return ret;

	tcmur_cmd->done = handle_generic_cbk;
	return aio_request_schedule(dev, tcmur_cmd, read_work_fn,
				    tcmur_cmd_complete);
}

/* FORMAT UNIT */
struct format_unit_state {
	size_t length;
	off_t offset;
	uint32_t done_blocks;
};

static int format_unit_work_fn(struct tcmu_device *dev, void *data)
{
	struct tcmur_handler *rhandler = tcmu_get_runner_handler(dev);
	struct tcmur_cmd *tcmur_cmd = data;
	struct format_unit_state *state = tcmur_cmd->cmd_state;

	return rhandler->write(dev, tcmur_cmd, tcmur_cmd->iovec,
			       tcmur_cmd->iov_cnt, tcmur_cmd->requested,
			       state->offset);
}

static void handle_format_unit_cbk(struct tcmu_device *dev,
				   struct tcmur_cmd *tcmur_cmd, int ret) {
	struct tcmur_device *rdev = tcmu_dev_get_private(dev);
	struct tcmulib_cmd *cmd = tcmur_cmd->lib_cmd;
	struct format_unit_state *state = tcmur_cmd->cmd_state;
	uint32_t block_size = tcmu_dev_get_block_size(dev);
	int rc;

	state->offset += tcmur_cmd->requested;
	state->done_blocks += tcmu_byte_to_lba(dev, tcmur_cmd->requested);
	if (state->done_blocks < dev->num_lbas)
		rdev->format_progress = (0x10000 * state->done_blocks) /
				       dev->num_lbas;

	/* Check for last commmand */
	if (state->done_blocks == dev->num_lbas) {
		tcmu_dev_dbg(dev,
			     "last format cmd, done_blocks:%u num_lbas:%"PRIu64" block_size:%u\n",
			     state->done_blocks, dev->num_lbas, block_size);
		goto free_state;
	}

	if (state->done_blocks < dev->num_lbas) {
		size_t left = tcmu_lba_to_byte(dev,
					       dev->num_lbas - state->done_blocks);
		if (left < tcmur_cmd->requested)
			tcmur_cmd->requested = left;

		/* Seek in handlers consume the iovec, thus we must reset */
		tcmur_cmd_iovec_reset(tcmur_cmd, tcmur_cmd->requested);

		tcmu_dev_dbg(dev,
			     "next format cmd, done_blocks:%u num_lbas:%"PRIu64" block_size:%u\n",
			     state->done_blocks, dev->num_lbas, block_size);

		rc = aio_request_schedule(dev, tcmur_cmd, format_unit_work_fn,
					  tcmur_cmd_complete);
		if (rc != TCMU_STS_ASYNC_HANDLED) {
			tcmu_dev_err(dev, " async handle cmd failure\n");
			ret = TCMU_STS_WR_ERR;
			goto free_state;
		}
	}

	return;

free_state:
	tcmur_cmd_state_free(tcmur_cmd);
	pthread_mutex_lock(&rdev->format_lock);
	rdev->flags &= ~TCMUR_DEV_FLAG_FORMATTING;
	pthread_mutex_unlock(&rdev->format_lock);
	aio_command_finish(dev, cmd, ret);
}

static int handle_format_unit(struct tcmu_device *dev, struct tcmulib_cmd *cmd) {
	struct tcmur_device *rdev = tcmu_dev_get_private(dev);
	struct tcmur_cmd *tcmur_cmd = cmd->hm_private;
	size_t max_xfer_length, length = 1024 * 1024;
	uint32_t block_size = tcmu_dev_get_block_size(dev);
	uint64_t num_lbas = tcmu_dev_get_num_lbas(dev);
	int ret;

	pthread_mutex_lock(&rdev->format_lock);
	if (rdev->flags & TCMUR_DEV_FLAG_FORMATTING) {
		pthread_mutex_unlock(&rdev->format_lock);
		tcmu_sense_set_key_specific_info(cmd->sense_buf,
						 rdev->format_progress);
		return TCMU_STS_FRMT_IN_PROGRESS;
	}
	rdev->format_progress = 0;
	rdev->flags |= TCMUR_DEV_FLAG_FORMATTING;
	pthread_mutex_unlock(&rdev->format_lock);

	max_xfer_length = tcmu_dev_get_max_xfer_len(dev) * block_size;
	length = round_up(length, max_xfer_length);
	/* Check length on first write to make sure its not less than 1MB */
	if (tcmu_lba_to_byte(dev, num_lbas) < length)
		length = tcmu_lba_to_byte(dev, num_lbas);

	if (tcmur_cmd_state_init(tcmur_cmd, sizeof(struct format_unit_state),
				 length))
		goto clear_format;
	tcmur_cmd->done = handle_format_unit_cbk;

	tcmu_dev_dbg(dev, "start emulate format, num_lbas:%"PRIu64" block_size:%u\n",
		     num_lbas, block_size);

	ret = aio_request_schedule(dev, tcmur_cmd, format_unit_work_fn,
				   tcmur_cmd_complete);
	if (ret != TCMU_STS_ASYNC_HANDLED)
		goto free_state;

	return TCMU_STS_ASYNC_HANDLED;

free_state:
	tcmur_cmd_state_free(tcmur_cmd);
clear_format:
	pthread_mutex_lock(&rdev->format_lock);
	rdev->flags &= ~TCMUR_DEV_FLAG_FORMATTING;
	pthread_mutex_unlock(&rdev->format_lock);
	return TCMU_STS_NO_RESOURCE;
}

/* ALUA */
static int handle_stpg(struct tcmu_device *dev, struct tcmulib_cmd *cmd)
{
	struct list_head group_list;
	int ret;

	list_head_init(&group_list);

	if (tcmu_get_alua_grps(dev, &group_list))
		return TCMU_STS_HW_ERR;

	ret = tcmu_emulate_set_tgt_port_grps(dev, &group_list, cmd);
	tcmu_release_alua_grps(&group_list);
	return ret;
}

static int handle_rtpg(struct tcmu_device *dev, struct tcmulib_cmd *cmd)
{
	struct list_head group_list;
	int ret;

	list_head_init(&group_list);

	if (tcmu_get_alua_grps(dev, &group_list))
		return TCMU_STS_HW_ERR;

	ret = tcmu_emulate_report_tgt_port_grps(dev, &group_list, cmd);
	tcmu_release_alua_grps(&group_list);
	return ret;
}

/* command passthrough */
static int passthrough_work_fn(struct tcmu_device *dev, void *data)
{
	struct tcmur_handler *rhandler = tcmu_get_runner_handler(dev);

	return rhandler->handle_cmd(dev, data);
}

static int handle_passthrough(struct tcmu_device *dev,
			      struct tcmur_cmd *tcmur_cmd)
{
	tcmur_cmd->done = handle_generic_cbk;
	return aio_request_schedule(dev, tcmur_cmd, passthrough_work_fn,
				    tcmur_cmd_complete);
}

bool tcmur_handler_is_passthrough_only(struct tcmur_handler *rhandler)
{
	if (rhandler->write || rhandler->read || rhandler->flush)
		return false;

	return true;
}

int tcmur_cmd_passthrough_handler(struct tcmu_device *dev,
				  struct tcmulib_cmd *cmd)
{
	struct tcmur_handler *rhandler = tcmu_get_runner_handler(dev);
	struct tcmur_device *rdev = tcmu_dev_get_private(dev);
	struct tcmur_cmd *tcmur_cmd = cmd->hm_private;
	int ret;

	if (!rhandler->handle_cmd)
		return TCMU_STS_NOT_HANDLED;

	/*
	 * Support handlers that implement their own threading/AIO
	 * and only use runner's main event loop.
	 */
	if (!rhandler->nr_threads)
		return rhandler->handle_cmd(dev, tcmur_cmd);
	/*
	 * Since we call ->handle_cmd via aio_request_schedule(), ->handle_cmd
	 * can finish in the callers context(asynchronous handler) or work
	 * queue context (synchronous handlers), thus we'd need to check if
	 * ->handle_cmd handled the passthough command here as well as in
	 * handle_passthrough_cbk().
	 */
	track_aio_request_start(rdev);
	ret = handle_passthrough(dev, tcmur_cmd);
	if (ret != TCMU_STS_ASYNC_HANDLED)
		track_aio_request_finish(rdev, NULL);

	return ret;
}

static int tcmur_cmd_handler(struct tcmu_device *dev, struct tcmulib_cmd *cmd)
{
	int ret = TCMU_STS_NOT_HANDLED;
	struct tcmur_handler *rhandler = tcmu_get_runner_handler(dev);
	struct tcmur_device *rdev = tcmu_dev_get_private(dev);
	uint8_t *cdb = cmd->cdb;
	bool is_read = false;

	track_aio_request_start(rdev);

	if (tcmu_dev_in_recovery(dev)) {
		ret = TCMU_STS_BUSY;
		goto untrack;
	}

	/* Don't perform alua implicit transition if command is not supported */
	switch(cdb[0]) {
	/* Skip to grab the lock for reads */
	case READ_6:
	case READ_10:
	case READ_12:
	case READ_16:
		is_read = true;
	case WRITE_6:
	case WRITE_10:
	case WRITE_12:
	case WRITE_16:
	case UNMAP:
	case SYNCHRONIZE_CACHE:
	case SYNCHRONIZE_CACHE_16:
	case EXTENDED_COPY:
	case COMPARE_AND_WRITE:
	case WRITE_VERIFY:
	case WRITE_VERIFY_16:
	case WRITE_SAME:
	case WRITE_SAME_16:
	case FORMAT_UNIT:
		ret = alua_check_state(dev, cmd, is_read);
		if (ret)
			goto untrack;
		break;
	default:
		break;
	}

	switch(cdb[0]) {
	case READ_6:
	case READ_10:
	case READ_12:
	case READ_16:
		ret = handle_read(dev, cmd);
		break;
	case WRITE_6:
	case WRITE_10:
	case WRITE_12:
	case WRITE_16:
		ret = handle_write(dev, cmd);
		break;
	case UNMAP:
		ret = handle_unmap(dev, cmd);
		break;
	case SYNCHRONIZE_CACHE:
	case SYNCHRONIZE_CACHE_16:
		if (rhandler->flush)
			ret = handle_flush(dev, cmd);
		break;
	case EXTENDED_COPY:
		ret = handle_xcopy(dev, cmd);
		break;
	case COMPARE_AND_WRITE:
		ret = handle_caw(dev, cmd);
		break;
	case WRITE_VERIFY:
	case WRITE_VERIFY_16:
		ret = handle_write_verify(dev, cmd);
		break;
	case WRITE_SAME:
	case WRITE_SAME_16:
		ret = handle_writesame(dev, cmd);
		break;
	case FORMAT_UNIT:
		ret = handle_format_unit(dev, cmd);
		break;
	default:
		ret = TCMU_STS_NOT_HANDLED;
	}

untrack:
	if (ret != TCMU_STS_ASYNC_HANDLED)
		track_aio_request_finish(rdev, NULL);
	return ret;
}

static int handle_inquiry(struct tcmu_device *dev, struct tcmulib_cmd *cmd)
{
	struct list_head group_list;
	struct tgt_port *port;
	int ret;

	list_head_init(&group_list);

	if (tcmu_get_alua_grps(dev, &group_list))
		return TCMU_STS_HW_ERR;

	port = tcmu_get_enabled_port(&group_list);
	if (!port) {
		tcmu_dev_dbg(dev, "no enabled ports found. Skipping ALUA support\n");
	} else {
		tcmu_update_dev_lock_state(dev);
	}

	ret = tcmu_emulate_inquiry(dev, port, cmd->cdb, cmd->iovec,
				   cmd->iov_cnt);
	tcmu_release_alua_grps(&group_list);
	return ret;
}

static int handle_sync_cmd(struct tcmu_device *dev, struct tcmulib_cmd *cmd)
{
	uint8_t *cdb = cmd->cdb;
	struct iovec *iovec = cmd->iovec;
	size_t iov_cnt = cmd->iov_cnt;
	uint32_t block_size = tcmu_dev_get_block_size(dev);
	uint64_t num_lbas = tcmu_dev_get_num_lbas(dev);

	switch (cdb[0]) {
	case INQUIRY:
		return handle_inquiry(dev, cmd);
	case TEST_UNIT_READY:
		return tcmu_emulate_test_unit_ready(cdb, iovec, iov_cnt);
	case SERVICE_ACTION_IN_16:
		if (cdb[1] == READ_CAPACITY_16)
			return tcmu_emulate_read_capacity_16(num_lbas,
							     block_size,
							     cdb, iovec,
							     iov_cnt);
		else
			return TCMU_STS_NOT_HANDLED;
	case READ_CAPACITY:
		if ((cdb[1] & 0x01) || (cdb[8] & 0x01))
			/* Reserved bits for MM logical units */
			return TCMU_STS_INVALID_CDB;
		else
			return tcmu_emulate_read_capacity_10(num_lbas,
							     block_size,
							     cdb, iovec,
							     iov_cnt);
	case MODE_SENSE:
	case MODE_SENSE_10:
		return tcmu_emulate_mode_sense(dev, cdb, iovec, iov_cnt);
	case START_STOP:
		return tcmu_emulate_start_stop(dev, cdb);
	case MODE_SELECT:
	case MODE_SELECT_10:
		return tcmu_emulate_mode_select(dev, cdb, iovec, iov_cnt);
	case RECEIVE_COPY_RESULTS:
		if ((cdb[1] & 0x1f) == RCR_SA_OPERATING_PARAMETERS)
			return handle_recv_copy_result(dev, cmd);
		return TCMU_STS_NOT_HANDLED;
	case MAINTENANCE_OUT:
		if (cdb[1] == MO_SET_TARGET_PGS)
			return handle_stpg(dev, cmd);
		return TCMU_STS_NOT_HANDLED;
	case MAINTENANCE_IN:
		if ((cdb[1] & 0x1f) == MI_REPORT_TARGET_PGS)
			return handle_rtpg(dev, cmd);
		return TCMU_STS_NOT_HANDLED;
	default:
		return TCMU_STS_NOT_HANDLED;
	}
}

static int handle_try_passthrough(struct tcmu_device *dev,
				  struct tcmulib_cmd *cmd)
{
	struct tcmur_handler *rhandler = tcmu_get_runner_handler(dev);
	struct tcmur_device *rdev = tcmu_dev_get_private(dev);
	struct tcmur_cmd *tcmur_cmd = cmd->hm_private;
	int ret;

	if (!rhandler->handle_cmd)
		return TCMU_STS_NOT_HANDLED;

	track_aio_request_start(rdev);

	ret = rhandler->handle_cmd(dev, tcmur_cmd);
	if (ret != TCMU_STS_ASYNC_HANDLED)
		track_aio_request_finish(rdev, NULL);

	return ret;
}

int tcmur_dev_update_size(struct tcmu_device *dev, uint64_t new_size)
{
	uint64_t old_size_lbas;
	int ret;

	if (!new_size)
		return -EINVAL;

	old_size_lbas = tcmu_dev_get_num_lbas(dev);

	tcmu_dev_set_num_lbas(dev, tcmu_byte_to_lba(dev, new_size));
	ret = tcmu_cfgfs_dev_set_ctrl_u64(dev, "dev_size", new_size);
	if (ret)
		tcmu_dev_set_num_lbas(dev, old_size_lbas);
	else
		tcmur_set_pending_ua(dev, TCMUR_UA_DEV_SIZE_CHANGED);
	return ret;
}

void tcmur_set_pending_ua(struct tcmu_device *dev, int ua)
{
	struct tcmur_device *rdev = tcmu_dev_get_private(dev);

	pthread_mutex_lock(&rdev->state_lock);
	rdev->pending_uas |= (1 << ua);
	pthread_mutex_unlock(&rdev->state_lock);
}

/*
 * TODO - coordinate with the kernel.
 */
static int handle_pending_ua(struct tcmur_device *rdev, struct tcmulib_cmd *cmd)
{
	uint8_t *cdb = cmd->cdb;
	int ret = TCMU_STS_NOT_HANDLED, ua;

	switch (cdb[0]) {
	case INQUIRY:
	case REQUEST_SENSE:
		/* The kernel will handle REPORT_LUNS */
		return TCMU_STS_NOT_HANDLED;
	}
	pthread_mutex_lock(&rdev->state_lock);

	if (!rdev->pending_uas) {
		ret = TCMU_STS_NOT_HANDLED;
		goto unlock;
	}

	ua = ffs(rdev->pending_uas) - 1;
	switch (ua) {
	case TCMUR_UA_DEV_SIZE_CHANGED:
		ret = TCMU_STS_CAPACITY_CHANGED;
		break;
	}
	rdev->pending_uas &= ~(1 << ua);

unlock:
	pthread_mutex_unlock(&rdev->state_lock);
	return ret;
}

int tcmur_generic_handle_cmd(struct tcmu_device *dev, struct tcmulib_cmd *cmd)
{
	struct tcmur_device *rdev = tcmu_dev_get_private(dev);
	int ret;

	ret = handle_pending_ua(rdev, cmd);
	if (ret != TCMU_STS_NOT_HANDLED)
		return ret;

	if (rdev->flags & TCMUR_DEV_FLAG_FORMATTING && cmd->cdb[0] != INQUIRY) {
		tcmu_sense_set_key_specific_info(cmd->sense_buf,
						 rdev->format_progress);
		return TCMU_STS_FRMT_IN_PROGRESS;
	}

	/*
	 * The handler want to handle some commands by itself,
	 * try to passthrough it first
	 */
	ret = handle_try_passthrough(dev, cmd);
	if (ret != TCMU_STS_NOT_HANDLED)
		return ret;

	/* Falls back to the runner's generic handle callout */
	ret = handle_sync_cmd(dev, cmd);
	if (ret == TCMU_STS_NOT_HANDLED)
		ret = tcmur_cmd_handler(dev, cmd);
	return ret;
}
