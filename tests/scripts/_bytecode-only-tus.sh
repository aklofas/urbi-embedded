# Single source of truth for the URBI_BYTECODE_ONLY TU keep-list.
# Echoes one source path per line, relative to repo root.
#
# Sourced by build-bytecode-only.sh (architecture smoke) and
# build-freestanding-host.sh (per-TU freestanding-symbol check).
#
# Keep-out list — these TUs are parser/emitter-coupled and elided
# by the real URBI_BYTECODE_ONLY build flag:
#   - src/lex/*.c          (lexer)
#   - src/parse/*.c        (parser + AST builder)
#   - src/emit/*.c         (bytecode emitter + disasm)
#   - src/urbi.c           (urbi_compile_source — parses + emits)
#   - src/chunk/uchunk_strand.c  (urbi_repl_eval — parses + emits)
#
# Sourced — not executable.  Leading underscore marks the convention.

list_kept_tus() {
    local keep_dirs="src/vm src/gc src/sched src/watcher src/event src/tag \
                     src/changed src/chunk src/value src/runtime src/realm \
                     src/object src/stdlib"

    # Sources at src/ root: keep everything except urbi.c (parser-coupled).
    for f in src/*.c; do
        [ -f "$f" ] || continue
        case "$(basename "$f")" in
            urbi.c) ;;
            *) echo "$f" ;;
        esac
    done

    # Subdir sources: keep all except chunk/uchunk_strand.c.
    for d in $keep_dirs; do
        for f in "$d"/*.c; do
            [ -f "$f" ] || continue
            case "$f" in
                src/chunk/uchunk_strand.c) ;;
                *) echo "$f" ;;
            esac
        done
    done
}
