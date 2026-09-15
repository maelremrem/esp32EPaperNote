# ESP32-S3 ePaper Voice Notes — Document de design technique

Version : 0.1  
Date : 15 septembre 2026  
Cible : Waveshare ESP32-S3-ePaper-1.54 V2 + serveur Whisper auto-hébergé en LXC

---

## 1. Résumé du projet

Le projet est un appareil portable minimaliste de prise de notes vocales basé sur un **Waveshare ESP32-S3-ePaper-1.54 V2**.

L'appareil ne réalise pas la transcription localement. Son rôle est de :

1. enregistrer une note audio lorsque l'utilisateur appuie sur l'unique bouton d'interface ;
2. sauvegarder immédiatement l'audio sur carte microSD/TF ;
3. fonctionner même sans réseau ;
4. se connecter automatiquement au meilleur réseau Wi-Fi connu ;
5. envoyer les notes en attente à un serveur Whisper hébergé dans un LXC ;
6. récupérer la transcription ;
7. stocker la note texte sur la carte SD ;
8. afficher le résultat sur l'écran ePaper.

Le principe fondamental est **offline-first** : aucune note ne doit être perdue si le Wi-Fi, le serveur ou Internet est indisponible.

---

## 2. Objectifs

### 2.1 Objectifs principaux

- Prise de note vocale en un geste.
- Fonctionnement hors ligne complet pour l'enregistrement.
- Synchronisation différée automatique.
- Transcription française via Whisper sur serveur local.
- Conservation locale de la transcription sur carte SD.
- Interface très simple adaptée à un écran ePaper.
- Utilisation avec un seul bouton applicatif.
- Connexion prioritaire au Wi-Fi domestique puis au hotspot iPhone.
- Faible consommation en veille.
- Architecture tolérante aux coupures Wi-Fi et aux redémarrages.

### 2.2 Hors périmètre initial

- STT Whisper directement sur l'ESP32-S3.
- Édition complète du texte sur l'appareil.
- Clavier Bluetooth dans la première version.
- Synchronisation cloud tierce.
- Interface tactile.
- Lecture audio/TTS dans la première version.

---

## 3. Matériel cible

### 3.1 Carte principale

**Waveshare ESP32-S3-ePaper-1.54 V2**.

Caractéristiques officielles pertinentes :

| Élément | Spécification |
|---|---|
| MCU | ESP32-S3-PICO-1-N8R8 |
| CPU | Dual-core Xtensa LX7, jusqu'à 240 MHz |
| Flash | 8 MB |
| PSRAM | 8 MB |
| SRAM interne | 512 KB |
| Wi-Fi | 2.4 GHz 802.11 b/g/n |
| Bluetooth | Bluetooth 5 LE |
| Écran | ePaper 1.54" |
| Résolution réelle | **200 × 200 px** |
| Audio | Codec ES8311 |
| Entrée audio | Microphone embarqué |
| Stockage | Slot carte TF/microSD |
| RTC | PCF85063 |
| Capteur | SHTC3 température/humidité |
| USB | USB-C natif ESP32-S3 |
| Batterie | Connecteur Li-ion + gestion de charge |

> Important : les maquettes UI de ce dossier ont été conçues comme références carrées. Le panneau réel est **200 × 200 px**. Les versions `screens/native_200x200/` sont uniquement des références réduites ; l'interface finale devra être redessinée nativement à 200 × 200 plutôt que simplement redimensionnée.

### 3.2 Révision matérielle

Le firmware doit cibler explicitement la **V2**.

La documentation Waveshare indique que :

- la V1 utilise un ESP32-S3FH4R2 avec 4 MB Flash et 2 MB PSRAM ;
- la V2 utilise un ESP32-S3-PICO-1-N8R8 avec 8 MB Flash et 8 MB PSRAM ;
- les exemples V1 et V2 ne sont pas interchangeables.

Le code devra donc isoler les définitions hardware dans un fichier dédié, par exemple :

```text
src/board/waveshare_epaper_154_v2.h
```

### 3.3 Bouton unique

Le design logiciel considère **un seul bouton applicatif**.

Le bouton PWR reste réservé à la gestion d'alimentation.

Recommandation : utiliser un bouton externe sur un GPIO libre, par exemple GPIO1, GPIO2 ou GPIO3 si le boîtier le permet.

Le bouton BOOT/GPIO0 peut techniquement être lu après démarrage, mais il s'agit d'une broche de strapping utilisée pour le mode bootloader. Il est préférable de ne pas en faire le bouton principal si un GPIO libre est disponible.

---

## 4. Architecture globale

