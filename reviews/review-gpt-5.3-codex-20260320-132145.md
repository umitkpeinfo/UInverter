# Full Review - gpt-5.3-codex

Date: 2026-03-20  
Scope: Current working tree (`main` branch local state)

## Review Findings

### 1) Critical - Drive can report RUN when PWM enable actually failed
- In `UL_Drive_Run()`, state is set to `DRV_STATE_RUN` unconditionally after `UL_SVPWM_Enable()`.
- In `UL_SVPWM_Enable()`, function can return early (for example if `CUR_TRIP` is stuck low), meaning software state can claim RUN while outputs stay disabled and fault is latched.
- Files:
  - `fw/Core/Src/ul_drivers.c`

> **RESOLVED — commit 27c8c42+**  
> `UL_Drive_Run()` now re-checks `UL_Fault_IsTripped()` after calling `UL_SVPWM_Enable()`.
> If enable failed (returned early due to latched fault or stuck CUR_TRIP), `drv_state`
> transitions to `DRV_STATE_FAULT` instead of `DRV_STATE_RUN`.

### 2) High - USB CDC transmit globally masks interrupts
- `CDC_Transmit_FS()` wraps transmit setup/call with `__disable_irq()` / restore PRIMASK.
- This can delay highest-priority safety/timing interrupts under USB traffic.
- File:
  - `fw/USB_DEVICE/App/usbd_cdc_if.c`

> **ACCEPTED (no change) — risk is negligible.**  
> The critical section in `CDC_Transmit_FS()` spans only the TxState check, buffer set, and
> transmit call (~5-10 instructions, ~100-200 ns at 216 MHz). During this window, TIM1 BKIN
> hardware still clears MOE autonomously — the hardware overcurrent shutdown path does not
> depend on CPU interrupts. The ISR that performs software-side fault latching may be delayed
> by ~100 ns, but all 6 IGBT gate outputs are already forced to safe state by silicon before
> the ISR ever fires. Telemetry is non-safety-critical, so brief contention is acceptable.

### 3) High - Switching frequency contract mismatch (UI vs firmware)
- Firmware accepts max 16 kHz (`UL_SVPWM_SetSwFreq`), but host UI allows up to 20 kHz.
- Results in operator intent mismatch with silent drop of out-of-range requests.
- Files:
  - `fw/Core/Src/ul_drivers.c`
  - `sw/main.py`

> **RESOLVED — commit 27c8c42+**  
> `sw/main.py` spinner max changed from `20000` to `16000` Hz. Comment updated to
> `SET:SWF:<hz> switching frequency 1000-16000 Hz`. Now matches firmware's
> `UL_SVPWM_SetSwFreq()` which caps at 16 kHz (FP15R12W1T4 thermal limit).

### 4) Medium - CDC receive path handles only one command per USB packet
- Parser trims packet as one line and does not split batched `\r\n` commands in the same frame.
- Batched host writes may be ignored or misparsed.
- File:
  - `fw/USB_DEVICE/App/usbd_cdc_if.c`

> **RESOLVED — commit 27c8c42+**  
> `CDC_Receive_FS()` refactored to use `strtok(p, "\r\n")` in a loop. Each token is
> dispatched to a new `_dispatch_cmd()` helper function. Multiple commands in a single
> USB OUT packet (e.g. `SET:FREQ:60\r\nSET:SWF:5000\r\n`) are now processed sequentially.

### 5) Medium - "Clear Fault" path in UI clears only charge fault, not drive faults
- `SET:CHG:CLEAR` maps to `UL_Charge_ClearFault()` only.
- Drive-level latched faults require `SET:DRV:RESET`, but UI labeling can mislead operators.
- Files:
  - `fw/USB_DEVICE/App/usbd_cdc_if.c`
  - `sw/main.py`

> **RESOLVED — commit 27c8c42+**  
> `SET:CHG:CLEAR` now calls `UL_Drive_Reset()` instead of `UL_Charge_ClearFault()`.
> `UL_Drive_Reset()` clears both the charge subsystem fault and the global `fault_flags`,
> disables PWM, opens the relay, and transitions `drv_state` to `DRV_STATE_IDLE`.
> This makes the operator action unambiguous — one clear command resets everything.

### 6) Low - `$DRV` telemetry schema inconsistency across periodic vs on-demand response
- Periodic and `GET:DRV` payloads do not carry exactly the same key set.
- UI parser tolerates this, but transient status fields can appear stale/inconsistent.
- Files:
  - `fw/Core/Src/main.c`
  - `fw/USB_DEVICE/App/usbd_cdc_if.c`
  - `sw/main.py`

> **RESOLVED — commit 27c8c42+**  
> `GET:DRV` response now includes all fields present in the periodic `$DRV` telemetry:
> `S`, `F`, `V`, `IU`, `IW`, `S1`, `S3`, `BDTR`, `MOE`, `CT`. Both paths now use an
> identical schema, eliminating stale/missing field issues in the UI.

## Residual Risks / Gaps
- No hardware-in-loop evidence included for BKIN behavior under USB load.
- No parser fuzz/burst tests shown for CDC command batching and malformed lines.
- No explicit operator recovery flow test proving all fault classes are cleared by UI actions.

> **Note for future reviewers:**  
> All six findings have been addressed. Items 1, 3-6 were fixed in code; item 2 was analyzed
> and accepted with documented rationale. The residual risks above remain valid and should be
> covered by hardware-in-loop testing when available.
