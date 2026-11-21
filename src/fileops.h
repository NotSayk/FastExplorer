#pragma once
#include "pidl.h"
#include "win.h"

#include <string>
#include <vector>

// Opérations sur les fichiers par le moteur de copie du Shell (IFileOperation) :
// la même fenêtre de progression, la même gestion des conflits (remplacer,
// ignorer, garder les deux), la même élévation UAC et la même Corbeille que
// l'Explorateur. Chaque opération tourne sur son propre thread : l'interface
// reste fluide pendant une copie de plusieurs minutes.

enum class FileOpKind { Copy, Move, Delete, Rename, NewFolder };

struct FileOp {
    FileOpKind kind = FileOpKind::Copy;
    // Dossier concerné : destination (Copy, Move, NewFolder) ou dossier des
    // éléments (Delete, Rename). Le résultat permet d'y resélectionner.
    std::wstring folder;
    std::vector<std::wstring> paths;  // sources, en chemins
    std::vector<Pidl> items;          // ou sources Shell (presse-papiers, éléments virtuels compris)
    std::wstring newName;             // Rename : nouveau nom ; NewFolder : nom de base
    bool permanent = false;           // Delete : sans passer par la Corbeille (confirmer AVANT : voir ConfirmDelete)
    bool silent = false;              // aucune fenêtre ni confirmation (tests)
    std::wstring focusAfter;          // renvoyé tel quel : élément à cibler ensuite
    bool clipboardCut = false;        // collage d'un « Couper » : vider le presse-papiers ensuite
    DWORD clipSeq = 0;
};

struct FileOpResult {
    FileOpKind kind = FileOpKind::Copy;
    HRESULT hr = S_OK;
    bool aborted = false;               // annulé par l'utilisateur (en tout ou partie)
    std::wstring folder;                // = FileOp::folder
    std::vector<std::wstring> created;  // noms créés directement dans `folder`
    std::wstring focusAfter;
    bool clipboardCut = false;
    DWORD clipSeq = 0;
};

// Lance l'opération ; `target` reçoit `msg` (lParam = FileOpResult*, à libérer)
// à la fin. `owner` possède les fenêtres de progression et de conflit.
void FileOps_Start(HWND owner, HWND target, UINT msg, FileOp op);

// Nombre d'opérations encore en cours.
int FileOps_Running();

// Confirmation de suppression, comme l'Explorateur : toujours pour une
// suppression définitive ; vers la Corbeille selon le réglage de celle-ci.
bool ConfirmDelete(HWND owner, const std::vector<std::wstring>& names, bool permanent);

// Échec réel (ni succès, ni annulation par l'utilisateur).
bool FileOps_IsFailure(const FileOpResult& r);

// ── Presse-papiers (thread de l'interface) ──
// Format du Shell : interopérable avec l'Explorateur et les autres applications.

// Copier / Couper des éléments du dossier.
bool Clipboard_Put(HWND owner, const std::wstring& folder, const std::vector<std::wstring>& names, bool cut);
// Le presse-papiers contient-il encore ce que l'on y a mis ?
bool Clipboard_IsOurs();
// Éléments à coller. cut : ils viennent d'un « Couper ». seq : à repasser à Clipboard_ClearAfterMove.
bool Clipboard_Get(std::vector<Pidl>& items, bool& cut, DWORD& seq);
// Même lecture sur un objet de données quelconque (presse-papiers, glisser-déposer, tests).
bool ItemsFromDataObject(IDataObject* dobj, std::vector<Pidl>& items, bool& cut);
// Après le collage d'un « Couper » : le presse-papiers est vidé (comme dans l'Explorateur).
void Clipboard_ClearAfterMove(DWORD seq);
// À la fermeture : rend le contenu indépendant du processus (il survit à l'appli).
void Clipboard_Flush();
