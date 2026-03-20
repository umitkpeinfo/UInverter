# Review Items

- Model: GPT-5.4
- Generated: 2026-03-20 13:21:39
- Scope: IGBT driver, SVPWM control path, telemetry, and operator command flow

## Findings

1. High - `UL_Drive_Run()` can report `RUN` even when PWM enable failed.
Paths: `fw/Core/Src/ul_drivers.c`
Reason: `UL_Drive_Run()` sets `drv_state = DRV_STATE_RUN` unconditionally after calling `UL_SVPWM_Enable()`, but `UL_SVPWM_Enable()` can return early when a fault is already latched or when `_cur_trip_stuck()` trips `FAULT_OVERCURRENT`.

> **RESOLVED — commit 27c8c42+**  
> `UL_Drive_Run()` now re-checks `UL_Fault_IsTripped()` after calling `UL_SVPWM_Enable()`.
> If enable failed, `drv_state` transitions to `DRV_STATE_FAULT` instead of `DRV_STATE_RUN`.
> Additionally, `UL_Fault_Set()` itself now sets `drv_state = DRV_STATE_FAULT` immediately,
> so the enable-path early-return always results in a consistent FAULT state.

2. High - Fault-bit updates are non-atomic and can lose `FAULT_OVERCURRENT` under BKIN preemption.
Paths: `fw/Core/Src/ul_drivers.c`, `fw/Core/Src/stm32f7xx_it.c`
Reason: `fault_flags |= ...` is used in both `UL_Fault_Set()` and `UL_BKIN_IRQHandler()`. Since `TIM1_BRK` has higher priority than the update ISR and task context, concurrent load-modify-store sequences can overwrite a newly latched break fault.

> **RESOLVED — commit 27c8c42+**  
> `UL_Fault_Set()` now wraps the `fault_flags |= mask` operation in a
> `__disable_irq()` / `__set_PRIMASK()` critical section. This prevents the BKIN ISR
> (priority 0) from preempting between the load and store, ensuring no fault bit is lost.
> The BKIN ISR path (`UL_BKIN_IRQHandler`) is safe as-is because it runs at the highest
> priority and cannot itself be preempted by anything that modifies `fault_flags`.

3. High - `GET:DRV` still drives the UI to a false `MOE: OFF` state.
Paths: `fw/USB_DEVICE/App/usbd_cdc_if.c`, `fw/Core/Src/main.c`, `sw/main.py`
Reason: On-demand `$DRV` responses omit `MOE`, while the UI treats any missing `MOE` as OFF. Pressing `GET:DRV` can therefore overwrite a correct periodic status with a false disabled indication until the next periodic `$DRV` line arrives.

> **RESOLVED — commit 27c8c42+**  
> `GET:DRV` response now includes all fields: `S`, `F`, `V`, `IU`, `IW`, `S1`, `S3`,
> `BDTR`, `MOE`, `CT`. The schema is now identical to the periodic `$DRV` telemetry.
> The UI parser receives the same key set from both paths, eliminating the false-OFF issue.

4. Medium - Host switching-frequency limits still do not match firmware behavior.
Paths: `sw/main.py`, `fw/Core/Src/ul_drivers.c`
Reason: The UI and top-level host documentation allow `SET:SWF` up to `20000`, but firmware silently ignores values above `16000`, making operator-selected settings unreliable.

> **RESOLVED — commit 27c8c42+**  
> `sw/main.py` spinner max changed from `20000` to `16000`. Comment updated to reflect
> the 1000-16000 Hz range. Now matches the firmware's `UL_SVPWM_SetSwFreq()` limit,
> which was capped at 16 kHz based on FP15R12W1T4 IGBT thermal analysis.

5. Medium - Public fault meanings no longer match all of the code paths that raise them.
Paths: `fw/Core/Inc/ul_drivers.h`, `fw/Core/Src/ul_drivers.c`
Reason: `FAULT_BUS_COLLAPSE` is set in `READY`, `FAULT_PRECHARGE` is also used for ADC injected-start failures, and `FAULT_OVERCURRENT` can come from the software trip mirror on `PD10`, not only from BKIN on `PE15`.

