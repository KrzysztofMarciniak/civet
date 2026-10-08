#ifndef HASH_H
#define HASH_H

#include <stddef.h>

/* Simple hash table for storing vfs entries by URL path */

struct hash_entry {
	struct vfs_entry* vfs_entry;
	struct hash_entry* next;  /* collision chain */
};

struct hash_table {
	struct hash_entry** buckets;
	size_t capacity;
	size_t size;
};

/* Initialize hash table with given capacity */
struct hash_table* hash_table_create(size_t capacity);

/* Destroy hash table and free all memory */
void hash_table_destroy(struct hash_table* ht);

/* Insert or update an entry in the hash table */
int hash_table_insert(struct hash_table* ht, const char* key,
                      struct vfs_entry* value);

/* Look up an entry by key; returns NULL if not found */
struct vfs_entry* hash_table_lookup(const struct hash_table* ht,
                                    const char* key);

/* Remove an entry by key; returns 0 if found and removed, -1 if not found */
int hash_table_remove(struct hash_table* ht, const char* key);

#endif
