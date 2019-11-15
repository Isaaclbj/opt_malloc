#include <stdint.h>
#include <sys/mman.h>
#include <assert.h>
#include <stdio.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>

#include "hmalloc.h"
/*
  typedef struct hm_stats {
  long pages_mapped;
  long pages_unmapped;
  long chunks_allocated;
  long chunks_freed;
  long free_length;
  } hm_stats;
*/

// bypass editor error
#ifndef MAP_ANON
#define MAP_ANON 0X20
#endif

typedef struct free_list_node {
  size_t size;
  struct free_list_node* next;
} free_list_node;

const size_t PAGE_SIZE = 4096;
static hm_stats stats; // This initializes the stats to 0.
static free_list_node* free_list;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;


void add_to_list(free_list_node* node);

long
free_list_length()
{
    long len = 0;
    free_list_node* curr = free_list;

    for(; curr; curr = curr->next) {
        len += 1;
    }
    return len;
}

hm_stats*
hgetstats()
{
    stats.free_length = free_list_length();
    return &stats;
}

void
hprintstats()
{
    int rv;
    rv = pthread_mutex_lock(&mutex);
    if(rv)
    {
        perror("print stats lock");
    }
    stats.free_length = free_list_length();
    fprintf(stderr, "\n== husky malloc stats ==\n");
    fprintf(stderr, "Mapped:   %ld\n", stats.pages_mapped);
    fprintf(stderr, "Unmapped: %ld\n", stats.pages_unmapped);
    fprintf(stderr, "Allocs:   %ld\n", stats.chunks_allocated);
    fprintf(stderr, "Frees:    %ld\n", stats.chunks_freed);
    fprintf(stderr, "Freelen:  %ld\n", stats.free_length);
    rv = pthread_mutex_unlock(&mutex);
    if(rv)
    {
        perror("print stats unlock");
    }
}

static
size_t
div_up(size_t xx, size_t yy)
{
    // This is useful to calculate # of pages
    // for large allocations.
    size_t zz = xx / yy;

    if (zz * yy == xx) {
        return zz;
    }
    else {
        return zz + 1;
    }
}

void*
hmalloc(size_t size)
{
    if(size <= 0)
    {
        return NULL;
    }
    int rv;
    rv = pthread_mutex_lock(&mutex);
    if(rv)
    {
        perror("failed to lock when malloc");
    }

    stats.chunks_allocated += 1;
    size += sizeof(size_t);

    // Actually allocate memory with mmap and a free list.

    // For requests with (B < 1 page = 4096 bytes):
    if (size < PAGE_SIZE) {
        free_list_node* large_block = NULL;
        free_list_node* prevNode = NULL;
        free_list_node* currNode = free_list;
        while (currNode)
        {
            // See if there's a big enough block on the free list.
            if (currNode->size >= size)
            {
                large_block = currNode;
                // If so, select the first one and remove it from the list.
                if (prevNode) {
                    prevNode->next = currNode->next;
                } else {
                    free_list = currNode->next;
                }
                break;
            }
            prevNode = currNode;
            currNode = currNode->next;
        }
        
        // If you don't have a block, mmap a new block (1 page = 4096).
        if (large_block == NULL) {
            large_block = mmap(0, PAGE_SIZE, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE, -1, 0);    
            assert(large_block != MAP_FAILED);
            stats.pages_mapped += 1;
            large_block->size = PAGE_SIZE;
        }
        // If the block is bigger than the request, and the leftover is big enough to store a free list cell
        if (large_block->size - size >= sizeof(free_list_node)) {    
            // return the extra to the free list
            free_list_node* extra = (free_list_node*) ((void*) large_block + size);
            extra->size = large_block->size - size;
            add_to_list(extra);
            large_block->size = size;
        }

        //unlock no.1
        rv = pthread_mutex_unlock(&mutex);
        if(rv)
        {
            perror("failed to unlock no.1");
        }

        // Return a pointer to the block after the size field.
        return (void*) large_block + sizeof(size_t);
    }
    // For requests with (B >= 1 page = 4096 bytes):
    else {
        // Calculate the number of pages needed for this block.
        size_t pages = div_up(size, PAGE_SIZE);
        // Allocate that many pages with mmap.
        // Fill in the size of the block as (# of pages * 4096).
        free_list_node* mem = mmap(0, pages * PAGE_SIZE, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE, -1, 0);
        assert(mem != MAP_FAILED);
    	mem->size = size;
	    mem->next = NULL;
        stats.pages_mapped += pages;

        //unlock no.2
        rv = pthread_mutex_unlock(&mutex);
        if(rv)
        {
            perror("failed to unlock no.2");
        }

        // Return a pointer to the block after the size field.
        return (void*) mem + sizeof(size_t);
    }

    // Did not reutrn a value for some reason
    exit(1);
}

void
add_to_list(free_list_node* node)
{
    if (free_list == NULL) {
        free_list = node;
        return;
    }

    free_list_node* prevNode = NULL;
    free_list_node* currNode = free_list;
    while (currNode != NULL)
    {
        if(node < currNode) {
            size_t prevNodeSize = 0;
            if (prevNode) {
                prevNodeSize = prevNode->size;
            }
            int leftSideSame = (void*) prevNode + prevNodeSize == node;
            int rightSideSame = (void*) node + node->size == currNode;
            // if leftNode -> node -> rightNode
            if (leftSideSame && rightSideSame) {
                prevNode->size = prevNodeSize + node->size + currNode->size;
                prevNode->next = currNode->next;
            }
            // if leftNode -> node
            else if (leftSideSame) {
                prevNode->size = prevNodeSize + node->size;
            }
            // if node -> rightNode
            else if (rightSideSame) {
                node->size = node->size + currNode->size;
                if (prevNode) {
                    prevNode->next = node;
                }
                node->next = currNode->next;
            }
            // if normal insert
            else {
                if (prevNode) {
                    prevNode->next = node;
                }
                // set the given node's next to the current node
                node->next = currNode;
            }

            if (prevNode == NULL) {
                free_list = node;
            }
            break;
        }
        prevNode = currNode;
        currNode = currNode->next;
    }
}

void
hfree(void* item)
{
    //lock
    int rv;
    rv = pthread_mutex_lock(&mutex);
    if(rv)
    {
        perror("failed to lock when freeing");
    }

    stats.chunks_freed += 1;
    if(!item){
        rv = pthread_mutex_unlock(&mutex);
        if(rv)
        {
            perror("failed to unlock when freeing NULL");
        }
        return;}
    // Actually free the item.

    // To free a block of memory, first find the beginning of the block by subtracting sizeof(size_t) from the provided pointer.
    free_list_node* node = (free_list_node*) (item - sizeof(size_t));
    // If the block is < 1 page then stick it on the free list.
    if (node->size < PAGE_SIZE) {
        add_to_list(node);
    }
    // If the block is >= 1 page, then munmap it.
    else {
        size_t pages = div_up(node->size, PAGE_SIZE);
        int rv = munmap((void*) node, node->size);
        assert(rv != -1);
        stats.pages_unmapped += pages;
    }
    //unlock
    rv = pthread_mutex_unlock(&mutex);
    if(rv)
    {
        perror("failed to unlock when freeing");
    }
}


void* hrealloc(void* old, size_t size)
{

    void* new = hmalloc(size);

    size_t* old_size_ptr = (void*) old - sizeof(size_t);
    size_t old_size = *old_size_ptr;
    if(old_size > size){
        memcpy(new, old, size);
    }
    else{
        memcpy(new, old, old_size);
    }
    memcpy(new, old, old_size);
    hfree(old);

    return new;
}
