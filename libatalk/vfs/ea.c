/*
  $Id: ea.c,v 1.2 2009-10-02 14:57:57 franklahm Exp $
  Copyright (c) 2009 Frank Lahm <franklahm@gmail.com>

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.
*/

/* According to man fsattr.5 we must define _ATFILE_SOURCE */
#ifdef HAVE_SOLARIS_EAS
#define _ATFILE_SOURCE
#endif

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif /* HAVE_CONFIG_H */

#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>

#include <atalk/adouble.h>
#include <atalk/ea.h>
#include <atalk/afp.h>
#include <atalk/logger.h>
#include <atalk/volume.h>
#include <atalk/vfs.h>
#include <atalk/util.h>

/*
 * Store Extended Attributes inside .AppleDouble folders as follows:
 *
 * filename "fileWithEAs" with EAs "testEA1" and "testEA2"
 *
 * - create header with with the format struct adouble_ea_ondisk, the file is written to
 *   ".AppleDouble/fileWithEAs::EA"
 * - store EAs in files "fileWithEAs::EA::testEA1" and "fileWithEAs::EA::testEA2"
 */

/*
 * Function: unpack_header
 *
 * Purpose: unpack and verify header file data buffer at ea->ea_data into struct ea
 *
 * Arguments:
 *
 *    ea      (rw) handle to struct ea
 *
 * Returns: 0 on success, -1 on error
 *
 * Effects:
 *
 * Verifies magic and version.
 */
static int unpack_header(struct ea * restrict ea)
{
    int ret = 0, count = 0;
    uint32_t uint32;
    char *buf;

    /* Check magic and version */
    buf = ea->ea_data;
    if (*(uint32_t *)buf != htonl(EA_MAGIC)) {
        LOG(log_error, logtype_afpd, "unpack_header: wrong magic 0x%08x", *(uint32_t *)buf);
        ret = -1;
        goto exit;
    }
    buf += 4;
    if (*(uint16_t *)buf != htons(EA_VERSION)) {
        LOG(log_error, logtype_afpd, "unpack_header: wrong version 0x%04x", *(uint16_t *)buf);
        ret = -1;
        goto exit;
    }
    buf += 2;

    /* Get EA count */
    ea->ea_count = ntohs(*(uint16_t *)buf);
    LOG(log_debug, logtype_afpd, "unpack_header: number of EAs: %u", ea->ea_count);
    buf += 2;

    if (ea->ea_count == 0)
        return 0;

    /* Allocate storage for the ea_entries array */
    ea->ea_entries = malloc(sizeof(struct ea_entry) * ea->ea_count);
    if ( ! ea->ea_entries) {
        LOG(log_error, logtype_afpd, "unpack_header: OOM");
        ret = -1;
        goto exit;
    }

    buf = ea->ea_data + EA_HEADER_SIZE;
    while (count < ea->ea_count) {
        memcpy(&uint32, buf, 4); /* EA size */
        buf += 4;
        (*(ea->ea_entries))[count].ea_size = ntohl(uint32);
        (*(ea->ea_entries))[count].ea_name = strdup(buf);
        if (! (*(ea->ea_entries))[count].ea_name) {
            LOG(log_error, logtype_afpd, "unpack_header: OOM");
            ret = -1;
            goto exit;
        }
        (*(ea->ea_entries))[count].ea_namelen = strlen((*(ea->ea_entries))[count].ea_name);
        buf += (*(ea->ea_entries))[count].ea_namelen + 1;

        LOG(log_maxdebug, logtype_afpd, "unpack_header: entry no:%u,\"%s\", size: %u, namelen: %u", count,
            (*(ea->ea_entries))[count].ea_name,
            (*(ea->ea_entries))[count].ea_size,
            (*(ea->ea_entries))[count].ea_namelen);

        count++;
    }

exit:
    return ret;
}

/*
 * Function: pack_header
 *
 * Purpose: pack everything from struct ea into buffer at ea->ea_data
 *
 * Arguments:
 *
 *    ea      (rw) handle to struct ea
 *
 * Returns: 0 on success, -1 on error
 *
 * Effects:
 *
 * adjust ea->ea_count in case an ea entry deletetion is detected
 */
static int pack_header(struct ea * restrict ea)
{
    int count = 0, eacount = 0;
    uint16_t uint16;
    uint32_t uint32;
    size_t bufsize = EA_HEADER_SIZE;

    char *buf = ea->ea_data + EA_HEADER_SIZE;

    LOG(log_debug, logtype_afpd, "pack_header('%s'): ea_count: %u, ea_size: %u",
        ea->filename, ea->ea_count, ea->ea_size);

    if (ea->ea_count == 0)
        /* nothing to do, magic, version and count are still valid in buffer */
        return 0;

    while(count < ea->ea_count) { /* the names */
        /* Check if its a deleted entry */
        if ( ! ((*ea->ea_entries)[count].ea_name)) {
            count++;
            continue;
        }

        bufsize += (*(ea->ea_entries))[count].ea_namelen + 1;
        count++;
        eacount++;
    }

    bufsize += (eacount * 4); /* header + ea_size for each EA */
    if (bufsize > ea->ea_size) {
        /* we must realloc */
        if ( ! (buf = realloc(ea->ea_data, bufsize)) ) {
            LOG(log_error, logtype_afpd, "pack_header: OOM");
            return -1;
        }
        ea->ea_data = buf;
    }
    ea->ea_size = bufsize;

    /* copy count */
    uint16 = htons(eacount);
    memcpy(ea->ea_data + EA_COUNT_OFF, &uint16, 2);

    count = 0;
    buf = ea->ea_data + EA_HEADER_SIZE;
    while (count < ea->ea_count) {
        /* Check if its a deleted entry */
        if ( ! ((*ea->ea_entries)[count].ea_name)) {
            count++;
            continue;
        }

        /* First: EA size */
        uint32 = htonl((*(ea->ea_entries))[count].ea_size);
        memcpy(buf, &uint32, 4);
        buf += 4;

        /* Second: EA name as C-string */
        strcpy(buf, (*(ea->ea_entries))[count].ea_name);
        buf += (*(ea->ea_entries))[count].ea_namelen + 1;

        LOG(log_maxdebug, logtype_afpd, "pack_header: entry no:%u,\"%s\", size: %u, namelen: %u", count,
            (*(ea->ea_entries))[count].ea_name,
            (*(ea->ea_entries))[count].ea_size,
            (*(ea->ea_entries))[count].ea_namelen);

        count++;
    }

    ea->ea_count = eacount;

    LOG(log_debug, logtype_afpd, "pack_header('%s'): ea_count: %u, ea_size: %u",
        ea->filename, ea->ea_count, ea->ea_size);
    
    return 0;
}

