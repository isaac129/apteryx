/**
 * @file apteryx.c
 * API for configuration and state shared between Apteryx processes.
 * Features:
 * - A simple path:value database.
 * - Tree like structure with each node being a value.
 * - Path specified in directory format (e.g. /root/node1/node2).
 * - Searching for nodes children requires substring search of the path.
 *
 * Copyright 2014, Allied Telesis Labs New Zealand, Ltd
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this library. If not, see <http://www.gnu.org/licenses/>
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <poll.h>
#include <errno.h>
#include "internal.h"
#include "apteryx.pb-c.h"
#include "apteryx.h"

/* Configuration */
bool debug = false;                      /* Debug enabled */
static int ref_count = 0;               /* Library reference count */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER; /* Protect ref_count */
static int stopfd = -1;                 /* Used to stop the RPC server service */
static pthread_t client_thread = -1;    /* Thread data */
static bool thread_running = false;

/* Callback for watched items */
static void
apteryx__watch (Apteryx__Client_Service *service,
                const Apteryx__Watch *watch,
                Apteryx__OKResult_Closure closure, void *closure_data)
{
    Apteryx__OKResult result = APTERYX__OKRESULT__INIT;
    apteryx_watch_callback cb = (apteryx_watch_callback) (long) watch->cb;
    unsigned char *value = NULL;
    size_t vsize = 0;
    (void) service;

    DEBUG ("WATCH CB \"%s\" = \"%s\" (0x%"PRIx64",0x%"PRIx64",0x%"PRIx64")\n",
           watch->path, bytes_to_string (watch->value.data, watch->value.len),
           watch->id, watch->cb, watch->priv);

    /* Check for empty value string */
    if (watch->value.len)
    {
        value = watch->value.data;
        vsize = watch->value.len;
    }

    /* Call the callback */
    if (cb)
        cb (watch->path, (void *) (long) watch->priv, value, vsize);

    /* Return result */
    closure (&result, closure_data);
    return;
}

/* Callback for provided items */
static void
apteryx__provide (Apteryx__Client_Service *service,
                  const Apteryx__Provide *provide,
                  Apteryx__GetResult_Closure closure, void *closure_data)
{
    Apteryx__GetResult result = APTERYX__GET_RESULT__INIT;
    apteryx_provide_callback cb = (apteryx_provide_callback) (long) provide->cb;
    unsigned char *value = NULL;
    size_t vsize = 0;
    (void) service;

    DEBUG ("PROVIDE CB: \"%s\" (0x%"PRIx64",0x%"PRIx64",0x%"PRIx64")\n",
           provide->path, provide->id, provide->cb, provide->priv);

    /* Call the callback */
    if (cb)
        cb (provide->path, (void *) (long) provide->priv, &value, &vsize);

    /* Return result */
    result.value.data = value;
    result.value.len = vsize;
    closure (&result, closure_data);
    if (value)
        free (value);
    return;
}

static int
listen_thread_handler (void *data)
{
    Apteryx__Client_Service service = APTERYX__CLIENT__INIT (apteryx__);
    char service_name[64];
    int pipefd[2];

    /* Create fd to stop server */
    if (pipe (pipefd) != 0)
    {
        ERROR ("Failed to create pipe\n");
        return -1;
    }
    stopfd = pipefd[1];

    DEBUG ("Watch/Provide Thread: started...\n");
    thread_running = true;

    /* Create server and process requests - 4 threads */
    sprintf (service_name, APTERYX_SERVER ".%"PRIu64"", (uint64_t)getpid ());
    if (!rpc_provide_service (service_name, (ProtobufCService *)&service, 0, pipefd[0]))
    {
        ERROR ("Watch/Provide Thread: Failed to start rpc service\n");
    }

    /* Clean up */
    DEBUG ("Watch/Provide Thread: Exiting\n");
    close (pipefd[0]);
    close (pipefd[1]);
    stopfd = -1;
    thread_running = false;
    client_thread = -1;
    return 0;
}

static void
start_client_thread (void)
{
    pthread_mutex_lock (&lock);
    if (!thread_running)
    {
        /* Start the thread */
        pthread_create (&client_thread, NULL, (void *) &listen_thread_handler,
                        (void *) NULL);
        usleep (1000);
    }
    pthread_mutex_unlock (&lock);
}

