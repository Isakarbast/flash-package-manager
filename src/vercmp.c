/* Version comparison with the same ordering as pacman/rpm:
 *   1.0a < 1.0 < 1.0.a < 1.0.1 < 1.1,  010 == 10,  numbers beat letters. */
#include "fpm.h"

#include <ctype.h>
#include <string.h>

#define DIGIT(c) isdigit((unsigned char)(c))
#define ALPHA(c) isalpha((unsigned char)(c))
#define ALNUM(c) isalnum((unsigned char)(c))

int vercmp(const char *a, const char *b)
{
    if (strcmp(a, b) == 0)
        return 0;
    while (*a && *b) {
        while (*a && !ALNUM(*a))
            a++;
        while (*b && !ALNUM(*b))
            b++;
        if (!*a || !*b)
            break;
        int na = DIGIT(*a) != 0, nb = DIGIT(*b) != 0;
        if (na != nb)
            return na ? 1 : -1;
        const char *sa = a, *sb = b;
        if (na) {
            while (DIGIT(*a))
                a++;
            while (DIGIT(*b))
                b++;
            while (*sa == '0' && sa + 1 < a)
                sa++;
            while (*sb == '0' && sb + 1 < b)
                sb++;
            size_t la = (size_t)(a - sa), lb = (size_t)(b - sb);
            if (la != lb)
                return la > lb ? 1 : -1;
            int r = strncmp(sa, sb, la);
            if (r)
                return r > 0 ? 1 : -1;
        } else {
            while (ALPHA(*a))
                a++;
            while (ALPHA(*b))
                b++;
            size_t la = (size_t)(a - sa), lb = (size_t)(b - sb);
            int r = strncmp(sa, sb, la < lb ? la : lb);
            if (r)
                return r > 0 ? 1 : -1;
            if (la != lb)
                return la > lb ? 1 : -1;
        }
    }
    if (!*a && !*b)
        return 0;
    /* One side has more left. Letters glued straight on ("1.0rc1") mean a
     * pre-release, so that side is older; anything else ("1.0.1") is newer. */
    if (!*a)
        return ALPHA(*b) ? 1 : -1;
    return ALPHA(*a) ? -1 : 1;
}

int vrcmp(const char *va, int ra, const char *vb, int rb)
{
    int r = vercmp(va, vb);
    if (r)
        return r;
    return (ra > rb) - (ra < rb);
}
