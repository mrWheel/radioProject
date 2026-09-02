Project: radioProject
Repository: https://github.com/mrWheel/radioProject

Superseded by projectPrompt.md. This file is kept only as a historical note; the authoritative English source of truth is now the main project prompt.

Doel

Breid het bestaande radioProject uit zodat de gebruiker kan kiezen waar de audio van het geselecteerde internetradiostation wordt afgespeeld:

1. Op het fysieke ESP32-device via de bestaande radio_audio -> decoder -> I2S -> PCM5102A keten.
2. Rechtstreeks in de browser waarin de bestaande WebGUI geopend is.

De browser moet de originele radiostream rechtstreeks van het radiostation ophalen en afspelen. Audio mag dus NIET als PCM, MP3, AAC of andere audiostream via de ESP32/WebSocket naar de browser worden doorgestuurd.

De ESP32 en browser mogen niet tegelijkertijd dezelfde stationstream afspelen.

Maak hiervoor expliciet onderscheid tussen:

AUDIO_OWNER_DEVICE
AUDIO_OWNER_BROWSER

WebGUI

Voeg in de bestaande WebGUI een duidelijke schuifschakelaar/slider toe waarmee gekozen wordt tussen:

Device  <–– slider ––>  Browser

of een vergelijkbare visueel passende implementatie.

De standaardstand is:

Device

De slider moet duidelijk aangeven waar de audio momenteel wordt afgespeeld.

De slider is NIET de bestaande volume-slider. Het is een aparte audio-output/audio-owner slider.

Wanneer de gebruiker de slider naar “Browser” schuift, geldt dit als de expliciete user interaction die nodig kan zijn om browser-audio te mogen starten.

Gebruik voor browser-audio bij voorkeur het standaard HTML5  mechanisme.

Houd rekening met Safari op iPad/iPhone en andere browsers die autoplay zonder expliciete user interaction blokkeren.

Browser neemt audio over

Wanneer de gebruiker de audio-output-slider van Device naar Browser zet:

1. Zet audioOwner op AUDIO_OWNER_BROWSER.
2. Stop de bestaande ESP32-radiostream volledig.

Gebruik hiervoor NIET alleen de bestaande pause-functionaliteit.

De HTTP-verbinding met het radiostation moet daadwerkelijk worden gesloten.

De bestaande ringbuffer hoeft niet gevuld te blijven en oude audio hoeft niet te worden bewaard.

3. Stuur via de bestaande WebSocket naar de browser alle informatie die nodig is om het momenteel geselecteerde station rechtstreeks te openen.

Gebruik hiervoor de originele stream-URL van het geselecteerde station zoals die uit stations.json / de bestaande stationstructuur beschikbaar is.

4. De browser opent deze stream rechtstreeks met het HTML5 audio-element.

Conceptueel:

Radio station -> Internet/LAN -> Browser -> browser audio output

en NIET:

Radio station -> ESP32 -> WebSocket -> Browser

5. De ESP32 produceert tijdens AUDIO_OWNER_BROWSER geen audio via I2S/PCM5102A.
6. De WebGUI blijft verder volledig functioneren. Stationkeuze, metadata, volume-GUI en andere bestaande functionaliteit mogen hierdoor niet worden geblokkeerd.

Browser geeft audio terug

Wanneer de gebruiker de slider van Browser terug naar Device schuift:

1. Stop onmiddellijk de browser-audio.
2. Maak de browser audio source vrij indien nodig.
3. Stuur via WebSocket naar de ESP32 dat de browser audio ownership opgeeft.
4. Zet:

audioOwner = AUDIO_OWNER_DEVICE

5. Start op de ESP32 het ACTUEEL geselecteerde station opnieuw via de normale radio_audio infrastructuur.

De ESP32 moet een nieuwe verbinding met het radiostation maken.

Gebruik dus geen eventueel achtergebleven oude audiobuffer.

Omdat dit live internetradio betreft, moet de ESP32 na opnieuw verbinden gewoon verdergaan bij het actuele/live punt van de uitzending.

WebSocket verbinding verbroken

De bestaande WebGUI heeft al WebSocket connect/disconnect/takeover-functionaliteit.

Gebruik de bestaande architectuur en callbacks en maak geen tweede parallel WebSocket-systeem.

Wanneer AUDIO_OWNER_BROWSER actief is en de WebSocket-verbinding met de browser wordt verbroken, moet de ESP32 automatisch audio ownership terugnemen.

Dus:

WebSocket disconnect
+
audioOwner == AUDIO_OWNER_BROWSER

