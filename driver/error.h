/* If test is true then do on_error, print message and goto target. */
#define TEST_(test, on_error, target, message) \
    do if (test) { \
        on_error; \
        printk(KERN_ERR "PandA: " message "\n"); \
        goto target; \
    } while (0)

/* If rc is an error code (< 0) then print message and goto target. */
#define TEST_RC(rc, target, message) \
    TEST_((rc) < 0, , target, message)

/* If ptr indicates an error then assign the associated error code to rc, print
 * message and goto target.  If ptr is in fact NULL we return -ENOMEM. */
#define TEST_PTR(ptr, rc, target, message) \
    TEST_(IS_ERR(ptr), rc = PTR_ERR(ptr)?:-ENOMEM, target, message)



/* We specify the size of a single FA block as a power of 2 (because we're
 * going to allocate the block with __get_free_page(). */
#define FA_BLOCK_SIZE       (1 << fa_block_shift)


