#include "dragdrop.h"

#include "shellmenu.h"

#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <atomic>

using Microsoft::WRL::ComPtr;

bool DragDrop_ShellTarget(HWND owner, const std::wstring& folder, const std::wstring& name, IDropTarget** out) {
    *out = nullptr;
    if (!name.empty()) return ShellObjectOf(owner, folder, {name}, IID_IDropTarget, (void**)out);
    Pidl pf = ParsePath(folder);
    if (!pf) return false;
    ComPtr<IShellFolder> sf;
    if (FAILED(SHBindToObject(nullptr, pf.get(), nullptr, IID_PPV_ARGS(&sf)))) return false;
    return SUCCEEDED(sf->CreateViewObject(owner, IID_IDropTarget, (void**)out)) && *out;
}

namespace {

constexpr int kNoRow = -2;     // aucune cible choisie
constexpr int kBlocked = -3;   // survol d'un élément glissé lui-même : interdit
constexpr ULONGLONG kScrollEveryMs = 60;

class ListDropTarget : public IDropTarget {
public:
    ListDropTarget(HWND list, DropHost host) : list_(list), host_(std::move(host)) {
        CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&helper_));
    }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *ppv = static_cast<IDropTarget*>(this);
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

    IFACEMETHODIMP DragEnter(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override {
        data_ = data;
        row_ = kNoRow;
        folder_ = host_.folder();
        if (helper_) {
            POINT p{pt.x, pt.y};
            helper_->DragEnter(list_, data, &p, *effect);
        }
        Track(keys, pt, effect);
        return S_OK;
    }

    IFACEMETHODIMP DragOver(DWORD keys, POINTL pt, DWORD* effect) override {
        AutoScroll(pt);
        Track(keys, pt, effect);
        if (helper_) {
            POINT p{pt.x, pt.y};
            helper_->DragOver(&p, *effect);
        }
        return S_OK;
    }

    IFACEMETHODIMP DragLeave() override {
        if (helper_) helper_->DragLeave();
        Reset();
        return S_OK;
    }

    IFACEMETHODIMP Drop(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override {
        if (helper_) {
            POINT p{pt.x, pt.y};
            helper_->Drop(data, &p, *effect);
        }
        const DWORD allowed = *effect;
        Track(keys, pt, effect);
        const bool intoFolder = !item_;
        // La même cible Shell reçoit le Drop : elle se souvient du bouton utilisé
        // (bouton droit → son menu « Copier ici / Déplacer ici… »).
        ComPtr<IDropTarget> t = std::move(target_);
        const bool accepted = t && *effect != DROPEFFECT_NONE;
        data_.Reset();
        item_ = false;
        row_ = kNoRow;
        host_.highlight(-1);  // surbrillance retirée avant un éventuel menu
        if (accepted) {
            DWORD e = allowed;
            t->Drop(data, keys, pt, &e);
            *effect = e;
            if (e != DROPEFFECT_NONE) host_.dropped(intoFolder);
        } else {
            if (t) t->DragLeave();
            *effect = DROPEFFECT_NONE;
        }
        return S_OK;
    }

private:
    // Choisit la cible sous le curseur et lui relaie le survol.
    void Track(DWORD keys, POINTL pt, DWORD* effect) {
        const DWORD allowed = *effect;
        POINT c{pt.x, pt.y};
        ScreenToClient(list_, &c);
        std::wstring name;
        int row = host_.hit(c, name);
        if (row >= 0 && host_.dragged(name)) row = kBlocked;

        if (row != row_) {
            if (target_) target_->DragLeave();
            target_.Reset();
            item_ = false;
            host_.highlight(-1);
            row_ = row;
            if (row >= 0 && DragDrop_ShellTarget(nullptr, folder_, name, &target_)) item_ = true;
            if (!target_ && row != kBlocked) DragDrop_ShellTarget(nullptr, folder_, L"", &target_);  // dossier affiché
            DWORD e = allowed;
            if (target_ && SUCCEEDED(target_->DragEnter(data_.Get(), keys, pt, &e))) {
                *effect = e;
                if (item_ && e != DROPEFFECT_NONE) host_.highlight(row);
            } else {
                *effect = DROPEFFECT_NONE;
            }
            return;
        }
        if (!target_) {
            *effect = DROPEFFECT_NONE;
            return;
        }
        DWORD e = allowed;
        if (FAILED(target_->DragOver(keys, pt, &e))) e = DROPEFFECT_NONE;
        *effect = e;
    }

    // Près du bord haut ou bas, la liste défile (une ligne toutes les 60 ms).
    void AutoScroll(POINTL pt) {
        const ULONGLONG now = GetTickCount64();
        if (now - lastScroll_ < kScrollEveryMs) return;
        POINT c{pt.x, pt.y};
        ScreenToClient(list_, &c);
        RECT rc, hr{};
        GetClientRect(list_, &rc);
        if (HWND hdr = ListView_GetHeader(list_)) GetWindowRect(hdr, &hr);
        const int top = rc.top + (hr.bottom - hr.top);
        RECT ir{};
        const int rowH = ListView_GetItemRect(list_, ListView_GetTopIndex(list_), &ir, LVIR_BOUNDS) ? ir.bottom - ir.top : 20;
        const int zone = rowH;
        int dy = 0;
        if (c.y >= top && c.y < top + zone) dy = -rowH;
        else if (c.y > rc.bottom - zone && c.y <= rc.bottom) dy = rowH;
        if (!dy) return;
        lastScroll_ = now;
        if (helper_) helper_->Show(FALSE);  // évite les traces de l'image glissée
        ListView_Scroll(list_, 0, dy);
        UpdateWindow(list_);
        if (helper_) helper_->Show(TRUE);
        row_ = kNoRow;  // la ligne sous le curseur a changé
    }

    void Reset() {
        if (target_) target_->DragLeave();
        target_.Reset();
        data_.Reset();
        item_ = false;
        row_ = kNoRow;
        host_.highlight(-1);
    }

    std::atomic<ULONG> ref_{1};
    HWND list_;
    DropHost host_;
    ComPtr<IDropTargetHelper> helper_;
    ComPtr<IDataObject> data_;
    ComPtr<IDropTarget> target_;  // cible Shell courante
    std::wstring folder_;
    int row_ = kNoRow;
    bool item_ = false;           // cible = un élément (sinon le dossier affiché)
    ULONGLONG lastScroll_ = 0;
};

}  // namespace

bool DragDrop_RegisterTarget(HWND list, DropHost host) {
    auto* t = new ListDropTarget(list, std::move(host));
    const HRESULT hr = RegisterDragDrop(list, t);  // OLE garde sa propre référence
    t->Release();
    return SUCCEEDED(hr);
}

void DragDrop_RevokeTarget(HWND list) { RevokeDragDrop(list); }

DWORD DragDrop_Start(HWND list, const std::wstring& folder, const std::vector<std::wstring>& names, POINT client) {
    if (names.empty()) return DROPEFFECT_NONE;
    ComPtr<IDataObject> data;
    if (!ShellObjectOf(list, folder, names, IID_IDataObject, (void**)data.GetAddressOf())) return DROPEFFECT_NONE;
    // Image glissée : celle que la liste dessine pour les éléments sélectionnés.
    ComPtr<IDragSourceHelper> helper;
    if (SUCCEEDED(CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&helper))))
        helper->InitializeFromWindow(list, &client, data.Get());
    DWORD effect = DROPEFFECT_NONE;
    // Source par défaut du Shell : boutons gauche et droit, Échap, curseurs.
    SHDoDragDrop(list, data.Get(), nullptr, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
    return effect;
}
