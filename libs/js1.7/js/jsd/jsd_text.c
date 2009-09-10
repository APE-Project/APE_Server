/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is mozilla.org code.
 *
 * The Initial Developer of the Original Code is
 * Netscape Communications Corporation.
 * Portions created by the Initial Developer are Copyright (C) 1998
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

/*
 * JavaScript Debugging support - Source Text functions
 */

#include <ctype.h>
#include "jsd.h"

#ifdef DEBUG
void JSD_ASSERT_VALID_SOURCE_TEXT(JSDSourceText* jsdsrc)
{
    JS_ASSERT(jsdsrc);
    JS_ASSERT(jsdsrc->url);
}
#endif

/***************************************************************************/
/* XXX add notification */

static void
_clearText(JSDContext* jsdc, JSDSourceText* jsdsrc)
{
    if( jsdsrc->text )
        free(jsdsrc->text);
    jsdsrc->text        = NULL;
    jsdsrc->textLength  = 0;
    jsdsrc->textSpace   = 0;
    jsdsrc->status      = JSD_SOURCE_CLEARED;
    jsdsrc->dirty       = JS_TRUE;
    jsdsrc->alterCount  = jsdc->sourceAlterCount++ ;
    jsdsrc->doingEval   = JS_FALSE;
}    

static JSBool
_appendText(JSDContext* jsdc, JSDSourceText* jsdsrc, 
            const char* text, size_t length)
{
#define MEMBUF_GROW 1000

    uintN neededSize = jsdsrc->textLength + length;

    if( neededSize > jsdsrc->textSpace )
    {
        char* newBuf;
        uintN iNewSize;

        /* if this is the first alloc, the req might be all that's needed*/
        if( ! jsdsrc->textSpace )
            iNewSize = length;
        else
            iNewSize = (neededSize * 5 / 4) + MEMBUF_GROW;

        newBuf = (char*) realloc(jsdsrc->text, iNewSize);
        if( ! newBuf )
        {
            /* try again with the minimal size really asked for */
            iNewSize = neededSize;
            newBuf = (char*) realloc(jsdsrc->text, iNewSize);
            if( ! newBuf )
            {
                /* out of memory */
                _clearText( jsdc, jsdsrc );
                jsdsrc->status = JSD_SOURCE_FAILED;
                return JS_FALSE;
            }
        }

        jsdsrc->text = newBuf;
        jsdsrc->textSpace = iNewSize;
    }

    memcpy(jsdsrc->text + jsdsrc->textLength, text, length);
    jsdsrc->textLength += length;
    return JS_TRUE;
}

static JSDSourceText*
_newSource(JSDContext* jsdc, const char* url)
{
    JSDSourceText* jsdsrc = (JSDSourceText*)calloc(1,sizeof(JSDSourceText));
    if( ! jsdsrc )
        return NULL;
    
    jsdsrc->url        = (char*) url; /* already a copy */
    jsdsrc->status     = JSD_SOURCE_INITED;
    jsdsrc->dirty      = JS_TRUE;
    jsdsrc->alterCount = jsdc->sourceAlterCount++ ;
            
    return jsdsrc;
}

static void
_destroySource(JSDContext* jsdc, JSDSourceText* jsdsrc)
{
    JS_ASSERT(NULL == jsdsrc->text);  /* must _clearText() first */
    free(jsdsrc->url);
    free(jsdsrc);
}

static void
_removeSource(JSDContext* jsdc, JSDSourceText* jsdsrc)
{
    JS_REMOVE_LINK(&jsdsrc->links);
    _clearText(jsdc, jsdsrc);
    _destroySource(jsdc, jsdsrc);
}

static JSDSourceText*
_addSource(JSDContext* jsdc, const char* url)
{
    JSDSourceText* jsdsrc = _newSource(jsdc, url);
    if( ! jsdsrc )
        return NULL;
    JS_INSERT_LINK(&jsdsrc->links, &jsdc->sources);
    return jsdsrc;
}

static void
_moveSourceToFront(JSDContext* jsdc, JSDSourceText* jsdsrc)
{
    JS_REMOVE_LINK(&jsdsrc->links);
    JS_INSERT_LINK(&jsdsrc->links, &jsdc->sources);
}