-> audioOwner = AUDIO_OWNER_DEVICE
-> start huidige station opnieuw op ESP32
-> audio via I2S/PCM5102A

Gebruik hiervoor de bestaande WebSocket close/disconnect infrastructuur, waaronder waar toepasselijk web_gui_httpd_close_fn() of de actuele equivalent daarvan.

Voorkom dubbele starts wanneer meerdere disconnect-events of callbacks kort achter elkaar optreden.

Fysieke bediening heeft altijd prioriteit

Dit is een belangrijke ontwerpregel:

IEDERE fysieke bediening van het ESP32-device moet onmiddellijk audio ownership teruggeven aan het device.

Dit geldt minimaal voor:

* EC11 links/rechts draaien
* EC11 indrukken
* extra/auxiliary push button
* short/medium/long press events indien die door het huidige component worden gegenereerd
* alle andere bestaande fysieke input-events die door de normale input event queue worden afgehandeld

Gebruik hiervoor de bestaande input/event infrastructuur.

Maak geen tweede parallel input-systeem.

Wanneer bijvoorbeeld in app_main.c een fysiek input-event binnenkomt:

if audioOwner == AUDIO_OWNER_BROWSER:

1. zet audioOwner naar AUDIO_OWNER_DEVICE;
2. laat de browser via WebSocket weten dat hij audio moet stoppen;
3. start het actuele station opnieuw op de ESP32;
4. handel daarna het oorspronkelijke fysieke input-event normaal af.

De browser moet bij ontvangst van deze melding:

* audio.pause() uitvoeren;
* de browserstream stoppen/vrijgeven;
* de audio-output-slider automatisch terugzetten naar “Device”.

BELANGRIJK:

De WebGUI/WebSocket-verbinding zelf moet hierbij NIET worden verbroken.

Alleen audio ownership gaat terug naar het ESP32-device.

De gebruiker kan daarna in dezelfde WebGUI de slider opnieuw naar Browser zetten.

Audio-owner synchronisatie

De ESP32 is de authoritative source voor de audio-owner status.

Definieer een duidelijke status, bijvoorbeeld:

typedef enum
{
AUDIO_OWNER_DEVICE,
AUDIO_OWNER_BROWSER
} audio_owner_t;

Pas naamgeving eventueel aan de bestaande coding style van het project aan.

De browser mag niet uitsluitend lokaal aannemen dat hij audio owner is.

Synchroniseer de status via het bestaande WebSocket-protocol.

Voeg hiervoor een passend WebSocket message type toe, bijvoorbeeld conceptueel:

{
“type”: “audioOwner”,
“owner”: “device”
}

of:

{
“type”: “audioOwner”,
“owner”: “browser”
}

Gebruik echter de bestaande message conventions en JSON-structuur van het project als die al bestaan.

Maak geen afwijkend protocol wanneer het bestaande protocol eenvoudig uitgebreid kan worden.

Nieuwe WebGUI verbinding

Wanneer een WebGUI-browser verbinding maakt, moet de ESP32 de actuele audio-owner status meesturen als onderdeel van de normale initial state/synchronisatie.

Een nieuw geopende browser mag NOOIT automatisch audio ownership overnemen.

Dus:

WebGUI openen -> audio blijft op Device.

Alleen een expliciete beweging van de audio-output-slider naar Browser mag AUDIO_OWNER_BROWSER activeren.

Meerdere browsers / takeover

Het project ondersteunt momenteel één actieve WebSocket-client en takeover van een bestaande browser.

Behoud dit gedrag.

Wanneer browser B browser A overneemt terwijl browser A AUDIO_OWNER_BROWSER is, mag niet onbedoeld audio op beide browsers blijven spelen.

Zorg voor een deterministische overgang.

Veilige voorkeursimplementatie:

Bij WebSocket/browser takeover wordt audio ownership eerst teruggezet naar:

AUDIO_OWNER_DEVICE

De oude browser krijgt waar mogelijk opdracht zijn audio te stoppen.

De nieuwe browser start met de slider op Device.

Browser B mag pas audio overnemen nadat de gebruiker daar expliciet de slider naar Browser heeft geschoven.

Autoplay mag dus nooit ontstaan door alleen een browser takeover.

Station wijzigen vanuit WebGUI

Denk expliciet na over stationwisselingen terwijl AUDIO_OWNER_BROWSER actief is.

Als de gebruiker via de WebGUI een ander station kiest terwijl Browser de audio owner is:

