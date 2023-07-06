// Copyright 2017 The Ray Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <boost/asio.hpp>
#include <chrono>

#include "absl/synchronization/mutex.h"
#include "ray/common/asio/instrumented_io_context.h"
#include "ray/common/grpc_util.h"
#include "ray/common/id.h"
#include "ray/common/status.h"
#include "ray/util/util.h"

namespace ray {
namespace rpc {

/// Represents an outgoing gRPC request.
///
/// NOTE(hchen): Compared to `ClientCallImpl`, this abstract interface doesn't use
/// template. This allows the users (e.g., `ClientCallMangager`) not having to use
/// template as well.
class ClientCall {
 public:
  /// The callback to be called by `ClientCallManager` when the reply of this request is
  /// received.
  virtual void OnReplyReceived() = 0;
  /// Return status.
  virtual ray::Status GetStatus() = 0;
  /// Set return status.
  virtual void SetReturnStatus() = 0;
  /// Get stats handle for this RPC (for recording end).
  virtual std::shared_ptr<StatsHandle> GetStatsHandle() = 0;

  virtual ~ClientCall() = default;
};

class ClientCallManager;

/// Represents the client callback function of a particular rpc method.
///
/// \tparam Reply Type of the reply message.
template <class Reply>
using ClientCallback = std::function<void(const Status &status, const Reply &reply)>;

/// Implementation of the `ClientCall`. It represents a `ClientCall` for a particular
/// RPC method.
///
/// \tparam Reply Type of the Reply message.
template <class Reply>
class ClientCallImpl : public ClientCall {
 public:
  /// Constructor.
  ///
  /// \param[in] callback The callback function to handle the reply.
  explicit ClientCallImpl(const ClientCallback<Reply> &callback,
                          const ClusterID &cluster_id,
                          std::shared_ptr<StatsHandle> stats_handle,
                          int64_t timeout_ms = -1)
      : callback_(std::move(const_cast<ClientCallback<Reply> &>(callback))),
        stats_handle_(std::move(stats_handle)) {
    if (timeout_ms != -1) {
      auto deadline =
          std::chrono::system_clock::now() + std::chrono::milliseconds(timeout_ms);
      context_.set_deadline(deadline);
    }
    if (!cluster_id.IsNil()) {
      context_.AddMetadata(kClusterIdKey, cluster_id.Hex());
    }
  }

  Status GetStatus() override {
    absl::MutexLock lock(&mutex_);
    return return_status_;
  }

  void SetReturnStatus() override {
    absl::MutexLock lock(&mutex_);
    return_status_ = GrpcStatusToRayStatus(status_);
  }

  void OnReplyReceived() override {
    ray::Status status;
    {
      absl::MutexLock lock(&mutex_);
      status = return_status_;
    }
    if (callback_ != nullptr) {
      callback_(status, reply_);
    }
  }

  std::shared_ptr<StatsHandle> GetStatsHandle() override { return stats_handle_; }

 private:
  /// The reply message.
  Reply reply_;

  /// The callback function to handle the reply.
  ClientCallback<Reply> callback_;

  /// The stats handle tracking this RPC.
  std::shared_ptr<StatsHandle> stats_handle_;

  /// The response reader.
  std::unique_ptr<grpc::ClientAsyncResponseReader<Reply>> response_reader_;

  /// gRPC status of this request.
  grpc::Status status_;

  /// Mutex to protect the return_status_ field.
  absl::Mutex mutex_;

  /// This is the status to be returned from GetStatus(). It is safe
  /// to read from other threads while they hold mutex_. We have
  /// return_status_ = GrpcStatusToRayStatus(status_) but need
  /// a separate variable because status_ is set internally by
  /// GRPC and we cannot control it holding the lock.
  ray::Status return_status_ GUARDED_BY(mutex_);

  /// Context for the client. It could be used to convey extra information to
  /// the server and/or tweak certain RPC behaviors.
  grpc::ClientContext context_;

  friend class ClientCallManager;
};

/// Represents the generic signature of a `FooService::Stub::PrepareAsyncBar`
/// function, where `Foo` is the service name and `Bar` is the rpc method name.
///
/// \tparam GrpcService Type of the gRPC-generated service class.
/// \tparam Request Type of the request message.
/// \tparam Reply Type of the reply message.
template <class GrpcService, class Request, class Reply>
using PrepareAsyncFunction = std::unique_ptr<grpc::ClientAsyncResponseReader<Reply>> (
    GrpcService::Stub::*)(grpc::ClientContext *context,
                          const Request &request,
                          grpc::CompletionQueue *cq);

/// `ClientCallManager` is used to manage outgoing gRPC requests and the lifecycles of
/// `ClientCall` objects.
///
/// It maintains a thread that keeps polling events from `CompletionQueue`, and post
/// the callback function to the main event loop when a reply is received.
///
/// Multiple clients can share one `ClientCallManager`.
class ClientCallManager {
 public:
  /// Constructor.
  ///
  /// \param[in] main_service The main event loop, to which the callback functions will be
  /// posted.
  explicit ClientCallManager(instrumented_io_context &main_service,
                             const ClusterID &cluster_id = ClusterID::Nil(),
                             int num_threads = 1,
                             int64_t call_timeout_ms = -1)
      : cluster_id_(ClusterID::Nil()),
        main_service_(main_service),
        num_threads_(num_threads),
        shutdown_(false),
        call_timeout_ms_(call_timeout_ms) {
    rr_index_ = rand() % num_threads_;
    // Start the polling threads.
    cqs_.reserve(num_threads_);
    for (int i = 0; i < num_threads_; i++) {
      cqs_.push_back(std::make_unique<grpc::CompletionQueue>());
      polling_threads_.emplace_back(
          &ClientCallManager::PollEventsFromCompletionQueue, this, i);
    }
  }

