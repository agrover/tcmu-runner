/*
 * Copyright 2016-2017 China Mobile, Inc.
 *
 * This file is licensed to you under your choice of the GNU Lesser
 * General Public License, version 2.1 or any later version (LGPLv2.1 or
 * later), or the Apache License 2.0.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>

#include "libtcmu_log.h"
#include "libtcmu_config.h"
#include "libtcmu_time.h"
#include "libtcmu_priv.h"
#include "libtcmu.h"
#include "string_priv.h"

/* tcmu ring buffer for log */
#define LOG_ENTRY_LEN 256 /* rb[0] is reserved for pri */
#define LOG_MSG_LEN (LOG_ENTRY_LEN - 1) /* the length of the log message */
#define LOG_ENTRYS (1024 * 32)

#define TCMU_LOG_FILENAME_MAX	32
#define TCMU_LOG_FILENAME	"tcmu-runner.log"

typedef int (*log_output_fn_t)(int priority, const char *timestamp,
			       const char *str, void *data);
typedef void (*log_close_fn_t)(void *data);

struct log_output {
	log_output_fn_t output_fn;
	log_close_fn_t close_fn;
	int priority;
	void *data;
};

struct log_buf {
	pthread_cond_t cond;
	pthread_mutex_t lock;

	bool thread_active;

	unsigned int head;
	unsigned int tail;
	char buf[LOG_ENTRYS][LOG_ENTRY_LEN];
	struct log_output *syslog_out;
	struct log_output *file_out;
	pthread_mutex_t file_out_lock;
	pthread_t thread_id;
};

static int tcmu_log_level = TCMU_LOG_INFO;
static struct log_buf *tcmu_logbuf;

static char *tcmu_log_dir;
static pthread_mutex_t tcmu_log_dir_lock = PTHREAD_MUTEX_INITIALIZER;

/* covert log level from tcmu config to syslog */
static inline int to_syslog_level(int level)
{
	switch (level) {
	case TCMU_CONF_LOG_CRIT:
		return TCMU_LOG_CRIT;
	case TCMU_CONF_LOG_ERROR:
		return TCMU_LOG_ERROR;
	case TCMU_CONF_LOG_WARN:
		return TCMU_LOG_WARN;
	case TCMU_CONF_LOG_INFO:
		return TCMU_LOG_INFO;
	case TCMU_CONF_LOG_DEBUG:
		return TCMU_LOG_DEBUG;
	case TCMU_CONF_LOG_DEBUG_SCSI_CMD:
		return TCMU_LOG_DEBUG_SCSI_CMD;
	default:
		return TCMU_LOG_INFO;
	}
}

/* get the log level of tcmu-runner */
unsigned int tcmu_get_log_level(void)
{
	return tcmu_log_level;
}

void tcmu_set_log_level(int level)
{
	if (tcmu_log_level == to_syslog_level(level)) {
		tcmu_dbg("No changes to current log_level: %s, skipping it.\n",
		         log_level_lookup[level]);
		return;
	}
	if (level > TCMU_CONF_LOG_LEVEL_MAX)
		level = TCMU_CONF_LOG_LEVEL_MAX;
	else if (level < TCMU_CONF_LOG_LEVEL_MIN)
		level = TCMU_CONF_LOG_LEVEL_MIN;

	tcmu_crit("log level now is %s\n", log_level_lookup[level]);
	tcmu_log_level = to_syslog_level(level);
}

static inline uint8_t rb_get_pri(struct log_buf *logbuf, unsigned int cur)
{
	return logbuf->buf[cur][0];
}

static inline void rb_set_pri(struct log_buf *logbuf, unsigned int cur, uint8_t pri)
{
	logbuf->buf[cur][0] = (char)pri;
}

static inline char *rb_get_msg(struct log_buf *logbuf, unsigned int cur)
{
	return logbuf->buf[cur] + 1;
}

static inline bool rb_is_empty(struct log_buf *logbuf)
{
	return logbuf->tail == logbuf->head;
}

