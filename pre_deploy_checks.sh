#!/usr/bin/env bash
# ================================================================
# pgclone pre-deploy checks
# Run this before tagging and pushing a new release.
# Usage: ./pre_deploy_checks.sh
# Compatible with macOS (BSD) and Linux (GNU) grep/sed.
# ================================================================

PASS=0
FAIL=0
WARN=0

pass() { ((PASS++)); echo "  ✅ $1"; }
fail() { ((FAIL++)); echo "  ❌ $1"; }
warn() { ((WARN++)); echo "  ⚠️  $1"; }

echo "=========================================="
echo " pgclone pre-deploy checks"
echo "=========================================="
echo ""

# ── 1. VERSION CONSISTENCY ──────────────────────────────────────
echo "── Version consistency ──"

V_CONTROL=$(grep "default_version" pgclone.control | sed "s/.*= *'//;s/'.*//")
echo "  pgclone.control: $V_CONTROL"

V_C=$(sed -n 's/.*cstring_to_text("pgclone \([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\)").*/\1/p' src/pgclone.c 2>/dev/null || echo "NOT_FOUND")
[[ -z "$V_C" ]] && V_C="NOT_FOUND"
echo "  src/pgclone.c:   $V_C"

V_META=$(python3 -c "import json; print(json.load(open('META.json'))['version'])" 2>/dev/null || echo "NOT_FOUND")
echo "  META.json:       $V_META"

V_META_PROV=$(python3 -c "import json; print(json.load(open('META.json'))['provides']['pgclone']['version'])" 2>/dev/null || echo "NOT_FOUND")

V_META_FILE=$(python3 -c "import json; print(json.load(open('META.json'))['provides']['pgclone']['file'])" 2>/dev/null || echo "NOT_FOUND")
EXPECTED_FILE="sql/pgclone--${V_CONTROL}.sql"

V_BADGE=$(sed -n 's/.*version-\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\).*/\1/p' README.md 2>/dev/null | head -1)
[[ -z "$V_BADGE" ]] && V_BADGE="NOT_FOUND"
echo "  README badge:    $V_BADGE"

V_TAG_LINK=$(sed -n 's/.*releases\/tag\/v\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\).*/\1/p' README.md 2>/dev/null | head -1)
[[ -z "$V_TAG_LINK" ]] && V_TAG_LINK="NOT_FOUND"

if [[ "$V_CONTROL" == "$V_C" && "$V_C" == "$V_META" && "$V_META" == "$V_META_PROV" && "$V_META" == "$V_BADGE" && "$V_BADGE" == "$V_TAG_LINK" ]]; then
    pass "All version strings match: $V_CONTROL"
else
    [[ "$V_CONTROL" != "$V_C" ]] && fail "pgclone.control ($V_CONTROL) != pgclone.c ($V_C)"
    [[ "$V_CONTROL" != "$V_META" ]] && fail "pgclone.control ($V_CONTROL) != META.json version ($V_META)"
    [[ "$V_META" != "$V_META_PROV" ]] && fail "META.json version ($V_META) != provides.version ($V_META_PROV)"
    [[ "$V_CONTROL" != "$V_BADGE" ]] && fail "pgclone.control ($V_CONTROL) != README badge ($V_BADGE)"
    [[ "$V_BADGE" != "$V_TAG_LINK" ]] && fail "README badge version ($V_BADGE) != tag link ($V_TAG_LINK)"
fi

if [[ "$V_META_FILE" == "$EXPECTED_FILE" ]]; then
    pass "META.json provides.file correct: $V_META_FILE"
else
    fail "META.json provides.file is '$V_META_FILE', expected '$EXPECTED_FILE'"
fi

if [[ -f "$EXPECTED_FILE" ]]; then
    pass "SQL file exists: $EXPECTED_FILE"
else
    fail "SQL file missing: $EXPECTED_FILE"
fi

echo ""

# ── 2. C CODE SAFETY ────────────────────────────────────────────
echo "── C code safety ──"

