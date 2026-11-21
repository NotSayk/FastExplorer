#include "fileops.h"

#include "paths.h"
#include "shellmenu.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <atomic>

using Microsoft::WRL::ComPtr;

namespace {

std::atomic<int> g_running{0};

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
    for (int i = 1; i < 10000; ++i) {
        const std::wstring name = i == 1 ? op.newName : op.newName + L" (" + std::to_wstring(i) + L")";
        if (CreateDirectoryW(ToNtPath(Join(op.folder, name.c_str())).c_str(), nullptr)) {
            r.created.push_back(name);
            return S_OK;
        }
        const DWORD e = GetLastError();
        if (e == ERROR_ALREADY_EXISTS) continue;
        if (e != ERROR_ACCESS_DENIED) return HRESULT_FROM_WIN32(e);
        ComPtr<IFileOperation> fo;
        ComPtr<IShellItem> dest = ItemFromPath(op.folder);
        if (!dest || FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&fo))))
            return HRESULT_FROM_WIN32(e);
        if (owner) fo->SetOwnerWindow(owner);
        Sink* sink = new Sink(dest.Get(), &r.created);
        fo->NewItem(dest.Get(), FILE_ATTRIBUTE_DIRECTORY, name.c_str(), nullptr, sink);
        sink->Release();
        const HRESULT hr = fo->PerformOperations();
        BOOL ab = FALSE;
        fo->GetAnyOperationsAborted(&ab);
        r.aborted = ab != FALSE;
        return hr;
    }
    return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
}

HRESULT Perform(const FileOp& op, HWND owner, FileOpResult& r) {
    if (op.kind == FileOpKind::NewFolder) return NewFolder(op, owner, r);

    ComPtr<IFileOperation> fo;
    HRESULT hr = CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&fo));
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
    for (const auto& p : op.paths)
        if (auto it = ItemFromPath(p)) sources.push_back(it);
    for (const auto& p : op.items) {
        ComPtr<IShellItem> it;
        if (SUCCEEDED(SHCreateItemFromIDList(p.get(), IID_PPV_ARGS(&it)))) sources.push_back(it);
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
    for (auto& s : sources) {
        switch (op.kind) {
            case FileOpKind::Copy:
                if (!folder) return E_INVALIDARG;
                hr = fo->CopyItem(s.Get(), folder.Get(), nullptr, nullptr);
                break;
            case FileOpKind::Move: {
                if (!folder) return E_INVALIDARG;
                ComPtr<IShellItem> parent;
                if (SUCCEEDED(s->GetParent(&parent)) && SameItem(parent.Get(), folder.Get())) continue;  // déjà là
                hr = fo->MoveItem(s.Get(), folder.Get(), nullptr, nullptr);
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
};

DWORD WINAPI Run(void* p) {
    std::unique_ptr<Task> t((Task*)p);
    const HRESULT init = OleInitialize(nullptr);
    auto* r = new FileOpResult;
    r->kind = t->op.kind;
    r->folder = t->op.folder;
    r->focusAfter = t->op.focusAfter;
    r->clipboardCut = t->op.clipboardCut;
    r->clipSeq = t->op.clipSeq;
    r->hr = SUCCEEDED(init) ? Perform(t->op, t->owner, *r) : init;
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
    auto* t = new Task{owner, target, msg, std::move(op)};
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
    delete t;
    if (!PostMessageW(target, msg, 0, (LPARAM)r)) delete r;
}

int FileOps_Running() { return g_running.load(); }

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
