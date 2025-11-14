#include <grpcpp/grpcpp.h>

#include <chrono>
#include <iostream>

#include "market.grpc.pb.h"

class CollectorService : public msim::rpc::MarketStream::Service {
 public:
  grpc::Status Publish(grpc::ServerContext*,
                       grpc::ServerReader<msim::rpc::EventBatch>* reader,
                       msim::rpc::Ack* ack) override {
    msim::rpc::EventBatch batch;
    uint64_t count = 0;

    auto start = std::chrono::steady_clock::now();

    while (reader->Read(&batch)) {
      count += static_cast<uint64_t>(batch.events_size());
      // you could also inspect batch.events(i) here if you want
    }

    auto end = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(end - start).count();

    std::cout << "Received " << count << " events at "
              << (secs > 0 ? count / secs : 0.0) << " ev/s\n";

    ack->set_count(count);
    return grpc::Status::OK;
  }
};

int main(int argc, char** argv) {
  std::string addr = "0.0.0.0:50051";
  if (argc > 1) {
    addr = argv[1];
  }

  CollectorService service;

  grpc::ServerBuilder builder;
  builder.AddListeningPort(addr, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());

  std::cout << "[collector] Listening on " << addr << "\n";
  server->Wait();
  return 0;
}
