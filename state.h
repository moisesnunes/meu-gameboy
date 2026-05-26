#ifndef _GB_STATE_H_
#define _GB_STATE_H_

#include <stdbool.h>

struct gb;

bool gb_state_save(struct gb *gb, const char *path);
bool gb_state_load(struct gb *gb, const char *path);

#endif /* _GB_STATE_H_ */
