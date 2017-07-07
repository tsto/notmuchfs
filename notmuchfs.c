/*============================================================================*/
/*
 * notmuchfs - A virtual maildir file system for notmuch queries
 *
 * Copyright © 2012-2016 Tim Stoakes
 *
 * This file is part of notmuchfs.
 *
 * Notmuchfs is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with notmuchfs.  If not, see http://www.gnu.org/licenses/ .
 *
 * Authors: Tim Stoakes <tim@stoakes.net>
 */
/*============================================================================*/

/**
 * @section message_names Message Names
 *
 * Messages in a notmuchfs virtual maildir are named by taking the full path
 * to the real message, and replacing all / with #. This whole string is now
 * the message name.
 *
 *
 * @section x_label X-Label Header
 *
 * Each message read from a virtual maildir has an X-Label header inserted
 * on-the-fly, containing the concatenation of the notmuch tags of this
 * message (comma separated), up to #MAX_XLABEL_LENGTH characters long.
 */

/*============================================================================*/

#define _GNU_SOURCE

#include <sys/types.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <dirent.h>
#include <assert.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/param.h>
#include <string.h>

#define FUSE_USE_VERSION 26
#include <fuse.h>

#include "notmuch.h"

/*============================================================================*/

#define NOTMUCHFS_VERSION "0.2"

/*============================================================================*/

/**
 * Global configuration information, from CLI.
 *
 * @{
 */
struct notmuchfs_config {
  /**
   * The backing directory path.
   */
  char *backing_dir;

  /**
   * The notmuch database directory path. This is actually the directory that
   * contains the .notmuch/ database directory, since that's what notmuch
   * requires.
   */
  char *mail_dir;

  /**
   * Mutt is not compliant with the maildir spec, see:
   * - http://dev.mutt.org/trac/ticket/2476
   * - http://notmuchmail.org/pipermail/notmuch/2011/004833.html
   * Notmuchfs can workaround this issue if this field is set.
   */
  bool  mutt_2476_workaround_allowed;
};

static struct notmuchfs_config global_config;

/** @} */

/*============================================================================*/

/**
 * Whether to enable debug tracing.
 */
#define NOTMUCHFS_DEBUG 0

/**
 * The maximum length of the X-Label header that notmuchfs will synthesize.
 *
 * @todo this limit is arbitrary.
 */
#define MAX_XLABEL_LENGTH 1024

/**
 * The text of the X-Label header.
 */
#define XLABEL "X-Label: "

/*============================================================================*/

/** Lock a pthread mutex with error checking. */
#define PTHREAD_LOCK(LOCK) \
  do { \
    int _ret = pthread_mutex_lock(LOCK); \
    assert(_ret == 0); \
  } while (0);

/** Unlock a pthread mutex with error checking. */
#define PTHREAD_UNLOCK(LOCK) \
  do { \
    int _ret = pthread_mutex_unlock(LOCK); \
    assert(_ret == 0); \
  } while (0);

/*============================================================================*/

/**
 * Logging.
 */
#if NOTMUCHFS_DEBUG
#define LOG_TRACE(...) \
  printf(__VA_ARGS__)
#else
#define LOG_TRACE(...)
#endif

/*============================================================================*/

/**
 * The context required to deal with the notmuch database.
 */
typedef struct
{
 /** Mutex to protect the database object from concurrent access. */
 pthread_mutex_t     mutex;

 /** The notmuch database handle. May be NULL if the database is not opened. */
 notmuch_database_t *db;

 /** Newline-delimited string list of tags to exclude from results. */
 char               *excluded_tags;
} notmuch_context_t;

/*============================================================================*/

/**
 * Replace every instance of character 'old' with 'new' in 'str_in'.
 *
 * @param[in,out] str_in The string to replace within.
 * @param[in]     old    The character to replace.
 * @param[in]     new    The character to replace with.
 */
static void string_replace (char *str_in, char old, char new)
{
 char *str = str_in;

 while (*str != '\0') {
   if (*str == old)
     *str = new;
   str++;
 }
}

/*============================================================================*/

/**
 * Open the notmuch database inside this context. Continue trying forever
 * if the open fails (e.g. the database was locked).
 *
 * @param[in,out] p_ctx      The notmuch context.
 * @param[in]     need_write Whether to open the database in read-only or
 *                           read-write mode.
 * @pre Database must not already be open.
 */
static void database_open (notmuch_context_t *p_ctx, bool need_write)
{
 LOG_TRACE("notmuch database_open\n");
 PTHREAD_LOCK(&p_ctx->mutex);
 assert(p_ctx->db == NULL);

 while (TRUE) {
   notmuch_status_t status =
     notmuch_database_open(global_config.mail_dir,
                           need_write ?
                             NOTMUCH_DATABASE_MODE_READ_WRITE:
                             NOTMUCH_DATABASE_MODE_READ_ONLY,
                           &p_ctx->db);

   if (status == NOTMUCH_STATUS_SUCCESS) {
     break;
   }
   else if (status == NOTMUCH_STATUS_XAPIAN_EXCEPTION) {
     /* Try again. */
     sleep(1);
   }
   else {
     fprintf(stderr, "ERROR: Database open error.\n");
     exit(1);
   }
 }

 if (notmuch_database_needs_upgrade(p_ctx->db)) {
   fprintf(stderr, "ERROR: Database needs upgrade.\n");
   exit(1);
 }
}

