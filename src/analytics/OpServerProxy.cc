/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include "viz_collector.h"
#include "viz_constants.h"
#include "OpServerProxy.h"
#include <tbb/mutex.h>
#include <boost/bind.hpp>
#include <boost/assign/list_of.hpp>
#include "base/util.h"
#include "base/logging.h"
#include "base/parse_object.h"
#include <cstdlib>
#include <utility>
#include "hiredis/hiredis.h"
#include "hiredis/base64.h"
#include "hiredis/boostasio.hpp"
#include <sandesh/sandesh.h>
#include <sandesh/common/vns_types.h>
#include <sandesh/common/vns_constants.h>

#include "rapidjson/document.h"

#include "redis_connection.h"
#include "redis_processor_vizd.h"
#include "viz_sandesh.h"
#include "viz_collector.h"
#include "redis_sentinel_client.h"

using std::string;
using boost::shared_ptr;
using boost::assign::list_of;
using boost::system::error_code;

class OpServerProxy::OpServerImpl {
    public:
        enum RacConnType {
            RAC_CONN_TYPE_INVALID = 0,
            RAC_CONN_TYPE_TO_OPS = 1,
            RAC_CONN_TYPE_FROM_OPS = 2,
        };

        enum RacStatus {
            RAC_UP = 1,
            RAC_DOWN = 2
        };

        static const char* RacStatusToString(RacStatus status) {
            switch(status) {
            case RAC_UP:
                return "Connected";
            case RAC_DOWN:
                return "Disconnected";
            default:
                return "Invalid";
            }
        }

        struct RedisMasterInfo {
            RedisMasterInfo() {
                rinfo_.set_ip("");
                rinfo_.set_port(0);
                rinfo_.set_master_last_updated(0);
                rinfo_.set_num_of_mastership_changes(0);
                rinfo_.set_delete_failed(0);
                rinfo_.set_delete_succeeded(0);
                rinfo_.set_delete_no_conn(0);
                rinfo_.set_update_failed(0);
                rinfo_.set_update_succeeded(0);
                rinfo_.set_update_no_conn(0);
                rinfo_.set_conn_cb_succeeded(0);
                rinfo_.set_conn_cb_failed(0);
                rinfo_.set_conn_cb_null(0);
                rinfo_.set_conn_call_disconnected(0);
                rinfo_.set_conn_call_succeeded(0);
                rinfo_.set_conn_call_failed(0);
            }

            void RedisMasterUpdate(const std::string& redis_ip, 
                                   unsigned short redis_port) {
                rinfo_.set_ip(redis_ip);
                rinfo_.set_port(redis_port);
                rinfo_.set_status(RacStatusToString(RAC_DOWN));
                rinfo_.set_master_last_updated(UTCTimestampUsec());
                rinfo_.set_num_of_mastership_changes(
                    rinfo_.get_num_of_mastership_changes()+1);
            }

            void RedisUveUpdate() {
                rinfo_.set_update_succeeded(rinfo_.get_update_succeeded()+1);
            }
            void RedisUveUpdateFail() {
                rinfo_.set_update_failed(rinfo_.get_update_failed()+1);
            }
            void RedisUveUpdateNoConn() {
                rinfo_.set_update_no_conn(rinfo_.get_update_no_conn()+1);
            }
            void RedisUveDelete() {
                rinfo_.set_delete_succeeded(rinfo_.get_delete_succeeded()+1);
            }
            void RedisUveDeleteFail() {
                rinfo_.set_delete_failed(rinfo_.get_delete_failed()+1);
            }
            void RedisUveDeleteNoConn() {
                rinfo_.set_delete_no_conn(rinfo_.get_delete_no_conn()+1);
            }

            void RedisStatusUpdate(RacStatus connection_status) {
                rinfo_.set_status(RacStatusToString(connection_status));
            }

            const std::string& GetIp() {
                return rinfo_.get_ip();
            }

            unsigned short GetPort() {
                return rinfo_.get_port();
            }
            
            RedisUveMasterInfo rinfo_;
        };

