#include "fileops.h"

#include "archive.h"

#include "fastcopy.h"
#include "paths.h"
#include "shellmenu.h"

#include <winioctl.h>
#include <winnetwk.h>
#include <restartmanager.h>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>

#pragma comment(lib, "rstrtmgr.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "mpr.lib")

using Microsoft::WRL::ComPtr;

namespace {

std::atomic<int> g_running{0};
std::atomic<bool> g_verify{false};

// ───────────────────────────── File d'attente des copies ─────────────────────────────

struct Job {
    uint64_t id = 0;
    std::wstring title;
    bool move = false;
    bool keysReady = false;            // disques connus (calculés sur le thread de l'opération)
    std::vector<std::wstring> keys;    // disques touchés ; vide : n'en bloque aucun
    bool running = false, hold = false, now = false, shell = false;
    ULONGLONG start = 0;
    FastProgress prog;                 // progression, annulation et pause de notre moteur
};

std::mutex g_qLock;
std::condition_variable g_qCv;
std::vector<std::shared_ptr<Job>> g_jobs;  // dans l'ordre d'arrivée
uint64_t g_nextJob = 0;

bool Shares(const Job& a, const Job& b) {
    for (const auto& k : a.keys)
        if (std::find(b.keys.begin(), b.keys.end(), k) != b.keys.end()) return true;
    return false;
}

// Sous g_qLock. Premier arrivé, premier servi, par disque.
bool CanStart(const Job& j) {
    if (j.prog.cancel || j.now) return true;
    if (j.hold || !j.keysReady) return false;
    for (const auto& o : g_jobs) {
        if (o.get() == &j) {
            // Plus loin : seules les copies en cours comptent (« démarrer maintenant »).
            for (const auto& later : g_jobs)
                if (later.get() != &j && later->running && Shares(*later, j)) return false;
            return true;
        }
        if (o->running ? Shares(*o, j) : (!o->hold && (!o->keysReady || Shares(*o, j)))) return false;
    }
    return true;
}

// Copies rapides en cours (progression affichée par l'interface).
std::mutex g_fastLock;
std::vector<FastProgress*> g_fast;

struct FastReg {
    FastProgress& p;
    explicit FastReg(FastProgress& prog) : p(prog) {
        std::lock_guard<std::mutex> l(g_fastLock);
        g_fast.push_back(&p);
    }
    ~FastReg() {
        std::lock_guard<std::mutex> l(g_fastLock);
        g_fast.erase(std::find(g_fast.begin(), g_fast.end(), &p));
    }
};

// Copie / déplacement par notre moteur (voir fastcopy.h). true : terminé ;
// sinon `left` reçoit ce qui reste à confier au Shell (tout, s'il a refusé).
bool TryFast(const FileOp& op, FileOpResult& r, HRESULT& hr, std::vector<std::pair<std::wstring, std::wstring>>& left,
             FastProgress& prog) {
    if ((op.kind != FileOpKind::Copy && op.kind != FileOpKind::Move) || op.folder.empty()) return false;
    std::vector<std::wstring> src = op.paths;
    for (const auto& p : op.items) {
        std::wstring buf(32768, L'\0');
        if (!SHGetPathFromIDListEx(p.get(), buf.data(), (DWORD)buf.size(), GPFIDL_DEFAULT)) return false;
        buf.resize(wcslen(buf.c_str()));
        src.push_back(std::move(buf));
    }
    if (op.kind == FileOpKind::Move)  // déjà dans la destination : rien à faire
        src.erase(std::remove_if(src.begin(), src.end(),
                                 [&](const std::wstring& s) {
                                     std::wstring parent, child;
                                     return ParentOf(s, parent, child) && _wcsicmp(parent.c_str(), op.folder.c_str()) == 0;
                                 }),
                  src.end());
    if (src.empty()) {
        hr = S_OK;  // tout était déjà dans la destination
        return true;
    }
    prog.move = op.kind == FileOpKind::Move;
    prog.start = GetTickCount64();
    FastOutcome out;
    {
        FastReg reg(prog);
        if (!FastTransfer(src, op.folder, prog.move, prog, out)) return false;
    }
    r.created = std::move(out.created);
    r.aborted = out.cancelled;
    r.retried = out.retried;
    r.verifyFailed = out.verifyFailed;
    r.verified = prog.verify;
    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, op.folder.c_str(), nullptr);
    hr = out.error ? HRESULT_FROM_WIN32(out.error) : S_OK;
    if (out.cancelled || out.shellLeft.empty()) return true;
    left = std::move(out.shellLeft);
    return false;
}

// Recueille les noms des éléments créés directement dans le dossier visé
// (pas ceux créés à l'intérieur des sous-dossiers copiés).
class Sink : public IFileOperationProgressSink {
public:
    explicit Sink(IShellItem* folder, std::vector<std::wstring>* out) : folder_(folder), out_(out) {}

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IFileOperationProgressSink) {
            *ppv = static_cast<IFileOperationProgressSink*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG r = --ref_;
        if (!r) delete this;
        return r;
    }

    IFACEMETHODIMP StartOperations() override { return S_OK; }
    IFACEMETHODIMP FinishOperations(HRESULT) override { return S_OK; }
    IFACEMETHODIMP PreRenameItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT hr, IShellItem* created) override {
        return Note(hr, created);
    }
    IFACEMETHODIMP PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP PostMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT hr, IShellItem* created) override {
        return Note(hr, created);
    }
    IFACEMETHODIMP PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP PostCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT hr, IShellItem* created) override {
        return Note(hr, created);
    }
    IFACEMETHODIMP PreDeleteItem(DWORD, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP PostDeleteItem(DWORD, IShellItem*, HRESULT, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT hr, IShellItem* created) override {
        return Note(hr, created);
    }
    IFACEMETHODIMP UpdateProgress(UINT, UINT) override { return S_OK; }
    IFACEMETHODIMP ResetTimer() override { return S_OK; }
    IFACEMETHODIMP PauseTimer() override { return S_OK; }
    IFACEMETHODIMP ResumeTimer() override { return S_OK; }

private:
    HRESULT Note(HRESULT hr, IShellItem* created) {
        if (FAILED(hr) || !created || !folder_) return S_OK;
        ComPtr<IShellItem> parent;
        int order = 1;
        if (FAILED(created->GetParent(&parent)) || FAILED(folder_->Compare(parent.Get(), SICHINT_CANONICAL, &order)) ||
            order != 0)
            return S_OK;
        PWSTR name = nullptr;
        if (SUCCEEDED(created->GetDisplayName(SIGDN_PARENTRELATIVEPARSING, &name)) && name) {
            out_->emplace_back(name);
            CoTaskMemFree(name);
        }
        return S_OK;
    }

    std::atomic<ULONG> ref_{1};
    IShellItem* folder_;
    std::vector<std::wstring>* out_;
};

ComPtr<IShellItem> ItemFromPath(const std::wstring& path) {
    ComPtr<IShellItem> it;
    SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&it));
    return it;
}

bool SameItem(IShellItem* a, IShellItem* b) {
    int order = 1;
    return a && b && SUCCEEDED(a->Compare(b, SICHINT_CANONICAL, &order)) && order == 0;
}

