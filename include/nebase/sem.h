
#ifndef NEB_SEM_H
#define NEB_SEM_H 1

#include "cdefs.h"

#include <time.h>

typedef void * neb_sem_t;

/**
 * Notify Semaphore for inter-thread usage
 *   For waiting for a signal in one process
 */

extern neb_sem_t neb_sem_notify_create(unsigned int value)
	__attribute_warn_unused_result__;
extern void neb_sem_notify_destroy(neb_sem_t sem);

extern int neb_sem_notify_signal(neb_sem_t sem)
	__attribute_warn_unused_result__;
extern int neb_sem_notify_timedwait(neb_sem_t sem, const struct timespec *abs_timeout)
	__attribute_warn_unused_result__ neb_attr_nonnull((2));

/**
 * Proc Semaphore for inter-process usage
 */

extern int neb_sem_proc_create(const char *path, int nsems)
	__attribute_warn_unused_result__ neb_attr_nonnull((1));
extern int neb_sem_proc_destroy(int semid);

extern int neb_sem_proc_setval(int semid, int subid, int value);
/**
 * \return -1 if error, excluding EINTR
 */
extern int neb_sem_proc_post(int semid, int subid);

/**
 * \param[in] count should be greater than zero
 * \return -1 if error, including EINTR
 */
extern int neb_sem_proc_wait_count(int semid, int subid, int count, struct timespec *timeout)
	__attribute_warn_unused_result__ neb_attr_nonnull((4));
/**
 * \return -1 if error, including EINTR
 */
extern int neb_sem_proc_wait_zerod(int semid, int subid, struct timespec *timeout)
	__attribute_warn_unused_result__ neb_attr_nonnull((3));

/*
 * Other Semaphore
 */

#endif