#include <efi.h>

int simple_wcscmp(const CHAR16 *a, const CHAR16 *b)
{
    while (*a && (*a == *b)) {
        a++;
        b++;
    }

    return *a - *b;
}

int simple_wcsncmp(const CHAR16 *a, const CHAR16 *b, unsigned int n)
{
    while (n && *a && (*a == *b)) {
        a++;
        b++;
        n--;
    }

    if (n == 0)
        return 0;

    return *a - *b;
}