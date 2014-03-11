#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "block-store.h"
#include "hash.h"
#include "config.h"
#include "file.h"
#include "util.h"
#include "hashtable.h"
#include "bitc_ui.h"
#include "peergroup.h"
#include "bitc.h"

#define LGPFX "BLCK:"


struct block_cpt_entry_str {
   uint32       height;
   const char  *hashStr;
};


struct block_cpt_entry {
   uint32       height;
   uint256      hash;
};


static const struct block_cpt_entry_str cpt_testnet[] = {
   {      0, "000000000933ea01ad0ee984209779baaec3ced90fa3f408719526f8d77f4943" },
};

static const struct block_cpt_entry_str cpt_main[] = {
   {      0, "000000000019d6689c085ae165831e934ff763ae46a2a6c172b3f1b60a8ce26f" },
   {  11111, "0000000069e244f73d78e8fd29ba2fd2ed618bd6fa2ee92559f542fdb26e7c1d" },
   {  33333, "000000002dd5588a74784eaa7ab0507a18ad16a236e7b1ce69f00d7ddfb5d0a6" },
   {  74000, "0000000000573993a3c9e41ce34471c079dcf5f52a0e824a81e7f953b8661a20" },
   { 105000, "00000000000291ce28027faea320c8d2b054b2e0fe44a773f3eefb151d6bdc97" },
   { 134444, "00000000000005b12ffd4cd315cd34ffd4a594f430ac814c91184a0d42d2b0fe" },
   { 168000, "000000000000099e61ea72015e79632f216fe6cb33d7899acb35b75c8303b763" },
   { 193000, "000000000000059f452a5f7340de6682a977387c17010ff6e6c3bd83ca8b1317" },
   { 210000, "000000000000048b95347e83192f69cf0366076336c639f9b7228e9ba171342e" },
   { 216116, "00000000000001b4f4b433e81ee46494af945cf96014816a4e2370f11b23df4e" },
   { 225430, "00000000000001c108384350f74090433e7fcf79a606b8e797f065b130575932" },
   { 250000, "000000000000003887df1f29024b06fc2200b55f8af8f35453d7be294df2d214" },
   { 275000, "00000000000000044750d80a0d3f3e307e54e8802397ae840d91adc28068f5bc" },
};

static struct block_cpt_entry block_cpt_testnet[ARRAYSIZE(cpt_testnet)];
static struct block_cpt_entry block_cpt_main[ARRAYSIZE(cpt_main)];

struct blockentry {
   struct blockentry   *prev;
   struct blockentry   *next;
   btc_block_header     header;
   int                  height;
   bool                 written;
};


struct blockset {
   char                   *filename;
   struct file_descriptor *desc;
   int64                   filesize;
};


struct blockstore {
   struct blockset       *blockSet;
   uint256                genesis_hash;
   uint256                best_hash;

   struct blockentry     *best_chain;
   struct blockentry     *genesis;

   int                    height;
   struct hashtable      *hash_blk;
   struct hashtable      *hash_orphans;
};


/*
 *------------------------------------------------------------------------
 *
 * blockstore_lookup --
 *
 *------------------------------------------------------------------------
 */

static struct blockentry *
blockstore_lookup(const struct blockstore *bs,
                  const uint256           *hash)
{
   struct blockentry *be;
   bool s;

   s = hashtable_lookup(bs->hash_orphans, hash, sizeof *hash, (void *)&be);
   if (s) {
      return be;
   }
   s = hashtable_lookup(bs->hash_blk, hash, sizeof *hash, (void *)&be);
   if (s) {
      return be;
   }
   return NULL;
}


/*
 *------------------------------------------------------------------------
 *
 * blockstore_get_timestamp --
 *
 *------------------------------------------------------------------------
 */

time_t
blockstore_get_timestamp(const struct blockstore *bs)
{
   if (bs->best_chain == NULL) {
      return 1231006505; //  2009-01-03 18:15:05
   }
   ASSERT(bs->height == bs->best_chain->height);
   return bs->best_chain->header.timestamp;
}


