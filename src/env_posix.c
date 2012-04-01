/* env_posix.c */

/*    Copyright 2009-2011 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

/* Networking and other niceties for POSIX systems. */
#include "env.h"
#include "mongo.h"
#include <string.h>
#include <errno.h>
#include <sys/time.h>

#ifndef NI_MAXSERV
# define NI_MAXSERV 32
#endif

static void mongo_set_error( mongo *conn, int err, const char *str ) {
    int errstr_size, str_size;

    errstr_size = sizeof( conn->errstr ) - 1;
    conn->err = err;

    if( !str ) {
        str = strerror( errno );
    }
    str_size = strlen( str );
    errstr_size = str_size > errstr_size ? errstr_size : str_size;
    memcpy( conn->errstr, str, errstr_size );
    conn->errstr[errstr_size] = '\0';
}

int mongo_write_socket( mongo *conn, const void *buf, int len ) {
    const char *cbuf = buf;
#ifdef MONGO_OSX_
    int flags = 0;
#else
    int flags = MSG_NOSIGNAL;
#endif

    while ( len ) {
        int sent = send( conn->sock, cbuf, len, flags );
        if ( sent == -1 ) {
            if (errno == EPIPE)
                conn->connected = 0;
            mongo_set_error( conn, MONGO_IO_ERROR, NULL );
            return MONGO_ERROR;
        }
        cbuf += sent;
        len -= sent;
    }

    return MONGO_OK;
}

int mongo_read_socket( mongo *conn, void *buf, int len ) {
    char *cbuf = buf;
    while ( len ) {
        int sent = recv( conn->sock, cbuf, len, 0 );
        if ( sent == 0 || sent == -1 ) {
            conn->err = MONGO_IO_ERROR;
            mongo_set_error( conn, MONGO_IO_ERROR, NULL );
            return MONGO_ERROR;
        }
        cbuf += sent;
        len -= sent;
    }

    return MONGO_OK;
}

int mongo_set_socket_op_timeout( mongo *conn, int millis ) {
    struct timeval tv;
    tv.tv_sec = millis / 1000;
    tv.tv_usec = ( millis % 1000 ) * 1000;

    if ( setsockopt( conn->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof( tv ) ) == -1 ) {
        conn->err = MONGO_IO_ERROR;
        return MONGO_ERROR;
    }

    if ( setsockopt( conn->sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof( tv ) ) == -1 ) {
        conn->err = MONGO_IO_ERROR;
        return MONGO_ERROR;
    }

    return MONGO_OK;
}

int mongo_socket_connect( mongo *conn, const char *host, int port ) {
    char port_str[NI_MAXSERV];
    int status;

    struct addrinfo ai_hints;
    struct addrinfo *ai_list = NULL;
    struct addrinfo *ai_ptr = NULL;

    conn->sock = 0;
    conn->connected = 0;
    sprintf(port_str,"%d",port);

    bson_sprintf( port_str, "%d", port );

    memset( &ai_hints, 0, sizeof( ai_hints ) );
#ifdef AI_ADDRCONFIG
    ai_hints.ai_flags = AI_ADDRCONFIG;
#endif
    ai_hints.ai_family = AF_UNSPEC;
    ai_hints.ai_socktype = SOCK_STREAM;

    status = getaddrinfo( host, port_str, &ai_hints, &ai_list );
    if ( status != 0 ) {
        bson_errprintf( "getaddrinfo failed: %s", gai_strerror( status ) );
        conn->err = MONGO_CONN_ADDR_FAIL;
        return MONGO_ERROR;
    }

    for ( ai_ptr = ai_list; ai_ptr != NULL; ai_ptr = ai_ptr->ai_next ) {
        conn->sock = socket( ai_ptr->ai_family, ai_ptr->ai_socktype, ai_ptr->ai_protocol );
        if ( conn->sock < 0 ) {
            conn->sock = 0;
            continue;
        }

        status = connect( conn->sock, ai_ptr->ai_addr, ai_ptr->ai_addrlen );
        if ( status != 0 ) {
            mongo_close_socket( conn->sock );
            conn->sock = 0;
            continue;
        }

        if ( ai_ptr->ai_protocol == IPPROTO_TCP ) {
            int flag = 1;

            setsockopt( conn->sock, IPPROTO_TCP, TCP_NODELAY,
                        ( void * ) &flag, sizeof( flag ) );
            if ( conn->op_timeout_ms > 0 )
                mongo_set_socket_op_timeout( conn, conn->op_timeout_ms );
        }

        conn->connected = 1;
        break;
    }

    freeaddrinfo( ai_list );

    if ( ! conn->connected ) {
        conn->err = MONGO_CONN_FAIL;
        return MONGO_ERROR;
    }

    return MONGO_OK;
}
