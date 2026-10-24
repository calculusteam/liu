<p align="center">
  <img src="assets/liu-bunny.png" width="92" height="128" alt="Liu Terminal">
</p>

<h1 align="center">Liu Terminal</h1>

<p align="center">
  <strong>一个终端，连接所有 AI 代理。</strong><br>
  Liu 位于你与各类编码代理之间，让十余条并行线程像在同一个地方工作。<br>
  使用 <strong>C</strong> 编写，macOS 原生，GPU 加速。
</p>

<p align="center">
  <a href="https://liu.software">下载</a>
  ·
  <a href="https://github.com/calculusteam/liu/releases">发行版</a>
  ·
  <a href="https://liu.software/docs">文档</a>
  ·
  <a href="https://github.com/calculusteam/liu">GitHub</a>
</p>

<p align="center">
  <strong>语言：</strong>
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

## 功能

### 代理历史记录

按 Cmd+K 打开命令面板，然后输入 "agent history" 即可浏览所有已安装的 AI CLI（Claude Code、Codex、Copilot、OpenCode、Qwen、Cline、Antigravity）。无需启动代理即可在本地恢复会话或滚动浏览转录文本。

### 代理视图

Liu 跟踪的是每个分屏面板中的代理，而不仅仅是每个标签页。标签页以代理自己的对话命名，为每个代理面板显示一个图标，并在产出内容时扫过一条活动条。把指针停在上面，面板会列出每个分屏：代理、对话主题，以及显示 `working 1m 12s` 或 `idle 40s` 的徽标。当代理完成一段工作时，该标签下方会落下一张卡片并播放完成提示音。

### 语音提示

按 **⌘⇧M** 然后说话。Whisper large-v3-turbo 在这台 Mac 上本地运行（无需联网、无需账号、不上传任何内容）识别出的文字会输入到当前获得焦点的分屏，因此在 Claude Code、普通 shell 和 REPL 中都能用。实时波形显示麦克风真正听到的内容；你停下后，波形冻结，并有一道扫描光掠过，同时音频被转写。可以选择麦克风、从 Whisper 支持的全部 100 种语言中挑选，或改用云端语音 API。

### 草图

按 **⌘K** 并选择 *Sketch*：终端上方会打开一块画板：画笔、直线、箭头、方框和橡皮擦，14 种颜色以及一个笔画粗细滑块。**P** **L** **A** **R** **E** 切换工具，**⌘Z** 撤销。**Esc** 或 **Return** 会把画作转成 PNG 并放入剪贴板，可直接粘贴给能读取图像的代理，与其描述界面，不如画给它看。**⌥+Esc** 不复制直接关闭。

### AI 创建主题

描述一种外观，本地代理会生成 JSON，Liu 会实时应用它。保存到 `~/.config/Liu/themes/`。代理会拿到 Liu 自带的主题指南，因此会填满全部 25 个颜色项。只列出 PATH 中能找到的代理，生成的主题会与 19 个内置主题一起出现在“设置 → Appearance”中。

### Markdown 阅读器与编辑器

文件浏览器打开 README、设计和代理笔记：窗口内样式化 Markdown 预览或原始编辑。阅读模式下渲染图像、代码和链接。代码块带有语言标签和“复制”按钮，`![[note]]` 可把另一篇笔记内嵌进来。

### 代码编辑器

按 **⌘K** 并选择 *Code Editor*：标签页变成一个小型 IDE：左侧是资源管理器栏，右侧是文件，每一行和每个标签都带有文件类型图标。一个表驱动的高亮器覆盖 50 多种语言，Vue、Svelte 或 Astro 组件内的 script 由与 .js 文件相同的代码进行词法分析。**⌘F** 查找，**⌘⇧F** 替换，**⌘S** 保存；状态栏显示行/列、语言、换行符和编码。

### 图谱视图

文件夹中每个 Markdown 文件的知识图谱：节点是文件，边是 `[[wikilinks]]` 和嵌入。力导向布局自动排布；悬停某条笔记会点亮其邻居，拖动可固定，点击可打开。

### 笔记库

每条笔记都存放在同一个库中。**⌘K** → 创建笔记，让本地代理直接把 Markdown 写入其中；`[[wikilinks]]` 将笔记互相连接并驱动图谱，"打开笔记 / 搜索笔记"可跳转到任意位置。在终端运行的代理会继承 `$LIU_VAULT` 并同样写入其中：**⌘⇧A** 可将任意笔记作为 `@source` 回传。一个知识库，由你和你的代理共享。