        void FillRedisUVEMasterInfo(RedisUveMasterInfo& redis_uve_info) {
            tbb::mutex::scoped_lock lock(rac_mutex_); 
            redis_uve_info = redis_uve_.rinfo_;
            if (to_ops_conn_) {
                redis_uve_info.set_conn_call_disconnected(to_ops_conn_->CallDisconnected());
                redis_uve_info.set_conn_call_failed(to_ops_conn_->CallFailed());
                redis_uve_info.set_conn_call_succeeded(to_ops_conn_->CallSucceeded()); 
                redis_uve_info.set_conn_cb_null(to_ops_conn_->CallbackNull());
                redis_uve_info.set_conn_cb_failed(to_ops_conn_->CallbackFailed());
                redis_uve_info.set_conn_cb_succeeded(to_ops_conn_->CallbackSucceeded());
            }
        }

        void ToOpsConnUpPostProcess() {
            processor_cb_proc_fn = boost::bind(&OpServerImpl::processorCallbackProcess, this, _1, _2, _3);
            to_ops_conn_.get()->SetClientAsyncCmdCb(processor_cb_proc_fn);

            string module = Sandesh::module();
            string source = Sandesh::source();
            string instance_id = Sandesh::instance_id();
            string node_type = Sandesh::node_type();
            
            if (!started_) {
                RedisProcessorExec::SyncDeleteUVEs(redis_uve_.GetIp(), 
                                                   redis_uve_.GetPort(),
                                                   source, node_type,
                                                   module, instance_id, "", 0);
                started_=true;
            }
            if (collector_) 
                collector_->RedisUpdate(true);
        }

        void ToOpsConnUp() {
            LOG(DEBUG, "ToOpsConnUp.. UP");
            {
                tbb::mutex::scoped_lock lock(rac_mutex_); 
                redis_uve_.RedisStatusUpdate(RAC_UP);
            }
            evm_->io_service()->post(boost::bind(&OpServerProxy::OpServerImpl::ToOpsConnUpPostProcess, this));
        }

        void FromOpsConnUpPostProcess() {
            analytics_cb_proc_fn = boost::bind(&OpServerImpl::analyticsCallbackProcess, this, _1, _2, _3);
            from_ops_conn_.get()->SetClientAsyncCmdCb(analytics_cb_proc_fn);
            from_ops_conn_.get()->RedisAsyncCommand(NULL, "SUBSCRIBE analytics");
        }

        void FromOpsConnUp() {
            LOG(DEBUG, "FromOpsConnUp.. UP");

            evm_->io_service()->post(boost::bind(&OpServerProxy::OpServerImpl::FromOpsConnUpPostProcess, this));
        }

        void RAC_ConnectProcess(RacConnType type) {
            if (type == RAC_CONN_TYPE_TO_OPS) {
                LOG(DEBUG, "Retry Connect to FromOpsConn");
                to_ops_conn_.get()->RAC_Connect();
            } else if (type == RAC_CONN_TYPE_FROM_OPS) {
                from_ops_conn_.get()->RAC_Connect();
            }
        }

        void ToOpsConnDown() {
            LOG(DEBUG, "ToOpsConnDown.. DOWN.. Reconnect..");
            {
                tbb::mutex::scoped_lock lock(rac_mutex_);
                redis_uve_.RedisStatusUpdate(RAC_DOWN);
            }
            collector_->RedisUpdate(false);
            evm_->io_service()->post(boost::bind(&OpServerProxy::OpServerImpl::RAC_ConnectProcess,
                        this, RAC_CONN_TYPE_TO_OPS));
        }

        void FromOpsConnDown() {
            LOG(DEBUG, "FromOpsConnDown.. DOWN.. Reconnect..");
            evm_->io_service()->post(boost::bind(&OpServerProxy::OpServerImpl::RAC_ConnectProcess,
                        this, RAC_CONN_TYPE_FROM_OPS));
        }

