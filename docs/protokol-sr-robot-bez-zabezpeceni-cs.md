# Protokol SR Robot bez zabezpečení

Verze: 1
Stav: návrh pro implementaci

Tento dokument popisuje jednoduchý obousměrný binární protokol pro
`SimpleRadio.sendBlob()`. Ovladač posílá stav vstupů robotu a robot vrací
telemetrii. Tato varianta záměrně neobsahuje PSK, HMAC, šifrování ani nonce.
Je vhodná pouze pro vývoj, výuku nebo uzavřený rádiový prostor.

> Každý účastník stejné SimpleRadio skupiny může vytvořit paket, který robot
> přijme jako řízení. `source_id` a `destination_id` jsou pouze adresování,
> nikoli ochrana. Robot musí mít vždy lokální timeout a nouzové zastavení.

## Zásady

- Každý `CONTROL` paket nese úplný aktuální stav. Ztracený paket tedy nahradí
  následující paket.
- Stejná obálka slouží pro ovladač i robot; směr určuje typ paketu.
- Rozšíření se přidávají jako TLV položky. Starší firmware přeskočí neznámý
  typ podle jeho délky.
- SimpleRadio/ESP-NOW již kontroluje integritu přenášeného rámce, proto tato
  varianta nepřidává kontrolní součet.
- Robot zastaví motory sám při výpadku nového platného řízení.

Vícebytová čísla jsou little-endian. Bity jsou číslovány od nejnižšího bitu;
bit 0 má masku `0x01`.

## Obálka paketu

Hlavička má 11 bajtů. Za ní následuje TLV payload až do konce blobu.

| Bajty | Pole | Popis |
|---:|---|---|
| 0-1 | magic | ASCII `SR` (`0x53`, `0x52`) |
| 2 | version | Verze protokolu, ve v1 `0x01` |
| 3 | packet_type | Typ paketu |
| 4 | packet_flags | Obecné příznaky |
| 5 | source_id | ID odesílajícího zařízení |
| 6 | destination_id | ID cílového zařízení; `0` znamená broadcast |
| 7-10 | sequence | Pořadové číslo (`u32`) |
| 11...n-1 | payload | Posloupnost TLV položek |

Minimální velikost paketu je 11 bajtů. Neznámá hlavní verze, špatná magic
hodnota nebo paket kratší než hlavička se odmítne.

### Typ paketu (`packet_type`)

| Hodnota | Název | Použití |
|---:|---|---|
| `0x01` | `HELLO` | Volitelné oznámení přítomnosti zařízení |
| `0x02` | `CONTROL` | Ovladač -> robot, stav řízení |
| `0x03` | `TELEMETRY` | Robot -> ovladač, stav a měření |
| `0x04` | `RESPONSE` | Volitelná odpověď na diagnostický požadavek |
| `0x05-0x7f` | rezervováno | Budoucí základní typy |
| `0x80-0xff` | rozšíření | Aplikační/výrobcem definované typy |

### Příznaky paketu (`packet_flags`, bajt 4)

| Bit | Maska | Význam |
|---:|---:|---|
| 0 | `0x01` | Odesílatel žádá odpověď |
| 1 | `0x02` | Paket je odpovědí |
| 2 | `0x04` | Odesílatel oznamuje chybu nebo poruchu |
| 3-7 | `0xf8` | Rezervováno; ve v1 musí být nula |

## Pořadí paketů

Každý odesílatel zvyšuje `sequence` pro každý paket. Příjemce sleduje poslední
číslo pro `source_id` a přijme jen novější hodnotu: `int32_t(new - last) > 0`.
Tím zahodí zpožděné duplikáty v běžícím spojení.

Po 150 ms bez nového platného `CONTROL` paketu robot zastaví motory a zruší
uložené pořadové číslo ovladače. Další platný `CONTROL` tedy může relaci znovu
zahájit i po restartu ovladače, jehož čítač začal od nuly. Toto pravidlo není
ochrana proti replay: útočník může po timeoutu přehrát starý paket. Proto se
tato varianta nesmí používat tam, kde je rádio dostupné nedůvěryhodným osobám.

`HELLO` je pro zobrazení přítomnosti a diagnostiku; nesmí ovlivňovat motory.

## Formát TLV

Payload je posloupnost položek:

| Bajt v položce | Pole |
|---:|---|
| 0 | `type` |
| 1 | `length` |
| 2 až `length + 1` | `value` |