// Nouveau dossier : CreateDirectory direct (~1 ms) avec numérotation
// « Nouveau dossier (2) »… ; IFileOperation seulement si l'accès est refusé
// (dossier protégé : il propose l'élévation UAC).
HRESULT NewFolder(const FileOp& op, HWND owner, FileOpResult& r) {
    // Fichier : le numéro va avant l'extension (« Nouveau document texte (2).txt »).
    const size_t dot = op.newFile ? op.newName.rfind(L'.') : std::wstring::npos;
    const std::wstring stem = dot == std::wstring::npos || dot == 0 ? op.newName : op.newName.substr(0, dot);
    const std::wstring ext = stem.size() == op.newName.size() ? L"" : op.newName.substr(dot);
    for (int i = 1; i < 10000; ++i) {
        const std::wstring name = i == 1 ? op.newName : stem + L" (" + std::to_wstring(i) + L")" + ext;
        const std::wstring nt = ToNtPath(Join(op.folder, name.c_str()));
        bool made = false;
        if (op.newFile) {
            HANDLE h = CreateFileW(nt.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            made = h != INVALID_HANDLE_VALUE;
            if (made) CloseHandle(h);
        } else {
            made = CreateDirectoryW(nt.c_str(), nullptr) != FALSE;
        }
        if (made) {
            r.created.push_back(name);
            SHChangeNotify(op.newFile ? SHCNE_CREATE : SHCNE_MKDIR, SHCNF_PATHW, Join(op.folder, name.c_str()).c_str(),
                           nullptr);
            return S_OK;
        }
        const DWORD e = GetLastError();
        if (e == ERROR_ALREADY_EXISTS || e == ERROR_FILE_EXISTS) continue;
        if (e != ERROR_ACCESS_DENIED) return HRESULT_FROM_WIN32(e);
        ComPtr<IFileOperation> fo;
        ComPtr<IShellItem> dest = ItemFromPath(op.folder);
        if (!dest || FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&fo))))
            return HRESULT_FROM_WIN32(e);
        if (owner) fo->SetOwnerWindow(owner);
        Sink* sink = new Sink(dest.Get(), &r.created);
        fo->NewItem(dest.Get(), op.newFile ? FILE_ATTRIBUTE_NORMAL : FILE_ATTRIBUTE_DIRECTORY, name.c_str(), nullptr, sink);
        sink->Release();
        const HRESULT hr = fo->PerformOperations();
        BOOL ab = FALSE;
        fo->GetAnyOperationsAborted(&ab);
        r.aborted = ab != FALSE;
        return hr;
    }
    return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
}

// Renommage simple : directement, sans le moteur du Shell (dont la fenêtre,
// même invisible, prend le focus et fermerait la saisie du nom suivant après
// Tab). Tout échec (nom pris, fichier ouvert, droits) repasse par le Shell, qui
// sait demander et expliquer.
bool TryFastRename(const FileOp& op, FileOpResult& r) {
    if (op.paths.size() != 1 || !op.items.empty() || op.newName.empty()) return false;
    const std::wstring& src = op.paths[0];
    const size_t slash = src.find_last_of(L'\\');
    if (slash == std::wstring::npos || slash + 1 >= src.size()) return false;
    const std::wstring dir = src.substr(0, slash);
    const std::wstring dst = dir + L"\\" + op.newName;  // « C: » + « \nom » à la racine
    const DWORD attr = GetFileAttributesW(src.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !MoveFileExW(src.c_str(), dst.c_str(), 0)) return false;
    if (_wcsicmp(dir.c_str(), op.folder.c_str()) == 0 ||
        (dir.size() == 2 && op.folder.size() == 3 && _wcsnicmp(dir.c_str(), op.folder.c_str(), 2) == 0))
        r.created.push_back(op.newName);
    SHChangeNotify((attr & FILE_ATTRIBUTE_DIRECTORY) ? SHCNE_RENAMEFOLDER : SHCNE_RENAMEITEM,
                   SHCNF_PATHW | SHCNF_FLUSHNOWAIT, src.c_str(), dst.c_str());
    return true;
}

// Renommage de plusieurs éléments (chacun dans son dossier). En deux temps,
// par des noms provisoires : « a » → « b » et « b » → « a » fonctionnent. Un
// nom déjà pris par un autre élément (hors sélection) laisse cet élément tel quel.
HRESULT RenameMany(const FileOp& op, FileOpResult& r) {
    const size_t n = op.paths.size();
    if (op.newNames.size() != n) return E_INVALIDARG;
    struct Item {
        std::wstring src, dst, tmp, dir;
        bool active = false, moved = false;
    };
    std::vector<Item> items(n);
    std::vector<std::wstring> srcLower(n);
    for (size_t i = 0; i < n; ++i) {
        srcLower[i] = op.paths[i];
        CharLowerBuffW(srcLower[i].data(), (DWORD)srcLower[i].size());
    }
    DWORD first = 0;
    const DWORD tick = GetTickCount();
    for (size_t i = 0; i < n; ++i) {
        Item& it = items[i];
        it.src = op.paths[i];
        std::wstring leaf;
        if (!ParentOf(it.src, it.dir, leaf) || op.newNames[i].empty()) continue;
        it.dst = Join(it.dir, op.newNames[i].c_str());
        if (it.dst == it.src) continue;  // rien à faire
        std::wstring dl = it.dst;
        CharLowerBuffW(dl.data(), (DWORD)dl.size());
        const bool sameItem = dl == srcLower[i];  // seule la casse change
        const bool inSel = std::find(srcLower.begin(), srcLower.end(), dl) != srcLower.end();
        if (!sameItem && !inSel && GetFileAttributesW(ToNtPath(it.dst).c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (!first) first = ERROR_ALREADY_EXISTS;
            continue;
        }
        it.tmp = Join(it.dir, (L".fe-ren-" + std::to_wstring(tick) + L"-" + std::to_wstring(i)).c_str());
        it.active = true;
    }
    for (Item& it : items) {  // 1. noms provisoires
        if (!it.active) continue;
        if (MoveFileExW(ToNtPath(it.src).c_str(), ToNtPath(it.tmp).c_str(), 0)) it.moved = true;
        else if (!first) first = GetLastError();
    }
    for (size_t i = 0; i < n; ++i) {  // 2. noms définitifs (échec : retour au nom d'origine)
        Item& it = items[i];
        if (!it.moved) continue;
        if (MoveFileExW(ToNtPath(it.tmp).c_str(), ToNtPath(it.dst).c_str(), 0)) {
            r.created.push_back(op.newNames[i]);
            r.renamedFrom.push_back(it.src);
            const DWORD a = GetFileAttributesW(ToNtPath(it.dst).c_str());
            SHChangeNotify(a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) ? SHCNE_RENAMEFOLDER : SHCNE_RENAMEITEM,
                           SHCNF_PATHW | SHCNF_FLUSHNOWAIT, it.src.c_str(), it.dst.c_str());
        } else {
            if (!first) first = GetLastError();
            MoveFileExW(ToNtPath(it.tmp).c_str(), ToNtPath(it.src).c_str(), 0);
        }
    }
    return first ? HRESULT_FROM_WIN32(first) : S_OK;
}

// Annuler une suppression : chaque élément revient de la Corbeille.
HRESULT Restore(const FileOp& op, FileOpResult& r) {
    DWORD first = 0;
    for (const auto& p : op.paths) {
        if (const DWORD e = Recycle_Restore(p, op.since)) {
            if (!first) first = e;
            continue;
        }
        std::wstring parent, leaf;
        if (ParentOf(p, parent, leaf) && _wcsicmp(parent.c_str(), op.folder.c_str()) == 0) r.created.push_back(leaf);
        SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, parent.c_str(), nullptr);
    }
    return first && r.created.empty() ? HRESULT_FROM_WIN32(first) : S_OK;
}