        void HandleRedisMasterUpdate(const std::string& service, 
                                     const std::string& redis_ip,
                                     unsigned short redis_port) {
            tbb::mutex::scoped_lock lock(rac_mutex_);
            to_ops_conn_.reset();
            from_ops_conn_.reset();
            redis_uve_.RedisMasterUpdate(redis_ip, redis_port);
            to_ops_conn_.reset(new RedisAsyncConnection(evm_, 
                redis_ip, redis_port, 
                boost::bind(&OpServerProxy::OpServerImpl::ToOpsConnUp, this),
                boost::bind(&OpServerProxy::OpServerImpl::ToOpsConnDown, this)));
            to_ops_conn_.get()->RAC_Connect();
            from_ops_conn_.reset(new RedisAsyncConnection(evm_, 
                redis_ip, redis_port, 
                boost::bind(&OpServerProxy::OpServerImpl::FromOpsConnUp, this),
                boost::bind(&OpServerProxy::OpServerImpl::FromOpsConnDown, this)));
            from_ops_conn_.get()->RAC_Connect();
        }

        void processorCallbackProcess(const redisAsyncContext *c, void *r, void *privdata) {
            redisReply *reply = (redisReply*)r;
            RedisProcessorIf * rpi = NULL;

            if (privdata)
                rpi = reinterpret_cast<RedisProcessorIf *>(privdata);

            if (reply == NULL) {
                LOG(DEBUG, "NULL Reply...\n");
                return;
            }

            if (rpi) {
                rpi->ProcessCallback(reply);
            }

        }


        void analyticsCallbackProcess(const redisAsyncContext *c, void *r, void *privdata) {
            redisReply *reply = (redisReply*)r;

            LOG(DEBUG, "Received data on analytics channel from REDIS...\n");

            if (reply == NULL) {
                LOG(DEBUG, "NULL Reply...\n");
                return;
            }

            if (reply->type == REDIS_REPLY_ARRAY) {
                LOG(DEBUG, "REDIS_REPLY_ARRAY == " << reply->elements);
                int i;
                for (i = 0; i < (int)reply->elements; i++) {
                    if (reply->element[i]->type == REDIS_REPLY_STRING) {
                        LOG(DEBUG, "Element" << i << "== " << reply->element[i]->str);
                    } else {
                        LOG(DEBUG, "Element" << i << " type == " << reply->element[i]->type);
                    }
                }
            } else if (reply->type == REDIS_REPLY_STRING) {
                LOG(DEBUG, "REDIS_REPLY_STRING == " << reply->str);
                return;
            } else {
                LOG(DEBUG, "reply->type == " << reply->type);
                return;
            }

            assert(reply->type == REDIS_REPLY_ARRAY);
            assert(reply->elements == 3);

            if (!strncmp(reply->element[0]->str, "subscribe", strlen("subscribe"))) {
                /* nothing to do, return */
                return;
            }

            assert(!strncmp(reply->element[0]->str, "message", strlen("message")));
            assert(!strncmp(reply->element[1]->str, "analytics", strlen("analytics")));
            assert(reply->element[2]->type == REDIS_REPLY_STRING);
            std::string message = base64_decode(reply->element[2]->str);
            //std::string message(reply->element[2]->str);

            LOG(DEBUG, "message ==" << reply->element[2]->str);

            rapidjson::Document document;	// Default template parameter uses UTF8 and MemoryPoolAllocator.
            if (document.ParseInsitu<0>(reply->element[2]->str).HasParseError()) {
                assert(0);
            }
            assert(document.HasMember("type"));
            assert(document["type"].IsString());

            assert(document.HasMember("destination"));
            assert(document["destination"].IsString());
            std::string destination(document["destination"].GetString());

            assert(document.HasMember("message"));
            assert(document["message"].IsString());
            std::string enc_sandesh(document["message"].GetString());

            std::string dec_sandesh = base64_decode(enc_sandesh);
            //std::string dec_sandesh(enc_sandesh);

            LOG(DEBUG, "decoded sandesh_message ==" << dec_sandesh);

            collector_->SendRemote(destination, dec_sandesh);
        }

