"""
miniPAR AS7341 raw counts -> irradiance-like signal.

Deployable version of the chain derived in
``spectralCalibration_miniPAR_LR1-B.ipynb``:

    §2.1  raw channel counts / (gain x integration time)  -> ams "basic counts"
    §2.2  subtract the datasheet dark offset, clip at zero
    §2.4  scale by ONE device-independent spectral coefficient vector

The coefficient vector is the mean of the per-device factors measured against
the LR1-B reference spectrometer (3 devices, channel spread 1.4-9.7%), so no
per-serial lookup is needed at deployment time.

Where the constants come from
-----------------------------
``spectral_coeffs_fleet.json``, written next to this file by
``spectralCalibration_miniPAR_LR1-B.ipynb``, is the single source of truth: it
carries the vectors at full precision plus the provenance (dataset, devices,
per-device spread, reconstruction-matrix source, LR1-B calibration file).

The literals below are a fallback, used when that file is missing or unusable so
that this module still imports and works standalone -- it has no hard dependency
on the AS7341 workbook or on any data directory. A fallback load emits a warning
rather than failing, and ``COEFF_SOURCE`` always says which path was taken.

Usage
-----
    from as7341_calibrate import calibrated_raw, basic_counts, COEFF_SOURCE

    X = calibrated_raw(df)                       # DataFrame (n, 10), cal_* columns
    X = calibrated_raw(df, channels=['clear'])   # any subset, any order
    arr = calibrated_raw(df, as_frame=False)     # plain ndarray
    B = basic_counts(df)                         # stop after 2.1, uncorrected
"""

import json
import warnings
from pathlib import Path

import numpy as np
import pandas as pd

__all__ = ['CHANNELS', 'OFFSET_BASIC', 'SPECTRAL_COEF', 'ASTEP_TICK_MS',
           'COEFFS_JSON', 'COEFF_SOURCE', 'load_coeffs',
           'gain_multiplier', 'integration_time_ms', 'basic_counts',
           'calibrated_raw']


#: Canonical AS7341 channel order used by the calibration constants below.
CHANNELS = ['f1_415', 'f2_445', 'f3_480', 'f4_515', 'f5_555',
            'f6_590', 'f7_630', 'f8_680', 'clear', 'nir']

#: One ASTEP tick, in milliseconds (AS7341: 2.78 us).
ASTEP_TICK_MS = 2.78e-3

#: Exported by spectralCalibration_miniPAR_LR1-B.ipynb, alongside this file.
COEFFS_JSON = Path(__file__).resolve().with_name('spectral_coeffs_fleet.json')

#: Fallback dark offset in ams basic-count units, from the workbook sheet
#: 'used Correction Values' ('Offset, measured with sensor', row 15, cols O..X).
_FALLBACK_OFFSET = [0.001970, 0.007249, 0.003194, 0.001315, 0.001468,
                    0.001858, 0.001763, 0.005217, 0.003000, 0.001000]

#: Fallback device-independent spectral coefficient: mean of the per-device
#: factors fitted against the LR1-B reference (notebook §2.4). Same numbers as
#: the JSON, rounded to six decimals.
_FALLBACK_COEF = [34.950663, 65.289484, 72.697997, 63.273264, 56.737110,
                  52.958660, 48.706781, 42.671670, 31.565986, 5.781618]


def _series_from(record, key, channels, what, sign):
    """One vector out of the JSON, aligned to CHANNELS by name and sanity-checked.

    `sign` is 'positive' for a multiplier (a zero would erase a channel) or
    'non-negative' for an offset. Raises ValueError on anything unusable.
    """
    values = record[key]
    if len(values) != len(channels):
        raise ValueError('%s has %d values for %d channels'
                         % (key, len(values), len(channels)))
    s = pd.Series(np.asarray(values, float), index=list(channels), name=what)
    missing = [c for c in CHANNELS if c not in s.index]
    if missing:
        raise ValueError('%s has no entry for channel(s): %s' % (key, missing))
    s = s.loc[CHANNELS]
    if not np.all(np.isfinite(s.to_numpy())):
        raise ValueError('%s contains a non-finite value' % key)
    if sign == 'positive' and not np.all(s.to_numpy() > 0):
        raise ValueError('%s must be strictly positive: a zero or negative '
                         'coefficient would erase a channel' % key)
    if sign == 'non-negative' and np.any(s.to_numpy() < 0):
        raise ValueError('%s must not be negative' % key)
    return s


def load_coeffs(path=None, strict=False):
    """Read the per-channel constants, JSON first, literals as fallback.

    Parameters
    ----------
    path : path-like, optional
        The export to read. Defaults to `COEFFS_JSON`.
    strict : bool
        Raise instead of falling back, for a caller that wants to know the JSON
        is unusable rather than quietly getting the literals.

    Returns
    -------
    (offset, coef, source) : Series, Series, str
        `source` is the file the numbers came from, or a note that the built-in
        literals were used and why.
    """
    path = COEFFS_JSON if path is None else Path(path)
    try:
        record = json.loads(path.read_text(encoding='utf-8'))
        channels = record.get('channels', CHANNELS)
        offset = _series_from(record, 'offset_basic', channels,
                              'offset_basic', 'non-negative')
        coef = _series_from(record, 'spectral_coeff', channels,
                            'spectral_coef', 'positive')
        return offset, coef, str(path)
    except Exception as exc:                     # missing, malformed, or rejected
        if strict:
            raise
        warnings.warn('%s: %s: %s - falling back to the built-in literals'
                      % (path.name, type(exc).__name__, exc), RuntimeWarning,
                      stacklevel=2)
        return (pd.Series(_FALLBACK_OFFSET, index=CHANNELS, name='offset_basic'),
                pd.Series(_FALLBACK_COEF, index=CHANNELS, name='spectral_coef'),
                'built-in literals (%s unusable: %s)' % (path.name, exc))


