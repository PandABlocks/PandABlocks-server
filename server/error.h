/* Helper macros and declarations to simplify error handling.
 *
 * Copyright (c) 2011 Michael Abbott, Diamond Light Source Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Contact:
 *      Dr. Michael Abbott,
 *      Diamond Light Source Ltd,
 *      Diamond House,
 *      Chilton,
 *      Didcot,
 *      Oxfordshire,
 *      OX11 0DE
 *      michael.abbott@diamond.ac.uk
 */

/* The following error handling macros are defined here:
 *
 *  TEST_OK      TEST_OK_      ASSERT_OK       Fail if expression is false
 *  TEST_IO      TEST_IO_      ASSERT_IO       Fail if expression is -1
 *  TEST_NULL    TEST_NULL_    ASSERT_NULL     Fail if expression equals NULL
 *  TEST_PTHREAD TEST_PTHREAD_ ASSERT_PTHREAD  Fail if expression is not 0
 *
 * There are also macros for handling file I/O in a similar form (but with
 * slightly different argument lists):
 *
 *  TEST_read   TEST_read_  ASSERT_read     Fail if read not of expected size
 *  TEST_write  TEST_write_ ASSERT_write    Fail if write not of expected size
 *
 * The three patterns behave thus:
 *
 *  TEST_xx(expr)
 *      If the test fails a canned error message (defined by the macro
 *      ERROR_MESSAGE) is generated and the macro evaluates to ERROR_OK,
 *      otherwise an error message is computed and returned.
 *
 *  TEST_xx_(expr, message...)
 *      If the test fails then the given error message (with sprintf formatting)
 *      is generated and returned, otherwise ERROR_OK is returned.
 *
 *  ASSERT_xx(expr)
 *      If the test fails then _error_panic() is called and execution does not
 *      continue from this point.
 *
 * Note that the _PTHREAD macros have the extra side effect of assigning any
 * non-zero expression to errno: these are designed to be used with the pthread
 * functions where this behaviour is appropriate.
 *
 * These macros are designed to be used as chained conjunctions of the form
 *
 *  TEST_xx(...)  ?:  TEST_xx(...)  ?:  ...
 *
 * To facilitate this three further macros are provided:
 *
 *  DO(statements)                  Performs statements and returns ERROR_OK
 *  IF(test, iftrue)                Only checks iftrue if test succeeds
 *  IF_ELSE(test, iftrue, iffalse)  Alternative spelling of (?:)
 *
 * The following macro is designed act as a kind of exception handler: if expr
 * generates an error then the on_error statement is executed, and the error
 * code from expr is returned.
 *
 *  TRY_CATCH(expr, on_error)       Executes on_error if expr fails
 */

#ifdef VX_WORKS
#include "vxdefs.h"
#endif

/* Hint to compiler that x is likely to be 0. */
#define unlikely(x)   __builtin_expect((x), 0)


/* Error messages are encoded as an opaque type, with NULL used to represent no
 * error.  The lifetime of error values must be managed by the methods here. */
struct error__t;
typedef struct error__t *error__t;  // Alas error_t is already spoken for!
#define ERROR_OK    ((error__t) NULL)


/* One of the following two functions must be called to release the resources
 * used by an error__t value. */

/* This reports the given error message.  If error was ERROR_OK and there was
 * nothing to report then false is returned, otherwise true is returned. */
bool error_report(error__t error);
/* A helper macro to extend the reported error with context. */
#define ERROR_REPORT(error, format...) \
    ( { error_extend(error, format); error_report(error); } )

/* This function silently discards the error code. */
void error_discard(error__t error);


/* This function extends the information associated with the given error with
 * the new message. */
