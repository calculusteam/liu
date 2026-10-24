<p align="center">
  <img src="assets/liu-bunny.png" width="92" height="128" alt="Liu Terminal">
</p>

<h1 align="center">Liu Terminal</h1>

<p align="center">
  <strong>Un terminal para cada agente de IA.</strong><br>
  Liu se sitúa entre tú y una variedad de agentes de codificación y hace que diez hilos en paralelo se sientan como un solo lugar.<br>
  Escrito en <strong>C</strong>, nativo en macOS, acelerado por GPU.
</p>

<p align="center">
  <a href="https://liu.software">Descargar</a>
  ·
  <a href="https://github.com/calculusteam/liu/releases">Releases</a>
  ·
  <a href="https://liu.software/docs">Documentación</a>
  ·
  <a href="https://github.com/calculusteam/liu">GitHub</a>
</p>

<p align="center">
  <strong>Idiomas:</strong>
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

## Funciones

### Historial del agente

Pulsa Cmd+K para abrir la paleta de comandos y escribe "agent history" para navegar por cada CLI de IA instalado (Claude Code, Codex, Copilot, OpenCode, Qwen, Cline, Antigravity). Reanude de forma nativa o desplácese por la transcripción sin iniciar el agente.

### Vista de agentes

Liu rastrea el agente de cada panel dividido, no solo el de cada pestaña. La pestaña se nombra por el chat del propio agente, lleva un logotipo por panel con agente y recorre una barra de actividad mientras se produce salida. Deja el puntero encima y un panel lista cada división: agente, tema del chat y una pastilla que dice `working 1m 12s` o `idle 40s`. Cuando un agente termina un tramo de trabajo, cae una tarjeta bajo esa pestaña y suena el aviso de finalización.

### Prompt por voz

Pulsa **⌘⇧M** y habla. Whisper large-v3-turbo se ejecuta en este Mac (sin red, sin cuenta, sin subir nada) y las palabras se escriben en el panel que tenga el foco, así que funciona en Claude Code, en una shell normal o en un REPL. Una forma de onda en vivo muestra lo que el micrófono está oyendo de verdad; al parar, la traza se congela y un barrido la recorre mientras se transcribe el audio. Elige el micrófono, elige entre los 100 idiomas que conoce Whisper, o apúntalo a una API de voz en la nube.

### Boceto

Pulsa **⌘K** y elige *Sketch*: se abre una hoja de dibujo sobre la terminal; lápiz, línea, flecha, rectángulo y goma, 14 colores y un control de grosor. **P** **L** **A** **R** **E** cambian de herramienta y **⌘Z** deshace. **Esc** o **Return** convierte el dibujo en un PNG y lo deja en el portapapeles, listo para pegar en un agente que lea imágenes: enseña un diseño en vez de describirlo. **⌥+Esc** cierra sin copiar.

### Crear tema con IA

Describa un aspecto, un agente local emitirá un archivo JSON y Liu lo aplicará en vivo. Guardado en `~/.config/Liu/themes/`. El agente recibe la guía de temas de Liu, así que rellena los 25 colores. Solo se ofrecen los agentes que hay en tu PATH, y el resultado se une a los 19 temas integrados en Ajustes → Appearance.

### Lector y editor de Markdown

El explorador abre README, diseños y notas del agente: vista previa Markdown con estilo o edición en bruto en la ventana. Imágenes, código y enlaces en modo lectura. Los bloques de código muestran su lenguaje y un botón Copiar, y `![[note]]` inserta otra nota en línea.

### Editor de código

Pulsa **⌘K** y elige *Code Editor*: la pestaña se convierte en un pequeño IDE; el explorador a la izquierda, el archivo a la derecha, un icono de tipo de archivo en cada fila y pestaña. Un único resaltador basado en tablas cubre más de 50 lenguajes, y el script dentro de un componente Vue, Svelte o Astro se analiza con el mismo código que un archivo .js. **⌘F** busca, **⌘⇧F** reemplaza, **⌘S** guarda; la barra de estado muestra línea/columna, lenguaje, fin de línea y codificación.

### Vista de grafo

Un grafo de conocimiento de cada archivo Markdown de una carpeta: los nodos son archivos y las aristas son `[[wikilinks]]` e incrustaciones. Un diseño dirigido por fuerzas los acomoda; pasa el cursor por una nota para iluminar sus vecinos, arrástrala para fijarla, haz clic para abrirla.

### Bóveda de notas

Cada nota vive en una sola Bóveda. **⌘K** → Crear nota deja que un agente local redacte Markdown directamente en ella, los `[[wikilinks]]` las conectan y alimentan el grafo, y Abrir nota / Buscar notas salta a cualquier parte. Los agentes del terminal heredan `$LIU_VAULT` y también escriben ahí: **⌘⇧A** devuelve cualquier nota como `@source`. Una base de conocimiento, compartida por ti y tus agentes.

### Sistema de notificaciones integrado

