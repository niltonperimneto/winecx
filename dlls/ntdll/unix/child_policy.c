/*
 * Whisky per-application child launch policy
 *
 * Copyright 2026 Nilton Perim Neto
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winnt.h"
#include "unix_private.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(module);

#define POLICY_MAX_BYTES (1024 * 1024)
#define POLICY_MAX_DEPTH 32

struct json_cursor
{
    const char *ptr;
    const char *end;
    unsigned int depth;
};

struct policy_match
{
    BOOL identity_match;
    BOOL game_mode;
    BOOL has_game_mode;
    char **keys;
    char **values;
    size_t env_count;
};

static void skip_space( struct json_cursor *cursor )
{
    while (cursor->ptr < cursor->end && isspace( (unsigned char)*cursor->ptr )) cursor->ptr++;
}

static BOOL consume( struct json_cursor *cursor, char value )
{
    skip_space( cursor );
    if (cursor->ptr == cursor->end || *cursor->ptr != value) return FALSE;
    cursor->ptr++;
    return TRUE;
}

static char *parse_string( struct json_cursor *cursor )
{
    const char *start;
    char *result, *out;

    skip_space( cursor );
    if (cursor->ptr == cursor->end || *cursor->ptr++ != '"') return NULL;
    start = cursor->ptr;
    result = malloc( cursor->end - start + 1 );
    if (!result) return NULL;
    out = result;

    while (cursor->ptr < cursor->end)
    {
        unsigned char ch = *cursor->ptr++;
        if (ch == '"')
        {
            *out = 0;
            return result;
        }
        if (ch < 0x20) break;
        if (ch != '\\')
        {
            *out++ = ch;
            continue;
        }
        if (cursor->ptr == cursor->end) break;
        ch = *cursor->ptr++;
        switch (ch)
        {
        case '"': case '\\': case '/': *out++ = ch; break;
        case 'b': *out++ = '\b'; break;
        case 'f': *out++ = '\f'; break;
        case 'n': *out++ = '\n'; break;
        case 'r': *out++ = '\r'; break;
        case 't': *out++ = '\t'; break;
        default: goto invalid;
        }
    }

invalid:
    free( result );
    return NULL;
}

static BOOL skip_value( struct json_cursor *cursor );

static BOOL skip_compound( struct json_cursor *cursor, char open, char close )
{
    if (cursor->depth++ >= POLICY_MAX_DEPTH || !consume( cursor, open )) return FALSE;
    skip_space( cursor );
    if (consume( cursor, close ))
    {
        cursor->depth--;
        return TRUE;
    }
    for (;;)
    {
        if (open == '{')
        {
            char *key = parse_string( cursor );
            if (!key || !consume( cursor, ':' ))
            {
                free( key );
                return FALSE;
            }
            free( key );
        }
        if (!skip_value( cursor )) return FALSE;
        if (consume( cursor, close ))
        {
            cursor->depth--;
            return TRUE;
        }
        if (!consume( cursor, ',' )) return FALSE;
    }
}

static BOOL skip_value( struct json_cursor *cursor )
{
    const char *start;
    char *string;

    skip_space( cursor );
    if (cursor->ptr == cursor->end) return FALSE;
    if (*cursor->ptr == '{') return skip_compound( cursor, '{', '}' );
    if (*cursor->ptr == '[') return skip_compound( cursor, '[', ']' );
    if (*cursor->ptr == '"')
    {
        string = parse_string( cursor );
        free( string );
        return !!string;
    }
    start = cursor->ptr;
    while (cursor->ptr < cursor->end && !strchr( " \t\r\n,]}" , *cursor->ptr )) cursor->ptr++;
    return cursor->ptr != start;
}

static BOOL parse_uint( struct json_cursor *cursor, unsigned int *value )
{
    uint64_t number = 0;
    const char *start;

    skip_space( cursor );
    start = cursor->ptr;
    while (cursor->ptr < cursor->end && isdigit( (unsigned char)*cursor->ptr ))
    {
        number = number * 10 + (*cursor->ptr++ - '0');
        if (number > UINT32_MAX) return FALSE;
    }
    if (cursor->ptr == start) return FALSE;
    *value = number;
    return TRUE;
}

static BOOL parse_bool( struct json_cursor *cursor, BOOL *value )
{
    skip_space( cursor );
    if (cursor->end - cursor->ptr >= 4 && !memcmp( cursor->ptr, "true", 4 ))
    {
        cursor->ptr += 4;
        *value = TRUE;
        return TRUE;
    }
    if (cursor->end - cursor->ptr >= 5 && !memcmp( cursor->ptr, "false", 5 ))
    {
        cursor->ptr += 5;
        *value = FALSE;
        return TRUE;
    }
    return FALSE;
}

static void normalize_identity( char *value )
{
    char *ptr;
    for (ptr = value; *ptr; ptr++)
    {
        if (*ptr == '\\') *ptr = '/';
        else *ptr = tolower( (unsigned char)*ptr );
    }
}

static const char *base_name( const char *path )
{
    const char *slash = strrchr( path, '/' );
    const char *backslash = strrchr( path, '\\' );
    if (!slash || (backslash && backslash > slash)) slash = backslash;
    return slash ? slash + 1 : path;
}

static BOOL string_array_matches( struct json_cursor *cursor, const char *image_path, BOOL basename_only )
{
    BOOL matched = FALSE;
    char *expected;

    if (!consume( cursor, '[' )) return FALSE;
    if (consume( cursor, ']' )) return FALSE;
    for (;;)
    {
        expected = parse_string( cursor );
        if (!expected) return FALSE;
        normalize_identity( expected );
        if (!strcmp( expected, basename_only ? base_name( image_path ) : image_path )) matched = TRUE;
        free( expected );
        if (consume( cursor, ']' )) return matched;
        if (!consume( cursor, ',' )) return FALSE;
    }
}

static BOOL environment_key_allowed( const char *key )
{
    static const char * const allowed[] =
    {
        "MTL_HUD_ENABLED", "D3DM_ENABLE_METALFX", "METAL_CAPTURE_ENABLED",
        "MTL_DEBUG_LAYER", "D3DM_VALIDATION", "D3DM_SUPPORT_DXR",
        "D3DM_FORCE_D3D11", "D3DM_MTL4", "DXVK_ASYNC", "DXVK_HUD",
        "WINEESYNC", "WINEMSYNC", "WINED3DMETAL", "WINEDXMT",
        "WHISKY_DISABLE_APP_NAP"
    };
    unsigned int i;
    for (i = 0; i < ARRAY_SIZE(allowed); i++) if (!strcmp( key, allowed[i] )) return TRUE;
    return FALSE;
}

static BOOL parse_environment( struct json_cursor *cursor, struct policy_match *match )
{
    char *key, *value;
    char **new_keys, **new_values;

    if (!consume( cursor, '{' )) return FALSE;
    if (consume( cursor, '}' )) return TRUE;
    for (;;)
    {
        key = parse_string( cursor );
        if (!key || !consume( cursor, ':' ) || !(value = parse_string( cursor )))
        {
            free( key );
            return FALSE;
        }
        if (environment_key_allowed( key ))
        {
            new_keys = realloc( match->keys, (match->env_count + 1) * sizeof(*match->keys) );
            if (!new_keys)
            {
                free( key );
                free( value );
                return FALSE;
            }
            match->keys = new_keys;
            new_values = realloc( match->values, (match->env_count + 1) * sizeof(*match->values) );
            if (!new_values)
            {
                free( key );
                free( value );
                return FALSE;
            }
            match->values = new_values;
            match->keys[match->env_count] = key;
            match->values[match->env_count++] = value;
        }
        else
        {
            free( key );
            free( value );
        }
        if (consume( cursor, '}' )) return TRUE;
        if (!consume( cursor, ',' )) return FALSE;
    }
}

static unsigned int steam_app_id_from_environment( const WCHAR *environment )
{
    static const WCHAR app_idW[] = {'S','t','e','a','m','A','p','p','I','d','='};
    static const WCHAR game_idW[] = {'S','t','e','a','m','G','a','m','e','I','d','='};
    const WCHAR *ptr;
    unsigned int value;

    for (ptr = environment; ptr && *ptr; ptr += wcslen( ptr ) + 1)
    {
        const WCHAR *number = NULL;
        if (!wcsnicmp( ptr, app_idW, ARRAY_SIZE(app_idW) )) number = ptr + ARRAY_SIZE(app_idW);
        else if (!wcsnicmp( ptr, game_idW, ARRAY_SIZE(game_idW) )) number = ptr + ARRAY_SIZE(game_idW);
        if (!number) continue;
        value = 0;
        while (*number >= '0' && *number <= '9') value = value * 10 + *number++ - '0';
        if (!*number) return value;
    }
    return 0;
}

static void free_match( struct policy_match *match )
{
    size_t i;
    for (i = 0; i < match->env_count; i++)
    {
        free( match->keys[i] );
        free( match->values[i] );
    }
    free( match->keys );
    free( match->values );
    memset( match, 0, sizeof(*match) );
}

static BOOL parse_policy( struct json_cursor *cursor, const char *image_path,
                          unsigned int steam_app_id, struct policy_match *match )
{
    char *key;
    unsigned int policy_app_id;

    if (!consume( cursor, '{' )) return FALSE;
    if (consume( cursor, '}' )) return TRUE;
    for (;;)
    {
        key = parse_string( cursor );
        if (!key || !consume( cursor, ':' ))
        {
            free( key );
            return FALSE;
        }
        if (!strcmp( key, "steamAppID" ) && parse_uint( cursor, &policy_app_id ))
        {
            if (steam_app_id && policy_app_id == steam_app_id) match->identity_match = TRUE;
        }
        else if (!strcmp( key, "executablePaths" ))
        {
            if (string_array_matches( cursor, image_path, FALSE )) match->identity_match = TRUE;
        }
        else if (!strcmp( key, "executableNames" ))
        {
            if (string_array_matches( cursor, image_path, TRUE )) match->identity_match = TRUE;
        }
        else if (!strcmp( key, "environment" ))
        {
            if (!parse_environment( cursor, match )) { free( key ); return FALSE; }
        }
        else if (!strcmp( key, "gameMode" ))
        {
            match->has_game_mode = parse_bool( cursor, &match->game_mode );
            if (!match->has_game_mode && !skip_value( cursor )) { free( key ); return FALSE; }
        }
        else if (!skip_value( cursor ))
        {
            free( key );
            return FALSE;
        }
        free( key );
        if (consume( cursor, '}' )) return TRUE;
        if (!consume( cursor, ',' )) return FALSE;
    }
}

static BOOL find_matching_policy( const char *data, size_t size, const char *image_path,
                                  const WCHAR *environment, struct policy_match *result )
{
    struct json_cursor cursor = {data, data + size, 0};
    struct policy_match candidate = {0};
    unsigned int schema = 0, steam_app_id = steam_app_id_from_environment( environment );
    char *key;

    if (!consume( &cursor, '{' )) return FALSE;
    while (!consume( &cursor, '}' ))
    {
        key = parse_string( &cursor );
        if (!key || !consume( &cursor, ':' )) { free( key ); return FALSE; }
        if (!strcmp( key, "schemaVersion" ))
        {
            if (!parse_uint( &cursor, &schema )) { free( key ); return FALSE; }
        }
        else if (!strcmp( key, "policies" ))
        {
            if (!consume( &cursor, '[' )) { free( key ); return FALSE; }
            while (!consume( &cursor, ']' ))
            {
                free_match( &candidate );
                if (!parse_policy( &cursor, image_path, steam_app_id, &candidate ))
                {
                    free( key );
                    free_match( &candidate );
                    return FALSE;
                }
                if (candidate.identity_match)
                {
                    if (result->identity_match)
                    {
                        WARN( "ambiguous Whisky child policy for %s; ignoring\n", debugstr_a(image_path) );
                        free( key );
                        free_match( &candidate );
                        free_match( result );
                        return FALSE;
                    }
                    *result = candidate;
                    memset( &candidate, 0, sizeof(candidate) );
                }
                if (consume( &cursor, ']' )) break;
                if (!consume( &cursor, ',' )) { free( key ); free_match( &candidate ); return FALSE; }
            }
        }
        else if (!skip_value( &cursor ))
        {
            free( key );
            free_match( &candidate );
            return FALSE;
        }
        free( key );
        if (consume( &cursor, '}' )) break;
        if (!consume( &cursor, ',' )) { free_match( &candidate ); return FALSE; }
    }
    free_match( &candidate );
    if (schema != 1)
    {
        free_match( result );
        return FALSE;
    }
    return result->identity_match;
}

static char *read_policy_file( const char *path, size_t *size )
{
    struct stat st;
    char *data;
    ssize_t count, total = 0;
    int fd;

    if ((fd = open( path, O_RDONLY | O_CLOEXEC
#ifdef O_NOFOLLOW
                    | O_NOFOLLOW
#endif
                    )) == -1) return NULL;
    if (fstat( fd, &st ) == -1 || !S_ISREG( st.st_mode ) || st.st_uid != getuid() ||
        (st.st_mode & 0022) || st.st_size <= 0 || st.st_size > POLICY_MAX_BYTES)
    {
        close( fd );
        return NULL;
    }
    if (!(data = malloc( st.st_size + 1 ))) { close( fd ); return NULL; }
    while (total < st.st_size)
    {
        count = read( fd, data + total, st.st_size - total );
        if (count > 0) total += count;
        else if (count == -1 && errno == EINTR) continue;
        else { free( data ); close( fd ); return NULL; }
    }
    close( fd );
    data[total] = 0;
    *size = total;
    return data;
}

void whisky_apply_child_launch_policy( const char *image_path, const WCHAR *environment )
{
    struct policy_match match = {0};
    const char *path = getenv( "WHISKY_CHILD_LAUNCH_POLICY_PATH" );
    char *data, *normalized_image;
    size_t size, i;

    if (!path || !*path || !image_path || !*image_path) return;
    if (!(data = read_policy_file( path, &size ))) return;
    if (!(normalized_image = strdup( image_path ))) { free( data ); return; }
    normalize_identity( normalized_image );

    if (find_matching_policy( data, size, normalized_image, environment, &match ))
    {
        for (i = 0; i < match.env_count; i++) setenv( match.keys[i], match.values[i], 1 );
        TRACE( "applied Whisky child policy to %s%s\n", debugstr_a(image_path),
               match.has_game_mode && match.game_mode ? " (Game Mode requested)" : "" );
    }

    free_match( &match );
    free( normalized_image );
    free( data );
}
