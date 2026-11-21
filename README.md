# FastExplorer

Explorateur de fichiers Windows natif (C++20 / Win32), conçu pour la vitesse.

## Compiler et tester

| Commande | Résultat |
|---|---|
| `build.bat` | release optimisée → `build\release\FastExplorer.exe` |
| `build.bat debug` | debug → `build\debug\` |
| `build.bat asan` | debug + AddressSanitizer → `build\asan\` |
| `build.bat test` | compile et lance les tests sous AddressSanitizer |

Mesurer un dossier : `FastExplorer.exe --bench <dossier> [--seq|--par]`

## Architecture (`src/`)

| Module | Rôle |
|---|---|
| `nt.*` | accès à ntdll (`NtQueryDirectoryFile`) |
| `paths.*` | chemins : `\\?\`, UNC, parent, normalisation de saisie |
| `listing.*` | lecture d'un dossier, clés de tri, tri — sans UI, testé |
| `jobs.*` | tâches de fond : génération, annulation, `CancelSynchronousIo` |
| `shellmenu.*` | menus contextuels et verbes du Shell, raccourcis `.lnk` |
| `icons.*` | icônes propres (exe, lnk…) et surimpressions, thread dédié |
| `watch.*` | surveillance du dossier affiché |
| `fileops.*` | copier, déplacer, supprimer, renommer, nouveau dossier (thread par opération ; `IFileOperation` pour ce que `fastcopy` ne fait pas) ; presse-papiers |
| `fastcopy.*` | copie / déplacement rapides (`CopyFileExW` sur 8 threads, `MoveFileExW`) : 12 fois plus rapide que le moteur du Shell sur 15 000 fichiers |
| `dragdrop.*` | glisser-déposer OLE : source (objet de données du Shell) et cible (relais vers la cible du Shell ; fichiers ordinaires vers un dossier : `fastcopy`) |
| `ui.*` | fenêtre, ListView virtuelle, raccourcis |
| `main.cpp` | entrée, mode benchmark |

## Règles de la base

- Le thread de l'interface ne fait **aucune E/S** disque ou réseau, et aucun travail
  proportionnel à la taille du dossier au-delà de 20 000 entrées.
- `DirData` est **immuable** une fois construite : partagée sans verrou entre l'UI et les tâches.
- L'affichage passe par une **permutation** (`order`) : trier ne déplace jamais les données,
  et la sélection survit au tri.
- Toute optimisation est **mesurée avant/après** (`--bench`, temps de démarrage dans la barre d'état).
  Sans gain mesuré, elle est retirée.
- Compilation en `/W4 /WX` : zéro avertissement.