```text
                ┌──────────────────────────┐
                │ Waveshare ESP32-S3       │
                │ ePaper 1.54 V2           │
                │                          │
Button ────────►│ State machine            │
Micro + ES8311 ►│ Recorder                 │
                │ SD queue                 │
                │ Wi-Fi manager            │
                │ Sync client              │
                │ ePaper UI                │
                └────────────┬─────────────┘
                             │ HTTPS
                  ┌──────────┴──────────┐
                  │                     │
             Wi-Fi maison          Hotspot iPhone
             priorité 100          priorité 50
                  │                     │
                  └──────────┬──────────┘
                             │
                             ▼
                    Reverse proxy HTTPS
                             │
                             ▼
                    ┌─────────────────┐
                    │ LXC Notes/STT   │
                    │                 │
                    │ REST API        │
                    │ Job queue       │
                    │ Whisper         │
                    │ SQLite          │
                    └─────────────────┘
```

---

## 5. Architecture firmware ESP32

### 5.1 Framework recommandé

**PlatformIO + ESP-IDF**.

Raisons :

- meilleur contrôle des tâches FreeRTOS ;
- API I2S native pour l'ES8311 ;
- contrôle fin du Wi-Fi ;
- SDMMC ;
- deep sleep ;
- watchdog ;
- stockage NVS ;
- meilleure séparation des composants qu'un sketch monolithique.

Structure proposée :

```text
firmware/
├── platformio.ini
├── sdkconfig.defaults
├── partitions.csv
├── include/
│   └── config.h
└── src/
    ├── main.cpp
    ├── app/
    │   ├── app_state.cpp
    │   ├── button.cpp
    │   └── events.cpp
    ├── audio/
    │   ├── audio_capture.cpp
    │   └── wav_writer.cpp
    ├── display/
    │   ├── epaper.cpp
    │   ├── ui.cpp
    │   └── icons.cpp
    ├── storage/
    │   ├── sd_store.cpp
    │   └── note_queue.cpp
    ├── network/
    │   ├── wifi_manager.cpp
    │   └── api_client.cpp
    ├── power/
    │   └── power_manager.cpp
    └── board/
        └── waveshare_epaper_154_v2.h
```

---

## 6. Machine d'états

États principaux :

```text
BOOT
  │
  ▼
IDLE
  │ click / double-click selon contexte
  ▼
RECORDING
  │ click
  ▼
SAVING
  │
  ▼
PENDING_SYNC
  │ réseau disponible
  ▼
UPLOADING
  │
  ▼
WAITING_TRANSCRIPTION
  │ résultat reçu
  ▼
STORE_TRANSCRIPT
  │
  └──────────────► IDLE
```

États secondaires :

```text
MENU
OFFLINE
SYNC_ERROR
SD_ERROR
SERVER_ERROR
LOW_BATTERY
```

### 6.1 Règle importante

La fin de l'enregistrement doit toujours écrire le fichier audio sur SD **avant** toute tentative réseau.

Le Wi-Fi ne fait jamais partie du chemin critique de création d'une note.

---

## 7. Interaction avec le bouton unique

Le comportement doit rester prévisible.

### 7.1 État IDLE

| Action | Fonction |
|---|---|
| Clic simple | Note suivante / écran suivant |
| Double clic | Démarrer une nouvelle note |
| Appui long | Ouvrir le menu |

### 7.2 État RECORDING

| Action | Fonction |
|---|---|
| Clic simple | Arrêter et sauvegarder |
| Appui long | Annuler l'enregistrement, optionnel |

La V1 peut ne pas implémenter l'annulation afin de réduire les risques de suppression accidentelle.

### 7.3 État MENU

| Action | Fonction |
|---|---|
| Clic simple | Élément suivant |
| Appui long | Valider l'élément sélectionné |

### 7.4 Détection logicielle

Valeurs de départ recommandées :

```text
Debounce       : 35 ms
Double-click   : <= 350 ms
Long press     : >= 800 ms
Very long press: >= 2500 ms, réservé
```

---

## 8. Interface ePaper

### 8.1 Principes graphiques

- monochrome ;
- fond blanc ;
- traits noirs fins ;
- typographie bitmap/monospace ;
- très peu de cadres ;
- beaucoup d'espace négatif ;
- icônes simples ;
- informations hiérarchisées ;
- pas de faux boutons tactiles ;
- une seule ligne d'aide en bas liée au bouton physique.

### 8.2 Contraintes ePaper

L'écran ePaper ne doit pas être utilisé comme un LCD temps réel.

À éviter :

