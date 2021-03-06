
#ifndef NEB_SRC_SOCK_PLATFORM_NETBSD_H
#define NEB_SRC_SOCK_PLATFORM_NETBSD_H 1

#include <nebase/cdefs.h>

#include <stdint.h>

/**
 * \param[out] sockptr *sockptr equals ki_fdata in struct kinfo_file
 * \note see source code for sockstat for more info
 */
extern int neb_sock_unix_get_sockptr(const char *path, uint64_t *sockptr, int *type)
	_nattr_hidden _nattr_nonnull((1, 2, 3));

#endif
