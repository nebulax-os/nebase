
#include <nebase/syslog.h>
#include <nebase/evdp.h>
#include <nebase/time.h>
#include <nebase/events.h>

#include "core.h"
#include "timer.h"

#include <stdlib.h>

static neb_evdp_source_t evdp_source_new_empty(neb_evdp_queue_t q)
{
	neb_evdp_source_t s = calloc(1, sizeof(struct neb_evdp_source));
	if (!s) {
		neb_syslog(LOG_ERR, "calloc: %m");
		return NULL;
	}
	s->type = EVDP_SOURCE_NONE;
	s->q_in_use = q;
	s->on_remove = neb_evdp_source_del;
	return s;
}

neb_evdp_queue_t neb_evdp_queue_create(int batch_size)
{
	if (batch_size <= 0)
		batch_size = NEB_EVDP_DEFAULT_BATCH_SIZE;

	neb_evdp_queue_t q = calloc(1, sizeof(struct neb_evdp_queue));
	if (!q) {
		neb_syslog(LOG_ERR, "malloc: %m");
		return NULL;
	}
	q->batch_size = batch_size;

	q->running_qs = evdp_source_new_empty(q);
	if (!q->running_qs) {
		neb_evdp_queue_destroy(q);
		return NULL;
	}
	q->stats.running = 1;
	q->pending_qs = evdp_source_new_empty(q);
	if (!q->pending_qs) {
		neb_evdp_queue_destroy(q);
		return NULL;
	}
	q->stats.pending = 1;

	q->context = evdp_create_queue_context(q);
	if (!q->context) {
		neb_evdp_queue_destroy(q);
		return NULL;
	}

	return q;
}

static void do_detach_from_queue(neb_evdp_queue_t q, neb_evdp_source_t s)
{
	evdp_queue_rm_pending_events(q, s);

	switch (s->type) {
	case EVDP_SOURCE_NONE:
		break;
	case EVDP_SOURCE_ITIMER_SEC:
	case EVDP_SOURCE_ITIMER_MSEC:
		evdp_source_itimer_detach(q, s);
		break;
	case EVDP_SOURCE_ABSTIMER:
		evdp_source_abstimer_detach(q, s);
		break;
	case EVDP_SOURCE_RO_FD:
		evdp_source_ro_fd_detach(q, s);
		break;
	case EVDP_SOURCE_OS_FD:
		evdp_source_os_fd_detach(q, s);
		break;
	case EVDP_SOURCE_LT_FD:
		// TODO type and platform specific detach
		break;
	default:
		neb_syslog(LOG_ERR, "Unsupported evdp_source type %d", s->type);
		break;
	}

	// remove all s from running_q or pending_q
	EVDP_SLIST_REMOVE(s);
	if (s->pending) {
		q->stats.pending--;
		s->pending = 0;
	} else {
		q->stats.running--;
	}
	s->q_in_use = NULL;

	if (s->on_remove) {
		neb_evdp_source_handler_t on_remove = s->on_remove;
		int ret = on_remove(s);
		if (ret != 0)
			neb_syslog(LOG_ERR, "evdp_source %p on_remove cb %p failed with ret %d", s, on_remove, ret);
	}
}

void neb_evdp_queue_destroy(neb_evdp_queue_t q)
{
	q->destroying = 1;
	if (q->stats.pending) {
		// always safe as user can not alter pending_qs within on_remove cb
		for (neb_evdp_source_t s = q->pending_qs->next; s; s = q->pending_qs->next)
			do_detach_from_queue(q, s);
		q->pending_qs->q_in_use = NULL;
		neb_evdp_source_del(q->pending_qs);
		q->pending_qs = NULL;
	}
	if (q->stats.running) {
		// always safe as user can not alter running_qs within on_remove cb
		for (neb_evdp_source_t s = q->running_qs->next; s; s = q->running_qs->next)
			do_detach_from_queue(q, s);
		q->running_qs->q_in_use = NULL;
		neb_evdp_source_del(q->running_qs);
		q->running_qs = NULL;
	}

	if (q->context) {
		evdp_destroy_queue_context(q->context);
		q->context = NULL;
	}

	free(q);
}

void neb_evdp_queue_set_event_handler(neb_evdp_queue_t q, neb_evdp_queue_handler_t ef)
{
	q->event_call = ef;
}

void neb_evdp_queue_set_batch_handler(neb_evdp_queue_t q, neb_evdp_queue_handler_t bf)
{
	q->batch_call = bf;
}

void neb_evdp_queue_set_user_data(neb_evdp_queue_t q, void *udata)
{
	q->udata = udata;
}

int64_t neb_evdp_queue_get_abs_timeout(neb_evdp_queue_t q, int msec)
{
	return q->cur_msec + msec;
}