- waveform animée à haute fréquence ;
- timer rafraîchi chaque seconde avec full refresh ;
- animations ;
- clignotements fréquents.

Pendant l'enregistrement, la waveform des mockups est une **indication visuelle conceptuelle**.

Implémentation recommandée :

- afficher l'écran RECORDING une fois au démarrage ;
- mettre à jour le timer à faible fréquence uniquement si le driver supporte correctement le partial refresh ;
- sinon ne mettre à jour l'écran qu'à la fin de l'enregistrement ;
- faire périodiquement un full refresh pour supprimer le ghosting.

### 8.3 Écrans inclus

#### 01 — IDLE / NOTE

Référence :

```text
screens/reference/01_idle_note.png
```

Contenu :

- statut Wi-Fi ;
- SD ;
- batterie ;
- date/heure ;
- numéro de note ;
- durée audio ;
- transcription ;
- état de synchronisation.

#### 02 — RECORDING

```text
screens/reference/02_recording.png
```

Contenu :

- micro ;
- état REC ;
- durée ;
- waveform illustrative ;
- rappel `1x arrêter`.

#### 03 — OFFLINE QUEUE

```text
screens/reference/03_offline_queue.png
```

Contenu :

- réseau absent ;
- nombre de notes en attente ;
- dernière note ;
- possibilité de forcer une synchro.

#### 04 — SYNCING

```text
screens/reference/04_syncing.png
```

Contenu :

- progression globale ;
- note actuellement envoyée ;
- indication Whisper.

#### 05 — MENU

```text
screens/reference/05_menu.png
```

Menu initial :

```text
Nouvelle note
Forcer sync
Historique
Réseau
```

---

## 9. Enregistrement audio

### 9.1 Format recommandé

Pour Whisper :

```text
Container       WAV
Codec           PCM signed 16-bit LE
Channels        Mono
Sample rate     16 kHz
Bitrate         256 kbit/s
```

Calcul stockage :

```text
16000 samples/s × 2 bytes = 32000 bytes/s
≈ 1.92 MB/minute
```

Une carte SD de 1 GB peut donc déjà contenir plusieurs heures de notes brutes.

### 9.2 Fichier temporaire

Pendant l'enregistrement :

```text
/audio/recording/current.tmp
```

À l'arrêt :

1. finaliser l'en-tête WAV ;
2. `fsync` ;
3. fermer le fichier ;
4. renommer atomiquement vers `/audio/pending/<id>.wav`.

Exemple :

```text
/audio/pending/20260915T220104Z-0043.wav
```

Cette séquence évite qu'une coupure batterie crée une note considérée à tort comme valide.

---

## 10. Identifiant des notes

Chaque note possède un identifiant stable :

```text
YYYYMMDDTHHMMSSZ-NNNN
```

Exemple :

```text
20260915T220104Z-0043
```

Cet identifiant est utilisé pour :

- le fichier WAV ;
- les métadonnées ;
- l'API ;
- l'idempotence ;
- le nom du Markdown final.

---

## 11. Organisation de la carte SD

```text
/
├── audio/
│   ├── pending/
│   ├── uploading/
│   ├── archive/
│   └── recording/
├── notes/
│   ├── 2026/
│   │   └── 09/
│   └── index.jsonl
├── queue/
│   └── queue.jsonl
├── config/
│   └── device.json
└── logs/
    └── latest.log
```

### 11.1 Politique audio

Option par défaut :

- conserver le WAV jusqu'à réception de la transcription ;
- après succès, déplacer le WAV dans `/audio/archive/` ;
- une option pourra supprimer automatiquement les archives âgées de N jours.

Jamais supprimer le WAV tant que le fichier Markdown n'a pas été écrit et synchronisé sur la SD.

---

## 12. Format d'une note locale

Exemple :

```markdown
# Note 0043

Réunion projet demain matin.

---

- ID: 20260915T220104Z-0043
- Recorded: 2026-09-15T22:01:04+02:00
- Duration: 11.2 s
- Language: fr
- STT: whisper
- Status: synced
```

Nom du fichier :

```text
/notes/2026/09/20260915T220104Z-0043.md
```

---

## 13. Index local

Pour éviter de scanner tous les Markdown au démarrage :

```json
{"id":"20260915T220104Z-0043","file":"/notes/2026/09/20260915T220104Z-0043.md","created":1789502464,"status":"synced"}
```

Un objet JSON par ligne dans :

```text
/notes/index.jsonl
```

Le format JSONL permet des append simples et réduit les risques de corruption d'un gros JSON monolithique.

---

## 14. Gestion Wi-Fi

