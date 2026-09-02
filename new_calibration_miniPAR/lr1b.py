"""
Driver and exposure auto-gain for the ASEQ Instruments LR1-B compact spectrometer.

Transport note
--------------
The LR1-B does *not* enumerate as a virtual COM port. It is a plain USB HID
device (VID 0xE220, PID 0x0100) -- the same interface the bundled
``spectrlib_shared.dll`` uses (it links ``hid.dll`` / ``HidD_*``). So there is no
``serial.Serial`` port to open; we talk 64-byte HID reports instead, via
``pip install hidapi``.

Detector geometry (Toshiba TCD1304)
-----------------------------------
A full frame is 3694 elements: 32 leading dummy/light-shielded, 3648 effective,
14 trailing dummy. Elements 16..27 are optically shielded and are used here as a
per-frame dark reference, so a spectrum can be baseline-corrected without
capturing a separate dark frame.

Typical use::

    from lr1b import LR1B, autogain

    with LR1B.discover() as spec:
        result = autogain(spec, target_counts=45000)
        print(result.exposure_ms, result.spectrum.peak)
"""

from __future__ import annotations

import logging
import math
import struct
import time
from dataclasses import dataclass, field
from enum import IntEnum, IntFlag
from typing import Iterable, Optional, Sequence

import numpy as np

try:
    import hid
except ImportError as exc:  # pragma: no cover - import guard
    raise ImportError(
        "The 'hid' module is missing. Install the cython-hidapi wheel with:\n"
        "    pip install hidapi\n"
        "(note: the similarly named 'hid' package on PyPI is a different, "
        "incompatible wrapper)"
    ) from exc

LOGGER = logging.getLogger(__name__)

# ============================================================================
# Protocol constants
# ============================================================================

VENDOR_ID = 0xE220
PRODUCT_ID = 0x0100

ZERO_REPORT_ID = 0
PACKET_SIZE = 64                 # bytes per HID report, excluding the report ID
STANDARD_TIMEOUT_MS = 200
FLASH_ERASE_TIMEOUT_MS = 5000

PIXELS_PER_PACKET = 30
MAX_PACKETS_IN_FRAME = 124
MAX_SPECTRA_MEMORIES = 64
PACKETS_REMAINING_ERROR = 250    # device signals an error with a huge counter

FLASH_PAYLOAD_SIZE = PACKET_SIZE - 4
FLASH_MAX_READ_PACKETS = 100
FLASH_MAX_OFFSET = 0x1FFFF
FLASH_MAX_BYTES = 0x20000
CALIBRATION_BYTES = 97089        # length of the factory calibration blob

# TCD1304 frame geometry
TOTAL_PIXELS = 3694
EFFECTIVE_SLICE = slice(32, 3680)
N_EFFECTIVE_PIXELS = 3648
DARK_REF_SLICE = slice(16, 28)   # optically shielded elements

# 16-bit ADC (the vendor application is "ASEQ_16bits"). Verify on your unit with
# LR1B.measure_full_scale() if spectra never reach the expected ceiling.
DEFAULT_FULL_SCALE = 65535

# Exposure is transmitted in units of 10 us.
EXPOSURE_TICK_MS = 0.01
MIN_EXPOSURE_MS = 0.01
MAX_EXPOSURE_MS = 60_000.0

PARAMETER_SET_DELAY_S = 0.1


class RequestCode(IntEnum):
    status = 0x01
    set_exposure = 0x02
    set_acquisition_parameters = 0x03
    set_frame_format = 0x04
    set_external_trigger = 0x05
    software_trigger = 0x06
    clear_memory = 0x07
    get_frame_format = 0x08
    get_acquisition_parameters = 0x09
    get_frame = 0x0A
    set_optical_trigger = 0x0B
    set_all_parameters = 0x0C
    read_flash = 0x1A
    write_flash = 0x1B
    erase_flash = 0x1C
    reset = 0xF1
    detach = 0xF2


class ReplyCode(IntEnum):
    status = 0x81
    set_exposure = 0x82
    set_acquisition_parameters = 0x83
    set_frame_format = 0x84
    set_external_trigger = 0x85
    software_trigger = 0x86
    clear_memory = 0x87
    get_frame_format = 0x88
    get_acquisition_parameters = 0x89
    get_frame = 0x8A
    set_optical_trigger = 0x8B
    set_all_parameters = 0x8C
    read_flash = 0x9A
    write_flash = 0x9B
    erase_flash = 0x9C


class ScanMode(IntEnum):
    continuous = 0
    idle = 1
    every_frame_idle = 2
    frame_averaging = 3


class AverageMode(IntEnum):
    disabled = 0
    average_2 = 1
    average_4 = 2
    average_8 = 3


class TriggerMode(IntEnum):
    disabled = 0
    enabled = 1
    oneshot = 2


class TriggerSlope(IntEnum):
    disabled = 0
    rising = 1
    falling = 2
    rise_fall = 3


class Status(IntFlag):
    idle = 0
    in_progress = 1
    memory_full = 2


class SpectrometerError(IOError):
    """Raised on protocol / transport failures."""


# ============================================================================
# Parameter containers
# ============================================================================


