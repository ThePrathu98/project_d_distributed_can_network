# Project D — Distributed CAN Network Design

## 1. Purpose

This document defines the CAN network architecture for Project D, including CAN ID ownership, message timing, signal ownership, integrity fields, and physical-layer assumptions.

The network consists of three logical ECUs:

- MOTOR_ECU
- GATEWAY_ECU
- SUPERVISOR_ECU

The initial implementation focuses on the Gateway ECU and Motor ECU. The Supervisor ECU is initially simulated and may be physically implemented later.

---

## 2. CAN Node Ownership

| ECU | Role | Primary Responsibility |
|---|---|---|
| GATEWAY_ECU | CAN command gateway | Receives system commands and transmits MOTOR_COMMAND |
| MOTOR_ECU | Motor controller | Executes commands and publishes motor status/diagnostics |
| SUPERVISOR_ECU | System supervisor | Provides heartbeat, power mode and arm authorization |

### CAN ID ownership

| CAN ID | Hex | Message | Transmitter | Receivers | Length |
|---:|---:|---|---|---|---:|
| 256 | 0x100 | MOTOR_COMMAND | GATEWAY_ECU | MOTOR_ECU | 8 bytes |
| 257 | 0x101 | MOTOR_STATUS | MOTOR_ECU | GATEWAY_ECU | 8 bytes |
| 258 | 0x102 | MOTOR_DIAGNOSTIC | MOTOR_ECU | GATEWAY_ECU | 8 bytes |
| 272 | 0x110 | SUPERVISOR_HEARTBEAT | SUPERVISOR_ECU | MOTOR_ECU, GATEWAY_ECU | 8 bytes |
| 288 | 0x120 | GATEWAY_HEARTBEAT | GATEWAY_ECU | MOTOR_ECU, SUPERVISOR_ECU | 8 bytes |

### ID allocation

The current Project D CAN ID allocation is:

- 0x100–0x10F: Motor command/status/diagnostic traffic
- 0x110: Supervisor heartbeat
- 0x120: Gateway heartbeat

No other CAN IDs are currently allocated.

New CAN IDs must be added to the DBC and this document before implementation.

---

## 3. Message Timing

### Defined CAN message periods

| Message | CAN ID | Period | Frequency | Purpose |
|---|---:|---:|---:|---|
| MOTOR_COMMAND | 0x100 | TBD | TBD | Gateway-to-Motor command |
| MOTOR_STATUS | 0x101 | 20 ms | 50 Hz | Motor operating status |
| MOTOR_DIAGNOSTIC | 0x102 | 100 ms | 10 Hz | Motor diagnostic information |
| SUPERVISOR_HEARTBEAT | 0x110 | 20 ms | 50 Hz | Supervisor health/authorization |
| GATEWAY_HEARTBEAT | 0x120 | 100 ms | 10 Hz | Gateway/network health |

The 20 ms MOTOR_STATUS period is the primary CAN status-update requirement.

The motor-control loop itself operates at 1 kHz (1 ms period). CAN status publication is therefore decoupled from the motor-control loop.

The 1 kHz motor-control loop must not be blocked by CAN transmission, reception, logging, or diagnostic processing.

---

## 4. Timing Budget

### Motor control

Motor control loop:

- Control-loop period: 1 ms
- Control-loop frequency: 1 kHz
- CAN status publication: every 20 ms
- CAN diagnostics publication: every 100 ms

The CAN stack should operate asynchronously relative to the motor-control loop.

### CAN timing requirements

#### MOTOR_COMMAND

The DBC currently does not define a periodic transmission time for MOTOR_COMMAND.

Therefore:

- Transmission timing: TBD
- Expected response/deadline: TBD
- Command timeout: TBD

The command timing requirement must be defined before final safety/deadline testing.

#### MOTOR_STATUS

- Period: 20 ms
- Frequency: 50 Hz
- Required publication rate: 20 ms

#### MOTOR_DIAGNOSTIC

- Period: 100 ms
- Frequency: 10 Hz

#### SUPERVISOR_HEARTBEAT

- Period: 20 ms
- Frequency: 50 Hz

#### GATEWAY_HEARTBEAT

- Period: 100 ms
- Frequency: 10 Hz

---

## 5. CAN Message Integrity