static void
stop_client_thread (void)
{
    /* Stop the thread */
    if (thread_running && client_thread != -1 && client_thread != pthread_self ())
    {
        int count = 5 * 1000;
        uint8_t dummy = 1;
        /* Signal stop and wait */
        thread_running = false;
        if (write (stopfd, &dummy, 1) != 1)
            ERROR ("Failed to stop server\n");
        while (count-- && client_thread != -1)
            usleep (1000);
        if (client_thread != -1)
        {
            DEBUG ("Shutdown: Killing Listen thread\n");
            pthread_cancel (client_thread);
            pthread_join (client_thread, NULL);
        }
    }
}

static void
handle_ok_response (const Apteryx__OKResult *result, void *closure_data)
{
    if (result == NULL)
        ERROR ("RESULT: Error processing request.\n");
    *(protobuf_c_boolean *) closure_data = 1;
}

bool
apteryx_init (bool debug_enabled)
{
    /* Increment refcount */
    pthread_mutex_lock (&lock);
    ref_count++;
    debug |= debug_enabled;
    pthread_mutex_unlock (&lock);

    /* Ready to go */
    if (ref_count > 1)
        DEBUG ("Init: Initialised\n");
    return true;
}

bool
apteryx_shutdown (void)
{
    /* Check if already shutdown */
    if (ref_count <= 0)
    {
        ERROR ("Shutdown: Already shutdown\n");
        return false;
    }

    /* Decrement ref count */
    pthread_mutex_lock (&lock);
    ref_count--;
    pthread_mutex_unlock (&lock);

    /* Check if there are still other users */
    if (ref_count > 0)
    {
        DEBUG ("Shutdown: More users (refcount=%d)\n", ref_count);
        return true;
    }

    /* Shutdown */
    DEBUG ("Shutdown: Shutting down\n");
    stop_client_thread ();
    thread_running = false;
    DEBUG ("Shutdown: Shutdown\n");
    return true;
}

bool
apteryx_prune (const char *path)
{
    ProtobufCService *rpc_client;
    Apteryx__Prune prune = APTERYX__PRUNE__INIT;
    protobuf_c_boolean is_done = 0;

    DEBUG ("PRUNE: %s\n", path);

    /* Check path */
    if (path[0] != '/')
    {
        ERROR ("PRUNE: invalid path (%s)!\n", path);
        assert(!debug || path[0] == '/');
        return false;
    }

    /* IPC */
    rpc_client = rpc_connect_service (APTERYX_SERVER, &apteryx__server__descriptor);
    if (!rpc_client)
    {
        ERROR ("PRUNE: Falied to connect to server: %s\n", strerror (errno));
        return false;
    }
    prune.path = (char *) path;
    apteryx__server__prune (rpc_client, &prune, handle_ok_response, &is_done);
    protobuf_c_service_destroy (rpc_client);
    if (!is_done)
    {
        ERROR ("PRUNE: No response\n");
        return false;
    }

    /* Success */
    return true;
}

bool
apteryx_dump (const char *path, FILE *fp)
{
    GList *children, *iter;
    unsigned char *value = NULL;
    size_t size;

    DEBUG ("DUMP: %s\n", path);

    /* Check initialised */
    if (ref_count <= 0)
    {
        ERROR ("DUMP: not initialised!\n");
        assert(ref_count > 0);
        return false;
    }

    if (apteryx_get (path, &value, &size) && value)
    {
        fprintf (fp, "%-64s%.*s\n", path, (int) size, value);
        free (value);
    }

    children = apteryx_search (path);
    for (iter = children; iter; iter = g_list_next (iter))
    {
        char *_path = NULL;
        int len = asprintf (&_path, "%s/", (const char *) iter->data);
        if (len)
        {
            apteryx_dump ((const char *) _path, fp);
            free (_path);
        }
    }
    g_list_free_full (children, free);
    return true;
}

bool
apteryx_set (const char *path, unsigned char *value, size_t size)
{
    ProtobufCService *rpc_client;
    Apteryx__Set set = APTERYX__SET__INIT;
    protobuf_c_boolean is_done = 0;

    DEBUG ("SET: %s = %s\n", path, bytes_to_string (value, size));

    /* Check path */
    if (path[0] != '/')
    {
        ERROR ("SET: invalid path (%s)!\n", path);
        assert(!debug || path[0] == '/');
        return false;
    }

    /* IPC */
    rpc_client = rpc_connect_service (APTERYX_SERVER, &apteryx__server__descriptor);
    if (!rpc_client)
    {
        ERROR ("SET: Falied to connect to server: %s\n", strerror (errno));
        return false;
    }
    set.path = (char *) path;
    set.value.data = value;
    set.value.len = size;
    apteryx__server__set (rpc_client, &set, handle_ok_response, &is_done);
    protobuf_c_service_destroy (rpc_client);
    if (!is_done)
    {
        ERROR ("SET: No response\n");
        return false;
    }

    /* Success */
    return true;
}