@dataclass
class AcquisitionParameters:
    """Acquisition settings. ``exposure_time_ms`` has 10 us resolution."""

    scan_count: int = 1
    blank_scan_count: int = 0
    scan_mode: ScanMode = ScanMode.continuous
    exposure_time_ms: float = 10.0

    @classmethod
    def from_reply(cls, reply: Sequence[int]) -> "AcquisitionParameters":
        scan_count, blank, mode, exp_ticks = struct.unpack(
            "<HHBL", bytes(reply[1:10])
        )
        return cls(
            scan_count=scan_count,
            blank_scan_count=blank,
            scan_mode=ScanMode(mode),
            exposure_time_ms=exp_ticks * EXPOSURE_TICK_MS,
        )

    @property
    def exposure_ticks(self) -> int:
        return int(round(self.exposure_time_ms / EXPOSURE_TICK_MS))

    def to_bytes(self) -> bytes:
        return struct.pack(
            "<HHBL",
            self.scan_count,
            self.blank_scan_count,
            int(self.scan_mode),
            self.exposure_ticks,
        )


@dataclass
class FrameFormat:
    """
    Which detector elements are read out. ``end_element`` is inclusive.

    ``start_element``/``end_element`` index the **effective** pixels (0..3647).
    The device always wraps the requested range with its 46 dummy elements (32
    leading, 14 trailing), so it reports back
    ``pixels_in_frame = end - start + 1 + 46``. Asking for the full effective
    range 0..3647 therefore yields 3694 pixels -- exactly 124 packets, which is
    the protocol maximum. Asking for 0..3693 (i.e. counting the dummies twice)
    yields 3740 and is rejected by the device's packet limit.
    """

    start_element: int = 0
    end_element: int = N_EFFECTIVE_PIXELS - 1
    reduction_mode: AverageMode = AverageMode.disabled
    pixels_in_frame: int = TOTAL_PIXELS

    @classmethod
    def from_reply(cls, reply: Sequence[int]) -> "FrameFormat":
        start, end, reduction, pixels = struct.unpack("<HHBH", bytes(reply[1:8]))
        return cls(
            start_element=start,
            end_element=end,
            reduction_mode=AverageMode(reduction),
            pixels_in_frame=pixels,
        )

    def to_bytes(self) -> bytes:
        return struct.pack(
            "<HHBH",
            self.start_element,
            self.end_element,
            int(self.reduction_mode),
            self.pixels_in_frame,
        )

    @property
    def is_full_frame(self) -> bool:
        return self.start_element == 0 and self.pixels_in_frame == TOTAL_PIXELS


# ============================================================================
# Calibration
# ============================================================================


def _maybe_float(text: str, default: Optional[float] = None) -> Optional[float]:
    try:
        return float(text)
    except (TypeError, ValueError):
        return default


@dataclass
class Calibration:
    """
    Factory calibration, either read from device flash or from a ``.clbr`` file.

    The blob is ASCII: a header line, two scalar lines, then three blank-line
    separated numeric blocks -- wavelength (3653 values, descending),
    PRNU normalisation (3654) and irradiance normalisation (3654). Only the
    first 3648 entries of each block line up with the effective detector pixels.

    Note the blocks are located by scanning rather than by hard-coded line
    numbers: the scalar on line 2 is blank in the ``.clbr`` files shipped with
    the vendor software, which trips up index-based parsers.
    """

    model: str = ""
    type: str = ""
    serial: str = ""
    irr_scaler: float = 1.0
    irr_wave: Optional[float] = None
    wavelengths_full: np.ndarray = field(default_factory=lambda: np.zeros(0))
    prnu_full: np.ndarray = field(default_factory=lambda: np.zeros(0))
    irr_full: np.ndarray = field(default_factory=lambda: np.zeros(0))
    block_sizes: list = field(default_factory=list)   # numeric runs actually found
    n_lines: int = 0

    @property
    def wavelengths(self) -> np.ndarray:
        """Wavelength (nm) of each of the 3648 effective pixels."""
        return self.wavelengths_full[:N_EFFECTIVE_PIXELS]

    @property
    def prnu_norm(self) -> np.ndarray:
        return self.prnu_full[:N_EFFECTIVE_PIXELS]

    @property
    def irr_norm(self) -> np.ndarray:
        return self.irr_full[:N_EFFECTIVE_PIXELS]

    @property
    def has_irradiance(self) -> bool:
        return self.irr_norm.size == N_EFFECTIVE_PIXELS and np.any(self.irr_norm)

    @classmethod
    def from_bytes(cls, raw: bytes) -> "Calibration":
        buf = bytearray(raw)
        while buf and buf[-1] == 0xFF:      # unwritten flash
            buf.pop()
        text = buf.decode("utf-8", errors="replace").replace("\t", "").replace("\r", "")
        lines = text.split("\n")
        if len(lines) < 3:
            raise ValueError("Calibration blob too short to parse")

        header = lines[0].split()
        cal = cls(
            model=header[0] if len(header) > 0 else "",
            type=header[1] if len(header) > 1 else "",
            serial=header[2] if len(header) > 2 else "",
            irr_scaler=_maybe_float(lines[1], 1.0) or 1.0,
            irr_wave=_maybe_float(lines[2]),
        )

        blocks = _numeric_blocks(lines, min_length=1000)
        cal.n_lines = len(lines)
        cal.block_sizes = [b.size for b in blocks]
        if len(blocks) < 1:
            raise ValueError(
                f"No numeric blocks found in calibration blob ({len(lines)} lines)"
            )
        cal.wavelengths_full = blocks[0]
        if len(blocks) > 1:
            cal.prnu_full = blocks[1]
        if len(blocks) > 2:
            cal.irr_full = blocks[2]
        if len(blocks) != 3:
            LOGGER.warning(
                "Expected 3 numeric blocks (3653/3654/3654), found %s in %d lines -- "
                "the flash read may be incomplete.", cal.block_sizes, len(lines)
            )
        return cal

    @classmethod
    def from_file(cls, path: str) -> "Calibration":
        """Load a vendor ``.clbr`` file (useful when flash holds no calibration)."""
        with open(path, "rb") as handle:
            return cls.from_bytes(handle.read())

    def __str__(self) -> str:
        span = (
            f"{self.wavelengths.min():.1f}-{self.wavelengths.max():.1f} nm"
            if self.wavelengths.size
            else "no wavelength data"
        )
        irradiance = "yes"
        if not self.has_irradiance:
            # Say which way it failed, so a truncated flash read is not mistaken
            # for a unit that simply ships without an irradiance calibration.
            irradiance = f"no (blocks found: {self.block_sizes or 'none'})"
        return (
            f"{self.model} {self.type} {self.serial} | {span} | "
            f"irr_scaler={self.irr_scaler:g} | irradiance cal: {irradiance}"
        )


