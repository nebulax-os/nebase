
#include <nebase/syslog.h>
#include <nebase/evdp.h>
#include <nebase/rbtree.h>
#include <nebase/resolver.h>

#include <stdlib.h>
#include <unistd.h>
#include <sys/queue.h>

#include <ares.h>

struct resolver_source_node {
	rb_node_t rbtree_ctx;
	SLIST_ENTRY(resolver_source_node) list_ctx;

	neb_evdp_source_t s;
	neb_resolver_t ref_r;
	int ref_fd;
};

SLIST_HEAD(resolver_source_list, resolver_source_node);

struct neb_resolver_ctx {
	SLIST_ENTRY(neb_resolver_ctx) list_ctx;

	neb_resolver_t ref_r;
	void *udata;
	int delete_after_timeout;
	int after_timeout;
};

SLIST_HEAD(resolver_ctx_list, neb_resolver_ctx);

struct neb_resolver {
	ares_channel channel;

	neb_evdp_queue_t q;
	neb_evdp_timer_point timeout_point;

	rb_tree_t active_tree;
	struct resolver_source_list cache_list;
	struct resolver_ctx_list ctx_list;
	// TODO add counting

	int critical_error;
};

static int resolver_rbtree_cmp_node(void *context, const void *node1, const void *node2);
static int resolver_rbtree_cmp_key(void *context, const void *node, const void *key);
rb_tree_ops_t resolver_rbtree_ops = {
	.rbto_compare_nodes = resolver_rbtree_cmp_node,
	.rbto_compare_key = resolver_rbtree_cmp_key,
	.rbto_node_offset = offsetof(struct resolver_source_node, rbtree_ctx),
};

static struct neb_resolver_ctx *resolver_ctx_node_new(void)
{
	struct neb_resolver_ctx *n = calloc(1, sizeof(struct neb_resolver_ctx));
	if (!n) {
		neb_syslog(LOG_ERR, "calloc: %m");
		return NULL;
	}

	return n;
}

static void resolver_ctx_node_del(struct neb_resolver_ctx *n)
{
	free(n);
}

static struct resolver_source_node *resolver_source_node_new(void)
{
	struct resolver_source_node *n = calloc(1, sizeof(struct resolver_source_node));
	if (!n) {
		neb_syslog(LOG_ERR, "calloc: %m");
		return NULL;
	}

	return n;
}

static void resolver_source_node_del(struct resolver_source_node *n)
{
	if (n->s) {
		neb_evdp_queue_t q = neb_evdp_source_get_queue(n->s);
		if (q)
			neb_syslog(LOG_CRIT, "resolver: you should detach source from queue first");
		neb_evdp_source_del(n->s);
	}
	free(n);
}

static int resolver_rbtree_cmp_node(void *context _nattr_unused, const void *node1, const void *node2)
{
	const struct resolver_source_node *e = node1;
	const struct resolver_source_node *p = node2;
	if (e->ref_fd < p->ref_fd)
		return -1;
	else if (e->ref_fd == p->ref_fd)
		return 0;
	else
		return 1;
}

static int resolver_rbtree_cmp_key(void *context _nattr_unused, const void *node, const void *key)
{
	const struct resolver_source_node *e = node;
	int fd = *(int *)key;
	if (e->ref_fd < fd)
		return -1;
	else if (e->ref_fd == fd)
		return 0;
	else
		return 1;
}

static void resolver_reset_timeout(neb_resolver_t r, struct timeval *maxtv)
{
	struct timeval tv;
	struct timeval *v = ares_timeout(r->channel, maxtv, &tv);
	if (!v)
		return;
	time_t timeout_ms = tv.tv_sec * 1000 + tv.tv_usec / 1000;
	neb_evdp_timer_t t = neb_evdp_queue_get_timer(r->q);
	int64_t abs_timeout = neb_evdp_queue_get_abs_timeout(r->q, timeout_ms);
	if (neb_evdp_timer_point_reset(t, r->timeout_point, abs_timeout) != 0) {
		neb_syslog(LOG_CRIT, "Failed to reset resolver timeout point");
		r->critical_error = 1;
	}
}

