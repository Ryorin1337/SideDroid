// Looks for _sidedroid._tcp on the local network through the Windows DNS-SD API,
// the way a phone's mDNS browser would. Prints what it finds.
#include <windows.h>
#include <windns.h>

#include <cstdio>

#pragma comment(lib, "dnsapi.lib")

static volatile LONG g_found = 0;

static VOID WINAPI onBrowse(DWORD status, PVOID, PDNS_RECORD records) {
    if (status != ERROR_SUCCESS) {
        printf("browse status %lu\n", status);
        return;
    }
    for (auto r = records; r; r = r->pNext) {
        wprintf(L"record type=%u name=%s", r->wType, r->pName);
        if (r->wType == DNS_TYPE_PTR) wprintf(L" -> %s", r->Data.PTR.pNameHost);
        if (r->wType == DNS_TYPE_SRV) wprintf(L" port=%u", r->Data.SRV.wPort);
        wprintf(L"\n");
        InterlockedIncrement(&g_found);
    }
    if (records) DnsRecordListFree(records, DnsFreeRecordList);
}

int main() {
    DNS_SERVICE_BROWSE_REQUEST req{};
    req.Version = DNS_QUERY_REQUEST_VERSION1;
    req.InterfaceIndex = 0;
    req.QueryName = L"_sidedroid._tcp.local";
    req.pBrowseCallback = onBrowse;
    req.pQueryContext = nullptr;
    DNS_SERVICE_CANCEL cancel{};
    DNS_STATUS s = DnsServiceBrowse(&req, &cancel);
    if (s != DNS_REQUEST_PENDING) {
        printf("DnsServiceBrowse failed: %ld\n", s);
        return 2;
    }
    Sleep(4000);
    DnsServiceBrowseCancel(&cancel);
    printf("records found: %ld\n", g_found);
    return g_found > 0 ? 0 : 1;
}
