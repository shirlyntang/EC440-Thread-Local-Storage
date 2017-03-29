#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include "tls.h"
#include <semaphore.h>

#define PAGE_SIZE getpagesize()

void initialize_signal_handler(void);
void tls_handle_page_fault(int sig, siginfo_t *si, void *context);

// TLS struct and Page struct
struct tls {
	pthread_t tid;
	unsigned int size; 		// bytes
	unsigned int page_num; 	// pages
	struct page** pages;	// array of pointers to pages
};

struct page {
	unsigned int address; 	// start of page
	int ref_count;			// how many threads share this page
};

static struct tls all_tls[128];
static sem_t sem;
int first = 0;

// Create local storage for a certain thread
int tls_create(unsigned int size)
{	
	// Set up signal handler when first local storage is created
	if(first == 0)
	{
		initialize_signal_handler();
		first = 1;
	}

	sem_wait(&sem);

	// Check for error conditions
	if(size <= 0)
	{
		sem_post(&sem);
		return -1;
	}
	int ii;
	for(ii = 0; ii<128; ii++)
	{
		if (all_tls[ii].tid == pthread_self())
		{
			sem_post(&sem);
			return -1;
		}
	}

	// Find a spot for the thread in the TLS array
	struct tls* storage;
	for(ii = 0; ii<128; ii++)
	{
		if (all_tls[ii].tid == 0)
		{
			storage = &all_tls[ii];
			break;
		}
	}

	// Prepare to allocate memory
	storage->tid = pthread_self();								// Set tls to be for current thread
	storage->size = size;
	int new_pages = (size+PAGE_SIZE-1)/PAGE_SIZE;				// Get number of pages
	storage->pages = malloc(sizeof(struct page*)*new_pages);	// Create array for page addresses
	storage->page_num = new_pages;

	// Allocate memory and pages
	for(ii = 0; ii<new_pages; ii++)
	{
		void* addr = NULL;
		addr = mmap(NULL, PAGE_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		storage->pages[ii]=malloc(sizeof(struct page));
		storage->pages[ii]->address = (int) addr;
		storage->pages[ii]->ref_count = 1;
	}

	sem_post(&sem);
	return 0;
}

// Writes to local storage
int tls_write(unsigned int offset, unsigned int length, char *buffer)
{
	sem_wait(&sem);

	// Get current thread's spot in TLS array
	int ii;
	struct tls* storage = NULL;
	for(ii = 0; ii<128; ii++)
	{
		if (all_tls[ii].tid == pthread_self())
		{
			storage = &all_tls[ii];
			break;
		}
	}

	// Check for error conditions
	if(storage==NULL)
	{
		sem_post(&sem);
		return -1;
	}
	if(offset+length > storage->size)
	{
		sem_post(&sem);
		return -1;
	}

	// Prepare to write
	int offset_pages = offset/PAGE_SIZE;
	int offset_bytes = offset%PAGE_SIZE;
	int length_pages = (offset_bytes+length+PAGE_SIZE-1)/PAGE_SIZE;
	int length_copied = 0;

	// Check to see if any of the pages are shared
	// - Create a new page if so
	// Then unprotect and write to the page
	for(ii = offset_pages; ii<offset_pages+length_pages; ii++)
	{
		// Unprotect the current page
		if(mprotect((void*) storage->pages[ii]->address, PAGE_SIZE, PROT_WRITE) == -1)
		{
			sem_post(&sem);
			return -1;
		}

		// Check if page is in use, if so...
		if(storage->pages[ii]->ref_count > 1)
		{
			// Update old page
			storage->pages[ii]->ref_count--;

			// Allocate new space for page and copy data
			void* addr = NULL;
			addr = mmap(NULL, PAGE_SIZE, PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
			memcpy(addr, (const void*) storage->pages[ii]->address, PAGE_SIZE);
			if(mprotect((void*) storage->pages[ii]->address, PAGE_SIZE, PROT_NONE) == -1)
			{
				sem_post(&sem);
				return -1;
			}

			// Update current page pointer with the new page
			storage->pages[ii]=malloc(sizeof(struct page));
			storage->pages[ii]->ref_count = 1;
			storage->pages[ii]->address = (int) addr;
		}

		// Write to memory
		if(length_pages == 1)											// Only one page to be written to total
		{
			int dest_addr = storage->pages[ii]->address + offset_bytes;
			int src_addr = (int) buffer;
			memcpy((void*) dest_addr, (void*) src_addr, length);		// Copy all the bytes
		}
		else if(ii == offset_pages) 									// First page to be written
		{
			int dest_addr = storage->pages[ii]->address + offset_bytes; 
			int src_addr = (int) buffer;
			memcpy((void*) dest_addr, (void*) src_addr, PAGE_SIZE-offset_bytes); 	// Copy as many bits that can fit
			length_copied = PAGE_SIZE-offset_bytes;
		}
		else if(ii == offset_pages+length_pages-1)						// Last page to be written
		{
			int dest_addr = storage->pages[ii]->address;
			int src_addr = (int) buffer+length_copied;
			memcpy((void*) dest_addr, (void*) src_addr, length-length_copied);		// Copy the last bits in the buffer
		}
		else															// Middle page
		{
			int dest_addr = storage->pages[ii]->address;
			int src_addr = (int) buffer+length_copied;
			memcpy((void*) dest_addr, (void*) src_addr, PAGE_SIZE);		// Copy a full page
			length_copied+=PAGE_SIZE;							
		}

		// Reprotect the current page
		if(mprotect((void*) storage->pages[ii]->address, PAGE_SIZE, PROT_NONE) == -1)
		{
			sem_post(&sem);
			return -1;
		}
	}

	sem_post(&sem);
	return 0;
}

// Read from local storage
int tls_read(unsigned int offset, unsigned int length, char *buffer)
{
	sem_wait(&sem);

	// Get current thread's spot in TLS array
	int ii;
	struct tls* storage = NULL;
	for(ii = 0; ii<128; ii++)
	{
		if (all_tls[ii].tid == pthread_self())
		{
			storage = &all_tls[ii];
			break;
		}
	}

	// Check for error conditions
	if(storage==NULL)
	{
		sem_post(&sem);
		return -1;
	}
	if(offset+length > storage->size)
	{
		sem_post(&sem);
		return -1;
	}

	// Prepare to write
	int offset_pages = offset/PAGE_SIZE;
	int offset_bytes = offset%PAGE_SIZE;
	int length_pages = (offset_bytes+length+PAGE_SIZE-1)/PAGE_SIZE;
	int length_copied = 0;

	// Preform read
	for(ii = offset_pages; ii < offset_pages+length_pages; ii++)
	{
		// Unprotect the current page for reading
		if(mprotect((void*) storage->pages[ii]->address, PAGE_SIZE, PROT_READ) == -1)
		{
			sem_post(&sem);
			return -1;
		}

		// Write to memory
		if(length_pages == 1)											// Only one page to be read from total
		{
			int src_addr = storage->pages[ii]->address + offset_bytes;
			int dest_addr = (int) buffer;
			memcpy((void*) dest_addr, (void*) src_addr, length);		// Copy all the bytes
		}
		else if(ii == offset_pages) 									// First page to be written
		{
			int src_addr = storage->pages[ii]->address + offset_bytes; 				// Prep address of new location
			int dest_addr = (int) buffer;
			memcpy((void*) dest_addr, (void*) src_addr, PAGE_SIZE-offset_bytes); 	// Copy as many bits that can fit
			length_copied = PAGE_SIZE-offset_bytes;
		}
		else if(ii == offset_pages+length_pages-1)						// Last page to be written
		{
			int src_addr = storage->pages[ii]->address;
			int dest_addr = (int) buffer+length_copied;
			memcpy((void*) dest_addr, (void*) src_addr, length-length_copied);		// Copy the last bits in the buffer
		}
		else															// Middle page
		{
			int src_addr = storage->pages[ii]->address;
			int dest_addr = (int) buffer+length_copied;
			memcpy((void*) dest_addr, (void*) src_addr, PAGE_SIZE);		// Copy a full page
			length_copied+=PAGE_SIZE;							
		}

		// Reprotect the current page
		if(mprotect((void*) storage->pages[ii]->address, PAGE_SIZE, PROT_NONE) == -1)
		{
			sem_post(&sem);
			return -1;
		}
	}

	sem_post(&sem);
	return 0;
}

// Deletes a threads TLS entry and memory with it
// - Does not delete memory shared by other threads
int tls_destroy()
{
	sem_wait(&sem);

	// Get current thread's spot in TLS array
	int ii;
	struct tls* storage = NULL;
	for(ii = 0; ii<128; ii++)
	{
		if (all_tls[ii].tid == pthread_self())
		{
			storage = &all_tls[ii];
			break;
		}
	}

	// Error checking
	if(storage == NULL)
	{
		sem_post(&sem);
		return -1;
	}

	// Clears all the pages in the thread's local storage
	for(ii = 0; ii < storage->page_num; ii++)
	{
		// Check to see if page is shared by another thread
		if(storage->pages[ii]->ref_count == 1)
		{
			// If not used by another thread, delete the memory itself
			void* addr = (void*) storage->pages[ii]->address;
			munmap(addr, PAGE_SIZE);

			// Free the page entry
			free(storage->pages[ii]);
		}
		else
		{
			storage->pages[ii]->ref_count--;
		}
	}

	// After all pages are freed, clear TLS
	storage->tid = 0;
	storage->size = 0;
	storage->page_num = 0;
	free(storage->pages);
	storage->pages = NULL;

	sem_post(&sem);
	return 0;
}

// Clone a thread's pages
int tls_clone(pthread_t tid)
{
	sem_wait(&sem);

	// Error check to see if current thread already has local storage. If yes, return -1
	int ii;
	for(ii = 0; ii<128; ii++)
	{
		if (all_tls[ii].tid == pthread_self())
		{
			sem_post(&sem);
			return -1;
		}
	}

	// Get target thread's spot in TLS array
	struct tls* target = NULL;
	for(ii = 0; ii<128; ii++)
	{
		if (all_tls[ii].tid == tid)
		{
			target = &all_tls[ii];
			break;
		}
	}
	if(target == NULL)
	{
		sem_post(&sem);
		return -1;
	}

	// Find a spot for the thread in the TLS array
	struct tls* storage;
	for(ii = 0; ii<128; ii++)
	{
		if (all_tls[ii].tid == 0)
		{
			storage = &all_tls[ii];
			break;
		}
	}

	// Prepare to clone memory, fill TLS and prep page references
	storage->tid = pthread_self();
	storage->size = target->size;
	storage->page_num = target->page_num;
	storage->pages = malloc(sizeof(struct page*)*storage->page_num);


	// Copy page pointers thus "cloning" memory
	for(ii = 0; ii < storage->page_num; ii++)
	{
		storage->pages[ii] = target->pages[ii];
		storage->pages[ii]->ref_count++;
	}

	sem_post(&sem);
	return 0;
}

// Initialize signal handler
void initialize_signal_handler()
{
	// Initialize array
	int ii;
	for(ii = 0; ii<128; ii++)
	{
		all_tls[ii].tid = 0;
	}

	// Initialize semaphore for guarding critical areas during execution
	sem_init(&sem, 0, 1);

	// Initialize Signal Handler
	struct sigaction sigact;
	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = SA_SIGINFO;
	sigact.sa_sigaction = tls_handle_page_fault;

	sigaction(SIGBUS, &sigact, NULL);
	sigaction(SIGSEGV, &sigact, NULL);
}

// Signal handler
void tls_handle_page_fault(int sig, siginfo_t *si, void *context)
{
	int p_fault = ((unsigned int) si->si_addr) & ~(PAGE_SIZE - 1);

	int ii;
	for(ii=0; ii < 128; ii++)
	{
		int jj;
		for(jj=0; jj < all_tls[ii].page_num; jj++)
		{
			if(all_tls[ii].pages[jj]->address == p_fault)
			{
				tls_destroy();
				pthread_exit(NULL);
			}
		}
	}

	signal(SIGSEGV, SIG_DFL);
	signal(SIGBUS, SIG_DFL);
	raise(sig);
}

int get_address()
{
	int ii;
	struct tls* storage = NULL;
	for(ii = 0; ii<128; ii++)
	{
		if (all_tls[ii].tid == pthread_self())
		{
			storage = &all_tls[ii];
			break;
		}
	}
	if(storage == NULL)
		return -1;

	return storage->pages[0]->address;
}

void print_tls()
{
	int ii;
	for(ii=0; ii < 10; ii++)
	{
		printf("II %d => PAGES: %d\n", ii, all_tls[ii].page_num);
		int jj;
		for(jj=0; jj < all_tls[ii].page_num; jj++)
		{
			printf("== PAGE: %d => %u\n", jj, all_tls[ii].pages[jj]->address);
		}
	}
}