static inline bool rb_is_full(struct log_buf *logbuf)
{
	return logbuf->tail == (logbuf->head + 1) % LOG_ENTRYS;
}

static inline void rb_update_tail(struct log_buf *logbuf)
{
	logbuf->tail = (logbuf->tail + 1) % LOG_ENTRYS;
}

static inline void rb_update_head(struct log_buf *logbuf)
{
	/* when the ring buffer is full, the oldest log will be dropped */
	if (rb_is_full(logbuf))
		rb_update_tail(logbuf);

	logbuf->head = (logbuf->head + 1) % LOG_ENTRYS;
}

static void log_cleanup_output(struct log_output *output)
{
	if (output->close_fn != NULL)
		output->close_fn(output->data);
	free(output);
}

static void tcmu_log_dir_free(void)
{
	if (tcmu_log_dir) {
		free(tcmu_log_dir);
		tcmu_log_dir = NULL;
	}
}

static void log_cleanup(void *arg)
{
	struct log_buf *logbuf = arg;

	pthread_cond_destroy(&logbuf->cond);
	pthread_mutex_destroy(&logbuf->lock);
	pthread_mutex_destroy(&logbuf->file_out_lock);

	if (logbuf->syslog_out)
		log_cleanup_output(logbuf->syslog_out);
	if (logbuf->file_out)
		log_cleanup_output(logbuf->file_out);

	free(logbuf);
	tcmu_log_dir_free();
}

static void log_output(struct log_buf *logbuf, int pri, const char *msg,
		       struct log_output *output)
{
	char timestamp[TCMU_TIME_STRING_BUFLEN] = {0, };

	if (!output)
		return;

	if (time_string_now(timestamp) < 0)
		return;

	output->output_fn(pri, timestamp, msg, output->data);
}

static void log_queue_msg(struct log_buf *logbuf, int pri, char *buf)
{
	unsigned int head;
	char *msg;

	pthread_mutex_lock(&logbuf->lock);

	head = logbuf->head;
	rb_set_pri(logbuf, head, pri);
	msg = rb_get_msg(logbuf, head);
	memcpy(msg, buf, LOG_MSG_LEN);
	rb_update_head(logbuf);

	if (logbuf->thread_active == false)
		pthread_cond_signal(&logbuf->cond);

	pthread_mutex_unlock(&logbuf->lock);
}

static void cleanup_file_out_lock(void *arg)
{
	struct log_buf *logbuf = arg;

	pthread_mutex_unlock(&logbuf->file_out_lock);
}

static void
log_internal(int pri, struct tcmu_device *dev, const char *funcname,
	     int linenr, const char *fmt, va_list args)
{
	char buf[LOG_MSG_LEN];
	int n;
	struct tcmulib_handler *handler;

	if (pri > tcmu_log_level)
		return;

	if (!fmt)
		return;

	if (!tcmu_logbuf) {
		/* handle early log calls by config and deamon setup */
		vfprintf(stderr, fmt, args);
		return;
	}

	/* Format the log msg */
	if (dev) {
		handler = tcmu_dev_get_handler(dev);
		n = sprintf(buf, "%s:%d %s/%s: ", funcname, linenr,
		            handler ? handler->subtype: "",
		            dev ? dev->tcm_dev_name: "");
	} else {
		n = sprintf(buf, "%s:%d: ", funcname, linenr);
	}

	vsnprintf(buf + n, LOG_MSG_LEN - n, fmt, args);

	/*
	 * Avoid overflowing the log buf with SCSI CDBs.
	 */
	if (pri < TCMU_LOG_DEBUG_SCSI_CMD)
		log_queue_msg(tcmu_logbuf, pri, buf);

	pthread_cleanup_push(cleanup_file_out_lock, tcmu_logbuf);
	pthread_mutex_lock(&tcmu_logbuf->file_out_lock);

	log_output(tcmu_logbuf, pri, buf, tcmu_logbuf->file_out);

	pthread_mutex_unlock(&tcmu_logbuf->file_out_lock);
	pthread_cleanup_pop(0);
}

