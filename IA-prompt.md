# Implementatieprompt: OTA zonder Task Watchdog-melding

Je werkt in een bestaand ESP-IDF-project voor een ESP32-S3 internet-radio. Implementeer de beste oplossing voor de Task Watchdog-melding tijdens een OTA-update.

## Probleem

Tijdens een geslaagde OTA-update verschijnen meldingen zoals:

```text
E (...) task_wdt: Task watchdog got triggered. The following tasks/users did not reset the watchdog in time:
E (...) task_wdt:  - IDLE1 (CPU 1)
E (...) task_wdt: Tasks currently running:
E (...) task_wdt: CPU 0: ota_upload
E (...) task_wdt: CPU 1: radio_stream
```

Daarna kan de image alsnog succesvol worden gevalideerd en bootbaar worden gemaakt:

```text
ota_upload: OTA successful; boot partition set to ota_0
```

De huidige OTA-task draait op CPU0 en de audio-decode/I2S-task `radio_stream` draait op CPU1. Tijdens `esp_ota_end()` en image-validatie krijgt de idle-task op CPU1 onvoldoende gelegenheid om de Task Watchdog te bedienen. De oplossing moet daarom de audio- en netwerkstream gecontroleerd stilleggen vóór de zware OTA-validatie begint.

## Doel

Breid het lokale `mrwheel/ota_upload`-component uit met een optionele OTA lifecycle-callback waarmee de applicatie vóór ontvangst/validatie van een image de actieve audiostream kan stoppen of pauzeren. Koppel die callback in `main/app_main.c` aan `radio_audio`.

De OTA-upload moet daarna:

1. De audio-uitvoer en streamtaken gecontroleerd stoppen vóór `esp_ota_begin()` of uiterlijk vóór de zware validatiefase.
2. Geen actieve `radio_stream`- of `radio_fetch`-taak meer hebben wanneer `esp_ota_end()` wordt uitgevoerd.
3. De bestaande OTA-wire-protocolwerking behouden.
4. Bij een succesvolle update blijven rebooten als `CONFIG_OTA_UPLOAD_REBOOT_AFTER_UPDATE=y`.
5. Bij een mislukte upload geen corrupte bootpartitie achterlaten.
6. Geen globale Task Watchdog uitschakelen en de watchdog-timeout niet simpelweg verhogen.
7. Bestaande normale radiofunctionaliteit buiten OTA behouden.

## Belangrijke actuele codefeiten

Lees eerst de relevante bestanden en pas de oplossing aan de bestaande architectuur aan:

- `managed_components/mrwheel__ota_upload/include/ota_upload.h`
- `managed_components/mrwheel__ota_upload/ota_upload.c`
- `components/radio_audio/include/radio_audio.h`
- `components/radio_audio/radio_audio.c`
- `main/app_main.c`
- `sdkconfig.defaults`
- `sdkconfig`
- `projectPrompt.md`

De huidige `ota_upload_config_t` bevat alleen:

```c
typedef struct
{
  const char *hostname;
  uint16_t port;
  bool enable_mdns;
  bool reboot_after_update;
} ota_upload_config_t;
```

De huidige componentfuncties zijn onder andere:

```c
esp_err_t ota_upload_start(const ota_upload_config_t *config);
esp_err_t ota_upload_stop(void);
bool ota_upload_is_running(void);
```

`ota_upload_stop()` is momenteel niet geïmplementeerd en retourneert `ESP_ERR_NOT_SUPPORTED`; verander dit alleen als dat voor deze oplossing nodig is.

De OTA-ontvangst doet momenteel in essentie:

```c
esp_ota_begin(...);
recv(...);
esp_ota_write(...);
esp_ota_end(...);
esp_ota_set_boot_partition(...);
```

De huidige radio-audio-architectuur maakt:

- `radio_audio`-controller-task op de standaardcore;
- `radio_fetch` op CPU0;
- `radio_stream` op CPU1;
- `radio_stream` met een stack van 12288 bytes;
- `radio_fetch` met een stack van 6144 bytes.