// Vue Corbeille : « Restaurer » (chaque élément $R à son emplacement d'origine).
HRESULT RestoreBin(const FileOp& op, FileOpResult& r) {
    DWORD first = 0;
    size_t done = 0;
    for (const auto& p : op.paths) {
        std::wstring to;
        if (const DWORD e = Recycle_RestoreItem(p, &to)) {
            if (!first) {
                first = e;
                std::wstring o;
                int64_t w = 0;
                uint64_t s = 0;
                if (e == ERROR_ALREADY_EXISTS && Recycle_ReadInfo(Recycle_InfoOf(p), o, w, s))
                    r.detail = L"« " + o + L" » existe déjà.";
            }
            continue;
        }
        ++done;
        r.created.push_back(to);  // chemins complets (dossiers différents)
        std::wstring parent, leaf;
        if (ParentOf(to, parent, leaf))
            SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, parent.c_str(), nullptr);
    }
    return first && !done ? HRESULT_FROM_WIN32(first) : S_OK;
}

// Nom libre dans `dir` : « nom », sinon « nom (2) »… (extension gardée pour un fichier).
std::wstring FreeName(const std::wstring& dir, const std::wstring& name, bool isDir) {
    auto taken = [&](const std::wstring& n) { return GetFileAttributesW(ToNtPath(Join(dir, n.c_str())).c_str()) != INVALID_FILE_ATTRIBUTES; };
    if (!taken(name)) return name;
    std::wstring stem = name, ext;
    const size_t dot = name.rfind(L'.');
    if (!isDir && dot != std::wstring::npos && dot > 0) {
        stem = name.substr(0, dot);
        ext = name.substr(dot);
    }
    for (int i = 2; i < 10000; ++i)
        if (std::wstring n = stem + L" (" + std::to_wstring(i) + L")" + ext; !taken(n)) return n;
    return name;
}

// Raccourcis vers les éléments (« Coller en tant que raccourci »).
HRESULT LinkOp(const FileOp& op, FileOpResult& r) {
    std::vector<Pidl> items;
    for (const auto& p : op.paths)
        if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(p.c_str())) items.emplace_back((ITEMIDLIST*)pidl);
    for (const auto& p : op.items)
        if (PIDLIST_ABSOLUTE c = ILCloneFull(p.get())) items.emplace_back((ITEMIDLIST*)c);
    if (items.empty()) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    HRESULT last = S_OK;
    for (const auto& item : items) {
        std::wstring path(32768, L'\0'), base;
        if (SHGetPathFromIDListEx(item.get(), path.data(), (DWORD)path.size(), GPFIDL_DEFAULT)) path.resize(wcslen(path.c_str()));
        else path.clear();
        std::wstring parent, leaf;
        if (!path.empty() && ParentOf(path, parent, leaf) && !leaf.empty()) base = leaf;
        else {
            PWSTR n = nullptr;
            if (SUCCEEDED(SHGetNameFromIDList(item.get(), SIGDN_NORMALDISPLAY, &n)) && n) base = n;
            CoTaskMemFree(n);
        }
        for (auto& c : base)
            if (wcschr(L"\\/:*?\"<>|", c)) c = L'_';
        if (base.empty()) base = L"Raccourci";
        ComPtr<IShellLinkW> sl;
        ComPtr<IPersistFile> pf;
        HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl));
        if (SUCCEEDED(hr)) hr = sl->SetIDList(item.get());
        if (SUCCEEDED(hr) && !parent.empty()) sl->SetWorkingDirectory(parent.c_str());
        if (SUCCEEDED(hr)) hr = sl.As(&pf);
        if (FAILED(hr)) {
            last = hr;
            continue;
        }
        const std::wstring name = ShortcutName(base, [&](const std::wstring& n) {
            return GetFileAttributesW(ToNtPath(Join(op.folder, n.c_str())).c_str()) != INVALID_FILE_ATTRIBUTES;
        });
        const std::wstring full = Join(op.folder, name.c_str());
        hr = pf->Save(full.c_str(), TRUE);
        if (FAILED(hr)) {
            last = hr;
            continue;
        }
        r.created.push_back(name);
        SHChangeNotify(SHCNE_CREATE, SHCNF_PATHW, full.c_str(), nullptr);
    }
    return r.created.empty() ? (FAILED(last) ? last : E_FAIL) : S_OK;
}

// Copie vers un volume qui ne garde pas tout (FAT, exFAT, certains NAS) : on prévient, comme l'Explorateur.
bool ConfirmPropertyLoss(const FileOp& op, HWND owner, const std::vector<std::wstring>& sources, FastProgress& prog) {
    wchar_t root[MAX_PATH] = {}, fs[MAX_PATH] = {};
    DWORD flags = 0;
    if (op.folder.empty() || !GetVolumePathNameW(op.folder.c_str(), root, MAX_PATH) ||
        !GetVolumeInformationW(root, nullptr, 0, nullptr, nullptr, &flags, fs, MAX_PATH))
        return true;
    if ((flags & FILE_NAMED_STREAMS) && (flags & FILE_SUPPORTS_ENCRYPTION)) return true;
    bool encrypted = false;
    const std::vector<std::wstring> lost = FileOps_PropertyLoss(sources, flags, &encrypted);
    if (lost.empty()) return true;
    prog.allowDecrypted = encrypted;
    if (op.silent) return true;
    std::wstring parent, leaf;
    std::wstring what = ParentOf(lost[0], parent, leaf) ? leaf : lost[0];
    what = L"« " + what + L" »";
    if (lost.size() > 1)
        what += L" et " + std::to_wstring(lost.size() - 1) + (lost.size() >= 50 ? L" autres éléments ou plus" : L" autre(s) élément(s)");
    std::wstring content = what + (lost.size() == 1 ? L" a" : L" ont") + L" des propriétés que le lecteur de destination (" +
                           fs + L") ne peut pas conserver :";
    if (!(flags & FILE_NAMED_STREAMS))
        content += L"\n• propriétés enregistrées dans des flux NTFS (résumé, étiquettes, miniatures…)";
    if (encrypted) content += L"\n• chiffrement : ces fichiers seront copiés en clair";
    TASKDIALOG_BUTTON buttons[] = {{IDYES, L"Copier sans ces propriétés"}, {IDCANCEL, L"Annuler"}};
    TASKDIALOGCONFIG c{sizeof c};
    c.hwndParent = owner;
    c.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW | TDF_ALLOW_DIALOG_CANCELLATION;
    c.pszWindowTitle = op.kind == FileOpKind::Move ? L"Déplacer" : L"Copier";
    c.pszMainIcon = TD_WARNING_ICON;
    c.pszMainInstruction = L"Copier sans les propriétés ?";
    c.pszContent = content.c_str();
    c.cButtons = 2;
    c.pButtons = buttons;
    c.nDefaultButton = IDYES;
    int pressed = IDCANCEL;
    // Résolu à l'exécution : TaskDialogIndirect n'existe que dans comctl32 v6 (chargé par l'application).
    using Fn = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
    HMODULE cc = GetModuleHandleW(L"comctl32.dll");
    auto fn = cc ? (Fn)(void*)GetProcAddress(cc, "TaskDialogIndirect") : nullptr;
    if (!fn) return true;
    return SUCCEEDED(fn(&c, &pressed, nullptr, nullptr)) && pressed == IDYES;
}