/*
 *------------------------------------------------------------------------
 *
 * blockstore_get_height --
 *
 *------------------------------------------------------------------------
 */

int
blockstore_get_height(const struct blockstore *bs)
{
   if (bs->best_chain == NULL) {
      return 0;
   }
   ASSERT(bs->height == bs->best_chain->height);
   return bs->height;
}


/*
 *-------------------------------------------------------------------------
 *
 * blockstore_get_highest --
 *
 *-------------------------------------------------------------------------
 */

void
blockstore_get_highest(struct blockstore *bs,
                       const uint256 *hash0,
                       const uint256 *hash1,
                       uint256 *hash)
{
   int height0;
   int height1;

   if (uint256_iszero(hash0)) {
      memcpy(hash, hash1, sizeof *hash1);
      return;
   }
   if (uint256_iszero(hash1)) {
      memcpy(hash, hash0, sizeof *hash0);
      return;
   }

   height0 = blockstore_get_block_height(bs, hash0);
   height1 = blockstore_get_block_height(bs, hash1);
   ASSERT(height0 > 0);
   ASSERT(height1 > 0);

   if (height0 < height1) {
      memcpy(hash, hash1, sizeof *hash1);
   } else {
      memcpy(hash, hash0, sizeof *hash0);
   }
}


/*
 *-------------------------------------------------------------------------
 *
 * blockstore_get_block_height --
 *
 *-------------------------------------------------------------------------
 */

int
blockstore_get_block_height(struct blockstore *bs,
                            const uint256 *hash)
{
   struct blockentry *be;
   bool s;

   if (uint256_iszero(hash)) {
      return 0;
   }

   s = hashtable_lookup(bs->hash_blk, hash, sizeof *hash, (void*)&be);
   if (s == 0) {
      char hashStr[80];

      uint256_snprintf_reverse(hashStr, sizeof hashStr, hash);
      Warning(LGPFX" block %s not found.\n", hashStr);
      //ASSERT(0);
      return 0;
   }

   return be->height;
}


/*
 *------------------------------------------------------------------------
 *
 * blockstore_validate_chkpt --
 *
 *------------------------------------------------------------------------
 */

static bool
blockstore_validate_chkpt(const uint256 *hash,
                          uint32 height)
{
   struct block_cpt_entry *array;
   size_t n;
   int i;

   if (btc->testnet) {
      array = block_cpt_testnet;
      n = ARRAYSIZE(block_cpt_testnet);
   } else {
      array = block_cpt_main;
      n = ARRAYSIZE(block_cpt_main);
   }

   for (i = 0; i < n; i++) {
      if (height == array[i].height) {
         if (!uint256_issame(hash, &array[i].hash)) {
            char str[128];
            uint256_snprintf_reverse(str, sizeof str, hash);
            Warning(LGPFX" chkpt validation failed. height=%u %s\n",
                    height, str);
            return 0;
         }
         return 1;
      }
   }

   return 1;
}


/*
 *------------------------------------------------------------------------
 *
 * blockstore_set_chain_links --
 *
 *------------------------------------------------------------------------
 */

static int
blockstore_set_chain_links(struct blockstore *bs,
                           struct blockentry *be)
{
   struct blockentry *prev;
   char hashStr[80];
   uint256 hash;
   bool s;

   hash256_calc(&be->header, sizeof be->header, &hash);
   uint256_snprintf_reverse(hashStr, sizeof hashStr, &hash);

   if (be->height >= 0) {
      struct blockentry *li;
      /*
       * We've reached the junction with the current/old best chain. All the
       * entries from now on need to be made orphans.
       */

      Log(LGPFX" Reached block %s\n", hashStr);

      li = be->next;
      while (li) {
         hash256_calc(&li->header, sizeof li->header, &hash);
         uint256_snprintf_reverse(hashStr, sizeof hashStr, &hash);
         Log(LGPFX" moving #%d %s from blk -> orphan\n", li->height, hashStr);
         s = hashtable_remove(bs->hash_blk, &hash, sizeof hash);
         ASSERT(s);
         li->height = -1;
         s = hashtable_insert(bs->hash_orphans, &hash, sizeof hash, li);
         ASSERT(s);
         li = li->next;
      }

      return be->height;
   }