`radio_audio_play()` start een nieuwe sessie en gebruikt `s_stop_requested`, `s_stream_running`, `s_fetch_running`, `s_stream_task` en `s_fetch_task` voor het wisselen van stations. Er bestaat al `radio_audio_set_paused(bool)` en `radio_audio_is_paused()`, maar verifieer of “paused” werkelijk beide streamtaken stopt. Gebruik geen schijnoplossing waarbij alleen PCM-uitvoer wordt gemute terwijl de netwerk- en decodeertaken CPU blijven gebruiken.

## Gewenste ontwerpkeuze

Gebruik bij voorkeur een kleine, optionele callback in het OTA-component, bijvoorbeeld conceptueel:

```c
typedef esp_err_t (*ota_upload_prepare_cb_t)(void *ctx);
```

en voeg aan de configuratie toe:

```c
ota_upload_prepare_cb_t prepare_cb;
void *prepare_ctx;
```

De exacte naam mag worden aangepast aan de lokale stijl, maar:

- de callback moet optioneel zijn;
- bestaande gebruikers van `OTA_UPLOAD_CONFIG_DEFAULT()` moeten blijven compileren;
- de defaultwaarde moet `NULL` zijn;
- de callback moet vóór de OTA-schrijf/validatiefase worden aangeroepen;
- de returnwaarde moet worden gecontroleerd;
- bij een fout moet de upload netjes worden afgebroken met een duidelijke status/logregel;
- de callback mag niet vanuit een ISR worden aangeroepen;
- de callback mag geen deadlock veroorzaken met de audio-task.

Overweeg of de callback vóór `esp_ota_begin()` moet worden aangeroepen. Dat heeft de voorkeur omdat de radio dan al stilstaat voordat flash-writes en image-validatie beginnen. Als de OTA-client pas na de 12-byte header voldoende bekend is, roep hem dan direct na header- en partitiegroottevalidatie aan en vóór `esp_ota_begin()`.

## Radio-stop implementeren

Voeg in `radio_audio` een kleine publieke API toe voor een gecontroleerde OTA-stop, bijvoorbeeld:

```c
esp_err_t radio_audio_prepare_for_ota(void);
```

De functie moet:

1. Een stopverzoek voor de actieve fetch/decode-sessie instellen.
2. De actieve sessie ongeldig maken, zodat oude taken geen data meer naar de ringbuffer of I2S schrijven.
3. Wachten totdat `s_stream_running` en `s_fetch_running` beide false zijn.
4. Een begrensde timeout gebruiken; nooit onbeperkt blokkeren.
5. De I2S-output veilig muten of uitschakelen indien dat nodig is.
6. De functie herhaalbaar maken als er geen actieve stream is.
7. Geen geheugenlekken veroorzaken bij vroegtijdig stoppen.
8. Vanuit de OTA-task aanroepbaar zijn zonder een queue-deadlock te veroorzaken.
9. Geen callbacks naar display/web GUI uitvoeren vanuit een context waar dat tot lock- of netwerkproblemen kan leiden.

Onderzoek zorgvuldig de bestaande sessie- en stoplogica. Gebruik de bestaande mechanismen waar mogelijk in plaats van een tweede concurrerende stoparchitectuur te introduceren.

Let extra op:

- `audio_task` wacht op berichten uit `s_queue`;
- `fetch_task` kan in netwerk/TLS-I/O zitten;
- `stream_task` kan in decoder- of I2S-werk zitten;
- `s_stop_requested` wordt door meerdere taken gebruikt;
- oude sessies mogen na de stop niet opnieuw `s_stream_running` of `s_fetch_running` overschrijven;
- de OTA-task draait op CPU0 en de streamtask op CPU1;
- wachtlussen moeten `vTaskDelay()` gebruiken, zodat andere taken kunnen draaien.

Als een bestaande functie semantisch beter geschikt is, mag die worden hergebruikt, maar documenteer in de code kort waarom. Voeg geen grote refactor toe.

## Callback koppelen

Koppel in `main/app_main.c` de nieuwe OTA-prepare-callback aan de nieuwe `radio_audio_prepare_for_ota()`-functie. Configureer dit vóór:

```c
ota_upload_start(&ota_cfg);
```

De callback moet alleen de OTA-start voorbereiden. De applicatie mag niet zelf automatisch flashen of uploaden; de upload blijft een handmatig door de gebruiker uitgevoerde opdracht.