static void
_moveSourceToRemovedList(JSDContext* jsdc, JSDSourceText* jsdsrc)
{
    _clearText(jsdc, jsdsrc);
    JS_REMOVE_LINK(&jsdsrc->links);
    JS_INSERT_LINK(&jsdsrc->links, &jsdc->removedSources);
}

static void
_removeSourceFromRemovedList( JSDContext* jsdc, JSDSourceText* jsdsrc )
{
    JS_REMOVE_LINK(&jsdsrc->links);
    _destroySource( jsdc, jsdsrc );
}

static JSBool
_isSourceInSourceList(JSDContext* jsdc, JSDSourceText* jsdsrcToFind)
{
    JSDSourceText *jsdsrc;

    for( jsdsrc = (JSDSourceText*)jsdc->sources.next;
         jsdsrc != (JSDSourceText*)&jsdc->sources;
         jsdsrc = (JSDSourceText*)jsdsrc->links.next ) 
    {
        if( jsdsrc == jsdsrcToFind )
            return JS_TRUE;
    }
    return JS_FALSE;
}

/*  compare strings in a case insensitive manner with a length limit
*/

static int 
strncasecomp (const char* one, const char * two, int n)
{
    const char *pA;
    const char *pB;
    
    for(pA=one, pB=two;; pA++, pB++) 
    {
        int tmp;
        if (pA == one+n) 
            return 0;   
        if (!(*pA && *pB)) 
            return *pA - *pB;
        tmp = tolower(*pA) - tolower(*pB);
        if (tmp) 
            return tmp;
    }
}

static char file_url_prefix[]    = "file:";
#define FILE_URL_PREFIX_LEN     (sizeof file_url_prefix - 1)

const char*
jsd_BuildNormalizedURL( const char* url_string )
{
    char *new_url_string;

    if( ! url_string )
        return NULL;

    if (!strncasecomp(url_string, file_url_prefix, FILE_URL_PREFIX_LEN) &&
        url_string[FILE_URL_PREFIX_LEN + 0] == '/' &&
        url_string[FILE_URL_PREFIX_LEN + 1] == '/') {
        new_url_string = JS_smprintf("%s%s",
                                     file_url_prefix,
                                     url_string + FILE_URL_PREFIX_LEN + 2);
    } else {
        new_url_string = strdup(url_string);
    }
    return new_url_string;
}

/***************************************************************************/

void
jsd_DestroyAllSources( JSDContext* jsdc )
{
    JSDSourceText *jsdsrc;
    JSDSourceText *next;

    for( jsdsrc = (JSDSourceText*)jsdc->sources.next;
         jsdsrc != (JSDSourceText*)&jsdc->sources;
         jsdsrc = next ) 
    {
        next = (JSDSourceText*)jsdsrc->links.next;
        _removeSource( jsdc, jsdsrc );
    }

    for( jsdsrc = (JSDSourceText*)jsdc->removedSources.next;
         jsdsrc != (JSDSourceText*)&jsdc->removedSources;
         jsdsrc = next ) 
    {
        next = (JSDSourceText*)jsdsrc->links.next;
        _removeSourceFromRemovedList( jsdc, jsdsrc );
    }

}

JSDSourceText*
jsd_IterateSources(JSDContext* jsdc, JSDSourceText **iterp)
{
    JSDSourceText *jsdsrc = *iterp;
    
    if( !jsdsrc )
        jsdsrc = (JSDSourceText *)jsdc->sources.next;
    if( jsdsrc == (JSDSourceText *)&jsdc->sources )
        return NULL;
    *iterp = (JSDSourceText *)jsdsrc->links.next;
    return jsdsrc;
}

JSDSourceText*
jsd_FindSourceForURL(JSDContext* jsdc, const char* url)
{
    JSDSourceText *jsdsrc;

    for( jsdsrc = (JSDSourceText *)jsdc->sources.next;
         jsdsrc != (JSDSourceText *)&jsdc->sources;
         jsdsrc = (JSDSourceText *)jsdsrc->links.next )
    {
        if( 0 == strcmp(jsdsrc->url, url) )
            return jsdsrc;
    }
    return NULL;
}

const char*
jsd_GetSourceURL(JSDContext* jsdc, JSDSourceText* jsdsrc)
{
    return jsdsrc->url;
}

JSBool
jsd_GetSourceText(JSDContext* jsdc, JSDSourceText* jsdsrc,
                  const char** ppBuf, intN* pLen )
{
    *ppBuf = jsdsrc->text;
    *pLen  = jsdsrc->textLength;
    return JS_TRUE;
}

