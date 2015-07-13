/*
 * Copyright (c) 2012, Groupon, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   - Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *   - Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *
 *   - Neither the name of Groupon nor the names of its contributors
 *     may be used to endorse or promote products derived from this
 *     software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define _FILE_OFFSET_BITS 64
#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <getopt.h>
#include <syslog.h>
#include <microhttpd.h>
#include <StreamBoyerMooreHorspool.h>

#define DAEMON_NAME "mysql_slowlogd"
#define USAGE "usage: mysql_slowlogd -f /path/to/slow_query.log\n"
#define FULL_USAGE USAGE \
    "  -h, --help        display this help and exit\n" \
    "  -x, --no-daemon   do not daemonize\n" \
    "  -f, --slowlog     path to MySQL slow log\n" \
    "  -p, --port        port to use\n"

#define DEFAULT_PORT 3307
const char *QUERY_DELIM = "# User@Host: ";
const int QUERY_DELIM_LEN = 13;

struct tailed_file {
    char *name;       /* pathname to file */
    int fd;           /* opened fd, or <= 0 if not opened */
    struct stat st;   /* cached stat() results */
    int wait_count;   /* count of iterations in wait state */
    int rate_limit;                /* desired rate limit; interpreted as keep 1/N */
    int rate_limit_counter;        /* number of queries skipped since last kept query */ 
    struct StreamBMH *bmh_context; /* Boyer-Moore-Horspool string search context */
    struct StreamBMH_Occ bmh_occ;  /* Boyer-Moore-Horspool occurrance table */
    enum {
        TAILED_FILE_STATE_START,   /* just started, move to DUMP state next */
        TAILED_FILE_STATE_WAIT,    /* reached eof; wait for more data */
        TAILED_FILE_STATE_DUMP     /* still have bytes to send */
    } state;
};

static void apply_rate_limit(struct StreamBMH *bmh_context,
                             struct StreamBMH_Occ *bmh_occ,
                             char *buf,
                             size_t *len, /* will be updated upon returning */
                             int rate_limit,
                             int *rate_limit_counter /* updated upon returning */
                             )
{
    char *dest = buf;
    char *next = buf;
    int keep = 0;
    size_t n;

    /* Unfortunately, libmicrohttpd doesn't have an fprintf(socket, ...)-esque
       interface. It provides a callback with a buffer to be filled. We filled
       the buffer, but now it has queries that we want to skip over. So, we
       overwrite the data, shrinking the length of data in the buffer.

       Example, assuming rate_limit=2:
           +--------------------------------+
           |query1\nDELIMquery2\nDELIMquery3|
           +--------------------------------+
            01234567890123456789012345678901 len=32
            ^-- buf

           1. buf, dest, next start at position 0.
           2. Search for the delimiter, find it at position 12.
           3. Keep query1. Advance `dest` to position 12; `next` to position 12.
           4. Search for next delimiter, find it at position 25.
           5. Move query3 to position 12 which skips query2

           Result:
           +-------------------+
           |query1\nDELIMquery3|
           +-------------------+
            0123456789012345678 len=19
    */

    while ((next - buf) < *len) {
        /* search for next query */
        n = sbmh_feed(
                bmh_context, bmh_occ,
                (const unsigned char *)QUERY_DELIM, QUERY_DELIM_LEN,
                (const unsigned char *)next, *len - (next - buf));
        keep = (*rate_limit_counter == 0);
        if (keep) {
            if(dest != next)
                memmove(dest, next, n);
            dest += n;
        }
        next += n;
        if (bmh_context->found) {
            *rate_limit_counter = (*rate_limit_counter + 1) % rate_limit;
            sbmh_reset(bmh_context);
        }
    }
    *len = (dest - buf);
}

static void free_tailed_file(struct tailed_file *tf) {
    if (tf->name)
        free (tf->name);
    if (tf->fd > 0) 
        close(tf->fd);
    if (tf->bmh_context)
        free(tf->bmh_context);
    free(tf);
}

