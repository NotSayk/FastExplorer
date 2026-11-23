#pragma once
#include "win.h"

#include <string>

// Terminal intégré (Ctrl+ù) : un volet en bas de la fenêtre, relié à un PowerShell sans
// console visible (tuyaux). Il suit le dossier affiché (Set-Location quand il est libre).
// Une commande à la fois, sortie texte seulement : pas de programmes plein écran (vim, less…).

HWND Term_Create(HWND parent, int id);
void Term_SetFont(int heightPx);
// Affiche (démarre PowerShell dans dir au premier affichage) ou masque le volet.
void Term_Show(bool on, const std::wstring& dir);
bool Term_Shown();
// Dossier affiché par l'explorateur : le terminal y va s'il ne travaille pas (sinon après la commande).
void Term_SetDir(const std::wstring& dir);
void Term_Focus();
bool Term_Owns(HWND h);  // h est le volet ou l'un de ses contrôles
void Term_Shutdown();

// Pour les tests : commande « Set-Location » d'un dossier ('' doublés), base64 d'un texte UTF-16.
std::wstring Term_CdCommand(const std::wstring& dir);
std::wstring Term_Base64(const std::wstring& utf16);
