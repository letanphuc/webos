# Why WebOS?

Embedded development often makes a small application change unnecessarily
expensive. Changing an LED pattern, adjusting a sensor rule, or fixing a request
handler can require compiling an entire firmware image, putting the device into
flash mode, writing megabytes over a serial connection, and rebooting every
service on the board.

That workflow is reasonable when the whole product is firmware. It becomes a
problem when the product needs to evolve like software.

WebOS changes the unit of development. The firmware becomes a stable platform,
and product behavior becomes a deployable application.

```text
Traditional embedded system

application + drivers + network + filesystem + RTOS -> one firmware image

WebOS

stable Zephyr host + replaceable WebAssembly application
```

This separation is the main reason WebOS exists. It keeps the parts that need
native access, careful integration, and infrequent updates in the host, while
moving frequently changing behavior into small WebAssembly payloads.

## The Firmware Is A Platform, Not The Application

A conventional embedded application usually owns everything: startup,
networking, drivers, storage, business logic, and update behavior. Every project
reassembles those pieces, and every application release risks changing the
whole device.

WebOS gives those responsibilities a durable home:

- Zephyr owns scheduling, drivers, networking, USB, and system resources.
- WebOS services own storage, HTTP, logs, health, shell access, and OTA.
- WAMR `iwasm` owns application loading and execution.
- WASM applications own the product-specific behavior.

This is similar to the boundary on a desktop or server. Installing a new program
does not require rebuilding the kernel, network stack, and filesystem. WebOS
brings that development model to a microcontroller without trying to turn the
microcontroller into a general-purpose computer.

## Fast Iteration Changes What Is Practical

The normal WebOS application loop is:

```text
edit -> build -> upload -> run -> inspect
```

A developer can perform that loop with:

```sh
wdb app run webos/sampleapps/blink
```

The command builds the application, uploads its WASM payload, runs it through
`iwasm`, and returns its output. The host firmware remains installed and the
board keeps its platform configuration.

This saves more than flash time. A shorter loop makes it practical to:

- test small behavior changes on real hardware;
- compare several implementations quickly;
- automate application-level device tests;
- let application developers work without rebuilding Zephyr;
- deploy a fix without replacing unrelated system components.

Fast feedback improves reliability because developers can afford to validate
more often. It also makes embedded work approachable to people who understand
application development but do not need to become Zephyr or board-port experts.

## Useful Services Are Already There

Most connected-device applications need the same foundations: a network
connection, persistent data, secure requests, logs, health information, update
support, and a way to reach the hardware. Reimplementing these in every payload
would waste flash, memory, and engineering time.

WebOS provides them once in native firmware:

| Platform capability | Application benefit |
| --- | --- |
| Wi-Fi and HTTP/HTTPS | Applications use connectivity without owning a network stack |
| FatFS storage | Payloads and persistent data have a known filesystem layout |
| `/dev` hardware model | Applications use stable paths instead of board-specific drivers |
| Logging and health | Failures can be observed without adding a custom debug protocol |
| Shell and WDB | Developers can inspect and control a live device consistently |
| MCUboot and OTA | The host can be upgraded and recovered independently of applications |
| Versioned WebOS ABI | Host and application releases have an explicit compatibility contract |

Centralizing these services also gives the device one place to enforce limits,
validate buffers, define policy, and improve implementations. A TLS fix or
logging improvement belongs in the host rather than in every application.

## Hardware Becomes Composable

Embedded APIs are often organized around driver-specific C calls. That is fast,
but it binds application source to a particular SDK, peripheral driver, and
board layout.

WebOS takes inspiration from Unix and Linux: hardware is represented as files.

```text
/dev/gpio/2/direction
/dev/gpio/2/value
/dev/led/48/color
```

Applications interact with those paths using the same read and write model they
use for other resources. Native Zephyr drivers still perform the real hardware
access, but applications no longer need to know their internal APIs.

This model is valuable because it is:

- **discoverable**: tools and applications can enumerate `/dev`;
- **consistent**: GPIO, LEDs, and future device classes share one access model;
- **testable**: WDB and the shell can exercise the same interface as an app;
- **replaceable**: a driver can change while its file contract remains stable;
- **policy-friendly**: the host can grant an app access to selected paths.