/*
 * Function: ea_path
 *
 * Purpose: return name of ea header filename
 *
 * Arguments:
 *
 *    ea           (r) ea handle
 *    eaname       (r) name of EA or NULL
 *
 * Returns: pointer to name in static buffer
 *
 * Effects:
 *
 * Calls ad_open, copies buffer, appends "::EA" and if supplied append eanme
 * Files: "file" -> "file/.AppleDouble/file::EA"
 * Dirs: "dir" -> "dir/.AppleDouble/.Parent::EA"
 * "file" with EA "myEA" -> "file/.AppleDouble/file::EA:myEA"
 */
static char * ea_path(const struct ea * restrict ea,
                      const char * restrict eaname)
{
    char *adname;
    static char pathbuf[MAXPATHLEN + 1];

    /* get name of a adouble file from uname */
    adname = ea->vol->vfs->ad_path(ea->filename, (ea->ea_flags & EA_DIR) ? ADFLAGS_DIR : 0);
    /* copy it so we can work with it */
    strlcpy(pathbuf, adname, MAXPATHLEN + 1);
    /* append "::EA" */
    strlcat(pathbuf, "::EA", MAXPATHLEN + 1);

    if (eaname) {
        strlcat(pathbuf, "::", MAXPATHLEN + 1);
        strlcat(pathbuf, eaname, MAXPATHLEN + 1);
    }

    return pathbuf;
}

/*
 * Function: ea_addentry
 *
 * Purpose: add one EA into ea->ea_entries[]
 *
 * Arguments:
 *
 *    ea            (rw) pointer to struct ea
 *    uname         (r) name of file
 *    attruname     (r) name of EA
 *    attrsize      (r) size of ea
 *    bitmap        (r) bitmap from FP func
 *
 * Returns: new number of EA entries, -1 on error
 *
 * Effects:
 *
 * Grow array ea->ea_entries[]. If ea->ea_entries is still NULL, start allocating.
 * Otherwise realloc and put entry at the end. Increments ea->ea_count.
 */
static int ea_addentry(struct ea * restrict ea,
                       const char * restrict uname,
                       const char * restrict attruname,
                       size_t attrsize,
                       int bitmap)
{
    int count = 0;
    void *tmprealloc;

    /* First check if an EA of the requested name already exist */
    if (ea->ea_count > 0) {
        while (count < ea->ea_count) {
            if (strcmp(attruname, (*ea->ea_entries)[count].ea_name) == 0) {
                LOG(log_debug, logtype_afpd, "ea_addentry('%s'): exists", attruname);
                if (bitmap & kXAttrCreate)
                    /* its like O_CREAT|O_EXCL -> fail */
                    return -1;
                if ( ! (bitmap & kXAttrReplace))
                    /* replace was not requested, then its an error */
                    return -1;
                break;
            }
            count++;
        }
    }

    if (ea->ea_count == 0) {
        ea->ea_entries = malloc(sizeof(struct ea_entry));
        if ( ! ea->ea_entries) {
            LOG(log_error, logtype_afpd, "ea_addentry: OOM");
            return -1;
        }
    } else {
        tmprealloc = realloc(ea->ea_entries, sizeof(struct ea_entry) * (ea->ea_count + 1));
        if ( ! tmprealloc) {
            LOG(log_error, logtype_afpd, "ea_addentry: OOM");
            return -1;
        }
        ea->ea_entries = tmprealloc;
    }

    /* We've grown the array, now store the entry */
    (*(ea->ea_entries))[ea->ea_count].ea_size = attrsize;
    (*(ea->ea_entries))[ea->ea_count].ea_name = strdup(attruname);
    if ( ! (*(ea->ea_entries))[ea->ea_count].ea_name) {
        LOG(log_error, logtype_afpd, "ea_addentry: OOM");
        goto error;
    }
    (*(ea->ea_entries))[ea->ea_count].ea_namelen = strlen(attruname);

    ea->ea_count++;
    return ea->ea_count;

error:
    if (ea->ea_count == 0 && ea->ea_entries) {
        /* We just allocated storage but had an error somewhere -> free storage*/
        free(ea->ea_entries);
        ea->ea_entries = NULL;
    }
    ea->ea_count = 0;
    return -1;
}

/*
 * Function: ea_delentry
 *
 * Purpose: delete one EA from ea->ea_entries[]
 *
 * Arguments:
 *
 *    ea            (rw) pointer to struct ea
 *    uname         (r) name of EA
 *    attruname     (r) size of ea
 *
 * Returns: new number of EA entries, -1 on error
 *
 * Effects:
 *
 * Remove entry from  ea->ea_entries[]. Decrement ea->ea_count.
 * Marks it as unused just by freeing name and setting it to NULL.
 * ea_close and pack_buffer must honor this.
 */
static int ea_delentry(struct ea * restrict ea,
                       const char * restrict uname,
                       const char * restrict attruname)
{
    int ret = 0, count = 0;

    if (ea->ea_count == 0) {
        return -1;
    }

    while (count < ea->ea_count) {
        /* search matching EA */
        if (strcmp(attruname, (*ea->ea_entries)[count].ea_name) == 0) {
            free((*ea->ea_entries)[count].ea_name);
            (*ea->ea_entries)[count].ea_name = NULL;

            LOG(log_debug, logtype_afpd, "ea_delentry('%s'): deleted no %u/%u",
                attruname, count + 1, ea->ea_count);

            break;
        }
        count++;
    }

    return ret;
}

