/**
 * @file benchmark_cpp.cpp
 * @brief C++ benchmark — exercises C++ and STL-specific decompiler modules.
 *
 * Modules targeted (not covered by benchmark.c):
 *   algo_recover     — std::for_each, std::transform, std::accumulate,
 *                      std::find, std::find_if, std::partition, std::copy
 *   sort_detect      — std::sort (introsort), std::stable_sort (mergesort),
 *                      manual quicksort, heapsort, insertion sort
 *   container_detect — std::vector, std::list, std::map, std::unordered_map,
 *                      std::string (SSO), std::shared_ptr
 *   concurrency_det  — std::mutex, std::atomic, CAS spinlock
 *   cxx_backend      — vtable, virtual dispatch, constructors, destructors,
 *                      new/delete, RTTI (dynamic_cast, typeid)
 *   eh_reconstruct   — try/catch/throw, std::exception hierarchy
 *   ipa              — call graph depth, tail recursion, mutual recursion
 *   type_inference   — template instantiations, type-parameterised functions
 *   string_detect    — std::string SSO, wide strings, string_view
 *
 * Compile:
 *   g++ -O1 -std=c++17 -o benchmark_cpp_O1 benchmark_cpp.cpp
 *   g++ -O1 -std=c++17 -s -o benchmark_cpp_O1_stripped benchmark_cpp.cpp
 *
 * Design: every class/function is __attribute__((noinline)) to defeat inlining.
 * volatile and side-effects prevent constant-folding.
 */

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>
#include <unordered_map>
#include <vector>

volatile int g_sink_cpp = 0;

/* ============================================================
 * SECTION 1 — Virtual dispatch / vtable
 * Target: cxx_backend::VtableDetector, CtorDtorDetector
 * ============================================================ */

class Shape {
public:
    virtual ~Shape() = default;
    virtual double area() const = 0;
    virtual double perimeter() const = 0;
    virtual const char *name() const = 0;
};

class Circle : public Shape {
    double r_;
public:
    explicit Circle(double r) : r_(r) {}
    double area()      const override { return 3.14159265 * r_ * r_; }
    double perimeter() const override { return 2.0 * 3.14159265 * r_; }
    const char *name() const override { return "Circle"; }
};

class Rectangle : public Shape {
    double w_, h_;
public:
    Rectangle(double w, double h) : w_(w), h_(h) {}
    double area()      const override { return w_ * h_; }
    double perimeter() const override { return 2.0 * (w_ + h_); }
    const char *name() const override { return "Rectangle"; }
};

class Triangle : public Shape {
    double a_, b_, c_;
public:
    Triangle(double a, double b, double c) : a_(a), b_(b), c_(c) {}
    double area() const override {
        double s = (a_ + b_ + c_) / 2.0;
        double val = s * (s-a_) * (s-b_) * (s-c_);
        return val > 0.0 ? val : 0.0;   /* Heron (no sqrt in benchmark) */
    }
    double perimeter() const override { return a_ + b_ + c_; }
    const char *name() const override { return "Triangle"; }
};

__attribute__((noinline))
double total_area(const std::vector<Shape*>& shapes) {
    double sum = 0.0;
    for (const Shape* s : shapes)
        sum += s->area();
    return sum;
}

/* ============================================================
 * SECTION 2 — Constructors / destructors / new-delete
 * Target: cxx_backend::CtorDtorDetector, new/delete recovery
 * ============================================================ */

class Buffer {
    uint8_t *data_;
    size_t   size_;
public:
    explicit Buffer(size_t n) : data_(new uint8_t[n]), size_(n) {
        std::memset(data_, 0, n);
    }
    ~Buffer() { delete[] data_; }
    size_t size() const { return size_; }
    uint8_t& operator[](size_t i) { return data_[i]; }
    const uint8_t& operator[](size_t i) const { return data_[i]; }
};

