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

FORBIDDEN_LIBC_REGEX='^(printf|fprintf|sprintf|snprintf|malloc|calloc|realloc|free|fopen|fclose|fread|fwrite|strtod|strtol|strtoul|abort|exit)$'