def _numeric_blocks(lines: Sequence[str], min_length: int = 1000) -> list[np.ndarray]:
    """Split ``lines`` into runs of consecutive parseable numbers."""
    blocks: list[np.ndarray] = []
    current: list[float] = []
    for line in lines:
        value = _maybe_float(line.strip())
        if value is None:
            if len(current) >= min_length:
                blocks.append(np.asarray(current, dtype=float))
            current = []
        else:
            current.append(value)
    if len(current) >= min_length:
        blocks.append(np.asarray(current, dtype=float))
    return blocks


# ============================================================================
# Spectrum container
# ============================================================================


@dataclass
class Spectrum:
    """A single readout, plus everything needed to interpret it."""

    wavelengths: np.ndarray          # nm, one per effective pixel
    raw: np.ndarray                  # raw ADC counts, no baseline removed
    exposure_ms: float
    dark_offset: float = 0.0         # scalar baseline from shielded pixels
    dark_frame: Optional[np.ndarray] = None
    n_average: int = 1
    full_scale: int = DEFAULT_FULL_SCALE

    @property
    def counts(self) -> np.ndarray:
        """Baseline-corrected counts (dark frame if supplied, else shielded pixels)."""
        if self.dark_frame is not None:
            return self.raw - self.dark_frame
        return self.raw - self.dark_offset

    @property
    def peak(self) -> float:
        return float(np.max(self.counts)) if self.counts.size else float("nan")

    @property
    def raw_peak(self) -> float:
        return float(np.max(self.raw)) if self.raw.size else float("nan")

    @property
    def peak_wavelength(self) -> float:
        return float(self.wavelengths[int(np.argmax(self.counts))])

    @property
    def n_saturated(self) -> int:
        return int(np.count_nonzero(self.raw >= 0.99 * self.full_scale))

    @property
    def is_saturated(self) -> bool:
        return self.n_saturated > 0

    def in_range(self, wl_min: float, wl_max: float) -> "Spectrum":
        """A copy restricted to a wavelength window."""
        mask = (self.wavelengths >= wl_min) & (self.wavelengths <= wl_max)
        return Spectrum(
            wavelengths=self.wavelengths[mask],
            raw=self.raw[mask],
            exposure_ms=self.exposure_ms,
            dark_offset=self.dark_offset,
            dark_frame=None if self.dark_frame is None else self.dark_frame[mask],
            n_average=self.n_average,
            full_scale=self.full_scale,
        )

    def sorted(self) -> "Spectrum":
        """A copy with wavelengths ascending (the device order is descending)."""
        order = np.argsort(self.wavelengths)
        return Spectrum(
            wavelengths=self.wavelengths[order],
            raw=self.raw[order],
            exposure_ms=self.exposure_ms,
            dark_offset=self.dark_offset,
            dark_frame=None if self.dark_frame is None else self.dark_frame[order],
            n_average=self.n_average,
            full_scale=self.full_scale,
        )

    def counts_per_ms(self) -> np.ndarray:
        """Exposure-normalised counts, comparable across integration times."""
        return self.counts / self.exposure_ms

    def irradiance(self, calibration: Calibration) -> np.ndarray:
        """
        Absolute irradiance using the factory calibration::

            E = counts * irr_norm / (prnu_norm * irr_scaler * exposure_in_10us)

        The vendor documents the shape of this expression but not its unit;
        verify the absolute scale against the ASEQ application before quoting
        physical numbers.
        """
        if not calibration.has_irradiance:
            raise ValueError("Calibration carries no irradiance data")
        exposure_ticks = self.exposure_ms / EXPOSURE_TICK_MS
        return (self.counts * calibration.irr_norm) / (
            calibration.prnu_norm * calibration.irr_scaler * exposure_ticks
        )

    def save_txt(self, path: str, values: Optional[np.ndarray] = None) -> None:
        """Write ``wavelength<TAB>value`` ascending, matching the ASEQ export format."""
        spectrum = self.sorted()
        data = spectrum.counts if values is None else np.asarray(values)[
            np.argsort(self.wavelengths)
        ]
        with open(path, "w", encoding="utf-8") as handle:
            for wl, value in zip(spectrum.wavelengths, data):
                handle.write(f"{wl:.3f}\t{value:.3f}\n")

    def __str__(self) -> str:
        return (
            f"Spectrum({self.exposure_ms:g} ms, n={self.n_average}): "
            f"peak {self.peak:.0f} counts @ {self.peak_wavelength:.1f} nm, "
            f"raw peak {self.raw_peak:.0f}, baseline {self.dark_offset:.0f}"
            f"{', SATURATED' if self.is_saturated else ''}"
        )