/*
 * Function: create_ea_header
 *
 * Purpose: create EA header file, only called from ea_open
 *
 * Arguments:
 *
 *    uname       (r)  filename for which we have to create a header
 *    ea          (rw) ea handle with already allocated storage pointed to
 *                     by ea->ea_data
 *
 * Returns: fd of open header file on success, -1 on error, errno semantics:
 *          EEXIST: open with O_CREAT | O_EXCL failed
 *
 * Effects:
 *
 * Creates EA header file and initialize ea->ea_data buffer.
 * Possibe race condition with other afpd processes:
 * we were called because header file didn't exist in eg. ea_open. We then
 * try to create a file with O_CREAT | O_EXCL, but the whole process in not atomic.
 * What do we do then? Someone else is in the process of creating the header too, but
 * it might not have finished it. That means we cant just open, read and use it!
 * We therefor currently just break with an error.
 * On return the header file is still r/w locked.
 */
static int create_ea_header(const char * restrict uname,
                            struct ea * restrict ea)
{
    int fd = -1, err = 0;
    char *ptr;

    if ((fd = open(uname, O_RDWR | O_CREAT | O_EXCL, 0666 & ~ea->vol->v_umask)) == -1) {
        LOG(log_error, logtype_afpd, "ea_create: open race condition with ea header for file: %s", uname);
        return -1;
    }

    /* lock it */
    if ((write_lock(fd, 0, SEEK_SET, 0)) != 0) {
        LOG(log_error, logtype_afpd, "ea_create: lock race condition with ea header for file: %s", uname);
        err = -1;
        goto exit;
    }

    /* Now init it */
    ptr = ea->ea_data;
    *(uint32_t *)ptr = htonl(EA_MAGIC);
    ptr += EA_MAGIC_LEN;
    *(uint16_t *)ptr = htons(EA_VERSION);
    ptr += EA_VERSION_LEN;
    *(uint16_t *)ptr = 0;       /* count */

    ea->ea_size = EA_HEADER_SIZE;

exit:
    if (err != 0) {
        close(fd);
        fd = -1;
    }
    return fd;
}

/*
 * Function: write_ea
 *
 * Purpose: write an EA to disk
 *
 * Arguments:
 *
 *    ea         (r) struct ea handle
 *    attruname  (r) EA name
 *    ibuf       (r) buffer with EA content
 *    attrsize   (r) size of EA
 *
 * Returns: 0 on success, -1 on error
 *
 * Effects:
 *
 * Creates/overwrites EA file.
 *
 */
static int write_ea(const struct ea * restrict ea,
                    const char * restrict attruname,
                    const char * restrict ibuf,
                    size_t attrsize)
{
    int fd = -1, ret = AFP_OK;
    struct stat st;
    char *eaname;

    eaname = ea_path(ea, attruname);
    LOG(log_maxdebug, logtype_afpd, "write_ea: ea_apth: %s", eaname);

    /* Check if it exists, remove if yes*/
    if ((stat(eaname, &st)) == 0) {
        if ((unlink(eaname)) != 0) {
            if (errno == EACCES)
                return AFPERR_ACCESS;
            else
                return AFPERR_MISC;
        }
    }

    if ((fd = open(eaname, O_RDWR | O_CREAT | O_EXCL, 0666 & ~ea->vol->v_umask)) == -1) {
        LOG(log_error, logtype_afpd, "write_ea: open race condition: %s", eaname);
        return -1;
    }

    /* lock it */
    if ((write_lock(fd, 0, SEEK_SET, 0)) != 0) {
        LOG(log_error, logtype_afpd, "write_ea: open race condition: %s", eaname);
        ret = -1;
        goto exit;
    }

    if ((write(fd, ibuf, attrsize)) != attrsize) {
        LOG(log_error, logtype_afpd, "write_ea: short write: %s", eaname);
        ret = -1;
        goto exit;
    }

exit:
    if (fd != -1)
        close(fd); /* and unlock */
    return ret;
}

/*
 * Function: delete_ea_file
 *
 * Purpose: delete EA file from disk
 *
 * Arguments:
 *
 *    ea         (r) struct ea handle
 *    attruname  (r) EA name
 *
 * Returns: 0 on success, -1 on error
 */
static int delete_ea_file(const struct ea * restrict ea,
                          const char *eaname)
{
    int ret = 0;
    char *eafile;
    struct stat st;

    eafile = ea_path(ea, eaname);

    /* Check if it exists, remove if yes*/
    if ((stat(eafile, &st)) == 0) {
        if ((unlink(eafile)) != 0) {
            LOG(log_error, logtype_afpd, "delete_ea_file('%s'): unlink: %s",
                eafile, strerror(errno));
            ret = -1;
        } else
            LOG(log_debug, logtype_afpd, "delete_ea_file('%s'): success", eafile);
    }

    return ret;
}

/*
 * Function: ea_open
 *
 * Purpose: open EA header file, create if it doesnt exits and called with O_CREATE
 *
 * Arguments:
 *
 *    vol         (r) current volume
 *    uname       (r) filename for which we have to open a header
 *    flags       (r) EA_CREATE: create if it doesn't exist (without it won't be created)
 *                    EA_RDONLY: open read only
 *                    EA_RDWR: open read/write
 *                    Eiterh EA_RDONLY or EA_RDWR MUST be requested
 *    ea          (w) pointer to a struct ea that we fill
 *
 * Returns: 0 on success, -1 on error
 *
 * Effects:
 *
 * opens header file and stores fd in ea->ea_fd. Size of file is put into ea->ea_size.
 * number of EAs is stored in ea->ea_count. flags are remembered in ea->ea_flags.
 * file is either read or write locked depending on the open flags.
 * When you're done with struct ea you must call ea_close on it.
 */
static int ea_open(const struct vol * restrict vol,
                   const char * restrict uname,
                   eaflags_t eaflags,
                   struct ea * restrict ea)
{
    int ret = 0;
    char *eaname;
    struct stat st;

    /* Enforce usage rules! */
    if ( ! (eaflags & (EA_RDONLY | EA_RDWR))) {
        LOG(log_error, logtype_afpd, "ea_open: called without EA_RDONLY | EA_RDWR", uname);
        return -1;
    }

