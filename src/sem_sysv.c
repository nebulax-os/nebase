
#include "options.h"

#include <nebase/syslog.h>
#include <nebase/sem.h>

#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

#if defined(OS_LINUX)
union semun {
	int              val;    /* Value for SETVAL */
	struct semid_ds *buf;    /* Buffer for IPC_STAT, IPC_SET */
	unsigned short  *array;  /* Array for GETALL, SETALL */
	struct seminfo  *__buf;  /* Buffer for IPC_INFO (Linux-specific) */
};
#elif defined(OS_FREEBSD) || defined(OS_NETBSD)
union semun {
	int     val;            /* value for SETVAL */
	struct  semid_ds *buf;  /* buffer for IPC_STAT & IPC_SET */
	u_short *array;         /* array for GETALL & SETALL */
};
#elif defined(OSTYPE_SUN)
union semun {
	int              val;
	struct semid_ds *buf;
	ushort_t        *array;
};
#elif defined(OS_DFLYBSD) || defined(OS_OPENBSD) || defined(OS_DARWIN)
// do nothing
#else
# error "fix me"
#endif

int neb_sem_proc_create(const char *path, int nsems)
{
	int semid = -1;
	if (path) {
		key_t key = ftok(path, 1);
		if (key == -1) {
			neb_syslogl(LOG_ERR, "ftok(%s): %m", path);
			return -1;
		}

		semid = semget(key, nsems, IPC_CREAT | IPC_EXCL | 0600);
		if (semid == -1) {
			neb_syslogl(LOG_ERR, "semget(%s, %d): %m", path, nsems);
			return -1;
		}
	} else {
		semid = semget(IPC_PRIVATE, nsems, 0600);
		if (semid == -1) {
			neb_syslogl(LOG_ERR, "semget(%d): %m", nsems);
			return -1;
		}
	}

	union semun arg = {.val = 0};
	for (int i = 0; i < nsems; i++) {
		if (semctl(semid, 0, SETVAL, arg) != 0) {
			neb_syslogl(LOG_ERR, "semctl: %m");
			neb_sem_proc_destroy(semid);
			return -1;
		}
	}

	return semid;
}

int neb_sem_proc_destroy(int semid)
{
	if (semctl(semid, 0, IPC_RMID) != 0) {
		neb_syslogl(LOG_ERR, "semctl(IPC_RMID): %m");
		return -1;
	}
	return 0;
}

int neb_sem_proc_setval(int semid, int subid, int value)
{
	union semun arg = {.val = value};
	if (semctl(semid, subid, SETVAL, arg) != 0) {
		neb_syslogl(LOG_ERR, "semctl: %m");
		return -1;
	}
	return 0;
}

int neb_sem_proc_post(int semid, int subid)
{
	struct sembuf sb = {
		.sem_num = subid,
		.sem_op = 1,
		.sem_flg = IPC_NOWAIT,
	};

	for (;;) {
		if (semop(semid, &sb, 1) == -1) {
			if (errno == EINTR)
				continue;
			neb_syslogl(LOG_ERR, "semop: %m");
			return -1;
		}
		return 0;
	}
}

static int neb_sem_timedop(int semid, struct sembuf *sops, int nsops, struct timespec *timeout)
{
#if defined(OS_LINUX) || defined(OSTYPE_SUN)
	if (semtimedop(semid, sops, nsops, timeout) == -1) {
		if (errno != EINTR)
			neb_syslogl(LOG_ERR, "semtimedop: %m");
		return -1;
	}
	return 0;
#elif defined(OSTYPE_BSD) || defined(OS_DARWIN)
	int timeout_ms = 0;
	if (timeout->tv_sec)
		timeout_ms += timeout->tv_sec * 1000;
	if (timeout->tv_nsec) {
		timeout_ms += timeout->tv_nsec / 1000000;
		if (timeout->tv_nsec % 1000000)
			timeout_ms += 1;
	}
	for (int i = 0; i < timeout_ms; i++) {
		if (semop(semid, sops, nsops) == -1) {
			switch (errno) {
			case EINTR:
				i -= 1;
				continue;
				break;
			case EAGAIN:
				usleep(1000);
				continue;
				break;
			default:
				neb_syslogl(LOG_ERR, "semop: %m");
				return -1;
				break;
			}
		}
		return 0;
	}
	errno = ETIMEDOUT;
	return -1;
#else
# error "fix me"
#endif
}

int neb_sem_proc_wait_count(int semid, int subid, int count, struct timespec *timeout)
{
	struct sembuf sb = {
		.sem_num = subid,
		.sem_op = 0 - count,
#if defined(OS_LINUX) || defined(OSTYPE_SUN)
		.sem_flg = 0,
#elif defined(OSTYPE_BSD) || defined(OS_DARWIN)
		.sem_flg = IPC_NOWAIT,
#else
# error "fix me"
#endif
	};

	return neb_sem_timedop(semid, &sb, 1, timeout);
}

int neb_sem_proc_wait_zeroed(int semid, int subid, struct timespec *timeout)
{
	struct sembuf sb = {
		.sem_num = subid,
		.sem_op = 0,
#if defined(OS_LINUX) || defined(OSTYPE_SUN)
		.sem_flg = 0,
#elif defined(OSTYPE_BSD) || defined(OS_DARWIN)
		.sem_flg = IPC_NOWAIT,
#else
# error "fix me"
#endif
	};

	return neb_sem_timedop(semid, &sb, 1, timeout);
}

int neb_sem_proc_wait_removed(int semid, int subid, struct timespec *timeout)
{
	struct sembuf sb = {
		.sem_num = subid,
		.sem_op = -1,
#if defined(OS_LINUX) || defined(OSTYPE_SUN)
		.sem_flg = 0,
#elif defined(OSTYPE_BSD) || defined(OS_DARWIN)
		.sem_flg = IPC_NOWAIT,
#else
# error "fix me"
#endif
	};

#if defined(OS_LINUX) || defined(OSTYPE_SUN)
	if (semtimedop(semid, &sb, 1, timeout) == -1) {
		switch (errno) {
		case EINVAL:
		case EIDRM:
			return 0;
			break;
		default:
			break;
		}
		neb_syslogl(LOG_ERR, "semtimedop: %m");
		return -1;
	}
	return -1;
#elif defined(OSTYPE_BSD) || defined(OS_DARWIN)
	int timeout_ms = 0;
	if (timeout->tv_sec)
		timeout_ms += timeout->tv_sec * 1000;
	if (timeout->tv_nsec) {
		timeout_ms += timeout->tv_nsec / 1000000;
		if (timeout->tv_nsec % 1000000)
			timeout_ms += 1;
	}
	for (int i = 0; i < timeout_ms; i++) {
		if (semop(semid, &sb, 1) == -1) {
			switch (errno) {
			case EINVAL:
			case EIDRM:
				return 0;
				break;
			case EAGAIN:
				usleep(1000);
				continue;
				break;
			default:
				neb_syslogl(LOG_ERR, "semop: %m");
				return -1;
				break;
			}
		}
		return -1;
	}
	errno = ETIMEDOUT;
	return -1;
#else
# error "fix me"
#endif
}