/*============================================================================*/

/**
 * Close the notmuch database inside this context.
 *
 * @param[in,out] p_ctx The notmuch context.
 * @pre Database must be open.
 */
static void database_close (notmuch_context_t *p_ctx)
{
 LOG_TRACE("notmuch database_close\n");
 assert(p_ctx->db != NULL);
 notmuch_database_close(p_ctx->db);
 notmuch_database_destroy(p_ctx->db);
 p_ctx->db = NULL;
 PTHREAD_UNLOCK(&p_ctx->mutex);
}

/*============================================================================*/

/* FUSE operations. */

/** The maximum length of the tag exclusion string. Arbitrarily chosen. */
#define EXCLUDED_TAGS_MAX_LENGTH 128

static void *notmuchfs_init (struct fuse_conn_info *conn)
{
 int res = chdir(global_config.backing_dir);
 if (res == -1)
   return NULL;

 notmuch_context_t *p_ctx = malloc(sizeof(notmuch_context_t));
 memset(p_ctx, 0, sizeof(notmuch_context_t));
 res = pthread_mutex_init(&p_ctx->mutex, NULL);
 if (res != 0) {
   free(p_ctx);
   return NULL;
 }

 /* Fetch the list of excluded tags from notmuch config.
  * If only there was an API for this...
  */
 p_ctx->excluded_tags = malloc(EXCLUDED_TAGS_MAX_LENGTH);
 p_ctx->excluded_tags[0] = '\0';
 FILE *fp = popen("notmuch config get search.exclude_tags", "r");
 if (fp != NULL) {
   size_t bytes_read = fread(p_ctx->excluded_tags, 1, EXCLUDED_TAGS_MAX_LENGTH,
                             fp);
   if (bytes_read > 0) {
     p_ctx->excluded_tags[bytes_read - 1] = '\0';
   }
   (void)pclose(fp);
 }

 return p_ctx;
}

/*============================================================================*/

static void notmuchfs_destroy (void *p_ctx_in)
{
 notmuch_context_t *p_ctx = (notmuch_context_t *)p_ctx_in;

 free(p_ctx->excluded_tags);
 int res = pthread_mutex_destroy(&p_ctx->mutex);
 /* Any failure here is a problem that we caused. */
 assert(res == 0);

 free(p_ctx);
}

/*============================================================================*/

static int notmuchfs_getattr (const char *path, struct stat *stbuf)
{
 int res = 0;

 memset(stbuf, 0, sizeof(struct stat));

 if (strcmp(path, "/") == 0) {
   /* Querying the base directory, pass to backing store. */
   if (stat(".", stbuf) != 0)
     res = -errno;
   return res;
 }

 char *last_slash  = strrchr(path + 1, '/');
 if (last_slash == NULL) {
   /* Querying '/<query>', pass to backing store. */
   LOG_TRACE("getattr stat1: %s\n", path + 1);
   if (lstat(path + 1, stbuf) != 0)
     res = -errno;
 }
 else if (strcmp(last_slash + 1, "new") == 0 ||
          strcmp(last_slash + 1, "tmp") == 0 ||
          strcmp(last_slash + 1, "cur") == 0) {
   /* Querying a maildir directory, so copy the parent directory. */
   char trans_name[PATH_MAX];
   strncpy(trans_name, path + 1, last_slash - path);
   trans_name[last_slash - path] = '\0';
   LOG_TRACE("getattr stat2: %s\n", trans_name);
   if (stat(trans_name, stbuf) != 0)
     res = -errno;
 }
 else {
   /* '/<query>/cur/translated#msg#name' */
   char *first_slash = strchr(path + 1, '/');
   bool  mutt_2476_workaround = FALSE;

   if (global_config.mutt_2476_workaround_allowed) {
     /* The workaround here is to intercept all getattr()s of a path like:
      *   /real/path/new/fake#maildir#cur#foofile
      * and treat it as if 'new' was 'cur' thus:
      *   /real/path/cur/fake#maildir#cur#foofile
      */

     if ((last_slash - path) >= 3 &&
         memcmp(last_slash - 3, "new", 3) == 0) {
       LOG_TRACE("Activating mutt_bug_2476 workaround for getattr(%s)\n", path);
       mutt_2476_workaround = TRUE;
     }
   }

   if (mutt_2476_workaround ||
       (last_slash - first_slash > 3 &&
        strncmp(first_slash, "/cur/", 5) == 0)) {
     char trans_name[PATH_MAX];
     strncpy(trans_name, last_slash + 1, PATH_MAX - 1);
     trans_name[PATH_MAX - 1] = '\0';
     string_replace(trans_name, '#', '/');

     LOG_TRACE("getattr stat3: %s\n", trans_name);
     if (stat(trans_name, stbuf) != 0)
       res = -errno;

     /* Inflate the size of the file by the maximum length of a synthetic
      * X-Label header.
      */
     stbuf->st_size += MAX_XLABEL_LENGTH;
   }
   else {
     res = -ENOENT;
   }
 }

 return res;
}

/*============================================================================*/

/**
 * Which type of directory read is being done?
 */
typedef enum
{
 /** A directory containing no files. */
 OPENDIR_TYPE_EMPTY_DIR,
 /** A maildir root (e.g. containing 'cur/'). */
 OPENDIR_TYPE_MAIL_DIR,
 /** A real directory in the backing store. */
 OPENDIR_TYPE_BACKING_DIR,
 /** A maildir with message files taken from a notmuch query. */
 OPENDIR_TYPE_NOTMUCH_QUERY
} opendir_type_t;


