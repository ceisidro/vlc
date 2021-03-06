/*****************************************************************************
 * net.c: Network related functions
 *****************************************************************************
 * Copyright (C) 2007-2008 the VideoLAN team
 * $Id$
 *
 * Authors: Antoine Cellerier <dionoea at videolan tod org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <assert.h>
#include <errno.h>
#ifdef _WIN32
#include <io.h>
#endif
#ifdef HAVE_POLL
#include <poll.h>       /* poll structures and defines */
#endif
#include <sys/stat.h>

#include <vlc_common.h>
#include <vlc_network.h>
#include <vlc_url.h>
#include <vlc_fs.h>
#include <vlc_interface.h>

#include "../vlc.h"
#include "../libs.h"

void vlclua_fd_init( intf_sys_t *sys )
{
    sys->fdv = NULL;
    sys->fdc = 0;
}

/** Releases all (leaked) VLC Lua file descriptors. */
void vlclua_fd_destroy( intf_sys_t *sys )
{
    for( unsigned i = 0; i < sys->fdc; i++ )
         net_Close( sys->fdv[i] );
    free( sys->fdv );
}

/** Maps an OS file descriptor to a VLC Lua file descriptor */
static int vlclua_fd_map( lua_State *L, int fd )
{
    intf_thread_t *intf = (intf_thread_t *)vlclua_get_this( L );
    intf_sys_t *sys = intf->p_sys;

    if( (unsigned)fd < 3u )
        return -1;

#ifndef NDEBUG
    for( unsigned i = 0; i < sys->fdc; i++ )
        assert( sys->fdv[i] != fd );
#endif

    if( sys->fdc >= 64 )
        return -1;

    int *fdv = realloc( sys->fdv, (sys->fdc + 1) * sizeof (sys->fdv[0]) );
    if( unlikely(fdv == NULL) )
        return -1;

    sys->fdv = fdv;
    sys->fdv[sys->fdc] = fd;
    fd = 3 + sys->fdc;
    sys->fdc++;
    return fd;
}

static int vlclua_fd_map_safe( lua_State *L, int fd )
{
    int luafd = vlclua_fd_map( L, fd );
    if( luafd == -1 )
        net_Close( fd );
    return luafd;
}

/** Gets the OS file descriptor mapped to a VLC Lua file descriptor */
static int vlclua_fd_get( lua_State *L, unsigned idx )
{
    intf_thread_t *intf = (intf_thread_t *)vlclua_get_this( L );
    intf_sys_t *sys = intf->p_sys;

    if( idx < 3u )
        return idx;
    idx -= 3;
    return (idx < sys->fdc) ? sys->fdv[idx] : -1;
}

/** Gets the VLC Lua file descriptor mapped from an OS file descriptor */
static int vlclua_fd_get_lua( lua_State *L, int fd )
{
    intf_thread_t *intf = (intf_thread_t *)vlclua_get_this( L );
    intf_sys_t *sys = intf->p_sys;

    if( (unsigned)fd < 3u )
        return fd;
    for( unsigned i = 0; i < sys->fdc; i++ )
        if( sys->fdv[i] == fd )
            return 3 + i;
    return -1;
}

/** Unmaps an OS file descriptor from VLC Lua */
static void vlclua_fd_unmap( lua_State *L, unsigned idx )
{
    intf_thread_t *intf = (intf_thread_t *)vlclua_get_this( L );
    intf_sys_t *sys = intf->p_sys;
    int fd = -1;

    if( idx < 3u )
        return; /* Never close stdin/stdout/stderr. */

    idx -= 3;
    if( idx >= sys->fdc )
        return;

    fd = sys->fdv[idx];
    sys->fdc--;
    memmove( sys->fdv + idx, sys->fdv + idx + 1,
             (sys->fdc - idx) * sizeof (sys->fdv[0]) );
    /* realloc() not really needed */
#ifndef NDEBUG
    for( unsigned i = 0; i < sys->fdc; i++ )
        assert( sys->fdv[i] != fd );
#endif
}

static void vlclua_fd_unmap_safe( lua_State *L, unsigned idx )
{
    int fd = vlclua_fd_get( L, idx );

    vlclua_fd_unmap( L, idx );
    if( fd != -1 )
        net_Close( fd );
}

/*****************************************************************************
 *
 *****************************************************************************/
