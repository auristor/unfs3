/*
 * UNFS3 low-level filehandle routines
 * (C) 2004, Pascal Schmidt <der.eremit@email.de>
 * see file LICENSE for license details
 */

#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <rpc/rpc.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if HAVE_LINUX_EXT2_FS_H == 1
# include <linux/ext2_fs.h>
#endif

#include "nfs.h"
#include "fh.h"

/*
 * hash function for inode numbers
 */
#define FH_HASH(n)	((n + 3 * (n >> 8) + 5 * (n >> 16)) & 0xFF)

/*
 * stat cache
 */
int st_cache_valid = FALSE;
struct stat st_cache;

/*
 * --------------------------------
 * INODE GENERATION NUMBER HANDLING
 * --------------------------------
 */

/*
 * obtain inode generation number if possible
 *
 * obuf: filled out stat buffer (must be given!)
 * fd:   open fd to file or FD_NONE (-1) if no fd open
 * path: path to object in case we need to open it here
 *
 * returns 0 on failure
 */
uint32
get_gen(struct stat obuf, U(int fd), U(const char *path))
{
#if HAVE_STRUCT_STAT_ST_GEN == 1
    return obuf.st_gen;
#endif

#if HAVE_STRUCT_STAT_ST_GEN == 0 && HAVE_LINUX_EXT2_FS_H == 1
    int newfd, res;
    uint32 gen;

    if (!S_ISREG(obuf.st_mode) && !S_ISDIR(obuf.st_mode))
        return 0;

    if (fd != FD_NONE) {
        res = ioctl(fd, EXT2_IOC_GETVERSION, &gen);
        if (res == -1)
            return 0;
        else
            return gen;
    }
    else {
        newfd = open(path, O_RDONLY);
        if (newfd == -1)
            return 0;

        res = ioctl(newfd, EXT2_IOC_GETVERSION, &gen);
        close(newfd);

        if (res == -1)
            return 0;
        else
            return gen;
    }
#endif

#if HAVE_STRUCT_STAT_ST_GEN == 0 && HAVE_LINUX_EXT2_FS_H == 0
    return obuf.st_ino;
#endif
}

/*
 * --------------------------------
 * FILEHANDLE COMPOSITION FUNCTIONS
 * --------------------------------
 */

/*
 * check whether an NFS filehandle is valid
 */
int
nfh_valid(nfs_fh3 fh)
{
    unfs3_fh_t *obj = (void *) fh.data.data_val;

    /* too small? */
    if (fh.data.data_len < FH_MINLEN)
        return FALSE;

    /* encoded length different from real length? */
    if (fh.data.data_len != fh_len(obj))
        return FALSE;

    return TRUE;
}

/*
 * check whether a filehandle is valid
 */
int
fh_valid(unfs3_fh_t fh)
{
    /* invalid filehandles have zero device and inode */
    return (int) (fh.dev != 0 && fh.ino != 0);
}

/*
 * invalid fh for error returns
 */
static const unfs3_fh_t invalid_fh =
    {.dev = 0,.ino = 0,.gen = 0,.len = 0,.inos = {0} };

/*
 * compose a filehandle for a given path
 * path:     path to compose fh for
 * need_dir: if not 0, path must point to a directory
 */
unfs3_fh_t
fh_comp_raw(const char *path, int need_dir)
{
    char work[NFS_MAXPATHLEN];
    unfs3_fh_t fh;
    struct stat buf;
    int res;
    char *last;
    int pos = 0;

    res = lstat(path, &buf);
    if (res == -1)
        return invalid_fh;

    /* check for dir if need_dir is set */
    if (need_dir != 0 && !S_ISDIR(buf.st_mode))
        return invalid_fh;

    fh.dev = buf.st_dev;
    fh.ino = buf.st_ino;
    fh.gen = get_gen(buf, FD_NONE, path);

    /* special case for root directory */
    if (strcmp(path, "/") == 0)
        return fh;

    strcpy(work, path);
    last = work;

    do {
        *last = '/';
        last = strchr(last + 1, '/');
        if (last != NULL)
            *last = 0;

        res = lstat(work, &buf);
        if (res == -1) {
            return invalid_fh;
        }

        /* store 8 bit hash of the component's inode */
        fh.inos[pos] = FH_HASH(buf.st_ino);
        pos++;

    } while (last && pos < FH_MAXLEN);

    if (last)                   /* path too deep for filehandle */
        return invalid_fh;

    fh.len = pos;

    return fh;
}

/*
 * get real length of a filehandle
 */
u_int
fh_len(const unfs3_fh_t * fh)
{
    return fh->len + sizeof(fh->len) + sizeof(fh->dev) +
        sizeof(fh->ino) + sizeof(fh->gen);
}

