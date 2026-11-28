# Sensors

Summit gives web pages the device's motion sensors through two APIs:

- **DeviceMotionEvent and DeviceOrientationEvent** (`devicemotion`,
  `deviceorientation` on `window`), which most sites use.
- **The Generic Sensor API**: `Sensor`, `Accelerometer`,
  `LinearAccelerationSensor` and `GravitySensor`, as in Chromium.

On air/OS the readings come from the accelerometers of 2-in-1 laptops, such
as Lenovo's Yoga 11e. The `bosch_accel` driver publishes them as
`/dev/sensor/accelerometer/<n>` (see `docs/airos/TABLET-MODE.md` in the Haiku
fork). Summit reads the one behind the screen, so the readings follow the
screen, as the specifications want.

Other sensors (gyroscope, magnetometer, orientation, ambient light) come with
the Android port, whose devices have them. The plumbing below already carries
every sensor type, so adding one is a platform backend and a DOM interface.

## What pages get

| API | On a 2-in-1 with an accelerometer | On a machine without |
| --- | --- | --- |
| `Accelerometer` | x, y, z in m/s², gravity included | `error` event, `NotReadableError` |
| `LinearAccelerationSensor` | acceleration without gravity | as above |
| `GravitySensor` | gravity alone | as above |
| `devicemotion` | `accelerationIncludingGravity` and `acceleration`; `rotationRate` null (no gyroscope); `interval` 16.7 ms | one event, every value null |
| `deviceorientation` | `beta` and `gamma` from gravity; `alpha` null and `absolute` false (no compass or gyroscope) | one event, every angle null |
| `navigator.permissions.query({name: "accelerometer"})` | `granted` (also `gyroscope`, `magnetometer`) | `granted` |

- Gravity and linear acceleration are made from the accelerometer with a
  low-pass filter (time constant 0.2 s), as Chromium does on devices without
  a gyroscope.
- As in Chromium there is no permission prompt. Permissions policy keeps the
  sensors from third-party frames unless the page allows them
  (`allow="accelerometer"`). All of it needs a secure context.
- `Sensor` readings reach a page only while it is visible and focused (the
  focused frame or a frame of the same origin), as the Generic Sensor
  specification asks. Device motion and orientation events stop while the page
  is hidden. The UI process sends readings only to pages on screen.
- Frequencies: a `Sensor` without `frequency` reports at 5 Hz, at most 60 Hz.
  Motion and orientation events come at 60 Hz. The hardware is polled at the
  highest rate anything asks for, and not at all while nothing listens.
- `referenceFrame: "screen"` turns x and y with `screen.orientation.angle`.
  On air/OS that angle does not yet follow tablet_server's upside-down
  rotation in tent mode (see below).

## html5test

html5test.co scores Device Orientation (3 points), Device Motion (2) and the
Generic Sensor API (5). Its other sensor rows carry no points. All three now
pass: on 28 November the page gave the X399 workstation's Summit (air/OS
hrev60207+2966, this engine) **577 / 588**, against 565 for the
[18 November run](html5test-2026-11-18.md). The sensors are 10 of the 12
points; the other 2 are speech synthesis, which scores there because the X399
has eSpeak voices and the stock VM of the 18 November run had none. Still
missing: Web Payments and WebXR (5 each) and unprefixed speech recognition (1).

## How it works

```
page JS ── Sensor / DeviceMotionController / DeviceOrientationController     (WebCore)
              │
        SensorController (Page supplement: observers per sensor type,
              │           highest frequency, latest reading)
              │  SensorClient
        WebSensorProvider ──IPC──▶ WebSensorProviderProxy (per WebPageProxy)  (WebKit)
                                         │  SensorBackendClient
                                   SensorBackend (one per UI process: runs the
                                         │        hardware, makes gravity and
                                         │        linear acceleration)
                                   SensorBackendHaiku (polling thread, ioctl)
```

Engine files (all in the Summit patch):

- `WebCore/Modules/sensors/`: `SensorType.h` (the types and units),
  `SensorClient.h` (what a port implements), `SensorController` (per page),
  `SensorDeviceClients` (the DeviceMotionClient and DeviceOrientationClient
  made from the controller's sensors, and the tilt from gravity), `Sensor`,
  `Accelerometer` (with the two subclasses), `SensorErrorEvent` and the IDL.
- `WebKit/WebProcess/WebCoreSupport/WebSensorProvider` and
  `WebKit/UIProcess/Sensors/WebSensorProviderProxy`: the IPC between them.
- `WebKit/UIProcess/Sensors/SensorBackend`: shared across ports; a port
  without a backend gets one that has no sensors.
- `WebKit/UIProcess/Sensors/haiku/SensorBackendHaiku.cpp`: the air/OS
  backend. It carries its own copy of the `accelerometer.h` ioctl ABI, because
  the SDKs Summit builds against (and Haiku R1/beta6) are older than the
  header.
- Build switches: `ENABLE_DEVICE_ORIENTATION` and the new
  `ENABLE_GENERIC_SENSORS` (`OptionsHaiku.cmake`, and
  `engine/arm64/rpi4-gl/init-cache-rtc.cmake`, which CI builds every engine
  with). The runtime preference is `GenericSensorAPIEnabled` (on by default).

## Testing

`tests/fixtures/sensors.html` runs html5test's checks, the permission
queries, the three sensors and the two events, and keeps everything in
`window.sensorState` for Summit's MCP `evaluate`. Open it from a secure
context (a `file:` URL works).

Without the hardware, `SUMMIT_SENSOR_ACCELEROMETER=<file>` makes the Haiku
backend read `x y z` (mg, as the driver gives them) from a text file, read
again at every poll. Writing `0 1000 0` into it is a screen standing upright,
`0 0 1000` one lying face up.

## Adding the other sensors (Android)

1. A `SensorBackend` for the port: `platformHasSensor()`, `platformStart()`
   and `platformStop()`, with readings handed to
   `didReceivePlatformReading()` on the main thread, in the units of
   `SensorType.h`. On Android the NDK `ASensorManager` gives every sensor type
   in the UI process. A platform that has its own gravity or linear
   acceleration sensor answers `platformHasSensor()` for it, and the backend
   stops making it from the accelerometer.
2. Turn on `ENABLE_GENERIC_SENSORS` and `ENABLE_DEVICE_ORIENTATION` for the
   port.
3. For each new sensor, its DOM interface in `Modules/sensors` (as
   `Accelerometer`): `Gyroscope` and `Magnetometer` read `values[0..2]`;
   `AbsoluteOrientationSensor` and `RelativeOrientationSensor` derive from
   `OrientationSensor` (quaternion in `values`, `populateMatrix()`);
   `AmbientLightSensor` reads `values[0]`.
4. `SensorDeviceMotionClient` already asks for the gyroscope
   (`rotationRate`). `SensorDeviceOrientationClient` should take `alpha`
   from the orientation sensors when there are any (absolute when the
   magnetometer is part of it).

## Known gaps

- `screen.orientation` on air/OS does not follow the display's rotation:
  tablet_server turns the built-in panel upside down in tent mode, and the
  engine still reports `landscape-primary`. `referenceFrame: "screen"` is
  then wrong by 180°.
- Out-of-process iframes (site isolation, off on air/OS) get no sensors.
- There is no site setting to turn motion sensors off; Chromium has one
  (allowed by default).
