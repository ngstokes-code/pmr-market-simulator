#pragma once
#include <grpcpp/grpcpp.h>

#include <memory>
#include <string>
#include <vector>

#include "event_convert.hpp"
#include "market.grpc.pb.h"

namespace msim {

class GrpcStorage {
 public:
  explicit GrpcStorage(const std::string& target)
      : target_(target),
        channel_(
            grpc::CreateChannel(target, grpc::InsecureChannelCredentials())),
        stub_(msim::rpc::MarketStream::NewStub(channel_)) {}

  bool open() {
    context_ = std::make_unique<grpc::ClientContext>();
    // Publish() returns std::unique_ptr<grpc::ClientWriter<EventBatch>>
    writer_ = stub_->Publish(context_.get(), &ack_);
    return static_cast<bool>(writer_);
  }

  bool write_event(const Event& ev) {
    if (!writer_) return false;

    msim::rpc::Event proto_ev;
    EventConvert::to_proto(ev, &proto_ev);
    batch_.push_back(std::move(proto_ev));

    if (batch_.size() >= BATCH_SIZE) {
      return flush_batch();
    }
    return true;
  }

  bool close() {
    if (!writer_) return false;

    // Allow the server to drain any in-flight batches
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Flush any remaining events in the current batch
    if (!batch_.empty()) {
      flush_batch();
    }

    writer_->WritesDone();
    context_->TryCancel();  // forces a full flush of outbound frames
    grpc::Status status = writer_->Finish();
    // writer_.reset();
    return status.ok();
  }

  uint64_t ack_count() const { return ack_.count(); }

 private:
  bool flush_batch() {
    if (!writer_ || batch_.empty()) return false;

    msim::rpc::EventBatch batch_msg;
    batch_msg.mutable_events()->Reserve(static_cast<int>(batch_.size()));
    for (auto& ev : batch_) {
      auto* dst = batch_msg.add_events();
      *dst = std::move(ev);  // move protobuf messages
    }
    batch_.clear();

    return writer_->Write(batch_msg);
  }

  static constexpr std::size_t BATCH_SIZE = 512;

  std::string target_;
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<msim::rpc::MarketStream::Stub> stub_;

  std::unique_ptr<grpc::ClientContext> context_;
  std::unique_ptr<grpc::ClientWriter<msim::rpc::EventBatch>> writer_;
  msim::rpc::Ack ack_;

  std::vector<msim::rpc::Event> batch_;
};

}  // namespace msim