void tcmu_crit_message(struct tcmu_device *dev, const char *funcname,
		       int linenr, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	log_internal(TCMU_LOG_CRIT, dev, funcname, linenr, fmt, args);
	va_end(args);
}

void tcmu_err_message(struct tcmu_device *dev, const char *funcname,
		      int linenr, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	log_internal(TCMU_LOG_ERROR, dev, funcname, linenr, fmt, args);
	va_end(args);
}

void tcmu_warn_message(struct tcmu_device *dev, const char *funcname,
		       int linenr, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	log_internal(TCMU_LOG_WARN, dev, funcname, linenr, fmt, args);
	va_end(args);
}

void tcmu_info_message(struct tcmu_device *dev, const char *funcname,
		       int linenr, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	log_internal(TCMU_LOG_INFO, dev, funcname, linenr, fmt, args);
	va_end(args);
}

void tcmu_dbg_message(struct tcmu_device *dev, const char *funcname,
		      int linenr, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	log_internal(TCMU_LOG_DEBUG, dev, funcname, linenr, fmt, args);
	va_end(args);
}

void tcmu_dbg_scsi_cmd_message(struct tcmu_device *dev, const char *funcname,
			       int linenr, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	log_internal(TCMU_LOG_DEBUG_SCSI_CMD, dev, funcname, linenr, fmt,
		     args);
	va_end(args);
}

static struct log_output *
create_output(log_output_fn_t output_fn, log_close_fn_t close_fn, void *data,
	      int pri)
{
	struct log_output *output;

	output = calloc(1, sizeof(*output));
	if (!output)
		return NULL;

	output->output_fn = output_fn;
	output->close_fn = close_fn;
	output->data = data;
	output->priority = pri;

	return output;
}

static int output_to_syslog(int pri, const char *timestamp,
                            const char *str, void *data)
{
	/* convert tcmu-runner private level to system level */
	if (pri > TCMU_LOG_DEBUG)
		pri = TCMU_LOG_DEBUG;
	syslog(pri, "%s", str);
	return strlen(str);
}

static void close_syslog(void *data)
{
	closelog();
}

static void close_fd(void *data)
{
	int fd = (intptr_t) data;
	close(fd);
}

static int create_syslog_output(struct log_buf *logbuf, int pri,
				const char *ident)
{
	openlog(ident, 0 ,0);
	logbuf->syslog_out = create_output(output_to_syslog, close_syslog, NULL,
					   pri);
	if (!logbuf->syslog_out) {
		closelog();
		return -1;
	}
	return 0;
}

static const char *loglevel_string(int priority)
{
	switch (priority) {
	case TCMU_LOG_CRIT:
		return "CRIT";
	case TCMU_LOG_ERROR:
		return "ERROR";
	case TCMU_LOG_WARN:
		return "WARN";
	case TCMU_LOG_INFO:
		return "INFO";
	case TCMU_LOG_DEBUG:
		return "DEBUG";
	case TCMU_LOG_DEBUG_SCSI_CMD:
		return "DEBUG_SCSI_CMD";
	}
	return "UNKONWN";
}

static int output_to_fd(int pri, const char *timestamp,
                        const char *str,void *data)
{
	int fd = (intptr_t) data;
	char *buf, *msg;
	int count, ret, written = 0, r, pid = 0;
	char pname[TCMU_THREAD_NAME_LEN];

	if (fd == -1)
		return -1;

	pid = getpid();
	if (pid <= 0)
		return -1;

	if (pthread_getname_np(pthread_self(), pname, TCMU_THREAD_NAME_LEN))
		return -1;

	/*
	 * format: timestamp pid [loglevel] msg
	 */
	ret = asprintf(&msg, "%s %d:%s [%s] %s", timestamp, pid, pname,
		       loglevel_string(pri), str);
	if (ret < 0)
		return -1;

	buf = msg;

	/* safe write */
	count = strlen(buf);
	while (count > 0) {
		r = write(fd, buf, count);
		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0) {
			written = r;
			goto out;
		}
		if (r == 0)
			break;
		buf = (char *) buf + r;
		count -= r;
		written += r;
	}
