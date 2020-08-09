/*
Copyright (C) 2020- The University of Notre Dame
This software is distributed under the GNU General Public License.
See the file COPYING for details.
*/

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <endian.h>
#include <fcntl.h>
#include <signal.h>
#include <poll.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "mq.h"
#include "list.h"
#include "itable.h"
#include "set.h"
#include "link.h"
#include "xxmalloc.h"
#include "debug.h"

#define HDR_SIZE sizeof(struct mq_msg_header)
#define HDR_MAGIC "DSmsg"

enum mq_msg_type {
	MQ_MSG_BUFFER = 0,
};

enum mq_socket {
	MQ_SOCKET_SERVER,
	MQ_SOCKET_INPROGRESS,
	MQ_SOCKET_CONNECTED,
	MQ_SOCKET_ERROR,
};

struct mq_msg_header {
	char magic[5];
	char pad[2]; // necessary for alignment
	uint8_t type;
	uint64_t length;
};

struct mq_msg {
	enum mq_msg_type type;
	size_t len;
	void *buf;
	struct mq_msg_header hdr;
	bool parsed_header;
	ptrdiff_t hdr_pos;
	ptrdiff_t buf_pos;
};

struct mq {
	struct link *link;
	enum mq_socket state;
	struct mq *acc;
	struct list *send;
	int err;
	struct mq_msg *recv;
	struct mq_msg *send_buf;
	struct mq_msg *recv_buf;
	struct mq_poll *poll_group;
};

struct mq_poll {
	struct itable *members;
	struct set *acceptable;
	struct set *readable;
	struct set *error;
};

static bool errno_is_temporary(void) {
	if (errno == EINTR || errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS || errno == EALREADY || errno == EISCONN) {
		return true;
	} else {
		return false;
	}
}

static struct mq_msg *msg_create (void) {
	return xxcalloc(1, sizeof(struct mq_msg));
}

static void mq_die(struct mq *mq, int err) {
	assert(mq);
	mq->state = MQ_SOCKET_ERROR;
	mq->err = err;

	mq_close(mq->acc);
	mq_msg_delete(mq->send_buf);
	mq_msg_delete(mq->recv_buf);
	mq_msg_delete(mq->recv);

	struct list_cursor *cur = list_cursor_create(mq->send);
	list_seek(cur, 0);
	for (struct mq_msg *msg; list_get(cur, (void **) &msg); list_next(cur)) {
		mq_msg_delete(msg);
	}
	list_cursor_destroy(cur);

	if (mq->poll_group) {
		set_remove(mq->poll_group->acceptable, mq);
		set_remove(mq->poll_group->readable, mq);
		if (err == 0) {
			set_remove(mq->poll_group->error, mq);
		} else {
			set_insert(mq->poll_group->error, mq);
		}
	}
}

static void delete_msg(struct mq_msg *msg) {
	if (!msg) return;
	free(msg->buf);
	free(msg);
}

static void write_header(struct mq_msg *msg) {
	assert(msg);
	memcpy(msg->hdr.magic, HDR_MAGIC, sizeof(msg->hdr.magic));
	msg->hdr.type = msg->type;
	msg->hdr.length = htobe64(msg->len);
}

static struct mq *mq_create(void) {
	struct mq *out = xxcalloc(1, sizeof(*out));
	out->send = list_create();
	return out;
}

void mq_close(struct mq *mq) {
	if (!mq) return;

	mq_die(mq, 0);
	if (mq->poll_group) {
		itable_remove(mq->poll_group->members, (uintptr_t) mq);
	}
	link_close(mq->link);
	list_delete(mq->send);
	free(mq);
}

int mq_geterror(struct mq *mq) {
	assert(mq);
	if (mq->state != MQ_SOCKET_ERROR) {
		return 0;
	} else {
		return mq->err;
	}
}

void mq_msg_delete(struct mq_msg *msg) {
	if (!msg) return;
	// once blobs are implemented, check for on-disk stuff to delete
	delete_msg(msg);
}

