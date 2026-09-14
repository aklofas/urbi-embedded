#include "rtest.h"
#include "rt/uvalue.h"
#include <stddef.h>
static void size_and_ctors(void) {
    RT_CHECK(sizeof(UValue) == (sizeof(void *) == 8 ? 16 : 12));
    RT_CHECK(offsetof(UValue, v) == (sizeof(void *) == 8 ? 8 : 4));
    RT_CHECK(uv_is(uv_int(3), UV_INT) && uv_int(3).v.i == 3);
    RT_CHECK(uv_as_double(uv_int(2)) == 2.0 && uv_as_double(uv_float(2.5)) == 2.5);
}
static void truthiness(void) {
    RT_CHECK(!uv_truthy(uv_nil()) && !uv_truthy(uv_void()) && !uv_truthy(uv_bool(false)));
    RT_CHECK(!uv_truthy(uv_int(0)) && !uv_truthy(uv_float(0.0)));
    RT_CHECK(uv_truthy(uv_int(1)) && uv_truthy(uv_bool(true)) && uv_truthy(uv_ptr(UV_OBJ, (void *)1)));
}
RT_SUITE(rt_value_suite) { rt_run("size_and_ctors", size_and_ctors); rt_run("truthiness", truthiness); }
