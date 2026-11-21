# L'Explorateur de fichiers Windows : inventaire complet

Base de travail pour FastExplorer. Ce document recense **tout** ce que fait l'Explorateur de
Windows 11 (version de référence : **25H2, build 26200**, avec les nouveautés 2026 signalées),
comment certaines choses fonctionnent en interne, et ce qu'il ne fait **pas** (nos opportunités).

## Légende

| Marque | Sens |
|---|---|
| ✅ | Déjà présent dans FastExplorer |
| 🟡 | Partiellement présent |
| **P0** | Indispensable : sans ça, on ne peut pas remplacer l'Explorateur au quotidien |
| **P1** | Important : attendu par la plupart des utilisateurs |
| **P2** | Confort, ou usage plus rare |
| **P3** | Marginal, ou spécifique à l'écosystème Microsoft (à ne faire que si demandé) |
| ⚙️ | Note sur le fonctionnement interne / l'API Windows concernée |
| ⚠️ | Comportement à vérifier sur la machine, ou dont le déploiement varie selon les versions |

---

## 1. Structure de la fenêtre

De haut en bas :

| Zone | Contenu | Prio |
|---|---|---|
| **Barre d'onglets** (barre de titre) | Onglets, bouton « + », boutons fenêtre | P1 |
| **Barre de navigation** | Précédent, Suivant, Dossier parent, Actualiser, barre d'adresse, recherche | ✅🟡 P0 |
| **Barre de commandes** | Nouveau ▾, Couper, Copier, Coller, Renommer, Partager, Supprimer, Trier ▾, Afficher ▾, Filtrer ▾ (selon la version), « … » (plus) | P1 |
| **Volet de navigation** (gauche) | Accueil, Galerie, OneDrive, dossiers épinglés, Ce PC (arborescence), Réseau, Linux, téléphone… | P0 |
| **Zone de contenu** | La liste des éléments | ✅ P0 |
| **Volet d'aperçu / de détails** (droite) | Aperçu du fichier, ou métadonnées | P1 |
| **Barre d'état** (bas) | Nombre d'éléments, nombre et taille de la sélection, boutons Détails / Grandes icônes | ✅🟡 P0 |

⚙️ Chaque volet se masque ou s'affiche via **Afficher ▸ Afficher**. L'état est mémorisé globalement.

---

## 2. Navigation

### 2.1 Commandes de base

