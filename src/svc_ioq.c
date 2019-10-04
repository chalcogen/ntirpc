/*
 * Copyright (c) 2013 Linux Box Corporation.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR `AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/poll.h>

#include <sys/un.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <syslog.h>
#include <misc/timespec.h>

#include <rpc/types.h>
#include <misc/portable.h>
#include <rpc/rpc.h>
#include <rpc/svc.h>
#include <rpc/svc_auth.h>

#include <intrinsic.h>
#include "rpc_com.h"
#include "clnt_internal.h"
#include "svc_internal.h"
#include "svc_xprt.h"
#include "rpc_dplx_internal.h"
#include "rpc_ctx.h"
#include <rpc/svc_rqst.h>
#include <rpc/xdr_inrec.h>
#include <rpc/xdr_ioq.h>
#include <getpeereid.h>
#include <misc/opr.h>
#include "svc_ioq.h"

static inline void
cfconn_set_dead(SVCXPRT *xprt)
{
	struct svc_vc_xprt *xd = VC_DR(REC_XPRT(xprt));

	mutex_lock(&xprt->xp_lock);
	xd->sx.strm_stat = XPRT_DIED;
	mutex_unlock(&xprt->xp_lock);
}

#define LAST_FRAG ((u_int32_t)(1 << 31))
#define MAXALLOCA (256)

static uint64_t sq_count;
static uint64_t sq_wait; /* cumulative */
static uint64_t sq_max;
bool SendqStatsEnabled = false;

void reset_sendq_stats(void)
{
	(void)atomic_store_uint64_t(&sq_max, 0);
	(void)atomic_store_uint64_t(&sq_count, 0);
	(void)atomic_store_uint64_t(&sq_wait, 0);
}

void record_sendq_stats(uint64_t wait_time)
{
	(void)atomic_inc_uint64_t(&sq_count);
	(void)atomic_add_uint64_t(&sq_wait, wait_time);
	if (wait_time > sq_max)
		(void)atomic_store_uint64_t(&sq_max, wait_time);
}

void enable_sendq_stats()
{
	SendqStatsEnabled = true;
}

void disable_sendq_stats()
{
	SendqStatsEnabled = false;
}

#define NS_PER_SEC ((uint64_t)1000000000)

void get_sendq_stats(uint64_t *count, uint64_t *wait, uint64_t *max)
{
	*count = sq_count;
	*wait = sq_wait;
	*max = sq_max;
}

static inline void now(struct timespec *ts)
{
	int rc;

	rc = clock_gettime(CLOCK_REALTIME, ts);
	assert(rc == 0);
}

static inline uint64_t
timespec_diff(struct timespec *start, struct timespec *end)
{
	if ((end->tv_sec > start->tv_sec)
			|| (end->tv_sec == start->tv_sec
				&& end->tv_nsec >= start->tv_nsec)) {
		return (end->tv_sec - start->tv_sec) * NS_PER_SEC +
			(end->tv_nsec - start->tv_nsec);
	} else {
		return (start->tv_sec - end->tv_sec) * NS_PER_SEC +
			(start->tv_nsec - end->tv_nsec);
	}
}

