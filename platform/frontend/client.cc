// -*- mode: c++; c-file-style: "k&r"; c-basic-offset: 4 -*-
// vim: set ts=4 sw=4:
/***********************************************************************
 *
 * frontend/client.cc
 *   Client for talking to Diamond frontend server
 *
 **********************************************************************/

#include "client.h"

using namespace std;

namespace diamond {
namespace frontend {

using namespace proto;
using namespace std;
    
Client::Client(const string &configPath, Transport *transport,
	       uint64_t client_id) :
    transport(transport), client_id(client_id)
{ 
    ifstream configStream(configPath);
    if (configStream.fail()) {
        fprintf(stderr, "unable to read configuration file: %s\n",
                configPath.c_str());
    }
    init(new transport::Configuration(configStream));
}

Client::Client(const string &hostname, const string &port, Transport *transport,
	       uint64_t client_id) :
    transport(transport), client_id(client_id)
{ 
    transport::ReplicaAddress frontendAddress(hostname, port);
    vector<transport::ReplicaAddress> addresses;
    addresses.push_back(frontendAddress);
    init(new transport::Configuration(1, 0, addresses));
}

void
Client::init(transport::Configuration *transportConfig) {
    config = transportConfig;
    transport->Register(this, *config, -1);
    
    msgid = 0;
}

Client::~Client() 
{ 
    delete config;
}

void
Client::Begin(const uint64_t tid)
{
    Debug("BEGIN [%lu]", tid);
}

void
Client::BeginRO(const uint64_t tid, const Timestamp &timestamp)
{
    Debug("BEGIN READ-ONLY [%lu]", tid);
}

void
Client::Get(const uint64_t tid,
            const string &key,
            const Timestamp &timestamp,
            Promise *promise)
{
    Debug("Sending GET [%s]", key.c_str());

    vector<string> keys;
    keys.push_back(key);
    MultiGet(tid, keys, timestamp, promise);
}

void
Client::MultiGet(const uint64_t tid,
                 const vector<string> &keys,
                 const Timestamp &timestamp,
                 Promise *promise)
{
    Debug("Sending MULTIGET [%lu keys] at %lu", keys.size(), timestamp);

    // Fill out protobuf
    // Send message
    transport->Timer(0, [=]() {
            GetMessage msg;
            msg.set_clientid(client_id);
            for (auto key : keys) {
                msg.add_keys(key);
            }
            msg.set_txnid(tid);
            msg.set_timestamp(timestamp);
            msg.set_msgid(msgid++);
            if (transport->SendMessageToReplica(this, 0, msg)) {
                if (promise != NULL)
                    waiting[msg.msgid()] = promise;
            } else if (promise != NULL) {
                promise->Reply(REPLY_NETWORK_FAILURE);
            }
        });
}

void
Client::Put(const uint64_t tid,
            const string &key, 
            const string &value,
            Promise *promise)
{
    Debug("Sending PUT [%s %s]", key.c_str(), value.c_str());
    Panic("Don't support PUT");
}

void
Client::Prepare(const uint64_t tid,
                const Transaction &txn,
                Promise *promise)
{
    Debug("Ignore PREPARE");
    Panic("Don't support PREPARE");
}

void
Client::Commit(const uint64_t tid,
               const Transaction &txn,
               Promise *promise)
{
    // If SI with no writes or read-only, just locally check the read set
    // Commit all reads locally
    if ((txn.IsolationMode() == READ_ONLY) ||
        ((txn.IsolationMode() == SNAPSHOT_ISOLATION) &&
         txn.GetWriteSet().empty() &&
         txn.GetIncrementSet().empty())) {
        // Run local checks
        Interval i(0);
        for (auto &read : txn.GetReadSet()) {
            Intersect(i, read.second);
        }
        if (i.Start() <= i.End()) {
            if (promise != NULL) promise->Reply(REPLY_OK, txn.GetTimestamp());
        } else {
            if (promise != NULL)  promise->Reply(REPLY_FAIL);
	}
        return;
    }
    
    // If eventual consistency, do no checks and don't wait for a response
    //if (txn.IsolationMode() == EVENTUAL) {
    //    if (promise != NULL) {
    //        promise->Reply(REPLY_OK);
    //    }
    //    promise = NULL;
    //}
    
    Debug("Sending COMMIT (mode=%i, t=%lu", txn.IsolationMode(), txn.GetTimestamp());
    

    // Send messages
    transport->Timer(0, [=]() {
            CommitMessage msg;
            msg.set_txnid(tid);
            txn.Serialize(msg.mutable_txn());
            msg.set_clientid(client_id);
            msg.set_msgid(msgid++);
            if (transport->SendMessageToReplica(this, 0, msg)) {
		if (promise != NULL)
		    waiting[msg.msgid()] = promise;
            } else if (promise != NULL) {
                promise->Reply(REPLY_NETWORK_FAILURE);
            }
        });
}

void
Client::Abort(const uint64_t tid,
              Promise *promise)
{
    Debug("Sending ABORT");


    // Send messages
    transport->Timer(0, [=]() {
            AbortMessage msg;
            msg.set_txnid(tid);
            msg.set_clientid(client_id);
            msg.set_msgid(msgid++);
            if (transport->SendMessageToReplica(this, 0, msg)) {
                if (promise != NULL) 
                    waiting[msg.msgid()] = promise;
            } else if (promise != NULL) {
                promise->Reply(REPLY_NETWORK_FAILURE);
            }
        });
}

// Callbacks from the messages that happen in the transport thread
void
Client::ReceiveMessage(const TransportAddress &remote,
                       const string &type,
                       const string &data)
{
    Debug("Received reply type: %s", type.c_str());

    static GetReply getReply;
    static CommitReply commitReply;
    static AbortReply abortReply;
    static Notification notification;
    static RegisterReply regReply;
    static DeregisterReply deregReply;

    if (type == getReply.GetTypeName()) {
        // Handle Get
        getReply.ParseFromString(data);
        auto it = waiting.find(getReply.msgid());

        if (it != waiting.end()) {
            Debug("Received GET response [%u] %i", getReply.msgid(), getReply.status());
            map<string, Version> ret;
            int status = getReply.status(); 
            if (status == REPLY_OK) {
                for (int i = 0; i < getReply.replies_size(); i++) {
                    string key = getReply.replies(i).key();
                    Version v = Version(getReply.replies(i));
                    ret[key] = v;
                    ASSERT(v.GetInterval().End() != MAX_TIMESTAMP);
                    Debug("Received Get timestamp %s %lu", key.c_str(), v.GetInterval().End());
                }
            }
            it->second->Reply(status, ret);
            waiting.erase(it);
        }
    } else if (type == commitReply.GetTypeName()) {
        commitReply.ParseFromString(data);
        auto it = waiting.find(commitReply.msgid());

        if (it != waiting.end() && it->second != NULL) {
	    Debug("Received COMMIT response [%u] %i", commitReply.msgid(), commitReply.status());
            it->second->Reply(commitReply.status(), commitReply.timestamp());
            waiting.erase(it);
        }
    } else if (type == abortReply.GetTypeName()) {
        abortReply.ParseFromString(data);
        auto it = waiting.find(abortReply.msgid());

        if (it != waiting.end() && it->second != NULL) {
	    Debug("Received ABORT response [%u]", abortReply.msgid());
            it->second->Reply(abortReply.status());
            waiting.erase(it);
        }
    } else if (type == notification.GetTypeName()) {
        notification.ParseFromString(data);
        uint64_t reactive_id = notification.reactiveid();
        Timestamp timestamp = notification.timestamp();
        Debug("Received NOTIFICATION (reactive_id %lu, timestamp %lu)", reactive_id, timestamp);

        // Ack notification
        ReplyToNotification(reactive_id, timestamp);

        if (timestamp > last_timestamp[reactive_id]) {
            last_timestamp[reactive_id] = timestamp;
            map<string, Version> cache_entries;
            for (int i = 0; i < notification.replies_size(); i++) {
                string key = notification.replies(i).key();
                Version value(notification.replies(i));
                cache_entries[key] = value;
            }

            // Let client know that a notification has arrived (for clients who call GetNextNotification in non-blocking mode)
            if (callback_registered) {
                notification_callback();
            }

            notification_lock.lock();
            if (reactive_promise != NULL) { // Directly signal reactive thread if it's waiting on a notification
                Promise * old_reactive_promise = reactive_promise;
                reactive_promise = NULL;
                notification_lock.unlock();
                old_reactive_promise->Reply(REPLY_OK, timestamp, cache_entries, reactive_id);
            }
            else { // Otherwise, put the notification in the "pending notifications" queue
                pending_notifications.push(notification);
                notification_lock.unlock();
            }
        }
        else {
            Debug("Notification is stale (timestamp %lu, last_timestamp %lu)", timestamp, last_timestamp[reactive_id]);
        }
    } else if (type == regReply.GetTypeName()) {
        regReply.ParseFromString(data);
        auto it = waiting.find(regReply.msgid());

        if (it != waiting.end() && it->second != NULL) {
	    Debug("Received REGISTER response [%u] %i", regReply.msgid(), regReply.status());
            it->second->Reply(regReply.status());
            waiting.erase(it);
        }
    } else if (type == deregReply.GetTypeName()) {
        deregReply.ParseFromString(data);
        auto it = waiting.find(deregReply.msgid());

        if (it != waiting.end() && it->second != NULL) {
	    Debug("Received DEREGISTER response [%u] %i", deregReply.msgid(), deregReply.status());
            it->second->Reply(deregReply.status());
            waiting.erase(it);
        }
    }
}

void
Client::GetNextNotification(bool blocking, Promise *promise) {
    notification_lock.lock();
    if (reactive_promise != NULL) {
        Panic("Error: GetNextNotification called from multiple threads");
    }
    if (!pending_notifications.empty()) {
        Notification notification = pending_notifications.front();
        pending_notifications.pop();
        notification_lock.unlock();
        map<string, Version> cache_entries;
        for (int i = 0; i < notification.replies_size(); i++) {
            string key = notification.replies(i).key();
            Version value(notification.replies(i));
            cache_entries[key] = value;
        }
        promise->Reply(REPLY_OK, notification.timestamp(), cache_entries, notification.reactiveid());
    }
    else if (!blocking) {
        notification_lock.unlock();
        map<string, Version> dummyMap;
        promise->Reply(REPLY_OK, 0, dummyMap, NO_NOTIFICATION);
    }
    else {
        reactive_promise = promise;
        notification_lock.unlock();
    }
}

void
Client::Register(const uint64_t reactive_id,
                 const Timestamp timestamp,
                 const std::set<std::string> &keys,
                 Promise *promise) {
    Debug("Sending REGISTER for reactive_id %lu at timestamp %lu", reactive_id, timestamp);
    for (auto &key : keys) {
        Debug("Registering key: %s", key.c_str());
    }
    
    // Send message
    transport->Timer(0, [=]() {
            RegisterMessage msg;
            msg.set_clientid(client_id);
            for (auto &key : keys) {
                msg.add_keys(key);
            }
            msg.set_reactiveid(reactive_id);
            msg.set_timestamp(timestamp);
            msg.set_msgid(msgid++);
            if (transport->SendMessageToReplica(this, 0, msg)) {
                if (promise != NULL)
                    waiting[msg.msgid()] = promise;
            } else if (promise != NULL) {
                promise->Reply(REPLY_NETWORK_FAILURE);
            }
        });
}

void
Client::Deregister(const uint64_t reactive_id,
                   Promise *promise) {
    Debug("Sending DEREGISTER for reactive_id %lu", reactive_id);
    // Send message
    transport->Timer(0, [=]() {
            DeregisterMessage msg;
            msg.set_clientid(client_id);
            msg.set_reactiveid(reactive_id);
            msg.set_msgid(msgid++);
            if (transport->SendMessageToReplica(this, 0, msg)) {
                if (promise != NULL)
                    waiting[msg.msgid()] = promise;
            } else if (promise != NULL) {
                promise->Reply(REPLY_NETWORK_FAILURE);
            }
        });
}

void
Client::Subscribe(const std::set<std::string> &keys,
                  const TransportAddress &address,
                  Promise *promise) {
    Panic("Subscribe not implemented for this client");
}

void
Client::ReplyToNotification(const uint64_t reactive_id,
                            const Timestamp timestamp) {
    Debug("Sending NOTIFICATION_REPLY for reactive_id %lu at timestamp %lu", reactive_id, timestamp);

    // Send message
    transport->Timer(0, [=]() {
            NotificationReply msg;
            msg.set_clientid(client_id);
            msg.set_reactiveid(reactive_id);
            msg.set_timestamp(timestamp);
            msg.set_msgid(msgid++);
	    transport->SendMessageToReplica(this, 0, msg);
	});
}

void
Client::NotificationInit(std::function<void (void)> callback) {
    notification_callback = callback;
    callback_registered = true;
}

} // namespace frontend
} // namespace diamond