A file interface is not the best abstraction for every high-throughput or
real-time peripheral. WebOS can retain native services for those cases. The
important point is that common hardware operations gain a simple, universal
contract.

## WebAssembly Is A Good Application Boundary

WebAssembly gives WebOS a compact executable format with a defined memory model
and explicit imports. Applications cannot accidentally call arbitrary Zephyr
functions; they can use only the host operations WebOS exposes.

That boundary provides several practical advantages:

- C and Rust can target the same application format.
- Payloads are independent of native firmware addresses.
- The host validates application memory before native code accesses it.
- Applications carry ABI version metadata that the host checks before launch.
- Interpreter mode favors portability and debugging; AOT mode can favor speed.
- Future package permissions and resource controls have a natural enforcement
  point.

WebAssembly does not automatically make untrusted code safe. The current project
assumes trusted developer payloads, and production use still requires stronger
package authentication, capability policy, quotas, and lifecycle isolation.
Even with that limitation, WASM creates a cleaner and safer engineering boundary
than directly linking changing product logic into the firmware image.

## Operations Are Part Of Development

A good application platform must help after code compiles. Embedded failures
often occur during boot, Wi-Fi setup, flash access, or interaction with physical
hardware, where a normal source debugger is not enough.

WDB treats the device as a system that can be observed and managed:

```sh
wdb devices
wdb status
wdb logcat --follow
wdb shell fs ls /dev
wdb app run webos/sampleapps/hello
```

Its daemon owns the serial port, retains boot logs across resets, and coordinates
flashing with other commands. HTTP handles structured runtime operations, while
serial remains available for early boot and recovery. Optional ADB adds a
familiar USB shell and log workflow without replacing WDB.

This makes deployment and debugging part of one coherent interface instead of a
collection of unrelated scripts, serial terminals, and manual HTTP requests.

## Independent Lifecycles Reduce Risk

WebOS has two release paths:

- **Host updates** change Zephyr, native drivers, system services, or the runtime
  and use signed firmware plus MCUboot OTA and rollback.
- **Application updates** change product behavior and replace a small payload in
  the device filesystem.

Separating those paths reduces the blast radius of routine application changes.
An application update does not need to rewrite the bootloader, Wi-Fi stack, or
flash layout. A host update does not require application behavior to be merged
into the same source tree and release artifact.

The versioned ABI connects the two lifecycles. Compatibility is checked
explicitly rather than depending on native symbol addresses or an accidental C
layout.

## Why Not Use Something Else?

WebOS occupies a useful space between bare firmware and a Linux computer.

| Approach | Strength | Cost for this use case |
| --- | --- | --- |
| Monolithic Zephyr firmware | Maximum native control and efficiency | Every behavior change rebuilds and reflashes the system |
| Embedded scripting runtime | Very quick iteration | Language-specific runtime and often a weaker deployment contract |
| Linux single-board computer | Rich processes, packages, and tools | More memory, storage, power, boot time, and operational complexity |
| WebOS | Zephyr-sized host with deployable WASM apps | Runtime overhead and a deliberately smaller application API |

WebOS is not intended to replace native firmware for hard real-time control,
extreme code-size constraints, or workloads that need unrestricted access to
every Zephyr API. It is strongest when a capable ESP32-class device needs stable
system software and behavior that changes independently.

## When WebOS Is A Good Fit

WebOS is a good match for:

- connected sensors and controllers with evolving rules;
- configurable lighting, automation, and edge devices;
- products that share one hardware platform across several behaviors;
- field devices that need small application updates and robust host recovery;
- teams that want firmware engineers to own the platform and application
  developers to own product logic;
- prototypes that should grow into an operable device rather than remain a
  collection of board-specific scripts.

It is a less natural fit for tiny MCUs without external memory, applications
whose critical path must be entirely native, or devices where the firmware and
product behavior always ship as one fixed artifact.

## The Core Advantage

WebOS makes an embedded device easier to change without making its foundation
easy to break.

Zephyr remains responsible for deterministic hardware and system work. WAMR
provides a portable application boundary. The WebOS ABI and `/dev` provide
stable contracts. WDB makes the whole system practical to build, deploy, and
debug.

The result is a device that keeps the efficiency and hardware access of an RTOS,
while gaining one of the most useful properties of a desktop or server platform:
applications can evolve independently of the operating system beneath them.
