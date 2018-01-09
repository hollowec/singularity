#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <archive.h>
#include <archive_entry.h>

#include "config.h"
#include "util/file.h"
#include "util/message.h"
#include "util/registry.h"
#include "util/util.h"

/* apply_opaque
 *  Given opq_marker as a path to a whiteout opaque marker
 *    e.g. usr/share/doc/test/.wh..wh..opq
 *  Make the directory containing the make opaque for this layer by removing it
 *  if it exists under rootfs_dir
 */
int apply_opaque(const char *opq_marker, char *rootfs_dir) {
    int retval = 0;
    char *token, *opq_dir, *opq_dir_rootfs;
    size_t buff_len;

    token = strrchr(opq_marker, '/');

    if (token == NULL) {
        singularity_message(ERROR, "Error getting dirname for opaque marker\n");
        ABORT(255);
    }

    buff_len = token - opq_marker + 1;
    opq_dir = malloc(buff_len);
    snprintf(opq_dir, buff_len, "%s", opq_marker);

    buff_len = strlen(rootfs_dir) + 1 + strlen(opq_dir) + 1;
    opq_dir_rootfs = malloc(buff_len);
    snprintf(opq_dir_rootfs, buff_len, "%s/%s", rootfs_dir, opq_dir);

    if (is_dir(opq_dir_rootfs) == 0) {
        s_rmdir(opq_dir_rootfs);
    }

    free(opq_dir);
    free(opq_dir_rootfs);

    return retval;
}

/* apply_whiteout
 *  Given wh_marker as a path to a whiteout marker
 *    e.g. usr/share/doc/test/.wh.deletedfile
 *  Whiteout the referenced file for this layer by removing it if it exists
 *  under rootfs_dir
 */
int apply_whiteout(const char *wh_marker, char *rootfs_dir) {
    int retval = 0;
    char *token, *wh_path, *prefix_str, *suffix_str, *wh_path_rootfs;
    size_t token_pos, buff_len;

    token = strstr(wh_marker, ".wh.");

    if (token == NULL) {
        singularity_message(ERROR, "Error getting filename for whiteout marker\n");
        ABORT(255);
    }

    // Location of the ".wh." substring
    token_pos = strlen(wh_marker) - strlen(token);
    // Dest for path stripped of .wh.
    buff_len = strlen(wh_marker) - strlen(".wh.") + 1;
    wh_path = malloc(buff_len);
    // Prefix before .wh. 
    prefix_str = malloc(token_pos + 1);
    snprintf(prefix_str, token_pos + 1, "%s", wh_marker);
    // Suffix after .wh.
    suffix_str = malloc(buff_len - token_pos);
    snprintf(suffix_str, buff_len - token_pos, "%s", token + 4);

    snprintf(wh_path, buff_len, "%s%s", prefix_str, suffix_str);

    buff_len = strlen(rootfs_dir) + 1 + strlen(wh_path) + 1;
    wh_path_rootfs = malloc(buff_len);
    snprintf(wh_path_rootfs, buff_len, "%s/%s", rootfs_dir, wh_path);

    if (is_dir(wh_path_rootfs) == 0) {
        retval = s_rmdir(wh_path_rootfs);
    } else if (is_file(wh_path_rootfs) == 0) {
        singularity_message(DEBUG, "Removing whiteout-ed file: %s\n",
                            wh_path_rootfs);
        retval = unlink(wh_path_rootfs);
    }

    free(prefix_str);
    free(suffix_str);
    free(wh_path);
    free(wh_path_rootfs);

    return retval;
}

/* apply_whiteouts
 *  Process tarfile and apply any aufs opaque/whiteouts on rootfs_dir
 */
int apply_whiteouts(char *tarfile, char *rootfs_dir) {
    int retval = 0;
    int errcode = 0;

    struct archive *a;
    struct archive_entry *entry;

    a = archive_read_new();
#if ARCHIVE_VERSION_NUMBER <= 3000000
    archive_read_support_compression_all(a);
#else
    archive_read_support_filter_all(a);
#endif
    archive_read_support_format_all(a);
    retval = archive_read_open_filename(a, tarfile, 10240);
    if (retval != ARCHIVE_OK) {
        return (1);
    }

    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {

        const char *pathname = archive_entry_pathname(entry);

        if (strstr(pathname, "/.wh..wh..opq")) {
            singularity_message(DEBUG, "Opaque Marker %s\n", pathname);
            errcode = apply_opaque(pathname, rootfs_dir);
            if (errcode != 0) {
                break;
            }
        } else if (strstr(pathname, "/.wh.")) {
            singularity_message(DEBUG, "Whiteout Marker %s\n", pathname);
            errcode = apply_whiteout(pathname, rootfs_dir);
            if (errcode != 0) {
                break;
            }
        }
    }
#if ARCHIVE_VERSION_NUMBER <= 3000000
    retval = archive_read_finish(a);
#else
    retval = archive_read_free(a);
#endif
    if (retval != ARCHIVE_OK){
        singularity_message(ERROR, "Error freeing archive\n");
        ABORT(255);
    }

    return errcode;
}

/* See  https://github.com/libarchive/libarchive/wiki/Examples#A_Complete_Extractor */
static int copy_data(struct archive *ar, struct archive *aw) {
    int r;
    const void *buff;
    size_t size;
    int64_t offset;

    for (;;) {
        r = archive_read_data_block(ar, &buff, &size, &offset);
        if (r == ARCHIVE_EOF) {
            return (ARCHIVE_OK);
        }
        if (r < ARCHIVE_OK) {
            return (r);
        }
        r = archive_write_data_block(aw, buff, size, offset);
        if (r < ARCHIVE_OK) {
            singularity_message(ERROR, "tar extraction error: %s\n", archive_error_string(aw));
            return (r);
        }
    }
}