    if ((stat(uname, &st)) != 0) {
        LOG(log_debug, logtype_afpd, "ea_open: cant stat: %s", uname);
        return -1;
    }

    /* set it all to 0 */
    memset(ea, 0, sizeof(struct ea));

    ea->vol = vol;              /* ea_close needs it */

    ea->ea_flags = eaflags;
    if (S_ISDIR(st.st_mode))
        ea->ea_flags |=  EA_DIR;

    if ( ! (ea->filename = strdup(uname))) {
        LOG(log_error, logtype_afpd, "ea_open: OOM");
        return -1;
    }

    eaname = ea_path(ea, NULL);
    LOG(log_maxdebug, logtype_afpd, "ea_open: ea_path: %s", eaname);

    /* Check if it exists, if not create it if EA_CREATE is in eaflags */
    if ((stat(eaname, &st)) != 0) {
        if (errno == ENOENT) {

            /* It doesnt exist */

            if ( ! (eaflags & EA_CREATE)) {
                /* creation was not requested, so return with error */
                ret = -1;
                goto exit;
            }

            /* Now create a header file */

            /* malloc buffer for minimal on disk data */
            ea->ea_data = malloc(EA_HEADER_SIZE);
            if (! ea->ea_data) {
                LOG(log_error, logtype_afpd, "ea_open: OOM");
                ret = -1;
                goto exit;
            }

            /* create it */
            ea->ea_fd = create_ea_header(eaname, ea);
            if (ea->ea_fd == -1) {
                ret = -1;
                goto exit;
            }

            return 0;

        } else {/* errno != ENOENT */
            ret = -1;
            goto exit;
        }
    }

    /* header file exists, so read and parse it */

    /* malloc buffer where we read disk file into */
    ea->ea_size = st.st_size;
    ea->ea_data = malloc(st.st_size);
    if (! ea->ea_data) {
        LOG(log_error, logtype_afpd, "ea_open: OOM");
        ret = -1;
        goto exit;
    }

    /* Now lock, open and read header file from disk */
    if ((ea->ea_fd = open(eaname, (ea->ea_flags & EA_RDWR) ? O_RDWR : O_RDONLY)) == -1) {
        LOG(log_error, logtype_afpd, "ea_open: error on open for header: %s", eaname);
        ret = -1;
        goto exit;
    }

    /* lock it */
    if (ea->ea_flags & EA_RDONLY) {
        /* read lock */
        if ((read_lock(ea->ea_fd, 0, SEEK_SET, 0)) != 0) {
            LOG(log_error, logtype_afpd, "ea_open: lock error on  header: %s", eaname);
            ret = -1;
            goto exit;
        }
    } else {  /* EA_RDWR */
        /* write lock */
        if ((write_lock(ea->ea_fd, 0, SEEK_SET, 0)) != 0) {
            LOG(log_error, logtype_afpd, "ea_open: lock error on  header: %s", eaname);
            ret = -1;
            goto exit;
        }
    }

    /* read it */
    if ((read(ea->ea_fd, ea->ea_data, ea->ea_size)) != ea->ea_size) {
        LOG(log_error, logtype_afpd, "ea_open: short read on header: %s", eaname);
        ret = -1;
        goto exit;
    }

    if ((unpack_header(ea)) != 0) {
        LOG(log_error, logtype_afpd, "ea_open: error unpacking header for: %s", eaname);
        ret = -1;
        goto exit;
    }

exit:
    if (ret != 0) {
        if (ea->ea_data) {
            free(ea->ea_data);
            ea->ea_data = NULL;
        }
        if (ea->ea_fd) {
            close(ea->ea_fd);
            ea->ea_fd = -1;
        }
    }
    return ret;
}

/*
 * Function: ea_close
 *
 * Purpose: flushes and closes an ea handle
 *
 * Arguments:
 *
 *    ea          (rw) pointer to ea handle
 *
 * Returns: 0 on success, -1 on error
 *
 * Effects:
 *
 * Flushes and then closes and frees all resouces held by ea handle.
 * Pack data in ea into ea_data, then write ea_data to disk
 */
static int ea_close(struct ea * restrict ea)
{
    int ret = 0, count = 0;
    char *eaname;
    struct stat st;

    LOG(log_debug, logtype_afpd, "ea_close('%s')", ea->filename);

    /* pack header and write it to disk if it was opened EA_RDWR*/
    if (ea->ea_flags & EA_RDWR) {
        if ((pack_header(ea)) != 0) {
            LOG(log_error, logtype_afpd, "ea_close: pack header");
            ret = -1;
        } else {
            if (ea->ea_count == 0) {
                /* Check if EA header exists and remove it */
                eaname = ea_path(ea, NULL);
                if ((stat(eaname, &st)) == 0) {
                    if ((unlink(eaname)) != 0) {
                        LOG(log_error, logtype_afpd, "ea_close('%s'): unlink: %s",
                            eaname, strerror(errno));
                        ret = -1;
                    }
                    else
                        LOG(log_debug, logtype_afpd, "ea_close(unlink '%s'): success", eaname);
                } else {
                    /* stat error */
                    if (errno != ENOENT) {
                        LOG(log_error, logtype_afpd, "ea_close('%s'): stat: %s",
                            eaname, strerror(errno));
                        ret = -1;
                    }
                }
            } else { /* ea->ea_count > 0 */
                if ((lseek(ea->ea_fd, 0, SEEK_SET)) == -1) {
                    LOG(log_error, logtype_afpd, "ea_close: lseek: %s", strerror(errno));
                    ret = -1;
                    goto exit;
                }

                if ((ftruncate(ea->ea_fd, 0)) == -1) {
                    LOG(log_error, logtype_afpd, "ea_close: ftruncate: %s", strerror(errno));
                    ret = -1;
                    goto exit;
                }

                if (write(ea->ea_fd, ea->ea_data, ea->ea_size) != ea->ea_size) {
                    LOG(log_error, logtype_afpd, "ea_close: write: %s", strerror(errno));
                    ret = -1;
                }
            }
        }
    }

exit:
    /* free names */
    while(count < ea->ea_count) {
        if ( (*ea->ea_entries)[count].ea_name ) {
            free((*ea->ea_entries)[count].ea_name);
            (*ea->ea_entries)[count].ea_name = NULL;
        }
        count++;
    }
    ea->ea_count = 0;

    if (ea->filename) {
        free(ea->filename);
        ea->filename = NULL;
    }

    if (ea->ea_entries) {
        free(ea->ea_entries);
        ea->ea_entries = NULL;
    }

    if (ea->ea_data) {
        free(ea->ea_data);
        ea->ea_data = NULL;
    }
    if (ea->ea_fd != -1) {
        close(ea->ea_fd);       /* also releases the fcntl lock */
        ea->ea_fd = -1;
    }

    return 0;
}



