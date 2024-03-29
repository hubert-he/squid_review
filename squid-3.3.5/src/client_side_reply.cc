/*
 * DEBUG: section 88    Client-side Reply Routines
 * AUTHOR: Robert Collins (Originally Duane Wessels in client_side.c)
 *
 * SQUID Web Proxy Cache          http://www.squid-cache.org/
 * ----------------------------------------------------------
 *
 *  Squid is the result of efforts by numerous individuals from
 *  the Internet community; see the CONTRIBUTORS file for full
 *  details.   Many organizations have provided support for Squid's
 *  development; see the SPONSORS file for full details.  Squid is
 *  Copyrighted (C) 2001 by the Regents of the University of
 *  California; see the COPYRIGHT file for full details.  Squid
 *  incorporates software developed and/or copyrighted by other
 *  sources; see the CREDITS file for full details.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111, USA.
 *
 */
#include "squid.h"
#include "acl/FilledChecklist.h"
#include "acl/Gadgets.h"
#include "anyp/PortCfg.h"
#include "client_side_reply.h"
#include "client_side.h"
#include "clientStream.h"
#include "dlink.h"
#include "errorpage.h"
#include "fd.h"
#include "fde.h"
#include "format/Token.h"
#include "forward.h"
#include "globals.h"
#include "globals.h"
#include "HttpHeaderTools.h"
#include "HttpReply.h"
#include "HttpRequest.h"
#include "ip/QosConfig.h"
#include "ipcache.h"
#include "log/access_log.h"
#include "MemObject.h"
#include "mime_header.h"
#include "neighbors.h"
#include "refresh.h"
#include "RequestFlags.h"
#include "SquidConfig.h"
#include "SquidTime.h"
#include "Store.h"
#include "StoreClient.h"
#include "StrList.h"
#include "tools.h"
#include "URL.h"
#if USE_AUTH
#include "auth/UserRequest.h"
#endif
#if USE_DELAY_POOLS
#include "DelayPools.h"
#endif
#if USE_SQUID_ESI
#include "esi/Esi.h"
#endif

CBDATA_CLASS_INIT(clientReplyContext);

/* Local functions */
extern "C" CSS clientReplyStatus;
ErrorState *clientBuildError(err_type, http_status, char const *, Ip::Address &, HttpRequest *);

/* privates */

clientReplyContext::~clientReplyContext()
{
    deleting = true;
    /* This may trigger a callback back into SendMoreData as the cbdata
     * is still valid
     */
    removeClientStoreReference(&sc, http);
    /* old_entry might still be set if we didn't yet get the reply
     * code in HandleIMSReply() */
    removeStoreReference(&old_sc, &old_entry);
    safe_free(tempBuffer.data);
    cbdataReferenceDone(http);
    HTTPMSGUNLOCK(reply);
}

clientReplyContext::clientReplyContext(ClientHttpRequest *clientContext) : http (cbdataReference(clientContext)), old_entry (NULL), old_sc(NULL), deleting(false)
{}

/** Create an error in the store awaiting the client side to read it.
 *
 * This may be better placed in the clientStream logic, but it has not been
 * relocated there yet
 */
void
clientReplyContext::setReplyToError(
    err_type err, http_status status, const HttpRequestMethod& method, char const *uri,
    Ip::Address &addr, HttpRequest * failedrequest, const char *unparsedrequest,
#if USE_AUTH
    Auth::UserRequest::Pointer auth_user_request
#else
    void*
#endif
)
{
    ErrorState *errstate = clientBuildError(err, status, uri, addr, failedrequest);

    if (unparsedrequest)
        errstate->request_hdrs = xstrdup(unparsedrequest);

#if USE_AUTH
    errstate->auth_user_request = auth_user_request;
#endif
    setReplyToError(method, errstate);
}

void clientReplyContext::setReplyToError(const HttpRequestMethod& method, ErrorState *errstate)
{
    if (errstate->httpStatus == HTTP_NOT_IMPLEMENTED && http->request)
        /* prevent confusion over whether we default to persistent or not */
        http->request->flags.proxyKeepalive = 0;

    http->al->http.code = errstate->httpStatus;

    createStoreEntry(method, RequestFlags());
    assert(errstate->callback_data == NULL);
    errorAppendEntry(http->storeEntry(), errstate);
    /* Now the caller reads to get this */
}

void clientReplyContext::setReplyToStoreEntry(StoreEntry *entry)
{
    entry->lock(); // removeClientStoreReference() unlocks
    sc = storeClientListAdd(entry, this);
#if USE_DELAY_POOLS
    sc->setDelayId(DelayId::DelayClient(http));
#endif
    reqofs = 0;
    reqsize = 0;
    flags.storelogiccomplete = 1;
    http->storeEntry(entry);
}

void
clientReplyContext::removeStoreReference(store_client ** scp,
        StoreEntry ** ep)
{
    StoreEntry *e;
    store_client *sc_tmp = *scp;

    if ((e = *ep) != NULL) {
        *ep = NULL;
        storeUnregister(sc_tmp, e, this);
        *scp = NULL;
        e->unlock();
    }
}

void
clientReplyContext::removeClientStoreReference(store_client **scp, ClientHttpRequest *aHttpRequest)
{
    StoreEntry *reference = aHttpRequest->storeEntry();
    removeStoreReference(scp, &reference);
    aHttpRequest->storeEntry(reference);
}

void *
clientReplyContext::operator new (size_t byteCount)
{
    /* derived classes with different sizes must implement their own new */
    assert (byteCount == sizeof (clientReplyContext));
    CBDATA_INIT_TYPE(clientReplyContext);
    return cbdataAlloc(clientReplyContext);
}

void
clientReplyContext::operator delete (void *address)
{
    clientReplyContext * tmp = (clientReplyContext *)address;
    cbdataFree (tmp);
}

void
clientReplyContext::saveState()
{
    assert(old_sc == NULL);
    debugs(88, 3, "clientReplyContext::saveState: saving store context");
    old_entry = http->storeEntry();
    old_sc = sc;
    old_reqsize = reqsize;
    tempBuffer.offset = reqofs;
    /* Prevent accessing the now saved entries */
    http->storeEntry(NULL);
    sc = NULL;
    reqsize = 0;
    reqofs = 0;
}

void
clientReplyContext::restoreState()
{
    assert(old_sc != NULL);
    debugs(88, 3, "clientReplyContext::restoreState: Restoring store context");
    removeClientStoreReference(&sc, http);
    http->storeEntry(old_entry);
    sc = old_sc;
    reqsize = old_reqsize;
    reqofs = tempBuffer.offset;
    /* Prevent accessed the old saved entries */
    old_entry = NULL;
    old_sc = NULL;
    old_reqsize = 0;
    tempBuffer.offset = 0;
}

void
clientReplyContext::startError(ErrorState * err)
{
    createStoreEntry(http->request->method, RequestFlags());
    triggerInitialStoreRead();
    errorAppendEntry(http->storeEntry(), err);
}

clientStreamNode *
clientReplyContext::getNextNode() const
{
    return (clientStreamNode *)ourNode->node.next->data;
}

/* This function is wrong - the client parameters don't include the
 * header offset
 */
void
clientReplyContext::triggerInitialStoreRead()
{
    /* when confident, 0 becomes reqofs, and then this factors into
     * startSendProcess
     */
    assert(reqofs == 0);
    StoreIOBuffer localTempBuffer (next()->readBuffer.length, 0, next()->readBuffer.data);
    storeClientCopy(sc, http->storeEntry(), localTempBuffer, SendMoreData, this);
}

/* there is an expired entry in the store.
 * setup a temporary buffer area and perform an IMS to the origin
 */
void
clientReplyContext::processExpired()
{
    char *url = http->uri;
    StoreEntry *entry = NULL;
    debugs(88, 3, "clientReplyContext::processExpired: '" << http->uri << "'");
    assert(http->storeEntry()->lastmod >= 0);
    /*
     * check if we are allowed to contact other servers
     * @?@: Instead of a 504 (Gateway Timeout) reply, we may want to return
     *      a stale entry *if* it matches client requirements
     */

    if (http->onlyIfCached()) {
        processOnlyIfCachedMiss();
        return;
    }

    http->request->flags.refresh = 1;
#if STORE_CLIENT_LIST_DEBUG
    /* Prevent a race with the store client memory free routines
     */
    assert(storeClientIsThisAClient(sc, this));
#endif
    /* Prepare to make a new temporary request */
    saveState();
    entry = storeCreateEntry(url,
                             http->log_uri, http->request->flags, http->request->method);
    /* NOTE, don't call StoreEntry->lock(), storeCreateEntry() does it */
    sc = storeClientListAdd(entry, this);
#if USE_DELAY_POOLS
    /* delay_id is already set on original store client */
    sc->setDelayId(DelayId::DelayClient(http));
#endif

    http->request->lastmod = old_entry->lastmod;
    debugs(88, 5, "clientReplyContext::processExpired : lastmod " << entry->lastmod );
    http->storeEntry(entry);
    assert(http->out.offset == 0);
    assert(http->request->clientConnectionManager == http->getConn());

    /*
     * A refcounted pointer so that FwdState stays around as long as
     * this clientReplyContext does
     */
    Comm::ConnectionPointer conn = http->getConn() != NULL ? http->getConn()->clientConnection : NULL;
    FwdState::Start(conn, http->storeEntry(), http->request, http->al);

    /* Register with storage manager to receive updates when data comes in. */

    if (EBIT_TEST(entry->flags, ENTRY_ABORTED))
        debugs(88, DBG_CRITICAL, "clientReplyContext::processExpired: Found ENTRY_ABORTED object");

    {
        /* start counting the length from 0 */
        StoreIOBuffer localTempBuffer(HTTP_REQBUF_SZ, 0, tempbuf);
        storeClientCopy(sc, entry, localTempBuffer, HandleIMSReply, this);
    }
}

