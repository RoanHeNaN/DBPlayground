Why virtual function template is invalid?

What is virtual function template.
```cpp
class Base {
  public:
      // ❌ 返回值是函数自己的模板参数 T
      template<typename T> virtual T Key();

      // ❌ 参数是函数自己的模板参数 T
      template<typename T> virtual void Put(T v);
  };
```
Like above, I want to define a base class with some virtual functions, which is template function, 目的 is very clear, user could use a single base class pointer to involve its method like bellow:
```cpp
int main() {
    Base* base = new Derived();
    int32_t value = 1;
    base->Put(value);
    delete base;
}
```
This looks reasonable, but the compiler complains: `'virtual' cannot be specified on member function templates`。

The key point here is virtual function table.

Virtual function table is actually an array of function pointers. Each class has its OWN vtable (not one shared global table). For example, if we have derived classes D1 and D2, then D1 has its own vtable and D2 has its own vtable, and they are independent. Every object stores a hidden pointer (`vptr`) to the vtable of its own class. A derived class's vtable holds slots for ALL virtual functions visible in it (the ones inherited from the base plus its own), and the inherited ones keep the SAME slot index as in the base — that fixed, shared index is exactly what makes runtime dispatch work: `base->func()` compiles down to "read the object's vptr, then jump to slot N", where N is a constant decided at compile time.

So the size of a class's vtable, and the index of each slot, must be fixed at compile time. This is where a virtual function template breaks down. A single templated function like `Put<T>` is not one function — it expands into infinitely many instances (`Put<int>`, `Put<std::string>`, `Put<Dog>`, ...), and each instance would need its own vtable slot. Infinitely many instances means infinitely many slots, so the number of virtual functions — and therefore the vtable size and the slot indices — can no longer be decided at compile time.

Note that the infiniteness comes from the template argument `T`, NOT from the number of derived classes or runtime types. Even with a single derived class, a virtual function template is still illegal.

There is a second, equally fatal problem: which `T` values are actually used is decided by the CALLERS, which may live in completely different translation units. When the compiler is building a class's vtable, it cannot see all the `T` versions that other files will eventually instantiate. So even if we wanted to "count the slots", we couldn't — the set of instantiations is open-ended and invisible at the point where the vtable is laid out.

In short, the size and layout of a vtable must be fixed at compile time, but a virtual function template would require an infinite, cross-translation-unit-unknown number of slots, so it is invalid.

With this understanding, let's try to see why a class template CAN work.

The fundamental difference between a class template and a virtual function template is WHERE the template parameter lives. For a class template with a virtual function, the template parameter belongs to the CLASS, not to the function. The moment you write `A<int>`, the whole class becomes concrete: `func` is simply `void func(int)`, an ordinary (non-template) virtual function. So we know the exact type of every member, and even though `func` uses the template parameter as an argument, the compiler can generate one concrete function for it. Each specialization — `A<int>`, `A<std::string>` — is a separate class with its OWN vtable of fixed size, all decidable at compile time.

(The distinguishing factor is whether the FUNCTION itself is a template, not whether the class is. A class template may still contain a member function template, and THAT still cannot be virtual — e.g. `template<typename U> virtual void f(U);` inside `A<T>` is illegal for the same reason as before.)

```cpp
#include <string>
#include <iostream>

template<typename T>
class A {
public:
    virtual void func(T t) {
        std::cout << "func called\n";
    }
    virtual T get() { return T{}; }   // NOTE: must have a definition —
                                      // the vtable of A<int>/A<std::string>
                                      // references get(), so a bodyless
                                      // declaration causes a LINK error.
    virtual ~A() = default;
};

int main() {
    A<int> a;            a.func(42);
    A<std::string> b;    b.func("hi");
}
```

That link requirement is itself the proof of everything above: the compiler emits a concrete vtable per specialization, and each vtable slot must point at a real, already-generated function. A virtual function template has no single concrete function to point at, which is exactly why it is forbidden.