static neb_evdp_cb_ret_t on_socket_hup(int fd, void *data _nattr_unused, const void *context)
{
	// the socket error is not used in ares
	int sockerr;
	if (neb_evdp_source_fd_get_sockerr(context, &sockerr) != 0) {
		neb_syslog(LOG_CRIT, "Failed to get sockerr for hupped resolver socket %d", fd);
		return NEB_EVDP_CB_BREAK_ERR;
	}
	if (sockerr != 0)
		neb_syslog_en(sockerr, LOG_ERR, "resolver socket fd %d: %m", fd);
	return NEB_EVDP_CB_CONTINUE;
}

static neb_evdp_cb_ret_t on_socket_readable(int fd, void *data)
{
	struct resolver_source_node *n = data;
	ares_process_fd(n->ref_r->channel, fd, ARES_SOCKET_BAD);
	resolver_reset_timeout(n->ref_r, NULL);
	if (n->ref_r->critical_error)
		return NEB_EVDP_CB_BREAK_ERR;
	return NEB_EVDP_CB_CONTINUE;
}

static neb_evdp_cb_ret_t on_socket_writable(int fd, void *data)
{
	struct resolver_source_node *n = data;
	ares_process_fd(n->ref_r->channel, ARES_SOCKET_BAD, fd);
	resolver_reset_timeout(n->ref_r, NULL);
	if (n->ref_r->critical_error)
		return NEB_EVDP_CB_BREAK_ERR;
	return NEB_EVDP_CB_CONTINUE;
}

static void resolver_sock_state_on_change(void *data, ares_socket_t socket_fd, int readable, int writable)
{
	neb_resolver_t r = data;
	if (readable) {
		struct resolver_source_node *n = rb_tree_find_node(&r->active_tree, &socket_fd);
		if (!n) {
			neb_syslog(LOG_CRIT, "No socket %d found in resolver %p", socket_fd, r);
			r->critical_error = 1;
			return;
		}
		if (neb_evdp_source_os_fd_next_read(n->s, on_socket_readable) != 0) {
			neb_syslog(LOG_CRIT, "Failed to enable next read on fd %d", socket_fd);
			r->critical_error = 1;
		}
	} else if (writable) {
		struct resolver_source_node *n = SLIST_FIRST(&r->cache_list);
		if (!n) {
			n = rb_tree_find_node(&r->active_tree, &socket_fd);
			if (!n) { // not found, create a new one and insert
				n = resolver_source_node_new();
				if (!n) {
					neb_syslog(LOG_ERR, "Failed to get new resolver source node");
					r->critical_error = 1;
					return;
				}
				n->s = neb_evdp_source_new_os_fd(socket_fd, on_socket_hup);
				neb_evdp_source_set_udata(n->s, n);
				if (neb_evdp_queue_attach(r->q, n->s) != 0) {
					neb_syslog(LOG_ERR, "Failed to attach resolver source to queue");
					r->critical_error = 1;
					resolver_source_node_del(n);
					return;
				}
				n->ref_fd = socket_fd;
				n->ref_r = r;
				rb_tree_insert_node(&r->active_tree, n);
			}
		} else {
			n->ref_fd = socket_fd;
			struct resolver_source_node *tn = rb_tree_insert_node(&r->active_tree, n);
			if (tn == n) { // inserted the cached one, reset it
				SLIST_REMOVE_HEAD(&r->cache_list, list_ctx);
				if (neb_evdp_source_os_fd_reset(n->s, socket_fd) != 0) {
					neb_syslog(LOG_ERR, "Failed to reset resolver source to fd %d", socket_fd);
					r->critical_error = 1;
					resolver_source_node_del(n);
					return;
				}
			} else { // if existed, just use the old one
				n = tn;
			}
		}
		if (neb_evdp_source_os_fd_next_write(n->s, on_socket_writable) != 0) {
			neb_syslog(LOG_CRIT, "Failed to enable next write on fd %d", socket_fd);
			r->critical_error = 1;
		}
	} else { // it's close
		struct resolver_source_node *n = rb_tree_find_node(&r->active_tree, &socket_fd);
		if (!n) {
			neb_syslog(LOG_CRIT, "No socket %d found in resolver %p", socket_fd, r);
			r->critical_error = 1;
			return;
		}
		rb_tree_remove_node(&r->active_tree, n);
		neb_evdp_queue_t q = neb_evdp_source_get_queue(n->s);
		if (q) {
			if (neb_evdp_queue_detach(q, n->s, 1) != 0) {
				neb_syslog(LOG_CRIT, "Failed to detach source %p from queue %p", n->s, q);
				r->critical_error = 1;
			}
		}
		SLIST_INSERT_HEAD(&r->cache_list, n, list_ctx);
	}
}

