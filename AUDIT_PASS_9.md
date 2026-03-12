# AUDIT PASS 9 — UI + Config

## Focus
Configuration parsing, UI input handling, command-line argument processing, information disclosure through UI.

## Findings

### P9-001 — Config values truncated to int on 32-bit (LOW)
- **File:** `src/app/config.c:45-55`
- **Description:** `wcstol` returns `long`, cast to `int`. On 32-bit platforms where `long` exceeds `int` range, values could wrap. However, all values are clamped immediately after (lines 59-67), limiting impact.
- **Status:** Low risk due to clamping.

### P9-002 — ScanDriveThreadArgs root buffer too small for non-drive paths (LOW)
- **File:** `src/app/anything.c:200, 2930`
- **Description:** `ScanDriveThreadArgs.root` is 8 wchars, but `wcscpy_s(in->root, 8, args.rootPath)` is called with user-provided rootPath which could exceed 7 characters. `wcscpy_s` safely truncates but the path would be incomplete.
- **Impact:** The scanner would scan a truncated path. No crash, but unexpected behavior for non-drive root paths (e.g., `C:\Users\Documents\`).
- **Recommendation:** Increase root buffer to MAX_LONG_PATH or validate that rootPath is a drive root.

### P9-003 — Config file path not validated (LOW)
- **File:** `src/app/config.c:32-33`
- **Description:** `config_load_file` opens any path passed to it. If the path comes from command-line args, a symlink attack could cause the config parser to read an attacker-controlled file. Impact is limited since the parser only extracts integer values with clamping.
- **Status:** Low risk — integer-only parser with bounds clamping.

## Summary
| Severity | Count |
|----------|-------|
| Critical | 0 |
| High     | 0 |
| Medium   | 0 |
| Low      | 3 |
| Total    | 3 |

## Notes
- ImGui UI uses safe std::string-based InputText — no buffer overflow vectors.
- Search queries go through a bounds-checked tokenizer (tokenlist_push, already hardened in Pass 1).
- No system command execution from UI input.
- Config clamping is comprehensive and correct.
