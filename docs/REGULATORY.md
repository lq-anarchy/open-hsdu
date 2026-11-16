# Radio rules

**This is compliance research, not legal advice.** Radio rules differ by country and change. The numbers here are for
the United States (47 CFR). Read the rules for where you are, and if in doubt ask the regulator or a communications
lawyer.

## The situation

The link runs near 418 MHz. In the US that is **not a licence-free data band**: 410–420 MHz is a federal government
band, and 413–419 MHz is also allocated to medical micropower networks on a secondary basis. No licensed service
fits a private venue. The one original unit examined carries no FCC ID on its label, and a search of the FCC equipment
database by the maker's name found no grant; that does not prove none exists, but it means there is no earlier
authorization to build on, and an old grant would not cover a new design anyway.

## Which rule fits a base that polls once or twice a second

| Rule | What it allows at 418 MHz | Fits? |
|---|---|---|
| 15.209, general limit | 200 µV/m at 3 m, any use; about 12 nW = **−49.2 dBm EIRP** | **the only one that clearly does** |
| 15.231(a)/(b) | 10,333 µV/m (about −14.9 dBm) for *control signals only*; bars periodic transmissions at regular intervals | no: polling is periodic |
| 15.231(e) | 4,133 µV/m (about −22.9 dBm), any content, but each transmission ≤ 1 s then silence of at least 30× its length and 10 s | no, unless silent for 10 s between transmissions |
| 15.205 restricted bands | 418 MHz is not restricted; harmonics fall in restricted bands (for example 1,672, 3,762 and 4,180 MHz) and must meet 15.209 there | not a barrier at the carrier |
| 15.23 home-built | no equipment authorization for a device that is not marketed, not built from a kit, and built in quantities of five or fewer for personal use. The limits above and the rule against interference still apply | covers a few copies, **not the power** |
| 15.203 antenna | prohibits a standard antenna jack: the antenna must be permanently attached or use a unique coupling (for example a reverse-polarity connector); no home-built exception was found | **an ordinary SMA jack does not comply as such** |

Being at or under the 15.209 field-strength limit is **necessary, not sufficient**: it does not settle 15.203 (the
connector), 15.23 (whether this is "personal use"), or the interference rules.

## What the firmware does about it

- **Default power is the lowest step in this project's table**, `PATABLE = 0x01` (about −36.5 dBm at the chip, nominal: TI's typical
  figure, not measured). With an inline 20 dB attenuator and a 2 dBi antenna that is about −54.5 dBm EIRP: 5.3 dB under
  the 15.209 limit. The loudest step the default build allows (`0x11`, −33.8 dBm) leaves 2.6 dB.
- **The firmware refuses any step above −33 dBm** at boot and from the console unless built with
  `esp32s3zero_highpower`, and `Device::begin` will not start at a step above the cap. `PATABLE = 0x00` is excluded
  (its harmonics are stronger than its carrier).
- **A carrier and a run of `0xAA` are the only things the console can transmit**, each time-limited.
- **A watchdog** ends any carrier after 2 s, a stuck chip is found within about 20 ms, and the firmware returns to
  receive on any fault. If the ESP32 itself resets, the chip keeps transmitting until the next boot (about half a
  second).
- **These are the chip's nominal output figures.** Radiation from the unshielded module and wiring is not counted and
  can exceed what the attenuated antenna carries. Measure the real level once (BRINGUP step 2, item 5). Until you have, treat
  the margins as estimates.

The arithmetic is in `core/include/openhsdu/power.h` and is tested in `test/test_power.cpp`.

## Range at that level

Computed free-space range, 0 dBi pack antenna, 10 dB margin, with two stand-in pack sensitivities (the pack's own is
unknown):

| Base EIRP | −100 dBm | −107 dBm |
|---|---|---|
| −49.2 dBm (the 15.209 limit) | 6.3 m | 14 m |
| −51.8 dBm (loudest allowed step) | 4.6 m | 10 m |
| −54.5 dBm (**default**) | **3.4 m** | **7.6 m** |

A vest antenna against a body can cost 5 to 15 dB more, which divides these by 1.8 to 5.6. The attenuator also costs
20 dB on receive (THEORY section 7). These are estimates; the range walk in BRINGUP is the test. If one base does not
cover an arena, the answer is several low-power bases, not one high-power one.

## Making copies, and sharing

- Up to five home-built units for personal use need no authorization under 15.23. Whether running a business venue
  is "personal use", and whether a unit assembled from a bought module is "home-built", are questions for the FCC or
  a lawyer.
- Selling units needs certification. Handing someone parts with instructions can make it a kit (15.3(p)), which the
  home-built rule excludes. Publishing a design is generally understood not to be marketing a device, but this
  repository does publish a parts list and wiring, so ask the FCC if it matters to you.
- Canada: the same numbers appear in RSS-210 and RSS-Gen, but general-limit transmitters must be certified, so there is
  no home-built route. Other countries differ again.

## Interference

Part 15 devices must accept interference and must stop if they cause it. If anything on the band (government,
medical) is affected, stop transmitting. Do not run this near other 410–420 MHz systems.

## Independence

This project is independent. It is not made by, endorsed by, or affiliated with the makers of the original system.
It contains no vendor software or documents and no game protocol.
