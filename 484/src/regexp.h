#ifndef REGEXP_H_
#define REGEXP_H_

#include "driver.h"

#define NSUBEXP  50
  /* Number of allowed () expressions.
   */

/* --- struct regexp: regular expression basis ---
 *
 * This structure is used to hold a compiled regular expression as well
 * intermediate results between two match calls.
 *
 * The structure is allocated big enough to fit the whole program
 * starting with .program.
 */

typedef struct regexp
{
    char *startp[NSUBEXP];
    char *endp[NSUBEXP];
      /* After a match, the start and endpointers for the matched
       * () expressions.
       */
    unsigned char regstart;
      /* Internal use: char that must begin a match, '\0' if non obvious
       */
    unsigned char reganch;
      /* Internal use: is the match anchored (at beginning-of-line only)?
       */
    unsigned char *regmust;
      /* Internal use: string (pointer into program) that match must
       * include, or NULL.
       */
    int regmlen;
      /* Internal  use: length of regmust.
       */
    long regalloc;          /* Allocated total length, used by rxcache */
    p_uint refs;            /* Number of refs, used+maintained by rxcache */
    Bool from_ed;           /* TRUE if compiled for ed() */
    unsigned char program[1];  /* The compiled regexp. */
} regexp;

/* --- Prototypes --- */
#ifdef DEBUG
extern Bool regnarrate;
void hs_regdump(regexp *rg);
#endif

extern regexp *hs_regcomp(unsigned char *expr, Bool excompat, Bool from_ed);
extern Bool hs_regexec(regexp *prog, char *string, char *start);
extern char *hs_regsub(regexp *prog, char *source, char *dest, int n, Bool quiet);
extern void hs_regerror(char *);

#endif /* REGEXP_H_ */