/************************************************************************************
 * VFS funcs called from afp_ea* funcs
 ************************************************************************************/

/*
 * Function: get_easize
 *
 * Purpose: get size of an EA
 *
 * Arguments:
 *
 *    vol          (r) current volume
 *    rbuf         (w) DSI reply buffer
 *    rbuflen      (rw) current length of data in reply buffer
 *    uname        (r) filename
 *    oflag        (r) link and create flag
 *    attruname    (r) name of attribute
 *
 * Returns: AFP code: AFP_OK on success or appropiate AFP error code
 *
 * Effects:
 *
 * Copies EA size into rbuf in network order. Increments *rbuflen +4.
 */
int get_easize(const struct vol * restrict vol,
               char * restrict rbuf,
               int * restrict rbuflen,
               const char * restrict uname,
               int oflag,
               const char * restrict attruname)
{
    int ret = AFPERR_MISC, count = 0;
    uint32_t uint32;
    struct ea ea;

    LOG(log_debug, logtype_afpd, "get_easize: file: %s", uname);

    if ((ea_open(vol, uname, EA_RDONLY, &ea)) != 0) {
        LOG(log_error, logtype_afpd, "get_easize: error calling ea_open for file: %s", uname);
        return AFPERR_MISC;
    }

    while (count < ea.ea_count) {
        if (strcmp(attruname, (*ea.ea_entries)[count].ea_name) == 0) {
            uint32 = htonl((*ea.ea_entries)[count].ea_size);
            memcpy(rbuf, &uint32, 4);
            *rbuflen += 4;
            ret = AFP_OK;

            LOG(log_debug, logtype_afpd, "get_easize(\"%s\"): size: %u",
                attruname, (*ea.ea_entries)[count].ea_size);
            break;
        }
        count++;
    }

    if ((ea_close(&ea)) != 0) {
        LOG(log_error, logtype_afpd, "get_easize: error closing ea handle for file: %s", uname);
        return AFPERR_MISC;
    }

    return ret;
}

/*
 * Function: get_eacontent
 *
 * Purpose: copy EA into rbuf
 *
 * Arguments:
 *
 *    vol          (r) current volume
 *    rbuf         (w) DSI reply buffer
 *    rbuflen      (rw) current length of data in reply buffer
 *    uname        (r) filename
 *    oflag        (r) link and create flag
 *    attruname    (r) name of attribute
 *    maxreply     (r) maximum EA size as of current specs/real-life
 *
 * Returns: AFP code: AFP_OK on success or appropiate AFP error code
 *
 * Effects:
 *
 * Copies EA into rbuf. Increments *rbuflen accordingly.
 */
int get_eacontent(const struct vol * restrict vol,
                  char * restrict rbuf,
                  int * restrict rbuflen,
                  const char * restrict uname,
                  int oflag,
                  const char * restrict attruname,
                  int maxreply)
{
    int ret = AFPERR_MISC, count = 0, fd = -1;
    uint32_t uint32;
    size_t toread;
    struct ea ea;

    LOG(log_debug, logtype_afpd, "get_eacontent('%s/%s')", uname, attruname);

    if ((ea_open(vol, uname, EA_RDONLY, &ea)) != 0) {
        LOG(log_error, logtype_afpd, "get_eacontent('%s'): ea_open error", uname);
        return AFPERR_MISC;
    }

    while (count < ea.ea_count) {
        if (strcmp(attruname, (*ea.ea_entries)[count].ea_name) == 0) {
            if ((fd = open(ea_path(&ea, attruname), O_RDONLY)) == -1) {
                ret = AFPERR_MISC;
                break;
            }

            /* Check how much the client wants, give him what we think is right */
            maxreply -= MAX_REPLY_EXTRA_BYTES;
            if (maxreply > MAX_EA_SIZE)
                maxreply = MAX_EA_SIZE;
            toread = (maxreply < (*ea.ea_entries)[count].ea_size) ? maxreply : (*ea.ea_entries)[count].ea_size;
            LOG(log_debug, logtype_afpd, "get_eacontent('%s'): sending %u bytes", attruname, toread);

            /* Put length of EA data in reply buffer */
            uint32 = htonl(toread);
            memcpy(rbuf, &uint32, 4);
            rbuf += 4;
            *rbuflen += 4;

            if ((read(fd, rbuf, toread)) != toread) {
                LOG(log_error, logtype_afpd, "get_eacontent('%s/%s'): short read", uname, attruname);
                ret = AFPERR_MISC;
                break;
            }
            *rbuflen += toread;
            close(fd);

            ret = AFP_OK;
            break;
        }
        count++;
    }

    if ((ea_close(&ea)) != 0) {
        LOG(log_error, logtype_afpd, "get_eacontent('%s'): error closing ea handle", uname);
        return AFPERR_MISC;
    }

    return ret;

}

/*
 * Function: list_eas
 *
 * Purpose: copy names of EAs into attrnamebuf
 *
 * Arguments:
 *
 *    vol          (r) current volume
 *    attrnamebuf  (w) store names a consecutive C strings here
 *    buflen       (rw) length of names in attrnamebuf
 *    uname        (r) filename
 *    oflag        (r) link and create flag
 *
 * Returns: AFP code: AFP_OK on success or appropiate AFP error code
 *
 * Effects:
 *
 * Copies names of all EAs of uname as consecutive C strings into rbuf.
 * Increments *buflen accordingly.
 */