/**
 * Context for opendir(), readdir(), releasedir().
 */
typedef struct
{
 opendir_type_t      type;

 /**
  * These are for type == OPENDIR_TYPE_NOTMUCH_QUERY.
  * @{
  */
 notmuch_query_t    *p_query;
 notmuch_messages_t *p_messages;
 /** @} */

 /** This is for type == OPENDIR_TYPE_BACKING_DIR. */
 DIR                *fd;

 off_t               next_offset;
} opendir_t;

/*============================================================================*/

static int notmuchfs_opendir (const char* path, struct fuse_file_info* fi)
{
 int        res    = 0;
 opendir_t *dir_fd = (opendir_t *) malloc(sizeof(opendir_t));

 if (strcmp(path, "/") == 0) {
   /* Listing '/', so show the backing directory. */
   dir_fd->type = OPENDIR_TYPE_BACKING_DIR;
   char trans_name[PATH_MAX];
   strncpy(trans_name, global_config.backing_dir, PATH_MAX - 1);
   strncpy(trans_name + strlen(global_config.backing_dir), path + 1,
           PATH_MAX - 1 - strlen(global_config.backing_dir));
   trans_name[PATH_MAX - 1] = '\0';

   LOG_TRACE("opendir list backing dir: %s\n", trans_name);
   dir_fd->fd = opendir(trans_name);
 }
 else {
   char *last_slash = strrchr(path + 1, '/');
   if (last_slash == NULL) {
     /* Listing '/<query>', so return the 3 maildir dirs. */
     LOG_TRACE("opendir fake maildir: %s\n", path);
     dir_fd->type = OPENDIR_TYPE_MAIL_DIR;
   }
   else if (strcmp(last_slash + 1, "new") == 0 ||
            strcmp(last_slash + 1, "tmp") == 0) {
     /* Listing '/<query>/new' or '/<query>/tmp', so return nothing. */
     LOG_TRACE("opendir fake empty new/, tmp/ maildir: %s\n", path);
     dir_fd->type = OPENDIR_TYPE_EMPTY_DIR;
   }
   else if (strcmp(last_slash + 1, "cur") == 0) {
     /* Listing '/<query>/cur', so parse the query from the pathname, and
      * execute it to get the iterator, and remember it.
      */
     dir_fd->type = OPENDIR_TYPE_NOTMUCH_QUERY;
     char trans_name[PATH_MAX];
     strncpy(trans_name, path + 1, last_slash - path - 1);
     trans_name[last_slash - path - 1] = '\0';

     /* If it's a symlink, dereference it. */
     struct stat stbuf;
     while (res == 0) {
       LOG_TRACE("opendir stat(%s)\n", trans_name);
       if (lstat(trans_name, &stbuf) == 0 &&
           S_ISLNK(stbuf.st_mode)) {
         char link_name[PATH_MAX + 1];
         LOG_TRACE("opendir dereference symlink %s for query\n", trans_name);
         res = readlink(trans_name, link_name, PATH_MAX);
         if (res >= 0) {
           link_name[res] = '\0';
           memcpy(trans_name, link_name, res + 1);
           res = 0;
         }
         else
           res = -errno;
       }
       else
         break;
     }

     LOG_TRACE("opendir notmuch query: '%s'\n", trans_name);

     struct fuse_context *p_fuse_ctx = fuse_get_context();
     notmuch_context_t *p_ctx = (notmuch_context_t *)p_fuse_ctx->private_data;
     database_open(p_ctx, FALSE);

     dir_fd->next_offset = 1;
     dir_fd->p_query = notmuch_query_create(p_ctx->db, trans_name);
     if (dir_fd->p_query != NULL) {
       /* Exclude messages that match the 'excluded' tags. */
       char *exclude_tag = strtok(p_ctx->excluded_tags, "\n");
       while (exclude_tag != NULL) {
         notmuch_query_add_tag_exclude(dir_fd->p_query, exclude_tag);
         exclude_tag = strtok(NULL, "\n");
       }
       notmuch_query_set_omit_excluded(dir_fd->p_query, NOTMUCH_EXCLUDE_ALL);

       /* Run the query. */
       notmuch_status_t status =
         notmuch_query_search_messages_st(dir_fd->p_query, &dir_fd->p_messages);
       if (status != NOTMUCH_STATUS_SUCCESS) {
         notmuch_query_destroy(dir_fd->p_query);
         dir_fd->p_query = NULL;
         database_close(p_ctx);
         res = -EIO;
       }
       else {
         /* On success, the database is left open here. */
       }
     }
     else {
       database_close(p_ctx);
       res = -EIO;
     }
   }
   else {
     /* Trying to open an unrecognized directory, that we did not put there.
      * Error it, since this is not supported behavior.
      */
     res = -ENOENT;
   }
 }

 if (res == 0) {
   fi->fh = (uint64_t)(uintptr_t)dir_fd;
 }
 else {
   free(dir_fd);
 }
 return res;
}

/*============================================================================*/

