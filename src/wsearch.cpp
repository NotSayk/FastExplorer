#include "wsearch.h"

#include <msdasc.h>
#include <oledb.h>
#include <oledberr.h>
#include <searchapi.h>
#include <wrl/client.h>

#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

using Microsoft::WRL::ComPtr;

namespace {

// GUID de commande par défaut d'OLE DB (DBGUID_DEFAULT), défini ici pour ne pas dépendre d'oledb.lib.
const GUID kDbGuidDefault = {0xc8b521fb, 0x5cf3, 0x11ce, {0xad, 0xe5, 0x00, 0xaa, 0x00, 0x44, 0x77, 0x3d}};

std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), (DWORD)s.size());
    return s;
}

// « C:\a\b » → « file:C:/a/b » (forme attendue par SCOPE).
std::wstring ScopeUrl(const std::wstring& folder) {
    std::wstring u = L"file:" + folder;
    for (auto& c : u)
        if (c == L'\\') c = L'/';
    while (u.size() > 5 && u.back() == L'/') u.pop_back();
    return u;
}

std::wstring SqlString(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        if (c == L'\'') out += L"''";
        else out += c;
    }
    return out;
}

}  // namespace

std::wstring WSearch_PathOfUrl(const std::wstring& url) {
    // System.ItemPathDisplay est traduit (« C:\Utilisateurs\… ») : on prend l'URL, qui garde le vrai chemin.
    if (_wcsnicmp(url.c_str(), L"file:", 5) != 0) return {};
    std::wstring p = url.substr(_wcsnicmp(url.c_str(), L"file:///", 8) == 0 ? 8 : 5);
    for (auto& c : p)
        if (c == L'/') c = L'\\';
    if (p.size() > 2 && p[0] == L'\\' && p[1] == L'\\') return p;    // file://serveur/partage
    return p;
}

std::wstring WSearch_Sql(const std::wstring& folder, const std::wstring& word, size_t max) {
    std::wstring w;
    for (wchar_t c : word)
        if (c != L'"' && c != L'\'') w += c;  // guillemets retirés (la valeur est déjà une phrase)
    while (!w.empty() && iswspace(w.back())) w.pop_back();
    while (!w.empty() && iswspace(w.front())) w.erase(0, 1);
    const bool phrase = w.find(L' ') != std::wstring::npos;
    // Un mot : tous les mots qui commencent par lui (« fact » trouve « facture ») ; une phrase : telle quelle.
    const std::wstring term = phrase ? L"\"" + w + L"\"" : L"\"" + w + L"*\"";
    std::wstring sql = L"SELECT TOP " + std::to_wstring(max) +
                       L" System.ItemUrl FROM SystemIndex WHERE SCOPE='" +
                       SqlString(folder.empty() ? std::wstring(L"file:") : ScopeUrl(folder)) +
                       L"' AND CONTAINS(System.Search.Contents, '" + SqlString(term) + L"')";
    return sql;
}

int WSearch_Indexed(const std::wstring& folder) {
    ComPtr<ISearchManager> mgr;
    if (FAILED(CoCreateInstance(__uuidof(CSearchManager), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&mgr)))) return -1;
    ComPtr<ISearchCatalogManager> cat;
    if (FAILED(mgr->GetCatalog(L"SystemIndex", &cat))) return -1;
    ComPtr<ISearchCrawlScopeManager> csm;
    if (FAILED(cat->GetCrawlScopeManager(&csm))) return -1;
    if (folder.empty()) return 1;  // tout le PC : ce que l'index contient
    std::wstring url = L"file:///" + folder;
    if (url.back() != L'\\') url += L'\\';
    BOOL included = FALSE;
    if (FAILED(csm->IncludedInCrawlScope(url.c_str(), &included))) return -1;
    return included ? 1 : 0;
}

bool WSearch_Contains(const std::wstring& folder, const std::wstring& word, std::unordered_set<std::wstring>& out,
                      size_t max) {
    ComPtr<IDataInitialize> di;
    if (FAILED(CoCreateInstance(CLSID_MSDAINITIALIZE, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&di)))) return false;
    ComPtr<IDBInitialize> init;
    wchar_t conn[] = L"Provider=Search.CollatorDSO;Extended Properties='Application=Windows';";
    if (FAILED(di->GetDataSource(nullptr, CLSCTX_INPROC_SERVER, conn, IID_IDBInitialize, (IUnknown**)init.GetAddressOf())) ||
        FAILED(init->Initialize()))
        return false;
    ComPtr<IDBCreateSession> cs;
    ComPtr<IDBCreateCommand> cc;
    ComPtr<ICommandText> cmd;
    if (FAILED(init.As(&cs)) || FAILED(cs->CreateSession(nullptr, IID_IDBCreateCommand, (IUnknown**)cc.GetAddressOf())) ||
        FAILED(cc->CreateCommand(nullptr, IID_ICommandText, (IUnknown**)cmd.GetAddressOf())))
        return false;
    const std::wstring sql = WSearch_Sql(folder, word, max);
    if (FAILED(cmd->SetCommandText(kDbGuidDefault, sql.c_str()))) return false;
    ComPtr<IRowset> rows;
    if (FAILED(cmd->Execute(nullptr, IID_IRowset, nullptr, nullptr, (IUnknown**)rows.GetAddressOf())) || !rows) return false;
    ComPtr<IAccessor> acc;
    if (FAILED(rows.As(&acc))) return false;
    constexpr DBLENGTH kMax = 4096 * sizeof(wchar_t);
    struct Row {
        DBSTATUS status;
        DBLENGTH length;
        wchar_t value[4096];
    };
    DBBINDING b{};
    b.iOrdinal = 1;
    b.obStatus = offsetof(Row, status);
    b.obLength = offsetof(Row, length);
    b.obValue = offsetof(Row, value);
    b.dwPart = DBPART_VALUE | DBPART_LENGTH | DBPART_STATUS;
    b.dwMemOwner = DBMEMOWNER_CLIENTOWNED;
    b.eParamIO = DBPARAMIO_NOTPARAM;
    b.cbMaxLen = kMax;
    b.wType = DBTYPE_WSTR;
    HACCESSOR hacc = 0;
    if (FAILED(acc->CreateAccessor(DBACCESSOR_ROWDATA, 1, &b, sizeof(Row), &hacc, nullptr))) return false;
    std::vector<Row> buf(1);
    for (;;) {
        DBCOUNTITEM got = 0;
        HROW handles[64];
        HROW* ph = handles;
        const HRESULT hr = rows->GetNextRows(DB_NULL_HCHAPTER, 0, 64, &got, &ph);
        if (FAILED(hr)) break;
        for (DBCOUNTITEM i = 0; i < got; ++i)
            if (SUCCEEDED(rows->GetData(handles[i], hacc, &buf[0])) && buf[0].status == DBSTATUS_S_OK)
                if (std::wstring path = WSearch_PathOfUrl(
                        std::wstring(buf[0].value, std::min<size_t>(buf[0].length / sizeof(wchar_t), 4095)));
                    !path.empty())
                    out.insert(Lower(std::move(path)));
        if (got) rows->ReleaseRows(got, handles, nullptr, nullptr, nullptr);
        if (hr == DB_S_ENDOFROWSET || !got || out.size() >= max) break;
    }
    acc->ReleaseAccessor(hacc, nullptr);
    return true;
}