/*
 * extend a filehandle with a given device, inode, and generation number
 */
unfs3_fh_t *
fh_extend(nfs_fh3 nfh, uint32 dev, uint32 ino, uint32 gen)
{
    static unfs3_fh_t new;
    unfs3_fh_t *fh = (void *) nfh.data.data_val;

    if (fh->len == FH_MAXLEN)
        return NULL;

    memcpy(&new, fh, fh_len(fh));

    new.dev = dev;
    new.ino = ino;
    new.gen = gen;
    new.inos[new.len] = FH_HASH(ino);
    new.len++;

    return &new;
}

/*
 * get post_op_fh3 extended by device, inode, and generation number
 */
post_op_fh3
fh_extend_post(nfs_fh3 fh, uint32 dev, uint32 ino, uint32 gen)
{
    post_op_fh3 post;
    unfs3_fh_t *new;

    new = fh_extend(fh, dev, ino, gen);

    if (new) {
        post.handle_follows = TRUE;
        post.post_op_fh3_u.handle.data.data_len = fh_len(new);
        post.post_op_fh3_u.handle.data.data_val = (char *) new;
    }
    else
        post.handle_follows = FALSE;

    return post;
}

/*
 * extend a filehandle given a path and needed type
 */
post_op_fh3
fh_extend_type(nfs_fh3 fh, const char *path, unsigned int type)
{
    post_op_fh3 result;
    struct stat buf;
    int res;

    res = lstat(path, &buf);
    if (res == -1 || (buf.st_mode & type) != type) {
        st_cache_valid = FALSE;
        result.handle_follows = FALSE;
        return result;
    }

    st_cache_valid = TRUE;
    st_cache = buf;

    return fh_extend_post(fh, buf.st_dev, buf.st_ino,
                          get_gen(buf, FD_NONE, path));
}

/*
 * -------------------------------
 * FILEHANDLE RESOLUTION FUNCTIONS
 * -------------------------------
 */

/*
 * filehandles have the following fields:
 * dev:  device of the file system object fh points to
 * ino:  inode of the file system object fh points to
 * gen:  inode generation number, if available
 * len:  number of entries in following inos array
 * inos: array of max FH_MAXLEN directories needed to traverse to reach
 *       object, for each name, an 8 bit hash of the inode number is stored
 *
 * - search functions traverse directory structure from the root looking
 *   for directories matching the inode information stored
 * - if such a directory is found, we descend into it trying to locate the
 *   object
 */

/*
 * recursive directory search
 * fh:     filehandle being resolved
 * pos:    position in filehandles path inode array
 * lead:   current directory for search
 * result: where to store path if seach is complete
 */
static int
fh_rec(const unfs3_fh_t * fh, int pos, const char *lead, char *result)
{
    DIR *search;
    struct dirent *entry;
    struct stat buf;
    int res, rec;
    char obj[NFS_MAXPATHLEN];

    /* went in too deep? */
    if (pos == fh->len)
        return FALSE;

    search = opendir(lead);
    if (!search)
        return FALSE;

    entry = readdir(search);

    while (entry) {
        if (strlen(lead) + strlen(entry->d_name) + 1 < NFS_MAXPATHLEN) {

            sprintf(obj, "%s/%s", lead, entry->d_name);

            res = lstat(obj, &buf);
            if (res == -1) {
                buf.st_dev = 0;
                buf.st_ino = 0;
            }

            if (buf.st_dev == fh->dev && buf.st_ino == fh->ino) {
                /* found the object */
                closedir(search);
                sprintf(result, "%s/%s", lead + 1, entry->d_name);
                /* update stat cache */
                st_cache_valid = TRUE;
                st_cache = buf;
                return TRUE;
            }

            if (strcmp(entry->d_name, "..") != 0 &&
                strcmp(entry->d_name, ".") != 0 &&
                FH_HASH(buf.st_ino) == fh->inos[pos]) {
                /*
                 * might be directory we're looking for,
                 * try descending into it
                 */
                rec = fh_rec(fh, pos + 1, obj, result);
                if (rec) {
                    /* object was found in dir */
                    closedir(search);
                    return TRUE;
                }
            }
        }
        entry = readdir(search);
    }

    closedir(search);
    return FALSE;
}

/*
 * resolve a filehandle into a path
 */
char *
fh_decomp_raw(const unfs3_fh_t * fh)
{
    int rec = 0;
    static char result[NFS_MAXPATHLEN];

    /* valid fh? */
    if (!fh)
        return NULL;

    /* special case for root directory */
    if (fh->len == 0)
        return "/";

    rec = fh_rec(fh, 0, "/", result);

    if (rec)
        return result;

    /* could not find object */
    return NULL;
}