int list_eas(const struct vol * restrict vol,
             char * restrict attrnamebuf,
             int * restrict buflen,
             const char * restrict uname,
             int oflag)
{
    int count = 0, attrbuflen = *buflen, ret = AFP_OK, len;
    char *buf = attrnamebuf;
    struct ea ea;

    LOG(log_debug, logtype_afpd, "list_eas: file: %s", uname);

    if ((ea_open(vol, uname, EA_RDONLY, &ea)) != 0) {
        if (errno != ENOENT) {
            LOG(log_error, logtype_afpd, "list_eas: error calling ea_open for file: %s", uname);
            return AFPERR_MISC;
        }
        else
            return AFP_OK;
    }

    while (count < ea.ea_count) {
        /* Convert name to CH_UTF8_MAC and directly store in in the reply buffer */
        if ( ( len = convert_string(vol->v_volcharset,
                                    CH_UTF8_MAC, 
                                    (*ea.ea_entries)[count].ea_name,
                                    (*ea.ea_entries)[count].ea_namelen,
                                    buf + attrbuflen,
                                    255))
             <= 0 ) {
            ret = AFPERR_MISC;
            goto exit;
        }
        if (len == 255)
            /* convert_string didn't 0-terminate */
            attrnamebuf[attrbuflen + 255] = 0;

        LOG(log_debug7, logtype_afpd, "list_eas(%s): EA: %s",
            uname, (*ea.ea_entries)[count].ea_name);

        attrbuflen += len + 1;
        if (attrbuflen > (ATTRNAMEBUFSIZ - 256)) {
            /* Next EA name could overflow, so bail out with error.
               FIXME: evantually malloc/memcpy/realloc whatever.
               Is it worth it ? */
            LOG(log_warning, logtype_afpd, "list_eas(%s): running out of buffer for EA names", uname);
            ret = AFPERR_MISC;
            goto exit;
        }
        count++;
    }

exit:
    *buflen = attrbuflen;

    if ((ea_close(&ea)) != 0) {
        LOG(log_error, logtype_afpd, "list_eas: error closing ea handle for file: %s", uname);
        return AFPERR_MISC;
    }

    return ret;
}

/*
 * Function: set_ea
 *
 * Purpose: set a Solaris native EA
 *
 * Arguments:
 *
 *    vol          (r) current volume
 *    uname        (r) filename
 *    attruname    (r) EA name
 *    ibuf         (r) buffer with EA content
 *    attrsize     (r) length EA in ibuf
 *    oflag        (r) link and create flag
 *
 * Returns: AFP code: AFP_OK on success or appropiate AFP error code
 *
 * Effects:
 *
 * Copies names of all EAs of uname as consecutive C strings into rbuf.
 * Increments *rbuflen accordingly.
 */
int set_ea(const struct vol * restrict vol,
           const char * restrict uname,
           const char * restrict attruname,
           const char * restrict ibuf,
           size_t attrsize,
           int oflag)
{
    int ret = AFP_OK;
    struct ea ea;

    LOG(log_debug, logtype_afpd, "set_ea: file: %s", uname);

    if ((ea_open(vol, uname, EA_CREATE | EA_RDWR, &ea)) != 0) {
        LOG(log_error, logtype_afpd, "set_ea('%s'): ea_open error", uname);
        return AFPERR_MISC;
    }

    if ((ea_addentry(&ea, uname, attruname, attrsize, oflag)) == -1) {
        LOG(log_error, logtype_afpd, "set_ea('%s'): ea_addentry error", uname);
        ret = AFPERR_MISC;
        goto exit;
    }

    if ((write_ea(&ea, attruname, ibuf, attrsize)) != 0) {
        LOG(log_error, logtype_afpd, "set_ea('%s'): write_ea error", uname);
        ret = AFPERR_MISC;
        goto exit;
    }

exit:
    if ((ea_close(&ea)) != 0) {
        LOG(log_error, logtype_afpd, "set_ea('%s'): ea_close error", uname);
        ret = AFPERR_MISC;
        goto exit;
    }

    return ret;
}

/*
 * Function: remove_ea
 *
 * Purpose: remove a EA from a file
 *
 * Arguments:
 *
 *    vol          (r) current volume
 *    uname        (r) filename
 *    attruname    (r) EA name
 *    oflag        (r) link and create flag
 *
 * Returns: AFP code: AFP_OK on success or appropiate AFP error code
 *
 * Effects:
 *
 * Removes EA attruname from file uname.
 */
int remove_ea(const struct vol * restrict vol,
              const char * restrict uname,
              const char * restrict attruname,
              int oflag)
{
    int ret = AFP_OK;
    struct ea ea;

    LOG(log_debug, logtype_afpd, "remove_ea('%s/%s')", uname, attruname);

    if ((ea_open(vol, uname, EA_RDWR, &ea)) != 0) {
        LOG(log_error, logtype_afpd, "remove_ea('%s'): ea_open error", uname);
        return AFPERR_MISC;
    }

    if ((ea_delentry(&ea, uname, attruname)) == -1) {
        LOG(log_error, logtype_afpd, "remove_ea('%s'): ea_delentry error", uname);
        ret = AFPERR_MISC;
        goto exit;
    }

    if ((delete_ea_file(&ea, attruname)) != 0) {
        LOG(log_error, logtype_afpd, "remove_ea('%s'): delete_ea error", uname);
        ret = AFPERR_MISC;
        goto exit;
    }

exit:
    if ((ea_close(&ea)) != 0) {
        LOG(log_error, logtype_afpd, "remove_ea('%s'): ea_close error", uname);
        ret = AFPERR_MISC;
        goto exit;
    }

    return ret;
}

/**********************************************************************************
 * Solaris EA VFS funcs
 **********************************************************************************/

/*
 * Function: sol_get_easize
 *
 * Purpose: get size of an EA on Solaris native EA
 *
 * Arguments:
 *
 *    vol          (r) current volume
 *    rbuf         (w) DSI reply buffer
 *    rbuflen      (rw) current length of data in reply buffer
 *    uname        (r) filename
 *    oflag        (r) link and create flag
 *    attruname    (r) name of attribute
 *
 * Returns: AFP code: AFP_OK on success or appropiate AFP error code
 *
 * Effects:
 *
 * Copies EA size into rbuf in network order. Increments *rbuflen +4.
 */