void
clientReplyContext::sendClientUpstreamResponse()
{
    StoreIOBuffer tempresult;
    removeStoreReference(&old_sc, &old_entry);
    /* here the data to send is the data we just received */
    tempBuffer.offset = 0;
    old_reqsize = 0;
    /* sendMoreData tracks the offset as well.
     * Force it back to zero */
    reqofs = 0;
    assert(!EBIT_TEST(http->storeEntry()->flags, ENTRY_ABORTED));
    /* TODO: provide sendMoreData with the ready parsed reply */
    tempresult.length = reqsize;
    tempresult.data = tempbuf;
    sendMoreData(tempresult);
}

void
clientReplyContext::HandleIMSReply(void *data, StoreIOBuffer result)
{
    clientReplyContext *context = (clientReplyContext *)data;
    context->handleIMSReply(result);
}

void
clientReplyContext::sendClientOldEntry()
{
    /* Get the old request back */
    restoreState();
    /* here the data to send is in the next nodes buffers already */
    assert(!EBIT_TEST(http->storeEntry()->flags, ENTRY_ABORTED));
    /* sendMoreData tracks the offset as well.
     * Force it back to zero */
    reqofs = 0;
    StoreIOBuffer tempresult (reqsize, reqofs, next()->readBuffer.data);
    sendMoreData(tempresult);
}

/* This is the workhorse of the HandleIMSReply callback.
 *
 * It is called when we've got data back from the origin following our
 * IMS request to revalidate a stale entry.
 */
void
clientReplyContext::handleIMSReply(StoreIOBuffer result)
{
    if (deleting)
        return;

    debugs(88, 3, "handleIMSReply: " << http->storeEntry()->url() << ", " << (long unsigned) result.length << " bytes" );

    if (http->storeEntry() == NULL)
        return;

    if (result.flags.error && !EBIT_TEST(http->storeEntry()->flags, ENTRY_ABORTED))
        return;

    /* update size of the request */
    reqsize = result.length + reqofs;

    const http_status status = http->storeEntry()->getReply()->sline.status;

    // request to origin was aborted
    if (EBIT_TEST(http->storeEntry()->flags, ENTRY_ABORTED)) {
        debugs(88, 3, "handleIMSReply: request to origin aborted '" << http->storeEntry()->url() << "', sending old entry to client" );
        http->logType = LOG_TCP_REFRESH_FAIL_OLD;
        sendClientOldEntry();
    }

    HttpReply *old_rep = (HttpReply *) old_entry->getReply();

    // origin replied 304
    if (status == HTTP_NOT_MODIFIED) {
        http->logType = LOG_TCP_REFRESH_UNMODIFIED;
        http->request->flags.staleIfHit = 0; // old_entry is no longer stale

        // update headers on existing entry
        old_rep->updateOnNotModified(http->storeEntry()->getReply());
        old_entry->timestampsSet();

        // if client sent IMS

        if (http->request->flags.ims && !old_entry->modifiedSince(http->request)) {
            // forward the 304 from origin
            debugs(88, 3, "handleIMSReply: origin replied 304, revalidating existing entry and forwarding 304 to client");
            sendClientUpstreamResponse();
        } else {
            // send existing entry, it's still valid
            debugs(88, 3, "handleIMSReply: origin replied 304, revalidating existing entry and sending " <<
                   old_rep->sline.status << " to client");
            sendClientOldEntry();
        }
    }

    // origin replied with a non-error code
    else if (status > HTTP_STATUS_NONE && status < HTTP_INTERNAL_SERVER_ERROR) {
        // forward response from origin
        http->logType = LOG_TCP_REFRESH_MODIFIED;
        debugs(88, 3, "handleIMSReply: origin replied " << status << ", replacing existing entry and forwarding to client");
        sendClientUpstreamResponse();
    }

    // origin replied with an error
    else if (http->request->flags.failOnValidationError) {
        http->logType = LOG_TCP_REFRESH_FAIL_ERR;
        debugs(88, 3, "handleIMSReply: origin replied with error " << status <<
               ", forwarding to client due to fail_on_validation_err");
        sendClientUpstreamResponse();
    } else {
        // ignore and let client have old entry
        http->logType = LOG_TCP_REFRESH_FAIL_OLD;
        debugs(88, 3, "handleIMSReply: origin replied with error " <<
               status << ", sending old entry (" << old_rep->sline.status << ") to client");
        sendClientOldEntry();
    }
}

SQUIDCEXTERN CSR clientGetMoreData;
SQUIDCEXTERN CSD clientReplyDetach;

/**
 * clientReplyContext::cacheHit Should only be called until the HTTP reply headers
 * have been parsed.  Normally this should be a single call, but
 * it might take more than one.  As soon as we have the headers,
 * we hand off to clientSendMoreData, processExpired, or
 * processMiss.
 */
void
clientReplyContext::CacheHit(void *data, StoreIOBuffer result)
{
    clientReplyContext *context = (clientReplyContext *)data;
    context->cacheHit(result);
}

/**
 * Process a possible cache HIT.
 */
void
clientReplyContext::cacheHit(StoreIOBuffer result)
{
    /** Ignore if the HIT object is being deleted. */
    if (deleting)
        return;

    StoreEntry *e = http->storeEntry();

    HttpRequest *r = http->request;

    debugs(88, 3, "clientCacheHit: " << http->uri << ", " << result.length << " bytes");

    if (http->storeEntry() == NULL) {
        debugs(88, 3, "clientCacheHit: request aborted");
        return;
    } else if (result.flags.error) {
        /* swap in failure */
        debugs(88, 3, "clientCacheHit: swapin failure for " << http->uri);
        http->logType = LOG_TCP_SWAPFAIL_MISS;
        removeClientStoreReference(&sc, http);
        processMiss();
        return;
    }

    if (result.length == 0) {
        /* the store couldn't get enough data from the file for us to id the
         * object
         */
        /* treat as a miss */
        http->logType = LOG_TCP_MISS;
        processMiss();
        return;
    }

    assert(!EBIT_TEST(e->flags, ENTRY_ABORTED));
    /* update size of the request */
    reqsize = result.length + reqofs;

    /*
     * Got the headers, now grok them
     */
    assert(http->logType == LOG_TCP_HIT);

    if (strcmp(e->mem_obj->url, urlCanonical(r)) != 0) {
        debugs(33, DBG_IMPORTANT, "clientProcessHit: URL mismatch, '" << e->mem_obj->url << "' != '" << urlCanonical(r) << "'");
        processMiss();
        return;
    }

    switch (varyEvaluateMatch(e, r)) {

    case VARY_NONE:
        /* No variance detected. Continue as normal */
        break;

    case VARY_MATCH:
        /* This is the correct entity for this request. Continue */
        debugs(88, 2, "clientProcessHit: Vary MATCH!");
        break;

    case VARY_OTHER:
        /* This is not the correct entity for this request. We need
         * to requery the cache.
         */
        removeClientStoreReference(&sc, http);
        e = NULL;
        /* Note: varyEvalyateMatch updates the request with vary information
         * so we only get here once. (it also takes care of cancelling loops)
         */
        debugs(88, 2, "clientProcessHit: Vary detected!");
        clientGetMoreData(ourNode, http);
        return;

    case VARY_CANCEL:
        /* varyEvaluateMatch found a object loop. Process as miss */
        debugs(88, DBG_IMPORTANT, "clientProcessHit: Vary object loop!");
        processMiss();
        return;
    }

    if (r->method == METHOD_PURGE) {
        removeClientStoreReference(&sc, http);
        e = NULL;
        purgeRequest();
        return;
    }

    if (e->checkNegativeHit()
            && !r->flags.noCacheHack()
       ) {
        http->logType = LOG_TCP_NEGATIVE_HIT;
        sendMoreData(result);
    } else if (!http->flags.internal && refreshCheckHTTP(e, r)) {
        debugs(88, 5, "clientCacheHit: in refreshCheck() block");
        /*
         * We hold a stale copy; it needs to be validated
         */
        /*
         * The 'needValidation' flag is used to prevent forwarding
         * loops between siblings.  If our copy of the object is stale,
         * then we should probably only use parents for the validation
         * request.  Otherwise two siblings could generate a loop if
         * both have a stale version of the object.
         */
        r->flags.needValidation = 1;

        if (e->lastmod < 0) {
            debugs(88, 3, "validate HIT object? NO. Missing Last-Modified header. Do MISS.");
            /*
             * Previous reply didn't have a Last-Modified header,
             * we cannot revalidate it.
             */
            http->logType = LOG_TCP_MISS;
            processMiss();
        } else if (r->flags.noCache) {
            debugs(88, 3, "validate HIT object? NO. Client sent CC:no-cache. Do CLIENT_REFRESH_MISS");
            /*
             * This did not match a refresh pattern that overrides no-cache
             * we should honour the client no-cache header.
             */
            http->logType = LOG_TCP_CLIENT_REFRESH_MISS;
            processMiss();
        } else if (r->protocol == AnyP::PROTO_HTTP) {
            debugs(88, 3, "validate HIT object? YES.");
            /*
             * Object needs to be revalidated
             * XXX This could apply to FTP as well, if Last-Modified is known.
             */
            processExpired();
        } else {
            debugs(88, 3, "validate HIT object? NO. Client protocol non-HTTP. Do MISS.");
            /*
             * We don't know how to re-validate other protocols. Handle
             * them as if the object has expired.
             */
            http->logType = LOG_TCP_MISS;
            processMiss();
        }
    } else if (r->conditional())
        processConditional(result);
    else {
        /*
         * plain ol' cache hit
         */

#if USE_DELAY_POOLS
        if (e->store_status != STORE_OK)
            http->logType = LOG_TCP_MISS;
        else
#endif
            if (e->mem_status == IN_MEMORY)
                http->logType = LOG_TCP_MEM_HIT;
            else if (Config.onoff.offline)
                http->logType = LOG_TCP_OFFLINE_HIT;

        sendMoreData(result);
    }
}

