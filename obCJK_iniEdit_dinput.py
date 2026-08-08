"""Minimal ctypes-only DirectInput8 joystick binding, used by obCJK_iniEdit.py's
gamepad hotkey capture dialog. No pygame/comtypes dependency — hand-rolls the COM
vtable calls and the DIDATAFORMAT table instead.

Button index N returned by poll() must mean the same physical button obCJK.dll
reads at runtime via IsGamepadButtonDown() (include/obCJK_Gamepad.h ->
QueryJoystickButtonState), because both sides read DIJOYSTATE.rgbButtons[N] —
this module's DIJOYSTATE layout (offsets/size) must stay byte-identical to the
real dinput.h struct for that to hold. The DIDFT_OPTIONAL flags used when
building the data format are a separate, purely-local negotiation with the
driver about which axes/POV/sliders the device happens to support — they do
NOT affect button numbering, so everything except X/Y axis is marked optional
here to maximize compatibility with gamepads that lack some axes.
"""

import ctypes
from ctypes import wintypes


# ── GUID ─────────────────────────────────────────────────────────────────────

class GUID(ctypes.Structure):
    _fields_ = [
        ("Data1", ctypes.c_uint32),
        ("Data2", ctypes.c_uint16),
        ("Data3", ctypes.c_uint16),
        ("Data4", ctypes.c_uint8 * 8),
    ]


def _guid(a, b, c, d):
    return GUID(a, b, c, (ctypes.c_uint8 * 8)(*d))


IID_IDirectInput8W       = _guid(0xBF798031, 0x483A, 0x4DA2, (0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00))
IID_IDirectInputDevice8W = _guid(0x54D41081, 0xDC15, 0x4833, (0xA4, 0x1B, 0x74, 0x8F, 0x73, 0xA3, 0x81, 0x79))

GUID_XAxis  = _guid(0xA36D02A0, 0xC9F3, 0x11CF, (0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00))
GUID_YAxis  = _guid(0xA36D02A1, 0xC9F3, 0x11CF, (0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00))
GUID_ZAxis  = _guid(0xA36D02A2, 0xC9F3, 0x11CF, (0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00))
GUID_RxAxis = _guid(0xA36D02F4, 0xC9F3, 0x11CF, (0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00))
GUID_RyAxis = _guid(0xA36D02F5, 0xC9F3, 0x11CF, (0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00))
GUID_RzAxis = _guid(0xA36D02E0, 0xC9F3, 0x11CF, (0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00))
GUID_Slider = _guid(0xA36D02E3, 0xC9F3, 0x11CF, (0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00))
GUID_POV    = _guid(0xA36D02C2, 0xC9F3, 0x11CF, (0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00))


# ── DIJOYSTATE — must byte-match dinput.h's real struct (see module docstring) ──

class DIJOYSTATE(ctypes.Structure):
    _fields_ = [
        ("lX", ctypes.c_long), ("lY", ctypes.c_long), ("lZ", ctypes.c_long),
        ("lRx", ctypes.c_long), ("lRy", ctypes.c_long), ("lRz", ctypes.c_long),
        ("rglSlider", ctypes.c_long * 2),
        ("rgdwPOV", ctypes.c_uint32 * 4),
        ("rgbButtons", ctypes.c_uint8 * 32),
    ]


assert ctypes.sizeof(DIJOYSTATE) == 0x50, "DIJOYSTATE layout drifted from dinput.h (must stay 0x50 bytes)"


class DIDEVICEINSTANCEW(ctypes.Structure):
    _MAX_PATH = 260
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("guidInstance", GUID),
        ("guidProduct", GUID),
        ("dwDevType", wintypes.DWORD),
        ("tszInstanceName", ctypes.c_wchar * _MAX_PATH),
        ("tszProductName", ctypes.c_wchar * _MAX_PATH),
        ("guidFFDriver", GUID),
        ("wUsagePage", ctypes.c_uint16),
        ("wUsage", ctypes.c_uint16),
    ]


class DIOBJECTDATAFORMAT(ctypes.Structure):
    _fields_ = [
        ("pguid", ctypes.POINTER(GUID)),
        ("dwOfs", wintypes.DWORD),
        ("dwType", wintypes.DWORD),
        ("dwFlags", wintypes.DWORD),
    ]


class DIDATAFORMAT(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("dwObjSize", wintypes.DWORD),
        ("dwFlags", wintypes.DWORD),
        ("dwDataSize", wintypes.DWORD),
        ("dwNumObjs", wintypes.DWORD),
        ("rgodf", ctypes.POINTER(DIOBJECTDATAFORMAT)),
    ]


class DIPROPHEADER(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD), ("dwHeaderSize", wintypes.DWORD),
        ("dwObj", wintypes.DWORD), ("dwHow", wintypes.DWORD),
    ]


