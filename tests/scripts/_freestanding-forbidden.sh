# Single source of truth for the forbidden-libc symbol regex used by
# both test-freestanding.sh (cross-build archives) and
# build-freestanding-host.sh (host-cc per-TU .o files).
#
# Sourced — not executable.  Leading underscore marks the convention.
#
# Symbol set rationale: any reference to these in a freestanding
# URBI_BYTECODE_ONLY=1 build means we leaked a hosted-libc dep that
# an embedded RTOS image will fail to resolve.  Add to the regex
# only after documenting an accepted exception or guarding the
# offending source under #if !defined(URBI_BYTECODE_ONLY).

# refactor-3 GATE-06: puts/putchar/fputs/fputc added because gcc rewrites
# printf("...\n") → puts() at any -O level — the old list missed the
# compiler's own substitution; vsnprintf/vprintf/strdup close the remaining
# stdio/alloc family gaps.  v0.13.0 review fix: vfprintf/vsprintf/sscanf
# complete the v-variant printf family and the scanf family.
FORBIDDEN_LIBC_REGEX='^(printf|fprintf|sprintf|snprintf|vsnprintf|vprintf|vfprintf|vsprintf|sscanf|puts|putchar|fputs|fputc|strdup|malloc|calloc|realloc|free|fopen|fclose|fread|fwrite|strtod|strtol|strtoul|abort|exit)$'