/**
 * Prepare to fetch the object as it's a cache miss of some kind.
 */
void
clientReplyContext::processMiss()
{
    char *url = http->uri;
    HttpRequest *r = http->request;
    ErrorState *err = NULL;
    debugs(88, 4, "clientProcessMiss: '" << RequestMethodStr(r->method) << " " << url << "'");

    /**
     * We might have a left-over StoreEntry from a failed cache hit
     * or IMS request.
     */
    if (http->storeEntry()) {
        if (EBIT_TEST(http->storeEntry()->flags, ENTRY_SPECIAL)) {
            debugs(88, DBG_CRITICAL, "clientProcessMiss: miss on a special object (" << url << ").");
            debugs(88, DBG_CRITICAL, "\tlog_type = " << Format::log_tags[http->logType]);
            http->storeEntry()->dump(1);
        }

        removeClientStoreReference(&sc, http);
    }

    /** Check if its a PURGE request to be actioned. */
    if (r->method == METHOD_PURGE) {
        purgeRequest();
        return;
    }

    /** Check if its an 'OTHER' request. Purge all cached entries if so and continue. */
    if (r->method == METHOD_OTHER) {
        purgeAllCached();
    }

    /** Check if 'only-if-cached' flag is set. Action if so. */
    if (http->onlyIfCached()) {
        processOnlyIfCachedMiss();
        return;
    }

    /// Deny loops
    if (r->flags.loopDetected) {
        http->al->http.code = HTTP_FORBIDDEN;
        err = clientBuildError(ERR_ACCESS_DENIED, HTTP_FORBIDDEN, NULL, http->getConn()->clientConnection->remote, http->request);
        createStoreEntry(r->method, RequestFlags());
        errorAppendEntry(http->storeEntry(), err);
        triggerInitialStoreRead();
        return;
    } else {
        assert(http->out.offset == 0);
        createStoreEntry(r->method, r->flags);  // watch out!!!!!!!!!!!!
        triggerInitialStoreRead();

        if (http->redirect.status) { // http status code : 3xx
            HttpReply *rep = new HttpReply;
            http->logType = LOG_TCP_REDIRECT;
            http->storeEntry()->releaseRequest();
            rep->redirect(http->redirect.status, http->redirect.location);
            http->storeEntry()->replaceHttpReply(rep);
            http->storeEntry()->complete();
            return;
        }

        /** Check for internal requests. Update Protocol info if so. */
        if (http->flags.internal)
            r->protocol = AnyP::PROTO_INTERNAL;

        assert(r->clientConnectionManager == http->getConn());

        /** Start forwarding to get the new object from network */
        Comm::ConnectionPointer conn = http->getConn() != NULL ? http->getConn()->clientConnection : NULL;
        FwdState::Start(conn, http->storeEntry(), r, http->al);
    }
}

/**
 * client issued a request with an only-if-cached cache-control directive;
 * we did not find a cached object that can be returned without
 *     contacting other servers;
 * respond with a 504 (Gateway Timeout) as suggested in [RFC 2068]
 */
void
clientReplyContext::processOnlyIfCachedMiss()
{
    debugs(88, 4, "clientProcessOnlyIfCachedMiss: '" <<
           RequestMethodStr(http->request->method) << " " << http->uri << "'");
    http->al->http.code = HTTP_GATEWAY_TIMEOUT;
    ErrorState *err = clientBuildError(ERR_ONLY_IF_CACHED_MISS, HTTP_GATEWAY_TIMEOUT, NULL,
                                       http->getConn()->clientConnection->remote, http->request);
    removeClientStoreReference(&sc, http);
    startError(err);
}

/// process conditional request from client
void
clientReplyContext::processConditional(StoreIOBuffer &result)
{
    StoreEntry *const e = http->storeEntry();

    if (e->getReply()->sline.status != HTTP_OK) {
        debugs(88, 4, "clientReplyContext::processConditional: Reply code " <<
               e->getReply()->sline.status << " != 200");
        http->logType = LOG_TCP_MISS;
        processMiss();
        return;
    }

    HttpRequest &r = *http->request;

    if (r.header.has(HDR_IF_MATCH) && !e->hasIfMatchEtag(r)) {
        // RFC 2616: reply with 412 Precondition Failed if If-Match did not match
        sendPreconditionFailedError();
        return;
    }

    bool matchedIfNoneMatch = false;
    if (r.header.has(HDR_IF_NONE_MATCH)) {
        if (!e->hasIfNoneMatchEtag(r)) {
            // RFC 2616: ignore IMS if If-None-Match did not match
            r.flags.ims = 0;
            r.ims = -1;
            r.imslen = 0;
            r.header.delById(HDR_IF_MODIFIED_SINCE);
            http->logType = LOG_TCP_MISS;
            sendMoreData(result);
            return;
        }

        if (!r.flags.ims) {
            // RFC 2616: if If-None-Match matched and there is no IMS,
            // reply with 304 Not Modified or 412 Precondition Failed
            sendNotModifiedOrPreconditionFailedError();
            return;
        }

        // otherwise check IMS below to decide if we reply with 304 or 412
        matchedIfNoneMatch = true;
    }

    if (r.flags.ims) {
        // handle If-Modified-Since requests from the client
        if (e->modifiedSince(&r)) {
            http->logType = LOG_TCP_IMS_HIT;
            sendMoreData(result);
            return;
        }

        if (matchedIfNoneMatch) {
            // If-None-Match matched, reply with 304 Not Modified or
            // 412 Precondition Failed
            sendNotModifiedOrPreconditionFailedError();
            return;
        }

        // otherwise reply with 304 Not Modified
        sendNotModified();
    }
}

void
clientReplyContext::purgeRequestFindObjectToPurge()
{
    /* Try to find a base entry */
    http->flags.purging = 1;
    lookingforstore = 1;

    // TODO: can we use purgeAllCached() here instead of doing the
    // getPublicByRequestMethod() dance?
    StoreEntry::getPublicByRequestMethod(this, http->request, METHOD_GET);
}

// Purges all entries with a given url
// TODO: move to SideAgent parent, when we have one
/*
 * We probably cannot purge Vary-affected responses because their MD5
 * keys depend on vary headers.
 */
void
purgeEntriesByUrl(HttpRequest * req, const char *url)
{
#if USE_HTCP
    bool get_or_head_sent = false;
#endif

    for (HttpRequestMethod m(METHOD_NONE); m != METHOD_ENUM_END; ++m) {
        if (m.isCacheble()) {
            if (StoreEntry *entry = storeGetPublic(url, m)) {
                debugs(88, 5, "purging " << RequestMethodStr(m) << ' ' << url);
#if USE_HTCP
                neighborsHtcpClear(entry, url, req, m, HTCP_CLR_INVALIDATION);
                if (m == METHOD_GET || m == METHOD_HEAD) {
                    get_or_head_sent = true;
                }
#endif
                entry->release();
            }
        }
    }

#if USE_HTCP
    if (!get_or_head_sent) {
        neighborsHtcpClear(NULL, url, req, HttpRequestMethod(METHOD_GET), HTCP_CLR_INVALIDATION);
    }
#endif
}

void
clientReplyContext::purgeAllCached()
{
    const char *url = urlCanonical(http->request);
    purgeEntriesByUrl(http->request, url);
}

void
clientReplyContext::created(StoreEntry *newEntry)
{
    if (lookingforstore == 1)
        purgeFoundGet(newEntry);
    else if (lookingforstore == 2)
        purgeFoundHead(newEntry);
    else if (lookingforstore == 3)
        purgeDoPurgeGet(newEntry);
    else if (lookingforstore == 4)
        purgeDoPurgeHead(newEntry);
    else if (lookingforstore == 5)
        identifyFoundObject(newEntry);
}

void
clientReplyContext::purgeFoundGet(StoreEntry *newEntry)
{
    if (newEntry->isNull()) {
        lookingforstore = 2;
        StoreEntry::getPublicByRequestMethod(this, http->request, METHOD_HEAD);
    } else
        purgeFoundObject (newEntry);
}

void
clientReplyContext::purgeFoundHead(StoreEntry *newEntry)
{
    if (newEntry->isNull())
        purgeDoMissPurge();
    else
        purgeFoundObject (newEntry);
}

