/* Simple tester for parse_lut. */

#include <stdbool.h>
#include <stdio.h>

#include "parse_lut.h"


struct lut_test {
    const char *input;
    enum parse_lut_status status;
    int result;
};

#define SUCCEED(test, result)   { test, LUT_PARSE_OK, result }
#define FAILURE(test, result)   { test, result, 0 }

static struct lut_test tests[] = {
    /* Simple constants. */
    SUCCEED("A",                                0xffff0000),
    SUCCEED("B",                                0xff00ff00),
    SUCCEED("C",                                0xf0f0f0f0),
    SUCCEED("D",                                0xcccccccc),
    SUCCEED("E",                                0xaaaaaaaa),
    /* Basic parsing errors. */
    FAILURE("",                                 LUT_PARSE_NO_VALUE),
    FAILURE("a",                                LUT_PARSE_TOKEN_ERROR),
    FAILURE(")",                                LUT_PARSE_NO_OPEN),
    FAILURE("A)",                               LUT_PARSE_NO_OPEN),
    FAILURE("(",                                LUT_PARSE_NO_CLOSE),
    FAILURE("((((((((((((((((((((",             LUT_PARSE_TOO_COMPLEX),
    FAILURE("AA",                               LUT_PARSE_NO_OPERATOR),
    FAILURE("A&",                               LUT_PARSE_NO_VALUE),
    FAILURE("A:B",                              LUT_PARSE_NO_IF),
    FAILURE("A?B",                              LUT_PARSE_NO_ELSE),
    FAILURE("(B:",                              LUT_PARSE_NO_IF),
    FAILURE("(B?C)",                            LUT_PARSE_NO_ELSE),
    /* More complex expressions. */
    SUCCEED("A==B",                             0xff0000ff),
    SUCCEED("A=B",                              0xff0000ff),
    SUCCEED("A&B",                              0xff000000),
    SUCCEED("A&B|C",                            0xfff0f0f0),
    SUCCEED("A?B:C",                            0xff00f0f0),
    SUCCEED("(A?B:C)",                          0xff00f0f0),
    SUCCEED("~A?B:C",                           0xf0f0ff00),
    SUCCEED("~(A?B:C)",                         0x00ff0f0f),
    SUCCEED("A&B|C&~D",                         0xff303030),
    SUCCEED("A?B:C?D:E",                        0xff00caca),
    SUCCEED("A=>B?C:D",                         0xf0ccf0f0),
    SUCCEED("A=>(B?C:D)",                       0xf0ccffff),
    SUCCEED("A=B&C",                            0xf00000f0),
    SUCCEED("(A=B)&C",                          0xf00000f0),
    SUCCEED("A=(B&C)",                          0xf0000fff),
    SUCCEED("A&B|C^D=E=>A?0:1",                 0x00006969),
    SUCCEED("A&B&C&D&E",                        0x80000000),
    SUCCEED("~A&~B&~C&~D&~E",                   0x00000001),
    SUCCEED("A=>B=>C",                          0xf0fff0f0),
    SUCCEED("A=>(B=>C)",                        0xf0ffffff),
};


int main(int argc, const char **argv)
{
    bool ok = true;
    if (argc > 1)
    {
        for (int i = 1; i < argc; i ++)
        {
            int result = 0;
            enum parse_lut_status status = parse_lut(argv[1], &result);
            printf("\"%s\" => (%d, %08x)\n", argv[1], status, result);
            if (status != LUT_PARSE_OK)
                ok = false;
        }
    }
    else
        for (size_t i = 0; i < sizeof(tests) / sizeof(struct lut_test); i ++)
        {
            struct lut_test *test = &tests[i];
            int result = 0;
            enum parse_lut_status status = parse_lut(test->input, &result);
            if (status != test->status  ||
                (status == LUT_PARSE_OK  &&  result != test->result))
            {
                printf("Test: \"%s\" => (%d, %08x) != (%d, %08x)\n",
                    test->input, status, result, test->status, test->result);
                ok = false;
            }
        }
    return ok ? 0 : 1;
}