__attribute__((noinline))
int buffer_checksum(volatile int n) {
    Buffer buf(static_cast<size_t>(n));
    for (int i = 0; i < n; i++)
        buf[static_cast<size_t>(i)] = static_cast<uint8_t>(i & 0xFF);
    int sum = 0;
    for (int i = 0; i < n; i++)
        sum += buf[static_cast<size_t>(i)];
    return sum;
}

/* ============================================================
 * SECTION 3 — RTTI: dynamic_cast, typeid
 * Target: rtti-finder, cxx_backend RTTI recovery
 * ============================================================ */

__attribute__((noinline))
const char *shape_type_name(const Shape *s) {
    if (dynamic_cast<const Circle*>(s))    return "is Circle";
    if (dynamic_cast<const Rectangle*>(s)) return "is Rectangle";
    if (dynamic_cast<const Triangle*>(s))  return "is Triangle";
    return typeid(*s).name();
}

__attribute__((noinline))
double dispatch_by_type(Shape *s, volatile double scale) {
    if (auto *c = dynamic_cast<Circle*>(s))
        return c->area() * scale;
    if (auto *r = dynamic_cast<Rectangle*>(s))
        return r->perimeter() * scale;
    return s->area();
}

/* ============================================================
 * SECTION 4 — Exception handling
 * Target: eh_reconstruct (Itanium EH), cxx_backend EH recovery
 * ============================================================ */

class DomainError : public std::runtime_error {
public:
    explicit DomainError(const char *msg) : std::runtime_error(msg) {}
};

__attribute__((noinline))
double safe_sqrt(volatile double x) {
    if (x < 0.0) throw DomainError("sqrt of negative");
    double r = x;
    for (int i = 0; i < 20; i++)   /* Newton iterations (no <cmath>) */
        r = 0.5 * (r + x / r);
    return r;
}

__attribute__((noinline))
double try_sqrt(volatile double x) {
    try {
        return safe_sqrt(x);
    } catch (const DomainError& e) {
        return -1.0;
    } catch (const std::exception& e) {
        return -2.0;
    }
}

__attribute__((noinline))
int exception_chain(volatile int x) {
    try {
        if (x < 0) throw std::invalid_argument("negative");
        if (x > 100) throw std::out_of_range("too large");
        return x * 2;
    } catch (const std::out_of_range&) {
        return -1;
    } catch (const std::invalid_argument&) {
        return -2;
    }
}

/* ============================================================
 * SECTION 5 — std::vector operations
 * Target: container_detect::VectorDetector
 * ============================================================ */

__attribute__((noinline))
std::vector<int> build_vec(volatile int n) {
    std::vector<int> v;
    v.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; i++)
        v.push_back(i * i);
    return v;
}

__attribute__((noinline))
int vec_sum(const std::vector<int>& v) {
    int s = 0;
    for (int x : v) s += x;
    return s;
}

__attribute__((noinline))
void vec_filter_inplace(std::vector<int>& v, volatile int threshold) {
    v.erase(std::remove_if(v.begin(), v.end(),
                           [=](int x){ return x > threshold; }),
            v.end());
}

/* ============================================================
 * SECTION 6 — std::list (doubly-linked)
 * Target: container_detect::ListDetector
 * ============================================================ */

__attribute__((noinline))
std::list<int> build_list(volatile int n) {
    std::list<int> lst;
    for (int i = 0; i < n; i++) {
        if (i % 2 == 0) lst.push_back(i);
        else             lst.push_front(i);
    }
    return lst;
}

__attribute__((noinline))
int list_sum_stl(const std::list<int>& lst) {
    int s = 0;
    for (int x : lst) s += x;
    return s;
}

/* ============================================================
 * SECTION 7 — std::map (red-black tree)
 * Target: container_detect::MapDetector
 * ============================================================ */

__attribute__((noinline))
std::map<std::string, int> word_count(const std::vector<std::string>& words) {
    std::map<std::string, int> freq;
    for (const auto& w : words)
        freq[w]++;
    return freq;
}

__attribute__((noinline))
int map_total(const std::map<std::string, int>& m) {
    int s = 0;
    for (const auto& [k, v] : m) s += v;
    return s;
}

