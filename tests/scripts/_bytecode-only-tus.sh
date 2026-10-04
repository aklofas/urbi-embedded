# Single source of truth for the URBI_BYTECODE_ONLY TU keep-list.
# Echoes one source path per line, relative to repo root.
#
# Sourced by build-bytecode-only.sh (architecture smoke) and
# build-freestanding-host.sh (per-TU freestanding-symbol check).
#
# Keep-out list — these TUs are elided from the freestanding /
# URBI_BYTECODE_ONLY build:
#   - src/lex/*.c          (lexer; parser-coupled)
#   - src/parse/*.c        (parser + AST builder)
#   - src/emit/*.c         (bytecode emitter + disasm)
#   - src/host/*.c         (hosted-only public API helpers, e.g. the
#                           snprintf-based urbi_value_to_string)
#   - src/repl/*.c         (REPL service; hosted transports / parses via
#                           urbi_repl_eval)
#   - src/ros/*.c          (ROS2 bridge; opt-in hosted component)
#   - src/urobotics/*.c    (Standard Robotics facet overlay; opt-in)
# Generated data TUs carry no code and are skipped.
#
# Sourced — not executable.  Leading underscore marks the convention.

# KEPT subdir set: sources under these directories are compiled into the
# freestanding / bytecode-only build.
BYTECODE_ONLY_KEEP_DIRS="src/rt src/chunk src/util src/stdlib"

# EXCLUDED subdir set: deliberately kept OUT of the freestanding /
# bytecode-only build (parser/emitter front-end + hosted opt-in components).
# Every src/*/ dir with C sources must appear in exactly one of these two
# lists; check_all_src_dirs_classified enforces that (BLD-CI-5).
BYTECODE_ONLY_EXCLUDE_DIRS="src/lex src/parse src/emit src/host src/repl src/ros src/urobotics"

list_kept_tus() {
    local d f
    for d in $BYTECODE_ONLY_KEEP_DIRS; do
        for f in "$d"/*.c; do
            [ -f "$f" ] || continue
            case "$f" in
                src/stdlib/urbi_stdlib_bytecode.gen.c) ;;   # generated data, no code
                *) echo "$f" ;;
            esac
        done
    done
}

# BLD-CI-5: drift guard.  Every src/*/ directory that contains C sources must
# be classified as either KEPT or explicitly EXCLUDED.  A new src/<dir>/
# appearing without a build decision here means the freestanding /
# bytecode-only keep-list has silently drifted from the source tree — fail
# loudly so the omission is caught at gate time, not at cross-compile time.
# Returns 0 when every dir is classified, 1 (and prints the offenders) if not.
check_all_src_dirs_classified() {
    local unclassified=""
    local d base
    for d in src/*/; do
        d="${d%/}"
        # Skip dirs with no C sources (headers-only or generated-empty).
        ls "$d"/*.c >/dev/null 2>&1 || continue
        case " $BYTECODE_ONLY_KEEP_DIRS $BYTECODE_ONLY_EXCLUDE_DIRS " in
            *" $d "*) ;;
            *) unclassified="$unclassified $d" ;;
        esac
    done
    if [ -n "$unclassified" ]; then
        echo "FAIL: unclassified src/ dir(s) in the bytecode-only keep-list:" >&2
        for base in $unclassified; do
            echo "  $base — add to BYTECODE_ONLY_KEEP_DIRS or BYTECODE_ONLY_EXCLUDE_DIRS in tests/scripts/_bytecode-only-tus.sh" >&2
        done
        return 1
    fi
    return 0
}
