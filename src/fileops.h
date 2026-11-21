#pragma once
#include "pidl.h"
#include "win.h"

#include <cstdint>
#include <string>
#include <vector>

// Opérations sur les fichiers. Copier / déplacer passent d'abord par notre
// moteur (fastcopy.h : 10 à 15 fois plus rapide sur des milliers de fichiers),
// dont la progression s'affiche dans la fenêtre. Tout le reste, et tout ce que
// ce moteur refuse ou rate, passe par le moteur de copie du Shell
// (IFileOperation) : la même fenêtre de progression, la même gestion des
// conflits (remplacer, ignorer, garder les deux), la même élévation UAC et la
// même Corbeille que l'Explorateur. Chaque opération tourne sur son propre
// thread : l'interface reste fluide pendant une copie de plusieurs minutes.

// Restore : remet des éléments de la Corbeille à leur place d'origine (annuler une suppression).
enum class FileOpKind { Copy, Move, Delete, Rename, NewFolder, Restore };

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
    int64_t since = 0;                // Restore : supprimés depuis ce moment (FILETIME UTC)
    int tag = 0;                      // renvoyé tel quel (annuler / rétablir)
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
    // Pour annuler / rétablir :
    std::vector<std::wstring> sources;  // sources en chemins complets (presse-papiers compris)
    std::wstring newName;               // = FileOp::newName
    bool permanent = false;             // = FileOp::permanent
    int64_t started = 0;                // début de l'opération (FILETIME UTC)
    int tag = 0;                        // = FileOp::tag
    std::wstring detail;                // explication d'un échec (« ouvert dans Word »…), sinon vide
};

// Remet `original` (chemin complet d'avant la suppression) depuis la Corbeille
// de son volume : l'élément supprimé le plus récemment, et pas avant `since`.
// Lit les fichiers $I de $Recycle.Bin\<SID> (format de Windows Vista à 11).
DWORD Recycle_Restore(const std::wstring& original, int64_t since);

// Applications qui gardent ces fichiers ouverts (Restart Manager), ex. « Word, Excel » ; "" si aucune connue.
std::wstring FileOps_Lockers(const std::vector<std::wstring>& paths);

// Lance l'opération ; `target` reçoit `msg` (lParam = FileOpResult*, à libérer)
// à la fin. `owner` possède les fenêtres de progression et de conflit.
void FileOps_Start(HWND owner, HWND target, UINT msg, FileOp op);

// Nombre d'opérations encore en cours.
int FileOps_Running();

// Progression cumulée des copies faites par notre moteur (false : aucune en cours).
struct FileOpsProgress {
    uint64_t bytesDone = 0, bytesTotal = 0;
    uint32_t filesDone = 0, filesTotal = 0;
    bool move = false;
    ULONGLONG start = 0;
};
bool FileOps_Progress(FileOpsProgress& p);
// Annule les copies de notre moteur (ce qui est déjà copié reste).
void FileOps_Cancel();

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