/* ============================================================
 * SECTION 8 — std::unordered_map (hash table)
 * Target: container_detect::UnorderedMapDetector
 * ============================================================ */

__attribute__((noinline))
std::unordered_map<int,int> square_map(volatile int n) {
    std::unordered_map<int,int> m;
    for (int i = 0; i < n; i++)
        m[i] = i * i;
    return m;
}

__attribute__((noinline))
int umap_lookup(const std::unordered_map<int,int>& m, volatile int k) {
    int key = k;
    auto it = m.find(key);
    return it != m.end() ? it->second : -1;
}

/* ============================================================
 * SECTION 9 — std::string / SSO
 * Target: container_detect::StringDetector, string_detect::SSODetect
 * ============================================================ */

__attribute__((noinline))
std::string build_greeting(const std::string& name) {
    return "Hello, " + name + "!";
}

__attribute__((noinline))
int count_vowels(const std::string& s) {
    int n = 0;
    for (char c : s)
        if (c=='a'||c=='e'||c=='i'||c=='o'||c=='u'||
            c=='A'||c=='E'||c=='I'||c=='O'||c=='U') n++;
    return n;
}

__attribute__((noinline))
std::string reverse_string(std::string s) {
    std::reverse(s.begin(), s.end());
    return s;
}

/* ============================================================
 * SECTION 10 — std::shared_ptr
 * Target: container_detect::SharedPtrDetector
 * ============================================================ */

struct Node {
    int value;
    std::shared_ptr<Node> next;
    explicit Node(int v) : value(v) {}
};

__attribute__((noinline))
std::shared_ptr<Node> build_shared_list(volatile int n) {
    std::shared_ptr<Node> head;
    for (int i = n - 1; i >= 0; i--) {
        auto node = std::make_shared<Node>(i);
        node->next = head;
        head = node;
    }
    return head;
}

__attribute__((noinline))
int shared_list_sum(std::shared_ptr<Node> head) {
    int s = 0;
    for (auto p = head; p; p = p->next)
        s += p->value;
    return s;
}

/* ============================================================
 * SECTION 11 — STL algorithms: for_each, transform, accumulate
 * Target: algo_recover (ForEachDetector, TransformDetector, AccumulateDetector)
 * ============================================================ */

__attribute__((noinline))
int stl_accumulate(const std::vector<int>& v) {
    return std::accumulate(v.begin(), v.end(), 0);
}

__attribute__((noinline))
std::vector<int> stl_transform_double(const std::vector<int>& v) {
    std::vector<int> out(v.size());
    std::transform(v.begin(), v.end(), out.begin(),
                   [](int x){ return x * 2; });
    return out;
}

__attribute__((noinline))
void stl_for_each_print(std::vector<int>& v) {
    std::for_each(v.begin(), v.end(),
                  [](int& x){ x += 1; });   /* in-place +1 */
}

/* ============================================================
 * SECTION 12 — STL algorithms: find, find_if, partition
 * Target: algo_recover (FindDetector, PartitionDetector)
 * ============================================================ */

__attribute__((noinline))
int stl_find_first(const std::vector<int>& v, volatile int target) {
    auto it = std::find(v.begin(), v.end(), target);
    return it != v.end() ? static_cast<int>(it - v.begin()) : -1;
}

__attribute__((noinline))
int stl_find_if_even(const std::vector<int>& v) {
    auto it = std::find_if(v.begin(), v.end(),
                           [](int x){ return x % 2 == 0; });
    return it != v.end() ? *it : -1;
}

__attribute__((noinline))
int stl_partition_evens(std::vector<int>& v) {
    auto mid = std::partition(v.begin(), v.end(),
                              [](int x){ return x % 2 == 0; });
    return static_cast<int>(mid - v.begin());   /* # evens */
}

/* ============================================================
 * SECTION 13 — STL algorithms: copy, any_of, all_of, count
 * Target: algo_recover (TransformDetector/copy, AllOfDetector, etc.)
 * ============================================================ */

