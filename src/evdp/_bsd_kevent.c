
#include "options.h"

#include <nebase/syslog.h>
#include <nebase/time.h>

#include "core.h"

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/event.h>

struct evdp_queue_context {
	int fd;
	struct kevent *ee;
};

struct evdp_source_timer_context {
	struct kevent ctl_event;
	int attached;
};

struct evdp_source_ro_fd_context {
	struct kevent ctl_event;
};

struct evdp_source_os_fd_context {
	struct {
		int added;
		int to_add;
		struct kevent ctl_event;
	} rd;
	struct {
		int added;
		int to_add;
		struct kevent ctl_event;
	} wr;
	int stats_updated;
};

void *evdp_create_queue_context(neb_evdp_queue_t q)
{
	struct evdp_queue_context *c = calloc(1, sizeof(struct evdp_queue_context));
	if (!c) {
		neb_syslog(LOG_ERR, "malloc: %m");
		return NULL;
	}
	c->fd = -1;

	c->ee = malloc(q->batch_size * sizeof(struct kevent));
	if (!c->ee) {
		neb_syslog(LOG_ERR, "malloc: %m");
		evdp_destroy_queue_context(c);
		return NULL;
	}

	c->fd = kqueue();
	if (c->fd == -1) {
		neb_syslog(LOG_ERR, "kqueue: %m");
		evdp_destroy_queue_context(c);
		return NULL;
	}

	return c;
}

void evdp_destroy_queue_context(void *context)
{
	struct evdp_queue_context *c = context;

	if (c->fd >= 0)
		close(c->fd);
	if (c->ee)
		free(c->ee);
	free(c);
}

void evdp_queue_rm_pending_events(neb_evdp_queue_t q, neb_evdp_source_t s)
{
	void *s_got = NULL, *s_to_rm = s;
	const struct evdp_queue_context *c = q->context;
	if (!c->ee)
		return;
	for (int i = q->current_event; i < q->nevents; i++) {
		struct kevent *e = c->ee + i;
		s_got = (neb_evdp_source_t)e->udata;
		if (s_got == s_to_rm)
#if defined(OS_NETBSD)
			e->udata = (intptr_t)NULL;
#else
			e->udata = NULL;
#endif
	}
}

int evdp_queue_wait_events(neb_evdp_queue_t q, int timeout_msec)
{
	const struct evdp_queue_context *c = q->context;

	struct timespec ts;
	struct timespec *timeout = NULL;
	if (timeout_msec != -1) {
		ts.tv_sec = timeout_msec / 1000;
		ts.tv_nsec = (timeout_msec % 1000) * 1000000;
		timeout = &ts;
	}

	int readd_nevents = q->nevents; // re add events first
	if (readd_nevents) {
		// c->ee is reused after kevent, so we need to mv s from pending to running,
		// NOTE this may lead to pending status of s invalid, always check ENOENT
		//      when detach
		q->stats.pending -= readd_nevents;
		q->stats.running += readd_nevents;
		for (int i = 0; i < readd_nevents; i++) {
			neb_evdp_source_t s = (neb_evdp_source_t)c->ee[i].udata;
			switch (s->type) {
			case EVDP_SOURCE_OS_FD:
			{
				struct evdp_source_os_fd_context *sc = s->context;
				struct kevent *e = &c->ee[i];
				switch (e->filter) {
				case EVFILT_READ:
					sc->rd.added = 1;
					sc->rd.to_add = 0;
					break;
				case EVFILT_WRITE:
					sc->wr.added = 1;
					sc->wr.to_add = 0;
					break;
				default:
					break;
				}
				if (sc->stats_updated) {
					q->stats.pending += 1;
					q->stats.running -= 1;
					continue;
				} else {
					sc->stats_updated = 1;
				}
			}
				break;
			default:
				break;
			}
			EVDP_SLIST_REMOVE(s);
			EVDP_SLIST_RUNNING_INSERT_NO_STATS(q, s);
		}
	}
	q->nevents = kevent(c->fd, c->ee, readd_nevents, c->ee, q->batch_size, timeout);
	if (q->nevents == -1) {
		switch (errno) {
		case EINTR:
			q->nevents = 0;
			break;
		default:
			neb_syslog(LOG_ERR, "kevent: %m");
			return -1;
			break;
		}
	}
	return 0;
}

