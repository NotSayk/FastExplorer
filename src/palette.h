#pragma once
#include "win.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

// Palette de commandes (Ctrl+Maj+P) et raccourcis clavier personnalisables.
// Les commandes viennent des menus de l'application (« Afficher ▸ Détails »…)
// et de quelques actions directes ; leur texte complet sert d'identifiant.

struct PaletteItem {
    std::wstring id;    // « Afficher ▸ Détails » : identifiant stable et texte affiché
    std::wstring keys;  // raccourci par défaut, affiché (« Ctrl+Maj+6 »), peut être vide
    std::function<void()> run;
    bool disabled = false;
    bool checked = false;
};

// Combinaison de touches. bits : vk (0-15), Ctrl (16), Maj (17), Alt (18). 0 : aucune.
using KeyChord = uint32_t;
constexpr KeyChord kChordCtrl = 1u << 16, kChordShift = 1u << 17, kChordAlt = 1u << 18;
KeyChord Chord_Make(UINT vk, bool ctrl, bool shift, bool alt);
// Touche enfoncée maintenant (WM_KEYDOWN / WM_SYSKEYDOWN), modificateurs lus au clavier.
KeyChord Chord_FromKey(UINT vk);
std::wstring Chord_Text(KeyChord c);  // « Ctrl+Maj+K », « Alt+F3 » ; "" pour 0

// Raccourcis de l'utilisateur (HKCU\Software\FastExplorer\Keys : une valeur DWORD par commande).
std::map<std::wstring, KeyChord> Keys_Load();
void Keys_Save(const std::map<std::wstring, KeyChord>& keys);

// Pertinence d'une commande pour la saisie : -1 si un mot manque, sinon plus petit = meilleur.
// Mots sans casse ni accents, n'importe où dans le texte ; un mot au début d'un mot du texte compte plus.
int Palette_Score(const std::wstring& query, const std::wstring& text);

// Affiche la palette sous la barre d'adresse (anchor : rectangle écran) et exécute la commande choisie.
// keys : raccourcis de l'utilisateur, affichés à la place des raccourcis par défaut.
void Palette_Show(HWND owner, const RECT& anchor, HFONT font, std::vector<PaletteItem> items,
                  const std::map<std::wstring, KeyChord>& keys);

// Fenêtre « Raccourcis clavier » : attribuer ou retirer un raccourci à chaque commande. true : modifiés.
bool Keys_Edit(HWND owner, HFONT font, const std::vector<PaletteItem>& items, std::map<std::wstring, KeyChord>& keys);