   ASSERT(be->height == -1); // orphan

   prev = blockstore_lookup(bs, &be->header.prevBlock);
   ASSERT(prev);

   be->height = 1 + blockstore_set_chain_links(bs, prev);

   Log(LGPFX" moving #%d %s from orphan -> blk\n", be->height, hashStr);

   prev->next = be;
   be->prev = prev;

   s = hashtable_remove(bs->hash_orphans, &hash, sizeof hash);
   ASSERT(s);
   s = hashtable_insert(bs->hash_blk, &hash, sizeof hash, be);
   ASSERT(s);

   return be->height;
}


/*
 *------------------------------------------------------------------------
 *
 * blockstore_find_alternate_chain_height --
 *
 *      Returns the height of an alternate chain starting at 'be'.
 *
 *------------------------------------------------------------------------
 */

static int
blockstore_find_alternate_chain_height(struct blockstore *bs,
                                       struct blockentry *be)
{
   struct blockentry *prev;

   if (be->height > 0) {
      return be->height;
   }

   ASSERT(be->height == -1); // orphan

   prev = blockstore_lookup(bs, &be->header.prevBlock);
   if (prev == NULL) {
      return 0;
   }

   return 1 + blockstore_find_alternate_chain_height(bs, prev);
}


/*
 *------------------------------------------------------------------------
 *
 * blockstore_set_best_chain --
 *
 *------------------------------------------------------------------------
 */

static void
blockstore_set_best_chain(struct blockstore *bs,
                          struct blockentry *be,
                          const uint256 *hash)
{
   int height;

   height = blockstore_find_alternate_chain_height(bs, be);

   Log(LGPFX" orphan block: alternate chain height is %d vs current %d\n",
       height, bs->height);

   if (height <= bs->height) {
      return;
   }

   /*
    * Properly wire the new chain.
    */
   blockstore_set_chain_links(bs, be);

   bs->best_chain = be;
   bs->height = height;

   memcpy(&bs->best_hash, hash, sizeof *hash);
}


/*
 *------------------------------------------------------------------------
 *
 * blockstore_add_entry --
 *
 *------------------------------------------------------------------------
 */

static void
blockstore_add_entry(struct blockstore *bs,
                     struct blockentry *be,
                     const uint256 *hash)
{
   bool s;

   if (bs->best_chain == NULL) {

      s = hashtable_insert(bs->hash_blk, hash, sizeof *hash, be);

      ASSERT(s);
      ASSERT(uint256_issame(hash, &bs->genesis_hash));
      ASSERT(be->prev == NULL);
      ASSERT(bs->height);

      bs->best_chain = be;
      bs->genesis = be;
      bs->height  = 0;
      be->height  = 0;

      memcpy(&bs->best_hash, hash, sizeof *hash);
   } else if (uint256_issame(&be->header.prevBlock, &bs->best_hash)) {

      s = hashtable_insert(bs->hash_blk, hash, sizeof *hash, be);
      ASSERT(s);

      bs->height++;
      be->height = bs->height;

      bs->best_chain->next = be;
      be->prev = bs->best_chain;
      bs->best_chain = be;

      memcpy(&bs->best_hash, hash, sizeof *hash);
   } else {
      char hashStr[80];
      uint32 count;

      be->height = -1;
      count = hashtable_getnumentries(bs->hash_orphans);

      uint256_snprintf_reverse(hashStr, sizeof hashStr, hash);
      Log(LGPFX" block %s orphaned. %u orphan%s total.\n",
          hashStr, count, count > 1 ? "s" : "");

      s = hashtable_insert(bs->hash_orphans, hash, sizeof *hash, be);
      ASSERT(s);

      blockstore_set_best_chain(bs, be, hash);
   }
}