        shared_ptr<RedisAsyncConnection> to_ops_conn() {
            tbb::mutex::scoped_lock lock(rac_mutex_);
            return to_ops_conn_;
        }

        shared_ptr<RedisAsyncConnection> from_ops_conn() {
            tbb::mutex::scoped_lock lock(rac_mutex_);
            return from_ops_conn_;
        }

        OpServerImpl(EventManager *evm, VizCollector *collector,
                     const std::string & redis_sentinel_ip, 
                     unsigned short redis_sentinel_port) :
            evm_(evm),
            collector_(collector),
            started_(false),
            analytics_cb_proc_fn(NULL),
            processor_cb_proc_fn(NULL) {
            RedisSentinelClient::RedisServices services;
            services.push_back("mymaster");
            redis_sentinel_client_.reset(new RedisSentinelClient(evm, 
                redis_sentinel_ip, redis_sentinel_port, services,
                boost::bind(&OpServerProxy::OpServerImpl::HandleRedisMasterUpdate, 
                            this, _1, _2, _3)));
        }

        ~OpServerImpl() {
        }

        RedisMasterInfo redis_uve_;
    private:
        /* these are made public, so they are accessed by OpServerProxy */
        EventManager *evm_;
        VizCollector *collector_;
        bool started_;
        boost::scoped_ptr<RedisSentinelClient> redis_sentinel_client_;
        shared_ptr<RedisAsyncConnection> to_ops_conn_;
        shared_ptr<RedisAsyncConnection> from_ops_conn_;
        RedisAsyncConnection::ClientAsyncCmdCbFn analytics_cb_proc_fn;
        RedisAsyncConnection::ClientAsyncCmdCbFn processor_cb_proc_fn;
        tbb::mutex rac_mutex_;
};

OpServerProxy::OpServerProxy(EventManager *evm, VizCollector *collector,
                             const std::string & redis_sentinel_ip, 
                             unsigned short redis_sentinel_port,
                             int gen_timeout) :
            gen_timeout_(gen_timeout) {
    impl_ = new OpServerImpl(evm, collector, redis_sentinel_ip, 
                             redis_sentinel_port);
}

OpServerProxy::~OpServerProxy() {
    if (impl_)
        delete impl_;
}

bool
OpServerProxy::UVEUpdate(const std::string &type, const std::string &attr,
                       const std::string &source, const std::string &node_type,
                       const std::string &module, 
                       const std::string &instance_id,
                       const std::string &key, const std::string &message,
                       int32_t seq, const std::string& agg, 
                       const std::string& atyp, int64_t ts) {

    shared_ptr<RedisAsyncConnection> prac = impl_->to_ops_conn();
    if (!prac) {
        impl_->redis_uve_.RedisUveUpdateNoConn();
        return false;
    }

    bool ret = RedisProcessorExec::UVEUpdate(prac.get(), NULL, type, attr,
            source, node_type, module, instance_id, key, message, seq, agg, atyp, ts);
    ret ? impl_->redis_uve_.RedisUveUpdate() : impl_->redis_uve_.RedisUveUpdateFail(); 
    return ret;
}

bool
OpServerProxy::UVEDelete(const std::string &type,
                       const std::string &source, const std::string &node_type,
                       const std::string &module, 
                       const std::string &instance_id,
                       const std::string &key, int32_t seq) {

    shared_ptr<RedisAsyncConnection> prac = impl_->to_ops_conn();
    if (!prac) {
        impl_->redis_uve_.RedisUveDeleteNoConn();
        return false;
    }

    bool ret = RedisProcessorExec::UVEDelete(prac.get(), NULL, type, source, 
            node_type, module, instance_id, key, seq);
    ret ? impl_->redis_uve_.RedisUveDelete() : impl_->redis_uve_.RedisUveDeleteFail(); 
    return ret;
}