### 14.1 Réseaux configurés

Exemple :

```cpp
struct WifiProfile {
    const char* ssid;
    const char* password;
    uint8_t priority;
};

WifiProfile profiles[] = {
    {"Maison", "...", 100},
    {"iPhone-Mael", "...", 50},
};
```

### 14.2 Priorité

Ordre :

```text
1. Wi-Fi maison
2. Hotspot iPhone
3. Offline
```

### 14.3 Règles de bascule

- ne jamais changer de réseau pendant un enregistrement ;
- ne jamais interrompre un upload actif uniquement pour changer de SSID ;
- après une opération réseau terminée, migrer vers un réseau de priorité supérieure s'il est disponible ;
- en offline, scanner périodiquement avec backoff.

Exemple :

```text
5 s → 15 s → 30 s → 60 s → 5 min
```

### 14.4 Hotspot iPhone

L'ESP32-S3 utilise le Wi-Fi **2.4 GHz**.

Sur les iPhone récents, le mode **Maximiser la compatibilité** désactive le 5 GHz/6 GHz pour le hotspot et force le 2.4 GHz avec WPA2, ce qui peut être nécessaire pour garantir la connexion de l'ESP32.

Configuration iPhone recommandée :

```text
Réglages
→ Partage de connexion
→ Maximiser la compatibilité : activé
```

### 14.5 VPN iPhone

Ne pas supposer que les clients Wi-Fi connectés au hotspot de l'iPhone bénéficient automatiquement du tunnel VPN actif sur l'iPhone.

Deux architectures sont possibles :

#### A. API purement privée

```text
ESP → hotspot iPhone → VPN → LAN → LXC
```

À utiliser uniquement après validation réelle du routage avec le VPN utilisé.

#### B. API HTTPS accessible depuis Internet — recommandée pour l'usage nomade

```text
ESP → hotspot iPhone → Internet → HTTPS → Nginx Proxy Manager → LXC
```

L'API est protégée par token, TLS, rate limiting et limite de taille.

Cette option rend le fonctionnement indépendant du comportement VPN/tethering d'iOS.

---

## 15. Synchronisation offline-first

### 15.1 Algorithme

```text
if recording:
    do nothing network-related

if network available:
    load oldest pending note
    upload note
    wait/poll transcription job
    validate response
    write Markdown
    fsync
    mark note synced
    archive audio
    process next pending note
```

### 15.2 File FIFO

Les notes sont synchronisées dans l'ordre chronologique.

```text
oldest first
```

### 15.3 Retry

Les erreurs suivantes sont retryables :

- DNS ;
- timeout ;
- connexion refusée ;
- HTTP 429 ;
- HTTP 500/502/503/504.

Backoff exemple :

```text
10 s
30 s
1 min
5 min
15 min
30 min
```

Les erreurs HTTP 400/401/403 doivent être affichées comme erreur de configuration et ne doivent pas être retryées agressivement.

---

## 16. API serveur proposée

### 16.1 Création d'un job

```http
POST /api/v1/notes
Authorization: Bearer <token>
Idempotency-Key: 20260915T220104Z-0043
Content-Type: multipart/form-data
```

Form fields :

```text
id
recorded_at
duration_ms
language=fr
device_id
file=<wav>
```

Réponse :

```json
{
  "id": "20260915T220104Z-0043",
  "status": "queued"
}
```

HTTP :

```text
202 Accepted
```

### 16.2 Lecture de l'état

```http
GET /api/v1/notes/20260915T220104Z-0043
Authorization: Bearer <token>
```

Pendant traitement :

```json
{
  "id": "20260915T220104Z-0043",
  "status": "transcribing"
}
```

Terminé :

```json
{
  "id": "20260915T220104Z-0043",
  "status": "done",
  "language": "fr",
  "duration": 11.2,
  "text": "Réunion projet demain matin."
}
```

### 16.3 Idempotence

Si l'ESP renvoie la même note après une coupure réseau, le serveur ne doit pas créer une seconde transcription.

La clé est :

```text
Idempotency-Key = note_id
```

---

## 17. Architecture LXC

Stack recommandée :

```text
Debian LXC
├── Nginx ou Nginx Proxy Manager en frontal
├── notes-api
│   ├── FastAPI
│   ├── SQLite
│   └── queue worker
└── whisper.cpp
```

Alternative : `faster-whisper` peut remplacer `whisper.cpp` si la machine hôte offre de meilleures performances avec cette stack.

### 17.1 Arborescence serveur

