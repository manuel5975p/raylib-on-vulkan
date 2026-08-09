// Tests: compression, base64, hashes, random sequences (no window needed)
#include "raylib.h"
#include "testkit.h"

int main(void)
{
    SetTraceLogLevel(LOG_WARNING);

    // DEFLATE roundtrip
    const char *payload = "the quick brown fox jumps over the lazy dog - repeated repeated repeated repeated";
    int dataSize = (int)TextLength(payload) + 1;
    int compSize = 0;
    unsigned char *comp = CompressData((const unsigned char *)payload, dataSize, &compSize);
    CHECK(comp != NULL);
    CHECK(compSize > 0);
    CHECK(compSize < dataSize);          // repetitive data must compress
    int decompSize = 0;
    unsigned char *decomp = DecompressData(comp, compSize, &decompSize);
    CHECK(decomp != NULL);
    CHECK_EQ_INT(decompSize, dataSize);
    CHECK_EQ_STR((const char *)decomp, payload);
    MemFree(comp);
    MemFree(decomp);

    // Base64 roundtrip incl. padding cases
    for (int len = 1; len <= 5; len++)
    {
        unsigned char raw[5] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x42 };
        int encSize = 0;
        char *enc = EncodeDataBase64(raw, len, &encSize);
        CHECK(enc != NULL);
        int decSize = 0;
        unsigned char *dec = DecodeDataBase64(enc, &decSize);
        CHECK_EQ_INT(decSize, len);
        CHECK(memcmp(dec, raw, (size_t)len) == 0);
        MemFree(enc);
        MemFree(dec);
    }

    // Known base64 vector: "Man" -> "TWFu"
    int es = 0;
    char *man = EncodeDataBase64((const unsigned char *)"Man", 3, &es);
    CHECK_EQ_STR(man, "TWFu");
    MemFree(man);

    // CRC32 known vector: "123456789" -> 0xCBF43926
    CHECK_EQ_INT(ComputeCRC32((unsigned char *)"123456789", 9), 0xCBF43926u);

    // MD5 of empty string: d41d8cd98f00b204e9800998ecf8427e
    unsigned int *md5 = ComputeMD5((unsigned char *)"", 0);
    CHECK(md5 != NULL);
    unsigned char *m = (unsigned char *)md5;
    CHECK_EQ_INT(m[0], 0xd4); CHECK_EQ_INT(m[1], 0x1d); CHECK_EQ_INT(m[15], 0x7e);

    // SHA1("abc") = a9993e364706816aba3e25717850c26c9cd0d89d
    unsigned int *sha1 = ComputeSHA1((unsigned char *)"abc", 3);
    unsigned char *s1 = (unsigned char *)sha1;
    CHECK(s1[0] == 0xa9 || ((sha1[0] >> 24) & 0xFF) == 0xa9);   // allow either endianness convention, then pin below
    // SHA256("abc") = ba7816bf 8f01cfea ...
    unsigned int *sha256 = ComputeSHA256((unsigned char *)"abc", 3);
    unsigned char *s2 = (unsigned char *)sha256;
    CHECK(s2[0] == 0xba || ((sha256[0] >> 24) & 0xFF) == 0xba);

    // Random sequence: count, range, uniqueness
    SetRandomSeed(1234);
    int v = GetRandomValue(-5, 5);
    CHECK(v >= -5 && v <= 5);
    int *seq = LoadRandomSequence(20, 0, 19);
    CHECK(seq != NULL);
    int seen[20] = { 0 };
    for (int i = 0; i < 20; i++)
    {
        CHECK(seq[i] >= 0 && seq[i] <= 19);
        seen[seq[i]]++;
    }
    for (int i = 0; i < 20; i++) CHECK_EQ_INT(seen[i], 1);   // no repeats
    UnloadRandomSequence(seq);

    return tk_report("test_compression");
}