1. ESP32 verwerkt en bewaart de nieuwe current station zoals nu al gebeurt.
2. ESP32 start dit station NIET via I2S.
3. Browser stopt zijn huidige stream.
4. Browser krijgt de URL van het nieuw geselecteerde station.
5. Browser start de nieuwe stream rechtstreeks.

audioOwner blijft:

AUDIO_OWNER_BROWSER

Een stationwissel vanuit de WebGUI is dus GEEN reden om ownership terug naar het device te zetten.

Een stationwissel via een FYSIEKE knop/encoder op het device is dat WEL, omdat iedere fysieke bediening device priority heeft.

Volume

Behandel volume zorgvuldig.

Wanneer AUDIO_OWNER_DEVICE actief is:

De bestaande ESP32-volume-functionaliteit blijft exact werken zoals nu.

Wanneer AUDIO_OWNER_BROWSER actief is:

De bestaande WebGUI volume-slider mag de browser audio-volume regelen indien dat logisch past binnen de huidige UI.

Voorkom dat browser-volume en ESP32-volume onbedoeld verschillende opgeslagen volume-instellingen veroorzaken.

Bekijk eerst hoe volume momenteel in het project wordt opgeslagen en gesynchroniseerd en kies daarna de minst invasieve oplossing.

Verander bestaande persistente volume-semantiek niet zonder noodzaak.

Documenteer in de code duidelijk welke volume-waarde device-volume is en welke eventueel alleen browser playback-volume is.

ICY metadata

De bestaande ESP32 verkrijgt ICY metadata zoals StreamTitle.

Probeer deze bestaande metadata-functionaliteit zoveel mogelijk te behouden.

Het is NIET noodzakelijk dat de browser zelf ICY metadata uit de browser-audiostream probeert te halen.

Als de ESP32 voor metadata momenteel afhankelijk is van dezelfde HTTP-audiostream die wordt gesloten wanneer Browser owner wordt, onderzoek dan eerst de bestaande implementatie.

Maak geen ingewikkelde tweede volledige audiostream uitsluitend om metadata te verkrijgen zonder dit expliciet te motiveren.

Prioriteit is:

1. correcte audio switching;
2. stabiele browser playback;
3. stabiele ESP32 playback;
4. daarna metadata tijdens browser playback.

Als metadata tijdens AUDIO_OWNER_BROWSER niet zonder aanzienlijke extra complexiteit beschikbaar kan blijven, documenteer dit duidelijk voordat je een grote architectuurwijziging maakt.

CORS / browser beperkingen

Houd rekening met browser security en CORS.

Een HTML5  kan veel internetradiostreams rechtstreeks afspelen, maar niet iedere radiostream/server gedraagt zich hetzelfde.

Voeg GEEN ESP32 audio proxy toe alleen om een theoretisch CORS-probleem op te lossen.

Test eerst de stations uit de bestaande stations.json.

Rapporteer welke stations rechtstreeks in Safari/Chrome via  werken en welke eventueel niet.

Als een station niet werkt, onderzoek eerst:

* HTTP versus HTTPS mixed content;
* redirects;
* Content-Type;
* browser codec support;
* CORS indien relevant;
* ICY response handling;
* Safari/iOS beperkingen.

Maak pas daarna een voorstel voor een fallback.

Startup buffering ESP32

Bij terugkeer van Browser naar Device moet radio_audio opnieuw verbinden.

De huidige audio-implementatie gebruikt een relatief grote PSRAM/ringbuffer en startup prefill.

Onderzoek of de huidige startup prefill een hinderlijk lange stilte veroorzaakt bij audio handover.

Verander dit niet blind.

Meet/log eerst:

* tijd vanaf ownership -> DEVICE;
* HTTP connected;
* eerste ontvangen audio;
* decoder start;
* eerste I2S audio.

Als de vertraging duidelijk door de startup prefill wordt veroorzaakt, maak deze dan eventueel configureerbaar of gebruik een kleinere veilige startup threshold.

Stabiliteit tegen buffer underruns blijft belangrijker dan een extreem snelle handover.

State machine / race conditions

Voorkom race conditions tussen:

* WebSocket disconnect;
* WebSocket takeover;
* fysieke input;
* stationwissel;
* slider naar Browser;
* slider naar Device;
* HTTP audio task;
* decoder task;
* I2S task.

Er mag nooit een situatie ontstaan waarin zowel Browser als Device denken dat zij audio owner zijn.

Audio-owner wijzigingen moeten idempotent zijn.

Bijvoorbeeld:

setAudioOwner(AUDIO_OWNER_DEVICE)

terwijl DEVICE al owner is, mag niet opnieuw onnodig radio_audio_start() uitvoeren.