static struct tailed_file * open_tailed_file(const char *filename) {
    struct tailed_file *tf = NULL;
    int result;

    tf = malloc(sizeof(struct tailed_file));
    if (!tf) goto bail;
    memset(tf, 0, sizeof(tf));

    tf->name = strdup(filename);
    if (!tf->name) goto bail;

    result = stat(tf->name, &tf->st);
    if (0 != result) goto bail;

    tf->fd = open(tf->name, O_RDONLY);
    if (tf->fd < 0) goto bail;

    tf->state = TAILED_FILE_STATE_START;

    tf->bmh_context = malloc(SBMH_SIZE(QUERY_DELIM_LEN));
    if (!tf->bmh_context) goto bail;

    sbmh_init(tf->bmh_context, &tf->bmh_occ, (const unsigned char *)QUERY_DELIM, QUERY_DELIM_LEN);
    return tf;

 bail:
    if (tf)
        free_tailed_file(tf);
    return NULL;
}

/* tailed_file_content_reader - implements a ContentReaderCallback function per
 *  http://www.gnu.org/software/libmicrohttpd/microhttpd.html#index-_002aMHD_005fContentReaderCallback
 */
static ssize_t tailed_file_content_reader (void *cls, uint64_t pos, char *buf, size_t max) {
    struct tailed_file *tf = cls;
    struct stat st_new;
    size_t n;

    if (TAILED_FILE_STATE_WAIT == tf->state) {
        /* wait a bit for more data */
        usleep( 250 * 1000 ); /* N * 1000 for ms */
        tf->wait_count++;

        if (0 == fstat(tf->fd, &st_new) )  {

            /* is the file the same? */
            if ( tf->st.st_mode == st_new.st_mode &&
                 tf->st.st_mtime == st_new.st_mtime &&
                 tf->st.st_size == st_new.st_size ) {

                /* maybe it was rolled? */
                if (tf->wait_count >= 4) {                    
                    if (0 == stat(tf->name, &st_new)) {
                        if (tf->st.st_ino != st_new.st_ino || tf->st.st_dev != st_new.st_dev) {
                            if (tf->fd > 0) {
                                close(tf->fd);
                                tf->fd = 0;
                            }
                            tf->st = st_new;
                            tf->fd = open(tf->name, O_RDONLY);
                            if(tf->fd > 0) {
                                tf->state = TAILED_FILE_STATE_DUMP;
                            }
                        }
                    }
                    tf->wait_count = 0;
                }

            } else {
                /* existing file has changed */

                if (st_new.st_size < tf->st.st_size) {
                    /* new file is smaller! It was likely truncated with logrotate
                       copytruncate. Start again from the beginning of the file */
                    lseek(tf->fd, 0, SEEK_SET); /* ignore error */
                }

                /* dump the file */
                tf->wait_count = 0;
                tf->st = st_new;
                tf->state = TAILED_FILE_STATE_DUMP;
            }

        }
    }

    if (TAILED_FILE_STATE_DUMP == tf->state) {
        n = read(tf->fd, buf, max);
        if (0 == n) {
            tf->state = TAILED_FILE_STATE_WAIT;
            return 0;
        }
        if (n < 0) {
            return MHD_CONTENT_READER_END_WITH_ERROR;
        }

        if (tf->rate_limit_counter < 0 || tf->rate_limit > 1) {
            apply_rate_limit(
                tf->bmh_context,
                &tf->bmh_occ,
                buf,
                &n,
                tf->rate_limit,
                &tf->rate_limit_counter);
        }
        return n;
    }

    if (TAILED_FILE_STATE_START == tf->state) {
        /* apply_rate_limit() will neglect to emit QUERY_DELIM on the first
           query after a client connects. So we force emitting the delimiter here. */

        if (QUERY_DELIM_LEN >= max)
            return MHD_CONTENT_READER_END_WITH_ERROR;

        memmove(buf, QUERY_DELIM, QUERY_DELIM_LEN);
        tf->state = TAILED_FILE_STATE_DUMP;

        /* skip the first, potentially partial, query */
        tf->rate_limit_counter = -1;

        return QUERY_DELIM_LEN;
    }

    return 0;
}