# ============================================================================
# Device
# ============================================================================


class LR1B:
    """USB-HID driver for the ASEQ LR1-B."""

    def __init__(
        self,
        serial_no: Optional[str] = None,
        full_scale: int = DEFAULT_FULL_SCALE,
    ) -> None:
        self.serial_no = serial_no
        self.full_scale = full_scale
        self.device = hid.device()
        self.connected = False
        self.status: Optional[Status] = None
        self.frames_in_memory = 0
        self.parameters = AcquisitionParameters()
        self.frame_format = FrameFormat()
        self.calibration: Optional[Calibration] = None
        self.calibration_raw: Optional[bytearray] = None

    # -- discovery ---------------------------------------------------------

    @staticmethod
    def list_devices() -> list[dict]:
        """Every attached LR1-family spectrometer, as hidapi info dicts."""
        return [
            info
            for info in hid.enumerate()
            if info["vendor_id"] == VENDOR_ID and info["product_id"] == PRODUCT_ID
        ]

    @classmethod
    def discover(cls, serial_no: Optional[str] = None, **kwargs) -> "LR1B":
        """Open the first matching spectrometer (already connected on return)."""
        devices = cls.list_devices()
        if not devices:
            raise SpectrometerError(
                "No LR1-B found. Check the USB cable and that no other program "
                "(e.g. the ASEQ application) has the device open."
            )
        if serial_no is None:
            serial_no = devices[0]["serial_number"]
        elif serial_no not in [d["serial_number"] for d in devices]:
            raise SpectrometerError(
                f"Spectrometer {serial_no!r} not among {[d['serial_number'] for d in devices]}"
            )
        instrument = cls(serial_no, **kwargs)
        instrument.open()
        return instrument

    # -- lifecycle ---------------------------------------------------------

    def open(
        self,
        reset: bool = True,
        full_frame: bool = True,
        load_calibration: bool = True,
    ) -> "LR1B":
        try:
            self.device.open(VENDOR_ID, PRODUCT_ID, self.serial_no)
        except OSError as exc:
            raise SpectrometerError(
                f"Unable to open spectrometer {self.serial_no!r}: {exc}"
            ) from exc
        self.connected = True

        if reset:
            self.reset()
            time.sleep(0.2)
        self._drain()

        self.get_parameters()
        if full_frame:
            self.set_full_frame()
        else:
            self.get_frame_format()
        self.get_status()

        if load_calibration:
            try:
                self.get_calibration()
            except Exception as exc:  # calibration is optional, keep going
                LOGGER.warning("Could not read calibration from flash: %s", exc)
        return self

    def close(self) -> None:
        if self.connected:
            self.device.close()
        self.connected = False

    def __enter__(self) -> "LR1B":
        if not self.connected:
            self.open()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> bool:
        self.close()
        return False

    def __str__(self) -> str:
        state = "connected" if self.connected else "disconnected"
        return f"LR1-B [{self.serial_no or '?'}] {state}"

    # -- transport ---------------------------------------------------------

    def _send(self, payload: Iterable[int]) -> None:
        """Write one zero-padded 64-byte HID output report."""
        report = bytearray([ZERO_REPORT_ID]) + bytearray(payload)
        if len(report) > PACKET_SIZE + 1:
            raise ValueError(f"Report of {len(report) - 1} bytes exceeds {PACKET_SIZE}")
        report += bytes(PACKET_SIZE + 1 - len(report))
        try:
            self.device.write(bytes(report))
        except Exception as exc:
            raise SpectrometerError(f"HID write failed: {exc}") from exc

    def _receive(self, expected: ReplyCode, timeout_ms: int = STANDARD_TIMEOUT_MS) -> list:
        """
        Read reports until one carries ``expected``.

        Unsolicited or stale packets (the device answers some fire-and-forget
        commands) are discarded rather than raised on.
        """
        deadline = time.monotonic() + timeout_ms / 1000.0
        while True:
            remaining_ms = max(1, int((deadline - time.monotonic()) * 1000))
            try:
                reply = self.device.read(PACKET_SIZE, remaining_ms)
            except Exception as exc:
                raise SpectrometerError(f"HID read failed: {exc}") from exc

            if reply and reply[0] == int(expected):
                return reply
            if not reply and time.monotonic() >= deadline:
                raise SpectrometerError(
                    f"Timed out after {timeout_ms} ms waiting for {expected.name}"
                )
            if reply:
                LOGGER.debug("Discarding unexpected reply 0x%02X", reply[0])
            if time.monotonic() >= deadline:
                raise SpectrometerError(
                    f"Timed out after {timeout_ms} ms waiting for {expected.name}"
                )

    def _drain(self, timeout_ms: int = 20) -> None:
        """Throw away anything already queued on the IN endpoint."""
        while True:
            try:
                if not self.device.read(PACKET_SIZE, timeout_ms):
                    return
            except Exception:
                return

    def _transact(
        self,
        payload: Iterable[int],
        expected: ReplyCode,
        timeout_ms: int = STANDARD_TIMEOUT_MS,
    ) -> list:
        self._send(payload)
        return self._receive(expected, timeout_ms)

    # -- basic commands ----------------------------------------------------

    def reset(self) -> None:
        self._send([RequestCode.reset])

    def detach(self) -> None:
        self._send([RequestCode.detach])

    def get_status(self) -> Status:
        reply = self._transact([RequestCode.status, 0x00], ReplyCode.status)
        self.status = Status(reply[1])
        self.frames_in_memory = int.from_bytes(bytes(reply[2:4]), "little")
        return self.status

    def clear_memory(self) -> None:
        self._transact([RequestCode.clear_memory], ReplyCode.clear_memory)

    def software_trigger(self) -> None:
        self._send([RequestCode.software_trigger])

    # -- acquisition parameters -------------------------------------------

    def get_parameters(self) -> AcquisitionParameters:
        reply = self._transact(
            [RequestCode.get_acquisition_parameters, 0x00],
            ReplyCode.get_acquisition_parameters,
        )
        self.parameters = AcquisitionParameters.from_reply(reply)
        return self.parameters

    def set_parameters(self, parameters: Optional[AcquisitionParameters] = None) -> None:
        if parameters is not None:
            self.parameters = parameters
        self._transact(
            bytes([RequestCode.set_acquisition_parameters]) + self.parameters.to_bytes(),
            ReplyCode.set_acquisition_parameters,
        )
        time.sleep(PARAMETER_SET_DELAY_S)

    def set_exposure_ms(self, exposure_ms: float) -> float:
        """Set integration time (10 us resolution). Returns the value actually set."""
        exposure_ms = clamp_exposure(exposure_ms)
        self.parameters.exposure_time_ms = exposure_ms
        self._transact(
            bytes([RequestCode.set_exposure])
            + struct.pack("<L", self.parameters.exposure_ticks),
            ReplyCode.set_exposure,
        )
        return self.parameters.exposure_time_ms

    def get_frame_format(self) -> FrameFormat:
        reply = self._transact(
            [RequestCode.get_frame_format], ReplyCode.get_frame_format
        )
        self.frame_format = FrameFormat.from_reply(reply)
        return self.frame_format

    def set_frame_format(self, frame_format: FrameFormat) -> FrameFormat:
        self._transact(
            bytes([RequestCode.set_frame_format]) + frame_format.to_bytes(),
            ReplyCode.set_frame_format,
        )
        return self.get_frame_format()      # read back what the device accepted

    def set_full_frame(self) -> FrameFormat:
        """
        Read out every effective pixel, so pixel indexing is unambiguous.

        Requests elements 0..3647; the device adds its 46 dummies and returns a
        3694-pixel frame.
        """
        fmt = self.set_frame_format(
            FrameFormat(
                start_element=0,
                end_element=N_EFFECTIVE_PIXELS - 1,
                reduction_mode=AverageMode.disabled,
                pixels_in_frame=TOTAL_PIXELS,
            )
        )
        if not fmt.is_full_frame:
            LOGGER.warning(
                "Device reported frame format %s instead of the full %d-pixel frame",
                fmt,
                TOTAL_PIXELS,
            )
        return fmt

    def set_external_trigger(self, mode: TriggerMode, slope: TriggerSlope) -> None:
        self._transact(
            [RequestCode.set_external_trigger, int(mode), int(slope)],
            ReplyCode.set_external_trigger,
        )

    # -- flash / calibration ----------------------------------------------

    def read_flash(self, n_bytes: int, offset: int = 0) -> bytearray:
        if n_bytes < 0 or offset < 0:
            raise ValueError("n_bytes and offset must be positive")
        if offset > FLASH_MAX_OFFSET or offset + n_bytes > FLASH_MAX_BYTES:
            raise ValueError("Flash read exceeds device memory")

        packets_total = int(math.ceil(n_bytes / FLASH_PAYLOAD_SIZE))
        buffer = bytearray(packets_total * FLASH_PAYLOAD_SIZE)
        packets_left = packets_total
        byte_cursor = 0

        while packets_left:
            batch = int(min(packets_left, FLASH_MAX_READ_PACKETS))
            self._send(
                struct.pack("<BIB", RequestCode.read_flash, offset + byte_cursor, batch)
            )
            for received in range(1, batch + 1):
                reply = self._receive(ReplyCode.read_flash)
                local_offset, remaining = struct.unpack("<HB", bytes(reply[1:4]))
                if remaining >= PACKETS_REMAINING_ERROR:
                    raise SpectrometerError("Device reported an error reading flash")
                if remaining != batch - received:
                    raise SpectrometerError("Dropped a flash packet")
                # Place by arrival order, not by the reported offset: the offset
                # field is only 16 bits, so it wraps past 65535 and would scatter
                # the tail of a >64 KB read (the calibration is ~95 KB). The
                # strict `remaining` check above already guarantees packets
                # arrive in order with none missing.
                expected = (received - 1) * FLASH_PAYLOAD_SIZE
                if local_offset != expected % 0x10000:
                    LOGGER.debug(
                        "Flash packet %d reported offset %d, expected %d",
                        received, local_offset, expected % 0x10000,
                    )
                start = byte_cursor + expected
                buffer[start : start + FLASH_PAYLOAD_SIZE] = bytes(reply[4:])
            packets_left -= batch
            byte_cursor += batch * FLASH_PAYLOAD_SIZE

        return buffer[:n_bytes]

    def get_calibration(self) -> Calibration:
        """
        Read the factory calibration out of device flash.

        The raw bytes are kept on ``self.calibration_raw`` so a partial or
        garbled read can be inspected (or dumped to a file and compared against
        the vendor ``.clbr``).
        """
        raw = self.read_flash(CALIBRATION_BYTES)
        self.calibration_raw = raw
        self.calibration = Calibration.from_bytes(raw)
        LOGGER.debug("Loaded calibration: %s", self.calibration)
        return self.calibration

    def load_calibration_file(self, path: str) -> Calibration:
        """Use a vendor ``.clbr`` file instead of the on-device calibration."""
        self.calibration = Calibration.from_file(path)
        return self.calibration

    @property
    def wavelengths(self) -> np.ndarray:
        """Wavelength axis, or pixel indices when no calibration is available."""
        if self.calibration is not None and self.calibration.wavelengths.size:
            return self.calibration.wavelengths
        return np.arange(N_EFFECTIVE_PIXELS, dtype=float)

    # -- frame readout -----------------------------------------------------

    def read_raw_frame(self, buffer_index: int = 0) -> np.ndarray:
        """Fetch one stored frame, all ``pixels_in_frame`` elements, uncropped."""
        if buffer_index >= MAX_SPECTRA_MEMORIES:
            raise ValueError(f"buffer_index must be < {MAX_SPECTRA_MEMORIES}")

        pixels_in_frame = self.frame_format.pixels_in_frame
        packets_to_get = int(math.ceil(pixels_in_frame / PIXELS_PER_PACKET))
        if packets_to_get > MAX_PACKETS_IN_FRAME:
            raise ValueError(
                f"Frame format asks for {pixels_in_frame} pixels "
                f"({packets_to_get} packets, max {MAX_PACKETS_IN_FRAME}). "
                f"start/end index the effective pixels (0..{N_EFFECTIVE_PIXELS - 1}); "
                "the device adds 46 dummy elements on top. Call set_full_frame()."
            )

        self._send(
            struct.pack(
                "<BHHB", RequestCode.get_frame, 0, buffer_index, packets_to_get
            )
        )

        frame = np.zeros(packets_to_get * PIXELS_PER_PACKET, dtype=np.int64)
        for received in range(1, packets_to_get + 1):
            reply = self._receive(ReplyCode.get_frame)
            pixel_offset, remaining = struct.unpack("<HB", bytes(reply[1:4]))
            if remaining >= PACKETS_REMAINING_ERROR:
                raise SpectrometerError("Device reported an error sending the frame")
            if remaining != packets_to_get - received:
                raise SpectrometerError(
                    f"Dropped a frame packet (remaining={remaining}, "
                    f"expected {packets_to_get - received})"
                )
            pixels = struct.unpack(f"<{PIXELS_PER_PACKET}H", bytes(reply[4:]))
            frame[pixel_offset : pixel_offset + PIXELS_PER_PACKET] = pixels

        return frame[:pixels_in_frame]

    def _split_frame(self, frame: np.ndarray) -> tuple[np.ndarray, float]:
        """Return (effective pixels, shielded-pixel baseline)."""
        if self.frame_format.is_full_frame:
            return frame[EFFECTIVE_SLICE].astype(float), float(
                np.median(frame[DARK_REF_SLICE])
            )
        # Non-standard readout window: no shielded pixels to lean on.
        return frame.astype(float), 0.0

    def acquire(self, exposure_ms: Optional[float] = None) -> np.ndarray:
        """Trigger one exposure and return the raw full frame."""
        if exposure_ms is not None:
            self.parameters.exposure_time_ms = clamp_exposure(exposure_ms)
        self.parameters.scan_count = 1
        self.parameters.blank_scan_count = 0
        self.parameters.scan_mode = ScanMode.continuous

        self.clear_memory()
        self.set_parameters()
        self.software_trigger()

        timeout_s = 2.0 + 3.0 * self.parameters.exposure_time_ms / 1000.0
        deadline = time.monotonic() + timeout_s
        poll_s = min(0.05, max(0.002, self.parameters.exposure_time_ms / 2000.0))
        while self.get_status() & Status.in_progress:
            if time.monotonic() > deadline:
                raise SpectrometerError(
                    f"Acquisition did not finish within {timeout_s:.1f} s"
                )
            time.sleep(poll_s)

        return self.read_raw_frame(0)

    def read_spectrum(
        self,
        exposure_ms: Optional[float] = None,
        n_average: int = 1,
        discard_first: bool = True,
        dark_frame: Optional[np.ndarray] = None,
    ) -> Spectrum:
        """
        Capture a spectrum.

        ``discard_first`` throws away one frame before measuring. A CCD keeps
        integrating between readouts, so the first frame after an idle period or
        an exposure change carries accumulated charge and reads high -- which
        would otherwise push the auto-gain loop toward too short an exposure.
        """
        if n_average < 1:
            raise ValueError("n_average must be >= 1")
        if discard_first:
            self.acquire(exposure_ms)
            exposure_ms = None          # already applied

        frames = [self.acquire(exposure_ms if i == 0 else None) for i in range(n_average)]
        stacked = np.mean(np.stack(frames), axis=0)
        pixels, baseline = self._split_frame(stacked)

        # A non-standard readout window can return more pixels than the calibration
        # covers; fall back to indices rather than silently truncating the data.
        axis = self.wavelengths
        if axis.size < pixels.size:
            LOGGER.warning(
                "Wavelength axis has %d entries but the frame returned %d pixels; "
                "falling back to pixel indices.", axis.size, pixels.size
            )
            axis = np.arange(pixels.size, dtype=float)

        return Spectrum(
            wavelengths=axis[: pixels.size],
            raw=pixels,
            exposure_ms=self.parameters.exposure_time_ms,
            dark_offset=baseline,
            dark_frame=dark_frame,
            n_average=n_average,
            full_scale=self.full_scale,
        )

    # -- helpers -----------------------------------------------------------

    def measure_dark_frame(
        self, exposure_ms: float, n_average: int = 8
    ) -> np.ndarray:
        """
        Average several frames with the light path blocked, for use as
        ``dark_frame``. Dark current scales with exposure, so measure it at the
        integration time you will actually use.
        """
        return self.read_spectrum(exposure_ms, n_average=n_average).raw

    def measure_full_scale(
        self, start_ms: float = 1.0, max_ms: float = 2000.0, factor: float = 2.0
    ) -> float:
        """
        Find the ADC ceiling by deliberately over-exposing a bright source.

        Doubles the exposure until the raw peak stops rising, and returns the
        plateau. Use it once per instrument to confirm ``full_scale`` (the code
        assumes a 16-bit ADC, i.e. 65535).
        """
        exposure = start_ms
        previous = -1.0
        peak = float("nan")
        while exposure <= max_ms:
            peak = self.read_spectrum(exposure).raw_peak
            LOGGER.info("full-scale probe: %.3f ms -> raw peak %.0f", exposure, peak)
            if previous > 0 and peak <= previous * 1.01:
                return peak
            previous = peak
            exposure *= factor
        LOGGER.warning("Peak still rising at %.0f ms; source may be too dim", max_ms)
        return peak


