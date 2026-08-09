// Minimal single-header test kit for raylib-on-vulkan tests
#ifndef TESTKIT_H
#define TESTKIT_H

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tk_failures = 0;
static int tk_checks = 0;

#define CHECK(cond) do { \
    tk_checks++; \
    if (!(cond)) { tk_failures++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

#define CHECK_EQ_INT(a, b) do { \
    tk_checks++; long long va = (long long)(a), vb = (long long)(b); \
    if (va != vb) { tk_failures++; \
        fprintf(stderr, "FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, va, vb); } \
    } while (0)

#define CHECK_EQ_STR(a, b) do { \
    tk_checks++; const char *va = (a), *vb = (b); \
    if ((va == NULL) || (vb == NULL) || (strcmp(va, vb) != 0)) { tk_failures++; \
        fprintf(stderr, "FAIL %s:%d: %s == %s (\"%s\" != \"%s\")\n", __FILE__, __LINE__, #a, #b, \
                va? va : "(null)", vb? vb : "(null)"); } \
    } while (0)

#define CHECK_NEAR(a, b, eps) do { \
    tk_checks++; double va = (a), vb = (b); \
    if (fabs(va - vb) > (eps)) { tk_failures++; \
        fprintf(stderr, "FAIL %s:%d: %s ~= %s (%g vs %g)\n", __FILE__, __LINE__, #a, #b, va, vb); } \
    } while (0)

static int tk_report(const char *name)
{
    if (tk_failures == 0) printf("PASS %s (%d checks)\n", name, tk_checks);
    else printf("FAIL %s (%d/%d checks failed)\n", name, tk_failures, tk_checks);
    return (tk_failures == 0)? 0 : 1;
}

#endif // TESTKIT_H
