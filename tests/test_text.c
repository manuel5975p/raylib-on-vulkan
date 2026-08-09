// Tests: text string manipulation + UTF-8 codepoint handling (no window needed)
#include "raylib.h"
#include "testkit.h"

int main(void)
{
    SetTraceLogLevel(LOG_WARNING);

    // Basic string ops
    CHECK_EQ_INT(TextLength("hello"), 5);
    CHECK_EQ_INT(TextLength(""), 0);
    CHECK(TextIsEqual("abc", "abc"));
    CHECK(!TextIsEqual("abc", "abd"));
    CHECK(!TextIsEqual("abc", "abcd"));

    char dst[32] = { 0 };
    CHECK_EQ_INT(TextCopy(dst, "copy me"), 7);
    CHECK_EQ_STR(dst, "copy me");

    CHECK_EQ_STR(TextFormat("%d-%s", 42, "x"), "42-x");
    CHECK_EQ_STR(TextSubtext("abcdef", 2, 3), "cde");
    CHECK_EQ_STR(TextSubtext("abc", 0, 100), "abc");     // clamped
    CHECK_EQ_STR(TextToUpper("aBc12"), "ABC12");
    CHECK_EQ_STR(TextToLower("AbC12"), "abc12");
    CHECK_EQ_STR(TextToPascal("hello_world"), "HelloWorld");
    CHECK_EQ_STR(TextToSnake("HelloWorld"), "hello_world");
    CHECK_EQ_STR(TextToCamel("hello_world"), "helloWorld");
    CHECK_EQ_INT(TextToInteger("-123"), -123);
    CHECK_NEAR(TextToFloat("3.5"), 3.5, 1e-6);
    CHECK_EQ_INT(TextFindIndex("hello world", "world"), 6);
    CHECK_EQ_INT(TextFindIndex("hello", "xyz"), -1);
    CHECK_EQ_STR(TextRemoveSpaces("a b  c"), "abc");

    CHECK_EQ_STR(TextReplace("aaa", "a", "bb"), "bbbbbb");           // static buffer
    char *repAlloc = TextReplaceAlloc("aaa", "a", "bb");
    CHECK_EQ_STR(repAlloc, "bbbbbb");
    MemFree(repAlloc);

    CHECK_EQ_STR(TextInsert("hello", "XX", 2), "heXXllo");           // static buffer
    char *insAlloc = TextInsertAlloc("hello", "XX", 2);
    CHECK_EQ_STR(insAlloc, "heXXllo");
    MemFree(insAlloc);

    // Join/split
    const char *parts[3] = { "a", "b", "c" };
    CHECK_EQ_STR(TextJoin((char **)parts, 3, "-"), "a-b-c");
    int splitCount = 0;
    char **sp = TextSplit("x;y;z", ';', &splitCount);
    CHECK_EQ_INT(splitCount, 3);
    CHECK_EQ_STR(sp[0], "x");
    CHECK_EQ_STR(sp[2], "z");

    // Between
    CHECK_EQ_STR(GetTextBetween("pre[MID]post", "[", "]"), "MID");   // static buffer

    // UTF-8 codepoints: "aĝ€" = 1+2+3 bytes
    const char *utf8 = "a\xC4\x9D\xE2\x82\xAC";
    CHECK_EQ_INT(GetCodepointCount(utf8), 3);
    int cpSize = 0;
    CHECK_EQ_INT(GetCodepoint(utf8, &cpSize), 'a');
    CHECK_EQ_INT(cpSize, 1);
    CHECK_EQ_INT(GetCodepointNext(utf8 + 1, &cpSize), 0x011D);
    CHECK_EQ_INT(cpSize, 2);

    int count = 0;
    int *cps = LoadCodepoints(utf8, &count);
    CHECK_EQ_INT(count, 3);
    CHECK_EQ_INT(cps[2], 0x20AC);
    char *back = LoadUTF8(cps, count);
    CHECK_EQ_STR(back, utf8);
    UnloadUTF8(back);
    UnloadCodepoints(cps);

    int utf8Size = 0;
    const char *enc = CodepointToUTF8(0x20AC, &utf8Size);
    CHECK_EQ_INT(utf8Size, 3);
    CHECK(memcmp(enc, "\xE2\x82\xAC", 3) == 0);

    // Invalid UTF-8 -> '?' fallback
    CHECK_EQ_INT(GetCodepoint("\xFF", &cpSize), 0x3F);

    // Lines
    int lineCount = 0;
    char **lines = LoadTextLines("l1\nl2\nl3", &lineCount);
    CHECK_EQ_INT(lineCount, 3);
    CHECK_EQ_STR(lines[1], "l2");
    UnloadTextLines(lines, lineCount);

    return tk_report("test_text");
}
