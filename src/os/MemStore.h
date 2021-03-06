// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2013- Sage Weil <sage@inktank.com>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */


#ifndef CEPH_MEMSTORE_H
#define CEPH_MEMSTORE_H

#include <mutex>
#include <boost/intrusive_ptr.hpp>

#include "include/unordered_map.h"
#include "include/memory.h"
#include "include/Spinlock.h"
#include "common/Finisher.h"
#include "common/RefCountedObj.h"
#include "common/RWLock.h"
#include "ObjectStore.h"
#include "PageSet.h"
#include "include/assert.h"

class MemStore : public ObjectStore {
private:
  CephContext *const cct;

public:
  struct Object : public RefCountedObject {
    std::mutex xattr_mutex;
    std::mutex omap_mutex;
    map<string,bufferptr> xattr;
    bufferlist omap_header;
    map<string,bufferlist> omap;

    typedef boost::intrusive_ptr<Object> Ref;
    friend void intrusive_ptr_add_ref(Object *o) { o->get(); }
    friend void intrusive_ptr_release(Object *o) { o->put(); }

    // interface for object data
    virtual size_t get_size() const = 0;
    virtual int read(uint64_t offset, uint64_t len, bufferlist &bl) = 0;
    virtual int write(uint64_t offset, const bufferlist &bl) = 0;
    virtual int clone(Object *src, uint64_t srcoff, uint64_t len,
                      uint64_t dstoff) = 0;
    virtual int truncate(uint64_t offset) = 0;
    virtual void encode(bufferlist& bl) const = 0;
    virtual void decode(bufferlist::iterator& p) = 0;

    void encode_base(bufferlist& bl) const {
      ::encode(xattr, bl);
      ::encode(omap_header, bl);
      ::encode(omap, bl);
    }
    void decode_base(bufferlist::iterator& p) {
      ::decode(xattr, p);
      ::decode(omap_header, p);
      ::decode(omap, p);
    }

    void dump(Formatter *f) const {
      f->dump_int("data_len", get_size());
      f->dump_int("omap_header_len", omap_header.length());

      f->open_array_section("xattrs");
      for (map<string,bufferptr>::const_iterator p = xattr.begin();
	   p != xattr.end();
	   ++p) {
	f->open_object_section("xattr");
	f->dump_string("name", p->first);
	f->dump_int("length", p->second.length());
	f->close_section();	
      }
      f->close_section();

      f->open_array_section("omap");
      for (map<string,bufferlist>::const_iterator p = omap.begin();
	   p != omap.end();
	   ++p) {
	f->open_object_section("pair");
	f->dump_string("key", p->first);
	f->dump_int("length", p->second.length());
	f->close_section();	
      }
      f->close_section();
    }
  };
  typedef Object::Ref ObjectRef;

  struct BufferlistObject : public Object {
    Spinlock mutex;
    bufferlist data;

    size_t get_size() const override { return data.length(); }

    int read(uint64_t offset, uint64_t len, bufferlist &bl) override;
    int write(uint64_t offset, const bufferlist &bl) override;
    int clone(Object *src, uint64_t srcoff, uint64_t len,
              uint64_t dstoff) override;
    int truncate(uint64_t offset) override;

    void encode(bufferlist& bl) const override {
      ENCODE_START(1, 1, bl);
      ::encode(data, bl);
      encode_base(bl);
      ENCODE_FINISH(bl);
    }
    void decode(bufferlist::iterator& p) override {
      DECODE_START(1, p);
      ::decode(data, p);
      decode_base(p);
      DECODE_FINISH(p);
    }
  };

  struct PageSetObject : public Object {
    PageSet data;
    size_t data_len;

    // use a thread-local vector for the pages returned by PageSet, so we
    // can avoid allocations in read/write()
    static thread_local PageSet::page_vector tls_pages;

    PageSetObject(size_t page_size) : data(page_size), data_len(0) {}

    size_t get_size() const override { return data_len; }

    int read(uint64_t offset, uint64_t len, bufferlist &bl) override;
    int write(uint64_t offset, const bufferlist &bl) override;
    int clone(Object *src, uint64_t srcoff, uint64_t len,
              uint64_t dstoff) override;
    int truncate(uint64_t offset) override;

    void encode(bufferlist& bl) const override {
      ENCODE_START(1, 1, bl);
      ::encode(data_len, bl);
      data.encode(bl);
      encode_base(bl);
      ENCODE_FINISH(bl);
    }
    void decode(bufferlist::iterator& p) override {
      DECODE_START(1, p);
      ::decode(data_len, p);
      data.decode(p);
      decode_base(p);
      DECODE_FINISH(p);
    }
  };

  struct Collection : public RefCountedObject {
    CephContext *cct;
    bool use_page_set;
    ceph::unordered_map<ghobject_t, ObjectRef> object_hash;  ///< for lookup
    map<ghobject_t, ObjectRef,ghobject_t::BitwiseComparator> object_map;        ///< for iteration
    map<string,bufferptr> xattr;
    RWLock lock;   ///< for object_{map,hash}