### 内置通知系统

`liu-notify` 仅在代理真正需要您时（且仅在此时）才会播放语音提醒、桌面横幅或自定义声音。它是一个独立的后台服务，即使 Liu 关闭也会持续运行。

### 节省 Token

在提示符中按两次 **Ctrl+⌘**：本地的 Metal 加速 LLM（或所选代理）会在请求离开机器*之前*将非英语翻译为英语。上下文窗口缩小，API 费用大幅下降。

### 内置 SSH

单一 Session 背后的完整 SSH 客户端：实时文件上传与下载、远程文件浏览器、会话菜单、端口转发（-L / -D）、密码/密钥/代理认证、known-hosts TOFU、Mosh 重连及最近会话历史。

### 命令面板

单个 **⌘K** 即可模糊搜索每个命令、设置、主机、片段、主题和 Agent History 条目：无需在菜单中翻找，无需肌肉记忆。它还会切换形态：`Open Note` 是笔记快速切换器，`Search Notes` 对整个库做全文搜索，`Go to Heading` 则在当前文档的大纲中跳转。

### 分屏 + 标签页

最多 32 个标签页，每个最多 8 个面板（H/V 分屏树）。面板拖放。**⌘⇧I** 将输入同时广播到所有活动面板。把标签拖出即可新建窗口，拖到另一窗口的标签栏上则并入该窗口。退出后重新打开，Liu 会恢复你的标签页。

### 文件浏览器

侧边栏本地 + SFTP 浏览器，一键打开。NSFilePromise 拖放到 Finder，后台传输文件。32 MiB LRU 图像缓存。在当前窗格的工作目录中打开。把 Finder 里的文件拖到窗格上，路径会直接输入到提示符；若该窗格是 SSH 会话，则通过 SFTP 上传。

### 标签页休眠

空闲 N 分钟的标签页自动挂起：CPU 和 RAM 降至接近零。点击即可立即从离开的位置恢复，回滚缓冲完整。只有停在提示符处的标签页才会休眠。构建、开发服务器或正在回合中的代理都会让它保持唤醒；休眠期间，回滚缓冲区会保存到磁盘。

### 标签页分组

在标签栏中将相关终端聚合为命名的、彩色编码的分组，Chrome 风格。通过 **⌘⇧G**、拖放或右键创建。折叠分组以隐藏其标签页并使其休眠，释放 PTY/SSH 连接。从 8 色调色板重新着色、内联重命名、取消分组，或一次性关闭整个分组。命名的彩色分组在重启后保留，最多同时 8 个。

### 站点管理器

将本地项目文件夹注册为站点，在一个面板中启动/停止开发服务器，并查看实时进程日志：Local URL、HMR 和状态一目了然。打开浏览器、重启或移除站点，无需离开终端。

### Liu 浏览器

在 Liu 内置浏览器中开启元素选择器，然后点击实时页面上的任意元素：一个按钮、一张卡片、一行错位的布局。Liu 会抓取它的 CSS 选择器、标签、文本、尺寸和完整 HTML，并作为上下文片段直接内联到代理的提示词中。无需截图，无需复制粘贴：指向确切的节点，Claude、Codex 或任意 CLI 就能看到你所看到的一切。非常适合"修一下这个""重新设计那个"或"这为什么没居中？"。

---

## 支持的平台

| 平台 | 架构 | 格式 |
| :--- | :--- | :--- |
| **macOS** | Apple Silicon (arm64) | `.dmg` |
| **macOS** | Intel (x86_64) | `.dmg` |

macOS 是主要且目前唯一支持的目标平台。

## 我们的目标

- [ ] **Linux 支持**，ARM64 与 x86_64 构建（`.tar.gz`）、apt (PPA)、dnf (COPR) 与 AUR。Linux 已在规划中，但当前不受支持。

---

## 从源码构建

**依赖**

- macOS，`brew install libssh2`
- SQLite3、stb_truetype、stb_image 与 cJSON 已置于 `third_party/`

```bash
git clone https://github.com/calculusteam/liu.git && cd liu

./build.sh              # 交互式构建 TUI（macOS）
./build.sh --yes        # 非交互推荐构建
./build.sh --package    # 构建 + 发布产物

# 或直接使用 CMake：
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(sysctl -n hw.ncpu)
./build/Liu
```

一次构建还会生成 `liu-notify`（通知守护进程）与 `liu-history`（代理历史 CLI）。

---

<p align="center">
  <sub>
    Apache-2.0
  </sub>
</p>
