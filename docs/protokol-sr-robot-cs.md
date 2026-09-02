# Protokol SR Robot

Verze: 1
Stav: návrh pro implementaci

Tento dokument popisuje binární protokol přenášený v `SimpleRadio.sendBlob()`.
Protokol přenáší stav ovladače na robot a telemetrii robota zpět na ovladač.
Je určen pro malé, časté zprávy: řízení kol, potenciometry, tlačítka, stav
motorů a diagnostiku linky.

## Zásady návrhu

- Každá zpráva obsahuje úplný aktuální stav vstupů. Ztráta jedné zprávy proto
  nezpůsobí chybějící událost; další stav ji nahradí.
- Oba směry používají stejnou obálku. Směr určuje typ zprávy a identifikátory
  zdroje a cíle, ne jiný formát.
- Nové hodnoty se přidávají jako TLV položky. Starší firmware neznámou položku
  bezpečně přeskočí.
- Každá zpráva je ověřená pomocí předem sdíleného klíče (PSK). PSK se do
  paketu nikdy nevkládá.
- Robot rozhoduje o bezpečnosti lokálně: při výpadku platného řízení zastaví
  motory, i kdyby ovladač nadále zobrazoval starý stav.

SimpleRadio již odděluje vlastní blob od ostatních typů zpráv a kontroluje
integritu ESP-NOW rámce. Protokol proto nepřidává další kontrolní součet.
HMAC ovšem chrání původ a obsah zprávy; nešifruje ji ani nebrání rušení rádia.

## Číselné konvence

Vícebytová čísla jsou little-endian. Neoznačená čísla jsou `u8`, `u16` a
`u32`; podepsaná čísla jsou v doplňkovém kódu (`i8`, `i32`). Bity jsou
číslovány od nejnižšího bitu, tedy bit 0 je maska `0x01`. Nonce není číslo,
ale přesně osm náhodných bajtů.

## Obálka paketu

Všechny pakety mají následující hlavičku o 28 bajtech, následovanou TLV
užitečným obsahem a 16bajtovým ověřovacím tagem.

| Bajty | Pole | Popis |
|---:|---|---|
| 0-1 | magic | ASCII `SR` (`0x53`, `0x52`) |
| 2 | version | Verze protokolu, ve verzi 1 hodnota `0x01` |
| 3 | packet_type | Typ paketu |
| 4 | packet_flags | Obecné příznaky paketu |
| 5 | source_id | ID zařízení, které paket odeslalo |
| 6 | destination_id | ID cílového zařízení; `0` znamená broadcast |
| 7 | key_id | Identifikátor aktivního PSK, zpočátku `0` |
| 8-15 | sender_nonce | Aktuální náhodný nonce odesílatele (`byte[8]`) |
| 16-23 | recipient_nonce | Aktuální nonce příjemce (`byte[8]`) |
| 24-27 | sequence | Pořadové číslo v dané relaci (`u32`) |
| 28...n-17 | payload | Posloupnost TLV položek |
| n-16...n-1 | auth_tag | Prvních 16 bajtů HMAC-SHA-256 |

`auth_tag` se počítá jako `HMAC-SHA-256(PSK[key_id], packet[0..n-17])[:16]`.
Příjemce tag porovná v konstantním čase. Minimální velikost paketu je tedy 44
bajtů (hlavička a tag bez payloadu).

### Typ paketu (`packet_type`)

| Hodnota | Název | Použití |
|---:|---|---|
| `0x01` | `HELLO` | Oznámení nové relace a nonce |
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

## Relace, nonce a ochrana proti replay

Každé zařízení při startu a po obnovení relace vytvoří kryptograficky náhodný
`sender_nonce` pomocí hardwarového generátoru náhodných čísel ESP32. Nonce je
osm neprůhledných bajtů a porovnává se po bajtech, ne jako little-endian číslo.
Zařízení
pravidelně (např. jednou za sekundu) vysílá autentizovaný `HELLO`. V paketu
`HELLO` je `recipient_nonce` nula; `sender_nonce` je nová hodnota relace.

Příjemce si ověřený nonce protějšku uloží, ale `HELLO` nesmí nikdy přímo
spustit pohyb motorů. Běžný paket musí splnit všechny podmínky:

1. `destination_id` odpovídá místnímu ID nebo je broadcast.
2. `key_id` označuje nakonfigurovaný klíč a HMAC je platný.
3. `recipient_nonce` odpovídá aktuálnímu místnímu nonce.
4. `sender_nonce` odpovídá naposledy objevené relaci zdroje.
5. `sequence` je novější než poslední přijaté číslo pro daný zdroj a nonce.