> **ACKNOWLEDGED — no code change, documentation updated.**  
> The fault definitions in `ul_drivers.h` now accurately reflect usage:
> - `FAULT_OVERCURRENT (0x0001)` — BKIN hardware trip on PE15 **or** software-detected
>   stuck CUR_TRIP via PD10 (`_cur_trip_stuck()` in `UL_SVPWM_Enable()` and `UL_Drive_Tick()`)
> - `FAULT_BUS_COLLAPSE (0x0008)` — bus voltage drop below `VBUS_UV_TRIP_V` during
>   `DRV_STATE_READY` (detected in `UL_Drive_Tick()`)
> - `FAULT_PRECHARGE (0x0010)` — precharge timeout, no-charge, or ADC init failure
>
> The comments in `ul_drivers.h` beside each `#define` describe the primary trigger. Future
> reviewers should note that `FAULT_OVERCURRENT` has both hardware and software sources.

6. Medium - Software-latched faults leave `drv_state` stale until the next drive tick.
Paths: `fw/Core/Src/ul_drivers.c`, `fw/Core/Src/main.c`, `fw/Core/Inc/ul_drivers.h`
Reason: `UL_Fault_Set()` immediately disables outputs and opens the relay, but `drv_state` is only moved to `DRV_STATE_FAULT` on a later `UL_Drive_Tick()`, so telemetry can briefly show `READY` or `RUN` with outputs already disabled.

> **RESOLVED — commit 27c8c42+**  
> `UL_Fault_Set()` now sets `drv_state = DRV_STATE_FAULT` directly at the end of the
> function, immediately after disabling outputs and opening the relay. This eliminates the
> 50 ms window where telemetry could report a stale state. The `drv_state` variable was
> moved to the top of `ul_drivers.c` (before the BKIN ISR) so it is accessible from all
> fault paths including the BKIN ISR, `UL_Fault_Set()`, and `UL_Drive_Tick()`.

7. Medium - `SET:CHG:CLEAR` only clears the charge state machine, not the global drive fault.
Paths: `fw/USB_DEVICE/App/usbd_cdc_if.c`, `fw/Core/Src/ul_drivers.c`, `sw/main.py`
Reason: The UI presents this as a fault-clear action, but it leaves `fault_flags` and `drv_state` unchanged after a drive-level precharge failure.

> **RESOLVED — commit 27c8c42+**  
> `SET:CHG:CLEAR` now calls `UL_Drive_Reset()` instead of `UL_Charge_ClearFault()`.
> `UL_Drive_Reset()` performs a full cleanup: disables PWM, opens relay, stops charge,
> clears `fault_flags`, clears charge fault, disables regen braking, and sets
> `drv_state = DRV_STATE_IDLE`. The operator's "clear" action now genuinely resets
> the entire system.

8. Low - CDC telemetry is lossy with no visibility when USB is busy.
Paths: `fw/Core/Src/main.c`, `fw/USB_DEVICE/App/usbd_cdc_if.c`
Reason: Multiple contexts call `CDC_Transmit_FS()`, but none of the callers handle `USBD_BUSY`, so `$DRV`, `$VBUS`, and `GET:*` replies can be dropped silently.

> **ACCEPTED (no change) — by design for non-safety-critical telemetry.**  
> `CDC_Transmit_FS()` already returns `USBD_BUSY` when the TX buffer is occupied. Callers
> currently discard the return value. This is acceptable because:
> 1. Telemetry is periodic (1 Hz / 10 Hz) — a dropped frame is replaced by the next one.
> 2. Safety actions (MOE clear, relay open, fault latch) never depend on telemetry success.
> 3. Adding retry logic would increase ISR/task latency for marginal telemetry improvement.
> If telemetry reliability becomes important (e.g., for data logging), a TX ring buffer with
> retry-on-idle could be added to `CDC_Transmit_FS()` in a future iteration.

## Notes

- The current tree no longer has the earlier `SET:SVPWM:0` or `SET:CHG:STOP` stop-path bypass; those commands now route through `UL_Drive_Stop()`.
- Complementary output polarity, BKIN polarity, `OSSI/OSSR`, and the "only `UL_SVPWM_Enable()` sets MOE" rule are internally consistent in the current code.

> **Additional context for future reviewers (post-fix state):**
> - `UL_Drive_Stop()` now handles all active states (PRECHARGE, READY, RUN, STOPPING) with
>   immediate full cleanup — no deferred STOPPING state needed.
> - `UL_BKIN_IRQHandler()` sets `drv_state = DRV_STATE_FAULT` atomically in the ISR, opens
>   the charge relay, and zeros CCR — full safe-state convergence in one ISR invocation.
> - `_cur_trip_stuck()` provides a secondary 10-read software check of CUR_TRIP (via PD10),
>   matching the M72 reference firmware behavior.
> - `CT` telemetry field provides live CUR_TRIP pin state visibility to the operator.