/*
 *------------------------------------------------------------------------
 *
 * blockstore_has_header --
 *
 *------------------------------------------------------------------------
 */

bool
blockstore_has_header(const struct blockstore *bs,
                      const uint256 *hash)
{
   return hashtable_lookup(bs->hash_blk, hash, sizeof *hash, NULL);
}


/*
 *------------------------------------------------------------------------
 *
 * blockstore_is_orphan --
 *
 *------------------------------------------------------------------------
 */

bool
blockstore_is_orphan(const struct blockstore *bs,
                     const uint256 *hash)
{
   return hashtable_lookup(bs->hash_orphans, hash, sizeof *hash, NULL);
}


/*
 *------------------------------------------------------------------------
 *
 * blockstore_is_block_known --
 *
 *------------------------------------------------------------------------
 */

bool
blockstore_is_block_known(const struct blockstore *bs,
                          const uint256 *hash)
{
   return blockstore_has_header(bs, hash) ||
          blockstore_is_orphan(bs, hash);
}


/*
 *------------------------------------------------------------------------
 *
 * blockstore_alloc_entry --
 *
 *------------------------------------------------------------------------
 */

static struct blockentry *
blockstore_alloc_entry(const btc_block_header *hdr)
{
   struct blockentry *be;

   be = safe_malloc(sizeof *be);
   be->prev = NULL;
   be->next = NULL;
   be->height = 0;
   be->written = 0;
   memcpy(&be->header, hdr, sizeof *hdr);

   return be;
}


/*
 *------------------------------------------------------------------------
 *
 * blockstore_write_headers --
 *
 *------------------------------------------------------------------------
 */

void
blockstore_write_headers(struct blockstore *bs)
{
   btc_block_header *buf;
   struct blockentry *e;
   size_t numWritten;
   uint32 count;
   uint32 numhdr;
   int res;

   e = bs->best_chain;
   count = 0;
   while (e && e->written == 0) {
      e = e->prev;
      count++;
   }
   ASSERT(count < 2048);
   numhdr = count;
   if (count == 0) {
      return;
   }

   buf = safe_malloc(count * sizeof *buf);

   e = bs->best_chain;
   while (e && e->written == 0) {
      count--;
      memcpy(buf + count, &e->header, sizeof e->header);
      e->written = 1;
      e = e->prev;
   }

   ASSERT(count == 0);
   ASSERT(bs->blockSet);
   ASSERT_ON_COMPILE(sizeof *buf == 80);

   res = file_pwrite(bs->blockSet->desc, bs->blockSet->filesize,
                     buf, numhdr * sizeof *buf, &numWritten);
   free(buf);

   if (res != 0 || numWritten != numhdr * sizeof *buf) {
      Warning(LGPFX" failed to write %u block entries.\n", numhdr);
      return;
   }

   bs->blockSet->filesize += numWritten;
}


/*
 *------------------------------------------------------------------------
 *
 * blockstore_add_header --
 *
 *------------------------------------------------------------------------
 */

bool
blockstore_add_header(struct blockstore      *bs,
                      const btc_block_header *hdr,
                      const uint256          *hash,
                      bool                   *orphan)
{
   static unsigned int count;
   struct blockentry *be;

   *orphan = 0;

   /*
    * The caller is supposed to compute the checksum of the header it's adding.
    * Verify that it doesn't get this wrong every few blocks.
    */
   count++;
   if ((count % 32) == 0) {
      uint256 hash0;
      hash256_calc(hdr, sizeof *hdr, &hash0);
      ASSERT(uint256_issame(hash, &hash0));
   }

   be = blockstore_lookup(bs, hash);
   if (be) {
      return 0;
   }

   ASSERT(blockstore_validate_chkpt(hash, bs->height + 1));
   ASSERT(bs->best_chain || uint256_issame(hash, &bs->genesis_hash));

   be = blockstore_alloc_entry(hdr);
   blockstore_add_entry(bs, be, hash);

   *orphan = be->height == -1;

   return 1;
}