#ifdef HAVE_SOLARIS_EAS
int sol_get_easize(const struct vol * restrict vol,
                   char * restrict rbuf,
                   int * restrict rbuflen,
                   const char * restrict uname,
                   int oflag,
                   cons char * restrict attruname)
{
    int                 ret, attrdirfd;
    uint32_t            attrsize;
    struct stat         st;

    LOG(log_debug7, logtype_afpd, "sol_getextattr_size(%s): attribute: \"%s\"", uname, attruname);

    if ( -1 == (attrdirfd = attropen(uname, ".", O_RDONLY | oflag))) {
        if (errno == ELOOP) {
            /* its a symlink and client requested O_NOFOLLOW  */
            LOG(log_debug, logtype_afpd, "sol_getextattr_size(%s): encountered symlink with kXAttrNoFollow", uname);

            memset(rbuf, 0, 4);
            *rbuflen += 4;

            return AFP_OK;
        }
        LOG(log_error, logtype_afpd, "sol_getextattr_size: attropen error: %s", strerror(errno));
        return AFPERR_MISC;
    }

    if ( -1 == (fstatat(attrdirfd, attruname, &st, 0))) {
        LOG(log_error, logtype_afpd, "sol_getextattr_size: fstatat error: %s", strerror(errno));
        ret = AFPERR_MISC;
        goto exit;
    }
    attrsize = (st.st_size > MAX_EA_SIZE) ? MAX_EA_SIZE : st.st_size;

    /* Start building reply packet */

    LOG(log_debug7, logtype_afpd, "sol_getextattr_size(%s): attribute: \"%s\", size: %u", uname, attruname, attrsize);

    /* length of attribute data */
    attrsize = htonl(attrsize);
    memcpy(rbuf, &attrsize, 4);
    *rbuflen += 4;

    ret = AFP_OK;

exit:
    close(attrdirfd);
    return ret;
}
#endif /* HAVE_SOLARIS_EAS */

/*
 * Function: sol_get_eacontent
 *
 * Purpose: copy Solaris native EA into rbuf
 *
 * Arguments:
 *
 *    vol          (r) current volume
 *    rbuf         (w) DSI reply buffer
 *    rbuflen      (rw) current length of data in reply buffer
 *    uname        (r) filename
 *    oflag        (r) link and create flag
 *    attruname    (r) name of attribute
 *    maxreply     (r) maximum EA size as of current specs/real-life
 *
 * Returns: AFP code: AFP_OK on success or appropiate AFP error code
 *
 * Effects:
 *
 * Copies EA into rbuf. Increments *rbuflen accordingly.
 */
#ifdef HAVE_SOLARIS_EAS
int sol_get_eacontent(const struct vol * restrict vol,
                      char * restrict rbuf,
                      int * restrict rbuflen,
                      const char * restrict uname,
                      int oflag,
                      char * restrict attruname,
                      int maxreply)
{
    int                 ret, attrdirfd;
    size_t              toread, okread = 0, len;
    char                *datalength;
    struct stat         st;

    if ( -1 == (attrdirfd = attropen(uname, attruname, O_RDONLY | oflag))) {
        if (errno == ELOOP) {
            /* its a symlink and client requested O_NOFOLLOW  */
            LOG(log_debug, logtype_afpd, "sol_getextattr_content(%s): encountered symlink with kXAttrNoFollow", uname);

            memset(rbuf, 0, 4);
            *rbuflen += 4;

            return AFP_OK;
        }
        LOG(log_error, logtype_afpd, "sol_getextattr_content(%s): attropen error: %s", attruname, strerror(errno));
        return AFPERR_MISC;
    }

    if ( -1 == (fstat(attrdirfd, &st))) {
        LOG(log_error, logtype_afpd, "sol_getextattr_content(%s): fstatat error: %s", attruname,strerror(errno));
        ret = AFPERR_MISC;
        goto exit;
    }

    /* Start building reply packet */

    maxreply -= MAX_REPLY_EXTRA_BYTES;
    if (maxreply > MAX_EA_SIZE)
        maxreply = MAX_EA_SIZE;

    /* But never send more than the client requested */
    toread = (maxreply < st.st_size) ? maxreply : st.st_size;

    LOG(log_debug7, logtype_afpd, "sol_getextattr_content(%s): attribute: \"%s\", size: %u", uname, attruname, maxreply);

    /* remember where we must store length of attribute data in rbuf */
    datalength = rbuf;
    rbuf += 4;
    *rbuflen += 4;

    while (1) {
        len = read(attrdirfd, rbuf, toread);
        if (len == -1) {
            LOG(log_error, logtype_afpd, "sol_getextattr_content(%s): read error: %s", attruname, strerror(errno));
            ret = AFPERR_MISC;
            goto exit;
        }
        okread += len;
        rbuf += len;
        *rbuflen += len;
        if ((len == 0) || (okread == toread))
            break;
    }

    okread = htonl((uint32_t)okread);
    memcpy(datalength, &okread, 4);

    ret = AFP_OK;

exit:
    close(attrdirfd);
    return ret;
}
#endif /* HAVE_SOLARIS_EAS */

/*
 * Function: sol_list_eas
 *
 * Purpose: copy names of Solaris native EA into attrnamebuf
 *
 * Arguments:
 *
 *    vol          (r) current volume
 *    attrnamebuf  (w) store names a consecutive C strings here
 *    buflen       (rw) length of names in attrnamebuf
 *    uname        (r) filename
 *    oflag        (r) link and create flag
 *
 * Returns: AFP code: AFP_OK on success or appropiate AFP error code
 *
 * Effects:
 *
 * Copies names of all EAs of uname as consecutive C strings into rbuf.
 * Increments *rbuflen accordingly.
 */
