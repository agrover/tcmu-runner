/*
 * Copyright (c) 2014 Red Hat, Inc.
 *
 * This file is licensed to you under your choice of the GNU Lesser
 * General Public License, version 2.1 or any later version (LGPLv2.1 or
 * later), or the Apache License 2.0.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>
#include <sys/uio.h>
#include <string.h>
#include <scsi/scsi.h>
#include <endian.h>
#include <errno.h>
#include <assert.h>

#include "libtcmu_log.h"
#include "libtcmu_common.h"
#include "libtcmu_priv.h"
#include "be_byteshift.h"

int tcmu_cdb_get_length(uint8_t *cdb)
{
	uint8_t group_code = cdb[0] >> 5;

	/* See spc-4 4.2.5.1 operation code */
	switch (group_code) {
	case 0: /*000b for 6 bytes commands */
		return 6;
	case 1: /*001b for 10 bytes commands */
	case 2: /*010b for 10 bytes commands */
		return 10;
	case 3: /*011b Reserved ? */
		if (cdb[0] == 0x7f)
			return 8 + cdb[7];
		goto cdb_not_supp;
	case 4: /*100b for 16 bytes commands */
		return 16;
	case 5: /*101b for 12 bytes commands */
		return 12;
	case 6: /*110b Vendor Specific */
	case 7: /*111b Vendor Specific */
	default:
		/* TODO: */
		goto cdb_not_supp;
	}

cdb_not_supp:
	tcmu_err("CDB %x0x not supported.\n", cdb[0]);
	return -EINVAL;
}

uint64_t tcmu_cdb_get_lba(uint8_t *cdb)
{
	uint16_t val;

	switch (tcmu_cdb_get_length(cdb)) {
	case 6:
		val = be16toh(*((uint16_t *)&cdb[2]));
		return ((cdb[1] & 0x1f) << 16) | val;
	case 10:
		return be32toh(*((u_int32_t *)&cdb[2]));
	case 12:
		return be32toh(*((u_int32_t *)&cdb[2]));
	case 16:
		return be64toh(*((u_int64_t *)&cdb[2]));
	default:
		assert_perror(EINVAL);
		return 0;	/* not reached */
	}
}

uint32_t tcmu_cdb_get_xfer_length(uint8_t *cdb)
{
	switch (tcmu_cdb_get_length(cdb)) {
	case 6:
		return cdb[4];
	case 10:
		return be16toh(*((uint16_t *)&cdb[7]));
	case 12:
		return be32toh(*((u_int32_t *)&cdb[6]));
	case 16:
		return be32toh(*((u_int32_t *)&cdb[10]));
	default:
		assert_perror(EINVAL);
		return 0;	/* not reached */
	}
}

/*
 * Returns location of first mismatch between bytes in mem and the iovec.
 * If they are the same, return -1.
 */
off_t tcmu_iovec_compare(void *mem, struct iovec *iovec, size_t size)
{
	off_t mem_off;
	int ret;

	mem_off = 0;
	while (size) {
		size_t part = min(size, iovec->iov_len);

		ret = memcmp(mem + mem_off, iovec->iov_base, part);
		if (ret) {
			size_t pos;
			char *spos = mem + mem_off;
			char *dpos = iovec->iov_base;

			/*
			 * Data differed, this is assumed to be 'rare'
			 * so use a much more expensive byte-by-byte
			 * comparison to find out at which offset the
			 * data differs.
			 */
			for (pos = 0; pos < part && *spos++ == *dpos++;
			     pos++)
				;

			return pos + mem_off;
		}

		size -= part;
		mem_off += part;
		iovec++;
	}

	return -1;
}

/*
 * Consume an iovec. Count must not exceed the total iovec[] size.
 */
size_t tcmu_iovec_seek(struct iovec *iovec, size_t count)
{
	size_t consumed = 0;

	while (count) {
		if (count >= iovec->iov_len) {
			count -= iovec->iov_len;
			iovec->iov_len = 0;
			iovec++;
			consumed++;
		} else {
			iovec->iov_base += count;
			iovec->iov_len -= count;
			count = 0;
		}
	}

	return consumed;
}

/*
 * Consume an iovec. Count must not exceed the total iovec[] size.
 * iove count should be updated.
 */
void tcmu_cmd_seek(struct tcmulib_cmd *cmd, size_t count)
{
	cmd->iov_cnt -= tcmu_iovec_seek(cmd->iovec, count);
}

size_t tcmu_iovec_length(struct iovec *iovec, size_t iov_cnt)
{
	size_t length = 0;

	while (iov_cnt) {
		length += iovec->iov_len;
		iovec++;
		iov_cnt--;
	}

	return length;
}

void __tcmu_sense_set_data(uint8_t *sense_buf, uint8_t key, uint16_t asc_ascq)
{
	sense_buf[0] |= 0x70;	/* fixed, current */
	sense_buf[2] = key;
	sense_buf[7] = 0xa;
	sense_buf[12] = (asc_ascq >> 8) & 0xff;
	sense_buf[13] = asc_ascq & 0xff;
}

