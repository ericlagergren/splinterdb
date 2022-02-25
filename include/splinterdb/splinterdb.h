// Copyright 2018-2021 VMware, Inc.
// SPDX-License-Identifier: Apache-2.0

/*
 * splinterdb.h --
 *
 *     Lower-level key/message API for SplinterDB.
 *
 *     An application using this API must provide a
 *     data_config that encodes values into messages.
 *
 *     For simple use cases, start with splinterdb_kv,
 *     which provides a key/value abstraction.
 */

#ifndef _SPLINTERDB_H_
#define _SPLINTERDB_H_

#include "splinterdb/data.h"


// Hack to accomodate encoding variable-length keys
#define SPLINTERDB_MAX_KEY_SIZE (MAX_KEY_SIZE - 1)

const char *
splinterdb_get_version();

typedef struct {
   const char *filename;
   uint64      cache_size;
   uint64      disk_size;

   data_config data_cfg;

   void *heap_handle;
   void *heap_id;

   // advanced config
   // if unset, defaults will be used
   uint64 page_size;
   uint64 extent_size;

   // io
   int    io_flags;
   uint32 io_perms;
   uint64 io_async_queue_depth;

   // cache
   bool        cache_use_stats;
   const char *cache_logfile;

   // btree
   uint64 btree_rough_count_height;

   // filter
   uint64 filter_remainder_size;
   uint64 filter_index_size;

   // log
   bool use_log;

   // splinter
   uint64 memtable_capacity;
   uint64 fanout;
   uint64 max_branches_per_node;
   uint64 use_stats;
   uint64 reclaim_threshold;
} splinterdb_config;

typedef struct splinterdb splinterdb;

// Create a new splinterdb from a given config
//
// The library will allocate and own the memory for splinterdb
// and will free it on splinterdb_close().
//
// It is ok for the caller to stack-allocate cfg, since it is not retained
int
splinterdb_create(const splinterdb_config *cfg, splinterdb **kvs);

// Open an existing splinterdb, using the provided config
//
// The library will allocate and own the memory for splinterdb
// and will free it on splinterdb_close().
//
// It is ok for the caller to stack-allocate cfg, since it is not retained
int
splinterdb_open(const splinterdb_config *cfg, splinterdb **kvs);

// Close a splinterdb
//
// This will flush everything to disk and release all resources
void
splinterdb_close(splinterdb *kvs);

// Register the current thread so that it can be used with the splinterdb.
// This causes scratch space to be allocated for the thread.
//
// Any thread that uses a splinterdb must first be registered with it.
//
// The only exception is the initial thread which called create or open,
// as that thread is implicitly registered.  Re-registering it will leak memory.
//
// A thread should not be registered more than once; that would leak memory.
//
// splinterdb_close will use scratch space, so the thread that calls it must
// have been registered (or implicitly registered by being the initial thread).
//
// Note: There is currently a limit of MAX_THREADS registered at a given time
void
splinterdb_register_thread(splinterdb *kvs);

// Deregister the current thread and free its scratch space.
//
// Call this function before exiting a registered thread.
// Otherwise, you'll leak memory.
void
splinterdb_deregister_thread(splinterdb *kvs);

// Insert a key and value.
// Relies on data_config->encode_message
int
splinterdb_insert_value(const splinterdb *kvsb,
                        size_t            key_len,
                        const char       *key,
                        size_t            val_len,
                        const char       *value);

// Insert a custom message at the given key
int
splinterdb_insert_message(const splinterdb *kvs,
                          size_t            key_length,
                          const char       *key,
                          size_t            message_length,
                          const char       *message);


// Delete a given key and associated value
int
splinterdb_delete(const splinterdb *kvsb, size_t key_len, const char *key);


/**************
 * Lookups
 **************/

/*
 * Lookup result buffer is sized by 6 8-byte fields, defined elsewhere.
 */
#define SPLINTERDB_LOOKUP_BUFSIZE (6 * sizeof(void *))

/* Lookup results are stored in a splinterdb_lookup_result.
 */
typedef struct {
   char opaque[SPLINTERDB_LOOKUP_BUFSIZE];
} splinterdb_lookup_result;

/*
 * Initialize a lookup result. If buffer is provided, then the result
 * of a query will be stored in buffer if it fits. Otherwise, it will
 * be stored in an allocated buffer.
 *
 * A splinterdb_lookup_result can be used for multiple queries after
 * being initialized once.
 */
void
splinterdb_lookup_result_init(const splinterdb         *kvs,        // IN
                              splinterdb_lookup_result *result,     // IN/OUT
                              size_t                    buffer_len, // IN
                              char                     *buffer      // IN
);

/*
 * Release any resources used by result. Note that this does _not_ deallocate
 * the buffer provided to splinterdb_lookup_result_init().
 */