static int notmuchfs_releasedir (const char *path, struct fuse_file_info *fi)
{
 opendir_t *dir_fd = (opendir_t *)(uintptr_t)fi->fh;
 if (dir_fd != NULL) {
   if (dir_fd->type == OPENDIR_TYPE_NOTMUCH_QUERY) {
     if (dir_fd->p_messages != NULL)
       notmuch_messages_destroy(dir_fd->p_messages);
     if (dir_fd->p_query != NULL)
       notmuch_query_destroy(dir_fd->p_query);

     struct fuse_context *p_fuse_ctx = fuse_get_context();
     notmuch_context_t *p_ctx = (notmuch_context_t *)p_fuse_ctx->private_data;
     database_close(p_ctx);
   }
   else if (dir_fd->type == OPENDIR_TYPE_BACKING_DIR) {
     int ret = closedir(dir_fd->fd);
     /* The only possible error value is EBADF, which would be a programming
      * error.
      */
     assert(ret == 0);
   }
   free(dir_fd);
   dir_fd = NULL;
 }
 return 0;
}

/*============================================================================*/

/**
 * Adds an entry to a readdir() buffer with a maildir file, representing the
 * given notmuch messages.
 *
 * @param[in,out] dir_fd    The opendir context.
 * @param[in]     p_message The notmuch message to add to the directory
 *                          listing.
 * @param[in,out] buf       The readdir() buffer.
 * @param[in]     filler    The filler function.
 *
 * @return A negative errno on error, 0 on success, #INT_MAX if the
 *         directory is too full to add this message.
 */
static int fill_dir_with_message (opendir_t         *dir_fd,
                                  notmuch_message_t *p_message,
                                  void              *buf,
                                  fuse_fill_dir_t    filler)
{
 assert(p_message != NULL);

 int res = 0;

 const char *fname = notmuch_message_get_filename(p_message);
 if (fname != NULL) {
   struct stat stbuf;
   if (stat(fname, &stbuf) == 0) {
     char trans_name[PATH_MAX];
     strncpy(trans_name, fname, PATH_MAX - 1);
     trans_name[PATH_MAX - 1] = '\0';
     string_replace(trans_name, '/', '#');

     /* Perpetuate the file size inflation lie told in getattr(). */
     stbuf.st_size += MAX_XLABEL_LENGTH;
     LOG_TRACE("readdir filling dir %s at %ld\n",
               trans_name, dir_fd->next_offset);
     if (filler(buf, trans_name, &stbuf, dir_fd->next_offset++) != 0) {
       LOG_TRACE("readdir filler full \"%s\".\n", trans_name);
       dir_fd->next_offset--;
       res = INT_MAX;
     }
   }
   else if (errno == ENOENT) {
     /* If a message is gone, don't stop the whole readdir(). */
     fprintf(stderr, "WARNING: Skipping missing file \"%s\".\n", fname);
   }
   else {
     fprintf(stderr, "ERROR: notmuch message stat error \"%s\" %s.\n", fname,
             strerror(errno));
     res = -errno;
   }
 }
 else {
   /* There's nothing we can do about this case, which I doubt can ever
    * happen. Just ignore it.
    */
 }

 return res;
}

/*============================================================================*/

static int notmuchfs_readdir (const char            *path,
                              void                  *buf,
                              fuse_fill_dir_t        filler,
                              off_t                  offset_in,
                              struct fuse_file_info *fi)
{
 int res = 0;

 opendir_t *dir_fd = (opendir_t *)(uintptr_t)fi->fh;

 switch (dir_fd->type) {
   case OPENDIR_TYPE_NOTMUCH_QUERY:
     {
      if (offset_in == 0) {
        filler(buf, ".", NULL, dir_fd->next_offset++);
        filler(buf, "..", NULL, dir_fd->next_offset++);
      }
      else if (offset_in + 1 != dir_fd->next_offset) {
        fprintf(stderr, "ERROR: discontiguous dir offsets %ld %ld.\n",
                (long int)offset_in, (long int)dir_fd->next_offset);
        res = -EDOM;
        break;
      }

      notmuch_message_t *p_message = NULL;
      while (res == 0 &&
             (p_message = notmuch_messages_get(dir_fd->p_messages)) != NULL) {

        res = fill_dir_with_message(dir_fd, p_message, buf, filler);

        notmuch_message_destroy(p_message);
        if (res == INT_MAX) {
          res = 0;
          break;
        }
        notmuch_messages_move_to_next(dir_fd->p_messages);
      }
      break;
     }

   case OPENDIR_TYPE_BACKING_DIR:
     {
      LOG_TRACE("readdir read from backing directory:\n");
      seekdir(dir_fd->fd, offset_in);

      struct dirent *de;
      while ((de = readdir(dir_fd->fd)) != NULL) {
        struct stat st;
        memset(&st, 0, sizeof(st));
        st.st_ino = de->d_ino;
        st.st_mode = de->d_type << 12;

        if (filler(buf, de->d_name, &st, telldir(dir_fd->fd)) != 0) {
          res = 0;
          break;
        }
      }
      break;
     }

   case OPENDIR_TYPE_EMPTY_DIR:
     {
      filler(buf, ".", NULL, 0);
      filler(buf, "..", NULL, 0);
      break;
     }

   case OPENDIR_TYPE_MAIL_DIR:
     {
      filler(buf, ".", NULL, 0);
      filler(buf, "..", NULL, 0);
      filler(buf, "cur", NULL, 0);
      filler(buf, "new", NULL, 0);
      filler(buf, "tmp", NULL, 0);
      break;
     }
 }

