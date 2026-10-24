#ifndef _INCFILE_H
#define _INCFILE_H

/** 
 * Include files in binary mode at the current location
 * 
 * There are 3 macro:
 * @code{c}
 * // Include a file, Optional offset and count 
 * // If no offset is specified, start from the beginning.
 * // If count is not specified, all remaining files will be included.
 * INCFILE(name, filename, [offset, [count]])
 * 
 * // Get file ptr by name
 * FILEPTR(name)
 * 
 * // Get file size
 * FILESIZE(name)
 * @endcode
 * 
 * Example usage:
 * @code{c}
 * // hello.txt content is 12345678hellohello12345
 * 
 * INCFILE(file1, "hello.txt")
 * INCFILE(file2, "hello.txt", 8)
 * INCFILE(file3, "hello.txt", 8, 10)
 * 
 * int main(void){
 *      assert(memcmp(FILEPTR(file1), "12345678hellohello12345", 
 *          FILESIZE(file1)) == 0);
 *      assert(memcmp(FILEPTR(file2), "hellohello12345", FILESIZE(file2)) == 0);
 *      assert(memcmp(FILEPTR(file3), "hellohello", FILESIZE(file3)) == 0);
 *      return 0;
 * }
 * 
 * @endcode
 */

#define INCF_STR2(x) #x
#define INCF_STR(x) INCF_STR2(x)

#if defined(__GNUC__)
#define GET_P4(p1, p2, p3, p4, ...) p4
#define SELECT_012_PARA(p0, p1, p2, ...) GET_P4(0, ##__VA_ARGS__, p2, p1, p0)

#define INCFILE_OFFSET(file, offset)                    \
        ".incbin \"" file "\"," INCF_STR(offset) "\n"
        
#define INCFILE_COUNT(file, offset, count, ...)         \
        ".incbin \"" file "\"," INCF_STR(offset)        \
        "," INCF_STR(count) "\n"

#define INCFILE_PARA(name, file, offset, ...)           \
    __asm(                                              \
        ".section .rodata." INCF_STR(name) "\n"         \
        ".global __" INCF_STR(name) "_start\n"          \
        ".type __" INCF_STR(name) "_start, %object\n"   \
        ".global __" INCF_STR(name) "_end\n"            \
        ".type __" INCF_STR(name) "_end, %object\n"     \
        "__" INCF_STR(name) "_start:\n"                 \
        SELECT_012_PARA(                                \
            INCFILE_OFFSET(file, 0),                    \
            INCFILE_OFFSET(file, offset),               \
            INCFILE_COUNT (file, offset, ##__VA_ARGS__, 0), \
            ##__VA_ARGS__)                              \
        "__" INCF_STR(name) "_end:\n"                   \
        ".word 0\n"                                     \
    )

#define INCFILE(name, file, ...)                        \
    INCFILE_PARA(name, file, ##__VA_ARGS__, 0);         \
    extern int __##name##_start;                        \
    extern int __##name##_end;

#define FILEPTR(name) ((void*)&(__##name##_start))

#define FILESIZE(name) ((char*)&(__##name##_end) - (char*)&(__##name##_start))
#else
#warning "Compiler can't support incfile"
#define INCFILE(name, file, ...)
#define FILEPTR(name)  ((void*)0)
#define FILESIZE(name) (0)
#endif

#endif