int evdp_queue_fetch_event(neb_evdp_queue_t q, struct neb_evdp_event *nee)
{
	const struct evdp_queue_context *c = q->context;

	struct kevent *e = c->ee + q->current_event;
	nee->event = e;
	nee->source = (neb_evdp_source_t)e->udata;
	if (e->flags & EV_ERROR) { // see return value of kevent
		neb_syslog_en(e->data, LOG_ERR, "kevent: %m");
		return -1;
	}
	return 0;
}

static int do_batch_flush(neb_evdp_queue_t q, int nr)
{
	const struct evdp_queue_context *qc = q->context;
	if (kevent(qc->fd, qc->ee, nr, NULL, 0, NULL) == -1) {
		neb_syslog(LOG_ERR, "kevent: %m");
		return -1;
	}
	q->stats.pending -= nr;
	q->stats.running += nr;
	for (int i = 0; i < nr; i++) {
		neb_evdp_source_t s = (neb_evdp_source_t)qc->ee[i].udata;
		switch (s->type) {
		case EVDP_SOURCE_OS_FD:
		{
			struct evdp_source_os_fd_context *sc = s->context;
			struct kevent *e = &qc->ee[i];
			switch (e->filter) {
			case EVFILT_READ:
				sc->rd.added = 1;
				sc->rd.to_add = 0;
				break;
			case EVFILT_WRITE:
				sc->wr.added = 1;
				sc->wr.to_add = 0;
				break;
			default:
				break;
			}
			if (sc->stats_updated) {
				q->stats.pending += 1;
				q->stats.running -= 1;
				continue;
			} else {
				sc->stats_updated = 1;
			}
		}
			break;
		default:
			break;
		}
		EVDP_SLIST_REMOVE(s);
		EVDP_SLIST_RUNNING_INSERT_NO_STATS(q, s);
	}
	return 0;
}

int evdp_queue_flush_pending_sources(neb_evdp_queue_t q)
{
	const struct evdp_queue_context *qc = q->context;
	int count = 0;
	neb_evdp_source_t last_s = NULL;
	for (neb_evdp_source_t s = q->pending_qs->next; s && last_s != s; ) {
		last_s = s;
		struct kevent *e = qc->ee + count++;
		switch (s->type) {
		case EVDP_SOURCE_ITIMER_SEC:
		case EVDP_SOURCE_ITIMER_MSEC:
		case EVDP_SOURCE_ABSTIMER:
			memcpy(e, &((struct evdp_source_timer_context *)s->context)->ctl_event, sizeof(struct kevent));
			break;
		case EVDP_SOURCE_RO_FD:
			memcpy(e, &((struct evdp_source_ro_fd_context *)s->context)->ctl_event, sizeof(struct kevent));
			break;
		case EVDP_SOURCE_OS_FD:
		{
			struct evdp_source_os_fd_context *sc = s->context;
			if (sc->rd.to_add)
				memcpy(e, &sc->rd.ctl_event, sizeof(struct kevent));
			if (sc->wr.to_add)
				memcpy(e, &sc->wr.ctl_event, sizeof(struct kevent));
			sc->stats_updated = 0;
		}
			break;
		case EVDP_SOURCE_LT_FD: // TODO
		default:
			neb_syslog(LOG_ERR, "Unsupported pending source type %d", s->type);
			return -1;
			break;
		}
		if (count >= q->batch_size) {
			if (do_batch_flush(q, count) != 0)
				return -1;
			count = 0;
			s = q->pending_qs->next;
		} else {
			s = s->next;
		}
	}
	if (count) // the last ones will be added during wait_events
		q->nevents = count;
	return 0;
}