out:
	free(msg);
	return written;
}

static int create_file_output(struct log_buf *logbuf, int pri,
			      const char *filename)
{
	char log_file_path[PATH_MAX];
	struct log_output *output;
	int fd, ret;

	ret = tcmu_make_absolute_logfile(log_file_path, filename);
	if (ret < 0) {
		tcmu_err("tcmu_make_absolute_logfile failed\n");
		return ret;
	}

	tcmu_dbg("Attempting to use '%s' as the log file path\n", log_file_path);

	fd = open(log_file_path, O_CREAT | O_APPEND | O_WRONLY, S_IRUSR | S_IWUSR);
	if (fd == -1) {
		tcmu_err("Failed to open %s:%m\n", log_file_path);
		return fd;
	}

	output = create_output(output_to_fd, close_fd, (void *)(intptr_t) fd,
			       pri);
	if (!output) {
		close(fd);
		tcmu_err("Failed to create output file: %s\n", log_file_path);
		return -ENOMEM;
	}

	pthread_cleanup_push(cleanup_file_out_lock, logbuf);
	pthread_mutex_lock(&logbuf->file_out_lock);

	if (logbuf->file_out) {
		log_cleanup_output(logbuf->file_out);
	}
	logbuf->file_out = output;

	pthread_mutex_unlock(&logbuf->file_out_lock);
	pthread_cleanup_pop(0);

	tcmu_crit("log file path now is '%s'\n", log_file_path);
	return 0;
}

static bool log_dequeue_msg(struct log_buf *logbuf)
{
	unsigned int tail;
	uint8_t pri;
	char *msg, buf[LOG_MSG_LEN];

	pthread_mutex_lock(&logbuf->lock);
	if (rb_is_empty(logbuf)) {
		logbuf->thread_active = false;
		pthread_mutex_unlock(&logbuf->lock);
		return false;
	}

	tail = logbuf->tail;
	pri = rb_get_pri(logbuf, tail);
	msg = rb_get_msg(logbuf, tail);
	memcpy(buf, msg, LOG_MSG_LEN);
	rb_update_tail(logbuf);
	pthread_mutex_unlock(&logbuf->lock);

	/*
	 * This may block due to rsyslog and syslog-ng, etc.
	 * And the log productors could still insert their log
	 * messages into the ring buffer without blocking. But
	 * the ring buffer may lose some old log rbs if the
	 * ring buffer is full.
	 */
	log_output(logbuf, pri, buf, logbuf->syslog_out);

	return true;
}

static void *log_thread_start(void *arg)
{
	tcmu_set_thread_name("logger", NULL);

	pthread_cleanup_push(log_cleanup, arg);

	while (1) {
		pthread_mutex_lock(&tcmu_logbuf->lock);
		pthread_cond_wait(&tcmu_logbuf->cond, &tcmu_logbuf->lock);
		tcmu_logbuf->thread_active = true;
		pthread_mutex_unlock(&tcmu_logbuf->lock);

		while (log_dequeue_msg(tcmu_logbuf));
	}

	pthread_cleanup_pop(1);
	return NULL;
}

static bool tcmu_log_dir_check(const char *path)
{
	if (!path)
		return false;

	if (strlen(path) >= PATH_MAX - TCMU_LOG_FILENAME_MAX) {
		tcmu_err("The length of log dir path '%s' exceeds %d characters\n",
			 path, PATH_MAX - TCMU_LOG_FILENAME_MAX - 1);
		return false;
	}

	return true;
}

#define TCMU_LOGROTATE_PATH "/etc/logrotate.d/tcmu-runner"