/* tailed_file_free_callback - implements a ContentReaderFreeCallback per
 *  http://www.gnu.org/software/libmicrohttpd/microhttpd.html#index-_002aMHD_005fContentReaderFreeCallback
 */
static void tailed_file_free_callback (void *cls) {
    struct tailed_file *tf = cls;
    if (tf)
        free_tailed_file(tf);
}


static int send_slow_log (struct MHD_Connection *connection, const char *filename, int rate_limit) {
    int ret;
    struct MHD_Response *response;
    struct tailed_file *tf;
    const char *server_error =
        "Internal server error: unable to open slow log. Check syslog for more information.\n";

    tf = open_tailed_file(filename);
    tf->rate_limit = rate_limit;

    if (tf) {
        /* seek to the end of the file */
        if (lseek(tf->fd, tf->st.st_size, SEEK_SET) < 0)
            return MHD_NO;

        response = MHD_create_response_from_callback(
            -1,                          /* -1 means unknown total size */
            4 * 1024,                    /* 4k block size */
            &tailed_file_content_reader, /* callback for serving content */
            tf,                          /* pass context */
            &tailed_file_free_callback   /* callback for freeing memory */
        );
        if (NULL == response) {
            free_tailed_file(tf);
            return MHD_NO;
        }
        MHD_add_response_header(response, MHD_HTTP_HEADER_CONTENT_ENCODING, "text/plain");
        ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
        MHD_destroy_response(response);
        return ret;
    } else {
        syslog(LOG_WARNING, "cannot open %s: %m", filename);
        response = MHD_create_response_from_buffer(
            strlen(server_error),
            (void *)server_error,
            MHD_RESPMEM_PERSISTENT
        );
        if (NULL == response)
            return MHD_NO;
        MHD_add_response_header(response, MHD_HTTP_HEADER_CONTENT_ENCODING, "text/plain");
        ret = MHD_queue_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, response);
        MHD_destroy_response(response);
        return ret;
    }
}

static int not_found_page (struct MHD_Connection *connection, const char *url)
{
    int ret;
    struct MHD_Response *response;
    const char *not_found_error =
        "Resource not found.\n";
    syslog(LOG_WARNING, "Resource not found: %s", url);
    response = MHD_create_response_from_buffer(
        strlen(not_found_error),
        (void *)not_found_error,
        MHD_RESPMEM_PERSISTENT
    );
    if (NULL == response)
        return MHD_NO;
    MHD_add_response_header(response, MHD_HTTP_HEADER_CONTENT_ENCODING, "text/plain");
    ret = MHD_queue_response(connection, MHD_HTTP_NOT_FOUND, response);
    MHD_destroy_response(response);
    return ret;
}

static int handle_request (void *cls,
                           struct MHD_Connection *connection, 
                           const char *url, 
                           const char *method,
                           const char *version, 
                           const char *upload_data,
                           size_t *upload_data_size,
                           void **con_cls)
{
    char *filename = cls;
    const char *rate_limit_argument;
    int rate_limit = 1;
    int ret;

    if (0 != strcmp(method, MHD_HTTP_METHOD_GET))
        return MHD_NO; /* unexpected method */

    /* Valid requests:
     *    GET /slow
     */
    if (0 == strcmp("/slow", url)) {
        /* accept /slow?rate_limit=4 */
        rate_limit_argument = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "rate_limit");
        if (rate_limit_argument)
            rate_limit = atoi(rate_limit_argument);
        if (rate_limit < 1)
            rate_limit = 1;
        ret = send_slow_log(connection, filename, rate_limit);
    } else {
        ret = not_found_page(connection, url);
    }
    return ret;
}