/*
 *------------------------------------------------------------------------
 *
 * blockset_close --
 *
 *------------------------------------------------------------------------
 */

static void
blockset_close(struct blockset *bset)
{
   if (file_valid(bset->desc)) {
      file_close(bset->desc);
   }

   free(bset->filename);
   free(bset);
}


/*
 *------------------------------------------------------------------------
 *
 * blockset_open_file --
 *
 *------------------------------------------------------------------------
 */

static int
blockset_open_file(struct blockstore *blockStore,
                   struct blockset *bs)
{
   uint64 offset;
   mtime_t ts;
   int res;

   res = file_open(bs->filename, 0 /* R/O */, 0 /* !unbuf */, &bs->desc);
   if (res) {
      return res;
   }

   bs->filesize = file_getsize(bs->desc);
   if (bs->filesize < 0) {
      return errno;
   }

   if (bs->filesize > 0) {
      char *s = print_size(bs->filesize);
      char *name = file_getname(bs->filename);
      Log(LGPFX" reading file %s -- %s -- %llu headers.\n",
          name, s, bs->filesize / sizeof(btc_block_header));
      free(name);
      free(s);
   }

   ts = time_get();
   offset = 0;
   while (offset < bs->filesize) {
      btc_block_header buf[10000];
      size_t numRead;
      size_t numBytes;
      int numHeaders;
      int i;

      numBytes = MIN(bs->filesize - offset, sizeof buf);

      res = file_pread(bs->desc, offset, buf, numBytes, &numRead);
      if (res != 0) {
         break;
      }

      if (btc->stop != 0) {
         res = 1;
         NOT_TESTED();
         break;
      }

      numHeaders = numRead / sizeof(btc_block_header);
      for (i = 0; i < numHeaders; i++) {
         struct blockentry *be;
         uint256 hash;

         be = blockstore_alloc_entry(buf + i);
         be->written = 1;
         hash256_calc(buf + i, sizeof buf[0], &hash);

         if (!blockstore_validate_chkpt(&hash, blockStore->height + 1)) {
            return 1;
         }

         blockstore_add_entry(blockStore, be, &hash);

         if (i == numHeaders - 1) {
#ifdef WITHUI
            bitcui_set_status("loading headers .. %llu%%",
                             (offset + numBytes) * 100 / bs->filesize);
#endif
         }
         if (i == numHeaders - 1 ||
             (numBytes < sizeof buf && i > numHeaders - 256)) {
#ifdef WITHUI
            bitcui_set_last_block_info(&hash, blockStore->height,
                                      be->header.timestamp);
#endif
         }
      }

      offset += numRead;
   }

   ts = time_get() - ts;

   char hashStr[80];
   char *latStr;

   uint256_snprintf_reverse(hashStr, sizeof hashStr, &blockStore->best_hash);
   Log(LGPFX" loaded blocks up to %s\n", hashStr);
   latStr = print_latency(ts);
   Log(LGPFX" this took %s\n", latStr);
   free(latStr);

   return res;
}


/*
 *------------------------------------------------------------------------
 *
 * blockset_open --
 *
 *------------------------------------------------------------------------
 */

static int
blockset_open(struct blockstore *blockStore,
             const char *filename)
{
   struct blockset *bs;

   bs = safe_calloc(1, sizeof *bs);
   bs->filename = safe_strdup(filename);

   blockStore->blockSet = bs;

   return blockset_open_file(blockStore, bs);
}


/*
 *------------------------------------------------------------------------
 *
 * blockstore_get_filename --
 *
 *------------------------------------------------------------------------
 */

static char *
blockstore_get_filename(struct config *config)
{
   char bsPath[PATH_MAX];
   char *dir;

   dir = bitc_get_directory();
   snprintf(bsPath, sizeof bsPath, "%s/headers.dat", dir);
   free(dir);

   return config_getstring(config, bsPath, "headers.filename");
}


/*
 *------------------------------------------------------------------------
 *
 * blockstore_zap --
 *
 *------------------------------------------------------------------------
 */