void
clientReplyContext::purgeFoundObject(StoreEntry *entry)
{
    assert (entry && !entry->isNull());

    if (EBIT_TEST(entry->flags, ENTRY_SPECIAL)) {
        http->logType = LOG_TCP_DENIED;
        ErrorState *err = clientBuildError(ERR_ACCESS_DENIED, HTTP_FORBIDDEN, NULL,
                                           http->getConn()->clientConnection->remote, http->request);
        startError(err);
        return;
    }

    StoreIOBuffer localTempBuffer;
    /* Swap in the metadata */
    http->storeEntry(entry);

    http->storeEntry()->lock();
    http->storeEntry()->createMemObject(http->uri, http->log_uri);

    http->storeEntry()->mem_obj->method = http->request->method;

    sc = storeClientListAdd(http->storeEntry(), this);

    http->logType = LOG_TCP_HIT;

    reqofs = 0;

    localTempBuffer.offset = http->out.offset;

    localTempBuffer.length = next()->readBuffer.length;

    localTempBuffer.data = next()->readBuffer.data;

    storeClientCopy(sc, http->storeEntry(),
                    localTempBuffer, CacheHit, this);
}

void
clientReplyContext::purgeRequest()
{
    debugs(88, 3, "Config2.onoff.enable_purge = " <<
           Config2.onoff.enable_purge);

    if (!Config2.onoff.enable_purge) {
        http->logType = LOG_TCP_DENIED;
        ErrorState *err = clientBuildError(ERR_ACCESS_DENIED, HTTP_FORBIDDEN, NULL, http->getConn()->clientConnection->remote, http->request);
        startError(err);
        return;
    }

    /* Release both IP cache */
    ipcacheInvalidate(http->request->GetHost());

    if (!http->flags.purging)
        purgeRequestFindObjectToPurge();
    else
        purgeDoMissPurge();
}

void
clientReplyContext::purgeDoMissPurge()
{
    http->logType = LOG_TCP_MISS;
    lookingforstore = 3;
    StoreEntry::getPublicByRequestMethod(this,http->request, METHOD_GET);
}

void
clientReplyContext::purgeDoPurgeGet(StoreEntry *newEntry)
{
    assert (newEntry);
    /* Move to new() when that is created */
    purgeStatus = HTTP_NOT_FOUND;

    if (!newEntry->isNull()) {
        /* Release the cached URI */
        debugs(88, 4, "clientPurgeRequest: GET '" << newEntry->url() << "'" );
#if USE_HTCP
        neighborsHtcpClear(newEntry, NULL, http->request, HttpRequestMethod(METHOD_GET), HTCP_CLR_PURGE);
#endif
        newEntry->release();
        purgeStatus = HTTP_OK;
    }

    lookingforstore = 4;
    StoreEntry::getPublicByRequestMethod(this, http->request, METHOD_HEAD);
}

void
clientReplyContext::purgeDoPurgeHead(StoreEntry *newEntry)
{
    if (newEntry && !newEntry->isNull()) {
        debugs(88, 4, "clientPurgeRequest: HEAD '" << newEntry->url() << "'" );
#if USE_HTCP
        neighborsHtcpClear(newEntry, NULL, http->request, HttpRequestMethod(METHOD_HEAD), HTCP_CLR_PURGE);
#endif
        newEntry->release();
        purgeStatus = HTTP_OK;
    }

    /* And for Vary, release the base URI if none of the headers was included in the request */

    if (http->request->vary_headers
            && !strstr(http->request->vary_headers, "=")) {
        StoreEntry *entry = storeGetPublic(urlCanonical(http->request), METHOD_GET);

        if (entry) {
            debugs(88, 4, "clientPurgeRequest: Vary GET '" << entry->url() << "'" );
#if USE_HTCP
            neighborsHtcpClear(entry, NULL, http->request, HttpRequestMethod(METHOD_GET), HTCP_CLR_PURGE);
#endif
            entry->release();
            purgeStatus = HTTP_OK;
        }

        entry = storeGetPublic(urlCanonical(http->request), METHOD_HEAD);

        if (entry) {
            debugs(88, 4, "clientPurgeRequest: Vary HEAD '" << entry->url() << "'" );
#if USE_HTCP
            neighborsHtcpClear(entry, NULL, http->request, HttpRequestMethod(METHOD_HEAD), HTCP_CLR_PURGE);
#endif
            entry->release();
            purgeStatus = HTTP_OK;
        }
    }

    /*
     * Make a new entry to hold the reply to be written
     * to the client.
     */
    /* FIXME: This doesn't need to go through the store. Simply
     * push down the client chain
     */
    createStoreEntry(http->request->method, RequestFlags());

    triggerInitialStoreRead();

    HttpReply *rep = new HttpReply;
    rep->setHeaders(purgeStatus, NULL, NULL, 0, 0, -1);
    http->storeEntry()->replaceHttpReply(rep);
    http->storeEntry()->complete();
}

void
clientReplyContext::traceReply(clientStreamNode * node)
{
    clientStreamNode *nextNode = (clientStreamNode *)node->node.next->data;
    StoreIOBuffer localTempBuffer;
    createStoreEntry(http->request->method, RequestFlags());
    localTempBuffer.offset = nextNode->readBuffer.offset + headers_sz;
    localTempBuffer.length = nextNode->readBuffer.length;
    localTempBuffer.data = nextNode->readBuffer.data;
    storeClientCopy(sc, http->storeEntry(),
                    localTempBuffer, SendMoreData, this);
    http->storeEntry()->releaseRequest();
    http->storeEntry()->buffer();
    HttpReply *rep = new HttpReply;
    rep->setHeaders(HTTP_OK, NULL, "text/plain", http->request->prefixLen(), 0, squid_curtime);
    http->storeEntry()->replaceHttpReply(rep);
    http->request->swapOut(http->storeEntry());
    http->storeEntry()->complete();
}

#define SENDING_BODY 0
#define SENDING_HDRSONLY 1
int
clientReplyContext::checkTransferDone()
{
    StoreEntry *entry = http->storeEntry();

    if (entry == NULL)
        return 0;

    /*
     * For now, 'done_copying' is used for special cases like
     * Range and HEAD requests.
     */
    if (http->flags.done_copying)
        return 1;

    if (http->request->flags.chunkedReply && !flags.complete) {
        // last-chunk was not sent
        return 0;
    }

    /*
     * Handle STORE_OK objects.
     * objectLen(entry) will be set proprely.
     * RC: Does objectLen(entry) include the Headers?
     * RC: Yes.
     */
    if (entry->store_status == STORE_OK) {
        return storeOKTransferDone();
    } else {
        return storeNotOKTransferDone();
    }
}

int
clientReplyContext::storeOKTransferDone() const
{
    assert(http->storeEntry()->objectLen() >= 0);
    assert(http->storeEntry()->objectLen() >= headers_sz);
    if (http->out.offset >= http->storeEntry()->objectLen() - headers_sz) {
        debugs(88,3,HERE << "storeOKTransferDone " <<
               " out.offset=" << http->out.offset <<
               " objectLen()=" << http->storeEntry()->objectLen() <<
               " headers_sz=" << headers_sz);
        return 1;
    }

    return 0;
}

int
clientReplyContext::storeNotOKTransferDone() const
{
    /*
     * Now, handle STORE_PENDING objects
     */
    MemObject *mem = http->storeEntry()->mem_obj;
    assert(mem != NULL);
    assert(http->request != NULL);
    /* mem->reply was wrong because it uses the UPSTREAM header length!!! */
    HttpReply const *curReply = mem->getReply();

    if (headers_sz == 0)
        /* haven't found end of headers yet */
        return 0;

    /*
     * Figure out how much data we are supposed to send.
     * If we are sending a body and we don't have a content-length,
     * then we must wait for the object to become STORE_OK.
     */
    if (curReply->content_length < 0)
        return 0;

    int64_t expectedLength = curReply->content_length + http->out.headers_sz;

    if (http->out.size < expectedLength)
        return 0;
    else {
        debugs(88,3,HERE << "storeNotOKTransferDone " <<
               " out.size=" << http->out.size <<
               " expectedLength=" << expectedLength);
        return 1;
    }
}

/* A write has completed, what is the next status based on the
 * canonical request data?
 * 1 something is wrong
 * 0 nothing is wrong.
 *
 */
int
clientHttpRequestStatus(int fd, ClientHttpRequest const *http)
{
#if SIZEOF_INT64_T == 4
    if (http->out.size > 0x7FFF0000) {
        debugs(88, DBG_IMPORTANT, "WARNING: closing FD " << fd << " to prevent out.size counter overflow");
        debugs(88, DBG_IMPORTANT, "\tclient " << http->getConn()->peer);
        debugs(88, DBG_IMPORTANT, "\treceived " << http->out.size << " bytes");
        debugs(88, DBG_IMPORTANT, "\tURI " << http->log_uri);
        return 1;
    }

    if (http->out.offset > 0x7FFF0000) {
        debugs(88, DBG_IMPORTANT, "WARNING: closing FD " << fd < " to prevent out.offset counter overflow");
        debugs(88, DBG_IMPORTANT, "\tclient " << http->getConn()->peer);
        debugs(88, DBG_IMPORTANT, "\treceived " << http->out.size << " bytes, offset " << http->out.offset);
        debugs(88, DBG_IMPORTANT, "\tURI " << http->log_uri);
        return 1;
    }

#endif
    return 0;
}