void *evdp_create_source_itimer_context(neb_evdp_source_t s)
{
	struct evdp_source_timer_context *c = calloc(1, sizeof(struct evdp_source_timer_context));
	if (!c) {
		neb_syslog(LOG_ERR, "calloc: %m");
		return NULL;
	}

	const struct evdp_conf_itimer *conf = s->conf;
	unsigned int fflags = 0;
	int64_t data = 0;
	switch (s->type) {
	case EVDP_SOURCE_ITIMER_SEC:
#ifdef NOTE_SECONDS
		fflags |= NOTE_SECONDS;
		data = conf->sec;
#else
		data = conf->sec * 1000;
#endif
		break;
	case EVDP_SOURCE_ITIMER_MSEC:
		data = conf->msec;
		break;
	default:
		neb_syslog(LOG_CRIT, "Invalid itimer source type");
		evdp_destroy_source_itimer_context(c);
		return NULL;
		break;
	}

	EV_SET(&c->ctl_event, conf->ident, EVFILT_TIMER, EV_ADD | EV_ENABLE, fflags, data, s);
	c->attached = 0;
	s->pending = 0;

	return c;
}

void evdp_destroy_source_itimer_context(void *context)
{
	struct evdp_source_timer_context *c = context;

	free(c);
}

int evdp_source_itimer_attach(neb_evdp_queue_t q, neb_evdp_source_t s)
{
	struct evdp_source_timer_context *sc = s->context;

	EVDP_SLIST_PENDING_INSERT(q, s);
	sc->attached = 1;

	return 0;
}

void evdp_source_itimer_detach(neb_evdp_queue_t q, neb_evdp_source_t s)
{
	struct evdp_source_timer_context *c = s->context;
	if (c->attached && !s->pending) {
		const struct evdp_queue_context *qc = q->context;
		const struct evdp_conf_itimer *conf = s->conf;
		struct kevent e;
		EV_SET(&e, conf->ident, EVFILT_TIMER, EV_DISABLE | EV_DELETE, 0, 0, NULL);
		if (kevent(qc->fd, &e, 1, NULL, 0, NULL) == -1 && errno != ENOENT)
			neb_syslog(LOG_ERR, "kevent: %m");
	}
	c->attached = 0;
}

neb_evdp_cb_ret_t evdp_source_itimer_handle(const struct neb_evdp_event *ne)
{
	neb_evdp_cb_ret_t ret = NEB_EVDP_CB_CONTINUE;

	const struct kevent *e = ne->event;
	int64_t overrun = e->data;

	const struct evdp_conf_itimer *conf = ne->source->conf;
	if (conf->do_wakeup)
		ret = conf->do_wakeup(conf->ident, overrun, ne->source->udata);

	return ret;
}

void *evdp_create_source_abstimer_context(neb_evdp_source_t s)
{
	struct evdp_source_timer_context *c = calloc(1, sizeof(struct evdp_source_timer_context));
	if (!c) {
		neb_syslog(LOG_ERR, "calloc: %m");
		return NULL;
	}

	c->attached = 0;
	s->pending = 0;

	return c;
}

void evdp_destroy_source_abstimer_context(void *context)
{
	struct evdp_source_timer_context *c = context;

	free(c);
}

int evdp_source_abstimer_regulate(neb_evdp_source_t s)
{
	struct evdp_source_timer_context *c = s->context;
	const struct evdp_conf_abstimer *conf = s->conf;

	time_t abs_ts;
	int delta_sec;
	if (neb_daytime_abs_nearest(conf->sec_of_day, &abs_ts, &delta_sec) != 0) {
		neb_syslog(LOG_ERR, "Failed to get next abs time for sec_of_day %d", conf->sec_of_day);
		return -1;
	}

	unsigned int fflags = 0;
	int64_t data = 0;

#ifdef NOTE_ABSTIME
	fflags |= NOTE_ABSTIME;
	data = abs_ts;
#else
	data = delta_sec;
#endif

#ifdef NOTE_SECONDS
	fflags |= NOTE_SECONDS;
#else
	data *= 1000;
#endif

	EV_SET(&c->ctl_event, conf->ident, EVFILT_TIMER, EV_ADD | EV_ENABLE | EV_ONESHOT, fflags, data, s);

	if (c->attached && !s->pending) {
		neb_evdp_queue_t q = s->q_in_use;
		struct evdp_queue_context *qc = q->context;
		if (kevent(qc->fd, &c->ctl_event, 1, NULL, 0, NULL) == -1) {
			neb_syslog(LOG_ERR, "kevent: %m");
			return -1;
		}
	}

	return 0;
}