`liu-notify` solo habla cuando un agente realmente te necesita, y solo entonces: una alerta de voz, una notificación de escritorio o un sonido personalizado. Un servicio en segundo plano independiente que sigue funcionando incluso con Liu cerrado.

### Ahorro de tokens

Presiona **Ctrl+⌘** dos veces en tu prompt: el LLM local acelerado por Metal (o el agente elegido) traduce a inglés *antes* de que la solicitud abandone tu máquina. La ventana de contexto se reduce y las facturas de la API caen drásticamente.

### SSH integrado

Cliente SSH completo tras una sola Session: envío y descarga de archivos en vivo, explorador remoto, menú por sesión, reenvío de puertos (-L / -D), auth contraseña/clave/agente, TOFU known-hosts, reconexión Mosh e historial reciente.

### Paleta de comandos

Un único **⌘K** realiza una búsqueda difusa de cada comando, ajuste, host, snippet, tema y entrada de Agent History: sin buscar en menús ni memoria muscular. También cambia de forma: `Open Note` es un selector rápido de notas, `Search Notes` busca en todo el texto de tu Bóveda y `Go to Heading` salta por el esquema del documento abierto.

### Divisiones + Pestañas

Hasta 32 pestañas, cada una con hasta 8 paneles en cualquier árbol de división H/V. Arrastrar y soltar paneles. **⌘⇧I** transmite la entrada a todos los paneles activos simultáneamente. Arrastra una pestaña fuera para crear una ventana, o sobre la barra de otra ventana para unirla. Al cerrar y volver a abrir, Liu recupera tus pestañas.

### Explorador de archivos

Explorador local + SFTP en la barra lateral con apertura de un clic. NSFilePromise arrastra a Finder y transfiere archivos en segundo plano. Caché LRU de imágenes de 32 MiB. Se abre en el directorio de trabajo del panel activo. Suelta un archivo de Finder en un panel y su ruta se escribe en el prompt; si el panel es una sesión SSH, se sube por SFTP.

### Suspensión de pestañas

Las pestañas inactivas durante N minutos se suspenden automáticamente: CPU y RAM caen a casi cero. Haz clic para reanudar al instante donde lo dejaste, scrollback intacto. Solo duerme una pestaña parada en el prompt. Una compilación, un servidor de desarrollo o un agente a mitad de turno la mantiene despierta; mientras duerme, el scrollback se guarda en disco.

### Grupos de pestañas

Agrupa terminales relacionados en grupos con nombre y color en la barra de pestañas, estilo Chrome. Crea con **⌘⇧G**, arrastrar y soltar, o clic derecho. Contrae un grupo para ocultar sus pestañas y suspenderlas, liberando sus conexiones PTY/SSH. Recolorea desde una paleta de 8 colores, renombra en línea, desagrupa o cierra todo un grupo a la vez. Los grupos con nombre y color sobreviven a los reinicios: hasta 8 a la vez.

### Gestor de sitios

Registra carpetas de proyecto locales como sitios, inicia y detiene sus servidores de desarrollo desde un panel y observa logs de proceso en vivo: URL local, HMR y estado de un vistazo. Abre el navegador, reinicia o elimina un sitio sin salir del terminal.

### Liu Browser

Activa el selector de elementos en el navegador integrado de Liu y haz clic en cualquier cosa de una página en vivo: un botón, una tarjeta, una fila desalineada. Liu captura su selector CSS, etiqueta, texto, tamaño y HTML completo y lo inserta en línea en el prompt de tu agente como un chip de contexto. Sin capturas, sin copiar y pegar: señala el nodo exacto y Claude, Codex o cualquier CLI ve exactamente lo que tú ves. Ideal para "arregla esto", "cambia el estilo de eso" o "¿por qué esto no está centrado?".

---

## Plataformas compatibles

| Plataforma | Arquitectura | Formato |
| :--- | :--- | :--- |
| **macOS** | Apple Silicon (arm64) | `.dmg` |
| **macOS** | Intel (x86_64) | `.dmg` |

macOS es el objetivo principal y el único admitido actualmente.

## Nuestro objetivo

- [ ] **Soporte Linux**, builds ARM64 y x86_64 (`.tar.gz`), apt (PPA), dnf (COPR) y AUR. Linux está planificado pero aún no se admite.

---

## Compilar desde el código fuente

**Dependencias**

- macOS, `brew install libssh2`
- SQLite3, stb_truetype, stb_image y cJSON están en `third_party/`

```bash
git clone https://github.com/calculusteam/liu.git && cd liu

./build.sh              # TUI de build interactiva (macOS)
./build.sh --yes        # build recomendada no interactiva
./build.sh --package    # build + artefacto de release

# O CMake directamente:
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(sysctl -n hw.ncpu)
./build/Liu
```

Una sola compilación también produce `liu-notify` (daemon de notificaciones) y `liu-history` (CLI de historial de agentes).

---

<p align="center">
  <sub>
    Apache-2.0
  </sub>
</p>
