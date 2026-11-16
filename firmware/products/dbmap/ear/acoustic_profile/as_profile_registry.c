#include "as_profile.h"

/*
 * Compile-time registry. Add revised / new profiles by extending
 * registry_entry() and as_profile_count(). Profile data stays outside AE.
 */
static const as_profile_t *registry_entry(size_t i)
{
    switch (i) {
    case 0:
        return as_profile_as001();
    default:
        return NULL;
    }
}

size_t as_profile_count(void)
{
    return 1u;
}

const as_profile_t *as_profile_get_by_index(size_t index)
{
    if (index >= as_profile_count()) {
        return NULL;
    }
    return registry_entry(index);
}

const as_profile_t *as_profile_get_by_id(uint16_t id)
{
    if (id == AS_PROFILE_ID_NONE) {
        return NULL;
    }
    const size_t n = as_profile_count();
    for (size_t i = 0; i < n; i++) {
        const as_profile_t *p = registry_entry(i);
        if (p != NULL && p->id == id) {
            return p;
        }
    }
    return NULL;
}
