// Announces the host on the local network as _sidedroid._tcp through mDNS.
#pragma once
#include <cstdint>
#include <string>

namespace sd {

class Discovery {
public:
    ~Discovery() { stop(); }
    bool start(uint16_t port);
    void stop();

private:
    void* request_ = nullptr;  // heap DNS_SERVICE_REGISTER_REQUEST
    void* instance_ = nullptr;
    std::wstring name_, hostName_;
    bool registered_ = false;
};

}  // namespace sd