void
jsd_ClearSourceText(JSDContext* jsdc, JSDSourceText* jsdsrc)
{
    if( JSD_SOURCE_INITED  != jsdsrc->status &&
        JSD_SOURCE_PARTIAL != jsdsrc->status )
    {
        _clearText(jsdc, jsdsrc);
    }
}

JSDSourceStatus
jsd_GetSourceStatus(JSDContext* jsdc, JSDSourceText* jsdsrc)
{
    return jsdsrc->status;
}

JSBool
jsd_IsSourceDirty(JSDContext* jsdc, JSDSourceText* jsdsrc)
{
    return jsdsrc->dirty;
}

void
jsd_SetSourceDirty(JSDContext* jsdc, JSDSourceText* jsdsrc, JSBool dirty)
{
    jsdsrc->dirty = dirty;
}

uintN
jsd_GetSourceAlterCount(JSDContext* jsdc, JSDSourceText* jsdsrc)
{
    return jsdsrc->alterCount;
}

uintN
jsd_IncrementSourceAlterCount(JSDContext* jsdc, JSDSourceText* jsdsrc)
{
    return jsdsrc->alterCount = jsdc->sourceAlterCount++;
}

/***************************************************************************/

#if defined(DEBUG) && 0
void DEBUG_ITERATE_SOURCES( JSDContext* jsdc )
{
    JSDSourceText* iterp = NULL;
    JSDSourceText* jsdsrc = NULL;
    int dummy;
    
    while( NULL != (jsdsrc = jsd_IterateSources(jsdc, &iterp)) )
    {
        const char*     url;
        const char*     text;
        int             len;
        JSBool          dirty;
        JSDStreamStatus status;
        JSBool          gotSrc;

        url     = JSD_GetSourceURL(jsdc, jsdsrc);
        dirty   = JSD_IsSourceDirty(jsdc, jsdsrc);
        status  = JSD_GetSourceStatus(jsdc, jsdsrc);
        gotSrc  = JSD_GetSourceText(jsdc, jsdsrc, &text, &len );
        
        dummy = 0;  /* gives us a line to set breakpoint... */
    }
}
#else
#define DEBUG_ITERATE_SOURCES(x) ((void)x)
#endif

/***************************************************************************/

JSDSourceText*
jsd_NewSourceText(JSDContext* jsdc, const char* url)
{
    JSDSourceText* jsdsrc;
    const char* new_url_string;

    JSD_LOCK_SOURCE_TEXT(jsdc);

#ifdef LIVEWIRE
    new_url_string = url; /* we take ownership of alloc'd string */
#else
    new_url_string = jsd_BuildNormalizedURL(url);
#endif
    if( ! new_url_string )
        return NULL;

    jsdsrc = jsd_FindSourceForURL(jsdc, new_url_string);

    if( jsdsrc )
    {
        if( jsdsrc->doingEval )
        {
#ifdef LIVEWIRE
            free((char*)new_url_string);
#endif
            JSD_UNLOCK_SOURCE_TEXT(jsdc);
            return NULL;
        }
        else    
            _moveSourceToRemovedList(jsdc, jsdsrc);
    }

    jsdsrc = _addSource( jsdc, new_url_string );

    JSD_UNLOCK_SOURCE_TEXT(jsdc);

    return jsdsrc;
}

JSDSourceText*
jsd_AppendSourceText(JSDContext* jsdc, 
                     JSDSourceText* jsdsrc,
                     const char* text,       /* *not* zero terminated */
                     size_t length,
                     JSDSourceStatus status)
{
    JSD_LOCK_SOURCE_TEXT(jsdc);

    if( jsdsrc->doingEval )
    {
        JSD_UNLOCK_SOURCE_TEXT(jsdc);
        return NULL;
    }

    if( ! _isSourceInSourceList( jsdc, jsdsrc ) )
    {
        _removeSourceFromRemovedList( jsdc, jsdsrc );
        JSD_UNLOCK_SOURCE_TEXT(jsdc);
        return NULL;
    }

    if( text && length && ! _appendText( jsdc, jsdsrc, text, length ) )
    {
        jsdsrc->dirty  = JS_TRUE;
        jsdsrc->alterCount  = jsdc->sourceAlterCount++ ;
        jsdsrc->status = JSD_SOURCE_FAILED;
        _moveSourceToRemovedList(jsdc, jsdsrc);
        JSD_UNLOCK_SOURCE_TEXT(jsdc);
        return NULL;    
    }

    jsdsrc->dirty  = JS_TRUE;
    jsdsrc->alterCount  = jsdc->sourceAlterCount++ ;
    jsdsrc->status = status;
    DEBUG_ITERATE_SOURCES(jsdc);
    JSD_UNLOCK_SOURCE_TEXT(jsdc);
    return jsdsrc;
}

