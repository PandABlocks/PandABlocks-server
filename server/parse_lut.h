/* Interface to LUT expression parser. */

/* Changes to this list of enums must be reflected in parse_lut_error_string()
 * defined in parse_lut.c. */
enum parse_lut_status {
    LUT_PARSE_OK,           // Parse ok, sensible value returned
    LUT_PARSE_TOKEN_ERROR,  // Invalid token in input stream
    LUT_PARSE_TOO_COMPLEX,  // Parse stack overflow
    LUT_PARSE_NO_OPERATOR,  // Missing operator between values
    LUT_PARSE_NO_OPEN,      // Missing open bracket
    LUT_PARSE_NO_CLOSE,     // Missing close bracket
    LUT_PARSE_NO_VALUE,     // Missing value
    LUT_PARSE_NO_IF,        // Missing ? before :
    LUT_PARSE_NO_ELSE,      // Missing : after ?
};

/* Parses the following simple language to express a 5-bit lookup table.  The
 * constants are the letters A, B, C, D, E representing the 5 inputs and numbers
 * 0 and 1, ~ represents complement, =, &, ^, |, =>, in descending order of
 * precedence represent XNOR, AND, XOR, OR, IMPLIES (A=>B is ~A|B) respectively,
 * and ?: is used to represent conditional choice (A?B:C is A&B|~A&C). */
enum parse_lut_status parse_lut(const char *input, int *result);

/* Returns error string corresponding to given status. */
const char *parse_lut_error_string(enum parse_lut_status status);