```text
/opt/voice-notes/
├── app/
├── models/
├── data/
│   ├── jobs/
│   ├── audio/
│   └── transcriptions.db
└── config/
    └── config.env
```

### 17.2 SQLite

Table minimale :

```sql
CREATE TABLE jobs (
    id TEXT PRIMARY KEY,
    device_id TEXT NOT NULL,
    created_at TEXT NOT NULL,
    status TEXT NOT NULL,
    audio_path TEXT,
    transcript TEXT,
    language TEXT,
    error TEXT
);
```

---

## 18. Sécurité

### 18.1 Transport

Toujours utiliser :

```text
HTTPS
```

sauf lors des tout premiers tests strictement LAN.

### 18.2 Authentification

V1 : bearer token généré aléatoirement d'au moins 32 octets.

```http
Authorization: Bearer <device-token>
```

Le token doit être conservé dans NVS ou dans une partition de configuration, pas dans les logs.

### 18.3 Serveur

À appliquer :

- maximum upload, par exemple 20 MB ;
- rate limit ;
- timeout ;
- validation stricte du MIME ;
- validation WAV côté serveur ;
- pas d'exécution de nom de fichier fourni par le client ;
- journalisation sans token ;
- service Whisper non exposé directement sur Internet.

---

## 19. Gestion du temps

Sources par ordre de priorité :

```text
1. RTC PCF85063
2. NTP quand Wi-Fi disponible
3. heure retournée par le serveur
```

Au démarrage :

- lire le RTC ;
- si réseau disponible, synchroniser NTP ;
- corriger le RTC si nécessaire.

Le stockage interne doit utiliser ISO 8601.

Exemple :

```text
2026-09-15T22:01:04+02:00
```

---

## 20. Batterie et énergie

### 20.1 Stratégie

En dehors d'un enregistrement ou d'une synchronisation :

```text
render ePaper
flush state
Wi-Fi off
deep sleep
```

Réveil possible par :

- bouton ;
- timer RTC ;
- événement programmé de synchronisation.

### 20.2 Synchronisation périodique

Si des notes sont en attente :

```text
wake every 5 min
→ test Wi-Fi
→ sync si disponible
→ sleep
```

Si aucune note n'est en attente, aucun réveil réseau périodique n'est nécessaire.

---

## 21. Stratégie de rendu ePaper

L'interface doit être générée par primitives plutôt qu'avec des screenshots bitmap complets.

Exemples :

```text
font rendering
icons 1-bit
horizontal lines
progress bar
text wrapping
```

Pourquoi :

- bien plus léger en Flash ;
- texte dynamique ;
- meilleur rendu à 200 × 200 ;
- localisation future ;
- moins de RAM ;
- pas de dépendance aux mockups rasterisés.

Les PNG du dossier servent uniquement de référence visuelle.

---

## 22. Configuration utilisateur

Fichier de configuration logique :

```json
{
  "device_name": "voice-note-01",
  "language": "fr",
  "api_url": "https://notes.example.net",
  "audio_archive_days": 30,
  "wifi": [
    {"ssid": "Maison", "priority": 100},
    {"ssid": "iPhone-Mael", "priority": 50}
  ]
}
```

Les mots de passe Wi-Fi et tokens ne doivent pas être écrits en clair sur la SD si cela peut être évité.

Recommandation : stockage NVS chiffré à terme.

---

## 23. Journalisation

Niveaux :

```text
ERROR
WARN
INFO
DEBUG
```

Sur USB série en développement.

Sur SD en production, limiter fortement les écritures pour :

- préserver la carte ;
- réduire la consommation ;
- éviter les risques de corruption.

Le fichier `/logs/latest.log` peut être circulaire avec une taille maximale, par exemple 128 KB.

---

## 24. Gestion des erreurs

### SD absente

```text
SD ERROR
Insert SD card
```

L'enregistrement est refusé si aucun stockage sûr n'est disponible.

### Wi-Fi absent

Aucun problème fonctionnel.

La note reste `pending`.

### Serveur indisponible

La note reste `pending`.

### Transcription échouée

La note reste disponible avec son WAV.

État :

```text
transcription_error
```

### Redémarrage pendant upload

Au reboot :

- déplacer toute entrée `/audio/uploading/` vers `/audio/pending/` ;
- reprendre grâce à l'idempotency key.

### Coupure pendant écriture Markdown

Écrire d'abord :

```text
<id>.md.tmp
```

puis rename atomique vers :

```text
<id>.md
```

---

## 25. État persistant

Le firmware doit pouvoir redémarrer à n'importe quel moment sans perdre le contexte.

État persistant minimal :