bool
apteryx_set_int (const char *path, const char *key, int32_t value)
{
    char *full_path;
    size_t len;
    unsigned char *v;
    bool res = false;

    /* Create full path */
    if (key)
        len = asprintf (&full_path, "%s/%s", path, key);
    else
        len = asprintf (&full_path, "%s", path);
    if (len)
    {
        /* Store as a string at the moment */
        len = asprintf ((char **) &v, "%d", value);
        res = apteryx_set (full_path, v, len + 1);
        free ((void *) v);
        free (full_path);
    }
    return res;
}

bool
apteryx_set_string (const char *path, const char *key, const char *value)
{
    char *full_path;
    size_t len;
    bool res = false;

    /* Create full path */
    if (key)
        len = asprintf (&full_path, "%s/%s", path, key);
    else
        len = asprintf (&full_path, "%s", path);
    if (len)
    {
        res = apteryx_set (full_path, (unsigned char *) value,
                           value ? strlen (value) + 1 : 0);
        free (full_path);
    }
    return res;
}

typedef struct _get_data_t
{
    unsigned char *value;
    size_t length;
    bool done;
} get_data_t;

static void
handle_get_response (const Apteryx__GetResult *result, void *closure_data)
{
    get_data_t *data = (get_data_t *)closure_data;
    if (result == NULL)
    {
        ERROR ("GET: Error processing request.\n");
    }
    else if (result->value.len != 0)
    {
        data->length = result->value.len;
        data->value = malloc (data->length);
        memcpy (data->value, result->value.data, data->length);
    }
    data->done = true;
}

bool
apteryx_get (const char *path, unsigned char **value, size_t *size)
{
    ProtobufCService *rpc_client;
    Apteryx__Get get = APTERYX__GET__INIT;
    get_data_t data = {0};

    DEBUG ("GET: %s\n", path);

    /* Check path */
    if (path[0] != '/')
    {
        ERROR ("GET: invalid path (%s)!\n", path);
        assert(!debug || path[0] == '/');
        return false;
    }

    /* Start blank */
    *value = NULL;
    *size = 0;

    /* IPC */
    rpc_client = rpc_connect_service (APTERYX_SERVER, &apteryx__server__descriptor);
    if (!rpc_client)
    {
        ERROR ("GET: Falied to connect to server: %s\n", strerror (errno));
        return false;
    }
    get.path = (char *) path;
    apteryx__server__get (rpc_client, &get, handle_get_response, &data);
    protobuf_c_service_destroy (rpc_client);
    if (!data.done)
    {
        ERROR ("GET: No response\n");
        return false;
    }

    /* Result */
    *size = data.length;
    *value = data.value;

    DEBUG ("    = %s\n", bytes_to_string (*value, *size));
    return (*value != NULL);
}

int32_t
apteryx_get_int (const char *path, const char *key)
{
    char *full_path;
    size_t len;
    unsigned char *v = NULL;
    int value = -1;

    /* Create full path */
    if (key)
        len = asprintf (&full_path, "%s/%s", path, key);
    else
        len = asprintf (&full_path, "%s", path);
    if (len)
    {
        if (apteryx_get (full_path, &v, &len) && v)
        {
            value = atoi ((char *) v);
            free (v);
        }
        free (full_path);
    }
    return value;
}

char *
apteryx_get_string (const char *path, const char *key)
{
    char *full_path;
    size_t len;
    unsigned char *value = NULL;
    char *str = NULL;

    /* Create full path */
    if (key)
        len = asprintf (&full_path, "%s/%s", path, key);
    else
        len = asprintf (&full_path, "%s", path);
    if (len)
    {
        if (!apteryx_get ((const char *) full_path, &value, &len))
        {
            value = NULL;
        }
        str = (char *) value;
        free (full_path);
    }
    return str;
}

typedef struct _search_data_t
{
    GList *paths;
    bool done;
} search_data_t;

static void
handle_search_response (const Apteryx__SearchResult *result, void *closure_data)
{
    search_data_t *data = (search_data_t *)closure_data;
    int i;
    data->paths = NULL;
    if (result == NULL)
    {
        ERROR ("SEARCH: Error processing request.\n");
    }
    else if (result->paths == NULL)
    {
        DEBUG ("    = (null)\n");
    }
    else if (result->n_paths != 0)
    {
        for (i = 0; i < result->n_paths; i++)
        {
            DEBUG ("    = %s\n", result->paths[i]);
            data->paths = g_list_append (data->paths,
                              (gpointer) strdup (result->paths[i]));
        }
    }
    data->done = true;
}