static inline void
svc_ioq_flushv(SVCXPRT *xprt, struct xdr_ioq *xioq)
{
	struct iovec *iov, *tiov, *wiov;
	struct poolq_entry *have;
	struct xdr_ioq_uv *data;
	ssize_t result;
	u_int32_t frag_header;
	u_int32_t fbytes;
	u_int32_t remaining = 0;
	u_int32_t vsize = (xioq->ioq_uv.uvqh.qcount + 1) * sizeof(struct iovec);
	int iw = 0;
	int ix = 1;
	struct timespec end_time;
	uint64_t wait_time;

	if (SendqStatsEnabled) {
		now(&end_time);
		if (!(xioq->start_time.tv_sec == 0 &&
				 xioq->start_time.tv_nsec == 0)) {
			wait_time = timespec_diff(&xioq->start_time, &end_time);
			record_sendq_stats(wait_time);
		}
	}

	if (unlikely(vsize > MAXALLOCA)) {
		iov = mem_alloc(vsize);
	} else {
		iov = alloca(vsize);
	}
	wiov = iov; /* position at initial fragment header */

	/* update the most recent data length, just in case */
	xdr_tail_update(xioq->xdrs);

	/* build list after initial fragment header (ix = 1 above) */
	TAILQ_FOREACH(have, &(xioq->ioq_uv.uvqh.qh), q) {
		data = IOQ_(have);
		tiov = iov + ix;
		tiov->iov_base = data->v.vio_head;
		tiov->iov_len = ioquv_length(data);
		remaining += tiov->iov_len;
		ix++;
	}

	while (remaining > 0) {
		if (iw == 0) {
			/* new fragment header, determine last iov */
			fbytes = 0;
			for (tiov = &wiov[++iw];
			     (tiov < &iov[ix]) && (iw < __svc_maxiov);
			     ++tiov, ++iw) {
				fbytes += tiov->iov_len;

				/* check for fragment value overflow */
				/* never happens, see ganesha FSAL_MAXIOSIZE */
				if (unlikely(fbytes >= LAST_FRAG)) {
					fbytes -= tiov->iov_len;
					break;
				}
			} /* for */

			/* fragment length doesn't include fragment header */
			if (&wiov[iw] < &iov[ix]) {
				frag_header = htonl((u_int32_t) (fbytes));
			} else {
				frag_header = htonl((u_int32_t) (fbytes | LAST_FRAG));
			}
			wiov->iov_base = &(frag_header);
			wiov->iov_len = sizeof(u_int32_t);

			/* writev return includes fragment header */
			remaining += sizeof(u_int32_t);
			fbytes += sizeof(u_int32_t);
		}

		/* blocking write */
		result = writev(xprt->xp_fd, wiov, iw);
		remaining -= result;

		if (result == fbytes) {
			wiov += iw - 1;
			iw = 0;
			continue;
		}
		if (unlikely(result < 0)) {
			__warnx(TIRPC_DEBUG_FLAG_ERROR,
				"%s() writev failed (%d)\n",
				__func__, errno);
			cfconn_set_dead(xprt);
			break;
		}
		fbytes -= result;

		/* rare? writev underrun? (assume never overrun) */
		for (tiov = wiov; iw > 0; ++tiov, --iw) {
			if (tiov->iov_len > result) {
				tiov->iov_len -= result;
				tiov->iov_base += result;
				wiov = tiov;
				break;
			} else {
				result -= tiov->iov_len;
			}
		} /* for */
	} /* while */

	if (unlikely(vsize > MAXALLOCA)) {
		mem_free(iov, vsize);
	}
}

static void
svc_ioq_write(SVCXPRT *xprt, struct xdr_ioq *xioq, struct poolq_head *ifph)
{
	struct poolq_entry *have;

	/* ifph is part of xprt, so make sure you don't access
	 * ifph after releasing xprt! ifph can be removed as the
	 * function parameter as well.
	 *
	 * For now REF xprt for ifph access and UNREF at the return
	 * of this function.
	 */
	SVC_REF(xprt, SVC_REF_FLAG_NONE);
	for (;;) {
		/* do i/o unlocked */
		if (svc_work_pool.params.thrd_max
		 && !(xprt->xp_flags & SVC_XPRT_FLAG_DESTROYED)) {
			/* all systems are go! */
			svc_ioq_flushv(xprt, xioq);
		}
		SVC_RELEASE(xprt, SVC_RELEASE_FLAG_NONE);
		XDR_DESTROY(xioq->xdrs);

		mutex_lock(&ifph->qmutex);
		if (--(ifph->qcount) == 0)
			break;

		have = TAILQ_FIRST(&ifph->qh);
		TAILQ_REMOVE(&ifph->qh, have, q);
		mutex_unlock(&ifph->qmutex);

		xioq = _IOQ(have);
		xprt = (SVCXPRT *)xioq->xdrs[0].x_lib[1];
	}
	mutex_unlock(&ifph->qmutex);
	SVC_RELEASE(xprt, SVC_RELEASE_FLAG_NONE);
}

static void
svc_ioq_write_callback(struct work_pool_entry *wpe)
{
	struct xdr_ioq *xioq = opr_containerof(wpe, struct xdr_ioq, ioq_wpe);
	SVCXPRT *xprt = (SVCXPRT *)xioq->xdrs[0].x_lib[1];
	struct poolq_head *ifph = &xprt->sendq;

	svc_ioq_write(xprt, xioq, ifph);
}