```text
last_note_id
current_queue_count
last_sync_at
wifi_failure_count
selected_ui_note
```

Le contenu critique reste néanmoins sur SD afin que la carte soit la source de vérité.

---

## 26. OTA

Prévoir OTA dès le début même si elle n'est pas activée dans le MVP.

Recommandation :

- dual OTA partitions ;
- firmware signé à terme ;
- update uniquement quand batterie suffisante ;
- jamais lancer OTA pendant enregistrement ou sync d'une note.

---

## 27. Découpage FreeRTOS proposé

```text
ui_task
audio_task
storage_task
network_task
sync_task
power_task
```

### Priorités conceptuelles

```text
audio_task   élevée
storage_task élevée
ui_task      moyenne
network_task moyenne
sync_task    basse
power_task   basse
```

Pendant RECORDING :

- l'audio et l'écriture SD sont prioritaires ;
- le Wi-Fi peut être désactivé ou laissé inactif ;
- aucun refresh ePaper lourd ne doit provoquer d'underrun audio.

---

## 28. Concurrence et buffers audio

Utiliser une architecture double-buffer ou ring-buffer :

```text
ES8311/I2S
   ↓
DMA
   ↓
ring buffer
   ↓
SD writer
```

Le réseau ne lit jamais directement le buffer d'enregistrement.

Il ne travaille que sur un fichier WAV déjà fermé.

---

## 29. MVP

Le MVP est terminé lorsque :

- [ ] un clic/double-clic démarre une note ;
- [ ] le micro ES8311 produit un WAV 16 kHz mono correct ;
- [ ] le fichier est sauvegardé sur SD ;
- [ ] l'appareil fonctionne sans Wi-Fi ;
- [ ] les notes pending persistent après reboot ;
- [ ] le Wi-Fi maison est prioritaire ;
- [ ] le hotspot iPhone fonctionne ;
- [ ] la note est envoyée au LXC ;
- [ ] Whisper retourne du français ;
- [ ] le Markdown final est écrit sur SD ;
- [ ] l'UI affiche la transcription ;
- [ ] le WAV n'est pas perdu en cas d'échec de sync.

---

## 30. Phases de développement

### Phase 1 — Bring-up hardware

- démarrage carte V2 ;
- ePaper ;
- SD ;
- bouton ;
- RTC ;
- batterie ;
- ES8311 et microphone.

### Phase 2 — Audio

- capture I2S ;
- WAV ;
- validation sur PC ;
- enregistrement fiable de 1 à 10 minutes.

### Phase 3 — Storage

- IDs ;
- queue ;
- index ;
- récupération après reboot.

### Phase 4 — UI

- IDLE ;
- RECORDING ;
- OFFLINE ;
- SYNCING ;
- MENU.

### Phase 5 — Wi-Fi

- multi-profile ;
- priorité ;
- hotspot iPhone ;
- backoff.

### Phase 6 — LXC

- FastAPI ;
- authentification ;
- stockage job ;
- whisper.cpp ;
- API asynchrone.

### Phase 7 — Sync

- upload ;
- poll ;
- retry ;
- idempotence ;
- Markdown final.

### Phase 8 — Power

- Wi-Fi off ;
- deep sleep ;
- réveil bouton ;
- sync périodique.

### Phase 9 — Robustesse

- tests coupure batterie ;
- SD pleine ;
- serveur down ;
- Wi-Fi instable ;
- audio long ;
- OTA.

---

## 31. Tests de validation

### Test A — Offline

1. désactiver tout Wi-Fi ;
2. enregistrer 5 notes ;
3. redémarrer l'ESP ;
4. vérifier que les 5 WAV sont présents ;
5. reconnecter le Wi-Fi ;
6. vérifier la transcription des 5 notes.

### Test B — Coupure réseau pendant upload

1. lancer sync ;
2. couper le Wi-Fi en milieu d'upload ;
3. restaurer Wi-Fi ;
4. vérifier qu'une seule note existe côté serveur ;
5. vérifier que le WAV local n'a pas été supprimé prématurément.

### Test C — Coupure d'alimentation pendant enregistrement

1. enregistrer ;
2. couper l'alimentation ;
3. reboot ;
4. vérifier que le `.tmp` n'est pas considéré comme note finale.

### Test D — Priorité réseau

1. rendre Maison et iPhone disponibles ;
2. vérifier Maison ;
3. couper Maison ;
4. vérifier bascule iPhone ;
5. restaurer Maison ;
6. vérifier retour vers Maison après la transaction réseau en cours.

### Test E — Serveur Whisper down