HRESULT ExtractOp(const FileOp& op, FileOpResult& r, Job* job) {
    if (op.paths.empty()) return E_INVALIDARG;
    FastProgress local;
    FastProgress& prog = job ? job->prog : local;
    prog.label = L"Extraction";
    prog.start = GetTickCount64();
    std::wstring dest = op.folder, made;
    if (op.newFolder) {
        std::wstring parent, leaf;
        if (!ParentOf(op.paths[0], parent, leaf)) leaf = op.paths[0];
        made = FreeName(op.folder, Archive_Stem(leaf), true);
        dest = Join(op.folder, made.c_str());
    }
    ExtractOptions o;
    o.base = op.innerBase;
    o.items = op.inner;
    o.overwrite = op.overwrite;
    std::vector<std::wstring> created;
    DWORD e;
    {
        FastReg reg(prog);
        e = Archive_Extract(op.paths[0], dest, o, prog, &created, &r.detail);
    }
    r.aborted = prog.cancel;
    if (op.newFolder) {
        if (GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES && !RemoveDirectoryW(dest.c_str())) r.created = {made};
    } else {
        for (auto& c : created)
            if (!c.empty()) r.created.push_back(std::move(c));
    }
    return e ? HRESULT_FROM_WIN32(e) : S_OK;
}

HRESULT CompressOp(const FileOp& op, FileOpResult& r, Job* job) {
    FastProgress local;
    FastProgress& prog = job ? job->prog : local;
    prog.label = L"Compression";
    prog.start = GetTickCount64();
    const std::wstring name = FreeName(op.folder, op.newName, false);
    DWORD e;
    {
        FastReg reg(prog);
        e = Zip_Create(Join(op.folder, name.c_str()), op.paths, prog, &r.detail);
    }
    r.aborted = prog.cancel;
    if (!e) r.created = {name};
    return e ? HRESULT_FROM_WIN32(e) : S_OK;
}

HRESULT Perform(const FileOp& op, HWND owner, FileOpResult& r, Job* job) {
    if (op.kind == FileOpKind::Extract) return ExtractOp(op, r, job);
    if (op.kind == FileOpKind::Compress) return CompressOp(op, r, job);
    if (op.kind == FileOpKind::NewFolder) return NewFolder(op, owner, r);
    if (op.kind == FileOpKind::Link) return LinkOp(op, r);
    if (op.kind == FileOpKind::Restore) return Restore(op, r);
    if (op.kind == FileOpKind::RestoreBin) return RestoreBin(op, r);
    if (op.kind == FileOpKind::EmptyBin) {
        // Le Shell confirme lui-même (sauf en silence), avec sa progression.
        const HRESULT hr = SHEmptyRecycleBinW(owner, nullptr, op.silent ? SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND : 0);
        return hr == E_UNEXPECTED ? S_OK : hr;  // Corbeille déjà vide
    }
    if (op.kind == FileOpKind::Rename && !op.newNames.empty()) return RenameMany(op, r);
    if (op.kind == FileOpKind::Rename && TryFastRename(op, r)) return S_OK;

    HRESULT hr = S_OK;
    if ((op.kind == FileOpKind::Copy || op.kind == FileOpKind::Move) && job) {
        job->prog.verify = g_verify;
        if (!ConfirmPropertyLoss(op, owner, r.sources, job->prog)) {
            r.aborted = true;
            return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        }
    }
    std::vector<std::pair<std::wstring, std::wstring>> left;  // (source, dossier) que notre moteur rend au Shell
    if (job && TryFast(op, r, hr, left, job->prog)) return hr;
    const bool rest = !left.empty();

    ComPtr<IFileOperation> fo;
    hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&fo));
    if (FAILED(hr)) return hr;
    if (owner) fo->SetOwnerWindow(owner);

    DWORD flags = FOF_ALLOWUNDO | FOF_NOCONFIRMMKDIR;
    if (op.kind == FileOpKind::Delete) {
        // La confirmation est demandée par l'appelant (comme la fenêtre de
        // l'Explorateur : le moteur de copie ne la fait pas). WANTNUKEWARNING :
        // si la Corbeille est impossible (réseau, trop gros), le Shell prévient
        // que la suppression sera définitive.
        flags = FOF_NOCONFIRMATION | (op.permanent ? 0 : (FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE | FOF_WANTNUKEWARNING));
    }
    if (op.silent) flags |= FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    fo->SetOperationFlags(flags);

    ComPtr<IShellItem> folder = op.folder.empty() ? nullptr : ItemFromPath(op.folder);
    Sink* sink = new Sink(folder.Get(), &r.created);
    DWORD cookie = 0;
    fo->Advise(sink, &cookie);
    sink->Release();

    std::vector<ComPtr<IShellItem>> sources;
    std::vector<ComPtr<IShellItem>> targets;  // reprise : dossier de destination propre à chaque source
    if (rest) {
        for (const auto& [src, to] : left)
            if (auto it = ItemFromPath(src)) {
                sources.push_back(it);
                targets.push_back(_wcsicmp(to.c_str(), op.folder.c_str()) == 0 ? nullptr : ItemFromPath(to));
            }
    } else {
        for (const auto& p : op.paths)
            if (auto it = ItemFromPath(p)) sources.push_back(it);
        for (const auto& p : op.items) {
            ComPtr<IShellItem> it;
            if (SUCCEEDED(SHCreateItemFromIDList(p.get(), IID_PPV_ARGS(&it)))) sources.push_back(it);
        }
    }

    // Coller dans le dossier d'origine : un double « - Copie », comme l'Explorateur
    // (sinon le moteur considère que le fichier se remplace lui-même).
    if (op.kind == FileOpKind::Copy && folder && !sources.empty()) {
        bool allHere = true;
        for (auto& s : sources) {
            ComPtr<IShellItem> parent;
            if (FAILED(s->GetParent(&parent)) || !SameItem(parent.Get(), folder.Get())) { allHere = false; break; }
        }
        if (allHere) fo->SetOperationFlags(flags | FOF_RENAMEONCOLLISION);
    }

    int queued = 0;
    for (size_t k = 0; k < sources.size(); ++k) {
        auto& s = sources[k];
        IShellItem* to = k < targets.size() && targets[k] ? targets[k].Get() : folder.Get();
        switch (op.kind) {
            case FileOpKind::Copy:
                if (!to) return E_INVALIDARG;
                hr = fo->CopyItem(s.Get(), to, nullptr, nullptr);
                break;
            case FileOpKind::Move: {
                if (!to) return E_INVALIDARG;
                ComPtr<IShellItem> parent;
                if (SUCCEEDED(s->GetParent(&parent)) && SameItem(parent.Get(), to)) continue;  // déjà là
                hr = fo->MoveItem(s.Get(), to, nullptr, nullptr);
                break;
            }
            case FileOpKind::Delete: hr = fo->DeleteItem(s.Get(), nullptr); break;
            case FileOpKind::Rename: hr = fo->RenameItem(s.Get(), op.newName.c_str(), nullptr); break;
            default: return E_INVALIDARG;
        }
        if (FAILED(hr)) return hr;
        ++queued;
    }
    if (!queued) return sources.empty() && !(op.paths.empty() && op.items.empty()) ? HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) : S_OK;

    if (job) {
        std::lock_guard<std::mutex> l(g_qLock);
        job->shell = true;
    }
    hr = fo->PerformOperations();
    BOOL ab = FALSE;
    fo->GetAnyOperationsAborted(&ab);
    r.aborted = ab != FALSE;
    fo->Unadvise(cookie);
    return hr;
}