int evdp_source_abstimer_attach(neb_evdp_queue_t q, neb_evdp_source_t s)
{
	struct evdp_source_timer_context *sc = s->context;

	EVDP_SLIST_PENDING_INSERT(q, s);
	sc->attached = 1;

	return 0;
}

void evdp_source_abstimer_detach(neb_evdp_queue_t q, neb_evdp_source_t s)
{
	struct evdp_source_timer_context *c = s->context;
	if (c->attached && !s->pending) {
		const struct evdp_queue_context *qc = q->context;
		const struct evdp_conf_abstimer *conf = s->conf;
		struct kevent e;
		EV_SET(&e, conf->ident, EVFILT_TIMER, EV_DISABLE | EV_DELETE, 0, 0, NULL);
		if (kevent(qc->fd, &e, 1, NULL, 0, NULL) == -1 && errno != ENOENT)
			neb_syslog(LOG_ERR, "kevent: %m");
	}
	c->attached = 0;
}

neb_evdp_cb_ret_t evdp_source_abstimer_handle(const struct neb_evdp_event *ne)
{
	neb_evdp_cb_ret_t ret = NEB_EVDP_CB_CONTINUE;

	struct evdp_source_timer_context *c = ne->source->context;
	c->attached = 0;

	const struct kevent *e = ne->event;
	int64_t overrun = e->data;

	const struct evdp_conf_abstimer *conf = ne->source->conf;
	if (conf->do_wakeup)
		ret = conf->do_wakeup(conf->ident, overrun, ne->source->udata);
	if (ret == NEB_EVDP_CB_CONTINUE) {
		// Try to use abstime instead of relative time
		if (evdp_source_abstimer_regulate(ne->source) != 0) {
			neb_syslog(LOG_ERR, "Failed to regulate abstimer source");
			return NEB_EVDP_CB_BREAK_ERR;
		}
		neb_evdp_queue_t q = ne->source->q_in_use;
		EVDP_SLIST_REMOVE(ne->source);
		q->stats.running--;
		EVDP_SLIST_PENDING_INSERT(q, ne->source);
		c->attached = 1;
	}

	return ret;
}

int neb_evdp_source_fd_get_sockerr(const void *context, int *sockerr)
{
	const struct kevent *e = context;

	*sockerr = e->fflags;
	return 0;
}

void *evdp_create_source_ro_fd_context(neb_evdp_source_t s)
{
	struct evdp_source_ro_fd_context *c = calloc(1, sizeof(struct evdp_source_ro_fd_context));
	if (!c) {
		neb_syslog(LOG_ERR, "calloc: %m");
		return NULL;
	}

	s->pending = 0;

	return c;
}

void evdp_destroy_source_ro_fd_context(void *context)
{
	struct evdp_source_ro_fd_context *c = context;

	free(c);
}

int evdp_source_ro_fd_attach(neb_evdp_queue_t q, neb_evdp_source_t s)
{
	struct evdp_source_ro_fd_context *sc = s->context;
	const struct evdp_conf_ro_fd *conf = s->conf;

	EV_SET(&sc->ctl_event, conf->fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, s);

	EVDP_SLIST_PENDING_INSERT(q, s);

	return 0;
}

void evdp_source_ro_fd_detach(neb_evdp_queue_t q, neb_evdp_source_t s, int to_close)
{
	const struct evdp_queue_context *qc = q->context;
	struct evdp_source_ro_fd_context *sc = s->context;

	if (to_close)
		return;

	if (!s->pending) {
		sc->ctl_event.flags = EV_DISABLE | EV_DELETE;
		if (kevent(qc->fd, &sc->ctl_event, 1, NULL, 0, NULL) == -1 && errno != ENOENT)
			neb_syslog(LOG_ERR, "kevent: %m");
	}
}

neb_evdp_cb_ret_t evdp_source_ro_fd_handle(const struct neb_evdp_event *ne)
{
	neb_evdp_cb_ret_t ret = NEB_EVDP_CB_CONTINUE;

	const struct kevent *e = ne->event;

	const struct evdp_conf_ro_fd *conf = ne->source->conf;
	if (e->filter == EVFILT_READ) {
		ret = conf->do_read(e->ident, ne->source->udata);
		if (ret != NEB_EVDP_CB_CONTINUE)
			return ret;
	}
	if (e->flags & EV_EOF) {
		ret = conf->do_hup(e->ident, ne->source->udata, e);
		switch (ret) {
		case NEB_EVDP_CB_BREAK_ERR:
		case NEB_EVDP_CB_BREAK_EXP:
		case NEB_EVDP_CB_CLOSE:
			return ret;
			break;
		default:
			return NEB_EVDP_CB_REMOVE;
			break;
		}
	}

	return ret;
}