void
splinterdb_lookup_result_deinit(splinterdb_lookup_result *result); // IN

_Bool
splinterdb_lookup_result_found(splinterdb_lookup_result *result); // IN

size_t
splinterdb_lookup_result_size(splinterdb_lookup_result *result); // IN

void *
splinterdb_lookup_result_data(splinterdb_lookup_result *result); // IN

// Lookup the message for a given key
int
splinterdb_lookup_message(const splinterdb         *kvs,        // IN
                          size_t                    key_length, // IN
                          const char               *key,        // IN
                          splinterdb_lookup_result *result      // IN/OUT
);

// Lookup the value for a given key
//
// Callers:
// - Set val_max_len to the size of the val buffer
//
// Upon return:
// - *found will be TRUE iff the key is present in the database.
//
// - If *found, then val will contain the first *val_bytes of the
//   value associated with key.
//
// - *val_truncated will be true if the size of the value associated
//   with key is larger than *val_bytes.
int
splinterdb_lookup_value(const splinterdb *kvsb,
                        const size_t      key_len,        // IN
                        const char       *key,            // IN
                        size_t            val_buffer_len, // IN
                        char             *out_val_buffer, // OUT
                        size_t           *val_bytes,      // OUT
                        _Bool            *val_truncated,  // OUT
                        _Bool            *found           // OUT
);

/*
SplinterDB Key/Message Iterator API

This API is modeled after the RocksDB Iterator and is intended to allow drop-in
replacement for most scenarios.

This documentation is heavily inspired by
  https://github.com/facebook/rocksdb/wiki/Iterator

The biggest difference is that RocksDB's Iterator exposes a Seek(key) method,
while SplinterDB expects the starting key to be provided at the time the
iterator is initialized.

Similar to RocksDB, if there is no error, then status()==0.  If status() != 0,
then valid() == false.  In other words, valid()==true implies status()== 0,
which means it is safe to proceed with other operations without checking
status().

On the other hand, if valid() == false, then there are two possibilities:
(1) We reached the end of the data without error. In this case, status() == 0;
(2) there is an error. In this case status() != 0;

It is always a good practice to check status() if the iterator is invalidated.

Iterators must be cleaned up via deinit.  Live iterators will block
other operations from the same thread, including close.

Known issue: a live iterator may block inserts and deletes from the same thread.


Sample application code:

   splinterdb_iterator* it;
   int rc = splinterdb_iterator_init(kvs, &it, 0, NULL);
   if (rc != 0) { ... handle error ... }

   const char* key;
   const char* msg;
   for(; splinterdb_iterator_valid(it); splinterdb_iterator_next(it)) {
      splinterdb_iterator_get_current(it, &key, &msg);
      // read key and msg ...
   }
   // loop exit may mean error, or just that we've reached the end of the range
   rc = splinterdb_iterator_status(it);
   if (rc != 0) { ... handle error ... }

   // Release resources acquired by the iterator
   splinterdb_iterator_deinit(it);
*/

typedef struct splinterdb_iterator splinterdb_iterator;

int
splinterdb_iterator_init(const splinterdb     *kvs,              // IN
                         splinterdb_iterator **iter,             // OUT
                         size_t                start_key_length, // IN
                         const char           *start_key         // IN
);

void
splinterdb_iterator_deinit(splinterdb_iterator *iter);

// Checks that the iterator status is OK (no errors) and that get_current()
// will succeed. If false, there are two possibilities:
// 1. Iterator has passed the final item.  In this case, status() == 0
// 2. Iterator has encountered an error.  In this case, status() != 0
bool
splinterdb_iterator_valid(splinterdb_iterator *iter);

// Attempts to advance the iterator to the next item.
// Any error will cause valid() == false and be visible with status()
void
splinterdb_iterator_next(splinterdb_iterator *iter);

// Sets *key and *message to the locations of the current item.
// Callers must not modify that memory
//
// If valid() == false, then behavior is undefined.
// Always check valid() before calling this function.
void
splinterdb_iterator_get_current_message(splinterdb_iterator *iter,       // IN
                                        size_t              *key_length, // OUT
                                        const char         **key,        // OUT
                                        size_t      *message_length,     // OUT
                                        const char **message             // OUT
);

// Sets *key and *value to the locations of the current item
// Callers must not modify that memory
//
// If valid() == false, then behavior is undefined.
// Always check valid() before calling this function.
void
splinterdb_iterator_get_current_value(splinterdb_iterator *iter,    // IN
                                      size_t              *key_len, // OUT
                                      const char         **key,     // OUT
                                      size_t              *val_len, // OUT
                                      const char         **value    // OUT
);

// Returns an error encountered from iteration, or 0 if successful.
//
// End-of-range is not an error
int
splinterdb_iterator_status(const splinterdb_iterator *iter);

#endif // _SPLINTERDB_H_