The following messages contain a 4-bit rolling counter and CRC8:

- MOTOR_COMMAND
- MOTOR_STATUS
- MOTOR_DIAGNOSTIC
- SUPERVISOR_HEARTBEAT
- GATEWAY_HEARTBEAT

The rolling counter is used to detect repeated, missing, or unexpected message sequences.

The DBC defines the counter as a 4-bit value.

Therefore the counter sequence is:

0 → 1 → 2 → ... → 15 → 0

The exact CRC8 polynomial, initialization value, final XOR value, byte coverage and bit/byte ordering must match the common C implementation used by the communicating ECUs.

The DBC intentionally does not define the CRC algorithm itself.

---

## 6. MOTOR_COMMAND — CAN ID 0x100

Transmitter:

- GATEWAY_ECU

Receiver:

- MOTOR_ECU

Length:

- 8 bytes

Signals:

| Signal | Start Bit | Length | Scaling | Unit |
|---|---:|---:|---|---|
| Command | 0 | 8 | 1 | — |
| TargetRPM | 8 | 16 | 1 | rpm |
| CommandCounter | 48 | 4 | 1 | — |
| CRC8 | 56 | 8 | 1 | — |

Command enumeration:

| Value | Command |
|---:|---|
| 0 | DISARM |
| 1 | ARM |
| 2 | SET_SPEED |
| 3 | STOP |
| 4 | CLEAR_FAULT |

Target RPM range:

- 0–5000 rpm

Command timing is currently TBD.

---

## 7. MOTOR_STATUS — CAN ID 0x101

Transmitter:

- MOTOR_ECU

Receiver:

- GATEWAY_ECU

Length:

- 8 bytes

Period:

- 20 ms
- 50 Hz

Signals:

| Signal | Start Bit | Length | Scaling | Unit |
|---|---:|---:|---|---|
| TargetRPM | 0 | 16 | 1 | rpm |
| ActualRPM | 16 | 16 | 1 | rpm |
| Current | 32 | 16 | 0.1 | A |
| MotorState | 48 | 4 | 1 | — |
| StatusCounter | 52 | 4 | 1 | — |
| CRC8 | 56 | 8 | 1 | — |

Motor state enumeration:

| Value | State |
|---:|---|
| 0 | IDLE |
| 1 | ARMED |
| 2 | RUNNING |
| 3 | FAULT |

Motor control continues to operate at 1 kHz while CAN status is published at 50 Hz.

---

## 8. MOTOR_DIAGNOSTIC — CAN ID 0x102

Transmitter:

- MOTOR_ECU

Receiver:

- GATEWAY_ECU

Length:

- 8 bytes

Period:

- 100 ms
- 10 Hz

Signals:

| Signal | Start Bit | Length | Scaling | Unit |
|---|---:|---:|---|---|
| FaultCode | 0 | 16 | 1 | — |
| RxErrorCount | 16 | 8 | 1 | — |
| TxErrorCount | 24 | 8 | 1 | — |
| MissedDeadlineCount | 32 | 8 | 1 | — |
| HeartbeatAgeMs | 40 | 8 | 10 | ms |
| DiagnosticCounter | 48 | 4 | 1 | — |
| CRC8 | 56 | 8 | 1 | — |

HeartbeatAgeMs range:

- 0–2550 ms

The fault-code enumeration should be expanded as implementation details are finalized.

---

## 9. SUPERVISOR_HEARTBEAT — CAN ID 0x110

Transmitter:

- SUPERVISOR_ECU

Receivers:

- MOTOR_ECU
- GATEWAY_ECU

Length:

- 8 bytes

Period:

- 20 ms
- 50 Hz

Signals:

| Signal | Start Bit | Length |
|---|---:|---:|
| HeartbeatCounter | 0 | 8 |
| PowerMode | 8 | 4 |
| ArmAuthorization | 12 | 1 |
| SupervisorState | 16 | 8 |
| SequenceCounter | 48 | 4 |
| CRC8 | 56 | 8 |

Power mode:

| Value | Mode |
|---:|---|
| 0 | OFF |
| 1 | STANDBY |
| 2 | RUN |
| 3 | FAULT |

Arm authorization:

| Value | State |
|---:|---|
| 0 | NOT_AUTHORIZED |
| 1 | AUTHORIZED |

