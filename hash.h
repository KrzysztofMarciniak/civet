#ifndef HASH_H
#define HASH_H

#include <stddef.h>

struct vfs_entry;

struct hash_entry {
	struct vfs_entry* vfs_entry;
	struct hash_entry* next;
};

struct hash_table {
	struct hash_entry** buckets;
	size_t capacity;
	size_t size;
};

struct hash_table* hash_table_create(size_t capacity);
void hash_table_destroy(struct hash_table* ht);

int hash_table_insert(struct hash_table* ht, const char* key,
                      struct vfs_entry* value);

struct vfs_entry* hash_table_lookup(const struct hash_table* ht,
                                    const char* key);

int hash_table_remove(struct hash_table* ht, const char* key);

#endif