__attribute__((noinline))
std::vector<int> stl_copy_positive(const std::vector<int>& v) {
    std::vector<int> out;
    std::copy_if(v.begin(), v.end(), std::back_inserter(out),
                 [](int x){ return x > 0; });
    return out;
}

__attribute__((noinline))
bool stl_all_positive(const std::vector<int>& v) {
    return std::all_of(v.begin(), v.end(), [](int x){ return x > 0; });
}

__attribute__((noinline))
int stl_count_even(const std::vector<int>& v) {
    return static_cast<int>(
        std::count_if(v.begin(), v.end(), [](int x){ return x % 2 == 0; }));
}

/* ============================================================
 * SECTION 14 — Sorting algorithms via std::sort / std::stable_sort
 * Target: sort_detect (IntrosortDetector, MergesortDetector)
 * ============================================================ */

__attribute__((noinline))
std::vector<int> stl_sort_copy(std::vector<int> v) {
    std::sort(v.begin(), v.end());
    return v;
}

__attribute__((noinline))
std::vector<int> stl_stable_sort_copy(std::vector<int> v) {
    std::stable_sort(v.begin(), v.end());
    return v;
}

/* ============================================================
 * SECTION 15 — Manual sorting algorithms
 * Target: sort_detect (HeapsortDetector, InsertionSortDetector, QuicksortDetector)
 * ============================================================ */

__attribute__((noinline))
void insertion_sort(std::vector<int>& v) {
    for (int i = 1; i < (int)v.size(); i++) {
        int key = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > key) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = key;
    }
}

__attribute__((noinline))
static void heapify(std::vector<int>& v, int n, int i) {
    int largest = i, left = 2*i+1, right = 2*i+2;
    if (left  < n && v[left]  > v[largest]) largest = left;
    if (right < n && v[right] > v[largest]) largest = right;
    if (largest != i) {
        std::swap(v[i], v[largest]);
        heapify(v, n, largest);
    }
}

__attribute__((noinline))
void heap_sort(std::vector<int>& v) {
    int n = static_cast<int>(v.size());
    for (int i = n/2 - 1; i >= 0; i--) heapify(v, n, i);
    for (int i = n - 1; i > 0; i--) {
        std::swap(v[0], v[i]);
        heapify(v, i, 0);
    }
}

__attribute__((noinline))
static int quicksort_partition(std::vector<int>& v, int lo, int hi) {
    int pivot = v[hi], i = lo - 1;
    for (int j = lo; j < hi; j++)
        if (v[j] <= pivot) std::swap(v[++i], v[j]);
    std::swap(v[i+1], v[hi]);
    return i + 1;
}

__attribute__((noinline))
void quicksort(std::vector<int>& v, int lo, int hi) {
    if (lo < hi) {
        int p = quicksort_partition(v, lo, hi);
        quicksort(v, lo, p - 1);
        quicksort(v, p + 1, hi);
    }
}

/* ============================================================
 * SECTION 16 — Concurrency: mutex, atomic, CAS spinlock
 * Target: concurrency_detect
 * ============================================================ */

static std::mutex g_mutex;
static std::atomic<int> g_shared_counter{0};

__attribute__((noinline))
void mutex_increment(volatile int n) {
    for (int i = 0; i < n; i++) {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_sink_cpp++;
    }
}

__attribute__((noinline))
int atomic_fetch_add_loop(volatile int n) {
    int total = 0;
    for (int i = 0; i < n; i++)
        total += g_shared_counter.fetch_add(1, std::memory_order_relaxed);
    return total;
}

__attribute__((noinline))
int cas_spinlock_demo(volatile int expected_val) {
    /* Simple CAS loop — compare_exchange_weak pattern */
    int expected = expected_val;
    int desired  = expected + 1;
    std::atomic<int> flag{expected};
    int tries = 0;
    while (!flag.compare_exchange_weak(expected, desired,
                                       std::memory_order_acq_rel)) {
        expected = expected_val;
        if (++tries > 1000) break;
    }
    return tries;
}

/* ============================================================
 * SECTION 17 — Templates: type-parameterised functions
 * Target: type_inference, cxx_backend::TemplateSkeleton
 * ============================================================ */

