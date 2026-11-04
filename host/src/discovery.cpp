#include "discovery.h"

#include <windows.h>
#include <windns.h>

#include "log.h"

namespace sd {

static VOID WINAPI onRegistered(DWORD status, PVOID, PDNS_SERVICE_INSTANCE instance) {
    if (status != ERROR_SUCCESS) logf("mDNS registration failed: %lu", status);
    else logf("mDNS service registered");
    if (instance) DnsServiceFreeInstance(instance);
}

bool Discovery::start(uint16_t port) {
    if (registered_) return true;
    wchar_t computer[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    if (!GetComputerNameW(computer, &size)) return false;
    name_ = std::wstring(computer) + L"._sidedroid._tcp.local";
    hostName_ = std::wstring(computer) + L".local";

    auto inst = DnsServiceConstructInstance(name_.c_str(), hostName_.c_str(), nullptr, nullptr, port, 0, 0, 0, nullptr, nullptr);
    if (!inst) return false;
    auto req = new DNS_SERVICE_REGISTER_REQUEST{};
    req->Version = DNS_QUERY_REQUEST_VERSION1;
    req->InterfaceIndex = 0;
    req->pServiceInstance = inst;
    req->pRegisterCompletionCallback = onRegistered;
    req->unicastEnabled = FALSE;
    DWORD r = DnsServiceRegister(req, nullptr);
    if (r != DNS_REQUEST_PENDING) {
        logf("DnsServiceRegister failed: %lu", r);
        DnsServiceFreeInstance(inst);
        delete req;
        return false;
    }
    request_ = req;
    instance_ = inst;
    registered_ = true;
    return true;
}

void Discovery::stop() {
    if (!registered_) return;
    auto req = static_cast<PDNS_SERVICE_REGISTER_REQUEST>(request_);
    DnsServiceDeRegister(req, nullptr);
    DnsServiceFreeInstance(static_cast<PDNS_SERVICE_INSTANCE>(instance_));
    delete req;
    request_ = nullptr;
    instance_ = nullptr;
    registered_ = false;
}

}  // namespace sd