bool 
OpServerProxy::GetSeq(const string &source, const string &node_type, 
        const string &module, const string &instance_id,
        std::map<std::string,int32_t> & seqReply) {

    shared_ptr<RedisAsyncConnection> prac = impl_->to_ops_conn();
    if  (!(prac && prac->IsConnUp())) return false;

    if (!impl_->to_ops_conn()) return false;
    VizSandeshContext * vsc = static_cast<VizSandeshContext *>(Sandesh::client_context());
    string coll;
    if (vsc)
        coll = vsc->Analytics()->name();
    else 
        coll = Sandesh::source();

    return RedisProcessorExec::SyncGetSeq(impl_->redis_uve_.GetIp(), 
            impl_->redis_uve_.GetPort(), source, node_type, module, 
            instance_id, coll, gen_timeout_, seqReply);
}

bool 
OpServerProxy::DeleteUVEs(const string &source, const string &module,
                          const string &node_type, const string &instance_id) {

    shared_ptr<RedisAsyncConnection> prac = impl_->to_ops_conn();
    if  (!(prac && prac->IsConnUp())) return false;

    VizSandeshContext * vsc = static_cast<VizSandeshContext *>(Sandesh::client_context());
    string coll;
    if (vsc)
        coll = vsc->Analytics()->name();
    else 
        coll = Sandesh::source();

    return RedisProcessorExec::SyncDeleteUVEs(impl_->redis_uve_.GetIp(), 
            impl_->redis_uve_.GetPort(), source, node_type, module, instance_id,
            coll, gen_timeout_);
}

bool 
OpServerProxy::GeneratorCleanup(GenCleanupReply gcr) {
    shared_ptr<RedisAsyncConnection> prac = impl_->to_ops_conn();
    if  (!(prac && prac->IsConnUp())) return false;

    GenCleanupReq * dr = new GenCleanupReq(prac.get(),
            boost::bind(gcr, _2));
    return dr->RedisSend();
}

bool
OpServerProxy::RefreshGenerator(const std::string &source, 
                                const std::string &node_type,
                                const std::string &module,
                                const std::string &instance_id) {

    if (!gen_timeout_) return true;

    shared_ptr<RedisAsyncConnection> prac = impl_->to_ops_conn();
    if  (!(prac && prac->IsConnUp())) return false;


    VizSandeshContext * vsc = static_cast<VizSandeshContext *>(Sandesh::client_context());
    string coll;
    if (vsc)
        coll = vsc->Analytics()->name();
    else 
        coll = Sandesh::source();

    RedisProcessorExec::RefreshGenerator(prac.get(), source, node_type, 
                                         module, instance_id,
                                         coll, gen_timeout_); 

    return true;    
}

bool
OpServerProxy::WithdrawGenerator(const std::string &source, 
                                 const std::string &node_type,
                                 const std::string &module,
                                 const std::string &instance_id) {
    shared_ptr<RedisAsyncConnection> prac = impl_->to_ops_conn();
    if  (!(prac && prac->IsConnUp())) return false;

    VizSandeshContext * vsc = static_cast<VizSandeshContext *>(Sandesh::client_context());
    string coll;
    if (vsc)
        coll = vsc->Analytics()->name();
    else 
        coll = Sandesh::source();

    RedisProcessorExec::WithdrawGenerator(prac.get(), source, node_type, 
                                          module, instance_id, coll);

    return true;
}

void 
OpServerProxy::FillRedisUVEMasterInfo(RedisUveMasterInfo& redis_uve_info) {
    impl_->FillRedisUVEMasterInfo(redis_uve_info);
}

void 
RedisUVEMasterRequest::HandleRequest() const {
    RedisUVEMasterResponse *resp(new RedisUVEMasterResponse);
    VizSandeshContext *vsc = static_cast<VizSandeshContext *>(
                                        Sandesh::client_context());
    assert(vsc); 
    RedisUveMasterInfo redis_uve_info; 
    vsc->Analytics()->GetOsp()->FillRedisUVEMasterInfo(redis_uve_info);
    resp->set_redis_uve_master(redis_uve_info);
    resp->set_context(context());
    resp->Response();
}

