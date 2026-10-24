<p align="center">
  <img src="assets/liu-bunny.png" width="92" height="128" alt="Liu Terminal">
</p>

<h1 align="center">Liu Terminal</h1>

<p align="center">
  <strong>Un terminale per ogni agente IA.</strong><br>
  Liu sta tra te e una varietà di agenti di coding e fa sembrare dieci thread paralleli un unico posto.<br>
  Scritto in <strong>C</strong>, nativo su macOS, accelerato dalla GPU.
</p>

<p align="center">
  <a href="https://liu.software">Download</a>
  ·
  <a href="https://github.com/calculusteam/liu/releases">Release</a>
  ·
  <a href="https://liu.software/docs">Documentazione</a>
  ·
  <a href="https://github.com/calculusteam/liu">GitHub</a>
</p>

<p align="center">
  <strong>Lingue:</strong>
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

## Funzionalità

### Cronologia agenti

Premi Cmd+K per aprire la palette comandi, poi digita "agent history" per sfogliare ogni CLI di IA installata (Claude Code, Codex, Copilot, OpenCode, Qwen, Cline, Antigravity). Ripristina in modo nativo o scorri la trascrizione senza avviare l'agente.

### Vista agenti

Liu traccia l'agente in ogni pannello diviso, non solo in ogni scheda. La scheda prende il nome dalla chat dell'agente, mostra un logo per ogni pannello con agente e fa scorrere una barra di attività mentre viene prodotto output. Tieni il puntatore sopra e un pannello elenca ogni divisione: agente, argomento della chat e una pillola con `working 1m 12s` oppure `idle 40s`. Quando un agente completa un tratto di lavoro, sotto quella scheda scende una scheda informativa e parte il suono di completamento.

### Prompt vocale

Premi **⌘⇧M** e parla. Whisper large-v3-turbo gira su questo Mac (niente rete, niente account, niente caricato) e le parole vengono scritte nel pannello che ha il focus, quindi funziona in Claude Code, in una shell normale, in un REPL. Una forma d'onda dal vivo mostra cosa sta davvero sentendo il microfono; quando ti fermi la traccia si congela e una scansione la attraversa mentre l'audio viene trascritto. Scegli il microfono, scegli fra tutte le 100 lingue che Whisper conosce, oppure puntalo a una API vocale in cloud.

### Schizzo

Premi **⌘K** e scegli *Sketch*: sopra il terminale si apre un foglio da disegno; penna, linea, freccia, rettangolo e gomma, 14 colori e un cursore per lo spessore del tratto. **P** **L** **A** **R** **E** cambiano strumento, **⌘Z** annulla. **Esc** o **Return** trasforma il disegno in un PNG e lo mette negli appunti, pronto da incollare in un agente che legge le immagini: mostra un layout invece di descriverlo. **⌥+Esc** chiude senza copiare.

### Crea tema con IA

Descrivi un aspetto, un agente locale emette JSON, Liu lo applica in tempo reale. Salvato in `~/.config/Liu/themes/`. L'agente riceve la guida ai temi di Liu e compila tutti i 25 colori. Vengono proposti solo gli agenti presenti nel tuo PATH; il risultato si aggiunge ai 19 temi integrati in Impostazioni → Appearance.

### Lettore e editor Markdown

Il browser dei file apre README, progetti e note agente: anteprima Markdown stilizzata o modifica raw in finestra. Immagini, codice e link in lettura. I blocchi di codice mostrano il linguaggio e un pulsante Copia, e `![[note]]` incorpora un'altra nota in linea.

### Editor di codice

Premi **⌘K** e scegli *Code Editor*: la scheda diventa un piccolo IDE; la barra esplora file a sinistra, il file a destra, un'icona del tipo di file su ogni riga e scheda. Un solo evidenziatore guidato da tabelle copre più di 50 linguaggi, e lo script dentro un componente Vue, Svelte o Astro è analizzato dallo stesso codice di un file .js. **⌘F** cerca, **⌘⇧F** sostituisce, **⌘S** salva; la barra di stato mostra riga/colonna, linguaggio, fine riga e codifica.

### Vista a grafo

Un grafo di conoscenza di ogni file Markdown in una cartella: i nodi sono file, gli archi sono `[[wikilink]]` e incorporamenti. Un layout a forze li dispone; passa il mouse su una nota per illuminare i suoi vicini, trascina per fissare, clicca per aprire.

### Vault delle note

Ogni nota vive in un unico Vault. **⌘K** → Crea nota fa scrivere il Markdown a un agente locale direttamente lì, i `[[wikilink]]` le collegano e alimentano il Grafo, e Apri nota / Cerca note salta ovunque. Gli agenti nel terminale ereditano `$LIU_VAULT` e scrivono anch'essi lì: **⌘⇧A** restituisce qualsiasi nota come `@source`. Un'unica base di conoscenza, condivisa da te e dai tuoi agenti.

### Sistema di notifiche integrato

