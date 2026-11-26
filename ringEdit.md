# Implement the RFS history editor in lastDSMRlogger

Buildnumber: 16
Last update-date: 2026-11-26

Implement this feature in [lastDSMRlogger](https://github.com/mrWheel/lastDSMRlogger). Follow the repository's coding rules and use the existing SPA, HTTP server and store task. Deliver working firmware and SPA changes, plus the verification results requested below.
Always use the field styles used in other SPA fields!

**You can NEVER edit the last key/record!**

## Repository integration

- Register an **Edit** view in the System group of the SPA's `VIEWS` registry in `components/logger_web/web/app.js`. Add its section to `web/index.html` and style it using `web/style.css`. Reuse the existing radio-group styling and navigation behavior. The place is in [Systems].
- Put storage and transaction logic behind `components/logger_store`. Inspect `logger_store.c`, `include/logger_store.h`, the pinned `ring_file_store` component, and the current history and tariff code before choosing APIs.
- Map **Hours**, **Days**, and **Months** to the existing ring selectors. The current checkout uses `hours.rfs` (49 slots, YYMMDDHH), `days.rfs` (32 slots, YYMMDD), and `months.rfs` (25 slots, YYMM). Read capacities and key schemas from the existing store definitions. Handle any internal century digits and component-owned key metadata through the ring APIs. Preserve the existing record layout and backward compatibility.
- Expose a bounded, typed SPA API for an editor snapshot and an explicit save. Accept only supported ring selectors and editable fields. Apply the project's existing management access controls. The browser must never write an RFS file or supply a filesystem path.

## Editor behavior

- Show **Hours**, **Days**, and **Months** as mutually exclusive radio buttons. After pressing [Load] button load every populated logical record for the selected ring, newest first, including the oldest baseline and the newest logical record when it is staged in RAM. Use logical append order across ring wraparound, even when keys are corrupt. Provide empty, loading, and error states.
- Pressing [Cancel] button cancels the load (including a load still in flight), discards the
  not-yet-saved edits of the shown ring and returns the view to its empty screen; the .rfs is
  left exactly as it was.
- Opening the editor (clicking **Edit** in the System group) always shows an empty screen:
  no ring is read from the device until [Load] is pressed.
- Show the period key and all five cumulative meter readings as read-only values. These are electricity import T1/T2 (check this! I think there is only one import tariff), electricity export T1/T2 (check this. I think there is only 1 export tariff), and gas. Show a consumption input next to each reading, with kWh or m³ as appropriate. Include the stored tariff amounts described below. Keep the table usable on narrow screens.
- Show invalid values in red with a field-level explanation (tooltip). Use a subtle background tint for modified inputs, including tariff edits. Also mark automatically repaired keys and recalculated readings as pending. Invalid styling must remain visible on a modified cell. Reverting an edit should remove its highlight when the value matches the snapshot.
- Put **Save** below the table. Preserve typed drafts when changing rings or leaving the view and re-apply them when that ring is loaded again. Save applies to the selected ring. Show progress and a specific validation, conflict, or storage error when applicable. Keep the draft on failure and report the reason. After a successful save, clear the draft and the table and show the empty screen again, and refresh any affected history views.

## Calendar keys

Use the existing ring's period semantics and device timezone. Determine the expected key for each position by stepping backward one calendar period at a time from a trustworthy newest-period anchor. Resolve that anchor using the store's authoritative period information or other reliable evidence. Avoid using the browser clock or assuming the last record belongs to the current period, since the logger may have been offline. If the anchor or physical chronology is ambiguous, report it and block the repair until it can be resolved.

Compare original keys with the expected sequence. Show the original invalid value in red and preview the replacement. Repair gaps, duplicates, malformed keys, and out-of-order labels by relabeling the records in their existing logical order. Preserve the count and payload association. Calculate across month and year boundaries, leap days, and the device's daylight-saving transitions. If the hour schema cannot represent both occurrences of the repeated local hour, use the existing logger's collision policy. Verify that each proposed stored key is legal and unique for that schema before saving. Key repairs take effect only when Save succeeds.

## Consumption editing and validation

For a given meter channel, let A be a displayed row and B the immediately older row. The consumption attributed to A is `reading(A) - reading(B)`. An edit to A's consumption keeps its current draft reading and sets `reading(B) = reading(A) - enteredConsumption`. Recalculate the neighboring displayed differences from the resulting readings. A single consumption edit may change the implied consumption displayed on the next older row as a consequence, but should change only the immediately older cumulative reading. Preserve the other meter channels.

Example, newest to oldest: readings of 120, 115 and 110 show consumption of 5 and 5. Entering 7 on the first row produces readings of 120, 113 and 110 and displays consumption of 7 and 3.

Keep explicit consumption edits separate from implied differences. For multiple edits to the same channel, rebuild the draft from the snapshot and apply explicit edits newest to oldest. This makes the result independent of input order and preserves each explicitly requested difference where the combined draft is valid. When an explicit edit is removed, recompute from the original data and the remaining edits. The oldest row has no older baseline, so it displays its readings but has no editable consumption.

Enforce the requested **strictly descending** cumulative readings across every recorded channel and every adjacent row. Each reading must be lower than the next newer reading and higher than the next older reading where those neighbors exist. Equal readings therefore appear as invalid, including genuine zero-use periods and unused tariff registers. Explain that clearly in the UI and block Save while such violations remain. Never invent usage or silently adjust unrelated readings to satisfy this rule. Where the project explicitly treats a channel as unavailable, use that existing representation and show it as unavailable.

Validate the entire candidate on the server, including finite values, nonnegative cumulative readings, supported range and precision, and the adjacency rules after conversion to the actual persisted representation. Use the project's supported meter precision and consistent decimal handling on both sides. A meter reset or discontinuity must be visible as a validation issue. Leave unrepairable data untouched and explain what prevents saving.

## Tariff editing

The current `loggerMeterRecord` stores `tariffAmountImportEuro`, `tariffAmountExportEuro`, and `tariffAmountGasEuro` as period amounts in EUR. Expose all three as editable amounts. Label the export amount as a credit and use the existing storage and display sign conventions, including any supported negative amounts. These edits apply to the selected historical record. Global manual prices and dynamic tariff settings stay in their existing settings workflow.

Keep the stored `networkCostsEuro` visible as a separate historical figure. Keep its existing semantics. Validate amounts at the project's money precision and reject invalid numbers. Preserve entered tariff amounts when consumption changes. Do not silently reprice history using today's rates. Verify that the periodic tariff calculator does not immediately overwrite any successfully saved tariff edit. Where the current open period is still recalculated by the logger, use a durable override compatible with the existing layout or make those continuously recalculated fields explicitly read-only until the period closes. Report the implemented behavior.

## Integrity, concurrency, and recovery

- Take a consistent snapshot through the store owner, including the staged record. Use a revision or equivalent change token covering every record and baseline the edit depends on. On Save, serialize against background commits, rollovers, import/populate and other writers. Recheck the snapshot and reject stale edits with a reload message. Incorporate harmless live changes only when the server can prove the edited values and their dependencies are unaffected and revalidate the merged result. Keep normal logging active while the page is open.
- Treat Save as one transaction for the selected ring. A key repair and its payload changes must land together. Reuse an existing transaction mechanism only if the pinned component guarantees whole-edit atomicity. A loop of independently committed record changes is insufficient.
- If a replacement is required, build a complete candidate with the ring library, using the existing schema, capacity and logical order. Flush and close it, reopen and verify the complete logical contents, then publish it using a power-loss-safe protocol supported by the mounted filesystem. Verify actual LittleFS/VFS durability guarantees. If the existing primitives cannot safely publish a replacement in one operation, use a recoverable journal or equivalent with deterministic startup recovery and retain the last validated copy until the new one is durable. Ensure recovery runs before normal ring open or automatic rebuild/resizing.
- Coordinate active handles and staged data with publication. Keep readers on a complete coherent generation and reopen/verify the active handle before normal writes resume. On success, later periodic writes must preserve saved historical corrections, including a repair affecting the staged boundary. If a save fails or power is interrupted, recovery must yield either the complete old state or the complete new state. Never replace an unreadable ring with an empty one as part of the editor or its recovery.
- Validate every request server-side before publication. Reject malformed values, duplicate edit targets, invalid keys, oversized requests, and stale revisions. Handle insufficient space, sync/close failures, reopen errors, and retries. Report success only after durable publication is confirmed. A response lost after commit must be recoverable by reloading, without applying the same corrections twice.
- Preserve unrelated fields, ring metadata semantics, migration behavior and the other two rings. Editing one period granularity changes only the selected ring. Do not infer or propagate cross-ring corrections.

## Verification

Build the ESP-IDF project and verify the SPA through its normal navigation. Exercise all three ring selections, wrapped and partially filled rings, empty and one-record rings, and staged newest records. Verify red validation, pending highlights, save/reload, switching with unsaved edits, and that the oldest consumption cell is unavailable.

Use fixtures with malformed and duplicate keys, missing periods, calendar rollover, leap days, and both daylight-saving transitions. Test equal and reversed readings, invalid numbers, and adjacent simultaneous edits in both input orders. Verify the numeric example above and confirm that changing a historical tariff persists without altering global prices or unrelated records.

Exercise stale snapshots and concurrent rollover, insufficient free space, and failure or reboot at each meaningful commit phase. Compare the entire selected ring after recovery against the pre-save and expected post-save states. Confirm that other rings, ongoing telegram logging, existing history displays and exports still work. Record the tests actually run, any hardware checks still pending, and the storage guarantees the implementation relies on.