/* Preconditions:
 * *http is a valid structure.
 * fd is either -1, or an open fd.
 *
 * TODO: enumify this
 *
 * This function is used by any http request sink, to determine the status
 * of the object.
 */
clientStream_status_t
clientReplyStatus(clientStreamNode * aNode, ClientHttpRequest * http)
{
    clientReplyContext *context = dynamic_cast<clientReplyContext *>(aNode->data.getRaw());
    assert (context);
    assert (context->http == http);
    return context->replyStatus();
}

clientStream_status_t
clientReplyContext::replyStatus()
{
    int done;
    /* Here because lower nodes don't need it */

    if (http->storeEntry() == NULL) {
        debugs(88, 5, "clientReplyStatus: no storeEntry");
        return STREAM_FAILED;	/* yuck, but what can we do? */
    }

    if (EBIT_TEST(http->storeEntry()->flags, ENTRY_ABORTED)) {
        /* TODO: Could upstream read errors (result.flags.error) be
         * lost, and result in undersize requests being considered
         * complete. Should we tcp reset such connections ?
         */
        debugs(88, 5, "clientReplyStatus: aborted storeEntry");
        return STREAM_FAILED;
    }

    if ((done = checkTransferDone()) != 0 || flags.complete) {
        debugs(88, 5, "clientReplyStatus: transfer is DONE");
        /* Ok we're finished, but how? */

        const int64_t expectedBodySize =
            http->storeEntry()->getReply()->bodySize(http->request->method);
        if (!http->request->flags.proxyKeepalive && expectedBodySize < 0) {
            debugs(88, 5, "clientReplyStatus: closing, content_length < 0");
            return STREAM_FAILED;
        }

        if (!done) {
            debugs(88, 5, "clientReplyStatus: closing, !done, but read 0 bytes");
            return STREAM_FAILED;
        }

        if (expectedBodySize >= 0 && !http->gotEnough()) {
            debugs(88, 5, "clientReplyStatus: client didn't get all it expected");
            return STREAM_UNPLANNED_COMPLETE;
        }

        if (http->request->flags.proxyKeepalive) {
            debugs(88, 5, "clientReplyStatus: stream complete and can keepalive");
            return STREAM_COMPLETE;
        }

        debugs(88, 5, "clientReplyStatus: stream was not expected to complete!");
        return STREAM_UNPLANNED_COMPLETE;
    }

    // XXX: Should this be checked earlier? We could return above w/o checking.
    if (reply->receivedBodyTooLarge(*http->request, http->out.offset - 4096)) {
        /* 4096 is a margin for the HTTP headers included in out.offset */
        debugs(88, 5, "clientReplyStatus: client reply body is too large");
        return STREAM_FAILED;
    }

    return STREAM_NONE;
}

/* Responses with no body will not have a content-type header,
 * which breaks the rep_mime_type acl, which
 * coincidentally, is the most common acl for reply access lists.
 * A better long term fix for this is to allow acl matchs on the various
 * status codes, and then supply a default ruleset that puts these
 * codes before any user defines access entries. That way the user
 * can choose to block these responses where appropriate, but won't get
 * mysterious breakages.
 */
bool
clientReplyContext::alwaysAllowResponse(http_status sline) const
{
    bool result;

    switch (sline) {

    case HTTP_CONTINUE:

    case HTTP_SWITCHING_PROTOCOLS:

    case HTTP_PROCESSING:

    case HTTP_NO_CONTENT:

    case HTTP_NOT_MODIFIED:
        result = true;
        break;

    default:
        result = false;
    }

    return result;
}

/**
 * Generate the reply headers sent to client.
 *
 * Filters out unwanted entries and hop-by-hop from original reply header
 * then adds extra entries if we have more info than origin server
 * then adds Squid specific entries
 */