void neb_neb_evdp_queue_update_cur_msec(neb_evdp_queue_t q)
{
	q->cur_msec = neb_time_get_msec();
}

void neb_evdp_queue_set_timer(neb_evdp_queue_t q, neb_evdp_timer_t t)
{
	q->timer = t;
}

int neb_evdp_queue_attach(neb_evdp_queue_t q, neb_evdp_source_t s)
{
	if (s->q_in_use) {
		neb_syslog(LOG_ERR, "It has already been added to queue %p", s->q_in_use);
		return -1;
	}
	if (q->destroying) {
		neb_syslog(LOG_ERR, "queue %p is destroying, attach is not allowed", q);
		return -1;
	}

	// s may be in running_q or in pending_q, it depends
	int ret = 0;
	switch (s->type) {
	case EVDP_SOURCE_NONE:
		neb_syslog(LOG_ERR, "Empty evdp_source should not be attached");
		ret = -1;
		break;
	case EVDP_SOURCE_ITIMER_SEC:
	case EVDP_SOURCE_ITIMER_MSEC:
		ret = evdp_source_itimer_attach(q, s);
		break;
	case EVDP_SOURCE_ABSTIMER:
		ret = evdp_source_abstimer_attach(q, s);
		break;
	case EVDP_SOURCE_RO_FD:
		ret = evdp_source_ro_fd_attach(q, s);
		break;
	case EVDP_SOURCE_OS_FD:
		ret = evdp_source_os_fd_attach(q, s);
		break;
	case EVDP_SOURCE_LT_FD:
		// TODO type and platform specific attach (pending)
		break;
	default:
		neb_syslog(LOG_ERR, "Unsupported evdp_source type %d", s->type);
		ret = -1;
		break;
	}
	if (ret != 0) {
		neb_syslog(LOG_ERR, "Failed to attach source");
		return -1;
	}

	s->q_in_use = q;
	return 0;
}

void neb_evdp_queue_detach(neb_evdp_queue_t q, neb_evdp_source_t s)
{
	if (s->q_in_use != q) // not belong to this queue
		return;
	do_detach_from_queue(q, s);
}

static neb_evdp_cb_ret_t handle_event(neb_evdp_queue_t q)
{
	struct neb_evdp_event ne;
	if (evdp_queue_fetch_event(q, &ne) != 0)
		return NEB_EVDP_CB_BREAK_ERR;

	if (!ne.source) // source detached
		return NEB_EVDP_CB_CONTINUE;

	int ret = NEB_EVDP_CB_CONTINUE;
	switch (ne.source->type) {
	case EVDP_SOURCE_NONE:
		break;
	case EVDP_SOURCE_ITIMER_SEC:
	case EVDP_SOURCE_ITIMER_MSEC:
		ret = evdp_source_itimer_handle(&ne);
		break;
	case EVDP_SOURCE_ABSTIMER:
		ret = evdp_source_abstimer_handle(&ne);
		break;
	case EVDP_SOURCE_RO_FD:
		ret = evdp_source_ro_fd_handle(&ne);
		break;
	case EVDP_SOURCE_OS_FD:
		ret = evdp_source_os_fd_handle(&ne);
		break;
	case EVDP_SOURCE_LT_FD:
		// TODO type and platform specific handle
		break;
	default:
		neb_syslog(LOG_ERR, "Unsupported evdp_source type %d", ne.source->type);
		break;
	}

	if (ret == NEB_EVDP_CB_REMOVE) {
		do_detach_from_queue(q, ne.source);
		ret = NEB_EVDP_CB_CONTINUE;
	}

	return ret;
}

int neb_evdp_queue_run(neb_evdp_queue_t q)
{
	for (;;) {
		if (thread_events) {
			if (q->event_call) {
				switch (q->event_call(q->udata)) {
				case NEB_EVDP_CB_BREAK_ERR:
					goto exit_err;
					break;
				case NEB_EVDP_CB_BREAK_EXP:
					goto exit_ok;
					break;
				case NEB_EVDP_CB_CONTINUE:
					continue;
					break;
				default:
					break;
				}
			} else {
				if (thread_events & T_E_QUIT)
					break;

				thread_events = 0;
			}
		}

		if (evdp_queue_flush_pending_sources(q) != 0) {
			neb_syslog(LOG_ERR, "Failed to add pending sources");
			goto exit_err;
		}

		q->cur_msec = neb_time_get_msec();
		int timeout_ms = q->timer ? evdp_timer_get_min(q->timer, q->cur_msec) : -1;

		if (evdp_queue_wait_events(q, timeout_ms) != 0) {
			neb_syslog(LOG_ERR, "Error occured while getting evdp events");
			goto exit_err;
		}

		q->cur_msec = neb_time_get_msec();
		int64_t expire_msec = q->cur_msec;

		q->stats.rounds++;
		int nevents = q->nevents;
		if (q->nevents) { /* handle normal events first */
			q->stats.events += q->nevents;
			for (int i = 0; i < q->nevents; i++) {
				q->current_event = i;
				switch (handle_event(q)) {
				case NEB_EVDP_CB_BREAK_ERR:
					goto exit_err;
					break;
				case NEB_EVDP_CB_BREAK_EXP:
					goto exit_ok;
					break;
				default:
					break;
				}
			}
			q->nevents = 0;
			q->current_event = 0;
		}

		if (q->timer) /* handle timeouts before we handle normal events */
			evdp_timer_run_until(q->timer, expire_msec);

		if (q->batch_call && nevents) {
			switch (q->batch_call(q->udata)) {
			case NEB_EVDP_CB_BREAK_ERR:
				goto exit_err;
				break;
			case NEB_EVDP_CB_BREAK_EXP:
				goto exit_ok;
				break;
			default:
				break;
			}
		}
	}

exit_ok:
	return 0;

exit_err:
	return -1;
}

