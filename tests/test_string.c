// tests/test_string.c — host-side unit tests for core/string.c
// Compile with -m32 to match the OS's 32-bit size_t/pointer model.
#include <stdio.h>
#include <string.h>

// glibc defines these as macros; drop them before renaming.
#undef strchr
#undef strrchr
#undef strstr

// Rename CamelOS symbols so they don't clash with the host libc.
#define strlen     cam_strlen
#define strcmp     cam_strcmp
#define strncmp    cam_strncmp
#define strcpy     cam_strcpy
#define strncpy    cam_strncpy
#define strcat     cam_strcat
#define strncat    cam_strncat
#define strchr     cam_strchr
#define strrchr    cam_strrchr
#define strstr     cam_strstr
#define memcmp     cam_memcmp
#define memmove    cam_memmove
#define sprintf    cam_sprintf
#define snprintf   cam_snprintf
#define vsprintf   cam_vsprintf
#define vsnprintf  cam_vsnprintf
#define atoi       cam_atoi
#define printk     cam_printk
#define int_to_str cam_int_to_str
#define int_to_hex cam_int_to_hex

#include "../core/string.c"

#undef strlen
#undef strcmp
#undef strncmp
#undef strcpy
#undef strncpy
#undef strcat
#undef strncat
#undef strchr
#undef strrchr
#undef strstr
#undef memcmp
#undef memmove
#undef sprintf
#undef snprintf
#undef vsprintf
#undef vsnprintf
#undef atoi
#undef printk
#undef int_to_str
#undef int_to_hex

// Stub for the kernel logger referenced by printk().
void s_printf(const char* fmt, ...) { (void)fmt; }

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

int main(void) {
    char buf[64];

    // strlen
    CHECK(cam_strlen("") == 0);
    CHECK(cam_strlen("hello") == 5);

    // strcmp / strncmp
    CHECK(cam_strcmp("abc", "abc") == 0);
    CHECK(cam_strcmp("abc", "abd") < 0);
    CHECK(cam_strcmp("abd", "abc") > 0);
    CHECK(cam_strncmp("abcdef", "abcxyz", 3) == 0);
    CHECK(cam_strncmp("abc", "abc", 10) == 0);

    // strcpy
    CHECK(cam_strcpy(buf, "test") == buf);
    CHECK(strcmp(buf, "test") == 0);

    // strncpy: pads with NUL when src is shorter than n
    cam_strcpy(buf, "hello");
    cam_strncpy(buf, "xy", 3);
    CHECK(buf[0] == 'x' && buf[1] == 'y' && buf[2] == '\0');

    // strncpy: does NOT NUL-terminate when src length >= n
    cam_strcpy(buf, "hello");
    cam_strncpy(buf, "abc", 3);
    CHECK(buf[0] == 'a' && buf[1] == 'b' && buf[2] == 'c' && buf[3] == 'l');

    // strcat / strncat
    cam_strcpy(buf, "foo");
    CHECK(strcmp(cam_strcat(buf, "bar"), "foobar") == 0);
    cam_strcpy(buf, "foo");
    cam_strncat(buf, "barbaz", 3);
    CHECK(strcmp(buf, "foobar") == 0);

    // strchr / strrchr
    CHECK(cam_strchr("hello", 'l') == strchr("hello", 'l'));
    CHECK(cam_strrchr("hello", 'l') == strrchr("hello", 'l'));
    CHECK(cam_strchr("hello", 'z') == NULL);

    // strstr
    CHECK(cam_strstr("hello world", "world") != NULL);
    CHECK(cam_strstr("hello", "z") == NULL);

    // memcmp
    CHECK(cam_memcmp("abc", "abc", 3) == 0);
    CHECK(cam_memcmp("abc", "abd", 3) < 0);

    // memmove (overlapping)
    cam_strcpy(buf, "abcdef");
    cam_memmove(buf + 2, buf, 4);
    CHECK(strcmp(buf, "ababcd") == 0);

    // int_to_str
    cam_int_to_str(123, buf); CHECK(strcmp(buf, "123") == 0);
    cam_int_to_str(-45, buf); CHECK(strcmp(buf, "-45") == 0);
    cam_int_to_str(0, buf);   CHECK(strcmp(buf, "0") == 0);

    // atoi
    CHECK(cam_atoi("123") == 123);
    CHECK(cam_atoi("-42") == -42);
    CHECK(cam_atoi("  99") == 99);

    // sprintf / snprintf
    cam_sprintf(buf, "%s-%d", "v", 7); CHECK(strcmp(buf, "v-7") == 0);
    cam_sprintf(buf, "%x", 255);       CHECK(strcmp(buf, "ff") == 0);
    cam_snprintf(buf, 4, "hello");     CHECK(strcmp(buf, "hel") == 0);

    if (failures == 0) { printf("test_string: ALL PASSED\n"); return 0; }
    printf("test_string: %d FAILED\n", failures);
    return 1;
}