void error_extend(error__t error, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

/* Converts an error code into a formatted string. */
const char *error_format(error__t error);




/* Called to report unrecoverable error.  Terminates program without return. */
void _error_panic(char *extra, const char *filename, int line)
    __attribute__((__noreturn__));
/* Performs normal error report. */
error__t _error_create(char *extra, const char *format, ...)
    __attribute__((format(printf, 2, 3)));

/* Two mechanisms for reporting extra error information. */
char *_error_extra_io(void);


/* Routines to write informative message or error to stderr or syslog. */
void log_message(const char *message, ...)
    __attribute__((format(printf, 1, 2)));
void log_error(const char *message, ...)
    __attribute__((format(printf, 1, 2)));
void vlog_message(int priority, const char *format, va_list args);



/* Hack for ensuring result is not ignored.  The IGNORE macro is available for
 * overriding this. */
static inline error__t __attribute__((warn_unused_result))
    _warn_unused_error__t(error__t error)
{
    return error;
}



/* A dance for generating unique local identifiers.  This involves a number of
 * tricky C preprocessor techniques, and uses the gcc __COUNTER__ extension. */
#define _CONCATENATE(a, b)  a##b
#define CONCATENATE(a, b)   _CONCATENATE(a, b)
#define UNIQUE_ID()         CONCATENATE(_tmp__, __COUNTER__)


/* Generic TEST macro: computes a boolean from expr using COND (should be a
 * macro), and generates the given error message if the boolean is false.  If
 * expr is successful then ERROR_OK is returned. */
#define _id_TEST(result, COND, EXTRA, expr, message...) \
    _warn_unused_error__t(( { \
        typeof(expr) result = (expr); \
        unlikely(!COND(result)) ? \
            _error_create(EXTRA(result), message) : \
            ERROR_OK; \
    } ))
#define _TEST(args...)  _id_TEST(UNIQUE_ID(), args)

/* An assert for tests that really really should not fail!  This exits
 * immediately. */
#define _ASSERT(COND, EXTRA, expr)  \
    do { \
        typeof(expr) __result__ = (expr); \
        if (unlikely(!COND(__result__))) \
            _error_panic(EXTRA(__result__), __FILE__, __LINE__); \
    } while (0)


/* Default error message for unexpected errors. */
#define ERROR_MESSAGE       "Unexpected error at %s:%d", __FILE__, __LINE__

/* Tests system calls: -1 => error, pick up error data from errno. */
#define _COND_IO(expr)              ((intptr_t) (expr) != -1)
#define _MSG_IO(expr)               _error_extra_io()
#define TEST_IO_(expr, message...)  _TEST(_COND_IO, _MSG_IO, expr, message)
#define TEST_IO(expr)               TEST_IO_(expr, ERROR_MESSAGE)
#define ASSERT_IO(expr)             _ASSERT(_COND_IO, _MSG_IO, expr)

/* Tests an ordinary boolean: false => error. */
#define _COND_OK(expr)              ((bool) (expr))
#define _MSG_OK(expr)               NULL
#define TEST_OK_(expr, message...)  _TEST(_COND_OK, _MSG_OK, expr, message)
#define TEST_OK(expr)               TEST_OK_(expr, ERROR_MESSAGE)
#define ASSERT_OK(expr)             _ASSERT(_COND_OK, _MSG_OK, expr)

/* Tests pointers: NULL => error.  If there is extra information in errno then
 * use the NULL_IO test, otherwise just NULL. */
#define _COND_NULL(expr)            ((expr) != NULL)
#define TEST_NULL_(expr, message...) \
    _TEST(_COND_NULL, _MSG_OK, expr, message)
#define TEST_NULL(expr)             TEST_NULL_(expr, ERROR_MESSAGE)
#define ASSERT_NULL(expr)           _ASSERT(_COND_NULL, _MSG_OK, expr)

#define TEST_NULL_IO_(expr, message...) \
    _TEST(_COND_NULL, _MSG_IO, expr, message)
#define TEST_NULL_IO(expr)          TEST_NULL_IO_(expr, ERROR_MESSAGE)
#define ASSERT_NULL_IO(expr)        _ASSERT(_COND_NULL, _MSG_IO, expr)

/* Tests the return from a pthread_ call: a non zero return is the error
 * code!  We just assign this to errno. */
#define _COND_PTHREAD(expr)         ((expr) == 0)
#define _MSG_PTHREAD(expr)          ({ errno = (expr); _error_extra_io(); })
#define TEST_PTHREAD_(expr, message...) \
    _TEST(_COND_PTHREAD, _MSG_PTHREAD, expr, message)
#define TEST_PTHREAD(expr)          TEST_PTHREAD_(expr, ERROR_MESSAGE)
#define ASSERT_PTHREAD(expr)        _ASSERT(_COND_PTHREAD, _MSG_PTHREAD, expr)


/* For marking unreachable code.  Same as ASSERT_OK(false). */
#define ASSERT_FAIL()               _error_panic(NULL, __FILE__, __LINE__)

/* For failing immediately.  Same as TEST_OK_(false, message...) */
#define FAIL_(message...) \
    _warn_unused_error__t(_error_create(NULL, message))


/* These two macros facilitate using the macros above by creating if
 * expressions that are slightly more sensible looking than ?: in context. */
#define DO(action)                      ({action; ERROR_OK;})
#define IF(test, iftrue)                ((test) ? (iftrue) : ERROR_OK)
#define IF_ELSE(test, iftrue, iffalse)  ((test) ? (iftrue) : (iffalse))

/* This macro is a workaround: passing a statement of the form { , } to a macro
 * doesn't work because the comma isn't guarded.  This macro adds the braces on
 * late enough to pass this form through. */
#define BRACES(args...)                 { args }


/* If action fails perform on_fail as a cleanup action.  Returns status of
 * action. */
#define _id_TRY_CATCH(error, action, on_fail) \
    _warn_unused_error__t(( { \
        error__t error = (action); \
        if (error) { on_fail; } \
        error; \
    } ))
#define TRY_CATCH(args...) _id_TRY_CATCH(UNIQUE_ID(), args)


/* Testing read and write happens often enough to be annoying, so some
 * special case macros here. */
#define _COND_rw(rw, fd, buf, count) \
    (rw(fd, buf, count) == (ssize_t) (count))
#define TEST_read(fd, buf, count)   TEST_OK(_COND_rw(read, fd, buf, count))
#define TEST_write(fd, buf, count)  TEST_OK(_COND_rw(write, fd, buf, count))
#define TEST_read_(fd, buf, count, message...) \
    TEST_OK_(_COND_rw(read, fd, buf, count), message)
#define TEST_write_(fd, buf, count, message...) \
    TEST_OK_(_COND_rw(write, fd, buf, count), message)


/* A tricksy compile time bug checking macro modified from the kernel. */
#define COMPILE_ASSERT(e)           ((void) sizeof(struct { int:-!(e); }))

/* A rather randomly placed helper routine.  This and its equivalents are
 * defined all over the place, but there doesn't appear to be a definitive
 * definition anywhere. */
#define ARRAY_SIZE(a)   (sizeof(a)/sizeof((a)[0]))

/* An agressive cast for use when the compiler needs special reassurance. */
#define _id_REINTERPRET_CAST(_union, type, value) \
    ( { \
        COMPILE_ASSERT(sizeof(type) == sizeof(typeof(value))); \
        union { \
            typeof(value) _value; \
            type _cast; \
        } _union = { ._value = (value) }; \
        _union._cast; \
    } )
#define REINTERPRET_CAST(args...) \
    _id_REINTERPRET_CAST(UNIQUE_ID(), args)

/* For ignoring return values even when warn_unused_result is in force.
 * Note: *don't* use this for error__t values, instead call error_discard(). */
#define IGNORE(e)       do if(e) {} while (0)


/* Use this to mark functions that can be constant folded, ie depend only on
 * their arguments and global state. */
#define __pure __attribute__((pure))
/* This function is a stronger variant of pure for functions which don't even
 * look at global memory.
 *    In truth and in practice we can get away with using this on functions
 * which inspect constant global memory.  Note however that pointer arguments
 * cannot be traversed by functions with this attribute. */
#define __const_ __attribute__((const))