neb_resolver_t neb_resolver_create(struct ares_options *options, int optmask)
{
	neb_resolver_t r = calloc(1, sizeof(struct neb_resolver));
	if (!r) {
		neb_syslog(LOG_ERR, "calloc: %m");
		return NULL;
	}

	SLIST_INIT(&r->ctx_list);
	SLIST_INIT(&r->cache_list);
	rb_tree_init(&r->active_tree, &resolver_rbtree_ops);

	options->sock_state_cb = resolver_sock_state_on_change;
	options->sock_state_cb_data = r;

	int ret = ares_init_options(&r->channel, options, optmask);
	if (ret != ARES_SUCCESS) {
		neb_syslog(LOG_ERR, "ares_init_options: %s", ares_strerror(ret));
		free(r);
		return NULL;
	}

	return r;
}

void neb_resolver_destroy(neb_resolver_t r)
{
	struct resolver_source_node *n, *t;
	RB_TREE_FOREACH_SAFE(n, &r->active_tree, t) {
		rb_tree_remove_node(&r->active_tree, n);
		neb_evdp_queue_t q = neb_evdp_source_get_queue(n->s);
		if (q) {
			if (neb_evdp_queue_detach(q, n->s, 1) != 0)
				neb_syslog(LOG_CRIT, "Failed to detach source %p from queue %p", n->s, q);
		}
		SLIST_INSERT_HEAD(&r->cache_list, n, list_ctx);
	}

	for (n = SLIST_FIRST(&r->cache_list); n; n = SLIST_FIRST(&r->cache_list)) {
		SLIST_REMOVE_HEAD(&r->cache_list, list_ctx);
		resolver_source_node_del(n);
	}

	for (struct neb_resolver_ctx *c = SLIST_FIRST(&r->ctx_list); c; c = SLIST_FIRST(&r->ctx_list)) {
		SLIST_REMOVE_HEAD(&r->ctx_list, list_ctx);
		resolver_ctx_node_del(c);
	}

	ares_destroy(r->channel);
	free(r);
}

static neb_evdp_timeout_ret_t reolver_on_timeout(void *data)
{
	neb_resolver_t r = data;
	ares_process_fd(r->channel, ARES_SOCKET_BAD, ARES_SOCKET_BAD);
	resolver_reset_timeout(r, NULL);
	return NEB_EVDP_TIMEOUT_KEEP;
}

int neb_resolver_associate(neb_resolver_t r, neb_evdp_queue_t q)
{
	neb_evdp_timer_t t = neb_evdp_queue_get_timer(q);
	if (!t) {
		neb_syslog(LOG_CRIT, "There is no timer set in queue %p", q);
		return -1;
	}
	r->timeout_point = neb_evdp_timer_new_point(t, INT64_MAX, reolver_on_timeout, r);
	if (!r->timeout_point) {
		neb_syslog(LOG_ERR, "Failed to get timer point");
		return -1;
	}
	r->q = q;
	return 0;
}

void neb_resolver_disassociate(neb_resolver_t r)
{
	ares_cancel(r->channel);
	if (r->timeout_point) {
		neb_evdp_timer_t t = neb_evdp_queue_get_timer(r->q);
		if (t) {
			neb_evdp_timer_del_point(t, r->timeout_point);
			r->timeout_point = NULL;
		} else {
			neb_syslog(LOG_CRIT, "No timer available while deleting timer point");
		}
	}
}

neb_resolver_ctx_t neb_resolver_new_ctx(neb_resolver_t r, void *udata)
{
	struct neb_resolver_ctx *c = SLIST_FIRST(&r->ctx_list);
	if (!c) {
		c = resolver_ctx_node_new();
		if (!c)
			return NULL;

		c->ref_r = r;
	} else {
		SLIST_REMOVE_HEAD(&r->ctx_list, list_ctx);
		c->delete_after_timeout = 0;
	}
	c->udata = udata;

	return c;
}

void neb_resolver_del_ctx(neb_resolver_t r, neb_resolver_ctx_t c)
{
	c->udata = NULL;
	if (c->after_timeout)
		SLIST_INSERT_HEAD(&r->ctx_list, c, list_ctx);
	else
		c->delete_after_timeout = 1;
}