DIDFT_ABSAXIS     = 0x00000002
DIDFT_AXIS        = 0x00000003
DIDFT_PSHBUTTON   = 0x00000004
DIDFT_POV         = 0x00000010
DIDFT_ANYINSTANCE = 0x00FFFF00
DIDFT_OPTIONAL    = 0x80000000
DIDF_ABSAXIS      = 0x00000001


def _offset_of(field_name):
    return getattr(DIJOYSTATE, field_name).offset


def _build_joystick_format():
    objs = []
    guid_keepalive = []  # DIOBJECTDATAFORMAT.pguid points into these — keep them alive

    def add(guid, offset, dwtype, optional):
        flags = dwtype | DIDFT_ANYINSTANCE | (DIDFT_OPTIONAL if optional else 0)
        if guid is not None:
            guid_keepalive.append(guid)
            pguid = ctypes.pointer(guid)
        else:
            pguid = None
        objs.append(DIOBJECTDATAFORMAT(pguid, offset, flags, 0))

    long_size = ctypes.sizeof(ctypes.c_long)
    dword_size = ctypes.sizeof(ctypes.c_uint32)

    # X/Y used to be required (optional=False) to mirror the textbook c_dfDIJoystick,
    # but that made SetDataFormat reject an Xbox-compatible pad presented through the
    # legacy DirectInput HID compat layer (observed: E_INVALIDARG / 0x80070057) —
    # this tool only reads buttons, so every axis is optional; DirectInput just
    # leaves an axis's DIJOYSTATE field untouched if the device doesn't have it.
    add(GUID_XAxis, _offset_of("lX"), DIDFT_AXIS, optional=True)
    add(GUID_YAxis, _offset_of("lY"), DIDFT_AXIS, optional=True)
    add(GUID_ZAxis, _offset_of("lZ"), DIDFT_AXIS, optional=True)
    add(GUID_RxAxis, _offset_of("lRx"), DIDFT_AXIS, optional=True)
    add(GUID_RyAxis, _offset_of("lRy"), DIDFT_AXIS, optional=True)
    add(GUID_RzAxis, _offset_of("lRz"), DIDFT_AXIS, optional=True)
    for i in range(2):
        add(GUID_Slider, _offset_of("rglSlider") + i * long_size, DIDFT_AXIS, optional=True)
    # pguid=None (match by dwType only, like buttons below) instead of the exact
    # GUID_POV: observed that a synthesized/XInput-compat hat switch object may
    # not carry the standard GUID_POV tag, which made SetDataFormat silently
    # leave rgdwPOV permanently at the "centered" sentinel (D-pad never detected).
    for i in range(4):
        add(None, _offset_of("rgdwPOV") + i * dword_size, DIDFT_POV, optional=True)
    for i in range(32):
        add(None, _offset_of("rgbButtons") + i, DIDFT_PSHBUTTON, optional=True)

    obj_array = (DIOBJECTDATAFORMAT * len(objs))(*objs)
    fmt = DIDATAFORMAT(
        ctypes.sizeof(DIDATAFORMAT),
        ctypes.sizeof(DIOBJECTDATAFORMAT),
        DIDF_ABSAXIS,
        ctypes.sizeof(DIJOYSTATE),
        len(objs),
        ctypes.cast(obj_array, ctypes.POINTER(DIOBJECTDATAFORMAT)),
    )
    return fmt, obj_array, guid_keepalive


# Module-level so the referenced GUIDs/array stay alive for the process lifetime.
c_dfDIJoystick, _c_dfDIJoystick_objs, _c_dfDIJoystick_guids = _build_joystick_format()


# ── COM vtables (hand-rolled — no comtypes dependency) ──────────────────────
# Unused slots are declared as plain c_void_p to preserve vtable offsets without
# needing a correct call signature for methods we never invoke.

HRESULT = ctypes.c_long


class _IDirectInput8Vtbl(ctypes.Structure):
    _fields_ = [
        ("QueryInterface", ctypes.WINFUNCTYPE(HRESULT, ctypes.c_void_p, ctypes.POINTER(GUID), ctypes.POINTER(ctypes.c_void_p))),
        ("AddRef",  ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)),
        ("Release", ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)),
        ("CreateDevice", ctypes.WINFUNCTYPE(HRESULT, ctypes.c_void_p, ctypes.POINTER(GUID), ctypes.POINTER(ctypes.c_void_p), ctypes.c_void_p)),
        ("EnumDevices", ctypes.WINFUNCTYPE(HRESULT, ctypes.c_void_p, wintypes.DWORD, ctypes.c_void_p, ctypes.c_void_p, wintypes.DWORD)),
        ("GetDeviceStatus", ctypes.c_void_p),
        ("RunControlPanel", ctypes.c_void_p),
        ("Initialize", ctypes.c_void_p),
        ("FindDevice", ctypes.c_void_p),
        ("EnumDevicesBySemantics", ctypes.c_void_p),
        ("ConfigureDevices", ctypes.c_void_p),
    ]