    typedef boost::intrusive_ptr<Collection> Ref;
    friend void intrusive_ptr_add_ref(Collection *c) { c->get(); }
    friend void intrusive_ptr_release(Collection *c) { c->put(); }

    ObjectRef create_object() const {
      if (use_page_set)
        return new PageSetObject(cct->_conf->memstore_page_size);
      return new BufferlistObject();
    }

    // NOTE: The lock only needs to protect the object_map/hash, not the
    // contents of individual objects.  The osd is already sequencing
    // reads and writes, so we will never see them concurrently at this
    // level.

    ObjectRef get_object(ghobject_t oid) {
      RWLock::RLocker l(lock);
      auto o = object_hash.find(oid);
      if (o == object_hash.end())
	return ObjectRef();
      return o->second;
    }

    ObjectRef get_or_create_object(ghobject_t oid) {
      RWLock::WLocker l(lock);
      auto result = object_hash.emplace(oid, ObjectRef());
      if (result.second)
        object_map[oid] = result.first->second = create_object();
      return result.first->second;
    }

    void encode(bufferlist& bl) const {
      ENCODE_START(1, 1, bl);
      ::encode(xattr, bl);
      ::encode(use_page_set, bl);
      uint32_t s = object_map.size();
      ::encode(s, bl);
      for (map<ghobject_t, ObjectRef,ghobject_t::BitwiseComparator>::const_iterator p = object_map.begin();
	   p != object_map.end();
	   ++p) {
	::encode(p->first, bl);
	p->second->encode(bl);
      }
      ENCODE_FINISH(bl);
    }
    void decode(bufferlist::iterator& p) {
      DECODE_START(1, p);
      ::decode(xattr, p);
      ::decode(use_page_set, p);
      uint32_t s;
      ::decode(s, p);
      while (s--) {
	ghobject_t k;
	::decode(k, p);
	auto o = create_object();
	o->decode(p);
	object_map.insert(make_pair(k, o));
	object_hash.insert(make_pair(k, o));
      }
      DECODE_FINISH(p);
    }

    uint64_t used_bytes() const {
      uint64_t result = 0;
      for (map<ghobject_t, ObjectRef,ghobject_t::BitwiseComparator>::const_iterator p = object_map.begin();
	   p != object_map.end();
	   ++p) {
        result += p->second->get_size();
      }

      return result;
    }

    Collection(CephContext *cct)
      : cct(cct), use_page_set(cct->_conf->memstore_page_set),
        lock("MemStore::Collection::lock") {}
  };
  typedef Collection::Ref CollectionRef;

private:
  class OmapIteratorImpl : public ObjectMap::ObjectMapIteratorImpl {
    CollectionRef c;
    ObjectRef o;
    map<string,bufferlist>::iterator it;
  public:
    OmapIteratorImpl(CollectionRef c, ObjectRef o)
      : c(c), o(o), it(o->omap.begin()) {}

    int seek_to_first() {
      std::lock_guard<std::mutex>(o->omap_mutex);
      it = o->omap.begin();
      return 0;
    }
    int upper_bound(const string &after) {
      std::lock_guard<std::mutex>(o->omap_mutex);
      it = o->omap.upper_bound(after);
      return 0;
    }
    int lower_bound(const string &to) {
      std::lock_guard<std::mutex>(o->omap_mutex);
      it = o->omap.lower_bound(to);
      return 0;
    }
    bool valid() {
      std::lock_guard<std::mutex>(o->omap_mutex);
      return it != o->omap.end();      
    }
    int next() {
      std::lock_guard<std::mutex>(o->omap_mutex);
      ++it;
      return 0;
    }
    string key() {
      std::lock_guard<std::mutex>(o->omap_mutex);
      return it->first;
    }
    bufferlist value() {
      std::lock_guard<std::mutex>(o->omap_mutex);
      return it->second;
    }
    int status() {
      return 0;
    }
  };


  ceph::unordered_map<coll_t, CollectionRef> coll_map;
  RWLock coll_lock;    ///< rwlock to protect coll_map
  Mutex apply_lock;    ///< serialize all updates

  CollectionRef get_collection(coll_t cid);

  Finisher finisher;

  uint64_t used_bytes;

  void _do_transaction(Transaction& t);

  int _touch(coll_t cid, const ghobject_t& oid);
  int _write(coll_t cid, const ghobject_t& oid, uint64_t offset, size_t len,
	      const bufferlist& bl, uint32_t fadvsie_flags = 0);
  int _zero(coll_t cid, const ghobject_t& oid, uint64_t offset, size_t len);
  int _truncate(coll_t cid, const ghobject_t& oid, uint64_t size);
  int _remove(coll_t cid, const ghobject_t& oid);
  int _setattrs(coll_t cid, const ghobject_t& oid, map<string,bufferptr>& aset);
  int _rmattr(coll_t cid, const ghobject_t& oid, const char *name);
  int _rmattrs(coll_t cid, const ghobject_t& oid);
  int _clone(coll_t cid, const ghobject_t& oldoid, const ghobject_t& newoid);
  int _clone_range(coll_t cid, const ghobject_t& oldoid,
		   const ghobject_t& newoid,
		   uint64_t srcoff, uint64_t len, uint64_t dstoff);
  int _omap_clear(coll_t cid, const ghobject_t &oid);
  int _omap_setkeys(coll_t cid, const ghobject_t &oid,
		    const map<string, bufferlist> &aset);
  int _omap_rmkeys(coll_t cid, const ghobject_t &oid, const set<string> &keys);
  int _omap_rmkeyrange(coll_t cid, const ghobject_t &oid,
		       const string& first, const string& last);
  int _omap_setheader(coll_t cid, const ghobject_t &oid, const bufferlist &bl);