def clamp_exposure(
    exposure_ms: float,
    minimum: float = MIN_EXPOSURE_MS,
    maximum: float = MAX_EXPOSURE_MS,
) -> float:
    """
    Clamp to the allowed range and snap to the 10 us hardware tick.

    Rounds half away from zero rather than to even, so stepping is predictable.
    """
    exposure_ms = float(np.clip(exposure_ms, minimum, maximum))
    ticks = math.floor(exposure_ms / EXPOSURE_TICK_MS + 0.5)
    return max(1, ticks) * EXPOSURE_TICK_MS


# ============================================================================
# Auto-gain
# ============================================================================


@dataclass
class AutoGainStep:
    iteration: int
    exposure_ms: float
    peak: float
    raw_peak: float
    n_saturated: int


@dataclass
class AutoGainResult:
    """Outcome of an auto-gain search."""

    exposure_ms: float
    spectrum: Spectrum
    history: list[AutoGainStep]
    converged: bool
    reason: str
    target_counts: float
    roi: Optional[tuple[float, float]] = None

    @property
    def n_iterations(self) -> int:
        return len(self.history)

    @property
    def measured(self) -> Spectrum:
        """The spectrum the loop actually optimised -- the ROI window, if any."""
        return self.spectrum.in_range(*self.roi) if self.roi else self.spectrum

    @property
    def peak(self) -> float:
        return self.measured.peak

    @property
    def error_fraction(self) -> float:
        return (self.peak - self.target_counts) / self.target_counts

    def __str__(self) -> str:
        mark = "converged" if self.converged else "NOT converged"
        where = f" in {self.roi[0]:g}-{self.roi[1]:g} nm" if self.roi else ""
        outside = ""
        if self.roi and self.spectrum.is_saturated and not self.measured.is_saturated:
            outside = (
                f" Note: {self.spectrum.n_saturated} pixels outside the ROI are "
                "saturated."
            )
        return (
            f"Auto-gain {mark} in {self.n_iterations} steps: "
            f"{self.exposure_ms:g} ms -> peak{where} {self.peak:.0f} counts "
            f"(target {self.target_counts:.0f}, off by {self.error_fraction:+.1%}). "
            f"{self.reason}{outside}"
        )


