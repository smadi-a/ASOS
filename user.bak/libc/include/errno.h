/*
 * errno.h — Error numbers.
 */

#ifndef _ERRNO_H
#define _ERRNO_H

extern int errno;

#define EPERM   1
#define ENOENT  2
#define ESRCH   3
#define EINTR   4
#define EIO     5
#define ENOMEM  12
#define EACCES  13
#define EFAULT  14
#define EEXIST  17
#define EINVAL  22
#define ENOSYS  38
#define ERANGE  34
#define ENAMETOOLONG 36

#endif /* _ERRNO_H */