1. enregistrer une note ;
2. arrêter l'API ;
3. vérifier état pending ;
4. redémarrer API ;
5. vérifier synchronisation automatique.

---

## 32. Points d'attention

### Résolution

Le hardware est **200 × 200**, pas 240 × 240.

### ePaper

Ne pas traiter l'écran comme un écran animé.

### Hotspot iPhone

Activer **Maximiser la compatibilité** si nécessaire afin de garantir du 2.4 GHz.

### VPN iPhone

Tester explicitement le routage des clients du hotspot vers le VPN. Ne pas dépendre de ce comportement sans validation.

### Bouton BOOT

GPIO0 est une broche de strapping. Préférer un GPIO libre pour le bouton principal si possible.

### SD

La documentation Waveshare demande une carte TF en FAT32.

### V1 / V2

Ne jamais mélanger les configurations GPIO et exemples de versions différentes.

---

## 33. Évolutions possibles

Après le MVP :

- recherche dans les notes ;
- tags automatiques via LLM local ;
- résumé journalier ;
- classement projet/personnel ;
- endpoint Home Assistant ;
- interface Web pour consulter les notes ;
- synchronisation Git/Markdown ;
- export Obsidian ;
- chiffrement local ;
- suppression vocale ;
- TTS pour lire une note ;
- clavier Bluetooth ;
- OTA depuis une release GitHub privée ;
- indicateur température/humidité discret ;
- batterie restante estimée ;
- page diagnostics réseau/SD/audio.

---

## 34. Maquettes incluses dans cette archive

```text
screens/
├── reference/
│   ├── 01_idle_note.png
│   ├── 02_recording.png
│   ├── 03_offline_queue.png
│   ├── 04_syncing.png
│   └── 05_menu.png
└── native_200x200/
    ├── 01_idle_note_200x200_1bit.png
    ├── 02_recording_200x200_1bit.png
    ├── 03_offline_queue_200x200_1bit.png
    ├── 04_syncing_200x200_1bit.png
    └── 05_menu_200x200_1bit.png
```

Les fichiers `reference/` sont les cinq dernières maquettes générées.

Les fichiers `native_200x200/` sont des conversions 1-bit destinées uniquement à aider à visualiser les contraintes du panneau réel. Il est recommandé de reconstruire l'UI en primitives graphiques.

---

## 35. Sources techniques

Documentation officielle Waveshare :

- ESP32-S3-ePaper-1.54 : https://docs.waveshare.com/ESP32-S3-ePaper-1.54
- Ressources et schéma : https://docs.waveshare.com/ESP32-S3-ePaper-1.54/Resources-And-Documents

Documentation Apple, hotspot :

- Personal Hotspot / Maximize Compatibility : https://support.apple.com/guide/security/wi-fi-security-secfd166f620/web

Ces sources doivent être revérifiées lors du développement si Waveshare publie une nouvelle révision matérielle.

---

## 24. Starter project implementation included in this ZIP

This design has now been materialized into an initial project tree.

```text
esp32_s3_voice_notes_project/
├── README.md
├── firmware/
│   ├── platformio.ini
│   ├── sdkconfig.defaults
│   ├── partitions.csv
│   ├── include/
│   │   ├── board_pins.h
│   │   ├── project_config.h
│   │   ├── secrets.example.h
│   │   └── secrets.h
│   └── src/
│       ├── main.cpp
│       ├── app/button.*
│       ├── audio/audio_recorder.*
│       ├── audio/wav_writer.*
│       ├── display/epaper_display.*
│       ├── display/ui.*
│       ├── network/api_client.*
│       ├── network/wifi_manager.*
│       └── storage/note_store.*
├── lxc/
│   ├── server/
│   │   ├── app.py
│   │   └── requirements.txt
│   ├── systemd/voice-notes-api.service
│   ├── lxc.env.example
│   └── scripts/
│       ├── deploy-proxmox.sh
│       ├── install-server.sh
│       ├── update-server.sh
│       └── test-api.sh
└── docs/
    ├── TECHNICAL_DESIGN.md
    ├── references/
    └── screens/
```

### 24.1 Firmware stack

The starter implementation uses **PlatformIO + ESP-IDF** and the Espressif `esp_codec_dev` managed component for the ES8311 codec.

Default audio format:

```text
16 kHz
16-bit signed PCM
mono
WAV
```

The firmware performs the following sequence:

```text
button double-click
      ↓
record to /sdcard/audio/recording/current.tmp
      ↓
short press
      ↓
finalize WAV header + fsync
      ↓
atomic rename to /sdcard/audio/pending/<note-id>.wav
      ↓
Wi-Fi available?
  ┌───┴────┐
 no       yes
  │         │
wait      POST WAV
            ↓
          Whisper
            ↓
      transcription JSON
            ↓
     write Markdown + fsync
            ↓
      archive original WAV
```

