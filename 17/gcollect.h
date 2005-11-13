#ifndef __GCOLLECT_H__
#define __GCOLLECT_H__

#include "driver.h"

#include "exec.h"       /* struct program */
#include "interpret.h"  /* struct svalue */
#include "object.h"     /* struct object */

#if defined(MALLOC_smalloc)

/* --- Variables --- */
extern int gcollect_outfd;
extern time_t time_last_gc;
extern int garbage_collection_in_progress;
extern struct object *gc_obj_list_destructed;
extern struct lambda *stale_misc_closures;
extern struct mapping *stale_mappings;


/* --- Prototypes --- */
extern void clear_memory_reference PROT((char *p));
extern void clear_inherit_ref PROT((struct program *p));
extern void mark_program_ref PROT((struct program *p));
extern void reference_destructed_object PROT((struct object *ob));
extern void note_malloced_block_ref PROT((char *p));
extern void count_ref_from_string PROT((char *p));
extern void count_ref_in_vector PROT((struct svalue *svp, int num));
extern void clear_ref_in_vector PROT((struct svalue *svp, int num));

#endif /* MALLOC_smalloc */

extern void garbage_collection PROT((void));
extern int32 renumber_programs PROT((void));
extern void setup_print_block_dispatcher PROT((void));

#endif /* __GCOLLECT_H__ */