template <typename T>
__attribute__((noinline))
T tpl_max(T a, T b) { return a > b ? a : b; }

template <typename T>
__attribute__((noinline))
T tpl_clamp(T v, T lo, T hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

template <typename T>
__attribute__((noinline))
T tpl_sum(const T* arr, int n) {
    T total = T{};
    for (int i = 0; i < n; i++) total += arr[i];
    return total;
}

/* ============================================================
 * SECTION 18 — IPA: deep call chains, mutual recursion
 * Target: ipa (call_graph, Tarjan SCC, inline_candidate)
 * ============================================================ */

__attribute__((noinline)) static int is_even(int n);
__attribute__((noinline)) static int is_odd(int n);

static int is_even(int n) { return n == 0 ? 1 : is_odd(n - 1); }
static int is_odd(int n)  { return n == 0 ? 0 : is_even(n - 1); }

__attribute__((noinline))
int count_even_mutual(volatile int n) {
    int count = 0;
    for (int i = 0; i <= n; i++)
        count += is_even(i);
    return count;
}

__attribute__((noinline))
static int ackermann(int m, int n) {
    if (m == 0) return n + 1;
    if (n == 0) return ackermann(m - 1, 1);
    return ackermann(m - 1, ackermann(m, n - 1));
}

__attribute__((noinline))
int ackermann_2_3() { return ackermann(2, 3); }

/* ============================================================
 * SECTION 19 — Operator overloading / complex classes
 * Target: cxx_backend (operator recovery, class member inference)
 * ============================================================ */

class Vec2 {
public:
    double x, y;
    Vec2(double x, double y) : x(x), y(y) {}
    Vec2 operator+(const Vec2& o) const { return {x+o.x, y+o.y}; }
    Vec2 operator*(double s) const { return {x*s, y*s}; }
    double dot(const Vec2& o) const { return x*o.x + y*o.y; }
    bool operator==(const Vec2& o) const { return x==o.x && y==o.y; }
};

__attribute__((noinline))
double vec2_ops(volatile double ax, volatile double ay,
                volatile double bx, volatile double by) {
    Vec2 a{ax, ay}, b{bx, by};
    Vec2 c = a + b;
    Vec2 d = c * 2.0;
    return d.dot(a);
}

/* ============================================================
 * SECTION 20 — Inheritance + polymorphism depth
 * Target: cxx_backend (multi-level inheritance, virtual chains)
 * ============================================================ */

class Animal {
public:
    virtual ~Animal() = default;
    virtual std::string speak() const = 0;
    virtual std::string kind()  const = 0;
};

class Mammal : public Animal {
public:
    std::string kind() const override { return "mammal"; }
};

class Dog : public Mammal {
    std::string name_;
public:
    explicit Dog(const std::string& name) : name_(name) {}
    std::string speak() const override { return "Woof! I am " + name_; }
};

class Cat : public Mammal {
public:
    std::string speak() const override { return "Meow"; }
};

class Bird : public Animal {
public:
    std::string kind()  const override { return "bird"; }
    std::string speak() const override { return "Tweet"; }
};

__attribute__((noinline))
std::string all_speak(const std::vector<Animal*>& animals) {
    std::string result;
    for (const Animal* a : animals) {
        result += a->speak();
        result += " (";
        result += a->kind();
        result += ") ";
    }
    return result;
}

/* ============================================================
 * main — exercises every section
 * ============================================================ */

int main() {
    /* Section 1: vtable / virtual dispatch */
    Circle    circ(5.0);
    Rectangle rect(3.0, 4.0);
    Triangle  tri(3.0, 4.0, 5.0);
    std::vector<Shape*> shapes = {&circ, &rect, &tri};
    g_sink_cpp += static_cast<int>(total_area(shapes));

    /* Section 2: constructor / destructor / new-delete */
    g_sink_cpp += buffer_checksum(16);

    /* Section 3: RTTI */
    g_sink_cpp += static_cast<int>(*shape_type_name(&circ) != 0);
    g_sink_cpp += static_cast<int>(dispatch_by_type(&rect, 1.0));

    /* Section 4: exceptions */
    g_sink_cpp += static_cast<int>(try_sqrt(9.0));
    g_sink_cpp += static_cast<int>(try_sqrt(-1.0));
    g_sink_cpp += exception_chain(50);
    g_sink_cpp += exception_chain(200);

    /* Section 5: std::vector */
    auto v = build_vec(10);
    g_sink_cpp += vec_sum(v);
    vec_filter_inplace(v, 50);
    g_sink_cpp += vec_sum(v);

    /* Section 6: std::list */
    auto lst = build_list(8);
    g_sink_cpp += list_sum_stl(lst);

    /* Section 7: std::map */
    std::vector<std::string> words = {"foo","bar","foo","baz","bar","foo"};
    auto freq = word_count(words);
    g_sink_cpp += map_total(freq);

    /* Section 8: std::unordered_map */
    auto um = square_map(10);
    g_sink_cpp += umap_lookup(um, 7);

    /* Section 9: std::string / SSO */
    auto greet = build_greeting("World");
    g_sink_cpp += count_vowels(greet);
    auto rev = reverse_string("benchmark");
    g_sink_cpp += static_cast<int>(rev[0]);

    /* Section 10: std::shared_ptr */
    auto slist = build_shared_list(6);
    g_sink_cpp += shared_list_sum(slist);

    /* Section 11: for_each / transform / accumulate */
    std::vector<int> nums = {1,2,3,4,5,6,7,8,9,10};
    g_sink_cpp += stl_accumulate(nums);
    auto doubled = stl_transform_double(nums);
    stl_for_each_print(doubled);
    g_sink_cpp += doubled[0];

    /* Section 12: find / find_if / partition */
    g_sink_cpp += stl_find_first(nums, 7);
    g_sink_cpp += stl_find_if_even(nums);
    auto nums2 = nums;
    g_sink_cpp += stl_partition_evens(nums2);

    /* Section 13: copy_if / all_of / count_if */
    auto pos = stl_copy_positive(nums);
    g_sink_cpp += static_cast<int>(stl_all_positive(nums));
    g_sink_cpp += stl_count_even(nums);

    /* Section 14: std::sort / std::stable_sort */
    std::vector<int> unsorted = {5,2,8,1,9,3,7,4,6};
    auto sorted1 = stl_sort_copy(unsorted);
    auto sorted2 = stl_stable_sort_copy(unsorted);
    g_sink_cpp += sorted1[0] + sorted2[0];

    /* Section 15: manual sorts */
    auto ins_v = unsorted;  insertion_sort(ins_v);  g_sink_cpp += ins_v[0];
    auto heap_v = unsorted; heap_sort(heap_v);       g_sink_cpp += heap_v[0];
    auto qs_v = unsorted;
    quicksort(qs_v, 0, static_cast<int>(qs_v.size()) - 1);
    g_sink_cpp += qs_v[0];

    /* Section 16: concurrency */
    mutex_increment(3);
    g_sink_cpp += atomic_fetch_add_loop(5);
    g_sink_cpp += cas_spinlock_demo(0);

    /* Section 17: templates */
    g_sink_cpp += tpl_max(3, 7);
    g_sink_cpp += tpl_clamp(150, 0, 100);
    int arr17[4] = {1,2,3,4};
    g_sink_cpp += tpl_sum(arr17, 4);

    /* Section 18: IPA */
    g_sink_cpp += count_even_mutual(10);
    g_sink_cpp += ackermann_2_3();

    /* Section 19: operator overloading */
    g_sink_cpp += static_cast<int>(vec2_ops(1.0, 2.0, 3.0, 4.0));

    /* Section 20: multi-level inheritance */
    Dog dog("Rex");
    Cat cat;
    Bird bird;
    std::vector<Animal*> animals = {&dog, &cat, &bird};
    auto sound = all_speak(animals);
    g_sink_cpp += static_cast<int>(sound.size());

    return 0;
}