void *evdp_create_source_os_fd_context(neb_evdp_source_t s)
{
	struct evdp_source_os_fd_context *c = calloc(1, sizeof(struct evdp_source_os_fd_context));
	if (!c) {
		neb_syslog(LOG_ERR, "calloc: %m");
		return NULL;
	}

	s->pending = 0;
	c->rd.added = 0;
	c->rd.to_add = 0;
	c->wr.added = 0;
	c->wr.to_add = 0;

	return c;
}

void evdp_destroy_source_os_fd_context(void *context)
{
	struct evdp_source_os_fd_context *c = context;

	free(c);
}

int evdp_source_os_fd_attach(neb_evdp_queue_t q, neb_evdp_source_t s)
{
	struct evdp_source_os_fd_context *sc = s->context;
	const struct evdp_conf_fd *conf = s->conf;

	EV_SET(&sc->rd.ctl_event, conf->fd, EVFILT_READ, EV_ADD | EV_ENABLE | EV_ONESHOT, 0, 0, s);
	EV_SET(&sc->wr.ctl_event, conf->fd, EVFILT_WRITE, EV_ADD | EV_ENABLE | EV_ONESHOT, 0, 0, s);

	if (sc->rd.to_add || sc->wr.to_add) {
		EVDP_SLIST_PENDING_INSERT(q, s);
		sc->stats_updated = 0;
	} else {
		EVDP_SLIST_RUNNING_INSERT(q, s);
	}

	return 0;
}

static int do_del_os_fd_rd(const struct evdp_queue_context *qc, struct evdp_source_os_fd_context *sc)
{
	sc->rd.ctl_event.flags = EV_DISABLE | EV_DELETE;
	if (kevent(qc->fd, &sc->rd.ctl_event, 1, NULL, 0, NULL) == -1 && errno != ENOENT) {
		neb_syslog(LOG_ERR, "kevent: %m");
		return -1;
	}
	sc->rd.added = 0;
	return 0;
}

static int do_del_os_fd_wr(const struct evdp_queue_context *qc, struct evdp_source_os_fd_context *sc)
{
	sc->wr.ctl_event.flags = EV_DISABLE | EV_DELETE;
	if (kevent(qc->fd, &sc->wr.ctl_event, 1, NULL, 0, NULL) == -1 && errno != ENOENT) {
		neb_syslog(LOG_ERR, "kevent: %m");
		return -1;
	}
	sc->wr.added = 0;
	return 0;
}

void evdp_source_os_fd_detach(neb_evdp_queue_t q, neb_evdp_source_t s, int to_close)
{
	const struct evdp_queue_context *qc = q->context;
	struct evdp_source_os_fd_context *sc = s->context;

	if (to_close) {
		sc->rd.added = 0;
		sc->wr.added = 0;
		return;
	}

	if (sc->rd.added)
		do_del_os_fd_rd(qc, sc);
	if (sc->wr.added)
		do_del_os_fd_wr(qc, sc);
}

