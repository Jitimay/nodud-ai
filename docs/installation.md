# Nodus AI — Gateway installation guide

For the person installing a gateway on site (NGO office, shop, community centre).
The gateway must already be flashed and configured (see `firmware/README.md`).

## 1. Parts list

| Part | Notes |
|---|---|
| LilyGO T-Call ESP32 SIM800L | flashed with `nodus_gateway` |
| GSM antenna | IPEX/u.FL, quad-band. **Never power the modem without it** |
| 3.7 V Li-ion battery | 18650 or LiPo, ≥ 1500 mAh, protected cell. Absorbs the 2 A transmit peaks and covers power cuts |
| 5 V / 2 A USB power supply | a real 2 A adapter, not a phone charger of unknown quality |
| Short, thick USB cable | thin long cables drop voltage |
| SIM card | 2G-capable operator, **PIN disabled**, SMS bundle/credit |
| Enclosure | ventilated plastic box (not metal: it blocks radio) |

## 2. Before you leave for the site

1. Put the SIM in a phone: disable the PIN, send and receive one SMS, check credit.
2. Note the SIM's phone number. This is the number users will text.
3. On the bench, power the gateway and send an SMS to it: you must get an AI reply.
4. Write the device id (e.g. `tcall-01`) and the SIM number on a label on the box.

## 3. Choosing the location

- **GSM signal:** check with any phone on the same operator: at least 2–3 bars. Near a window, up high,
  away from thick concrete and metal. The status command shows `CSQ`; aim for ≥ 10 (15+ is good).
- **Wi-Fi:** in solid range of the access point (`status` shows RSSI; better than -75 dBm).
- **Power:** a socket that is not switched off at night. The battery bridges short cuts only.
- **Heat:** not in direct sun, not on top of other electronics. Li-ion batteries degrade fast above 45 °C.
- Out of reach of children; cables secured so they cannot be pulled.

## 4. Assembly (power OFF)

1. Insert the SIM (check orientation against the holder's marking).
2. Screw/clip the GSM antenna on the modem's antenna connector. Place the antenna outside the box or
   against its wall, pointing up.
3. **Battery polarity:** compare the battery connector's red wire with the `+` mark on the board.
   Batteries from other vendors are often wired the opposite way, and reversing it destroys the board.
   If unsure, do not connect: ask.
4. Connect the battery, then the USB power.

## 5. Power on and check

The LED on the board shows the state:

| LED | Meaning | What to do |
|---|---|---|
| fast blink (first minute) | modem starting / registering | wait up to 2 minutes |
| fast blink (stays) | modem problem | check SIM, antenna, battery (see troubleshooting) |
| slow blink | modem OK, no backend | check Wi-Fi / internet |
| solid | working | done |

The modem's own small LED: fast blink = searching network, slow blink (~every 3 s) = registered.

**Final test:** send an SMS from your phone to the gateway's number, e.g. "Bonjour". A reply should
arrive within about 10–20 seconds.

If a laptop is available, open the Serial Monitor (115200, Carriage return) and type `status`.

## 6. Troubleshooting

| Symptom | Likely cause | Fix |
|---|---|---|
| LED fast-blinks forever | SIM missing, PIN on, no antenna, weak power | Reseat SIM, disable PIN, attach antenna, connect/charge battery |
| `status`: `modem resets` keeps increasing | power brownout during transmission | Charge/replace battery, better 5 V 2 A supply, shorter USB cable |
| `status`: CREG not 1 or 5 | no network | Move gateway, check operator still runs 2G there |
| `status`: CSQ < 10 | weak signal | Move higher / near window, reposition antenna |
| LED slow-blinks | Wi-Fi or internet down, server down | Check router; `status` shows wifi/backend; operator checks the server |
| log says `token REJECTED` | gateway token does not match the server | Operator: compare `config` token with server `GATEWAY_TOKENS` |
| SMS received, no reply | SIM out of credit, or sending blocked | Check credit; `status` shows `failed` count |
| Reply arrives twice | gateway rebooted at the wrong moment | Rare and harmless; if frequent, fix power |

## 7. Maintenance

- Monthly: check SIM credit/bundle, check the battery is not swollen (replace immediately if it is).
- The operator watches `/v1/admin/stats` (see `docs/deployment.md`): an offline gateway or falling
  `modem_supply_mv` means a site visit.
- Replace the battery every 1–2 years, sooner in hot locations.