class _IDirectInputDevice8Vtbl(ctypes.Structure):
    _fields_ = [
        ("QueryInterface", ctypes.WINFUNCTYPE(HRESULT, ctypes.c_void_p, ctypes.POINTER(GUID), ctypes.POINTER(ctypes.c_void_p))),
        ("AddRef",  ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)),
        ("Release", ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)),
        ("GetCapabilities", ctypes.c_void_p),
        ("EnumObjects", ctypes.c_void_p),
        ("GetProperty", ctypes.WINFUNCTYPE(HRESULT, ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(DIPROPHEADER))),
        ("SetProperty", ctypes.c_void_p),
        ("Acquire", ctypes.WINFUNCTYPE(HRESULT, ctypes.c_void_p)),
        ("Unacquire", ctypes.WINFUNCTYPE(HRESULT, ctypes.c_void_p)),
        ("GetDeviceState", ctypes.WINFUNCTYPE(HRESULT, ctypes.c_void_p, wintypes.DWORD, ctypes.c_void_p)),
        ("GetDeviceData", ctypes.c_void_p),
        ("SetDataFormat", ctypes.WINFUNCTYPE(HRESULT, ctypes.c_void_p, ctypes.POINTER(DIDATAFORMAT))),
        ("SetEventNotification", ctypes.c_void_p),
        ("SetCooperativeLevel", ctypes.WINFUNCTYPE(HRESULT, ctypes.c_void_p, wintypes.HWND, wintypes.DWORD)),
        ("GetObjectInfo", ctypes.c_void_p),
        ("GetDeviceInfo", ctypes.c_void_p),
        ("RunControlPanel", ctypes.c_void_p),
        ("Initialize", ctypes.c_void_p),
        ("CreateEffect", ctypes.c_void_p),
        ("EnumEffects", ctypes.c_void_p),
        ("GetEffectInfo", ctypes.c_void_p),
        ("GetForceFeedbackState", ctypes.c_void_p),
        ("SendForceFeedbackCommand", ctypes.c_void_p),
        ("EnumCreatedEffectObjects", ctypes.c_void_p),
        ("Escape", ctypes.c_void_p),
        ("Poll", ctypes.WINFUNCTYPE(HRESULT, ctypes.c_void_p)),
    ]


def _vtbl_of(interface_ptr, vtbl_type):
    """interface_ptr: ctypes.c_void_p pointing at a COM object (vtbl-ptr-first layout)."""
    pp = ctypes.cast(interface_ptr, ctypes.POINTER(ctypes.POINTER(vtbl_type)))
    return pp.contents.contents


def _check(hr, what):
    if hr < 0:
        raise OSError(f"{what} failed: 0x{hr & 0xFFFFFFFF:08X}")


def pov_direction_active(pov: int, direction: int) -> bool:
    """direction: 0=Up, 1=Right, 2=Down, 3=Left. Mirrors obCJK_Gamepad.h's
    IsPovDirectionActive exactly (same +-45 deg arc) so a captured D-pad index
    means the same physical direction obCJK.dll checks at runtime. A diagonal
    POV counts as both adjacent cardinals, matching typical D-pad behavior."""
    if pov == 0xFFFFFFFF:
        return False
    diff = (pov - direction * 9000) % 36000
    if diff > 18000:
        diff -= 36000
    return -4500 <= diff <= 4500


DI8DEVCLASS_GAMECTRL = 4
DIEDFL_ATTACHEDONLY  = 0x00000001
DISCL_EXCLUSIVE      = 0x00000001
DISCL_NONEXCLUSIVE   = 0x00000002
DISCL_FOREGROUND     = 0x00000004
DISCL_BACKGROUND     = 0x00000008