static int flush_send(struct mq *mq) {
	assert(mq);

	int socket = link_fd(mq->link);

	while (true) {
		if (!mq->send_buf) {
			mq->send_buf = list_pop_head(mq->send);
			if (!mq->send_buf) return 0;
			write_header(mq->send_buf);
		}
		struct mq_msg *snd = mq->send_buf;

		// make sure the cast below won't overflow
		assert(HDR_SIZE < PTRDIFF_MAX);
		assert(snd->len < PTRDIFF_MAX);
		if (snd->hdr_pos < (ptrdiff_t) HDR_SIZE) {
			ssize_t s = send(socket, &snd->hdr + snd->hdr_pos,
					HDR_SIZE - snd->hdr_pos, 0);
			if (s == -1 && errno_is_temporary()) {
				return 0;
			} else if (s <= 0) {
				return -1;
			}
			snd->hdr_pos += s;
		} else if (snd->buf_pos < (ptrdiff_t) snd->len) {
			ssize_t s = send(socket, snd->buf + snd->buf_pos,
					snd->len - snd->buf_pos, 0);
			if (s == -1 && errno_is_temporary()) {
				return 0;
			} else if (s <= 0) {
				return -1;
			}
			snd->buf_pos += s;
		} else {
			delete_msg(snd);
			mq->send_buf = NULL;
		}
	}
}

static int flush_recv(struct mq *mq) {
	assert(mq);

	int socket = link_fd(mq->link);

	while (!mq->recv) {
		if (!mq->recv_buf) {
			mq->recv_buf = msg_create();
		}
		struct mq_msg *rcv = mq->recv_buf;

		// make sure the cast below won't overflow
		assert(HDR_SIZE < PTRDIFF_MAX);
		assert(rcv->len < PTRDIFF_MAX);
		if (rcv->hdr_pos < (ptrdiff_t) HDR_SIZE) {
			ssize_t r = recv(socket, &rcv->hdr + rcv->hdr_pos,
					HDR_SIZE - rcv->hdr_pos, 0);
			if (r == -1 && errno_is_temporary()) {
				return 0;
			} else if (r <= 0) {
				return -1;;
			}
			rcv->hdr_pos += r;
		} else if (!rcv->parsed_header) {
			if (memcmp(rcv->hdr.magic, HDR_MAGIC, sizeof(rcv->hdr.magic))) {
				return -1;
			}
			rcv->type = rcv->hdr.type;
			rcv->len = be64toh(rcv->hdr.length);
			rcv->buf = xxmalloc(rcv->len + 1);
			((char *) rcv->buf)[rcv->len] = 0;
			//TODO validate
			rcv->parsed_header = true;
		} else if (rcv->buf_pos < (ptrdiff_t) rcv->len) {
			ssize_t r = recv(socket, rcv->buf + rcv->buf_pos,
					rcv->len - rcv->buf_pos, 0);
			if (r == -1 && errno_is_temporary()) {
				return 0;
			} else if (r <= 0) {
				return -1;;
			}
			rcv->buf_pos += r;
		} else {
			// parse JX, etc.
			mq->recv = mq->recv_buf;
			mq->recv_buf = NULL;
		}
	}

	return 0;
}

static short poll_events(struct mq *mq) {
	assert(mq);

	short out = 0;

	switch (mq->state) {
		case MQ_SOCKET_INPROGRESS:
			out |= POLLOUT;
			break;
		case MQ_SOCKET_CONNECTED:
			if (mq->send_buf || list_length(mq->send)) {
				out |= POLLOUT;
			}
			// falls through
		case MQ_SOCKET_SERVER:
			if (!mq->acc && !mq->recv) {
				out |= POLLIN;
			}
			break;
		case MQ_SOCKET_ERROR:
			break;
	}

	return out;
}

static void update_poll_group(struct mq *mq) {
	assert(mq);

	if (!mq->poll_group) return;
	if (mq->state == MQ_SOCKET_ERROR) {
		set_insert(mq->poll_group->error, mq);
	}
	if (mq->recv) {
		set_insert(mq->poll_group->readable, mq);
	}
	if (mq->acc) {
		set_insert(mq->poll_group->acceptable, mq);
	}
}

