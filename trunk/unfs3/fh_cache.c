/*
 * UNFS3 filehandle cache
 * (C) 2004, Pascal Schmidt <der.eremit@email.de>
 * see file LICENSE for license details
 */

#include "config.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <rpc/rpc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nfs.h"
#include "fh.h"
#include "fh_cache.h"

/* number of entries in fh cache */
#define CACHE_ENTRIES	4096

typedef struct
{
    uint32 dev;                 /* device */
    uint32 ino;                 /* inode */
    char path[NFS_MAXPATHLEN];  /* pathname */
    time_t use;                 /* last use */
} unfs3_cache_t;

static unfs3_cache_t fh_cache[CACHE_ENTRIES];

/* statistics */
int fh_cache_max = 0;
int fh_cache_use = 0;
int fh_cache_hit = 0;

/* counter for LRU */
static int fh_cache_time = 0;

/*
 * return next pseudo-time value for LRU counter
 */
static int
fh_cache_next(void)
{
    return ++fh_cache_time;
}

/*
 * initialize cache
 */
void
fh_cache_init(void)
{
    memset(fh_cache, 0, sizeof(unfs3_cache_t) * CACHE_ENTRIES);
}

/*
 * find cache index to use for new entry
 * returns either an empty slot or the least recently used slot if no
 * empty slot is present
 */
static int
fh_cache_lru(void)
{
    int best = -1;
    int best_idx = 0;
    int i;

    /* if cache is not full, we simply hand out the next slot */
    if (fh_cache_max < CACHE_ENTRIES - 1)
        return fh_cache_max++;

    for (i = 0; i < CACHE_ENTRIES; i++) {
        if (fh_cache[i].use == 0)
            return i;
        if (fh_cache[i].use < best) {
            best = fh_cache[i].use;
            best_idx = i;
        }
    }
    return best_idx;
}

/*
 * invalidate (clear) a cache entry
 */
static void
fh_cache_inval(int idx)
{
    fh_cache[idx].dev = 0;
    fh_cache[idx].ino = 0;
    fh_cache[idx].use = 0;
    fh_cache[idx].path[0] = 0;
}

/*
 * find index given device and inode number
 */
static int
fh_cache_index(uint32 dev, uint32 ino)
{
    int i, res = -1;

    for (i = 0; i < fh_cache_max + 1; i++)
        if (fh_cache[i].dev == dev && fh_cache[i].ino == ino) {
            res = i;
            break;
        }

    return res;
}

/*
 * add an entry to the filehandle cache
 */
void
fh_cache_add(uint32 dev, uint32 ino, const char *path)
{
    int idx;

    /* if we already have a matching entry, overwrite that */
    idx = fh_cache_index(dev, ino);

    /* otherwise overwrite least recently used entry */
    if (idx == -1)
        idx = fh_cache_lru();

    fh_cache_inval(idx);

    fh_cache[idx].dev = dev;
    fh_cache[idx].ino = ino;
    fh_cache[idx].use = fh_cache_next();

    strcpy(fh_cache[idx].path, path);
}

/*
 * lookup an entry in the cache given a device, inode, and generation number
 */
static char *
fh_cache_lookup(uint32 dev, uint32 ino)
{
    int i, res;
    struct stat buf;

    i = fh_cache_index(dev, ino);

    if (i != -1) {
        /* check whether path to <dev,ino> relation still holds */
        res = lstat(fh_cache[i].path, &buf);
        if (res == -1) {
            /* object does not exist any more */
            fh_cache_inval(i);
            return NULL;
        }
        if (buf.st_dev == dev && buf.st_ino == ino) {
            /* cache hit, update time on cache entry */
            fh_cache[i].use = fh_cache_next();
            /* update stat cache */
            st_cache_valid = TRUE;
            st_cache = buf;
            return fh_cache[i].path;
        }
        else {
            /* path to <dev,ino> relation has changed */
            fh_cache_inval(i);
            return NULL;
        }
    }

    return NULL;
}

/*
 * resolve a filename into a path
 * cache-using wrapper for fh_decomp_raw
 */
char *
fh_decomp(nfs_fh3 fh)
{
    char *result;
    unfs3_fh_t *obj = (void *) fh.data.data_val;

    if (!nfh_valid(fh)) {
        st_cache_valid = FALSE;
        return NULL;
    }

    /* try lookup in cache, increase cache usage counter */
    result = fh_cache_lookup(obj->dev, obj->ino);
    fh_cache_use++;

    if (!result) {
        /* not found, resolve the hard way */
        result = fh_decomp_raw(obj);
        if (result)
            /* add to cache for later use if resolution ok */
            fh_cache_add(obj->dev, obj->ino, result);
        else
            /* could not resolve in any way */
            st_cache_valid = FALSE;
    }
    else
        /* found, update cache hit statistic */
        fh_cache_hit++;

    return result;
}

/*
 * compose a filehandle for a path
 * cache-using wrapper for fh_comp_raw
 */
unfs3_fh_t
fh_comp(const char *path, int need_dir)
{
    unfs3_fh_t res;

    res = fh_comp_raw(path, need_dir);
    if (fh_valid(res))
        /* add to cache for later use */
        fh_cache_add(res.dev, res.ino, path);

    return res;
}