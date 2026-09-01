# ESP32-S3 Internetradio

ESP32-S3 Internetradio is firmware voor het TFT-LCD-Display-EC11 piggyback-bord.
Het project gebruikt native ESP-IDF en werkt rechtstreeks vanuit VS Code.
De radio verbindt met Wi-Fi via eerder opgeslagen netwerkgegevens.
Wanneer die gegevens ontbreken, helpt een captive portal met de configuratie.

De EC11-draaiknop verzorgt de dagelijkse bediening.
In de standaardmodus Volume verandert draaien het geluidsniveau.
Druk op de knop om door de ingestelde internetzenders te bladeren.
Draai om een zender te kiezen en druk nogmaals om deze af te spelen.
Na twintig seconden zonder activiteit keert het scherm terug naar Volume.

De zenders staan in een LittleFS JSON-bestand en zijn via de webinterface te beheren.
De browserinterface toont zenders, afspeelstatus en volumebediening.
Wijzigingen via browser en fysieke bediening blijven met elkaar gesynchroniseerd.
De audiopijplijn ondersteunt directe HTTP- en HTTPS-streams in MP3 en AAC.
ICY-titels verschijnen zowel op het TFT-scherm als in de webinterface.

Tussen netwerk en decoder staat een buffer voor gecomprimeerde audio.
Zo verstoren tijdelijke netwerkvertragingen de I2S-audio-uitvoer niet.
De PCM5102A DAC ontvangt gedecodeerde 16-bits PCM via I2S.
De normale build maakt een LittleFS-image met de webinterface en zenderlijst.
Het project is ingesteld voor een ESP32-S3 met de meegeleverde 4 MB-partitietabel.