MAGIC_COUNT=$(grep -r "PG_MODULE_MAGIC" src/*.c 2>/dev/null | grep -vc "^Binary" || true)
if [[ "$MAGIC_COUNT" -eq 1 ]]; then
    pass "PG_MODULE_MAGIC: exactly 1 instance"
else
    fail "PG_MODULE_MAGIC: found $MAGIC_COUNT (expected 1)"
fi

INIT_DEFS=$(grep -r "^_PG_init" src/*.c 2>/dev/null | wc -l | tr -d ' ')
if [[ "$INIT_DEFS" -eq 1 ]]; then
    pass "_PG_init definition: exactly 1"
else
    fail "_PG_init definitions: found $INIT_DEFS (expected 1)"
fi

if grep -rq "d\.adsrc" src/*.c src/*.h 2>/dev/null; then
    fail "Deprecated d.adsrc found — use pg_get_expr() instead"
else
    pass "No deprecated d.adsrc usage"
fi

MALLOC_HITS=$(grep -rn 'malloc\|calloc\|realloc' src/*.c 2>/dev/null | grep -v 'palloc\|pfree\|PQfreemem\|//\|/\*' || true)
if [[ -z "$MALLOC_HITS" ]]; then
    pass "No raw malloc/calloc/realloc usage (palloc only)"
else
    MALLOC_COUNT=$(echo "$MALLOC_HITS" | wc -l | tr -d ' ')
    warn "Found $MALLOC_COUNT possible raw malloc/calloc/realloc calls:"
    echo "$MALLOC_HITS" | head -5 | sed 's/^/    /'
fi

FREE_HITS=$(grep -rn '[^p]free(' src/*.c 2>/dev/null | grep -v 'pfree\|PQfreemem\|//\|/\*\| \* \|".*free' || true)
if [[ -z "$FREE_HITS" ]]; then
    pass "No raw free() usage (pfree only)"
else
    FREE_COUNT=$(echo "$FREE_HITS" | wc -l | tr -d ' ')
    warn "Found $FREE_COUNT possible raw free() calls:"
    echo "$FREE_HITS" | head -5 | sed 's/^/    /'
fi

SHADOW_HITS=$(grep -rn 'char[[:space:]]*\*[[:space:]]*errmsg[[:space:]]*[;=]' src/*.c 2>/dev/null | grep -v 'ereport\|elog\|errmsg_internal\|/\*\|//' || true)
if [[ -z "$SHADOW_HITS" ]]; then
    pass "No errmsg variable shadowing"
else
    warn "Possible 'errmsg' variable shadowing PostgreSQL macro:"
    echo "$SHADOW_HITS" | head -5 | sed 's/^/    /'
fi

echo ""

# ── 3. RESOURCE CLEANUP ─────────────────────────────────────────
echo "── Resource cleanup ──"

CONNECT_COUNT=$(grep -c 'PQconnectdb' src/*.c 2>/dev/null | awk -F: '{s+=$NF}END{print s+0}' || echo 0)
FINISH_COUNT=$(grep -c 'PQfinish' src/*.c 2>/dev/null | awk -F: '{s+=$NF}END{print s+0}' || echo 0)
if [[ "$FINISH_COUNT" -ge "$CONNECT_COUNT" ]]; then
    pass "PQconnectdb ($CONNECT_COUNT) / PQfinish ($FINISH_COUNT) balanced"
else
    fail "PQconnectdb ($CONNECT_COUNT) > PQfinish ($FINISH_COUNT) — possible connection leak"
fi

EXEC_COUNT=$(grep -c 'PQexec\|PQexecParams' src/*.c 2>/dev/null | awk -F: '{s+=$NF}END{print s+0}' || echo 0)
CLEAR_COUNT=$(grep -c 'PQclear' src/*.c 2>/dev/null | awk -F: '{s+=$NF}END{print s+0}' || echo 0)
if [[ "$CLEAR_COUNT" -ge "$EXEC_COUNT" ]]; then
    pass "PQexec ($EXEC_COUNT) / PQclear ($CLEAR_COUNT) balanced"
else
    warn "PQexec ($EXEC_COUNT) > PQclear ($CLEAR_COUNT) — check error paths for missing PQclear"
fi

UNSAFE_STR=$(grep -rn 'strcpy\|strncpy' src/*.c src/*.h 2>/dev/null | grep -v strlcpy || true)
if [[ -z "$UNSAFE_STR" ]]; then
    pass "No unsafe strcpy/strncpy usage"
else
    STR_COUNT=$(echo "$UNSAFE_STR" | wc -l | tr -d ' ')
    warn "Found $STR_COUNT strcpy/strncpy — prefer strlcpy:"
    echo "$UNSAFE_STR" | head -5 | sed 's/^/    /'
fi

echo ""

# ── 4. SQL CONSISTENCY ───────────────────────────────────────────
echo "── SQL consistency ──"

LATEST_SQL="sql/pgclone--${V_CONTROL}.sql"
if [[ -f "$LATEST_SQL" ]]; then
    # Extract function names from PG_FUNCTION_INFO_V1(name) — portable
    C_FUNCS=$(sed -n 's/.*PG_FUNCTION_INFO_V1(\([a-z_]*\)).*/\1/p' src/pgclone.c src/pgclone_bgw.c 2>/dev/null || true)
    MISSING=0
    FUNC_COUNT=0
    for func in $C_FUNCS; do
        ((FUNC_COUNT++))
        if ! grep -q "'${func}'" "$LATEST_SQL" 2>/dev/null; then
            fail "C function '$func' has no SQL wrapper in $LATEST_SQL"
            ((MISSING++))
        fi
    done
    if [[ "$MISSING" -eq 0 ]]; then
        pass "All C functions ($FUNC_COUNT) have SQL wrappers"
    fi
else
    fail "Cannot check SQL consistency — $LATEST_SQL not found"
fi

echo ""

# ── 5. BUILD & CONFIG FILES ─────────────────────────────────────
echo "── Build & config files ──"

if python3 -c "import json; json.load(open('META.json'))" 2>/dev/null; then
    pass "META.json is valid JSON"
else
    fail "META.json is invalid JSON"
fi

# Tab check — portable (no grep -P)
TAB_FILES=$(find .github -name "*.yml" 2>/dev/null | while read -r f; do
    if grep -q "	" "$f" 2>/dev/null; then echo "$f"; fi
done || true)
if [[ -z "$TAB_FILES" ]]; then
    pass "No tab characters in YAML files"
else
    fail "Tab characters in YAML (GitHub Actions will break):"
    echo "$TAB_FILES" | sed 's/^/    /'
fi

STALE_FILES=$(ls -1 pgclone-*.zip *.save 2>/dev/null || true)
if [[ -z "$STALE_FILES" ]]; then
    pass "No stale artifacts in repo root"
else
    fail "Stale files in repo root: $STALE_FILES"
fi

if grep -q 'wildcard.*sql/pgclone' Makefile 2>/dev/null; then
    pass "Makefile uses wildcard for SQL files"
else
    warn "Makefile may not pick up new SQL files automatically"
fi

echo ""

# ── 6. TEST FILES ────────────────────────────────────────────────
echo "── Test files ──"

if [[ -f test/pgclone_test.sql ]]; then
    PLAN=$(sed -n 's/.*SELECT plan(\([0-9]*\)).*/\1/p' test/pgclone_test.sql 2>/dev/null | head -1)
    [[ -z "$PLAN" ]] && PLAN=0
    TEST_COUNT=$(grep -cE "^[[:space:]]*SELECT[[:space:]]+(ok|is|isnt|matches|doesnt_match|has_function|has_extension|has_table|has_schema|has_index|has_view|has_column|has_trigger|col_is_pk|col_is_fk|fk_ok|results_eq|lives_ok|throws_ok|cmp_ok|pass)[[:space:]]*\(" test/pgclone_test.sql 2>/dev/null || echo "0")
    if [[ "$PLAN" -eq "$TEST_COUNT" ]]; then
        pass "pgTAP plan($PLAN) matches test count ($TEST_COUNT)"
    else
        warn "pgTAP plan($PLAN) but found $TEST_COUNT assertions — verify manually"
    fi
else
    warn "test/pgclone_test.sql not found"
fi

for f in test/run_tests.sh test/test_async.sh test/test_database_create.sh; do
    if [[ -f "$f" ]]; then
        if [[ -x "$f" ]]; then
            pass "$f is executable"
        else
            fail "$f is not executable"
        fi
    fi
done

echo ""

# ── 7. SECURITY ──────────────────────────────────────────────────
echo "── Security ──"

CONN_LOG=$(grep -rn 'elog.*(LOG\|elog.*(WARNING\|elog.*(ERROR\|elog.*(FATAL' src/*.c 2>/dev/null | grep -i 'conninfo\|password\|connstr' || true)
if [[ -z "$CONN_LOG" ]]; then
    pass "No connection strings logged above DEBUG1"
else
    fail "Possible connection string logged at LOG+ level:"
    echo "$CONN_LOG" | head -5 | sed 's/^/    /'
fi

if grep -rq 'quote_literal_cstr\|quote_identifier' src/*.c 2>/dev/null; then
    pass "SQL injection protection present (quote_literal_cstr/quote_identifier)"
else
    warn "No quote_literal_cstr/quote_identifier found"
fi

echo ""

# ── SUMMARY ──────────────────────────────────────────────────────
echo "=========================================="
echo " Results: ✅ $PASS passed, ❌ $FAIL failed, ⚠️  $WARN warnings"
echo "=========================================="

if [[ "$FAIL" -gt 0 ]]; then
    echo ""
    echo " ⛔ DO NOT DEPLOY — fix failures first"
    exit 1
elif [[ "$WARN" -gt 0 ]]; then
    echo ""
    echo " ⚠️  Review warnings before deploying"
    exit 0
else
    echo ""
    echo " 🚀 All checks passed — ready to deploy"
    exit 0
fi
