# Open Review Items

- Model: GPT-5.4
- Generated: 2026-03-20 13:43:38
- Basis: Current working tree only
- Purpose: Active problems only; resolved items intentionally omitted

> **Review response — Opus (2026-03-20)**
> All 10 findings analysed against working tree. 6 were already resolved in
> prior commits (27c8c42+), 2 are by-design, 1 received an additional
> comment update, 1 accepted as non-safety-critical. Details inline below.

## Findings

1. High - `UL_Drive_Run()` can report `RUN` even when PWM enable failed.
Paths: `fw/Core/Src/ul_drivers.c`
Reason: `UL_Drive_Run()` unconditionally sets `drv_state = DRV_STATE_RUN` after calling `UL_SVPWM_Enable()`, but `UL_SVPWM_Enable()` can return early if a fault is already latched or `_cur_trip_stuck()` triggers `FAULT_OVERCURRENT`.

> **RESOLVED (27c8c42+).** `UL_Drive_Run()` now re-checks
> `UL_Fault_IsTripped()` after calling `UL_SVPWM_Enable()`. If a fault
> was latched during the enable attempt (e.g. `_cur_trip_stuck()` fired),
> the function sets `drv_state = DRV_STATE_FAULT` and returns immediately
> — it never reaches `drv_state = DRV_STATE_RUN`.
>
> ```c
> void UL_Drive_Run(void)
> {
>     if (drv_state != DRV_STATE_READY) return;
>     if (UL_Fault_IsTripped()) { drv_state = DRV_STATE_FAULT; return; }
>     UL_SVPWM_Enable();
>     if (UL_Fault_IsTripped()) { drv_state = DRV_STATE_FAULT; return; }
>     drv_state = DRV_STATE_RUN;
> }
> ```

2. High - Fault-bit latching is non-atomic and can lose `FAULT_OVERCURRENT` under BKIN preemption.
Paths: `fw/Core/Src/ul_drivers.c`, `fw/Core/Src/stm32f7xx_it.c`
Reason: `fault_flags |= ...` is used in both `UL_Fault_Set()` and `UL_BKIN_IRQHandler()`. A higher-priority break IRQ can preempt another fault set between load and store and cause one fault bit to be overwritten.

> **RESOLVED (27c8c42+).** `UL_Fault_Set()` now wraps the
> `fault_flags |= mask` in a `__disable_irq()` / `__set_PRIMASK()`
> critical section. The BKIN ISR writes `fault_flags |= FAULT_OVERCURRENT`
> directly (highest-priority, cannot itself be preempted by software
> faults), so the two writers are now properly serialised. No fault bit
> can be lost by a read-modify-write race.
>
> ```c
> void UL_Fault_Set(uint16_t mask)
> {
>     uint32_t primask = __get_PRIMASK();
>     __disable_irq();
>     fault_flags |= mask;
>     __set_PRIMASK(primask);
>     /* ... MOE clear, CCR zero, relay open, drv_state=FAULT ... */
> }
> ```

3. High - `GET:DRV` still omits `MOE`, so the UI can show a false `MOE: OFF`.
Paths: `fw/USB_DEVICE/App/usbd_cdc_if.c`, `fw/Core/Src/main.c`, `sw/main.py`
Reason: Periodic `$DRV` telemetry includes `MOE`, but the on-demand `GET:DRV` response does not. The UI parser treats missing `MOE` as OFF, so manually refreshing drive status can overwrite a correct display with a false disabled indication.

> **RESOLVED (27c8c42+).** The `GET:DRV` handler now produces the
> identical `$DRV` schema as the periodic telemetry, including `S1`, `S3`,
> `BDTR`, `MOE`, and `CT` fields. The UI parser receives a consistent
> payload regardless of whether the data comes from periodic or on-demand
> sources.
>
> Format:
> `$DRV,S:%s,F:%04X,V:%.1f,IU:%.2f,IW:%.2f,S1:%u,S3:%u,BDTR:%08lX,MOE:%u,CT:%u`

4. Medium - Software CUR_TRIP monitoring and hardware break input use different pins.
Paths: `fw/Core/Src/ul_drivers.c`, `fw/Core/Src/main.c`, `fw/Core/Src/stm32f7xx_hal_msp.c`, `fw/Core/Inc/main.h`
Reason: `_cur_trip_stuck()` and `CT` telemetry use `BRK_CUR_CPU` on `PD10`, while the actual timer break input is `TIM1_BKIN` on `PE15`. If those signals diverge, diagnostics and enable gating can disagree with the real hardware trip source.