class DinputJoystick:
    """Enumerates attached DirectInput game controllers and polls one's buttons."""

    def __init__(self):
        self._di = None      # c_void_p — IDirectInput8W*
        self._device = None  # c_void_p — IDirectInputDevice8W*

    def _ensure_di(self):
        if self._di is not None:
            return
        # GetModuleHandleW's default ctypes return type is a 32-bit c_int, which
        # would silently truncate the pointer on 64-bit Windows — must set restype.
        ctypes.windll.kernel32.GetModuleHandleW.restype = ctypes.c_void_p
        ctypes.windll.kernel32.GetModuleHandleW.argtypes = [ctypes.c_wchar_p]
        hinst = ctypes.windll.kernel32.GetModuleHandleW(None)
        dinput8 = ctypes.WinDLL("dinput8.dll")
        dinput8.DirectInput8Create.argtypes = [
            wintypes.HINSTANCE, wintypes.DWORD, ctypes.POINTER(GUID),
            ctypes.POINTER(ctypes.c_void_p), ctypes.c_void_p,
        ]
        dinput8.DirectInput8Create.restype = HRESULT
        out = ctypes.c_void_p()
        hr = dinput8.DirectInput8Create(hinst, 0x0800, ctypes.byref(IID_IDirectInput8W),
                                         ctypes.byref(out), None)
        _check(hr, "DirectInput8Create")
        self._di = out

    def enumerate(self):
        """Return [(guid_instance: GUID, guid_product: GUID, product_name: str), ...]
        for attached controllers."""
        self._ensure_di()
        vtbl = _vtbl_of(self._di, _IDirectInput8Vtbl)
        found = []

        @ctypes.WINFUNCTYPE(wintypes.BOOL, ctypes.POINTER(DIDEVICEINSTANCEW), ctypes.c_void_p)
        def _callback(lpddi, _pv_ref):
            inst = lpddi.contents
            # Copy out of the transient COM-owned buffer before it's reused for
            # the next callback invocation in this same enumeration.
            found.append((GUID.from_buffer_copy(inst.guidInstance),
                          GUID.from_buffer_copy(inst.guidProduct), inst.tszProductName))
            return 1  # DIENUM_CONTINUE

        hr = vtbl.EnumDevices(self._di, DI8DEVCLASS_GAMECTRL, _callback, None, DIEDFL_ATTACHEDONLY)
        _check(hr, "EnumDevices")
        return found

    def open(self, guid_instance, hwnd):
        """Create+acquire guid_instance (from enumerate()) in non-exclusive foreground mode."""
        self._ensure_di()
        di_vtbl = _vtbl_of(self._di, _IDirectInput8Vtbl)
        dev = ctypes.c_void_p()
        hr = di_vtbl.CreateDevice(self._di, ctypes.byref(guid_instance), ctypes.byref(dev), None)
        _check(hr, "CreateDevice")
        self._device = dev

        dev_vtbl = _vtbl_of(self._device, _IDirectInputDevice8Vtbl)
        _check(dev_vtbl.SetDataFormat(self._device, ctypes.byref(c_dfDIJoystick)), "SetDataFormat")

        # MSDN: SetCooperativeLevel requires a top-level window handle — a Tk
        # Toplevel's winfo_id() can be a child of the real OS-level root window,
        # which DirectInput rejects (observed: ERROR_INVALID_HANDLE / 0x80070006).
        # GetAncestor(hwnd, GA_ROOT) is Microsoft's documented fix for this.
        GA_ROOT = 2
        ctypes.windll.user32.GetAncestor.restype = ctypes.c_void_p
        ctypes.windll.user32.GetAncestor.argtypes = [ctypes.c_void_p, ctypes.c_uint]
        root_hwnd = ctypes.windll.user32.GetAncestor(hwnd, GA_ROOT)
        if root_hwnd:
            hwnd = root_hwnd

        _check(dev_vtbl.SetCooperativeLevel(self._device, hwnd, DISCL_NONEXCLUSIVE | DISCL_FOREGROUND),
               "SetCooperativeLevel")
        dev_vtbl.Acquire(self._device)  # best-effort; failures surface via GetDeviceState below

    def poll(self):
        """Return a 36-element bool list: index 0-31 = button held down,
        32-35 = D-pad (POV hat) Up/Right/Down/Left held. Stick/trigger axes
        are NOT included — no hotkey use case for them."""
        if self._device is None:
            raise RuntimeError("DinputJoystick.open() not called")
        vtbl = _vtbl_of(self._device, _IDirectInputDevice8Vtbl)
        vtbl.Poll(self._device)  # ignore result — some devices don't need/support this
        state = DIJOYSTATE()
        hr = vtbl.GetDeviceState(self._device, ctypes.sizeof(state), ctypes.byref(state))
        if hr < 0:
            vtbl.Acquire(self._device)  # e.g. lost focus — reacquire once and retry
            hr = vtbl.GetDeviceState(self._device, ctypes.sizeof(state), ctypes.byref(state))
            _check(hr, "GetDeviceState")
        buttons = [bool(b & 0x80) for b in state.rgbButtons]
        pov = state.rgdwPOV[0]
        dpad = [pov_direction_active(pov, d) for d in range(4)]  # Up,Right,Down,Left
        return buttons + dpad

    def close(self):
        if self._device is not None:
            vtbl = _vtbl_of(self._device, _IDirectInputDevice8Vtbl)
            vtbl.Unacquire(self._device)
            vtbl.Release(self._device)
            self._device = None
        if self._di is not None:
            vtbl = _vtbl_of(self._di, _IDirectInput8Vtbl)
            vtbl.Release(self._di)
            self._di = None