static int handle_revents(struct pollfd *pfd, struct mq *mq) {
	assert(pfd);
	assert(mq);

	int rc = 0;
	int err;
	socklen_t size = sizeof(err);

	switch (mq->state) {
		case MQ_SOCKET_ERROR:
			break;
		case MQ_SOCKET_INPROGRESS:
			if (pfd->revents & POLLOUT) {
				rc = getsockopt(link_fd(mq->link), SOL_SOCKET, SO_ERROR,
						&err, &size);
				assert(rc == 0);
				if (err == 0) {
					mq->state = MQ_SOCKET_CONNECTED;
				} else {
					mq_die(mq, err);
				}
			}
			break;
		case MQ_SOCKET_CONNECTED:
			if (pfd->revents & POLLOUT) {
				rc = flush_send(mq);
			}
			if (rc == -1) {
				mq_die(mq, errno);
				goto DONE;
			}
			if (pfd->revents & POLLIN) {
				rc = flush_recv(mq);
			}
			if (rc == -1) {
				mq_die(mq, errno);
				goto DONE;
			}
			break;
		case MQ_SOCKET_SERVER:
			if (pfd->revents & POLLIN) {
				struct link *link = link_accept(mq->link, LINK_NOWAIT);
				// If the server socket polls readable,
				// this should never block.
				assert(link);
				// Should only poll on writing if accept slot is free
				assert(!mq->acc);
				struct mq *out = mq_create();
				out->link = link;
				out->state = MQ_SOCKET_CONNECTED;
				mq->acc = out;
			}
			break;
	}

DONE:
	update_poll_group(mq);
	return rc;
}

struct mq_msg *mq_wrap_buffer(const void *b, size_t size) {
	assert(b);
	struct mq_msg *out = msg_create();
	out->type = MQ_MSG_BUFFER;
	out->len = size;
	out->buf = xxmalloc(size);
	memcpy(out->buf, b, size);
	return out;
}

void *mq_unwrap_buffer(struct mq_msg *msg) {
	assert(msg);
	if (msg->type != MQ_MSG_BUFFER) return NULL;
	void *out = msg->buf;
	free(msg);
	return out;
}

void mq_send(struct mq *mq, struct mq_msg *msg) {
	assert(mq);
	assert(msg);
	list_push_tail(mq->send, msg);
}

struct mq_msg *mq_recv(struct mq *mq) {
	assert(mq);
	struct mq_msg *out = mq->recv;
	mq->recv = NULL;
	if (mq->poll_group) {
		set_remove(mq->poll_group->readable, mq);
	}
	return out;
}

struct mq *mq_serve(const char *addr, int port) {
	struct link *link = link_serve_address(addr, port);
	if (!link) return NULL;
	struct mq *out = mq_create();
	out->link = link;
	out->state = MQ_SOCKET_SERVER;
	return out;
}

struct mq *mq_connect(const char *addr, int port) {
	struct link *link = link_connect(addr, port, LINK_NOWAIT);
	if (!link) return NULL;
	struct mq *out = mq_create();
	out->link = link;
	out->state = MQ_SOCKET_INPROGRESS;
	return out;
}

struct mq *mq_accept(struct mq *mq) {
	assert(mq);
	struct mq *out = mq->acc;
	mq->acc = NULL;
	if (mq->poll_group) {
		set_remove(mq->poll_group->acceptable, mq);
	}
	return out;
}

int mq_wait(struct mq *mq, time_t stoptime) {
	assert(mq);

	int rc;
	struct pollfd pfd;
	struct timespec stop;
	sigset_t mask;
	pfd.fd = link_fd(mq->link);
	pfd.revents = 0;

	do {
		pfd.events = poll_events(mq);

		// NB: we're using revents from the *previous* iteration
		if (handle_revents(&pfd, mq) == -1) {
			return -1;
		}

		if (mq->recv || mq->acc) {
			return 1;
		}

		stop.tv_nsec = 0;
		stop.tv_sec = stoptime - time(NULL);
		if (stop.tv_sec < 0) {
			return 0;
		}
		sigemptyset(&mask);
	} while ((rc = ppoll(&pfd, 1, &stop, &mask)) > 0);

	if (rc == 0 || (rc == -1 && errno == EINTR)) {
		return 0;
	} else {
		return -1;
	}
}

