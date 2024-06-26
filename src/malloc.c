#include "../include/header.h"

extern Memory memory;
extern Mutex lock;

static Fixed* init_fixed(ushort size){
    Fixed* const area = mmap(NULL, memory.fixed_size,
                             PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (area == MAP_FAILED) return NULL;
    _set_env(INTERN_ALLOC, memory.fixed_size);

    area->size = size;
    size *= STACK_BUFF;
    pthread_mutex_lock(&lock.opt);
    area->memory = mmap(NULL, size, memory.opt.prot,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    pthread_mutex_unlock(&lock.opt);

    if (area->memory == MAP_FAILED){
        munmap(area, memory.fixed_size);
        _set_env(INTERN_FREED, memory.fixed_size);
        _set_env(INTERN_ALLOC, -memory.fixed_size);
        return NULL;
    }
    _set_env(ALLOC, size);
    area->memory_start = (void*)(((size_t)area->memory + 0xf)
                       & ~0xf);
    return area;
}

static void* get_fixed(Fixed* area, const ushort type,
                       const size_t size){
    while (area->in_use == STACK_BUFF - 1){
        if (!area->next){
            area->next = init_fixed(type);
            if (!area->next) return NULL;
            area->next->prev = area;
        }
        area = area->next;
    }
    const size_t index = area->next_ptr;
    area->used[index] = size;
    area->in_use++;

    if (!area->ptr[index]) area->ptr[index] = area->memory_start
                                            + area->size * index;
    pthread_mutex_lock(&lock.opt);
    if (memory.opt.bzero) ft_bzero(area->ptr[index], size);
    pthread_mutex_unlock(&lock.opt);
    _set_env(IN_USE, size);

    while (area->next_ptr < STACK_BUFF - 1
          && area->used[area->next_ptr]) area->next_ptr++;
    return area->ptr[index];
}

static int new_variable(Variable* const area, size_t size){
    const int index = area->next_ptr;
    const size_t used = size;

    if (size < (size_t)memory.page_size) size = memory.page_size;
    else size = memory.page_size * (size / memory.page_size + 1);

    pthread_mutex_lock(&lock.opt);
    area->memory[index] = mmap(NULL, size, memory.opt.prot,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (area->memory[index] == MAP_FAILED){
        pthread_mutex_unlock(&lock.opt);
        return -1;
    }
    if ((size_t)area->memory[index] % 2)
        area->memory_start[index] =
            (void*)(((size_t)area->memory[index] + 0xf) & ~0xf);
    else area->memory_start[index] = area->memory[index];

    if (memory.opt.bzero) ft_bzero(area->memory_start[index], used);
    pthread_mutex_unlock(&lock.opt);
    _set_env(ALLOC, size);
    _set_env(IN_USE, used);

    area->size[index] = size;
    area->used[index] = used;
    area->in_use++;

    while (area->next_ptr < BIG_STACK_BUFF
          && area->used[area->next_ptr]) area->next_ptr++;
    return index;
}

static void* split_variable(Variable* const area, const ushort src,
                            const size_t size, const ushort offset){
    const ushort dst = area->next_ptr;
    if (dst != src){
        area->memory[dst] = area->memory_start[src]
                          + area->used[src] + offset;
        area->memory_start[dst] = area->memory[dst];
        area->size[dst] = area->memory[src] + area->size[src]
                        - area->memory[dst];
        area->size[src] -= area->size[dst];
    }
    _set_env(IN_USE, size);
    area->used[dst] = size;
    area->in_use++;

    pthread_mutex_lock(&lock.opt);
    if (memory.opt.bzero) ft_bzero(area->memory_start[dst], size);
    pthread_mutex_unlock(&lock.opt);

    while (area->next_ptr < BIG_STACK_BUFF
          && area->used[area->next_ptr]) area->next_ptr++;
    return area->memory_start[dst];
}

static void* get_variable(size_t size){
    Variable* area = memory.variable;
    while (area->in_use == BIG_STACK_BUFF){
        if (!area->next){
            area->next = mmap(NULL, memory.variable_size,
                              PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (!area->next) return NULL;
            _set_env(INTERN_ALLOC, memory.variable_size);
            area->next->prev = area;
        }
        area = area->next;
    }
    for (ushort x = 0; x < BIG_STACK_BUFF; x++){
        if (!area->memory[x]) continue;
        const ushort offset = area->used[x] % 2 ? 1 : 0;

        if (area->used[x] + size + offset <= area->size[x]
            - (area->memory_start[x] - area->memory[x]))
            return split_variable(area, x, size, offset);
    }
    const int index = new_variable(area, size);
    return index < 0 ? NULL : area->memory_start[index];
}

void* malloc(size_t size){
    if (!size) return NULL;
    if (!memory.page_size) _init_memory();

    void* ptr;
    pthread_mutex_lock(&lock.opt);
    if (size <= (size_t)memory.opt.small){
        ushort type;
        Fixed** area;
        pthread_mutex_t* mptr;

        if (size <= (size_t)memory.opt.tiny){
            type = memory.opt.tiny;
            mptr = &lock.tiny;
            pthread_mutex_lock(mptr);
            area = &memory.tiny;
        }
        else{
            type = memory.opt.small;
            mptr = &lock.small;
            pthread_mutex_lock(mptr);
            area = &memory.small;
        }
        pthread_mutex_unlock(&lock.opt);
        if (!*area){
            *area = init_fixed(type);
            if (!*area){
                pthread_mutex_unlock(mptr);
                return NULL;
            }
        }
        ptr = get_fixed(*area, type, size);
        pthread_mutex_unlock(mptr);
    }
    else{
        pthread_mutex_unlock(&lock.opt);
        pthread_mutex_lock(&lock.variable);
        if (!memory.variable){
            memory.variable = mmap(NULL, memory.variable_size,
                                   PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS,
                                   -1, 0);
            if (!memory.variable) return NULL;
            _set_env(INTERN_ALLOC, memory.variable_size);
        }
        ptr = get_variable(size);
        pthread_mutex_unlock(&lock.variable);
    }
    if (!ptr) errno = ENOMEM;
    return ptr;
}