static int vlclua_url_parse( lua_State *L )
{
    const char *psz_url = luaL_checkstring( L, 1 );
    const char *psz_option = luaL_optstring( L, 2, NULL );
    vlc_url_t url;

    vlc_UrlParse( &url, psz_url, psz_option?*psz_option:0 );

    lua_newtable( L );
    lua_pushstring( L, url.psz_protocol );
    lua_setfield( L, -2, "protocol" );
    lua_pushstring( L, url.psz_username );
    lua_setfield( L, -2, "username" );
    lua_pushstring( L, url.psz_password );
    lua_setfield( L, -2, "password" );
    lua_pushstring( L, url.psz_host );
    lua_setfield( L, -2, "host" );
    lua_pushinteger( L, url.i_port );
    lua_setfield( L, -2, "port" );
    lua_pushstring( L, url.psz_path );
    lua_setfield( L, -2, "path" );
    lua_pushstring( L, url.psz_option );
    lua_setfield( L, -2, "option" );

    vlc_UrlClean( &url );

    return 1;
}

/*****************************************************************************
 * Net listen
 *****************************************************************************/
static int vlclua_net_listen_close( lua_State * );
static int vlclua_net_accept( lua_State * );
static int vlclua_net_fds( lua_State * );

static const luaL_Reg vlclua_net_listen_reg[] = {
    { "accept", vlclua_net_accept },
    { "fds", vlclua_net_fds },
    { NULL, NULL }
};

static int vlclua_net_listen_tcp( lua_State *L )
{
    vlc_object_t *p_this = vlclua_get_this( L );
    const char *psz_host = luaL_checkstring( L, 1 );
    int i_port = luaL_checkint( L, 2 );
    int *pi_fd = net_ListenTCP( p_this, psz_host, i_port );
    if( pi_fd == NULL )
        return luaL_error( L, "Cannot listen on %s:%d", psz_host, i_port );

    for( unsigned i = 0; pi_fd[i] != -1; i++ )
        if( vlclua_fd_map( L, pi_fd[i] ) == -1 )
        {
            while( i > 0 )
                vlclua_fd_unmap( L, vlclua_fd_get_lua( L, pi_fd[--i] ) );

            net_ListenClose( pi_fd );
            return luaL_error( L, "Cannot listen on %s:%d", psz_host, i_port );
        }

    int **ppi_fd = lua_newuserdata( L, sizeof( int * ) );
    *ppi_fd = pi_fd;

    if( luaL_newmetatable( L, "net_listen" ) )
    {
        lua_newtable( L );
        luaL_register( L, NULL, vlclua_net_listen_reg );
        lua_setfield( L, -2, "__index" );
        lua_pushcfunction( L, vlclua_net_listen_close );
        lua_setfield( L, -2, "__gc" );
    }

    lua_setmetatable( L, -2 );
    return 1;
}

static int vlclua_net_listen_close( lua_State *L )
{
    int **ppi_fd = (int**)luaL_checkudata( L, 1, "net_listen" );
    int *pi_fd = *ppi_fd;

    for( unsigned i = 0; pi_fd[i] != -1; i++ )
        vlclua_fd_unmap( L, vlclua_fd_get_lua( L, pi_fd[i] ) );

    net_ListenClose( pi_fd );
    return 0;
}

static int vlclua_net_fds( lua_State *L )
{
    int **ppi_fd = (int**)luaL_checkudata( L, 1, "net_listen" );
    int *pi_fd = *ppi_fd;

    int i_count = 0;
    while( pi_fd[i_count] != -1 )
        lua_pushinteger( L, vlclua_fd_get_lua( L, pi_fd[i_count++] ) );

    return i_count;
}

static int vlclua_net_accept( lua_State *L )
{
    vlc_object_t *p_this = vlclua_get_this( L );
    int **ppi_fd = (int**)luaL_checkudata( L, 1, "net_listen" );
    int i_fd = net_Accept( p_this, *ppi_fd );

    lua_pushinteger( L, vlclua_fd_map_safe( L, i_fd ) );
    return 1;
}

/*****************************************************************************
 *
 *****************************************************************************/
static int vlclua_net_connect_tcp( lua_State *L )
{
    vlc_object_t *p_this = vlclua_get_this( L );
    const char *psz_host = luaL_checkstring( L, 1 );
    int i_port = luaL_checkint( L, 2 );
    int i_fd = net_Connect( p_this, psz_host, i_port, SOCK_STREAM, IPPROTO_TCP );
    lua_pushinteger( L, vlclua_fd_map_safe( L, i_fd ) );
    return 1;
}