struct Task {
    HWND owner, target;
    UINT msg;
    FileOp op;
    std::shared_ptr<Job> job;  // copie ou déplacement : sa place dans la file
};

bool IsUnc(const std::wstring& p) { return p.size() > 2 && p[0] == L'\\' && p[1] == L'\\' && p[2] != L'?' && p[2] != L'.'; }

std::wstring VolumeOf(const std::wstring& p) {
    wchar_t v[MAX_PATH] = {};
    return GetVolumePathNameW(p.c_str(), v, MAX_PATH) ? std::wstring(v) : std::wstring();
}

// Attend son tour dans la file (disques libres), puis passe « en cours ».
void WaitTurn(Job& job, const FileOp& op, const std::vector<std::wstring>& sources) {
    std::vector<std::wstring> keys;
    const std::wstring dv = VolumeOf(op.folder);
    bool sameVolume = op.kind == FileOpKind::Move && !dv.empty() && !sources.empty();
    for (const auto& s : sources) sameVolume = sameVolume && _wcsicmp(VolumeOf(s).c_str(), dv.c_str()) == 0;
    if (!sameVolume) {  // déplacement sur un même volume : renommages instantanés, il ne gêne personne
        auto add = [&](const std::wstring& p) {
            std::wstring k = FileOps_DiskKey(p);
            if (!k.empty() && std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(std::move(k));
        };
        add(op.folder);
        for (const auto& s : sources) add(s);
    }
    std::unique_lock<std::mutex> l(g_qLock);
    job.keys = std::move(keys);
    job.keysReady = true;
    g_qCv.notify_all();
    g_qCv.wait(l, [&] { return CanStart(job); });
    job.running = true;
    job.hold = false;
    job.start = GetTickCount64();
    g_qCv.notify_all();
}

void Leave(const std::shared_ptr<Job>& job) {
    std::lock_guard<std::mutex> l(g_qLock);
    g_jobs.erase(std::remove(g_jobs.begin(), g_jobs.end(), job), g_jobs.end());
    g_qCv.notify_all();
}

DWORD WINAPI Run(void* p) {
    std::unique_ptr<Task> t((Task*)p);
    const HRESULT init = OleInitialize(nullptr);
    auto* r = new FileOpResult;
    r->kind = t->op.kind;
    r->folder = t->op.folder;
    r->focusAfter = t->op.focusAfter;
    r->clipboardCut = t->op.clipboardCut;
    r->clipSeq = t->op.clipSeq;
    r->newName = t->op.newName;
    r->permanent = t->op.permanent;
    r->newFile = t->op.newFile;
    r->tag = t->op.tag;
    r->then = t->op.then;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    r->started = (int64_t)(((uint64_t)now.dwHighDateTime << 32) | now.dwLowDateTime);
    r->sources = t->op.paths;
    for (const auto& item : t->op.items) {
        std::wstring buf(32768, L'\0');
        if (!SHGetPathFromIDListEx(item.get(), buf.data(), (DWORD)buf.size(), GPFIDL_DEFAULT)) continue;
        buf.resize(wcslen(buf.c_str()));
        r->sources.push_back(std::move(buf));
    }
    if (t->job) WaitTurn(*t->job, t->op, r->sources);
    if (t->job && t->job->prog.cancel) {  // annulée avant son tour : rien n'a été fait
        r->aborted = true;
        r->hr = HRESULT_FROM_WIN32(ERROR_CANCELLED);
    } else {
        r->hr = SUCCEEDED(init) ? Perform(t->op, t->owner, *r, t->job.get()) : init;
    }
    if (t->job) Leave(t->job);
    if (r->hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) || r->hr == HRESULT_FROM_WIN32(ERROR_LOCK_VIOLATION)) {
        const std::wstring who = FileOps_Lockers(r->sources);
        if (!who.empty()) r->detail = L"ouvert dans " + who;
    }
    t->op.items.clear();  // PIDL libérés avant OleUninitialize (simple mémoire, mais par ordre)
    if (SUCCEEDED(init)) OleUninitialize();
    --g_running;
    if (!PostMessageW(t->target, t->msg, 0, (LPARAM)r)) delete r;
    return 0;
}

ComPtr<IDataObject> g_clip;  // ce que l'on a mis dans le presse-papiers

CLIPFORMAT DropEffectFormat() {
    static const CLIPFORMAT f = (CLIPFORMAT)RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
    return f;
}

}  // namespace

void FileOps_Start(HWND owner, HWND target, UINT msg, FileOp op) {
    auto* t = new Task{owner, target, msg, std::move(op), nullptr};
    if (t->op.kind == FileOpKind::Extract || t->op.kind == FileOpKind::Compress) {
        auto job = std::make_shared<Job>();
        std::wstring parent, leaf;
        if (t->op.kind == FileOpKind::Extract) {
            if (!t->op.paths.empty() && !ParentOf(t->op.paths[0], parent, leaf)) leaf = t->op.paths[0];
            job->title = L"Extraction de « " + leaf + L" » vers " + t->op.folder;
        } else {
            const size_t n = t->op.paths.size();
            job->title = L"Compression de " + (n == 1 && ParentOf(t->op.paths[0], parent, leaf) ? L"« " + leaf + L" »"
                                                                                                   : std::to_wstring(n) + L" éléments") +
                         L" dans « " + t->op.newName + L" »";
        }
        job->now = t->op.then != ThenDo::Nothing;  // ouvrir / copier un élément : sans attendre la file
        std::lock_guard<std::mutex> l(g_qLock);
        job->id = ++g_nextJob;
        g_jobs.push_back(job);
        t->job = std::move(job);
    } else if (t->op.kind == FileOpKind::Copy || t->op.kind == FileOpKind::Move) {
        auto job = std::make_shared<Job>();
        job->move = t->op.kind == FileOpKind::Move;
        const size_t n = t->op.paths.size() + t->op.items.size();
        std::wstring what;
        if (n == 1 && t->op.paths.size() == 1) {
            std::wstring parent, leaf;
            what = ParentOf(t->op.paths[0], parent, leaf) ? L"« " + leaf + L" »" : L"« " + t->op.paths[0] + L" »";
        } else if (n == 1) {  // collage : un élément Shell (nom tiré de son identifiant, sans accès disque)
            PWSTR name = nullptr;
            if (SUCCEEDED(SHGetNameFromIDList(t->op.items[0].get(), SIGDN_PARENTRELATIVEPARSING, &name)) && name)
                what = L"« " + std::wstring(name) + L" »";
            else
                what = L"1 élément";
            CoTaskMemFree(name);
        } else {
            what = std::to_wstring(n) + (n > 1 ? L" éléments" : L" élément");
        }
        job->title = (job->move ? L"Déplacement de " : L"Copie de ") + what + L" vers " + t->op.folder;
        std::lock_guard<std::mutex> l(g_qLock);  // inscrite tout de suite : l'ordre d'arrivée est gardé
        job->id = ++g_nextJob;
        g_jobs.push_back(job);
        t->job = std::move(job);
    }
    ++g_running;
    HANDLE th = CreateThread(nullptr, 0, Run, t, 0, nullptr);
    if (th) {
        CloseHandle(th);
        return;
    }
    --g_running;
    auto* r = new FileOpResult;
    r->kind = t->op.kind;
    r->folder = t->op.folder;
    r->hr = HRESULT_FROM_WIN32(GetLastError());
    if (t->job) Leave(t->job);
    delete t;
    if (!PostMessageW(target, msg, 0, (LPARAM)r)) delete r;
}