static int tcmu_logrotate_conf_set(const char *logdir)
{
	char *buf = NULL, *line = NULL, *p;
	int ret, m, len;
	size_t n;
	FILE *fp;

	fp = fopen(TCMU_LOGROTATE_PATH, "r+");
	if (fp == NULL) {
		ret = -errno;
		tcmu_err("Failed to open file '%s', %m\n", TCMU_LOGROTATE_PATH);
		return ret;
	}

	ret = fseek(fp, 0L, SEEK_END);
	if (ret == -1) {
		ret = -errno;
		tcmu_err("Failed to seek file '%s', %m\n", TCMU_LOGROTATE_PATH);
		goto error;
	}

	len = ftell(fp);
	if (len == -1) {
		ret = -errno;
		tcmu_err("Failed to get the length of file '%s', %m\n", TCMU_LOGROTATE_PATH);
		goto error;
	}

	/* to make sure we have enough size */
	len += strlen(logdir) + 1;
	buf = calloc(len, sizeof(char));
	if (!buf) {
		ret = -ENOMEM;
		goto error;
	}

	p = buf;
	fseek(fp, 0L, SEEK_SET);
	while ((m = getline(&line, &n, fp)) != -1) {
		if (strstr(line, "tcmu-runner*.log") && strchr(line, '{'))
			m = sprintf(p, "%s/tcmu-runner*.log {\n", logdir);
		else
			m = sprintf(p, "%s", line);
		if (m < 0) {
			ret = m;
			goto error;
		}

		p += m;
	}
	*p = '\0';
	len = p - buf;

	fseek(fp, 0L, SEEK_SET);
	truncate(TCMU_LOGROTATE_PATH, 0L);
	ret = fwrite(buf, 1, len, fp);
	if (ret != len) {
		tcmu_err("Failed to update '%s', %m\n", TCMU_LOGROTATE_PATH);
		goto error;
	}

	ret = 0;
error:
	if (fp)
		fclose(fp);
	free(buf);
	free(line);
	return ret;
}

static int tcmu_log_dir_set(const char *log_dir)
{
	char *new_dir;

	new_dir = strdup(log_dir);
	if (!new_dir) {
		tcmu_err("Failed to copy log dir: %s\n", log_dir);
		return -ENOMEM;
	}

	tcmu_log_dir_free();
	tcmu_log_dir = new_dir;

	return tcmu_logrotate_conf_set(tcmu_log_dir);
}

static int tcmu_mkdir(const char *path)
{
	DIR *dir;

	dir = opendir(path);
	if (dir) {
		closedir(dir);
	} else if (errno == ENOENT) {
		if (mkdir(path, 0755) == -1) {
			tcmu_err("mkdir(%s) failed: %m\n", path);
			return -errno;
		}
	} else {
		tcmu_err("opendir(%s) failed: %m\n", path);
		return -errno;
	}

	return 0;
}

static int tcmu_mkdirs(const char *pathname)
{
	char path[PATH_MAX], *ch;
	int ind = 0, ret;

	strlcpy(path, pathname, PATH_MAX);

	if (path[0] == '/')
		ind++;

	do {
		ch = strchr(path + ind, '/');
		if (!ch)
			break;

		*ch = '\0';

		ret = tcmu_mkdir(path);
		if (ret)
			return ret;

		*ch = '/';
		ind = ch - path + 1;
	} while (1);

	return tcmu_mkdir(path);
}

static void cleanup_log_dir_lock(void *arg)
{
	pthread_mutex_unlock(&tcmu_log_dir_lock);
}

static int tcmu_log_dir_create(const char *path)
{
	int ret = 0;

	if (!tcmu_log_dir_check(path))
		return -EINVAL;

	pthread_cleanup_push(cleanup_log_dir_lock, NULL);
	pthread_mutex_lock(&tcmu_log_dir_lock);
	if (tcmu_log_dir && !strcmp(path, tcmu_log_dir))
		goto unlock;

	ret = tcmu_mkdirs(path);
	if (ret)
		goto unlock;

	ret = tcmu_log_dir_set(path);
unlock:
	pthread_mutex_unlock(&tcmu_log_dir_lock);
	pthread_cleanup_pop(0);
	return ret;
}

