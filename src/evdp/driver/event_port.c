
#include <nebase/syslog.h>
#include <nebase/time.h>

#include "core.h"

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <port.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>
#include <stropts.h>

struct evdp_queue_context {
	int fd;
	port_event_t *ee;
};

struct evdp_source_timer_context {
	timer_t id;
	int created;
	int in_action;
	struct itimerspec its;
};

struct evdp_source_ro_fd_context {
	int associated;
};

struct evdp_source_os_fd_context {
	int associated;
	int events;
};

void *evdp_create_queue_context(neb_evdp_queue_t q)
{
	struct evdp_queue_context *c = calloc(1, sizeof(struct evdp_queue_context));
	if (!c) {
		neb_syslogl(LOG_ERR, "malloc: %m");
		return NULL;
	}
	c->fd = -1;

	c->ee = malloc(q->batch_size * sizeof(port_event_t));
	if (!c->ee) {
		neb_syslogl(LOG_ERR, "malloc: %m");
		evdp_destroy_queue_context(c);
		return NULL;
	}

	c->fd = port_create();
	if (c->fd == -1) {
		neb_syslogl(LOG_ERR, "port_create: %m");
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
		port_event_t *e = c->ee + i;
		s_got = e->portev_user;
		if (s_got == s_to_rm)
			e->portev_user = NULL;
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

	uint_t nget = 1;
	if (port_getn(c->fd, c->ee, q->batch_size, &nget, timeout) == -1) {
		switch(errno) {
		case EINTR:
			q->nevents = 0;
			return 0;
			break;
		case ETIME:
			break;
		default:
			neb_syslogl(LOG_ERR, "port_getn: %m");
			return -1;
			break;
		}
	}
	q->nevents = nget;
	return 0;
}

int evdp_queue_fetch_event(neb_evdp_queue_t q, struct neb_evdp_event *nee)
{
	const struct evdp_queue_context *c = q->context;

	port_event_t *e = c->ee + q->current_event;
	nee->event = e;
	nee->source = e->portev_user;
	return 0;
}

static int do_associate_ro_fd(const struct evdp_queue_context *qc, neb_evdp_source_t s)
{
	struct evdp_source_ro_fd_context *sc = s->context;
	const struct evdp_conf_ro_fd *conf = s->conf;
	if (port_associate(qc->fd, PORT_SOURCE_FD, conf->fd, POLLIN, s) == -1) {
		neb_syslogl(LOG_ERR, "port_associate: %m");
		return -1;
	}
	sc->associated = 1;
	return 0;
}

static int do_associate_os_fd(const struct evdp_queue_context *qc, neb_evdp_source_t s)
{
	struct evdp_source_os_fd_context *sc = s->context;
	const struct evdp_conf_fd *conf = s->conf;
	if (port_associate(qc->fd, PORT_SOURCE_FD, conf->fd, sc->events, s) == -1) {
		neb_syslogl(LOG_ERR, "port_associate: %m");
		return -1;
	}
	sc->associated = 1;
	return 0;
}

static int do_disassociate_os_fd(const struct evdp_queue_context *qc, neb_evdp_source_t s)
{
	struct evdp_source_os_fd_context *sc = s->context;
	const struct evdp_conf_fd *conf = s->conf;
	if (port_dissociate(qc->fd, PORT_SOURCE_FD, conf->fd) == -1) {
		if (errno == ENOENT)
			sc->associated = 0;
		neb_syslogl(LOG_ERR, "port_dissociate: %m");
		return -1;
	}
	sc->associated = 0;
	return 0;
}

int evdp_queue_flush_pending_sources(neb_evdp_queue_t q)
{
	const struct evdp_queue_context *qc = q->context;
	int count = 0;
	for (neb_evdp_source_t s = q->pending_qs->next; s; s = q->pending_qs->next) {
		int ret = 0;
		switch (s->type) {
		case EVDP_SOURCE_RO_FD:
			ret = do_associate_ro_fd(qc, s);
			break;
		case EVDP_SOURCE_OS_FD:
			ret = do_associate_os_fd(qc, s);
			break;
		case EVDP_SOURCE_LT_FD:
			break;
		// TODO add other source type here
		default:
			neb_syslog(LOG_ERR, "Unsupported associate source type %d", s->type);
			ret = -1;
			break;
		}
		if (ret)
			return ret;
		EVDP_SLIST_REMOVE(s);
		EVDP_SLIST_RUNNING_INSERT_NO_STATS(q, s);
		count++;
	}
	if (count) {
		q->stats.pending -= count;
		q->stats.running += count;
	}
	return 0;
}

void *evdp_create_source_itimer_context(neb_evdp_source_t s)
{
	struct evdp_source_timer_context *c = calloc(1, sizeof(struct evdp_source_timer_context));
	if (!c) {
		neb_syslogl(LOG_ERR, "calloc: %m");
		return NULL;
	}

	const struct evdp_conf_itimer *conf = s->conf;
	switch (s->type) {
	case EVDP_SOURCE_ITIMER_SEC:
		c->its.it_value.tv_sec = conf->sec;
		c->its.it_interval.tv_sec = c->its.it_value.tv_sec;
		break;
	case EVDP_SOURCE_ITIMER_MSEC:
		c->its.it_value.tv_nsec = conf->msec * 1000000;
		c->its.it_interval.tv_nsec = c->its.it_value.tv_nsec;
		break;
	default:
		neb_syslog(LOG_CRIT, "Invalid itimer source type");
		evdp_destroy_source_itimer_context(c);
		return NULL;
		break;
	}

	c->created = 0;
	c->in_action = 0;
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
	const struct evdp_queue_context *qc = q->context;
	struct evdp_source_timer_context *sc = s->context;

	port_notify_t pn = {
		.portnfy_port = qc->fd,
		.portnfy_user = s,
	};
	struct sigevent e = {
		.sigev_notify = SIGEV_PORT,
		.sigev_value.sival_ptr = &pn,
	};
	if (timer_create(CLOCK_MONOTONIC, &e, &sc->id) == -1) {
		neb_syslogl(LOG_ERR, "timer_create: %m");
		return -1;
	}
	sc->created = 1;

	if (timer_settime(sc->id, 0, &sc->its, NULL) == -1) {
		neb_syslogl(LOG_ERR, "timer_settime: %m");
		timer_delete(sc->id);
		return -1;
	}
	sc->in_action = 1;

	EVDP_SLIST_RUNNING_INSERT(q, s);

	return 0;
}

void evdp_source_itimer_detach(neb_evdp_queue_t q _nattr_unused, neb_evdp_source_t s)
{
	struct evdp_source_timer_context *sc = s->context;

	sc->in_action = 0;

	if (sc->created) {
		if (timer_delete(sc->id) == -1)
			neb_syslogl(LOG_ERR, "timer_delete: %m");
		sc->created = 0;
	}
}

neb_evdp_cb_ret_t evdp_source_itimer_handle(const struct neb_evdp_event *ne)
{
	neb_evdp_cb_ret_t ret = NEB_EVDP_CB_CONTINUE;

	const port_event_t *e = ne->event;
	int overrun = e->portev_events;

	const struct evdp_conf_itimer *conf = ne->source->conf;
	if (conf->do_wakeup)
		ret = conf->do_wakeup(conf->ident, overrun, ne->source->udata);

	return ret;
}

void *evdp_create_source_abstimer_context(neb_evdp_source_t s)
{
	struct evdp_source_timer_context *c = calloc(1, sizeof(struct evdp_source_timer_context));
	if (!c) {
		neb_syslogl(LOG_ERR, "calloc: %m");
		return NULL;
	}

	c->its.it_interval.tv_sec = TOTAL_DAY_SECONDS;
	c->its.it_interval.tv_nsec = 0;

	c->created = 0;
	c->in_action = 0;
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

	c->its.it_value.tv_sec = abs_ts;
	c->its.it_value.tv_nsec = 0;

	if (c->in_action) {
		if (timer_settime(c->id, TIMER_ABSTIME, &c->its, NULL) == -1) {
			neb_syslogl(LOG_ERR, "timer_settime: %m");
			return -1;
		}
	}

	return 0;
}

int evdp_source_abstimer_attach(neb_evdp_queue_t q, neb_evdp_source_t s)
{
	const struct evdp_queue_context *qc = q->context;
	struct evdp_source_timer_context *sc = s->context;

	port_notify_t pn = {
		.portnfy_port = qc->fd,
		.portnfy_user = s,
	};
	struct sigevent e = {
		.sigev_notify = SIGEV_PORT,
		.sigev_value.sival_ptr = &pn,
	};
	if (timer_create(CLOCK_REALTIME, &e, &sc->id) == -1) {
		neb_syslogl(LOG_ERR, "timer_create: %m");
		return -1;
	}
	sc->created = 1;

	if (timer_settime(sc->id, TIMER_ABSTIME, &sc->its, NULL) == -1) {
		neb_syslogl(LOG_ERR, "timer_settime: %m");
		timer_delete(sc->id);
		return -1;
	}
	sc->in_action = 1;

	EVDP_SLIST_RUNNING_INSERT(q, s);

	return 0;
}

void evdp_source_abstimer_detach(neb_evdp_queue_t q _nattr_unused, neb_evdp_source_t s)
{
	struct evdp_source_timer_context *sc = s->context;

	sc->in_action = 0;

	if (sc->created) {
		if (timer_delete(sc->id) == -1)
			neb_syslogl(LOG_ERR, "timer_delete: %m");
		sc->created = 0;
	}
}

neb_evdp_cb_ret_t evdp_source_abstimer_handle(const struct neb_evdp_event *ne)
{
	neb_evdp_cb_ret_t ret = NEB_EVDP_CB_CONTINUE;

	const port_event_t *e = ne->event;
	int overrun = e->portev_events;

	const struct evdp_conf_itimer *conf = ne->source->conf;
	if (conf->do_wakeup)
		ret = conf->do_wakeup(conf->ident, overrun, ne->source->udata);

	return ret;
}

int neb_evdp_source_fd_get_sockerr(const void *context, int *sockerr)
{
	const int *fdp = context;

	socklen_t len = sizeof(int);
	if (getsockopt(*fdp, SOL_SOCKET, SO_ERROR, sockerr, &len) == -1) {
		neb_syslogl(LOG_ERR, "getsockopt(SO_ERROR): %m");
		return -1;
	}

	return 0;
}

int neb_evdp_source_fd_get_nread(const void *context, int *nbytes)
{
	const int *fdp = context;

	if (ioctl(*fdp, I_NREAD, nbytes) == -1) {
		neb_syslogl(LOG_ERR, "ioctl(I_NREAD): %m");
		return -1;
	}

	return 0;
}

void *evdp_create_source_ro_fd_context(neb_evdp_source_t s)
{
	struct evdp_source_ro_fd_context *c = calloc(1, sizeof(struct evdp_source_ro_fd_context));
	if (!c) {
		neb_syslogl(LOG_ERR, "calloc: %m");
		return NULL;
	}

	c->associated = 0;
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
	EVDP_SLIST_PENDING_INSERT(q, s);

	return 0;
}

void evdp_source_ro_fd_detach(neb_evdp_queue_t q, neb_evdp_source_t s, int to_close)
{
	const struct evdp_queue_context *qc = q->context;
	const struct evdp_conf_ro_fd *conf = s->conf;
	struct evdp_source_ro_fd_context *sc = s->context;

	if (to_close) {
		sc->associated = 0;
		return;
	}

	if (sc->associated) {
		if (port_dissociate(qc->fd, PORT_SOURCE_FD, conf->fd) == -1)
			neb_syslogl(LOG_ERR, "port_dissociate: %m");
		sc->associated = 0;
	}
}

neb_evdp_cb_ret_t evdp_source_ro_fd_handle(const struct neb_evdp_event *ne)
{
	neb_evdp_cb_ret_t ret = NEB_EVDP_CB_CONTINUE;

	struct evdp_source_ro_fd_context *sc = ne->source->context;
	sc->associated = 0;

	const port_event_t *e = ne->event;

	const int fd = e->portev_object;
	const struct evdp_conf_ro_fd *conf = ne->source->conf;
	if (e->portev_events & POLLIN) {
		ret = conf->do_read(fd, ne->source->udata, &fd);
		if (ret != NEB_EVDP_CB_CONTINUE)
			return ret;
	}
	if (e->portev_events & POLLHUP) {
		ret = conf->do_hup(fd, ne->source->udata, &fd);
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
	if (ret == NEB_EVDP_CB_CONTINUE) {
		neb_evdp_queue_t q = ne->source->q_in_use;
		EVDP_SLIST_REMOVE(ne->source);
		q->stats.running--;
		EVDP_SLIST_PENDING_INSERT(q, ne->source);
	}

	return ret;
}

void *evdp_create_source_os_fd_context(neb_evdp_source_t s)
{
	struct evdp_source_os_fd_context *c = calloc(1, sizeof(struct evdp_source_os_fd_context));
	if (!c) {
		neb_syslogl(LOG_ERR, "calloc: %m");
		return NULL;
	}

	c->associated = 0;
	s->pending = 0;

	return c;
}

void evdp_reset_source_os_fd_context(neb_evdp_source_t s)
{
	struct evdp_source_os_fd_context *c = s->context;

	c->associated = 0;
	s->pending = 0;
	c->events = 0;
}

void evdp_destroy_source_os_fd_context(void *context)
{
	struct evdp_source_os_fd_context *c = context;

	free(c);
}

int evdp_source_os_fd_attach(neb_evdp_queue_t q, neb_evdp_source_t s)
{
	const struct evdp_source_os_fd_context *sc = s->context;

	if (sc->events & (POLLIN | POLLOUT)) {
		EVDP_SLIST_PENDING_INSERT(q, s);
	} else {
		EVDP_SLIST_RUNNING_INSERT(q, s);
	}

	return 0;
}

void evdp_source_os_fd_detach(neb_evdp_queue_t q, neb_evdp_source_t s, int to_close)
{
	const struct evdp_queue_context *qc = q->context;
	struct evdp_source_os_fd_context *sc = s->context;

	if (to_close) {
		sc->associated = 0;
		return;
	}

	if (sc->associated)
		do_disassociate_os_fd(qc, s);
}

neb_evdp_cb_ret_t evdp_source_os_fd_handle(const struct neb_evdp_event *ne)
{
	neb_evdp_cb_ret_t ret = NEB_EVDP_CB_CONTINUE;

	neb_evdp_source_t s = ne->source;
	struct evdp_source_os_fd_context *sc = s->context;
	sc->associated = 0;

	const port_event_t *e = ne->event;

	const int fd = e->portev_object;
	const struct evdp_conf_fd *conf = s->conf;
	if ((e->portev_events & POLLIN) && conf->do_read) {
		sc->events &= ~POLLIN;
		ret = conf->do_read(fd, s->udata, &fd);
		if (ret != NEB_EVDP_CB_CONTINUE)
			return ret;
	}
	if (e->portev_events & POLLHUP) {
		ret = conf->do_hup(fd, s->udata, &fd);
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
	if ((e->portev_events & POLLOUT) && conf->do_write) {
		sc->events &= ~POLLOUT;
		ret = conf->do_write(fd, s->udata, &fd);
		if (ret != NEB_EVDP_CB_CONTINUE)
			return ret;
	}

	if (sc->events & (POLLIN | POLLOUT)) { // do pending if only handled one of them
		neb_evdp_queue_t q = s->q_in_use;
		EVDP_SLIST_REMOVE(s);
		q->stats.running--;
		EVDP_SLIST_PENDING_INSERT(q, s);
	}

	return ret;
}

void evdp_source_os_fd_init_read(neb_evdp_source_t s, neb_evdp_io_handler_t rf)
{
	struct evdp_source_os_fd_context *sc = s->context;
	if (rf)
		sc->events |= POLLIN;
	else
		sc->events &= ~POLLIN;
}

void evdp_source_os_fd_init_write(neb_evdp_source_t s, neb_evdp_io_handler_t rf)
{
	struct evdp_source_os_fd_context *sc = s->context;
	if (rf)
		sc->events |= POLLOUT;
	else
		sc->events &= ~POLLOUT;
}

int evdp_source_os_fd_reset_read(neb_evdp_source_t s)
{
	struct evdp_source_os_fd_context *sc = s->context;
	if (sc->associated) {
		if (sc->events & POLLIN)
			return 0;
		sc->events |= POLLIN;
		return do_associate_os_fd(s->q_in_use->context, s);
	} else {
		sc->events |= POLLIN;
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
	if (sc->associated) {
		if (sc->events & POLLOUT)
			return 0;
		sc->events |= POLLOUT;
		return do_associate_os_fd(s->q_in_use->context, s);
	} else {
		sc->events |= POLLOUT;
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
	if (sc->associated) {
		if (!(sc->events & POLLIN))
			return 0;
		sc->events ^= POLLIN;
		if (sc->events & POLLOUT)
			return 0;
		return do_disassociate_os_fd(s->q_in_use->context, s);
	} else if (s->pending) {
		sc->events &= ~POLLIN;
		if (sc->events & POLLOUT)
			return 0;
		neb_evdp_queue_t q = s->q_in_use;
		EVDP_SLIST_REMOVE(s);
		q->stats.pending--;
		EVDP_SLIST_RUNNING_INSERT(q, s);
	} else {
		sc->events &= ~POLLIN;
	}
	return 0;
}

int evdp_source_os_fd_unset_write(neb_evdp_source_t s)
{
	struct evdp_source_os_fd_context *sc = s->context;
	if (sc->associated) {
		if (!(sc->events & POLLOUT))
			return 0;
		sc->events ^= POLLOUT;
		if (sc->events & POLLIN)
			return 0;
		return do_disassociate_os_fd(s->q_in_use->context, s);
	} else if (s->pending) {
		sc->events &= ~POLLOUT;
		if (sc->events & POLLIN)
			return 0;
		neb_evdp_queue_t q = s->q_in_use;
		EVDP_SLIST_REMOVE(s);
		q->stats.pending--;
		EVDP_SLIST_RUNNING_INSERT(q, s);
	} else {
		sc->events &= ~POLLOUT;
	}
	return 0;
}