int FileOps_Running() { return g_running.load(); }

bool FileOps_Progress(FileOpsProgress& p) {
    p = FileOpsProgress{};
    {
        std::lock_guard<std::mutex> q(g_qLock);
        for (const auto& j : g_jobs) p.waiting += j->running ? 0 : 1;
    }
    std::lock_guard<std::mutex> l(g_fastLock);
    for (const FastProgress* f : g_fast) {
        p.bytesDone += f->bytesDone;
        p.bytesTotal += f->bytesTotal;
        p.filesDone += f->filesDone;
        p.filesTotal += f->filesTotal;
        p.move |= f->move;
        if (f->label) p.label = f->label;
        if (!p.start || f->start < p.start) p.start = f->start;
    }
    return !g_fast.empty();
}

void FileOps_Cancel() {
    {
        std::lock_guard<std::mutex> l(g_fastLock);
        for (FastProgress* f : g_fast) f->cancel = true;
    }
    std::lock_guard<std::mutex> q(g_qLock);
    for (const auto& j : g_jobs)
        if (!j->running || !j->shell) j->prog.cancel = true;  // le Shell a sa propre fenêtre et son bouton
    g_qCv.notify_all();
}

std::vector<FileOpJob> FileOps_Jobs() {
    std::vector<FileOpJob> v;
    std::lock_guard<std::mutex> l(g_qLock);
    for (const auto& j : g_jobs) {
        FileOpJob o;
        o.id = j->id;
        o.title = j->title;
        o.running = j->running;
        o.paused = j->running ? j->prog.pause.load() : j->hold;
        o.shell = j->shell;
        o.move = j->move;
        o.bytesDone = j->prog.bytesDone;
        o.bytesTotal = j->prog.bytesTotal;
        o.filesDone = j->prog.filesDone;
        o.filesTotal = j->prog.filesTotal;
        o.start = j->start;
        v.push_back(std::move(o));
    }
    return v;
}

template <class F>
void WithJob(uint64_t id, F f) {
    std::lock_guard<std::mutex> l(g_qLock);
    for (const auto& j : g_jobs)
        if (j->id == id) f(*j);
    g_qCv.notify_all();
}

void FileOps_Pause(uint64_t id, bool pause) {
    WithJob(id, [&](Job& j) {
        if (j.running) j.prog.pause = pause && !j.shell;
        else j.hold = pause;
    });
}

void FileOps_StartNow(uint64_t id) {
    WithJob(id, [](Job& j) {
        j.now = true;
        j.hold = false;
    });
}

void FileOps_CancelJob(uint64_t id) {
    WithJob(id, [](Job& j) {
        j.prog.cancel = true;
        j.prog.pause = false;
    });
}

std::wstring FileOps_DiskKey(const std::wstring& path) {
    std::wstring vol = VolumeOf(path);
    if (vol.empty()) {
        if (!IsUnc(path)) return L"";
        vol = path;
    }
    if (IsUnc(vol)) {  // \\serveur\partage : le serveur
        const size_t e = vol.find(L'\\', 2);
        std::wstring s = vol.substr(2, e == std::wstring::npos ? std::wstring::npos : e - 2);
        CharLowerBuffW(s.data(), (DWORD)s.size());
        return L"net:" + s;
    }
    if (GetDriveTypeW(vol.c_str()) == DRIVE_REMOTE) {
        wchar_t remote[MAX_PATH] = {};
        DWORD n = MAX_PATH;
        const std::wstring drive = vol.substr(0, 2);
        if (WNetGetConnectionW(drive.c_str(), remote, &n) == NO_ERROR && IsUnc(remote)) return FileOps_DiskKey(remote);
        return L"net:" + drive;
    }
    wchar_t guid[MAX_PATH] = {};
    if (!GetVolumeNameForVolumeMountPointW(vol.c_str(), guid, MAX_PATH)) return L"vol:" + vol;
    std::wstring dev = guid;
    if (!dev.empty() && dev.back() == L'\\') dev.pop_back();  // \\?\Volume{…} : le volume lui-même
    HANDLE h = CreateFileW(dev.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        STORAGE_DEVICE_NUMBER sdn{};
        DWORD got = 0;
        const bool ok = DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, nullptr, 0, &sdn, sizeof sdn, &got, nullptr) != FALSE;
        CloseHandle(h);
        if (ok) return L"disk:" + std::to_wstring(sdn.DeviceNumber);  // deux partitions d'un même disque : même clé
    }
    return L"vol:" + std::wstring(guid);
}

bool ConfirmDelete(HWND owner, const std::vector<std::wstring>& names, bool permanent) {
    if (names.empty()) return false;
    if (!permanent) {
        SHELLSTATEW ss{};
        SHGetSetSettings(&ss, SSF_NOCONFIRMRECYCLE, FALSE);
        if (ss.fNoConfirmRecycle) return true;  // réglage par défaut de Windows 10/11
    }
    std::wstring what = names.size() == 1 ? L"« " + names[0] + L" »" : L"ces " + std::to_wstring(names.size()) + L" éléments";
    const std::wstring text = permanent ? L"Voulez-vous vraiment supprimer définitivement " + what + L" ?"
                                        : L"Voulez-vous vraiment déplacer " + what + L" vers la Corbeille ?";
    const wchar_t* title = names.size() == 1 ? L"Supprimer le fichier" : L"Supprimer plusieurs éléments";
    return MessageBoxW(owner, text.c_str(), title, MB_YESNO | (permanent ? MB_ICONWARNING : MB_ICONQUESTION)) == IDYES;
}

bool FileOps_IsFailure(const FileOpResult& r) {
    if (SUCCEEDED(r.hr) || r.aborted) return false;
    return r.hr != HRESULT_FROM_WIN32(ERROR_CANCELLED) && r.hr != (HRESULT)0x80270000L;  // COPYENGINE_E_USER_CANCELLED
}