Maak bij voorkeur één centrale functie/API voor ownership transitions in plaats van verspreide directe wijzigingen van de state.

Logging

Voeg duidelijke ESP_LOGI logging toe voor ownership transitions.

Bijvoorbeeld conceptueel:

Audio owner: DEVICE -> BROWSER (WebGUI slider)
Audio owner: BROWSER -> DEVICE (physical EC11)
Audio owner: BROWSER -> DEVICE (WebSocket disconnect)
Audio owner: BROWSER -> DEVICE (browser slider)
Audio owner: BROWSER -> DEVICE (WebSocket takeover)

Log ook browser playback requests en het station waarop de handover betrekking heeft.

Log geen continue spam tijdens normale playback.

Architectuurregels

BELANGRIJK:

Lees vóór wijzigingen eerst de volledige actuele projectstructuur en relevante componenten.

Gebruik bestaande componenten, APIs, callbacks, event queues, WebSocket infrastructuur en coding conventions.

Maak geen parallelle infrastructuur als bestaande infrastructuur uitgebreid kan worden.

Browser audio wordt rechtstreeks vanaf de station-server afgespeeld.

Stuur GEEN PCM audio over WebSocket.

Stuur GEEN MP3/AAC audiostream via WebSocket.

Maak van de ESP32 GEEN audio proxy tenzij later expliciet wordt vastgesteld dat dit noodzakelijk is.

Fysieke bediening heeft altijd prioriteit.

Een fysieke bediening verbreekt NIET de WebGUI.

Een nieuwe browserverbinding neemt NOOIT automatisch audio over.

Na reboot is de audio owner altijd DEVICE.

Behoud bestaande radiofunctionaliteit wanneer de nieuwe browser-audiofunctionaliteit niet wordt gebruikt.

Implementatievolgorde

Werk in kleine, controleerbare stappen.

1. Analyseer eerst de huidige radio_audio, WebGUI, WebSocket, input/event, station en volume architectuur.
2. Rapporteer kort welke bestaande bestanden/componenten aangepast moeten worden en waarom.
3. Implementeer centrale audio-owner state en transitions.
4. Implementeer WebSocket messages voor audio ownership.
5. Implementeer de Device/Browser slider in de WebGUI.
6. Implementeer browser  playback.
7. Implementeer Browser -> Device handover.
8. Implementeer automatische handover bij WebSocket disconnect.
9. Implementeer fysieke-input-priority.
10. Implementeer station switching tijdens browser playback.
11. Controleer WebSocket takeover.
12. Controleer volume.
13. Test de bestaande stations uit stations.json.
14. Build het volledige ESP-IDF project en los compile-errors op.
15. Geef na implementatie een korte technische samenvatting van:
    * gewijzigde bestanden;
    * nieuwe state/messages;
    * browser audio flow;
    * device audio flow;
    * handover gedrag;
    * eventuele browser/station beperkingen.

Acceptance criteria

De implementatie is pas gereed wanneer minimaal het volgende werkt:

* Boot -> audio speelt op ESP32.
* WebGUI openen -> audio blijft op ESP32.
* Slider Device -> Browser -> ESP32 stopt en browser speelt hetzelfde station.
* Slider Browser -> Device -> browser stopt en ESP32 speelt het actuele station.
* Browser sluiten tijdens browser playback -> ESP32 neemt automatisch over.
* WebSocket verliezen tijdens browser playback -> ESP32 neemt automatisch over.
* EC11 draaien tijdens browser playback -> browser stopt, slider gaat naar Device en ESP32 neemt over.
* EC11 indrukken tijdens browser playback -> idem.
* Auxiliary button tijdens browser playback -> idem.
* Station wijzigen via WebGUI tijdens browser playback -> browser blijft owner en speelt nieuwe station.
* Station wijzigen via fysieke bediening -> Device wordt owner.
* Nieuwe WebGUI openen -> neemt audio niet automatisch over.
* Browser takeover -> geen dubbele browser/device playback.
* Reboot -> DEVICE is altijd owner.
* Normaal gebruik zonder browser -> bestaande radiofunctionaliteit blijft ongewijzigd.
* Er wordt geen audio via WebSocket geproxied.
* Er is nooit bewust gelijktijdige playback op ESP32 en browser.

Voer geen grote refactoring uit die niet noodzakelijk is voor deze functionaliteit.

Behoud de bestaande projectarchitectuur zoveel mogelijk en implementeer deze uitbreiding als een gecontroleerde toevoeging aan het huidige radioProject.