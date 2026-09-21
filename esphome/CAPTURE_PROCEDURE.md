# RF capture procedure

Goal: map every button on the original remote to its 9-bit command, learn the
20-bit address of each of the three fans, and settle the parity scheme and the
OOK timings from measured frames rather than from assumption.

Flash `esphome_rf_capture.yaml`. It only receives - it never transmits - so it
cannot disturb the fans while you work.

```bash
cd esphome
esphome run esphome_rf_capture.yaml
esphome logs esphome_rf_capture.yaml | tee capture.log
```

Keep the device's web UI open (`http://rf-capture.local/`) next to you; that is
where the **Next Button** control lives.

## What a captured press looks like

```
-------- BURST #007   step 05: SPEED 3 --------
  bits=30  reps=18  rssi=-41dBm
  raw   = 10101010110010101100|110101001|1
  hex   = 0x0000000055651A93
  addr  = 0xAACAC      cmd = 0x1A9
  parity: ones(29)=13 p=1 total=14 -> EVEN   [odd:FAIL even:OK]
  short : n=340 avg=302 min=284 max=322
  long  : n=200 avg=1148 min=1126 max=1174
  gap   : n=18  avg=6021 min=5980 max=6070
  marks : 300:340 1100:200
```

`reps` is how many times the remote repeated the frame - that is what
`OOK_TX_REPS` should be. The `short` / `long` / `gap` averages are what
`OOK_SHORT_US` / `OOK_LONG_US` / `OOK_GAP_US` should be. The `parity` line,
collected across many different commands, is what settles the parity rule.

## Part A - full button sweep, one remote

Pick **one** remote to start with (note which room it belongs to) and work
through all fifteen buttons in this order. The firmware knows this order and
labels each burst with it, so the log stays aligned with what you pressed.

For each step:

1. Press **Next Button** in the web UI. The log prints `===== STEP nn/15 ... =====`.
2. Press that button on the remote.
3. Wait about two seconds.
4. Press the same button on the remote a second time.
5. Wait about two seconds, then go to the next step.

| Step | Button | Why two presses matter |
|------|--------|------------------------|
| 01 | Power | |
| 02 | Breeze (wave symbol) | |
| 03 | Speed 1 | |
| 04 | Speed 2 | |
| 05 | Speed 3 | |
| 06 | Speed 4 | |
| 07 | Speed 5 | |
| 08 | Speed 6 | |
| 09 | F/R (direction) | may cycle - two presses show whether the code alternates |
| 10 | Timer 1H | |
| 11 | Timer 4H | |
| 12 | Light (bulb) | |
| 13 | Light colour (three dots) | see below; press this one **four** times |
| 14 | LED- | tap briefly; holding it may repeat |
| 15 | LED+ | tap briefly; holding it may repeat |

The two presses per button are the point of the exercise: if both bursts carry
the same 9-bit command, the button is stateless and safe to expose in Home
Assistant. If the code alternates, the button cycles through states and needs
different handling.

### Step 13 in particular

On these fans the colour temperature changes when the light is switched off and
straight back on again. So the colour button may well have no command of its
own - it may simply key the ordinary light command twice in quick succession,
and let the receiver interpret the fast off/on.

The capture reports this directly. Repeats within one transmission arrive about
50ms apart; anything slower than 150ms is a separate keying, and the log says
so:

```
  ** this single press keyed the SAME code 2 times, 210ms apart **
```

If that line appears with `cmd` equal to the light command, the colour button is
a double-tap of the light and Home Assistant can reproduce it by sending the
light command twice - no new command needed. If instead step 13 yields a command
that appears nowhere else, it is a real colour command and gets its own control.

If a press produces no burst in the log, press **Previous Button (redo)** and
repeat that step.

## Part B - the other two remotes

The command set should be identical across remotes; only the 20-bit address
differs. So for remotes two and three, press **Restart Procedure**, then capture
only four buttons each:

- Step 01 Power
- Step 03 Speed 1
- Step 12 Light
- Step 15 LED+

Four commands against a third address is also what confirms the parity rule
generalises - it is the case most likely to disprove it.

Label clearly in the log or in your notes which remote is which:

| Remote | Room | Address |
|--------|------|---------|
| 1 | | |
| 2 | | |
| 3 | | |

## Part C - if something looks wrong

Turn on the **Dump Raw Pulse Timings** switch and repeat the press. The log then
also prints the raw mark/space microsecond pairs of the burst's first frame,
which is what to look at if the decoded bit count is not 30, or if the `marks`
histogram shows more than two clusters.

## What to send back

The whole `capture.log`. Raw is better than summarised - the bursts carry the
pulse statistics and the parity evidence, not just the command codes.