GList *
apteryx_search (const char *path)
{
    ProtobufCService *rpc_client;
    Apteryx__Search search = APTERYX__SEARCH__INIT;
    search_data_t data = {0};

    DEBUG ("SEARCH: %s\n", path);

    /* Validate path */
    if (!path ||
        strcmp (path, "/") == 0 ||
        strcmp (path, "/*") == 0 ||
        strcmp (path, "*") == 0 ||
        strlen (path) == 0)
    {
        path = "";
    }
    else if (path[0] != '/' ||
             path[strlen (path) - 1] != '/' ||
             strstr (path, "//") != NULL)
    {
        ERROR ("SEARCH: invalid root (%s)!\n", path);
        assert(!debug || path[0] == '/');
        assert(!debug || path[strlen (path) - 1] == '/');
        assert(!debug || strstr (path, "//") == NULL);
        return NULL;
    }

    /* IPC */
    rpc_client = rpc_connect_service (APTERYX_SERVER, &apteryx__server__descriptor);
    if (!rpc_client)
    {
        ERROR ("SEARCH: Falied to connect to server: %s\n", strerror (errno));
        return false;
    }
    search.path = (char *) path;
    apteryx__server__search (rpc_client, &search, handle_search_response, &data);
    protobuf_c_service_destroy (rpc_client);
    if (!data.done)
    {
        ERROR ("SEARCH: No response\n");
        return NULL;
    }

    /* Result */
    return data.paths;
}

bool
apteryx_watch (const char *path, apteryx_watch_callback cb, void *priv)
{
    ProtobufCService *rpc_client;
    Apteryx__Watch watch = APTERYX__WATCH__INIT;
    char *empty_root = "/*";
    protobuf_c_boolean is_done = 0;

    DEBUG ("WATCH: %s %p %p\n", path, cb, priv);

    /* Check path */
    if (!path ||
        strcmp (path, "/") == 0 ||
        strcmp (path, "/*") == 0 || strcmp (path, "*") == 0 || strlen (path) == 0)
    {
        path = empty_root;
    }
    if (path[0] != '/')
    {
        ERROR ("WATCH: invalid path (%s)!\n", path);
        assert(!debug || path[0] == '/');
        return false;
    }

    /* IPC */
    rpc_client = rpc_connect_service (APTERYX_SERVER, &apteryx__server__descriptor);
    if (!rpc_client)
    {
        ERROR ("WATCH: Falied to connect to server: %s\n", strerror (errno));
        return false;
    }
    watch.path = (char *) path;
    watch.id = (uint64_t) getpid ();
    watch.cb = (uint64_t) (long) cb;
    watch.priv = (uint64_t) (long) priv;
    apteryx__server__watch (rpc_client, &watch, handle_ok_response, &is_done);
    protobuf_c_service_destroy (rpc_client);
    if (!is_done)
    {
        ERROR ("WATCH: No response\n");
        return false;
    }

    /* Start the listen thread if required */
    if (cb)
        start_client_thread ();

    /* Success */
    return true;
}

bool
apteryx_provide (const char *path, apteryx_provide_callback cb, void *priv)
{
    ProtobufCService *rpc_client;
    Apteryx__Provide provide = APTERYX__PROVIDE__INIT;
    protobuf_c_boolean is_done = 0;

    DEBUG ("PROVIDE: %s %p %p\n", path, cb, priv);

    /* Check path */
    if (path[0] != '/')
    {
        ERROR ("PROVIDE: invalid path (%s)!\n", path);
        assert(!debug || path[0] == '/');
        return false;
    }

    /* IPC */
    rpc_client = rpc_connect_service (APTERYX_SERVER, &apteryx__server__descriptor);
    if (!rpc_client)
    {
        ERROR ("PROVIDE: Falied to connect to server: %s\n", strerror (errno));
        return false;
    }
    provide.path = (char *) path;
    provide.id = (uint64_t) getpid ();
    provide.cb = (uint64_t) (long) cb;
    provide.priv = (uint64_t) (long) priv;
    apteryx__server__provide (rpc_client, &provide, handle_ok_response, &is_done);
    protobuf_c_service_destroy (rpc_client);
    if (!is_done)
    {
        ERROR ("PROVIDE: No response\n");
        return false;
    }

    /* Start the listen thread if required */
    if (cb)
        start_client_thread ();

    /* Success */
    return true;
}
