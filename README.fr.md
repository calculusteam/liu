<p align="center">
  <img src="assets/liu-bunny.png" width="92" height="128" alt="Liu Terminal">
</p>

<h1 align="center">Liu Terminal</h1>

<p align="center">
  <strong>Un terminal pour chaque agent IA.</strong><br>
  Liu s’intercale entre vous et une variété d’agents de codage et fait en sorte que dix fils parallèles se sentent comme un seul lieu.<br>
  Écrit en <strong>C</strong>, natif sur macOS, accéléré par GPU.
</p>

<p align="center">
  <a href="https://liu.software">Télécharger</a>
  ·
  <a href="https://github.com/calculusteam/liu/releases">Releases</a>
  ·
  <a href="https://liu.software/docs">Documentation</a>
  ·
  <a href="https://github.com/calculusteam/liu">GitHub</a>
</p>

<p align="center">
  <strong>Langues :</strong>
  <a href="README.md">English</a>
  ·
  <a href="README.tr.md">Türkçe</a>
  ·
  <a href="README.zh.md">简体中文</a>
  ·
  <a href="README.ko.md">한국어</a>
  ·
  <a href="README.es.md">Español</a>
  ·
  <a href="README.de.md">Deutsch</a>
  ·
  <a href="README.it.md">Italiano</a>
  ·
  <a href="README.fr.md">Français</a>
  ·
  <a href="README.ja.md">日本語</a>
  ·
  <a href="README.ar.md">العربية</a>
</p>

---

## Fonctionnalités

### Historique des agents

Appuyez sur Cmd+K pour ouvrir la palette de commandes, puis tapez "agent history" pour parcourir chaque CLI d'IA installée : Claude Code, Codex, Copilot, OpenCode, Qwen, Cline, Antigravity. Reprenez nativement ou parcourez la transcription sans lancer l'agent.

### Vue des agents

Liu suit l'agent de chaque volet divisé, pas seulement de chaque onglet. L'onglet porte le nom du chat de l'agent, affiche un logo par volet doté d'un agent et fait défiler une barre d'activité pendant que de la sortie est produite. Posez le pointeur dessus : un panneau liste chaque volet; agent, sujet du chat et une pastille indiquant `working 1m 12s` ou `idle 40s`. Quand un agent termine une phase de travail, une carte descend sous cet onglet et le son de fin est joué.

### Prompt vocal

