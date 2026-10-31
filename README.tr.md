<p align="center">
  <img src="assets/liu-bunny.png" width="92" height="128" alt="Liu Terminal">
</p>

<h1 align="center">Liu Terminal</h1>

<p align="center">
  <strong>Her yapay zeka ajanı için tek terminal.</strong><br>
  Liu, sizi çeşitli kodlama ajanlarıyla buluşturur ve on paralel iş parçacığını tek bir yer gibi hissettirir.<br>
  <strong>C</strong> ile yazılmıştır, macOS'ta yerel, GPU hızlandırmalı.
</p>

<p align="center">
  <a href="https://liu.software">İndir</a>
  ·
  <a href="https://github.com/calculusteam/liu/releases">Sürümler</a>
  ·
  <a href="https://liu.software/docs">Belgeler</a>
  ·
  <a href="https://github.com/calculusteam/liu">GitHub</a>
</p>

<p align="center">
  <strong>Diller:</strong>
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

## Özellikler

### Ajan Geçmişi

Cmd+K ile komut paletini açın, ardından "agent history" yazarak yüklü tüm yapay zeka CLI araçlarına (Claude Code, Codex, Copilot, OpenCode, Qwen, Cline, Antigravity) göz atın. Ajanı başlatmadan yerel olarak oturumu devam ettirin veya transkripti kaydırın.

### Ajan Görünümü

Liu, ajanı sekme başına değil, her bölme için izler. Sekme kendini ajanın sohbetine göre adlandırır, ajan çalıştıran her bölme için bir logo taşır ve çıktı üretilirken bir etkinlik çubuğu süzülür. İşaretçiyi üzerinde tutun; bir panel her bölmeyi listeler: ajan, sohbet konusu ve `working 1m 12s` ya da `idle 40s` yazan bir rozet. Bir ajan çalışma dilimini bitirdiğinde o sekmenin altına bir kart iner ve tamamlanma sesi çalar.

### Ajanlar Ajan Başlatır

Bir bölmedeki ajan işi Liu üzerinden başka bir ajana devredebilir: `liu-agent start codex "port the tests"`. Liu önce size sorar, sonra onu isteyen sekmenin sağına bitişik küçük bir iş sekmesi olarak çalıştırır: durumunu gösteren hareketli bir pusun üzerinde logosu. İşaretçiyi üzerinde tutun, en yeni satırlarını görün; tıklayın, sohbeti açılsın: sıradaki turu gönderin, durdurun ya da oturumunu bir bölmede sürdürün. Birlikte başlatılan ajanlar ekip olarak çalışır; ikisi aynı dosyaya dokunduğunda haberdar edilirler.

### Alt Ajan Sekmeleri