JSDSourceText*
jsd_AppendUCSourceText(JSDContext* jsdc,
                       JSDSourceText* jsdsrc,
                       const jschar* text,       /* *not* zero terminated */
                       size_t length,
                       JSDSourceStatus status)
{
#define UNICODE_TRUNCATE_BUF_SIZE 1024
    static char* buf = NULL;
    int remaining = length;

    if(!text || !length)
        return jsd_AppendSourceText(jsdc, jsdsrc, NULL, 0, status);

    JSD_LOCK_SOURCE_TEXT(jsdc);
    if(!buf)
    {
        buf = malloc(UNICODE_TRUNCATE_BUF_SIZE);
        if(!buf)
        {
            JSD_UNLOCK_SOURCE_TEXT(jsdc);
            return NULL;
        }
    }
    while(remaining && jsdsrc) {
        int bytes = JS_MIN(remaining, UNICODE_TRUNCATE_BUF_SIZE);
        int i;
        for(i = 0; i < bytes; i++)
            buf[i] = (const char) *(text++);
        jsdsrc = jsd_AppendSourceText(jsdc,jsdsrc,
                                      buf, bytes,
                                      JSD_SOURCE_PARTIAL);
        remaining -= bytes;
    }
    if(jsdsrc && status != JSD_SOURCE_PARTIAL)
        jsdsrc = jsd_AppendSourceText(jsdc, jsdsrc, NULL, 0, status);

    JSD_UNLOCK_SOURCE_TEXT(jsdc);
    return jsdsrc;
}

/* convienence function for adding complete source of url in one call */
JSBool
jsd_AddFullSourceText(JSDContext* jsdc, 
                      const char* text,       /* *not* zero terminated */
                      size_t      length,
                      const char* url)
{
    JSDSourceText* jsdsrc;

    JSD_LOCK_SOURCE_TEXT(jsdc);

    jsdsrc = jsd_NewSourceText(jsdc, url);
    if( jsdsrc )
        jsdsrc = jsd_AppendSourceText(jsdc, jsdsrc,
                                      text, length, JSD_SOURCE_PARTIAL );
    if( jsdsrc )
        jsdsrc = jsd_AppendSourceText(jsdc, jsdsrc,
                                      NULL, 0, JSD_SOURCE_COMPLETED );

    JSD_UNLOCK_SOURCE_TEXT(jsdc);

    return jsdsrc ? JS_TRUE : JS_FALSE;
}

/***************************************************************************/

void
jsd_StartingEvalUsingFilename(JSDContext* jsdc, const char* url)
{
    JSDSourceText* jsdsrc;

    /* NOTE: We leave it locked! */
    JSD_LOCK_SOURCE_TEXT(jsdc); 

    jsdsrc = jsd_FindSourceForURL(jsdc, url);
    if(jsdsrc)
    {
#if 0
#ifndef JSD_LOWLEVEL_SOURCE
        JS_ASSERT(! jsdsrc->doingEval);
#endif
#endif
        jsdsrc->doingEval = JS_TRUE;
    }
}    

void
jsd_FinishedEvalUsingFilename(JSDContext* jsdc, const char* url)
{
    JSDSourceText* jsdsrc;

    /* NOTE: We ASSUME it is locked! */

    jsdsrc = jsd_FindSourceForURL(jsdc, url);
    if(jsdsrc)
    {
#if 0
#ifndef JSD_LOWLEVEL_SOURCE
        /*
        * when using this low level source addition, this jsdsrc might 
        * not have existed before the eval, but does exist now (without
        * this flag set!)
        */
        JS_ASSERT(jsdsrc->doingEval);
#endif
#endif
        jsdsrc->doingEval = JS_FALSE;
    }

    JSD_UNLOCK_SOURCE_TEXT(jsdc);
}    