#ifdef HAVE_SOLARIS_EAS
int sol_list_eas(const struct vol * restrict vol,
                 char * restrict attrnamebuf,
                 int * restrict buflen,
                 const char * restrict uname,
                 int oflag)
{
    int ret, attrbuflen = *buflen, len, attrdirfd = 0;
    struct dirent *dp;
    DIR *dirp = NULL;

    /* Now list file attribute dir */
    if ( -1 == (attrdirfd = attropen( uname, ".", O_RDONLY | oflag))) {
        if (errno == ELOOP) {
            /* its a symlink and client requested O_NOFOLLOW */
            ret = AFPERR_BADTYPE;
            goto exit;
        }
        LOG(log_error, logtype_afpd, "sol_list_extattr(%s): error opening atttribute dir: %s", uname, strerror(errno));
        ret = AFPERR_MISC;
        goto exit;
    }

    if (NULL == (dirp = fdopendir(attrdirfd))) {
        LOG(log_error, logtype_afpd, "sol_list_extattr(%s): error opening atttribute dir: %s", uname, strerror(errno));
        ret = AFPERR_MISC;
        goto exit;
    }

    while ((dp = readdir(dirp)))  {
        /* check if its "." or ".." */
        if ((strcmp(dp->d_name, ".") == 0) || (strcmp(dp->d_name, "..") == 0) ||
            (strcmp(dp->d_name, "SUNWattr_ro") == 0) || (strcmp(dp->d_name, "SUNWattr_rw") == 0))
            continue;

        len = strlen(dp->d_name);

        /* Convert name to CH_UTF8_MAC and directly store in in the reply buffer */
        if ( 0 >= ( len = convert_string(vol->v_volcharset, CH_UTF8_MAC, dp->d_name, len, attrnamebuf + attrbuflen, 255)) ) {
            ret = AFPERR_MISC;
            goto exit;
        }
        if (len == 255)
            /* convert_string didn't 0-terminate */
            attrnamebuf[attrbuflen + 255] = 0;

        LOG(log_debug7, logtype_afpd, "sol_list_extattr(%s): attribute: %s", uname, dp->d_name);

        attrbuflen += len + 1;
        if (attrbuflen > (ATTRNAMEBUFSIZ - 256)) {
            /* Next EA name could overflow, so bail out with error.
               FIXME: evantually malloc/memcpy/realloc whatever.
               Is it worth it ? */
            LOG(log_warning, logtype_afpd, "sol_list_extattr(%s): running out of buffer for EA names", uname);
            ret = AFPERR_MISC;
            goto exit;
        }
    }

    ret = AFP_OK;

exit:
    if (dirp)
        closedir(dirp);

    if (attrdirfd > 0)
        close(attrdirfd);

    *buflen = attrbuflen;
    return ret;
}
#endif /* HAVE_SOLARIS_EAS */

/*
 * Function: sol_set_ea
 *
 * Purpose: set a Solaris native EA
 *
 * Arguments:
 *
 *    vol          (r) current volume
 *    uname        (r) filename
 *    attruname    (r) EA name
 *    ibuf         (r) buffer with EA content
 *    attrsize     (r) length EA in ibuf
 *    oflag        (r) link and create flag
 *
 * Returns: AFP code: AFP_OK on success or appropiate AFP error code
 *
 * Effects:
 *
 * Copies names of all EAs of uname as consecutive C strings into rbuf.
 * Increments *rbuflen accordingly.
 */
#ifdef HAVE_SOLARIS_EAS
int sol_set_ea(const struct vol * restrict vol,
               const char * restrict u_name,
               const char * restrict attruname,
               const char * restrict ibuf,
               size_t attrsize,
               int oflag)
{
    int attrdirfd;

    if ( -1 == (attrdirfd = attropen(u_name, attruname, oflag, 0666))) {
        if (errno == ELOOP) {
            /* its a symlink and client requested O_NOFOLLOW  */
            LOG(log_debug, logtype_afpd, "afp_setextattr(%s): encountered symlink with kXAttrNoFollow", s_path->u_name);
            return AFP_OK;
        }
        LOG(log_error, logtype_afpd, "afp_setextattr(%s): attropen error: %s", s_path->u_name, strerror(errno));
        return AFPERR_MISC;
    }

    if ((write(attrdirfd, ibuf, attrsize)) != attrsize) {
        LOG(log_error, logtype_afpd, "afp_setextattr(%s): read error: %s", attruname, strerror(errno));
        return AFPERR_MISC;
    }

    return AFP_OK;
}
#endif /* HAVE_SOLARIS_EAS */

/*
 * Function: sol_remove_ea
 *
 * Purpose: remove a Solaris native EA
 *
 * Arguments:
 *
 *    vol          (r) current volume
 *    uname        (r) filename
 *    attruname    (r) EA name
 *    oflag        (r) link and create flag
 *
 * Returns: AFP code: AFP_OK on success or appropiate AFP error code
 *
 * Effects:
 *
 * Removes EA attruname from file uname.
 */
#ifdef HAVE_SOLARIS_EAS
int sol_remove_ea(const struct vol * restrict vol,
                  const char * restrict uname,
                  const char * restrict attruname,
                  int oflag)
{
    int attrdirfd;

    if ( -1 == (attrdirfd = attropen(uname, ".", oflag))) {
        switch (errno) {
        case ELOOP:
            /* its a symlink and client requested O_NOFOLLOW  */
            LOG(log_debug, logtype_afpd, "afp_remextattr(%s): encountered symlink with kXAttrNoFollow", s_path->u_name);
            return AFP_OK;
        case EACCES:
            LOG(log_debug, logtype_afpd, "afp_remextattr(%s): unlinkat error: %s", s_path->u_name, strerror(errno));
            return AFPERR_ACCESS;
        default:
            LOG(log_error, logtype_afpd, "afp_remextattr(%s): attropen error: %s", s_path->u_name, strerror(errno));
            return AFPERR_MISC;
        }
    }

    if ( -1 == (unlinkat(attrdirfd, attruname, 0)) ) {
        if (errno == EACCES) {
            LOG(log_debug, logtype_afpd, "afp_remextattr(%s): unlinkat error: %s", s_path->u_name, strerror(errno));
            return AFPERR_ACCESS;
        }
        LOG(log_error, logtype_afpd, "afp_remextattr(%s): unlinkat error: %s", s_path->u_name, strerror(errno));
        return AFPERR_MISC;
    }

}
#endif /* HAVE_SOLARIS_EAS */