Appuyez sur **⌘⇧M** et parlez. Whisper large-v3-turbo tourne sur ce Mac (pas de réseau, pas de compte, rien d'envoyé) et les mots sont tapés dans le volet qui a le focus : dans Claude Code, dans un shell simple, dans un REPL. Une forme d'onde en direct montre ce que le micro entend vraiment ; quand vous arrêtez, la trace se fige et un balayage la parcourt pendant la transcription. Choisissez le micro, choisissez parmi les 100 langues que connaît Whisper, ou pointez-le vers une API vocale hébergée.

### Croquis

Appuyez sur **⌘K** et choisissez *Sketch* : une feuille de dessin s'ouvre sur le terminal; crayon, ligne, flèche, rectangle et gomme, 14 couleurs et un curseur d'épaisseur de trait. **P** **L** **A** **R** **E** changent d'outil, **⌘Z** annule. **Esc** ou **Return** transforme le dessin en PNG et le place dans le presse-papiers, prêt à coller dans un agent qui lit les images : montrez une mise en page au lieu de la décrire. **⌥+Esc** ferme sans copier.

### Création de thèmes par IA

Décrivez un look, un agent local émet du JSON, Liu l'applique en direct. Enregistré dans `~/.config/Liu/themes/`. L'agent reçoit le guide de thèmes de Liu et renseigne les 25 couleurs. Seuls les agents présents dans votre PATH sont proposés ; le résultat rejoint les 19 thèmes intégrés dans Paramètres → Appearance.

### Lecteur et éditeur Markdown

Le navigateur ouvre README, conceptions et notes agent : aperçu Markdown stylisé ou édition brute dans la fenêtre. Images, code et liens en lecture. Les blocs de code affichent leur langage et un bouton Copier, et `![[note]]` intègre une autre note en ligne.

### Éditeur de code

Appuyez sur **⌘K** et choisissez *Code Editor* : l'onglet devient un petit IDE; l'explorateur à gauche, le fichier à droite, une icône de type de fichier sur chaque ligne et chaque onglet. Un seul surligneur piloté par table couvre plus de 50 langages, et le script d'un composant Vue, Svelte ou Astro est analysé par le même code qu'un fichier .js. **⌘F** cherche, **⌘⇧F** remplace, **⌘S** enregistre ; la barre d'état affiche ligne/colonne, langage, fin de ligne et encodage.

### Vue en graphe

Un graphe de connaissances de chaque fichier Markdown d'un dossier : les nœuds sont des fichiers, les arêtes sont des `[[wikilinks]]` et des intégrations. Une disposition dirigée par les forces les place ; survolez une note pour éclairer ses voisins, glissez pour épingler, cliquez pour ouvrir.

### Coffre de notes

Chaque note vit dans un seul Coffre. **⌘K** → Créer une note laisse un agent local rédiger du Markdown directement dedans, les `[[wikilinks]]` les relient et alimentent le graphe, et Ouvrir une note / Rechercher des notes sautent n'importe où. Les agents du terminal héritent de `$LIU_VAULT` et y écrivent aussi : **⌘⇧A** renvoie n'importe quelle note comme `@source`. Une base de connaissances, partagée par vous et vos agents.

### Système de notification intégré

`liu-notify` ne parle que lorsqu'un agent a vraiment besoin de vous, et seulement à ce moment-là : une alerte vocale, une bannière de bureau ou un son personnalisé. Un service en arrière-plan autonome qui continue de fonctionner même lorsque Liu est fermé.

### Économie de jetons

Appuyez deux fois sur **Ctrl+⌘** dans votre prompt : le LLM local accéléré par Metal (ou l'agent choisi) traduit en anglais *avant* que la requête ne quitte votre machine. La fenêtre de contexte rétrécit, la facture API chute drastiquement.

### SSH intégré

Client SSH complet derrière une Session : envoi/téléchargement de fichiers en direct, navigateur distant, menu par session, redirection (-L / -D), auth mot de passe/clé/agent, TOFU known-hosts, reconnexion Mosh et historique.

### Palette de commandes

Un seul **⌘K** effectue une recherche floue sur chaque commande, paramètre, hôte, snippet, thème et entrée Agent History : pas besoin de chasser dans les menus. Elle change aussi de forme : `Open Note` est un sélecteur rapide de notes, `Search Notes` cherche dans tout le texte de votre Coffre et `Go to Heading` parcourt le plan du document ouvert.

### Divisions + Onglets

Jusqu'à 32 onglets, chacun avec jusqu'à 8 panneaux dans un arbre de division H/V. Glisser-déposer de panneaux. **⌘⇧I** diffuse l'entrée à tous les panneaux actifs simultanément. Faites glisser un onglet hors de la barre pour créer une fenêtre, ou sur la barre d'une autre fenêtre pour l'y joindre. Après un redémarrage, Liu rouvre vos onglets.

### Explorateur de fichiers

Navigateur local + SFTP dans la barre latérale, ouverture en un clic. NSFilePromise glisse vers le Finder et transfère les fichiers en arrière-plan. Cache d'images LRU de 32 Mio. Il s'ouvre dans le répertoire de travail du volet actif. Déposez un fichier du Finder sur un volet : son chemin est saisi dans l'invite, ou envoyé par SFTP si le volet est une session SSH.

### Mise en veille des onglets

Les onglets inactifs pendant N minutes se suspendent automatiquement : CPU et RAM tombent presque à zéro. Cliquez pour reprendre instantanément exactement là où vous vous étiez arrêté, scrollback intact. Seul un onglet resté à l'invite s'endort. Une compilation, un serveur de dev ou un agent en plein tour le garde éveillé ; pendant son sommeil, le scrollback est écrit sur disque.

### Grouper les onglets

Regroupez les terminaux liés en groupes nommés et colorés directement dans la barre d'onglets, façon Chrome. Créez avec **⌘⇧G**, glisser-déposer ou clic droit. Réduisez un groupe pour masquer ses onglets et les mettre en veille, libérant leurs connexions PTY/SSH. Recolorez depuis une palette de 8 couleurs, renommez en ligne, dissociez ou fermez tout un groupe d'un coup. Les groupes nommés et colorés survivent aux redémarrages : jusqu'à 8 à la fois.

### Gestionnaire de sites

Enregistrez des dossiers de projet locaux comme sites, démarrez et arrêtez leurs serveurs de dev depuis un panneau, et suivez les logs de processus en direct : URL locale, HMR et statut en un coup d'œil. Ouvrez le navigateur, redémarrez ou supprimez un site sans quitter le terminal.

### Liu Browser

Activez le sélecteur d'éléments dans le navigateur intégré de Liu et cliquez sur n'importe quoi dans une page en direct : un bouton, une carte, une ligne mal alignée. Liu récupère son sélecteur CSS, sa balise, son texte, sa taille et son HTML complet et l'insère en ligne dans le prompt de votre agent sous forme de puce de contexte. Pas de capture d'écran, pas de copier-coller : pointez le nœud exact et Claude, Codex ou n'importe quelle CLI voit précisément ce que vous voyez. Idéal pour « corrige ça », « restyle ça » ou « pourquoi ce n'est pas centré ? ».

---

## Plateformes prises en charge

| Plateforme | Architecture | Format |
| :--- | :--- | :--- |
| **macOS** | Apple Silicon (arm64) | `.dmg` |
| **macOS** | Intel (x86_64) | `.dmg` |

macOS est la cible principale et la seule actuellement prise en charge.

## Notre objectif

- [ ] **Support Linux**, builds ARM64 et x86_64 (`.tar.gz`), apt (PPA), dnf (COPR) et AUR. Linux est prévu mais pas encore pris en charge.

---

## Compiler depuis les sources

**Dépendances**

- macOS, `brew install libssh2`
- SQLite3, stb_truetype, stb_image et cJSON sont sous `third_party/`

```bash
git clone https://github.com/calculusteam/liu.git && cd liu

./build.sh              # TUI de build interactive (macOS)
./build.sh --yes        # build recommandée non interactive
./build.sh --package    # build + artefact de release

# Ou CMake directement :
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(sysctl -n hw.ncpu)
./build/Liu
```

Une seule compilation produit aussi `liu-notify` (daemon de notifications) et `liu-history` (CLI d’historique d’agents).

---

<p align="center">
  <sub>
    Apache-2.0
  </sub>
</p>
