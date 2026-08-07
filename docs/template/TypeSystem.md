# The Type System of a KV Engine

A key-value system has to deal with data of many types. Keys and values each
have their own type, and the two combine freely:

* `int` - `string`
* `string` - `string`
* `int` - `int`
* ...

The obvious first idea is to make the engine a template, parameterized by the
key and value types:

```cpp
template<typename Key, typename Value>
class KVStore {
public:
    KVStore();

    Value Get(Key target);
    void  Insert(Key key, Value value);
};
```

Then, at startup, we open a database, read its metadata to learn which types it
stores, and try to create the matching store:

```cpp
int main() {
    std::string path;
    auto db   = open(path);
    auto meta = db.readMeta();

    std::unique_ptr</* ??? */> store;
    if (meta.key == Int) {
        switch (meta.value) {
            case String:
                store = std::make_unique<KVStore<int, std::string>>();
                break;
            // ...
        }
    }
    // ...
}
```

Start writing this and you hit a wall almost immediately: **what type does
`store` have?** You cannot even declare the variable. `KVStore<int, string>`
and `KVStore<string, string>` are two unrelated types, and the one you need is
only known *after* `readMeta()` runs — at runtime.

## The real problem is not "too many combinations"

It is tempting to conclude: "the number of type combinations is unbounded, so
templates were the wrong tool from the start." That reasoning is wrong, and
`std::map` is the counter-example.

`std::map<int, string>` also lives in an unbounded space of combinations —
`map<A, B>` works for *any* `A` and `B`. Yet templates handle it perfectly.
Why?

Because the type is **written in the source code by the programmer**, so the
compiler knows it at compile time:

```cpp
std::map<int, std::string> m;   // int, string are right here — the compiler sees them
```

The compiler also instantiates lazily: out of the infinite space of possible
`map<K, V>`, it only generates the handful you actually name. "Unbounded" was
never the problem.

Our engine is different, and the difference has nothing to do with counting:

```cpp
auto meta = db.readMeta();                    // the type comes off disk — runtime
store = std::make_unique<KVStore< ?, ? >>();  // nothing can fill the ? in source
```

Template instantiation is a **compile-time** act; it requires the type to be a
concrete entity written *somewhere* in the source. The engine's type is a
**runtime value** read from `meta`. There is no place in the source where we can
spell it out.

> **The dividing line is compile-time type vs. runtime type — not the number of
> combinations.** `std::map` works because its type is given in source;
> `KVStore` at the engine granularity does not, because its type is decided by
> data at runtime.

## What about a factory that enumerates every combination?

One could argue: just list every case.

```cpp
switch (meta) {
    case Int_String:    return std::make_unique<KVStore<int,    std::string>>();
    case String_String: return std::make_unique<KVStore<std::string, std::string>>();
    // ...
}
```

Each `case` compiles, because the type inside it *is* written in source. But now
the count finally starts to bite:

* You must hand-write the full Cartesian product — 3 key types × 5 value types =
  15 cases, and every new type forces you to touch all of them.
* It only works for a **closed, finite** type set. The moment types become
  extensible, it collapses.

So combinatorial explosion is a *practical* pain of the factory approach — it is
**not** the root reason templates fail here. The root reason is still the same
one sentence: runtime type vs. compile-time type.

## Erasing the type: pushing the check into the Slice

Since the engine granularity has no compile-time type to work with, the store
has to become **type-erased** — one handle type that can point at a
`<int,string>` database now and a `<string,string>` database later:

```cpp
std::unique_ptr<KVStore> store = std::make_unique<KVStore>();
store->init(path1);                 // opens an <?, int32> database
store->Insert(key, int_value);

store->init(path2);                 // opens a *different*, <?, string> database
store->Insert(key, string_value);
```

Here `init(path)` reads that database's metadata and rebuilds the concrete
backend behind the handle; `path1` and `path2` are different databases with
different type combinations. This is exactly what a template can *not* express:
a single variable that outlives the concrete type it currently holds.

### A tempting dead end: a member template `Insert`

Before we get there, it is worth walking into one more wall, because it is the
one everybody tries:

```cpp
class KVStore {
public:
    void init(const std::string& path);

    template<typename Key, typename Value>
    void Insert(Key k, Value v);        // looks type-safe... does it help?
};
```

It does not, for three reasons that pile up:

1. **Who instantiates it?** The call is `store.Insert(key, value)`. To
   instantiate `Insert<int, string>` the compiler must know `int, string` at
   compile time — but they came from `meta` at runtime. You cannot write
   `store.Insert<meta.key, meta.value>(...)`. The original "can never be
   implemented" problem just moved from the class to the call site.
2. **Even if the call site knew the types**, `Insert` still has to store `k, v`
   into a backend whose type was fixed at runtime by `init`. So the first thing
   its body must do is check "is my runtime type == `typeid(Key)`?" and cast —
   the template parameters are thrown away on line one.
3. **A template member function cannot be `virtual`**, so it cannot even
   participate in the "swap the backend on `init`" polymorphism.