def autogain(
    spectrometer: LR1B,
    target_counts: Optional[float] = None,
    target_fraction: float = 0.75,
    tolerance: float = 0.05,
    exposure_min_ms: float = MIN_EXPOSURE_MS,
    exposure_max_ms: float = 2000.0,
    start_exposure_ms: Optional[float] = None,
    max_iterations: int = 15,
    roi: Optional[tuple[float, float]] = None,
    n_average: int = 1,
    max_step: float = 10.0,
    noise_floor: float = 20.0,
    verbose: bool = True,
) -> AutoGainResult:
    """
    Tune the integration time until the peak of the spectrum hits a target level.

    The detector is linear, so ``peak - baseline`` scales with exposure and one
    measurement predicts the next exposure directly: ``t_new = t * target/peak``.
    Two things break that model and are handled separately:

    * **Clipping.** A saturated peak reads ``full_scale`` no matter how far over
      it really is, so the ratio would barely move. Saturated exposures instead
      become an upper bracket and the search steps down geometrically.
    * **No signal.** With the peak in the noise the ratio explodes, so the step
      is capped at ``max_step`` per iteration.

    Parameters
    ----------
    target_counts : absolute peak level to aim for, in baseline-corrected counts.
        Defaults to ``target_fraction`` of full scale.
    tolerance : relative band around the target that counts as converged.
    roi : optional ``(wl_min, wl_max)`` window in nm to auto-gain on, so a bright
        feature outside your band of interest does not drive the exposure.
    max_step : largest exposure change factor per iteration.
    noise_floor : peak below this is treated as "no signal detected".

    Returns
    -------
    AutoGainResult with the final exposure, the spectrum measured at it, and the
    full search history. Always returns the best exposure found -- check
    ``.converged`` and ``.reason`` before trusting the result.
    """
    full_scale = spectrometer.full_scale
    if target_counts is None:
        target_counts = target_fraction * full_scale
    if not 0 < target_counts < full_scale:
        raise ValueError(
            f"target_counts must be between 0 and full scale ({full_scale})"
        )
    if target_counts > 0.95 * full_scale:
        LOGGER.warning(
            "Target %.0f is within 5%% of full scale; the peak may clip on noise.",
            target_counts,
        )

    exposure = clamp_exposure(
        start_exposure_ms
        if start_exposure_ms is not None
        else spectrometer.parameters.exposure_time_ms,
        exposure_min_ms,
        exposure_max_ms,
    )

    history: list[AutoGainStep] = []
    best: Optional[tuple[float, Spectrum]] = None   # (|relative error|, spectrum)
    saturated_at: Optional[float] = None            # smallest exposure known to clip
    clean_at: Optional[float] = None                # largest exposure known not to clip
    visited: set[float] = set()                     # exposures already measured
    spectrum: Optional[Spectrum] = None
    reason = f"Stopped after {max_iterations} iterations without converging."
    converged = False

    for iteration in range(1, max_iterations + 1):
        spectrum = spectrometer.read_spectrum(exposure, n_average=n_average)
        measured = spectrum.in_range(*roi) if roi else spectrum
        peak = measured.peak
        history.append(
            AutoGainStep(
                iteration=iteration,
                exposure_ms=exposure,
                peak=peak,
                raw_peak=measured.raw_peak,
                n_saturated=measured.n_saturated,
            )
        )
        if verbose:
            flag = f"  [{measured.n_saturated} px saturated]" if measured.is_saturated else ""
            print(
                f"  {iteration:2d}: {exposure:9.2f} ms -> peak {peak:8.0f} counts{flag}"
            )

        if not measured.is_saturated:
            error = abs(peak - target_counts) / target_counts
            if best is None or error < best[0]:
                best = (error, spectrum)
            if error <= tolerance:
                converged = True
                reason = "Peak within tolerance of the target."
                break

        # --- choose the next exposure ---
        if measured.is_saturated:
            saturated_at = exposure if saturated_at is None else min(saturated_at, exposure)
            if clean_at is not None and clean_at < saturated_at:
                candidate = math.sqrt(clean_at * saturated_at)   # bisect in log space
            else:
                # A clipped peak carries no information about how far over it is,
                # so there is nothing to extrapolate from -- descend geometrically
                # until one exposure comes back unclipped, then bisect.
                candidate = exposure / max_step
        elif peak <= noise_floor:
            candidate = exposure * max_step
        else:
            clean_at = exposure if clean_at is None else max(clean_at, exposure)
            candidate = exposure * (target_counts / peak)
            candidate = float(
                np.clip(candidate, exposure / max_step, exposure * max_step)
            )
            if saturated_at is not None and candidate >= saturated_at:
                candidate = math.sqrt(exposure * saturated_at)

        candidate = clamp_exposure(candidate, exposure_min_ms, exposure_max_ms)

        # Stop when the search can make no further progress: pinned at a bound,
        # or hopping between adjacent 10 us ticks that both miss the tolerance.
        at_floor = exposure <= exposure_min_ms + EXPOSURE_TICK_MS / 2
        at_ceiling = exposure >= exposure_max_ms - EXPOSURE_TICK_MS / 2
        if candidate == exposure or candidate in visited:
            if measured.is_saturated and at_floor:
                reason = (
                    f"Still saturated at the minimum exposure ({exposure:g} ms) -- "
                    "attenuate the source or add a neutral-density filter."
                )
            elif measured.is_saturated:
                reason = (
                    f"Saturated at {exposure:g} ms and the search stalled; try a "
                    "lower exposure_min_ms."
                )
            elif at_ceiling and peak < target_counts:
                reason = (
                    f"Peak only reaches {peak:.0f} counts at the maximum exposure "
                    f"({exposure:g} ms) -- raise exposure_max_ms or use averaging."
                )
            else:
                # One tick is EXPOSURE_TICK_MS, so the peak moves in steps of
                # peak/exposure*tick counts. Near the minimum exposure that step
                # can be larger than the requested tolerance.
                step = peak / exposure * EXPOSURE_TICK_MS if exposure else float("nan")
                reason = (
                    f"Exposure resolution limit: at {exposure:g} ms one 10 us tick "
                    f"moves the peak by ~{step:.0f} counts, coarser than the "
                    f"{tolerance:.0%} tolerance. Attenuate the source so a longer "
                    "exposure is needed, or relax the tolerance."
                )
            break

        visited.add(exposure)
        exposure = candidate

    # Prefer the best usable spectrum seen over a final clipped one. In ROI mode
    # "usable" means unclipped inside the ROI; saturation elsewhere is fine.
    final_measured = spectrum.in_range(*roi) if (roi and spectrum) else spectrum
    if best is not None and (final_measured is None or final_measured.is_saturated):
        spectrum = best[1]
    assert spectrum is not None

    result = AutoGainResult(
        exposure_ms=spectrum.exposure_ms,
        spectrum=spectrum,
        history=history,
        converged=converged,
        reason=reason,
        target_counts=target_counts,
        roi=roi,
    )
    if verbose:
        print(result)
    return result