  ~ClientCallManager() {
    shutdown_ = true;
    for (auto &cq : cqs_) {
      cq->Shutdown();
    }
    for (auto &polling_thread : polling_threads_) {
      polling_thread.join();
    }
  }

  /// Create a new `ClientCall` and send request.
  ///
  /// \tparam GrpcService Type of the gRPC-generated service class.
  /// \tparam Request Type of the request message.
  /// \tparam Reply Type of the reply message.
  ///
  /// \param[in] stub The gRPC-generated stub.
  /// \param[in] prepare_async_function Pointer to the gRPC-generated
  /// `FooService::Stub::PrepareAsyncBar` function.
  /// \param[in] request The request message.
  /// \param[in] callback The callback function that handles reply.
  /// \param[in] call_name The name of the gRPC method call.
  /// \param[in] method_timeout_ms The timeout of the RPC method in ms.
  /// -1 means it will use the default timeout configured for the handler.
  ///
  /// \return A `ClientCall` representing the request that was just sent.
  template <class GrpcService, class Request, class Reply>
  void CreateCall(
      typename GrpcService::Stub &stub,
      const PrepareAsyncFunction<GrpcService, Request, Reply> prepare_async_function,
      const Request &request,
      const ClientCallback<Reply> &callback,
      std::string call_name,
      int64_t method_timeout_ms = -1) {
    auto stats_handle = main_service_.stats().RecordStart(call_name);
    if (method_timeout_ms == -1) {
      method_timeout_ms = call_timeout_ms_;
    }

    auto call = new ClientCallImpl<Reply>(
        callback, cluster_id_.load(), std::move(stats_handle), method_timeout_ms);
    // Send request.
    // Find the next completion queue to wait for response.
    call->response_reader_ = (stub.*prepare_async_function)(
        &call->context_, request, cqs_[rr_index_++ % num_threads_].get());
    call->response_reader_->StartCall();
    // Create a new tag object. This object will eventually be deleted in the
    // `ClientCallManager::PollEventsFromCompletionQueue` when reply is received.
    call->response_reader_->Finish(&call->reply_, &call->status_, (void *)call);
  }

  void SetClusterId(const ClusterID &cluster_id) {
    auto old_id = cluster_id_.exchange(ClusterID::Nil());
    if (!old_id.IsNil() && (old_id != cluster_id)) {
      RAY_LOG(FATAL) << "Expected cluster ID to be Nil or " << cluster_id << ", but got"
                     << old_id;
    }
  }

  /// Get the main service of this rpc.
  instrumented_io_context &GetMainService() { return main_service_; }

 private:
  /// This function runs in a background thread. It keeps polling events from the
  /// `CompletionQueue`, and dispatches the event to the callbacks via the `ClientCall`
  /// objects.
  void PollEventsFromCompletionQueue(int index) {
    SetThreadName("client.poll" + std::to_string(index));
    void *got_tag = nullptr;
    bool ok = false;
    // Keep reading events from the `CompletionQueue` until it's shutdown.
    // NOTE(edoakes): we use AsyncNext here because for some unknown reason,
    // synchronous cq_.Next blocks indefinitely in the case that the process
    // received a SIGTERM.
    while (true) {
      auto deadline = gpr_time_add(gpr_now(GPR_CLOCK_REALTIME),
                                   gpr_time_from_millis(250, GPR_TIMESPAN));
      auto status = cqs_[index]->AsyncNext(&got_tag, &ok, deadline);
      if (status == grpc::CompletionQueue::SHUTDOWN) {
        break;
      } else if (status == grpc::CompletionQueue::TIMEOUT && shutdown_) {
        // If we timed out and shutdown, then exit immediately. This should not
        // be needed, but gRPC seems to not return SHUTDOWN correctly in these
        // cases (e.g., test_wait will hang on shutdown without this check).
        break;
      } else if (status != grpc::CompletionQueue::TIMEOUT) {
        // NOTE: CompletionQueue::TIMEOUT and gRPC deadline exceeded are different.
        // If the client deadline is exceeded, event is obtained at this block.
        auto call = static_cast<ClientCall *>(got_tag);
        call->SetReturnStatus();
        std::shared_ptr<StatsHandle> stats_handle = call->GetStatsHandle();
        RAY_CHECK(stats_handle != nullptr);
        if (ok && !main_service_.stopped() && !shutdown_) {
          // Post the callback to the main event loop.
          main_service_.post(
              [call]() {
                call->OnReplyReceived();
                // The call is finished, and we can delete this tag now.
                delete call;
              },
              std::move(stats_handle));
        } else {
          delete call;
        }
      }
    }
  }

  /// UUID of the cluster. Potential race between creating a ClientCall object
  /// and setting the cluster ID.
  SafeClusterID cluster_id_;

  /// The main event loop, to which the callback functions will be posted.
  instrumented_io_context &main_service_;

  /// The number of polling threads.
  int num_threads_;

  /// Whether the client has shutdown.
  std::atomic<bool> shutdown_;

  /// The index to send RPCs in a round-robin fashion
  std::atomic<unsigned int> rr_index_;

  /// The gRPC `CompletionQueue` object used to poll events.
  std::vector<std::unique_ptr<grpc::CompletionQueue>> cqs_;

  /// Polling threads to check the completion queue.
  std::vector<std::thread> polling_threads_;

  // Timeout in ms for calls created.
  int64_t call_timeout_ms_;
};

}  // namespace rpc
}  // namespace ray