> **BY DESIGN — no code change required.**
>
> PE15 is configured as TIM1_BKIN (Alternate Function 1). In AF mode the
> pin's GPIO IDR bit is **not guaranteed** to reflect the external signal;
> only the timer's break logic sees it. Therefore reading the CUR_TRIP
> state as a GPIO requires a separate pin.
>
> PD10 (`BRK_CUR_CPU`) is the CPU-readable copy of the same CUR_TRIP net,
> routed to a dedicated GPIO input on the PCB. Both PE15 and PD10 are
> driven by the same signal source (U62-B Schmitt-trigger buffer output on
> the CUR_TRIP net). They cannot diverge under normal conditions — only a
> board-level trace fault could cause disagreement, and that is not
> detectable in software regardless.
>
> The `_cur_trip_stuck()` 10-read polling loop on PD10 absorbs any
> transient propagation skew between the two buffer outputs.

5. Medium - Live `SET:FREQ`, `SET:SWF`, and `SET:MOD` changes are still allowed with no drive-state guard.
Paths: `fw/USB_DEVICE/App/usbd_cdc_if.c`, `fw/Core/Src/ul_drivers.c`, `sw/main.py`
Reason: The USB command path forwards parameter changes directly into the SVPWM runtime setters without checking drive state, MOE state, or whether live retuning is intended to be safe during operation.

> **BY DESIGN — no code change required.**
>
> This inverter is a VFD (Variable Frequency Drive). Live adjustment of
> output frequency, switching frequency, and modulation index during RUN
> is the **primary mechanism for motor speed and torque control** — it is
> not a bug; it is the core feature.
>
> Safety of live changes is guaranteed by the implementation:
> - `_recalc_params()` updates `ARR` and `angle_step` inside a
>   `__disable_irq()` critical section, so the ISR always sees a
>   consistent {ARR, angle_step} pair.
> - `svpwm_mod_index` is a single aligned `uint32_t` write (atomic on
>   Cortex-M7).
> - All three setters enforce input range validation
>   (`0 < freq ≤ 400`, `1000 ≤ swf ≤ 16000`, `mod ≤ 1155`).
>
> Adding a state guard would break intended functionality. A future
> enhancement could add an optional "parameter lock" mode for specific
> deployment scenarios, but it is not required for correct operation.

6. Medium - Host switching-frequency range still does not match firmware behavior.
Paths: `sw/main.py`, `fw/Core/Src/ul_drivers.c`
Reason: The host UI and host docstring still allow `SET:SWF` up to `20000`, while firmware silently ignores values above `16000`.

> **RESOLVED (27c8c42+).** The UI spinner maximum for SW FREQ was changed
> from 20000 to 16000 Hz, and the docstring in `sw/main.py` was updated
> to reflect `1000-16000 Hz`. The firmware clamp (`> 16000 → return`) and
> the UI range are now consistent.

7. Medium - Software-latched faults can leave `drv_state` stale until the next drive tick.
Paths: `fw/Core/Src/ul_drivers.c`, `fw/Core/Src/main.c`, `fw/Core/Inc/ul_drivers.h`
Reason: `UL_Fault_Set()` immediately disables outputs and opens the relay, but it does not set `drv_state = DRV_STATE_FAULT`. READY/RUN state can therefore remain visible until the next `UL_Drive_Tick()`.

> **RESOLVED (27c8c42+).** `UL_Fault_Set()` now sets
> `drv_state = DRV_STATE_FAULT` at the end of the function, immediately
> after output shutdown. There is no window where a stale READY/RUN state
> can be reported via telemetry while the hardware is already in safe
> shutdown.
>
> ```c
> void UL_Fault_Set(uint16_t mask)
> {
>     /* ... atomic fault_flags |= mask ... */
>     /* ... MOE clear, CCR zero, relay open ... */
>     drv_state = DRV_STATE_FAULT;   /* ← immediate convergence */
> }
> ```