`liu-notify` parla solo quando un agente ha davvero bisogno di te, e solo allora: un avviso vocale, una notifica desktop o un suono personalizzato. Un servizio in background indipendente che continua a funzionare anche con Liu chiuso.

### Risparmio di token

Premi **Ctrl+⌘** due volte nel prompt: il LLM locale accelerato da Metal (o l'agente scelto) traduce in inglese *prima* che la richiesta lasci la tua macchina. La finestra di contesto si riduce, i costi API crollano.

### SSH integrato

Client SSH completo dietro una Session: invio e download file live, browser remoto, menu per sessione, port forwarding (-L / -D), auth password/chiave/agent, TOFU known-hosts, riconnessione Mosh e cronologia recente.

### Tavolozza dei comandi

Un singolo **⌘K** esegue ricerca fuzzy su ogni comando, impostazione, host, snippet, tema e voce di Agent History: niente caccia ai menu, nessuna memoria muscolare necessaria. Cambia anche forma: `Open Note` è un selettore rapido di note, `Search Notes` cerca nel testo dell'intero Vault e `Go to Heading` scorre l'indice del documento aperto.

### Divisioni + Schede

Fino a 32 schede, ciascuna con fino a 8 pannelli in qualsiasi albero di divisione H/V. Trascina e rilascia pannelli. **⌘⇧I** trasmette l'input a tutti i pannelli attivi simultaneamente. Trascina una scheda fuori per creare una finestra, o sulla barra di un'altra finestra per unirla. Dopo la chiusura e la riapertura, Liu ripristina le tue schede.

### Esplora file

Browser locale + SFTP nella barra laterale con apertura con un clic. NSFilePromise trascina su Finder e trasferisce file in background. Cache LRU immagini da 32 MiB. Si apre nella directory di lavoro del pannello attivo. Trascina un file dal Finder su un pannello e il suo percorso viene scritto nel prompt, oppure caricato via SFTP se il pannello è una sessione SSH.

### Sospensione schede

Le schede inattive per N minuti si sospendono automaticamente: CPU e RAM scendono quasi a zero. Clicca per riprendere all'istante esattamente dove l'hai lasciato, scrollback intatto. Va in sospensione solo una scheda ferma al prompt. Una build, un server di sviluppo o un agente a metà turno la tengono sveglia; durante il sonno lo scrollback viene salvato su disco.

### Gruppi di schede

Raggruppa i terminali correlati in gruppi con nome e colore direttamente nella barra delle schede, in stile Chrome. Crea con **⌘⇧G**, trascina e rilascia o clic destro. Comprimi un gruppo per nascondere le sue schede e sospenderle, liberando le loro connessioni PTY/SSH. Ricolora da una palette di 8 colori, rinomina in linea, separa o chiudi un intero gruppo in una volta. I gruppi con nome e colore sopravvivono ai riavvii: fino a 8 alla volta.

### Gestore siti

Registra cartelle di progetto locali come siti, avvia e ferma i loro server di sviluppo da un unico pannello e osserva i log di processo in tempo reale: URL locale, HMR e stato a colpo d'occhio. Apri il browser, riavvia o rimuovi un sito senza lasciare il terminale.

### Liu Browser

Attiva il selettore di elementi nel browser integrato di Liu e clicca su qualsiasi cosa in una pagina live: un pulsante, una scheda, una riga disallineata. Liu ne cattura il selettore CSS, il tag, il testo, le dimensioni e l'HTML completo e lo inserisce inline nel prompt del tuo agente come chip di contesto. Niente screenshot, niente copia-incolla: indica il nodo esatto e Claude, Codex o qualsiasi CLI vede esattamente ciò che vedi tu. Perfetto per "sistema questo", "ristila quello" o "perché non è centrato?".

---

## Piattaforme supportate

| Piattaforma | Architettura | Formato |
| :--- | :--- | :--- |
| **macOS** | Apple Silicon (arm64) | `.dmg` |
| **macOS** | Intel (x86_64) | `.dmg` |

macOS è l’obiettivo principale e l’unico supportato al momento.

## Il nostro obiettivo

- [ ] **Supporto Linux**, build ARM64 e x86_64 (`.tar.gz`), apt (PPA), dnf (COPR) e AUR. Linux è pianificato ma non ancora supportato.

---

## Compilare dal sorgente

**Dipendenze**

- macOS, `brew install libssh2`
- SQLite3, stb_truetype, stb_image e cJSON sono in `third_party/`

```bash
git clone https://github.com/calculusteam/liu.git && cd liu

./build.sh              # TUI di build interattiva (macOS)
./build.sh --yes        # build consigliata non interattiva
./build.sh --package    # build + artefatto di release

# Oppure CMake direttamente:
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(sysctl -n hw.ncpu)
./build/Liu
```

Una singola build produce anche `liu-notify` (daemon notifiche) e `liu-history` (CLI cronologia agenti).

---

<p align="center">
  <sub>
    Apache-2.0
  </sub>
</p>