int neb_evdp_source_del(neb_evdp_source_t s)
{
	if (s->q_in_use) {
		neb_syslog(LOG_ERR, "It is currently attached to queue %p, detach it first", s->q_in_use);
		return -1;
	}

	if (s->context) {
		switch (s->type) {
		case EVDP_SOURCE_ITIMER_SEC:
		case EVDP_SOURCE_ITIMER_MSEC:
			evdp_destroy_source_itimer_context(s->context);
			s->context = NULL;
			break;
		case EVDP_SOURCE_ABSTIMER:
			evdp_destroy_source_abstimer_context(s->context);
			s->context = NULL;
			break;
		case EVDP_SOURCE_RO_FD:
			evdp_destroy_source_ro_fd_context(s->context);
			s->context = NULL;
			break;
		case EVDP_SOURCE_OS_FD:
			evdp_destroy_source_os_fd_context(s->context);
			s->context = NULL;
			break;
		case EVDP_SOURCE_LT_FD:
			// TODO type and platform specific deinit
			break;
		default:
			neb_syslog(LOG_ERR, "Unsupported evdp_source type %d", s->type);
			break;
		}
	}

	if (s->conf)
		free(s->conf);
	free(s);
	return 0;
}

void neb_evdp_source_set_udata(neb_evdp_source_t s, void *udata)
{
	s->udata = udata;
}

void *neb_evdp_source_get_udata(neb_evdp_source_t s)
{
	return s->udata;
}

neb_evdp_queue_t neb_evdp_source_get_queue(neb_evdp_source_t s)
{
	return s->q_in_use;
}

void neb_evdp_source_set_on_remove(neb_evdp_source_t s, neb_evdp_source_handler_t on_remove)
{
	s->on_remove = on_remove;
}

neb_evdp_source_t neb_evdp_source_new_itimer_s(unsigned int ident, int val, neb_evdp_wakeup_handler_t tf)
{
	neb_evdp_source_t s = calloc(1, sizeof(struct neb_evdp_source));
	if (!s) {
		neb_syslog(LOG_ERR, "calloc: %m");
		return NULL;
	}
	s->type = EVDP_SOURCE_ITIMER_SEC;

	struct evdp_conf_itimer *conf = calloc(1, sizeof(struct evdp_conf_itimer));
	if (!conf) {
		neb_syslog(LOG_ERR, "calloc: %m");
		neb_evdp_source_del(s);
		return NULL;
	}
	conf->ident = ident;
	conf->sec = val;
	conf->do_wakeup = tf;
	s->conf = conf;

	s->context = evdp_create_source_itimer_context(s);
	if (!s->context) {
		neb_evdp_source_del(s);
		return NULL;
	}

	return s;
}

neb_evdp_source_t neb_evdp_source_new_itimer_ms(unsigned int ident, int val, neb_evdp_wakeup_handler_t tf)
{
	neb_evdp_source_t s = calloc(1, sizeof(struct neb_evdp_source));
	if (!s) {
		neb_syslog(LOG_ERR, "calloc: %m");
		return NULL;
	}
	s->type = EVDP_SOURCE_ITIMER_MSEC;

	struct evdp_conf_itimer *conf = calloc(1, sizeof(struct evdp_conf_itimer));
	if (!conf) {
		neb_syslog(LOG_ERR, "calloc: %m");
		neb_evdp_source_del(s);
		return NULL;
	}
	conf->ident = ident;
	conf->msec = val;
	conf->do_wakeup = tf;
	s->conf = conf;

	s->context = evdp_create_source_itimer_context(s);
	if (!s->context) {
		neb_evdp_source_del(s);
		return NULL;
	}

	return s;
}