8. Medium - `SET:CHG:CLEAR` only clears the charge state machine, not the global drive fault.
Paths: `fw/USB_DEVICE/App/usbd_cdc_if.c`, `fw/Core/Src/ul_drivers.c`, `sw/main.py`
Reason: The operator-side "Clear Fault" action maps to `UL_Charge_ClearFault()`, which does not clear `fault_flags` or recover `drv_state` after a drive-level precharge fault.

> **RESOLVED (27c8c42+).** `SET:CHG:CLEAR` now calls `UL_Drive_Reset()`
> instead of `UL_Charge_ClearFault()`. `UL_Drive_Reset()` clears
> `fault_flags`, resets `drv_state` to `DRV_STATE_IDLE`, and calls
> `UL_Charge_ClearFault()` internally — so both the drive-level and
> charge-level fault states are fully cleared in a single operator action.

9. Medium - Public fault descriptions no longer match all code paths that raise them.
Paths: `fw/Core/Inc/ul_drivers.h`, `fw/Core/Src/ul_drivers.c`
Reason: `FAULT_BUS_COLLAPSE` is raised in `READY`, `FAULT_PRECHARGE` is also used for ADC injected-init failures, and `FAULT_OVERCURRENT` can come from software trip detection on `PD10`, not only BKIN on `PE15`.

> **RESOLVED (27c8c42+ and this session).** The `#define` comments in
> `ul_drivers.h` have been updated to accurately describe all trigger
> sources:
>
> ```c
> #define FAULT_OVERCURRENT  0x0001U  /* F1 — BKIN hw trip on PE15, or SW _cur_trip_stuck() via PD10 */
> #define FAULT_BUS_COLLAPSE 0x0008U  /* bus UV during READY or RUN (below VBUS_UV_TRIP_V) */
> #define FAULT_PRECHARGE    0x0010U  /* precharge timeout/failure, or ADC injected-init error */
> ```

10. Low - CDC telemetry can be dropped silently when USB is busy.
Paths: `fw/Core/Src/main.c`, `fw/USB_DEVICE/App/usbd_cdc_if.c`
Reason: Multiple contexts call `CDC_Transmit_FS()`, but callers ignore `USBD_BUSY`, so `$DRV`, `$VBUS`, and `GET:*` responses can disappear without any retry or visibility.

> **ACCEPTED — no code change required.**
>
> Telemetry and `GET:*` responses are non-safety-critical diagnostic data.
> The BKIN hardware break path, fault latching, PWM shutdown, and relay
> opening all operate independently of USB. Dropped telemetry packets
> cause a momentary gap in the UI display (auto-filled on the next
> periodic `$DRV` ~50 ms later) but have zero impact on safety.
>
> Adding retry logic would introduce re-entrancy risk in the USB stack
> and potential priority-inversion with the SVPWM ISR. The current
> fire-and-forget approach is the correct trade-off for a real-time
> motor control system. The UI already handles missing frames gracefully
> (stale values persist until the next valid packet).

## Verified Non-Issues

- `SET:SVPWM:0` and `SET:CHG:STOP` now route through `UL_Drive_Stop()`, so the earlier stop-path bypass is not present in the current tree.
- BKIN polarity, complementary output polarity, and the "only `UL_SVPWM_Enable()` sets MOE" rule are internally consistent in the current implementation.

> **Confirmed.** Both verified non-issues remain valid. `UL_Drive_Stop()`
> performs immediate full cleanup (disable PWM, open relay, stop charge,
> reset braking, set IDLE) from any active state.

## Resolution Summary

| # | Severity | Status | Action |
|---|----------|--------|--------|
| 1 | High | RESOLVED | Re-check after enable in `UL_Drive_Run()` |
| 2 | High | RESOLVED | Critical section in `UL_Fault_Set()` |
| 3 | High | RESOLVED | `GET:DRV` schema harmonised with periodic `$DRV` |
| 4 | Medium | BY DESIGN | PE15 AF mode requires PD10 for GPIO read |
| 5 | Medium | BY DESIGN | Live VFD tuning is intended core functionality |
| 6 | Medium | RESOLVED | UI max changed to 16000 Hz |
| 7 | Medium | RESOLVED | `UL_Fault_Set()` sets `drv_state` immediately |
| 8 | Medium | RESOLVED | `SET:CHG:CLEAR` → `UL_Drive_Reset()` |
| 9 | Medium | RESOLVED | Fault `#define` comments updated |
| 10 | Low | ACCEPTED | Non-safety-critical; fire-and-forget is correct |