static int vlclua_net_close( lua_State *L )
{
    int i_fd = luaL_checkint( L, 1 );
    vlclua_fd_unmap_safe( L, i_fd );
    return 0;
}

static int vlclua_net_send( lua_State *L )
{
    int i_fd = luaL_checkint( L, 1 );
    size_t i_len;
    const char *psz_buffer = luaL_checklstring( L, 2, &i_len );
    i_len = luaL_optint( L, 3, i_len );
    i_len = send( vlclua_fd_get( L, i_fd ), psz_buffer, i_len, 0 );
    lua_pushinteger( L, i_len );
    return 1;
}

static int vlclua_net_recv( lua_State *L )
{
    int i_fd = luaL_checkint( L, 1 );
    size_t i_len = luaL_optint( L, 2, 1 );
    char psz_buffer[i_len];
    ssize_t i_ret = recv( vlclua_fd_get( L, i_fd ), psz_buffer, i_len, 0 );
    if( i_ret > 0 )
        lua_pushlstring( L, psz_buffer, i_ret );
    else
        lua_pushnil( L );
    return 1;
}

#ifndef _WIN32
/*****************************************************************************
 *
 *****************************************************************************/
/* Takes a { fd : events } table as first arg and modifies it to { fd : revents } */
static int vlclua_net_poll( lua_State *L )
{
    intf_thread_t *intf = (intf_thread_t *)vlclua_get_this( L );
    intf_sys_t *sys = intf->p_sys;

    luaL_checktype( L, 1, LUA_TTABLE );

    int i_fds = 1;
    lua_pushnil( L );
    while( lua_next( L, 1 ) )
    {
        i_fds++;
        lua_pop( L, 1 );
    }

    struct pollfd *p_fds = xmalloc( i_fds * sizeof( *p_fds ) );
    int *luafds = xmalloc( i_fds * sizeof( *luafds ) );

    lua_pushnil( L );
    int i = 1;
    p_fds[0].fd = sys->fd[0];
    p_fds[0].events = POLLIN;
    while( lua_next( L, 1 ) )
    {
        luafds[i] = luaL_checkinteger( L, -2 );
        p_fds[i].fd = vlclua_fd_get( L, luafds[i] );
        p_fds[i].events = luaL_checkinteger( L, -1 );
        p_fds[i].events &= POLLIN | POLLOUT | POLLPRI;
        lua_pop( L, 1 );
        i++;
    }

    int i_ret;
    do
        i_ret = poll( p_fds, i_fds, -1 );
    while( i_ret == -1 && errno == EINTR );

    for( i = 1; i < i_fds; i++ )
    {
        lua_pushinteger( L, luafds[i] );
        lua_pushinteger( L, p_fds[i].revents );
        lua_settable( L, 1 );
    }
    lua_pushinteger( L, i_ret );

    if( p_fds[0].revents )
        i_ret = luaL_error( L, "Interrupted." );
    else
        i_ret = 1;
    free( luafds );
    free( p_fds );

    return i_ret;
}

/*****************************************************************************
 *
 *****************************************************************************/
/*
static int vlclua_fd_open( lua_State *L )
{
}
*/

static int vlclua_fd_write( lua_State *L )
{
    int i_fd = luaL_checkint( L, 1 );
    size_t i_len;
    ssize_t i_ret;
    const char *psz_buffer = luaL_checklstring( L, 2, &i_len );
    i_len = luaL_optint( L, 3, i_len );
    i_ret = write( vlclua_fd_get( L, i_fd ), psz_buffer, i_len );
    lua_pushinteger( L, i_ret );
    return 1;
}

static int vlclua_fd_read( lua_State *L )
{
    int i_fd = luaL_checkint( L, 1 );
    size_t i_len = luaL_optint( L, 2, 1 );
    char psz_buffer[i_len];
    ssize_t i_ret = read( vlclua_fd_get( L, i_fd ), psz_buffer, i_len );
    if( i_ret > 0 )
        lua_pushlstring( L, psz_buffer, i_ret );
    else
        lua_pushnil( L );
    return 1;
}
#endif

/*****************************************************************************
 *
 *****************************************************************************/