Parser před čtením ověří, že celé `value` leží v payloadu. Neznámý typ přeskočí
přesně o `length` bajtů; známý typ se špatnou délkou odmítne celý paket. Typy
`0x01-0x3f` jsou základní, `0x40-0x7f` volitelné a `0x80-0xff` lokální
rozšíření.

## Řídicí payload (`CONTROL`)

### `0x01` — potenciometry

Hodnota je `present_mask:u8` následovaná jedním `u16` pro každý nastavený bit,
ve vzestupném pořadí kanálů. Hodnoty jsou normalizované do rozsahu `0..65535`.
Robot je mapuje na plyn, řízení nebo jinou funkci podle svého profilu.

| Bit | Maska | Kanál |
|---:|---:|---|
| 0 | `0x01` | Potenciometr 1 |
| 1 | `0x02` | Potenciometr 2 |
| 2 | `0x04` | Potenciometr 3 |
| 3 | `0x08` | Potenciometr 4 |
| 4-7 | `0xf0` | Rezervováno |

Při všech čtyřech potenciometrech má value délku 9 bajtů: maska a čtyři
hodnoty `u16`.

### `0x02` — spínače

Hodnota obsahuje `present_mask:u16`, potom `pressed_mask:u16`. Nastavený bit
v `pressed_mask` znamená stisknuto. Spínače jsou úrovňový stav, ne událost.

| Bit | Maska | Vstup |
|---:|---:|---|
| 0-8 | `0x0001` až `0x0100` | Tlačítko 1 až 9 |
| 9-15 | `0xfe00` | Rezervováno |

### `0x03` — bezpečnostní stav

Hodnota má délku 1 bajt.

| Bit | Maska | Význam |
|---:|---:|---|
| 0 | `0x01` | Deadman je držen; pohyb je povolen |
| 1 | `0x02` | Požadavek nouzového zastavení |
| 2-7 | `0xfc` | Rezervováno |

Robot smí pohánět kola jen při čerstvém platném `CONTROL` paketu a nastaveném
deadman bitu. Nouzové zastavení robot západkuje; jeho zrušení má vyžadovat
bezpečný lokální postup nebo samostatný explicitní příkaz.

## Telemetrický payload (`TELEMETRY`)

| TLV | Délka | Hodnota |
|---:|---:|---|
| `0x20` | 2 | Napětí baterie v mV (`u16`) |
| `0x21` | 8 | Polohy enkodérů: levé `i32`, pravé `i32` |
| `0x22` | 2 | Stav motorů (`u16`) |
| `0x23` | 4 | Poslední aplikované `CONTROL.sequence` (`u32`) |
| `0x24` | 1 | RSSI posledního řídicího paketu v dBm (`i8`) |

### Stav motorů (`0x22`)

| Bit | Maska | Význam |
|---:|---:|---|
| 0 | `0x0001` | Motory jsou ozbrojené |
| 1 | `0x0002` | Aktivní timeout řízení / failsafe |
| 2 | `0x0004` | Nouzové zastavení je západkované |
| 3 | `0x0008` | Porucha budiče motoru |
| 4 | `0x0010` | Nadproud |
| 5 | `0x0020` | Podpětí |
| 6-15 | `0xffc0` | Rezervováno |

## Doporučené časování

- Ovladač odesílá úplný `CONTROL` stav přibližně 50× za sekundu.
- Robot odesílá `TELEMETRY` přibližně 10× za sekundu a ihned při změně poruchy.
- Robot zastaví kola po 150 ms bez nového platného `CONTROL` paketu.
- Ovladač má použít TLV `0x23` a stav motorů, aby zobrazil, který příkaz robot
  skutečně aplikoval.

## Příklad velikosti

Řídicí paket se čtyřmi potenciometry, devíti tlačítky a bezpečnostním stavem
má 11 bajtů hlavičky, 11 bajtů TLV potenciometrů (typ, délka a 9bajtová
hodnota), 6 bajtů TLV spínačů a 3 bajty TLV bezpečnosti: celkem 31 bajtů. Je
tedy výrazně menší než limit blob payloadu
SimpleRadio 1490 bajtů.

## Kompatibilita s TypeScriptem

Všechny číselné hodnoty jsou nejvýše `u32`, a proto se přesně vejdou do
TypeScript `number`. Implementace používá `Uint8Array` pro paket a `DataView`
s parametrem little-endian `true` pro `getUint16`, `getUint32`, `setUint16` a
`setUint32`. Modulární porovnání pořadových čísel je
`((next - last) | 0) > 0`.
