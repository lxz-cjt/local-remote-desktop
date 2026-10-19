#pragma once

#include <memory>
#include <mutex>
#include <thread>

class NetworkServer;

class AudioManager {
public:
    explicit AudioManager(std::shared_ptr<NetworkServer> server);
    ~AudioManager();

    void Start();
    void Stop();

private:
    struct State;

    std::shared_ptr<NetworkServer> server_;
    std::shared_ptr<State> state_;
    std::thread worker_;
    std::mutex lifecycle_mutex_;
};
