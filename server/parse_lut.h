/* Interface to LUT expression parser. */

enum parse_lut_status {
    LUT_PARSE_OK,           // Parse ok, sensible value returned
    LUT_PARSE_TOKEN_ERROR,  // Invalid token in input stream
    LUT_PARSE_TOO_COMPLEX,  // Parse stack overflow
    LUT_PARSE_NO_OPERATOR,  // Missing operator between values
    LUT_PARSE_NO_OPEN,      // Missing open bracket
    LUT_PARSE_NO_CLOSE,     // Missing close bracket
    LUT_PARSE_NO_VALUE,     // Missing value (or multiple values???)
    LUT_PARSE_NO_TOKEN,     // Missing token (can this happen???)
    LUT_PARSE_NO_IF,        // Missing ? before :
    LUT_PARSE_NO_ELSE,      // Missing : after ?
    LUT_PARSE_ERROR,        // Internal error, should not occur
};

enum parse_lut_status parse_lut(const char *input, int *result);