void
clientReplyContext::buildReplyHeader()
{
    HttpHeader *hdr = &reply->header;
    int is_hit = logTypeIsATcpHit(http->logType);
    HttpRequest *request = http->request;
#if DONT_FILTER_THESE
    /* but you might want to if you run Squid as an HTTP accelerator */
    /* hdr->delById(HDR_ACCEPT_RANGES); */
    hdr->delById(HDR_ETAG);
#endif

    if (is_hit)
        hdr->delById(HDR_SET_COOKIE);
    // TODO: RFC 2965 : Must honour Cache-Control: no-cache="set-cookie2" and remove header.

    // if there is not configured a peer proxy with login=PASS or login=PASSTHRU option enabled
    // remove the Proxy-Authenticate header
    if ( !request->peer_login || (strcmp(request->peer_login,"PASS") != 0 && strcmp(request->peer_login,"PASSTHRU") != 0))
        reply->header.delById(HDR_PROXY_AUTHENTICATE);

    reply->header.removeHopByHopEntries();

    //    if (request->range)
    //      clientBuildRangeHeader(http, reply);

    /*
     * Add a estimated Age header on cache hits.
     */
    if (is_hit) {
        /*
         * Remove any existing Age header sent by upstream caches
         * (note that the existing header is passed along unmodified
         * on cache misses)
         */
        hdr->delById(HDR_AGE);
        /*
         * This adds the calculated object age. Note that the details of the
         * age calculation is performed by adjusting the timestamp in
         * StoreEntry::timestampsSet(), not here.
         */
#if DEAD_CODE
        // XXX: realy useless? or is there a bug now that this is detatched from the below if-sequence ?
        // looks like this pre-if was supposed to be the browser workaround...
        if (NULL == http->storeEntry())
            (void) 0;
        else if (http->storeEntry()->timestamp < 0)
            (void) 0;
#endif

        if (EBIT_TEST(http->storeEntry()->flags, ENTRY_SPECIAL)) {
            hdr->delById(HDR_DATE);
            hdr->insertTime(HDR_DATE, squid_curtime);
        } else if (http->getConn() && http->getConn()->port->actAsOrigin) {
            // Swap the Date: header to current time if we are simulating an origin
            HttpHeaderEntry *h = hdr->findEntry(HDR_DATE);
            if (h)
                hdr->putExt("X-Origin-Date", h->value.termedBuf());
            hdr->delById(HDR_DATE);
            hdr->insertTime(HDR_DATE, squid_curtime);
            h = hdr->findEntry(HDR_EXPIRES);
            if (h && http->storeEntry()->expires >= 0) {
                hdr->putExt("X-Origin-Expires", h->value.termedBuf());
                hdr->delById(HDR_EXPIRES);
                hdr->insertTime(HDR_EXPIRES, squid_curtime + http->storeEntry()->expires - http->storeEntry()->timestamp);
            }
            if (http->storeEntry()->timestamp <= squid_curtime) {
                // put X-Cache-Age: instead of Age:
                char age[64];
                snprintf(age, sizeof(age), "%ld", (long int) squid_curtime - http->storeEntry()->timestamp);
                hdr->putExt("X-Cache-Age", age);
            }
        } else if (http->storeEntry()->timestamp <= squid_curtime) {
            hdr->putInt(HDR_AGE,
                        squid_curtime - http->storeEntry()->timestamp);
            /* Signal old objects.  NB: rfc 2616 is not clear,
             * by implication, on whether we should do this to all
             * responses, or only cache hits.
             * 14.46 states it ONLY applys for heuristically caclulated
             * freshness values, 13.2.4 doesn't specify the same limitation.
             * We interpret RFC 2616 under the combination.
             */
            /* TODO: if maxage or s-maxage is present, don't do this */

            if (squid_curtime - http->storeEntry()->timestamp >= 86400) {
                char tbuf[512];
                snprintf (tbuf, sizeof(tbuf), "%s %s %s",
                          "113", ThisCache,
                          "This cache hit is still fresh and more than 1 day old");
                hdr->putStr(HDR_WARNING, tbuf);
            }
        }
    }

    /* RFC 2616: Section 14.18
     *
     * Add a Date: header if missing.
     * We have access to a clock therefore are required to amend any shortcoming in servers.
     *
     * NP: done after Age: to prevent ENTRY_SPECIAL double-handling this header.
     */
    if ( !hdr->has(HDR_DATE) ) {
        if (!http->storeEntry())
            hdr->insertTime(HDR_DATE, squid_curtime);
        else if (http->storeEntry()->timestamp > 0)
            hdr->insertTime(HDR_DATE, http->storeEntry()->timestamp);
        else {
            debugs(88,DBG_IMPORTANT,"BUG 3279: HTTP reply without Date:");
            /* dump something useful about the problem */
            http->storeEntry()->dump(DBG_IMPORTANT);
        }
    }

    // add Warnings required by RFC 2616 if serving a stale hit
    if (http->request->flags.staleIfHit && logTypeIsATcpHit(http->logType)) {
        hdr->putWarning(110, "Response is stale");
        if (http->request->flags.needValidation)
            hdr->putWarning(111, "Revalidation failed");
    }

    /* Filter unproxyable authentication types */
    if (http->logType != LOG_TCP_DENIED &&
            hdr->has(HDR_WWW_AUTHENTICATE)) {
        HttpHeaderPos pos = HttpHeaderInitPos;
        HttpHeaderEntry *e;

        int connection_auth_blocked = 0;
        while ((e = hdr->getEntry(&pos))) {
            if (e->id == HDR_WWW_AUTHENTICATE) {
                const char *value = e->value.rawBuf();

                if ((strncasecmp(value, "NTLM", 4) == 0 &&
                        (value[4] == '\0' || value[4] == ' '))
                        ||
                        (strncasecmp(value, "Negotiate", 9) == 0 &&
                         (value[9] == '\0' || value[9] == ' '))
                        ||
                        (strncasecmp(value, "Kerberos", 8) == 0 &&
                         (value[8] == '\0' || value[8] == ' '))) {
                    if (request->flags.connectionAuthDisabled) {
                        hdr->delAt(pos, connection_auth_blocked);
                        continue;
                    }
                    request->flags.mustKeepalive = 1;
                    if (!request->flags.accelerated && !request->flags.intercepted) {
                        httpHeaderPutStrf(hdr, HDR_PROXY_SUPPORT, "Session-Based-Authentication");
                        /*
                          We send "[Proxy-]Connection: Proxy-Support" header to mark
                          Proxy-Support as a hop-by-hop header for intermediaries that do not
                          understand the semantics of this header. The RFC should have included
                          this recommendation.
                        */
                        httpHeaderPutStrf(hdr, HDR_CONNECTION, "Proxy-support");
                    }
                    break;
                }
            }
        }

        if (connection_auth_blocked)
            hdr->refreshMask();
    }

#if USE_AUTH
    /* Handle authentication headers */
    if (http->logType == LOG_TCP_DENIED &&
            ( reply->sline.status == HTTP_PROXY_AUTHENTICATION_REQUIRED ||
              reply->sline.status == HTTP_UNAUTHORIZED)
       ) {
        /* Add authentication header */
        /*! \todo alter errorstate to be accel on|off aware. The 0 on the next line
         * depends on authenticate behaviour: all schemes to date send no extra
         * data on 407/401 responses, and do not check the accel state on 401/407
         * responses
         */
        authenticateFixHeader(reply, request->auth_user_request, request, 0, 1);
    } else if (request->auth_user_request != NULL)
        authenticateFixHeader(reply, request->auth_user_request, request, http->flags.accel, 0);
#endif

    /* Append X-Cache */
    httpHeaderPutStrf(hdr, HDR_X_CACHE, "%s from %s",
                      is_hit ? "HIT" : "MISS", getMyHostname());

#if USE_CACHE_DIGESTS
    /* Append X-Cache-Lookup: -- temporary hack, to be removed @?@ @?@ */
    httpHeaderPutStrf(hdr, HDR_X_CACHE_LOOKUP, "%s from %s:%d",
                      lookup_type ? lookup_type : "NONE",
                      getMyHostname(), getMyPort());

#endif

    const bool maySendChunkedReply = !request->multipartRangeRequest() &&
                                     reply->sline.protocol == AnyP::PROTO_HTTP && // response is HTTP
                                     (request->http_ver >= HttpVersion(1, 1));

    /* Check whether we should send keep-alive */
    if (!Config.onoff.error_pconns && reply->sline.status >= 400 && !request->flags.mustKeepalive) {
        debugs(33, 3, "clientBuildReplyHeader: Error, don't keep-alive");
        request->flags.proxyKeepalive = 0;
    } else if (!Config.onoff.client_pconns && !request->flags.mustKeepalive) {
        debugs(33, 2, "clientBuildReplyHeader: Connection Keep-Alive not requested by admin or client");
        request->flags.proxyKeepalive = 0;
    } else if (request->flags.proxyKeepalive && shutting_down) {
        debugs(88, 3, "clientBuildReplyHeader: Shutting down, don't keep-alive.");
        request->flags.proxyKeepalive = 0;
    } else if (request->flags.connectionAuth && !reply->keep_alive) {
        debugs(33, 2, "clientBuildReplyHeader: Connection oriented auth but server side non-persistent");
        request->flags.proxyKeepalive = 0;
    } else if (reply->bodySize(request->method) < 0 && !maySendChunkedReply) {
        debugs(88, 3, "clientBuildReplyHeader: can't keep-alive, unknown body size" );
        request->flags.proxyKeepalive = 0;
    } else if (fdUsageHigh()&& !request->flags.mustKeepalive) {
        debugs(88, 3, "clientBuildReplyHeader: Not many unused FDs, can't keep-alive");
        request->flags.proxyKeepalive = 0;
    } else if (request->flags.sslBumped && !reply->persistent()) {
        // We do not really have to close, but we pretend we are a tunnel.
        debugs(88, 3, "clientBuildReplyHeader: bumped reply forces close");
        request->flags.proxyKeepalive = 0;
    } else if (request->pinnedConnection() && !reply->persistent()) {
        // The peer wants to close the pinned connection
        debugs(88, 3, "pinned reply forces close");
        request->flags.proxyKeepalive = 0;
    }

    // Decide if we send chunked reply
    if (maySendChunkedReply &&
            request->flags.proxyKeepalive &&
            reply->bodySize(request->method) < 0) {
        debugs(88, 3, "clientBuildReplyHeader: chunked reply");
        request->flags.chunkedReply = 1;
        hdr->putStr(HDR_TRANSFER_ENCODING, "chunked");
    }

    /* Append VIA */
    if (Config.onoff.via) {
        LOCAL_ARRAY(char, bbuf, MAX_URL + 32);
        String strVia;
        hdr->getList(HDR_VIA, &strVia);
        snprintf(bbuf, MAX_URL + 32, "%d.%d %s",
                 reply->sline.version.major,
                 reply->sline.version.minor,
                 ThisCache);
        strListAdd(&strVia, bbuf, ',');
        hdr->delById(HDR_VIA);
        hdr->putStr(HDR_VIA, strVia.termedBuf());
    }
    /* Signal keep-alive or close explicitly */
    hdr->putStr(HDR_CONNECTION, request->flags.proxyKeepalive ? "keep-alive" : "close");

#if ADD_X_REQUEST_URI
    /*
     * Knowing the URI of the request is useful when debugging persistent
     * connections in a client; we cannot guarantee the order of http headers,
     * but X-Request-URI is likely to be the very last header to ease use from a
     * debugger [hdr->entries.count-1].
     */
    hdr->putStr(HDR_X_REQUEST_URI,
                http->memOjbect()->url ? http->memObject()->url : http->uri);

#endif

    /* Surrogate-Control requires Surrogate-Capability from upstream to pass on */
    if ( hdr->has(HDR_SURROGATE_CONTROL) ) {
        if (!request->header.has(HDR_SURROGATE_CAPABILITY)) {
            hdr->delById(HDR_SURROGATE_CONTROL);
        }
        /* TODO: else case: drop any controls intended specifically for our surrogate ID */
    }

    httpHdrMangleList(hdr, request, ROR_REPLY);
}

void
clientReplyContext::cloneReply()
{
    assert(reply == NULL);

    HttpReply *rep = http->storeEntry()->getReply()->clone();

    reply = HTTPMSGLOCK(rep);

    if (reply->sline.protocol == AnyP::PROTO_HTTP) {
        /* RFC 2616 requires us to advertise our 1.1 version (but only on real HTTP traffic) */
        reply->sline.version = HttpVersion(1,1);
    }

    /* do header conversions */
    buildReplyHeader();
}

void
clientReplyContext::identifyStoreObject()
{
    HttpRequest *r = http->request;

    if (r->flags.cachable || r->flags.internal) {
        lookingforstore = 5;
        StoreEntry::getPublicByRequest (this, r);
    } else {
        identifyFoundObject (NullStoreEntry::getInstance());
    }
}

/**
 * Check state of the current StoreEntry object.
 * to see if we can determine the final status of the request.
 */
void
clientReplyContext::identifyFoundObject(StoreEntry *newEntry)
{
    StoreEntry *e = newEntry;
    HttpRequest *r = http->request;

    /** \li If the entry received isNull() then we ignore it. */
    if (e->isNull()) {
        http->storeEntry(NULL);
    } else {
        http->storeEntry(e);
    }

    e = http->storeEntry();

    /* Release IP-cache entries on reload */
    /** \li If the request has no-cache flag set or some no_cache HACK in operation we
      * 'invalidate' the cached IP entries for this request ???
      */
    if (r->flags.noCache) {

#if USE_DNSHELPER
        ipcacheInvalidate(r->GetHost());
#else
        ipcacheInvalidateNegative(r->GetHost());
#endif /* USE_DNSHELPER */

    } else if (r->flags.noCacheHack()) {

#if USE_DNSHELPER
        ipcacheInvalidate(r->GetHost());
#else
        ipcacheInvalidateNegative(r->GetHost());
#endif /* USE_DNSHELPER */

    }

#if USE_CACHE_DIGESTS
    lookup_type = http->storeEntry() ? "HIT" : "MISS";
#endif

    if (NULL == http->storeEntry()) {
        /** \li If no StoreEntry object is current assume this object isn't in the cache set MISS*/
        debugs(85, 3, "clientProcessRequest2: StoreEntry is NULL -  MISS");
        http->logType = LOG_TCP_MISS;
        doGetMoreData();
        return;
    }

    if (Config.onoff.offline) {
        /** \li If we are running in offline mode set to HIT */
        debugs(85, 3, "clientProcessRequest2: offline HIT");
        http->logType = LOG_TCP_HIT;
        doGetMoreData();
        return;
    }

    if (http->redirect.status) {
        /** \li If redirection status is True force this to be a MISS */
        debugs(85, 3, HERE << "REDIRECT status forced StoreEntry to NULL (no body on 3XX responses)");
        http->storeEntry(NULL);
        http->logType = LOG_TCP_REDIRECT;
        doGetMoreData();
        return;
    }

    if (!e->validToSend()) {
        debugs(85, 3, "clientProcessRequest2: !storeEntryValidToSend MISS" );
        http->storeEntry(NULL);
        http->logType = LOG_TCP_MISS;
        doGetMoreData();
        return;
    }

    if (EBIT_TEST(e->flags, ENTRY_SPECIAL)) {
        /* \li Special entries are always hits, no matter what the client says */
        debugs(85, 3, "clientProcessRequest2: ENTRY_SPECIAL HIT");
        http->logType = LOG_TCP_HIT;
        doGetMoreData();
        return;
    }

    if (r->flags.noCache) {
        debugs(85, 3, "clientProcessRequest2: no-cache REFRESH MISS");
        http->storeEntry(NULL);
        http->logType = LOG_TCP_CLIENT_REFRESH_MISS;
        doGetMoreData();
        return;
    }

    debugs(85, 3, "clientProcessRequest2: default HIT");
    http->logType = LOG_TCP_HIT;
    doGetMoreData();
}

