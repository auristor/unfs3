/*
 * UNFS3 filehandle cache
 * (C) 2003, Pascal Schmidt <der.eremit@email.de>
 * see file LICENSE for license details
 */

#ifndef UNFS3_FH_CACHE_H
#define UNFS3_FH_CACHE_H

/* statistics */
extern int fh_cache_max;
extern int fh_cache_use;
extern int fh_cache_hit;

void fh_cache_init(void);

char *fh_decomp(nfs_fh3 fh);
unfs3_fh_t fh_comp(const char *path, int need_dir);

char *fh_cache_add(uint32 dev, uint32 ino, const char *path);

#endif