 return res;
}

/*============================================================================*/

/**
 * The string to replace the list of message tags with in the X-Label header,
 * if the header will not fit in #MAX_XLABEL_LENGTH.
 */
#define TAG_ERROR_STRING "ERROR"

/**
 * Fill the provided buffer with all the tags of the given message, comma
 * separated. If they don't all fit, replace the whole string with
 * #TAG_ERROR_STRING. No NULL termination.
 *
 * @param[in,out] buf_in    The buffer to fill.
 * @param[in]     length    The length of 'buf_in'.
 * @param[in]     p_message The message to read tags from.
 * @return The number of bytes written to the buffer.
 */
static size_t fill_string_with_tags (char              *buf_in,
                                     size_t             length,
                                     notmuch_message_t *p_message)
{
 char           *buf     = buf_in;
 notmuch_tags_t *tags    = notmuch_message_get_tags(p_message);
 const char     *tag_str = NULL;
 bool            error   = FALSE;

 while ((tag_str = notmuch_tags_get(tags)) != NULL) {
   LOG_TRACE("Adding tag \"%s\" to X-label\n", tag_str);

   /* If this tag can fit in the buffer, append it. Otherwise, error out. */
   if (strlen(tag_str) >= length - (buf - buf_in)) {
     error = TRUE;
     break;
   }
   memcpy(buf, tag_str, strlen(tag_str));
   buf += strlen(tag_str);

   notmuch_tags_move_to_next(tags);

   if (notmuch_tags_valid(tags)) {
     /* There's another one coming, add separator. */
     if (length - (buf - buf_in) < 1) {
       error = TRUE;
       break;
     }
     buf[0] = ',';
     buf++;
   }
 }
 if (error) {
   LOG_TRACE("X-Label buffer overflow\n");
   buf = buf_in;
   memcpy(buf, TAG_ERROR_STRING, strlen(TAG_ERROR_STRING));
   buf += strlen(TAG_ERROR_STRING);
 }
 notmuch_tags_destroy(tags);

 return buf - buf_in;
}

/*============================================================================*/

/**
 * A notmuchfs open file handle type, created by notmuchfs_open().
 */
typedef struct
{
 /** The actual file handle. */
 int  fh;
 /** The X-Label header - filled by open(), used later. */
 char x_label[MAX_XLABEL_LENGTH];
} open_t;


static int notmuchfs_open (const char *path, struct fuse_file_info *fi)
{
 if ((fi->flags & 3) != O_RDONLY)
   return -EACCES;

 open_t *p_open = malloc(sizeof(open_t));
 memset(p_open, 0, sizeof(open_t));

 char *last_slash = strrchr(path + 1, '/');
 if (last_slash == NULL) {
   p_open->fh = open(path + 1, O_RDONLY);
   if (p_open->fh == -1) {
     int err = errno;
     free(p_open);
     return -err;
   }
 }
 else {
   char trans_name[PATH_MAX];
   strncpy(trans_name, last_slash + 1, PATH_MAX - 1);
   trans_name[PATH_MAX - 1] = '\0';

   char *first_pslash = strchr(trans_name, '#');
   if (first_pslash != NULL) {
     string_replace(trans_name, '#', '/');

     struct fuse_context *p_fuse_ctx = fuse_get_context();
     notmuch_context_t   *p_ctx      =
       (notmuch_context_t *)p_fuse_ctx->private_data;

     /**
      * @todo shouldn't need writeable database here, but otherwise get:
      *   "Internal error: Failure to ensure database is writable"
      * why?
      */
     database_open(p_ctx, TRUE);

     LOG_TRACE("open notmuch lookup by name: %s\n", trans_name);
     notmuch_message_t *p_message;
     if (notmuch_database_find_message_by_filename(p_ctx->db, trans_name,
                                                   &p_message) ==
         NOTMUCH_STATUS_SUCCESS) {
       if (p_message == NULL) {
         LOG_TRACE("WARNING: Message not found in DB - ignoring.");
       }
       else {
         char *buf = p_open->x_label;
         /* Make sure the buffer is big enough to at least take the
          * representation of overflow.
          */
         assert(MAX_XLABEL_LENGTH >
                strlen(XLABEL) + strlen(TAG_ERROR_STRING) + 1);
         memcpy(buf, XLABEL, strlen(XLABEL));
         buf += strlen(XLABEL);
         buf += fill_string_with_tags(buf,
                                      MAX_XLABEL_LENGTH - strlen(XLABEL) - 1,
                                      p_message);

         /* Pad the header out. RFC5322 doesn't say anything about this that I
          * can see. NULs don't work, nor \n's, so spaces are used.
          */
         while (buf - p_open->x_label < (MAX_XLABEL_LENGTH - 1)) {
           assert(1 <= MAX_XLABEL_LENGTH - (buf - p_open->x_label));
           buf[0] = ' ';
           buf++;
         }

         assert(1 <= MAX_XLABEL_LENGTH - (buf - p_open->x_label));
         buf[0] = '\n';
         buf++;

         notmuch_message_destroy(p_message);
       }
       database_close(p_ctx);
     }
     else {
       /* Notmuch somehow failed to do anything successfully, fail the open. */
       database_close(p_ctx);
       free(p_open);
       return -EIO;
     }
   }

   LOG_TRACE("open(%s)\n", trans_name);
   p_open->fh = open(trans_name, O_RDONLY);
   if (p_open->fh == -1) {
     int err = errno;
     free(p_open);
     return -err;
   }
 }

 fi->fh = (uint64_t)(uintptr_t)p_open;

 return 0;
}