neb_evdp_cb_ret_t evdp_source_os_fd_handle(const struct neb_evdp_event *ne)
{
	neb_evdp_cb_ret_t ret = NEB_EVDP_CB_CONTINUE;

	struct evdp_source_os_fd_context *sc = ne->source->context;

	const struct kevent *e = ne->event;

	const struct evdp_conf_fd *conf = ne->source->conf;
	switch (e->filter) {
	case EVFILT_READ:
		sc->rd.added = 0;
		// sc->rd.to_add = 0;

		ret = conf->do_read(e->ident, ne->source->udata);
		if (ret != NEB_EVDP_CB_CONTINUE)
			return ret;

		if (e->flags & EV_EOF) {
			ret = conf->do_hup(e->ident, ne->source->udata, e);
			switch (ret) {
			case NEB_EVDP_CB_BREAK_ERR:
			case NEB_EVDP_CB_BREAK_EXP:
			case NEB_EVDP_CB_CLOSE:
				return ret;
				break;
			default:
				return NEB_EVDP_CB_REMOVE;
				break;
			}
		}
		break;
	case EVFILT_WRITE:
		sc->wr.added = 0;
		// sc->wr.to_add = 0;

		if (e->flags & EV_EOF) {
			ret = conf->do_hup(e->ident, ne->source->udata, e);
			switch (ret) {
			case NEB_EVDP_CB_BREAK_ERR:
			case NEB_EVDP_CB_BREAK_EXP:
			case NEB_EVDP_CB_CLOSE:
				return ret;
				break;
			default:
				return NEB_EVDP_CB_REMOVE;
				break;
			}
		}

		ret = conf->do_write(e->ident, ne->source->udata);
		if (ret != NEB_EVDP_CB_CONTINUE)
			return ret;
		break;
	default:
		neb_syslog(LOG_ERR, "Invalid filter type %d for os_fd", e->filter);
		return NEB_EVDP_CB_BREAK_ERR;
		break;
	}

	return ret;
}

void evdp_source_os_fd_init_read(neb_evdp_source_t s, neb_evdp_io_handler_t rf)
{
	struct evdp_source_os_fd_context *sc = s->context;
	if (rf)
		sc->rd.to_add = 1;
	else
		sc->rd.to_add = 0;
}

void evdp_source_os_fd_init_write(neb_evdp_source_t s, neb_evdp_io_handler_t rf)
{
	struct evdp_source_os_fd_context *sc = s->context;
	if (rf)
		sc->wr.to_add = 1;
	else
		sc->wr.to_add = 0;
}

int evdp_source_os_fd_reset_read(neb_evdp_source_t s)
{
	struct evdp_source_os_fd_context *sc = s->context;
	if (sc->rd.added) {
		return 0;
	} else {
		sc->rd.to_add = 1;
		sc->rd.ctl_event.flags = EV_ADD | EV_ENABLE | EV_ONESHOT;
		if (!s->pending) { // Make sure add to pending
			neb_evdp_queue_t q = s->q_in_use;
			EVDP_SLIST_REMOVE(s);
			q->stats.running--;
			EVDP_SLIST_PENDING_INSERT(q, s);
		}
	}
	return 0;
}

int evdp_source_os_fd_reset_write(neb_evdp_source_t s)
{
	struct evdp_source_os_fd_context *sc = s->context;
	if (sc->wr.added) {
		return 0;
	} else {
		sc->wr.to_add = 1;
		sc->wr.ctl_event.flags = EV_ADD | EV_ENABLE | EV_ONESHOT;
		if (!s->pending) { // Make sure add to pending
			neb_evdp_queue_t q = s->q_in_use;
			EVDP_SLIST_REMOVE(s);
			q->stats.running--;
			EVDP_SLIST_PENDING_INSERT(q, s);
		}
	}
	return 0;
}

int evdp_source_os_fd_unset_read(neb_evdp_source_t s)
{
	struct evdp_source_os_fd_context *sc = s->context;
	if (sc->rd.added) {
		return do_del_os_fd_rd(s->q_in_use->context, sc);
	} else if (s->pending) {
		sc->rd.to_add = 0;
		if (sc->wr.to_add)
			return 0;
		neb_evdp_queue_t q = s->q_in_use;
		EVDP_SLIST_REMOVE(s);
		q->stats.pending--;
		EVDP_SLIST_RUNNING_INSERT(q, s);
	} else {
		sc->rd.to_add = 0;
	}
	return 0;
}

int evdp_source_os_fd_unset_write(neb_evdp_source_t s)
{
	struct evdp_source_os_fd_context *sc = s->context;
	if (sc->wr.added) {
		return do_del_os_fd_wr(s->q_in_use->context, sc);
	} else if (s->pending) {
		sc->wr.to_add = 0;
		if (sc->rd.to_add)
			return 0;
		neb_evdp_queue_t q = s->q_in_use;
		EVDP_SLIST_REMOVE(s);
		q->stats.pending--;
		EVDP_SLIST_RUNNING_INSERT(q, s);
	} else {
		sc->wr.to_add = 0;
	}
	return 0;
}