neb_evdp_source_t neb_evdp_source_new_abstimer(unsigned int ident, int sec_of_day, neb_evdp_wakeup_handler_t tf)
{
	if (sec_of_day < 0 || sec_of_day >= TOTAL_DAY_SECONDS) {
		neb_syslog(LOG_ERR, "Invalid sec_of_day value: %d", sec_of_day);
		return NULL;
	}

	neb_evdp_source_t s = calloc(1, sizeof(struct neb_evdp_source));
	if (!s) {
		neb_syslog(LOG_ERR, "calloc: %m");
		return NULL;
	}
	s->type = EVDP_SOURCE_ABSTIMER;

	struct evdp_conf_abstimer *conf = calloc(1, sizeof(struct evdp_conf_abstimer));
	if (!conf) {
		neb_syslog(LOG_ERR, "calloc: %m");
		neb_evdp_source_del(s);
		return NULL;
	}
	conf->ident = ident;
	conf->sec_of_day = sec_of_day;
	conf->do_wakeup = tf;
	s->conf = conf;

	s->context = evdp_create_source_abstimer_context(s);
	if (!s->context) {
		neb_evdp_source_del(s);
		return NULL;
	}

	if (evdp_source_abstimer_regulate(s) != 0) {
		neb_syslog(LOG_ERR, "Failed to set initial wakeup time");
		return NULL;
	}

	return s;
}

int neb_evdp_source_abstimer_regulate(neb_evdp_source_t s, int sec_of_day)
{
	if (s->type != EVDP_SOURCE_ABSTIMER) {
		neb_syslog(LOG_ERR, "Invalid evdp_source type %d to regulate abstime", s->type);
		return -1;
	}
	if (sec_of_day >= 0) {
		if (sec_of_day >= TOTAL_DAY_SECONDS) {
			neb_syslog(LOG_ERR, "Invalid sec_of_day value: %d", sec_of_day);
			return -1;
		}
		struct evdp_conf_abstimer *conf = s->conf;
		conf->sec_of_day = sec_of_day;
	}
	return evdp_source_abstimer_regulate(s);
}

neb_evdp_source_t neb_evdp_source_new_ro_fd(int fd, neb_evdp_io_handler_t rf, neb_evdp_eof_handler_t hf)
{
	neb_evdp_source_t s = calloc(1, sizeof(struct neb_evdp_source));
	if (!s) {
		neb_syslog(LOG_ERR, "calloc: %m");
		return NULL;
	}
	s->type = EVDP_SOURCE_RO_FD;

	struct evdp_conf_ro_fd *conf = calloc(1, sizeof(struct evdp_conf_ro_fd));
	if (!conf) {
		neb_syslog(LOG_ERR, "calloc: %m");
		neb_evdp_source_del(s);
		return NULL;
	}
	conf->fd = fd;
	conf->do_read = rf;
	conf->do_hup = hf;
	s->conf = conf;

	s->context = evdp_create_source_ro_fd_context(s);
	if (!s->context) {
		neb_evdp_source_del(s);
		return NULL;
	}

	return s;
}

neb_evdp_source_t neb_evdp_source_new_os_fd(int fd, neb_evdp_eof_handler_t hf)
{
	neb_evdp_source_t s = calloc(1, sizeof(struct neb_evdp_source));
	if (!s) {
		neb_syslog(LOG_ERR, "calloc: %m");
		return NULL;
	}
	s->type = EVDP_SOURCE_OS_FD;

	struct evdp_conf_fd *conf = calloc(1, sizeof(struct evdp_conf_fd));
	if (!conf) {
		neb_syslog(LOG_ERR, "calloc: %m");
		neb_evdp_source_del(s);
		return NULL;
	}
	conf->fd = fd;
	conf->do_hup = hf;
	s->conf = conf;

	s->context = evdp_create_source_os_fd_context(s);
	if (!s->context) {
		neb_evdp_source_del(s);
		return NULL;
	}

	return s;
}

int neb_evdp_source_os_fd_next_read(neb_evdp_source_t s, neb_evdp_io_handler_t rf)
{
	struct evdp_conf_fd *conf = s->conf;
	conf->do_read = rf;
	if (!s->q_in_use) {
		evdp_source_os_fd_init_read(s, rf);
		return 0;
	} else if (rf) {
		return evdp_source_os_fd_reset_read(s);
	} else {
		return evdp_source_os_fd_unset_read(s);
	}
}

int neb_evdp_source_os_fd_next_write(neb_evdp_source_t s, neb_evdp_io_handler_t wf)
{
	struct evdp_conf_fd *conf = s->conf;
	conf->do_write = wf;
	if (!s->q_in_use) {
		evdp_source_os_fd_init_write(s, wf);
		return 0;
	} else if (wf) {
		return evdp_source_os_fd_reset_write(s);
	} else {
		return evdp_source_os_fd_unset_write(s);
	}
}