/*============================================================================*/

static int notmuchfs_release (const char *path, struct fuse_file_info *fi)
{
 open_t *p_open = (open_t *)(uintptr_t)fi->fh;
 assert(p_open != NULL);

 LOG_TRACE("close(%d)\n", p_open->fh);
 int res = close(p_open->fh);
 assert(res == 0);

 free(p_open);
 fi->fh = (uint64_t)(uintptr_t)NULL;

 return 0; /* Documentation says this is ignored. */
}

/*============================================================================*/

static int notmuchfs_read (const char *path,
                           char       *buf_in,
                           size_t      size,
                           off_t       offset,
                           struct      fuse_file_info *fi)
{
 char   *buf        = buf_in;
 size_t  offset_adj = MAX_XLABEL_LENGTH;
 open_t *p_open     = (open_t *)(uintptr_t)fi->fh;

 assert(p_open != NULL);

 if (offset < MAX_XLABEL_LENGTH) {
   size_t bytes_to_copy = MIN(MAX_XLABEL_LENGTH - offset, size);
   memcpy(buf, p_open->x_label + offset, bytes_to_copy);
   buf += bytes_to_copy;
   offset_adj = offset + bytes_to_copy;
   offset += bytes_to_copy;
 }

 size_t bytes_to_read = size - (buf - buf_in);
 if (bytes_to_read > 0) {
   LOG_TRACE("read(%s, %ld, %ld)\n", path, offset - offset_adj,
             bytes_to_read);
   ssize_t bytes_read = pread(p_open->fh, buf, bytes_to_read,
                              offset - offset_adj);
   if (bytes_read == -1)
     return -errno;
   buf += bytes_read;
 }

 return (int)(buf - buf_in);
}

/*============================================================================*/

static int notmuchfs_mkdir (const char* path, mode_t mode)
{
 assert(path[0] == '/');

 if (mkdir(path + 1, mode) == -1)
   return -errno;
 return 0;
}

/*============================================================================*/

static int notmuchfs_rmdir (const char* path)
{
 assert(path[0] == '/');

 if (rmdir(path + 1) == -1)
   return -errno;
 return 0;
}

/*============================================================================*/

