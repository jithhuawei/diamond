// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
// vim: set ts=4 sw=4:
/***********************************************************************
 *
 * store/common/frontend/cacheclient.cc:
 *   Single shard caching client implementation.
 *
 * Copyright 2015 Irene Zhang <iyzhang@cs.washington.edu>
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 **********************************************************************/

#include "cacheclient.h"

using namespace std;

CacheClient::CacheClient(TxnClient* txnclient)
{
    this->txnclient = txnclient;
}

CacheClient::~CacheClient() { }

/* Begins a transaction. */
void
CacheClient::Begin(const uint64_t tid)
{
    Debug("BEGIN [%lu]", tid);
    txnclient->Begin(tid);
}

void
CacheClient::BeginRO(const uint64_t tid, const Timestamp &timestamp)
{
    Debug("BEGIN [%lu]", tid);
    txnclient->BeginRO(tid, timestamp);
}

/* Get value for a key.
 * Returns 0 on success, else -1. */
void
CacheClient::Get(const uint64_t tid, const string &key, const Timestamp &timestamp, Promise *promise)
{
    Debug("GET %s", key.c_str());
    cache_lock.lock();

    Version value;
    // look for it in the cache
    if (cache.Get(key, timestamp, value)) {
        // Make some decision about the timestamp?
        cache_lock.unlock();
        
	Debug("CACHE HIT %s at ts %lu", key.c_str(), value.GetTimestamp());
        if (promise != NULL)
            promise->Reply(REPLY_OK, key, value);
        return;
    }

    cache_lock.unlock();
    // Otherwise, get latest value from server.
    Promise p(GET_TIMEOUT);
    Promise *pp = (promise != NULL) ? promise : &p;

    txnclient->Get(tid, key, timestamp, pp);
    if (pp->GetReply() == REPLY_OK) {
        Debug("Adding [%s] with ts %lu to the cache", key.c_str(), pp->GetValue(key).GetTimestamp());
        cache_lock.lock();
        cache.Put(key, pp->GetValue(key));
        cache_lock.unlock();
    }
}

void
CacheClient::MultiGet(const uint64_t tid, const vector<string> &keys, const Timestamp &timestamp, Promise *promise)
{
    Debug("MULTIGET %lu", keys.size());

    cache_lock.lock();

    map<string, Version> keysRead;
    vector<string> keysToRead;
    Version value;

    for (auto &key : keys) {
        // look for it in the cache
        if (cache.Get(key, timestamp, value)) {
            // Make some decision about the timestamp?

            keysRead[key] = value;  
        } else {
            keysToRead.push_back(key);
        }
    }
    cache_lock.unlock();
    
    if (keysRead.size() == keys.size()) {
        // we found everything in the cache. Hooray!
        if (promise != NULL)
            promise->Reply(REPLY_OK, keysRead);
        return;
    }
    
    Promise p(GET_TIMEOUT);
    Promise *pp = (promise != NULL) ? promise : &p;

    txnclient->MultiGet(tid, keysToRead, timestamp, pp);
    if (pp->GetReply() == REPLY_OK){
        map<string, Version> values = pp->GetValues();

        cache_lock.lock();
        for (auto &value : values) {
            Debug("Adding [%s] with ts %lu to the cache", value.first.c_str(), value.second.GetTimestamp());
            cache.Put(value.first, value.second);
            keysRead[value.first] = value.second;
        }
        cache_lock.unlock();
        pp->Reply(REPLY_OK, keysRead);
    }
}

/* Set value for a key. (Always succeeds).
 * Returns 0 on success, else -1. */
void
CacheClient::Put(const uint64_t tid, const string &key, const string &value, Promise *promise)
{
    Debug("PUT %s %s", key.c_str(), value.c_str());
    txnclient->Put(tid, key, value, promise);
}

/* Prepare the transaction. */
void
CacheClient::Prepare(const uint64_t tid, const Transaction &txn, Promise *promise)
{
    Debug("PREPARE [%lu]", tid);
    Promise p(COMMIT_TIMEOUT);
    Promise *pp = (promise != NULL) ? promise : &p;
    
    txnclient->Prepare(tid, txn, promise);

    // save the transactions for later
    if (pp->GetReply() == REPLY_OK) {
        cache_lock.lock();
        if (prepared.find(tid) == prepared.end()) {
            prepared[tid] = txn;
        }
        cache_lock.unlock();
    }
}

void
CacheClient::Commit(const uint64_t tid, const Transaction &txn, Promise *promise)
{
    Debug("COMMIT [%lu]", tid);
    Promise p(COMMIT_TIMEOUT);
    Promise *pp = (promise != NULL) ? promise : &p;
    txnclient->Commit(tid, txn, pp);

    // update the cache
    int reply = pp->GetReply();
    Transaction t;

    cache_lock.lock();
    if (prepared.find(tid) != prepared.end()) {
        t = prepared[tid];
        prepared.erase(tid);
    } else {
        t = txn;
    }
    
    // update the cache
    if (reply == REPLY_OK) {
        for (auto &write : t.GetWriteSet()) {
	    Debug("Adding [%s] with ts %lu to the cache", write.first.c_str(), pp->GetTimestamp());
            cache.Put(write.first, write.second, pp->GetTimestamp());
        }
    } else if (reply == REPLY_FAIL) {
        for (auto &read : t.GetReadSet()) {
            Debug("Removing [%s] from the cache", read.first.c_str());
            cache.Remove(read.first);
        }        
    }
    cache_lock.unlock();
}

/* Aborts the ongoing transaction. */
void
CacheClient::Abort(const uint64_t tid, Promise *promise)
{
    Debug("ABORT [%lu]", tid);

    Promise p(COMMIT_TIMEOUT);
    Promise *pp = (promise != NULL) ? promise : &p;

    txnclient->Abort(tid, pp);
    int reply = pp->GetReply();

    if (reply == REPLY_OK) {
        cache_lock.lock();
        if (prepared.find(tid) != prepared.end()) {
            // clear out the write set
            for (auto &read : prepared[tid].GetReadSet()) {
                Debug("Removing [%s] from the cache", read.first.c_str());
                cache.Remove(read.first);
            }
            prepared.erase(tid);
        }
        cache_lock.unlock();
    }
 }