static void child_handler(int signum)
{
    switch(signum) {
    case SIGALRM: exit(1); break;
    case SIGUSR1: exit(0); break;
    case SIGCHLD: exit(1); break;
    }
}

/* daemonize() sourced from: http://www.itp.uzh.ch/~dpotter/howto/daemonize 
 * dedicated to the Public Domain by Doug Potter */
static void daemonize(void)
{
    pid_t pid, sid, parent;

    if ( getppid() == 1 ) return; /* already a daemon */

    signal(SIGCHLD,child_handler);
    signal(SIGUSR1,child_handler);
    signal(SIGALRM,child_handler);

    pid = fork();
    if (pid < 0)
        exit(1);
    if (pid > 0) {
        /* Wait for confirmation from the child via SIGTERM or SIGCHLD, or
           for two seconds to elapse (SIGALRM).  pause() should not return. */
        alarm(2);
        pause();
        exit(1);
    }

    /* in child process */
    parent = getppid();

    signal(SIGCHLD,SIG_DFL);
    signal(SIGTSTP,SIG_IGN);
    signal(SIGTTOU,SIG_IGN);
    signal(SIGTTIN,SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    signal(SIGTERM,SIG_DFL);

    umask(0);
    sid = setsid();
    if (sid < 0)
        exit(1);
    if (chdir("/") < 0) 
        exit(1);
    freopen( "/dev/null", "r", stdin);
    freopen( "/dev/null", "w", stdout);
    freopen( "/dev/null", "w", stderr);
    kill( parent, SIGUSR1 );
}

int main (int argc, char **argv) 
{
    char *filename = NULL;
    struct MHD_Daemon *d;
    int opt_daemon = 1;
    unsigned short port = DEFAULT_PORT;

    int c;
    static const char shortopts[] = "h?xf:p:";
    static struct option longopts[] = {
        { "help",      no_argument,       NULL, 'h' },
        { "no-daemon", no_argument,       NULL, 'x' },
        { "slowlog",   required_argument, NULL, 'f' },
        { "port",      required_argument, NULL, 'p' },
        { NULL,        0,                 NULL, 0   }
    };

    while ((c = getopt_long(argc, argv, shortopts, longopts, NULL)) != -1) {
        switch(c) {
        case 'h': /* fall through */
        case '?':
            printf(FULL_USAGE);
            exit(0);
            break;
        case 'x':
            opt_daemon = 0;
            break;
        case 'f':
            filename = optarg;
            break;
        case 'p':
            if (sscanf(optarg, "%hu", &port)) {
                printf("listening on port %d\n", port);
            } else {
                fprintf(stderr, "invalid port %s\n", optarg);
                exit(1);
            }
            if (port < 1 || port > 65535) {
                fprintf(stderr, "port number needs to fall between 1 and 65535\n");
                exit(1);
            }
            break;
        default:
            fprintf(stderr, USAGE);
            exit(1);
        }
    }
    argc -= optind;
    argv += optind;

    if (!filename) {
        fprintf(stderr, USAGE);
        return 1;
    }

    openlog(DAEMON_NAME, LOG_PID, LOG_LOCAL4);
    syslog(LOG_INFO, "starting");

    if (opt_daemon) {
        daemonize();
        syslog(LOG_INFO, "deamonized as pid %d", getpid());
    }

    d = MHD_start_daemon(MHD_USE_THREAD_PER_CONNECTION,
                         port,
                         NULL,
                         NULL,
                         &handle_request,
                         (void *)filename,
                         MHD_OPTION_END);
    if (d == NULL)
        return 1;

    if (opt_daemon)
        pause();         /* Wait for a kill signal to exit the process */
    else
        (void)getchar(); /* Wait for a key press to exit the process */

    MHD_stop_daemon(d);
    syslog(LOG_NOTICE, "terminated");
    closelog();
    return 0;
}