static int notmuchfs_rename (const char* from, const char* to)
{
 assert(from[0] == '/');
 assert(to[0] == '/');

 char    *last_pslash_from     = strrchr(from + 1, '#');
 char    *last_pslash_to       = strrchr(to + 1, '#');
 char    *last_slash_from      = strrchr(from + 1, '/');
 char    *last_slash_to        = strrchr(to + 1, '/');
 /* Values are 0 (no workaround), 1 or 2 (see below). */
 unsigned mutt_2476_workaround = 0;

 if (last_pslash_from == NULL && last_pslash_to == NULL) {
   /* Renaming from a non-maildir name to another non-maildir name - just pass
    * it through.
    */
   LOG_TRACE("rename(%s, %s)\n", from + 1, to + 1);

   if (rename(from + 1, to + 1) == 0)
     return 0;
   else
     return -errno;
 }

 if (last_pslash_from == NULL || last_pslash_to == NULL) {
   /* Renaming from a non-maildir name to a maildir name, or vice versa.
    * Doesn't make much sense - deny it.
    */
   LOG_TRACE("ERROR: Rename die 1\n");
   return -ENOTSUP;
 }

 if ((last_pslash_from - from) != (last_pslash_to - to)) {
   /* Renaming from one maildir name to another, in different paths. */
   LOG_TRACE("ERROR: Rename die 2\n");
   return -ENOTSUP;
 }

 if (strncmp(from, to, last_pslash_from - from) != 0) {
   /* Renaming from one maildir name to another, in different paths, but the
    * paths have the same length.
    */

   if (global_config.mutt_2476_workaround_allowed) {
     /* The workaround here is to intercept all renames of the form:
      * Case 1:
      *   rename(/real/path/cur/fake#maildir#cur#foofile,
      *          /real/path/new/fake#maildir#cur#barfile)
      * and
      * Case 2:
      *   rename(/real/path/new/fake#maildir#cur#foofile,
      *          /real/path/cur/fake#maildir#cur#barfile)
      * and ignore the 'new' part - treat it as if the 'new' was 'cur'.
      */

     if ((last_slash_from - from) >= 3 &&
         memcmp(from, to, last_slash_from - from - 3) == 0) {
       if (memcmp(last_slash_from - 3, "cur", 3) == 0 &&
           memcmp(last_slash_to - 3, "new", 3) == 0) {
         /* Case 1. */
         mutt_2476_workaround = 1;
       }
       else if (memcmp(last_slash_from - 3, "new", 3) == 0 &&
                memcmp(last_slash_to - 3, "cur", 3) == 0) {
         /* Case 2. */
         mutt_2476_workaround = 2;
       }

       if (mutt_2476_workaround != 0) {
         LOG_TRACE("Activating mutt_bug_2476 workaround for rename(%s, %s)\n",
                   from, to);
       }
     }
   }
   if (mutt_2476_workaround == 0) {
     fprintf(stderr, "ERROR: Rename die 3 %s %s %ld\n", from, to,
             (long int)(last_pslash_from - from));
     return -ENOTSUP;
   }
 }

 /* Renaming from one file name to another, both in the same (maildir)
  * directory.
  */
 char trans_name_from[PATH_MAX];
 strncpy(trans_name_from, last_slash_from + 1, PATH_MAX - 1);
 trans_name_from[PATH_MAX - 1] = '\0';
 string_replace(trans_name_from, '#', '/');

 char trans_name_to[PATH_MAX];
 strncpy(trans_name_to, last_slash_to + 1, PATH_MAX - 1);
 trans_name_to[PATH_MAX - 1] = '\0';
 string_replace(trans_name_to, '#', '/');

 LOG_TRACE("rename(%s, %s)\n", trans_name_from, trans_name_to);
 if (rename(trans_name_from, trans_name_to) == -1)
   return -errno;


 /* Rename it in the notmuch database too. */
 int                  res        = 0;
 struct fuse_context *p_fuse_ctx = fuse_get_context();
 notmuch_context_t    *p_ctx     =
   (notmuch_context_t *)p_fuse_ctx->private_data;

 database_open(p_ctx, TRUE);

 if (notmuch_database_begin_atomic(p_ctx->db) != NOTMUCH_STATUS_SUCCESS) {
   res = -EIO;
 }
 else {
   /* If renaming from/to the same name, skip this - it gets confused. The
    * mutt bug 2476 workaround can cause this, but it's also legitimately
    * possible.
    */
   if (strncmp(trans_name_from, trans_name_to, PATH_MAX) != 0) {
     LOG_TRACE("notmuch_database_add_message(%s)\n", trans_name_to);
     if (notmuch_database_add_message(p_ctx->db, trans_name_to, NULL) !=
         NOTMUCH_STATUS_DUPLICATE_MESSAGE_ID) {
       LOG_TRACE("WARNING: Did not find message in database: %s\n",
                 trans_name_to);
     }
     else {
       LOG_TRACE("notmuch_database_remove_message(%s)\n", trans_name_from);
       notmuch_status_t status =
         notmuch_database_remove_message(p_ctx->db, trans_name_from);
       if (status != NOTMUCH_STATUS_DUPLICATE_MESSAGE_ID) {
         LOG_TRACE("WARNING: Did not find old message in database: %s\n",
                   trans_name_from);
         /* Continue, can't do anything about it anyway. */
       }
     }
   }

   /* Lookup the message again here to sync the maildir flags. Do *not* use
    * the message returned by notmuch_database_add_message(), it seems to
    * refer to the file name that is subsequently removed above.
    */
   notmuch_message_t *p_message;
   LOG_TRACE("rename notmuch lookup by name: %s\n", trans_name_to);
   if (notmuch_database_find_message_by_filename(p_ctx->db, trans_name_to,
                                                 &p_message) ==
       NOTMUCH_STATUS_SUCCESS) {
     /* We just put it there, it should still be there. */
     assert(p_message != NULL);

     LOG_TRACE("notmuch_message_maildir_flags_to_tags(%s)\n", trans_name_to);
     notmuch_message_maildir_flags_to_tags(p_message);

     if (global_config.mutt_2476_workaround_allowed) {
       /* If mutt just moved the file to 'new', add the 'unread' flag.
        * notmuch_message_maildir_flags_to_tags() does not do this because it's
        * somewhat against the interpretation of the maildir spec, but it is
        * what mutt means.
        */
       if (mutt_2476_workaround == 1) {
         LOG_TRACE("notmuch_message_add_tag(%s, unread)\n", trans_name_to);
         if (notmuch_message_add_tag(p_message, "unread") !=
             NOTMUCH_STATUS_SUCCESS) {
           /* Ignore all errors. Flags will go slightly out of sync now until
            * 'notmuch new' fixes them.
            */
         }
       }
     }
     notmuch_message_destroy(p_message);
   }
   else {
     /* Ignore all errors. Flags will go slightly out of sync now until
      * 'notmuch new' fixes them.
      */
   }
   if (notmuch_database_end_atomic(p_ctx->db) != NOTMUCH_STATUS_SUCCESS)
     res = -EIO;
 }

 database_close(p_ctx);

 return res;
}

/*============================================================================*/

static int notmuchfs_unlink (const char* path)
{
 /* Ignore the initial '/' */
 assert(path[0] == '/');
 path++;

 char *last_pslash = strrchr(path, '#');

 if (last_pslash != NULL) {
   char *last_slash = strrchr(path, '/');
   char  trans_name[PATH_MAX];

   strncpy(trans_name, last_slash + 1, PATH_MAX - 1);
   trans_name[PATH_MAX - 1] = '\0';
   string_replace(trans_name, '#', '/');

#if 0
   /* Delete the message from the notmuch database too. Is this the right
    * thing to do?
    */
   struct fuse_context *p_fuse_ctx = fuse_get_context();
   notmuch_context_t    *p_ctx     =
     (notmuch_context_t *)p_fuse_ctx->private_data;

   database_open(p_ctx, TRUE);

   LOG_TRACE("notmuch_database_remove_message(%s)\n", trans_name);
   notmuch_database_remove_message(p_ctx->db, trans_name);

   database_close(p_ctx);
#endif

   LOG_TRACE("unlink(%s)\n", trans_name);
   if (unlink(trans_name) != 0)
     return -errno;
   return 0;
 }
 else {
   LOG_TRACE("unlink(%s)\n", path);
   if (unlink(path) != 0)
     return -errno;
   return 0;
 }
}