/**
 * Request more data from the store for the client Stream
 * This is *the* entry point to this module.
 *
 * Preconditions:
 *  - This is the head of the list.
 *  - There is at least one more node.
 *  - Data context is not null
 */
void
clientGetMoreData(clientStreamNode * aNode, ClientHttpRequest * http)
{
    /* Test preconditions */
    assert(aNode != NULL);
    assert(cbdataReferenceValid(aNode));
    assert(aNode->node.prev == NULL);
    assert(aNode->node.next != NULL);
    clientReplyContext *context = dynamic_cast<clientReplyContext *>(aNode->data.getRaw());
    assert (context);
    assert(context->http == http);

    clientStreamNode *next = ( clientStreamNode *)aNode->node.next->data;

    if (!context->ourNode)
        context->ourNode = aNode;

    /* no cbdatareference, this is only used once, and safely */
    if (context->flags.storelogiccomplete) {
        StoreIOBuffer tempBuffer;
        tempBuffer.offset = next->readBuffer.offset + context->headers_sz;
        tempBuffer.length = next->readBuffer.length;
        tempBuffer.data = next->readBuffer.data;

        storeClientCopy(context->sc, http->storeEntry(),
                        tempBuffer, clientReplyContext::SendMoreData, context);
        return;
    }

    if (context->http->request->method == METHOD_PURGE) {
        context->purgeRequest();
        return;
    }

    // OPTIONS with Max-Forwards:0 handled in clientProcessRequest()

    if (context->http->request->method == METHOD_TRACE) {
        if (context->http->request->header.getInt64(HDR_MAX_FORWARDS) == 0) {
            context->traceReply(aNode);
            return;
        }

        /* continue forwarding, not finished yet. */
        http->logType = LOG_TCP_MISS;

        context->doGetMoreData();
    } else
        context->identifyStoreObject();
}

void
clientReplyContext::doGetMoreData()
{
    /* We still have to do store logic processing - vary, cache hit etc */
    if (http->storeEntry() != NULL) {
        /* someone found the object in the cache for us */
        StoreIOBuffer localTempBuffer;

        http->storeEntry()->lock();

        if (http->storeEntry()->mem_obj == NULL) {
            /*
             * This if-block exists because we don't want to clobber
             * a preexiting mem_obj->method value if the mem_obj
             * already exists.  For example, when a HEAD request
             * is a cache hit for a GET response, we want to keep
             * the method as GET.
             */
            http->storeEntry()->createMemObject(http->uri, http->log_uri);
            http->storeEntry()->mem_obj->method = http->request->method;
        }

        sc = storeClientListAdd(http->storeEntry(), this);
#if USE_DELAY_POOLS
        sc->setDelayId(DelayId::DelayClient(http));
#endif

        assert(http->logType == LOG_TCP_HIT);
        reqofs = 0;
        /* guarantee nothing has been sent yet! */
        assert(http->out.size == 0);
        assert(http->out.offset == 0);

        if (Ip::Qos::TheConfig.isHitTosActive()) {
            Ip::Qos::doTosLocalHit(http->getConn()->clientConnection);
        }

        if (Ip::Qos::TheConfig.isHitNfmarkActive()) {
            Ip::Qos::doNfmarkLocalHit(http->getConn()->clientConnection);
        }

        localTempBuffer.offset = reqofs;
        localTempBuffer.length = getNextNode()->readBuffer.length;
        localTempBuffer.data = getNextNode()->readBuffer.data;
        storeClientCopy(sc, http->storeEntry(), localTempBuffer, CacheHit, this);
    } else {
        /* MISS CASE, http->logType is already set! */
        processMiss();
    }
}

/** The next node has removed itself from the stream. */
void
clientReplyDetach(clientStreamNode * node, ClientHttpRequest * http)
{
    /** detach from the stream */
    clientStreamDetach(node, http);
}

/**
 * Accepts chunk of a http message in buf, parses prefix, filters headers and
 * such, writes processed message to the message recipient
 */
void
clientReplyContext::SendMoreData(void *data, StoreIOBuffer result)
{
    clientReplyContext *context = static_cast<clientReplyContext *>(data);
    context->sendMoreData (result);
}

void
clientReplyContext::makeThisHead()
{
    /* At least, I think thats what this does */
    dlinkDelete(&http->active, &ClientActiveRequests);
    dlinkAdd(http, &http->active, &ClientActiveRequests);
}

bool
clientReplyContext::errorInStream(StoreIOBuffer const &result, size_t const &sizeToProcess)const
{
    return /* aborted request */
        (http->storeEntry() && EBIT_TEST(http->storeEntry()->flags, ENTRY_ABORTED)) ||
        /* Upstream read error */ (result.flags.error) ||
        /* Upstream EOF */ (sizeToProcess == 0);
}

void
clientReplyContext::sendStreamError(StoreIOBuffer const &result)
{
    /** call clientWriteComplete so the client socket gets closed
     *
     * We call into the stream, because we don't know that there is a
     * client socket!
     */
    debugs(88, 5, "clientReplyContext::sendStreamError: A stream error has occured, marking as complete and sending no data.");
    StoreIOBuffer localTempBuffer;
    flags.complete = 1;
    http->request->flags.streamError = 1;
    localTempBuffer.flags.error = result.flags.error;
    clientStreamCallback((clientStreamNode*)http->client_stream.head->data, http, NULL,
                         localTempBuffer);
}

void
clientReplyContext::pushStreamData(StoreIOBuffer const &result, char *source)
{
    StoreIOBuffer localTempBuffer;

    if (result.length == 0) {
        debugs(88, 5, "clientReplyContext::pushStreamData: marking request as complete due to 0 length store result");
        flags.complete = 1;
    }

    assert(result.offset - headers_sz == next()->readBuffer.offset);
    localTempBuffer.offset = result.offset - headers_sz;
    localTempBuffer.length = result.length;

    if (localTempBuffer.length)
        localTempBuffer.data = source;

    clientStreamCallback((clientStreamNode*)http->client_stream.head->data, http, NULL,
                         localTempBuffer);
}

clientStreamNode *
clientReplyContext::next() const
{
    assert ( (clientStreamNode*)http->client_stream.head->next->data == getNextNode());
    return getNextNode();
}

void
clientReplyContext::sendBodyTooLargeError()
{
    Ip::Address tmp_noaddr;
    tmp_noaddr.SetNoAddr(); // TODO: make a global const
    http->logType = LOG_TCP_DENIED_REPLY;
    ErrorState *err = clientBuildError(ERR_TOO_BIG, HTTP_FORBIDDEN, NULL,
                                       http->getConn() != NULL ? http->getConn()->clientConnection->remote : tmp_noaddr,
                                       http->request);
    removeClientStoreReference(&(sc), http);
    HTTPMSGUNLOCK(reply);
    startError(err);

}

/// send 412 (Precondition Failed) to client
void
clientReplyContext::sendPreconditionFailedError()
{
    http->logType = LOG_TCP_HIT;
    ErrorState *const err =
        clientBuildError(ERR_PRECONDITION_FAILED, HTTP_PRECONDITION_FAILED,
                         NULL, http->getConn()->clientConnection->remote, http->request);
    removeClientStoreReference(&sc, http);
    HTTPMSGUNLOCK(reply);
    startError(err);
}

/// send 304 (Not Modified) to client
void
clientReplyContext::sendNotModified()
{
    StoreEntry *e = http->storeEntry();
    const time_t timestamp = e->timestamp;
    HttpReply *const temprep = e->getReply()->make304();
    http->logType = LOG_TCP_IMS_HIT;
    removeClientStoreReference(&sc, http);
    createStoreEntry(http->request->method, RequestFlags());
    e = http->storeEntry();
    // Copy timestamp from the original entry so the 304
    // reply has a meaningful Age: header.
    e->timestampsSet();
    e->timestamp = timestamp;
    e->replaceHttpReply(temprep);
    e->complete();
    /*
     * TODO: why put this in the store and then serialise it and
     * then parse it again. Simply mark the request complete in
     * our context and write the reply struct to the client side.
     */
    triggerInitialStoreRead();
}

