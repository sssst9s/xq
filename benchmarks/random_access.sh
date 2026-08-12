#!/bin/bash
# SPDX-License-Identifier: Apache-2.0

set -u
IN=${1:?usage: random_access.sh <input-file>}
READS=${READS:-5000}
WORK=${WORK:-/tmp/xq-bench}

if [ -z "${XQ:-}" ]; then
    if   [ -x build/with-zstd/xq ]; then XQ=build/with-zstd/xq
    elif [ -x build/xq ];           then XQ=build/xq
    else echo "build first: make" >&2; exit 1; fi
fi
BENCH=$(dirname "$XQ")/bench/bench_random_access
[ -x "$BENCH" ] || { echo "build the benchmarks first: make bench" >&2; exit 1; }

if [ -z "${CODEC:-}" ]; then
    if "$XQ" compress -c zstd -o /dev/null /dev/null 2>/dev/null; then CODEC=zstd
    else CODEC=stored; fi
fi

to_bytes() {
    case "$1" in
        *K|*k) echo $(( ${1%[Kk]} * 1024 )) ;;
        *M|*m) echo $(( ${1%[Mm]} * 1024 * 1024 )) ;;
        *G|*g) echo $(( ${1%[Gg]} * 1024 * 1024 * 1024 )) ;;
        *)     echo "$1" ;;
    esac
}

mkdir -p "$WORK"
RAW=$(stat -f%z "$IN" 2>/dev/null || stat -c%s "$IN")

echo "input   $IN ($(( RAW / 1048576 )) MiB)"
echo "codec   $CODEC, $READS reads of 4096 bytes per configuration"
[ "$CODEC" = stored ] && echo "note    the stored codec does not compress; the ratio column will read 1.000x"
echo
printf "%-14s %-10s %10s %10s %10s\n" "block" "dict" "ratio" "seek p50" "seek p99"
printf -- "------------------------------------------------------------\n"

for BS in 64K 256K 1M 8M; do
    for D in 0 8M; do
        OUT="$WORK/sweep-$BS-$D.xq"
        "$XQ" compress -c "$CODEC" -b "$BS" -D "$D" -o "$OUT" "$IN" 2>/dev/null || continue
        SZ=$(stat -f%z "$OUT" 2>/dev/null || stat -c%s "$OUT")
        BSB=$(to_bytes "$BS")
        NBLK=$(( (RAW + BSB - 1) / BSB ))
        CACHE=4
        if [ "$NBLK" -lt 32 ]; then
            printf "%-14s %-10s %9.3fx %s\n" "$BS" "$([ "$D" = 0 ] && echo none || echo "$D")" \
                   "$(awk -v r=$RAW -v s=$SZ 'BEGIN{print r/s}')" "  (only $NBLK blocks; too few to measure seeks)"
            rm -f "$OUT"; continue
        fi
        STATS=$("$BENCH" "$OUT" "$READS" 4096 "$CACHE" 2>/dev/null)
        P50=$(printf '%s\n' "$STATS" | awk '/p50/{print $2}')
        P99=$(printf '%s\n' "$STATS" | awk '/p99  /{print $2}')
        awk -v b="$BS" -v d="$D" -v raw="$RAW" -v sz="$SZ" -v p50="$P50" -v p99="$P99" 'BEGIN{
            printf "%-14s %-10s %9.3fx %8s us %8s us\n", b, (d=="0"?"none":d), raw/sz, p50, p99;
        }'
        rm -f "$OUT"
    done
done

echo
echo "Read a dictionary row against the no-dictionary row of a larger block:"
echo "matching ratio at a smaller block size is the same seek made cheaper."