void
blockstore_zap(struct config *config)
{
   char *file;

   file = blockstore_get_filename(config);

   Warning(LGPFX" removing blockset '%s'.\n", file);
   file_unlink(file);
   free(file);
}


/*
 *------------------------------------------------------------------------
 *
 * blockstore_init --
 *
 *------------------------------------------------------------------------
 */

int
blockstore_init(struct config *config,
                struct blockstore **blockStore)
{
   struct blockstore *bs;
   char *file;
   bool s;
   int res;
   int i;

   *blockStore = NULL;

   file = blockstore_get_filename(config);
   Log(LGPFX" Using headers at '%s.\n", file);
   if (!file_exists(file)) {
      Log(LGPFX" file '%s' does not exist. Creating..\n", file);
      res = file_create(file);
      if (res) {
         Warning(LGPFX" failed to create file: %s\n",
                 strerror(res));
         free(file);
         return res;
      }
      res = file_chmod(file, 0600);
      if (res != 0) {
         Warning(LGPFX" failed to chmod 0600 '%s': %s\n", file,
                 strerror(res));
         free(file);
         return res;
      }
   }

   bs = safe_calloc(1, sizeof *bs);
   bs->height       = -1;
   bs->hash_blk     = hashtable_create();
   bs->hash_orphans = hashtable_create();

   const struct block_cpt_entry_str *arrayStr;
   struct block_cpt_entry *array;
   int n;

   if (btc->testnet) {
      n = ARRAYSIZE(cpt_testnet);
      array = block_cpt_testnet;
      arrayStr = cpt_testnet;
   } else {
      n = ARRAYSIZE(cpt_main);
      array = block_cpt_main;
      arrayStr = cpt_main;
   }

   for (i = 0; i < n; i++) {
      array[i].height = arrayStr[i].height;
      s = uint256_from_str(arrayStr[i].hashStr, &array[i].hash);
      ASSERT(s);
   }
   memcpy(&bs->genesis_hash.data, &array[0].hash.data, sizeof bs->genesis_hash);
   Log(LGPFX" Genesis: %s\n", arrayStr[0].hashStr);

   res = blockset_open(bs, file);
   free(file);
   if (res != 0) {
      goto exit;
   }

   Log(LGPFX" loaded %d headers.\n", bs->height + 1);
   *blockStore = bs;

   return 0;

exit:
   blockstore_exit(bs);

   return res;
}


/*
 *------------------------------------------------------------------------
 *
 * blockstore_exit --
 *
 *------------------------------------------------------------------------
 */

void
blockstore_exit(struct blockstore *blockStore)
{
   if (blockStore == NULL) {
      return;
   }

   blockstore_write_headers(blockStore);

   if (blockStore->height > 0) {
      Log(LGPFX" closing blockstore w/ height=%d\n", blockStore->height);
   }

   blockset_close(blockStore->blockSet);

   hashtable_printstats(blockStore->hash_blk, "blocks");
   hashtable_clear_with_free(blockStore->hash_blk);
   hashtable_clear_with_free(blockStore->hash_orphans);
   hashtable_destroy(blockStore->hash_blk);
   hashtable_destroy(blockStore->hash_orphans);

   memset(blockStore, 0, sizeof *blockStore);
   free(blockStore);
}


/*
 *-------------------------------------------------------------------------
 *
 * blockstore_get_hash_from_birth --
 *
 *-------------------------------------------------------------------------
 */

void
blockstore_get_hash_from_birth(const struct blockstore *bs,
                               time_t                   birth,
                               uint256                 *hash)
{
   struct blockentry *e;

   for (e = bs->best_chain; e != bs->genesis; e = e->prev)  {
      if (e->header.timestamp < birth) {
         char hashStr[80];
         uint64 ts = birth;

         hash256_calc(&e->header, sizeof e->header, hash);
         uint256_snprintf_reverse(hashStr, sizeof hashStr, hash);
         Log(LGPFX" birth %llu --> block %s.\n", ts, hashStr);
         return;
      }
   }
   memcpy(hash, &bs->genesis_hash, sizeof *hash);
}