### 24.2 Hardware definitions used by the starter

All board assumptions live in `firmware/include/board_pins.h`.

| Function | GPIO |
|---|---:|
| ePaper power | 6 |
| ePaper BUSY | 8 |
| ePaper RST | 9 |
| ePaper DC | 10 |
| ePaper CS | 11 |
| ePaper SCLK | 12 |
| ePaper MOSI | 13 |
| ES8311 MCLK | 14 |
| ES8311 BCLK | 15 |
| ES8311 ADC data to ESP | 16 |
| ES8311 LRCK | 38 |
| ES8311 DAC data from ESP | 45 |
| Audio power | 42 |
| PA control | 46 |
| I2C SDA | 47 |
| I2C SCL | 48 |
| SD CLK | 39 |
| SD D0 | 40 |
| SD CMD | 41 |
| Application button | 1 |

The application button is intentionally placed on a free GPIO rather than GPIO0/BOOT. It is expected to be wired between GPIO1 and GND and uses the ESP32 internal pull-up.

### 24.3 ePaper implementation rule

The included display driver is deliberately minimal and uses full refresh for the MVP. The application must not attempt LCD-like animation.

The recording screen is drawn once at recording start. The waveform is a static visual indicator. A live timer is intentionally not refreshed once per second.

Before enclosure freeze, validate the following on the real V2 board:

- image orientation;
- BUSY polarity;
- full refresh waveform behavior;
- ghosting after multiple UI transitions;
- sleep/power-off sequence.

If required, only `display/epaper_display.cpp` should need adaptation to the exact Waveshare waveform implementation.

### 24.4 LXC API implementation

The starter server uses:

```text
FastAPI
Uvicorn
faster-whisper
SQLite
systemd
```

Default listen address:

```text
0.0.0.0:8080
```

Main endpoint:

```http
POST /api/v1/notes/{note_id}/transcribe
Authorization: Bearer <token>
Content-Type: audio/wav
```

The request body is the raw WAV file. The API stores the audio file before starting transcription.

Successful response:

```json
{
  "id": "20260915T220104Z-0043",
  "status": "done",
  "language": "fr",
  "duration": 11.2,
  "text": "Réunion projet demain matin.",
  "model": "small",
  "sha256": "...",
  "error": null
}
```

### 24.5 API idempotence

`note_id` is the primary key of the server SQLite table.

If the ESP retries a note for which transcription is already complete, the server returns the existing result. This is required because the ESP cannot safely know whether a connection failure happened before or after the server completed a request.

### 24.6 Initial LXC sizing

Suggested starting point for CPU inference:

```text
Debian 13 LXC
4 vCPU
4 GB RAM
1 GB swap
16 GB disk
Whisper model: small
Compute type: int8
Language: fr
```

These values are configurable in `lxc/lxc.env`.

### 24.7 Deployment flow

On Proxmox:

```bash
cd lxc
cp lxc.env.example lxc.env
nano lxc.env
./scripts/deploy-proxmox.sh
```

The deployment script:

1. locates a Debian 13 LXC template;
2. creates an unprivileged LXC;
3. starts it;
4. copies the server files;
5. installs Python, FFmpeg and the virtual environment;
6. installs faster-whisper;
7. generates an API token if none is supplied;
8. installs and starts a systemd unit.

### 24.8 First development milestones

Recommended order:

1. build and flash a minimal firmware;
2. validate SD mounting;
3. validate ePaper full refresh;
4. validate the one-button event detector;
5. record a WAV and inspect it on a PC;
6. deploy the LXC and test it with `curl`;
7. configure the ESP API URL and token;
8. test one online recording end-to-end;
9. disable Wi-Fi and record three notes;
10. restore Wi-Fi and verify automatic queue synchronization;
11. test hotspot fallback;
12. add deep sleep only after the complete flow is stable.

### 24.9 Known MVP limitations

- history browsing is represented in the menu but not implemented yet;
- RTC-specific time acquisition is not implemented yet; NTP is used when Wi-Fi is available;
- battery percentage is not yet read from the ADC;
- OTA is reserved for a later milestone;
- ePaper partial refresh is not enabled in the initial driver;
- network credentials are compile-time values in `secrets.h`; moving them to NVS is planned;
- the synchronous HTTP request can keep the ESP awake while Whisper works; an asynchronous job API can be introduced later if needed.
