/* Disassembles Direct3D 9 shader bytecode files (raw token streams, as XIVLEGACY_DUMP_SHADERS writes
 * them): sm3dis <file>... ; -q only checks that each parses. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sm3.h"

int main(int argc, char** argv)
{
    int quiet = 0, bad = 0, files = 0;
    for (int a = 1; a < argc; ++a)
    {
        if (!strcmp(argv[a], "-q"))
        {
            quiet = 1;
            continue;
        }
        FILE* f = fopen(argv[a], "rb");
        if (!f)
        {
            perror(argv[a]);
            bad++;
            continue;
        }
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint32_t* tk = (uint32_t*)malloc((size_t)size + 4);
        size_t n = fread(tk, 4, (size_t)size / 4, f);
        fclose(f);
        Sm3Shader s;
        char err[128];
        files++;
        if (!sm3_parse(tk, n, &s, err, sizeof err))
        {
            printf("%s: %s\n", argv[a], err);
            bad++;
        }
        else if (!quiet)
        {
            printf("// %s: %u instructions, constant table %u bytes\n%s_%u_%u\n", argv[a], (unsigned)s.nins, s.ctab_size,
                s.pixel ? "ps" : "vs", s.major, s.minor);
            for (size_t i = 0; i < s.nins; ++i)
            {
                char line[256];
                sm3_format(&s, &s.ins[i], line, sizeof line);
                printf("    %s\n", line);
            }
        }
        sm3_free(&s);
        free(tk);
    }
    if (quiet)
        printf("%d files, %d failed\n", files, bad);
    return bad != 0;
}