/*
 *------------------------------------------------------------------------
 *
 * blockstore_is_next --
 *
 *------------------------------------------------------------------------
 */

bool
blockstore_is_next(struct blockstore *bs,
                   const uint256 *prev,
                   const uint256 *next)
{
   struct blockentry *be;
   uint256 hash;
   bool s;

   s = hashtable_lookup(bs->hash_blk, prev, sizeof *prev, (void*)&be);
   if (s == 0 || be->next == NULL) {
      return 0;
   }

   hash256_calc(&be->next->header, sizeof be->next->header, &hash);
   return uint256_issame(&hash, next);
}


/*
 *-------------------------------------------------------------------------
 *
 * blockstore_get_next_hashes --
 *
 *-------------------------------------------------------------------------
 */

void
blockstore_get_next_hashes(struct blockstore *bs,
                           const uint256 *start,
                           uint256 **hash,
                           int *n)
{
   struct blockentry *be;
   uint256 *table;
   int num;
   bool s;
   int i;

   i = 0;
   table = NULL;

   s = hashtable_lookup(bs->hash_blk, start, sizeof *start, (void*)&be);
   if (s == 0 || be->next == NULL) {
      goto exit;
   }

   be = be->next;
   num = 1000;
   table = safe_malloc(num * sizeof *table);

   while (be && i < num) {
      hash256_calc(&be->header, sizeof be->header, table + i);
      i++;
      be = be->next;
   }

exit:
   *n = i;
   *hash = table;
}


/*
 *-------------------------------------------------------------------------
 *
 * blockstore_get_best_hash --
 *
 *-------------------------------------------------------------------------
 */

void
blockstore_get_best_hash(const struct blockstore *bs,
                         uint256 *hash)
{
   if (bs->best_chain == NULL) {
      memset(hash, 0, sizeof *hash);
   } else {
      memcpy(hash, &bs->best_hash, sizeof *hash);
   }
}


/*
 *-------------------------------------------------------------------------
 *
 * blockstore_get_locator_hashes --
 *
 *-------------------------------------------------------------------------
 */

void
blockstore_get_locator_hashes(const struct blockstore *bs,
                              uint256 **hash,
                              int *num)
{
   volatile struct blockentry *be;
   uint256 h[64];
   uint32 step = 1;
   int n = 0;

   *hash = NULL;
   *num = 0;

   be = bs->best_chain;
   while (be) {
      int i;

      ASSERT(n < ARRAYSIZE(h));
      hash256_calc((void *)&be->header, sizeof be->header, h + n);
      n++;
      if (n >= 10) {
         step *= 2;
      }
      for (i = 0; i < step && be; i++) {
         be = be->prev;
      }
   }
   *num = n;
   if (n > 0) {
      *hash = safe_malloc(n * sizeof(uint256));
      memcpy(*hash, h, n * sizeof(uint256));
   }
}


/*
 *-------------------------------------------------------------------------
 *
 * blockstore_get_genesis --
 *
 *-------------------------------------------------------------------------
 */

void
blockstore_get_genesis(const struct blockstore *bs,
                       uint256 *genesis)
{
   memcpy(genesis->data, bs->genesis_hash.data, sizeof *genesis);
}


/*
 *-------------------------------------------------------------------------
 *
 * blockstore_get_block_timestamp --
 *
 *-------------------------------------------------------------------------
 */

time_t
blockstore_get_block_timestamp(const struct blockstore *bs,
                               const uint256 *hash)
{
   struct blockentry *be;

   if (uint256_iszero(hash)) {
      return 0;
   }

   be = blockstore_lookup(bs, hash);
   if (be == NULL) {
      char hashStr[80];

      uint256_snprintf_reverse(hashStr, sizeof hashStr, hash);
      Warning(LGPFX" block %s not found.\n", hashStr);
      ASSERT(0);
      return 0;
   }

   return be->header.timestamp;
}