int tcmu_sense_set_data(uint8_t *sense_buf, uint8_t key, uint16_t asc_ascq)
{
	memset(sense_buf, 0, SENSE_BUFFERSIZE);
	__tcmu_sense_set_data(sense_buf, key, asc_ascq);
	return TCMU_STS_PASSTHROUGH_ERR;
}

void tcmu_sense_set_key_specific_info(uint8_t *sense_buf, uint16_t info)
{
	memset(sense_buf, 0, 18);

	put_unaligned_be16(info, &sense_buf[16]);
	/* Set SKSV bit */
	sense_buf[15] |= 0x80;
}

void tcmu_sense_set_info(uint8_t *sense_buf, uint32_t info)
{
	memset(sense_buf, 0, 18);

	put_unaligned_be32(info, &sense_buf[3]);
	/* Set VALID bit */
	sense_buf[0] |= 0x80;
}

/*
 * Zero iovec.
 */
void tcmu_iovec_zero(struct iovec *iovec, size_t iov_cnt)
{
	while (iov_cnt) {
		bzero(iovec->iov_base, iovec->iov_len);

		iovec++;
		iov_cnt--;
	}
}

static inline bool tcmu_zeroed_mem(const char *buf, size_t size)
{
    int i;

    for (i = 0; i < size; i++) {
        if (buf[i])
		return false;
    }

    return true;
}

bool tcmu_iovec_zeroed(struct iovec *iovec, size_t iov_cnt)
{
    int i;

    for (i = 0; i < iov_cnt; i++) {
        if (!tcmu_zeroed_mem(iovec[i].iov_base, iovec[i].iov_len))
		return false;
    }

    return true;
}

/*
 * Copy data into an iovec, and consume the space in the iovec.
 *
 * Will truncate instead of overrunning the iovec.
 */
size_t tcmu_memcpy_into_iovec(
	struct iovec *iovec,
	size_t iov_cnt,
	void *src,
	size_t len)
{
	size_t copied = 0;

	while (len && iov_cnt) {
		void *iov_base = iovec->iov_base;
		size_t iov_len = iovec->iov_len;
		size_t to_copy = min(iov_len, len);

		while (to_copy) {
			memcpy(iov_base, src + copied, to_copy);

			len -= to_copy;
			copied += to_copy;
			iov_base += to_copy;
			iov_len -= to_copy;
			to_copy = min(iov_len, len);
		}

		iovec++;
		iov_cnt--;
	}

	return copied;
}

/*
 * Copy data from an iovec, and consume the space in the iovec.
 */
size_t tcmu_memcpy_from_iovec(
	void *dest,
	size_t len,
	struct iovec *iovec,
	size_t iov_cnt)
{
	size_t copied = 0;

	while (len && iov_cnt) {
		void *iov_base = iovec->iov_base;
		size_t iov_len = iovec->iov_len;
		size_t to_copy = min(iov_len, len);

		while (to_copy) {
			memcpy(dest + copied, iovec->iov_base, to_copy);

			len -= to_copy;
			copied += to_copy;
			iov_base += to_copy;
			iov_len -= to_copy;
			to_copy = min(iov_len, len);
		}

		iovec++;
		iov_cnt--;
	}

	return copied;
}

#define CDB_TO_BUF_SIZE(bytes) ((bytes) * 3 + 2)
#define CDB_FIX_BYTES 64 /* 64 bytes for default */
#define CDB_FIX_SIZE CDB_TO_BUF_SIZE(CDB_FIX_BYTES)
void tcmu_cdb_print_info(struct tcmu_device *dev,
			 const struct tcmulib_cmd *cmd,
			 const char *info)
{
	int i, n, bytes, info_len = 0;
	char fix[CDB_FIX_SIZE], *buf;

	buf = fix;

	bytes = tcmu_cdb_get_length(cmd->cdb);
	if (bytes < 0)
		return;

	if (info)
		info_len = strlen(info);

	if (CDB_TO_BUF_SIZE(bytes) + info_len > CDB_FIX_SIZE) {
		buf = malloc(CDB_TO_BUF_SIZE(bytes) + info_len);
		if (!buf) {
			tcmu_dev_err(dev, "out of memory\n");
			return;
		}
	}

	for (i = 0, n = 0; i < bytes; i++) {
		n += sprintf(buf + n, "%x ", cmd->cdb[i]);
	}

	if (info)
		n += sprintf(buf + n, "%s", info);

	sprintf(buf + n, "\n");

	if (info) {
		tcmu_dev_dbg(dev, "%s", buf);
	} else {
		tcmu_dev_dbg_scsi_cmd(dev, "%s", buf);
	}

	if (buf != fix)
		free(buf);
}

void tcmu_thread_cancel(pthread_t thread)
{
	void *join_retval;
	int ret;

	ret = pthread_cancel(thread);
	if (ret) {
		tcmu_err("pthread_cancel failed with value %d\n", ret);
		return;
	}

	ret = pthread_join(thread, &join_retval);
	if (ret) {
		tcmu_err("pthread_join failed with value %d\n", ret);
		return;
	}

	if (join_retval != PTHREAD_CANCELED)
		tcmu_err("unexpected join retval: %p\n", join_retval);
}