/// send 304 (Not Modified) or 412 (Precondition Failed) to client
/// depending on request method
void
clientReplyContext::sendNotModifiedOrPreconditionFailedError()
{
    if (http->request->method == METHOD_GET ||
            http->request->method == METHOD_HEAD)
        sendNotModified();
    else
        sendPreconditionFailedError();
}

void
clientReplyContext::processReplyAccess ()
{
    /* NP: this should probably soft-fail to a zero-sized-reply error ?? */
    assert(reply);

    /** Don't block our own responses or HTTP status messages */
    if (http->logType == LOG_TCP_DENIED ||
            http->logType == LOG_TCP_DENIED_REPLY ||
            alwaysAllowResponse(reply->sline.status)) {
        headers_sz = reply->hdr_sz;
        processReplyAccessResult(ACCESS_ALLOWED);
        return;
    }

    /** Check for reply to big error */
    if (reply->expectedBodyTooLarge(*http->request)) {
        sendBodyTooLargeError();
        return;
    }

    headers_sz = reply->hdr_sz;

    /** check for absent access controls (permit by default) */
    if (!Config.accessList.reply) {
        processReplyAccessResult(ACCESS_ALLOWED);
        return;
    }

    /** Process http_reply_access lists */
    ACLFilledChecklist *replyChecklist =
        clientAclChecklistCreate(Config.accessList.reply, http);
    replyChecklist->reply = HTTPMSGLOCK(reply);
    replyChecklist->nonBlockingCheck(ProcessReplyAccessResult, this);
}

void
clientReplyContext::ProcessReplyAccessResult(allow_t rv, void *voidMe)
{
    clientReplyContext *me = static_cast<clientReplyContext *>(voidMe);
    me->processReplyAccessResult(rv);
}

void
clientReplyContext::processReplyAccessResult(const allow_t &accessAllowed)
{
    debugs(88, 2, "The reply for " << RequestMethodStr(http->request->method)
           << " " << http->uri << " is " << accessAllowed << ", because it matched '"
           << (AclMatchedName ? AclMatchedName : "NO ACL's") << "'" );

    if (accessAllowed != ACCESS_ALLOWED) {
        ErrorState *err;
        err_type page_id;
        page_id = aclGetDenyInfoPage(&Config.denyInfoList, AclMatchedName, 1);

        http->logType = LOG_TCP_DENIED_REPLY;

        if (page_id == ERR_NONE)
            page_id = ERR_ACCESS_DENIED;

        Ip::Address tmp_noaddr;
        tmp_noaddr.SetNoAddr();
        err = clientBuildError(page_id, HTTP_FORBIDDEN, NULL,
                               http->getConn() != NULL ? http->getConn()->clientConnection->remote : tmp_noaddr,
                               http->request);

        removeClientStoreReference(&sc, http);

        HTTPMSGUNLOCK(reply);

        startError(err);

        return;
    }

    /* Ok, the reply is allowed, */
    http->loggingEntry(http->storeEntry());

    ssize_t body_size = reqofs - reply->hdr_sz;
    if (body_size < 0) {
        reqofs = reply->hdr_sz;
        body_size = 0;
    }

    debugs(88, 3, "clientReplyContext::sendMoreData: Appending " <<
           (int) body_size << " bytes after " << reply->hdr_sz <<
           " bytes of headers");

#if USE_SQUID_ESI

    if (http->flags.accel && reply->sline.status != HTTP_FORBIDDEN &&
            !alwaysAllowResponse(reply->sline.status) &&
            esiEnableProcessing(reply)) {
        debugs(88, 2, "Enabling ESI processing for " << http->uri);
        clientStreamInsertHead(&http->client_stream, esiStreamRead,
                               esiProcessStream, esiStreamDetach, esiStreamStatus, NULL);
    }

#endif

    if (http->request->method == METHOD_HEAD) {
        /* do not forward body for HEAD replies */
        body_size = 0;
        http->flags.done_copying = 1;
        flags.complete = 1;
    }

    assert (!flags.headersSent);
    flags.headersSent = true;

    StoreIOBuffer localTempBuffer;
    char *buf = next()->readBuffer.data;
    char *body_buf = buf + reply->hdr_sz;

    //Server side may disable ranges under some circumstances.

    if ((!http->request->range))
        next()->readBuffer.offset = 0;

    body_buf -= next()->readBuffer.offset;

    if (next()->readBuffer.offset != 0) {
        if (next()->readBuffer.offset > body_size) {
            /* Can't use any of the body we received. send nothing */
            localTempBuffer.length = 0;
            localTempBuffer.data = NULL;
        } else {
            localTempBuffer.length = body_size - next()->readBuffer.offset;
            localTempBuffer.data = body_buf + next()->readBuffer.offset;
        }
    } else {
        localTempBuffer.length = body_size;
        localTempBuffer.data = body_buf;
    }

    /* TODO??: move the data in the buffer back by the request header size */
    clientStreamCallback((clientStreamNode *)http->client_stream.head->data,
                         http, reply, localTempBuffer);

    return;
}

void
clientReplyContext::sendMoreData (StoreIOBuffer result)
{
    if (deleting)
        return;

    StoreEntry *entry = http->storeEntry();

    ConnStateData * conn = http->getConn();

    // too late, our conn is closing
    // TODO: should we also quit?
    if (conn == NULL) {
        debugs(33,3, "not sending more data to a closed connection" );
        return;
    }
    if (!conn->isOpen()) {
        debugs(33,3, "not sending more data to closing connection " << conn->clientConnection);
        return;
    }
    if (conn->pinning.zeroReply) {
        debugs(33,3, "not sending more data after a pinned zero reply " << conn->clientConnection);
        return;
    }

    char *buf = next()->readBuffer.data;

    if (buf != result.data) {
        /* we've got to copy some data */
        assert(result.length <= next()->readBuffer.length);
        memcpy(buf, result.data, result.length);
    }

    if (reqofs==0 && !logTypeIsATcpHit(http->logType) && Comm::IsConnOpen(conn->clientConnection)) {
        if (Ip::Qos::TheConfig.isHitTosActive()) {
            Ip::Qos::doTosLocalMiss(conn->clientConnection, http->request->hier.code);
        }
        if (Ip::Qos::TheConfig.isHitNfmarkActive()) {
            Ip::Qos::doNfmarkLocalMiss(conn->clientConnection, http->request->hier.code);
        }
    }

    /* We've got the final data to start pushing... */
    flags.storelogiccomplete = 1;

    reqofs += result.length;

    assert(reqofs <= HTTP_REQBUF_SZ || flags.headersSent);

    assert(http->request != NULL);

    /* ESI TODO: remove this assert once everything is stable */
    assert(http->client_stream.head->data
           && cbdataReferenceValid(http->client_stream.head->data));

    makeThisHead();

    debugs(88, 5, "clientReplyContext::sendMoreData: " << http->uri << ", " <<
           reqofs << " bytes (" << result.length <<
           " new bytes)");
    debugs(88, 5, "clientReplyContext::sendMoreData:"
           << conn->clientConnection <<
           " '" << entry->url() << "'" <<
           " out.offset=" << http->out.offset);

    /* update size of the request */
    reqsize = reqofs;

    if (errorInStream(result, reqofs)) {
        sendStreamError(result);
        return;
    }

    if (flags.headersSent) {
        pushStreamData (result, buf);
        return;
    }

    cloneReply();

    /* handle headers */

    if (Config.onoff.log_mime_hdrs) {
        size_t k;

        if ((k = headersEnd(buf, reqofs))) {
            safe_free(http->al->headers.reply);
            http->al->headers.reply = (char *)xcalloc(k + 1, 1);
            xstrncpy(http->al->headers.reply, buf, k);
        }
    }

    holdingBuffer = result;
    processReplyAccess();
    return;
}

/* Using this breaks the client layering just a little!
 */
void
clientReplyContext::createStoreEntry(const HttpRequestMethod& m, RequestFlags reqFlags)
{
    assert(http != NULL);
    /*
     * For erroneous requests, we might not have a h->request,
     * so make a fake one.
     */

    if (http->request == NULL)
        http->request = HTTPMSGLOCK(new HttpRequest(m, AnyP::PROTO_NONE, null_string));

    StoreEntry *e = storeCreateEntry(http->uri, http->log_uri, reqFlags, m);

    sc = storeClientListAdd(e, this);

#if USE_DELAY_POOLS
    sc->setDelayId(DelayId::DelayClient(http));
#endif

    reqofs = 0;

    reqsize = 0;

    /* I don't think this is actually needed! -- adrian */
    /* http->reqbuf = http->norm_reqbuf; */
    //    assert(http->reqbuf == http->norm_reqbuf);
    /* The next line is illegal because we don't know if the client stream
     * buffers have been set up
     */
    //    storeClientCopy(http->sc, e, 0, HTTP_REQBUF_SZ, http->reqbuf,
    //        SendMoreData, this);
    /* So, we mark the store logic as complete */
    flags.storelogiccomplete = 1;

    /* and get the caller to request a read, from whereever they are */
    /* NOTE: after ANY data flows down the pipe, even one step,
     * this function CAN NOT be used to manage errors
     */
    http->storeEntry(e);
}

ErrorState *
clientBuildError(err_type page_id, http_status status, char const *url,
                 Ip::Address &src_addr, HttpRequest * request)
{
    ErrorState *err = new ErrorState(page_id, status, request);
    err->src_addr = src_addr;

    if (url)
        err->url = xstrdup(url);

    return err;
}