static void store_sockip(struct sockaddr_storage *addr, char *buf, int len)
{
	const char *name = NULL;

	buf[0] = '\0';
	switch (addr->ss_family) {
	case AF_INET:
		name =
		    inet_ntop(addr->ss_family,
			      &(((struct sockaddr_in *)addr)->sin_addr), buf,
			      len);
		break;
	case AF_INET6:
		name =
		    inet_ntop(addr->ss_family,
			      &(((struct sockaddr_in6 *)addr)->sin6_addr), buf,
			      len);
		break;
	case AF_LOCAL:
		strncpy(buf, ((struct sockaddr_un *)addr)->sun_path, len);
		name = buf;
	}

	if (name == NULL)
		strncpy(buf, "<unknown>", len);
}

static time_t last_time;
void
svc_ioq_write_now(SVCXPRT *xprt, struct xdr_ioq *xioq)
{
	struct poolq_head *ifph = &xprt->sendq;
	char ipaddr[INET6_ADDRSTRLEN];
	time_t ctime;

	SVC_REF(xprt, SVC_REF_FLAG_NONE);
	now(&xioq->start_time);
	mutex_lock(&ifph->qmutex);

	if ((ifph->qcount)++ > 0) {
		/* If too many responses in the queue, drop them to
		 * avoid consuming too much memory.  Constant 2K here is
		 * ok for now, but ideally it should be configurable!
		 */
		if (unlikely(ifph->qcount > 2000)) {
			SVC_RELEASE(xprt, SVC_RELEASE_FLAG_NONE);
			XDR_DESTROY(xioq->xdrs);
			--(ifph->qcount);
			ctime = time(NULL);
			if (ctime > last_time + 30) { /* More than 30 seconds */
				last_time = ctime;
				store_sockip(&xprt->xp_remote.ss, ipaddr,
					     sizeof(ipaddr));
				syslog(LOG_ERR,
				       "nfs-ganesha: NFS client %s is slow, "
				       "resetting socket 0x%p", ipaddr, xprt);
			}
			mutex_unlock(&ifph->qmutex);
			return;
		}

		/* queue additional output requests without task switch */
		TAILQ_INSERT_TAIL(&ifph->qh, &(xioq->ioq_s), q);
		mutex_unlock(&ifph->qmutex);
		return;
	}
	mutex_unlock(&ifph->qmutex);

	/* handle this output request without queuing, then any additional
	 * output requests without a task switch (using this thread).
	 */
	svc_ioq_write(xprt, xioq, ifph);
}

/*
 * Handle rare case of first output followed by heavy traffic that prevents the
 * original thread from continuing for too long.
 *
 * In the more common case, server traffic will already have begun and this
 * will rapidly queue the output and return.
 */
void
svc_ioq_write_submit(SVCXPRT *xprt, struct xdr_ioq *xioq)
{
	struct poolq_head *ifph = &xprt->sendq;
	char ipaddr[INET6_ADDRSTRLEN];
	time_t ctime;

	SVC_REF(xprt, SVC_REF_FLAG_NONE);
	now(&xioq->start_time);
	mutex_lock(&ifph->qmutex);

	if ((ifph->qcount)++ > 0) {
		/* If too many responses in the queue, drop them to
		 * avoid consuming too much memory.  Constant 2K here is
		 * ok for now, but ideally it should be configurable!
		 */
		if (unlikely(ifph->qcount > 2000)) {
			SVC_RELEASE(xprt, SVC_RELEASE_FLAG_NONE);
			XDR_DESTROY(xioq->xdrs);
			--(ifph->qcount);
			ctime = time(NULL);
			if (ctime > last_time + 30) { /* More than 30 seconds */
				last_time = ctime;
				store_sockip(&xprt->xp_remote.ss, ipaddr,
					     sizeof(ipaddr));
				syslog(LOG_ERR,
				       "nfs-ganesha: NFS client %s is slow, "
				       "resetting socket 0x%p", ipaddr, xprt);
			}
			mutex_unlock(&ifph->qmutex);
			return;
		}

		/* queue additional output requests, they will be handled by
		 * existing thread without another task switch.
		 */
		TAILQ_INSERT_TAIL(&ifph->qh, &(xioq->ioq_s), q);
		mutex_unlock(&ifph->qmutex);
		return;
	}
	mutex_unlock(&ifph->qmutex);

	xioq->ioq_wpe.fun = svc_ioq_write_callback;
	work_pool_submit(&svc_work_pool, &xioq->ioq_wpe);
}
