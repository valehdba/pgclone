<!--
Thanks for contributing to pgclone! Please fill in the sections below.
Delete sections that don't apply (e.g. "Database/SQL changes" for a docs-only PR).
-->

## Summary

<!-- One or two sentences: what does this PR do and why? -->

Closes #<!-- issue number, or remove this line -->

## Type of change

<!-- Check all that apply -->

- [ ] 🐛 Bug fix (non-breaking change that fixes an issue)
- [ ] ✨ New feature (non-breaking change that adds functionality)
- [ ] 💥 Breaking change (fix or feature that changes existing behavior)
- [ ] 🏗️ Refactor / internal cleanup (no functional change)
- [ ] ⚡ Performance improvement
- [ ] 📖 Documentation only
- [ ] 🔧 Build / CI / tooling
- [ ] 🧪 Test-only change

## Description of changes

<!--
Walk through what changed and why. For C changes, briefly note:
 - which files in src/ were touched
 - any new memory contexts, locks, or shared-memory state
 - any new libpq connections (and where they're freed)
-->

## Database / SQL changes

<!-- Delete this section if the PR doesn't add or modify SQL functions -->

- [ ] New or modified SQL functions are declared in `sql/pgclone--<version>.sql`
- [ ] `PG_FUNCTION_INFO_V1` names in C match the `AS '<lib>', '<sym>'` clauses in SQL
- [ ] Return types in `.sql` match the C `PG_RETURN_*` paths
- [ ] `COMMENT ON FUNCTION ...` added for new public functions
- [ ] Function volatility (`VOLATILE` / `STABLE` / `IMMUTABLE`) and `PARALLEL` safety set correctly

## Testing

<!-- Describe how this was tested. At minimum, say which PG versions you ran the suite against. -->

**PostgreSQL versions tested locally:**
<!-- e.g. "14, 17, 18" or "all of 14–18" -->

- [ ] pgTAP tests added or updated in `test/pgclone_test.sql`
- [ ] `plan()` count exactly matches the actual number of assertions
- [ ] Shell tests added/updated in `test/test_async.sh` or `test/test_database_create.sh` (if applicable)
- [ ] `test/fixtures/seed.sql` updated if new test objects are needed
- [ ] CI is green on **all** of PG 14, 15, 16, 17, 18

```sh
# Output of pre_deploy_checks.sh (paste the summary line)
# e.g. "22 passed, 0 failed"
```

## C code safety checklist

<!-- Skip the items that aren't applicable to this PR -->

- [ ] `palloc` / `palloc0` / `pfree` only — no `malloc`/`free`
- [ ] Every `PQconnectdb` has a matching `PQfinish` in **all** paths (success and error)
- [ ] Every `PQexec` result is `PQclear`-ed in all paths
- [ ] Dynamic SQL uses `quote_literal_cstr()` and `quote_identifier()`
- [ ] `StringInfo` used for dynamic strings; no fixed stack buffers for SQL
- [ ] `strlcpy` instead of `strcpy` / `strncpy`
- [ ] `ereport` / `elog` with appropriate level — connection strings never logged at `LOG` level or above
- [ ] PG-version-specific APIs are guarded with `#if PG_VERSION_NUM >= XXXXXX`
- [ ] No new uses of removed APIs (`d.adsrc`, pre-PG15 shmem-request pattern, etc.)
- [ ] Shared-memory state protected by `LWLock` where needed

## Documentation

- [ ] `CHANGELOG.md` updated under the appropriate version heading
- [ ] Relevant doc updated: `docs/USAGE.md` / `docs/ASYNC.md` / `docs/ARCHITECTURE.md` / `docs/TESTING.md`
- [ ] `README.md` updated if the change affects the user-facing feature list or quick-start

## Version bump

<!-- Delete this section if this PR doesn't bump the version -->

- [ ] `pgclone.control` (`default_version`)
- [ ] `META.json` (`version` and `provides.pgclone.version` + `file`)
- [ ] `README.md` version badge
- [ ] New `sql/pgclone--<old>--<new>.sql` migration script (if applicable)
- [ ] `CHANGELOG.md` has a heading for the new version

## Backward compatibility

<!--
If this is a breaking change, describe:
 - what breaks (signature change, removed function, behavioral change)
 - the migration path for existing users
 - whether a deprecation warning was considered first
Otherwise, write "No breaking changes."
-->

## Screenshots / sample output

<!--
Optional. For features with visible output (progress bar, pgclone_verify table,
masking report), paste a sample. Redact any real connection strings.
-->

```
```

## Additional notes

<!-- Anything reviewers should know: design tradeoffs, follow-up work, related PRs. -->
