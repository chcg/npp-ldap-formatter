What this change does: It checks a packaged plugin with the existing regression harness and records a privacy audit. It keeps failed preservation checks and untested native checks separate from passing tests.

Reviewed load: one developer or CI job, one approved local harness and package at a time. Inputs stay stable during execution; this tool is not a sandbox for untrusted executables.

**Must fix — fixed before integration**

1. **Evidence could name different artifacts** (`scripts/audit_package_regressions.py`)
   - **What this is:** The runner reports which package and revision it tested.
   - **Problem:** A well-formed revision label did not bind the ZIP, DLL, harness or required case count to the reviewed build. A reduced or different harness could produce plausible evidence.
   - **Fix:** Require a small reviewed manifest. Check revision, architecture and all three hashes before execution; check the case count afterwards and report the harness identity. Tests reject mismatches.
   - **If we skip it:** Reviewers can approve evidence from different binaries or incomplete coverage. The manifest remains an operator-reviewed record, not a signed build attestation.

2. **Argument errors repeated input values** (`scripts/audit_package_regressions.py`)
   - **What this is:** The command-line parser reports invalid arguments.
   - **Problem:** Its default error included supplied values before the ordinary error sanitizer ran.
   - **Fix:** A concrete parser error override emits only a fixed message. Tests check both output channels with sensitive markers.
   - **If we skip it:** An invalid argument can enter console logs even though the runner promises sanitized errors.

**Should fix — fixed before integration**

3. **Architecture dispatch had no positive tests** (`tests/test_audit_package_regressions.py`)
   - **What this is:** The runner permits matching x86 and ARM64 jobs on supported machines.
   - **Problem:** Success tests covered only x64. Removing another supported path would not fail those tests.
   - **Fix:** Add matching synthetic PE fixtures and successful mocked x86/AMD64 and ARM64/ARM64 calls.
   - **If we skip it:** A dispatch regression can silently reject supported audits. These tests do not certify native ARM64 execution.

4. **The source digest could not be reproduced** (`docs/package-privacy-audit.md`)
   - **What this is:** The audit identifies the reviewed product sources with one hash.
   - **Problem:** The report did not state the input set, order or line-ending normalization.
   - **Fix:** Document the exact algorithm and retain the source path/hash manifest in the consolidated evidence.
   - **If we skip it:** Another reviewer cannot establish what that hash covers.

5. **A harness could write to the error stream and still pass** (`scripts/host_evidence.py`)
   - **What this is:** The shared runner checks the harness report before certifying results.
   - **Problem:** It discarded stderr but accepted a valid stdout report with a non-empty error stream.
   - **Fix:** Reject any stderr without echoing it. The shared test and package-wrapper tests cover the case.
   - **If we skip it:** A harness that logs unexpected data can receive a passing result.

The existing shared runner has a documented limit: captured pipes have a time limit but no byte cap. This audit uses approved local binaries with known output; extending it to arbitrary or untrusted runners requires bounded in-memory capture. That pre-existing hardening item is recorded in the BMad deferred-work ledger. No output is redirected to temporary log files.

Checked the complete diff, connected callers, package validation, report schema, source data flow, real PE imports, tests and artifact hashes. There is no README in this workspace. Reused `validate_package_data` and `host_evidence.run`; no extra dependency, PE parser or product abstraction was added. CTest x64 passed 7/7, Python passed 95/95, and the approved x64 package ran 103 cases with no assertion failures. Six fault cases still record integrity loss.

Verdict: Ship.

This verdict applies to the audit changes. The plugin release remains blocked.

Not checked: system-wide runtime network/file tracing, native ARM64 execution, missing x86 ABI/E2E executables, and the remaining native preservation/callback gates.