/* extract_tar
 *  Extract a tar file to rootfs_dir using libarchive. Handles compression.
 *  Exclude any .wh. whiteout files, and device/pipe/fifo entries
 *
 * See https://github.com/libarchive/libarchive/wiki/Examples#A_Complete_Extractor
 */
int extract_tar(const char *tarfile, const char *rootfs_dir) {
    int retval = 0;
    struct archive *a;
    struct archive *ext;
    struct archive_entry *entry;
    int flags;
    int r;
    char *orig_dir;
    const char *pathname;
    int pathtype;

    orig_dir = get_current_dir_name();

    /* Select which attributes we want to restore. */
    flags = ARCHIVE_EXTRACT_TIME;
    flags |= ARCHIVE_EXTRACT_PERM;
    flags |= ARCHIVE_EXTRACT_ACL;
    flags |= ARCHIVE_EXTRACT_FFLAGS;

    a = archive_read_new();
    archive_read_support_format_all(a);
#if ARCHIVE_VERSION_NUMBER <= 3000000
    archive_read_support_compression_all(a);
#else
    archive_read_support_filter_all(a);
#endif
    ext = archive_write_disk_new();
    archive_write_disk_set_options(ext, flags);
    archive_write_disk_set_standard_lookup(ext);
    if ((r = archive_read_open_filename(a, tarfile, 10240))){
        singularity_message(ERROR, "Error opening tar file %s\n", tarfile);
        ABORT(255);
    }

    // Extract into the SINGULARITY_ROOTFS
    r = chdir(rootfs_dir);
    if (r < 0 ){
        singularity_message(ERROR, "Could not chdir to SINGULARITY_ROOTFS %s\n", rootfs_dir);
        ABORT(255);
    }

    for (;;) {
        r = archive_read_next_header(a, &entry);

        if (r == ARCHIVE_EOF) {
            break;
        }

        if (r < ARCHIVE_OK) {
            singularity_message(WARNING, "Warning reading tar header: %s\n", archive_error_string(a));
        }
        if (r < ARCHIVE_WARN) {
            singularity_message(ERROR, "Error reading tar header: %s\n", archive_error_string(a));
            ABORT(255);
        }

        pathname = archive_entry_pathname(entry);
        pathtype = archive_entry_filetype(entry);

        // Do not extract whiteout markers (handled in apply_whiteouts)
        // Do not extract sockers, chr/blk devices, pipes
        if (strstr(pathname, "/.wh.") || pathtype == AE_IFSOCK ||
            pathtype == AE_IFCHR || pathtype == AE_IFBLK || pathtype == AE_IFIFO) {
            continue;
        }

        r = archive_write_header(ext, entry);
        if (r < ARCHIVE_OK) {
            singularity_message(WARNING, "Warning handling tar header: %s\n", archive_error_string(ext));
        }else if (archive_entry_size(entry) > 0) {
            r = copy_data(a, ext);
            if (r < ARCHIVE_OK) {
                singularity_message(WARNING, "Warning handling tar header: %s\n", archive_error_string(ext));
            }
            if (r < ARCHIVE_WARN) {
                singularity_message(ERROR, "Error handling tar header: %s\n", archive_error_string(ext));
                ABORT(255);
            }
        }
        r = archive_write_finish_entry(ext);
        if (r < ARCHIVE_OK) {
            singularity_message(WARNING, "Warning freeing archive entry: %s\n", archive_error_string(ext));
        }
        if (r < ARCHIVE_WARN) {
            singularity_message(ERROR, "Error freeing archive entry: %s\n", archive_error_string(ext));
            ABORT(255);
        }
    }
    archive_read_close(a);
#if ARCHIVE_VERSION_NUMBER <= 3000000
    archive_read_finish(a);
    archive_write_close(ext);
    archive_write_finish(ext);
#else
    archive_read_free(a);
    archive_write_close(ext);
    archive_write_free(ext);
#endif
    r = chdir(orig_dir);
    if (r < 0 ){
        singularity_message(ERROR, "Could not chdir back to %s\n", orig_dir);
        ABORT(255);
    }

    free(orig_dir);

    return (retval);
}

int main(int argc, char **argv) {
    int retval = 0;
    char *rootfs_dir = singularity_registry_get("ROOTFS");
    char *tarfile = NULL;

    if (argc != 2) {
        singularity_message(ERROR, "Provide a single docker tar file to extract\n");
        ABORT(255);
    }

    if (rootfs_dir == NULL) {
        singularity_message(ERROR, "Environment is not properly setup\n");
        ABORT(255);
    }

    if (is_dir(rootfs_dir) < 0) {
        singularity_message(ERROR, "SINGULARITY_ROOTFS does not exist\n");
        ABORT(255);
    }

    tarfile = argv[1];

    if (is_file(tarfile) < 0) {
        singularity_message(ERROR, "tar file does not exist: %s\n", tarfile);
        ABORT(255);
    }

    singularity_message(DEBUG, "Applying whiteouts for tar file %s\n", tarfile);
    retval = apply_whiteouts(tarfile, rootfs_dir);

    if (retval != 0) {
        singularity_message(ERROR, "Error applying layer whiteouts\n");
        ABORT(255);
    }

    singularity_message(DEBUG, "Extracting docker tar file %s\n", tarfile);
    retval = extract_tar(tarfile, rootfs_dir);

    if (retval != 0) {
        singularity_message(ERROR, "Error extracting tar file\n");
        ABORT(255);
    }

    return (retval);
}