## OTA-foutpaden

Controleer alle relevante foutpaden in `ota_upload.c`:

- headerfout;
- ongeldig magic-getal;
- image te groot voor de updatepartitie;
- geen geldige updatepartitie;
- callback faalt;
- `esp_ota_begin()` faalt;
- socket verbreekt tijdens ontvangst;
- `esp_ota_write()` faalt;
- `esp_ota_end()` faalt;
- `esp_ota_set_boot_partition()` faalt.

Gebruik bestaande foutstatussen waar mogelijk. Voeg alleen een nieuwe status toe als de hostclient die duidelijk kan onderscheiden. Zorg dat een mislukte voorbereiding of overdracht geen half-geactiveerde bootpartitie veroorzaakt.

## Watchdog- en schedulingcriteria

De oplossing mag niet:

- `CONFIG_ESP_TASK_WDT_EN` globaal uitschakelen;
- CPU1 idle uit de watchdog verwijderen;
- de watchdog-timeout alleen verhogen als workaround;
- de actieve audiotaken laten doorlopen tijdens `esp_ota_end()`;
- `taskYIELD()` gebruiken als vervanging voor een echte stop- of wachtstrategie;
- onbeperkt wachten zonder timeout;
- firmware uploaden, flashen of resetten vanuit de ontwikkelomgeving.

De oplossing mag wel:

- de audio-output tijdens OTA stilleggen;
- de actieve streamsessie gecontroleerd beëindigen;
- een kleine lokale timeout toevoegen;
- extra logging toevoegen met subsystem-tag `ota_upload` of `RADIO`;
- de OTA-task-prioriteit alleen wijzigen als met code-analyse blijkt dat dit nodig is.

## Tests en verificatie

Voer na de wijziging uit:

1. Controleer compileerfouten en diagnostics in alle gewijzigde bestanden.
2. Activeer ESP-IDF 6.0.2 met:

```sh
source "$HOME/.espressif/tools/activate_idf_v6.0.2.sh"
```

3. Voer uit:

```sh
idf.py reconfigure
idf.py build
```

4. Controleer dat de applicatie-image binnen `ota_0`/`ota_1` van 3 MB blijft.
5. Controleer met `git diff` dat alleen noodzakelijke bestanden zijn gewijzigd.
6. Upload of flash nooit automatisch. De gebruiker voert zelf uit:

```sh
idf.py ota --host <hostname> --timeout 120
```

7. Als runtime-validatie mogelijk is nadat de gebruiker handmatig heeft geüpload, controleer dan dat:

- de logmelding `ota_upload: OTA successful` verschijnt;
- er vóór `esp_ota_end()` een duidelijke logregel staat dat de audio is gestopt;
- `IDLE1` geen Task Watchdog-melding meer veroorzaakt;
- de nieuwe firmware na reboot vanaf de nieuwe OTA-partitie start;
- WiFi/TLS-fouten direct rond de reboot niet ten onrechte als OTA-fout worden geïnterpreteerd.

## Documentatie

Werk `flashFirmwareOTA.md` alleen bij als de nieuwe callback of het OTA-stopgedrag relevant is voor gebruikers of troubleshooting. Documenteer dat:

- de upload handmatig blijft;
- de audiostream vóór OTA-validatie wordt beëindigd;
- de firmware na succesvolle validatie automatisch reboot als de configuratie dat toestaat;
- een watchdogmelding tijdens een oude firmware-build niet als succescriterium mag worden gebruikt.

## Werkwijze

Werk incrementeel en behoud de bestaande stijl van het project. Gebruik Allman-braces en twee spaties inspringing voor C-code. Voeg geen licentieheaders toe. Voeg geen niet-bestaande API’s toe zonder ze eerst in de juiste header en implementatie te definiëren. Maak geen commit. Rapporteer aan het einde:

- welke bestanden zijn aangepast;
- hoe de audio vóór OTA wordt gestopt;
- hoe de OTA-component de callback aanroept;
- welke buildcontrole succesvol was;
- welke runtime-validatie de gebruiker nog handmatig moet uitvoeren;
- eventuele resterende risico’s.
