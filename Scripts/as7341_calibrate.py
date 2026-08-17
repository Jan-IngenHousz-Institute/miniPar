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

Constants are baked in here on purpose: this module has no dependency on the
AS7341 workbook (``Firmware/docs/AS7341_AD000198_3-00.xlsx``) or on any data
directory, so it can be imported from any script or notebook.

Usage
-----
    from minipar_signal import to_signal, CHANNELS

    X = to_signal(df)                       # DataFrame (n, 10), df's index
    X = to_signal(df, channels=['clear'])   # any subset, any order
    arr = to_signal(df, as_frame=False)     # plain ndarray
"""

import numpy as np
import pandas as pd

__all__ = ['CHANNELS', 'OFFSET_BASIC', 'SPECTRAL_COEF', 'ASTEP_TICK_MS',
           'gain_multiplier', 'integration_time_ms', 'basic_counts', 'to_signal']


#: Canonical AS7341 channel order used by the calibration constants below.
CHANNELS = ['f1_415', 'f2_445', 'f3_480', 'f4_515', 'f5_555',
            'f6_590', 'f7_630', 'f8_680', 'clear', 'nir']

#: One ASTEP tick, in milliseconds (AS7341: 2.78 us).
ASTEP_TICK_MS = 2.78e-3

#: Dark offset in ams basic-count units, from the workbook sheet
#: 'used Correction Values' ('Offset, measured with sensor', row 15, cols O..X).
OFFSET_BASIC = pd.Series(
    [0.001970, 0.007249, 0.003194, 0.001315, 0.001468,
     0.001858, 0.001763, 0.005217, 0.003000, 0.001000],
    index=CHANNELS, name='offset_basic')

#: Device-independent spectral coefficient: mean of the per-device factors
#: fitted against the LR1-B reference (notebook §2.4).
SPECTRAL_COEF = pd.Series(
    [34.950663, 65.289484, 72.697997, 63.273264, 56.737110,
     52.958660, 48.706781, 42.671670, 31.565986, 5.781618],
    index=CHANNELS, name='spectral_coef')


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


def calibrated_raw(frame, channels=None, offset=OFFSET_BASIC, coef=SPECTRAL_COEF,
              tick_ms=ASTEP_TICK_MS, clip=True, as_frame=True):
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
    off = _vector(offset, channels, 0.0, 'offset')
    cf = _vector(coef, channels, 1.0, 'coef')

    b = basic_counts(frame, channels, tick_ms, as_frame=False)   # §2.1
    s = b - off                                                  # §2.2
    if clip:
        s = np.clip(s, 0.0, None)
    out = s * cf     
    new_channels = [f"cal_{x}" for x in channels]                                            # §2.4
    return pd.DataFrame(out, columns=new_channels, index=frame.index) if as_frame else out
