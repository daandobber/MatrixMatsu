# media-proxy

Kleine reverse-proxy die vóór je *echte* Matrix-homeserver hangt, alleen voor MatrixMatsu (de Tanmatsu-client).

**Waarom**: MatrixMatsu's software H.264-decoder ondersteunt alleen Baseline/Constrained Baseline profile — een harde beperking van de decoder-library, niet iets dat in de firmware op te lossen is. De meeste telefooncamera's encoderen standaard High Profile, dus zonder deze proxy speelt bijna geen enkele binnenkomende `m.video` af op het apparaat.

**Hoe het werkt**: alle verkeer gaat gewoon één-op-één door naar je echte homeserver, **behalve** video-downloads. Die worden opgehaald, met `ffprobe` gecontroleerd (H.264-profiel, audio-codec, breedte), en zo nodig met `ffmpeg` omgezet naar Baseline + AAC-audio + max 480px breed. Het resultaat wordt gecachet op basis van het media-ID (Matrix-media is onveranderlijk na upload, dus de cache hoeft nooit ververst te worden).

**Alleen MatrixMatsu praat met deze proxy.** Je echte homeserver wordt op geen enkele manier aangepast — Element op je telefoon/PC blijft gewoon rechtstreeks verbinden en merkt hier niets van.

## Installeren

```sh
pip install -r requirements.txt
```

Daarnaast moet `ffmpeg` en `ffprobe` geïnstalleerd zijn en in je PATH staan (bijv. via je package manager, of [ffmpeg.org](https://ffmpeg.org/download.html) op Windows).

## Draaien

```sh
python proxy.py https://jouw-homeserver.example.org --port 8008
```

Opties:
- `--host` — luisteradres (standaard `0.0.0.0`, dus bereikbaar vanaf je LAN)
- `--cache-dir` — waar getranscodeerde bestanden komen (standaard `./media-proxy-cache`)
- `--max-width` — video's breder dan dit worden ook verkleind, zelfs als het profiel al klopt (standaard `240` — niet om op het scherm te passen, maar omdat decoderen op het apparaat zelf de bottleneck is: een 384×384-clip met Main-profile mat ~130ms/frame, dus flink kleiner dan het scherm van 480px is nodig om richting realtime te komen; `0` = uit)

## Draaien als achtergronddienst (bijv. naast je Synapse-server)

Als je toch al een server met je homeserver draaiend hebt, kost dit vrijwel niets extra — geen nieuwe machine nodig, gewoon een extra proces ernaast. Op een Linux-server met systemd:

1. Zet dit script ergens neer, bijv. `/opt/media-proxy/` (kopieer `proxy.py` en `requirements.txt`), en installeer de dependencies in een virtualenv:
   ```sh
   cd /opt/media-proxy
   python3 -m venv .venv
   .venv/bin/pip install -r requirements.txt
   ```
2. Zorg dat `ffmpeg`/`ffprobe` systeembreed geïnstalleerd zijn (bijv. `apt install ffmpeg` op Debian/Ubuntu).
3. Maak `/etc/systemd/system/media-proxy.service` aan (zie `media-proxy.service` in deze map als voorbeeld — pas het `ExecStart`-pad en je homeserver-URL aan).
4. Activeer 'm:
   ```sh
   sudo systemctl daemon-reload
   sudo systemctl enable --now media-proxy
   sudo systemctl status media-proxy   # controleren dat hij draait
   journalctl -u media-proxy -f        # logs live volgen
   ```

Draait dan gewoon mee vanaf boot, net als je Synapse-server zelf.

## MatrixMatsu instellen

Vul bij het inloggen op de Tanmatsu als "homeserver" het adres van deze proxy in, bijvoorbeeld:

```
http://192.168.1.50:8008
```

(niet het adres van je echte homeserver — die praat de proxy namens jou aan).

## Beperkingen / aandachtspunten

- Draait op plain HTTP; prima voor gebruik binnen je eigen LAN. Wil je van buitenaf kunnen inloggen, zet er zelf een TLS-terminating reverse proxy (nginx/caddy) voor.
- Moet blijven draaien terwijl je MatrixMatsu gebruikt (dus een PC/NAS/Raspberry Pi die aanstaat, niet je laptop die je dichtklapt).
- De eerste keer dat een video wordt opgevraagd kost transcoding tijd (seconden tot een halve minuut, afhankelijk van lengte/resolutie/host-cpu) — daarna komt hij uit de cache.
- Long-polling `/sync`-requests worden simpelweg doorgegeven met een ruime timeout; bij een extreem trage upstream kan dit een keer een sync-cyclus missen, dan probeert de client het vanzelf opnieuw.