Supervisor state:

| Value | State |
|---:|---|
| 0 | INIT |
| 1 | READY |
| 2 | RUNNING |
| 3 | FAULT |

The Supervisor ECU is initially simulated.

---

## 10. GATEWAY_HEARTBEAT — CAN ID 0x120

Transmitter:

- GATEWAY_ECU

Receivers:

- MOTOR_ECU
- SUPERVISOR_ECU

Length:

- 8 bytes

Period:

- 100 ms
- 10 Hz

Signals:

| Signal | Start Bit | Length |
|---|---:|---:|
| HeartbeatCounter | 0 | 8 |
| NetworkState | 8 | 4 |
| TcpConnected | 12 | 1 |
| SequenceCounter | 48 | 4 |
| CRC8 | 56 | 8 |

Network state:

| Value | State |
|---:|---|
| 0 | DISCONNECTED |
| 1 | CONNECTED |
| 2 | DEGRADED |
| 3 | FAULT |

TCP connection state:

| Value | State |
|---:|---|
| 0 | DISCONNECTED |
| 1 | CONNECTED |

---

## 11. Physical Layer

The Project D CAN network uses a differential CAN physical layer.

The physical-layer implementation must include:

- CAN_H
- CAN_L
- CAN transceiver
- Common CAN ground/reference
- Proper bus termination
- CAN-compatible twisted-pair wiring

### Termination

A properly terminated CAN bus requires termination at the two physical ends of the CAN bus.

The exact termination resistance and physical topology must match the selected CAN transceiver and hardware design.

### Bit rate

CAN bit rate is TBD and must be finalized based on:

- Selected CAN controller
- Selected CAN transceiver
- Bus length
- Required message latency
- Hardware capability
- CAN analyzer configuration

The same CAN bit rate must be configured on all Project D nodes.

### Physical-layer validation

The CAN physical layer should be validated using:

- CAN analyzer
- Oscilloscope or logic analyzer where appropriate
- CAN_H/CAN_L differential waveform inspection
- Bus termination verification
- Error-frame monitoring

---

## 12. Fault Handling and Monitoring

The network should monitor:

- Missing CAN messages
- Message deadline violations
- Rolling-counter errors
- CRC errors
- CAN receive errors
- CAN transmit errors
- Heartbeat timeout
- Invalid motor state
- Invalid supervisor authorization
- CAN bus/network state

The MOTOR_DIAGNOSTIC message provides counters for:

- RX errors
- TX errors
- Missed deadlines
- Heartbeat age

Fault-handling thresholds and timeout values are implementation items and must be finalized during testing.

---

## 13. Initial Project Implementation

Initial implementation priority:

1. Bring up CAN hardware.
2. Configure CAN controller and transceiver.
3. Verify CAN physical-layer connectivity.
4. Implement MOTOR_COMMAND transmission.
5. Implement MOTOR_COMMAND reception.
6. Implement MOTOR_STATUS transmission.
7. Implement MOTOR_STATUS reception.
8. Implement rolling counters.
9. Implement CRC8.
10. Implement heartbeat messages.
11. Implement diagnostic reporting.
12. Validate message timing using a CAN analyzer.
13. Inject communication faults and verify detection.
14. Document test results.

---

## 14. DBC as Network Source of Truth

The Project D DBC file is the source of truth for:

- CAN IDs
- Message names
- Message lengths
- Signal positions
- Signal sizes
- Signal scaling
- Signal ranges
- Enumerated values
- Message cycle times

Any change to the CAN protocol must be reflected in both:

1. `dbc/ALSO_Project_D_DBC_File.dbc`
2. `docs/network_design.md`

The firmware implementation must remain consistent with the DBC.

---

## 15. Open Items / TBD

The following items remain to be finalized:

- CAN bus bit rate
- MOTOR_COMMAND transmission period
- MOTOR_COMMAND timeout/deadline
- CAN bus physical topology
- Exact termination resistance
- Selected CAN transceiver
- Maximum bus length
- Exact CRC8 algorithm
- CRC coverage
- CRC initialization value
- CRC final XOR value
- Detailed fault-code enumeration
- Heartbeat timeout thresholds
- Message deadline thresholds
- CAN fault recovery behavior

These values should be finalized before the Project D network is considered production-ready.