bool Clipboard_Put(HWND owner, const std::wstring& folder, const std::vector<std::wstring>& names, bool cut) {
    ComPtr<IDataObject> dobj;
    if (!ShellObjectOf(owner, folder, names, IID_IDataObject, (void**)dobj.GetAddressOf())) return false;
    // « Couper » = effet préféré DÉPLACER : celui qui colle (nous, l'Explorateur…) déplace.
    if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD))) {
        *(DWORD*)GlobalLock(g) = cut ? DROPEFFECT_MOVE : (DROPEFFECT_COPY | DROPEFFECT_LINK);
        GlobalUnlock(g);
        FORMATETC fe{DropEffectFormat(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM sm{};
        sm.tymed = TYMED_HGLOBAL;
        sm.hGlobal = g;
        if (FAILED(dobj->SetData(&fe, &sm, TRUE))) GlobalFree(g);
    }
    if (FAILED(OleSetClipboard(dobj.Get()))) return false;
    g_clip = dobj;
    return true;
}

bool Clipboard_IsOurs() { return g_clip && OleIsCurrentClipboard(g_clip.Get()) == S_OK; }

bool ItemsFromDataObject(IDataObject* dobj, std::vector<Pidl>& items, bool& cut) {
    items.clear();
    cut = false;
    // Liste d'éléments Shell (Explorateur, éléments virtuels compris)…
    ComPtr<IShellItemArray> arr;
    if (SUCCEEDED(SHCreateShellItemArrayFromDataObject(dobj, IID_PPV_ARGS(&arr)))) {
        DWORD n = 0;
        arr->GetCount(&n);
        for (DWORD i = 0; i < n; ++i) {
            ComPtr<IShellItem> it;
            PIDLIST_ABSOLUTE p = nullptr;
            if (SUCCEEDED(arr->GetItemAt(i, &it)) && SUCCEEDED(SHGetIDListFromObject(it.Get(), &p)))
                items.emplace_back((ITEMIDLIST*)p);
        }
    }
    // … sinon de simples chemins (CF_HDROP) : éditeurs, navigateurs, la plupart des applications.
    FORMATETC hd{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM hs{};
    if (items.empty() && SUCCEEDED(dobj->GetData(&hd, &hs))) {
        if (HDROP drop = (HDROP)GlobalLock(hs.hGlobal)) {
            const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n; ++i) {
                std::wstring path(DragQueryFileW(drop, i, nullptr, 0) + 1, L'\0');
                path.resize(DragQueryFileW(drop, i, path.data(), (UINT)path.size()));
                if (Pidl p = ParsePath(path)) items.push_back(std::move(p));
            }
            GlobalUnlock(hs.hGlobal);
        }
        ReleaseStgMedium(&hs);
    }
    FORMATETC fe{DropEffectFormat(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM sm{};
    if (SUCCEEDED(dobj->GetData(&fe, &sm))) {
        if (const DWORD* e = (const DWORD*)GlobalLock(sm.hGlobal)) {
            cut = (*e & DROPEFFECT_MOVE) && !(*e & DROPEFFECT_COPY);
            GlobalUnlock(sm.hGlobal);
        }
        ReleaseStgMedium(&sm);
    }
    return !items.empty();
}

bool Clipboard_Get(std::vector<Pidl>& items, bool& cut, DWORD& seq) {
    items.clear();
    cut = false;
    ComPtr<IDataObject> dobj;
    if (FAILED(OleGetClipboard(&dobj))) return false;
    const bool ok = ItemsFromDataObject(dobj.Get(), items, cut);
    seq = GetClipboardSequenceNumber();
    return ok;
}

void Clipboard_ClearAfterMove(DWORD seq) {
    if (Clipboard_IsOurs()) {
        OleSetClipboard(nullptr);
        g_clip.Reset();
        return;
    }
    // Venu d'une autre application : on ne vide que si rien n'a changé depuis.
    if (GetClipboardSequenceNumber() == seq && OpenClipboard(nullptr)) {
        EmptyClipboard();
        CloseClipboard();
    }
}

void Clipboard_Flush() {
    if (Clipboard_IsOurs()) OleFlushClipboard();
    g_clip.Reset();
}

// ───────────────────────────── Corbeille, fichiers verrouillés ─────────────────────────────

namespace {

const std::wstring& UserSid() {
    static const std::wstring sid = [] {
        std::wstring s;
        HANDLE tok = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return s;
        DWORD n = 0;
        GetTokenInformation(tok, TokenUser, nullptr, 0, &n);
        std::vector<BYTE> b(n);
        LPWSTR str = nullptr;
        if (n && GetTokenInformation(tok, TokenUser, b.data(), n, &n) &&
            ConvertSidToStringSidW(((TOKEN_USER*)b.data())->User.Sid, &str)) {
            s = str;
            LocalFree(str);
        }
        CloseHandle(tok);
        return s;
    }();
    return sid;
}

}  // namespace

bool Recycle_ReadInfo(const std::wstring& iPath, std::wstring& original, int64_t& when, uint64_t& size) {
    // $I : version (8 octets), taille (8), date de suppression (8, FILETIME UTC), puis
    // version 2 : longueur (4, en caractères, \0 compris) et chemin ; version 1 : chemin sur 260 caractères.
    std::vector<BYTE> buf(28 + 2 * 32768);
    HANDLE f = CreateFileW(iPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD got = 0;
    const BOOL ok = ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr);
    CloseHandle(f);
    if (!ok || got < 24) return false;
    int64_t ver = 0;
    memcpy(&ver, buf.data(), 8);
    memcpy(&size, buf.data() + 8, 8);
    memcpy(&when, buf.data() + 16, 8);
    size_t off = 24, chars = (got - 24) / 2;
    if (ver == 2) {
        if (got < 28) return false;
        uint32_t n = 0;
        memcpy(&n, buf.data() + 24, 4);
        off = 28;
        chars = std::min<size_t>(n, (got - 28) / 2);
    } else if (ver != 1) {
        return false;
    }
    original.assign(chars, L'\0');
    memcpy(original.data(), buf.data() + off, chars * 2);
    original.resize(wcsnlen(original.c_str(), chars));
    return !original.empty();
}

std::vector<std::wstring> Recycle_Dirs() {
    std::vector<std::wstring> out;
    if (UserSid().empty()) return out;
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        const wchar_t root[] = {wchar_t(L'A' + i), L':', L'\\', 0};
        const UINT type = GetDriveTypeW(root);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;
        std::wstring dir = std::wstring(root) + L"$Recycle.Bin\\" + UserSid() + L"\\";
        const DWORD a = GetFileAttributesW(dir.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(dir);
    }
    return out;
}

std::wstring Recycle_InfoOf(const std::wstring& rPath) {
    const size_t s = rPath.find_last_of(L'\\');
    std::wstring i = rPath;
    if (s == std::wstring::npos || s + 2 >= i.size() || i[s + 1] != L'$') return L"";
    i[s + 2] = L'I';
    return i;
}

DWORD Recycle_RestoreItem(const std::wstring& rPath, std::wstring* restoredTo) {
    const std::wstring info = Recycle_InfoOf(rPath);
    std::wstring orig;
    int64_t when = 0;
    uint64_t size = 0;
    if (info.empty() || !Recycle_ReadInfo(info, orig, when, size)) return ERROR_FILE_NOT_FOUND;
    if (GetFileAttributesW(ToNtPath(orig).c_str()) != INVALID_FILE_ATTRIBUTES) return ERROR_ALREADY_EXISTS;
    std::wstring parent, leaf;
    if (ParentOf(orig, parent, leaf) && GetFileAttributesW(parent.c_str()) == INVALID_FILE_ATTRIBUTES)
        SHCreateDirectoryExW(nullptr, parent.c_str(), nullptr);  // dossier d'origine supprimé depuis : recréé
    if (!MoveFileExW(ToNtPath(rPath).c_str(), ToNtPath(orig).c_str(), 0)) return GetLastError();
    DeleteFileW(info.c_str());
    if (restoredTo) *restoredTo = orig;
    return 0;
}

DWORD Recycle_Restore(const std::wstring& original, int64_t since) {
    if (original.size() < 4 || original[1] != L':' || UserSid().empty()) return ERROR_NOT_SUPPORTED;
    const std::wstring bin = original.substr(0, 2) + L"\\$Recycle.Bin\\" + UserSid() + L"\\";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((bin + L"$I*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return ERROR_FILE_NOT_FOUND;
    std::wstring best;
    int64_t bestT = INT64_MIN;
    do {
        std::wstring orig;
        int64_t when = 0;
        uint64_t size = 0;
        if (!Recycle_ReadInfo(bin + fd.cFileName, orig, when, size)) continue;
        // Petite marge : l'horloge de la suppression est prise par le Shell, un peu après la nôtre.
        if (when >= since - 20000000LL && when >= bestT && _wcsicmp(orig.c_str(), original.c_str()) == 0) {
            bestT = when;
            best = fd.cFileName;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (best.empty()) return ERROR_FILE_NOT_FOUND;
    std::wstring data = best;
    data[1] = L'R';  // $Ixxxx → $Rxxxx : l'élément lui-même
    if (!MoveFileExW(ToNtPath(bin + data).c_str(), ToNtPath(original).c_str(), 0)) return GetLastError();
    DeleteFileW((bin + best).c_str());
    return 0;
}

std::vector<Locker> FileOps_LockerList(const std::vector<std::wstring>& paths) {
    // Fichiers à interroger : ceux donnés, et ceux des dossiers donnés (Restart Manager ne voit que des fichiers).
    std::vector<std::wstring> files;
    std::vector<std::wstring> dirs;
    for (const auto& p : paths) {
        const DWORD a = GetFileAttributesW(ToNtPath(p).c_str());
        if (a == INVALID_FILE_ATTRIBUTES) continue;
        if (a & FILE_ATTRIBUTE_DIRECTORY) dirs.push_back(p);
        else files.push_back(p);
    }
    for (size_t k = 0; k < dirs.size() && files.size() < 256; ++k) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(ToNtPath(Join(dirs[k], L"*")).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                    nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            const wchar_t* n = fd.cFileName;
            if (n[0] == L'.' && (!n[1] || (n[1] == L'.' && !n[2]))) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (dirs.size() < 2000) dirs.push_back(Join(dirs[k], n));
            } else {
                files.push_back(Join(dirs[k], n));
            }
        } while (files.size() < 256 && FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::vector<Locker> out;
    if (files.empty()) return out;
    DWORD session = 0;
    WCHAR key[CCH_RM_SESSION_KEY + 1] = {};
    if (RmStartSession(&session, 0, key) != ERROR_SUCCESS) return out;
    std::vector<LPCWSTR> names;
    for (const auto& f : files) names.push_back(f.c_str());
    if (RmRegisterResources(session, (UINT)names.size(), names.data(), 0, nullptr, 0, nullptr) == ERROR_SUCCESS) {
        UINT need = 0, have = 0;
        DWORD reasons = 0;
        std::vector<RM_PROCESS_INFO> info;
        DWORD e = RmGetList(session, &need, &have, nullptr, &reasons);
        if (e == ERROR_MORE_DATA && need) {
            info.resize(need);
            have = need;
            e = RmGetList(session, &need, &have, info.data(), &reasons);
        }
        if (e == ERROR_SUCCESS)
            for (UINT i = 0; i < have; ++i) {
                const RM_PROCESS_INFO& pi = info[i];
                if (std::any_of(out.begin(), out.end(), [&](const Locker& l) { return l.pid == pi.Process.dwProcessId; }))
                    continue;
                Locker l;
                l.pid = pi.Process.dwProcessId;
                l.started = pi.Process.ProcessStartTime;
                l.app = pi.strAppName;
                l.service = pi.ApplicationType == RmService;
                l.explorer = pi.ApplicationType == RmExplorer;
                if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, l.pid)) {
                    wchar_t exe[MAX_PATH * 2] = {};
                    DWORD n = _countof(exe);
                    if (QueryFullProcessImageNameW(h, 0, exe, &n)) l.exe.assign(exe, n);
                    CloseHandle(h);
                }
                if (l.app.empty()) {
                    std::wstring parent, leaf;
                    l.app = !l.exe.empty() && ParentOf(l.exe, parent, leaf) ? leaf : L"Processus " + std::to_wstring(l.pid);
                }
                out.push_back(std::move(l));
            }
    }
    RmEndSession(session);
    return out;
}

std::wstring FileOps_Lockers(const std::vector<std::wstring>& paths) {
    std::vector<std::wstring> files;
    for (const auto& p : paths) {
        const DWORD a = GetFileAttributesW(p.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) files.push_back(p);
        if (files.size() >= 64) break;
    }
    std::wstring out;
    if (files.empty()) return out;
    for (const auto& l : FileOps_LockerList(files))
        if (out.find(l.app) == std::wstring::npos) out += (out.empty() ? L"" : L", ") + l.app;
    return out;
}

void FileOps_SetVerify(bool on) { g_verify = on; }
bool FileOps_Verify() { return g_verify; }

std::wstring ShortcutName(const std::wstring& base, const std::function<bool(const std::wstring&)>& taken) {
    for (int i = 1; i < 10000; ++i) {
        std::wstring n = base + L" - Raccourci" + (i == 1 ? L"" : L" (" + std::to_wstring(i) + L")") + L".lnk";
        if (!taken(n)) return n;
    }
    return base + L" - Raccourci.lnk";
}

std::vector<std::wstring> FileOps_PropertyLoss(const std::vector<std::wstring>& sources, DWORD destFlags, bool* encrypted) {
    std::vector<std::wstring> out;
    if (encrypted) *encrypted = false;
    const bool streams = !(destFlags & FILE_NAMED_STREAMS), efs = !(destFlags & FILE_SUPPORTS_ENCRYPTION);
    if (!streams && !efs) return out;
    // Flux qui ne sont pas des propriétés : l'Explorateur les laisse partir sans rien dire.
    auto ignorable = [](const wchar_t* s) {
        return _wcsicmp(s, L"::$DATA") == 0 || _wcsicmp(s, L":Zone.Identifier:$DATA") == 0 ||
               _wcsicmp(s, L":SmartScreen:$DATA") == 0;
    };
    auto check = [&](const std::wstring& path, DWORD attr) {
        bool lose = false;
        if (efs && (attr & FILE_ATTRIBUTE_ENCRYPTED)) {
            lose = true;
            if (encrypted) *encrypted = true;
        }
        if (streams && !lose) {
            WIN32_FIND_STREAM_DATA sd;
            HANDLE h = FindFirstStreamW(ToNtPath(path).c_str(), FindStreamInfoStandard, &sd, 0);
            if (h != INVALID_HANDLE_VALUE) {
                do
                    if (!ignorable(sd.cStreamName)) lose = true;
                while (!lose && FindNextStreamW(h, &sd));
                FindClose(h);
            }
        }
        if (lose && out.size() < 50) out.push_back(path);
    };
    size_t visited = 0;
    std::vector<std::wstring> dirs;
    for (const auto& s : sources) {
        const DWORD a = GetFileAttributesW(ToNtPath(s).c_str());
        if (a == INVALID_FILE_ATTRIBUTES) continue;
        ++visited;
        if (a & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(a & FILE_ATTRIBUTE_REPARSE_POINT)) dirs.push_back(s);
        } else {
            check(s, a);
        }
    }
    for (size_t k = 0; k < dirs.size() && visited < 5000 && out.size() < 50; ++k) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(ToNtPath(Join(dirs[k], L"*")).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                    nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            const wchar_t* n = fd.cFileName;
            if (n[0] == L'.' && (!n[1] || (n[1] == L'.' && !n[2]))) continue;
            ++visited;
            const std::wstring full = Join(dirs[k], n);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) dirs.push_back(full);
            } else {
                check(full, fd.dwFileAttributes);
            }
        } while (visited < 5000 && out.size() < 50 && FindNextFileW(h, &fd));
        FindClose(h);
    }
    return out;
}