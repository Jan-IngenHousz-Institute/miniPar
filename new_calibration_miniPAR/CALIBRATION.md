# miniPAR / AS7341 calibration

Two things are measured with the AS7341, and they need **two different calibrations**.
They are deliberately kept independent — see
[why goal B does not reuse goal A's calibration](#why-goal-b-does-not-reuse-goal-as-calibration).

| | goal | output | fitted against |
|---|---|---|---|
| **A** | spectral characteristics of the light | per-channel irradiance, or a 1 nm reconstructed spectrum | LR1-B spectrometer |
| **B** | PAR | one scalar, µmol m⁻² s⁻¹ | Li-Cor Li-250A (`tia_par`) |

Both start from the same raw registers, and both need the gain/integration-time
normalisation. They diverge immediately after.

---

## Common first step — basic counts (AN000633 §2.1)

Not a calibration, just arithmetic from the registers. Mandatory for both goals,
because raw counts are meaningless without the exposure that produced them.

```
x = raw / (gain × ATIME × ASTEP × tick)
```

Both sides use the tick in **milliseconds**, so a coefficient vector fitted on host basic
counts goes into firmware unscaled:

- Host: `basic_counts()` in [as7341_calibrate.py](as7341_calibrate.py) — `ASTEP_TICK_MS = 2.78e-3`
- Firmware: `spectrometerGetBasicCountDivisor()` in [../Firmware/src/app/spectrometer_api.cpp](../Firmware/src/app/spectrometer_api.cpp#L347) — `kAs7341AStepTickMs = 2.78e-3f`

⚠️ **Firmware ≤ 1.05 used the tick in seconds**, making its basic counts 1000× the host
ones; coefficients fitted on host units had to be divided by 1000 first. Since 1.06 that
is no longer true — check `hello`'s third field before uploading a `w` to a device, and see
[the units migration](#units-migration-firmware-105--106).

---

## Goal A — spectral characteristics

| step | what | constant |
|---|---|---|
| §2.1 | basic counts | — |
| §2.2 | subtract ams dark offset, clip at 0 | `OFFSET_BASIC` |
| §2.4 | per-channel spectral coefficient → irradiance-like signal | `SPECTRAL_COEF` |
| optional | 1 nm spectrum, 380–1100 nm: `spectrum = signal @ CM.T` | `CM` (721 × 10) |

`SPECTRAL_COEF` is fleet-level: the median of `reference(LR1-B) / sensor` per channel
per device, then averaged over 3 devices (channel spread 1.4–9.7%), so no per-serial
lookup is needed at deployment.

`CM` is the ams Golden-Device spectral reconstruction matrix, sheet
`used Correction Values` of [../Firmware/docs/AS7341_AD000198_3-00.xlsx](../Firmware/docs/AS7341_AD000198_3-00.xlsx)
(rows 6–726 = 380–1100 nm at 1 nm, columns B–K = F1…F8, Clear, NIR). The workbook
applies it to §2.2 output, i.e. **before** any per-channel spectral coefficient.

- **Notebook:** [spectralCalibration_miniPAR_LR1-B.ipynb](spectralCalibration_miniPAR_LR1-B.ipynb) — `SPECTRAL_COEF` is computed in the §2.4 cell
- **Deployable module:** [as7341_calibrate.py](as7341_calibrate.py) — `calibrated_raw()` implements §2.1 → §2.2 → §2.4 with the constants baked in

Do **not** use the workbook's own row-26 correction vector (1.028 … 1.269). That is the
ams demo unit's golden-diode balance; applied to miniPAR data it inflates Clear against
its large negative CM weight and destroys the result (R² 0.43).

---

## Goal B — PAR, three tiers

PAR is a scalar functional of the spectrum, so it is fitted as a direct 10 → 1 linear
map. Going through goal A's intermediate spectrum first can only lose accuracy.

```
tier 1   x       = raw / (gain × tint)        per sample, no calibration
tier 2   PAR_DEF = w · x                      fleet default, 10 floats
tier 3   PAR     = a · PAR_DEF + b            per device, 2 floats
```

### Tier 2 — fleet vector `w`

Fitted once, from the pooled multi-source dataset (462 samples, 6 devices, spanning
daylight, canopy, gel filters, office and LED panels). This is where all 10 degrees of
freedom belong, because only a spectrally diverse set can identify them.

- **Notebook:** [regression_PAR_miniPAR.ipynb](regression_PAR_miniPAR.ipynb) — the `basic_counts` regression cell fits `w`; the cell below it refits on all samples, validates leave-one-device-out, writes `par_coeffs_fleet.json` and prints the C array
- **Firmware slot:** `kDefaultParCoefficients` in [../Firmware/include/app/spectrometer_api.h](../Firmware/include/app/spectrometer_api.h#L27), loaded into `par_coefficients[]`

### Tier 3 — per-device `a`, `b`

Fitted at onboarding from an **intensity sweep** against the Li-250A. A sweep on one
source identifies a scale and an offset — two parameters — and nothing more.

- **Firmware slot:** `slope` / `intercept`, applied at [spectrometer_api.cpp:584](../Firmware/src/app/spectrometer_api.cpp#L584)
- **Notebook:** [tier3_calibration_miniPAR_DCsource.ipynb](tier3_calibration_miniPAR_DCsource.ipynb)
  — LED panel on the KIPRIM supply: preflight that tier 2 is the fleet `w`, headroom check,
  up/down current sweep, fit on the up leg and validate on the held-out down leg, upload,
  re-check at unfitted levels, one record per run under `data/tier3/`
- **Manual variant:** [../Scripts/MiniParManualCalibration.ipynb](../Scripts/MiniParManualCalibration.ipynb)
  — three points typed in by hand, no DC supply or automated reference

The tier-2 intercept is discarded on export. Composing the tiers gives
`PAR = a·(w·x) + (a·b₀ + b)`, so `b₀` is absorbed by tier 3's intercept; fit with an
intercept so `w` is unbiased, then keep only `w`.

### Firmware already implements this

[spectrometer_api.cpp:578-584](../Firmware/src/app/spectrometer_api.cpp#L578-L584):

```cpp
const float basic_count = (float)result.channels[i] / divisor;   // tier 1
par_raw += basic_count * par_coefficients[i];                     // tier 2
...
Serial.print(par_raw * slope + intercept);                        // tier 3
```

All three slots exist. Only tier 2's numbers need regenerating.

<a id="units-migration-firmware-105--106"></a>
### Units migration — firmware 1.05 → 1.06

1.06 changed tier 1's divisor from `gain × t_int[s]` to `gain × t_int[ms]`, so basic counts
are now 1000× smaller. What that means per interface:

| interface | effect |
|---|---|
| `spec` channel values | **1000× smaller.** Gate on the `hello` version if you interpret them |
| `spec_raw` | unchanged — raw ADC counts, never scaled |
| `par`, `par_raw` | **unchanged.** `kDefaultParCoefficients` grows by the same 1000× (via `kAs7341BasicCountScale`), and NVS-stored coefficients are migrated on first boot by `loadpref()`, keyed on the `units_ver` stamp so it runs once |
| `slope` / `intercept` | unchanged — they multiply `par_raw`, which did not move |
| `set_spec_coeff` | takes per-ms coefficients now, i.e. host-fitted values **directly**, no `/1000` |
| raw `spec_status` | untouched. It is parsed positionally by hosts, so no field was added; the version in `hello` is the detection point |

Downgrading a 1.06 device to ≤ 1.05 re-breaks it: the old firmware ignores `units_ver` and
reads the migrated coefficients as per-second, reporting PAR 1000× low. Re-upload tier 2 if
you roll back.

---

## Why it is built this way

Measured on the 462-sample dataset, 5-fold CV unless stated otherwise.

**Summing the channels throws away most of the accuracy.** A uniform sum weights an
80 nm-wide green channel the same as a narrow violet one and ignores stray light:

| PAR model | R² | median \|err\| |
|---|---|---|
| `sum(cal_f1…f8)`, 1 fitted slope | 0.949 | 17.2% |
| CM → spectrum → ∫400–700, 1 fitted slope | 0.990 | 6.3% |
| 10 channels, free coefficients | 0.998 | ~2% |

**The CM route is a fixed-weight special case, not an alternative.** Because PAR is a
linear functional, `CM → spectrum → integrate` collapses to a single fixed weight vector
`W = CMᵀ·k(λ)`. It is therefore a rank-1 restriction of tier 2 and cannot beat it. Its
value is provenance: `W` needs no reference instrument, and it is smooth and monotone in
λ, so it is a good sanity check on a fitted `w`.

<a id="why-goal-b-does-not-reuse-goal-as-calibration"></a>
**Why goal B does not reuse goal A's calibration.** Two different chainings get confused
here, and only one of them is a mistake:

| chaining | verdict |
|---|---|
| goal A → then **fit** tier 2's 10 coefficients freely on `cal_*` | equivalent to fitting directly, harmless, redundant |
| goal A → then **compute** PAR analytically from the reconstructed spectrum | strictly worse: R² 0.990 vs 0.998 |

The first is a genuine equivalence, not a prohibition. `SPECTRAL_COEF` is a per-channel
diagonal rescale, so fitting `w'` on `cal_*` yields `PAR = w'ᵀ·diag(coef)·s`, and since
`w'` is free, `diag(coef)·w'` ranges over exactly the same vectors as a direct `w`.
Measured: fitting on offset-corrected counts vs raw basic counts agree to 1.8e-12 µmol.

It is nonetheless kept out of the PAR path, for **coupling** reasons rather than accuracy.
If PAR were `w'·diag(SPECTRAL_COEF)·s`, the product would be what determines PAR — so
revising `SPECTRAL_COEF` for goal-A reasons (a better LR1-B calibration, more devices in
the fleet average) would silently shift PAR on every deployed unit with no PAR
measurement having changed. Fitting tier 2 directly against the Li-250A lets the two
goals be revised independently, and keeps the firmware PAR path to a single dot product.

One numerical wrinkle, against chaining: `calibrated_raw` clips at zero after offset
subtraction, which is nonlinear, so the equivalence is not exact there. It fires on 6 of
462 rows (the darkest samples) and makes the chained fit marginally worse —
R² 0.998768230 vs 0.998768642, max prediction difference 1.32 µmol.

The second chaining is the one that costs real accuracy, because going through the
spectrum with **fixed** PAR weights is a rank-1 restriction of a rank-10 fit. See the CM
paragraph above.

**Tier 2 cannot be fitted per device from an intensity sweep.** An intensity sweep moves
the sample along a single ray in 10-space; it identifies a scale, not 10 weights.
Leave-one-device-out, fitting 10 coefficients on the held-out device's own calibration
rows and testing on a different source family:

| | R² | median \|err\| |
|---|---|---|
| 10 coefficients fitted per device | −31 … −7443 | 434 … 779% |
| fleet `w` + 2 per-device params | 0.958 … 0.999 | 0.5 … 3.1% |

(both rows from the same run: tier 3 fitted on the held-out device's daylight rows,
scored on its LED rows)

The per-device 10-coefficient fit does not degrade gracefully — it produces arbitrary
signs and detonates on any spectrum outside its calibration set. This is the failure
mode of the legacy flow (see below).

**Which source to use for the tier-3 sweep.** Both directions work; daylight is the
better calibrator, LEDs are the more repeatable one:

| tier-3 calibration source | scored on | median \|err\| across 6 devices |
|---|---|---|
| daylight rows | LED panels | 2.4% (range 0.5–3.1%) |
| LED panels | daylight | 4.4% (range 2.5–9.8%) |

---

## Current state and open items

- **`kDefaultParCoefficients` is stale and not reproducible from this repo.** The values
  in [spectrometer_api.h:27](../Firmware/include/app/spectrometer_api.h#L27) were fitted on
  **raw** counts at one fixed gain/ATIME/ASTEP and back-converted with the
  `kAs7341BasicCountScale` retrofit multiplier. They trace to the legacy per-device flow;
  the file cell 28 of that notebook loads (`calibration_coeffs_par_window.json`) is not
  in the repo, and `Scripts/calibration_coeffs.json` holds only 9 `channel_coeffs` and
  does not match the header. Their shape is also implausible — F2/F1 = 0.086 against the
  CM's 0.659. **Regenerate from the tier-2 cell, and drop the
  `* kAs7341BasicCountScale` factor**, since the new values are already basic-count based.
  The 1.06 units change deliberately kept that retrofit intact (the multiplier grew 1000×
  with the divisor) so the units migration is verifiable on its own — identical `par`
  before and after. Regenerating tier 2 is still the open item, now unblocked: a `w` fitted
  in `regression_PAR_miniPAR.ipynb` can be pasted in as-is.

- **`w` from plain OLS has unphysical signs** (negative on F3, F5, F8; positive on Clear).
  The shape-normalised design matrix has condition number ≈ 451 and ~290 of 462 samples
  are daylight, so those signs are partly fitting collinearity. Consider constraining
  F1–F8 ≥ 0 with Clear/NIR free, or shrinking toward the CM weights:
  `argmin ‖Xw − y‖² + λ‖w − W‖²`.

- **ValidDevice_2 is the outlier** in both calibration directions (worst R² calibrating on
  daylight, 9.8% median calibrating on LEDs). Worth investigating independently of the
  calibration design before quoting a fleet spec.

- **More narrowband spectra would help.** The dataset is daylight-dominated; breaking the
  collinearity needs more single-colour LED and gel-filter measurements.

---

## Notebook index

| notebook | role |
|---|---|
| [spectralCalibration_miniPAR_LR1-B.ipynb](spectralCalibration_miniPAR_LR1-B.ipynb) | **goal A** — derives `SPECTRAL_COEF` against LR1-B |
| [regression_PAR_miniPAR.ipynb](regression_PAR_miniPAR.ipynb) | **goal B** — compares PAR models, fits and exports tier-2 `w` |
| [tier3_calibration_miniPAR_DCsource.ipynb](tier3_calibration_miniPAR_DCsource.ipynb) | **tier 3** — per-device `a`, `b` from an LED-panel intensity sweep against the Li-250A |
| [acquisition_multi_stable_minipar_lr1b.ipynb](acquisition_multi_stable_minipar_lr1b.ipynb) | acquisition — multi-device + LR1-B + Li-250A, randomised settings → `data/multi_par_spec_lr1b.csv` |
| [acquisition_multi_stabe_minipar.ipynb](acquisition_multi_stabe_minipar.ipynb) | acquisition — multi-device + Li-250A → `data/multi_par_spec.csv` |
| [../Scripts/calibrate_spec_notebook.ipynb](../Scripts/calibrate_spec_notebook.ipynb) | **superseded** — legacy per-device 10-coefficient fit on raw counts, uploaded via `set_spec_coeff`. Wrong tier and missing tier 1; kept for the serial upload/validation cells only |

## Supporting files

| file | role |
|---|---|
| [as7341_calibrate.py](as7341_calibrate.py) | deployable goal-A chain, no workbook or data dependency |
| [../Firmware/docs/AS7341_AD000198_3-00.xlsx](../Firmware/docs/AS7341_AD000198_3-00.xlsx) | ams workbook — `CM`, dark offsets, XYZ matrix |
| [../Firmware/docs/AS7341_AN000633_2-00.pdf](../Firmware/docs/AS7341_AN000633_2-00.pdf) | ams app note — §2.1, §2.2, §2.4 definitions |
| `data/multi_par_spec.csv`, `data/multi_par_spec_lr1b.csv` | 462 samples, 6 devices |
| `par_coeffs_fleet.json` | tier-2 export (generated) |