OFFSET_BASIC, SPECTRAL_COEF, COEFF_SOURCE = load_coeffs()


def gain_multiplier(reg):
    """AS7341 AGAIN register value -> linear gain multiplier (0.5x .. 512x)."""
    return 0.5 if reg == 0 else float(1 << (int(reg) - 1))


def _resolve(frame, channels):
    """Validate the frame and return the channel list to work with."""
    channels = list(CHANNELS if channels is None else channels)
    missing = [c for c in channels + ['gain', 'aint', 'astep']
               if c not in frame.columns]
    if missing:
        raise KeyError('frame is missing required column(s): %s' % missing)
    return channels


def _vector(values, channels, default, what):
    """Align a per-channel constant to `channels`, accepting Series/array/scalar."""
    if values is None:
        values = default
    if np.isscalar(values):
        return np.full(len(channels), float(values))
    if isinstance(values, pd.Series):
        unknown = [c for c in channels if c not in values.index]
        if unknown:
            raise KeyError('%s has no entry for channel(s): %s' % (what, unknown))
        return values.loc[channels].to_numpy(float)
    values = np.asarray(values, float)
    if values.shape != (len(channels),):
        raise ValueError('%s must have one value per channel (%d), got shape %s'
                         % (what, len(channels), values.shape))
    return values


def integration_time_ms(frame, tick_ms=ASTEP_TICK_MS):
    """gain x integration time per row, in ms -- the ams basic-count divisor.

    Returns a Series aligned to `frame.index`.
    """
    gain = frame['gain'].map(gain_multiplier).astype(float)
    return (gain
            * (frame['aint'].astype(float) + 1)
            * (frame['astep'].astype(float) + 1)
            * tick_ms)


def basic_counts(frame, channels=None, tick_ms=ASTEP_TICK_MS, as_frame=True):
    """§2.1 -- raw counts normalised by gain and integration time (ams units)."""
    channels = _resolve(frame, channels)
    gt = integration_time_ms(frame, tick_ms).to_numpy()
    if not np.all(gt > 0):
        raise ValueError('non-positive gain x integration time in %d row(s)'
                         % int((gt <= 0).sum()))
    b = frame[channels].to_numpy(float) / gt[:, None]
    return pd.DataFrame(b, columns=channels, index=frame.index) if as_frame else b


#: Distinct from None, which DISABLES a correction step. Binding the module
#: vectors at call time rather than at def time means a load_coeffs() reload,
#: or a monkeypatched OFFSET_BASIC / SPECTRAL_COEF, actually takes effect.
_MODULE_DEFAULT = object()


def calibrated_raw(frame, channels=None, offset=_MODULE_DEFAULT,
              coef=_MODULE_DEFAULT, tick_ms=ASTEP_TICK_MS, clip=True,
              as_frame=True):
    """Raw AS7341 counts -> irradiance-like signal.

    Works on any DataFrame carrying the channel columns plus ``gain``, ``aint``
    and ``astep``; the calibration vectors are looked up *by channel name*, so
    a subset or a different column order is fine.

    Parameters
    ----------
    frame : DataFrame
        Must contain `channels` + ``gain`` (AGAIN register), ``aint``, ``astep``.
    channels : list of str, optional
        Channel columns to convert. Defaults to all ten of `CHANNELS`.
    offset, coef : Series, array, scalar or None
        Per-channel dark offset (basic-count units) and spectral coefficient.
        A Series is aligned by channel name; an array must match `channels`
        positionally; ``None`` disables that step (0.0 / 1.0 respectively).
        Omitted, they come from `OFFSET_BASIC` / `SPECTRAL_COEF`, i.e. from
        `COEFF_SOURCE`.
    tick_ms : float
        ASTEP tick in ms. Override for a sensor with a different time base.
    clip : bool
        Clip offset-subtracted counts at zero (as the workbook does).
    as_frame : bool
        Return a DataFrame carrying `frame`'s index (default) or an ndarray.

    Returns
    -------
    DataFrame or ndarray, shape (len(frame), len(channels)).
    """
    channels = _resolve(frame, channels)
    if offset is _MODULE_DEFAULT:
        offset = OFFSET_BASIC
    if coef is _MODULE_DEFAULT:
        coef = SPECTRAL_COEF
    off = _vector(offset, channels, 0.0, 'offset')
    cf = _vector(coef, channels, 1.0, 'coef')

    b = basic_counts(frame, channels, tick_ms, as_frame=False)   # §2.1
    s = b - off                                                  # §2.2
    if clip:
        s = np.clip(s, 0.0, None)
    out = s * cf     
    new_channels = [f"cal_{x}" for x in channels]                                            # §2.4
    return pd.DataFrame(out, columns=new_channels, index=frame.index) if as_frame else out
