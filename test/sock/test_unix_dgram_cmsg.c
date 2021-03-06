
#include <nebase/sock/unix.h>
#include <nebase/sock/common.h>
#include <nebase/sem.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <poll.h>

static struct sockaddr_un addr = {
	.sun_family = AF_UNIX,
	.sun_path = "/tmp/.nebase.test"
};

static int test_unix_sock_cred(void)
{
	int sfd = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (sfd == -1) {
		perror("socket");
		return -1;
	}
	if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
		perror("bind");
		return -1;
	}

	char tmp_file[] = "/tmp/.nebase.test.sem-XXXXXX";
	int fd = mkstemp(tmp_file);
	close(fd);
	int semid = neb_sem_proc_create(tmp_file, 1);

# define BUFLEN 4
	char wbuf[BUFLEN] = {0x01, 0x02, 0x03, 0x04};
	char rbuf[BUFLEN] = NEB_STRUCT_INITIALIZER;

	int ret = 0;
	pid_t cpid = fork();
	if (cpid == -1) {
		perror("fork");
		neb_sem_proc_destroy(semid);
		unlink(tmp_file);
		return -1;
	}
	if (cpid == 0) {
		close(sfd);
		int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
		if (fd == -1) {
			perror("socket");
			return -1;
		}

		fprintf(stdout, "Sending with cred from child\n");
		int nw = neb_sock_unix_send_with_cred(fd, wbuf, BUFLEN, &addr, sizeof(addr));
		if (nw != BUFLEN) {
			fprintf(stderr, "Failed to send with cred\n");
			return -1;
		}

		int null_fd = open("/dev/null", O_RDWR);
		if (null_fd == -1) {
			perror("open");
			return -1;
		}

		struct timespec ts = {.tv_sec = 4, .tv_nsec = 0}; // 4s
		if (neb_sem_proc_wait_count(semid, 0, 1, &ts) != 0) {
			fprintf(stderr, "Failed to wait sem\n");
			return -1;
		}

		fprintf(stdout, "Sending with fd from child\n");
		nw = neb_sock_unix_send_with_fds(fd, wbuf, BUFLEN, &null_fd, 1, &addr, sizeof(addr));
		if (nw != BUFLEN) {
			fprintf(stderr, "Failed to send data along with fd %d\n", null_fd);
			return -1;
		}
		close(null_fd);

		close(fd);
	} else {
		if (neb_sock_unix_enable_recv_cred(SOCK_DGRAM, sfd) != 0) {
			fprintf(stderr, "Failed to enable recv of cred\n");
			ret = -1;
			goto exit_unlink;
		}

		int hup = 0;
		if (!neb_sock_timed_read_ready(sfd, 500, &hup)) {
			fprintf(stderr, "Timeout to wait for data along with cred\n");
			ret = -1;
			goto exit_unlink;
		}
		if (hup) {
			fprintf(stderr, "sock closed unexpectedly at stage 1\n");
			ret = -1;
			goto exit_unlink;
		}

		struct neb_ucred u;
		int nr = neb_sock_unix_recv_with_cred(SOCK_DGRAM, sfd, rbuf, BUFLEN, &u);
		if (nr != BUFLEN) {
			fprintf(stderr, "Failed to recv along with cred\n");
			ret = -1;
			goto exit_unlink;
		}

		uid_t ruid = getuid();
		gid_t rgid = getgid();
		fprintf(stdout, "Children pid: %d, uid: %d, gid: %d\n", cpid, ruid, rgid);
		fprintf(stdout, "Received pid: %d, uid: %d, gid: %d\n", u.pid, u.uid, u.gid);
		if (u.pid != cpid || u.uid != ruid || u.gid != rgid) {
			fprintf(stderr, "cred not match\n");
			ret = -1;
			goto exit_unlink;
		}
		if (memcmp(wbuf, rbuf, BUFLEN) != 0) {
			fprintf(stderr, "wbuf != rbuf in stage 1\n");
			ret = -1;
			goto exit_unlink;
		}

		neb_sem_proc_post(semid, 0);

		if (!neb_sock_timed_read_ready(sfd, 500, &hup)) {
			fprintf(stderr, "Timeout to wait for data along with fd\n");
			ret = -1;
			goto exit_unlink;
		}
		if (hup) {
			fprintf(stderr, "sock closed unexpectedly at stage 2\n");
			ret = -1;
			goto exit_unlink;
		}

		int null_fd = -1;
		int fd_num = 0;
		nr = neb_sock_unix_recv_with_fds(sfd, rbuf, sizeof(rbuf), &null_fd, &fd_num);
		if (nr != (int)sizeof(rbuf)) {
			fprintf(stderr, "Failed to recv along with fd\n");
			ret = -1;
			goto exit_unlink;
		}
		close(sfd);
		if (fd_num != 1) {
			fprintf(stderr, "fd_num mismatch, exp 1, real: %d\n", fd_num);
			ret = -1;
			goto exit_unlink;
		}
		if (memcmp(wbuf, rbuf, BUFLEN) != 0) {
			fprintf(stderr, "wbuf != rbuf in stage 2\n");
			ret = -1;
			goto exit_unlink;
		}
		close(null_fd);

		int wstatus = 0;
		waitpid(cpid, &wstatus, 0);
		if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) {
			fprintf(stderr, "Child exit with error\n");
			ret = -1;
		}

exit_unlink:
		neb_sem_proc_destroy(semid);
		unlink(tmp_file);
	}

	return ret;
}

int main(void)
{
	unlink(addr.sun_path);
	int ret = test_unix_sock_cred();
	unlink(addr.sun_path);
	return ret;
}