Po změně `sender_nonce` se pořadové číslo začíná znovu od nuly. Porovnání
pořadových čísel se provede přes modulární rozdíl `int32_t(new - last) > 0`.
Nonce příjemce znemožní přehrát paket z minulé relace po restartu robota;
pořadové číslo znemožní opakování paketu v aktivní relaci.

Zachycený `HELLO` může nanejvýš dočasně zhoršit dostupnost spojení. Nemůže
autorizovat pohyb ani obejít HMAC; pravidelný aktuální `HELLO` relaci obnoví.
Rušení rádia a zahlcení přehrávanými platnými rámci nelze PSK odstranit, proto
je nutný lokální timeout motorů.

## Formát TLV

Payload je posloupnost položek:

| Bajt v položce | Pole |
|---:|---|
| 0 | `type` |
| 1 | `length` |
| 2 až `length + 1` | `value` |

Parser musí před čtením vždy ověřit, že celé `value` leží v payloadu. Neznámé
typy přeskočí přesně podle `length`; známý typ se špatnou délkou způsobí
odmítnutí celého paketu. Typy `0x01-0x3f` jsou základní, `0x40-0x7f` jsou
volitelné a `0x80-0xff` jsou lokální rozšíření.

## Řídicí payload (`CONTROL`)

### `0x01` — potenciometry

Hodnota začíná `present_mask:u8` a následuje jedno `u16` pro každý nastavený
bit, ve vzestupném pořadí kanálů. Hodnota je normalizovaná do rozsahu
`0..65535`; robot si ji převádí na plyn, řízení nebo jinou funkci podle svého
profilu. Pro čtyři potenciometry je délka 9 bajtů.

| Bit | Maska | Kanál |
|---:|---:|---|
| 0 | `0x01` | Potenciometr 1 |
| 1 | `0x02` | Potenciometr 2 |
| 2 | `0x04` | Potenciometr 3 |
| 3 | `0x08` | Potenciometr 4 |
| 4-7 | `0xf0` | Rezervováno |

### `0x02` — spínače

Hodnota obsahuje `present_mask:u16`, potom `pressed_mask:u16`. Bit v
`pressed_mask` znamená stisknuto; bit nenastavený znamená uvolněno. Stav
spínačů je úroveň, nikoli hrana, proto ztracený paket neztratí stisk.

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

Robot smí pohánět kola jen při platném čerstvém `CONTROL` paketu a nastaveném
deadman bitu. Nouzové zastavení se v robotu západkuje; uvolnění musí vyžadovat
bezpečný lokální postup nebo samostatný, explicitně navržený autentizovaný
příkaz.

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

## Doporučené časování a bezpečnost

- Ovladač vysílá plný `CONTROL` stav přibližně 50× za sekundu.
- Robot vysílá `TELEMETRY` přibližně 10× za sekundu; při změně poruchy může
  odeslat okamžitou zprávu navíc.
- Robot zastaví motory po 150 ms bez nového platného `CONTROL` paketu.
- Telemetrie je informativní. Ovladač nesmí předpokládat, že pohyb pokračuje,
  dokud neobdrží potvrzení v `0x23` a stav motorů.
- Pro každý robot se doporučuje jiný náhodný 32bajtový PSK. Klíče se neukládají
  do repozitáře ani do logů; ve výrobním zařízení patří do chráněného úložiště.
- `SimpleRadio` group není bezpečnostní hranice. Správný `destination_id`,
  HMAC, nonce a timeout jsou povinné i při použití skupin.

## Příklad velikosti

Řídicí paket se čtyřmi potenciometry, devíti tlačítky a bezpečnostním stavem
obsahuje 28 bajtů hlavičky, 11 bajtů TLV potenciometrů (typ, délka a 9bajtová
hodnota), 6 bajtů TLV spínačů, 3 bajty TLV bezpečnosti a 16 bajtů tagu: celkem
64 bajtů. To je výrazně méně
než limit blob payloadu SimpleRadio (1490 bajtů).

## Kompatibilita s TypeScriptem

Protokol nepoužívá `u64`, protože JavaScript `number` přesně reprezentuje jen
celá čísla do `2^53 - 1`. Implementace používá `Uint8Array` pro celý paket a
`DataView` s parametrem `true` pro `getUint16`, `getUint32`, `setUint16` a
`setUint32`. Nonce je `Uint8Array` o délce 8; nevytváří se z něj `number` ani
`bigint`. HMAC přijímá a vrací bajty, takže Web Crypto i Node `crypto` mohou
pracovat přímo s obálkou paketu. `u32` pořadové číslo je v bezpečném rozsahu
TypeScript `number`; pro modulární porovnání se použije
`((next - last) | 0) > 0`.