struct mq_poll *mq_poll_create(void) {
	struct mq_poll *out = xxcalloc(1, sizeof(*out));
	out->members = itable_create(0);
	out->acceptable = set_create(0);
	out->readable = set_create(0);
	out->error = set_create(0);
	return out;
}

void mq_poll_delete(struct mq_poll *p) {
	if (!p) return;

	uint64_t key;
	uintptr_t ptr;
	void *value;
	itable_firstkey(p->members);
	while (itable_nextkey(p->members, &key, &value)) {
		ptr = key;
		struct mq *mq = (struct mq *) ptr;
		mq->poll_group = NULL;
	}
	itable_delete(p->members);
	set_delete(p->readable);
	set_delete(p->acceptable);
	set_delete(p->error);
	free(p);
}

int mq_poll_add(struct mq_poll *p, struct mq *mq, void *tag) {
	assert(p);
	assert(mq);

	if (mq->poll_group == p) {
		errno = EEXIST;
		return -1;
	}
	if (mq->poll_group) {
		errno = EINVAL;
		return -1;
	}

	if (!tag) tag = mq;
	mq->poll_group = p;
	itable_insert(p->members, (uintptr_t) mq, tag);

	return 0;
}

int mq_poll_rm(struct mq_poll *p, struct mq *mq) {
	assert(p);
	assert(mq);

	if (mq->poll_group != p) {
		errno = ENOENT;
		return -1;
	}
	mq->poll_group = NULL;
	itable_remove(p->members, (uintptr_t) mq);
	set_remove(p->acceptable, mq);
	set_remove(p->readable, mq);
	set_remove(p->error, mq);

	return 0;
}

void *mq_poll_acceptable(struct mq_poll *p) {
	assert(p);

	set_first_element(p->acceptable);
	struct mq *mq = set_next_element(p->acceptable);
	assert(mq);
	void *tag = itable_lookup(p->members, (uintptr_t) mq);
	assert(tag);
	return tag;
}

void *mq_poll_readable(struct mq_poll *p) {
	assert(p);

	set_first_element(p->readable);
	struct mq *mq = set_next_element(p->readable);
	assert(mq);
	void *tag = itable_lookup(p->members, (uintptr_t) mq);
	assert(tag);
	return tag;
}

void *mq_poll_error(struct mq_poll *p) {
	assert(p);

	set_first_element(p->error);
	struct mq *mq = set_next_element(p->error);
	assert(mq);
	void *tag = itable_lookup(p->members, (uintptr_t) mq);
	assert(tag);
	return tag;
}

int mq_poll_wait(struct mq_poll *p, time_t stoptime) {
	assert(p);

	struct timespec stop;
	sigset_t mask;
	int rc;

	int count = itable_size(p->members);
	struct pollfd *pfds = xxcalloc(count, sizeof(*pfds));

	do {
		uint64_t key;
		uintptr_t ptr;
		void *value;
		itable_firstkey(p->members);
		for (int i = 0; itable_nextkey(p->members, &key, &value); i++) {
			ptr = key;
			struct mq *mq = (struct mq *) ptr;
			pfds[i].fd = link_fd(mq->link);
			pfds[i].events = poll_events(mq);

			// NB: we're using revents from the *previous* iteration
			rc = handle_revents(&pfds[i], mq);
			if (rc == -1) {
				goto DONE;
			}
		}

		rc = 0;
		rc += set_size(p->acceptable);
		rc += set_size(p->readable);
		rc += set_size(p->error);
		if (rc > 0) goto DONE;

		stop.tv_nsec = 0;
		stop.tv_sec = stoptime - time(NULL);
		if (stop.tv_sec < 0) {
			rc = 0;
			goto DONE;
		}
		sigemptyset(&mask);
	} while ((rc = ppoll(pfds, count, &stop, &mask)) > 0);

DONE:
	free(pfds);

	if (rc >= 0) {
		return rc;
	} else if (rc == -1 && errno == EINTR) {
		return 0;
	} else {
		return -1;
	}
}
