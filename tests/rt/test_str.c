#include "rtest.h"
#include "fakevm.h"
#include "rt/ustr.h"
static void intern_is_pointer_equal(void) {
    struct UVM vm; fakevm_init(&vm, NULL, 0);
    USym *a = usym_cstr(&vm, "hello"), *b = usym_intern(&vm, "hellox", 5);
    RT_CHECK(a == b && a->len == 5 && memcmp(a->bytes, "hello", 5) == 0 && a->bytes[5] == '\0');
    RT_CHECK(usym_cstr(&vm, "world") != a);
    ugc_collect(&vm);                                  /* symbols survive GC untouched */
    RT_CHECK(usym_cstr(&vm, "hello") == a);
    ustrtab_destroy(&vm, &vm.strings); ugc_destroy(&vm);
}
static void strings_are_collected(void) {
    struct UVM vm; fakevm_init(&vm, NULL, 0);
    for (int i = 0; i < 100; i++) ustr_concat(&vm, "ab", 2, "cd", 2);
    ugc_collect(&vm);
    RT_EQ(vm.gc.cells_live, 0u);
    ustrtab_destroy(&vm, &vm.strings); ugc_destroy(&vm);
}
static void sym_str_equality(void) {
    struct UVM vm; fakevm_init(&vm, NULL, 0);
    UValue s = uv_sym(usym_cstr(&vm, "abcd")), t = uv_str(ustr_concat(&vm, "ab", 2, "cd", 2));
    RT_CHECK(uv_str_equal(s, t) && uv_to_sym(&vm, t) == (USym *)s.v.p);
    ustrtab_destroy(&vm, &vm.strings); ugc_destroy(&vm);
}
static void table_grows(void) {
    struct UVM vm; fakevm_init(&vm, NULL, 0);
    char buf[16];
    for (int i = 0; i < 5000; i++) { int n = snprintf(buf, sizeof buf, "s%d", i); usym_intern(&vm, buf, (size_t)n); }
    RT_EQ(vm.strings.count, 5000u);
    RT_CHECK(vm.strings.nbuckets >= 4096);
    ustrtab_destroy(&vm, &vm.strings); ugc_destroy(&vm);
}
static void concat_produces_correct_bytes(void) {
    struct UVM vm; UCell *roots[1] = { NULL }; fakevm_init(&vm, roots, 1);
    UStr *ab = ustr_concat(&vm, "abc", 3, "def", 3);
    roots[0] = (UCell *)ab;         /* root before the second concat may collect under stress */
    UStr *cd = ustr_concat(&vm, "abc", 3, "def", 3);
    (void)cd;
    RT_CHECK(ab->len == 6 && memcmp(ab->bytes, "abcdef", 6) == 0 && ab->bytes[6] == '\0');
    ustrtab_destroy(&vm, &vm.strings); ugc_destroy(&vm);
}
RT_SUITE(rt_str_suite) {
    rt_run("intern_is_pointer_equal", intern_is_pointer_equal);
    rt_run("strings_are_collected", strings_are_collected);
    rt_run("sym_str_equality", sym_str_equality);
    rt_run("table_grows", table_grows);
    rt_run("concat_produces_correct_bytes", concat_produces_correct_bytes);
}
