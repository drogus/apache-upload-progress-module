/**
 * This macro is not present in Apache HTTP server version 2.2.3 
 * Red-Hat 5 / CentOS 5
 */
#ifndef ap_is_HTTP_VALID_RESPONSE
#  define ap_is_HTTP_VALID_RESPONSE(x) (((x) >= 100)&&((x) < 600))
#endif

/*
 * apr_time_from_msec is defined in APR 1.4.0 or later
 */
#if (APR_MAJOR_VERSION < 1) || ((APR_MAJOR_VERSION == 1) && (APR_MINOR_VERSION < 4))
#define apr_time_from_msec(x) (x * 1000)
#endif

/*
 * The following is a patch ported from mod_fcgid for Apache 2.0 releases
 * which don't provide a version of apr_shm_remove.
 *
 * Please have a look at the mod_fcgid mailing list for details.
 * The original patch can be found at:
 *     http://lists.freebsd.org/pipermail/freebsd-ports-bugs/2007-June/122731.html
 */
/* BEGIN OF PATCH ---------------------------------------------------------- */
/* apr version 0.x not support apr_shm_remove, I have to copy it from apr version 1.x */
#if (APR_MAJOR_VERSION < 1)

#if APR_HAS_SHARED_MEMORY

	#ifdef HAVE_SYS_MMAN_H
		#include <sys/mman.h>
	#endif
	#ifdef HAVE_SYS_IPC_H
		#include <sys/ipc.h>
	#endif
	#ifdef HAVE_SYS_MUTEX_H
		#include <sys/mutex.h>
	#endif
	#ifdef HAVE_SYS_SHM_H
		#include <sys/shm.h>
	#endif
	#if !defined(SHM_R)
		#define SHM_R 0400
	#endif
	#if !defined(SHM_W)
		#define SHM_W 0200
	#endif
	#ifdef HAVE_SYS_FILE_H
		#include <sys/file.h>
	#endif

/* To avoid conflicts with mod_fcgid and that instance of the patch there.
 * This basically replaces each of our instances of apr_shm_remove with a local
 * identifier only used by us thus avoiding hard-to-find bugs if the wrong
 * version of this function gets linked in when loading the module.
 *
 * If re-using this patch you are therefore encouraged to change the name of
 * the resulting function in the makro and the function declaration below.
 */
#define apr_shm_remove modup_backport_apr_shm_remove
static apr_status_t modup_backport_apr_shm_remove(const char *filename, apr_pool_t * pool)
{
#if APR_USE_SHMEM_SHMGET
    apr_status_t status;
    apr_file_t *file;
    key_t shmkey;
    int shmid;
#endif

#if APR_USE_SHMEM_MMAP_TMP
    return apr_file_remove(filename, pool);
#endif

#if APR_USE_SHMEM_MMAP_SHM
    if (shm_unlink(filename) == -1) {
        return errno;
    }
    return APR_SUCCESS;
#endif

#if APR_USE_SHMEM_SHMGET
    /* Presume that the file already exists; just open for writing */
    status = apr_file_open(&file, filename, APR_WRITE, APR_OS_DEFAULT, pool);
    if (status) {
        return status;
    }

    /* ftok() (on solaris at least) requires that the file actually
     * exist before calling ftok(). */
    shmkey = ftok(filename, 1);
    if (shmkey == (key_t) - 1) {
        goto shm_remove_failed;
    }

    apr_file_close(file);
    if ((shmid = shmget(shmkey, 0, SHM_R | SHM_W)) < 0) {
        goto shm_remove_failed;
    }

    /* Indicate that the segment is to be destroyed as soon
     * as all processes have detached. This also disallows any
     * new attachments to the segment. */
    if (shmctl(shmid, IPC_RMID, NULL) == -1) {
        goto shm_remove_failed;
    }
    return apr_file_remove(filename, pool);

shm_remove_failed:
    status = errno;
    /* ensure the file has been removed anyway. */
    apr_file_remove(filename, pool);
    return status;
#endif

    /* No support for anonymous shm */
    return APR_ENOTIMPL;
}
/* END OF PATCH ------------------------------------------------------------ */

#endif
/* (APR_MAJOR_VERSION < 1) */
#endif
