/* Host build tool: embed a binary file as a C array (Makefile counterpart of cmake/embed_spirv.cmake).
   Usage: embed_spirv <input.spv> <symbol> <output.c>
   Emits: const unsigned char <symbol>_data[]; const unsigned int <symbol>_size; */
#include <stdio.h>

int main(int argc, char **argv)
{
    if (argc != 4)
    {
        fprintf(stderr, "usage: %s <input.spv> <symbol> <output.c>\n", argv[0]);
        return 1;
    }

    FILE *in = fopen(argv[1], "rb");
    if (in == NULL) { perror(argv[1]); return 1; }
    FILE *out = fopen(argv[3], "wb");
    if (out == NULL) { perror(argv[3]); fclose(in); return 1; }

    fprintf(out, "/* Generated from SPIR-V, do not edit */\n");
    fprintf(out, "const unsigned char %s_data[] = {", argv[2]);
    unsigned long size = 0;
    for (int c = fgetc(in); c != EOF; c = fgetc(in), size++) fprintf(out, "0x%02x,", c);
    fprintf(out, "};\nconst unsigned int %s_size = %luu;\n", argv[2], size);

    int readOk = (ferror(in) == 0);
    fclose(in);
    if ((fclose(out) != 0) || !readOk || (size == 0))
    {
        fprintf(stderr, "embed_spirv: failed to embed %s\n", argv[1]);
        remove(argv[3]);
        return 1;
    }
    return 0;
}
