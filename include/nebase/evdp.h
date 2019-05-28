
#ifndef NEB_EVDP_H
#define NEB_EVDP_H 1

#include "cdefs.h"

#include <time.h>

struct neb_evdp_timer;
typedef struct neb_evdp_timer* neb_evdp_timer_t;

struct neb_evdp_queue;
typedef struct neb_evdp_queue* neb_evdp_queue_t;

struct neb_evdp_source;
typedef struct neb_evdp_source* neb_evdp_source_t;

typedef enum {
	NEB_EVDP_CB_CONTINUE = 0,
	NEB_EVDP_CB_REMOVE,
	NEB_EVDP_CB_BREAK, // NOTE, this doesn't apply remove
	NEB_EVDP_CB_READD, // NOTE, for internal usage
} neb_evdp_cb_ret_t;

#define NEB_EVDP_DEFAULT_BATCH_SIZE 10

/*
 * Queue Functions
 */

typedef int (*neb_evdp_queue_gettime_t)(struct timespec *ts);
typedef neb_evdp_cb_ret_t (*neb_evdp_queue_handler_t)(void *udata);

/**
 * \param[in] batch_size default to NEB_DISPATCH_DEFAULT_BATCH_SIZE
 */
extern neb_evdp_queue_t neb_evdp_queue_create(int batch_size)
	_nattr_warn_unused_result;
extern void neb_evdp_queue_destroy(neb_evdp_queue_t q)
 	_nattr_nonnull((1));

/**
 * \param[in] ef if NULL, the default one will be used
 */
extern void neb_evdp_queue_set_event_handler(neb_evdp_queue_t q, neb_evdp_queue_handler_t ef)
	_nattr_nonnull((1));
/**
 * \param[in] bf NULL if there should be no batch action
 */
extern void neb_evdp_queue_set_batch_handler(neb_evdp_queue_t q, neb_evdp_queue_handler_t bf)
	_nattr_nonnull((1));
extern void neb_evdp_queue_set_user_data(neb_evdp_queue_t q, void *udata)
	_nattr_nonnull((1));

extern int neb_evdp_queue_attach(neb_evdp_queue_t q, neb_evdp_source_t s)
	_nattr_warn_unused_result _nattr_nonnull((1, 2));
extern void neb_evdp_queue_detach(neb_evdp_queue_t q, neb_evdp_source_t s)
	_nattr_nonnull((1, 2));

extern int neb_evdp_queue_run(neb_evdp_queue_t q)
	_nattr_nonnull((1));

/*
 * Source Functions
 */

typedef int (*neb_evdp_source_handler_t)(neb_evdp_source_t s);

extern int neb_evdp_source_del(neb_evdp_source_t s)
	_nattr_nonnull((1));
extern void neb_evdp_source_set_udata(neb_evdp_source_t s, void *udata)
	_nattr_nonnull((1));
extern void *neb_evdp_source_get_udata(neb_evdp_source_t s)
	_nattr_nonnull((1));
extern neb_evdp_queue_t neb_evdp_source_get_queue(neb_evdp_source_t s)
	_nattr_nonnull((1));
/**
 * \brief set cb that is called when source is removed from queue
 * \note the source is not auto deleted after on_remove, but you can always call
 *       it before exiting on_remove
 */
extern void neb_evdp_source_set_on_remove(neb_evdp_source_t s, neb_evdp_source_handler_t on_remove)
	_nattr_nonnull((1, 2));

#endif