int tcmu_make_absolute_logfile(char *path, const char *filename)
{
	int ret = 0;

	pthread_mutex_lock(&tcmu_log_dir_lock);
	if (!tcmu_log_dir) {
		ret = -EINVAL;
		goto unlock;
	}

	if (snprintf(path, PATH_MAX, "%s/%s", tcmu_log_dir, filename) < 0)
		ret = -EINVAL;
unlock:
	pthread_mutex_unlock(&tcmu_log_dir_lock);
	return ret;
}

int tcmu_setup_log(char *log_dir)
{
	struct log_buf *logbuf;
	int ret;

	ret = tcmu_log_dir_create(log_dir);
	if (ret) {
		tcmu_err("Could not setup log dir %s. Error %d.\n", log_dir,
			  ret);
		return ret;
	}

	logbuf = calloc(1, sizeof(struct log_buf));
	if (!logbuf)
		goto free_log_dir;

	logbuf->thread_active = false;
	logbuf->head = 0;
	logbuf->tail = 0;
	pthread_cond_init(&logbuf->cond, NULL);
	pthread_mutex_init(&logbuf->lock, NULL);
	pthread_mutex_init(&logbuf->file_out_lock, NULL);

	ret = create_syslog_output(logbuf, TCMU_LOG_INFO, NULL);
	if (ret < 0)
		tcmu_err("create syslog output error \n");

	ret = create_file_output(logbuf, TCMU_LOG_DEBUG_SCSI_CMD,
				 TCMU_LOG_FILENAME);
	if (ret < 0)
		tcmu_err("create file output error \n");

	tcmu_logbuf = logbuf;
	ret = pthread_create(&logbuf->thread_id, NULL, log_thread_start,
			     logbuf);
	if (ret) {
		tcmu_logbuf = NULL;
		log_cleanup(logbuf);
		return ret;
	}

	return 0;

free_log_dir:
	tcmu_log_dir_free();
	return -ENOMEM;
}

static bool is_same_path(const char* path1, const char* path2)
{
	struct stat st1 = {0,};
	struct stat st2 = {0,};

	if (!path1 || !path2)
		return false;

	if (stat(path1, &st1) == -1 || stat(path2, &st2) == -1) {
		return false;
	}

	return st1.st_dev == st2.st_dev && st1.st_ino == st2.st_ino;
}

int tcmu_resetup_log_file(struct tcmu_config *cfg, char *log_dir)
{
	struct tcmulib_handler *handler;
	int ret;

	if (is_same_path(tcmu_log_dir, log_dir)) {
		tcmu_dbg("No changes to current log_dir: %s, skipping it.\n",
		         log_dir);
		return 0;
	}

	if (log_dir) {
		ret = tcmu_log_dir_create(log_dir);
		if (ret) {
			tcmu_err("Could not reset log dir to %s. Error %d.\n",
				 log_dir, ret);
			return ret;
		}
	}

	if (!tcmu_logbuf)
		/* Early call from config file parser or race with logrotate */
		return 0;

	ret = create_file_output(tcmu_logbuf, TCMU_LOG_DEBUG_SCSI_CMD,
				 TCMU_LOG_FILENAME);
	if (ret < 0) {
		tcmu_err("Could not change log path to %s, ret:%d.\n",
				log_dir, ret);
		return ret;
	}

	if (!cfg || !cfg->ctx)
		return 0;

	darray_foreach(handler, cfg->ctx->handlers) {
		if (!handler->update_logdir)
			continue;

		if (!handler->update_logdir())
			tcmu_err("Failed to update logdir for handler (%s)\n",
				 handler->name);
	}

	return 0;
}

void tcmu_destroy_log()
{
	pthread_t thread;
	void *join_retval;

	thread = tcmu_logbuf->thread_id;
	if (pthread_cancel(thread))
		return;

	pthread_join(thread, &join_retval);
}
