#include <stdlib.h>
#include <sys/mman.h>
#include <stdio.h>
#include <assert.h>
#include <pthread.h>
#include <string.h>

#include "hmalloc.h"
/*
    IDEAS:
        use 8, 16, 32, 64, 128, 256, 512, 1024, 2048
        9 bins
        if a user asks for 18, they will get 32. ask for 63 get 64

        dont coalesce
        according to man munmap, the region is automatically unmapped
        when the process is terminated. (dont worry about unmapping)
        header is a (size_t)
*/

// bypass editor error
#ifndef MAP_ANON
#define MAP_ANON 0X20
#endif
// -------------------

static const int BASE_SIZE = 8;
static const int BIN_NUMBER = 10;
static const size_t PAGE_SIZE = 4096;
static const size_t LARGE_CHUNK = 64 * PAGE_SIZE;
static const size_t SMALL_CHUNK = 4 * PAGE_SIZE;
static const size_t OSS = 256 * PAGE_SIZE;
static pthread_mutex_t lock;

typedef struct free_list {
  struct free_list* next;
} free_list;

// an array of integers, indicating how much memory
// we have left
static __thread int leftover[10];
static __thread struct free_list* bins[10];

// store some space
static __thread void* galaxy;
static __thread long galaxy_size;

//this is where we store a ton of pages
static void* universe;
static long universe_size;

void universe_init()
{
    universe = mmap(0, OSS, PROT_READ|PROT_WRITE, 
    MAP_ANON|MAP_PRIVATE, -1, 0);
    universe_size = OSS;
}

void* ask_universe()
{
    if(universe_size < LARGE_CHUNK){
        universe_init();
    }
    void* ret = universe;
    universe_size -= LARGE_CHUNK;
    universe += LARGE_CHUNK;
    return ret;
}

void galaxy_init()
{
    int rv;
    rv = pthread_mutex_lock(&lock);
    if(rv){perror("locking galaxy get");}
    galaxy = ask_universe();
    galaxy_size = LARGE_CHUNK;  
    rv = pthread_mutex_unlock(&lock);
    if(rv){perror("unlocking galaxy get");}
}

void* ask_galaxy(size_t size)
{
    if(galaxy_size < size){
        galaxy_init();
    }
    void* ret = galaxy;
    galaxy_size -= size;
    galaxy += size;
    return ret;
}

//initialize the local bin
void bin_init(int bin_num)
{
    size_t ss = BASE_SIZE;
    for(int ii = 0; ii< bin_num; ii++){
        ss *= 2;
    }
    leftover[bin_num] = SMALL_CHUNK;
    bins[bin_num] = ask_galaxy(leftover[bin_num]);
    free_list* temp = bins[bin_num];

    for(int xx = 0; xx < ((leftover[bin_num] / ss) - 1); xx++)
    {
        free_list* temp_next = (((void*)temp) + ss);
        temp->next = temp_next;
        temp = temp->next;
    }
}

// find which bin [size] belongs to
int find_bin_num(size_t size)
{
    size_t cur = BASE_SIZE;
    for(int ii = 0; ii < BIN_NUMBER; ii++)
    {
        if(size <= cur)
        {
            return ii;
        }
        cur *= 2;
    }
    perror("shouldn't be here: find_bin_num");
    exit(1);
}

// check if given bin has enough space,
// if not, re-init it
// give out space
free_list* bin_get(int bin_num)
{
    size_t ss = BASE_SIZE;
    free_list* ret;
    for(int ii = 0; ii<bin_num; ii++){
        ss *= 2;
    }

    if(leftover[bin_num] < ss)
    {
        bin_init(bin_num);
    }

    leftover[bin_num] -= ss;
    ret = bins[bin_num];
    //point to next block
    bins[bin_num] = bins[bin_num]->next;
    return ret;
}

// recycle the space by putting it back into the bin
void bin_put(size_t* pointer)
{
    pointer --;
    size_t size = (*pointer);
    int bin_num = find_bin_num(size);
    ((free_list*)pointer)->next = bins[bin_num];
    bins[bin_num] = (free_list*)pointer;
    leftover[bin_num] += size;
}

void* opt_malloc(size_t size)
{
    int rv;
    if(size <= 0)
    {
        return NULL;
    }
    size += sizeof(size_t);
    if(size >= PAGE_SIZE)
    {
        int pages = size/PAGE_SIZE;
        if(size%PAGE_SIZE) {pages++;}
        size_t* ret = ask_galaxy(pages*PAGE_SIZE);
        *ret = pages * PAGE_SIZE;
        return (void*)(ret + 1);
    }
    
    int bin_num = find_bin_num(size);
    size_t* ret = (size_t*)bin_get(bin_num);
    //upscale it to be a multiple of BASE_SIZE
    size_t node = BASE_SIZE;
    for (int ii =0; ii < bin_num; ii++){
        node *= 2;
    }
    
    *ret = node;
    return (void*) (ret + 1);
}

void opt_free(void* pointer)
{
    int rv;
    if(!pointer)
    {
        return;
    }

    size_t * node = (size_t*)(pointer - sizeof(size_t));
    size_t size = *node;
    if(size > PAGE_SIZE)
    {
        rv = munmap(node, size);
        if(rv){perror("somethings wrong: large munmap");}
        return;
    }
    // if we are freeing a small block        
    bin_put(pointer);
}

void* opt_realloc(void* pointer, size_t new_size)
{

    if(new_size <= 0)
    {
        opt_free(pointer);
        return NULL;
    }
    size_t* node = pointer - sizeof(size_t);
    size_t old_size = *(node);
    
    // only malloc when you absolutely have to.
    void* ret = opt_malloc(new_size);
    if(old_size > new_size)
    {
        return pointer;
    }
    else{
        void* ret = opt_malloc(new_size);
        memcpy(ret, pointer, old_size);
        opt_free(pointer);
    }
    return ret;
}
