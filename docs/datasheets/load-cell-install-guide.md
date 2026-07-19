# Load Cell — Installation & Wiring Guide (translated)

English translation of the Chinese installation sheet that shipped with
the strain-gauge weight sensor (load cell). This is the sensor that feeds
the NAU7802 ADC in the weigh station.

**Source:** [`load-cell-install-guide.jpeg`](./load-cell-install-guide.jpeg)
(original Chinese single-sheet insert, title **安装必看** — "Read Before
Installing").

**See also:** [`hardware/netlist.md`](../../hardware/netlist.md) — the
load-cell section carries the condensed wire-color-to-terminal table.

> Translation notes: the original is a general-purpose insert covering
> both half-bridge and full-bridge cells and several mounting layouts.
> Only the full-bridge wiring and single-beam mounting apply to this
> project; the rest is included for completeness.

---

## Installation (安装操作)

- Mount the sensor exactly per the orientation diagram. The **force
  direction must be perpendicular to the horizontal plane** — load must
  push straight down onto the cell.
- Wire strictly according to the wiring diagram. **Solder every
  connection point with a soldering iron.**
- ⚠️ *Do not join the sensor leads by twisting or knotting them — this
  makes the sensor signal unstable.*

## Avoid stress damage (避免应力损伤)

- Handle small/low-capacity sensors gently; avoid excessive force or
  drops/impacts. Protect the cell from overload during handling and use.

## Calibration & setup (校准与调试)

- After **installing or replacing** a sensor, recalibrate.
- Follow your readout meter's calibration procedure.
- Calibrate with a known weight in the **50–80 % of rated range** band
  (large cells: not less than ⅓ of rated range).

> In this project that maps directly to the serial workflow: run `ZERO`
> with nothing on the scale, then `CAL <grams>` with a reference weight
> in the 50–80 % band. See the "Scale calibration" section of
> `CLAUDE.md`.

---

## Wiring — full-bridge load cell (全桥称重传感器接线方法)

4 signal wires + optional shield. This is the configuration used in this
project (single full-bridge cell → NAU7802).

| Wire color   | Function       | Cell terminal | → NAU7802        |
|--------------|----------------|---------------|------------------|
| Red (红)     | Excitation +   | **E+**        | EX+ / AVDD side  |
| Black (黑)   | Excitation −   | **E−**        | EX− / AGND side  |
| Green (绿)   | Signal +       | **A+ / S+**   | VIN+             |
| White (白)   | Signal −       | **A− / S−**   | VIN−             |
| Yellow (黄)  | Shield / drain | **GND**       | ground           |

> Note from the sheet: *if the full-bridge 4-wire cell has no yellow
> wire, no separate ground connection is needed.*

## Wiring — half-bridge load cell (半桥称重传感器接线方法)

Shown on the sheet for reference (3-wire, multiple cells A/B/C combined
into a bridge). **Not used in this project** — included only to document
the original insert.

Terminals: `S+`, `E+`, `E−`, `S−` with red / black / white leads.

---

## Mounting (安装结构示意图)

### Single parallel-beam cell (平行梁传感器)

- One end is the **fixed end** (bolted to the base plate), the other is
  the **load / force end** (carries the platform).
- Use a **spacer block** so the beam can deflect freely.
- Hardware per the diagram: top plate ×1, bottom plate ×1, spacer ×1,
  screws ×4.
- **Wire-exit end** faces away from the load.

### Four-cell platform (四个传感器)

- Keep **all four corners level**; the four sensor support points define
  the weighing-pan center.
- ⚠️ *The sensor's sealed / glued (potted) face points **down**; this
  gives a **positive** output.*

---

## Manufacturer contact

Printed on the sheet: **Manager Li (李经理), +86 153 9970 8396** (same
number on WeChat). For questions about using the sensor.