Bir ajan kendi alt ajanlarını başlattığında (Claude Code'un Agent aracı, Codex, OpenCode, Copilot CLI, Cursor, Qwen Code, Droid, Crush ve diğerleri), her biri ajanın sekmesinin yanında bir iş sekmesi olur ve ajanın kendi kayıtlarından canlı okunur. Beş alt ajan, beş sekme; her birinin pusu kendine ait bir görüntü. Birinin üzerinde durun, en yeni satırlarını görün; tıklayın, tüm kaydını açın. Bittiğinde her biri tamamlandı ya da başarısız olarak görünür ve bir sonraki isteminizde temizlenir.

### Sesli Komut

**⌘⇧M**'ye basın ve konuşun. Whisper large-v3-turbo bu Mac'te çalışır (ağ yok, hesap yok, hiçbir şey yüklenmez) ve kelimeler hangi bölme odaktaysa oraya yazılır; yani Claude Code'da da, düz bir kabukta da, bir REPL'de de çalışır. Canlı dalga formu mikrofonun gerçekte ne duyduğunu gösterir; durduğunuzda iz donar ve ses yazıya dökülürken üzerinden bir tarama geçer. Mikrofonu seçin, Whisper'ın bildiği 100 dilden birini seçin ya da bunun yerine bulut tabanlı bir konuşma API'sine yönlendirin.

### Çizim Tahtası

**⌘K**'ya basıp *Sketch*'i seçin: terminalin üzerinde bir çizim tahtası açılır; kalem, çizgi, ok, kutu ve silgi, 14 renk ve bir kalınlık kaydırıcısı. **P** **L** **A** **R** **E** araç değiştirir, **⌘Z** geri alır. **Esc** ya da **Return** çizimi PNG'ye çevirip panoya koyar; görüntü okuyan bir ajana yapıştırmaya hazırdır; bir arayüzü anlatmak yerine gösterin. **⌥+Esc** kopyalamadan kapatır.

### Yapay Zeka ile Tema Oluşturma

Bir görünüm tarif edin, yerel bir ajan JSON üretsin, Liu bunu canlı olarak uygulsın. `~/.config/Liu/themes/` dizinine kaydedilir. Ajan, Liu'nun kendi tema rehberini alır ve 25 renk yuvasının tamamını doldurur. Yalnızca PATH'inizde bulunan ajanlar listelenir; sonuç Ayarlar → Appearance'ta 19 yerleşik temanın yanında yer alır.

### Markdown Okuyucu ve Düzenleyici

Dosya tarayıcı README, tasarım ve ajan notlarını açar: pencere içinde stilli Markdown önizleme veya ham düzenleme. Okuma modunda resim, kod ve bağlantılar işlenir. Kod blokları dil etiketi ve Kopyala düğmesi taşır; `![[note]]` başka bir notu satır içinde gömer.

### Kod Düzenleyici

**⌘K**'ya basıp *Code Editor*'ü seçin: sekme küçük bir IDE'ye dönüşür; solda gezgin rayı, sağda dosya, her satırda ve sekmede bir dosya türü simgesi. Tablo güdümlü tek vurgulayıcı 50'den fazla dili kapsar; bir Vue, Svelte ya da Astro bileşeninin içindeki script, bir .js dosyasıyla aynı kodla ayrıştırılır. **⌘F** bulur, **⌘⇧F** değiştirir, **⌘S** kaydeder; durum çubuğu satır/sütun, dil, satır sonu ve kodlamayı gösterir.

### Grafik Görünümü

Bir klasördeki her Markdown dosyası için bilgi grafiği: düğümler dosyalar, kenarlar `[[wikilink]]` ve gömülerdir. Kuvvet yönelimli yerleşim onları oturtur; bir notun üzerine gelince komşuları aydınlanır, sürükleyip sabitleyin, tıklayıp açın.

### Notlar Kasası

Her not tek bir Kasa'da yaşar. **⌘K** → Not Oluştur, yerel bir ajanın doğrudan buraya Markdown yazmasını sağlar; `[[wikilink]]`'ler notları birbirine bağlar ve Grafiği besler, Not Aç / Notlarda Ara ile her yere atlarsınız. Terminaldeki ajanlar `$LIU_VAULT`'u devralıp oraya da yazar: **⌘⇧A** herhangi bir notu `@kaynak` olarak geri besler. Sizin ve ajanlarınızın paylaştığı tek bilgi tabanı.

### Yerleşik Bildirim Sistemi

`liu-notify`, AI ajanları gerçekten size ihtiyaç duyduğunda (ve yalnızca o zaman) sesli uyarı, masaüstü bildirimi veya kişiselleştirilmiş bir ses çalar. Liu'nun içinde çalışır: kancalar açılışta ajanlarınıza eklenir ve Liu kapanınca kaldırılır; böylece başka yerde çalıştırdığınız ajanlar etkilenmez.

### Token Tasarrufu

İstem satırında **Ctrl+⌘** tuşlarına iki kez basın: yerel Metal-hızlandırmalı LLM (veya seçtiğiniz ajan) talep makineden çıkmadan İngilizce dışındaki metni İngilizceye çevirir. Bağlam penceresi küçülür, API faturaları dramatik düşer.

### Yerleşik SSH

Tek Session arkasında tam SSH istemcisi: canlı dosya gönderme ve indirme, uzak dosya tarayıcısı, oturum menüsü, port yönlendirme (-L / -D), parola / anahtar / agent kimlik doğrulama, known-hosts TOFU, Mosh yeniden bağlanma ve son oturum geçmişi.

### Komut Paleti

Tek **⌘K** ile her komut, ayar, sunucu, snippet, tema ve Ajan Geçmişi kaydında fuzzy arama: menü avına gerek yok, kas hafızası gerekmez. Şekil de değiştirir: `Open Note` bir not hızlı geçişi, `Search Notes` Kasa'nızda tam metin arama, `Go to Heading` ise açık belgenin ana hattında gezinme sağlar.

### Bölmeler + Sekmeler

32 sekmeye kadar, her birinde H/V split ağacında 8 bölmeye kadar. Bölme sürükle-bırak. **⌘⇧I** tüm aktif bölmelere aynı komutu eşzamanlı yayınlar. Bir sekmeyi dışarı sürükleyerek yeni pencere açın ya da başka bir pencerenin sekme şeridine bırakarak ona katın. Çıkıp yeniden açtığınızda Liu sekmelerinizi geri getirir.

### Kaldığınız Yerden Devam

Liu çalışırken sekmelerinizi kaydeder; çıkış, çökme ya da yeniden başlatma sonrası aynı pencereler, sekmeler, bölmeler ve klasörler geri gelir, her bölme önceki ekranını soluk bir `Restored session` satırının üzerinde gösterir. Çalışmakta olan bir ajan, kabuk tarafından kendi oturumunda yeniden başlatılır (Claude Code, Codex, OpenCode ve devam bayrağı olan her ajan); içine hiçbir şey yazılmaz.

### Dosya Tarayıcı

Kenar çubuğunda yerel + SFTP tarayıcı, tek tıkla açma. NSFilePromise sürükle-bırak Finder'a dosya transferi arka planda. 32 MiB LRU görüntü önbelleği. Odaklı bölmenin çalışma dizininde açılır. Finder'dan bir dosyayı bölmeye bırakırsanız yolu isteme yazılır; bölme bir SSH oturumuysa dosya SFTP ile yüklenir.

### Sekme Uyutma

N dakika boşta kalan sekmeler kendini otomatik askıya alır: CPU ve RAM neredeyse sıfıra düşer. Tıklayın, kaldığınız yerden anında devam edin, scrollback bozulmaz. Yalnızca istem satırında bekleyen sekme uykuya alınır. Bir derleme, geliştirme sunucusu ya da yanıt üreten bir ajan sekmeyi uyanık tutar; uykudayken kaydırma geçmişi diske yazılır.

### Sekme Grupları

İlgili terminalleri sekme çubuğunda adlandırılmış, renk kodlu gruplara toplayın, Chrome tarzı. Sekmenin sağ tık menüsünden veya komut paletinden oluşturun; sekmeleri sürükleyerek gruba alın ya da çıkarın. Bir grubu daraltıp sekmelerini gizleyin ve uyutun, PTY/SSH bağlantılarını serbest bırakın. 8 renklik paletten yeniden renklendirin, satır içinde yeniden adlandırın, grubu çözün veya tüm grubu tek seferde kapatın. Adlandırılmış, renkli gruplar yeniden başlatmalarda korunur: aynı anda en fazla 8 grup.

### Site Manager

Yerel proje klasörlerini site olarak kaydedin, dev sunucularını tek panelden başlatıp durdurun ve canlı süreç loglarını izleyin: Local URL, HMR ve durum bir bakışta. Tarayıcıyı açın, yeniden başlatın veya siteyi terminalden ayrılmadan kaldırın.

### Liu Browser

Liu'nun yerleşik tarayıcısında element seçiciyi açın ve canlı bir sayfadaki herhangi bir şeye tıklayın: bir buton, bir kart, hizası bozuk bir satır. Liu; öğenin CSS seçicisini, etiketini, metnini, boyutunu ve tüm HTML'ini yakalayıp bir bağlam çipi olarak doğrudan ajanınızın istemine ekler. Ekran görüntüsü yok, kopyala-yapıştır yok: tam olarak istediğiniz düğümü işaret edin; Claude, Codex ya da herhangi bir CLI tam olarak sizin gördüğünüzü görsün. "Şunu düzelt", "bunu yeniden biçimlendir" veya "bu neden ortalı değil?" için birebir.

---

## Desteklenen platformlar

| Platform | Mimari | Biçim |
| :--- | :--- | :--- |
| **macOS** | Apple Silicon (arm64) | `.dmg` |
| **macOS** | Intel (x86_64) | `.dmg` |

macOS birincil ve şu an desteklenen tek hedeftir.

## Hedefimiz

- [ ] **Linux desteği**, ARM64 ve x86_64 derlemeleri (`.tar.gz`), apt (PPA), dnf (COPR) ve AUR paketleri. Linux planlanıyor ancak şu an desteklenmiyor.

---

## Kaynaktan derleme

**Bağımlılıklar**

- macOS, `brew install libssh2`
- SQLite3, stb_truetype, stb_image ve cJSON `third_party/` altında gömülüdür

```bash
git clone https://github.com/calculusteam/liu.git && cd liu

./build.sh              # etkileşimli derleme TUI (macOS)
./build.sh --yes        # etkileşimsiz önerilen derleme
./build.sh --package    # derleme + sürüm paketi

# Veya doğrudan CMake:
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(sysctl -n hw.ncpu)
./build/Liu
```

Tek bir derleme ayrıca `liu-notify` (bildirim daemon’u) ve `liu-history` (ajan geçmişi CLI) üretir.

---

<p align="center">
  <sub>
    Apache-2.0
  </sub>
</p>
