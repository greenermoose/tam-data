#!/bin/sh
set -eu

BIN="${1:-./build/tam-data}"
TMPDIR="$(mktemp -d "${TMPDIR:-/tmp}/td_smoke_XXXXXX")"
trap 'rm -rf "$TMPDIR"' EXIT

# 1. --version and --help
ver="$("$BIN" --version)"
case "$ver" in
    "tam-data 1.0.0"*) ;;
    *) echo "FAIL: unexpected --version output: $ver" >&2; exit 1 ;;
esac

help_out="$("$BIN" --help)"
for pat in "--json" "--jsonl" "--ini" "--fwf" "--ps" "Space / Ctrl+H" "S / gS / C / gC / O"; do
    case "$help_out" in
        *"$pat"*) ;;
        *) echo "FAIL: --help missing '$pat'" >&2; exit 1 ;;
    esac
done

# 2. Invalid CLI flag returns non-zero
if "$BIN" --no-such-flag >/dev/null 2>&1; then
    echo "FAIL: invalid flag should exit non-zero" >&2
    exit 1
fi

# 3. Prepare .vd replay scripts
cat >"$TMPDIR/noop.vd" <<'EOF'
sheet	col	row	longname	input	comment
		0	go-down		
EOF

cat >"$TMPDIR/transform.vd" <<EOF
sheet	col	row	longname	input	comment
		0	addcol-expr	val * 2	
		0	save-sheet	$TMPDIR/out.csv	
EOF

# 4. CSV file + batch expression transform + save
cat >"$TMPDIR/in.csv" <<'EOF'
name,val
alpha,10
beta,20
EOF

"$BIN" -p "$TMPDIR/transform.vd" -b "$TMPDIR/in.csv"
grep -q "alpha,20,10" "$TMPDIR/out.csv"
grep -q "beta,40,20" "$TMPDIR/out.csv"

# 5. Piped stdin CSV
printf 'k,v\nx,1\ny,2\n' | "$BIN" --csv -p "$TMPDIR/noop.vd" -b

# 6. Extended loaders (JSON, JSONL, INI, FWF, directory, --demo)
printf '[{"a":1,"b":"ok"},{"a":2,"b":"yes"}]\n' >"$TMPDIR/in.json"
printf '{"k":"p","n":1}\n{"k":"q","n":2}\n' >"$TMPDIR/in.jsonl"
printf '[sec]\nhost = localhost\nport = 8080\n' >"$TMPDIR/in.ini"
printf 'ID   NAME\n1    Ada\n2    Bob\n' >"$TMPDIR/in.fwf"

"$BIN" -p "$TMPDIR/noop.vd" -b "$TMPDIR/in.json"
"$BIN" -p "$TMPDIR/noop.vd" -b "$TMPDIR/in.jsonl"
"$BIN" -p "$TMPDIR/noop.vd" -b "$TMPDIR/in.ini"
"$BIN" -p "$TMPDIR/noop.vd" -b "$TMPDIR/in.fwf"
"$BIN" -p "$TMPDIR/noop.vd" -b "$TMPDIR"
"$BIN" --demo -p "$TMPDIR/noop.vd" -b

echo "ok: smoke tests passed"