Conclusion: it does not matter where you put the `template` keyword — on the
class, on the method — the engine granularity never has the compile-time types
templates demand.

### The honest interface: bytes in, bytes out

So the interface degrades to something with a single, fixed signature that
carries no static type at all:

```cpp
class KVStore {
public:
    void init(const std::string& path);
    void Insert(Slice key, Slice value);   // uniform signature; no static type
    Slice Get(Slice key);
};
```

The type information no longer lives in the *signature*. It lives at **runtime**,
described by the schema that `init` loaded, and it is carried by the `Slice`
(raw bytes plus the knowledge of how to interpret them). **The type check has
moved from compile time into the Slice / runtime.** The price is exactly what
you would expect: safety that the compiler used to guarantee is now a runtime
responsibility.

## Templates were not useless — they belong at the Column granularity

The turning point: *whether a type is known at compile time depends on which
granularity you look at.*

* **KVStore / one row / one `Insert`**: the type is a runtime property — every
  database is different. Only erasure works.
* **Column**: within a single column, every element is, by construction, the
  **same** type — and that type is fixed *once*, the moment `init` reads the
  metadata. From that point down, everything is statically typed.

So a template lives very comfortably here:

```cpp
template<typename T>
class Column {                 // one column == one homogeneous type
    std::vector<T> data_;
public:
    void Append(T v);
    T    Get(size_t i);
};
```

The system splits into two layers, and the type check happens exactly once, at
the seam between them:

```
Insert(Slice, Slice)              <-- type-erased; interpret bytes per schema at runtime
        |   (one runtime dispatch / validation)
        v
Column<int> / Column<string>      <-- template instantiation; fully static inside
```

`init` *is* that seam: it reads `meta`, decides which `Column<T>` to instantiate
(one `switch` in a factory), and from then on incoming Slices are decoded once
and handed to the right `Column<T>`. Inside the column there is no runtime type
check left at all.

## Multi-column: the type system moves up to the application layer

A pure KV engine like LevelDB takes this one step further than everything above:
its value is not "a typed scalar decoded per schema" — it is just an **opaque
blob of bytes**. LevelDB has no notion of column, schema, or type at all. `Put`
and `Get` only ever see `Slice` in, `Slice` out.

So where do *columns* come from? You build them **above** the engine, by
serializing several fields into that one value blob yourself, and deserializing
them back on read:

```cpp
// store a row (id=42, name="alice", score=9.5) as ONE value
std::string EncodeRow(int64_t id, const std::string& name, double score) {
    std::string v;
    v.append(reinterpret_cast<char*>(&id), 8);
    uint32_t len = name.size();
    v.append(reinterpret_cast<char*>(&len), 4);
    v.append(name);
    v.append(reinterpret_cast<char*>(&score), 8);
    return v;                       // three columns squeezed into bytes
}

db->Put(opts, key, EncodeRow(42, "alice", 9.5));   // engine only sees bytes
db->Get(opts, key, &raw);                          // gets bytes back; YOU split them
```

The engine never learns that the value "has three columns". That knowledge —
how many columns, what type each is, where each starts — lives entirely in the
**application layer**: your encode/decode code plus the schema you maintain
yourself. In other words:

> **If we want multiple columns, we implement the type system in the application
> layer, on top of a type-erased byte engine.** The storage engine stays a
> dumb, schema-less "bytes in, bytes out" box; all type interpretation is the
> application's job.

This also settles how multi-column composes with the `Column<T>` idea from the
previous section. A row of N columns is **not** modeled as one
`Row<T1, T2, ..., TN>` template — that would make the instantiation count
*types^columns* and explode. Instead each column is an independent, single-type
`Column<T>`, and a row is assembled from several of them at the **type-erased
seam**. The exponent stays at 1 per column; the columns are glued together
above, in bytes.

This is exactly how real relational engines built on KV stores work —
CockroachDB / TiDB on RocksDB, InnoDB, etc. all encode a row's columns into the
KV value, and keep the schema and type system in the SQL layer above the engine.

## Summary

1. A KV system must handle many key/value types.
2. Making the *engine* a template fails — but not because there are too many
   combinations. It fails because the type is decided at runtime (`meta`), while
   templates require the type at compile time.
3. `std::map` works precisely because its type is written in source (compile-time
   known); the count of combinations is irrelevant.
4. A factory that enumerates combinations compiles, but explodes into a
   Cartesian product and only supports a closed type set — a practical dead end,
   not the root fix.
5. So the engine interface must be type-erased: the type check sinks into the
   **Slice** at runtime.
6. Templates are not wasted — they belong at the homogeneous **Column**
   granularity, where the type is compile-time known. Type checking then happens
   exactly once, at the Slice → Column seam.
7. A pure KV engine (LevelDB) pushes this to the extreme: its value is opaque
   bytes with no schema at all. **Multiple columns are an application-layer
   construct** — you serialize columns into the value and keep the type system
   above the engine. A row is composed of several single-type `Column<T>` at the
   erased seam, never a single `Row<T1..TN>` (which would explode as
   *types^columns*). This is how CockroachDB / TiDB / InnoDB build tables on top
   of a KV store.