  int _collection_hint_expected_num_objs(coll_t cid, uint32_t pg_num,
      uint64_t num_objs) const { return 0; }
  int _create_collection(coll_t c);
  int _destroy_collection(coll_t c);
  int _collection_add(coll_t cid, coll_t ocid, const ghobject_t& oid);
  int _collection_move_rename(coll_t oldcid, const ghobject_t& oldoid,
			      coll_t cid, const ghobject_t& o);
  int _split_collection(coll_t cid, uint32_t bits, uint32_t rem, coll_t dest);

  int _save();
  int _load();

  void dump(Formatter *f);
  void dump_all();

public:
  MemStore(CephContext *cct, const string& path)
    : ObjectStore(path),
      cct(cct),
      coll_lock("MemStore::coll_lock"),
      apply_lock("MemStore::apply_lock"),
      finisher(cct),
      used_bytes(0),
      sharded(false) {}
  ~MemStore() { }

  int peek_journal_fsid(uuid_d *fsid);

  bool test_mount_in_use() {
    return false;
  }

  int mount();
  int umount();

  unsigned get_max_object_name_length() {
    return 4096;
  }
  unsigned get_max_attr_name_length() {
    return 256;  // arbitrary; there is no real limit internally
  }

  int mkfs();
  int mkjournal() {
    return 0;
  }
  bool wants_journal() {
    return false;
  }
  bool allows_journal() {
    return false;
  }
  bool needs_journal() {
    return false;
  }

  bool sharded;
  void set_allow_sharded_objects() {
    sharded = true;
  }
  bool get_allow_sharded_objects() {
    return sharded;
  }

  int statfs(struct statfs *buf);

  bool exists(coll_t cid, const ghobject_t& oid);
  int stat(
    coll_t cid,
    const ghobject_t& oid,
    struct stat *st,
    bool allow_eio = false); // struct stat?
  int read(
    coll_t cid,
    const ghobject_t& oid,
    uint64_t offset,
    size_t len,
    bufferlist& bl,
    uint32_t op_flags = 0,
    bool allow_eio = false);
  int fiemap(coll_t cid, const ghobject_t& oid, uint64_t offset, size_t len, bufferlist& bl);
  int getattr(coll_t cid, const ghobject_t& oid, const char *name, bufferptr& value);
  int getattrs(coll_t cid, const ghobject_t& oid, map<string,bufferptr>& aset);

  int list_collections(vector<coll_t>& ls);
  bool collection_exists(coll_t c);
  bool collection_empty(coll_t c);
  int collection_list(coll_t cid, ghobject_t start, ghobject_t end,
		      bool sort_bitwise, int max,
		      vector<ghobject_t> *ls, ghobject_t *next);

  int omap_get(
    coll_t cid,                ///< [in] Collection containing oid
    const ghobject_t &oid,   ///< [in] Object containing omap
    bufferlist *header,      ///< [out] omap header
    map<string, bufferlist> *out /// < [out] Key to value map
    );

  /// Get omap header
  int omap_get_header(
    coll_t cid,                ///< [in] Collection containing oid
    const ghobject_t &oid,   ///< [in] Object containing omap
    bufferlist *header,      ///< [out] omap header
    bool allow_eio = false ///< [in] don't assert on eio
    );

  /// Get keys defined on oid
  int omap_get_keys(
    coll_t cid,              ///< [in] Collection containing oid
    const ghobject_t &oid, ///< [in] Object containing omap
    set<string> *keys      ///< [out] Keys defined on oid
    );

  /// Get key values
  int omap_get_values(
    coll_t cid,                    ///< [in] Collection containing oid
    const ghobject_t &oid,       ///< [in] Object containing omap
    const set<string> &keys,     ///< [in] Keys to get
    map<string, bufferlist> *out ///< [out] Returned keys and values
    );

  /// Filters keys into out which are defined on oid
  int omap_check_keys(
    coll_t cid,                ///< [in] Collection containing oid
    const ghobject_t &oid,   ///< [in] Object containing omap
    const set<string> &keys, ///< [in] Keys to check
    set<string> *out         ///< [out] Subset of keys defined on oid
    );

  ObjectMap::ObjectMapIterator get_omap_iterator(
    coll_t cid,              ///< [in] collection
    const ghobject_t &oid  ///< [in] object
    );

  void set_fsid(uuid_d u);
  uuid_d get_fsid();

  objectstore_perf_stat_t get_cur_stats();

  int queue_transactions(
    Sequencer *osr, list<Transaction*>& tls,
    TrackedOpRef op = TrackedOpRef(),
    ThreadPool::TPHandle *handle = NULL);
};




#endif