| Fonction | Détail | État |
|---|---|---|
| Précédent / Suivant | Boutons, Alt+← / Alt+→, boutons souris 4/5, **Retour arrière = Précédent** (et non dossier parent !) | ✅ (⚠️ chez nous, Retour arrière remonte au parent : à aligner ou rendre configurable) |
| Historique déroulant | Clic droit (ou appui long) sur Précédent/Suivant : liste des derniers dossiers visités | P1 |
| Dossier parent | Bouton ↑, Alt+↑ | ✅ |
| Actualiser | F5, Ctrl+R, bouton | ✅ (F5) |
| Rafraîchissement automatique | Le dossier se met à jour seul quand des fichiers sont créés, supprimés ou renommés | ✅🟡 P0 (dossier affiché ; pas encore les lecteurs de Ce PC) ⚙️ `ReadDirectoryChangesW` / `SHChangeNotifyRegister` |
| Ouvrir dans une nouvelle fenêtre | Ctrl+N, Ctrl+double-clic, Ctrl+Entrée | P1 |
| Ouvrir dans un nouvel onglet | Clic milieu sur un dossier (contenu, volet de navigation, et depuis 2026 barre d'adresse et Accueil), menu contextuel | P1 |
| Développer l'arborescence jusqu'au dossier courant | Ctrl+Maj+E (option « Développer jusqu'au dossier ouvert ») | P2 |

### 2.2 Barre d'adresse

| Fonction | Détail | Prio |
|---|---|---|
| **Fil d'Ariane** (breadcrumb) | Chaque segment est cliquable | **P0** |
| Chevrons `>` entre les segments | Clic : menu des sous-dossiers frères, pour sauter latéralement | P1 |
| Chevron le plus à gauche | Liste des emplacements racines (Bureau, Ce PC, Réseau…) | P2 |
| Clic dans le vide de la barre | Bascule en mode texte, chemin complet sélectionné | ✅ (mode texte seul pour l'instant) |
| Saisie de chemin | Chemin absolu, relatif, `..`, `%VARIABLES%`, UNC `\\serveur\partage` | ✅ |
| **Autocomplétion** | Suggestions de sous-dossiers et fichiers pendant la frappe, Tab pour compléter | P1 |
| Liste déroulante d'historique | F4 ou flèche de droite : chemins tapés récemment | P2 |
| Lancer une commande | Taper `cmd`, `powershell`, `wt`, `notepad`… dans la barre : ouvre le programme **dans le dossier courant** | P1 |
| URL | `https://…` ouvre le navigateur, `ftp://` ouvre l'accès FTP en lecture | P3 |
| Commandes `shell:` | `shell:startup`, `shell:sendto`, `shell:downloads`, `shell:appsfolder`, `shell:recyclebinfolder`, `shell:common startup`… (~120 noms) | P2 ⚙️ Known Folders + `SHParseDisplayName` |
| CLSID | `::{20D04FE0-3AEA-1069-A2D8-08002B30309D}` (Ce PC), etc. | P3 |
| Copier le chemin | Clic droit sur la barre ▸ Copier l'adresse / Copier l'adresse sous forme de texte | P1 |
| Glisser-déposer sur un segment | Déposer des fichiers sur un segment du fil d'Ariane = copier/déplacer vers ce dossier | P2 |
| Icône de la barre glissable | Glisser l'icône du dossier = créer un raccourci ailleurs | P3 |
| Modifier dans la barre ▸ Échap | Annule et revient au fil d'Ariane | ✅ |

### 2.3 Emplacements spéciaux (volet de navigation)

| Emplacement | Ce que c'est | Prio |
|---|---|---|
| **Accueil** (Home) | Page d'accueil : *Accès rapide* (dossiers épinglés et fréquents), *Favoris* (fichiers épinglés), *Récents*, *Recommandés* (2025 : aussi pour les comptes locaux et Microsoft), *Partagés* (comptes pro). Colonne « Activité » avec avatars pour les comptes Entra ID. Depuis 2026 : « Ouvrir l'emplacement du fichier » et « Demander à Copilot » (comptes pro/scolaires) | P1 (Accès rapide), P3 (le reste) |
| **Galerie** | Mosaïque chronologique de toutes les photos (dossier Images + sources ajoutées, OneDrive, iCloud), regroupées par mois, filtre de collection | P2 |
| **OneDrive** / autres clouds | Racine du fournisseur cloud, avec états de synchronisation | P2 |
| **Dossiers épinglés** | Bureau, Téléchargements, Documents, Images, Musique, Vidéos + vos épingles (« Épingler à Accès rapide ») | **P0** |
| **Ce PC** | Lecteurs (avec jauge d'espace libre), dossiers de l'utilisateur, emplacements réseau, appareils (téléphones MTP, appareils photo) | ✅🟡 P0 (jauge et libellé de volume manquants) |
| **Réseau** | Machines découvertes (SMB, WSD), serveurs multimédia (DLNA), routeurs (UPnP) | P2 |
| **Linux** | Distributions WSL via `\\wsl.localhost\<distro>` | P2 |
| **Téléphone mobile Android** | Accès sans fil aux fichiers du téléphone via « Lien avec Windows » (2025) | P3 |
| **Bibliothèques** | Masquées par défaut. Vues agrégées de plusieurs dossiers (Documents, Musique, Images, Vidéos) | P3 |
| **Corbeille** | Voir §11 | P1 |
| **Panneau de configuration** | Visible si « Afficher tous les dossiers » est coché | P3 |
| Clic droit sur le volet | Épingler, détacher, développer, ouvrir dans un nouvel onglet, propriétés | P1 |

Options du volet (clic droit sur une zone vide) : *Afficher les bibliothèques*, *Afficher tous les dossiers*,
*Développer jusqu'au dossier ouvert*, *Afficher Réseau / Ce PC*.

---

## 3. Onglets (Windows 11, depuis 22H2)

| Fonction | Raccourci / geste | Prio |
|---|---|---|
| Nouvel onglet | Ctrl+T, bouton « + », clic milieu sur un dossier | P1 |
| Fermer l'onglet | Ctrl+W, Ctrl+F4, clic milieu sur l'onglet | P1 |
| Onglet suivant / précédent | Ctrl+Tab / Ctrl+Maj+Tab | P1 |
| Aller à l'onglet N | Ctrl+1 … Ctrl+9 ⚠️ | P2 |
| Réorganiser | Glisser l'onglet dans la barre | P1 |
| Détacher / fusionner | Glisser un onglet hors de la fenêtre = nouvelle fenêtre ; dans une autre = fusion | P2 |
| Menu de l'onglet | Dupliquer, Fermer, Fermer les autres, Fermer à droite, Déplacer vers une nouvelle fenêtre | P2 |
| Déposer des fichiers sur un onglet | Survoler un onglet pendant un glisser l'active, pour y déposer | P2 |
| Restaurer à la réouverture | Option *Restaurer les fenêtres de dossiers précédentes à l'ouverture de session* | P2 |
| Clic milieu dans la barre d'onglets vide | ⚠️ selon la version | P3 |

---

## 4. Affichage du contenu

### 4.1 Modes d'affichage

| Mode | Raccourci | Prio |
|---|---|---|
| Très grandes icônes | Ctrl+Maj+1 | P2 |
| Grandes icônes | Ctrl+Maj+2 | P1 |
| Icônes moyennes | Ctrl+Maj+3 | P1 |
| Petites icônes | Ctrl+Maj+4 | P2 |
| Liste | Ctrl+Maj+5 | P2 |
| **Détails** | Ctrl+Maj+6 | ✅ P0 |
| Mosaïques | Ctrl+Maj+7 | P2 |
| Contenu | Ctrl+Maj+8 | P3 |
| Zoom continu | Ctrl+molette : passe d'un mode à l'autre et ajuste la taille des icônes | P1 |
| **Mode compact** | Afficher ▸ Mode compact : réduit l'espacement entre les lignes | P1 |

### 4.2 Options d'affichage (menu Afficher ▸ Afficher)

| Option | Prio |
|---|---|
| Volet de navigation | P0 |
| Volet de détails (Alt+Maj+P) | P1 |
| Volet d'aperçu (Alt+P) | P1 |
| Cases à cocher des éléments | P2 |
| **Extensions de noms de fichiers** | **P0** (aujourd'hui : toujours affichées) |
| **Éléments masqués** | **P0** (aujourd'hui : tout est affiché, sans distinction visuelle) |
| Mode compact | P1 |

### 4.3 Colonnes (vue Détails)

| Fonction | Détail | État |
|---|---|---|
| Colonnes par défaut | Nom, Modifié le, Type, Taille | ✅ |
| **Choix des colonnes** | Clic droit sur l'en-tête : coche rapide + « Plus… » = plus de 300 propriétés (date de création, d'accès, auteur, dimensions, durée, débit, EXIF, attributs, propriétaire, chemin d'accès, état de synchro…) | P1 ⚙️ Property System (`IPropertyStore`, `PKEY_*`) |
| Redimensionner | Glisser le séparateur ; double-clic = ajuster au contenu | ✅ (natif ListView) |
| Ajuster toutes les colonnes | Clic droit en-tête ▸ Ajuster toutes les colonnes / Ctrl+pavé num. « + » | P2 |
| Réordonner | Glisser l'en-tête | ✅ |
| Trier | Clic sur l'en-tête, flèche de tri | ✅ |
| **Filtres de colonne** | Survol de l'en-tête ▸ flèche ▾ : filtre par valeurs (« A–H », « Aujourd'hui », « Grand (128 Mo–1 Go) »…), cases multiples | P2 |
| Taille des dossiers | **Jamais affichée** (colonne vide pour les dossiers) | ⭐ opportunité (§20) |
| Colonnes par type de dossier | Musique : Titre, Artiste, Album… ; Images : Date de prise de vue, Dimensions… | P2 |

### 4.4 Tri et regroupement

| Fonction | Détail | Prio |
|---|---|---|
| Trier par | Nom, Date, Type, Taille + toute colonne ; croissant/décroissant | ✅ (4 colonnes) |
| Tri naturel | `fichier2` avant `fichier10` (désactivable par stratégie de groupe) | ✅ |
| Dossiers en premier | Toujours (sauf en regroupement) | ✅ |
| **Regrouper par** | Nom (A–H, I–P…), Date (Aujourd'hui, Hier, Plus tôt cette semaine, Semaine dernière, Le mois dernier, Plus tôt cette année, Il y a longtemps), Type, Taille (Vide, Minuscule, Petit, Moyen, Grand, Très grand, Énorme, Gigantesque), toute propriété ; groupes repliables (clic sur l'en-tête de groupe) | P2 |
| Téléchargements | Regroupé par date par défaut | P3 |

### 4.5 Mémoire des affichages par dossier

| Fonction | Détail | Prio |
|---|---|---|
| Vue mémorisée par dossier | Mode, colonnes, tri et regroupement mémorisés pour chaque dossier | P1 ⚙️ registre `Bags` / `BagMRU` (HKCU\…\Shell) |
| **Type de dossier** | Éléments généraux, Documents, Images, Musique, Vidéos : détecté automatiquement selon le contenu, ou forcé (Propriétés ▸ Personnaliser) | P2 |
| « Appliquer aux dossiers » | Options des dossiers ▸ Affichage : applique la vue courante à tous les dossiers du même type | P2 |
| Réinitialiser les dossiers | Retour aux vues par défaut | P2 |

### 4.6 Rendu des éléments

| Fonction | Détail | Prio |
|---|---|---|
| Icônes de type | Selon l'extension ; icônes propres aux `.exe`, `.ico`, `.lnk`, `.url` | ✅ P0 (thread dédié, icône du type affichée en attendant) ⚙️ `SHGetFileInfo` sans `USEFILEATTRIBUTES` pour ces types, en tâche de fond |
| **Miniatures** | Images, vidéos, PDF, documents Office, polices ; dossier = aperçu des premiers fichiers | P1 ⚙️ `IThumbnailCache` / `IShellItemImageFactory`, cache `thumbcache_*.db` |
| Surimpressions (overlays) | Flèche des raccourcis, cadenas, états de synchro cloud (nuage, coche verte), double flèche bleue (fichier compressé NTFS) | ✅ P1 (via `SHGFI_OVERLAYINDEX`) ⚙️ `IShellIconOverlayIdentifier` (15 emplacements max) |
| Éléments masqués | Icône semi-transparente | P0 |
| Couleur des fichiers compressés/chiffrés NTFS | Bleu / vert (option) | P3 |
| Couper (Ctrl+X) | L'élément coupé devient semi-transparent jusqu'au collage | P1 |
| **Info-bulles** | Survol : type, taille, date ; pour les dossiers, taille et premiers fichiers (option) | P2 |
| Mise en évidence des nouveaux éléments | Un élément créé ou collé est sélectionné et rendu visible | P1 |

---

## 5. Sélection

| Fonction | Geste | État |
|---|---|---|
| Sélection simple | Clic | ✅ |
| Ajouter / retirer | Ctrl+clic | ✅ |
| Plage | Maj+clic, Maj+flèches | ✅ |
| Tout sélectionner | Ctrl+A | ✅ (natif ListView) |
| Inverser la sélection | Barre de commandes ▸ « … » | P2 |
| Aucun | Clic dans le vide, barre « … » ▸ Ne rien sélectionner | ✅ |
| **Rectangle de sélection** (lasso) | Glisser depuis une zone vide | ✅ (natif) |
| Cases à cocher | Option d'affichage | P2 |
| Déplacer le focus sans sélectionner | Ctrl+flèches, puis Ctrl+Espace pour (dé)sélectionner | ✅ (natif) |
| **Recherche par frappe** | Taper le début d'un nom ; option alternative : « Taper dans la zone de recherche » | ✅ |
| Résumé de sélection | Barre d'état : « 3 éléments sélectionnés, 12,4 Mo » | P0 |

---

## 6. Opérations sur les fichiers

### 6.1 Création, renommage

| Fonction | Détail | Prio |
|---|---|---|
| Nouveau dossier | Ctrl+Maj+N, Nouveau ▸ Dossier ; nom « Nouveau dossier (2) » si déjà pris ; directement en mode renommage | **P0** |
| Nouveau fichier | Nouveau ▸ (Document texte, Raccourci, Image bitmap, Dossier compressé, + entrées des applications) | P1 ⚙️ clés `ShellNew` du registre (`NullFile`, `FileName`, `Data`, `Command`) |
| **Renommer** | F2, clic lent sur un élément sélectionné, bouton | **P0** |
| Sélection intelligente | À l'édition, seul le nom **sans l'extension** est sélectionné | P0 |
| Tab / Maj+Tab pendant le renommage | Valide et passe au fichier suivant / précédent | P1 |
| Renommage multiple | Sélection de N fichiers + F2 : « Vacances (1).jpg », « Vacances (2).jpg »… | P1 |
| Validation | Caractères interdits `\ / : * ? " < > |` (info-bulle d'erreur), noms réservés (`CON`, `PRN`, `AUX`, `NUL`, `COM1`…`LPT9`), point ou espace final retirés | P0 |
| Changement d'extension | Avertissement « Si vous modifiez l'extension… » | P0 |
| Renommage par la voix | Saisie vocale dans le champ de renommage (26H2) | P3 |

### 6.2 Copier, couper, coller, déplacer

| Fonction | Détail | Prio |
|---|---|---|
| Copier / Couper / Coller | Ctrl+C / Ctrl+X / Ctrl+V ; interopérable avec les autres applications via le presse-papiers | **P0** ⚙️ formats `CF_HDROP`, `Shell IDList Array`, `Preferred DropEffect` |
| Coller en tant que raccourci | Menu contextuel classique | P2 |
| Copier en tant que chemin d'accès | Ctrl+Maj+C : chemin(s) entre guillemets | P1 |
| Copier vers / Déplacer vers | Choix d'un dossier de destination | P2 |
| **Glisser-déposer** | Même volume = déplacer ; autre volume = copier ; Ctrl = forcer la copie ; Maj = forcer le déplacement ; Alt ou Ctrl+Maj = créer un raccourci ; **clic droit + glisser** = menu au dépôt (Copier ici, Déplacer ici, Créer un raccourci ici). Interopérable avec les autres applications | **P0** ⚙️ OLE `IDropTarget` / `IDropSource` / `IDataObject` |
| Ouverture au survol pendant le glisser | Survoler un dossier ou un onglet ouvre celui-ci après un délai | P2 |
| Dupliquer sur place | Ctrl+C, Ctrl+V dans le même dossier : « fichier - Copie.txt » | P0 |
| **Boîte de dialogue de copie** | Progression, débit, **graphique de vitesse** (« Plus de détails »), temps restant, pause, annulation ; plusieurs copies regroupées dans une seule fenêtre | **P0** |
| **Conflits** | Remplacer, Ignorer, **Comparer** (choix fichier par fichier, avec dates et tailles), « Faire ceci pour les N conflits suivants » ; fusion des dossiers de même nom (option *Masquer les conflits de fusion de dossiers*) | **P0** |
| Fichier en cours d'utilisation | « Le fichier est ouvert dans <Application> » : Réessayer / Ignorer | P1 ⚙️ Restart Manager (`RmGetList`) |
| Droits insuffisants | « Vous devez disposer d'une autorisation » ▸ Continuer (élévation UAC pour cette opération) | P1 ⚙️ élévation via COM (`IFileOperation` dans un processus élevé) |
| Perte de propriétés | Copie vers FAT/exFAT ou réseau : avertissement quand les propriétés (flux NTFS…) ne peuvent pas être conservées | P2 |
| Espace insuffisant, chemin trop long | Messages dédiés | P1 |
| Moteur | ⚙️ `IFileOperation` (moteur de copie de l'Explorateur : annuler, conflits, progression, élévation) ou moteur maison (`CopyFileEx` / `MoveFileWithProgress`, E/S non tamponnées pour les gros fichiers) | — |

### 6.3 Suppression, annulation

| Fonction | Détail | Prio |
|---|---|---|
| Supprimer vers la Corbeille | Suppr, Ctrl+D, bouton | **P0** |
| Suppression définitive | Maj+Suppr (avec confirmation) | **P0** |
| Confirmation | Optionnelle (propriétés de la Corbeille) ; toujours demandée pour une suppression définitive | P0 |
| Trop gros pour la Corbeille | « Supprimer définitivement ? » | P1 |
| **Annuler / Rétablir** | Ctrl+Z / Ctrl+Y : copie, déplacement, renommage, suppression, création, sur plusieurs niveaux | **P1** |
| Supprimer sur un lecteur réseau ou amovible | Définitif (pas de Corbeille), avec avertissement | P1 |

---

## 7. Menu contextuel (clic droit)

### 7.1 Menu moderne (Windows 11)

- **Rangée d'icônes en haut** : Couper, Copier, Renommer, Partager, Supprimer (+ Coller selon le contexte).
- Ouvrir, **Ouvrir avec ▸** (applications suggérées, « Choisir une autre application »), Ouvrir dans un nouvel onglet / une nouvelle fenêtre (dossiers).
- Compresser en ▸ (ZIP, 7z, TAR + options avancées), Extraire tout (archives).
- Copier en tant que chemin d'accès, Propriétés, **Ouvrir dans le Terminal** (dossiers).
- Épingler à Accès rapide / à l'écran de démarrage, Ajouter aux Favoris (fichiers).
- **Actions IA** (2025) : sur les images (recherche visuelle Bing, flou d'arrière-plan, effacement d'objets, suppression d'arrière-plan) et sur les documents (résumé Copilot, Microsoft 365). Section masquée si aucune action n'est disponible ; désactivable dans Paramètres ▸ Applications ▸ Actions.
- Applications tierces modernes (extensions packagées, `IExplorerCommand`).
- **Afficher plus d'options** (Maj+F10, ou Maj+clic droit direct) : ouvre l'ancien menu complet.
- **2026** : menu personnalisable (choisir les commandes intégrées et les extensions visibles, ranger les extensions dans un sous-menu, ou les masquer), et option « menu de style Windows 10 ». Séparateurs entre les icônes du haut.

### 7.2 Menu classique (« plus d'options »)

Contient en plus toutes les extensions héritées (7-Zip, Git, antivirus, éditeurs…), **Envoyer vers ▸**
(Bureau (créer un raccourci), Destinataire, Dossier compressé, lecteurs amovibles, contenu de `shell:sendto`),
Créer un raccourci, Restaurer les versions précédentes, Accorder l'accès à, Inclure dans une bibliothèque,
Exécuter en tant qu'administrateur, Résoudre les problèmes de compatibilité, Imprimer, Modifier, Définir
comme arrière-plan, Pivoter (images), Monter (ISO/VHD), Graver…

| Point | Prio |
|---|---|
| Afficher le menu **complet du Shell** (avec toutes les extensions installées) | ✅ P0 (Maj = verbes étendus ; doublons d'extensions retirés) ⚙️ `IContextMenu` / `IContextMenu2/3` via `IShellFolder::GetUIObjectOf` + `QueryContextMenu` / `InvokeCommand` |
| Menu sur une zone vide | Affichage, Trier par, Regrouper par, Actualiser, Coller, Nouveau ▸, Propriétés, Ouvrir dans le Terminal | ✅ P0 (menu Shell du dossier) |
| Menu sur plusieurs éléments | Commandes communes | ✅ P0 |
| Menu sur un lecteur | Formater, Éjecter, BitLocker, Propriétés | P1 |
| Touche Menu / Maj+F10 au clavier | P0 |
| Notre propre menu rapide (commandes internes) + accès au menu Shell | P1 |

---

## 8. Recherche

| Fonction | Détail | Prio |
|---|---|---|
| Barre de recherche | Ctrl+E, Ctrl+F, F3 ; cherche dans le dossier courant et ses sous-dossiers | **P0** |
| Résultats en direct | Au fil de la frappe, nom et contenu des fichiers indexés | P1 |
| Index Windows Search | Rapide dans les emplacements indexés, lent (parcours disque) ailleurs ; options *Ne pas utiliser l'index*, *Inclure les fichiers système*, *Fichiers compressés*, *Correspondances partielles* | P2 ⚙️ requêtes OLE DB / `ISearchQueryHelper` sur `SystemIndex` |
| **Syntaxe AQS** | `nom:`, `ext:.pdf`, `type:image`, `kind:music`, `taille:>100Mo`, `size:gigantic`, `date:aujourd'hui`, `datemodified:01/01/2026..31/01/2026`, `auteur:`, opérateurs `AND`, `OR`, `NOT`, `-`, guillemets, joker `*` | P2 |
| Filtres du ruban de recherche | Date de modification, Type, Taille, Autres propriétés ; emplacement (dossier courant / tous les sous-dossiers) | P1 |
| Recherches récentes | Historique des recherches | P2 |
| Enregistrer la recherche | Fichier `.search-ms` (dossier virtuel) | P3 |
| Ouvrir l'emplacement du fichier | Depuis un résultat | P1 |
| Recherche dans le contenu | Via l'index ; filtres IFilter par type (PDF, Office, texte) | P2 |
| Lenteur notoire | Hors index : parcours séquentiel, parfois très lent | ⭐ opportunité (§20) |

---

## 9. Volets d'aperçu et de détails

| Volet | Détail | Prio |
|---|---|---|
| **Aperçu** (Alt+P) | Images, texte et code, PDF, Word/Excel/PowerPoint (si Office est installé), vidéo/audio avec lecteur, polices, e-mails `.msg`/`.eml`, HTML | P1 ⚙️ preview handlers `IPreviewHandler` (hébergés hors processus par `prevhost.exe`) |
| Blocage pour les fichiers téléchargés | Depuis octobre 2025, l'aperçu des fichiers marqués « provenant d'Internet » est bloqué ; les PDF ont été rétablis en octobre 2026 | P2 ⚙️ flux `Zone.Identifier` (Mark of the Web) |
| **Détails** (Alt+Maj+P) | Grande icône ou miniature, métadonnées (taille, dates, dimensions, auteur, balises, note, commentaire…), **modifiables directement** | P1 ⚙️ `IPropertyStore` en écriture |
| Sélection multiple | Résumé : nombre, taille totale, valeurs communes | P1 |
| Largeur réglable | Glisser le séparateur | P1 |

---

## 10. Propriétés (Alt+Entrée, Alt+double-clic)

### 10.1 Fichier

| Onglet | Contenu |
|---|---|
| **Général** | Nom (modifiable), type, « S'ouvre avec » (Modifier…), emplacement, **taille et taille sur le disque**, dates de création, de modification et d'accès, attributs (Lecture seule, Caché, **Avancé…** : archivage, indexation, compression et chiffrement NTFS), **Débloquer** (fichiers téléchargés) |
| **Sécurité** | Listes de contrôle d'accès (ACL) par utilisateur et groupe, Modifier, Avancé (propriétaire, héritage, audit, accès effectif) |
| **Détails** | Toutes les métadonnées (EXIF, ID3, Office…), modifiables ; « Supprimer les propriétés et les informations personnelles » |
| **Versions précédentes** | Clichés VSS et Historique des fichiers |
| **Compatibilité** | Exécutables : mode de compatibilité, DPI, exécuter en tant qu'administrateur |
| **Signatures numériques** | Exécutables signés |
| **Raccourci** (`.lnk`) | Cible, Démarrer dans, Touche de raccourci, Exécuter (normale/réduite/agrandie), Commentaire, **Emplacement du fichier**, Changer d'icône, Avancé (administrateur) |

### 10.2 Dossier

Général (taille, nombre de fichiers et de dossiers : **calculé en direct**), Partage (partage réseau
avancé, autorisations), Sécurité, Versions précédentes, **Personnaliser** (type de dossier, image du
dossier, icône du dossier) ⚙️ `desktop.ini`.

### 10.3 Lecteur

Général (jauge espace utilisé/libre, système de fichiers, **Nettoyage de disque**, compression du lecteur,
indexation), **Outils** (Vérification des erreurs, Optimiser et défragmenter), Matériel, Partage, Sécurité,
Quota, Personnaliser, BitLocker (selon l'édition).

| Point | Prio |
|---|---|
| Ouvrir le dialogue de propriétés natif du Shell | ✅ P0 (Alt+Entrée, menu ; dossier courant si rien n'est sélectionné) ⚙️ `SHObjectProperties` / `SHMultiFileProperties` (gratuit et complet) |
| Notre propre panneau rapide (taille, dates, hash…) | P2 |

---

## 11. Corbeille

| Fonction | Détail | Prio |
|---|---|---|
| Parcourir | Colonnes *Emplacement d'origine* et *Date de suppression* | P1 |
| Restaurer | La sélection, ou tous les éléments | P1 |
| Vider | Tout supprimer définitivement | P1 |
| Propriétés | Taille maximale **par lecteur**, option « Ne pas déplacer les fichiers vers la Corbeille », confirmation de suppression | P2 |
| Fonctionnement | ⚙️ Un dossier `$Recycle.Bin\<SID>` par volume ; chaque élément = `$R…` (données) + `$I…` (nom d'origine, taille, date) ; API : `IFileOperation` avec `FOFX_RECYCLEONDELETE`, énumération via le dossier Shell `CLSID_RecycleBin` | — |

---

## 12. Archives

| Fonction | Détail | Prio |
|---|---|---|
| Parcourir un ZIP comme un dossier | Double-clic : naviguer, glisser des fichiers dehors, ouvrir (extraction temporaire) | P1 |
| Extraire tout | Assistant avec choix de destination | P1 |
| Compresser | ZIP (historique) ; **7z et TAR** (23H2+) avec options : format, niveau de compression, méthode | P1 |
| Lecture native | RAR, 7z, TAR, GZ, BZ2, XZ, ZSTD… (via libarchive, 23H2+), formats étendus en 26H2 | P1 |
| Performances | Extraction ZIP native réputée lente (améliorée en 2024–2025) | ⭐ opportunité |

---

## 13. Cloud et synchronisation

| Fonction | Détail | Prio |
|---|---|---|
| États de synchro | Nuage (en ligne uniquement), coche verte creuse (disponible localement), coche pleine (toujours conserver), flèches (synchro en cours), croix rouge (erreur) | P2 ⚙️ attributs `FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS` / `PINNED` / `UNPINNED` + Cloud Files API |
| Toujours conserver / Libérer de l'espace | Menu contextuel | P2 |
| **Ne pas déclencher de téléchargement** | Lire les métadonnées d'un fichier « en ligne uniquement » ne doit **jamais** le télécharger (miniatures et aperçus compris) | **P0** (piège de performance majeur) |
| Partager | Lien OneDrive/SharePoint, ou partage proche (Nearby Share), envoi par mail ou application | P3 |
| Autres fournisseurs | Dropbox, Google Drive, iCloud, Box : mêmes mécanismes (Cloud Files API) | P2 |
| Colonne État / Disponibilité | Vue Détails | P2 |

---

## 14. Réseau

| Fonction | Détail | Prio |
|---|---|---|
| Partages SMB | `\\serveur\partage`, authentification, mémorisation des identifiants | ✅ (UNC) / P1 (identifiants : déléguer à Windows) |
| Lister les partages d'un serveur | Taper `\\serveur` | P1 ⚙️ `NetShareEnum` / `WNetEnumResource` |
| Connecter un lecteur réseau | Ce PC ▸ « … » ▸ Connecter un lecteur réseau ; reconnexion à l'ouverture de session | P2 ⚙️ `WNetAddConnection2` |
| Ajouter un emplacement réseau | Raccourci FTP / WebDAV dans Ce PC | P3 |
| Découverte réseau | Machines, imprimantes, serveurs multimédia (DLNA) | P3 |
| Fichiers hors connexion | Centre de synchronisation (éditions Pro) | P3 |
| Robustesse | Un partage lent ou absent ne doit pas figer la fenêtre | ✅ (annulation `CancelSynchronousIo`) |

---

## 15. Lecteurs et périphériques

| Fonction | Prio |
|---|---|
| Jauge d'espace libre, libellé du volume (« Windows (C:) ») | **P0** |
| Éjecter (clés USB, disques externes) | P1 ⚙️ `CM_Request_Device_Eject` |
| Formater (FAT32, exFAT, NTFS, ReFS) | P2 (déléguer au dialogue `SHFormatDrive`) |
| Renommer le volume | P2 |
| **Monter** une image ISO/VHD/VHDX (double-clic) ; Éjecter pour démonter | P2 |
| Graver (CD/DVD) | P3 |
| Exécution automatique (AutoPlay) à l'insertion | P3 |
| Téléphones et appareils photo (**MTP**) : parcourir et copier | P2 ⚙️ espace de noms Shell (pas de chemin Win32 : PIDL seulement) |
| BitLocker (déverrouiller, gérer) | P3 |
| Dev Drive (ReFS) | P3 |
| Arrivée et départ de lecteurs en direct | **P1** ⚙️ `WM_DEVICECHANGE` / `SHChangeNotify(SHCNE_DRIVEADD…)` |

---

## 16. Raccourcis et liens

| Type | Comportement | Prio |
|---|---|---|
| Raccourci `.lnk` | Flèche en surimpression, extension toujours masquée ; ouvrir = ouvrir la cible ; Propriétés ▸ Emplacement du fichier ; dossier : navigue dans la cible | ✅ P0 (cible dossier ouverte dans FastExplorer, sans `Resolve`) ⚙️ `IShellLink` |
| Raccourci Internet `.url` | Ouvre le navigateur | P1 |
| Lien symbolique (fichier, dossier) | Flèche ; le dossier s'ouvre comme un dossier ; colonne « Cible du lien » ⚠️ | P1 |
| Jonction | Idem pour les dossiers ; jonctions système masquées (« Documents and Settings ») | P1 |
| Lien physique (hard link) | Invisible dans l'Explorateur | P3 |
| Créer un raccourci | Glisser avec Alt ou Ctrl+Maj, clic droit ▸ Créer un raccourci, Nouveau ▸ Raccourci | P1 |
| Cible absente | « Le raccourci a été modifié ou déplacé » + recherche de la cible | P2 |

---

## 17. Options des dossiers (dialogue complet)

Accès : « … » ▸ Options. Trois onglets. Une ⭐ signale les options que nous devrions reprendre.

### Général
- Ouvrir l'Explorateur sur : **Accueil** / **Ce PC** / OneDrive ⭐
- Parcourir les dossiers : même fenêtre / nouvelle fenêtre
- Cliquer : **simple clic** (souligner les titres : toujours ou au survol) / double-clic ⭐
- Confidentialité : fichiers récemment utilisés, dossiers fréquents, fichiers Office.com, recommandations ; **Effacer** l'historique ⭐
- Volet de navigation : afficher les bibliothèques, tous les dossiers, développer jusqu'au dossier ouvert

### Affichage
- Toujours afficher des icônes, jamais de miniatures ⭐
- Afficher l'icône du type sur les miniatures
- Afficher les informations de taille dans les info-bulles des dossiers
- Afficher le chemin complet dans la barre de titre ⭐
- Fichiers et dossiers cachés : ne pas afficher / **afficher** ⭐
- Masquer les lecteurs vides
- **Masquer les extensions des fichiers dont le type est connu** ⭐
- Masquer les conflits de fusion de dossiers
- **Masquer les fichiers protégés du système d'exploitation** ⭐
- Ouvrir les fenêtres des dossiers dans un processus distinct
- Restaurer les fenêtres de dossiers précédentes à l'ouverture de session ⭐
- Afficher les lettres de lecteur ⭐
- Afficher l'état de disponibilité (cloud)
- Afficher en couleur les fichiers NTFS chiffrés ou compressés
- Afficher les info-bulles
- Afficher les gestionnaires d'aperçu dans le volet de visualisation
- Utiliser les cases à cocher pour sélectionner les éléments
- Utiliser l'Assistant Partage
- Quand on tape dans la liste : sélectionner automatiquement l'élément / taper dans la zone de recherche ⭐
- Boutons « Appliquer aux dossiers » / « Réinitialiser les dossiers »

### Recherche
- Ne pas utiliser l'index pour les fichiers système
- Inclure les répertoires système, les fichiers compressés (ZIP, CAB…)
- Rechercher toujours dans les noms et le contenu des fichiers (peut être lent)

⚙️ Ces options sont dans `HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced`
(`Hidden`, `HideFileExt`, `ShowSuperHidden`, `LaunchTo`…). FastExplorer peut **lire les préférences de
l'utilisateur** au démarrage, pour respecter ses habitudes sans reconfiguration.

---

## 18. Raccourcis clavier (référence)

### Fenêtres et onglets
| Raccourci | Action | État |
|---|---|---|
| Win+E | Ouvrir l'Explorateur | P2 (remplacement par défaut, voir §20) |
| Ctrl+N | Nouvelle fenêtre | P1 |
| Ctrl+T / Ctrl+W / Ctrl+F4 | Nouvel onglet / fermer l'onglet | P1 |
| Ctrl+Tab / Ctrl+Maj+Tab | Onglet suivant / précédent | P1 |
| F11 | Plein écran | P2 |

### Navigation
| Raccourci | Action | État |
|---|---|---|
| Alt+← / Alt+→ / **Retour arrière** | Précédent / Suivant / Précédent | ✅ (⚠️ Retour arrière) |
| Alt+↑ | Dossier parent | ✅ |
| Alt+D, Ctrl+L, F4 | Barre d'adresse (F4 : avec la liste déroulante) | ✅ (sauf F4) |
| Ctrl+E, Ctrl+F, F3 | Recherche | P0 |
| F5, Ctrl+R | Actualiser | ✅ (F5) |
| F6, Tab | Passer d'un volet à l'autre | 🟡 (Tab : adresse ↔ liste) |
| Ctrl+Maj+E | Développer le volet de navigation jusqu'au dossier courant | P2 |
| Pavé num. * / + / − , ← / → (volet de navigation) | Tout développer / développer / réduire | P1 |
| Entrée | Ouvrir | ✅ |
| Ctrl+Entrée | Ouvrir le dossier dans une nouvelle fenêtre ⚠️ | P2 |

### Fichiers
| Raccourci | Action | État |
|---|---|---|
| Ctrl+C / Ctrl+X / Ctrl+V | Copier / Couper / Coller | P0 |
| Ctrl+Maj+C | Copier en tant que chemin d'accès | P1 |
| Ctrl+Z / Ctrl+Y | Annuler / Rétablir | P1 |
| Suppr, Ctrl+D / Maj+Suppr | Corbeille / suppression définitive | P0 |
| F2 | Renommer | P0 |
| Ctrl+Maj+N | Nouveau dossier | P0 |
| Alt+Entrée | Propriétés | P0 |
| Maj+F10, touche Menu | Menu contextuel | P0 |
| Ctrl+A | Tout sélectionner | ✅ |

### Affichage
| Raccourci | Action | État |
|---|---|---|
| Ctrl+Maj+1 … 8 | Mode d'affichage (voir §4.1) | P1 |
| Ctrl+molette | Zoom des icônes | P1 |
| Alt+P / Alt+Maj+P | Volet d'aperçu / de détails | P1 |
| Ctrl+pavé num. + | Ajuster toutes les colonnes | P2 |

---

## 19. Souris, glisser-déposer, tactile

| Geste | Action | État |
|---|---|---|
| Double-clic (ou simple clic selon l'option) | Ouvrir | ✅ (double) |
| Clic lent sur un élément sélectionné | Renommer | P1 |
| Clic milieu sur un dossier | Nouvel onglet | P1 |
| Boutons 4/5 | Précédent / Suivant | ✅ |
| Alt+double-clic | Propriétés | P2 |
| Ctrl+double-clic | Nouvelle fenêtre | P2 |
| Glisser (et ses variantes avec modificateurs) | Voir §6.2 | P0 |
| Glisser **depuis** l'application vers d'autres (mail, navigateur, éditeur) | P0 |
| Glisser **vers** l'application depuis d'autres (navigateur, Outlook : fichiers virtuels `CFSTR_FILEDESCRIPTOR`) | P1 |
| Défilement automatique pendant le glisser près des bords | P1 |
| Tactile | Appui long = clic droit, défilement inertiel, sélection par cases | P3 |

---

## 20. Ce que l'Explorateur ne fait PAS (nos opportunités ⭐)

Points faibles connus, sur lesquels FastExplorer peut se démarquer :

| Manque | Idée | Prio |
|---|---|---|
| **Lenteur** (ouverture, gros dossiers, réseau, « Travail en cours… ») | Notre cœur : lecture NT, liste virtuelle, rien de bloquant | ✅ |
| **Taille des dossiers** | Calcul en tâche de fond, colonne Taille remplie pour les dossiers, cache | P1 ⭐ |
| **Recherche instantanée** sur tout le disque | Index MFT + journal USN (à la *Everything*) | P1 ⭐ |
| **Double panneau** (à la Total Commander) | Deux listes côte à côte, F5/F6 copier/déplacer vers l'autre panneau | P1 ⭐ |
| **File d'attente de copies** | Copies mises en file (pas en parallèle sur le même disque), pause/reprise, reprise après erreur, vérification | P1 ⭐ |
| **Renommage par lot avancé** | Motifs, expressions régulières, compteur, casse, aperçu avant/après | P2 ⭐ |
| **Filtre instantané** du dossier courant | Taper pour filtrer la liste (sans lancer de recherche) | P1 ⭐ |
| **Raccourcis personnalisables** | Table de raccourcis éditable | P2 ⭐ |
| **Palette de commandes** | Ctrl+Maj+P : toutes les actions accessibles au clavier | P2 ⭐ |
| **Terminal intégré** | Volet terminal qui suit le dossier courant | P2 ⭐ |
| **Aperçu du code avec coloration** | Volet d'aperçu rapide pour le texte et le code | P2 ⭐ |
| **Hash et vérification** | MD5/SHA-256 dans les propriétés ou le menu | P3 ⭐ |
| **Comparaison de dossiers** | Différences entre deux dossiers, synchronisation | P3 ⭐ |
| **Sessions / espaces de travail** | Ensembles d'onglets nommés, restaurés en un clic | P2 ⭐ |
| **Signets** | Favoris de dossiers avec raccourcis clavier | P2 ⭐ |
| **Fichiers verrouillés** | « Qui utilise ce fichier ? » + fermer l'application | P2 ⭐ |
| **Statut Git** | Colonne/surimpression pour l'état des fichiers d'un dépôt | P3 ⭐ |
| **Portable** | Un seul .exe, aucune installation, configuration dans un fichier | ✅ ⭐ |
| Chemins de plus de 260 caractères | L'Explorateur les gère mal (selon les opérations) | ✅ ⭐ |
| Remplacer l'Explorateur par défaut | Win+E et « Ouvrir le dossier » des autres applications ⚙️ clé `HKCU\Software\Classes\Folder\shell` (à faire proprement et de façon réversible) | P2 |

---

## 21. Fonctionnement interne (référence technique)

| Sujet | Comment ça marche |
|---|---|
| **Espace de noms Shell** | Tout est un `IShellItem` / PIDL : fichiers, mais aussi Ce PC, Réseau, téléphone, Corbeille, Panneau de configuration. Les emplacements virtuels **n'ont pas de chemin Win32**. FastExplorer lit les dossiers Win32 par l'API NT (rapide) et devra passer par le Shell pour les emplacements virtuels (MTP, Corbeille, Réseau, Galerie). |
| **Known Folders** | Emplacements standard (Documents, Téléchargements…) via `SHGetKnownFolderPath(FOLDERID_*)`, jamais en dur (ils peuvent être déplacés ou redirigés vers OneDrive). Noms localisés via `desktop.ini` (`LocalizedResourceName`). |
| **Extensions Shell** | Menus contextuels (`IContextMenu`, `IExplorerCommand`), propriétés (`IPropertyStore` + property handlers), miniatures (`IThumbnailProvider`), aperçus (`IPreviewHandler`), surimpressions (`IShellIconOverlayIdentifier`), feuilles de propriétés, gestionnaires de glisser-déposer, copy hooks. Code tiers chargé **dans le processus** : un plantage ou une lenteur d'extension affecte l'application. Les héberger avec prudence (hors du thread de l'interface, délai maximal). |
| **Moteur de copie** | `IFileOperation` : conflits, progression, annulation, élévation, Corbeille ; s'intègre à l'historique d'annulation de l'Explorateur. |
| **Notifications de changement** | `ReadDirectoryChangesW` (par dossier, précis) et `SHChangeNotifyRegister` (événements Shell : lecteurs, renommages par d'autres applications). Il faut fusionner les rafales d'événements (copie de 10 000 fichiers). |
| **Miniatures** | `IShellItemImageFactory::GetImage` (cache système `thumbcache_*.db` partagé avec l'Explorateur) ; toujours en tâche de fond, annulables. |
| **Indexation** | Service Windows Search ; requêtes SQL via OLE DB sur `SystemIndex` (`SELECT System.ItemPathDisplay FROM SystemIndex WHERE SCOPE='file:C:/…' AND CONTAINS(…)`). |
| **Vues par dossier** | Registre `HKCU\Software\Classes\Local Settings\Software\Microsoft\Windows\Shell\Bags` + `BagMRU` (limité en nombre : les vues finissent par être « oubliées »). |
| **Personnalisation de dossier** | `desktop.ini` (attributs Système + Lecture seule sur le dossier) : icône, type de dossier, nom localisé. |
| **Mark of the Web** | Flux NTFS alternatif `:Zone.Identifier` sur les fichiers téléchargés : avertissements, aperçu bloqué, bouton « Débloquer ». |
| **Corbeille** | `$Recycle.Bin\<SID>\$Rxxxxxx` + `$Ixxxxxx` par volume (voir §11). |
| **Fichiers cloud** | Fichiers de substitution (placeholders) avec attributs `RECALL_ON_DATA_ACCESS` / `RECALL_ON_OPEN` : **ne jamais lire leur contenu** sans action de l'utilisateur. |
| **Fichiers verrouillés** | Restart Manager (`RmStartSession`, `RmRegisterResources`, `RmGetList`) pour savoir quel processus tient un fichier. |
| **Noms réservés et limites** | `CON`, `PRN`, `AUX`, `NUL`, `COM1-9`, `LPT1-9` (même avec une extension) ; caractères interdits ; limite de 260 caractères (contournée par `\\?\`) ; 255 caractères par composant. |
| **Tri** | `StrCmpLogicalW` (naturel), ou clés `LCMapStringEx` avec `SORT_DIGITSASNUMBERS` (ce que fait FastExplorer, en plus rapide). |
| **Processus** | `explorer.exe` héberge aussi la barre des tâches et le bureau ; option « processus séparé » pour les fenêtres de dossiers. FastExplorer est indépendant : s'il plante, la barre des tâches reste en vie. |

---

## 22. Feuille de route proposée

Regroupement des **P0** pour remplacer l'Explorateur au quotidien, dans un ordre logique de dépendances :

1. ✅ **Fondations Shell** : menu contextuel Shell complet, propriétés natives, icônes réelles (exe/lnk) en tâche de fond, raccourcis `.lnk`.
2. **Opérations de fichiers** : nouveau dossier, renommer (F2, validation), copier/couper/coller (presse-papiers interopérable), supprimer (Corbeille et définitif), moteur de copie avec progression et conflits.
3. **Glisser-déposer** OLE, dans les deux sens.
4. 🟡 **Rafraîchissement automatique** : fait pour le dossier affiché ; reste l'arrivée des lecteurs.
5. **Volet de navigation** : dossiers épinglés + arborescence de Ce PC ; libellés et jauges des lecteurs.
6. **Fil d'Ariane** dans la barre d'adresse.
7. **Affichage** : éléments cachés et système, masquage des extensions (en respectant les préférences de l'utilisateur).
8. **Barre d'état** : résumé de la sélection (nombre, taille).
9. **Recherche** dans le dossier courant (puis l'index MFT en P1 ⭐).

Puis les **P1** : onglets, annuler/rétablir, aperçu, miniatures, taille des dossiers ⭐, double panneau ⭐,
file d'attente de copies ⭐, filtre instantané ⭐, autocomplétion, archives…

---

### Sources consultées (nouveautés 2025–2026)

- [Windows Insider Blog : améliorations de l'Explorateur et du menu contextuel (août 2026)](https://blogs.windows.com/windows-insider/2026/08/17/improving-file-explorer-context-menu-faster-simpler-and-more-customizable/)
- [Pureinfotech : mise à jour de novembre 2026 (KB5132061)](https://pureinfotech.com/kb5127936-windows-11-november-2026-update-best-features/)
- [Windows Latest : mise à jour d'octobre 2026](https://www.windowslatest.com/2026/10/13/i-tested-windows-11-october-2026-update-heres-everything-new-improved-and-fixed/)
- [Windows Latest : fonctionnalités prévues en 2026](https://www.windowslatest.com/2026/04/09/full-list-of-features-coming-to-windows-11-in-2026/)
- [Mobigyaan : Windows 11 26H2](https://www.mobigyaan.com/microsoft-windows-11-26h2-2026-update)
- [Mobileos : guide des nouveautés 2026](https://mobileos.it/2026/08/windows-11-file-explorer-new-features-2026-guide/)
- [Windows Central : nouveautés IA de l'Explorateur en 2025](https://www.windowscentral.com/microsoft/windows-11/file-explorers-best-features-available-in-2025-on-windows-11-version-25h2-and-24h2)
- [Windows Central : gérer les actions IA](https://www.windowscentral.com/microsoft/windows-11/how-to-manage-ai-actions-in-file-explorer-on-windows-11)
- [Windows Report : fichiers recommandés et Copilot (KB5067036)](https://windowsreport.com/?p=1476213)
- [Windows Central : section IA masquée quand elle est vide](https://www.windowscentral.com/microsoft/windows-11/windows-11s-file-explorer-will-no-longer-show-ai-actions-when-theres-no-actionable-ai-finally-decluttering-the-right-click-menu)

Les éléments marqués ⚠️ sont à confirmer sur la machine. Les nouveautés 2026 proviennent en partie de
sites tiers, et leur déploiement varie selon les versions.