/*============================================================================*/

static int notmuchfs_symlink (const char* to, const char* from)
{
 assert(from[0] == '/');

 if (symlink(to, from + 1) != 0)
   return -errno;
 return 0;
}

/*============================================================================*/

static int notmuchfs_readlink (const char* path, char* buf, size_t size)
{
 assert(path[0] == '/');

 int res = readlink(path + 1, buf, size);
 if (res >=0) {
   buf[res] = '\0';
   res = 0;
 }
 else
   res = -errno;
 return res;
}

/*============================================================================*/

static struct fuse_operations notmuchfs_oper = {
    .init       = notmuchfs_init,
    .destroy    = notmuchfs_destroy,
    .getattr    = notmuchfs_getattr,
    .opendir    = notmuchfs_opendir,
    .releasedir = notmuchfs_releasedir,
    .readdir    = notmuchfs_readdir,
    .open       = notmuchfs_open,
    .release    = notmuchfs_release,
    .read       = notmuchfs_read,
    .mkdir      = notmuchfs_mkdir,
    .rmdir      = notmuchfs_rmdir,
    .rename     = notmuchfs_rename,
    .unlink     = notmuchfs_unlink,
    .symlink    = notmuchfs_symlink,
    .readlink   = notmuchfs_readlink
};

/*============================================================================*/

/**
 * Option key types used in the CLI parser.
 */

enum {
  KEY_HELP,
  KEY_VERSION,
};

#define NOTMUCHFS_OPT(t, p, v) { t, offsetof(struct notmuchfs_config, p), v }

static struct fuse_opt notmuchfs_opts[] = {
  NOTMUCHFS_OPT("backing_dir=%s",               backing_dir, 0),
  NOTMUCHFS_OPT("mail_dir=%s",                  mail_dir, 0),
  NOTMUCHFS_OPT("mutt_2476_workaround",         mutt_2476_workaround_allowed, 1),
  NOTMUCHFS_OPT("nomutt_2476_workaround",       mutt_2476_workaround_allowed, 0),
  NOTMUCHFS_OPT("--mutt_2476_workaround=true",  mutt_2476_workaround_allowed, 1),
  NOTMUCHFS_OPT("--mutt_2476_workaround=false", mutt_2476_workaround_allowed, 0),

  FUSE_OPT_KEY("-V",        KEY_VERSION),
  FUSE_OPT_KEY("--version", KEY_VERSION),
  FUSE_OPT_KEY("-h",        KEY_HELP),
  FUSE_OPT_KEY("--help",    KEY_HELP),
  FUSE_OPT_END
};

static void print_notmuchfs_usage (char *arg0) {
  fprintf(stderr,
          "Usage: %s mountpoint -o backing_dir=PATH -o mail_dir=PATH [options]\n"
          "\n"
          "General options:\n"
          "    -o opt,[opt...]  mount options\n"
          "    -h   --help      print help\n"
          "    -V   --version   print version\n"
          "\n"
          "Notmuchfs options:\n"
          "    -o backing_dir=PATH  Path to backing directory (required)\n"
          "    -o mail_dir=PATH     Path to parent directory of notmuch database (required)\n"
          "    -o mutt_2476_workaround\n"
          "    -o nomutt_2476_workaround (default)\n"
          , arg0);
}


static int notmuchfs_opt_proc (void             *data,
                               const char       *arg,
                               int               key,
                               struct fuse_args *outargs)
{
 switch (key) {
   case KEY_HELP:
     print_notmuchfs_usage(outargs->argv[0]);
     fuse_opt_add_arg(outargs, "-ho");
     fuse_main(outargs->argc, outargs->argv, &notmuchfs_oper, NULL);
     exit(1);

   case KEY_VERSION:
     fprintf(stderr, "Notmuchfs version %s\n", NOTMUCHFS_VERSION);
     fuse_opt_add_arg(outargs, "--version");
     fuse_main(outargs->argc, outargs->argv, &notmuchfs_oper, NULL);
     exit(0);
 }
 return 1;
}

/*============================================================================*/

int main(int argc, char *argv[])
{
 struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

 fuse_opt_parse(&args, &global_config, notmuchfs_opts, notmuchfs_opt_proc);

 if (global_config.backing_dir == NULL ||
     global_config.mail_dir == NULL) {
   fprintf(stderr, "Required option(s) missing. See \"%s --help\".\n",
           args.argv[0]);
   exit(1);
 }

 struct stat stbuf;
 if (stat(global_config.backing_dir, &stbuf) != 0 ||
     !S_ISDIR(stbuf.st_mode)) {
   fprintf(stderr, "Can't find backing dir \"%s\".\n",
           global_config.backing_dir);
   exit(1);
 }

 if (stat(global_config.mail_dir, &stbuf) != 0 ||
     !S_ISDIR(stbuf.st_mode)) {
   fprintf(stderr, "Can't find mail dir \"%s\".\n",
           global_config.mail_dir);
   exit(1);
 }


 int ret = fuse_main(args.argc, args.argv, &notmuchfs_oper,
                     NULL /* userdata */);
 fuse_opt_free_args(&args);
 return ret;
}

/*============================================================================*/