static int vlclua_stat( lua_State *L )
{
    const char *psz_path = luaL_checkstring( L, 1 );
    struct stat s;
    if( vlc_stat( psz_path, &s ) )
        return 0;
        //return luaL_error( L, "Couldn't stat %s.", psz_path );
    lua_newtable( L );
    if( S_ISREG( s.st_mode ) )
        lua_pushliteral( L, "file" );
    else if( S_ISDIR( s.st_mode ) )
        lua_pushliteral( L, "dir" );
#ifdef S_ISCHR
    else if( S_ISCHR( s.st_mode ) )
        lua_pushliteral( L, "character device" );
#endif
#ifdef S_ISBLK
    else if( S_ISBLK( s.st_mode ) )
        lua_pushliteral( L, "block device" );
#endif
#ifdef S_ISFIFO
    else if( S_ISFIFO( s.st_mode ) )
        lua_pushliteral( L, "fifo" );
#endif
#ifdef S_ISLNK
    else if( S_ISLNK( s.st_mode ) )
        lua_pushliteral( L, "symbolic link" );
#endif
#ifdef S_ISSOCK
    else if( S_ISSOCK( s.st_mode ) )
        lua_pushliteral( L, "socket" );
#endif
    else
        lua_pushliteral( L, "unknown" );
    lua_setfield( L, -2, "type" );
    lua_pushinteger( L, s.st_mode );
    lua_setfield( L, -2, "mode" );
    lua_pushinteger( L, s.st_uid );
    lua_setfield( L, -2, "uid" );
    lua_pushinteger( L, s.st_gid );
    lua_setfield( L, -2, "gid" );
    lua_pushinteger( L, s.st_size );
    lua_setfield( L, -2, "size" );
    lua_pushinteger( L, s.st_atime );
    lua_setfield( L, -2, "access_time" );
    lua_pushinteger( L, s.st_mtime );
    lua_setfield( L, -2, "modification_time" );
    lua_pushinteger( L, s.st_ctime );
    lua_setfield( L, -2, "creation_time" );
    return 1;
}

static int vlclua_opendir( lua_State *L )
{
    const char *psz_dir = luaL_checkstring( L, 1 );
    DIR *p_dir;
    int i = 0;

    if( ( p_dir = vlc_opendir( psz_dir ) ) == NULL )
        return luaL_error( L, "cannot open directory `%s'.", psz_dir );

    lua_newtable( L );
    for( ;; )
    {
        char *psz_filename = vlc_readdir( p_dir );
        if( !psz_filename ) break;
        i++;
        lua_pushstring( L, psz_filename );
        lua_rawseti( L, -2, i );
        free( psz_filename );
    }
    closedir( p_dir );
    return 1;
}

/*****************************************************************************
 *
 *****************************************************************************/
static const luaL_Reg vlclua_net_intf_reg[] = {
    { "listen_tcp", vlclua_net_listen_tcp },
    { "connect_tcp", vlclua_net_connect_tcp },
    { "close", vlclua_net_close },
    { "send", vlclua_net_send },
    { "recv", vlclua_net_recv },
#ifndef _WIN32
    { "poll", vlclua_net_poll },
    { "read", vlclua_fd_read },
    { "write", vlclua_fd_write },
#endif
    /* The following functions do not depend on intf_thread_t and do not really
     * belong in net.* but are left here for backward compatibility: */
    { "url_parse", vlclua_url_parse },
    { "stat", vlclua_stat }, /* Not really "net" */
    { "opendir", vlclua_opendir }, /* Not really "net" */
    { NULL, NULL }
};

void luaopen_net_intf( lua_State *L )
{
    lua_newtable( L );
    luaL_register( L, NULL, vlclua_net_intf_reg );
#define ADD_CONSTANT( value )    \
    lua_pushinteger( L, POLL##value ); \
    lua_setfield( L, -2, "POLL"#value );
    ADD_CONSTANT( IN )
    ADD_CONSTANT( PRI )
    ADD_CONSTANT( OUT )
    ADD_CONSTANT( ERR )
    ADD_CONSTANT( HUP )
    ADD_CONSTANT( NVAL )
    lua_setfield( L, -2, "net" );
}

static const luaL_Reg vlclua_net_generic_reg[] = {
    { "url_parse", vlclua_url_parse },
    { "stat", vlclua_stat }, /* Not really "net" */
    { "opendir", vlclua_opendir }, /* Not really "net" */
    { NULL, NULL }
};

void luaopen_net_generic( lua_State *L )
{
    lua_newtable( L );
    luaL_register( L, NULL, vlclua_net_generic_reg );
    lua_setfield( L, -2, "net" );
}
